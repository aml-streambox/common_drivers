/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef __MESON_T7_DRV_H__
#define __MESON_T7_DRV_H__

#include <linux/device.h>
#include <linux/types.h>

#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>

#include "meson_t7_bind.h"
#include "meson_t7_pipeline.h"
#include "meson_t7_print.h"

#define MESON_T7_MAX_CRTC	3
#define MESON_T7_MAX_OSD	4
#define MESON_T7_MAX_VIDEO	3

enum meson_t7_vpu_enc_type {
	MESON_T7_ENCODER_HDMI = 0,
	MESON_T7_ENCODER_LCD,
	MESON_T7_ENCODER_CVBS,
	MESON_T7_ENCODER_MAX,
};

struct meson_t7_connector {
	struct drm_connector connector;
	struct meson_t7_drm *drm_priv;
	void (*update)(struct drm_connector_state *new_state,
		       struct drm_connector_state *old_state);
};

#define connector_to_meson_t7_connector(x) \
	container_of(x, struct meson_t7_connector, connector)

struct meson_t7_of_conf {
	u32 crtc_masks[MESON_T7_ENCODER_MAX];
	u32 vfm_mode;
	u32 osd_afbc_mask;
	u32 crtcmask_osd[MESON_T7_MAX_OSD];
	u32 crtcmask_video[MESON_T7_MAX_VIDEO];
	u64 drm_policy_mask;
	const char *pref_mode;
	u32 force_slice;
};

struct meson_t7_drm {
	struct device *dev;
	struct drm_device *drm;
	struct meson_t7_vpu_pipeline *pipeline;
	struct meson_t7_of_conf of_conf;
	struct meson_t7_drm_bound_data bound_data;
	u32 osd_occupied_index;
	u8 dummyl_from_hdmitx;
	u8 remove_get_vblank_timestamp;
	u32 pxp_mode;
};

void meson_t7_of_init(struct device *vpu_dev, struct drm_device *drm,
			     struct meson_t7_drm *priv);

#endif /* __MESON_T7_DRV_H__ */
