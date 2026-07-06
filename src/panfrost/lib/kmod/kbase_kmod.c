/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 *
 * kmod backend for the ARM Mali kbase kernel driver (/dev/mali*).
 * Targets the Bifrost/Valhall kbase driver r32p0–r44p0.
 *
 * Key architectural differences from the panthor / panfrost DRM backends:
 *  - kbase is not a DRM driver; the fd is not a DRM fd.
 *  - Memory is allocated via KBASE_IOCTL_MEM_ALLOC, which directly returns a
 *    48-bit GPU VA. There are no GEM handles.
 *  - The GPU address space is implicit (one per context) so vm_create and
 *    vm_bind are thin wrappers that record the VAs assigned by the kernel.
 *  - CPU access to a BO is obtained by mmap()-ing the kbase fd with the GPU VA
 *    as the file offset. bo_get_mmap_offset() therefore returns the GPU VA.
 *  - drmPrimeFDToHandle() does not work on a kbase fd; pan_kmod_bo_import()
 *    will fail gracefully (returns NULL). dma-buf import / WSI is left for
 *    future work.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/vma.h"
#include "util/simple_mtx.h"
#include "util/log.h"

#include "drm-uapi/mali_kbase_ioctl.h"
#include "pan_kmod_backend.h"
#include "pan_props.h"

/* Forward declaration — the full definition is at the end of this file. */
const struct pan_kmod_ops kbase_kmod_ops;

/* -------------------------------------------------------------------------
 * Internal device / BO / VM objects
 * ---------------------------------------------------------------------- */

struct kbase_kmod_dev {
   struct pan_kmod_dev base;

   /* Monotonically-increasing handle counter.
    * kbase has no GEM handles; we mint our own u32 handles for use with the
    * pan_kmod handle_to_bo sparse array. */
   uint32_t next_handle; /* accessed with p_atomic_inc */
};

struct kbase_kmod_vm {
   struct pan_kmod_vm base;

   /* Userspace VA heap for AUTO_VA allocations.
    * kbase assigns VAs automatically via MEM_ALLOC; we record them here so
    * that pan_kmod callers can read back va.start after vm_bind MAP. */
   struct {
      simple_mtx_t lock;
      struct util_vma_heap heap;
   } auto_va;
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;

   /* GPU virtual address returned by KBASE_IOCTL_MEM_ALLOC (or MEM_IMPORT).
    * Also used as the mmap() file offset to obtain a CPU mapping. */
   uint64_t gpu_va;
};

/* -------------------------------------------------------------------------
 * GPU properties blob parsing helpers
 * ---------------------------------------------------------------------- */

/*
 * Each entry in the GPU props blob is encoded as:
 *   4 bytes header: (key << 2) | size_code
 *     size_code 0 → u8 (1 byte)
 *     size_code 1 → u16 (2 bytes)
 *     size_code 2 → u32 (4 bytes)
 *     size_code 3 → u64 (8 bytes)
 *   N bytes value
 */
static uint64_t
kbase_gpuprop_get(const uint8_t *buf, size_t buf_size,
                  uint32_t target_key, uint64_t default_val)
{
   size_t offset = 0;

   while (offset + 4 <= buf_size) {
      uint32_t hdr;
      memcpy(&hdr, buf + offset, 4);
      offset += 4;

      uint32_t key = hdr >> 2;
      uint32_t size_code = hdr & 0x3;
      uint32_t val_size = 1u << size_code;

      if (offset + val_size > buf_size)
         break;

      if (key == target_key) {
         uint64_t val = 0;
         memcpy(&val, buf + offset, val_size);
         return val;
      }

      offset += val_size;
   }

   return default_val;
}

/* Fetch the GPU props blob from the kernel.  Returns a heap-allocated buffer
 * (must be freed by the caller) and its byte length, or NULL on error. */
static uint8_t *
kbase_get_gpuprops(int fd, size_t *out_size)
{
   /* First call: size=0 probes the required buffer length. */
   struct kbase_ioctl_get_gpuprops req = { 0 };
   int ret = drmIoctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (ret < 0) {
      mesa_loge("kbase: KBASE_IOCTL_GET_GPUPROPS (probe) failed: %s",
                strerror(errno));
      return NULL;
   }

   size_t size = (size_t)ret;
   if (size == 0) {
      mesa_loge("kbase: KBASE_IOCTL_GET_GPUPROPS returned zero size");
      return NULL;
   }

   uint8_t *buf = malloc(size);
   if (!buf)
      return NULL;

   req.buffer = (uintptr_t)buf;
   req.size = (uint32_t)size;

   ret = drmIoctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (ret < 0) {
      mesa_loge("kbase: KBASE_IOCTL_GET_GPUPROPS (fill) failed: %s",
                strerror(errno));
      free(buf);
      return NULL;
   }

   *out_size = size;
   return buf;
}

static void
kbase_dev_query_props(struct kbase_kmod_dev *kbase_dev,
                      const uint8_t *buf, size_t buf_size)
{
   struct pan_kmod_dev_props *props = &kbase_dev->base.props;

   /* GPU ID
    *   product_id  = bits[31:16] of the GPU_ID register
    *   major_rev   = bits[15:12]
    *   minor_rev   = bits[11:4]
    *   version_status = bits[3:0]
    *
    * KBASE_GPUPROP_RAW_GPU_ID holds the complete 32-bit GPU_ID register.
    * If that property is absent (older drivers), fall back to reconstructing
    * from the individual fields. */
   uint32_t raw_gpu_id = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_GPU_ID, 0);

   if (raw_gpu_id) {
      /* gpu_id as used by pan_arch(): product_id (top 16 bits) << 16 |
       * revision (bottom 16 bits). */
      props->gpu_id = ((uint64_t)(raw_gpu_id & 0xffff0000u)) |
                      (uint64_t)(raw_gpu_id & 0xffffu);
   } else {
      /* Fallback: reassemble from individual props (older kbase). */
      uint32_t product_id     = (uint32_t)kbase_gpuprop_get(
         buf, buf_size, KBASE_GPUPROP_PRODUCT_ID, 0);
      uint32_t major_revision = (uint32_t)kbase_gpuprop_get(
         buf, buf_size, KBASE_GPUPROP_MAJOR_REVISION, 0);
      uint32_t minor_revision = (uint32_t)kbase_gpuprop_get(
         buf, buf_size, KBASE_GPUPROP_MINOR_REVISION, 0);
      uint32_t version_status = (uint32_t)kbase_gpuprop_get(
         buf, buf_size, KBASE_GPUPROP_VERSION_STATUS, 0);

      props->gpu_id = ((uint64_t)product_id << 16) |
                      ((uint64_t)(major_revision & 0xf) << 12) |
                      ((uint64_t)(minor_revision & 0xff) << 4) |
                      (uint64_t)(version_status & 0xf);
   }

   /* GPU variant (lower 8 bits of CORE_FEATURES register) */
   props->gpu_variant = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_CORE_FEATURES, 0) & 0xff;

   /* Hardware feature registers */
   props->shader_present = kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_SHADER_PRESENT, 0);
   props->tiler_features = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_TILER_FEATURES, 0);
   props->mem_features = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_MEM_FEATURES, 0);
   props->mmu_features = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_MMU_FEATURES, 0);

   for (unsigned i = 0; i < ARRAY_SIZE(props->texture_features); i++) {
      props->texture_features[i] = (uint32_t)kbase_gpuprop_get(
         buf, buf_size,
         KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0 + i, 0);
   }

   props->afbc_features = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_AFBC_FEATURES, 0);

   /* Thread / core properties */
   props->max_threads_per_core = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS, 0);
   props->max_threads_per_wg = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE, 0);

   uint32_t thread_features = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_THREAD_FEATURES, 0);
   props->max_tasks_per_core = MAX2(thread_features >> 24, 1);
   props->num_registers_per_core = thread_features & 0xffff;

   props->max_tls_instance_per_core = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_THREAD_TLS_ALLOC, 0);
   if (!props->max_tls_instance_per_core)
      props->max_tls_instance_per_core = props->max_threads_per_core;

   /* If num_registers_per_core is zero, use fallback defaults per arch. */
   if (!props->num_registers_per_core) {
      unsigned arch = pan_arch(props->gpu_id);
      switch (arch) {
      case 4:
      case 5:
         props->num_registers_per_core = props->max_threads_per_core * 4;
         break;
      case 6:
         props->num_registers_per_core = props->max_threads_per_core * 64;
         break;
      default:
         props->num_registers_per_core = props->max_threads_per_core * 32;
         break;
      }
   }

   /* Coherency: kbase raw coherency mode.
    * 0 = none, 1 = ACE-Lite, 2 = ACE. */
   uint32_t coherency_mode = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_COHERENCY_MODE, 0);
   props->is_io_coherent = coherency_mode != 0;

   /* Timestamp: expose if gpu_id is non-zero (safe assumption). */
   props->gpu_can_query_timestamp = (props->gpu_id != 0);
   props->timestamp_device_coherent = (coherency_mode != 0);
   /* The GPU cycle counter frequency is not exposed through the GPU props
    * blob in the kbase uAPI.  It can be inferred at runtime by correlating
    * KBASE_IOCTL_GET_CPU_GPU_TIMEINFO with OS monotonic time, but that
    * requires a sampling window.  Leave it as 0 so the upper layers use
    * OS timestamps instead of cycle counts; callers that need accurate
    * frequency should perform their own calibration. */
   props->timestamp_frequency = 0;

   /* Priorities: kbase currently only advertises medium. */
   props->allowed_group_priorities_mask =
      PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM |
      PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW;

   /* Supported BO flags — WB_MMAP requires cache-sync support which we
    * leave for future work; skip it for now so flush_bo_map_syncs is a
    * no-op. */
   props->supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |
                                PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
                                PAN_KMOD_BO_FLAG_NO_MMAP;
}

/* -------------------------------------------------------------------------
 * Device creation / destruction
 * ---------------------------------------------------------------------- */

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags,
                      const struct pan_kmod_driver *drv_info,
                      const struct pan_kmod_allocator *allocator)
{
   /* Version handshake — request at least major=3, minor=0. */
   struct kbase_ioctl_version_check ver = { .major = 3, .minor = 0 };
   if (drmIoctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver)) {
      if (errno == EPERM || errno == EACCES || errno == ENOTTY ||
          errno == EINVAL || errno == ENODEV) {
         mesa_logd("kbase: KBASE_IOCTL_VERSION_CHECK probe rejected: %s",
                   strerror(errno));
      } else {
         mesa_loge("kbase: KBASE_IOCTL_VERSION_CHECK failed: %s",
                   strerror(errno));
      }
      return NULL;
   }

   if (ver.major < 3) {
      mesa_loge("kbase: unsupported kernel driver version %d.%d "
                "(need >= 3.0)", ver.major, ver.minor);
      return NULL;
   }

   /* Set context creation flags.
    * Use zero flags for maximum compatibility. */
   struct kbase_ioctl_set_flags set_flags = { .create_flags = 0 };
   if (drmIoctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags)) {
      mesa_loge("kbase: KBASE_IOCTL_SET_FLAGS failed: %s", strerror(errno));
      return NULL;
   }

   /* Fetch GPU properties. */
   size_t props_size = 0;
   uint8_t *props_buf = kbase_get_gpuprops(fd, &props_size);
   if (!props_buf) {
      mesa_loge("kbase: failed to read GPU properties");
      return NULL;
   }

   struct kbase_kmod_dev *kbase_dev =
      pan_kmod_alloc(allocator, sizeof(*kbase_dev));
   if (!kbase_dev) {
      mesa_loge("kbase: failed to allocate kbase_kmod_dev");
      free(props_buf);
      return NULL;
   }

   /* Use a fallback driver version if not provided by pan_kmod_dev_create
    * (kbase fd is not a DRM fd so drmGetVersion() fails). */
   struct pan_kmod_driver kbase_drv = {
      .version = { .major = ver.major, .minor = ver.minor },
   };
   if (drv_info)
      kbase_drv = *drv_info;

   pan_kmod_dev_init(&kbase_dev->base, fd, flags, &kbase_drv,
                     &kbase_kmod_ops, allocator);

   kbase_dev->next_handle = 1;

   kbase_dev_query_props(kbase_dev, props_buf, props_size);
   free(props_buf);

   if (!kbase_dev->base.props.gpu_id) {
      mesa_loge("kbase: failed to determine GPU ID from properties");
      pan_kmod_dev_cleanup(&kbase_dev->base);
      pan_kmod_free(allocator, kbase_dev);
      return NULL;
   }

   return &kbase_dev->base;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct kbase_kmod_dev *kbase_dev =
      container_of(dev, struct kbase_kmod_dev, base);

   pan_kmod_dev_cleanup(dev);
   pan_kmod_free(dev->allocator, kbase_dev);
}

/* -------------------------------------------------------------------------
 * VA range query
 *
 * kbase supports up to 48-bit VAs on Valhall; on JM GPUs the VA space is
 * 32-bit (mmu_features.va_bits = 32). Reserve the bottom 32 MB as panfrost
 * does.
 * ---------------------------------------------------------------------- */

#define KBASE_KMOD_VA_RESERVE 0x2000000ull

static struct pan_kmod_va_range
kbase_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev)
{
   uint8_t va_bits = MMU_FEATURES_VA_BITS(dev->props.mmu_features);
   if (va_bits < 32)
      va_bits = 32;

   uint64_t end = va_bits == 32 ? (1ull << 32) : (1ull << (va_bits - 1));

   return (struct pan_kmod_va_range){
      .start = KBASE_KMOD_VA_RESERVE,
      .size  = end - KBASE_KMOD_VA_RESERVE,
   };
}

/* -------------------------------------------------------------------------
 * Buffer object allocation / free
 * ---------------------------------------------------------------------- */

static uint64_t
to_kbase_mem_flags(uint32_t kmod_flags)
{
   uint64_t flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;

   /* CPU access is needed unless the caller explicitly asks for a GPU-only
    * buffer. */
   if (!(kmod_flags & PAN_KMOD_BO_FLAG_NO_MMAP))
      flags |= BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;

   if (kmod_flags & PAN_KMOD_BO_FLAG_EXECUTABLE)
      flags |= BASE_MEM_PROT_GPU_EX;

   if (kmod_flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)
      flags |= BASE_MEM_GROW_ON_GPF;

   if (kmod_flags & PAN_KMOD_BO_FLAG_GPU_UNCACHED)
      flags |= BASE_MEM_UNCACHED_GPU;

   if (kmod_flags & PAN_KMOD_BO_FLAG_IO_COHERENT)
      flags |= BASE_MEM_COHERENT_SYSTEM;

   return flags;
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev,
                    struct pan_kmod_vm *exclusive_vm, uint64_t size,
                    uint32_t kmod_flags)
{
   struct kbase_kmod_dev *kbase_dev =
      container_of(dev, struct kbase_kmod_dev, base);

   uint64_t page_size = 4096;
   uint64_t va_pages = (size + page_size - 1) / page_size;
   /* For growable (ALLOC_ON_FAULT) buffers commit nothing up-front. */
   uint64_t commit_pages = (kmod_flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)
                              ? 0 : va_pages;

   struct kbase_kmod_bo *kbase_bo =
      pan_kmod_dev_alloc(dev, sizeof(*kbase_bo));
   if (!kbase_bo) {
      mesa_loge("kbase: failed to allocate kbase_kmod_bo");
      return NULL;
   }

   union kbase_ioctl_mem_alloc req = {
      .in = {
         .va_pages      = va_pages,
         .commit_pages  = commit_pages,
         .extent        = (kmod_flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)
                             ? va_pages : 0,
         .flags         = to_kbase_mem_flags(kmod_flags),
      },
   };

   if (drmIoctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &req)) {
      mesa_loge("kbase: KBASE_IOCTL_MEM_ALLOC failed: %s", strerror(errno));
      goto err_free_bo;
   }

   kbase_bo->gpu_va = req.out.gpu_va;

   /* Allocate a unique u32 handle for the pan_kmod handle_to_bo table. */
   uint32_t handle = p_atomic_inc_return(&kbase_dev->next_handle);

   pan_kmod_bo_init(&kbase_bo->base, dev, exclusive_vm,
                    va_pages * page_size, kmod_flags, handle);
   return &kbase_bo->base;

err_free_bo:
   pan_kmod_dev_free(dev, kbase_bo);
   return NULL;
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbase_bo =
      container_of(bo, struct kbase_kmod_bo, base);

   pan_kmod_bo_cleanup(bo);

   struct kbase_ioctl_mem_free req = { .gpu_addr = kbase_bo->gpu_va };
   if (drmIoctl(bo->dev->fd, KBASE_IOCTL_MEM_FREE, &req))
      mesa_loge("kbase: KBASE_IOCTL_MEM_FREE failed: %s", strerror(errno));

   pan_kmod_dev_free(bo->dev, kbase_bo);
}

/*
 * dma-buf import is not yet implemented: pan_kmod_bo_import() first calls
 * drmPrimeFDToHandle() which requires a DRM fd.  It will fail gracefully
 * (returns NULL) on a kbase fd, so callers get NULL without a crash.
 * A full implementation would call KBASE_IOCTL_MEM_IMPORT here.
 */
static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *dev, uint32_t handle, uint64_t size)
{
   mesa_loge("kbase: bo_import not yet implemented "
             "(dma-buf import requires KBASE_IOCTL_MEM_IMPORT)");
   errno = ENOSYS;
   return NULL;
}

/* In kbase the GPU VA is used directly as the mmap() file offset. */
static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbase_bo =
      container_of(bo, struct kbase_kmod_bo, base);
   return (off_t)kbase_bo->gpu_va;
}

/* kbase does not expose per-BO fence objects.
 *
 * Returning true here is safe for non-shared BOs that have an exclusive_vm
 * (the common case for compute/render work) because the GPU is serialised by
 * the queue-submission path before the CPU touches the buffer.  For shared /
 * exported BOs proper inter-process synchronisation must be handled at a
 * higher level (e.g. via sync_file / Android fence).
 *
 * A full implementation would use KBASE_IOCTL_KCPU_QUEUE_ENQUEUE with a
 * fence-wait operation to block until the GPU has finished with the BO. */
static bool
kbase_kmod_bo_wait(struct pan_kmod_bo *bo, int64_t timeout_ns,
                   bool for_read_only_access)
{
   return true;
}

/* WB_MMAP is not advertised in supported_bo_flags so this path should never
 * be reached in practice.  Return success to avoid assertion failures. */
static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   return 0;
}

/* Eviction hints — kbase does not expose a madvise ioctl in the userspace
 * API so these are no-ops. */
static void
kbase_kmod_bo_make_evictable(UNUSED struct pan_kmod_bo *bo)
{
}

static bool
kbase_kmod_bo_make_unevictable(UNUSED struct pan_kmod_bo *bo)
{
   return true;
}

/* -------------------------------------------------------------------------
 * VM (GPU address space) management
 *
 * kbase automatically provides one GPU address space per context.  There is
 * no kernel ioctl to explicitly create/destroy a VM.
 *
 * vm_create allocates a bookkeeping structure and (for AUTO_VA) a userspace
 * VMA heap that mirrors the range the kernel will assign allocations in.
 *
 * vm_bind MAP: records the GPU VA already assigned by KBASE_IOCTL_MEM_ALLOC.
 * vm_bind UNMAP: no-op; the unmap happens automatically when bo_free calls
 *                KBASE_IOCTL_MEM_FREE.
 * ---------------------------------------------------------------------- */

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   struct kbase_kmod_vm *kbase_vm =
      pan_kmod_dev_alloc(dev, sizeof(*kbase_vm));
   if (!kbase_vm) {
      mesa_loge("kbase: failed to allocate kbase_kmod_vm");
      return NULL;
   }

   if (flags & PAN_KMOD_VM_FLAG_AUTO_VA) {
      simple_mtx_init(&kbase_vm->auto_va.lock, mtx_plain);
      util_vma_heap_init(&kbase_vm->auto_va.heap, va_start, va_range);
   }

   pan_kmod_vm_init(&kbase_vm->base, dev, 0 /* no kernel handle */,
                    flags, PAN_PGSIZE_4K);
   return &kbase_vm->base;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   struct kbase_kmod_vm *kbase_vm =
      container_of(vm, struct kbase_kmod_vm, base);

   if (vm->flags & PAN_KMOD_VM_FLAG_AUTO_VA) {
      simple_mtx_destroy(&kbase_vm->auto_va.lock);
      util_vma_heap_finish(&kbase_vm->auto_va.heap);
   }

   pan_kmod_dev_free(vm->dev, kbase_vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   if (mode == PAN_KMOD_VM_OP_MODE_ASYNC) {
      mesa_loge("kbase: PAN_KMOD_VM_OP_MODE_ASYNC not supported");
      return -1;
   }

   for (uint32_t i = 0; i < op_count; i++) {
      struct pan_kmod_vm_op *op = &ops[i];

      if (op->type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct kbase_kmod_bo *kbase_bo =
            container_of(op->map.bo, struct kbase_kmod_bo, base);

         /* kbase assigns the GPU VA at alloc time (MEM_ALLOC).  There is no
          * separate map operation.  Report the VA back to the caller whether
          * they asked for AUTO_VA or an explicit address. */
         op->va.start = kbase_bo->gpu_va;

      } else if (op->type == PAN_KMOD_VM_OP_TYPE_UNMAP) {
         /* Unmap happens automatically at bo_free time via MEM_FREE. */
      } else if (op->type == PAN_KMOD_VM_OP_TYPE_SYNC_ONLY) {
         /* No-op. */
      }
   }

   return 0;
}

static enum pan_kmod_vm_state
kbase_kmod_vm_query_state(struct pan_kmod_vm *vm)
{
   /* kbase does not expose a VM-fault query; assume usable. */
   return PAN_KMOD_VM_USABLE;
}

/* -------------------------------------------------------------------------
 * Timestamp
 * ---------------------------------------------------------------------- */

static uint64_t
kbase_kmod_query_timestamp(const struct pan_kmod_dev *dev)
{
   struct kbase_ioctl_get_cpu_gpu_timeinfo req = {
      .in.request_flags = BASE_TIMEINFO_TIMESTAMP_FLAG,
   };

   if (drmIoctl(dev->fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &req) == 0)
      return req.out.timestamp;

   return 0;
}

/* -------------------------------------------------------------------------
 * BO labelling (not available in the kbase uAPI, silently ignored)
 * ---------------------------------------------------------------------- */

static void
kbase_kmod_bo_set_label(UNUSED struct pan_kmod_dev *dev,
                        UNUSED struct pan_kmod_bo *bo,
                        UNUSED const char *label)
{
}

/* -------------------------------------------------------------------------
 * ops vtable
 * ---------------------------------------------------------------------- */

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create             = kbase_kmod_dev_create,
   .dev_destroy            = kbase_kmod_dev_destroy,
   .dev_query_user_va_range = kbase_kmod_dev_query_user_va_range,
   .bo_alloc               = kbase_kmod_bo_alloc,
   .bo_free                = kbase_kmod_bo_free,
   .bo_import              = kbase_kmod_bo_import,
   .bo_get_mmap_offset     = kbase_kmod_bo_get_mmap_offset,
   .flush_bo_map_syncs     = kbase_kmod_flush_bo_map_syncs,
   .bo_wait                = kbase_kmod_bo_wait,
   .bo_make_evictable      = kbase_kmod_bo_make_evictable,
   .bo_make_unevictable    = kbase_kmod_bo_make_unevictable,
   .vm_create              = kbase_kmod_vm_create,
   .vm_destroy             = kbase_kmod_vm_destroy,
   .vm_bind                = kbase_kmod_vm_bind,
   .vm_query_state         = kbase_kmod_vm_query_state,
   .query_timestamp        = kbase_kmod_query_timestamp,
   .bo_set_label           = kbase_kmod_bo_set_label,
};
