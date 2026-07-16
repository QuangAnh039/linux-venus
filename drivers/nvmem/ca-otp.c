// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Cortina Access, Inc.
 *              http://www.cortina-access.com
 *
 * Based on qfprom.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#ifdef	CONFIG_CORTINA_ACCESS_SMCC
#include <linux/arm-smccc.h>
#include <soc/cortina-access/ca-smc.h>
#include <soc/cortina-access/ca-soc.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#define OTP_APP_RANGE_SZ	0x100
#define GPH_PARAM_OFSET		0x60
#define GPHY_PARAMETER_SZ	0x40
unsigned char *gphy_parameters;
phys_addr_t phys_addr;
#endif

static int ca_otp_reg_read(void *context,
			   unsigned int reg, void *_val, size_t bytes)
{
#ifndef	CONFIG_CORTINA_ACCESS_SMCC
	void __iomem *base = context;
#endif
	u8 *val = _val;
	int i = 0, words = bytes;

#ifdef	CONFIG_CORTINA_ACCESS_SMCC
	CA_SMC_GET_GPHY_PARAM((phys_addr + GPH_PARAM_OFSET));

	while (words--)
		*val++ = gphy_parameters[reg + i++];
#else
	while (words--)
		*val++ = readb(base + reg + i++);
#endif
	return 0;
}

static int ca_otp_reg_write(void *context,
			    unsigned int reg, void *_val, size_t bytes)
{
	void __iomem *base = context;
	u8 *val = _val;
	int i = 0, words = bytes;

#ifdef	CONFIG_CORTINA_ACCESS_SMCC
	return -EIO;
#endif
	while (words--)
		writeb(*val++, base + reg + i++);

	return 0;
}

static int ca_otp_remove(struct platform_device *pdev)
{
	nvmem_unregister(platform_get_drvdata(pdev));

	return 0;
}

static struct nvmem_config econfig = {
	.name = "ca-otp",
	.owner = THIS_MODULE,
	.read_only = true,
	.stride = 1,
	.word_size = 1,
	.reg_read = ca_otp_reg_read,
	.reg_write = ca_otp_reg_write,
};

static int ca_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
#ifndef	CONFIG_CORTINA_ACCESS_SMCC
	void __iomem *base;
#endif
	struct nvmem_device *nvmem;

#ifdef	CONFIG_CORTINA_ACCESS_SMCC
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	econfig.size = resource_size(res);
	econfig.dev = dev;
	econfig.priv = 0;

	gphy_parameters = kzalloc(OTP_APP_RANGE_SZ, 0);
	phys_addr = dma_map_single(dev, gphy_parameters,
				   OTP_APP_RANGE_SZ, DMA_BIDIRECTIONAL);
	CA_SMC_GET_GPHY_PARAM((phys_addr + GPH_PARAM_OFSET));
#else

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (IS_ERR(base))
		return PTR_ERR(base);

	econfig.size = resource_size(res);
	econfig.dev = dev;
	econfig.priv = base;
#endif
	nvmem = devm_nvmem_register(dev, &econfig);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	platform_set_drvdata(pdev, nvmem);

	return 0;
}

static const struct of_device_id ca_otp_of_match[] = {
	{ .compatible = "cortina-access,ca-otp",},
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, ca_otp_of_match);

static struct platform_driver ca_otp_driver = {
	.probe = ca_otp_probe,
	.remove = ca_otp_remove,
	.driver = {
		.name = "cortina-access,cortina-otp",
		.of_match_table = ca_otp_of_match,
	},
};

static int __init ca_otp_init(void)
{
	int ret;

	ret = platform_driver_register(&ca_otp_driver);
	if (ret) {
		pr_err("Failed to register otp driver\n");
		return ret;
	}

	return 0;
}

static void __exit ca_otp_exit(void)
{
	return platform_driver_unregister(&ca_otp_driver);
}

subsys_initcall(ca_otp_init);
module_exit(ca_otp_exit);

MODULE_DESCRIPTION("Cortina-Access OTP driver");
MODULE_LICENSE("GPL");
