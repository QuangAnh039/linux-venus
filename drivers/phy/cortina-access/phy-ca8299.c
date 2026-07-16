// SPDX-License-Identifier: GPL-2.0
/*
 * PHY driver for Cortina-Access CA8299
 *
 * Copyright (C) 2024 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 * Based on phy-bcm-cygnus-pcie.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <dt-bindings/phy/phy-ca8299.h>
#ifdef CONFIG_ARCH_CORTINA_SMC
#include <soc/cortina/cortina-smc.h>
#endif

/* GLOBAL_PHY_CONTROL_A (0xf43200b8) */
#define	PHY_CONTROL_A				0x00
#define	S0_PIPE_PHYMODE(x)			(((x) & 0x3) << 0)
#define	S1_PIPE_PHYMODE(x)			(((x) & 0x3) << 2)
#define	S2_PCIE_PIPE_PHYMODE(x)		(((x) & 0x3) << 4)
#define	S2_PCIE_USB3_SFI_SEL		BIT(6)
#define	S2_PCIE_USB3_SEL			BIT(7)
#define	S7_PCIE_USB3_SGMII_SEL(x)	(((x) & 0x3) << 8)
#define	S7_PCIE_USB3_SEL			BIT(10)
#define	S8_PCIE_USB3_SGMII_SEL(x)	(((x) & 0x3) << 11)
#define	S8_PCIE_USB3_SEL			BIT(13)
#define	S0_P_MDIO_ENABLE_REG		BIT(17)
#define	S1_P_MDIO_ENABLE_REG		BIT(18)
#define	S2_P_MDIO_ENABLE_REG		BIT(19)
#define	S6_P_MDIO_ENABLE_REG		BIT(20)
#define	S7_P_MDIO_ENABLE_REG		BIT(21)
#define	S8_P_MDIO_ENABLE_REG		BIT(22)
#define	S9_P_MDIO_ENABLE_REG		BIT(23)

#define	PHY_CONTROL_B				0x04
#define	CFG_XFI0_10G				BIT(0)
#define	CFG_XFI1_10G				BIT(1)
#define	USB2_LPM_PLL_ALIVE_0		BIT(2)
#define	USB2_REF_SEL_0(x)			(((x) & 0xF) << 3)
#define	USB2_HSTXVM_0				BIT(7)
#define	USB2_UA09PC_EN_0			BIT(8)
#define	USB2_UA18PC_EN_0			BIT(9)
#define	USB2_UA33PC_EN_0			BIT(10)
#define	USB2_UD09PC_EN_0			BIT(11)
#define	USB2PHY_P0_FORCE_SLB		BIT(12)
#define	USB2PHY_P0_SLB_HS			BIT(13)
#define	USB2_LPM_PLL_ALIVE_1		BIT(16)
#define	USB2_REF_SEL_1(x)			(((x) & 0xF) << 17)
#define	USB2_HSTXVM_1				BIT(21)
#define	USB2_UA09PC_EN_1			BIT(22)
#define	USB2_UA18PC_EN_1			BIT(23)
#define	USB2_UA33PC_EN_1			BIT(24)
#define	USB2_UD09PC_EN_1			BIT(25)
#define	USB2PHY_P1_FORCE_SLB		BIT(26)
#define	USB2PHY_P1_SLB_HS			BIT(27)

#define	PHY_MISC_CONTROL			0x08
#define	S0_PCIE_RX50_LINK			BIT(0)
#define	S1_PCIE_RX50_LINK			BIT(1)
#define	S2_USB3_BG_EN				BIT(2)
#define	S2_USB3_MBIAS_EN			BIT(3)
#define	S2_USB3_OOBS_PDB			BIT(4)
#define	S2_PCIE_RX50_LINK			BIT(5)
#define	S2_USB3_RX50_LINK			BIT(6)
#define	S2_PLL_DOWN_FROM_EMAC		BIT(7)
#define	S6_RX_OOBS_RSTB_S0			BIT(8)
#define	S7_USB3_BG_EN				BIT(10)
#define	S7_USB3_MBIAS_EN			BIT(11)
#define	S7_USB3_OOBS_PDB			BIT(12)
#define	S8_USB3_BG_EN				BIT(13)
#define	S8_USB3_MBIAS_EN			BIT(14)
#define	S8_USB3_OOBS_PDB			BIT(15)

#define	PHY_STATUS					0x0c
#define	STATUS_CKUSABLE_USB2_0			BIT(0)
#define	STATUS_CKUSABLE_USB2_1			BIT(1)
#define	STATUS_S0_PCIE_PCLK_RDY			BIT(2)
#define	STATUS_S0_EPHY_MAC_ISR			BIT(3)
#define	STATUS_S1_PCIE_PCLK_RDY			BIT(4)
#define	STATUS_S1_EPHY_MAC_ISR			BIT(5)
#define	STATUS_S2_CKUSABLE				BIT(6)
#define	STATUS_S2_PCIE_PCLK_RDY			BIT(7)
#define	STATUS_S2_PCIE_EPHY_MAC_ISR		BIT(8)
#define	STATUS_S2_EPHY_PCIE_RXERR		BIT(9)
#define	STATUS_S6_RX_OOBS_INV_S0		BIT(10)
#define	STATUS_S6_RXIDLE_S0				BIT(12)
#define	STATUS_S6_CMU_CK_25M_DIG		BIT(14)
#define	STATUS_S7_CKUSABLE				BIT(15)
#define	STATUS_S8_CKUSABLE				BIT(16)

#define	USB2PHY0_TEST				0x10
#define	USB2PHY_P0_SLB_ERR_NUM		GENMASK(15, 0)
#define	USB2PHY_P0_SLB_DONE			BIT(16)
#define	USB2PHY_P0_SLB_FAIL			BIT(17)

#define	USB2PHY1_TEST				0x14
#define	USB2PHY_P1_SLB_ERR_NUM		GENMASK(15, 0)
#define	USB2PHY_P1_SLB_DONE			BIT(16)
#define	USB2PHY_P1_SLB_FAIL			BIT(17)

#define	PHY_SGMII_MISC_CONTROL		0x94

/* GLOBAL_PHY_ISO_POWER_CTRL (0xf43203e0) */
#define	PHY_ISO_POWER_CTRL	0x00
#define	S2_POW_PCIE			BIT(0)
#define	S2_POW_USB3			BIT(1)
#define	S2_CPHY_ISOLATE		BIT(2)
#define	S6_POW_USB31		BIT(4)
#define	S7_POW_PCIE			BIT(5)
#define	S7_POW_USB3			BIT(6)
#define	S7_CPHY_ISOLATE		BIT(7)
#define	S7_ISO_ANA_B_V09	BIT(8)
#define	S8_POW_PCIE			BIT(9)
#define	S8_POW_USB3			BIT(10)
#define	S8_CPHY_ISOLATE		BIT(11)
#define	S8_ISO_ANA_B_V09	BIT(12)

static int debug;
struct ca8299_phy_core;
struct serdes_cfg {
	u32 addr;
	u32 val;
};

/**
 * struct ca8299_phy
 * @core: pointer to the PHY core control
 * @id: internal ID to identify the PHY
 * @phy: pointer to the kernel PHY device
 */
struct ca8299_phy {
	struct ca8299_phy_core *core;
	struct phy *phy;
	char *dts_name;
	//phys_addr_t serdes_addr, serdes_size;
	void __iomem *serdes_base;
	struct serdes_cfg *cfg;
	int cfg_cnt;
	int id;
};

/**
 * struct ca8299_phy_core - PHY core control
 * @dev: pointer to device
 * @base: base register
 * @lock: mutex to protect access to individual PHYs
 * @phys: pointer to PHY device
 */
struct ca8299_phy_core {
	struct device *dev;
	void __iomem *ctrl_base;
	void __iomem *pwr_base;
	struct mutex lock; //mutex lock flag
	struct ca8299_phy phys[PHY_NUMS];
};

static int ca8299_phy_ctrla_set(struct ca8299_phy *phy, bool enable)
{
	struct ca8299_phy_core *core = phy->core;
	u32 ctrl_val, set_mask, clr_mask;

	mutex_lock(&core->lock);
	switch (phy->id) {
	case PHY_S0_PCIE:
		set_mask = 0x0;
		clr_mask = S0_PIPE_PHYMODE(3);
		break;
	case PHY_S0_SFI:
		set_mask = S0_PIPE_PHYMODE(3);
		clr_mask = 0x0;
		break;
	case PHY_S1_PCIE:
		set_mask = 0x0;
		clr_mask = S1_PIPE_PHYMODE(3);
		break;
	case PHY_S1_SFI:
		set_mask = S1_PIPE_PHYMODE(3);
		clr_mask = 0x0;
		break;
	case PHY_S2_PCIE:
		set_mask = 0x0;
		clr_mask = S2_PCIE_PIPE_PHYMODE(3) | S2_PCIE_USB3_SFI_SEL
					| S2_PCIE_USB3_SEL;
		break;
	case PHY_S2_SFI:
		set_mask = S2_PCIE_PIPE_PHYMODE(3) | S2_PCIE_USB3_SFI_SEL;
		clr_mask = 0x0;
		break;
	case PHY_S2_USB:
		set_mask = S2_PCIE_USB3_SEL;
		clr_mask = S2_PCIE_PIPE_PHYMODE(3) | S2_PCIE_USB3_SFI_SEL;
		break;
	case PHY_S6_USB:
		set_mask = 0x0;
		clr_mask = 0x0;
		break;
	case PHY_S7_PCIE:
		set_mask = 0x0;
		clr_mask = S7_PCIE_USB3_SGMII_SEL(3) | S7_PCIE_USB3_SEL;
		break;
	case PHY_S7_USB:
		set_mask = S7_PCIE_USB3_SEL;
		clr_mask = S7_PCIE_USB3_SGMII_SEL(3);
		break;
	case PHY_S7_SGMII:
		set_mask = S7_PCIE_USB3_SGMII_SEL(1);
		clr_mask = 0x0;
		break;
	case PHY_S7_HISGMII:
		set_mask = S7_PCIE_USB3_SGMII_SEL(2);
		clr_mask = 0x0;
		break;
	case PHY_S8_PCIE:
		set_mask = 0x0;
		clr_mask = S8_PCIE_USB3_SGMII_SEL(3) | S8_PCIE_USB3_SEL;
		break;
	case PHY_S8_USB:
		set_mask = S8_PCIE_USB3_SEL | S8_P_MDIO_ENABLE_REG;
		clr_mask = S8_PCIE_USB3_SGMII_SEL(3);
		break;
	case PHY_S8_SGMII:
		set_mask = S8_PCIE_USB3_SGMII_SEL(1);
		clr_mask = 0x0;
		break;
	case PHY_S8_HISGMII:
		set_mask = S8_PCIE_USB3_SGMII_SEL(2);
		clr_mask = 0x0;
		break;
	default:
		break;
	}

	ctrl_val = readl(core->ctrl_base + PHY_CONTROL_A);
	if (debug)
		dev_info(core->dev, "%s - read ctrl: 0x%X\n",
			 __func__, ctrl_val);

	if (enable) {
		ctrl_val |= set_mask;
		ctrl_val &= ~clr_mask;
	} else {
		ctrl_val &= ~set_mask;
	}
	if (debug)
		dev_info(core->dev, "%s - write ctrl: 0x%X\n",
			 __func__, ctrl_val);

	writel(ctrl_val, core->ctrl_base + PHY_CONTROL_A);
	mutex_unlock(&core->lock);

	return 0;
}

static int ca8299_phy_misc_ctrl_set(struct ca8299_phy *phy, bool enable)
{
	struct ca8299_phy_core *core = phy->core;
	u32 misc_ctrl_val, set_mask, clr_mask;

	mutex_lock(&core->lock);
	switch (phy->id) {
	case PHY_S2_USB:
		set_mask |= S2_USB3_BG_EN;
		clr_mask = 0x0;
		break;
	case PHY_S8_USB:
		set_mask |= S8_USB3_BG_EN;
		clr_mask = 0x0;
		break;
	default:
		break;
	}

	misc_ctrl_val = readl(core->ctrl_base + PHY_MISC_CONTROL);
	if (enable) {
		misc_ctrl_val |= set_mask;
		misc_ctrl_val &= ~clr_mask;
	} else {
		misc_ctrl_val &= ~set_mask;
	}
	writel(misc_ctrl_val, core->ctrl_base + PHY_MISC_CONTROL);
	if (debug)
		dev_info(core->dev, "%s - write misc_ctrl_val: 0x%X\n",
			 __func__, misc_ctrl_val);

	mutex_unlock(&core->lock);

	return 0;
}

static int ca8299_phy_power_set(struct ca8299_phy *phy, bool enable)
{
	struct ca8299_phy_core *core = phy->core;
	u32 pwr_val, set_mask, clr_mask;

	mutex_lock(&core->lock);
	switch (phy->id) {
	case PHY_S2_PCIE:
		set_mask = S2_POW_PCIE;
		clr_mask = S2_POW_USB3 | S2_CPHY_ISOLATE;
		break;
	case PHY_S2_SFI:
		set_mask = 0x0;
		clr_mask = S2_POW_PCIE | S2_POW_USB3 | S2_CPHY_ISOLATE;
		break;
	case PHY_S2_USB:
		set_mask = S2_POW_USB3;
		clr_mask = S2_POW_PCIE | S2_CPHY_ISOLATE;
		break;
	case PHY_S6_USB:
		set_mask = S6_POW_USB31;
		clr_mask = 0x0;
		break;
	case PHY_S7_PCIE:
		set_mask = S7_POW_PCIE | S7_ISO_ANA_B_V09;
		clr_mask = S7_POW_USB3 | S7_CPHY_ISOLATE;
		break;
	case PHY_S7_USB:
		set_mask = S7_POW_USB3 | S7_ISO_ANA_B_V09;
		clr_mask = S7_POW_PCIE | S7_CPHY_ISOLATE;
		break;
	case PHY_S7_SGMII:
		set_mask = S7_ISO_ANA_B_V09;
		clr_mask = S7_POW_PCIE | S7_POW_USB3 | S7_CPHY_ISOLATE;
		break;
	case PHY_S7_HISGMII:
		set_mask = S7_ISO_ANA_B_V09;
		clr_mask = S7_POW_PCIE | S7_POW_USB3 | S7_CPHY_ISOLATE;
		break;
	case PHY_S8_PCIE:
		set_mask = S8_POW_PCIE | S8_ISO_ANA_B_V09;
		clr_mask = S8_POW_USB3 | S8_CPHY_ISOLATE;
		break;
	case PHY_S8_USB:
		set_mask = S8_POW_USB3 | S8_ISO_ANA_B_V09;
		clr_mask = S8_POW_PCIE | S8_CPHY_ISOLATE;
		break;
	case PHY_S8_SGMII:
		set_mask = S8_ISO_ANA_B_V09;
		clr_mask = S8_POW_PCIE | S8_POW_USB3 | S8_CPHY_ISOLATE;
		break;
	case PHY_S8_HISGMII:
		set_mask = S8_ISO_ANA_B_V09;
		clr_mask = S8_POW_PCIE | S8_POW_USB3 | S8_CPHY_ISOLATE;
		break;
	default:
		break;
	}

	pwr_val = readl(core->pwr_base + PHY_ISO_POWER_CTRL);
	if (debug)
		dev_info(core->dev, "%s - read pwr: 0x%X\n",
			 __func__, pwr_val);

	if (enable) {
		pwr_val |= set_mask;
		pwr_val &= ~clr_mask;
	} else {
		pwr_val &= ~set_mask;
	}
	if (debug)
		dev_info(core->dev, "%s - write pwr: 0x%X\n",
			 __func__, pwr_val);

	writel(pwr_val, core->pwr_base + PHY_ISO_POWER_CTRL);
	mutex_unlock(&core->lock);

	return 0;
}

static int ca8299_phy_config(struct ca8299_phy *phy, bool enable)
{
	struct ca8299_phy_core *core = phy->core;

	if (!(ca8299_phy_ctrla_set(phy, enable) ||
	      ca8299_phy_misc_ctrl_set(phy, enable) ||
	      ca8299_phy_power_set(phy, enable))) {
		dev_dbg(core->dev, "PHY %d %s\n", phy->id,
			enable ? "enabled" : "disabled");
		return 0;
	}

	return -1;
}

static int ca8299_phy_power_on(struct phy *p)
{
	struct ca8299_phy *phy = phy_get_drvdata(p);

	if (debug)
		dev_info(phy->core->dev, "Request to power_on phy(%d)\n",
			 phy->id);

	return ca8299_phy_config(phy, true);
}

static int ca8299_phy_power_off(struct phy *p)
{
	struct ca8299_phy *phy = phy_get_drvdata(p);

	if (debug)
		dev_info(phy->core->dev, "Request to power_off phy(%d)\n",
			 phy->id);

	return ca8299_phy_config(phy, false);
}

static const struct phy_ops ca8299_phy_ops = {
	.init = NULL,
	.exit = NULL,
	.power_on = ca8299_phy_power_on,
	.power_off = ca8299_phy_power_off,
	.owner = THIS_MODULE,
};

static int ca8299_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node, *child;
	struct ca8299_phy_core *core;
	struct phy_provider *provider;
	struct resource *res;
	unsigned int cnt = 0;
	int ret;

	if (of_get_child_count(node) == 0) {
		dev_err(dev, "PHY no child node\n");
		return -ENODEV;
	}

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;
	core->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "glb_phy_ctrl");
	core->ctrl_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(core->ctrl_base))
		return PTR_ERR(core->ctrl_base);
	dev_info(dev, "ctrl_base resource - %pr mapped at 0x%pK\n",
		 res, core->ctrl_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "glb_phy_pwr");
	core->pwr_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(core->pwr_base))
		return PTR_ERR(core->pwr_base);
	dev_info(dev, "pwr_base resource - %pr mapped at 0x%pK\n",
		 res, core->pwr_base);

	mutex_init(&core->lock);

	for_each_available_child_of_node(node, child) {
		unsigned int id;
		struct ca8299_phy *p;

		if (of_property_read_u32(child, "reg", &id)) {
			dev_err(dev, "missing reg property for %s\n",
				child->name);
			ret = -EINVAL;
			goto put_child;
		}
		if (id >= PHY_NUMS) {
			dev_err(dev, "invalid PHY id: %u\n", id);
			ret = -EINVAL;
			goto put_child;
		}
		if (core->phys[id].phy) {
			dev_err(dev, "duplicated PHY id: %u\n", id);
			ret = -EINVAL;
			goto put_child;
		}
		p = &core->phys[id];
		p->phy = devm_phy_create(dev, child, &ca8299_phy_ops);
		if (IS_ERR(p->phy)) {
			dev_err(dev, "failed to create PHY\n");
			ret = PTR_ERR(p->phy);
			goto put_child;
		}
		p->core = core;
		p->id = id;

		phy_set_drvdata(p->phy, p);
		cnt++;
	}
	dev_set_drvdata(dev, core);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "failed to register PHY provider\n");
		return PTR_ERR(provider);
	}
	dev_dbg(dev, "registered %u PHY(s)\n", cnt);

	return 0;

put_child:
	of_node_put(child);
	return ret;
}

static const struct of_device_id ca8299_phy_match_table[] = {
	{ .compatible = "cortina-access,ca8299-phy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ca8299_phy_match_table);

static struct platform_driver ca8299_phy_driver = {
	.driver = {
		.name = "ca8299-phy",
		.of_match_table = ca8299_phy_match_table,
	},
	.probe = ca8299_phy_probe,
};
module_platform_driver(ca8299_phy_driver);

MODULE_DESCRIPTION("Cortina-Access CA8299 PHY driver");
MODULE_LICENSE("GPL");
