// SPDX-License-Identifier: GPL-2.0
/*
 * PHY driver for Cortina-Access SoCs
 *
 * Copyright (C) 2016 Cortina Access, Inc.
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
#include <dt-bindings/phy/phy-ca8289.h>
#if defined(CONFIG_SATA_CORTINA_ACCESS)
#include <soc/cortina-access/ca-soc.h>
#endif
#ifdef CONFIG_CORTINA_ACCESS_SMCC
#include <soc/cortina-access/ca-smc.h>
#endif

#define PHY_CONTROL				0x00
#define	S0_PCIE2_ISOLATE_0		BIT(2)
#define	S0_POW_PCIE2_0			BIT(3)
#define	S0_PCIE2_ISOLATE_1		BIT(4)
#define	S0_POW_PCIE2_1			BIT(5)

#if defined(CONFIG_ARCH_CA_VENUS)
#define   S1_PCIE2_ISOLATE_0      BIT(6)
#define   S1_POW_PCIE2_0          BIT(7)
#define   S1_PCIE2_ISOLATE_1      BIT(8)
#define   S1_POW_PCIE2_1          BIT(9)
#define   S2_CPHY_ISOLATE         BIT(10)
#define   S2_POW_PCIE2            BIT(12)
#define   S2_ISO_ANA_B            BIT(11)
#define   S2_POW_USB3             BIT(13)
#define   S3_ISO_ANA_B            BIT(15)
#define   S3_POW_USB3             BIT(17)
#define   S3_POW_SGMII            BIT(18)
#define   USB2_ISOLATE            BIT(19)
#define   S2_COMBO_SEL            BIT(23)
#define   S3_COMBO_SEL            BIT(24)
#define   S3_SATA_SGMII_SEL(x)    ((0x3 & (x)) << 25)
#define   S0_RXAUI_MODE           BIT(27)
#elif defined(CONFIG_ARCH_CA_G3) || defined(CONFIG_ARCH_CA_G3HGU)
#define	S1_SATA_ISOLATE			BIT(6)
#define	S1_POW_SATA				BIT(7)
#define	S1_PCIE2_ISOLATE		BIT(8)
#define	S1_POW_PCIE2			BIT(9)
#define	S2_PCIE2_ISOLATE		BIT(10)
#define	S2_POW_PCIE2			BIT(12)
#define	S2_USB3_ISOLATE			BIT(11)
#define	S2_POW_USB3				BIT(13)
#define	S3_USB3_ISOLATE			BIT(14)
#define	S3_SATA_SGMII_ISOLATE	BIT(15)
#define	S3_POW_SATA				BIT(16)
#define	S3_POW_USB3				BIT(17)
#define	S1_COMBO_SEL			BIT(22)
#define	S2_COMBO_SEL			BIT(23)
#define	S3_COMBO_SEL			BIT(24)
#define	S3_SATA_SGMII_SEL		BIT(25)
#define	S0_RXAUI_MODE			BIT(26)
#define	SATA_SEL_PHY			BIT(27)
#endif

#define	S0_P_MDIO_ENABLE_REG	BIT(28)
#define	S1_P_MDIO_ENABLE_REG	BIT(29)
#define	S2_P_MDIO_ENABLE_REG	BIT(30)
#define	S3_P_MDIO_ENABLE_REG	BIT(31)
#define PHY_MISC_CONTROL		0x04
#define	S3_RX50_LINK			BIT(28)
#define	S1_RX50_LINK			BIT(27)
#define	S3_MBIAS_EN				BIT(11)
#define	S1_MBIAS_EN				BIT(1)
#define PHY_STATUS				0x08
#if defined(CONFIG_ARCH_CA_VENUS)
#define	STATUS_USB2_CKUSABLE	GENMASK(2, 0)
#define	STATUS_CKUSABLE_2		BIT(3)
#define	STATUS_CKUSABLE_3		BIT(4)
#define	STATUS_S0_CKUSABLE0		BIT(5)
#define	STATUS_S0_CKUSABLE1		BIT(6)
#define	STATUS_CKUSABLE_SGMII	BIT(7)
#else
#define	STATUS_USB2_CKUSABLE	BIT(0)
#define	STATUS_S1_CKUSABLE		BIT(1)
#define	STATUS_S2_CKUSABLE		BIT(2)
#define	STATUS_S3_CKUSABLE		BIT(3)
#define	STATUS_S0_CKUSABLE0		BIT(4)
#define	STATUS_S0_CKUSABLE1		BIT(5)
#define	STATUS_SATA0_PHYRDY		BIT(6)
#define	STATUS_SATA1_PHYRDY		BIT(7)
#endif
#define PHY_SGMII_MISC_CONTROL	0x0c
#define PHY_SGMII_PCS_CONTROL	0x10
#define PHY_SGMII_PCS_INFO		0x14
#define SGMII_PCS_INTERRUPT		0x18
#define SGMII_PCS_INTENABLE		0x1c

static int debug;
struct ca_phy_core;

struct serdes_cfg {
	u32 addr;
	u32 val;
};

/**
 * struct ca_phy
 * @core: pointer to the PHY core control
 * @id: internal ID to identify the PHY
 * @phy: pointer to the kernel PHY device
 */
struct ca_phy {
	struct ca_phy_core *core;
	struct phy *phy;
	char *dts_name;
	phys_addr_t serdes_addr, serdes_size;
	void __iomem *serdes_base;
	struct serdes_cfg *cfg;
	int cfg_cnt;
	int id;
};

/**
 * struct ca_phy_core - PHY core control
 * @dev: pointer to device
 * @base: base register
 * @lock: mutex to protect access to individual PHYs
 * @phys: pointer to PHY device
 */
struct ca_phy_core {
	struct device *dev;
	void __iomem *base;
	unsigned int paddr;
	struct mutex lock; /* mutex to protect access to individual PHYs */
	struct ca_phy phys[PHY_NUMS];
	u32 flag;
};

#define PHY_FORCE_ON	BIT(1)

static int ca_phy_sgmii_config(struct ca_phy *phy, bool enable)
{
	struct ca_phy_core *core = phy->core;

	if (!enable)
		return 0;

	switch (phy->id) {
	case PHY_S3_SGMII:
		writel(0x00000030, core->base + PHY_SGMII_MISC_CONTROL);
		writel(0x03600248, core->base + PHY_SGMII_PCS_CONTROL);
		break;
	}

	return 0;
}

#if defined(CONFIG_SATA_CORTINA_ACCESS)
static int ca_sata_config(struct ca_phy *phy, bool enable)
{
	struct ca_phy_core *core = phy->core;
	u32 val;

	if (!enable)
		return 0;

	switch (phy->id) {
	case PHY_S1_SATA:
#ifdef CONFIG_CORTINA_ACCESS_SMCC
		val = CA_SMC_CALL_REG_READ(core->paddr + PHY_MISC_CONTROL);
#else
		val = readl(core->base + PHY_MISC_CONTROL);
#endif
		val |= S1_MBIAS_EN | S1_RX50_LINK;
		val &= ~(S3_MBIAS_EN | S3_RX50_LINK);
#ifdef CONFIG_CORTINA_ACCESS_SMCC
		CA_SMC_CALL_REG_WRITE(core->paddr + PHY_MISC_CONTROL, val);
#else
		writel(val, core->base + PHY_MISC_CONTROL);
#endif
		break;
	case PHY_S3_SATA:
#ifdef CONFIG_CORTINA_ACCESS_SMCC
		val = CA_SMC_CALL_REG_READ(core->paddr + PHY_MISC_CONTROL);
#else
		val = readl(core->base + PHY_MISC_CONTROL);
#endif
		val |= S3_MBIAS_EN | S3_RX50_LINK;
		val &= ~(S1_MBIAS_EN | S1_RX50_LINK);
#ifdef CONFIG_CORTINA_ACCESS_SMCC
		CA_SMC_CALL_REG_WRITE(core->paddr + PHY_MISC_CONTROL, val);
#else
		writel(val, core->base + PHY_MISC_CONTROL);
#endif
		break;
	}
	return 0;
}
#endif

static int ca_phy_power_config(struct ca_phy *phy, bool enable)
{
	struct ca_phy_core *core = phy->core;
	u32 val, enable_set_mask, enable_clr_mask;

	switch (phy->id) {
	case PHY_S0_PCIE0:
		enable_set_mask = S0_POW_PCIE2_0 | S0_POW_PCIE2_1;
		enable_clr_mask = S0_PCIE2_ISOLATE_0 | S0_RXAUI_MODE |
				  S0_PCIE2_ISOLATE_1;
		break;
	case PHY_S0_PCIE1:
		enable_set_mask = S0_POW_PCIE2_1;
		enable_clr_mask = S0_PCIE2_ISOLATE_1;
		break;
#if defined(CONFIG_ARCH_CA_G3) || defined(CONFIG_ARCH_CA_G3HGU)
	case PHY_S1_PCIE0:
		enable_set_mask = S1_POW_PCIE2;
		enable_clr_mask = S1_PCIE2_ISOLATE | S1_COMBO_SEL;
		break;
	case PHY_S2_PCIE0:
		enable_set_mask = S2_POW_PCIE2;
		enable_clr_mask = S2_PCIE2_ISOLATE | S2_COMBO_SEL;
		break;
	case PHY_S2_USB:
		enable_set_mask = S2_POW_USB3 | S2_COMBO_SEL;
		enable_clr_mask = S2_USB3_ISOLATE;
		break;
	case PHY_S3_USB:
		enable_set_mask = S3_POW_USB3 | S3_COMBO_SEL;
		enable_clr_mask = S3_USB3_ISOLATE;
		break;
	case PHY_S3_SGMII:
		enable_set_mask = S3_POW_SATA;
		enable_clr_mask = S3_SATA_SGMII_ISOLATE | S3_SATA_SGMII_SEL;
		break;
#if defined(CONFIG_SATA_CORTINA_ACCESS)
	case PHY_S1_SATA:
		enable_set_mask = S1_COMBO_SEL | S1_POW_SATA | S0_RXAUI_MODE;
		enable_clr_mask = SATA_SEL_PHY | S3_POW_SATA;
		break;
	case PHY_S3_SATA:
		enable_set_mask = S3_SATA_SGMII_SEL | SATA_SEL_PHY |
				  S3_POW_SATA | S0_RXAUI_MODE;
		enable_clr_mask = S3_COMBO_SEL | S1_POW_SATA;
		break;
#endif
#endif
#if defined(CONFIG_ARCH_CA_VENUS)
	case PHY_S1_PCIE0:
		enable_set_mask = S1_POW_PCIE2_0 | S1_POW_PCIE2_1;
		enable_clr_mask = S1_PCIE2_ISOLATE_0 | S1_PCIE2_ISOLATE_1;
		break;
	case PHY_S1_PCIE1:
		enable_set_mask = S1_POW_PCIE2_1;
		enable_clr_mask = S1_PCIE2_ISOLATE_1;
		break;
	case PHY_S2_PCIE0:
		enable_set_mask = S2_POW_PCIE2;// | S2_ISO_ANA_B;
		enable_clr_mask = S2_POW_USB3 | S2_COMBO_SEL | S2_CPHY_ISOLATE;
		break;
	case PHY_S2_USB:
		enable_set_mask = S2_POW_USB3 | S2_COMBO_SEL;// | S2_ISO_ANA_B;
		enable_clr_mask = S2_POW_PCIE2 | S2_CPHY_ISOLATE;
		break;
	case PHY_S3_USB:
		enable_set_mask = S3_POW_USB3 | S3_COMBO_SEL;// | S3_ISO_ANA_B;
		enable_clr_mask = S3_POW_SGMII | S3_SATA_SGMII_SEL(3);
		break;
#endif
	default:
		dev_err(core->dev, "PHY %d invalid\n", phy->id);
		return -EINVAL;
	}

	mutex_lock(&core->lock);

#ifdef CONFIG_CORTINA_ACCESS_SMCC
	val = CA_SMC_CALL_REG_READ(core->paddr);
#else
	val = readl(core->base + PHY_CONTROL);
#endif
	if (debug)
		dev_info(core->dev, "%s - read val 0x%X\n", __func__, val);
	if (enable) {
		val |= enable_set_mask;
		val &= ~enable_clr_mask;
	} else {
		val &= ~enable_set_mask;
		//val |= enable_clr_mask;
	}
	if (core->flag & PHY_FORCE_ON)
		val |= 0x3322b;
	if (debug)
		dev_info(core->dev, "%s - write val 0x%X\n", __func__, val);
#ifdef CONFIG_CORTINA_ACCESS_SMCC
	CA_SMC_CALL_REG_WRITE(core->paddr, val);
#else
	writel(val, core->base + PHY_CONTROL);
#endif
	mutex_unlock(&core->lock);
	dev_dbg(core->dev, "PHY %d %s\n", phy->id,
		enable ? "enabled" : "disabled");

#if defined(CONFIG_SATA_CORTINA_ACCESS)
	ca_sata_config(phy, enable);
#endif
//	ca_phy_serdes_config(phy, enable);

	ca_phy_sgmii_config(phy, enable);

	return 0;
}

static int ca_phy_power_on(struct phy *p)
{
	struct ca_phy *phy = phy_get_drvdata(p);

	if (debug)
		dev_info(phy->core->dev, "Request to power_on phy(%d)\n",
			 phy->id);

	return ca_phy_power_config(phy, true);
}

static int ca_phy_power_off(struct phy *p)
{
	struct ca_phy *phy = phy_get_drvdata(p);

	if (debug)
		dev_info(phy->core->dev, "Request to power_off phy(%d)\n",
			 phy->id);

	return ca_phy_power_config(phy, false);
}

static const struct phy_ops ca_phy_ops = {
	.init = NULL,
	.exit = NULL,
	.power_on = ca_phy_power_on,
	.power_off = ca_phy_power_off,
	.owner = THIS_MODULE,
};

static void ca_serdes_probe(struct device *dev, struct device_node *np,
			    struct ca_phy *p)
{
	phys_addr_t serdes[2];
	int ret, size, cnt;

#ifdef CONFIG_PHYS_ADDR_T_64BIT
	ret = of_property_read_u64_array(np, "serdes-reg", serdes, 2);
#else
	ret = of_property_read_u32_array(np, "serdes-reg", serdes, 2);
#endif
	if (ret) {
		p->dts_name = NULL;
		p->serdes_addr = 0;
		p->serdes_size = 0;
		p->serdes_base = NULL;

		return;
	}

	p->dts_name = (char *)np->name;
	p->serdes_addr = serdes[0];
	p->serdes_size = serdes[1];
	p->serdes_base = NULL;

	size = sizeof(struct serdes_cfg);
	p->cfg_cnt = of_property_count_elems_of_size(np, "serdes-cfg", size);
	if (p->cfg_cnt > 0) {
		p->cfg = devm_kmalloc_array(dev, p->cfg_cnt, size, GFP_KERNEL);

		cnt = p->cfg_cnt * sizeof(struct serdes_cfg) / sizeof(u32);
		of_property_read_u32_array(np, "serdes-cfg", (u32 *)p->cfg,
					   cnt);
	} else {
		p->cfg_cnt = 0;
		p->cfg = NULL;
	}
}

static int ca_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node, *child;
	struct ca_phy_core *core;
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

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
#ifdef CONFIG_CORTINA_ACCESS_SMCC
	core->paddr = (unsigned int)res->start;
#endif
	core->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(core->base))
		return PTR_ERR(core->base);
	dev_info(dev, "resource - %pr mapped at 0x%pK\n", res, core->base);

	if (of_property_read_bool(node, "forced-on"))
		core->flag |= PHY_FORCE_ON;

	mutex_init(&core->lock);

	for_each_available_child_of_node(node, child) {
		unsigned int id;
		struct ca_phy *p;

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
		p->phy = devm_phy_create(dev, child, &ca_phy_ops);
		if (IS_ERR(p->phy)) {
			dev_err(dev, "failed to create PHY\n");
			ret = PTR_ERR(p->phy);
			goto put_child;
		}

		p->core = core;
		p->id = id;

		ca_serdes_probe(dev, child, p);

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

static const struct of_device_id ca_phy_match_table[] = {
	{ .compatible = "cortina-access,venus-phy" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ca_phy_match_table);

static struct platform_driver ca_phy_driver = {
	.driver = {
		.name = "ca-phy",
		.of_match_table = ca_phy_match_table,
	},
	.probe = ca_phy_probe,
};
module_platform_driver(ca_phy_driver);

MODULE_DESCRIPTION("Cortina-Access PHY driver");
MODULE_LICENSE("GPL");
