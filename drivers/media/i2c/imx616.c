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

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX616_REG_CHIP_ID	0x0016
#define IMX616_CHIP_ID		0x0616
#define IMX616_REG_MODE_SELECT	0x0100
#define IMX616_REG_EXPOSURE	0x0202	/* 16-bit coarse integration time */
#define IMX616_REG_ANALOG_GAIN	0x0204	/* 16-bit */
#define IMX616_REG_FLL		0x0340	/* frame length lines, 16-bit */
#define IMX616_REG_LLP		0x0342	/* line length pck, 16-bit */
#define IMX616_REG_TEST_PATTERN	0x0600	/* 16-bit */

/* mode[2]: 3264x2448 @30fps, 4-lane RAW10, op_pixel_clock 822.4 MHz. */
#define IMX616_WIDTH		3264
#define IMX616_HEIGHT		2448
#define IMX616_LLP_DEF		7536
#define IMX616_FLL_DEF		2514
#define IMX616_EXPOSURE_DEF	2466
#define IMX616_VBLANK_MIN	(IMX616_FLL_DEF - IMX616_HEIGHT)
#define IMX616_LINK_FREQ	1028000000LL
/* pixel_rate = link_freq * 2 (DDR) * lanes / bits_per_sample */
#define IMX616_PIXEL_RATE	822400000LL

struct imx616_reg {
	u16 addr;
	u16 val;
	u8 len;
};

#include "imx616_modes.h"

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
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX616_NUM_SUPPLIES];
	u32 mclk_freq;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	struct mutex lock;	/* serialise power/stream state */
	bool streaming;
};

static inline struct imx616 *to_imx616(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx616, sd);
}

static int imx616_write(struct imx616 *s, u16 addr, u16 val, u8 len)
{
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	u8 buf[4] = { addr >> 8, addr & 0xff };
	int n = 2;

	if (len == 2)
		buf[n++] = val >> 8;
	buf[n++] = val & 0xff;

	if (i2c_master_send(c, buf, n) != n) {
		dev_err(&c->dev, "write 0x%04x failed\n", addr);
		return -EIO;
	}
	return 0;
}

static int imx616_read(struct imx616 *s, u16 addr, u16 *val, u8 len)
{
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	u8 wb[2] = { addr >> 8, addr & 0xff };
	u8 rb[2] = { 0 };
	struct i2c_msg msg[2] = {
		{ .addr = c->addr, .flags = 0, .len = 2, .buf = wb },
		{ .addr = c->addr, .flags = I2C_M_RD, .len = len, .buf = rb },
	};

	if (i2c_transfer(c->adapter, msg, 2) != 2)
		return -EIO;
	*val = (len == 2) ? (rb[0] << 8) | rb[1] : rb[0];
	return 0;
}

static int imx616_write_regs(struct imx616 *s, const struct imx616_reg *r,
			     unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = imx616_write(s, r[i].addr, r[i].val, r[i].len);
		if (ret)
			return ret;
	}
	return 0;
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
	int ret;

	ret = imx616_write_regs(s, imx616_init, ARRAY_SIZE(imx616_init));
	if (ret)
		return ret;
	ret = imx616_write_regs(s, imx616_mode_regs, ARRAY_SIZE(imx616_mode_regs));
	if (ret)
		return ret;
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	return imx616_write(s, IMX616_REG_MODE_SELECT, 1, 1);
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
		imx616_write(s, IMX616_REG_MODE_SELECT, 0, 1);
		pm_runtime_put(&c->dev);
	}
	s->streaming = enable;
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int imx616_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx616 *s = container_of(ctrl->handler, struct imx616, ctrls);
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(s->exposure, 2,
					 IMX616_HEIGHT + ctrl->val - 8, 1,
					 IMX616_EXPOSURE_DEF);

	if (!pm_runtime_get_if_in_use(&c->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = imx616_write(s, IMX616_REG_EXPOSURE, ctrl->val, 2);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx616_write(s, IMX616_REG_ANALOG_GAIN, ctrl->val, 2);
		break;
	case V4L2_CID_VBLANK:
		ret = imx616_write(s, IMX616_REG_FLL, IMX616_HEIGHT + ctrl->val, 2);
		break;
	case V4L2_CID_HBLANK:
		ret = imx616_write(s, IMX616_REG_LLP, IMX616_WIDTH + ctrl->val, 2);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx616_write(s, IMX616_REG_TEST_PATTERN, ctrl->val, 2);
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

static void imx616_fill_fmt(struct v4l2_mbus_framefmt *f)
{
	f->width = IMX616_WIDTH;
	f->height = IMX616_HEIGHT;
	f->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
}

static int imx616_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	imx616_fill_fmt(&fmt->format);
	return 0;
}

static int imx616_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int imx616_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = IMX616_WIDTH;
	fse->min_height = fse->max_height = IMX616_HEIGHT;
	return 0;
}

static int imx616_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	imx616_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops imx616_video_ops = {
	.s_stream = imx616_s_stream,
};

static const struct v4l2_subdev_pad_ops imx616_pad_ops = {
	.enum_mbus_code = imx616_enum_mbus_code,
	.enum_frame_size = imx616_enum_frame_size,
	.get_fmt = imx616_get_fmt,
	.set_fmt = imx616_get_fmt,	/* single fixed mode */
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

	v4l2_ctrl_handler_init(h, 10);

	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      imx616_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE,
				 IMX616_PIXEL_RATE, IMX616_PIXEL_RATE, 1,
				 IMX616_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->exposure = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_EXPOSURE,
					2, IMX616_FLL_DEF - 8, 1, IMX616_EXPOSURE_DEF);
	v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0, 1023, 1, 112);
	s->vblank = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_VBLANK,
				      IMX616_VBLANK_MIN, 0xffff - IMX616_HEIGHT,
				      1, IMX616_VBLANK_MIN);
	s->hblank = v4l2_ctrl_new_std(h, &imx616_ctrl_ops, V4L2_CID_HBLANK,
				      IMX616_LLP_DEF - IMX616_WIDTH,
				      IMX616_LLP_DEF - IMX616_WIDTH, 1,
				      IMX616_LLP_DEF - IMX616_WIDTH);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

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
	u16 id;
	unsigned int i;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	v4l2_i2c_subdev_init(&s->sd, c, &imx616_subdev_ops);
	s->sd.internal_ops = &imx616_internal_ops;

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

	ret = imx616_read(s, IMX616_REG_CHIP_ID, &id, 2);
	if (ret || id != IMX616_CHIP_ID) {
		dev_err(dev, "chip id 0x%04x (expected 0x%04x) ret %d\n",
			id, IMX616_CHIP_ID, ret);
		ret = ret ? ret : -ENODEV;
		goto err_power;
	}
	dev_info(dev, "Sony IMX616 detected, chip id 0x%04x\n", id);

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
MODULE_AUTHOR("a52q mainline bringup");
MODULE_LICENSE("GPL");
