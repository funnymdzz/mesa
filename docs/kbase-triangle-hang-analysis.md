<!--
Copyright 2026 Collabora, Ltd.
SPDX-License-Identifier: MIT
-->

# kbase panvk: triangle draw hang — investigation state

Milestone-2 status: device enumeration + vkCreateDevice + queue init all
work on kbase (Pixel 7 / Mali-G710 / CSF uAPI 1.38).
`dEQP-VK.api.smoke.triangle` still fails: `vkQueueSubmit` →
`VK_ERROR_DEVICE_LOST`.

## Ground-truth facts (hardware, do not re-derive)

Each Vulkan queue = one kbase CS queue group with 3 subqueues
(0=VERTEX_TILER, 1=FRAGMENT, 2=COMPUTE), each a userspace ring driven by
flush+CALL wrappers. A smoke-triangle submit emits 3 jobs per subqueue
(target seqno 3): job-1 = init/setup, job-2 = tiny setup, job-3 = the real
draw stream.

On timeout **all three subqueues** are stuck with `extract == 608`, which
is exactly the wrapper CALL into the job-3 draw stream; `active=0`,
`error=0`. Ring completion seqnos are 0/0/2 and panvk cross-subqueue
syncobjs (init'd to 1) are 1/1/2.

**Critical correction to earlier rounds:** the seqno=2 on COMPUTE is just
its job-1 + job-2 completing (target is 3). COMPUTE's job-3 CALL is stuck
at 608 too. So this is **not** a VT↔FRAGMENT tiler cross-sync problem —
**all three job-3 draw-stream CALLs fail to return identically.** The hang
is in something common to every real draw stream, not in cross-queue sync.

`dmesg` is completely clean even with `printk` at level 8: no GPU fault, no
`PROGRESS_TIMER timeout`, no CS dump. Combined with `active=0`, the CSG is
suspended/idle, and the firmware reports nothing wrong. (Our CPU-side wait
times out at 10s and gives up before the kernel's progress timer fires.)

The job-3 draw stream (decoded via genxml CS opcodes) contains a
BRANCH/SET_SB_ENTRY/ADD_IMM32/WAIT loop and then **RUN_IDVS (op 0x06)** —
the instruction that dispatches vertex+tiler work to the shader cores. The
CALL not returning is consistent with execution stalling at/after RUN_IDVS
(actual shader/tiler execution), one layer below CS-level sync.

## Ruled out (all verified on hardware or by panfork/source)

- CPU cache coherency of ring/content (DSB SY + KBASE_IOCTL_MEM_SYNC).
- Stale subqueue context register (reload before CALL confirmed applied).
- REQ_RESOURCE mask (restored conservative COMPUTE|IDVS|TILER / COMPUTE|FRAG).
- CALL length units (verified bytes, matches panvk's own cs_call sites).
- Wrapper scoreboard/SB-state setup (byte-identical to init stream).
- Per-CSI kick / submission concurrency (all subqueues emitted+kicked
  before any wait — confirmed in panvk_queue_submit_ioctl_kbase).
- csi_handlers / BASE_CSF_TILER_OOM_EXCEPTION_FLAG — panfork renders GL on
  this exact device using legacy 1.6 group-create with no CSI exception
  handlers, so arming it is not required (reverted).
- tiler-heap memory group id (was passing CS group handle; harmless since
  it was 0 == default mem group, but corrected).
- tiler-heap descriptor base/bottom/top (shared with panthor path, values
  from kernel-returned first_chunk_va, look correct).

## Leading hypotheses (not yet tested)

1. **RUN_IDVS / shader-core execution doesn't start on kbase.** The draw
   streams dispatch real shader/tiler work; job-1/2 (setup only) complete,
   job-3 (with RUN_IDVS/RUN_FRAGMENT/RUN_COMPUTE) does not. Something the
   shader/tiler execution needs — shader program descriptor reachability,
   shader-core power/affinity, or a global endpoint-enable — may be unset
   on kbase. Note GL-on-kbase is gated off in pan_device.c and was never
   exercised, so this execution path is genuinely untested here.
2. **poll()/event-driven kernel servicing.** panfork allocates CSF event
   memory + a KCPU queue and poll()s the kbase fd during waits
   (pan_vX_base.c: alloc_event_mem, kbase_poll_event, kcpu_cqs_*), while
   our wait is a pure CPU spin that never poll()s the fd or creates event
   memory. Some kernel-side servicing tied to the draw may be starved.

## Tooling / environment

Local container cannot meson-build (no pkg-config/llvm/mesa_clc); validate
via GitHub Actions "Pixel 7 kbase debug Mesa", download artifact, test on
device. Kernel reference for this GPU: google-modules/gpu branch
android-gs-tangorpro-6.1-android16 (Tensor G2), cached under ~/kref during
investigation. panfork reference: github.com/tokokudo/mesa-Panfork-android
src/panfrost/base/. USER_IO page only exposes CS_INSERT/EXTRACT/ACTIVE;
CS_STATUS_WAIT/BLOCKED_REASON live in the firmware interface and are
readable only by the kernel, not userspace — so "where is the CS blocked"
can only come from a kernel CSG dump, which this Android kernel does not
emit at its default log level.
