/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "drm-uapi/panthor_drm.h"

#include "genxml/cs_builder.h"
#include "genxml/decode.h"

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_device_memory.h"
#include "panvk_macros.h"
#include "panvk_queue.h"
#include "panvk_utrace.h"

#include "pan_trace.h"

#include "util/bitscan.h"
#include "util/os_time.h"
#include "vk_drm_syncobj.h"
#include "vk_log.h"
#include "vk_sync.h"

#ifdef HAVE_PAN_KMOD_KBASE
#include <inttypes.h>
#include <unistd.h>
#include "drm-uapi/mali_kbase_ioctl.h"
#include "kmod/kbase_kmod.h"
#endif

#define MIN_DESC_TRACEBUF_SIZE (128 * 1024)
#define DEFAULT_DESC_TRACEBUF_SIZE (2 * 1024 * 1024)
#define MIN_CS_TRACEBUF_SIZE (512 * 1024)
#define DEFAULT_CS_TRACEBUF_SIZE (2 * 1024 * 1024)

#ifdef HAVE_PAN_KMOD_KBASE

/* kbase-backed queue support.
 *
 * On panthor, the kernel owns a per-queue ring buffer: every
 * drm_panthor_queue_submit makes the kernel emit a small
 * flush+CALL+sync sequence into that ring.  kbase has no such
 * per-submission ioctl — userspace owns the ring — so we emit the
 * equivalent sequence ourselves, publish the new insert offset through
 * the USER_IO input page and kick the scheduler.
 *
 * Synchronization is currently fully synchronous: every submission is
 * CPU-waited (by polling a per-subqueue seqno cell bumped by a deferred
 * SYNC_ADD64 that waits on all scoreboard slots first, mirroring the
 * sequence the panthor kernel emits), and semaphore waits/signals are
 * resolved on the CPU.  Asynchronous submission needs a real kbase
 * fence/event integration and is left for later.
 */

#define KBASE_RINGBUF_SIZE     (64 * 1024)
/* Worst-case size of one ring entry, in bytes. */
#define KBASE_RING_JOB_MAX_SIZE 256
/* Generous timeout for the synchronous submission model. */
#define KBASE_WAIT_TIMEOUT_NS  (60ll * 1000000000ll)

static bool
gpu_queue_uses_kbase(const struct panvk_device *dev)
{
   return to_panvk_physical_device(dev->vk.physical)->kbase_node_path[0] !=
          '\0';
}

static uint32_t
kbase_seqno_stride(void)
{
   return ALIGN_POT(sizeof(struct panvk_cs_sync64), 64);
}

static volatile struct panvk_cs_sync64 *
kbase_subqueue_seqno_cell(struct panvk_gpu_queue *queue, uint32_t subqueue)
{
   uint8_t *base = panvk_priv_mem_host_addr(queue->kbase_seqnos);

   return (volatile struct panvk_cs_sync64 *)(base +
                                              subqueue *
                                                 kbase_seqno_stride());
}

static uint64_t
kbase_subqueue_seqno_dev_addr(struct panvk_gpu_queue *queue, uint32_t subqueue)
{
   return panvk_priv_mem_dev_addr(queue->kbase_seqnos) +
          subqueue * kbase_seqno_stride();
}

/* Emit one job into the subqueue ring: cache maintenance, a CALL to the
 * command stream, then a SYNC_ADD64 on the subqueue seqno cell deferred on
 * all scoreboard slots — the same sequence the panthor kernel driver emits
 * into its kernel-owned rings.  Only the FW-unpreserved registers (the top
 * 4 of the register file) are clobbered, which the rest of the driver
 * stays away from. */
static VkResult
kbase_subqueue_emit_job(struct panvk_gpu_queue *queue, uint32_t subqueue,
                        uint64_t stream_addr, uint32_t stream_size,
                        uint32_t flush_id)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct drm_panthor_csif_info *csif_info = panvk_get_csif_props(dev);
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];

   uint32_t offset = subq->kbase.insert % KBASE_RINGBUF_SIZE;

   /* If the entry would straddle the end of the ring, pad with NOPs
    * (zero-filled instructions) and restart at the beginning. */
   if (offset + KBASE_RING_JOB_MAX_SIZE > KBASE_RINGBUF_SIZE) {
      memset((uint8_t *)subq->kbase.ringbuf_cpu + offset, 0,
             KBASE_RINGBUF_SIZE - offset);
      subq->kbase.insert += KBASE_RINGBUF_SIZE - offset;
      offset = 0;
   }

   struct cs_buffer ring_buf = {
      .cpu = (uint64_t *)((uint8_t *)subq->kbase.ringbuf_cpu + offset),
      .gpu = subq->kbase.ringbuf_dev + offset,
      .capacity = KBASE_RING_JOB_MAX_SIZE / sizeof(uint64_t),
   };
   struct cs_builder_conf conf = {
      .nr_registers = csif_info->cs_reg_count,
      .ls_sb_slot = SB_ID(LS),
   };
   struct cs_builder b;

   cs_builder_init(&b, &conf, ring_buf);

   /* The ring sequence may only clobber the FW-unpreserved registers (the
    * top 4), but cs_builder_init() reserves at least 3 registers for its
    * own chunk linking — which our fixed-size ring entries never trigger —
    * and cs_reg_tuple() refuses to hand those out.  Construct the indices
    * directly instead. */
   uint32_t reg = csif_info->cs_reg_count - 4;
   struct cs_index addr64 = {
      .type = CS_INDEX_REGISTER,
      .size = 2,
      .reg = reg,
   };
   struct cs_index val32 = {
      .type = CS_INDEX_REGISTER,
      .size = 1,
      .reg = reg + 2,
   };
   struct cs_index val64 = {
      .type = CS_INDEX_REGISTER,
      .size = 2,
      .reg = reg + 2,
   };

   if (stream_size) {
      /* Make CPU-written command-stream/descriptor memory visible to the
       * GPU before calling into it. */
      cs_move32_to(&b, val32, flush_id);
      cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                      MALI_CS_OTHER_FLUSH_MODE_NONE, val32,
                      cs_defer(0, SB_ID(IMM_FLUSH)));
      cs_wait_slot(&b, SB_ID(IMM_FLUSH));

      cs_move64_to(&b, addr64, stream_addr);
      cs_move32_to(&b, val32, stream_size);
      cs_call(&b, addr64, val32);
   }

   /* Bump the subqueue seqno once all prior operations retired. */
   cs_move64_to(&b, addr64, kbase_subqueue_seqno_dev_addr(queue, subqueue));
   cs_move64_to(&b, val64, 1);
   cs_sync64_add(&b, true, MALI_CS_SYNC_SCOPE_SYSTEM, val64, addr64,
                 cs_defer(dev->csf.sb.all_mask, SB_ID(DEFERRED_SYNC)));

   cs_end(&b);

   if (!cs_is_valid(&b))
      return panvk_errorf(dev, VK_ERROR_UNKNOWN,
                          "kbase: CS ring emission failed");

   assert(cs_root_chunk_size(&b) <= KBASE_RING_JOB_MAX_SIZE);

   subq->kbase.insert += cs_root_chunk_size(&b);
   subq->kbase.emitted_jobs++;
   return VK_SUCCESS;
}

/* Publish the new insert offset in the USER_IO input page and kick the
 * scheduler. */
static void
kbase_subqueue_publish(struct panvk_gpu_queue *queue, uint32_t subqueue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];
   uint8_t *input_page = (uint8_t *)subq->kbase.user_io + 4096;

   /* Ring contents must be visible before the insert offset moves. */
   __sync_synchronize();

   *(volatile uint64_t *)(input_page + CS_USER_IO_INPUT_CS_INSERT) =
      subq->kbase.insert;

   __sync_synchronize();

   kbase_kmod_csf_queue_kick(dev->kmod.dev, subq->kbase.ringbuf_dev);
}

static VkResult
kbase_subqueue_wait_idle(struct panvk_gpu_queue *queue, uint32_t subqueue)
{
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];
   volatile struct panvk_cs_sync64 *cell =
      kbase_subqueue_seqno_cell(queue, subqueue);
   int64_t start = os_time_get_nano();

   while (cell->seqno < subq->kbase.emitted_jobs) {
      if (cell->error)
         return vk_queue_set_lost(&queue->vk,
                                  "kbase: CS error %" PRIu64 " on subqueue %u",
                                  (uint64_t)cell->error, subqueue);

      if (os_time_get_nano() - start > KBASE_WAIT_TIMEOUT_NS) {
         const uint8_t *output_page = (uint8_t *)subq->kbase.user_io + 8192;
         uint64_t extract = *(volatile uint64_t *)(output_page +
                                                   CS_USER_IO_OUTPUT_CS_EXTRACT);

         return vk_queue_set_lost(&queue->vk,
                                  "kbase: timeout on subqueue %u "
                                  "(seqno %" PRIu64 "/%" PRIu64
                                  ", insert %" PRIu64 ", extract %" PRIu64 ")",
                                  subqueue, (uint64_t)cell->seqno,
                                  subq->kbase.emitted_jobs, subq->kbase.insert,
                                  extract);
      }

      usleep(100);
   }

   return VK_SUCCESS;
}

static void
kbase_destroy_group(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct panvk_subqueue *subq = &queue->subqueues[i];

      if (subq->kbase.user_io)
         kbase_kmod_csf_queue_term(dev->kmod.dev, subq->kbase.ringbuf_dev,
                                   subq->kbase.user_io);

      pan_kmod_bo_put(subq->kbase.ringbuf_bo);
      subq->kbase.ringbuf_bo = NULL;
      subq->kbase.user_io = NULL;
   }

   if (queue->group_handle != UINT32_MAX)
      kbase_kmod_csf_group_destroy(dev->kmod.dev, queue->group_handle);
}

static VkResult
kbase_create_group(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   VkResult result;

   queue->group_handle = UINT32_MAX;

   uint32_t group_handle;
   if (kbase_kmod_csf_group_create(dev->kmod.dev, PANVK_SUBQUEUE_COUNT,
                                   &group_handle))
      return panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                          "Failed to create a kbase queue group");

   queue->group_handle = group_handle;

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct panvk_subqueue *subq = &queue->subqueues[i];

      subq->kbase.ringbuf_bo =
         pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, KBASE_RINGBUF_SIZE,
                           PAN_KMOD_BO_FLAG_GPU_UNCACHED);
      if (!subq->kbase.ringbuf_bo) {
         result = panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                               "Failed to allocate a kbase CS ring buffer");
         goto err_destroy_group;
      }

      subq->kbase.ringbuf_cpu = pan_kmod_bo_mmap(
         subq->kbase.ringbuf_bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
      if (subq->kbase.ringbuf_cpu == MAP_FAILED) {
         subq->kbase.ringbuf_cpu = NULL;
         result = panvk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "Failed to map a kbase CS ring buffer");
         goto err_destroy_group;
      }

      struct pan_kmod_vm_op op = {
         .type = PAN_KMOD_VM_OP_TYPE_MAP,
         .va = {
            .start = PAN_KMOD_VM_MAP_AUTO_VA,
            .size = KBASE_RINGBUF_SIZE,
         },
         .map = {
            .bo = subq->kbase.ringbuf_bo,
            .bo_offset = 0,
         },
      };
      if (pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op,
                           1)) {
         result = panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                               "Failed to GPU map a kbase CS ring buffer");
         goto err_destroy_group;
      }
      subq->kbase.ringbuf_dev = op.va.start;

      subq->kbase.user_io = kbase_kmod_csf_queue_bind(
         dev->kmod.dev, queue->group_handle, i, subq->kbase.ringbuf_dev,
         KBASE_RINGBUF_SIZE);
      if (!subq->kbase.user_io) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to bind a kbase CS queue");
         goto err_destroy_group;
      }

      subq->kbase.insert = 0;
      subq->kbase.emitted_jobs = 0;
   }

   return VK_SUCCESS;

err_destroy_group:
   kbase_destroy_group(queue);
   return result;
}

#endif /* HAVE_PAN_KMOD_KBASE */

static void
finish_render_desc_ringbuf(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const bool tracing_enabled = PANVK_DEBUG(TRACE);
   struct panvk_desc_ringbuf *ringbuf = &queue->render_desc_ringbuf;

   panvk_pool_free_mem(&ringbuf->syncobj);

   if (dev->debug.decode_ctx && ringbuf->addr.dev) {
      pandecode_inject_free(dev->debug.decode_ctx, ringbuf->addr.dev,
                            ringbuf->size);
      if (!tracing_enabled)
         pandecode_inject_free(dev->debug.decode_ctx,
                               ringbuf->addr.dev + ringbuf->size,
                               ringbuf->size);
   }

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      /* The BO's own mapping goes away with the BO; only the alias region
       * (not created in tracing mode) needs explicit teardown. */
      if (ringbuf->addr.dev && !tracing_enabled)
         kbase_kmod_alias_destroy(dev->kmod.dev, ringbuf->addr.dev,
                                  ringbuf->size, 2);
   } else
#endif
   if (ringbuf->addr.dev) {
      struct pan_kmod_vm_op op = {
         .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
         .va = {
            .start = ringbuf->addr.dev,
            .size = ringbuf->size * (tracing_enabled ? 2 : 1),
         },
      };

      ASSERTED int ret =
         pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
      assert(!ret);

      panvk_as_free(dev, dev->as.priv_heap, ringbuf->addr.dev,
                    ringbuf->size * 2);
   }

   if (ringbuf->addr.host) {
      ASSERTED int ret =
         pan_kmod_bo_munmap(ringbuf->bo, ringbuf->addr.host, ringbuf->size);
      assert(!ret);
   }

   pan_kmod_bo_put(ringbuf->bo);
}

static VkResult
init_render_desc_ringbuf(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const bool tracing_enabled = PANVK_DEBUG(TRACE);
   uint32_t flags = panvk_device_adjust_bo_flags(dev, PAN_KMOD_BO_FLAG_NO_MMAP);
   struct panvk_desc_ringbuf *ringbuf = &queue->render_desc_ringbuf;
   uint64_t dev_addr = 0;
   int ret;

   if (tracing_enabled) {
      ringbuf->size = debug_get_num_option("PANVK_DESC_TRACEBUF_SIZE",
                                           DEFAULT_DESC_TRACEBUF_SIZE);
      flags |= PAN_KMOD_BO_FLAG_GPU_UNCACHED;
      assert(ringbuf->size > MIN_DESC_TRACEBUF_SIZE &&
             util_is_power_of_two_nonzero(ringbuf->size));
   } else {
      ringbuf->size = RENDER_DESC_RINGBUF_SIZE;
   }

   ringbuf->bo =
      pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, ringbuf->size, flags);
   if (!ringbuf->bo)
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to create a descriptor ring buffer context");

   if (!(flags & PAN_KMOD_BO_FLAG_NO_MMAP)) {
      ringbuf->addr.host = pan_kmod_bo_mmap(ringbuf->bo, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, NULL);
      if (ringbuf->addr.host == MAP_FAILED)
         return panvk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                             "Failed to CPU map ringbuf BO");
   }

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      /* kbase assigns the BO VA itself; report it back through an AUTO_VA
       * map op. */
      struct pan_kmod_vm_op map_op = {
         .type = PAN_KMOD_VM_OP_TYPE_MAP,
         .va = {
            .start = PAN_KMOD_VM_MAP_AUTO_VA,
            .size = ringbuf->size,
         },
         .map = {
            .bo = ringbuf->bo,
            .bo_offset = 0,
         },
      };
      ret = pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE,
                             &map_op, 1);
      if (ret)
         return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                             "Failed to GPU map ringbuf BO");

      if (tracing_enabled) {
         /* No wraparound mirror needed (and no guard page support). */
         ringbuf->addr.dev = map_op.va.start;
      } else {
         /* Mapping one BO twice back-to-back at a chosen address is not
          * possible on kbase; use a MEM_ALIAS region instead.  The helper
          * guarantees the mapping never crosses a 4G boundary, so the
          * wraparound can be encoded with 32-bit operations. */
         ringbuf->addr.dev = kbase_kmod_alias_create(
            dev->kmod.dev, map_op.va.start, ringbuf->size, 2);
         if (!ringbuf->addr.dev)
            return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "Failed to GPU map ringbuf BO (alias)");
      }
      goto ringbuf_mapped;
   }
#endif

   /* We choose the alignment to guarantee that we won't ever cross a 4G
    * boundary when accessing the mapping. This way we can encode the wraparound
    * using 32-bit operations. */
   dev_addr = panvk_as_alloc(dev, dev->as.priv_heap, ringbuf->size * 2,
                             ringbuf->size * 2);

   if (!dev_addr)
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address for ringbuf BO");

   struct pan_kmod_vm_op vm_ops[] = {
      {
         .type = PAN_KMOD_VM_OP_TYPE_MAP,
         .va = {
            .start = dev_addr,
            .size = ringbuf->size,
         },
         .map = {
            .bo = ringbuf->bo,
            .bo_offset = 0,
         },
      },
      {
         .type = PAN_KMOD_VM_OP_TYPE_MAP,
         .va = {
            .start = dev_addr + ringbuf->size,
            .size = ringbuf->size,
         },
         .map = {
            .bo = ringbuf->bo,
            .bo_offset = 0,
         },
      },
   };

   /* If tracing is enabled, we keep the second part of the mapping unmapped
    * to serve as a guard region. */
   ret = pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, vm_ops,
                          tracing_enabled ? 1 : ARRAY_SIZE(vm_ops));
   if (ret) {
      panvk_as_free(dev, dev->as.priv_heap, dev_addr, ringbuf->size * 2);
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to GPU map ringbuf BO");
   }

   ringbuf->addr.dev = dev_addr;

#ifdef HAVE_PAN_KMOD_KBASE
ringbuf_mapped:
#endif
   if (dev->debug.decode_ctx) {
      pandecode_inject_mmap(dev->debug.decode_ctx, ringbuf->addr.dev,
                            ringbuf->addr.host, ringbuf->size, NULL);
      if (!tracing_enabled)
         pandecode_inject_mmap(dev->debug.decode_ctx,
                               ringbuf->addr.dev + ringbuf->size,
                               ringbuf->addr.host, ringbuf->size, NULL);
   }

   struct panvk_pool_alloc_info alloc_info = {
      .size = sizeof(struct panvk_cs_sync32),
      .alignment = 64,
   };

   ringbuf->syncobj = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   if (!panvk_priv_mem_check_alloc(ringbuf->syncobj))
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to create the render desc ringbuf context");

   panvk_priv_mem_write(ringbuf->syncobj, 0, struct panvk_cs_sync32, syncobj) {
      *syncobj = (struct panvk_cs_sync32){
         .seqno = RENDER_DESC_RINGBUF_SIZE,
      };
   }

   return VK_SUCCESS;
}

static void
finish_subqueue_tracing(struct panvk_gpu_queue *queue,
                        enum panvk_subqueue_id subqueue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];

   if (subq->tracebuf.addr.dev) {
      uint64_t pgsize = panvk_get_gpu_page_size(dev);

      pandecode_inject_free(dev->debug.decode_ctx, subq->tracebuf.addr.dev,
                            subq->tracebuf.size);

      struct pan_kmod_vm_op op = {
         .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
         .va = {
            .start = subq->tracebuf.addr.dev,
            .size = subq->tracebuf.size,
         },
      };

      ASSERTED int ret =
         pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
      assert(!ret);

      panvk_as_free(dev, dev->as.priv_heap, subq->tracebuf.addr.dev,
                    subq->tracebuf.size + pgsize);
   }

   if (subq->tracebuf.addr.host) {
      ASSERTED int ret =
         pan_kmod_bo_munmap(subq->tracebuf.bo, subq->tracebuf.addr.host,
                            subq->tracebuf.size);
      assert(!ret);
   }

   pan_kmod_bo_put(subq->tracebuf.bo);

   vk_free(&dev->vk.alloc, subq->reg_file);
}

static VkResult
init_subqueue_tracing(struct panvk_gpu_queue *queue,
                      enum panvk_subqueue_id subqueue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];
   uint64_t dev_addr;

   if (!PANVK_DEBUG(TRACE))
      return VK_SUCCESS;

   subq->reg_file =
      vk_zalloc(&dev->vk.alloc, sizeof(uint32_t) * 256, sizeof(uint64_t),
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!subq->reg_file)
      return panvk_errorf(dev->vk.physical, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "Failed to allocate reg file cache");

   subq->tracebuf.size = debug_get_num_option("PANVK_CS_TRACEBUF_SIZE",
                                              DEFAULT_CS_TRACEBUF_SIZE);
   assert(subq->tracebuf.size > MIN_CS_TRACEBUF_SIZE &&
          util_is_power_of_two_nonzero(subq->tracebuf.size));

   subq->tracebuf.bo =
      pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, subq->tracebuf.size,
                        PAN_KMOD_BO_FLAG_GPU_UNCACHED);
   if (!subq->tracebuf.bo)
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to create a CS tracebuf");

   subq->tracebuf.addr.host = pan_kmod_bo_mmap(
      subq->tracebuf.bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
   if (subq->tracebuf.addr.host == MAP_FAILED) {
      subq->tracebuf.addr.host = NULL;
      return panvk_errorf(dev, VK_ERROR_OUT_OF_HOST_MEMORY,
                          "Failed to CPU map tracebuf");
   }

   /* Add a guard page. */
   uint64_t pgsize = panvk_get_gpu_page_size(dev);
   dev_addr = panvk_as_alloc(dev, dev->as.priv_heap,
                             subq->tracebuf.size + pgsize, pgsize);

   if (!dev_addr)
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address for tracebuf");

   struct pan_kmod_vm_op vm_op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = dev_addr,
         .size = subq->tracebuf.size,
      },
      .map = {
         .bo = subq->tracebuf.bo,
         .bo_offset = 0,
      },
   };

   /* If tracing is enabled, we keep the second part of the mapping unmapped
    * to serve as a guard region. */
   int ret =
      pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &vm_op, 1);
   if (ret) {
      panvk_as_free(dev, dev->as.priv_heap, dev_addr,
                    subq->tracebuf.size + pgsize);
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to GPU map ringbuf BO");
   }

   subq->tracebuf.addr.dev = dev_addr;

   if (dev->debug.decode_ctx) {
      pandecode_inject_mmap(dev->debug.decode_ctx, subq->tracebuf.addr.dev,
                            subq->tracebuf.addr.host, subq->tracebuf.size,
                            NULL);
   }

   return VK_SUCCESS;
}

static void
finish_subqueue(struct panvk_gpu_queue *queue, enum panvk_subqueue_id subqueue)
{
   panvk_pool_free_mem(&queue->subqueues[subqueue].context);
   panvk_pool_free_mem(&queue->subqueues[subqueue].req_resource.buf);
   panvk_pool_free_mem(&queue->subqueues[subqueue].regs_save);
   finish_subqueue_tracing(queue, subqueue);
}

static VkResult
init_utrace(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   VkResult result;

   const struct vk_sync_type *sync_type = phys_dev->sync_types[0];

#ifdef HAVE_PAN_KMOD_KBASE
   /* kbase queues use the synchronous submission model and never process
    * utrace on the GPU timeline (timestamp_frequency is 0 there), so no
    * utrace sync object is needed. */
   if (gpu_queue_uses_kbase(dev))
      return VK_SUCCESS;
#endif

   /* A DRM-backed timeline sync is required for CSF queue operation. */
   if (!sync_type || !vk_sync_type_is_drm_syncobj(sync_type) ||
       !(sync_type->features & VK_SYNC_FEATURE_TIMELINE)) {
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                       "panvk CSF: timeline DRM syncobj required for queue "
                       "creation");
   }

   result = vk_sync_create(&dev->vk, sync_type, VK_SYNC_IS_TIMELINE, 0,
                           &queue->utrace.sync);
   if (result != VK_SUCCESS)
      return result;

   queue->utrace.next_value = 1;

   return VK_SUCCESS;
}

static uint32_t
get_resource_mask(enum panvk_subqueue_id subqueue)
{
   switch (subqueue) {
   case PANVK_SUBQUEUE_VERTEX_TILER:
      return CS_IDVS_RES | CS_TILER_RES;
   case PANVK_SUBQUEUE_FRAGMENT:
      return CS_FRAG_RES;
   case PANVK_SUBQUEUE_COMPUTE:
      return CS_COMPUTE_RES;
   default:
      UNREACHABLE("Unknown subqueue");
   }
}

static VkResult
init_subqueue(struct panvk_gpu_queue *queue, enum panvk_subqueue_id subqueue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_subqueue *subq = &queue->subqueues[subqueue];
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(queue->vk.base.device->physical);

   VkResult result = init_subqueue_tracing(queue, subqueue);
   if (result != VK_SUCCESS)
      return result;

   struct panvk_pool_alloc_info alloc_info;

   if (dev->dump_region_size[subqueue]) {
      alloc_info.size = dev->dump_region_size[subqueue];
      alloc_info.alignment = sizeof(uint32_t);
      subq->regs_save = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
      if (!panvk_priv_mem_check_alloc(subq->regs_save)) {
         return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                             "Failed to allocate register save area");
      }
   }

   /* When tracing is enabled, we want to use a non-cached pool, so can get
    * up-to-date context even if the CS crashed in the middle. */
   struct panvk_pool *mempool =
      PANVK_DEBUG(TRACE) ? &dev->mempools.rw_nc : &dev->mempools.rw;

   alloc_info.size = sizeof(uint64_t);
   alloc_info.alignment = 64;
   subq->req_resource.buf = panvk_pool_alloc_mem(mempool, alloc_info);
   if (!panvk_priv_mem_check_alloc(subq->req_resource.buf))
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to create a req_resource buffer");

   struct cs_builder b;
   const struct drm_panthor_csif_info *csif_info =
      panvk_get_csif_props(dev);

   struct cs_buffer root_cs = {
      .cpu = panvk_priv_mem_host_addr(subq->req_resource.buf),
      .gpu = panvk_priv_mem_dev_addr(subq->req_resource.buf),
      .capacity = 1,
   };
   struct cs_builder_conf conf = {
      .nr_registers = csif_info->cs_reg_count,
      .nr_kernel_registers = MAX2(csif_info->unpreserved_cs_reg_count, 4),
      .ls_sb_slot = SB_ID(LS),
   };

   cs_builder_init(&b, &conf, root_cs);
   cs_req_res(&b, get_resource_mask(subqueue));
   cs_end(&b);
   assert(cs_is_valid(&b));
   subq->req_resource.cs_buffer_size = cs_root_chunk_size(&b);
   subq->req_resource.cs_buffer_addr = cs_root_chunk_gpu_addr(&b);
   cs_builder_fini(&b);
   panvk_priv_mem_flush(subq->req_resource.buf, 0,
                        subq->req_resource.cs_buffer_size);

   alloc_info.size = sizeof(struct panvk_cs_subqueue_context);
   alloc_info.alignment = 64;

   subq->context = panvk_pool_alloc_mem(mempool, alloc_info);
   if (!panvk_priv_mem_check_alloc(subq->context))
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to create a queue context");

   panvk_priv_mem_write(subq->context, 0, struct panvk_cs_subqueue_context,
                        cs_ctx) {
      *cs_ctx = (struct panvk_cs_subqueue_context){
         .syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs),
         .debug.tracebuf.cs = subq->tracebuf.addr.dev,
#if PAN_ARCH == 10
         /* On the VT/COMPUTE queue, the first iter_sb will skipped since
          * cs_next_iter_sb() is called before the first use, but that's okay,
          * because the next slot will be equally free, and the skipped one will
          * be re-used at some point.
          * On the fragment queue, we increment the iterator when the
          * FINISH_FRAGMENT job is issued, which is why we need this value
          * to point to a valid+free scoreboard from the start.
          */
         .iter_sb = SB_ITER(0),
#endif
         .reg_dump_addr = panvk_priv_mem_dev_addr(subq->regs_save),
      };

      if (subqueue != PANVK_SUBQUEUE_COMPUTE) {
         cs_ctx->render.tiler_heap =
            panvk_priv_mem_dev_addr(queue->tiler_heap.desc);
         /* Our geometry buffer comes 4k after the tiler heap, and we encode the
          * size in the lower 12 bits so the address can be copied directly
          * to the tiler descriptors. */
         cs_ctx->render.geom_buf =
            (cs_ctx->render.tiler_heap + 4096) | ((64 * 1024) >> 12);

         /* Initialize the ringbuf */
         cs_ctx->render.desc_ringbuf = (struct panvk_cs_desc_ringbuf){
            .syncobj =
               panvk_priv_mem_dev_addr(queue->render_desc_ringbuf.syncobj),
            .ptr = queue->render_desc_ringbuf.addr.dev,
            .pos = 0,
         };
      }

      if (subqueue == PANVK_SUBQUEUE_FRAGMENT) {
         /* The tiler OOM exception handler is registered to the fragment
          * queue, so the scratch FBD buffer is only needed there. We leave
          * it to NULL on other queues to make sure any attempt to access it
          * results in a NULL deref that can be caught.
          */
         cs_ctx->tiler_oom_ctx.ir_scratch_fbd_ptr =
            panvk_priv_mem_dev_addr(queue->tiler_heap.oom_fbd);
      }
   }

   /* We use the geometry buffer for our temporary CS buffer. */
   root_cs = (struct cs_buffer){
      .cpu = panvk_priv_mem_host_addr(queue->tiler_heap.desc) + 4096,
      .gpu = panvk_priv_mem_dev_addr(queue->tiler_heap.desc) + 4096,
      .capacity = 64 * 1024 / sizeof(uint64_t),
   };
   conf = (struct cs_builder_conf){
      .nr_registers = csif_info->cs_reg_count,
      .nr_kernel_registers = MAX2(csif_info->unpreserved_cs_reg_count, 4),
      .ls_sb_slot = SB_ID(LS),
   };

   assert(panvk_priv_mem_dev_addr(queue->tiler_heap.desc) != 0);

   cs_builder_init(&b, &conf, root_cs);
   /* Pass the context. */
   cs_move64_to(&b, cs_subqueue_ctx_reg(&b),
                panvk_priv_mem_dev_addr(subq->context));

   /* Intialize scoreboard slots used for asynchronous operations. */
#if PAN_ARCH >= 11
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, SB_ITER(0));
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_WAIT, SB_WAIT_ITER(0));
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, SB_ID(LS));
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_DEFERRED,
                      SB_ID(DEFERRED_SYNC));
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_STREAM,
                      dev->csf.sb.all_iters_mask & ~SB_WAIT_ITER(0));
#else
   cs_set_scoreboard_entry(&b, SB_ITER(0), SB_ID(LS));
#endif

   /* We do greater than test on sync objects, and given the reference seqno
    * registers are all zero at init time, we need to initialize all syncobjs
    * with a seqno of one. */
   panvk_priv_mem_write(queue->syncobjs,
                        subqueue * sizeof(struct panvk_cs_sync64),
                        struct panvk_cs_sync64, syncobj) {
      syncobj->seqno = 1;
   }

   if (subqueue != PANVK_SUBQUEUE_COMPUTE) {
      struct cs_index heap_ctx_addr = cs_scratch_reg64(&b, 0);

      /* Pre-set the heap context on the vertex-tiler/fragment queues. */
      cs_move64_to(&b, heap_ctx_addr, queue->tiler_heap.context.dev_addr);
      cs_heap_set(&b, heap_ctx_addr);
   }
   cs_end(&b);

   assert(cs_is_valid(&b));

   panvk_priv_mem_flush(queue->tiler_heap.desc, 4096, cs_root_chunk_size(&b));

   struct drm_panthor_sync_op syncop = {
      .flags =
         DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ | DRM_PANTHOR_SYNC_OP_SIGNAL,
      .handle = queue->syncobj_handle,
      .timeline_value = 0,
   };
   struct drm_panthor_queue_submit qsubmit = {
      .queue_index = subqueue,
      .stream_size = cs_root_chunk_size(&b),
      .stream_addr = cs_root_chunk_gpu_addr(&b),
      .latest_flush = panvk_get_flush_id(dev),
      .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &syncop),
   };
   struct drm_panthor_group_submit gsubmit = {
      .group_handle = queue->group_handle,
      .queue_submits = DRM_PANTHOR_OBJ_ARRAY(1, &qsubmit),
   };

   cs_builder_fini(&b);

   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      VkResult res =
         kbase_subqueue_emit_job(queue, subqueue, qsubmit.stream_addr,
                                 qsubmit.stream_size, qsubmit.latest_flush);
      if (res == VK_SUCCESS) {
         kbase_subqueue_publish(queue, subqueue);
         res = kbase_subqueue_wait_idle(queue, subqueue);
      }
      if (res != VK_SUCCESS)
         return panvk_errorf(dev->vk.physical, VK_ERROR_INITIALIZATION_FAILED,
                             "Failed to initialize subqueue");
   } else
#endif
   {
      int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT,
                               &gsubmit);
      if (ret)
         return panvk_errorf(dev->vk.physical, VK_ERROR_INITIALIZATION_FAILED,
                             "Failed to initialized subqueue: %m");

      ret = drmSyncobjWait(dev->drm_fd, &queue->syncobj_handle, 1, INT64_MAX,
                           0, NULL);
      if (ret)
         return panvk_errorf(dev->vk.physical, VK_ERROR_INITIALIZATION_FAILED,
                             "SyncobjWait failed: %m");

      drmSyncobjReset(dev->drm_fd, &queue->syncobj_handle, 1);
   }

   if (PANVK_DEBUG(TRACE)) {
      pandecode_user_msg(dev->debug.decode_ctx, "Init subqueue %d binary\n\n",
                         subqueue);
      pandecode_cs_binary(dev->debug.decode_ctx, qsubmit.stream_addr,
                          qsubmit.stream_size,
                          phys_dev->kmod.dev->props.gpu_id);
   }

   return VK_SUCCESS;
}

static void
cleanup_queue(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      finish_subqueue(queue, i);

   if (queue->utrace.sync)
      vk_sync_destroy(&dev->vk, queue->utrace.sync);

   finish_render_desc_ringbuf(queue);

   panvk_pool_free_mem(&queue->syncobjs);
#ifdef HAVE_PAN_KMOD_KBASE
   panvk_pool_free_mem(&queue->kbase_seqnos);
#endif
}

static VkResult
init_queue(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   VkResult result;

   struct panvk_pool_alloc_info alloc_info = {
      .size =
         ALIGN_POT(sizeof(struct panvk_cs_sync64), 64) * PANVK_SUBQUEUE_COUNT,
      .alignment = 64,
   };

   queue->syncobjs = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   if (!panvk_priv_mem_check_alloc(queue->syncobjs))
      return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate subqueue sync objects");

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      queue->kbase_seqnos = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
      if (!panvk_priv_mem_check_alloc(queue->kbase_seqnos))
         return panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                             "Failed to allocate kbase seqno cells");

      memset(panvk_priv_mem_host_addr(queue->kbase_seqnos), 0,
             alloc_info.size);
   }
#endif

   result = init_render_desc_ringbuf(queue);
   if (result != VK_SUCCESS)
      goto err_cleanup_queue;

   result = init_utrace(queue);
   if (result != VK_SUCCESS)
      goto err_cleanup_queue;

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      result = init_subqueue(queue, i);
      if (result != VK_SUCCESS)
         goto err_cleanup_queue;
   }

   if (PANVK_DEBUG(TRACE))
      pandecode_next_frame(dev->debug.decode_ctx);

   return VK_SUCCESS;

err_cleanup_queue:
   cleanup_queue(queue);
   return result;
}

static VkResult
create_group(struct panvk_gpu_queue *queue,
             enum drm_panthor_group_priority group_priority,
             uint32_t shader_core_count)
{
   const struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(queue->vk.base.device->physical);

   struct drm_panthor_queue_create qc[] = {
      [PANVK_SUBQUEUE_VERTEX_TILER] =
         {
            .priority = 1,
            .ringbuf_size = 64 * 1024,
         },
      [PANVK_SUBQUEUE_FRAGMENT] =
         {
            .priority = 1,
            .ringbuf_size = 64 * 1024,
         },
      [PANVK_SUBQUEUE_COMPUTE] =
         {
            .priority = 1,
            .ringbuf_size = 64 * 1024,
         },
   };

   uint8_t max_compute_cores = util_bitcount64(phys_dev->compute_core_mask);
   uint8_t max_fragment_cores = util_bitcount64(phys_dev->fragment_core_mask);

   if (shader_core_count) {
      max_compute_cores = MIN2(shader_core_count, max_compute_cores);
      max_fragment_cores = MIN2(shader_core_count, max_fragment_cores);
   }

   struct drm_panthor_group_create gc = {
      .compute_core_mask = phys_dev->compute_core_mask,
      .fragment_core_mask = phys_dev->fragment_core_mask,
      .tiler_core_mask = 1,
      .max_compute_cores = max_compute_cores,
      .max_fragment_cores = max_fragment_cores,
      .max_tiler_cores = 1,
      .priority = group_priority,
      .queues = DRM_PANTHOR_OBJ_ARRAY(ARRAY_SIZE(qc), qc),
      .vm_id = pan_kmod_vm_handle(dev->kmod.vm),
   };

   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, &gc);
   if (ret)
      return panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                          "Failed to create a scheduling group");

   queue->group_handle = gc.group_handle;
   return VK_SUCCESS;
}

static void
destroy_group(struct panvk_gpu_queue *queue)
{
   const struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct drm_panthor_group_destroy gd = {
      .group_handle = queue->group_handle,
   };

   ASSERTED int ret =
      pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, &gd);
   assert(!ret);
}

static VkResult
init_tiler(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_tiler_heap *tiler_heap = &queue->tiler_heap;
   VkResult result;

   /* We allocate the tiler heap descriptor and geometry buffer in one go,
    * so we can pass it through a single 64-bit register to the VERTEX_TILER
    * command streams. */
   struct panvk_pool_alloc_info alloc_info = {
      .size = (64 * 1024) + 4096,
      .alignment = 4096,
   };

   tiler_heap->desc = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   if (!panvk_priv_mem_check_alloc(tiler_heap->desc)) {
      result = panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "Failed to create a tiler heap context");
      goto err_free_desc;
   }

   tiler_heap->chunk_size = phys_dev->csf.tiler.chunk_size;

   alloc_info.size = get_fbd_size(true, MAX_RTS);
#if PAN_ARCH >= 14
   const unsigned fbds_alignment = alignof(struct panvk_fb_layer_state);
#else
   const unsigned fbds_alignment = pan_alignment(FRAMEBUFFER);
#endif
   alloc_info.alignment = fbds_alignment;
   tiler_heap->oom_fbd = panvk_pool_alloc_mem(&dev->mempools.rw, alloc_info);
   if (!panvk_priv_mem_check_alloc(tiler_heap->oom_fbd)) {
      result = panvk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                            "Failed to create a scratch FBD");
      goto err_free_desc;
   }

   uint64_t first_heap_chunk;

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      uint64_t heap_ctx_va, first_chunk_va;

      if (kbase_kmod_csf_tiler_heap_create(
             dev->kmod.dev, tiler_heap->chunk_size,
             phys_dev->csf.tiler.initial_chunks, phys_dev->csf.tiler.max_chunks,
             65535, &heap_ctx_va, &first_chunk_va)) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to create a tiler heap context");
         goto err_free_desc;
      }

      tiler_heap->context.handle = 0;
      tiler_heap->context.dev_addr = heap_ctx_va;
      first_heap_chunk = first_chunk_va;
   } else
#endif
   {
      struct drm_panthor_tiler_heap_create thc = {
         .vm_id = pan_kmod_vm_handle(dev->kmod.vm),
         .chunk_size = tiler_heap->chunk_size,
         .initial_chunk_count = phys_dev->csf.tiler.initial_chunks,
         .max_chunks = phys_dev->csf.tiler.max_chunks,
         .target_in_flight = 65535,
      };

      int ret = pan_kmod_ioctl(dev->drm_fd,
                               DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, &thc);
      if (ret) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to create a tiler heap context");
         goto err_free_desc;
      }

      tiler_heap->context.handle = thc.handle;
      tiler_heap->context.dev_addr = thc.tiler_heap_ctx_gpu_va;
      first_heap_chunk = thc.first_heap_chunk_gpu_va;
   }

   panvk_priv_mem_write_desc(tiler_heap->desc, 0, TILER_HEAP, cfg) {
      cfg.size = tiler_heap->chunk_size;
      cfg.base = first_heap_chunk;
      cfg.bottom = cfg.base + 64;
      cfg.top = cfg.base + cfg.size;
   }

   return VK_SUCCESS;

err_free_desc:
   panvk_pool_free_mem(&tiler_heap->desc);
   panvk_pool_free_mem(&tiler_heap->oom_fbd);
   return result;
}

static void
cleanup_tiler(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_tiler_heap *tiler_heap = &queue->tiler_heap;

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      kbase_kmod_csf_tiler_heap_destroy(dev->kmod.dev,
                                        tiler_heap->context.dev_addr);
   } else
#endif
   {
      struct drm_panthor_tiler_heap_destroy thd = {
         .handle = tiler_heap->context.handle,
      };
      ASSERTED int ret = pan_kmod_ioctl(
         dev->drm_fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, &thd);
      assert(!ret);
   }

   panvk_pool_free_mem(&tiler_heap->desc);
   panvk_pool_free_mem(&tiler_heap->oom_fbd);
}

struct panvk_queue_submit {
   const struct panvk_physical_device *phys_dev;
   struct panvk_device *dev;
   struct panvk_gpu_queue *queue;

   bool process_utrace;
   bool force_sync;

   uint32_t qsubmit_count;
   uint32_t wait_queue_mask;
   uint32_t signal_queue_mask;
   uint32_t req_resource_subqueue_mask;

   struct drm_panthor_queue_submit *qsubmits;
   struct drm_panthor_sync_op *wait_ops;
   struct drm_panthor_sync_op *signal_ops;

   struct {
      uint32_t queue_mask;
      enum panvk_subqueue_id first_subqueue;
      enum panvk_subqueue_id last_subqueue;
      bool needs_clone;
      const struct u_trace *last_ut;
      struct panvk_utrace_flush_data *data_storage;

      struct panvk_utrace_flush_data *data[PANVK_SUBQUEUE_COUNT];
   } utrace;
};

struct panvk_queue_submit_stack_storage {
   struct drm_panthor_queue_submit qsubmits[8];
   struct drm_panthor_sync_op syncops[8];
};

static void
panvk_queue_submit_init(struct panvk_queue_submit *submit,
                        struct vk_queue *vk_queue)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   struct vk_device *vk_dev = vk_queue->base.device;

   *submit = (struct panvk_queue_submit){
      .phys_dev = to_panvk_physical_device(vk_dev->physical),
      .dev = to_panvk_device(vk_dev),
      .queue = container_of(vk_queue, struct panvk_gpu_queue, vk),
   };

   submit->process_utrace =
      u_trace_should_process(&submit->dev->utrace.utctx) &&
      submit->phys_dev->kmod.dev->props.timestamp_frequency;

   submit->force_sync = PANVK_DEBUG(TRACE) || PANVK_DEBUG(SYNC);
}

static void
panvk_queue_submit_init_storage(
   struct panvk_queue_submit *submit, const struct vk_queue_submit *vk_submit,
   struct panvk_queue_submit_stack_storage *stack_storage)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   submit->utrace.first_subqueue = PANVK_SUBQUEUE_COUNT;
   VkPipelineStageFlags2 cmd_stage_mask = VK_PIPELINE_STAGE_2_NONE;
   for (uint32_t i = 0; i < vk_submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(
         vk_submit->command_buffers[i], struct panvk_cmd_buffer, vk);

      for (uint32_t j = 0; j < ARRAY_SIZE(cmdbuf->state.cs); j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         assert(cs_is_valid(b));
         if (cs_is_empty(b))
            continue;

         cmd_stage_mask |= panvk_get_subqueue_stages(j);
         submit->qsubmit_count++;

         struct panvk_subqueue *subq = &submit->queue->subqueues[j];
         /* If we need a resource the subqueue has not requested yet. */
         if (b->req_resource_mask & (~subq->req_resource.mask)) {
            /* Ensure we do not need a resource not expected for this subqueue. */
            assert(!(b->req_resource_mask & (~get_resource_mask(j))));
            submit->qsubmit_count++;
            submit->req_resource_subqueue_mask |= BITFIELD_BIT(j);
            subq->req_resource.mask = get_resource_mask(j);
         }

         struct u_trace *ut = &cmdbuf->utrace.uts[j];
         if (submit->process_utrace && u_trace_has_points(ut)) {
            submit->utrace.queue_mask |= BITFIELD_BIT(j);
            if (submit->utrace.first_subqueue == PANVK_SUBQUEUE_COUNT)
               submit->utrace.first_subqueue = j;
            submit->utrace.last_subqueue = j;
            submit->utrace.last_ut = ut;

            if (!(cmdbuf->flags &
                  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) {
               /* we will follow the user cs with a timestamp copy cs */
               submit->qsubmit_count++;
               submit->utrace.needs_clone = true;
            }
         }
      }
   }

   /* wait_stages_mask is pipeline stages which limit
    * the second synchronization scope of a semaphore wait operation */
   VkPipelineStageFlags2 wait_stages_mask = cmd_stage_mask;
   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      wait_stages_mask |= vk_submit->waits[i].stage_mask;
   }

   /* signal_stages_mask is pipeline stages which limit
    * the first synchronization scope of a semaphore signal operation */
   VkPipelineStageFlags2 signal_stages_mask = cmd_stage_mask;
   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      signal_stages_mask |= vk_submit->signals[i].stage_mask;
   }

   /* if there is no cs in any subqueue */
   if (cmd_stage_mask == VK_PIPELINE_STAGE_2_NONE) {
      /* signal stage mask is TOP_OF_PIPE/NONE, signal immediately */
      if (signal_stages_mask == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT ||
          signal_stages_mask == VK_PIPELINE_STAGE_2_NONE) {
         signal_stages_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      }

      /* wait stage mask is BOTTOM_OF_PIPE/NONE, wait deferred */
      if (wait_stages_mask == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT ||
          wait_stages_mask == VK_PIPELINE_STAGE_2_NONE) {
         wait_stages_mask = panvk_get_subqueue_stages(PANVK_SUBQUEUE_FRAGMENT) |
                            panvk_get_subqueue_stages(PANVK_SUBQUEUE_COMPUTE);
      }
   }

   submit->wait_queue_mask =
      vk_stages_to_subqueue_mask(wait_stages_mask, SYNC_SCOPE_SECOND);

   submit->signal_queue_mask =
      vk_stages_to_subqueue_mask(signal_stages_mask, SYNC_SCOPE_FIRST) |
      submit->utrace.queue_mask;

   /* Signal all subqueues if force_sync */
   if (submit->force_sync) {
      submit->signal_queue_mask |= BITFIELD_MASK(PANVK_SUBQUEUE_COUNT);
   }

   uint32_t syncop_count = 0;

   /* We add sync-only queue submits to place our wait/signal operations. */
   submit->qsubmit_count += util_bitcount(submit->wait_queue_mask);
   syncop_count += vk_submit->wait_count;

   submit->qsubmit_count += util_bitcount(submit->signal_queue_mask);
   syncop_count += util_bitcount(submit->signal_queue_mask);

   submit->qsubmits =
      submit->qsubmit_count <= ARRAY_SIZE(stack_storage->qsubmits)
         ? stack_storage->qsubmits
         : malloc(sizeof(*submit->qsubmits) * submit->qsubmit_count);

   submit->wait_ops = syncop_count <= ARRAY_SIZE(stack_storage->syncops)
                         ? stack_storage->syncops
                         : malloc(sizeof(*submit->wait_ops) * syncop_count);
   submit->signal_ops = submit->wait_ops + vk_submit->wait_count;

   /* reset so that we can initialize submit->qsubmits incrementally */
   submit->qsubmit_count = 0;

   if (submit->utrace.queue_mask) {
      submit->utrace.data_storage =
         malloc(sizeof(*submit->utrace.data_storage) *
                util_bitcount(submit->utrace.queue_mask));
   }
}

static void
panvk_queue_submit_cleanup_storage(
   struct panvk_queue_submit *submit,
   const struct panvk_queue_submit_stack_storage *stack_storage)
{
   if (submit->qsubmits != stack_storage->qsubmits)
      free(submit->qsubmits);
   if (submit->wait_ops != stack_storage->syncops)
      free(submit->wait_ops);

   /* either no utrace flush data or the data has been transferred to u_trace */
   assert(!submit->utrace.data_storage);
}

static void
panvk_queue_submit_init_utrace(struct panvk_queue_submit *submit,
                               const struct vk_queue_submit *vk_submit)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);

   if (!submit->utrace.queue_mask)
      return;

   /* u_trace_context processes trace events in order.  We want to make sure
    * it waits for the timestamp writes before processing the first event and
    * it can free the flush data after processing the last event.
    */
   struct panvk_utrace_flush_data *next = submit->utrace.data_storage;
   submit->utrace.data[submit->utrace.last_subqueue] = next++;
   submit->utrace.data[submit->utrace.last_subqueue]->free_self = true;

   u_foreach_bit(i, submit->utrace.queue_mask) {
      if (i != submit->utrace.last_subqueue)
         submit->utrace.data[i] = next++;

      const bool wait = i == submit->utrace.first_subqueue;
      *submit->utrace.data[i] = (struct panvk_utrace_flush_data){
         .subqueue = i,
         .sync = wait ? submit->queue->utrace.sync : NULL,
         .wait_value = wait ? submit->queue->utrace.next_value : 0,
         .free_self = false,
      };
   }
}

static void
panvk_queue_submit_init_req_resource(struct panvk_queue_submit *submit)
{
   if (!submit->req_resource_subqueue_mask)
      return;

   struct panvk_device *dev = submit->dev;
   uint32_t flush_id = panvk_get_flush_id(dev);

   u_foreach_bit(i, submit->req_resource_subqueue_mask) {
      struct panvk_subqueue *subq = &submit->queue->subqueues[i];
      submit->qsubmits[submit->qsubmit_count++] =
         (struct drm_panthor_queue_submit){
            .queue_index = i,
            .stream_size = subq->req_resource.cs_buffer_size,
            .stream_addr = subq->req_resource.cs_buffer_addr,
            .latest_flush = flush_id,
         };
   }
}

static void
panvk_queue_submit_init_waits(struct panvk_queue_submit *submit,
                              const struct vk_queue_submit *vk_submit)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   if (!submit->wait_queue_mask)
      return;

#ifdef HAVE_PAN_KMOD_KBASE
   /* No DRM syncobjs on kbase: semaphore waits are resolved on the CPU
    * right before submission instead. */
   if (gpu_queue_uses_kbase(submit->dev))
      return;
#endif

   for (uint32_t i = 0; i < vk_submit->wait_count; i++) {
      const struct vk_sync_wait *wait = &vk_submit->waits[i];
      const struct vk_drm_syncobj *syncobj = vk_sync_as_drm_syncobj(wait->sync);
      assert(syncobj);

      submit->wait_ops[i] = (struct drm_panthor_sync_op){
         .flags = (syncobj->base.flags & VK_SYNC_IS_TIMELINE
                      ? DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ
                      : DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ) |
                  DRM_PANTHOR_SYNC_OP_WAIT,
         .handle = syncobj->syncobj,
         .timeline_value = wait->wait_value,
      };
   }

   u_foreach_bit(i, submit->wait_queue_mask) {
      submit->qsubmits[submit->qsubmit_count++] =
         (struct drm_panthor_queue_submit){
            .queue_index = i,
            .syncs =
               DRM_PANTHOR_OBJ_ARRAY(vk_submit->wait_count, submit->wait_ops),
         };
   }
}

static void
panvk_queue_submit_init_cmdbufs(struct panvk_queue_submit *submit,
                                const struct vk_queue_submit *vk_submit)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   struct panvk_device *dev = submit->dev;

   for (uint32_t i = 0; i < vk_submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(
         vk_submit->command_buffers[i], struct panvk_cmd_buffer, vk);

      uint32_t flush_id = panvk_get_flush_id(dev);

      for (uint32_t j = 0; j < ARRAY_SIZE(cmdbuf->state.cs); j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (cs_is_empty(b))
            continue;

         submit->qsubmits[submit->qsubmit_count++] =
            (struct drm_panthor_queue_submit){
               .queue_index = j,
               .stream_size = cs_root_chunk_size(b),
               .stream_addr = cs_root_chunk_gpu_addr(b),
               .latest_flush = flush_id,
            };
      }

      if (util_bitcount(submit->utrace.queue_mask) > 0)
         flush_id = panvk_get_flush_id(dev);

      u_foreach_bit(j, submit->utrace.queue_mask) {
         struct u_trace *ut = &cmdbuf->utrace.uts[j];

         if (!u_trace_has_points(ut))
            continue;

         /* The last subqueue frees the flush data itself. */
         bool free_data = ut == submit->utrace.last_ut;

         struct u_trace clone_ut;
         if (!(cmdbuf->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) {
            u_trace_init(&clone_ut, &dev->utrace.utctx);

            const uint64_t root_buf_size = sizeof(uint64_t) * 1024;
            struct panvk_utrace_buf *cs_root_buf =
               panvk_utrace_create_buffer(&dev->utrace.utctx, root_buf_size);
            assert(cs_root_buf);
            /* For every sq, the cs buffer needs to be freed. */
            free_data = true;

            const struct cs_buffer cs_root = (struct cs_buffer){
               .cpu = cs_root_buf->host,
               .gpu = cs_root_buf->dev,
               .capacity = root_buf_size / sizeof(uint64_t),
            };

            submit->utrace.data[j]->clone_cs_root = cs_root_buf;
            struct cs_builder clone_builder;
            panvk_per_arch(utrace_clone_init_builder)(&clone_builder, dev,
                                                      &cs_root);

            u_trace_clone_append(
               u_trace_begin_iterator(ut), u_trace_end_iterator(ut), &clone_ut,
               &clone_builder, panvk_per_arch(utrace_copy_buffer));

            panvk_per_arch(utrace_clone_finish_builder)(&clone_builder);

            submit->qsubmits[submit->qsubmit_count++] =
               (struct drm_panthor_queue_submit){
                  .queue_index = j,
                  .stream_size = cs_root_chunk_size(&clone_builder),
                  .stream_addr = cs_root_chunk_gpu_addr(&clone_builder),
                  .latest_flush = flush_id,
               };

            ut = &clone_ut;
         }

         u_trace_flush(ut, submit->utrace.data[j], dev->vk.current_frame,
                       free_data);
      }
   }

   /* we've transferred the data ownership to utrace, if any */
   submit->utrace.data_storage = NULL;
}

static void
panvk_queue_submit_init_signals(struct panvk_queue_submit *submit,
                                const struct vk_queue_submit *vk_submit)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   struct panvk_gpu_queue *queue = submit->queue;

#ifdef HAVE_PAN_KMOD_KBASE
   /* No DRM syncobjs on kbase: semaphore signals are resolved on the CPU
    * after the synchronous wait instead. */
   if (gpu_queue_uses_kbase(submit->dev))
      return;
#endif

   uint32_t signal_op = 0;
   u_foreach_bit(i, submit->signal_queue_mask) {
      submit->signal_ops[signal_op] = (struct drm_panthor_sync_op){
         .flags = DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ |
                  DRM_PANTHOR_SYNC_OP_SIGNAL,
         .handle = queue->syncobj_handle,
         .timeline_value = signal_op + 1,
      };

      submit->qsubmits[submit->qsubmit_count++] =
         (struct drm_panthor_queue_submit){
            .queue_index = i,
            .syncs = DRM_PANTHOR_OBJ_ARRAY(1, &submit->signal_ops[signal_op++]),
         };
   }
}

#ifdef HAVE_PAN_KMOD_KBASE
/* Synchronous kbase submission: resolve semaphore waits on the CPU, write
 * the ring entries, kick every touched subqueue, then wait for all of them
 * to drain before signaling the semaphores on the CPU.  All subqueues must
 * be kicked before waiting on any of them: their streams synchronize with
 * each other through the syncobjs table, so running them serially would
 * deadlock. */
static VkResult
panvk_queue_submit_ioctl_kbase(struct panvk_queue_submit *submit,
                               const struct vk_queue_submit *vk_submit)
{
   struct panvk_device *dev = submit->dev;
   struct panvk_gpu_queue *queue = submit->queue;
   VkResult result;

   if (vk_submit->wait_count) {
      result = vk_sync_wait_many(&dev->vk, vk_submit->wait_count,
                                 vk_submit->waits, VK_SYNC_WAIT_COMPLETE,
                                 UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   /* Flush pending synchronization requests before submitting the job, to
    * make sure things are GPU-visible. */
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   uint32_t touched = 0;
   for (uint32_t i = 0; i < submit->qsubmit_count; i++) {
      const struct drm_panthor_queue_submit *qsubmit = &submit->qsubmits[i];

      if (!qsubmit->stream_size)
         continue;

      result = kbase_subqueue_emit_job(queue, qsubmit->queue_index,
                                       qsubmit->stream_addr,
                                       qsubmit->stream_size,
                                       qsubmit->latest_flush);
      if (result != VK_SUCCESS)
         return vk_queue_set_lost(&queue->vk, "kbase: ring emission failed");

      touched |= BITFIELD_BIT(qsubmit->queue_index);
   }

   u_foreach_bit(i, touched)
      kbase_subqueue_publish(queue, i);

   u_foreach_bit(i, touched) {
      result = kbase_subqueue_wait_idle(queue, i);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static void
panvk_queue_submit_process_signals_kbase(struct panvk_queue_submit *submit,
                                         const struct vk_queue_submit *vk_submit)
{
   struct panvk_device *dev = submit->dev;

   /* The GPU work already completed (synchronous model), so semaphore
    * signals happen right away on the CPU. */
   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      const struct vk_sync_signal *signal = &vk_submit->signals[i];

      ASSERTED VkResult result =
         vk_sync_signal(&dev->vk, signal->sync, signal->signal_value);
      assert(result == VK_SUCCESS);
   }
}
#endif /* HAVE_PAN_KMOD_KBASE */

static VkResult
panvk_queue_submit_ioctl(struct panvk_queue_submit *submit)
{
   const struct panvk_device *dev = submit->dev;
   struct panvk_gpu_queue *queue = submit->queue;
   int ret;

   if (PANVK_DEBUG(TRACE)) {
      /* If we're tracing, we need to reset the desc ringbufs and the CS
       * tracebuf. */
      for (uint32_t i = 0; i < ARRAY_SIZE(queue->subqueues); i++) {
         panvk_priv_mem_rmw(queue->subqueues[i].context, 0,
                            struct panvk_cs_subqueue_context, ctx) {
            if (ctx->render.desc_ringbuf.ptr) {
               ctx->render.desc_ringbuf.ptr =
                  queue->render_desc_ringbuf.addr.dev;
               ctx->render.desc_ringbuf.pos = 0;
            }

            if (ctx->debug.tracebuf.cs)
               ctx->debug.tracebuf.cs = queue->subqueues[i].tracebuf.addr.dev;
         }
      }
   }

   /* Flush pending synchronization requests before submitting the job, to
    * make sure things are GPU-visible. */
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   struct drm_panthor_group_submit gsubmit = {
      .group_handle = queue->group_handle,
      .queue_submits =
         DRM_PANTHOR_OBJ_ARRAY(submit->qsubmit_count, submit->qsubmits),
   };

   ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, &gsubmit);
   if (ret)
      return vk_queue_set_lost(&queue->vk, "GROUP_SUBMIT: %m");

   return VK_SUCCESS;
}

static void
panvk_queue_submit_process_signals(struct panvk_queue_submit *submit,
                                   const struct vk_queue_submit *vk_submit)
{
   struct panvk_device *dev = submit->dev;
   struct panvk_gpu_queue *queue = submit->queue;
   ASSERTED int ret;

   if (!submit->signal_queue_mask)
      return;

   if (submit->force_sync) {
      uint64_t point = util_bitcount(submit->signal_queue_mask);
      ret = drmSyncobjTimelineWait(dev->drm_fd, &queue->syncobj_handle,
                                   &point, 1, INT64_MAX,
                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
      assert(!ret);
   }

   for (uint32_t i = 0; i < vk_submit->signal_count; i++) {
      const struct vk_sync_signal *signal = &vk_submit->signals[i];
      const struct vk_drm_syncobj *syncobj =
         vk_sync_as_drm_syncobj(signal->sync);
      assert(syncobj);

      drmSyncobjTransfer(dev->drm_fd, syncobj->syncobj, signal->signal_value,
                         queue->syncobj_handle, 0, 0);
   }

   if (submit->utrace.queue_mask) {
      const struct vk_drm_syncobj *syncobj =
         vk_sync_as_drm_syncobj(queue->utrace.sync);

      drmSyncobjTransfer(dev->drm_fd, syncobj->syncobj,
                         queue->utrace.next_value++, queue->syncobj_handle, 0,
                         0);

      /* process flushed events after the syncobj is set up */
      u_trace_context_process(&dev->utrace.utctx, false);
   }

   drmSyncobjReset(dev->drm_fd, &queue->syncobj_handle, 1);
}

static void
panvk_queue_submit_process_debug(const struct panvk_queue_submit *submit,
                                 const struct vk_queue_submit *vk_submit)
{
   struct panvk_gpu_queue *queue = submit->queue;
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct pandecode_context *decode_ctx = submit->dev->debug.decode_ctx;

   if (PANVK_DEBUG(TRACE)) {
      const struct pan_kmod_dev_props *props =
         &submit->phys_dev->kmod.dev->props;

      /* First we invalidate all desc buffers to make sure we see GPU updates
       * on those. */
      for (uint32_t i = 0; i < vk_submit->command_buffer_count; i++) {
         struct panvk_cmd_buffer *cmdbuf = container_of(
            vk_submit->command_buffers[i], struct panvk_cmd_buffer, vk);

         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
      }

      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

      for (uint32_t i = 0; i < submit->qsubmit_count; i++) {
         const struct drm_panthor_queue_submit *qsubmit = &submit->qsubmits[i];
         if (!qsubmit->stream_size)
            continue;

         pandecode_user_msg(decode_ctx, "CS %d on subqueue %d binaries\n\n", i,
                            qsubmit->queue_index);
         pandecode_cs_binary(decode_ctx, qsubmit->stream_addr,
                             qsubmit->stream_size, props->gpu_id);
         pandecode_user_msg(decode_ctx, "\n");
      }

      for (uint32_t i = 0; i < ARRAY_SIZE(queue->subqueues); i++) {
         panvk_priv_mem_readback(queue->subqueues[i].context, 0,
                                 struct panvk_cs_subqueue_context, ctx) {
            size_t trace_size =
               ctx->debug.tracebuf.cs - queue->subqueues[i].tracebuf.addr.dev;

            if (trace_size) {
               assert(
                  trace_size <= queue->subqueues[i].tracebuf.size ||
                  !"OOB access on the CS tracebuf, pass a bigger PANVK_CS_TRACEBUF_SIZE");

               assert(
                  !ctx->render.desc_ringbuf.ptr ||
                  ctx->render.desc_ringbuf.pos <=
                     queue->render_desc_ringbuf.size ||
                  !"OOB access on the desc tracebuf, pass a bigger PANVK_DESC_TRACEBUF_SIZE");

               uint64_t trace = queue->subqueues[i].tracebuf.addr.dev;

               pandecode_user_msg(decode_ctx, "\nCS traces on subqueue %d\n\n",
                                  i);
               pandecode_cs_trace(decode_ctx, trace, trace_size, props->gpu_id);
               pandecode_user_msg(decode_ctx, "\n");
            }
         }
      }
   }

   if (PANVK_DEBUG(DUMP))
      pandecode_dump_mappings(decode_ctx);

   if (PANVK_DEBUG(TRACE))
      pandecode_next_frame(decode_ctx);

   /* validate last after the command streams are dumped */
   if (submit->force_sync)
      panvk_per_arch(gpu_queue_check_status)(&queue->vk);
}

VkResult
panvk_per_arch(gpu_queue_submit)(struct vk_queue *vk_queue, struct vk_queue_submit *vk_submit)
{
   PAN_TRACE_FUNC(PAN_TRACE_VK_CSF);
   struct panvk_queue_submit_stack_storage stack_storage;
   struct panvk_queue_submit submit;
   VkResult result = VK_SUCCESS;

   if (vk_queue_is_lost(vk_queue))
      return VK_ERROR_DEVICE_LOST;

   panvk_queue_submit_init(&submit, vk_queue);
   panvk_queue_submit_init_storage(&submit, vk_submit, &stack_storage);
   panvk_queue_submit_init_utrace(&submit, vk_submit);
   panvk_queue_submit_init_req_resource(&submit);
   panvk_queue_submit_init_waits(&submit, vk_submit);
   panvk_queue_submit_init_cmdbufs(&submit, vk_submit);
   panvk_queue_submit_init_signals(&submit, vk_submit);

#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(submit.dev)) {
      result = panvk_queue_submit_ioctl_kbase(&submit, vk_submit);
      if (result != VK_SUCCESS)
         goto out;

      panvk_queue_submit_process_signals_kbase(&submit, vk_submit);
      panvk_queue_submit_process_debug(&submit, vk_submit);
      goto out;
   }
#endif

   result = panvk_queue_submit_ioctl(&submit);
   if (result != VK_SUCCESS)
      goto out;

   panvk_queue_submit_process_signals(&submit, vk_submit);
   panvk_queue_submit_process_debug(&submit, vk_submit);

out:
   panvk_queue_submit_cleanup_storage(&submit, &stack_storage);
   return result;
}

static enum drm_panthor_group_priority
get_panthor_group_priority(const VkDeviceQueueCreateInfo *create_info)
{
   const VkDeviceQueueGlobalPriorityCreateInfoKHR *priority_info =
      vk_find_struct_const(create_info->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);
   const VkQueueGlobalPriorityKHR priority =
      priority_info ? priority_info->globalPriority
                    : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;

   switch (priority) {
   case VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR:
      return PANTHOR_GROUP_PRIORITY_LOW;
   case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR:
      return PANTHOR_GROUP_PRIORITY_MEDIUM;
   case VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR:
      return PANTHOR_GROUP_PRIORITY_HIGH;
   case VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR:
      return PANTHOR_GROUP_PRIORITY_REALTIME;
   default:
      UNREACHABLE("Invalid global priority");
   }
}

VkResult
panvk_per_arch(create_gpu_queue)(struct panvk_device *dev,
                                 const VkDeviceQueueCreateInfo *create_info,
                                 uint32_t queue_idx,
                                 struct vk_queue **out_queue)
{
   struct panvk_gpu_queue *queue = vk_zalloc(&dev->vk.alloc, sizeof(*queue), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queue)
      return panvk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_queue_init(&queue->vk, &dev->vk, create_info, queue_idx);
   if (result != VK_SUCCESS)
      goto err_free_queue;

#ifdef HAVE_PAN_KMOD_KBASE
   const bool uses_kbase = gpu_queue_uses_kbase(dev);
#else
   const bool uses_kbase = false;
#endif

   if (!uses_kbase) {
      int ret = drmSyncobjCreate(dev->drm_fd, 0, &queue->syncobj_handle);
      if (ret) {
         result = panvk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED,
                               "Failed to create our internal sync object");
         goto err_finish_queue;
      }
   }

   result = init_tiler(queue);
   if (result != VK_SUCCESS)
      goto err_destroy_syncobj;

   const VkDeviceQueueShaderCoreControlCreateInfoARM *core_ctrl =
      vk_find_struct_const(create_info->pNext,
                           DEVICE_QUEUE_SHADER_CORE_CONTROL_CREATE_INFO_ARM);

#ifdef HAVE_PAN_KMOD_KBASE
   if (uses_kbase)
      result = kbase_create_group(queue);
   else
#endif
      result = create_group(queue, get_panthor_group_priority(create_info),
                            core_ctrl ? core_ctrl->shaderCoreCount : 0);
   if (result != VK_SUCCESS)
      goto err_cleanup_tiler;

   result = init_queue(queue);
   if (result != VK_SUCCESS)
      goto err_destroy_group;

   queue->vk.driver_submit = panvk_per_arch(gpu_queue_submit);
   *out_queue = &queue->vk;
   return VK_SUCCESS;

err_destroy_group:
#ifdef HAVE_PAN_KMOD_KBASE
   if (uses_kbase)
      kbase_destroy_group(queue);
   else
#endif
      destroy_group(queue);

err_cleanup_tiler:
   cleanup_tiler(queue);

err_destroy_syncobj:
   if (!uses_kbase)
      drmSyncobjDestroy(dev->drm_fd, queue->syncobj_handle);

err_finish_queue:
   vk_queue_finish(&queue->vk);

err_free_queue:
   vk_free(&dev->vk.alloc, queue);
   return result;
}

void
panvk_per_arch(destroy_gpu_queue)(struct vk_queue *vk_queue)
{
   struct panvk_gpu_queue *queue = container_of(vk_queue, struct panvk_gpu_queue, vk);
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   cleanup_queue(queue);
#ifdef HAVE_PAN_KMOD_KBASE
   if (gpu_queue_uses_kbase(dev)) {
      kbase_destroy_group(queue);
      cleanup_tiler(queue);
   } else
#endif
   {
      destroy_group(queue);
      cleanup_tiler(queue);
      drmSyncobjDestroy(dev->drm_fd, queue->syncobj_handle);
   }
   vk_queue_finish(&queue->vk);
   vk_free(&dev->vk.alloc, queue);
}

VkResult
panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue)
{
   struct panvk_gpu_queue *queue = container_of(vk_queue, struct panvk_gpu_queue, vk);
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct drm_panthor_group_get_state state = {
      .group_handle = queue->group_handle,
   };

   /* check for CS error and treat it as device lost */
   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      panvk_priv_mem_readback(queue->subqueues[i].context, 0,
                              struct panvk_cs_subqueue_context, subq_ctx) {
         if (subq_ctx->last_error != 0) {
            /* Check printf buffer one more time before exiting */
            u_printf_with_ctx(stdout, &dev->printf.ctx);

            return vk_queue_set_lost(&queue->vk, "CS_FAULT");
         }
      }
   }

#ifdef HAVE_PAN_KMOD_KBASE
   /* No group-state query on kbase; the per-subqueue error check above is
    * all we have until fault notifications are wired up. */
   if (gpu_queue_uses_kbase(dev))
      return VK_SUCCESS;
#endif

   int ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_PANTHOR_GROUP_GET_STATE,
                            &state);
   if (!ret && !state.state)
      return VK_SUCCESS;

   /* Check printf buffer one more time before exiting */
   u_printf_with_ctx(stdout, &dev->printf.ctx);

   vk_queue_set_lost(&queue->vk,
                     "group state: err=%d, state=0x%x, fatal_queues=0x%x", ret,
                     state.state, state.fatal_queues);

   return VK_ERROR_DEVICE_LOST;
}
