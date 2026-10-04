// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Awinic AW9120 20-channel I2C LED controller.
 *
 * Copyright (C) 2026 Miroslav Mraz <miroslav.mraz@techmania.cz>
 *
 * The chip exposes 20 constant-current LED outputs behind an 8-bit register
 * address / 16-bit big-endian data I2C interface. Each output is controlled
 * independently: the per-channel drive current lives in the IMAX registers and
 * the per-channel PWM duty is written through the command register (CMDR) with
 * a single "set PWM" instruction.
 *
 * Outputs wired together as an RGB group are exposed through the multicolor LED
 * class (one output per colour component); remaining outputs are exposed as
 * plain single-colour LED class devices. Only the outputs described in the
 * device tree are enabled.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define AW9120_MAX_LEDS		20

/* Reset / chip-id register; reads back the chip id. */
#define AW9120_REG_RSTR		0x00
#define AW9120_CHIP_ID		0xb223

/* Global control register. */
#define AW9120_REG_GCR		0x01
#define AW9120_GCR_ENABLE	BIT(0)

/* LED enable registers: LER1 outputs 0-11, LER2 outputs 12-19. */
#define AW9120_REG_LER1		0x50
#define AW9120_REG_LER2		0x51

/* Control-source select: 1 = I2C/command register, 0 = internal SRAM. */
#define AW9120_REG_CTRS1	0x55
#define AW9120_REG_CTRS2	0x56

/* Per-channel max-current registers: five registers, four 4-bit nibbles each. */
#define AW9120_REG_IMAX(n)	(0x57 + (n))
#define AW9120_IMAX_DEFAULT	0x1111

/* Command register: executes one instruction immediately. */
#define AW9120_REG_CMDR		0x61
#define AW9120_CMD_SETPWM	0xa000
#define AW9120_CMD_CHAN(x)	((x) << 8)

#define AW9120_REG_MAX		0x7f
#define AW9120_MAX_BRIGHTNESS	0xff

struct aw9120;

struct aw9120_led {
	struct aw9120 *chip;
	union {
		struct led_classdev cdev;
		struct led_classdev_mc mcdev;
	};
	u32 channel;
};

struct aw9120 {
	struct regmap *regmap;
	struct device *dev;
	u32 used_mask;
	unsigned long num_leds;
	struct aw9120_led leds[] __counted_by(num_leds);
};

static int aw9120_set_channel(struct aw9120 *chip, u32 channel,
			      enum led_brightness brightness)
{
	return regmap_write(chip->regmap, AW9120_REG_CMDR,
			    AW9120_CMD_SETPWM | AW9120_CMD_CHAN(channel) |
			    brightness);
}

static int aw9120_brightness_single_set(struct led_classdev *cdev,
					enum led_brightness value)
{
	struct aw9120_led *led = container_of(cdev, struct aw9120_led, cdev);

	return aw9120_set_channel(led->chip, led->channel, value);
}

static int aw9120_brightness_mc_set(struct led_classdev *cdev,
				    enum led_brightness value)
{
	struct led_classdev_mc *mc = lcdev_to_mccdev(cdev);
	struct aw9120_led *led = container_of(mc, struct aw9120_led, mcdev);
	int i, ret;

	led_mc_calc_color_components(mc, value);

	for (i = 0; i < mc->num_colors; i++) {
		ret = aw9120_set_channel(led->chip, mc->subled_info[i].channel,
					 mc->subled_info[i].brightness);
		if (ret)
			return ret;
	}

	return 0;
}

static int aw9120_claim_channel(struct aw9120 *chip, u32 channel)
{
	if (channel >= AW9120_MAX_LEDS)
		return dev_err_probe(chip->dev, -EINVAL,
				     "LED output %u out of range\n", channel);

	if (chip->used_mask & BIT(channel))
		return dev_err_probe(chip->dev, -EINVAL,
				     "duplicate LED output %u\n", channel);

	chip->used_mask |= BIT(channel);

	return 0;
}

static int aw9120_setup_rgb(struct aw9120 *chip, struct fwnode_handle *fwnode,
			    struct aw9120_led *led,
			    struct led_init_data *init_data)
{
	struct mc_subled *info;
	int num_colors, i = 0, ret;

	num_colors = fwnode_get_child_node_count(fwnode);
	if (!num_colors || num_colors > AW9120_MAX_LEDS)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid colour count in %pfw\n", fwnode);

	info = devm_kcalloc(chip->dev, num_colors, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	fwnode_for_each_child_node_scoped(fwnode, child) {
		u32 color, reg;

		ret = fwnode_property_read_u32(child, "color", &color);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "missing colour in %pfw\n", child);

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "missing reg in %pfw\n", child);

		ret = aw9120_claim_channel(chip, reg);
		if (ret)
			return ret;

		info[i].color_index = color;
		info[i].channel = reg;
		info[i].intensity = AW9120_MAX_BRIGHTNESS;
		i++;
	}

	led->mcdev.subled_info = info;
	led->mcdev.num_colors = num_colors;
	led->mcdev.led_cdev.max_brightness = AW9120_MAX_BRIGHTNESS;
	led->mcdev.led_cdev.brightness_set_blocking = aw9120_brightness_mc_set;

	return devm_led_classdev_multicolor_register_ext(chip->dev, &led->mcdev,
							 init_data);
}

static int aw9120_setup_single(struct aw9120 *chip, struct fwnode_handle *fwnode,
			       struct aw9120_led *led,
			       struct led_init_data *init_data)
{
	u32 reg;
	int ret;

	ret = fwnode_property_read_u32(fwnode, "reg", &reg);
	if (ret)
		return dev_err_probe(chip->dev, ret, "missing reg in %pfw\n",
				     fwnode);

	ret = aw9120_claim_channel(chip, reg);
	if (ret)
		return ret;

	led->channel = reg;
	led->cdev.max_brightness = AW9120_MAX_BRIGHTNESS;
	led->cdev.brightness_set_blocking = aw9120_brightness_single_set;

	return devm_led_classdev_register_ext(chip->dev, &led->cdev, init_data);
}

static int aw9120_add_led(struct aw9120 *chip, struct fwnode_handle *fwnode,
			  unsigned int index)
{
	struct aw9120_led *led = &chip->leds[index];
	struct led_init_data init_data = { .fwnode = fwnode };
	u32 color = 0;
	int ret;

	led->chip = chip;

	/* "color" is optional for a single LED, but selects the RGB path. */
	ret = fwnode_property_read_u32(fwnode, "color", &color);
	if (ret && ret != -EINVAL)
		return dev_err_probe(chip->dev, ret, "bad colour in %pfw\n",
				     fwnode);

	if (color == LED_COLOR_ID_RGB)
		return aw9120_setup_rgb(chip, fwnode, led, &init_data);

	return aw9120_setup_single(chip, fwnode, led, &init_data);
}

static int aw9120_chip_init(struct aw9120 *chip)
{
	u32 mask = chip->used_mask;
	int i, ret;

	ret = regmap_write(chip->regmap, AW9120_REG_GCR, 0);
	if (ret)
		return ret;

	for (i = 0; i < 5; i++) {
		ret = regmap_write(chip->regmap, AW9120_REG_IMAX(i),
				   AW9120_IMAX_DEFAULT);
		if (ret)
			return ret;
	}

	/* Enable and command-control only the outputs described in DT. */
	ret = regmap_write(chip->regmap, AW9120_REG_LER1, mask & 0xfff);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW9120_REG_LER2, (mask >> 12) & 0xff);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW9120_REG_CTRS1, mask & 0xfff);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, AW9120_REG_CTRS2, (mask >> 12) & 0xff);
	if (ret)
		return ret;

	return regmap_write(chip->regmap, AW9120_REG_GCR, AW9120_GCR_ENABLE);
}

static void aw9120_power_off(void *data)
{
	gpiod_set_value_cansleep(data, 0);
}

static const struct regmap_config aw9120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW9120_REG_MAX,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int aw9120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *enable_gpio;
	struct aw9120 *chip;
	unsigned int chip_id;
	int count, i = 0, ret;

	count = device_get_child_node_count(dev);
	if (!count || count > AW9120_MAX_LEDS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid LED node count %d\n", count);

	chip = devm_kzalloc(dev, struct_size(chip, leds, count), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->num_leds = count;

	chip->regmap = devm_regmap_init_i2c(client, &aw9120_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "failed to init regmap\n");

	ret = devm_regulator_get_enable(dev, "vcc");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vcc\n");

	/* Optional PDN pin: pulse low then high to reset and power the chip. */
	enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(enable_gpio))
		return dev_err_probe(dev, PTR_ERR(enable_gpio),
				     "failed to get enable gpio\n");
	if (enable_gpio) {
		usleep_range(5000, 6000);
		gpiod_set_value_cansleep(enable_gpio, 1);
		usleep_range(5000, 6000);

		ret = devm_add_action_or_reset(dev, aw9120_power_off,
					       enable_gpio);
		if (ret)
			return ret;
	}

	ret = regmap_read(chip->regmap, AW9120_REG_RSTR, &chip_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id\n");
	if (chip_id != AW9120_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip id 0x%04x\n", chip_id);

	device_for_each_child_node_scoped(dev, child) {
		ret = aw9120_add_led(chip, child, i++);
		if (ret)
			return ret;
	}

	return aw9120_chip_init(chip);
}

static const struct of_device_id aw9120_of_match[] = {
	{ .compatible = "awinic,aw9120" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9120_of_match);

static struct i2c_driver aw9120_driver = {
	.driver = {
		.name = "leds-aw9120",
		.of_match_table = aw9120_of_match,
	},
	.probe = aw9120_probe,
};
module_i2c_driver(aw9120_driver);

MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_DESCRIPTION("Awinic AW9120 20-channel LED controller driver");
MODULE_LICENSE("GPL");
