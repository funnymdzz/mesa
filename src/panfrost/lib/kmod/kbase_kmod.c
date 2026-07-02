/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include "util/macros.h"

#include "pan_kmod_backend.h"

static void
kbase_unsupported(const char *op)
{
   mesa_loge("kbase backend: %s is not implemented", op);
   errno = ENOSYS;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(UNUSED int fd, UNUSED uint32_t flags,
                      UNUSED const struct pan_kmod_driver *drv_info,
                      UNUSED const struct pan_kmod_allocator *allocator)
{
   kbase_unsupported("dev_create");
   return NULL;
}

static void
kbase_kmod_dev_destroy(UNUSED struct pan_kmod_dev *dev)
{
   unreachable("kbase_kmod_dev_destroy should never be called");
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(UNUSED struct pan_kmod_dev *dev,
                    UNUSED struct pan_kmod_vm *exclusive_vm, UNUSED uint64_t size,
                    UNUSED uint32_t flags)
{
   kbase_unsupported("bo_alloc");
   return NULL;
}

static void
kbase_kmod_bo_free(UNUSED struct pan_kmod_bo *bo)
{
   unreachable("kbase_kmod_bo_free should never be called");
}

static struct pan_kmod_bo *
kbase_kmod_bo_import(UNUSED struct pan_kmod_dev *dev, UNUSED uint32_t handle,
                     UNUSED uint64_t size)
{
   kbase_unsupported("bo_import");
   return NULL;
}

static int
kbase_kmod_bo_export(UNUSED struct pan_kmod_bo *bo, UNUSED int dmabuf_fd)
{
   kbase_unsupported("bo_export");
   return -1;
}

static off_t
kbase_kmod_bo_get_mmap_offset(UNUSED struct pan_kmod_bo *bo)
{
   kbase_unsupported("bo_get_mmap_offset");
   return -1;
}

static int
kbase_kmod_flush_bo_map_syncs(UNUSED struct pan_kmod_dev *dev)
{
   kbase_unsupported("flush_bo_map_syncs");
   return -1;
}

static bool
kbase_kmod_bo_wait(UNUSED struct pan_kmod_bo *bo, UNUSED int64_t timeout_ns,
                   UNUSED bool for_read_only_access)
{
   kbase_unsupported("bo_wait");
   return false;
}

static void
kbase_kmod_bo_make_evictable(UNUSED struct pan_kmod_bo *bo)
{
   kbase_unsupported("bo_make_evictable");
}

static bool
kbase_kmod_bo_make_unevictable(UNUSED struct pan_kmod_bo *bo)
{
   kbase_unsupported("bo_make_unevictable");
   return false;
}

static struct pan_kmod_vm *
kbase_kmod_vm_create(UNUSED struct pan_kmod_dev *dev, UNUSED uint32_t flags,
                     UNUSED uint64_t va_start, UNUSED uint64_t va_range)
{
   kbase_unsupported("vm_create");
   return NULL;
}

static void
kbase_kmod_vm_destroy(UNUSED struct pan_kmod_vm *vm)
{
   unreachable("kbase_kmod_vm_destroy should never be called");
}

static int
kbase_kmod_vm_bind(UNUSED struct pan_kmod_vm *vm,
                   UNUSED enum pan_kmod_vm_op_mode mode,
                   UNUSED struct pan_kmod_vm_op *ops, UNUSED uint32_t op_count)
{
   kbase_unsupported("vm_bind");
   return -1;
}

static enum pan_kmod_vm_state
kbase_kmod_vm_query_state(UNUSED struct pan_kmod_vm *vm)
{
   kbase_unsupported("vm_query_state");
   return PAN_KMOD_VM_FAULTY;
}

static uint64_t
kbase_kmod_query_timestamp(UNUSED const struct pan_kmod_dev *dev)
{
   kbase_unsupported("query_timestamp");
   return 0;
}

static void
kbase_kmod_bo_set_label(UNUSED struct pan_kmod_dev *dev,
                        UNUSED struct pan_kmod_bo *bo, UNUSED const char *label)
{
   kbase_unsupported("bo_set_label");
}

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create = kbase_kmod_dev_create,
   .dev_destroy = kbase_kmod_dev_destroy,
   .bo_alloc = kbase_kmod_bo_alloc,
   .bo_free = kbase_kmod_bo_free,
   .bo_import = kbase_kmod_bo_import,
   .bo_export = kbase_kmod_bo_export,
   .bo_get_mmap_offset = kbase_kmod_bo_get_mmap_offset,
   .flush_bo_map_syncs = kbase_kmod_flush_bo_map_syncs,
   .bo_wait = kbase_kmod_bo_wait,
   .bo_make_evictable = kbase_kmod_bo_make_evictable,
   .bo_make_unevictable = kbase_kmod_bo_make_unevictable,
   .vm_create = kbase_kmod_vm_create,
   .vm_destroy = kbase_kmod_vm_destroy,
   .vm_bind = kbase_kmod_vm_bind,
   .vm_query_state = kbase_kmod_vm_query_state,
   .query_timestamp = kbase_kmod_query_timestamp,
   .bo_set_label = kbase_kmod_bo_set_label,
};
