// SPDX-License-Identifier: GPL-2.0+
/*
 * Physical USB3.0 PHY Device Driver for usb core controller on
 * Cortina-Access SoCs
 *
 * Copyright (C) 2022 Cortina Access, Inc.
 *		 http://www.cortina-access.com
 *
 * Based on phy-rtk-usb3.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/usb/otg.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/ch11.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/reset.h>

#define CA_USB3PHY_NAME "ca-usb3phy"

/* USB3(USB Super Speed) register offsets */
#define USB3CFG_CNTRL_OFFSET				0x00
#define USB3CFG_PHY_VAUX_RESET				BIT(31)
#define USB3CFG_BUS_CLKEN_GSLAVE			BIT(30)
#define USB3CFG_BUS_CLKEN_GMASTER			BIT(29)
#define USB3CFG_BIGENDIAN_GSLAVE			BIT(28)
#define USB3CFG_HOST_PORT_POWER_CTRL_PRESENT		BIT(27)
#define USB3CFG_HOST_MSI_ENABLE				BIT(26)
#define USB3CFG_HOST_LEGACY_SMI_PCI_CMD_REG_WR		BIT(25)
#define USB3CFG_HOST_LEGACY_SMI_BAR_WR			BIT(24)
#define USB3CFG_FLADJ_30MHZ_REG_MASK(x)			((0x3f & (x)) << 16)
#define USB3CFG_XHCI_BUS_MASTER_ENABLE			BIT(4)
#define USB3CFG_BUS_FILTER_BYPASS_MASK(x)		(0xf & (x))

#define USB3CFG_STATUS_OFFSET				0x04
#define USB3CFG_HOST_SYSTEM_ERR				BIT(31)
#define USB3CFG_LEGACY_SMI_INTERRUPT			BIT(16)
#define USB3CFG_HOST_CURRENT_BELT_MASK(x)		(0xfff & (x))

#define USB3CFG_PORT_CONFIG_OFFSET			0x08
#define USB3CFG_U3P1_HOST_DISABLE			BIT(26)
#define USB3CFG_U3P1_HUB_PORT_OVERCURRENT		BIT(25)
#define USB3CFG_U3P1_HUB_PORT_PERM_ATTACH		BIT(24)
#define USB3CFG_U3P0_HOST_DISABLE			BIT(18)
#define USB3CFG_U3P0_HUB_PORT_OVERCURRENT		BIT(17)
#define USB3CFG_U3P0_HUB_PORT_PERM_ATTACH		BIT(16)
#define USB3CFG_U2P1_HOST_DISABLE			BIT(10)
#define USB3CFG_U2P1_HUB_PORT_OVERCURRENT		BIT(9)
#define USB3CFG_U2P1_HUB_PORT_PERM_ATTACH		BIT(8)
#define USB3CFG_U2P0_HOST_DISABLE			BIT(2)
#define USB3CFG_U2P0_HUB_PORT_OVERCURRENT		BIT(1)
#define USB3CFG_U2P0_HUB_PORT_PERM_ATTACH		BIT(0)

#define USB3CFG_PORT_STATUS_OFFSET			0x0c
#define USB3CFG_U3P1_PIPE3_PHY_MODE_MASK(x)		((0x3 & (x)) << 28)
#define USB3CFG_U3P1_HUB_VBUS_CTRL			BIT(24)
#define USB3CFG_U3P0_PIPE3_PHY_MODE_MASK(x)		((0x3 & (x)) << 20)
#define USB3CFG_U3P0_HUB_VBUS_CTRL			BIT(16)
#define USB3CFG_U2P1_UTMI_FSLS_LOW_POWER		BIT(9)
#define USB3CFG_U2P1_HUB_VBUS_CTRL			BIT(8)
#define USB3CFG_U2P0_UTMI_FSLS_LOW_POWER		BIT(1)
#define USB3CFG_U2P0_HUB_VBUS_CTRL			BIT(0)

#define USB3CFG_PHY_PORT0_CONFIG0_OFFSET		0x10
#define USB3CFG_PHY_PORT1_CONFIG0_OFFSET		0x1c
#define USB3CFG_PHY_PORT_CKBUF_EN			BIT(31)
#define USB3CFG_PHY_PORT_MAC_PHY_PLL_EN_REG		BIT(30)
#define USB3CFG_PHY_PORT_MAC_PHY_RECV_DET_EN_REG	BIT(29)
#define USB3CFG_PHY_PORT_MAC_PHY_RECV_DET_REQ_REG	BIT(28)
#define USB3CFG_PHY_PORT_CLK_MODE_SEL_MASK(x)		(0x3 & (x))

#define USB3CFG_PHY_PORT0_STATUS0_OFFSET		0x14
#define USB3CFG_PHY_PORT1_STATUS0_OFFSET		0x20
#define USB3CFG_PHY_PORT_PHY_MAC_RECV_DET_END		BIT(21)
#define USB3CFG_PHY_PORT_PHY_MAC_RECV_DET_ACK		BIT(20)
#define USB3CFG_PHY_PORT_CLK_RDY			BIT(19)
#define USB3CFG_PHY_PORT_COUNT_NUM_VAL(x)		(0x7ffff & (x))

#define USB3CFG_PHY_PORT0_STATUS1_OFFSET		0x18
#define USB3CFG_PHY_PORT1_STATUS1_OFFSET		0x24
#define USB3CFG_PHY_PORT_DISPARITY_ERR_MASK(x)		((0xff & (x)) << 24)
#define USB3CFG_PHY_PORT_ELASTIC_BUF_UDF_MASK(x)	((0xff & (x)) << 16)
#define USB3CFG_PHY_PORT_ELASTIC_BUF_OVF_MASK(x)	((0xff & (x)) << 8)
#define USB3CFG_PHY_PORT_DECODE_ERR_MASK(x)		(0xff & (x))

struct phy_data {
	int size;
	u8 *addr;
	u16 *data;
};

struct ca_usb3_phy {
	struct usb_phy phy;
	struct device *dev;
	void __iomem *phy_regbase;
	int u2port_mask;
	int u3port_mask;
#if defined(CONFIG_ARCH_CA_VENUS)
	struct gpio_desc *u3port0_vbus;
	struct gpio_desc *u3port1_vbus;
	struct regulator *port0_vbus_supply;
	struct regulator *port1_vbus_supply;
#endif
	struct phy_data *phy_data;
	void __iomem *p0_regbase;
	void __iomem *p1_regbase;
#if defined(CONFIG_ARCH_CA_G3) || defined(CONFIG_ARCH_CA_G3HGU)
	struct gpio_desc *phy_vbus;
#endif
	struct reset_control *s2usb_dphy_reset;
	struct reset_control *s3usb_dphy_reset;
	struct phy *s2_phy;
	struct phy *s3_phy;
	spinlock_t lock;	/* lock access to bank regs */
};

static inline u32 ca_usb3_phy_read(struct ca_usb3_phy *ca_phy,
				   u8 addr, int port_id)
{
	u32 data = 0;
	u32 offset = addr << 2;

	if (port_id == 0)
		data = readl(ca_phy->p0_regbase + offset);
	if (port_id == 1)
		data = readl(ca_phy->p1_regbase + offset);
	dev_info(ca_phy->dev,
		 "Read port<%d>, offset=0x%04x, data=0x%08x\n",
		 port_id, offset, data);

	return data;
}

static inline void ca_usb3_phy_write(struct ca_usb3_phy *ca_phy,
				     u8 addr, u16 data, int port_id)
{
	u32 offset = addr << 2;

	if (port_id == 0)
		writel(data, ca_phy->p0_regbase + offset);
	if (port_id == 1)
		writel(data, ca_phy->p1_regbase + offset);
}

static void ca_usb3_host_reset(struct ca_usb3_phy *ca_phy)
{
	int u2port_mask = ca_phy->u2port_mask;
	int u3port_mask = ca_phy->u3port_mask;
	u32 reg_val = 0;
	unsigned long flags;

	/* DPhy reset */
	reset_control_assert(ca_phy->s2usb_dphy_reset);
	msleep(20);
	reset_control_assert(ca_phy->s3usb_dphy_reset);
	msleep(20);

	/* On Venus platform, it should release S3 reset first,
	 * and then release S2 reset later.
	 * But G3/G3HGU don't have this requirement.
	 */
	if (u3port_mask & 0x2) {
		phy_power_on(ca_phy->s3_phy);
		msleep(20);
	}
	if (u3port_mask & 0x1) {
		phy_power_on(ca_phy->s2_phy);
		msleep(20);
	}
	reset_control_deassert(ca_phy->s2usb_dphy_reset);
	msleep(20);
	reset_control_deassert(ca_phy->s3usb_dphy_reset);
	msleep(20);

	/* For USB3 PHY RESET */
	spin_lock_irqsave(&ca_phy->lock, flags);
	reg_val = readl(ca_phy->phy_regbase + USB3CFG_CNTRL_OFFSET);
	reg_val |= USB3CFG_PHY_VAUX_RESET;
	writel(reg_val, ca_phy->phy_regbase + USB3CFG_CNTRL_OFFSET);
	dev_dbg(ca_phy->dev, "read USB3CFG_CNTRL reg_val = 0x%08x",
		readl(ca_phy->phy_regbase + USB3CFG_CNTRL_OFFSET));

	/* For USB3 PORT CONFIG */
	reg_val = readl(ca_phy->phy_regbase + USB3CFG_PORT_CONFIG_OFFSET);
	if ((u2port_mask & 0x2) == 0)
		reg_val |= USB3CFG_U2P1_HOST_DISABLE;
	if ((u2port_mask & 0x1) == 0)
		reg_val |= USB3CFG_U2P0_HOST_DISABLE;

	if ((u3port_mask & 0x2) == 0)
		reg_val |= USB3CFG_U3P1_HOST_DISABLE;
	if ((u3port_mask & 0x1) == 0)
		reg_val |= USB3CFG_U3P0_HOST_DISABLE;

	writel(reg_val, ca_phy->phy_regbase + USB3CFG_PORT_CONFIG_OFFSET);
	dev_dbg(ca_phy->dev,
		"read USB3CFG_PORT_CONFIG reg_val = 0x%08x",
		readl(ca_phy->phy_regbase +
		USB3CFG_PORT_CONFIG_OFFSET));
	spin_unlock_irqrestore(&ca_phy->lock, flags);
}

static int ca_usb3_phy_init(struct usb_phy *phy)
{
	struct ca_usb3_phy *ca_phy = (struct ca_usb3_phy *)phy;
	u8 *addr = ca_phy->phy_data->addr;
	u16 *data = ca_phy->phy_data->data;
	int size = ca_phy->phy_data->size;
	int u3port_mask = ca_phy->u3port_mask;
	int index;

#if defined(CONFIG_ARCH_CA_G3) || defined(CONFIG_ARCH_CA_G3HGU)
	if (ca_phy->phy_vbus) {
		gpio_set_value(ca_phy->phy_vbus, 0);
		msleep(100);
		gpio_set_value(ca_phy->phy_vbus, 1);
		msleep(100);
	}
#endif
	if (u3port_mask & 0x1) {
		for (index = 0; index < size; index++) {
			ca_usb3_phy_write(ca_phy, *(addr + index),
					  *(data + index), 0);
		}
	}
	if (u3port_mask & 0x2) {
		for (index = 0; index < size; index++) {
			ca_usb3_phy_write(ca_phy, *(addr + index),
					  *(data + index), 1);
		}
	}

	return 0;
}

static int ca_usb3_phy_set_vbus(struct usb_phy *phy, int on)
{
#if defined(CONFIG_ARCH_CA_VENUS)
	struct ca_usb3_phy *ca_phy = (struct ca_usb3_phy *)phy;
	int u3port_mask = ca_phy->u3port_mask;
	int ret;

	if (on) { /* on == true */
		if (u3port_mask & 0x1) {
			if (ca_phy->port0_vbus_supply) {
				ret = regulator_enable(ca_phy->port0_vbus_supply);
				if (ret)
					return ret;
			} else {
				gpiod_set_value_cansleep(ca_phy->u3port0_vbus, 1);
			}
		}
		if (u3port_mask & 0x2) {
			if (ca_phy->port1_vbus_supply) {
				ret = regulator_enable(ca_phy->port1_vbus_supply);
				if (ret)
					return ret;
			} else {
				gpiod_set_value_cansleep(ca_phy->u3port1_vbus, 1);
			}
		}
	} else { /* on == false */
		if (u3port_mask & 0x1) {
			if (ca_phy->port0_vbus_supply) {
				ret = regulator_disable(ca_phy->port0_vbus_supply);
				if (ret)
					return ret;
			} else {
				gpiod_set_value_cansleep(ca_phy->u3port0_vbus, 0);
			}
		}
		if (u3port_mask & 0x2) {
			if (ca_phy->port1_vbus_supply) {
				ret = regulator_disable(ca_phy->port1_vbus_supply);
				if (ret)
					return ret;
			} else {
				gpiod_set_value_cansleep(ca_phy->u3port1_vbus, 0);
			}
		}
	}

	msleep(100);
#endif
	return 0;
}

static int ca_usb3_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct ca_usb3_phy *ca_usb_phy;
	struct phy_data *phy_data;
	struct resource *res;
	int ret = 0;

	ca_usb_phy = devm_kzalloc(dev, sizeof(*ca_usb_phy), GFP_KERNEL);
	if (!ca_usb_phy)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "u3host_reg");
	ca_usb_phy->phy_regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(ca_usb_phy->phy_regbase))
		return PTR_ERR(ca_usb_phy->phy_regbase);
	dev_info(dev, "usb3_phy resource - %pr mapped at 0x%pK\n", res,
		 ca_usb_phy->phy_regbase);

#if defined(CONFIG_ARCH_CA_VENUS)
	ca_usb_phy->port0_vbus_supply =
		devm_regulator_get_optional(dev, "port0-vbus");
	if (IS_ERR(ca_usb_phy->port0_vbus_supply)) {
		ret = PTR_ERR(ca_usb_phy->port0_vbus_supply);
		if (ret != -ENODEV)
			return ret;
		ca_usb_phy->port0_vbus_supply = NULL;
	}
	
	if (ca_usb_phy->port0_vbus_supply == NULL) {
		ca_usb_phy->u3port0_vbus =
			devm_gpiod_get_optional(dev, "u3port0-vbus",
					GPIOD_OUT_HIGH);
		if (IS_ERR(ca_usb_phy->u3port0_vbus))
			dev_info(dev, "U3port0 VBUS GPIO Get Failed / Just ignored\n");
		else
			dev_info(dev, "U3port0 VBUS GPIO Get OK / u3port0_vbus:%d\n",
					gpiod_get_value_cansleep(ca_usb_phy->u3port0_vbus));
	}

	ca_usb_phy->port1_vbus_supply =
		devm_regulator_get_optional(dev, "port1-vbus");
	if (IS_ERR(ca_usb_phy->port1_vbus_supply)) {
		ret = PTR_ERR(ca_usb_phy->port1_vbus_supply);
		if (ret != -ENODEV)
			return ret;
		ca_usb_phy->port1_vbus_supply = NULL;
	}

	if (ca_usb_phy->port1_vbus_supply == NULL) {
		ca_usb_phy->u3port1_vbus =
			devm_gpiod_get_optional(dev, "u3port1-vbus",
					GPIOD_OUT_HIGH);
		if (IS_ERR(ca_usb_phy->u3port1_vbus))
			dev_info(dev, "U3port1 VBUS GPIO Get Failed / Just ignored\n");
		else
			dev_info(dev, "U3port1 VBUS GPIO Get OK / u3port1_vbus:%d\n",
					gpiod_get_value_cansleep(ca_usb_phy->u3port1_vbus));
	}
#endif

#if defined(CONFIG_ARCH_CA_G3) || defined(CONFIG_ARCH_CA_G3HGU)
	ca_usb_phy->phy_vbus =
		devm_gpiod_get_optional(dev, "phy-vbus", GPIOD_OUT_LOW);
	if (IS_ERR(ca_usb_phy->phy_vbus))
		dev_info(dev, "U3 PHY VBUS GPIO Get Failed / Just ignored\n");
	else
		dev_info(dev, "U3 PHY VBUS GPIO Get OK / phy_vbus:%d\n",
			 gpiod_get_value(ca_usb_phy->phy_vbus));
#endif

	ca_usb_phy->dev = dev;
	ca_usb_phy->phy.dev = ca_usb_phy->dev;
	ca_usb_phy->phy.label = CA_USB3PHY_NAME;
	ca_usb_phy->phy.init = ca_usb3_phy_init;
	ca_usb_phy->phy.set_vbus = ca_usb3_phy_set_vbus;
	ca_usb_phy->phy.type = USB_PHY_TYPE_USB3;

	spin_lock_init(&ca_usb_phy->lock);
	phy_data = devm_kzalloc(dev, sizeof(*phy_data), GFP_KERNEL);
	if (!phy_data)
		return -ENOMEM;

	if (node) {
		ca_usb_phy->s2usb_dphy_reset =
				of_reset_control_get(node, "s2u3_dphy_reset");
		if (IS_ERR(ca_usb_phy->s2usb_dphy_reset)) {
			ret = PTR_ERR(ca_usb_phy->s2usb_dphy_reset);
			dev_err(dev, "Failed to get s2usb_dphy_reset.\n");
			return ret;
		}
		ca_usb_phy->s2_phy = devm_phy_get(dev, "s2u3-phy");
		if (IS_ERR(ca_usb_phy->s2_phy)) {
			ret = PTR_ERR(ca_usb_phy->s2_phy);
			dev_err(dev, "Failed to get s2usb-phy.\n");
			return ret;
		}

		ca_usb_phy->s3usb_dphy_reset =
				of_reset_control_get(node, "s3u3_dphy_reset");
		if (IS_ERR(ca_usb_phy->s3usb_dphy_reset)) {
			ret = PTR_ERR(ca_usb_phy->s3usb_dphy_reset);
			dev_err(dev, "Failed to get s3usb_dphy_reset.\n");
			return ret;
		}
		ca_usb_phy->s3_phy = devm_phy_get(dev, "s3u3-phy");
		if (IS_ERR(ca_usb_phy->s3_phy)) {
			ret = PTR_ERR(ca_usb_phy->s3_phy);
			dev_err(dev, "Failed to get s3usb-phy.\n");
			return ret;
		}

		ret = of_property_read_u32_index(node, "u2portmask", 0,
						 &ca_usb_phy->u2port_mask);
		if (ret)
			goto err;
		dev_info(dev, "USB2 setting u2portmask = %d\n",
			 ca_usb_phy->u2port_mask);
		ret = of_property_read_u32_index(node, "u3portmask", 0,
						 &ca_usb_phy->u3port_mask);
		if (ret)
			goto err;
		dev_info(dev, "USB3 setting u3portmask = %d\n",
			 ca_usb_phy->u3port_mask);

		ret = of_property_read_u32_index(node, "phy_data_size", 0,
						 &phy_data->size);
		if (ret)
			goto err;
		phy_data->addr = devm_kzalloc(dev, sizeof(u8) * phy_data->size,
					      GFP_KERNEL);
		if (!phy_data->addr)
			return -ENOMEM;
		phy_data->data = devm_kzalloc(dev, sizeof(u16) * phy_data->size,
					      GFP_KERNEL);
		if (!phy_data->data)
			return -ENOMEM;
		ret = of_property_read_u8_array(node, "phy_data_addr",
						phy_data->addr, phy_data->size);
		if (ret)
			goto err;
		ret = of_property_read_u16_array(node, "phy_data_array",
						 phy_data->data,
						 phy_data->size);
		if (ret)
			goto err;
		ca_usb_phy->phy_data = phy_data;
	}

	/* Enable S2 for USB3.0 port0 */
	if (ca_usb_phy->u3port_mask & 0x1) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "s2_u3port0");
		ca_usb_phy->p0_regbase = devm_ioremap_resource(dev, res);
		if (IS_ERR(ca_usb_phy->p0_regbase))
			return PTR_ERR(ca_usb_phy->p0_regbase);
		dev_info(dev,
			 "Enabled S2/port0 resource - %pr mapped at 0x%pK\n",
			 res, ca_usb_phy->p0_regbase);
	}

	/* Enable S3 for USB3.0 port1 */
	if (ca_usb_phy->u3port_mask & 0x2) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "s3_u3port1");
		ca_usb_phy->p1_regbase = devm_ioremap_resource(dev, res);
		if (IS_ERR(ca_usb_phy->p1_regbase))
			return PTR_ERR(ca_usb_phy->p1_regbase);
		dev_info(dev,
			 "Enabled S3/port1 resource - %pr mapped at 0x%pK\n",
			 res, ca_usb_phy->p1_regbase);
	}

	platform_set_drvdata(pdev, ca_usb_phy);
	ret = usb_add_phy_dev(&ca_usb_phy->phy);
	if (ret)
		goto err;

	/* dphy reset and AUX bus reset */
	ca_usb3_host_reset(ca_usb_phy);

err:
	return ret;
}

static int ca_usb3_phy_remove(struct platform_device *pdev)
{
	struct ca_usb3_phy *ca_usb_phy = platform_get_drvdata(pdev);

	usb_remove_phy(&ca_usb_phy->phy);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id usbphy_ca_dt_match[] = {
	{ .compatible = "cortina-access,ca8289-u3phy", },
	{},
};
MODULE_DEVICE_TABLE(of, usbphy_ca_dt_match);
#endif

static struct platform_driver ca_usb3_phy_driver = {
	.probe = ca_usb3_phy_probe,
	.remove = ca_usb3_phy_remove,
	.driver = {
		.name = CA_USB3PHY_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(usbphy_ca_dt_match),
	},
};
module_platform_driver(ca_usb3_phy_driver);

MODULE_DESCRIPTION("Cortina-Access USB3port phy driver");
MODULE_ALIAS("platform:" CA_USB3PHY_NAME);
MODULE_LICENSE("GPL");
