// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <linux/dma-mapping.h>
#include <linux/of.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "meson_t7_drv.h"

static void meson_t7_parse_crtc_masks(struct device_node *node,
				      struct meson_t7_of_conf *conf)
{
	u32 crtc_masks[MESON_T7_ENCODER_MAX];
	int i, ret;

	for (i = 0; i < MESON_T7_ENCODER_MAX; i++)
		conf->crtc_masks[i] = 1;

	ret = of_property_read_u32_array(node, "crtc_masks", crtc_masks,
					  MESON_T7_ENCODER_MAX);
	if (ret)
		return;

	for (i = 0; i < MESON_T7_ENCODER_MAX; i++)
		conf->crtc_masks[i] = crtc_masks[i];
}

static void meson_t7_parse_dma_mask(struct device *dev)
{
	u32 vpu_dma_mask;
	int ret;

	ret = of_property_read_u32(dev->of_node, "vpu_dma_mask", &vpu_dma_mask);
	if (ret || vpu_dma_mask != 1)
		return;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		dev_err(dev, "failed to set display DMA mask\n");
}

static void meson_t7_connect_parse_config(struct drm_device *drm,
					 struct meson_t7_of_conf *conf)
{
	if (of_property_read_string(drm->dev->of_node, "pref_mode",
				    &conf->pref_mode))
		conf->pref_mode = NULL;
}

static void meson_t7_video_parse_config(struct drm_device *drm,
				       struct meson_t7_of_conf *conf)
{
	u32 mode_flag = 0;

	of_property_read_u32(drm->dev->of_node, "vfm_mode", &mode_flag);
	conf->vfm_mode = mode_flag;
}

static void meson_t7_osd_parse_config(struct drm_device *drm,
				     struct meson_t7_of_conf *conf)
{
	u32 osd_afbc_mask = 0xff;
	u32 osd_force_slice = 0;

	of_property_read_u32(drm->dev->of_node, "osd_afbc_mask", &osd_afbc_mask);
	conf->osd_afbc_mask = osd_afbc_mask;

	of_property_read_u32(drm->dev->of_node, "force_slice", &osd_force_slice);
	conf->force_slice = osd_force_slice;
}

static void meson_t7_parse_gfcd_config(struct drm_device *drm,
				      struct meson_t7_of_conf *conf)
{
	u32 value = 0;

	of_property_read_u32(drm->dev->of_node, "gfcd_afbc_enable", &value);
	conf->drm_policy_mask |= ((u64)!!value << MESON_T7_GFCD_ODD_SIZE);
}

static void meson_t7_get_plane_crtc_mask(struct meson_t7_drm *priv,
					const char *name, u32 num, u32 *crtc_mask)
{
	struct device_node *np = priv->dev->of_node;

	of_property_read_u32_array(np, name, crtc_mask, num);
}

void meson_t7_of_init(struct device *vpu_dev, struct drm_device *drm,
			     struct meson_t7_drm *priv)
{
	struct meson_t7_of_conf *conf = &priv->of_conf;
	struct meson_t7_vpu_pipeline *pipeline = priv->pipeline;
	u32 osd_occupied_index;

	meson_t7_parse_crtc_masks(drm->dev->of_node, conf);
	meson_t7_parse_dma_mask(drm->dev);

	if (pipeline) {
		of_property_read_u8(vpu_dev->of_node, "osd_ver",
				    &pipeline->osd_version);
		of_property_read_u32(vpu_dev->of_node, "osd_axi_sel",
				     &pipeline->osd_axi_sel);
	}

	if (!of_property_read_u32(vpu_dev->of_node, "osd_occupied_index",
				   &osd_occupied_index))
		priv->osd_occupied_index = osd_occupied_index;

	of_property_read_u8(drm->dev->of_node, "dummyl_from_hdmitx",
			    &priv->dummyl_from_hdmitx);
	of_property_read_u8(drm->dev->of_node, "remove_get_vblank_timestamp",
			    &priv->remove_get_vblank_timestamp);
	of_property_read_u32(drm->dev->of_node, "pxp_mode", &priv->pxp_mode);

	if (pipeline) {
		meson_t7_get_plane_crtc_mask(priv, "crtcmask_of_osd",
					  pipeline->num_osds, conf->crtcmask_osd);
		meson_t7_get_plane_crtc_mask(priv, "crtcmask_of_video",
					  pipeline->num_video, conf->crtcmask_video);
		meson_t7_get_plane_crtc_mask(priv, "overwrite_crtcmask_of_osd",
					  pipeline->num_osds, conf->crtcmask_osd);
		meson_t7_get_plane_crtc_mask(priv, "overwrite_crtcmask_of_video",
					  pipeline->num_video, conf->crtcmask_video);
	}

	meson_t7_osd_parse_config(drm, conf);
	meson_t7_video_parse_config(drm, conf);
	meson_t7_connect_parse_config(drm, conf);
	meson_t7_parse_gfcd_config(drm, conf);
}
