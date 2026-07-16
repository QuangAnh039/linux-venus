// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kirkwood thermal sensor driver
 *
 * Copyright (C) 2024 Cortina Access, Inc.
 *		http://www.cortina-access.com
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/delay.h>

#define TMS_REG_A		0x0
#define TMS_REG_B		0x4
#define TMS_REG_C		0x8
#define TMS_REG_DATA	0xC
#define TMS_REG_RANGE	0x10

#define CA_TEMPERATURE_NEGATIVE_FLAG	BIT(18)
#define CA_TEMPERATURE_MASK	0x0003FC00
#define CA_TEMPERATURE_FRACT_MASK	0x000003FF
#define CA_TEMPERATURE_SHIFT	10

/* Cortina Thermal Sensor Dev Structure */
struct ca_thermal_priv {
	void __iomem *sensor;
};

static int ca_get_temp(struct thermal_zone_device *thermal, int *temp)
{
	unsigned long reg;
	struct ca_thermal_priv *priv = thermal_zone_device_priv(thermal);
	int negative = 0;

	reg = readl_relaxed(priv->sensor + TMS_REG_DATA);

	/*
	 * Calculate temperature.
	 * The value is Celsius with lower 10 bits as fraction and
	 * [17:10] is integer and [18] is sign.
	 */
	negative = (reg & CA_TEMPERATURE_NEGATIVE_FLAG) ? 1 : 0;
	*temp = (reg & CA_TEMPERATURE_MASK) >> CA_TEMPERATURE_SHIFT;
	*temp *= 1000;
	*temp += (reg & CA_TEMPERATURE_FRACT_MASK);
	if (negative == 1) {
		*temp = 0x3ffff - *temp;
		*temp = 0 - *temp;
	}

	return 0;
}

static struct thermal_zone_device_ops ops = {
	.get_temp = ca_get_temp,
};

static const struct of_device_id ca_thermal_id_table[] = {
	{.compatible = "cortina-access,thermal-sensor"},
	{}
};

static int ca_thermal_probe(struct platform_device *pdev)
{
	struct thermal_zone_device *thermal = NULL;
	struct ca_thermal_priv *priv;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	unsigned int reg_v;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->sensor = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->sensor))
		return PTR_ERR(priv->sensor);

	if (of_property_read_u32(np, "tms-reg-a", &reg_v))
		writel_relaxed(0x07f80000, priv->sensor + TMS_REG_A);
	else
		writel_relaxed(reg_v, priv->sensor + TMS_REG_A);

	if (of_property_read_u32(np, "tms-reg-b", &reg_v))
		writel_relaxed(0x00374000, priv->sensor + TMS_REG_B);
	else
		writel_relaxed(reg_v, priv->sensor + TMS_REG_B);

	/* Reset thermal */
	writel_relaxed(0x0101010c, priv->sensor + TMS_REG_C);
	writel_relaxed(0x0101011d, priv->sensor + TMS_REG_C);

	mdelay(300);
	reg_v = readl_relaxed(priv->sensor + TMS_REG_C);
	reg_v = readl_relaxed(priv->sensor + TMS_REG_DATA);
	reg_v = readl_relaxed(priv->sensor + TMS_REG_DATA);
	dev_info(&pdev->dev, "SoC Temperature=%s%d\n",
		 reg_v & CA_TEMPERATURE_NEGATIVE_FLAG ? "-" : " ",
	       (reg_v & CA_TEMPERATURE_MASK) >> CA_TEMPERATURE_SHIFT);

	thermal = devm_thermal_of_zone_register(&pdev->dev, 0, priv,
						&ops);

	if (IS_ERR(thermal)) {
		dev_err(&pdev->dev, "Failed to register thermal zone device\n");
		return PTR_ERR(thermal);
	}

	platform_set_drvdata(pdev, thermal);

	return 0;
}

static int ca_thermal_exit(struct platform_device *pdev)
{
	struct thermal_zone_device *ca_thermal =
	    platform_get_drvdata(pdev);

	thermal_zone_device_unregister(ca_thermal);

	return 0;
}

MODULE_DEVICE_TABLE(of, ca_thermal_id_table);

static struct platform_driver ca_thermal_driver = {
	.probe = ca_thermal_probe,
	.remove = ca_thermal_exit,
	.driver = {
		   .name = "ca_thermal",
		   .of_match_table = ca_thermal_id_table,
		   },
};

module_platform_driver(ca_thermal_driver);

MODULE_DESCRIPTION("Cortina-Access thermal driver");
MODULE_LICENSE("GPL");
