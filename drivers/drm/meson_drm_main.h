/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef _MESON_DRM_MAIN_H__
#define _MESON_DRM_MAIN_H__

#if defined(CONFIG_AMLOGIC_DRM_VPU) || defined(CONFIG_AMLOGIC_DRM_MODULE)
int am_meson_vpu_init(void);
void am_meson_vpu_exit(void);
#else
static inline int am_meson_vpu_init(void)
{
	return 0;
}

static inline void am_meson_vpu_exit(void) {}
#endif

int am_meson_drm_init(void);
void am_meson_drm_exit(void);

#endif
