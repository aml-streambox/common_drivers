/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_PRINT_H__
#define __MESON_T7_PRINT_H__

#include <drm/drm_print.h>

#define MESON_T7_DRM_UT_FENCE		0x200
#define MESON_T7_DRM_UT_REG		0x400
#define MESON_T7_DRM_UT_FBDEV		0x800
#define MESON_T7_DRM_UT_TRAVERSE	0x1000
#define MESON_T7_DRM_UT_BLOCK		0x2000

#define MESON_T7_DRM_FENCE(fmt, ...)	\
	__drm_dbg(MESON_T7_DRM_UT_FENCE, fmt, ##__VA_ARGS__)
#define MESON_T7_DRM_REG(fmt, ...)	\
	__drm_dbg(MESON_T7_DRM_UT_REG, fmt, ##__VA_ARGS__)
#define MESON_T7_DRM_FBDEV(fmt, ...)	\
	__drm_dbg(MESON_T7_DRM_UT_FBDEV, fmt, ##__VA_ARGS__)
#define MESON_T7_DRM_TRAVERSE(fmt, ...)	\
	__drm_dbg(MESON_T7_DRM_UT_TRAVERSE, fmt, ##__VA_ARGS__)
#define MESON_T7_DRM_BLOCK(fmt, ...)	\
	__drm_dbg(MESON_T7_DRM_UT_BLOCK, fmt, ##__VA_ARGS__)

#endif /* __MESON_T7_PRINT_H__ */
