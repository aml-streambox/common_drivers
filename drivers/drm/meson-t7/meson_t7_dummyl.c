// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2021 Amlogic, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/slab.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "meson_t7_dummyl.h"

static const struct drm_display_mode meson_t7_dummy_mode = {
	.name = "dummy_l",
	.type = DRM_MODE_TYPE_USERDEF,
	.status = MODE_OK,
	.clock = 25000,
	.hdisplay = 720,
	.hsync_start = 736,
	.hsync_end = 798,
	.htotal = 858,
	.vdisplay = 480,
	.vsync_start = 489,
	.vsync_end = 495,
	.vtotal = 525,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static int meson_t7_dummyl_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &meson_t7_dummy_mode);
	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);

	return 1;
}

static enum drm_mode_status
meson_t7_dummyl_check_mode(struct drm_connector *connector,
				   const struct drm_display_mode *mode)
{
	return MODE_OK;
}

static struct drm_encoder *
meson_t7_dummyl_best_encoder(struct drm_connector *connector)
{
	struct meson_t7_dummyl *dummyl = connector_to_meson_t7_dummyl(connector);

	return &dummyl->encoder;
}

static const struct drm_connector_helper_funcs meson_t7_dummyl_helper_funcs = {
	.get_modes = meson_t7_dummyl_get_modes,
	.mode_valid = meson_t7_dummyl_check_mode,
	.best_encoder = meson_t7_dummyl_best_encoder,
};

static enum drm_connector_status
meson_t7_dummyl_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void meson_t7_dummyl_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static void meson_t7_dummyl_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
}

static struct drm_connector_state *
meson_t7_dummyl_connector_duplicate_state(struct drm_connector *connector)
{
	return drm_atomic_helper_connector_duplicate_state(connector);
}

static void
meson_t7_dummyl_connector_destroy_state(struct drm_connector *connector,
					struct drm_connector_state *state)
{
	drm_atomic_helper_connector_destroy_state(connector, state);
}

static int meson_t7_dummyl_atomic_set_property(struct drm_connector *connector,
					      struct drm_connector_state *state,
					      struct drm_property *property,
					      u64 val)
{
	return -EINVAL;
}

static int meson_t7_dummyl_atomic_get_property(struct drm_connector *connector,
					      const struct drm_connector_state *state,
					      struct drm_property *property,
					      u64 *val)
{
	return -EINVAL;
}

static const struct drm_connector_funcs meson_t7_dummyl_funcs = {
	.detect = meson_t7_dummyl_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = meson_t7_dummyl_connector_destroy,
	.reset = meson_t7_dummyl_connector_reset,
	.atomic_duplicate_state = meson_t7_dummyl_connector_duplicate_state,
	.atomic_destroy_state = meson_t7_dummyl_connector_destroy_state,
	.atomic_set_property = meson_t7_dummyl_atomic_set_property,
	.atomic_get_property = meson_t7_dummyl_atomic_get_property,
};

static void meson_t7_dummyl_encoder_atomic_enable(struct drm_encoder *encoder,
						 struct drm_atomic_state *state)
{
}

static void meson_t7_dummyl_encoder_atomic_disable(struct drm_encoder *encoder,
						  struct drm_atomic_state *state)
{
}

static int meson_t7_dummyl_encoder_atomic_check(struct drm_encoder *encoder,
					       struct drm_crtc_state *crtc_state,
					       struct drm_connector_state *conn_state)
{
	return 0;
}

static const struct drm_encoder_helper_funcs meson_t7_dummyl_encoder_helper_funcs = {
	.atomic_enable = meson_t7_dummyl_encoder_atomic_enable,
	.atomic_disable = meson_t7_dummyl_encoder_atomic_disable,
	.atomic_check = meson_t7_dummyl_encoder_atomic_check,
};

static const struct drm_encoder_funcs meson_t7_dummyl_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int meson_t7_dummyl_dev_bind(struct drm_device *drm, int type,
				    struct meson_t7_connector_dev *intf)
{
	struct meson_t7_drm *priv = drm->dev_private;
	struct meson_t7_dummyl *dummyl;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	u32 i, num_postblend = 1;
	int ret;

	if (priv && priv->dummyl_from_hdmitx)
		return 0;

	dummyl = kzalloc(sizeof(*dummyl), GFP_KERNEL);
	if (!dummyl)
		return -ENOMEM;

	dummyl->drm = drm;
	dummyl->base.drm_priv = priv;
	encoder = &dummyl->encoder;

	if (priv && priv->pipeline && priv->pipeline->num_postblend)
		num_postblend = priv->pipeline->num_postblend;

	for (i = 0; i < num_postblend; i++)
		encoder->possible_crtcs |= BIT(i);

	drm_encoder_helper_add(encoder, &meson_t7_dummyl_encoder_helper_funcs);
	ret = drm_encoder_init(drm, encoder, &meson_t7_dummyl_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, "meson-t7-dummyl");
	if (ret)
		goto err_free;

	connector = &dummyl->base.connector;
	drm_connector_helper_add(connector, &meson_t7_dummyl_helper_funcs);
	ret = drm_connector_init(drm, connector, &meson_t7_dummyl_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		goto err_encoder_cleanup;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		goto err_connector_cleanup;

	return 0;

err_connector_cleanup:
	drm_connector_cleanup(connector);
err_encoder_cleanup:
	drm_encoder_cleanup(encoder);
err_free:
	kfree(dummyl);
	return ret;
}

int meson_t7_dummyl_dev_unbind(struct drm_device *drm, int type,
				      int connector_id)
{
	struct drm_connector *connector;
	struct meson_t7_dummyl *dummyl;

	connector = drm_connector_lookup(drm, NULL, connector_id);
	if (!connector)
		return -EINVAL;

	dummyl = connector_to_meson_t7_dummyl(connector);
	drm_connector_put(connector);
	kfree(dummyl);

	return 0;
}
