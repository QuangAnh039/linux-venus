// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cortina-Access eFuse Driver
 *
 * Copyright (C) 2017 Cortina Access, Inc.
 *              http://www.cortina-access.com
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */
#include <linux/platform_device.h>
#include <linux/nvmem-provider.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>

#define FUSE_CONTROL		0x00
#define	  FUSE_GROUP_OFFSET	  0
#define	  FUSE_GROUP_MASK	  0x1F
#define	  FUSE_GROUP_MAX	  31
#define	  FUSE_NUMBER_OFFSET	  5
#define	  FUSE_NUMBER_MASK	  0x1F
#define	  FUSE_NUMBER_MAX	  31
#define	  FUSE_MR		  BIT(10)
#define	  FUSE_PROGRAM		  BIT(11)
#define	  FUSE_READ		  BIT(12)
#define FUSE_RESULT		0x04
#define	  FUSE_DONE		  BIT(0)
#define FUSE_VALUE		0x08
#define FUSE_PGM_TIMING		0x0C
#define FUSE_READ_TIMING	0x10

#define OP_READ			0
#define OP_WRITE		1

#define POLL_COUNT		100

struct ca_efuse_context {
	struct device *dev;
	void __iomem *base;
	unsigned int nregs;
	bool redundant;
};

static int ca_efuse_access(void *context, int group, u32 *val, int op)
{
	struct ca_efuse_context *priv = context;
	u32 reg;
	int i;

	if (op == OP_READ) {
		*val = 0;

		reg = (group & FUSE_GROUP_MASK) << FUSE_GROUP_OFFSET |
		      FUSE_READ;
		writel(reg, priv->base + FUSE_CONTROL);

		i = POLL_COUNT;
		do {
			reg = readl(priv->base + FUSE_RESULT);
			if (reg & FUSE_DONE)
				break;
			usleep_range(10, 20);
		} while (--i);

		if (i)
			*val = readl(priv->base + FUSE_VALUE);
		else
			dev_err(priv->dev, "FUSE: read group %d TIMEOUT!\n",
				group);

		writel(0, priv->base + FUSE_CONTROL);

		return i ? EIO : 0;
	}

	return 0;
}

static int ca_efuse_read(void *context, unsigned int offset,
			 void *val, size_t bytes)
{
	struct ca_efuse_context *priv = context;
	u32 redundant;
	int i, index, count;

	index = offset >> 2;
	count = bytes >> 2;

	if (count > (priv->nregs - index))
		count = priv->nregs - index;

	for (i = index; i < (index + count); i++) {
		ca_efuse_access(context, i, val, OP_READ);
		if (priv->redundant) {
			ca_efuse_access(context, i + priv->nregs,
					&redundant, OP_READ);
			*(u32 *)val = *(u32 *)val | redundant;
		}
		val += 4;
	}

	return (i - index) * 4;
}

static struct nvmem_config econfig = {
	.name = "ca-efuse",
	.owner = THIS_MODULE,
	.read_only = true,
	.stride = 4,
	.word_size = 4,
	.reg_read = ca_efuse_read,
};

static int ca_efuse_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct nvmem_device *nvmem;
	void __iomem *base;
	struct ca_efuse_context *context;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	context = devm_kzalloc(dev, sizeof(struct ca_efuse_context),
			       GFP_KERNEL);
	if (IS_ERR(context))
		return PTR_ERR(context);

	context->dev = dev;
	context->base = base;
	if (of_property_read_u32(np, "ngroups", &context->nregs)) {
		dev_warn(dev, "Missing ngroups OF property\n");
		context->nregs = 32;
	}
	if (of_property_read_bool(np, "redundant"))
		context->redundant = true;
	else
		context->redundant = false;
	if (context->redundant)
		context->nregs /= 2;

	econfig.size = context->nregs * econfig.word_size;
	econfig.dev = dev;
	econfig.priv = context;
	nvmem = nvmem_register(&econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static int ca_efuse_remove(struct platform_device *pdev)
{
	nvmem_unregister(platform_get_drvdata(pdev));

	return 0;
}

static const struct of_device_id ca_efuse_match[] = {
	{ .compatible = "cortina-access,ca-efuse",},
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, ca_efuse_match);

static struct platform_driver ca_efuse_driver = {
	.probe = ca_efuse_probe,
	.remove = ca_efuse_remove,
	.driver = {
		.name = "ca-efuse",
		.of_match_table = ca_efuse_match,
	},
};

module_platform_driver(ca_efuse_driver);
MODULE_DESCRIPTION("Cortina-Access eFuse driver");
MODULE_LICENSE("GPL");
