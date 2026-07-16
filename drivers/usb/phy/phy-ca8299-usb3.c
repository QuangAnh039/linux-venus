// SPDX-License-Identifier: GPL-2.0+
/*
 * Physical USB3 & USB2 PHY Controller Driver on
 * Cortina-Access CA8299 SoC.
 *
 * Copyright (C) 2024 Cortina Access, Inc.
 *		 http://www.cortina-access.com
 *
 * Based on phy-cortina-usb3.c & phy-cortina-usb2.c
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
#include <linux/usb/ch11.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/reset.h>

#define CA_USB3PHY_NAME "ca8299-usb3phy"

/* U3 register offset */
#define USBCFG_USB_TOP_CTRL0			0x00
#define USB3_P2_VAUX_RESETN				BIT(20)
#define USB3_P2_VAUX_RESETN_PORT		BIT(16)
#define USB31_P1_VAUX_RESETN			BIT(12)
#define USB31_P1_VAUX_RESETN_PORT		BIT(8)
#define USB31_P0_VAUX_RESETN			BIT(4)
#define USB31_P0_VAUX_RESETN_PORT		BIT(0)

#define USBCFG_USB_TOP_CTRL1			0x04
#define R_SSUS_2						BIT(20)
#define USB3_P2_SUSPEND_CTRL_MSK(x)		((0x5 & (x)) << 16)
#define R_SSUS_1						BIT(12)
#define USB31_P1_SUSPEND_CTRL_MSK(x)	((0x3 & (x)) << 8)
#define R_SSUS_0						BIT(12)
#define USB31_P0_SUSPEND_CTRL_MSK(x)	(0x3 & (x))

#define USBCFG_U31_P0_CTRL					0x10
#define USBCFG_U31_P0_STS					0x18
#define USBCFG_U31_P0_PIPE_BIST_ERROR_CNT	0x24
#define USBCFG_U31_P0_PIPE_BIST_CORRECT_CNT	0x28
#define USBCFG_U31_P0_LTSSM_INT				0x2c
#define USBCFG_U31_P0_SUBLTSSM_INT0			0x30
#define USBCFG_U31_P0_SUBLTSSM_INT1			0x34
#define USBCFG_U31_P0_MISC0					0x38
#define USBCFG_U31_P0_MISC1					0x3c

#define USBCFG_U31_P1_CTRL					0x40
#define USBCFG_U31_P1_STS					0x48
#define USBCFG_U31_P1_PIPE_BIST_ERROR_CNT	0x54
#define USBCFG_U31_P1_PIPE_BIST_CORRECT_CNT	0x58
#define USBCFG_U31_P1_LTSSM_INT				0x5c
#define USBCFG_U31_P1_SUBLTSSM_INT0			0x60
#define USBCFG_U31_P1_SUBLTSSM_INT1			0x64
#define USBCFG_U31_P1_MISC0					0x68
#define USBCFG_U31_P1_MISC1					0x6c

#define USBCFG_U3_P2_CTRL					0x80
#define USBCFG_U3_P2_STS					0x88
#define USBCFG_U3_P2_PIPE_BIST_ERROR_CNT	0x94
#define USBCFG_U3_P2_PIPE_BIST_CORRECT_CNT	0x98
#define USBCFG_U3_P2_LTSSM_INT				0x9c

#define USBCFG_U31_P0_CFG					0x14
#define USBCFG_U31_P1_CFG					0x44
#define USBCFG_U3_P2_CFG					0x84
#define USB3_PX_HOST_U3P_DISABLE			BIT(12)
#define USB3_PX_HOST_U2P_DISABLE			BIT(8)

#define USBCFG_U31_P0_EXT_CTRL				0x20
#define USBCFG_U31_P1_EXT_CTRL				0x50
#define USBCFG_U3_P2_EXT_CTRL				0x90
#define USB3_PORT_TERM_SEL					BIT(28)

#define USBCFG_U3PHY_P0_CFG				0x100
#define USBCFG_U3PHY_P1_CFG				0x10c
#define USBCFG_U3PHY_P2_CFG				0x118
#define U3PHY_PORT_CKBUF_EN				BIT(31)
#define U3PHY_PORT_PIPE_LANE_MODE		BIT(30)
#define U3PHY_PORT_PIPE_LANE_CFG		BIT(29)
#define U3PHY_PORT_LANE01_MUX_SEL		BIT(28)
#define U3PHY_PORT_PIPE_PCLKEN			BIT(27)
#define U3PHY_PORT_CLK_MODE_SEL_MSK(x)	(0x3 & (x))

#define USBCFG_U3PHY_P0_STS0			0x104
#define USBCFG_U3PHY_P1_STS0			0x110
#define USBCFG_U3PHY_P2_STS0			0x11c
#define U3PHY_PORT_MAC_RECV_DET_END		BIT(21)
#define U3PHY_PORT_MAC_RECV_DET_ACK		BIT(20)
#define U3PHY_PORT_CLK_RDY				BIT(19)

#define USBCFG_U3PHY_P0_STS1				0x108
#define USBCFG_U3PHY_P1_STS1				0x114
#define USBCFG_U3PHY_P2_STS1				0x120
#define U3PHY_PORT_DISPARITY_ERR_MSK(x)		((0xff & (x)) << 24)
#define U3PHY_PORT_ELASTIC_BUF_UDF_MSK(x)	((0xff & (x)) << 16)
#define U3PHY_PORT_ELASTIC_BUF_OVF_MSK(x)	((0xff & (x)) << 8)
#define U3PHY_PORT_DECODE_ERR_MASK(x)		(0xff & (x))

/* U2 register offset */
#define USBCFG_U2_P0_EXT_CTRL			0x1c
#define USBCFG_U2_P1_EXT_CTRL			0x4c
#define USBCFG_U2_P2_EXT_CTRL			0x8c
#define R_FTERMSEL_0					BIT(3)
#define R_STERMSEL_0					BIT(2)
#define R_FDISCON_0						BIT(1)
#define R_SDISCON_0						BIT(0)

#define USBCFG_U2PHY_P0_CFG				0x130
#define USBCFG_U2PHY_P1_CFG				0x138
#define USBCFG_U2PHY_P2_CFG				0x140
#define U2PHY_PORT_VSTATUS_IN_MSK(x)	((0xff & (x)) << 24)
#define U2PHY_PORT_VCNTRL_MSK(x)		((0xf & (x)) << 20)
#define U2PHY_PORT_VLOADM				BIT(19)
#define U2PHY_PORT_BY_PASS_ON			BIT(18)
#define U2PHY_PORT_REG_LDO_PW			BIT(10)
#define U2PHY_PORT_LF_PD_R_EN			BIT(9)
#define U2PHY_PORT_CLKTSTEN				BIT(8)
#define U2PHY_PORT_DPPULLDOWN			BIT(6)
#define U2PHY_PORT_DMPULLDOWN			BIT(5)
#define U2PHY_PORT_TXBITSTUFF_EN		BIT(4)
#define U2PHY_PORT_TXBITSTUFF_ENH		BIT(3)
#define U2PHY_PORT_TX_EN_N				BIT(2)
#define U2PHY_PORT_TX_DAT				BIT(1)
#define U2PHY_PORT_TX_SE0				BIT(0)

#define USBCFG_U2PHY_P0_STS				0x134
#define USBCFG_U2PHY_P1_STS				0x13c
#define USBCFG_U2PHY_P2_STS				0x144
#define U2PHY_PORT_VSTATUS_OUT_MSK(x)	((0xff & (x)) << 24)
#define U2PHY_PORT_DEBUG_MSK(x)			(0xff & (x))

#define U2_TYPE		0
#define U3_TYPE		1
#define USB_TYPE_MAX	2
#define USB_PORT_MAX	3

static const char *port_name[USB_PORT_MAX] = {
	"u3s2_port0",
	"u3s6_port1",
	"u3s8_port2"
};

static const char *reset_name[USB_PORT_MAX] = {
	"u3s2_reset",
	"u3s6_reset",
	"u3s8_reset"
};

static const char *phy_name[USB_PORT_MAX] = {
	"u3s2_phy",
	"u3s6_phy",
	"u3s8_phy"
};

static const char *phydata_name[USB_TYPE_MAX][3] = {
	{"u2phy_data_size", "u2phy_data_addr", "u2phy_data_array"},
	{"u3phy_data_size", "u3phy_data_addr", "u3phy_data_array"}
};

u32 u2_ext_ctrl[USB_PORT_MAX] = {
	USBCFG_U2_P0_EXT_CTRL,
	USBCFG_U2_P1_EXT_CTRL,
	USBCFG_U2_P2_EXT_CTRL
};

u32 u2_cfg[USB_PORT_MAX] = {
	USBCFG_U2PHY_P0_CFG,
	USBCFG_U2PHY_P1_CFG,
	USBCFG_U2PHY_P2_CFG
};

u32 u3_ext_ctrl[USB_PORT_MAX] = {
	USBCFG_U31_P0_EXT_CTRL,
	USBCFG_U31_P1_EXT_CTRL,
	USBCFG_U3_P2_EXT_CTRL
};

u32 u3_cfg[USB_PORT_MAX] = {
	USBCFG_U31_P0_CFG,
	USBCFG_U31_P1_CFG,
	USBCFG_U3_P2_CFG
};

u32 usb_rst_flag[USB_PORT_MAX] = {
	(USB31_P0_VAUX_RESETN | USB31_P0_VAUX_RESETN_PORT),
	(USB31_P1_VAUX_RESETN | USB31_P1_VAUX_RESETN_PORT),
	(USB3_P2_VAUX_RESETN | USB3_P2_VAUX_RESETN_PORT)
};

struct phy_data {
	int size;
	u8 *u2addr;
	u8 *u2data;
	u16 *u3addr;
	u16 *u3data;
};

struct ca_usb3_phy {
	struct usb_phy phy;
	struct device *dev;
	void __iomem *host_regbase;
	/*
	 * Index  | Serde name
	 *   0       S2
	 *   1       S6
	 *   2       S8
	 */
	void __iomem *serdes_regbase[USB_PORT_MAX];
	struct reset_control *u3_reset[USB_PORT_MAX];
	struct phy *u3_phy[USB_PORT_MAX];
	/*
	 * Index  | USB type
	 *   0       U2
	 *   1       U3
	 */
	struct phy_data *phy_data[USB_TYPE_MAX];
	int u2port_mask;
	int u3port_mask;
	spinlock_t lock;
};

static inline u32 ca_usb3_phy_read(struct ca_usb3_phy *ca_phy,
				   u16 ofs, int port)
{
	return readl(ca_phy->serdes_regbase[port] + ofs);
}

static inline void ca_usb3_phy_write(struct ca_usb3_phy *ca_phy,
				     u16 ofs, u16 data, int port)
{
	writel(data, ca_phy->serdes_regbase[port] + ofs);
}

static void ca_usb3_host_reset(struct ca_usb3_phy *ca_phy)
{
	int u2port_mask = ca_phy->u2port_mask;
	int u3port_mask = ca_phy->u3port_mask;
	u32 reg_val;
	unsigned long flags;
	int port;

	/* usb3 block reset & phy power */
	for (port = 0; port < USB_PORT_MAX; port++) {
		if (u3port_mask & (0b1 << port)) {
			reset_control_assert(ca_phy->u3_reset[port]);
			msleep(20);
			phy_power_on(ca_phy->u3_phy[port]);
			msleep(20);
			reset_control_deassert(ca_phy->u3_reset[port]);
			msleep(20);
		}
	}

	/* For USB3 & USB2.0 PHY RESET */
	spin_lock_irqsave(&ca_phy->lock, flags);
	for (port = 0; port < USB_PORT_MAX; port++) {
		if (u3port_mask & (0b1 << port)) {
			/* For USB3 & USB2.0 MAC/PHY RESET */
			reg_val = readl(ca_phy->host_regbase + USBCFG_USB_TOP_CTRL0);
			reg_val |= usb_rst_flag[port];
			writel(reg_val, ca_phy->host_regbase + USBCFG_USB_TOP_CTRL0);
			dev_dbg(ca_phy->dev, "read USBCFG_USB_TOP_CTRL0 reg_val = 0x%08x",
				readl(ca_phy->host_regbase + USBCFG_USB_TOP_CTRL0));

			/* Release USB3 port term_sel */
			reg_val = readl(ca_phy->host_regbase + u3_ext_ctrl[port]);
			reg_val &= ~USB3_PORT_TERM_SEL;
			writel(reg_val, ca_phy->host_regbase + u3_ext_ctrl[port]);
			dev_dbg(ca_phy->dev, "read USBCFG_U3_P%d_EXT_CTRL reg_val = 0x%08x",
				port, readl(ca_phy->host_regbase + u3_ext_ctrl[port]));

			/* For USB3 PORT CONFIG */
			reg_val = readl(ca_phy->host_regbase + u3_cfg[port]);
			reg_val &= ~(USB3_PX_HOST_U2P_DISABLE | USB3_PX_HOST_U3P_DISABLE);
			writel(reg_val, ca_phy->host_regbase + u3_cfg[port]);
			dev_dbg(ca_phy->dev, "read USBCFG_U3_P%d_CFG reg_val = 0x%08x",
				port, readl(ca_phy->host_regbase + u3_cfg[port]));
		}
		if (u2port_mask & (0b1 << port)) {
			/* USB2 termsel disable force mode */
			reg_val = readl(ca_phy->host_regbase + u2_ext_ctrl[port]);
			reg_val &= ~R_FTERMSEL_0;
			writel(reg_val, ca_phy->host_regbase + u2_ext_ctrl[port]);
			dev_dbg(ca_phy->dev, "read USBCFG_U2_P%d_EXT_CTRL reg_val = 0x%08x",
				port, readl(ca_phy->host_regbase + u2_ext_ctrl[port]));
		}
	}
	spin_unlock_irqrestore(&ca_phy->lock, flags);
}

static void ca_usb2_phy_port_cfg(struct ca_usb3_phy *ca_phy,
				 u8 vstat_in_mask, u8 vctrl_mask, int port)
{
	u32 reg_val;
	u8 vctrl_mask1, vctrl_mask2;
	unsigned long flags;

	dev_dbg(ca_phy->dev, "u2data = 0x%08x, addr = 0x%08x\n",
		vstat_in_mask, vctrl_mask);

	spin_lock_irqsave(&ca_phy->lock, flags);

	vctrl_mask1 = vctrl_mask & 0x0F;
	vctrl_mask2 = (vctrl_mask >> 4) & 0x0F;
	reg_val = (U2PHY_PORT_VSTATUS_IN_MSK(vstat_in_mask)
		| U2PHY_PORT_VCNTRL_MSK(vctrl_mask1)
		| U2PHY_PORT_DPPULLDOWN
		| U2PHY_PORT_DMPULLDOWN
		| U2PHY_PORT_VLOADM
		| U2PHY_PORT_TX_EN_N);
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	reg_val &= ~U2PHY_PORT_VLOADM;
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	reg_val |= U2PHY_PORT_VLOADM;
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	/* Clear VCONTROL field before refilling */
	reg_val &= ~U2PHY_PORT_VCNTRL_MSK(0xF);
	reg_val |= U2PHY_PORT_VCNTRL_MSK(vctrl_mask2);
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	reg_val &= ~U2PHY_PORT_VLOADM;
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	reg_val |= U2PHY_PORT_VLOADM;
	writel(reg_val, ca_phy->host_regbase + u2_cfg[port]);
	dev_dbg(ca_phy->dev, "read USBCFG_U2PHY_P%d_CFG reg_val = 0x%08x",
		port, readl(ca_phy->host_regbase + u2_cfg[port]));

	spin_unlock_irqrestore(&ca_phy->lock, flags);
}

static int ca_usb_phy_init(struct usb_phy *phy)
{
	struct ca_usb3_phy *ca_phy = (struct ca_usb3_phy *)phy;
	int u2port_mask = ca_phy->u2port_mask;
	int u3port_mask = ca_phy->u3port_mask;
	int port, index;

	for (port = 0; port < USB_PORT_MAX; port++) {
		if (u3port_mask & (0b1 << port)) {
			/* Init USB3 PHY */
			for (index = 0;
				 index < ca_phy->phy_data[U3_TYPE]->size; index++) {
				ca_usb3_phy_write(ca_phy,
						ca_phy->phy_data[U3_TYPE]->u3addr[index],
						ca_phy->phy_data[U3_TYPE]->u3data[index],
						port);
				dev_dbg(ca_phy->dev, "u3port_%d ofs:%04x, d:0x%04x\n",
					port,
					ca_phy->phy_data[U3_TYPE]->u3addr[index],
					ca_usb3_phy_read(ca_phy,
							 ca_phy->phy_data[U3_TYPE]->u3addr[index],
							 port));
			}
			dev_dbg(ca_phy->dev, "u3port_%d init is ready!\n", port);
		}
		if (u2port_mask & (0b1 << port)) {
			/* Init USB2 PHY */
			for (index = 0;
				 index < ca_phy->phy_data[U2_TYPE]->size; index++) {
				ca_usb2_phy_port_cfg(ca_phy,
						*(ca_phy->phy_data[U2_TYPE]->u2data + index),
						*(ca_phy->phy_data[U2_TYPE]->u2addr + index),
						port);
			}
			dev_dbg(ca_phy->dev, "u2port_%d init is ready!\n", port);
		}
	}
	return 0;
}

static int ca_usb3_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct ca_usb3_phy *ca_usb_phy;
	struct phy_data *phydata[USB_TYPE_MAX];
	int port, type_id;

	ca_usb_phy = devm_kzalloc(dev, sizeof(*ca_usb_phy), GFP_KERNEL);
	if (!ca_usb_phy)
		return -ENOMEM;

	ca_usb_phy->host_regbase =
			devm_platform_ioremap_resource_byname(pdev, "u3host_reg");
	if (IS_ERR(ca_usb_phy->host_regbase))
		return PTR_ERR(ca_usb_phy->host_regbase);

	ca_usb_phy->dev = dev;
	ca_usb_phy->phy.dev = ca_usb_phy->dev;
	ca_usb_phy->phy.label = CA_USB3PHY_NAME;
	ca_usb_phy->phy.init = ca_usb_phy_init;
	ca_usb_phy->phy.type = USB_PHY_TYPE_USB3;

	spin_lock_init(&ca_usb_phy->lock);
	if (node) {
		/* get reset/phy_ctrl nodes from dts */
		for (port = 0; port < USB_PORT_MAX; port++) {
			ca_usb_phy->u3_reset[port] =
					of_reset_control_get(node, reset_name[port]);
			if (IS_ERR(ca_usb_phy->u3_reset[port]))
				return PTR_ERR(ca_usb_phy->u3_reset[port]);

			ca_usb_phy->u3_phy[port] =
					devm_phy_get(dev, phy_name[port]);
			if (IS_ERR(ca_usb_phy->u3_phy[port]))
				return PTR_ERR(ca_usb_phy->u3_phy[port]);
		}

		if (of_property_read_u32_index(node,
				"u2portmask", 0, &ca_usb_phy->u2port_mask))
			return -EINVAL;

		/* check port mask */
		if (ca_usb_phy->u2port_mask > 0b111) {
			dev_err(dev, "Invalid PHY u2mask (%d), Should be (0 .. 7)!\n",
				ca_usb_phy->u2port_mask);
			return -EINVAL;
		}
		dev_info(dev, "USB3 setting u2portmask = %d\n",
			 ca_usb_phy->u2port_mask);

		if (of_property_read_u32_index(node,
				"u3portmask", 0, &ca_usb_phy->u3port_mask))
			return -EINVAL;

		/* check port mask */
		if (ca_usb_phy->u3port_mask > 0b111) {
			dev_err(dev, "Invalid PHY u3mask (%d), Should be (0 .. 7)!\n",
				ca_usb_phy->u3port_mask);
			return -EINVAL;
		}
		dev_info(dev, "USB3 setting u3portmask = %d\n",
			 ca_usb_phy->u3port_mask);

		/* u2/u3 phy data/size/array get from dts */
		for (type_id = 0; type_id < USB_TYPE_MAX; type_id++) {
			phydata[type_id] =
				devm_kzalloc(dev, sizeof(struct phy_data), GFP_KERNEL);
			if (!phydata[type_id])
				return -ENOMEM;

			if (of_property_read_u32_index(node,
					phydata_name[type_id][0], 0, &phydata[type_id]->size))
				return -EINVAL;

			if (type_id == U2_TYPE) {
				phydata[type_id]->u2addr =
					devm_kzalloc(dev,
						     sizeof(u8) * phydata[type_id]->size,
						     GFP_KERNEL);
				if (!(phydata[type_id]->u2addr))
					return -ENOMEM;

				if (of_property_read_u8_array(node,
						phydata_name[type_id][1], phydata[type_id]->u2addr,
						phydata[type_id]->size))
					return -EINVAL;

				phydata[type_id]->u2data =
					devm_kzalloc(dev,
						     sizeof(u8) * phydata[type_id]->size,
						     GFP_KERNEL);
				if (!(phydata[type_id]->u2data))
					return -ENOMEM;

				if (of_property_read_u8_array(node,
						phydata_name[type_id][2], phydata[type_id]->u2data,
						phydata[type_id]->size))
					return -EINVAL;

			} else if (type_id == U3_TYPE) {
				phydata[type_id]->u3addr =
					devm_kzalloc(dev,
						     sizeof(u16) * phydata[type_id]->size,
						     GFP_KERNEL);
				if (!(phydata[type_id]->u3addr))
					return -ENOMEM;

				if (of_property_read_u16_array(node,
						phydata_name[type_id][1], phydata[type_id]->u3addr,
						phydata[type_id]->size))
					return -EINVAL;

				phydata[type_id]->u3data =
					devm_kzalloc(dev,
						     sizeof(u16) * phydata[type_id]->size,
						     GFP_KERNEL);
				if (!(phydata[type_id]->u3data))
					return -ENOMEM;

				if (of_property_read_u16_array(node,
						phydata_name[type_id][2], phydata[type_id]->u3data,
						phydata[type_id]->size))
					return -EINVAL;
			} else {
				dev_err(dev, "Invalid USB TYPE_ID (%d)!\n", type_id);
				return -EINVAL;
			}
		}
		memcpy(ca_usb_phy->phy_data, phydata, sizeof(ca_usb_phy->phy_data));
	}

	/* usb serdes get resource mapping from dts */
	for (port = 0; port < USB_PORT_MAX; port++) {
		if (ca_usb_phy->u3port_mask & (0b1 << port)) {
			ca_usb_phy->serdes_regbase[port] =
				devm_platform_ioremap_resource_byname(pdev, port_name[port]);
			if (IS_ERR(ca_usb_phy->serdes_regbase[port]))
				return PTR_ERR(ca_usb_phy->serdes_regbase[port]);
		}
	}

	platform_set_drvdata(pdev, ca_usb_phy);

	if (usb_add_phy_dev(&ca_usb_phy->phy))
		return -EINVAL;

	/* dphy reset and AUX bus reset */
	ca_usb3_host_reset(ca_usb_phy);

	return 0;
}

static int ca_usb3_phy_remove(struct platform_device *pdev)
{
	struct ca_usb3_phy *ca_usb_phy = platform_get_drvdata(pdev);
	int port;

	for (port = 0; port < USB_PORT_MAX; port++) {
		/* release u3_reset resource */
		if (ca_usb_phy->u3_reset[port]) {
			reset_control_put(ca_usb_phy->u3_reset[port]);
			ca_usb_phy->u3_reset[port] = NULL;
		}
		/* release u3_phy resource */
		if (ca_usb_phy->u3_phy[port]) {
			phy_put(&pdev->dev, ca_usb_phy->u3_phy[port]);
			ca_usb_phy->u3_phy[port] = NULL;
		}
	}
	usb_remove_phy(&ca_usb_phy->phy);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id usb3phy_ca_dt_match[] = {
	{ .compatible = "cortina-access,ca8299-u3phy", },
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
		.of_match_table = of_match_ptr(usb3phy_ca_dt_match),
	},
};
module_platform_driver(ca_usb3_phy_driver);

MODULE_DESCRIPTION("Cortina-Access CA8299 USB3 phy driver");
MODULE_ALIAS("platform:" CA_USB3PHY_NAME);
MODULE_LICENSE("GPL");
