// SPDX-License-Identifier: GPL-2.0

/*
 * tps56921-regulator.c -- TI TPS56921
 *
 * Regulator driver for TPS56921 Synchronous Step Down SWIFT Converter With VID
 * Control.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/bitmap.h>
#include <linux/mod_devicetable.h>

#define TPS56921_BASE_VOLTAGE			720000
#define TPS56921_VOLTAGE_STEP			10000
#define TPS56921_N_VOLTAGES			77
#define TPS56921_MAX_VOLTAGE			1480000
#define TPS56921_EXTERNAL_MODE			0x7F
#define TPS56921_DEFAULT_VOLTAGE		1000000
#define TPS56921_CODE_MASK			0x7F

/* TPS56921 chip information */
struct tps56921_chip {
	struct device *dev;
	struct regulator_desc desc;
	struct regulator_dev *rdev;
	struct i2c_client *client;

	u8 voltage_sel;
	bool bypass;
};

static const struct linear_range tps56921_voltage_ranges[] = {
	REGULATOR_LINEAR_RANGE(TPS56921_BASE_VOLTAGE, 0,
			       TPS56921_N_VOLTAGES - 1,
			       TPS56921_VOLTAGE_STEP),
};

struct tps56921_chip *dbg_tps56921;
static unsigned char tps56921_code;

static inline u8 reg_chksum_update(u8 code)
{
	unsigned long temp;

	code &= TPS56921_CODE_MASK;
	temp = (unsigned long)code;

	temp = (bitmap_weight(&temp, 7) % 2 ? 0x80 : 0) | temp;
	code = (u8)temp;

	return code;
}

static int tps56921_write_reg(struct tps56921_chip *tps, u8 code)
{
	struct i2c_client *client = tps->client;
	u8 val;
	int ret;

	val = reg_chksum_update(code);
	dev_dbg(&client->dev, "select 0x%x, val 0x%x\n", code, val);
	ret = i2c_master_send(client, (char *)&val, 1);
	if (ret < 0) {
		dev_err(&client->dev, "I2C write error\n");
		return ret;
	}

	return 0;
}

static int tps56921_get_voltage_sel(struct regulator_dev *rdev)
{
	struct tps56921_chip *tps = rdev_get_drvdata(rdev);

	/* register is write-only */
	return tps->voltage_sel;
}

static int tps56921_set_voltage_sel(struct regulator_dev *rdev,
				    unsigned int selector)
{
	struct tps56921_chip *tps = rdev_get_drvdata(rdev);
	int ret;

	if (selector >= TPS56921_N_VOLTAGES)
		return -EINVAL;

	if (tps->bypass) {
		tps->voltage_sel = selector;
		return 0;
	}

	ret = tps56921_write_reg(tps, selector);
	if (!ret)
		tps->voltage_sel = selector;

	return ret;
}

static int tps56921_get_bypass(struct regulator_dev *reg, bool *enable)
{
	struct tps56921_chip *tps = rdev_get_drvdata(reg);

	*enable = tps->bypass;

	return 0;
}

static int tps56921_set_bypass(struct regulator_dev *reg, bool enable)
{
	struct tps56921_chip *tps = rdev_get_drvdata(reg);
	u8 selector;
	int ret;

	if (enable == tps->bypass)
		return 0;

	selector = enable ? TPS56921_EXTERNAL_MODE : tps->voltage_sel;
	ret = tps56921_write_reg(tps, selector);
	if (!ret)
		tps->bypass = enable;

	return ret;
}

static const struct regulator_ops tps56921_vdd_ops = {
	.get_voltage_sel	= tps56921_get_voltage_sel,
	.set_voltage_sel	= tps56921_set_voltage_sel,
	.list_voltage		= regulator_list_voltage_linear_range,
	.get_bypass		= tps56921_get_bypass,
	.set_bypass		= tps56921_set_bypass,
};

int tps56921_regulator_init(void *driver_data)
{
	struct tps56921_chip *tps = (struct tps56921_chip *)driver_data;
	u8 selector;

	if (tps->bypass)
		selector = TPS56921_EXTERNAL_MODE;
	else
		selector = tps->voltage_sel;

	return tps56921_write_reg(tps, selector);
}

static int tps56921_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = client->dev.of_node;
	struct regulator_init_data *init_data;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct tps56921_chip *tps;
	const __be32 *default_uV;
	int init_uV;

	tps = devm_kzalloc(dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;

	tps->client = client;

	tps->dev = &client->dev;
	tps->desc.name = client->name;
	tps->desc.id = 0;
	tps->desc.min_uV = TPS56921_BASE_VOLTAGE;
	tps->desc.uV_step = TPS56921_VOLTAGE_STEP;
	tps->desc.n_voltages = TPS56921_N_VOLTAGES;
	tps->desc.linear_ranges = tps56921_voltage_ranges,
	tps->desc.n_linear_ranges = ARRAY_SIZE(tps56921_voltage_ranges),
	tps->desc.ops = &tps56921_vdd_ops;
	tps->desc.type = REGULATOR_VOLTAGE;
	tps->desc.owner = THIS_MODULE;

	i2c_set_clientdata(client, tps);

	init_data = of_get_regulator_init_data(dev, np, &tps->desc);
	tps->bypass = of_property_read_bool(np, "ti,external-vdd-mode");
	default_uV = of_get_property(np, "ti,default-uV", NULL);
	if (default_uV)
		init_uV = be32_to_cpu(*default_uV);
	else
		init_uV = TPS56921_DEFAULT_VOLTAGE;
	if (init_uV < init_data->constraints.min_uV)
		init_uV = init_data->constraints.min_uV;
	else if (init_uV > init_data->constraints.max_uV)
		init_uV = init_data->constraints.max_uV;

	tps->voltage_sel = (init_uV - TPS56921_BASE_VOLTAGE) /
			   TPS56921_VOLTAGE_STEP;

	init_data->regulator_init = tps56921_regulator_init;
	init_data->driver_data = tps;

	/* Register the regulators */
	config.dev = &client->dev;
	config.init_data = init_data;
	config.driver_data = tps;
	config.of_node = client->dev.of_node;

	rdev = devm_regulator_register(&client->dev, &tps->desc, &config);
	if (IS_ERR(rdev)) {
		dev_err(tps->dev, "regulator register failed\n");
		return PTR_ERR(rdev);
	}

	tps->rdev = rdev;
	dbg_tps56921 = tps;
	tps56921_code = tps->voltage_sel;

	return 0;
}

static int tps56921_code_param_set(const char *arg,
				   const struct kernel_param *kp)
{
	u8 new_code;
	int ret;

	ret = kstrtou8(arg, 10, &new_code);
	if (ret)
		return ret;

	if (!dbg_tps56921)
		return -ENODEV;

	ret = tps56921_write_reg(dbg_tps56921, new_code);
	if (!ret)
		dbg_tps56921->voltage_sel = new_code;

	return ret;
}

static int tps56921_code_param_get(char *buffer, const struct kernel_param *kp)
{
	if (!dbg_tps56921)
		return -ENODEV;

	return sprintf(buffer, "%d", dbg_tps56921->voltage_sel);
}

static const struct kernel_param_ops tps56921_code_param_ops = {
	.set =		tps56921_code_param_set,
	.get =		tps56921_code_param_get,
};
module_param_cb(code, &tps56921_code_param_ops, &tps56921_code, 0644);

static const struct i2c_device_id tps56921_id[] = {
	{.name = "tps56921"},
	{},
};
MODULE_DEVICE_TABLE(i2c, tps56921_id);

#ifdef CONFIG_OF
static const struct of_device_id tps56921_of_match[] = {
	{ .compatible = "ti,tps56921"},
	{},
};
MODULE_DEVICE_TABLE(of, tps56921_of_match);
#endif

static struct i2c_driver tps56921_driver = {
	.driver = {
		.name	= "tps56921",
		.of_match_table = of_match_ptr(tps56921_of_match),
	},
	.probe		= tps56921_probe,
	.id_table	= tps56921_id,
};
module_i2c_driver(tps56921_driver);

MODULE_DESCRIPTION("TPS56921 voltage regulator driver");
MODULE_LICENSE("GPL");
