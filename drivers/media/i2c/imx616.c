// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX616 CMOS image sensor driver (Samsung Galaxy A52 4G front camera).
 *
 * Register tables decoded from the vendor CamX sensor module
 * (com.samsung.sensormodule.1_0_sony_imx616.bin), see imx616_modes.h.
 * Structure modelled on the s5k3l6xx driver that works on this SoC (SM7125).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX616_REG_CHIP_ID	CCI_REG16(0x0016)
#define IMX616_CHIP_ID		0x0616
#define IMX616_REG_MODE_SELECT	CCI_REG8(0x0100)
#define IMX616_REG_EXPOSURE	CCI_REG16(0x0202)	/* coarse integration time */
#define IMX616_REG_ANALOG_GAIN	CCI_REG16(0x0204)
#define IMX616_REG_FLL		CCI_REG16(0x0340)	/* frame length lines */
#define IMX616_REG_LLP		CCI_REG16(0x0342)	/* line length pck */
#define IMX616_REG_TEST_PATTERN	CCI_REG16(0x0600)
#define IMX616_REG_ORIENTATION	CCI_REG8(0x0101)	/* bit0 h-mirror, bit1 v-flip */

/*
 * Limits from the vendor sensor library (com.samsung.sensor.imx616.so):
 * analogue gain = 1024 / (1024 - reg), reg 112 (1.12x) .. 960 (16x);
 * coarse integration time >= 16 lines and <= frame_length_lines - 48.
 */
#define IMX616_AGAIN_MIN	112
#define IMX616_AGAIN_MAX	960
#define IMX616_EXPOSURE_MIN	16
#define IMX616_EXPOSURE_MARGIN	48

/*
 * The vendor OP PLL runs the link at 2056 Mbps/lane, faster than CAMSS
 * receives cleanly on this SoC (scrambled, truncated frames); every mode
 * table halves it (OPPLL_MPY 257 -> 129), which all modes share.
 */
#define IMX616_LINK_FREQ	516000000LL

/* Full pixel array and the 6528x4896 active area both modes are binned from */
#define IMX616_NATIVE_WIDTH	6560
#define IMX616_NATIVE_HEIGHT	4928
#define IMX616_ACTIVE_LEFT	16
#define IMX616_ACTIVE_TOP	16
#define IMX616_ACTIVE_WIDTH	6528
#define IMX616_ACTIVE_HEIGHT	4896

#include "imx616_modes.h"

struct imx616_mode {
	u32 width;
	u32 height;
	u32 llp;		/* line_length_pck */
	u32 fll;		/* frame_length_lines at the mode's max frame rate */
	u32 fll_def;		/* frame_length_lines for 30 fps */
	/* VT pixel rate: 24 MHz / 4 * pll_mpy / vt_pix_clk_div 5 * 4 / vt_sys_clk_div */
	s64 pixel_rate;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct imx616_mode imx616_modes[] = {
	{	/* 2x2 binned, 30 fps */
		.width = 3264, .height = 2448, .llp = 7536, .fll = 2514,
		.fll_def = 2514,
		.pixel_rate = 568800000,
		.regs = imx616_mode_3264x2448_regs,
		.num_regs = ARRAY_SIZE(imx616_mode_3264x2448_regs),
	},
	{	/* 4x4 binned, up to 120 fps */
		.width = 1632, .height = 1224, .llp = 2248, .fll = 1290,
		.fll_def = 5160,
		.pixel_rate = 348000000,
		.regs = imx616_mode_1632x1224_regs,
		.num_regs = ARRAY_SIZE(imx616_mode_1632x1224_regs),
	},
};

/* Bayer order of the readout, indexed by (vflip << 1 | hflip) */
static const u32 imx616_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10,
};

static const char * const imx616_supply_names[] = { "vio", "vana", "vdig" };
#define IMX616_NUM_SUPPLIES ARRAY_SIZE(imx616_supply_names)

static const s64 imx616_link_freq_menu[] = { IMX616_LINK_FREQ };

static const char * const imx616_test_pattern_menu[] = {
	"Disabled", "Solid Color", "100% Color Bars", "Fade to Grey", "PN9",
};

struct imx616 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *clk;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX616_NUM_SUPPLIES];
	u32 mclk_freq;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	struct mutex lock;	/* serialise mode/power/stream state and controls */
	const struct imx616_mode *mode;
	bool streaming;
};

static inline struct imx616 *to_imx616(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx616, sd);
}

static int imx616_power_on(struct device *dev)
{
	struct imx616 *s = to_imx616(dev_get_drvdata(dev));
	int ret;

	ret = regulator_bulk_enable(IMX616_NUM_SUPPLIES, s->supplies);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(s->clk);
	if (ret)
		goto err_reg;
	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(s->reset_gpio, 0);	/* release reset */
	usleep_range(8000, 9000);
	return 0;

err_reg:
	regulator_bulk_disable(IMX616_NUM_SUPPLIES, s->supplies);
	return ret;
}

static int imx616_power_off(struct device *dev)
{
	struct imx616 *s = to_imx616(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	clk_disable_unprepare(s->clk);
	regulator_bulk_disable(IMX616_NUM_SUPPLIES, s->supplies);
	return 0;
}

static int imx616_start(struct imx616 *s)
{
	int ret = 0;

	cci_multi_reg_write(s->regmap, imx616_init, ARRAY_SIZE(imx616_init), &ret);
	cci_multi_reg_write(s->regmap, s->mode->regs, s->mode->num_regs, &ret);
	if (ret)
		return ret;
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	return cci_write(s->regmap, IMX616_REG_MODE_SELECT, 1, NULL);
}

static int imx616_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx616 *s = to_imx616(sd);
	struct i2c_client *c = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&s->lock);
	if (s->streaming == !!enable)
		goto out;

	if (enable) {
		ret = pm_runtime_resume_and_get(&c->dev);
		if (ret < 0)
			goto out;
		ret = imx616_start(s);
		if (ret) {
			pm_runtime_put(&c->dev);
			goto out;
		}
	} else {
		cci_write(s->regmap, IMX616_REG_MODE_SELECT, 0, NULL);
		pm_runtime_put(&c->dev);
	}
	s->streaming = enable;
	/* flips change the bayer order: no changes while streaming */
	__v4l2_ctrl_grab(s->hflip, enable);
	__v4l2_ctrl_grab(s->vflip, enable);
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int imx616_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx616 *s = container_of(ctrl->handler, struct imx616, ctrls);
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		int max = s->mode->height + ctrl->val - IMX616_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(s->exposure, IMX616_EXPOSURE_MIN, max,
					 1, min(s->exposure->default_value, max));
	}

	if (!pm_runtime_get_if_in_use(&c->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s->regmap, IMX616_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s->regmap, IMX616_REG_ANALOG_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s->regmap, IMX616_REG_FLL,
				s->mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_HBLANK:
		ret = cci_write(s->regmap, IMX616_REG_LLP,
				s->mode->width + ctrl->val, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(s->regmap, IMX616_REG_ORIENTATION,
				s->hflip->val | s->vflip->val << 1, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s->regmap, IMX616_REG_TEST_PATTERN, ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_put(&c->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx616_ctrl_ops = {
	.s_ctrl = imx616_set_ctrl,
};

static u32 imx616_code(struct imx616 *s)
{
	return imx616_mbus_codes[s->vflip->val << 1 | s->hflip->val];
}

static void imx616_fill_fmt(struct imx616 *s, const struct imx616_mode *mode,
			    struct v4l2_mbus_framefmt *f)
{
	f->width = mode->width;
	f->height = mode->height;
	f->code = imx616_code(s);
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
	f->ycbcr_enc = V4L2_YCBCR_ENC_601;
	f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	f->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx616_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx616 *s = to_imx616(sd);

	mutex_lock(&s->lock);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);
		fmt->format.code = imx616_code(s);
	} else {
		imx616_fill_fmt(s, s->mode, &fmt->format);
	}
	mutex_unlock(&s->lock);
	return 0;
}

/* Called with s->lock held: switch the active mode and its timing controls */
static int imx616_apply_mode(struct imx616 *s, const struct imx616_mode *mode)
{
	int vblank_min = mode->fll - mode->height;
	int vblank = mode->fll_def - mode->height;
	int hblank = mode->llp - mode->width;
	int ret;

	s->mode = mode;
	ret = __v4l2_ctrl_s_ctrl_int64(s->pixel_rate, mode->pixel_rate);
	if (!ret)
		ret = __v4l2_ctrl_modify_range(s->hblank, hblank, hblank, 1, hblank);
	if (!ret)
		ret = __v4l2_ctrl_modify_range(s->vblank, vblank_min,
					       0xffff - mode->height, 1, vblank);
	if (!ret)
		ret = __v4l2_ctrl_s_ctrl(s->vblank, vblank);
	if (!ret)
		ret = __v4l2_ctrl_modify_range(s->exposure, IMX616_EXPOSURE_MIN,
					       mode->fll_def - IMX616_EXPOSURE_MARGIN,
					       1, mode->fll - IMX616_EXPOSURE_MARGIN);
	return ret;
}

static int imx616_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx616 *s = to_imx616(sd);
	const struct imx616_mode *mode;
	int ret = 0;

	mode = v4l2_find_nearest_size(imx616_modes, ARRAY_SIZE(imx616_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);

	mutex_lock(&s->lock);
	imx616_fill_fmt(s, mode, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;
	} else if (s->mode != mode) {
		if (s->streaming)
			ret = -EBUSY;
		else
			ret = imx616_apply_mode(s, mode);
	}
	mutex_unlock(&s->lock);
	return ret;
}

static int imx616_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx616 *s = to_imx616(sd);

	if (code->index)
		return -EINVAL;
	mutex_lock(&s->lock);
	code->code = imx616_code(s);
	mutex_unlock(&s->lock);
	return 0;
}

static int imx616_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx616 *s = to_imx616(sd);
	u32 code;

	mutex_lock(&s->lock);
	code = imx616_code(s);
	mutex_unlock(&s->lock);

	if (fse->index >= ARRAY_SIZE(imx616_modes) || fse->code != code)
		return -EINVAL;
	fse->min_width = fse->max_width = imx616_modes[fse->index].width;
	fse->min_height = fse->max_height = imx616_modes[fse->index].height;
	return 0;
}

static int imx616_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect){ 0, 0, IMX616_NATIVE_WIDTH,
					     IMX616_NATIVE_HEIGHT };
		return 0;
	case V4L2_SEL_TGT_CROP:		/* both modes bin the whole active area */
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = (struct v4l2_rect){ IMX616_ACTIVE_LEFT, IMX616_ACTIVE_TOP,
					     IMX616_ACTIVE_WIDTH,
					     IMX616_ACTIVE_HEIGHT };
		return 0;
	}
	return -EINVAL;
}

static int imx616_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx616 *s = to_imx616(sd);

	imx616_fill_fmt(s, &imx616_modes[0], v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops imx616_video_ops = {
	.s_stream = imx616_s_stream,
};

static const struct v4l2_subdev_pad_ops imx616_pad_ops = {
	.enum_mbus_code = imx616_enum_mbus_code,
	.enum_frame_size = imx616_enum_frame_size,
	.get_fmt = imx616_get_fmt,
	.set_fmt = imx616_set_fmt,
	.get_selection = imx616_get_selection,
};

static const struct v4l2_subdev_ops imx616_subdev_ops = {
	.video = &imx616_video_ops,
	.pad = &imx616_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx616_internal_ops = {
	.init_state = imx616_init_state,
};

static int imx616_init_ctrls(struct imx616 *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	const struct imx616_mode *mode = s->mode;

	v4l2_ctrl_handler_init(h, 12);
	h->lock = &s->lock;

	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      imx616_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->pixel_rate = v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE,
					  imx616_modes[1].pixel_rate,
					  imx616_modes[0].pixel_rate, 1,
					  mode->pixel_rate);
	if (s->pixel_rate)
		s->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->exposure = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_EXPOSURE,
					IMX616_EXPOSURE_MIN,
					mode->fll_def - IMX616_EXPOSURE_MARGIN,
					1, mode->fll - IMX616_EXPOSURE_MARGIN);
	v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX616_AGAIN_MIN, IMX616_AGAIN_MAX, 1, IMX616_AGAIN_MIN);
	s->vblank = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_VBLANK,
				      mode->fll - mode->height,
				      0xffff - mode->height, 1,
				      mode->fll_def - mode->height);
	s->hblank = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_HBLANK,
				      mode->llp - mode->width,
				      mode->llp - mode->width, 1,
				      mode->llp - mode->width);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->hflip = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s->hflip)
		s->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s->vflip = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s->vflip)
		s->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	v4l2_ctrl_new_std_menu_items(h, &imx616_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx616_test_pattern_menu) - 1,
				     0, 0, imx616_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(&c->dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(h, &imx616_ctrl_ops, &props);

	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	s->sd.ctrl_handler = h;
	return ret;
}

static int imx616_parse_dt(struct imx616 *s, struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *fh;
	int ret;

	s->clk = devm_clk_get(dev, "mclk");
	if (IS_ERR(s->clk))
		return dev_err_probe(dev, PTR_ERR(s->clk), "no mclk\n");
	s->mclk_freq = clk_get_rate(s->clk);

	s->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s->reset_gpio), "no reset gpio\n");

	fh = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!fh)
		return dev_err_probe(dev, -EINVAL, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_parse(fh, &ep);
	fwnode_handle_put(fh);
	if (ret)
		return dev_err_probe(dev, ret, "endpoint parse failed\n");
	if (ep.bus.mipi_csi2.num_data_lanes != 4)
		return dev_err_probe(dev, -EINVAL, "4 data lanes required\n");
	return 0;
}

static int imx616_probe(struct i2c_client *c)
{
	struct device *dev = &c->dev;
	struct imx616 *s;
	u64 id;
	unsigned int i;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	s->mode = &imx616_modes[0];
	v4l2_i2c_subdev_init(&s->sd, c, &imx616_subdev_ops);
	s->sd.internal_ops = &imx616_internal_ops;

	s->regmap = devm_cci_regmap_init_i2c(c, 16);
	if (IS_ERR(s->regmap))
		return dev_err_probe(dev, PTR_ERR(s->regmap), "regmap init\n");

	ret = imx616_parse_dt(s, dev);
	if (ret)
		return ret;

	for (i = 0; i < IMX616_NUM_SUPPLIES; i++)
		s->supplies[i].supply = imx616_supply_names[i];
	ret = devm_regulator_bulk_get(dev, IMX616_NUM_SUPPLIES, s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "regulators\n");

	ret = imx616_power_on(dev);
	if (ret)
		return ret;

	cci_read(s->regmap, IMX616_REG_CHIP_ID, &id, &ret);
	if (ret || id != IMX616_CHIP_ID) {
		dev_err(dev, "chip id 0x%04llx (expected 0x%04x) ret %d\n",
			id, IMX616_CHIP_ID, ret);
		ret = ret ? ret : -ENODEV;
		goto err_power;
	}
	dev_info(dev, "Sony IMX616 detected, chip id 0x%04llx\n", id);

	ret = imx616_init_ctrls(s);
	if (ret)
		goto err_power;

	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto err_ctrl;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&s->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&s->ctrls);
err_power:
	imx616_power_off(dev);
	return ret;
}

static void imx616_remove(struct i2c_client *c)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(c);
	struct imx616 *s = to_imx616(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&c->dev);
	if (!pm_runtime_status_suspended(&c->dev))
		imx616_power_off(&c->dev);
	pm_runtime_set_suspended(&c->dev);
}

static const struct dev_pm_ops imx616_pm_ops = {
	SET_RUNTIME_PM_OPS(imx616_power_off, imx616_power_on, NULL)
};

static const struct of_device_id imx616_of_match[] = {
	{ .compatible = "sony,imx616" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx616_of_match);

static struct i2c_driver imx616_i2c_driver = {
	.driver = {
		.name = "imx616",
		.of_match_table = imx616_of_match,
		.pm = &imx616_pm_ops,
	},
	.probe = imx616_probe,
	.remove = imx616_remove,
};
module_i2c_driver(imx616_i2c_driver);

MODULE_DESCRIPTION("Sony IMX616 sensor driver");
MODULE_AUTHOR("Miroslav Mráz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
