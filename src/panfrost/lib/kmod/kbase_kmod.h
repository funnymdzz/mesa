/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct drm_panthor_csif_info;

struct pan_kmod_dev;

/* CSF interface information for a kbase CSF device, presented in the
 * panthor uAPI layout so CSF-generic code can consume either backend.
 * Filled from KBASE_IOCTL_CS_GET_GLB_IFACE at device-create time.
 * Only valid for CSF (arch >= 10) kbase devices.
 */
const struct drm_panthor_csif_info *
kbase_kmod_get_csif_props(const struct pan_kmod_dev *dev);

#if defined(__cplusplus)
} // extern "C"
#endif
