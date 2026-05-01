/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_BIND_H__
#define __MESON_T7_BIND_H__

#include <drm/drm_device.h>

#include "meson_t7_connector.h"

struct meson_t7_drm_bound_data {
	struct drm_device *drm;

	int (*connector_component_bind)(struct drm_device *drm, int type,
					      struct meson_t7_connector_dev *intf);
	int (*connector_component_unbind)(struct drm_device *drm, int type,
						int connector_id);
};

int meson_t7_connector_dev_bind(struct drm_device *drm, int type,
				       struct meson_t7_connector_dev *intf);
int meson_t7_connector_dev_unbind(struct drm_device *drm, int type,
					 int connector_id);

#endif /* __MESON_T7_BIND_H__ */
