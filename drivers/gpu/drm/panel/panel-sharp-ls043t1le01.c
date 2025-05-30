// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Red Hat
 * Copyright (C) 2015 Sony Mobile Communications Inc.
 * Author: Werner Johansson <werner.johansson@sonymobile.com>
 *
 * Based on AUO panel driver by Rob Clark <robdclark@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct sharp_nt_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator *supply;
	struct gpio_desc *reset_gpio;
};

static inline struct sharp_nt_panel *to_sharp_nt_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_nt_panel, base);
}

static int sharp_nt_panel_init(struct sharp_nt_panel *sharp_nt)
{
	struct mipi_dsi_device *dsi = sharp_nt->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);

	mipi_dsi_msleep(&dsi_ctx, 120);

	/* Novatek two-lane operation */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xae,  0x03);

	/* Set both MCU and RGB I/F to 24bpp */
	mipi_dsi_dcs_set_pixel_format_multi(&dsi_ctx,
					    MIPI_DCS_PIXEL_FMT_24BIT |
					    (MIPI_DCS_PIXEL_FMT_24BIT << 4));

	return dsi_ctx.accum_err;
}

static int sharp_nt_panel_on(struct sharp_nt_panel *sharp_nt)
{
	struct mipi_dsi_device *dsi = sharp_nt->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int sharp_nt_panel_off(struct sharp_nt_panel *sharp_nt)
{
	struct mipi_dsi_device *dsi = sharp_nt->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int sharp_nt_panel_unprepare(struct drm_panel *panel)
{
	struct sharp_nt_panel *sharp_nt = to_sharp_nt_panel(panel);
	int ret;

	ret = sharp_nt_panel_off(sharp_nt);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);
		return ret;
	}

	regulator_disable(sharp_nt->supply);
	if (sharp_nt->reset_gpio)
		gpiod_set_value(sharp_nt->reset_gpio, 0);

	return 0;
}

static int sharp_nt_panel_prepare(struct drm_panel *panel)
{
	struct sharp_nt_panel *sharp_nt = to_sharp_nt_panel(panel);
	int ret;

	ret = regulator_enable(sharp_nt->supply);
	if (ret < 0)
		return ret;

	msleep(20);

	if (sharp_nt->reset_gpio) {
		gpiod_set_value(sharp_nt->reset_gpio, 1);
		msleep(1);
		gpiod_set_value(sharp_nt->reset_gpio, 0);
		msleep(1);
		gpiod_set_value(sharp_nt->reset_gpio, 1);
		msleep(10);
	}

	ret = sharp_nt_panel_init(sharp_nt);
	if (ret < 0) {
		dev_err(panel->dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

	ret = sharp_nt_panel_on(sharp_nt);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel on: %d\n", ret);
		goto poweroff;
	}

	return 0;

poweroff:
	regulator_disable(sharp_nt->supply);
	if (sharp_nt->reset_gpio)
		gpiod_set_value(sharp_nt->reset_gpio, 0);
	return ret;
}

static const struct drm_display_mode default_mode = {
	.clock = (540 + 48 + 32 + 80) * (960 + 3 + 10 + 15) * 60 / 1000,
	.hdisplay = 540,
	.hsync_start = 540 + 48,
	.hsync_end = 540 + 48 + 32,
	.htotal = 540 + 48 + 32 + 80,
	.vdisplay = 960,
	.vsync_start = 960 + 3,
	.vsync_end = 960 + 3 + 10,
	.vtotal = 960 + 3 + 10 + 15,
};

static int sharp_nt_panel_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 54;
	connector->display_info.height_mm = 95;

	return 1;
}

static const struct drm_panel_funcs sharp_nt_panel_funcs = {
	.unprepare = sharp_nt_panel_unprepare,
	.prepare = sharp_nt_panel_prepare,
	.get_modes = sharp_nt_panel_get_modes,
};

static int sharp_nt_panel_add(struct sharp_nt_panel *sharp_nt)
{
	struct device *dev = &sharp_nt->dsi->dev;
	int ret;

	sharp_nt->supply = devm_regulator_get(dev, "avdd");
	if (IS_ERR(sharp_nt->supply))
		return PTR_ERR(sharp_nt->supply);

	sharp_nt->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sharp_nt->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(sharp_nt->reset_gpio));
		sharp_nt->reset_gpio = NULL;
	} else {
		gpiod_set_value(sharp_nt->reset_gpio, 0);
	}

	drm_panel_init(&sharp_nt->base, &sharp_nt->dsi->dev,
		       &sharp_nt_panel_funcs, DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&sharp_nt->base);
	if (ret)
		return ret;

	drm_panel_add(&sharp_nt->base);

	return 0;
}

static void sharp_nt_panel_del(struct sharp_nt_panel *sharp_nt)
{
	if (sharp_nt->base.dev)
		drm_panel_remove(&sharp_nt->base);
}

static int sharp_nt_panel_probe(struct mipi_dsi_device *dsi)
{
	struct sharp_nt_panel *sharp_nt;
	int ret;

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			MIPI_DSI_MODE_VIDEO_HSE |
			MIPI_DSI_CLOCK_NON_CONTINUOUS |
			MIPI_DSI_MODE_NO_EOT_PACKET;

	sharp_nt = devm_kzalloc(&dsi->dev, sizeof(*sharp_nt), GFP_KERNEL);
	if (!sharp_nt)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, sharp_nt);

	sharp_nt->dsi = dsi;

	ret = sharp_nt_panel_add(sharp_nt);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		sharp_nt_panel_del(sharp_nt);
		return ret;
	}

	return 0;
}

static void sharp_nt_panel_remove(struct mipi_dsi_device *dsi)
{
	struct sharp_nt_panel *sharp_nt = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	sharp_nt_panel_del(sharp_nt);
}

static const struct of_device_id sharp_nt_of_match[] = {
	{ .compatible = "sharp,ls043t1le01-qhd", },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_nt_of_match);

static struct mipi_dsi_driver sharp_nt_panel_driver = {
	.driver = {
		.name = "panel-sharp-ls043t1le01-qhd",
		.of_match_table = sharp_nt_of_match,
	},
	.probe = sharp_nt_panel_probe,
	.remove = sharp_nt_panel_remove,
};
module_mipi_dsi_driver(sharp_nt_panel_driver);

MODULE_AUTHOR("Werner Johansson <werner.johansson@sonymobile.com>");
MODULE_DESCRIPTION("Sharp LS043T1LE01 NT35565-based qHD (540x960) video mode panel driver");
MODULE_LICENSE("GPL v2");
