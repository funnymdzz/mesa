/*
 * Copyright (C) 2014-2024 ARM Ltd
 * SPDX-License-Identifier: MIT
 *
 * ARM Mali kbase userspace API definitions.
 * Targets the ARM Bifrost/Valhall kernel driver r32p0–r44p0.
 *
 * NOTE: ioctl numbers and struct layouts must be verified against the exact
 * kernel driver version in use on the target system. The definitions here are
 * based on the publicly-available r44p0 release.
 */

#pragma once

#include <linux/ioctl.h>
#include <linux/types.h>

/* The ioctl type for all kbase ioctls */
#define KBASE_IOCTL_TYPE 0x80

/* -----------------------------------------------------------------------
 * Version handshake — first ioctl to call on a fresh /dev/mali* fd.
 * The kernel fills in the version it supports. Accept if major matches.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_version_check {
   __u16 major;
   __u16 minor;
};
#define KBASE_IOCTL_VERSION_CHECK \
   _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)

/* -----------------------------------------------------------------------
 * Set context-creation flags.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_set_flags {
   __u32 create_flags;
};
/* Flags for create_flags */
#define BASE_CONTEXT_CCTX_EMBEDDED       ((__u32)1)
#define BASE_CONTEXT_CSF_EVENT_THREAD    ((__u32)(1 << 2))
#define BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED ((__u32)(1 << 3))

#define KBASE_IOCTL_SET_FLAGS \
   _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)

/* -----------------------------------------------------------------------
 * Job submit (JM GPUs, arch < 10).
 * Not used by the kmod layer directly; retained for completeness.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_job_submit {
   __u64 addr;
   __u32 nr_atoms;
   __u32 stride;
};
#define KBASE_IOCTL_JOB_SUBMIT \
   _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)

/* -----------------------------------------------------------------------
 * Query GPU properties.
 * On success the ioctl returns the byte-length of the property blob.
 * Call with size=0 / buffer=0 to probe the required length, then
 * allocate and call again.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_get_gpuprops {
   __u64 buffer;
   __u32 size;
   __u32 flags;
};
#define KBASE_IOCTL_GET_GPUPROPS \
   _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

/* -----------------------------------------------------------------------
 * GPU property key IDs used in the serialised blob returned by
 * KBASE_IOCTL_GET_GPUPROPS.  Each entry is encoded as:
 *
 *   [ 4-byte header: (key << 2) | size_code ]  [ value bytes ]
 *
 *   size_code: 0 = u8, 1 = u16, 2 = u32, 3 = u64
 *
 * The values below are from the ARM Bifrost/Valhall r44p0 source tree.
 * They may differ slightly on older driver releases.
 * ----------------------------------------------------------------------- */

/* Core computed properties */
#define KBASE_GPUPROP_PRODUCT_ID            1   /* u32 */
#define KBASE_GPUPROP_VERSION_STATUS        2   /* u32 */
#define KBASE_GPUPROP_MINOR_REVISION        3   /* u32 */
#define KBASE_GPUPROP_MAJOR_REVISION        4   /* u32 */

/* Raw GPU register mirror values */
#define KBASE_GPUPROP_RAW_SHADER_PRESENT    56  /* u64 */
#define KBASE_GPUPROP_RAW_TILER_PRESENT     57  /* u64 */
#define KBASE_GPUPROP_RAW_L2_PRESENT        58  /* u64 */
#define KBASE_GPUPROP_RAW_STACK_PRESENT     59  /* u64 */
#define KBASE_GPUPROP_RAW_L2_FEATURES       60  /* u32 */
#define KBASE_GPUPROP_RAW_CORE_FEATURES     61  /* u32 */
#define KBASE_GPUPROP_RAW_TILER_FEATURES    62  /* u32 */
#define KBASE_GPUPROP_RAW_MEM_FEATURES      63  /* u32 */
#define KBASE_GPUPROP_RAW_MMU_FEATURES      64  /* u32 */
#define KBASE_GPUPROP_RAW_AS_PRESENT        65  /* u32 */
#define KBASE_GPUPROP_RAW_JS_PRESENT        66  /* u32 */
/* JS_FEATURES_0..15 occupy keys 67-82 */
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0  83 /* u32 */
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1  84 /* u32 */
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2  85 /* u32 */
#define KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3  86 /* u32 */
#define KBASE_GPUPROP_RAW_GPU_ID            87  /* u32: top-16-bit product + bottom-16-bit rev */
#define KBASE_GPUPROP_RAW_THREAD_MAX_THREADS 88  /* u32 */
#define KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE 89 /* u32 */
#define KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE   90 /* u32 */
#define KBASE_GPUPROP_RAW_THREAD_FEATURES   91  /* u32 */
#define KBASE_GPUPROP_RAW_COHERENCY_MODE    92  /* u32 */

/* Thread properties (computed) */
#define KBASE_GPUPROP_THREAD_TLS_ALLOC      93  /* u32 */
#define KBASE_GPUPROP_AFBC_FEATURES         94  /* u32 */

/* -----------------------------------------------------------------------
 * Memory allocation.
 * va_pages: number of pages to reserve in the GPU address space.
 * commit_pages: pages backed at allocation time (can be <= va_pages).
 * extent: growth granularity for GROW_ON_GPF allocations.
 * flags: combination of BASE_MEM_* flags.
 * On success the output half contains the assigned GPU VA and actual flags.
 * ----------------------------------------------------------------------- */
union kbase_ioctl_mem_alloc {
   struct {
      __u64 va_pages;
      __u64 commit_pages;
      __u64 extent;
      __u64 flags;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
   } out;
};
#define KBASE_IOCTL_MEM_ALLOC \
   _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)

/* Memory allocation flags (BASE_MEM_*) */
#define BASE_MEM_PROT_CPU_RD        ((__u64)(1ULL <<  0))
#define BASE_MEM_PROT_CPU_WR        ((__u64)(1ULL <<  1))
#define BASE_MEM_PROT_GPU_RD        ((__u64)(1ULL <<  2))
#define BASE_MEM_PROT_GPU_WR        ((__u64)(1ULL <<  3))
#define BASE_MEM_PROT_GPU_EX        ((__u64)(1ULL <<  4))
#define BASE_MEM_GROW_ON_GPF        ((__u64)(1ULL <<  9))
#define BASE_MEM_DONT_NEED          ((__u64)(1ULL << 11))
#define BASE_MEM_IMPORT_SHARED      ((__u64)(1ULL << 13))
#define BASE_MEM_COHERENT_SYSTEM    ((__u64)(1ULL << 14))
#define BASE_MEM_CACHED_CPU         ((__u64)(1ULL << 15))
#define BASE_MEM_TILER_ALIGN_TOP    ((__u64)(1ULL << 16))
#define BASE_MEM_UNCACHED_GPU       ((__u64)(1ULL << 17))
#define BASE_MEM_PERMANENT_KERNEL_MAPPING ((__u64)(1ULL << 19))
#define BASE_MEM_KERNEL_SYNC        ((__u64)(1ULL << 20))
#define BASE_MEM_NO_USER_FREE       ((__u64)(1ULL << 21))
#define BASE_MEM_FIXED              ((__u64)(1ULL << 27))

/* -----------------------------------------------------------------------
 * Memory query — query flags/size of an existing allocation.
 * ----------------------------------------------------------------------- */
union kbase_ioctl_mem_query {
   struct {
      __u64 gpu_addr;
      __u64 query;
   } in;
   struct {
      __u64 value;
   } out;
};
#define KBASE_IOCTL_MEM_QUERY \
   _IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)

/* -----------------------------------------------------------------------
 * Memory free — release an allocation previously created by MEM_ALLOC.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_mem_free {
   __u64 gpu_addr;
};
#define KBASE_IOCTL_MEM_FREE \
   _IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)

/* -----------------------------------------------------------------------
 * CPU–GPU cache synchronisation.
 * handle: GPU VA of the allocation.
 * user_addr: CPU virtual address of the mapped region.
 * size: byte length to sync.
 * type: direction (see KBASE_SYNC_* below).
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_sync {
   __u64 handle;
   __u64 user_addr;
   __u64 size;
   __u32 type;
   __u32 padding;
};
/* Sync direction */
#define KBASE_SYNC_TO_CPU    0
#define KBASE_SYNC_TO_DEVICE 1

#define KBASE_IOCTL_SYNC \
   _IOW(KBASE_IOCTL_TYPE, 8, struct kbase_ioctl_sync)

/* -----------------------------------------------------------------------
 * Memory import — map an external buffer (e.g. dma-buf) into the GPU
 * address space.
 * phandle: file descriptor (for UMM/dma-buf type).
 * type: one of BASE_MEM_IMPORT_TYPE_*.
 * flags: BASE_MEM_* access flags.
 * On success the output half contains the GPU VA, actual flags, and page count.
 * ----------------------------------------------------------------------- */
union kbase_ioctl_mem_import {
   struct {
      __u64 phandle;
      __u32 type;
      __u32 padding;
      __u64 flags;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
      __u64 va_pages;
   } out;
};
/* Import types */
#define BASE_MEM_IMPORT_TYPE_INVALID  0
#define BASE_MEM_IMPORT_TYPE_UMM      2 /* dma-buf (UMM = Unified Memory Model) */
#define BASE_MEM_IMPORT_TYPE_USER_BUFFER 3
#define BASE_MEM_IMPORT_TYPE_ANDROID_HARDWARE_BUFFER 4

#define KBASE_IOCTL_MEM_IMPORT \
   _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)

/* -----------------------------------------------------------------------
 * Memory commit — change the number of pages backing a sparse allocation.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_mem_commit {
   __u64 gpu_addr;
   __u64 pages;
};
#define KBASE_IOCTL_MEM_COMMIT \
   _IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_commit)

/* -----------------------------------------------------------------------
 * Memory alias — create an alias mapping of one or more BOs.
 * ----------------------------------------------------------------------- */
struct kbase_mem_alias_info {
   __u64 handle;
   __u64 offset;
   __u64 length;
};
union kbase_ioctl_mem_alias {
   struct {
      __u64 flags;
      __u64 stride;
      __u64 nents;
      __u64 aliasing_info;
   } in;
   struct {
      __u64 flags;
      __u64 gpu_va;
      __u64 va_pages;
   } out;
};
#define KBASE_IOCTL_MEM_ALIAS \
   _IOWR(KBASE_IOCTL_TYPE, 24, union kbase_ioctl_mem_alias)

/* -----------------------------------------------------------------------
 * CPU–GPU timestamp correlation.
 * ----------------------------------------------------------------------- */
struct kbase_ioctl_get_cpu_gpu_timeinfo {
   struct {
      __u32 request_flags;
      __u32 paddings[7];
   } in;
   struct {
      __u64 sec;
      __u32 nsec;
      __u32 padding;
      __u64 timestamp;
      __u64 cycle_counter;
   } out;
};
#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO \
   _IOWR(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_get_cpu_gpu_timeinfo)

/* Flags for get_cpu_gpu_timeinfo request */
#define BASE_TIMEINFO_CYCLE_COUNTER_FLAG   (1U << 0)
#define BASE_TIMEINFO_TIMESTAMP_FLAG       (1U << 1)
#define BASE_TIMEINFO_MONOTONIC_FLAG       (1U << 2)
