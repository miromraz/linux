// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 map220v <map220v300@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct panel_info {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct panel_desc *desc;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[2];

	bool enabled;
};

struct panel_desc {
	unsigned int width_mm;
	unsigned int height_mm;

	unsigned int bpc;
	unsigned int lanes;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;

	const struct drm_display_mode *modes;
	unsigned int num_modes;
	const struct mipi_dsi_device_info dsi_info;
	void (*init_sequence)(struct mipi_dsi_multi_context *dsi_ctx);
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, panel);
}

static void ams646yd04_init_sequence(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 30);

	/* Unlock Level 1 Key */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x5a, 0x5a);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf2,
				     0x00, 0x05, 0x0e, 0x58, 0x54, 0x01, 0x0c, 0x00,
				     0xb4, 0x26, 0xe4, 0x2f, 0xb0, 0x0c, 0x09, 0x74,
				     0x26, 0xe4, 0x0c, 0x00, 0x04, 0x10, 0x00, 0x10,
				     0x26, 0xa8, 0x10, 0x00, 0x10, 0x10, 0x34, 0x10,
				     0x00, 0x40, 0x30, 0xc8, 0x00, 0xc8, 0x00, 0x00,
				     0xce);

	mipi_dsi_dcs_set_tear_on_multi(dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	/* Set Column Address to 1079 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37);
	/* Set Page Address to 2399 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x5f);

	/* Partial update limitation */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc2,
				     0x1b, 0x41, 0xb0, 0x0e, 0x00, 0x3c, 0x5a, 0x00,
				     0x00);

	/* Unlock Level 2 Key */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfc, 0x5a, 0x5a);

	/* Default 1711 Mbps (REV D) */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x2a, 0xc5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc5, 0x0d, 0x10, 0x80, 0x45);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x2e, 0xc5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc5, 0x36, 0x41);

	/* Lock Level 2 Key */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xfc, 0xa5, 0xa5);

	/* ERR_FG Setting */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe5, 0x15);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xed, 0x44, 0x4c, 0x20);

	/* PCD Setting */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcc, 0x5c, 0x51);

	/* Frequency setting */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x27, 0xf2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf2, 0x00);

	/* 90 FPS */
	mipi_dsi_usleep_range(dsi_ctx, 17000, 18000);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x01, 0xb4, 0x65);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x65,
					 0x0c, 0x94, 0x0c, 0x5a, 0x0c, 0x30, 0x0b,
					 0xb8, 0x0b, 0x46, 0x0a, 0xf6, 0x09, 0xea,
					 0x09, 0x08, 0x08, 0x1a, 0x07, 0x12, 0x06,
					 0x2a, 0x05, 0x12, 0x03, 0x82, 0x02, 0x08,
					 0x02, 0x08, 0x00, 0x18);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x2a, 0x6a);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x6a, 0x07, 0x00, 0xc0);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_usleep_range(dsi_ctx, 17000, 18000);
	/* Frequency setting 1 */
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x60, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x08, 0xf2);
	/* Frequency setting 2 */
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xf2, 0x04);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x2c, 0x6a);
	/* Frequency setting 3 */
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x6a, 0x80);
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x28, 0x68);
	/* Frequency setting 4 */
	mipi_dsi_generic_write_seq_multi(dsi_ctx, 0x68, 0x22);

	/* Smooth Dimming 4 frame */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x92, 0x63);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x63, 0x04);

	/* Gamma mode 2 Normal (REV A) */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x02, 0x90);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x90, 0x1c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb5, 0x14);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x76, 0x63);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x04, 0x63, 0x00, 0x00, 0x00);

	/* Frequency update */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf7, 0x0f);
	/* Lock Level 1 Key */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_msleep(dsi_ctx, 90);

	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);
}

static const struct drm_display_mode ams646yd04_modes[] = {
	{	/* 90 Hz */
		.clock = (1080 + 80 + 84 + 88) * (2400 + 15 + 2 + 2) * 90 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 80,
		.hsync_end = 1080 + 80 + 84,
		.htotal = 1080 + 80 + 84 + 88,
		.vdisplay = 2400,
		.vsync_start = 2400 + 15,
		.vsync_end = 2400 + 15 + 2,
		.vtotal = 2400 + 15 + 2 + 2,
	},
	{	/* 60 Hz */
		.clock = (1080 + 80 + 84 + 88) * (2400 + 16 + 2 + 2) * 60 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 80,
		.hsync_end = 1080 + 80 + 84,
		.htotal = 1080 + 80 + 84 + 88,
		.vdisplay = 2400,
		.vsync_start = 2400 + 16,
		.vsync_end = 2400 + 16 + 2,
		.vtotal = 2400 + 16 + 2 + 2,
	},
};

static const struct panel_desc ams646yd04_desc = {
	.modes = ams646yd04_modes,
	.num_modes = ARRAY_SIZE(ams646yd04_modes),
	.dsi_info = {
		.type = "s6e3fc3-ams646yd04",
		.channel = 0,
		.node = NULL,
	},
	.width_mm = 67,
	.height_mm = 150,
	.bpc = 8,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM,
	.init_sequence = ams646yd04_init_sequence,
};

static void s6e3fc3_reset(struct panel_info *pinfo)
{
	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	usleep_range(12000, 13000);
	gpiod_set_value_cansleep(pinfo->reset_gpio, 0);
	usleep_range(12000, 13000);
}

static int s6e3fc3_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };

	pinfo->desc->init_sequence(&dsi_ctx);
	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	} else {
		pinfo->enabled = true;
	}

	return dsi_ctx.accum_err;
}

static int s6e3fc3_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);
	if (ret < 0) {
		dev_err(panel->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	s6e3fc3_reset(pinfo);

	return 0;
}

static int s6e3fc3_disable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = pinfo->dsi };

	pinfo->enabled = false;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 70);

	return dsi_ctx.accum_err;
}

static int s6e3fc3_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);

	gpiod_set_value_cansleep(pinfo->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(pinfo->supplies), pinfo->supplies);

	return 0;
}

static int s6e3fc3_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int i;

	for (i = 0; i < pinfo->desc->num_modes; i++) {
		const struct drm_display_mode *m = &pinfo->desc->modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
			return -ENOMEM;
		}

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;
	connector->display_info.bpc = pinfo->desc->bpc;

	return pinfo->desc->num_modes;
}

static const struct drm_panel_funcs s6e3fc3_panel_funcs = {
	.disable = s6e3fc3_disable,
	.enable = s6e3fc3_enable,
	.prepare = s6e3fc3_prepare,
	.unprepare = s6e3fc3_unprepare,
	.get_modes = s6e3fc3_get_modes,
};

static int s6e3fc3_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	if (!pinfo->enabled)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int s6e3fc3_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	u16 brightness;
	int ret;

	if (!pinfo->enabled)
		return 0;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops s6e3fc3_bl_ops = {
	.update_status = s6e3fc3_bl_update_status,
	.get_brightness = s6e3fc3_bl_get_brightness,
};

static struct backlight_device *s6e3fc3_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1023,
		.max_brightness = 1023,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &s6e3fc3_bl_ops, &props);
}

static int s6e3fc3_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct panel_info *pinfo;
	int ret;

	pinfo = devm_drm_panel_alloc(dev, struct panel_info, panel,
				     &s6e3fc3_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(pinfo))
		return PTR_ERR(pinfo);

	pinfo->supplies[0].supply = "vddio";
	pinfo->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(pinfo->supplies),
				      pinfo->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pinfo->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio), "failed to get reset gpio\n");

	pinfo->desc = of_device_get_match_data(dev);
	if (!pinfo->desc)
		return -ENODEV;

	pinfo->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);

	pinfo->panel.prepare_prev_first = true;

	pinfo->panel.backlight = s6e3fc3_create_backlight(dsi);
	if (IS_ERR(pinfo->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(pinfo->panel.backlight),
				     "failed to create backlight\n");

	drm_panel_add(&pinfo->panel);

	dsi->lanes = pinfo->desc->lanes;
	dsi->format = pinfo->desc->format;
	dsi->mode_flags = pinfo->desc->mode_flags;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&pinfo->panel);
		return dev_err_probe(dev, ret, "cannot attach to DSI host\n");
	}

	return 0;
}

static void s6e3fc3_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&pinfo->panel);
}

static const struct of_device_id s6e3fc3_of_match[] = {
	{
		.compatible = "samsung,s6e3fc3-ams646yd04",
		.data = &ams646yd04_desc,
	},
	{}
};
MODULE_DEVICE_TABLE(of, s6e3fc3_of_match);

static struct mipi_dsi_driver s6e3fc3_driver = {
	.probe = s6e3fc3_probe,
	.remove = s6e3fc3_remove,
	.driver = {
		.name = "panel-samsung-s6e3fc3",
		.of_match_table = s6e3fc3_of_match,
	},
};
module_mipi_dsi_driver(s6e3fc3_driver);

MODULE_AUTHOR("map220v <map220v300@gmail.com>");
MODULE_DESCRIPTION("DRM driver for Samsung S6E3FC3 based MIPI DSI panels");
MODULE_LICENSE("GPL");
