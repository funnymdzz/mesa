<!--
Copyright 2026 Collabora, Ltd.
SPDX-License-Identifier: MIT
-->

# kbase panvk: triangle draw hang resolution

## Status

The kbase CSF path can create a Vulkan device and execute graphics work on a
Pixel 7 with a Mali-G710 (kbase uAPI 1.38).  The following tests pass with the
Vulkan-only aarch64 build:

- `vulkaninfo --summary`
- `dEQP-VK.api.smoke.triangle` in five consecutive processes
- all six `dEQP-VK.api.smoke.*` cases

## Root cause

The Android kbase stack accepts registration and binding of CSI1 and CSI2 in
the same command-stream group as CSI0, and their user-visible `CS_EXTRACT`
values can advance to `CS_INSERT`.  That does not prove that those command
streams executed.  On the tested kernel, only CSI0 produced command-stream
breadcrumbs or completion writes.  CSI1 and CSI2 remained idle without a
reported CS fault.

This was obscured by the original initialization fallback, which treated a
drained ring as successful execution.  During a draw, the queue assigned CSI0
completed while the other two queues timed out.  Changing the logical queue
assigned to CSI0 changed which queue progressed, proving that the failure was
associated with adding later CSIs to the scheduled group rather than with a
particular PanVK draw stream.

All eight CSI feature words reported by the device are identical, so this is
not a per-CSI job-type restriction.  Legacy versus current group-create ABI,
resource-mask changes, cache maintenance, and command-stream CALL encoding
also did not change the single-CSG result.

## Compatibility design

The kbase backend now creates one command-stream group for each PanVK
subqueue.  Each group contains one queue, bound to CSI0:

- vertex/tiler: one CSG, CSI0, `IDVS | TILER` resources
- fragment: one CSG, CSI0, `FRAGMENT` resources
- compute: one CSG, CSI0, `COMPUTE` resources

The subqueues retain their existing shared GPU sync objects, so PanVK's
cross-subqueue dependencies continue to work across the three groups.  All
groups are published before ordinary submission waits begin, allowing the
firmware to run dependent graphics work concurrently.  Initialization is
validated strictly and sequentially; ring drain is no longer accepted as a
substitute for a completion write.

The current group-create ABI remains preferred so recoverable CS faults are
delivered through kbase notifications, with the legacy ABI retained as a
fallback.

## Validation evidence

On the Pixel 7, all three independent groups complete initialization through
CSI0.  A triangle submission then completes all three job-3 wrappers with
stream progress `0xff0`; dEQP reports `Pass (Rendering succeeded)`.  The full
API smoke group reports 6 passed, 0 failed, and 0 unsupported.

## Known limitation

Each Vulkan queue consumes three CSG slots instead of one.  The tested G710
reports eight slots, which is sufficient for this design.  Devices with fewer
than three concurrently schedulable groups need additional validation,
particularly around firmware time-slicing of cross-group GPU waits.  WSI and
broad Vulkan conformance remain outside this milestone.
