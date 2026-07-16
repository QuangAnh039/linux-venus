// SPDX-License-Identifier: GPL-2.0
/*
 *
 * BRIEF MODULE DESCRIPTION
 *  Driver for Cortina Access RESET controller.
 *
 * Copyright (C) 2015 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 * Based on reset-socfpga.c
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define NR_BANKS		1
#define SZ_BANK			32
#define OFFSET_BLOCK_RESET	0x0

struct ca_reset_data {
	spinlock_t			lock;	/* lock access to bank regs */
	void __iomem			*iobase;

	struct reset_controller_dev	rcdev;
};

static int ca_reset_assert(struct reset_controller_dev *rcdev,
			   unsigned long id)
{
	struct ca_reset_data *data = container_of(rcdev,
						  struct ca_reset_data,
						  rcdev);
	int bank = id / SZ_BANK;
	int offset = id % SZ_BANK;
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&data->lock, flags);

	reg = readl(data->iobase + OFFSET_BLOCK_RESET + (bank * NR_BANKS));
	writel(reg | BIT(offset), data->iobase + OFFSET_BLOCK_RESET +
				 (bank * NR_BANKS));
	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int ca_reset_deassert(struct reset_controller_dev *rcdev,
			     unsigned long id)
{
	struct ca_reset_data *data = container_of(rcdev,
						  struct ca_reset_data,
						  rcdev);

	int bank = id / SZ_BANK;
	int offset = id % SZ_BANK;
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&data->lock, flags);

	reg = readl(data->iobase + OFFSET_BLOCK_RESET + (bank * NR_BANKS));
	writel(reg & ~BIT(offset), data->iobase + OFFSET_BLOCK_RESET +
				  (bank * NR_BANKS));
	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int ca_reset_status(struct reset_controller_dev *rcdev,
			   unsigned long id)
{
	struct ca_reset_data *data = container_of(rcdev,
						     struct ca_reset_data,
						     rcdev);
	int bank = id / SZ_BANK;
	int offset = id % SZ_BANK;
	u32 reg;

	reg = readl(data->iobase + OFFSET_BLOCK_RESET + (bank * NR_BANKS));
	return (reg & BIT(offset));
}

static const struct reset_control_ops ca_reset_ops = {
	.assert		= ca_reset_assert,
	.deassert	= ca_reset_deassert,
	.status		= ca_reset_status,
};

static int ca_reset_probe(struct platform_device *pdev)
{
	struct ca_reset_data *data;
	struct resource *res;

	/*
	 * The binding was mainlined without the required property.
	 * Do not continue, when we encounter an old DT.
	 */
	if (!of_find_property(pdev->dev.of_node, "#reset-cells", NULL)) {
		dev_err(&pdev->dev, "%s missing #reset-cells property\n",
			pdev->dev.of_node->full_name);
		return -EINVAL;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	data->iobase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->iobase))
		return PTR_ERR(data->iobase);
	dev_info(&pdev->dev, "resource - %pr mapped at 0x%pK\n", res,
		 data->iobase);

	spin_lock_init(&data->lock);

	data->rcdev.owner = THIS_MODULE;
	data->rcdev.nr_resets = NR_BANKS * SZ_BANK;
	data->rcdev.ops = &ca_reset_ops;
	data->rcdev.of_node = pdev->dev.of_node;
	reset_controller_register(&data->rcdev);

	return 0;
}

static int ca_reset_remove(struct platform_device *pdev)
{
	struct ca_reset_data *data = platform_get_drvdata(pdev);

	reset_controller_unregister(&data->rcdev);

	return 0;
}

static const struct of_device_id ca_reset_dt_ids[] = {
	{ .compatible = "cortina-access,rst-mgr", },
	{ /* sentinel */ },
};

static struct platform_driver ca_reset_driver = {
	.probe	= ca_reset_probe,
	.remove	= ca_reset_remove,
	.driver = {
		.name		= "ca-reset",
		.of_match_table	= ca_reset_dt_ids,
	},
};
module_platform_driver(ca_reset_driver);

MODULE_DESCRIPTION("Cortina-Access Reset Controller Driver");
MODULE_LICENSE("GPL");
