// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/printk.h>

#include <drm/drm_connector.h>

#include "meson_t7_bind.h"
#include "meson_t7_dummyl.h"

int meson_t7_connector_dev_bind(struct drm_device *drm, int type,
				       struct meson_t7_connector_dev *intf)
{
	if (type > DRM_MODE_MESON_CONNECTOR_PANEL_START &&
	    type < DRM_MODE_MESON_CONNECTOR_PANEL_END)
		return -EOPNOTSUPP;

	switch (type) {
	case DRM_MODE_CONNECTOR_MESON_DUMMY_L:
		return meson_t7_dummyl_dev_bind(drm, type, intf);
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
	case DRM_MODE_CONNECTOR_TV:
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_DSI:
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_MESON_DUMMY_P:
		return -EOPNOTSUPP;
	default:
		pr_err("meson-t7-drm: unknown connector type %d\n", type);
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(meson_t7_connector_dev_bind);

int meson_t7_connector_dev_unbind(struct drm_device *drm, int type,
					 int connector_id)
{
	if (type > DRM_MODE_MESON_CONNECTOR_PANEL_START &&
	    type < DRM_MODE_MESON_CONNECTOR_PANEL_END)
		return -EOPNOTSUPP;

	switch (type) {
	case DRM_MODE_CONNECTOR_MESON_DUMMY_L:
		return meson_t7_dummyl_dev_unbind(drm, type, connector_id);
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
	case DRM_MODE_CONNECTOR_TV:
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_DSI:
	case DRM_MODE_CONNECTOR_eDP:
	case DRM_MODE_CONNECTOR_MESON_DUMMY_P:
		return -EOPNOTSUPP;
	default:
		pr_err("meson-t7-drm: unknown connector type %d\n", type);
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(meson_t7_connector_dev_unbind);
