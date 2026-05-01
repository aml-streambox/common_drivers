/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_CONNECTOR_H__
#define __MESON_T7_CONNECTOR_H__

#include <drm/drm_connector.h>
#include <drm/drm_modes.h>

#define MESON_T7_CONNECTOR_TYPE_PROP_NAME "meson.connector_type"

enum {
	MESON_T7_DRM_CONNECTOR_V10 = 0,
};

struct meson_t7_connector_dev {
	int ver;
};

struct meson_t7_hdmitx_dev {
	struct meson_t7_connector_dev base;
};

struct meson_t7_dummyl_dev {
	struct meson_t7_connector_dev base;
};

struct meson_t7_dummyp_dev {
	struct meson_t7_connector_dev base;
};

#define DRM_MODE_CONNECTOR_MESON_DUMMY_L	0x200
#define DRM_MODE_CONNECTOR_MESON_DUMMY_P	0x201

#define DRM_MODE_MESON_CONNECTOR_PANEL_START	0xff
#define DRM_MODE_MESON_CONNECTOR_PANEL_END	0x1ff

#define to_meson_t7_hdmitx_dev(x) \
	container_of(x, struct meson_t7_hdmitx_dev, base)
#define to_meson_t7_dummyl_dev(x) \
	container_of(x, struct meson_t7_dummyl_dev, base)
#define to_meson_t7_dummyp_dev(x) \
	container_of(x, struct meson_t7_dummyp_dev, base)

#endif /* __MESON_T7_CONNECTOR_H__ */
