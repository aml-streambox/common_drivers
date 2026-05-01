/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_DUMMYL_H__
#define __MESON_T7_DUMMYL_H__

#include <drm/drm_encoder.h>

#include "meson_t7_connector.h"
#include "meson_t7_drv.h"

struct meson_t7_dummyl {
	struct meson_t7_connector base;
	struct drm_encoder encoder;
	struct drm_device *drm;
};

#define connector_to_meson_t7_dummyl(x) \
	container_of(connector_to_meson_t7_connector(x), \
		     struct meson_t7_dummyl, base)
#define encoder_to_meson_t7_dummyl(x) \
	container_of(x, struct meson_t7_dummyl, encoder)

int meson_t7_dummyl_dev_bind(struct drm_device *drm, int type,
				    struct meson_t7_connector_dev *intf);
int meson_t7_dummyl_dev_unbind(struct drm_device *drm, int type,
				      int connector_id);

#endif /* __MESON_T7_DUMMYL_H__ */
