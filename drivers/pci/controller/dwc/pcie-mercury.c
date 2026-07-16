// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PCIe host controller driver for Cortina-Access SoCs
 *
 * Copyright (C) 2020 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 * Based on dwc/pci-exynos.c, dwc/pci-dra7xx.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/reboot.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/reset.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <soc/cortina-access/ca-soc.h>

#include "../../pci.h"
#include "pcie-designware.h"

#define CA_PCIE_SERDES_CFG_VER_UNKNOWN "unknown"
/*
 * Original interrupt pins
 * INTR 0: GIC
 * INTR 1: PE0
 *
 * Extended interrupt pins
 * INTR 2: GIC
 * INTR 3: GIC
 * INTR 4: GIC
 * INTR 5: PE1
 *
 * GIC interrupt pins
 * PIN 0: INTR 0 -> intr_en_bitmap bit[0]
 * PIN 1: INTR 2 -> intr_en_bitmap bit[1] //extended pin
 * PIN 2: INTR 3 -> intr_en_bitmap bit[2] //extended pin
 * PIN 3: INTR 4 -> intr_en_bitmap bit[3] //extended pin
 */
#define MAX_INTR_PINS       4

#define SERDES_PHY_INIT		0
#define SERDES_PHY_AUTO_CAL	1

/* pci-keystone.c - driver specific constants */
#define MAX_INTX_HOST_IRQS	4

#define MAX_BER_POLL_COUNT	50
#define MAX_LINKUP_POLL_COUNT	500

#define MAX_LANE_NUM		2
#define MAX_NAME_LEN		16

#define to_ca_pcie(x)	dev_get_drvdata((x)->dev)
//#define CONFIG_RTK_PCIE_AFFINITY 1

struct serdes_cfg {
	u32 addr;
	u32 val;
};

struct mac_pset_cfg {
	u32 pre;
	u32 main;
	u32 post;
};

struct tx_matrix_data {
	int size;
	u16 *val;
	int ephy_cnt;
	u16 *ephy_addr;
};

struct ca_pcie {
	struct dw_pcie *pci;
	void __iomem *reg_base; /* elbi */
	void __iomem *serdes_base; /* serdes phy */
	phys_addr_t dbi_start;
	phys_addr_t dbi_end;

	phys_addr_t iatu_unroll_start;
	void __iomem *iatu_unroll_base;

	struct clk *bus_clk;
	struct reset_control *core_reset;
	struct reset_control *phy_reset;
	//struct reset_control *device_reset;
	struct reset_control *device_power;
	struct gpio_desc *gpio_reset;

	struct irq_domain *intx_domain;
	struct phy *phy;
	phys_addr_t serdes_addr;
	resource_size_t serdes_size;
	const char *serdes_cfg_ver;
	struct serdes_cfg *cfg[MAX_LANE_NUM];
	int cfg_cnt[MAX_LANE_NUM];

	u32 idx;
	u8 lanes;

	bool auto_calibration;
	bool init;
	//u32 cur_irq_sts;

	struct raw_spinlock lock;
	int cpu_intx;	/* legacy IRQ */
	u32 masks[MAX_INTR_PINS]; /* MSI data */

	/* Device-Specific */
	/* linking up ready/stable time */
	u16 device_ready_time;
	/*GEN3 EQ local Fs LF*/
	u32 pset_fs;
	u32 pset_lf;
	struct mac_pset_cfg *pset_cfg;
	int pset_cfg_cnt;
	struct tx_matrix_data *txm_data;
	u8 pcie_gen;

	struct notifier_block reboot_nb;
	struct work_struct  work;
};

#define PCIE_GLBL_INTERRUPT_0					0x80
#define INT_RADM_INTA_ASSERTED					0x00000001
#define INT_RADM_INTA_DEASSERTED				0x00000002
#define INT_RADM_INTB_ASSERTED					0x00000004
#define INT_RADM_INTB_DEASSERTED				0x00000008
#define INT_RADM_INTC_ASSERTED					0x00000010
#define INT_RADM_INTC_DEASSERTED				0x00000020
#define INT_RADM_INTD_ASSERTED					0x00000040
#define INT_RADM_INTD_DEASSERTED				0x00000080
#define INT_MSI_CTR_INT							0x00000100
#define INT_SMLH_LINK_UP						0x00000200
#define INT_HP_INT								0x00000400
#define INT_RADM_CORRECTABLE_ERR				0x00000800
#define INT_RADM_NONFATAL_ERR					0x00001000
#define INT_RADM_FATAL_ERR						0x00002000
#define INT_RADM_PM_TO_ACK						0x00004000
#define INT_RADM_PM_PME							0x00008000
#define INT_RADM_QOVERFLOW						0x00010000
#define INT_LINK_DOWN							0x00400000
#define INT_CFG_AER_RC_ERR_MSI					0x00800000
#define INT_CFG_PME_MSI							0x01000000
#define INT_HP_PME								0x02000000
#define INT_HP_MSI								0x04000000
#define INT_CFG_UR_RESP							0x08000000
#define PCIE_GLBL_INTERRUPT_ENABLE_0			0x84
#define PCIE_GLBL_INTERRUPT_1					0x90
#define PCIE_GLBL_INTERRUPT_ENABLE_1				0x94

#define PCIE_GLBL_INTERRUPT_2					0xa0
#define PCIE_GLBL_INTERRUPT_ENABLE_2				0xa4
#define PCIE_GLBL_INTERRUPT_3					0xb0
#define PCIE_GLBL_INTERRUPT_ENABLE_3				0xb4
#define PCIE_GLBL_INTERRUPT_4					0xc0
#define PCIE_GLBL_INTERRUPT_ENABLE_4				0xc4
#define PCIE_GLBL_INTERRUPT_5					0xd0
#define PCIE_GLBL_INTERRUPT_ENABLE_5				0xd4
#define PCIE_GLBL_INTERRUPT_6                                   0xe0
#define PCIE_GLBL_INTERRUPT_ENABLE_6                            0xe4
#define PCIE_GLBL_INTERRUPT_7                                   0xf0
#define PCIE_GLBL_INTERRUPT_ENABLE_7                            0xf4
#define PCIE_GLBL_INTERRUPT_8                                   0x100
#define PCIE_GLBL_INTERRUPT_ENABLE_8                            0x104

#define PCIE_GLBL_AXI_MASTER_RESP_MISC_INFO			0x10
#define PCIE_GLBL_AXI_MSTR_SLV_RESP_ERR_LOW_PW_MAP	0x14
#define PCIE_GLBL_CORE_CONFIG_REG					0x18
#define PCIE_LTSSM_ENABLE							BIT(0)
#define PCIE_LINK_DOWN_RST							BIT(6)
#define PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS	0x1C
#define PWR_STATUS_SMLH_LTSSM_STATE_OFFSET			0x4
#define PWR_STATUS_SMLH_LTSSM_STATE_MASK			0x000003F0
#define PWR_STATUS_RDLH_LINK_UP						BIT(18)
#define PCIE_GLBL_RTLH_INFO							0x20
#define PCIE_GLBL_AXI_MASTER_WR_MISC_INFO			0x24
#define PCIE_GLBL_AXI_MASTER_RD_MISC_INFO			0x28
#define PCIE_GLBL_AXI_SLAVE_BRESP_MISC_INFO			0x2C
#define PCIE_GLBL_AXI_SLAVE_RD_RESP_MISC_INFO_COMP_TIMEOUT	0x30
#define PCIE_GLBL_CORE_DEBUG_0						0x34
#define PCIE_GLBL_CORE_DEBUG_1						0x38
#define PCIE_GLBL_CORE_DEBUG_E1						0x3C
#define PCIE_GLBL_PCIE_CONTR_CFG_START_ADDR			0x110
#define PCIE_GLBL_PCIE_CONTR_CFG_END_ADDR			0x114
#define PCIE_GLBL_PCIE_CONTR_IATU_BASE_ADDR			0x118
#ifdef CONFIG_ARCH_CORTINA_VENUS
#define RMLH_RCVD_ERR								0x124
#endif
#define RC_IATU_BASE_ADDR_MASK					0xFFF80000
#ifdef CONFIG_PCIE_CA_MERCURY
#define PCIE_MSI_INTERRUPT_STATUS_0				0x88
#define PCIE_MSI_INTERRUPT_ENABLE_0				0x8c
#define PCIE_MSI_INTERRUPT_STATUS_1				0x98
#define PCIE_MSI_INTERRUPT_ENABLE_1				0x9c
#define PCIE_MSI_INTERRUPT_STATUS_2				0xa8
#define PCIE_MSI_INTERRUPT_ENABLE_2				0xac
#define PCIE_MSI_INTERRUPT_STATUS_3				0xb8
#define PCIE_MSI_INTERRUPT_ENABLE_3				0xbc
#define PCIE_MSI_INTERRUPT_STATUS_4				0xc8
#define PCIE_MSI_INTERRUPT_ENABLE_4				0xcc
#define PCIE_MSI_INTERRUPT_STATUS_5				0xd8
#define PCIE_MSI_INTERRUPT_ENABLE_5				0xdc
#define PCIE_MSI_INTERRUPT_STATUS_6                             0xe8
#define PCIE_MSI_INTERRUPT_ENABLE_6                             0xec
#define PCIE_MSI_INTERRUPT_STATUS_7                             0xf8
#define PCIE_MSI_INTERRUPT_ENABLE_7                             0xfc
#define PCIE_MSI_INTERRUPT_STATUS_8                             0x108
#define PCIE_MSI_INTERRUPT_ENABLE_8                             0x10c
#define PCIE_RX_OFFSET_FLOW_MASK				GENMASK(4, 1)
#define PCIE_RX_LEQ_ADAPTION_FLOW_MASK				GENMASK(6, 5)
#define PCIE_RX_LEQ_ADAPTION_DBG_MASK				GENMASK(15, 9)
#define PCIE_RX_LEQ_ADAPTION_ANA24_MASK				GENMASK(6, 0)
#endif

/*Ack Frequency Register*/
#define GEN1_FTS_OFFSET	0x70C
#define GEN2_FTS_OFFSET	0x80c

/* ltssm definition */
#define LTSSM_DETECT_QUIET		0x00
#define LTSSM_DETECT_ACT		0x01
#define LTSSM_POLL_ACTIVE		0x02
#define LTSSM_POLL_COMPLIANCE	0x03
#define LTSSM_POLL_CONFIG		0x04
#define LTSSM_PRE_DETECT_QUIET	0x05
#define LTSSM_DETECT_WAIT		0x06
#define LTSSM_CFG_LINKWD_START	0x07
#define LTSSM_CFG_LINKWD_ACEPT	0x08
#define LTSSM_CFG_LANENUM_WAI	0x09
#define LTSSM_CFG_LANENUM_ACEPT	0x0A
#define LTSSM_CFG_COMPLETE		0x0B
#define LTSSM_CFG_IDLE			0x0C
#define LTSSM_RCVRY_LOCK		0x0D
#define LTSSM_RCVRY_SPEED		0x0E
#define LTSSM_RCVRY_RCVRCFG		0x0F
#define LTSSM_RCVRY_IDLE		0x10
#define LTSSM_L0				0x11
#define LTSSM_L0S				0x12
#define LTSSM_L123_SEND_EIDLE	0x13
#define LTSSM_L1_IDLE			0x14
#define LTSSM_L2_IDLE			0x15
#define LTSSM_L2_WAKE			0x16
#define LTSSM_DISABLED_ENTRY	0x17
#define LTSSM_DISABLED_IDLE		0x18
#define LTSSM_DISABLED			0x19
#define LTSSM_ENTRY				0x1A
#define LTSSM_LPBK_ACTIVE		0x1B
#define LTSSM_LPBK_EXIT			0x1C
#define LTSSM_LPBK_EXIT_TIMEOUT	0x1D
#define LTSSM_HOT_RESET_ENTRY	0X1E
#define LTSSM_HOT_RESET			0x1F
#define LTSSM_RCVRY_EQ0			0x20
#define LTSSM_RCVRY_EQ1			0x21
#define LTSSM_RCVRY_EQ2			0x22
#define LTSSM_RCVRY_EQ3			0x23

#define MAC_DWC_CFG		0x18
#define MAC_DWC_INFO1		0x10
#define MAC_BDF			0x1c
#define BIT_CHECK(a, b) (!!((a) & (1ULL << (b))))
#define GEN3_RELATED_OFF_REG            0x890
#define GEN3_EQ_FS_LF_REG               0x894
#define GEN3_EQ_PSET_COEF_MAP_REG       0x898
#define GEN3_EQ_PSET_INDEX_OFF_REG      0x89c
#define GEN3_EQ_CONTROL_OFF             0x8a8
#define SPCIE_CAP_OFF_GEN3             0x180
static const u32 intc_en_regs[MAX_INTR_PINS] = {
	PCIE_GLBL_INTERRUPT_ENABLE_0,
	PCIE_GLBL_INTERRUPT_ENABLE_1,
	PCIE_GLBL_INTERRUPT_ENABLE_2,
	PCIE_GLBL_INTERRUPT_ENABLE_3,
};

static const u32 intc_st_regs[MAX_INTR_PINS] = {
	PCIE_GLBL_INTERRUPT_0,
	PCIE_GLBL_INTERRUPT_1,
	PCIE_GLBL_INTERRUPT_2,
	PCIE_GLBL_INTERRUPT_3,
};

static const u32 msi_en_regs[MAX_INTR_PINS] = {
	PCIE_MSI_INTERRUPT_ENABLE_0,
	PCIE_MSI_INTERRUPT_ENABLE_1,
	PCIE_MSI_INTERRUPT_ENABLE_2,
	PCIE_MSI_INTERRUPT_ENABLE_3,
};

static const char * const __ltssm_str[] = {
	"DETECT_QUIET",
	"DETECT_ACT",
	"POLL_ACTIVE",
	"POLL_COMPLIANCE",
	"POLL_CONFIG",
	"PRE_DETECT_QUIET",
	"DETECT_WAIT",
	"CFG_LINKWD_START",
	"CFG_LINKWD_ACEPT",
	"CFG_LANENUM_WAI",
	"CFG_LANENUM_ACEPT",
	"CFG_COMPLETE",
	"CFG_IDLE",
	"RCVRY_LOCK",
	"RCVRY_SPEED",
	"RCVRY_RCVRCFG",
	"RCVRY_IDLE",
	"L0",
	"L0S",
	"L123_SEND_EIDLE",
	"L1_IDLE",
	"L2_IDLE",
	"L2_WAKE",
	"DISABLED_ENTRY",
	"DISABLED_IDLE",
	"DISABLED",
	"LPBK_ENTRY",
	"LPBK_ACTIVE",
	"LPBK_EXIT",
	"LPBK_EXIT_TIMEOUT",
	"HOT_RESET_ENTRY",
	"HOT_RESET",
	"RCVRY_EQ0",
	"RCVRY_EQ1",
	"RCVRY_EQ2",
	"RCVRY_EQ3"
};

static const char *ltssm_str(int state)
{
	if (state >= 0 && state < ARRAY_SIZE(__ltssm_str))
		return __ltssm_str[state];
	else
		return "";
}

static inline u32 ca_pcie_readl(struct ca_pcie *pcie, u32 reg);
static inline void ca_pcie_writel(struct ca_pcie *pcie, u32 val,
				  u32 reg);
static void serdes_phy_calibration(struct ca_pcie *pcie);
static int mac_preset(struct ca_pcie *pcie);
static int pcie_tx_matrix_probe(struct device *dev, struct device_node *np,
				struct ca_pcie *pcie);
static int pcie_tx_matrix_patch(struct ca_pcie *pcie);

static int ca_pcie_device_reset(struct ca_pcie *pcie);
static int ca_pcie_ltssm(struct ca_pcie *pcie);

static ssize_t device_reset_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t count)
{
	struct ca_pcie *pcie = dev_get_drvdata(dev);
	int result;

	result = ca_pcie_device_reset(pcie);
	if (result < 0)
		return result;

	return count;
}

//Taurus PHY REG 0x1f is multi-functional.
//It needs the following commands to switch to output OOBS level
static int ca_pcie_serdes_phy_switch_oobs(void __iomem *phy_base)
{
	writel(0xf75a, phy_base + 0x0034);
	usleep_range(100, 200);
	writel(0xf75a, phy_base + 0x0134);
	usleep_range(100, 200);
	writel(0x4500, phy_base + 0x00bc);
	usleep_range(100, 200);
	writel(0x3d00, phy_base + 0x01bc);
	usleep_range(100, 200);
	writel(0x0011, phy_base + 0x006c);
	usleep_range(100, 200);
	writel(0x0011, phy_base + 0x016c);
	usleep_range(100, 200);
	writel(0x01a4, phy_base + 0x0040);
	usleep_range(100, 200);
	writel(0x01a4, phy_base + 0x0140);
	usleep_range(100, 200);
	return 0;
}

static DEVICE_ATTR_WO(device_reset);

static ssize_t serdes_phy_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ca_pcie *ca_pcie = dev_get_drvdata(dev);
	ssize_t ret = 0;
	void __iomem *phy_base;
	int lane, addr;
	u16 val;

	ret += sprintf(buf, "PCIe PHY ver: %s\n", ca_pcie->serdes_cfg_ver);

	for (lane = 0; lane < ca_pcie->lanes; lane++) {
		ret += sprintf(buf + ret, "lane [%d]\n", lane);
		ret += sprintf(buf + ret, "Gen 1:  \tGen 2:\n");

		phy_base = ca_pcie->serdes_base + lane * 0x1000;
		ca_pcie_serdes_phy_switch_oobs(phy_base);
		ret += sprintf(buf + ret, "Page 0  \tPage 0\n");
		for (addr = 0x0; addr <= 0x1f; addr++) {
			val = readl(phy_base + addr * 0x4) & 0xffff;
			ret += sprintf(buf + ret, "%02x: %04x\t", addr, val);
			val = readl(phy_base + (addr + 0x40) * 0x4) & 0xffff;
			ret += sprintf(buf + ret, "%02x: %04x\n", addr, val);
		}
		ret += sprintf(buf + ret, "Page 1  \tPage 1\n");
		for (addr = 0x20; addr <= 0x34; addr++) {
			val = readl(phy_base + addr * 0x4) & 0xffff;
			ret += sprintf(buf + ret, "%02x: %04x\t", addr - 0x20, val);
			val = readl(phy_base + (addr + 0x40) * 0x4) & 0xffff;
			ret += sprintf(buf + ret, "%02x: %04x\n", addr - 0x20, val);
		}
		ret += sprintf(buf + ret, "\n");
	}

	return ret;
}

static ssize_t serdes_phy_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	pr_info("Use \"devmem 0xf433[serdes phy id]000 + addr*0x4 32 [value]\" to write PHY parameter\n");
	pr_info("ex: devmem 0xf4333004 32 0x1852\n");
	return count;
}

static DEVICE_ATTR_RW(serdes_phy);

static struct attribute *ca_pcie_attributes[] = {
	&dev_attr_device_reset.attr,
	&dev_attr_serdes_phy.attr,
	NULL
};

static const struct attribute_group ca_pcie_attr_group = {
	.attrs = ca_pcie_attributes,
};

static struct attribute *ca_pcie_dbg_attributes[] = {
	&dev_attr_serdes_phy.attr,
	NULL
};

static const struct attribute_group ca_pcie_dbg_attr_group = {
	.attrs = ca_pcie_dbg_attributes,
};

static void ca_pcie_mask_intx_irq(struct irq_data *irqd)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(irqd);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);
	u32 reg, val;

	raw_spin_lock(&pcie->lock);
	reg = intc_en_regs[pcie->cpu_intx];
	val = ca_pcie_readl(pcie, reg);
	val &= ~(INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
		 INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);
	ca_pcie_writel(pcie, val, reg);
	raw_spin_unlock(&pcie->lock);
}

static void ca_pcie_unmask_intx_irq(struct irq_data *irqd)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(irqd);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);
	u32 reg, val;

	raw_spin_lock(&pcie->lock);

	reg = intc_en_regs[pcie->cpu_intx];
			val = ca_pcie_readl(pcie, reg);

			val |= (INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
				INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);
	ca_pcie_writel(pcie, val, reg);

	raw_spin_unlock(&pcie->lock);
}

static int ca_pcie_intx_set_affinity(struct irq_data *d,
				     const struct cpumask *mask,
				     bool force)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);
	struct cpumask m;
	unsigned int cpu;
	u32 reg, val;

	cpumask_and(&m, cpu_online_mask, mask);
	cpu = cpumask_first(&m);
	raw_spin_lock(&pcie->lock);

	if (cpu != pcie->cpu_intx) {
		reg = intc_en_regs[pcie->cpu_intx];
		val = ca_pcie_readl(pcie, reg);
		val &= ~(INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
		INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);
		ca_pcie_writel(pcie, val, reg);

		pcie->cpu_intx = cpu;

		reg = intc_en_regs[pcie->cpu_intx];
			val = ca_pcie_readl(pcie, reg);
		val |= (INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
			INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);
		ca_pcie_writel(pcie, val, reg);
	}

	raw_spin_unlock(&pcie->lock);
	return 0;
}

static struct irq_chip ca_pcie_intx_irq_chip = {
	.name = "PCI-INTx",
	.irq_enable = ca_pcie_unmask_intx_irq,
	.irq_disable = ca_pcie_mask_intx_irq,
	.irq_mask = ca_pcie_mask_intx_irq,
	.irq_unmask = ca_pcie_unmask_intx_irq,
	.irq_set_affinity = ca_pcie_intx_set_affinity,
};

static int ca_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			    irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &ca_pcie_intx_irq_chip,
				 handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static int ca_irqd_intx_xlate(struct irq_domain *d,
			      struct device_node *node,
			      const u32 *intspec,
			      unsigned int intsize,
			      unsigned long *out_hwirq,
			      unsigned int *out_type)
{
	int ret;

	ret = pci_irqd_intx_xlate(d, node, intspec, intsize, out_hwirq, out_type);

	return ret;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = ca_pcie_intx_map,
	.xlate = ca_irqd_intx_xlate,
};

static int ca_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	struct ca_pcie *pcie = to_ca_pcie(pci);
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node =  of_get_next_child(node, NULL);

	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODEV;
	}

	pcie->intx_domain = irq_domain_add_linear(pcie_intc_node, PCI_NUM_INTX,
						  &intx_domain_ops, pp);

	of_node_put(pcie_intc_node);

	if (!pcie->intx_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENODEV;
	}

	return 0;
}

static int ca_pcie_serdes_phy_init(struct ca_pcie *ca_pcie)
{
	u32 val;
	int i, lane = 0;
	struct dw_pcie *pci = ca_pcie->pci;
	struct device *dev = pci->dev;

	dev_info(dev, "PCIe Serdes CFG version: %s\n",
		 ca_pcie->serdes_cfg_ver);

		for (i = 0; i < ca_pcie->cfg_cnt[lane]; i++) {
			val = ca_pcie->cfg[lane][i].val;
			usleep_range(1000, 2000);
			writel(val, ca_pcie->serdes_base + (phys_addr_t)ca_pcie->cfg[lane][i].addr);
		}
	return 0;
}

static int ca_pcie_serdes_ber_notify(struct ca_pcie *ca_pcie)
{
	int cnt = 0;
	int reg;

	do {
		reg = 1;
		reg &= readl(ca_pcie->serdes_base + 0x007c);
		reg &= readl(ca_pcie->serdes_base + 0x017c);

		if (ca_pcie->lanes == 2) {
			reg &= readl(ca_pcie->serdes_base + 0x107c);
			reg &= readl(ca_pcie->serdes_base + 0x117c);
		}

		if (reg)
			return reg;

		usleep_range(1000, 2000);
	} while (cnt++ < MAX_BER_POLL_COUNT);

	return reg;
}

static inline u32 ca_pcie_readl(struct ca_pcie *pcie, u32 reg)
{
	return readl(pcie->reg_base + reg);
}

static inline void ca_pcie_writel(struct ca_pcie *pcie, u32 val, u32 reg)
{
	writel(val, pcie->reg_base + reg);
}

static int ca_pcie_device_reset(struct ca_pcie *pcie)
{
	gpiod_set_value_cansleep(pcie->gpio_reset, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(pcie->gpio_reset, 0);
	msleep(pcie->device_ready_time);
	return 0;
}

static int ca_pcie_host_reset(struct ca_pcie *pcie,
			      int serdes_phase)
{
	u8 offset;
	struct dw_pcie *pci = pcie->pci;

	gpiod_set_value_cansleep(pcie->gpio_reset, 1);
	usleep_range(1000, 2000);

	reset_control_assert(pcie->core_reset);
	usleep_range(1000, 2000);

	reset_control_assert(pcie->phy_reset);
	usleep_range(1000, 2000);
	ca_pcie_serdes_phy_init(pcie);

	if (pcie->phy)
		phy_power_on(pcie->phy);
	usleep_range(1000, 2000);

	reset_control_deassert(pcie->phy_reset);
	usleep_range(1000, 2000);
	reset_control_deassert(pcie->core_reset);
	usleep_range(1000, 2000);
	//EQ training
	writel(0x770701EC, pci->dbi_base + 0x8f8);
	writel(0x000f7770, pci->dbi_base + 0x8fc);

	serdes_phy_calibration(pcie);
	usleep_range(1000, 2000);

	offset = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);
	pcie->pcie_gen = (dw_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP) & PCI_EXP_LNKCAP_SLS);

	if (pcie->pcie_gen >= PCI_EXP_LNKCTL2_TLS_8_0GT)
		mac_preset(pcie);

	gpiod_set_value_cansleep(pcie->gpio_reset, 0);

	if (!ca_pcie_serdes_ber_notify(pcie))
		dev_err(pci->dev, "No BER Notify!\n");

	if (pcie->idx <= 2)
		pcie_tx_matrix_patch(pcie);

	msleep(pcie->device_ready_time);

	return 0;
}

static int ca_pcie_host_setup(struct ca_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;

	ca_pcie_writel(pcie, pcie->dbi_start,
		       PCIE_GLBL_PCIE_CONTR_CFG_START_ADDR);
	ca_pcie_writel(pcie, pcie->dbi_end,
		       PCIE_GLBL_PCIE_CONTR_CFG_END_ADDR);
	if (pcie->iatu_unroll_start) {
		u32 base_addr = (u32)pcie->iatu_unroll_start &
				RC_IATU_BASE_ADDR_MASK;
		ca_pcie_writel(pcie, base_addr,
			       PCIE_GLBL_PCIE_CONTR_IATU_BASE_ADDR);
	}

	// Config DWC cfg
	dev_err(pci->dev, "Config DWC CFG\n");
	ca_pcie_writel(pcie, 0x0, MAC_DWC_CFG);
	// Config PCIe tlp type

	return 0;
}

static void ca_pcie_stop_link(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);
	// link_down_reset
	ca_pcie_writel(pcie, 0x40, PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
	ca_pcie_writel(pcie, 0x0,  PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
	pr_err("stop link\n");
}

static int ca_pcie_establish_link(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);

	if (dw_pcie_link_up(pci))
		return 0;

	/* assert LTSSM enable */
	ca_pcie_writel(pcie, 0x80, MAC_DWC_CFG);
	usleep_range(1000, 2000);

	return 0;
}

static void ca_pcie_misc_enable(struct ca_pcie *pcie)
{
	u32 val;

	val = ca_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_ENABLE_0);

	val |= (INT_HP_INT |
		INT_RADM_CORRECTABLE_ERR |
		INT_RADM_NONFATAL_ERR |
		INT_RADM_FATAL_ERR |
		INT_RADM_PM_TO_ACK |
		INT_RADM_PM_PME |
		INT_RADM_QOVERFLOW |
		INT_LINK_DOWN |
		INT_CFG_AER_RC_ERR_MSI |
		INT_CFG_PME_MSI	|
		INT_HP_PME |
		INT_HP_MSI |
		INT_CFG_UR_RESP);

	ca_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_ENABLE_0);
}

static void ca_pcie_msi_enable(struct ca_pcie *pcie)
{
	u32 reg, val;
	int i;
	//struct dw_pcie *pci = pcie->pci;

//#if defined(CONFIG_ARCH_REALTEK_9607F)
	/* Ehanced Interrupt Design */
	/* Set MSI interrupt mask first */

	pcie->masks[0] = ~0;
	pcie->masks[1] = 0;
	pcie->masks[2] = 0;
	pcie->masks[3] = 0;

	for (i = 0; i < MAX_INTR_PINS; i++) {
		ca_pcie_writel(pcie, pcie->masks[i], msi_en_regs[i]);
		reg = intc_en_regs[i];
			val = ca_pcie_readl(pcie, reg);
			/* Ensure the value is fully read before performing the next operation.
			 * This guarantees the ordering of the read and subsequent
			 * value assignment to prevent data corruption in multi-core systems.
			 */
			smp_rmb();
			val |= INT_MSI_CTR_INT;
			/* Ensure that the previous read operation is completed
			 * before proceeding with the subsequent write. This prevents
			 * write reordering in a multi-core environment.
			 */
			smp_wmb();
			ca_pcie_writel(pcie, val, reg);
		}
}

static void ca_pcie_enable_interrupts(struct ca_pcie *pcie)
{
	/* link up/down/mis interrupt */
	ca_pcie_misc_enable(pcie);
	ca_pcie_msi_enable(pcie);
}

/* MSI int handler */
irqreturn_t ca_handle_msi_irq(struct dw_pcie_rp *pp)
{
	int i, pos;
	unsigned long val;
	u32 status, mask;
	irqreturn_t ret = IRQ_NONE;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);

	for (i = 0; i < MAX_MSI_CTRLS; i++) {
		status = dw_pcie_readl_dbi(pci,
					   PCIE_MSI_INTR0_STATUS +
					   (i * MSI_REG_CTRL_BLOCK_SIZE));

		mask = pcie->masks[smp_processor_id()];
		status &= mask;

		if (!status)
			continue;

		ret = IRQ_HANDLED;
		val = status;
		pos = 0;
		while ((pos = find_next_bit(&val, MAX_MSI_IRQS_PER_CTRL,
					    pos)) != MAX_MSI_IRQS_PER_CTRL) {
			dw_pcie_write_dbi(pci, PCIE_MSI_INTR0_STATUS +
					  i * MSI_REG_CTRL_BLOCK_SIZE, 4,
					  1 << pos);
			generic_handle_domain_irq(pp->irq_domain,
						  (i * MAX_MSI_IRQS_PER_CTRL) +
						  pos);
			pos++;
		}
	}

	return ret;
}

static void ca_pcie_chained_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dw_pcie_rp *pp;
	struct dw_pcie *pci;
	struct ca_pcie *pcie;
	u32 reg, val;

	chained_irq_enter(chip, desc);

	pcie = irq_desc_get_handler_data(desc);
	pci = pcie->pci;
	pp = &pci->pp;

	reg = intc_st_regs[smp_processor_id()];

	val = ca_pcie_readl(pcie, reg);

	if (val & INT_MSI_CTR_INT)
		ca_handle_msi_irq(pp);

	if (val & INT_RADM_INTA_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_domain, 0));

	if (val & INT_RADM_INTB_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_domain, 1));

	if (val & INT_RADM_INTC_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_domain, 2));

	if (val & INT_RADM_INTD_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_domain, 3));

	if (val & INT_LINK_DOWN) {
		int ltssm = ca_pcie_ltssm(pcie);

		dev_err(pci->dev,
			"Link Down!!!(ltssm = 0x%x - %s)\n",
			ltssm, ltssm_str(ltssm));

		/* An extra 1000ns delay might be required if it reaches
		 * here within 1000ns after INT_LINK_DOWN is triggered
		 */
		//ndelay(1000);

		ca_pcie_writel(pcie, PCIE_LINK_DOWN_RST,
			       PCIE_GLBL_CORE_CONFIG_REG);

		schedule_work(&pcie->work);
	}

	ca_pcie_writel(pcie, val, reg);

	chained_irq_exit(chip, desc);
}

static int ca_pcie_ltssm(struct ca_pcie *pcie)
{
	u32 reg, val;

	reg = PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS;
	val = ca_pcie_readl(pcie, reg);
	val = (val & PWR_STATUS_SMLH_LTSSM_STATE_MASK) >>
	      PWR_STATUS_SMLH_LTSSM_STATE_OFFSET;

	return val;
}

void dw_pcie_link_check(struct dw_pcie *pci, u8 *speed, u8 *lanes)
{
	u32 val;

	val = readb(pci->dbi_base + 0x82);
	*speed = val & PCI_EXP_LNKSTA_CLS;
	*lanes = (val & PCI_EXP_LNKSTA_NLW) >> 4;
}

static int ca_pcie_link_up(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);
	u32 val;

	val = ca_pcie_readl(pcie, MAC_DWC_INFO1);
	val &= (1 << 5);
	return val ? 1 :  0;
}


static pci_ers_result_t ca_pcie_reset_link(struct pci_dev *pdev)
{
	struct dw_pcie_rp *pp = pdev->bus->sysdata;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	pp->ops->host_init(pp);
	if (!ca_pcie_link_up(pci))
		return PCI_ERS_RESULT_DISCONNECT;
	else
		return PCI_ERS_RESULT_RECOVERED;
}

static void ca_pcie_host_link_recovery(struct work_struct *work)
{
	struct ca_pcie *pcie = container_of(work, struct ca_pcie, work);
	struct dw_pcie_rp *pp = &pcie->pci->pp;
	struct pci_dev *pdev;

	//let us have more time after activating link_down_reset
	usleep_range(100, 200);
	//clear link_down_reset and app_ltssm_enable
	ca_pcie_writel(pcie, 0x0,  PCIE_GLBL_CORE_CONFIG_REG);

	pdev = pci_get_domain_bus_and_slot(pci_domain_nr(pp->bridge->bus), 0, 0);
#ifdef CONFIG_PCIEPORTBUS
	pcie_do_recovery(pdev, pci_channel_io_frozen, ca_pcie_reset_link);
#else
	switch (ca_pcie_reset_link(pdev)) {
	case PCI_ERS_RESULT_RECOVERED:
		dev_info(pcie->pci->dev, "Link recovery successful\n");
		break;

	case PCI_ERS_RESULT_DISCONNECT:
		dev_info(pcie->pci->dev, "Link recovery failed\n");
		break;
}

#endif
}

static int ca_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);
	int ret, ltssm;
	//u32 ctrl, num_ctrls;

	//uint32_t reg;
	/* disable_interrupts */
	ca_pcie_writel(pcie, 0, PCIE_GLBL_INTERRUPT_ENABLE_0);
	ca_pcie_host_reset(pcie, SERDES_PHY_INIT);
	ca_pcie_host_setup(pcie);
	ca_pcie_establish_link(pci);
	ret = dw_pcie_wait_for_link(pci);
	if (ret) {
		ltssm = ca_pcie_ltssm(pcie);

		dev_err(pci->dev, "Link Fail!!!(ltssm = 0x%x - %s)\n",
			ltssm, ltssm_str(ltssm));
		goto END_HOST_INIT;
	} else {
		u8 speed, lanes;

		dw_pcie_link_check(pci, &speed, &lanes);
		dev_info(pci->dev, "Speed Gen%d Lanes x%d", speed, lanes);
	}

	ca_pcie_enable_interrupts(pcie);
	ca_pcie_init_irq_domain(pp);

	INIT_WORK(&pcie->work, ca_pcie_host_link_recovery);

	return 0;
END_HOST_INIT:
	return (ltssm == LTSSM_DETECT_QUIET ? -ENODEV : -EIO);
}

static const struct dw_pcie_host_ops ca_pcie_host_ops = {
	.host_init = ca_pcie_host_init,
};

static const struct dw_pcie_ops ca_pcie_ops = {
	.link_up = ca_pcie_link_up,
	.start_link = ca_pcie_establish_link,
	.stop_link = ca_pcie_stop_link,
};

static int __init ca_add_pcie_port(struct ca_pcie *pcie,
				   struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = pci->dev;
	struct resource *res;
	int ret;
	//u16 fts_val;
	//u32 reg;

	pp->irq = platform_get_irq(pdev, 0);

	if (pp->irq < 0)
		return pp->irq;

#define REQIRQ 0
#if REQIRQ
	ret = devm_request_irq(dev, pp->irq, ca_pcie_irq_handler,
			       IRQF_SHARED | IRQF_PROBE_SHARED, dev_name(dev), pcie);

	if (ret) {
		dev_err(dev, "fail to request irq\n");
		return ret;
	}

#else
	irq_set_chained_handler_and_data(pp->irq, ca_pcie_chained_handler, pcie);
#endif

	do {
		int i, irq;

		for (i = 1; i < MAX_INTR_PINS; i++) {
			irq = platform_get_irq(pdev, i); /* one for INTx, MSI, and misc */

			if (irq < 0)
				return irq;

#if REQIRQ
			ret = devm_request_irq(dev, irq, ca_pcie_irq_handler,
					       0, dev_name(dev), pcie);
#else

			irq_set_chained_handler_and_data(irq, ca_pcie_chained_handler, pcie);
#endif
			ret = irq_set_affinity(irq, cpumask_of(i));

			if (ret)
				dev_err(dev, "Fail to set affinity for cpu %d, %d\n", i, ret);
		}
	} while (0);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbi");
	pci->dbi_base = devm_ioremap_resource(dev, res);
	if (!pci->dbi_base)
		return -ENOMEM;

	dev_info(dev, "resource - %pr mapped at 0x%llx\n", res,
		 (u64)pci->dbi_base);
	pcie->dbi_start = res->start;
	pcie->dbi_end = res->end;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iatu");
	pcie->iatu_unroll_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->iatu_unroll_base)) {
		ret = PTR_ERR(pcie->iatu_unroll_base);
		return ret;
	}

	pci->atu_base = pcie->iatu_unroll_base;
	if (!pcie->iatu_unroll_base) {
		dev_info(dev, "outbound iATU not used\n");
		pcie->iatu_unroll_start = 0;
	} else {
		dev_info(dev, "outbound iATU enable\n");
		pcie->iatu_unroll_start = res->start;
	}

	pp->ops = &ca_pcie_host_ops;
	pp->num_vectors = MAX_MSI_IRQS;

	// config for separate CA/DWC path
	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host(%d)\n", ret);
		return ret;
	}
	return 0;
}

enum chip_rev {
	REV_A = 0,
	REV_B,
	REV_A_40M,
	REV_B_40M,
	REV_CNT,
};

const char *cfg_ver_str[] = {
	"serdes-cfg-verA",
	"serdes-cfg-verB",
	"serdes-cfg-verA_40M",
	"serdes-cfg-verB_40M",
};

const char *cfg_data_str[] = {
	"serdes-cfg-dataA",
	"serdes-cfg-dataB",
	"serdes-cfg-dataA_40M",
	"serdes-cfg-dataB_40M",
};

static int mac_preset(struct ca_pcie *pcie)
{
	u32 val = 0, reg = 0;
	int i, j;
	struct dw_pcie *pci = pcie->pci;

	//Set enable
	for (i = 0; i <= (pcie->pcie_gen >> 2); i++) {
		//Set GEN3
		val = readl(pci->dbi_base + GEN3_RELATED_OFF_REG);
		val &= ~GENMASK(25, 24);
		val |= FIELD_PREP(GENMASK(25, 24), i);
		writel(val, pci->dbi_base + GEN3_RELATED_OFF_REG);
		val = readl(pci->dbi_base + GEN3_EQ_CONTROL_OFF);
		val &= ~GENMASK(18, 8); //Only enable P0 - P10
		writel(val, pci->dbi_base + GEN3_EQ_CONTROL_OFF);

		//Write FS: 0x894[11:6], LF: 0x894[5:0]
		val = (pcie->pset_fs << 6) + (pcie->pset_lf);
		reg = readl(pci->dbi_base + GEN3_EQ_FS_LF_REG);
		reg &= ~GENMASK(11, 0);
		reg |= val;
		writel(reg, pci->dbi_base + GEN3_EQ_FS_LF_REG);
		for (j = 0; j <= 10; j++) {
			val = readl(pci->dbi_base + GEN3_EQ_PSET_INDEX_OFF_REG);
			val &= ~GENMASK(3, 0);
			val = j;
			writel(val, pci->dbi_base + GEN3_EQ_PSET_INDEX_OFF_REG);
			reg = 0;
			val = pcie->pset_cfg[j].pre; //Pre
			reg |= FIELD_PREP(GENMASK(5, 0), val);
			val =  pcie->pset_cfg[j].main; // Main
			reg |= FIELD_PREP(GENMASK(11, 6), val);
			val = pcie->pset_cfg[j].post; //Post
			reg |= FIELD_PREP(GENMASK(17, 12), val);
			writel(reg, pci->dbi_base + GEN3_EQ_PSET_COEF_MAP_REG);
		}

		reg = readl(pci->dbi_base + SPCIE_CAP_OFF_GEN3);
		reg &= ~GENMASK(3, 0);
		val = 0x1;//DSP RX_PRESET0
		reg |= FIELD_PREP(GENMASK(3, 0), val);
		reg &= ~GENMASK(6, 4);
		val = 0x1;//DSP_TX_HINT0
		reg |= FIELD_PREP(GENMASK(6, 4), val);
		reg &= ~GENMASK(11, 8);
		val = 0x1;//DSP_TX_HINT0
		reg |= FIELD_PREP(GENMASK(11, 8), val);
		reg &= ~GENMASK(14, 12);
		val = 0x1;//DSP_TX_HINT0
		reg |= FIELD_PREP(GENMASK(14, 12), val);

		reg &= ~GENMASK(19, 16);
		val = 0x1;//DSP_TX_PRESET1
		reg |= FIELD_PREP(GENMASK(19, 16), val);
		reg &= ~GENMASK(22, 20);
		val = 0x1;//DSP_TX_HINT1
		reg |= FIELD_PREP(GENMASK(22, 20), val);
		reg &= ~GENMASK(27, 24);
		val = 0x1;//DSP_TX_HINT1
		reg |= FIELD_PREP(GENMASK(27, 24), val);
		reg &= ~GENMASK(30, 28);
		val = 0x1;//DSP_TX_HINT1
		reg |= FIELD_PREP(GENMASK(30, 28), val);

		writel(reg, pci->dbi_base + SPCIE_CAP_OFF_GEN3);
		//Set enable
		val = readl(pci->dbi_base + GEN3_EQ_CONTROL_OFF);
		val &= ~GENMASK(18, 8); //Only enable P0 - P10
		val |= GENMASK(18, 8); //Only enable P0 - P10
		writel(val, pci->dbi_base + GEN3_EQ_CONTROL_OFF);
	}
	return 0;
}

static inline u32 genmask(u8 hi, u8 lo)
{
	u32 mask;

	if (hi > 31 || lo > 31 || hi < lo)
		return 0;

	mask = ((1U << (hi - lo + 1)) - 1) << lo;

	return mask;
}

static void phywrite(const struct ca_pcie *pcie, u32 offset, u8 hi, u8 lo, u16 data)
{
	void __iomem *addr = pcie->serdes_base + (offset << 2);

	if (hi == 31 && lo == 0) {
		writel(data, addr);
	} else {
		u32 tmp, mask;

		mask = genmask(hi, lo);
		tmp = readl(addr) & ~mask;
		tmp |= ((data << lo) & mask);
		writel(tmp, addr);
	}
	usleep_range(100, 110);
}

static u32 phyread(const struct ca_pcie *pcie, u32 offset, u8 hi, u8 lo)
{
	void __iomem *addr = pcie->serdes_base + (offset << 2);
	u32 data;

	if (hi == 31 && lo == 0) {
		data = readl(addr);
	} else {
		data = readl(addr);
		data = (genmask(hi, lo) & data) >> lo;
	}
	usleep_range(100, 110);
	return data;
}

#define _W(ofs, hi, lo, val) phywrite(pcie, ofs, hi, lo, val)
#define _R(ofs, hi, lo) phyread(pcie, ofs, hi, lo)

static u16 signed_check(const struct ca_pcie *pcie, u16 ephy_addr, u16 val)
{
	u32 tmp, inverse;

	if (BIT_CHECK(val, 7) == 0)/*unsigned*/
		return val;

	tmp = _R(ephy_addr, 6, 0);
	inverse = ((~tmp) & 0x7f);
	inverse |= BIT(7);
	return inverse;
}

static void s0_s1_phy_cali_start(const struct ca_pcie *pcie)
{
//	printk("%s(%d) Enter\n",__func__,__LINE__);
	_W(0x1c48, 31, 0, 0x8185);
	_W(0x1c4a, 31, 0, 0xffff);
	_W(0x1c4c, 31, 0, 0xffff);
	_W(0x1c5c, 31, 0, 0x8084);
	_W(0x2030, 31, 0, 0xbfd0);
	_W(0x2030, 31, 0, 0xffd0);
	_W(0x2130, 31, 0, 0xffd0);
	_W(0x2430, 31, 0, 0xffd0);
	_W(0x2530, 31, 0, 0xffd0);
	_W(0x2830, 31, 0, 0xffd0);
	_W(0x2930, 31, 0, 0xffd0);
	_W(0x2c30, 31, 0, 0xffd0);
	_W(0x2d30, 31, 0, 0xffd0);
//	printk("%s(%d) Leave\n",__func__,__LINE__);
}

static void s0_s1_phy_cali_gen1(const struct ca_pcie *pcie)
{
	u32 tmp;

//	printk("%s(%d) Enter\n",__func__,__LINE__);
	_W(0x1c5c, 31, 0, 0x8084);
	_W(0x201e, 31, 0, 0x5059);
	_W(0x211e, 31, 0, 0x5059);
	_W(0x1c48, 31, 0, 0x8185);
	_W(0x1c48, 31, 0, 0x818f);
	_W(0x205c, 31, 0, 0x0050);
	_W(0x1c64, 31, 0, 0x001a);
	while (_R(0x1c68, 9, 9) != 0x1)
		;

	_W(0x201e, 31, 0, 0x7059);
	_W(0x211e, 31, 0, 0x7059);
	_W(0x202e, 31, 0, 0x0c1e);
	_W(0x212e, 31, 0, 0x0c1e);
	while (_R(0x1804, 2, 2) != 0x1)
		;

	while (_R(0x1904, 2, 2) != 0x1)
		;

	_W(0x202e, 31, 0, 0x0c11);
	tmp = _R(0x1804, 4, 0);
	_W(0x201c, 12, 8, tmp);
	_W(0x201c, 2, 2, 0x1);
	_W(0x202e, 31, 0, 0x0c12);
	tmp = _R(0x1804, 4, 0);
	_W(0x201e, 12, 8, tmp);
	_W(0x201e, 2, 2, 0x1);
	_W(0x202e, 31, 0, 0x0c13);
	tmp = _R(0x1804, 7, 0);
	tmp = signed_check(pcie, 0x1804, tmp);
	_W(0x202c, 15, 8, tmp);
	_W(0x202c, 1, 1, 0x1);
	_W(0x202e, 31, 0, 0x0c16);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a4, 6, 0, tmp);
	_W(0x2022, 0, 0, 0x1);
	_W(0x202e, 31, 0, 0x0c17);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a4, 14, 8, tmp);
	_W(0x2022, 1, 1, 0x1);
	_W(0x202e, 31, 0, 0x0c18);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a6, 6, 0, tmp);
	_W(0x2022, 2, 2, 0x1);
	_W(0x202e, 31, 0, 0x0c19);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a6, 14, 8, tmp);
	_W(0x2022, 3, 3, 0x1);
	_W(0x202e, 31, 0, 0x0c1a);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a8, 6, 0, tmp);
	_W(0x2022, 4, 4, 0x1);
	_W(0x202e, 31, 0, 0x0c1b);
	tmp = _R(0x1804, 6, 0);
	_W(0x20a8, 14, 8, tmp);
	_W(0x2022, 5, 5, 0x1);
	_W(0x202e, 31, 0, 0x0c1c);
	tmp = _R(0x1804, 6, 0);
	_W(0x20aa, 6, 0, tmp);
	_W(0x2022, 6, 6, 0x1);
	_W(0x202e, 31, 0, 0x0c1d);
	tmp = _R(0x1804, 6, 0);
	_W(0x20aa, 14, 8, tmp);
	_W(0x2022, 7, 7, 0x1);
	_W(0x212e, 31, 0, 0x0c11);
	tmp = _R(0x1904, 4, 0);
	_W(0x211c, 12, 8, tmp);
	_W(0x211c, 2, 2, 0x1);
	_W(0x212e, 31, 0, 0x0c12);
	tmp = _R(0x1904, 4, 0);
	_W(0x211e, 12, 8, tmp);
	_W(0x211e, 2, 2, 0x1);
	_W(0x212e, 31, 0, 0x0c13);
	tmp = _R(0x1904, 7, 0);
	tmp = signed_check(pcie, 0x1904, tmp);
	_W(0x212c, 15, 8, tmp);
	_W(0x212c, 1, 1, 0x1);
	_W(0x212e, 31, 0, 0x0c16);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a4, 6, 0, tmp);
	_W(0x2122, 0, 0, 0x1);
	_W(0x212e, 31, 0, 0x0c17);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a4, 14, 8, tmp);
	_W(0x2122, 1, 1, 0x1);
	_W(0x212e, 31, 0, 0x0c18);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a6, 6, 0, tmp);
	_W(0x2122, 2, 2, 0x1);
	_W(0x212e, 31, 0, 0x0c19);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a6, 14, 8, tmp);
	_W(0x2122, 3, 3, 0x1);
	_W(0x212e, 31, 0, 0x0c1a);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a8, 6, 0, tmp);
	_W(0x2122, 4, 4, 0x1);
	_W(0x212e, 31, 0, 0x0c1b);
	tmp = _R(0x1904, 6, 0);
	_W(0x21a8, 14, 8, tmp);
	_W(0x2122, 5, 5, 0x1);
	_W(0x212e, 31, 0, 0x0c1c);
	tmp = _R(0x1904, 6, 0);
	_W(0x21aa, 6, 0, tmp);
	_W(0x2122, 6, 6, 0x1);
	_W(0x212e, 31, 0, 0x0c1d);
	tmp = _R(0x1904, 6, 0);
	_W(0x21aa, 14, 8, tmp);
	_W(0x2122, 7, 7, 0x1);
//	printk("%s(%d) Leave\n",__func__,__LINE__);
}

static void s0_s1_phy_cali_gen2(const struct ca_pcie *pcie)
{
	u32 tmp;

//	printk("%s(%d) Enter\n",__func__,__LINE__);
	_W(0x1c5c, 31, 0, 0x8085);
	_W(0x241e, 31, 0, 0x5059);
	_W(0x251e, 31, 0, 0x5059);
	_W(0x1c48, 31, 0, 0x8185);
	_W(0x1c48, 31, 0, 0x818f);
	_W(0x241e, 31, 0, 0x7059);
	_W(0x251e, 31, 0, 0x7059);
	_W(0x242e, 31, 0, 0x0c1e);
	_W(0x252e, 31, 0, 0x0c1e);
	while (_R(0x1804, 2, 2) != 0x1)
		;

	while (_R(0x1904, 2, 2) != 0x1)
		;

	_W(0x242e, 31, 0, 0x0c11);
	tmp = _R(0x1804, 4, 0);
	_W(0x241c, 12, 8, tmp);
	_W(0x241c, 2, 2, 0x1);
	_W(0x242e, 31, 0, 0x0c12);
	tmp = _R(0x1804, 4, 0);
	_W(0x241e, 12, 8, tmp);
	_W(0x241e, 2, 2, 0x1);
	_W(0x242e, 31, 0, 0x0c13);
	tmp = _R(0x1804, 7, 0);
	tmp = signed_check(pcie, 0x1804, tmp);
	_W(0x242c, 15, 8, tmp);
	_W(0x242c, 1, 1, 0x1);
	_W(0x242e, 31, 0, 0x0c16);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a4, 6, 0, tmp);
	_W(0x2422, 0, 0, 0x1);
	_W(0x242e, 31, 0, 0x0c17);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a4, 14, 8, tmp);
	_W(0x2422, 1, 1, 0x1);
	_W(0x242e, 31, 0, 0x0c18);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a6, 6, 0, tmp);
	_W(0x2422, 2, 2, 0x1);
	_W(0x242e, 31, 0, 0x0c19);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a6, 14, 8, tmp);
	_W(0x2422, 3, 3, 0x1);
	_W(0x242e, 31, 0, 0x0c1a);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a8, 6, 0, tmp);
	_W(0x2422, 4, 4, 0x1);
	_W(0x242e, 31, 0, 0x0c1b);
	tmp = _R(0x1804, 6, 0);
	_W(0x24a8, 14, 8, tmp);
	_W(0x2422, 5, 5, 0x1);
	_W(0x242e, 31, 0, 0x0c1c);
	tmp = _R(0x1804, 6, 0);
	_W(0x24aa, 6, 0, tmp);
	_W(0x2422, 6, 6, 0x1);
	_W(0x242e, 31, 0, 0x0c1d);
	tmp = _R(0x1804, 6, 0);
	_W(0x24aa, 14, 8, tmp);
	_W(0x2422, 7, 7, 0x1);
	_W(0x252e, 31, 0, 0x0c11);
	tmp = _R(0x1904, 4, 0);
	_W(0x251c, 12, 8, tmp);
	_W(0x251c, 2, 2, 0x1);
	_W(0x252e, 31, 0, 0x0c12);
	tmp = _R(0x1904, 4, 0);
	_W(0x251e, 12, 8, tmp);
	_W(0x251e, 2, 2, 0x1);
	_W(0x252e, 31, 0, 0x0c13);
	tmp = _R(0x1904, 7, 0);
	tmp = signed_check(pcie, 0x1904, tmp);
	_W(0x252c, 15, 8, tmp);
	_W(0x252c, 1, 1, 0x1);
	_W(0x252e, 31, 0, 0x0c16);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a4, 6, 0, tmp);
	_W(0x2522, 0, 0, 0x1);
	_W(0x252e, 31, 0, 0x0c17);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a4, 14, 8, tmp);
	_W(0x2522, 1, 1, 0x1);
	_W(0x252e, 31, 0, 0x0c18);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a6, 6, 0, tmp);
	_W(0x2522, 2, 2, 0x1);
	_W(0x252e, 31, 0, 0x0c19);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a6, 14, 8, tmp);
	_W(0x2522, 3, 3, 0x1);
	_W(0x252e, 31, 0, 0x0c1a);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a8, 6, 0, tmp);
	_W(0x2522, 4, 4, 0x1);
	_W(0x252e, 31, 0, 0x0c1b);
	tmp = _R(0x1904, 6, 0);
	_W(0x25a8, 14, 8, tmp);
	_W(0x2522, 5, 5, 0x1);
	_W(0x252e, 31, 0, 0x0c1c);
	tmp = _R(0x1904, 6, 0);
	_W(0x25aa, 6, 0, tmp);
	_W(0x2522, 6, 6, 0x1);
	_W(0x252e, 31, 0, 0x0c1d);
	tmp = _R(0x1904, 6, 0);
	_W(0x25aa, 14, 8, tmp);
	_W(0x2522, 7, 7, 0x1);
//	printk("%s(%d) Leave\n",__func__,__LINE__);
}

static void s0_s1_phy_cali_gen3(const struct ca_pcie *pcie)
{
	u32 tmp;

//	printk("%s(%d) Enter\n",__func__,__LINE__);
	_W(0x1c5c, 31, 0, 0x8086);
	_W(0x281e, 31, 0, 0x5059);
	_W(0x291e, 31, 0, 0x5059);
	_W(0x1c48, 31, 0, 0x8185);
	_W(0x1c48, 31, 0, 0x818f);
	_W(0x281e, 31, 0, 0x7059);
	_W(0x291e, 31, 0, 0x7059);
	_W(0x282e, 31, 0, 0x0c1e);
	_W(0x292e, 31, 0, 0x0c1e);
	while (_R(0x1804, 2, 2) != 0x1)
		;

	while (_R(0x1904, 2, 2) != 0x1)
		;

	_W(0x282e, 31, 0, 0x0c11);
	tmp = _R(0x1804, 4, 0);
	_W(0x281c, 12, 8, tmp);
	_W(0x281c, 2, 2, 0x1);
	_W(0x282e, 31, 0, 0x0c12);
	tmp = _R(0x1804, 4, 0);
	_W(0x281e, 12, 8, tmp);
	_W(0x281e, 2, 2, 0x1);
	_W(0x282e, 31, 0, 0x0c13);
	tmp = _R(0x1804, 7, 0);
	tmp = signed_check(pcie, 0x1804, tmp);
	_W(0x282c, 15, 8, tmp);
	_W(0x282c, 1, 1, 0x1);
	_W(0x282e, 31, 0, 0x0c16);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a4, 6, 0, tmp);
	_W(0x2822, 0, 0, 0x1);
	_W(0x282e, 31, 0, 0x0c17);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a4, 14, 8, tmp);
	_W(0x2822, 1, 1, 0x1);
	_W(0x282e, 31, 0, 0x0c18);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a6, 6, 0, tmp);
	_W(0x2822, 2, 2, 0x1);
	_W(0x282e, 31, 0, 0x0c19);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a6, 14, 8, tmp);
	_W(0x2822, 3, 3, 0x1);
	_W(0x282e, 31, 0, 0x0c1a);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a8, 6, 0, tmp);
	_W(0x2822, 4, 4, 0x1);
	_W(0x282e, 31, 0, 0x0c1b);
	tmp = _R(0x1804, 6, 0);
	_W(0x28a8, 14, 8, tmp);
	_W(0x2822, 5, 5, 0x1);
	_W(0x282e, 31, 0, 0x0c1c);
	tmp = _R(0x1804, 6, 0);
	_W(0x28aa, 6, 0, tmp);
	_W(0x2822, 6, 6, 0x1);
	_W(0x282e, 31, 0, 0x0c1d);
	tmp = _R(0x1804, 6, 0);
	_W(0x28aa, 14, 8, tmp);
	_W(0x2822, 7, 7, 0x1);
	_W(0x292e, 31, 0, 0x0c11);
	tmp = _R(0x1904, 4, 0);
	_W(0x291c, 12, 8, tmp);
	_W(0x291c, 2, 2, 0x1);
	_W(0x292e, 31, 0, 0x0c12);
	tmp = _R(0x1904, 4, 0);
	_W(0x291e, 12, 8, tmp);
	_W(0x291e, 2, 2, 0x1);
	_W(0x292e, 31, 0, 0x0c13);
	tmp = _R(0x1904, 7, 0);
	tmp = signed_check(pcie, 0x1904, tmp);
	_W(0x292c, 15, 8, tmp);
	_W(0x292c, 1, 1, 0x1);
	_W(0x292e, 31, 0, 0x0c16);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a4, 6, 0, tmp);
	_W(0x2922, 0, 0, 0x1);
	_W(0x292e, 31, 0, 0x0c17);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a4, 14, 8, tmp);
	_W(0x2922, 1, 1, 0x1);
	_W(0x292e, 31, 0, 0x0c18);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a6, 6, 0, tmp);
	_W(0x2922, 2, 2, 0x1);
	_W(0x292e, 31, 0, 0x0c19);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a6, 14, 8, tmp);
	_W(0x2922, 3, 3, 0x1);
	_W(0x292e, 31, 0, 0x0c1a);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a8, 6, 0, tmp);
	_W(0x2922, 4, 4, 0x1);
	_W(0x292e, 31, 0, 0x0c1b);
	tmp = _R(0x1904, 6, 0);
	_W(0x29a8, 14, 8, tmp);
	_W(0x2922, 5, 5, 0x1);
	_W(0x292e, 31, 0, 0x0c1c);
	tmp = _R(0x1904, 6, 0);
	_W(0x29aa, 6, 0, tmp);
	_W(0x2922, 6, 6, 0x1);
	_W(0x292e, 31, 0, 0x0c1d);
	tmp = _R(0x1904, 6, 0);
	_W(0x29aa, 14, 8, tmp);
	_W(0x2922, 7, 7, 0x1);
//	printk("%s(%d) Leave\n",__func__,__LINE__);
}

static void s0_s1_phy_cali_gen4(const struct ca_pcie *pcie)
{
	u32 tmp;

//	printk("%s(%d) Enter\n",__func__,__LINE__);
	_W(0x1c5c, 31, 0, 0x8087);
	_W(0x2c1e, 31, 0, 0x5059);
	_W(0x2d1e, 31, 0, 0x5059);
	_W(0x1c48, 31, 0, 0x8185);
	_W(0x1c48, 31, 0, 0x818f);
	_W(0x2c1e, 31, 0, 0x7059);
	_W(0x2d1e, 31, 0, 0x7059);
	_W(0x2c2e, 31, 0, 0x0c1e);
	_W(0x2d2e, 31, 0, 0x0c1e);
	while (_R(0x1804, 2, 2) != 0x1)
		;

	while (_R(0x1904, 2, 2) != 0x1)
		;

	_W(0x2c2e, 31, 0, 0x0c11);
	tmp = _R(0x1804, 4, 0);
	_W(0x2c1c, 12, 8, tmp);
	_W(0x2c1c, 2, 2, 0x1);
	_W(0x2c2e, 31, 0, 0x0c12);
	tmp = _R(0x1804, 4, 0);
	_W(0x2c1e, 12, 8, tmp);
	_W(0x2c1e, 2, 2, 0x1);
	_W(0x2c2e, 31, 0, 0x0c13);
	tmp = _R(0x1804, 7, 0);
	tmp = signed_check(pcie, 0x1804, tmp);
	_W(0x2c2c, 15, 8, tmp);
	_W(0x2c2c, 1, 1, 0x1);
	_W(0x2c2e, 31, 0, 0x0c16);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca4, 6, 0, tmp);
	_W(0x2c22, 0, 0, 0x1);
	_W(0x2c2e, 31, 0, 0x0c17);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca4, 14, 8, tmp);
	_W(0x2c22, 1, 1, 0x1);
	_W(0x2c2e, 31, 0, 0x0c18);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca6, 6, 0, tmp);
	_W(0x2c22, 2, 2, 0x1);
	_W(0x2c2e, 31, 0, 0x0c19);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca6, 14, 8, tmp);
	_W(0x2c22, 3, 3, 0x1);
	_W(0x2c2e, 31, 0, 0x0c1a);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca8, 6, 0, tmp);
	_W(0x2c22, 4, 4, 0x1);
	_W(0x2c2e, 31, 0, 0x0c1b);
	tmp = _R(0x1804, 6, 0);
	_W(0x2ca8, 14, 8, tmp);
	_W(0x2c22, 5, 5, 0x1);
	_W(0x2c2e, 31, 0, 0x0c1c);
	tmp = _R(0x1804, 6, 0);
	_W(0x2caa, 6, 0, tmp);
	_W(0x2c22, 6, 6, 0x1);
	_W(0x2c2e, 31, 0, 0x0c1d);
	tmp = _R(0x1804, 6, 0);
	_W(0x2caa, 14, 8, tmp);
	_W(0x2c22, 7, 7, 0x1);
	_W(0x2d2e, 31, 0, 0x0c11);
	tmp = _R(0x1904, 4, 0);
	_W(0x2d1c, 12, 8, tmp);
	_W(0x2d1c, 2, 2, 0x1);
	_W(0x2d2e, 31, 0, 0x0c12);
	tmp = _R(0x1904, 4, 0);
	_W(0x2d1e, 12, 8, tmp);
	_W(0x2d1e, 2, 2, 0x1);
	_W(0x2d2e, 31, 0, 0x0c13);
	tmp = _R(0x1904, 7, 0);
	tmp = signed_check(pcie, 0x1904, tmp);
	_W(0x2d2c, 15, 8, tmp);
	_W(0x2d2c, 1, 1, 0x1);
	_W(0x2d2e, 31, 0, 0x0c16);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da4, 6, 0, tmp);
	_W(0x2d22, 0, 0, 0x1);
	_W(0x2d2e, 31, 0, 0x0c17);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da4, 14, 8, tmp);
	_W(0x2d22, 1, 1, 0x1);
	_W(0x2d2e, 31, 0, 0x0c18);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da6, 6, 0, tmp);
	_W(0x2d22, 2, 2, 0x1);
	_W(0x2d2e, 31, 0, 0x0c19);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da6, 14, 8, tmp);
	_W(0x2d22, 3, 3, 0x1);
	_W(0x2d2e, 31, 0, 0x0c1a);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da8, 6, 0, tmp);
	_W(0x2d22, 4, 4, 0x1);
	_W(0x2d2e, 31, 0, 0x0c1b);
	tmp = _R(0x1904, 6, 0);
	_W(0x2da8, 14, 8, tmp);
	_W(0x2d22, 5, 5, 0x1);
	_W(0x2d2e, 31, 0, 0x0c1c);
	tmp = _R(0x1904, 6, 0);
	_W(0x2daa, 6, 0, tmp);
	_W(0x2d22, 6, 6, 0x1);
	_W(0x2d2e, 31, 0, 0x0c1d);
	tmp = _R(0x1904, 6, 0);
	_W(0x2daa, 14, 8, tmp);
	_W(0x2d22, 7, 7, 0x1);
}

static void s0_s1_phy_cali_end(const struct ca_pcie *pcie)
{
	_W(0x1c48, 31, 0, 0x8180);
	_W(0x1c4a, 31, 0, 0x0000);
	_W(0x1c4c, 31, 0, 0x0000);
	_W(0x1c5c, 31, 0, 0x8080);
}

static void s2_phy_cali_start(const struct ca_pcie *pcie)
{
	_W(0x1c3e, 31, 0, 0x0007);
}

static void s2_phy_cali_gen1(const struct ca_pcie *pcie)
{
	u32 tmp;

	_W(0x1f00, 31, 0, 0x000d);
	_W(0x1f02, 31, 0, 0x0000);
	_W(0x1f04, 31, 0, 0x0000);
	_W(0x1f02, 31, 0, 0xfc01);
	_W(0x1f04, 31, 0, 0x00fc);
	_W(0x1f00, 31, 0, 0x000f);
	_W(0x1c44, 31, 0, 0x0002);
	_W(0x1c46, 31, 0, 0x0002);
	while (_R(0x1c98, 7, 7) != 0x1)
		;

	_W(0x102e, 31, 0, 0x0002);
	tmp = _R(0x1030, 12, 8);
	_W(0x1126, 12, 8, tmp);
	_W(0x1126, 13, 13, 0x1);
	_W(0x102e, 31, 0, 0x0003);
	tmp = _R(0x1030, 12, 8);
	_W(0x1126, 4, 0, tmp);
	_W(0x1126, 5, 5, 0x1);
	_W(0x102e, 31, 0, 0x0004);
	tmp = _R(0x1030, 14, 8);
	_W(0x1228, 6, 0, tmp);
	_W(0x1228, 8, 8, 0x1);
	_W(0x102e, 31, 0, 0x0005);
	tmp = _R(0x1030, 14, 8);
	_W(0x1128, 6, 0, tmp);
	_W(0x1128, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0000);
	tmp = _R(0x1030, 15, 8);
	_W(0x112e, 7, 0, tmp);
	_W(0x112e, 10, 10, 0x1);
	_W(0x102e, 31, 0, 0x0014);
	tmp = _R(0x1030, 14, 8);
	_W(0x1125, 6, 0, tmp);
	_W(0x1125, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0015);
	tmp = _R(0x1030, 14, 8);
	_W(0x1127, 6, 0, tmp);
	_W(0x1127, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0016);
	tmp = _R(0x1030, 14, 8);
	_W(0x1129, 6, 0, tmp);
	_W(0x1129, 7, 7, 0x1);
}

static void s2_phy_cali_gen2(const struct ca_pcie *pcie)
{
	u32 tmp;

	_W(0x1f00, 31, 0, 0x000d);
	_W(0x1f02, 31, 0, 0x0000);
	_W(0x1f04, 31, 0, 0x0000);
	_W(0x1f02, 31, 0, 0xfc01);
	_W(0x1f04, 31, 0, 0x00fc);
	_W(0x1f00, 31, 0, 0x000f);
	_W(0x1c44, 31, 0, 0x0002);
	_W(0x1c46, 31, 0, 0x0002);
	while (_R(0x1c98, 7, 7) != 0x1)
		;

	_W(0x102e, 31, 0, 0x0002);
	tmp = _R(0x1030, 12, 8);
	_W(0x1156, 12, 8, tmp);
	_W(0x1156, 13, 13, 0x1);
	_W(0x102e, 31, 0, 0x0003);
	tmp = _R(0x1030, 12, 8);
	_W(0x1156, 4, 0, tmp);
	_W(0x1156, 5, 5, 0x1);
	_W(0x102e, 31, 0, 0x0004);
	tmp = _R(0x1030, 14, 8);
	_W(0x1258, 6, 0, tmp);
	_W(0x1258, 8, 8, 0x1);
	_W(0x102e, 31, 0, 0x0005);
	tmp = _R(0x1030, 14, 8);
	_W(0x1158, 6, 0, tmp);
	_W(0x1158, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0000);
	tmp = _R(0x1030, 15, 8);
	_W(0x115e, 7, 0, tmp);
	_W(0x115e, 10, 10, 0x1);
	_W(0x102e, 31, 0, 0x0014);
	tmp = _R(0x1030, 14, 8);
	_W(0x1155, 6, 0, tmp);
	_W(0x1155, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0015);
	tmp = _R(0x1030, 14, 8);
	_W(0x1157, 6, 0, tmp);
	_W(0x1157, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0016);
	tmp = _R(0x1030, 14, 8);
	_W(0x1159, 6, 0, tmp);
	_W(0x1159, 7, 7, 0x1);
}

static void s2_phy_cali_gen3(const struct ca_pcie *pcie)
{
	u32 tmp;

	_W(0x1f00, 31, 0, 0x000d);
	_W(0x1f02, 31, 0, 0x0000);
	_W(0x1f04, 31, 0, 0x0000);
	_W(0x1f02, 31, 0, 0xfc01);
	_W(0x1f04, 31, 0, 0x00fc);
	_W(0x1f00, 31, 0, 0x000f);
	_W(0x1c44, 31, 0, 0x0002);
	_W(0x1c46, 31, 0, 0x0002);
	while (_R(0x1c98, 7, 7) != 0x1)
		;

	_W(0x102e, 31, 0, 0x0002);
	tmp = _R(0x1030, 12, 8);
	_W(0x1186, 12, 8, tmp);
	_W(0x1186, 13, 13, 0x1);
	_W(0x102e, 31, 0, 0x0003);
	tmp = _R(0x1030, 12, 8);
	_W(0x1186, 4, 0, tmp);
	_W(0x1186, 5, 5, 0x1);
	_W(0x102e, 31, 0, 0x0004);
	tmp = _R(0x1030, 14, 8);
	_W(0x1288, 6, 0, tmp);
	_W(0x1288, 8, 8, 0x1);
	_W(0x102e, 31, 0, 0x0005);
	tmp = _R(0x1030, 14, 8);
	_W(0x1188, 6, 0, tmp);
	_W(0x1188, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0000);
	tmp = _R(0x1030, 15, 8);
	_W(0x118e, 7, 0, tmp);
	_W(0x118e, 10, 10, 0x1);
	_W(0x102e, 31, 0, 0x0014);
	tmp = _R(0x1030, 14, 8);
	_W(0x1185, 6, 0, tmp);
	_W(0x1185, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0015);
	tmp = _R(0x1030, 14, 8);
	_W(0x1187, 6, 0, tmp);
	_W(0x1187, 7, 7, 0x1);
	_W(0x102e, 31, 0, 0x0016);
	tmp = _R(0x1030, 14, 8);
	_W(0x1189, 6, 0, tmp);
	_W(0x1189, 7, 7, 0x1);
}

static void s2_phy_cali_end(const struct ca_pcie *pcie)
{
	_W(0x1c40, 31, 0, 0x0001);
	_W(0x1f02, 31, 0, 0x0000);
	_W(0x1f04, 31, 0, 0x0000);
	_W(0x1f00, 31, 0, 0x0001);
	_W(0x1f00, 31, 0, 0x0000);
	_W(0x1c3e, 31, 0, 0x8007);
}

static void s7_s8_phy_cali(const struct ca_pcie *pcie)
{
		u32 tmp;

		_W(0x1a, 9, 9, 0x0);
		_W(0x1a, 5, 4, 0x1);
		_W(0x13, 31, 0, 0x049c);
		_W(0x13, 31, 0, 0x0c9c);
		_W(0x13, 31, 0, 0x8c9c);
		_W(0x13, 31, 0, 0xcc9c);
		_W(0x09, 9, 9, 0x0);
		_W(0x09, 9, 9, 0x1);
		usleep_range(1000, 2000);
		tmp = _R(0x1f, 4, 1);
		_W(0x0b, 8, 5, tmp);
		_W(0x0d, 13, 13, 0x0);
		_W(0x8b, 8, 5, tmp);
		_W(0x8d, 13, 13, 0x0);
		_W(0x1a, 9, 9, 0x0);
		_W(0x1a, 5, 4, 0x1);
		_W(0x13, 31, 0, 0x049c);
		_W(0x13, 31, 0, 0x0c9c);
		_W(0x13, 31, 0, 0x8c9c);
		_W(0x13, 31, 0, 0xcc9c);
		_W(0x09, 9, 9, 0x0);
		_W(0x09, 9, 9, 0x1);
		usleep_range(1000, 2000);
		_W(0x0d, 6, 6, 0x1);
		_W(0x19, 2, 2, 0x1);
		_W(0x10, 31, 0, 0x03c4);
		tmp = _R(0x1f, 12, 8);
		_W(0x03, 5, 1, tmp);
		_W(0x09, 4, 4, 0x1);
		_W(0x83, 5, 1, tmp);
		_W(0x89, 4, 4, 0x1);
		_W(0x19, 2, 2, 0x1);
		_W(0x10, 31, 0, 0x0354);
		_W(0x99, 2, 2, 0x1);
		_W(0x10, 31, 0, 0x0354);
		_W(0x0a, 10, 10, 0x1);
		_W(0x07, 10, 10, 0x0);
		_W(0x07, 10, 10, 0x1);
		usleep_range(1000, 2000);
		tmp = _R(0x1f, 15, 10);
		_W(0x44, 15, 10, tmp);
		_W(0x8, 3, 3, 0x0);
		_W(0xc4, 15, 10, tmp);
		_W(0x88, 3, 3, 0x0);
		_W(0x13, 31, 0, 0x0c81);
		_W(0x1a, 9, 9, 0x1);
		_W(0x19, 2, 2, 0x0);
	   _W(0x0a, 10, 10, 0x0);
}

static int pcie_tx_matrix_patch(struct ca_pcie *pcie)
{
	int i;
	u16 val;

	if (!pcie->txm_data)
		return 0;
	for (i = 0; i < pcie->txm_data->size; i++) {
		if (i % 3 != 0) {
			val = pcie->txm_data->val[i] & 0x3ff;
			_W(pcie->txm_data->ephy_addr[i % 3], 9, 0, val);
		} else {
			val = pcie->txm_data->val[i] & 0xffff;
			_W(pcie->txm_data->ephy_addr[i % 3], 15, 0, val);
		}
		usleep_range(1000, 2000);
	}
	return 0;
}

#undef _W
#undef _R
static void serdes_phy_calibration(struct ca_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;

	switch (pcie->idx) {
	case 0:
	case 1:
			s0_s1_phy_cali_start(pcie);
			s0_s1_phy_cali_gen1(pcie);
			s0_s1_phy_cali_gen2(pcie);
			s0_s1_phy_cali_gen3(pcie);
			s0_s1_phy_cali_gen4(pcie);
			s0_s1_phy_cali_end(pcie);
		break;
	case 2:
			s2_phy_cali_start(pcie);
			s2_phy_cali_gen1(pcie);
			s2_phy_cali_gen2(pcie);
			s2_phy_cali_gen3(pcie);
			s2_phy_cali_end(pcie);
		break;
	case 7:
	case 8:
			s7_s8_phy_cali(pcie);
		break;
	default:
			dev_info(pci->dev, "%s: No SERDES offset calibration flow\n", __func__);
		break;
	}
}

static void __serdes_probe_chip_rev_aware(struct device *dev,
					  struct device_node *np,
					  struct ca_pcie *pcie)
{
	int rev, lane, size, cnt, ret;

	rev = REV_A;

	//dev_info(dev, "Chip revision is %d using params %s\n", soc.chip_revision,
	//cfg_ver_str[rev]);
	dev_info(dev, "Chip revision is %d using params %s\n", 'A', cfg_ver_str[rev]);

	ret = of_property_read_string(np, cfg_ver_str[rev], &pcie->serdes_cfg_ver);
	if (ret)
		pcie->serdes_cfg_ver = CA_PCIE_SERDES_CFG_VER_UNKNOWN;

	size = sizeof(struct serdes_cfg);

	for (lane = 0; lane < 1; lane++) {
		pcie->cfg_cnt[lane] = of_property_count_elems_of_size(np, cfg_data_str[rev], size);
		if (pcie->cfg_cnt[lane] < 1) {
			pcie->cfg_cnt[lane] = 0;
			continue;
		}

		pcie->cfg[lane] = devm_kmalloc_array(dev, pcie->cfg_cnt[lane], size,
						     GFP_KERNEL);
		cnt = pcie->cfg_cnt[lane] * size / sizeof(u32);
		of_property_read_u32_array(np, cfg_data_str[rev], (u32 *)pcie->cfg[lane], cnt);
	}

	//pre-processing for DTS format compatibility
	for (lane = 0; lane < pcie->lanes; lane++) {
		if (pcie->cfg_cnt[lane] <= 0 || !pcie->cfg[lane])
			continue;
	}
}

static void mac_pset_probe(struct device *dev, struct device_node *np,
			   struct ca_pcie *pcie)
{
	int size, cnt;

	size = sizeof(struct mac_pset_cfg);
	pcie->pset_cfg_cnt = of_property_count_elems_of_size(np, "mac_preset_cfg", size);
	pcie->pset_cfg = devm_kmalloc_array(dev, pcie->pset_cfg_cnt, size, GFP_KERNEL);
	cnt = pcie->pset_cfg_cnt * size / sizeof(u32);
	of_property_read_u32_array(np, "mac_preset_cfg", (u32 *)pcie->pset_cfg, cnt);
}

static int pcie_tx_matrix_probe(struct device *dev, struct device_node *np,
				struct ca_pcie *pcie)
{
	struct tx_matrix_data *tx_d;
	int ret = -1;

	tx_d = devm_kzalloc(dev, sizeof(*tx_d), GFP_KERNEL);
	if (IS_ERR(tx_d))
		return -ENOMEM;
	//ret = of_property_read_u32_index(np, "tx_data_size", 0, &tx_d->size);
	ret = of_property_read_u32(np, "tx_data_size", &tx_d->size);
	dev_err(dev, "data probe size: (%d)\n", tx_d->size);
	if (ret)
		goto err;

	tx_d->val = devm_kzalloc(dev, sizeof(u16) * tx_d->size, GFP_KERNEL);
	if (!tx_d->val)
		return -ENOMEM;
	ret = of_property_read_u16_array(np, "tx_matrix_data", tx_d->val, tx_d->size);
	if (ret)
		goto err;

	ret = of_property_read_u32(np, "ephy_cnt", &tx_d->ephy_cnt);
	dev_err(dev, "data probe size: (%d)\n", tx_d->ephy_cnt);
	if (ret)
		goto err;

	tx_d->ephy_addr = devm_kzalloc(dev, sizeof(u16) * tx_d->ephy_cnt, GFP_KERNEL);
	if (!tx_d->ephy_addr)
		return -ENOMEM;
	ret = of_property_read_u16_array(np, "ephy_addr", tx_d->ephy_addr, tx_d->ephy_cnt);
	if (ret)
		goto err;

	goto done;
err:
	dev_err(dev, "ERROR in TX matrix data probe (%d)\n", ret);
	tx_d->size = 0;
	tx_d->ephy_cnt = 0;
done:
	pcie->txm_data = tx_d;
	return ret;
}

static void ca_serdes_probe(struct device *dev, struct device_node *np,
			    struct ca_pcie *pcie)
{
	int i, size, cnt, ret;
	char name[MAX_NAME_LEN];

	ret = of_property_read_string(np, "serdes-cfg-ver", &pcie->serdes_cfg_ver);
	if (ret) {
		//probably the DTS is the version of chip revision awared
		return __serdes_probe_chip_rev_aware(dev, np, pcie);
	}

	size = sizeof(struct serdes_cfg);

	for (i = 0; i < pcie->lanes; i++) {
		snprintf(name, MAX_NAME_LEN, "serdes-cfg%d", i);
		pcie->cfg_cnt[i] = of_property_count_elems_of_size(np, name, size);
		if (pcie->cfg_cnt[i] < 1) {
			pcie->cfg_cnt[i] = 0;
			continue;
		}

		pcie->cfg[i] = devm_kmalloc_array(dev, pcie->cfg_cnt[i], size,
						  GFP_KERNEL);
		cnt = pcie->cfg_cnt[i] * size / sizeof(u32);
		of_property_read_u32_array(np, name, (u32 *)pcie->cfg[i], cnt);
	}
}

static int ca_pcie_reboot_notifier(struct notifier_block *nb,
				   unsigned long code, void *data)
{
	return NOTIFY_DONE;
}

static int ca_pcie_probe(struct platform_device *pdev)
{
	struct dw_pcie *pci;
	struct ca_pcie *pcie;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *reg_res;
	int lanes, i, ret;
	char name[MAX_NAME_LEN];
	const char *pcie_name = NULL;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &ca_pcie_ops;

	pcie->pci = pci;
	raw_spin_lock_init(&pcie->lock);

	reg_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					       "glbl_regs");
	pcie->reg_base = devm_ioremap_resource(dev, reg_res);
	if (IS_ERR(pcie->reg_base)) {
		ret = PTR_ERR(pcie->reg_base);
		goto fail_bus_clk;
	}
	dev_info(dev, "resource - %pr mapped at 0x%llx\n", reg_res,
		 (u64)pcie->reg_base);
	if (of_property_read_u32(np, "id", &pcie->idx)) {
		dev_err(dev, "missing id property\n");
		goto fail_bus_clk;
	}
	dev_info(dev, "id %d\n", pcie->idx);
	ret = of_property_read_u32(np, "num-lanes", &lanes);
	if (ret || lanes < 1 || lanes > MAX_LANE_NUM)
		pcie->lanes = 1;
	else
		pcie->lanes = lanes;
	dev_info(dev, "num-lanes %d\n", pcie->lanes);
	reg_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					       "serdes_phy");
	pcie->serdes_base = devm_ioremap_resource(dev, reg_res);
	if (IS_ERR(pcie->serdes_base)) {
		ret = PTR_ERR(pcie->serdes_base);
		goto fail_bus_clk;
	}
	dev_info(dev, "resource - %pr mapped at 0x%llx\n", reg_res,
		 (u64)pcie->serdes_base);
	pcie->serdes_addr = reg_res->start;
	pcie->serdes_size = resource_size(reg_res);

	for (i = 0; i < MAX_LANE_NUM; i++)
		pcie->cfg[i] = NULL;

	ca_serdes_probe(dev, np, pcie);
	//serdes_cal_probe(dev, np, pcie);
	if (pcie->idx <= 2) {
		if (of_property_read_u32(np, "mac_pset_fs", &pcie->pset_fs))
			dev_err(dev, "missing mac_pset_fs property\n");
		if (of_property_read_u32(np, "mac_pset_lf", &pcie->pset_lf))
			dev_err(dev, "missing mac_pset_lf property\n");
		mac_pset_probe(dev, np, pcie);
		pcie_tx_matrix_probe(dev, np, pcie);
	}

	pcie->core_reset = of_reset_control_get(np, "core_reset");
	if (IS_ERR(pcie->core_reset)) {
		dev_err(dev, "failed to get core_reset, NODE(%s)\n", np->full_name);
		ret = PTR_ERR(pcie->core_reset);
		goto fail_bus_clk;
	}

	pcie->phy_reset = of_reset_control_get(np, "phy_reset");
	if (IS_ERR(pcie->phy_reset)) {
		dev_err(dev, "failed to get phy_reset, NODE(%s)\n", np->full_name);
		ret = PTR_ERR(pcie->phy_reset);
		goto fail_bus_clk;
	}

	if (pcie->idx >= 2) {
		snprintf(name, MAX_NAME_LEN, "pcie-phy");
		pcie->phy = devm_phy_get(&pdev->dev, name);
		if (IS_ERR(pcie->phy)) {
			ret = PTR_ERR(pcie->phy);
			pcie->phy = NULL;
		}

		for (i = 0; i < pcie->lanes; i++) {
			if (!pcie->phy) {
				dev_err(dev, "failed to get phy %d\n", i);
				goto fail_bus_clk;
			}
		}
	}

	pcie->gpio_reset = devm_gpiod_get(dev, "reset", 0);
	if (IS_ERR(pcie->gpio_reset)) {
		dev_err(dev, "Failed to get reset-gpios\n");
		goto fail_bus_clk;
	}
	gpiod_direction_output(pcie->gpio_reset, 0);

	pcie->reboot_nb.notifier_call = ca_pcie_reboot_notifier;
	ret = register_reboot_notifier(&pcie->reboot_nb);
	if (ret)
		dev_warn(dev, "Cannot register reboot notifier (%d)\n", ret);

	ret = of_property_read_u16(np, "ready-time",
				   &pcie->device_ready_time);
	if (ret)
		pcie->device_ready_time = 0;

	platform_set_drvdata(pdev, pcie);

	if (of_property_read_bool(np, "forced-gen1"))
		pci->link_gen = PCI_EXP_LNKCTL2_TLS_2_5GT;
	if (of_property_read_string(np, "pdev-name", &pcie_name) == 0)
		kobject_set_name(&pci->dev->kobj, "%s", pcie_name);

	ret = ca_add_pcie_port(pcie, pdev);

	ret = sysfs_create_group(&pci->dev->kobj,
				 &ca_pcie_attr_group);
	if (ret) {
		dev_err(dev, "failed to register sysfs\n");
		goto fail_bus_clk;
	}
	return 0;

fail_bus_clk:
	//create sysfs for debug
	ret = sysfs_create_group(&pci->dev->kobj,
				 &ca_pcie_dbg_attr_group);
	if (ret)
		dev_err(dev, "failed to register sysfs for debug\n");

	if (pcie->reboot_nb.notifier_call)
		unregister_reboot_notifier(&pcie->reboot_nb);

	if (pcie->phy_reset)
		reset_control_put(pcie->phy_reset);
	if (pcie->core_reset)
		reset_control_put(pcie->core_reset);
	//	for (i = 0; i < pcie->lanes; i++)
	//		phy_power_off(pcie->phy[i]);

	return ret;
}

static int __exit ca_pcie_remove(struct platform_device *pdev)
{
	struct ca_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;

	sysfs_remove_group(&pdev->dev.kobj, &ca_pcie_attr_group);

	dw_pcie_host_deinit(pp);

	flush_work(&pcie->work);

	if (pcie->reboot_nb.notifier_call)
		unregister_reboot_notifier(&pcie->reboot_nb);

	return 0;
}

static const struct of_device_id ca_pcie_of_match[] = {
	{
		.compatible = "cortina-access,mercury-pcie",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ca_pcie_of_match);

static struct platform_driver ca_pcie_driver = {
	.probe		= ca_pcie_probe,
	.remove		= ca_pcie_remove,
	.driver = {
		.name	= "ca-pcie",
		.of_match_table = of_match_ptr(ca_pcie_of_match),
	},
};

static int __init ca_pcie_init(void)
{
	return platform_driver_register(&ca_pcie_driver);
}
late_initcall(ca_pcie_init);

static void __exit ca_pcie_exit(void)
{
	platform_driver_unregister(&ca_pcie_driver);
}
module_exit(ca_pcie_exit);

MODULE_DESCRIPTION("Cortina-Access PCIe host controller driver");
MODULE_LICENSE("GPL");
