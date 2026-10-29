// SPDX-License-Identifier: GPL-2.0+
//
// da9211-regulator.c - Regulator device driver for DA9211/DA9212
// /DA9213/DA9223/DA9214/DA9224/DA9215/DA9225
// Copyright (C) 2015  Dialog Semiconductor Ltd.

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regmap.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/da9211.h>
#include <dt-bindings/regulator/dlg,da9211-regulator.h>
#include "da9211-regulator.h"
#include "mt6797-boot-barrier.h"

/* DEVICE IDs */
#define DA9211_DEVICE_ID	0x22
#define DA9213_DEVICE_ID	0x23
#define DA9215_DEVICE_ID	0x24

/* DA9211 REGULATOR IDs */
#define DA9211_ID_BUCKA	0
#define DA9211_ID_BUCKB	1

struct da9211 {
	struct device *dev;
	struct regmap *regmap;
	struct da9211_pdata *pdata;
	struct regulator_dev *rdev[DA9211_MAX_REGULATORS];
	unsigned long present;
	int chip_irq;
	int chip_id;
	bool boot_barrier;
	struct mutex gpio_lock;
	unsigned long gpio_enabled;
};

static const struct regmap_range_cfg da9211_regmap_range[] = {
	{
		.selector_reg = DA9211_REG_PAGE_CON,
		.selector_mask  = DA9211_REG_PAGE_MASK,
		.selector_shift = DA9211_REG_PAGE_SHIFT,
		.window_start = 0,
		.window_len = 256,
		.range_min = 0,
		.range_max = 5*128,
	},
};

static bool da9211_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case DA9211_REG_STATUS_A:
	case DA9211_REG_STATUS_B:
	case DA9211_REG_EVENT_A:
	case DA9211_REG_EVENT_B:
		return true;
	}
	return false;
}

static const struct regmap_config da9211_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 5 * 128,
	.volatile_reg = da9211_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
	.ranges = da9211_regmap_range,
	.num_ranges = ARRAY_SIZE(da9211_regmap_range),
};

/* Default limits measured in millivolts and milliamps */
#define DA9211_MIN_MV		300
#define DA9211_MAX_MV		1570
#define DA9211_STEP_MV		10

/* Current limits for DA9211 buck (uA) indices
 * corresponds with register values
 */
static const int da9211_current_limits[] = {
	2000000, 2200000, 2400000, 2600000, 2800000, 3000000, 3200000, 3400000,
	3600000, 3800000, 4000000, 4200000, 4400000, 4600000, 4800000, 5000000
};
/* Current limits for DA9213 buck (uA) indices
 * corresponds with register values
 */
static const int da9213_current_limits[] = {
	3000000, 3200000, 3400000, 3600000, 3800000, 4000000, 4200000, 4400000,
	4600000, 4800000, 5000000, 5200000, 5400000, 5600000, 5800000, 6000000
};
/* Current limits for DA9215 buck (uA) indices
 * corresponds with register values
 */
static const int da9215_current_limits[] = {
	4000000, 4200000, 4400000, 4600000, 4800000, 5000000, 5200000, 5400000,
	5600000, 5800000, 6000000, 6200000, 6400000, 6600000, 6800000, 7000000
};

static unsigned int da9211_map_buck_mode(unsigned int mode)
{
	switch (mode) {
	case DA9211_BUCK_MODE_SLEEP:
		return REGULATOR_MODE_STANDBY;
	case DA9211_BUCK_MODE_SYNC:
		return REGULATOR_MODE_FAST;
	case DA9211_BUCK_MODE_AUTO:
		return REGULATOR_MODE_NORMAL;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static unsigned int da9211_buck_get_mode(struct regulator_dev *rdev)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret, mode = 0;

	ret = regmap_read(chip->regmap, DA9211_REG_BUCKA_CONF+id, &data);
	if (ret < 0)
		return ret;

	switch (data & 0x03) {
	case DA9211_BUCK_MODE_SYNC:
		mode = REGULATOR_MODE_FAST;
		break;
	case DA9211_BUCK_MODE_AUTO:
		mode = REGULATOR_MODE_NORMAL;
		break;
	case DA9211_BUCK_MODE_SLEEP:
		mode = REGULATOR_MODE_STANDBY;
		break;
	}

	return mode;
}

static int da9211_buck_set_mode(struct regulator_dev *rdev,
					unsigned int mode)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	int val = 0;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = DA9211_BUCK_MODE_SYNC;
		break;
	case REGULATOR_MODE_NORMAL:
		val = DA9211_BUCK_MODE_AUTO;
		break;
	case REGULATOR_MODE_STANDBY:
		val = DA9211_BUCK_MODE_SLEEP;
		break;
	}

	return regmap_update_bits(chip->regmap, DA9211_REG_BUCKA_CONF+id,
					0x03, val);
}

static int da9211_set_current_limit(struct regulator_dev *rdev, int min,
				    int max)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	int i, max_size;
	const int *current_limits;

	switch (chip->chip_id) {
	case DA9211:
		current_limits = da9211_current_limits;
		max_size = ARRAY_SIZE(da9211_current_limits)-1;
		break;
	case DA9213:
		current_limits = da9213_current_limits;
		max_size = ARRAY_SIZE(da9213_current_limits)-1;
		break;
	case DA9215:
		current_limits = da9215_current_limits;
		max_size = ARRAY_SIZE(da9215_current_limits)-1;
		break;
	default:
		return -EINVAL;
	}

	/* search for closest to maximum */
	for (i = max_size; i >= 0; i--) {
		if (min <= current_limits[i] &&
		    max >= current_limits[i]) {
				return regmap_update_bits(chip->regmap,
					DA9211_REG_BUCK_ILIM,
					(0x0F << id*4), (i << id*4));
		}
	}

	return -EINVAL;
}

static int da9211_get_current_limit(struct regulator_dev *rdev)
{
	int id = rdev_get_id(rdev);
	struct da9211 *chip = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret;
	const int *current_limits;

	switch (chip->chip_id) {
	case DA9211:
		current_limits = da9211_current_limits;
		break;
	case DA9213:
		current_limits = da9213_current_limits;
		break;
	case DA9215:
		current_limits = da9215_current_limits;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(chip->regmap, DA9211_REG_BUCK_ILIM, &data);
	if (ret < 0)
		return ret;

	/* select one of 16 values: 0000 (2000mA or 3000mA)
	 * to 1111 (5000mA or 6000mA).
	 */
	data = (data >> id*4) & 0x0F;
	return current_limits[data];
}

/* Only the shared A53/CCI buck is subject to the board boot barrier. */
static bool da9211_boot_barrier(struct regulator_dev *rdev)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);

	return chip->boot_barrier && rdev_get_id(rdev) == DA9211_ID_BUCKA;
}

static int da9211_barrier_state(struct regulator_dev *rdev)
{
	unsigned int control;
	int ret;

	ret = regmap_read(rdev->regmap, DA9211_REG_BUCKA_CONT, &control);
	if (ret)
		return ret;
	/* The generic descriptor addresses bank A; do not infer bank B. */
	if (control & (DA9211_VBUCKA_SEL | DA9211_VBUCKA_GPI_MASK |
		       DA9211_BUCKA_GPI_MASK))
		return -EIO;
	return !!(control & DA9211_BUCKA_EN);
}

static int da9211_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	if (da9211_boot_barrier(rdev))
		return mt6797_boot_barrier_check(da9211_barrier_state(rdev),
				regulator_get_voltage_sel_regmap(rdev), sel);
	return regulator_set_voltage_sel_regmap(rdev, sel);
}

/* Supplied descriptors are borrowed from the platform owner. Never transfer
 * their only acquisition to a retryable child or regulator core. Serialize
 * votes for the two physical bucks when platform data shares one descriptor.
 */
static int da9211_supplied_gpio_set(struct regulator_dev *rdev, bool enable)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev), i;
	struct gpio_desc *gpio = chip->pdata->gpiod_ren[id];
	bool value = enable;
	unsigned long saved;
	int ret;

	mutex_lock(&chip->gpio_lock);
	saved = chip->gpio_enabled;
	if (enable)
		chip->gpio_enabled |= BIT(id);
	else
		chip->gpio_enabled &= ~BIT(id);
	for (i = 0; i < DA9211_MAX_REGULATORS; i++)
		if (chip->pdata->gpiod_ren[i] == gpio &&
		    (chip->gpio_enabled & BIT(i)))
			value = true;
	ret = gpiod_set_value_cansleep(gpio, value);
	if (!ret) {
		ret = gpiod_get_value_cansleep(gpio);
		if (ret >= 0)
			ret = ret == value ? 0 : -EIO;
	}
	if (ret)
		chip->gpio_enabled = saved;
	mutex_unlock(&chip->gpio_lock);
	return ret;
}

static int da9211_is_enabled(struct regulator_dev *rdev)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);
	struct gpio_desc *gpio = chip->pdata->gpiod_ren[rdev_get_id(rdev)];

	return gpio ? gpiod_get_value_cansleep(gpio) :
		regulator_is_enabled_regmap(rdev);
}

static int da9211_enable(struct regulator_dev *rdev)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);

	if (da9211_boot_barrier(rdev))
		return mt6797_boot_barrier_check(da9211_barrier_state(rdev), 0, 0);
	if (chip->pdata->gpiod_ren[rdev_get_id(rdev)])
		return da9211_supplied_gpio_set(rdev, true);
	return regulator_enable_regmap(rdev);
}

static int da9211_disable(struct regulator_dev *rdev)
{
	struct da9211 *chip = rdev_get_drvdata(rdev);

	if (da9211_boot_barrier(rdev))
		return -EBUSY;
	if (chip->pdata->gpiod_ren[rdev_get_id(rdev)])
		return da9211_supplied_gpio_set(rdev, false);
	return regulator_disable_regmap(rdev);
}

static const struct regulator_ops da9211_buck_ops = {
	.get_mode = da9211_buck_get_mode,
	.set_mode = da9211_buck_set_mode,
	.enable = da9211_enable,
	.disable = da9211_disable,
	.is_enabled = da9211_is_enabled,
	.set_voltage_sel = da9211_set_voltage_sel,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.set_current_limit = da9211_set_current_limit,
	.get_current_limit = da9211_get_current_limit,
};

#define DA9211_BUCK(_id) \
{\
	.name = #_id,\
	.ops = &da9211_buck_ops,\
	.type = REGULATOR_VOLTAGE,\
	.id = DA9211_ID_##_id,\
	.n_voltages = (DA9211_MAX_MV - DA9211_MIN_MV) / DA9211_STEP_MV + 1,\
	.min_uV = (DA9211_MIN_MV * 1000),\
	.uV_step = (DA9211_STEP_MV * 1000),\
	.enable_reg = DA9211_REG_BUCKA_CONT + DA9211_ID_##_id,\
	.enable_mask = DA9211_BUCKA_EN,\
	.vsel_reg = DA9211_REG_VBUCKA_A + DA9211_ID_##_id * 2,\
	.vsel_mask = DA9211_VBUCK_MASK,\
	.owner = THIS_MODULE,\
	.of_map_mode = da9211_map_buck_mode,\
}

static const struct regulator_desc da9211_regulators[] = {
	DA9211_BUCK(BUCKA),
	DA9211_BUCK(BUCKB),
};

#ifdef CONFIG_OF
static const struct of_regulator_match da9211_match_template[] = {
	[DA9211_ID_BUCKA] = {
		.name = "BUCKA",
		.desc = &da9211_regulators[DA9211_ID_BUCKA],
	},
	[DA9211_ID_BUCKB] = {
		.name = "BUCKB",
		.desc = &da9211_regulators[DA9211_ID_BUCKB],
	},
	};

static struct da9211_pdata *da9211_parse_regulators_dt(
		struct da9211 *chip)
{
	struct device *dev = chip->dev;
	struct of_regulator_match *matches;
	struct da9211_pdata *pdata;
	struct device_node *node;
	int i, num;

	matches = devm_kmemdup(dev, da9211_match_template,
			       sizeof(da9211_match_template), GFP_KERNEL);
	if (!matches)
		return ERR_PTR(-ENOMEM);

	node = of_get_child_by_name(dev->of_node, "regulators");
	if (!node) {
		dev_err(dev, "regulators node not found\n");
		return ERR_PTR(-ENODEV);
	}

	num = of_regulator_match(dev, node, matches,
				 ARRAY_SIZE(da9211_match_template));
	of_node_put(node);
	if (num < 0) {
		dev_err(dev, "Failed to match regulators\n");
		return ERR_PTR(num);
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->num_buck = num;

	for (i = 0; i < ARRAY_SIZE(da9211_match_template); i++) {
		if (!matches[i].of_node)
			continue;
		chip->present |= BIT(i);
		pdata->init_data[i] = matches[i].init_data;
		pdata->reg_node[i] = matches[i].of_node;
	}

	return pdata;
}
#else
static struct da9211_pdata *da9211_parse_regulators_dt(
		struct da9211 *chip)
{
	return ERR_PTR(-ENODEV);
}
#endif

static irqreturn_t da9211_irq_handler(int irq, void *data)
{
	struct da9211 *chip = data;
	int reg_val, err, ret = IRQ_NONE;

	err = regmap_read(chip->regmap, DA9211_REG_EVENT_B, &reg_val);
	if (err < 0)
		goto error_i2c;

	if (reg_val & DA9211_E_OV_CURR_A) {
		/* Pair with publication after regulator initialization. */
		struct regulator_dev *rdev = smp_load_acquire(&chip->rdev[0]);

		if (rdev)
			regulator_notifier_call_chain(rdev,
				REGULATOR_EVENT_OVER_CURRENT, NULL);

		err = regmap_write(chip->regmap, DA9211_REG_EVENT_B,
			DA9211_E_OV_CURR_A);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	if (reg_val & DA9211_E_OV_CURR_B) {
		/* Pair with publication after regulator initialization. */
		struct regulator_dev *rdev = smp_load_acquire(&chip->rdev[1]);

		if (rdev)
			regulator_notifier_call_chain(rdev,
				REGULATOR_EVENT_OVER_CURRENT, NULL);

		err = regmap_write(chip->regmap, DA9211_REG_EVENT_B,
			DA9211_E_OV_CURR_B);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	return ret;

error_i2c:
	dev_err(chip->dev, "I2C error : %d\n", err);
	return IRQ_NONE;
}

/* Clear publication and drain IRQ readers before devres frees the rdev. */
static void da9211_buck_unpublish(void *data)
{
	struct platform_device *pdev = data;
	struct da9211 *chip = dev_get_drvdata(pdev->dev.parent);
	int id = *(int *)dev_get_platdata(&pdev->dev);

	if (chip->chip_irq)
		regmap_update_bits(chip->regmap, DA9211_REG_MASK_B,
				   DA9211_M_OV_CURR_A << id,
				   DA9211_M_OV_CURR_A << id);
	/* Stop new IRQ readers before draining readers of the old pointer. */
	smp_store_release(&chip->rdev[id], NULL);
	if (chip->chip_irq)
		synchronize_irq(chip->chip_irq);
}

static void da9211_regulator_unregister(void *data)
{
	regulator_unregister(data);
}

static int da9211_buck_probe(struct platform_device *pdev)
{
	struct da9211 *chip = dev_get_drvdata(pdev->dev.parent);
	int id = *(int *)dev_get_platdata(&pdev->dev);
	struct regulator_config config = {
		.dev = &pdev->dev,
		.driver_data = chip,
		.regmap = chip->regmap,
		.init_data = chip->pdata->init_data[id],
		.of_node = chip->pdata->reg_node[id],
		.ena_gpiod = chip->pdata->gpiod_ren[id],
	};
	struct regulator_dev *rdev;
	unsigned int control;
	int ret;

	if (chip->boot_barrier && id == DA9211_ID_BUCKA) {
		/* Both names are accepted by OF GPIO lookup. Reject before get. */
		if (config.ena_gpiod ||
		    of_property_present(config.of_node, "enable-gpio") ||
		    of_property_present(config.of_node, "enable-gpios"))
			return -EOPNOTSUPP;
		ret = regmap_read(chip->regmap, DA9211_REG_BUCKA_CONT, &control);
		if (ret)
			return ret;
		if (!(control & DA9211_BUCKA_EN) ||
		    (control & (DA9211_VBUCKA_SEL | DA9211_VBUCKA_GPI_MASK |
				DA9211_BUCKA_GPI_MASK)))
			return -EIO;
	}

	if (config.ena_gpiod) {
		ret = gpiod_get_value_cansleep(config.ena_gpiod);
		if (ret < 0)
			return ret;
		mutex_lock(&chip->gpio_lock);
		if (ret)
			chip->gpio_enabled |= BIT(id);
		mutex_unlock(&chip->gpio_lock);
		/* The caller retains ownership through parent and child retries. */
		config.ena_gpiod = NULL;
	} else if (config.of_node) {
		config.ena_gpiod = devm_fwnode_gpiod_get(&pdev->dev,
				of_fwnode_handle(config.of_node), "enable",
				GPIOD_OUT_HIGH | GPIOD_FLAGS_BIT_NONEXCLUSIVE,
				"da9211-enable");
		if (IS_ERR(config.ena_gpiod)) {
			ret = PTR_ERR(config.ena_gpiod);
			if (ret != -ENOENT)
				return dev_err_probe(&pdev->dev, ret, "enable GPIO\n");
			config.ena_gpiod = NULL;
		} else {
			devm_gpiod_unhinge(&pdev->dev, config.ena_gpiod);
		}
	}
	rdev = regulator_register(&pdev->dev, &da9211_regulators[id], &config);
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev), "register buck\n");

	ret = devm_add_action_or_reset(&pdev->dev, da9211_regulator_unregister, rdev);
	if (ret)
		return ret;

	/* IRQ acquire loads must observe the fully initialized regulator. */
	smp_store_release(&chip->rdev[id], rdev);
	ret = devm_add_action_or_reset(&pdev->dev, da9211_buck_unpublish, pdev);
	if (ret)
		return ret;
	if (chip->chip_irq)
		return regmap_update_bits(chip->regmap, DA9211_REG_MASK_B,
					  DA9211_M_OV_CURR_A << id, 0);
	return 0;
}

static struct platform_driver da9211_buck_driver = {
	.probe = da9211_buck_probe,
	.driver.name = "da9211-buck",
};

static void da9211_buck_unregister(void *data)
{
	platform_device_unregister(data);
}

static int da9211_regulator_init(struct da9211 *chip)
{
	struct platform_device *buck;
	unsigned int data;
	int id, ret;

	ret = regmap_read(chip->regmap, DA9211_REG_CONFIG_E, &data);
	if (ret)
		return ret;
	if (!chip->present)
		return -ENODEV;
	/* Physical B does not exist in four-phase single-buck mode. */
	if (!(data & DA9211_SLAVE_SEL) && (chip->present & BIT(DA9211_ID_BUCKB)))
		return -EINVAL;

	/* Preserve enabled unbound siblings sharing a platform GPIO. */
	for (id = 0; id < DA9211_MAX_REGULATORS; id++) {
		if (!(chip->present & BIT(id)) || !chip->pdata->gpiod_ren[id])
			continue;
		ret = gpiod_get_value_cansleep(chip->pdata->gpiod_ren[id]);
		if (ret < 0)
			return ret;
		if (ret)
			chip->gpio_enabled |= BIT(id);
	}

	chip->boot_barrier = mt6797_boot_barrier_supply(
		chip->pdata->reg_node[DA9211_ID_BUCKA], "cpu-supply");
	for (id = 0; id < DA9211_MAX_REGULATORS; id++) {
		if (!(chip->present & BIT(id)))
			continue;
		buck = platform_device_register_data(chip->dev, "da9211-buck",
				PLATFORM_DEVID_AUTO, &id, sizeof(id));
		if (IS_ERR(buck))
			return PTR_ERR(buck);
		ret = devm_add_action_or_reset(chip->dev, da9211_buck_unregister, buck);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * I2C driver interface functions
 */
static int da9211_i2c_probe(struct i2c_client *i2c)
{
	struct da9211 *chip;
	int error, ret;
	unsigned int data;

	chip = devm_kzalloc(&i2c->dev, sizeof(struct da9211), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &i2c->dev;
	mutex_init(&chip->gpio_lock);
	chip->regmap = devm_regmap_init_i2c(i2c, &da9211_regmap_config);
	if (IS_ERR(chip->regmap)) {
		error = PTR_ERR(chip->regmap);
		dev_err(chip->dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	i2c_set_clientdata(i2c, chip);

	chip->pdata = i2c->dev.platform_data;

	ret = regmap_read(chip->regmap, DA9211_REG_DEVICE_ID, &data);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read DEVICE_ID reg: %d\n", ret);
		return ret;
	}

	switch (data) {
	case DA9211_DEVICE_ID:
		chip->chip_id = DA9211;
		break;
	case DA9213_DEVICE_ID:
		chip->chip_id = DA9213;
		break;
	case DA9215_DEVICE_ID:
		chip->chip_id = DA9215;
		break;
	default:
		dev_err(chip->dev, "Unsupported device id = 0x%x.\n", data);
		return -ENODEV;
	}

	if (!chip->pdata) {
		chip->pdata = da9211_parse_regulators_dt(chip);
	} else {
		if (chip->pdata->num_buck < 1 ||
		    chip->pdata->num_buck > DA9211_MAX_REGULATORS)
			return -EINVAL;
		chip->present = BIT(chip->pdata->num_buck) - 1;
	}

	if (IS_ERR(chip->pdata)) {
		dev_err(chip->dev, "No regulators defined for the platform\n");
		return PTR_ERR(chip->pdata);
	}

	chip->chip_irq = i2c->irq;

	if (chip->chip_irq != 0) {
		/* Children unmask only after publishing a successfully registered rdev. */
		ret = regmap_update_bits(chip->regmap, DA9211_REG_MASK_B,
				DA9211_M_OV_CURR_A | DA9211_M_OV_CURR_B,
				DA9211_M_OV_CURR_A | DA9211_M_OV_CURR_B);
		if (ret)
			return ret;
		ret = devm_request_threaded_irq(chip->dev, chip->chip_irq, NULL,
					da9211_irq_handler,
					IRQF_TRIGGER_LOW|IRQF_ONESHOT,
					"da9211", chip);
		if (ret != 0) {
			dev_err(chip->dev, "Failed to request IRQ: %d\n",
				chip->chip_irq);
			return ret;
		}
	} else {
		dev_warn(chip->dev, "No IRQ configured\n");
	}

	return da9211_regulator_init(chip);
}

static const struct i2c_device_id da9211_i2c_id[] = {
	{"da9211", DA9211},
	{"da9212", DA9212},
	{"da9213", DA9213},
	{"da9223", DA9223},
	{"da9214", DA9214},
	{"da9224", DA9224},
	{"da9215", DA9215},
	{"da9225", DA9225},
	{},
};
MODULE_DEVICE_TABLE(i2c, da9211_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id da9211_dt_ids[] = {
	{ .compatible = "dlg,da9211", .data = &da9211_i2c_id[0] },
	{ .compatible = "dlg,da9212", .data = &da9211_i2c_id[1] },
	{ .compatible = "dlg,da9213", .data = &da9211_i2c_id[2] },
	{ .compatible = "dlg,da9223", .data = &da9211_i2c_id[3] },
	{ .compatible = "dlg,da9214", .data = &da9211_i2c_id[4] },
	{ .compatible = "dlg,da9224", .data = &da9211_i2c_id[5] },
	{ .compatible = "dlg,da9215", .data = &da9211_i2c_id[6] },
	{ .compatible = "dlg,da9225", .data = &da9211_i2c_id[7] },
	{},
};
MODULE_DEVICE_TABLE(of, da9211_dt_ids);
#endif

static struct i2c_driver da9211_regulator_driver = {
	.driver = {
		.name = "da9211",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(da9211_dt_ids),
	},
	.probe = da9211_i2c_probe,
	.id_table = da9211_i2c_id,
};

static int __init da9211_init(void)
{
	int ret;

	ret = platform_driver_register(&da9211_buck_driver);
	if (ret)
		return ret;
	ret = i2c_add_driver(&da9211_regulator_driver);
	if (ret)
		platform_driver_unregister(&da9211_buck_driver);
	return ret;
}
module_init(da9211_init);

static void __exit da9211_exit(void)
{
	i2c_del_driver(&da9211_regulator_driver);
	platform_driver_unregister(&da9211_buck_driver);
}
module_exit(da9211_exit);

MODULE_AUTHOR("James Ban <James.Ban.opensource@diasemi.com>");
MODULE_DESCRIPTION("DA9211/DA9212/DA9213/DA9223/DA9214/DA9224/DA9215/DA9225 regulator driver");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_REGULATOR_DA9211_KUNIT_TEST)
#include "da9211-regulator-test.c"
#endif
