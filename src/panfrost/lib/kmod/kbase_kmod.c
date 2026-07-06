/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 *
 * kmod backend for the ARM Mali kbase kernel driver (/dev/mali*).
 * Targets the Bifrost/Valhall kbase driver r32p0–r44p0, both the JM
 * (arch <= 9, uAPI 11.x) and CSF (arch >= 10, uAPI 1.x) flavours.
 *
 * Key architectural differences from the panthor / panfrost DRM backends
 * (see the panfork driver, src/panfrost/base/, for a working reference):
 *
 *  - kbase is not a DRM driver; the fd is not a DRM fd.  The handshake is:
 *    VERSION_CHECK (CSF nr 52 / JM nr 0) -> SET_FLAGS -> mmap() of the
 *    tracking page.  Every other ioctl returns -EPERM before that.
 *  - There are no GEM handles; we mint our own u32 handles for the
 *    pan_kmod handle_to_bo sparse array.
 *  - For 64-bit clients the kernel forces SAME_VA on all non-executable
 *    allocations: KBASE_IOCTL_MEM_ALLOC returns a cookie, and mmap()-ing
 *    the fd at that cookie establishes the mapping, with CPU VA == GPU VA.
 *    We therefore mmap() every BO right at allocation time and keep that
 *    mapping until the BO is freed (bo_mmap/bo_munmap ops return/release
 *    the cached mapping).  munmap() of a SAME_VA region frees the GPU
 *    mapping too (free-on-close), so bo_free relies on it.
 *  - GPU-executable BOs come from the EXEC_VA zone (initialised with
 *    KBASE_IOCTL_MEM_EXEC_INIT at device-open time) and return a real
 *    GPU VA, which doubles as the mmap offset for CPU access.
 *  - The GPU address space is implicit (one per context), so vm_create /
 *    vm_bind are thin wrappers that record the VAs assigned by the kernel.
 *  - drmPrimeFDToHandle() does not work on a kbase fd; pan_kmod_bo_import()
 *    will fail gracefully (returns NULL). dma-buf import / WSI is left for
 *    future work (KBASE_IOCTL_MEM_IMPORT).
 *  - Command submission (CSF queue groups / JM job atoms) is not wired up
 *    yet; this backend currently only supports device enumeration and
 *    memory management.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/log.h"

#include "drm-uapi/mali_kbase_ioctl.h"
/* Only used as the interchange format for CSF interface information; no
 * panthor functionality is required. */
#include "drm-uapi/panthor_drm.h"

#include "kbase_kmod.h"
#include "pan_kmod_backend.h"
#include "pan_props.h"

/* Forward declaration — the full definition is at the end of this file. */
const struct pan_kmod_ops kbase_kmod_ops;

/* -------------------------------------------------------------------------
 * Internal device / BO / VM objects
 * ---------------------------------------------------------------------- */

struct kbase_kmod_dev {
   struct pan_kmod_dev base;

   /* True for the CSF flavour of kbase (arch >= 10), false for JM. */
   bool is_csf;

   /* CSF interface information (CSF only), queried from
    * KBASE_IOCTL_CS_GET_GLB_IFACE and stored in the panthor uAPI layout
    * so CSF-generic code can consume either backend. */
   struct drm_panthor_csif_info csif_info;

   /* Tracking page mapped at BASE_MEM_MAP_TRACKING_HANDLE.  Required by
    * the kernel before any memory operation; unmapped at destroy time. */
   void *tracking_page;

   /* Monotonically-increasing handle counter.
    * kbase has no GEM handles; we mint our own u32 handles for use with the
    * pan_kmod handle_to_bo sparse array. */
   uint32_t next_handle; /* accessed with p_atomic_inc */
};

struct kbase_kmod_vm {
   struct pan_kmod_vm base;

   /* Nothing kbase-specific: the kernel manages one implicit address space
    * per context and assigns all VAs itself. */
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;

   /* GPU virtual address.  For SAME_VA allocations this equals the CPU
    * mapping address; for EXEC_VA-zone allocations it is the address
    * returned by KBASE_IOCTL_MEM_ALLOC. */
   uint64_t gpu_va;

   /* CPU mapping established at allocation time, valid for the whole BO
    * lifetime.  bo_mmap returns it; bo_munmap is a no-op. */
   void *cpu_ptr;

   /* Whether this is a SAME_VA region (freed by munmap()) or a zone
    * region (freed by KBASE_IOCTL_MEM_FREE). */
   bool same_va;
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
   int ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
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

   ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
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

   /* TEXTURE_FEATURES_0..2 are consecutive keys, but TEXTURE_FEATURES_3
    * was added later and got a non-contiguous key. */
   STATIC_ASSERT(ARRAY_SIZE(props->texture_features) == 4);
   for (unsigned i = 0; i < 3; i++) {
      props->texture_features[i] = (uint32_t)kbase_gpuprop_get(
         buf, buf_size,
         KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0 + i, 0);
   }
   props->texture_features[3] = (uint32_t)kbase_gpuprop_get(
      buf, buf_size, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3, 0);

   /* AFBC_FEATURES has no equivalent in the kbase GPU props blob; panfork
    * reports 0 for it as well. */
   props->afbc_features = 0;

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
      buf, buf_size, KBASE_GPUPROP_TLS_ALLOC, 0);
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

   /* Supported BO flags — WB_MMAP requires cache-sync support
    * (KBASE_IOCTL_MEM_SYNC / BASE_MEM_CACHED_CPU) which we leave for
    * future work; skip it for now so flush_bo_map_syncs is a no-op. */
   props->supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |
                                PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
                                PAN_KMOD_BO_FLAG_NO_MMAP |
                                PAN_KMOD_BO_FLAG_GPU_UNCACHED;
}

/* -------------------------------------------------------------------------
 * Device creation / destruction
 * ---------------------------------------------------------------------- */

/* Query the CSF global interface and derive the information CSF-generic
 * code needs, in the panthor uAPI layout.  Returns 0 on success. */
static int
kbase_query_csif_info(int fd, struct drm_panthor_csif_info *csif)
{
   /* First call with zero sizes to learn the group/stream counts. */
   union kbase_ioctl_cs_get_glb_iface req = { 0 };
   if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &req) < 0) {
      mesa_loge("kbase: KBASE_IOCTL_CS_GET_GLB_IFACE (probe) failed: %s",
                strerror(errno));
      return -1;
   }

   uint32_t group_num = req.out.group_num;
   uint32_t total_stream_num = req.out.total_stream_num;

   if (!group_num || !total_stream_num) {
      mesa_loge("kbase: GLB interface reports no queue groups/streams");
      return -1;
   }

   struct basep_cs_group_control *groups =
      calloc(group_num, sizeof(*groups));
   struct basep_cs_stream_control *streams =
      calloc(total_stream_num, sizeof(*streams));
   if (!groups || !streams) {
      free(groups);
      free(streams);
      return -1;
   }

   req.in.max_group_num = group_num;
   req.in.max_total_stream_num = total_stream_num;
   req.in.groups_ptr = (uintptr_t)groups;
   req.in.streams_ptr = (uintptr_t)streams;

   int ret = ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &req);
   if (ret < 0) {
      mesa_loge("kbase: KBASE_IOCTL_CS_GET_GLB_IFACE (fill) failed: %s",
                strerror(errno));
      free(groups);
      free(streams);
      return -1;
   }

   /* Stream features nominally encode the work register count in [7:0]
    * and the scoreboard slot count in [15:8], but the encoding is not
    * reliable across kbase/firmware versions (a Pixel 7 kbase 1.38
    * reports a work register value that underflows the register file the
    * CS compiler was designed for).  Mirror the panthor kernel driver
    * instead, which hardcodes 96 CS registers with 4 unpreserved ones on
    * all shipping CSF parts, and only trust the scoreboard count when it
    * is in the plausible [PANVK-supported] 1..16 range. */
   uint32_t stream_features = streams[0].features;
   uint32_t scoreboard_slot_count = (stream_features >> 8) & 0xff;

   mesa_logd("kbase: GLB iface: version 0x%x, features 0x%x, %u groups, "
             "%u streams (stream 0: features 0x%x, group 0: %u streams, "
             "suspend size %u)",
             req.out.glb_version, req.out.features, group_num,
             total_stream_num, stream_features, groups[0].stream_num,
             groups[0].suspend_size);

   *csif = (struct drm_panthor_csif_info){
      .csg_slot_count = group_num,
      .cs_slot_count = groups[0].stream_num,
      .cs_reg_count = 96,
      .scoreboard_slot_count =
         (scoreboard_slot_count - 1) < 16 ? scoreboard_slot_count : 8,
      /* Number of CS registers the FW may clobber; matches panthor's
       * CSF_UNPRESERVED_REG_COUNT. */
      .unpreserved_cs_reg_count = 4,
   };

   free(groups);
   free(streams);
   return 0;
}

const struct drm_panthor_csif_info *
kbase_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   assert(dev->ops == &kbase_kmod_ops);

   struct kbase_kmod_dev *kbase_dev =
      container_of(dev, struct kbase_kmod_dev, base);

   assert(kbase_dev->is_csf);
   return &kbase_dev->csif_info;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags,
                      UNUSED const struct pan_kmod_driver *drv_info,
                      const struct pan_kmod_allocator *allocator)
{
   /* Version handshake.  This must be the very first ioctl on the fd; all
    * other ioctls return -EPERM until it succeeds.  The CSF flavour
    * (arch >= 10: G610/G710/...) uses ioctl nr 52, the JM flavour
    * (arch <= 9) uses nr 0 — and each flavour rejects the other's number
    * with -EPERM, which is what makes this probe reliable.  We pass 0.0 and
    * let the kernel report the version it implements (CSF: 1.x, JM: 11.x,
    * legacy Midgard: 3.x). */
   struct kbase_ioctl_version_check ver = { 0 };
   bool is_csf = false;

   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK_CSF, &ver) == 0) {
      is_csf = true;
   } else if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK_JM, &ver) == 0) {
      is_csf = false;

      if (ver.major < 11) {
         mesa_loge("kbase: legacy JM driver version %d.%d not supported "
                   "(need >= 11.0)", ver.major, ver.minor);
         return NULL;
      }
   } else {
      /* Not a usable kbase fd (or a device node we can't handshake with).
       * This is an expected outcome when probing device nodes, so keep it
       * quiet. */
      mesa_logd("kbase: version handshake rejected: %s", strerror(errno));
      return NULL;
   }

   mesa_logd("kbase: %s driver, uAPI version %d.%d",
             is_csf ? "CSF" : "JM", ver.major, ver.minor);

   /* Set context creation flags.  Zero for maximum compatibility; this also
    * creates the kernel-side context. */
   struct kbase_ioctl_set_flags set_flags = { .create_flags = 0 };
   if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags)) {
      mesa_loge("kbase: KBASE_IOCTL_SET_FLAGS failed: %s", strerror(errno));
      return NULL;
   }

   /* Map the tracking page.  The kernel requires this before any memory
    * operation. */
   void *tracking_page = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd,
                              BASE_MEM_MAP_TRACKING_HANDLE);
   if (tracking_page == MAP_FAILED) {
      mesa_loge("kbase: mmap(BASE_MEM_MAP_TRACKING_HANDLE) failed: %s",
                strerror(errno));
      return NULL;
   }

   /* Fetch GPU properties. */
   size_t props_size = 0;
   uint8_t *props_buf = kbase_get_gpuprops(fd, &props_size);
   if (!props_buf) {
      mesa_loge("kbase: failed to read GPU properties");
      goto err_unmap_tracking;
   }

   /* Initialise the EXEC_VA zone so that GPU-executable allocations
    * (shader BOs) are possible.  4G of executable VA (0x100000 pages)
    * matches what panfork uses.  Failure is not fatal for enumeration, but
    * executable allocations will fail later, so warn. */
   struct kbase_ioctl_mem_exec_init exec_init = { .va_pages = 0x100000 };
   if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec_init)) {
      mesa_logw("kbase: KBASE_IOCTL_MEM_EXEC_INIT failed: %s "
                "(executable BO allocation will not work)", strerror(errno));
   }

   struct kbase_kmod_dev *kbase_dev =
      pan_kmod_alloc(allocator, sizeof(*kbase_dev));
   if (!kbase_dev) {
      mesa_loge("kbase: failed to allocate kbase_kmod_dev");
      free(props_buf);
      goto err_unmap_tracking;
   }

   /* Report the kernel uAPI version from the handshake (the caller can't
    * use drmGetVersion() on a kbase fd, so drv_info is either NULL or a
    * zeroed fallback). */
   struct pan_kmod_driver kbase_drv = {
      .version = { .major = ver.major, .minor = ver.minor },
   };

   pan_kmod_dev_init(&kbase_dev->base, fd, flags, &kbase_drv,
                     &kbase_kmod_ops, allocator);

   kbase_dev->is_csf = is_csf;
   kbase_dev->tracking_page = tracking_page;
   kbase_dev->next_handle = 1;

   if (is_csf && kbase_query_csif_info(fd, &kbase_dev->csif_info)) {
      mesa_loge("kbase: failed to query the CSF global interface");
      free(props_buf);
      munmap(tracking_page, 4096);
      kbase_dev->base.flags &= ~PAN_KMOD_DEV_FLAG_OWNS_FD;
      pan_kmod_dev_cleanup(&kbase_dev->base);
      pan_kmod_free(allocator, kbase_dev);
      return NULL;
   }

   kbase_dev_query_props(kbase_dev, props_buf, props_size);
   free(props_buf);

   if (!kbase_dev->base.props.gpu_id) {
      mesa_loge("kbase: failed to determine GPU ID from properties");
      munmap(tracking_page, 4096);
      /* On failure the caller keeps ownership of the fd (it closes it on
       * NULL return), so don't let pan_kmod_dev_cleanup() close it too. */
      kbase_dev->base.flags &= ~PAN_KMOD_DEV_FLAG_OWNS_FD;
      pan_kmod_dev_cleanup(&kbase_dev->base);
      pan_kmod_free(allocator, kbase_dev);
      return NULL;
   }

   return &kbase_dev->base;

err_unmap_tracking:
   munmap(tracking_page, 4096);
   return NULL;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   struct kbase_kmod_dev *kbase_dev =
      container_of(dev, struct kbase_kmod_dev, base);

   if (kbase_dev->tracking_page)
      munmap(kbase_dev->tracking_page, 4096);

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
   uint64_t flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR;

   /* Keep GPU cores coherent with each other (panfork does the same). */
   flags |= BASE_MEM_COHERENT_LOCAL;

   if (kmod_flags & PAN_KMOD_BO_FLAG_EXECUTABLE) {
      /* The kernel rejects GPU_EX|GPU_WR (W^X), and executable regions
       * must come from the EXEC_VA zone rather than SAME_VA. */
      flags |= BASE_MEM_PROT_GPU_EX;
      flags &= ~BASE_MEM_PROT_GPU_WR;
   } else {
      /* The kernel forces SAME_VA on non-executable allocations from
       * 64-bit clients anyway; request it explicitly so the behavior is
       * uniform. */
      flags |= BASE_MEM_SAME_VA;
   }

   if (kmod_flags & PAN_KMOD_BO_FLAG_GPU_UNCACHED)
      flags |= BASE_MEM_UNCACHED_GPU;

   if (kmod_flags & PAN_KMOD_BO_FLAG_IO_COHERENT)
      flags |= BASE_MEM_COHERENT_SYSTEM;

   if (kmod_flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)
      flags |= BASE_MEM_GROW_ON_GPF;

   /* Note: no BASE_MEM_CACHED_CPU — CPU mappings are write-combine until
    * cache maintenance (KBASE_IOCTL_MEM_SYNC) is wired up, which keeps
    * things coherent without any flush support. */

   return flags;
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *dev,
                    struct pan_kmod_vm *exclusive_vm, uint64_t size,
                    uint32_t kmod_flags)
{
   struct kbase_kmod_dev *kbase_dev =
      container_of(dev, struct kbase_kmod_dev, base);

   const uint64_t page_size = 4096;
   uint64_t va_pages = (size + page_size - 1) / page_size;

   struct kbase_kmod_bo *kbase_bo =
      pan_kmod_dev_alloc(dev, sizeof(*kbase_bo));
   if (!kbase_bo) {
      mesa_loge("kbase: failed to allocate kbase_kmod_bo");
      return NULL;
   }

   union kbase_ioctl_mem_alloc req = {
      .in = {
         .va_pages     = va_pages,
         .commit_pages = va_pages,
         .flags        = to_kbase_mem_flags(kmod_flags),
      },
   };

   if (kmod_flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) {
      /* Growable region: nothing committed up-front, grown in 2 MB
       * increments on GPU page fault (matches panfork's heap setup). */
      req.in.commit_pages = 0;
      req.in.extension = (2 * 1024 * 1024) / page_size;
   }

   if (ioctl(dev->fd, KBASE_IOCTL_MEM_ALLOC, &req)) {
      mesa_loge("kbase: KBASE_IOCTL_MEM_ALLOC failed: %s", strerror(errno));
      goto err_free_bo;
   }

   kbase_bo->same_va = (req.out.flags & BASE_MEM_SAME_VA) != 0;

   /* Establish the CPU mapping right away:
    *  - for SAME_VA regions out.gpu_va is a cookie, and this mmap() is what
    *    actually creates the region mapping — the returned CPU address IS
    *    the GPU VA;
    *  - for EXEC_VA-zone regions out.gpu_va is already a real GPU VA that
    *    doubles as the mmap offset.
    * The mapping is kept for the whole BO lifetime (see bo_mmap/bo_munmap).
    */
   void *cpu_ptr = mmap(NULL, va_pages * page_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, dev->fd, req.out.gpu_va);
   if (cpu_ptr == MAP_FAILED) {
      mesa_loge("kbase: mmap of BO (cookie/VA 0x%" PRIx64 ", size %" PRIu64
                ") failed: %s", (uint64_t)req.out.gpu_va,
                va_pages * page_size, strerror(errno));

      /* MEM_FREE works on both real VAs and pending cookies. */
      struct kbase_ioctl_mem_free free_req = { .gpu_addr = req.out.gpu_va };
      ioctl(dev->fd, KBASE_IOCTL_MEM_FREE, &free_req);
      goto err_free_bo;
   }

   kbase_bo->cpu_ptr = cpu_ptr;
   kbase_bo->gpu_va = kbase_bo->same_va ? (uintptr_t)cpu_ptr : req.out.gpu_va;

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

   /* For SAME_VA regions, munmap() alone tears the whole region down
    * (free-on-close); a MEM_FREE afterwards would hit a stale VA.  For
    * zone regions (EXEC_VA), munmap() only drops the CPU mapping and the
    * region must be freed explicitly. */
   if (kbase_bo->cpu_ptr)
      munmap(kbase_bo->cpu_ptr, bo->size);

   if (!kbase_bo->same_va) {
      struct kbase_ioctl_mem_free req = { .gpu_addr = kbase_bo->gpu_va };
      if (ioctl(bo->dev->fd, KBASE_IOCTL_MEM_FREE, &req))
         mesa_loge("kbase: KBASE_IOCTL_MEM_FREE failed: %s", strerror(errno));
   }

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

/* The GPU VA doubles as the mmap() file offset in kbase, but BOs are mapped
 * once at allocation time and bo_mmap below returns the cached mapping, so
 * this is only kept as a fallback. */
static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   struct kbase_kmod_bo *kbase_bo =
      container_of(bo, struct kbase_kmod_bo, base);
   return (off_t)kbase_bo->gpu_va;
}

/* Return the CPU mapping established at allocation time.  A SAME_VA region
 * has exactly one CPU mapping (its address is the GPU VA), so we cannot
 * honor requests for a caller-chosen address. */
static void *
kbase_kmod_bo_mmap(struct pan_kmod_bo *bo, UNUSED int prot, UNUSED int flags,
                   void *host_addr)
{
   struct kbase_kmod_bo *kbase_bo =
      container_of(bo, struct kbase_kmod_bo, base);

   if (host_addr != NULL && host_addr != kbase_bo->cpu_ptr) {
      mesa_loge("kbase: mapping a BO at a caller-chosen address is not "
                "supported (SAME_VA)");
      errno = ENOTSUP;
      return MAP_FAILED;
   }

   if (!kbase_bo->cpu_ptr) {
      errno = EINVAL;
      return MAP_FAILED;
   }

   return kbase_bo->cpu_ptr;
}

/* The mapping belongs to the BO and lives until bo_free: unmapping a
 * SAME_VA region would free its GPU mapping too. */
static int
kbase_kmod_bo_munmap(UNUSED struct pan_kmod_bo *bo, UNUSED void *host_addr,
                     UNUSED size_t size)
{
   return 0;
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
 * kbase provides exactly one GPU address space per context and assigns all
 * VAs itself (SAME_VA: the CPU mapping address; EXEC_VA: kernel-chosen).
 * There is no kernel object to create/destroy, so the VM is pure
 * bookkeeping:
 *
 * vm_bind MAP: reports the GPU VA already assigned at allocation time.
 * vm_bind UNMAP: no-op; the unmap happens when the BO is freed.
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

   pan_kmod_vm_init(&kbase_vm->base, dev, 0 /* no kernel handle */,
                    flags, PAN_PGSIZE_4K);
   return &kbase_vm->base;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   struct kbase_kmod_vm *kbase_vm =
      container_of(vm, struct kbase_kmod_vm, base);

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
         uint64_t va = kbase_bo->gpu_va + op->map.bo_offset;

         if (op->va.start == PAN_KMOD_VM_MAP_AUTO_VA) {
            /* kbase assigned the VA at alloc time; report it back. */
            op->va.start = va;
         } else if (op->va.start != va) {
            /* Mapping at a caller-chosen address is impossible in kbase
             * (no vm_bind equivalent).  Fail loudly rather than letting
             * the caller use a VA the GPU doesn't know about. */
            mesa_loge("kbase: cannot map BO at explicit VA 0x%" PRIx64
                      " (kbase-assigned VA is 0x%" PRIx64 ")",
                      op->va.start, va);
            return -1;
         }
      } else if (op->type == PAN_KMOD_VM_OP_TYPE_UNMAP) {
         /* Unmap happens automatically when the BO is freed. */
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
   union kbase_ioctl_get_cpu_gpu_timeinfo req = {
      .in.request_flags = BASE_TIMEINFO_TIMESTAMP_FLAG,
   };

   if (ioctl(dev->fd, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, &req) == 0)
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
   .bo_mmap                = kbase_kmod_bo_mmap,
   .bo_munmap              = kbase_kmod_bo_munmap,
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
