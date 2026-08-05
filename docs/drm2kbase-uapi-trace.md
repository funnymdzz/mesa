# PanVK Kbase UAPI Trace for DroidVM

This document records the Kbase operations used by the `droidvm` PanVK branch.
It is the guest-side input to the versioned `drm2kbase` native-context protocol.
It is not a declaration that native Kbase ioctl structures are a safe wire ABI.

## Target

- Device: Google Pixel 7 / Tensor G2 / GS201 / Mali-G710.
- Host kernel family: Android Linux 6.1, proprietary Mali Kbase CSF.
- Device node: `/dev/mali0`.
- Runtime-observed driver: `r54p3-00eac0`.
- Runtime-observed CSF UK UAPI: `1.38`.
- Guest userspace: arm64.

These values come from the separately captured proprietary-driver analysis in
`/home/ubuntu/panvk/libmali/ANALYSIS-G710.md`; they are not established by the
partial `/home/ubuntu/kref` source set.

The working compatibility header is `include/drm-uapi/mali_kbase_ioctl.h`.
It was assembled from older Arm UAPI headers and validated against observed
ioctl behavior. It is not the authoritative Google r54p3 header set. Before a
production wire ABI is frozen, retrieve the exact target kernel UAPI headers
and ioctl dispatcher and compare every structure size, command number, and
reserved-field rule.

## Execution Model

The active path is:

```text
PanVK
  -> pan_kmod Kbase backend
  -> per-VkDevice /dev/mali0 fd and implicit Kbase GPU address space
  -> userspace-owned CS rings
  -> CSF queue register/bind
  -> mapped USER_IO CS_INSERT and doorbell
  -> Kbase notification poll/read
```

This is not Panthor DRM. Panthor structs are used in a few internal Mesa
interfaces, but no Panthor ioctl is issued on the Kbase path.

## Context Initialization

Each physical-device probe and each logical `VkDevice` opens a fresh Kbase fd.
The logical device must not duplicate the physical-device fd because the Kbase
version handshake is per context and the fd owns an implicit GPU address space.

Required order:

1. `KBASE_IOCTL_VERSION_CHECK_CSF` (number 52).
2. `KBASE_IOCTL_SET_FLAGS` (number 1).
3. Map `BASE_MEM_MAP_TRACKING_HANDLE` (`0x3000`) as one `PROT_NONE` page.
4. Query GPU properties with a size probe followed by a fill request.
5. Initialize JIT/CUSTOM_VA with `KBASE_IOCTL_MEM_JIT_INIT` (number 14).
6. Initialize executable VA with `KBASE_IOCTL_MEM_EXEC_INIT` (number 38).
7. Query CSF global interface with a size probe followed by a fill request.
8. Optionally map `BASEP_MEM_CSF_USER_REG_PAGE_HANDLE` (`0x2f000`) read-only;
   failure conservatively makes the latest-flush value zero.
9. Optionally open `/dev/dma_heap/system`.
10. Optionally create a KCPU queue.

Relevant implementations:

- `src/panfrost/lib/kmod/kbase_kmod.c:kbase_kmod_dev_create`
- `src/panfrost/lib/kmod/kbase_kmod.c:kbase_get_gpuprops`
- `src/panfrost/lib/kmod/kbase_kmod.c:kbase_query_csif_info`
- `src/panfrost/vulkan/panvk_vX_device.c:create_device`

For the v1 triangle milestone, successful JIT/CUSTOM_VA initialization is
required for tiler heaps. Executable-VA availability is also required for shader
BOs, either through successful EXEC init or a target UAPI that establishes it
automatically. The native-context host must fail context creation when those
required allocation classes are unavailable; it must not copy the current
warning-only behavior blindly.

## Pointer-Bearing Ioctls

The following native fields are process-local pointers and must never be copied
verbatim into a native-context packet:

| Native field | Host-side reconstruction |
|---|---|
| `kbase_ioctl_get_gpuprops.buffer` | Allocate a bounded host output buffer and return serialized bytes. |
| `kbase_ioctl_cs_get_glb_iface.groups_ptr` | Allocate a bounded host group array. |
| `kbase_ioctl_cs_get_glb_iface.streams_ptr` | Allocate a bounded host stream array. |
| `kbase_ioctl_mem_alias.aliasing_info` | Resolve protocol memory IDs into a host alias array. |
| `kbase_ioctl_mem_import.phandle` | Receive/duplicate a dma-buf fd out of band and point to a host-local `int`. |
| `kbase_ioctl_mem_sync.user_addr` | Resolve memory ID plus offset into a host CPU mapping. |
| `kbase_ioctl_kcpu_queue_enqueue.addr` | Rebuild a bounded host command array. |
| KCPU CQS/fence nested pointers | Rebuild host arrays and return sync-file fds out of band. |

All protocol requests use fixed-width scalars, explicit array counts, and
opaque object IDs. Host pointers, mmap cookies, Kbase handles, callback
pointers, and fd integers are prohibited on the wire.

## Memory and GPU VA

### Native allocations

The current backend chooses the assembled-header `KBASE_IOCTL_MEM_ALLOC_EX`
(number 59) for UAPI 1.9 or newer and falls back to
`KBASE_IOCTL_MEM_ALLOC` (number 5). These values were accepted by the tested
binary/kernel combination but remain unverified against the matching r54p3
ioctl dispatcher.

Normal non-executable BOs request `BASE_MEM_SAME_VA`. The returned `gpu_va` is
initially an mmap cookie. Mapping the Kbase fd establishes the allocation, and
the returned CPU virtual address becomes the GPU virtual address. Executable
BOs use the EXEC zone and receive a real GPU VA before mmap.

Consequences for DroidVM:

- A guest process pointer cannot be reused as a host process pointer.
- Guest-chosen IOVA, as used by the KGSL/msm backend, is not the Kbase model.
- The host must allocate and own the Kbase GPU VA.
- The guest must receive that GPU VA before writing command streams or GPU
  descriptors that embed it.
- CPU-visible memory must be genuinely shared or explicitly uploaded before
  `QUEUE_PUBLISH`.

The protocol therefore returns `{memory_id, gpu_va, size, map_attributes}`.
`gpu_va` is valid only in the server-side Kbase context that owns `memory_id`.

### dma-buf allocation/import

Eligible BOs may be allocated from dma-heap and imported with
`KBASE_IOCTL_MEM_IMPORT` (number 22). The native `phandle` points to a local fd;
the fd value itself is not portable. Imported UMM memory uses:

- a Kbase-fd mmap to establish the GPU mapping; and
- a dma-buf mmap for CPU access on Pixel kernels that reject CPU faults through
  the Kbase UMM mapping.

Guest-backed blob support therefore needs crosvm to pass a duplicated dma-buf
fd into virglrenderer. Shared dma-buf import is one supported backing mode, but
cannot cover executable, grow-on-fault, CS ring, or CSF-event objects that must
remain native Kbase allocations. Those objects require a guest-visible staging
blob and explicit upload/download operations.

### Alias mappings

PanVK uses `KBASE_IOCTL_MEM_ALIAS` (number 21) for the descriptor-ring mirror.
The protocol expresses each alias as a source memory ID, page offset, page
length, and repetition/stride. The host rebuilds `base_mem_aliasing_info[]`.
Aliases are GPU-VA views only: they have no independent staging blob, and CPU
updates are made/uploaded through their source objects. Each alias retains all
source memory objects until the alias is destroyed.

### Destruction

- SAME_VA and alias mappings are released by `munmap()`.
- Non-SAME_VA allocations additionally require `KBASE_IOCTL_MEM_FREE`.
- A protocol `MEM_FREE` operation destroys the whole typed object; it is not a
  generic CPU unmap request.

## Cache and Visibility

PanVK currently uses two cache-maintenance paths:

1. arm64 userspace cache operations (`dc cvac`, `dc civac`, `dsb sy`);
2. `KBASE_IOCTL_MEM_SYNC` (number 15) as a configurable fallback.

Direct userspace operations cover CS rings, both classes of CSF-event objects,
queue initialization streams, tiler-heap descriptors, and descriptor-ring
synchronization metadata. Generic Vulkan flush/invalidate ranges may use either
userspace cache operations or MEM_SYNC.

The native-context contract must define visibility at `QUEUE_PUBLISH`:

- all dirty ring ranges are visible to the host GPU;
- all BO writes declared by the guest are clean before CS_INSERT is advanced;
- completion reads are invalidated before values are returned to the guest.

If CPU mappings are not truly coherent shared mappings, the transport must use
explicit upload/download operations. A local shared dma-buf implementation may
make those no-ops while preserving their ordering semantics. Kbase-native
objects always use staging-copy semantics unless a future exportable native
mapping mechanism is proven.

## CSF Groups and Queues

Runtime testing on the recorded Pixel 7 build found that multiple CSI bindings
were accepted but only CSI0 executed reliably. The root cause and applicability
to other r54p3 stacks are unverified. Current PanVK therefore creates three CSGs
per Vulkan queue:

- vertex/tiler CSG, CSI0;
- fragment CSG, CSI0;
- compute CSG, CSI0.

Each subqueue is created in this order:

1. Create group with the assembled-header current ioctl 58 when available; fall
   back to the 1.6 ioctl 42.
2. Allocate a 64 KiB native Kbase CS ring BO. Available target kernel evidence
   rejects imported and shrinkable queue-ring regions.
3. Register it with `KBASE_IOCTL_CS_QUEUE_REGISTER` (number 36).
4. Bind it at CSI0 with `KBASE_IOCTL_CS_QUEUE_BIND` (number 39).
5. Map the returned USER_IO cookie as three pages.

USER_IO pages:

| Page | Fields used |
|---|---|
| 0 | Doorbell write. |
| 1 | `CS_INSERT` at 0x0, `CS_EXTRACT_INIT` at 0x8. |
| 2 | `CS_EXTRACT` at 0x0, `CS_ACTIVE` at 0x8. |

Direct USER_IO sharing is optional. Protocol v1 requires `QUEUE_PUBLISH`, where
the host first copies declared dirty bytes from the guest staging blob into the
native ring, then writes CS_INSERT, performs barriers, rings the doorbell if
active, and falls back to `KBASE_IOCTL_CS_QUEUE_KICK` (number 37). This avoids
exposing live firmware register pages to the guest and gives the host a
validation point.

Queue teardown order:

1. Wait or terminate outstanding work.
2. Unmap USER_IO.
3. `KBASE_IOCTL_CS_QUEUE_TERMINATE` (number 41).
4. Free the ring BO after queue termination.
5. `KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE` (number 43).

At logical queue teardown, all groups are terminated before current or retired
tiler heaps are terminated. A completed ring alone does not remove firmware's
retained `HEAP_SET` references.

## Tiler Heap

PanVK uses `KBASE_IOCTL_CS_TILER_HEAP_INIT` (number 48) and TERM (number 49).
The host returns heap-context and first-chunk GPU VAs. These addresses belong to
the same Kbase context and may be embedded in command streams.

The Pixel compatibility path periodically drains graphics work and replaces the
heap to avoid per-CSG memory growth. The old heap is retained until both
graphics subqueues have executed a later wrapper that reissues `HEAP_SET`;
destroying it immediately after the pre-renewal drain can leave firmware state
pointing at reused custom VA. Heap object IDs must therefore have an independent
lifecycle from queue/group IDs.

## Submission

There is no Kbase submit ioctl for the active CSF path. Submission consists of:

1. CPU-side waits for incoming Vulkan syncs.
2. Flush pending mapped-memory synchronization.
3. Write wrapper command streams into mapped CS ring memory.
4. Clean the written ring range.
5. Publish the new byte offset to CS_INSERT.
6. Ring the doorbell or issue queue kick.

The wrapper stream contains server-context GPU VAs for application command
streams, completion cells, descriptors, shaders, and the tiler heap. These bytes
are valid only if all returned GPU VAs belong to the same host Kbase context.

`QUEUE_PUBLISH` must validate:

- queue and ring object ownership;
- monotonic ring occupancy with checked arithmetic:
  `new_insert >= old_insert`, `old_insert - extract <= ring_size`, and
  `new_insert - extract <= ring_size`;
- dirty ranges completely cover newly exposed bytes from old insert to new
  insert, including physical wrap padding;
- that all declared directly referenced protocol objects belong to the context;
- context-lost state before touching Kbase.

Deep validation of arbitrary Mali CS instructions and recursively referenced
descriptors is not a v1 requirement. The host can validate only declared
objects, queue/ring ownership, upload ranges, and publish arithmetic. Therefore
v1 conservatively forbids freeing any GPU-visible memory, alias, or tiler heap
while any context queue has an incomplete published generation. This is less
efficient than per-submit dependency tracking but prevents stale-VA teardown
without claiming full command-stream validation.

## Logical Queue Initialization

Native REGISTER/BIND only creates the firmware-facing queue. PanVK must still:

1. Allocate one CSF-event BO for inter-subqueue synchronization objects.
2. Allocate a separate CSF-event BO containing the three wrapper completion
   cells (the cells are subranges of one page, not separate BOs).
3. Initialize synchronization values, queue context registers, scoreboard
   state, and graphics heap state in per-subqueue initialization streams.
4. Clean/upload those streams and metadata.
5. Publish all three groups before waiting.
6. Synchronously verify each completion write; ring drain alone is not success.

The guest native-context backend performs these steps through ordinary memory
and queue operations. A logical Vulkan queue is not ready until they complete.

## Completion and Notifications

Each subqueue increments a 64-bit completion cell in the wrapper-completion
CSF-event BO. Application streams use the separate CSF-event synchronization BO
for cross-subqueue dependencies. CPU waits normally:

1. inspect CS_EXTRACT and CS_ACTIVE;
2. invalidate/read the completion cell;
3. re-kick stalled inactive queues;
4. wait for the context notification owner to poll/read Kbase events.

The notification stream carries group fatal, queue fatal, timeout, tiler OOM,
and kernel-classified recoverable queue-fault records. Consumption is ordered
and stateful. Current PanVK latches all group errors as device loss and does not
implement clear-fault recovery.

Protocol v1 provides normalized event records and a `WAIT_SEQNO` operation. It
does not serialize PanVK's process-local CPU sync object, which contains callback
and queue pointers. A pending Vulkan sync is represented as:

```text
queue_id + three per-subqueue target sequence numbers
```

Optional KCPU CQS64/sync-file conversion is feature-gated. When enabled it is
attempted before the notification-wait fallback. It is not required for the
initial `vulkaninfo` and triangle milestones.

## Reachable Ioctl Set

Mandatory or normal-path commands:

| Number | Command |
|---:|---|
| 1 | `KBASE_IOCTL_SET_FLAGS` |
| 3 | `KBASE_IOCTL_GET_GPUPROPS` |
| 5 | assembled-header `KBASE_IOCTL_MEM_ALLOC` fallback |
| 7 | `KBASE_IOCTL_MEM_FREE` |
| 14 | `KBASE_IOCTL_MEM_JIT_INIT` |
| 21 | `KBASE_IOCTL_MEM_ALIAS` |
| 22 | `KBASE_IOCTL_MEM_IMPORT` when dma-buf is used |
| 36 | `KBASE_IOCTL_CS_QUEUE_REGISTER` |
| 37 | `KBASE_IOCTL_CS_QUEUE_KICK` |
| 38 | `KBASE_IOCTL_MEM_EXEC_INIT` |
| 39 | `KBASE_IOCTL_CS_QUEUE_BIND` |
| 41 | `KBASE_IOCTL_CS_QUEUE_TERMINATE` |
| 42 | assembled-header `KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6` fallback |
| 43 | `KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE` |
| 48 | `KBASE_IOCTL_CS_TILER_HEAP_INIT` |
| 49 | `KBASE_IOCTL_CS_TILER_HEAP_TERM` |
| 51 | `KBASE_IOCTL_CS_GET_GLB_IFACE` |
| 52 | `KBASE_IOCTL_VERSION_CHECK_CSF` |
| 58 | assembled-header current `KBASE_IOCTL_CS_QUEUE_GROUP_CREATE` |
| 59 | assembled-header `KBASE_IOCTL_MEM_ALLOC_EX` |

Conditional commands:

- 15: MEM_SYNC fallback.
- 45/46/47: optional KCPU queue and sync-file path.
- 50: CPU/GPU timestamp correlation.
- DMA_HEAP_IOCTL_ALLOC on `/dev/dma_heap/system`.

The command numbers and layouts in this table are working assembled-header
values observed on the tested setup, not yet verified against the matching
r54p3 dispatcher.

The working code does not use JM JOB_SUBMIT, Panthor group submit, DRM syncobj,
MEM_QUERY, MEM_COMMIT, or CS_EVENT_SIGNAL for normal PanVK CSF execution.

## ABI Gaps Before Production Freeze

The partial Pixel-era kernel files under `/home/ubuntu/kref` reference newer
operations absent from the assembled header, including queue register-ex and
queue-group clear-faults. The exact r54p3 command numbers and layouts remain
unknown.

Required follow-up evidence:

1. Exact Android build fingerprint, kernel build ID, and Mali module hash.
2. Matching Google/Arm r54p3 UAPI headers and ioctl dispatcher.
3. Payload-aware traces of proprietary libmali and this PanVK branch on the
   same minimal workloads.
4. Verification of mmap cookie dispatch and reserved-field checks.
5. Confirmation that 32-bit guests are out of scope; protocol v1 assumes an
   arm64 guest but remains fixed-width and pointer-size independent.

Until those are available, the wire protocol must expose normalized operations
and negotiated features rather than mirroring any unverified native structure.
