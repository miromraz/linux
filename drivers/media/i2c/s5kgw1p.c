// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5KGW1P CMOS image sensor driver (Galaxy A52 4G main camera).
 *
 * Register tables decoded from the vendor CamX sensor module
 * (com.samsung.sensormodule.0_1_lsi_s5kgw1p.bin), see s5kgw1p_modes.h. The init
 * list uses indirect page writes and is replayed exactly in order. Structure
 * modelled on the s5k3l6xx driver that works on this SoC (SM7125).
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

#define S5KGW1P_REG_CHIP_ID	0x0000
#define S5KGW1P_CHIP_ID		0xf971
#define S5KGW1P_REG_MODE_SELECT	0x0100
#define S5KGW1P_REG_EXPOSURE	0x0202	/* 16-bit coarse integration time */
#define S5KGW1P_REG_ANALOG_GAIN	0x0204	/* 16-bit */
#define S5KGW1P_REG_FLL		0x0340	/* frame length lines, 16-bit */
#define S5KGW1P_REG_LLP		0x0342	/* line length pck, 16-bit */
#define S5KGW1P_REG_TEST_PATTERN 0x0600	/* 16-bit */

/* mode[1]: 4624x3468 @30fps, 3-lane RAW10, op_pixel_clock 600 MHz. */
#define S5KGW1P_WIDTH		4624
#define S5KGW1P_HEIGHT		3468
#define S5KGW1P_LLP_DEF		13672
#define S5KGW1P_FLL_DEF		3512
#define S5KGW1P_EXPOSURE_DEF	3000
#define S5KGW1P_VBLANK_MIN	(S5KGW1P_FLL_DEF - S5KGW1P_HEIGHT)
#define S5KGW1P_LINK_FREQ	1000000000LL
/* pixel_rate = link_freq * 2 (DDR) * lanes / bits_per_sample */
#define S5KGW1P_PIXEL_RATE	600000000LL

struct s5kgw1p_reg {
	u16 addr;
	u16 val;
	u8 len;
};

#include "s5kgw1p_modes.h"

static const char * const s5kgw1p_supply_names[] = {
	"vio", "vana", "vdig", "vaf", "vcustom",
};
#define S5KGW1P_NUM_SUPPLIES ARRAY_SIZE(s5kgw1p_supply_names)

static const s64 s5kgw1p_link_freq_menu[] = { S5KGW1P_LINK_FREQ };

static const char * const s5kgw1p_test_pattern_menu[] = {
	"Disabled", "Solid Color", "100% Color Bars", "Fade to Grey", "PN9",
};

struct s5kgw1p {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5KGW1P_NUM_SUPPLIES];
	u32 mclk_freq;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	struct mutex lock;	/* serialise power/stream state */
	bool streaming;
};

static inline struct s5kgw1p *to_s5kgw1p(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kgw1p, sd);
}

static int s5kgw1p_write(struct s5kgw1p *s, u16 addr, u16 val, u8 len)
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

static int s5kgw1p_read(struct s5kgw1p *s, u16 addr, u16 *val, u8 len)
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

static int s5kgw1p_write_regs(struct s5kgw1p *s, const struct s5kgw1p_reg *r,
			      unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = s5kgw1p_write(s, r[i].addr, r[i].val, r[i].len);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5kgw1p_power_on(struct device *dev)
{
	struct s5kgw1p *s = to_s5kgw1p(dev_get_drvdata(dev));
	int ret;

	ret = regulator_bulk_enable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(s->clk);
	if (ret)
		goto err_reg;
	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(s->reset_gpio, 0);	/* release reset */
	usleep_range(10000, 11000);
	return 0;

err_reg:
	regulator_bulk_disable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	return ret;
}

static int s5kgw1p_power_off(struct device *dev)
{
	struct s5kgw1p *s = to_s5kgw1p(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	clk_disable_unprepare(s->clk);
	regulator_bulk_disable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	return 0;
}

/* vendor masterSettings: free-running, not waiting for an external sync */
static const struct s5kgw1p_reg s5kgw1p_master[] = {
	{0x3f0b, 0x00, 1}, {0x3041, 0x00, 1}, {0x3040, 0x00, 1}, {0x4b81, 0x00, 1},
};

static int s5kgw1p_start(struct s5kgw1p *s)
{
	int ret;

	/* init[0..1] = page 0x4000 + software reset (0x6010); let the sensor reboot */
	ret = s5kgw1p_write_regs(s, s5kgw1p_init, 2);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	ret = s5kgw1p_write_regs(s, s5kgw1p_init + 2, ARRAY_SIZE(s5kgw1p_init) - 2);
	if (ret)
		return ret;
	ret = s5kgw1p_write_regs(s, s5kgw1p_master, ARRAY_SIZE(s5kgw1p_master));
	if (ret)
		return ret;
	ret = s5kgw1p_write_regs(s, s5kgw1p_mode_regs,
				 ARRAY_SIZE(s5kgw1p_mode_regs));
	if (ret)
		return ret;
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	/* Vendor stream-on writes the 16-bit value 0x0100 to mode_select. */
	return s5kgw1p_write(s, S5KGW1P_REG_MODE_SELECT, 0x0100, 2);
}

static int s5kgw1p_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5kgw1p *s = to_s5kgw1p(sd);
	struct i2c_client *c = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&s->lock);
	if (s->streaming == !!enable)
		goto out;

	if (enable) {
		ret = pm_runtime_resume_and_get(&c->dev);
		if (ret < 0)
			goto out;
		ret = s5kgw1p_start(s);
		if (ret) {
			pm_runtime_put(&c->dev);
			goto out;
		}
	} else {
		s5kgw1p_write(s, S5KGW1P_REG_MODE_SELECT, 0, 1);
		pm_runtime_put(&c->dev);
	}
	s->streaming = enable;
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int s5kgw1p_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kgw1p *s = container_of(ctrl->handler, struct s5kgw1p, ctrls);
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(s->exposure, 2,
					 S5KGW1P_HEIGHT + ctrl->val - 8, 1,
					 S5KGW1P_EXPOSURE_DEF);

	if (!pm_runtime_get_if_in_use(&c->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = s5kgw1p_write(s, S5KGW1P_REG_EXPOSURE, ctrl->val, 2);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = s5kgw1p_write(s, S5KGW1P_REG_ANALOG_GAIN, ctrl->val, 2);
		break;
	case V4L2_CID_VBLANK:
		ret = s5kgw1p_write(s, S5KGW1P_REG_FLL, S5KGW1P_HEIGHT + ctrl->val, 2);
		break;
	case V4L2_CID_HBLANK:
		ret = s5kgw1p_write(s, S5KGW1P_REG_LLP, S5KGW1P_WIDTH + ctrl->val, 2);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = s5kgw1p_write(s, S5KGW1P_REG_TEST_PATTERN, ctrl->val, 2);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_put(&c->dev);
	return ret;
}

static const struct v4l2_ctrl_ops s5kgw1p_ctrl_ops = {
	.s_ctrl = s5kgw1p_set_ctrl,
};

static void s5kgw1p_fill_fmt(struct v4l2_mbus_framefmt *f)
{
	f->width = S5KGW1P_WIDTH;
	f->height = S5KGW1P_HEIGHT;
	f->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
}

static int s5kgw1p_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	s5kgw1p_fill_fmt(&fmt->format);
	return 0;
}

static int s5kgw1p_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5kgw1p_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;
	fse->min_width = fse->max_width = S5KGW1P_WIDTH;
	fse->min_height = fse->max_height = S5KGW1P_HEIGHT;
	return 0;
}

static int s5kgw1p_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	s5kgw1p_fill_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops s5kgw1p_video_ops = {
	.s_stream = s5kgw1p_s_stream,
};

static const struct v4l2_subdev_pad_ops s5kgw1p_pad_ops = {
	.enum_mbus_code = s5kgw1p_enum_mbus_code,
	.enum_frame_size = s5kgw1p_enum_frame_size,
	.get_fmt = s5kgw1p_get_fmt,
	.set_fmt = s5kgw1p_get_fmt,	/* single fixed mode */
};

static const struct v4l2_subdev_ops s5kgw1p_subdev_ops = {
	.video = &s5kgw1p_video_ops,
	.pad = &s5kgw1p_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5kgw1p_internal_ops = {
	.init_state = s5kgw1p_init_state,
};

static int s5kgw1p_init_ctrls(struct s5kgw1p *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(h, 10);

	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      s5kgw1p_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE,
				 S5KGW1P_PIXEL_RATE, S5KGW1P_PIXEL_RATE, 1,
				 S5KGW1P_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->exposure = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_EXPOSURE,
					2, S5KGW1P_FLL_DEF - 8, 1,
					S5KGW1P_EXPOSURE_DEF);
	v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0, 4095, 1, 256);
	s->vblank = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_VBLANK,
				      S5KGW1P_VBLANK_MIN, 0xffff - S5KGW1P_HEIGHT,
				      1, S5KGW1P_VBLANK_MIN);
	s->hblank = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_HBLANK,
				      S5KGW1P_LLP_DEF - S5KGW1P_WIDTH,
				      S5KGW1P_LLP_DEF - S5KGW1P_WIDTH, 1,
				      S5KGW1P_LLP_DEF - S5KGW1P_WIDTH);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std_menu_items(h, &s5kgw1p_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5kgw1p_test_pattern_menu) - 1,
				     0, 0, s5kgw1p_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(&c->dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(h, &s5kgw1p_ctrl_ops, &props);

	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	s->sd.ctrl_handler = h;
	return ret;
}

static int s5kgw1p_parse_dt(struct s5kgw1p *s, struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_CPHY };
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
	if (ep.bus.mipi_csi2.num_data_lanes != 3)
		return dev_err_probe(dev, -EINVAL, "3 data lanes required\n");
	return 0;
}

static int s5kgw1p_probe(struct i2c_client *c)
{
	struct device *dev = &c->dev;
	struct s5kgw1p *s;
	u16 id;
	unsigned int i;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	v4l2_i2c_subdev_init(&s->sd, c, &s5kgw1p_subdev_ops);
	s->sd.internal_ops = &s5kgw1p_internal_ops;

	ret = s5kgw1p_parse_dt(s, dev);
	if (ret)
		return ret;

	for (i = 0; i < S5KGW1P_NUM_SUPPLIES; i++)
		s->supplies[i].supply = s5kgw1p_supply_names[i];
	ret = devm_regulator_bulk_get(dev, S5KGW1P_NUM_SUPPLIES, s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "regulators\n");

	ret = s5kgw1p_power_on(dev);
	if (ret)
		return ret;

	ret = s5kgw1p_read(s, S5KGW1P_REG_CHIP_ID, &id, 2);
	if (ret || id != S5KGW1P_CHIP_ID) {
		dev_err(dev, "chip id 0x%04x (expected 0x%04x) ret %d\n",
			id, S5KGW1P_CHIP_ID, ret);
		ret = ret ? ret : -ENODEV;
		goto err_power;
	}
	dev_info(dev, "Samsung S5KGW1P detected, chip id 0x%04x\n", id);

	ret = s5kgw1p_init_ctrls(s);
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
	s5kgw1p_power_off(dev);
	return ret;
}

static void s5kgw1p_remove(struct i2c_client *c)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(c);
	struct s5kgw1p *s = to_s5kgw1p(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&c->dev);
	if (!pm_runtime_status_suspended(&c->dev))
		s5kgw1p_power_off(&c->dev);
	pm_runtime_set_suspended(&c->dev);
}

static const struct dev_pm_ops s5kgw1p_pm_ops = {
	SET_RUNTIME_PM_OPS(s5kgw1p_power_off, s5kgw1p_power_on, NULL)
};

static const struct of_device_id s5kgw1p_of_match[] = {
	{ .compatible = "samsung,s5kgw1p" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5kgw1p_of_match);

static struct i2c_driver s5kgw1p_i2c_driver = {
	.driver = {
		.name = "s5kgw1p",
		.of_match_table = s5kgw1p_of_match,
		.pm = &s5kgw1p_pm_ops,
	},
	.probe = s5kgw1p_probe,
	.remove = s5kgw1p_remove,
};
module_i2c_driver(s5kgw1p_i2c_driver);

MODULE_DESCRIPTION("Samsung S5KGW1P sensor driver");
MODULE_AUTHOR("a52q mainline bringup");
MODULE_LICENSE("GPL");
