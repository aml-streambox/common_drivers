// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Linux v7.1 DRM master for the Amlogic T7 vendor display port.
 * Hardware objects are added incrementally after the VPU/vout/HDMITX blocks
 * are migrated; this layer only owns DRM device lifetime and native helpers.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>

#include "meson_t7_drv.h"

#define DRIVER_NAME	"meson-t7-drm"
#define DRIVER_DESC	"Amlogic T7 vendor DRM display driver"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

static const struct drm_mode_config_funcs meson_t7_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs meson_t7_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail,
};

DEFINE_DRM_GEM_DMA_FOPS(meson_t7_drm_fops);

static const struct drm_driver meson_t7_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.fops = &meson_t7_drm_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static void meson_t7_mode_config_setup(struct drm_device *drm)
{
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 8192;
	drm->mode_config.max_height = 8192;
	drm->mode_config.funcs = &meson_t7_mode_config_funcs;
	drm->mode_config.helper_private = &meson_t7_mode_config_helpers;
}

static int meson_t7_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_t7_drm *priv;
	struct drm_device *drm;
	int ret;

	drm = drm_dev_alloc(&meson_t7_drm_driver, dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_drm_put;
	}

	drm->dev_private = priv;
	priv->dev = dev;
	priv->drm = drm;
	priv->bound_data.drm = drm;
	priv->bound_data.connector_component_bind = meson_t7_connector_dev_bind;
	priv->bound_data.connector_component_unbind = meson_t7_connector_dev_unbind;
	priv->osd_occupied_index = ~0U;
	platform_set_drvdata(pdev, priv);

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_clear_drvdata;

	meson_t7_mode_config_setup(drm);
	meson_t7_of_init(dev, drm, priv);
	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_poll_fini;

	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(drm);
err_clear_drvdata:
	drm->dev_private = NULL;
	platform_set_drvdata(pdev, NULL);
err_drm_put:
	drm_dev_put(drm);
	return ret;
}

static void meson_t7_drm_remove(struct platform_device *pdev)
{
	struct meson_t7_drm *priv = platform_get_drvdata(pdev);
	struct drm_device *drm;

	if (!priv)
		return;

	drm = priv->drm;
	drm_dev_unregister(drm);
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);
	drm->dev_private = NULL;
	platform_set_drvdata(pdev, NULL);
	drm_dev_put(drm);
}

static void meson_t7_drm_shutdown(struct platform_device *pdev)
{
	struct meson_t7_drm *priv = platform_get_drvdata(pdev);

	if (priv && priv->drm)
		drm_atomic_helper_shutdown(priv->drm);
}

static int __maybe_unused meson_t7_drm_pm_suspend(struct device *dev)
{
	struct meson_t7_drm *priv = dev_get_drvdata(dev);

	if (!priv || !priv->drm)
		return 0;

	return drm_mode_config_helper_suspend(priv->drm);
}

static int __maybe_unused meson_t7_drm_pm_resume(struct device *dev)
{
	struct meson_t7_drm *priv = dev_get_drvdata(dev);

	if (!priv || !priv->drm)
		return 0;

	return drm_mode_config_helper_resume(priv->drm);
}

static const struct dev_pm_ops meson_t7_drm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(meson_t7_drm_pm_suspend, meson_t7_drm_pm_resume)
};

static const struct of_device_id meson_t7_drm_dt_match[] = {
	{ .compatible = "amlogic,t7-drm-scaffold" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, meson_t7_drm_dt_match);

static struct platform_driver meson_t7_drm_platform_driver = {
	.probe = meson_t7_drm_probe,
	.remove = meson_t7_drm_remove,
	.shutdown = meson_t7_drm_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = meson_t7_drm_dt_match,
		.pm = &meson_t7_drm_pm_ops,
	},
};

module_platform_driver(meson_t7_drm_platform_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
