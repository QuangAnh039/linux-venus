// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Cortina-Access SoCs
 *
 * Copyright (C) 2020 Cortina Access, Inc.
 *		http://www.cortina-access.com
 * Author: Arthur Li <arthur.li@cortina-access.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/reset.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#include "pcie-designware.h"

#define to_ca_pcie(x)	dev_get_drvdata((x)->dev)

#define MAX_INTX_HOST_IRQS	4
#define MAX_BER_POLL_COUNT	50
#define MAX_LINKUP_POLL_COUNT	500
#define MAX_LANE_NUM		2
#define MAX_NAME_LEN		16

/* global registers */
#define PCIE_GLBL_INTERRUPT_0					0x00
#define INT_RADM_INTA_ASSERTED					0x00000001
#define INT_RADM_INTA_DEASSERTED				0x00000002
#define INT_RADM_INTB_ASSERTED					0x00000004
#define INT_RADM_INTB_DEASSERTED				0x00000008
#define INT_RADM_INTC_ASSERTED					0x00000010
#define INT_RADM_INTC_DEASSERTED				0x00000020
#define INT_RADM_INTD_ASSERTED					0x00000040
#define INT_RADM_INTD_DEASSERTED				0x00000080
#define INT_MSI_CTR_INT						0x00000100
#define INT_SMLH_LINK_UP					0x00000200
#define INT_HP_INT						0x00000400
#define INT_RADM_CORRECTABLE_ERR				0x00000800
#define INT_RADM_NONFATAL_ERR					0x00001000
#define INT_RADM_FATAL_ERR					0x00002000
#define INT_RADM_PM_TO_ACK					0x00004000
#define INT_RADM_PM_PME						0x00008000
#define INT_RADM_QOVERFLOW					0x00010000
#define INT_LINK_DOWN						0x00400000
#define INT_CFG_AER_RC_ERR_MSI					0x00800000
#define INT_CFG_PME_MSI						0x01000000
#define INT_HP_PME						0x02000000
#define INT_HP_MSI						0x04000000
#define INT_CFG_UR_RESP						0x08000000
#define PCIE_GLBL_INTERRUPT_ENABLE_0				0x04
#define PCIE_GLBL_INTERRUPT_1					0x08
#define PCIE_GLBL_INTERRUPT_ENABLE_1				0x0C
#define PCIE_GLBL_AXI_MASTER_RESP_MISC_INFO			0x10
#define PCIE_GLBL_AXI_MSTR_SLV_RESP_ERR_LOW_PW_MAP		0x14
#define PCIE_GLBL_CORE_CONFIG_REG				0x18
#define PCIE_LTSSM_ENABLE					BIT(0)
#define PCIE_LINK_DOWN_RST					BIT(6)
#define PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS		0x1C
#define PWR_STATUS_SMLH_LTSSM_STATE_OFFSET			0x4
#define PWR_STATUS_SMLH_LTSSM_STATE_MASK			0x000003F0
#define PWR_STATUS_RDLH_LINK_UP					BIT(18)
#define PCIE_GLBL_RTLH_INFO					0x20
#define PCIE_GLBL_AXI_MASTER_WR_MISC_INFO			0x24
#define PCIE_GLBL_AXI_MASTER_RD_MISC_INFO			0x28
#define PCIE_GLBL_AXI_SLAVE_BRESP_MISC_INFO			0x2C
#define PCIE_GLBL_AXI_SLAVE_RD_RESP_MISC_INFO_COMP_TIMEOUT	0x30
#define PCIE_GLBL_CORE_DEBUG_0					0x34
#define PCIE_GLBL_CORE_DEBUG_1					0x38
#define PCIE_GLBL_CORE_DEBUG_E1					0x3C
#define PCIE_GLBL_PCIE_CONTR_CFG_START_ADDR			0x40
#define PCIE_GLBL_PCIE_CONTR_CFG_END_ADDR			0x44
#define PCIE_GLBL_PCIE_CONTR_IATU_BASE_ADDR			0x48

#define RC_IATU_BASE_ADDR_MASK					0xFFF80000

/* ltssm definition */
#define LTSSM_DETECT_QUIET	0x00
#define LTSSM_DETECT_ACT	0x01
#define LTSSM_POLL_ACTIVE	0x02
#define LTSSM_POLL_COMPLIANCE	0x03
#define LTSSM_POLL_CONFIG	0x04
#define LTSSM_PRE_DETECT_QUIET	0x05
#define LTSSM_DETECT_WAIT	0x06
#define LTSSM_CFG_LINKWD_START	0x07
#define LTSSM_CFG_LINKWD_ACEPT	0x08
#define LTSSM_CFG_LANENUM_WAI	0x09
#define LTSSM_CFG_LANENUM_ACEPT	0x0A
#define LTSSM_CFG_COMPLETE	0x0B
#define LTSSM_CFG_IDLE		0x0C
#define LTSSM_RCVRY_LOCK	0x0D
#define LTSSM_RCVRY_SPEED	0x0E
#define LTSSM_RCVRY_RCVRCFG	0x0F
#define LTSSM_RCVRY_IDLE	0x10
#define LTSSM_L0		0x11
#define LTSSM_L0S		0x12
#define LTSSM_L123_SEND_EIDLE	0x13
#define LTSSM_L1_IDLE		0x14
#define LTSSM_L2_IDLE		0x15
#define LTSSM_L2_WAKE		0x16
#define LTSSM_DISABLED_ENTRY	0x17
#define LTSSM_DISABLED_IDLE	0x18
#define LTSSM_DISABLED		0x19
#define LTSSM_ENTRY		0x1A
#define LTSSM_LPBK_ACTIVE	0x1B
#define LTSSM_LPBK_EXIT		0x1C
#define LTSSM_LPBK_EXIT_TIMEOUT	0x1D
#define LTSSM_HOT_RESET_ENTRY	0X1E
#define LTSSM_HOT_RESET		0x1F
#define LTSSM_RCVRY_EQ0		0x20
#define LTSSM_RCVRY_EQ1		0x21
#define LTSSM_RCVRY_EQ2		0x22
#define LTSSM_RCVRY_EQ3		0x23

struct serdes_cfg {
	u32 addr;
	u32 val;
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
	struct irq_domain *intx_irq_domain;
	struct irq_domain *msi_irq_domain;

	struct reset_control *core_reset;
	struct reset_control *phy_reset;
	struct gpio_desc *gpio_reset;

	struct phy *phy[MAX_LANE_NUM];
	phys_addr_t serdes_addr;
	resource_size_t serdes_size;
	struct serdes_cfg *cfg[MAX_LANE_NUM];
	int cfg_cnt[MAX_LANE_NUM];

	u32 idx;
	u8 lanes;
	bool auto_calibration;
	bool init;
	u32 cur_irq_sts;
	unsigned long msi_pages;

	/* Device-Specific */
	/* linking up ready/stable time */
	u16 device_ready_time;
};

const char *ltssm_str[] = {
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

static int ca_pcie_serdes_ber_notify(struct ca_pcie *pcie)
{
	int cnt = 0;
	int reg;
	do {
		reg = 1;
		reg &= readl(pcie->serdes_base + 0x017c);

		if (reg)
			return reg;

		usleep_range(10000, 20000);
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

static int ca_pcie_ltssm(struct ca_pcie *pcie)
{
	u32 reg, val;

	reg = PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS;
	val = ca_pcie_readl(pcie, reg);
	val = (val & PWR_STATUS_SMLH_LTSSM_STATE_MASK) >>
	       PWR_STATUS_SMLH_LTSSM_STATE_OFFSET;
	return val;
}

static void ca_pcie_intx_enable(struct ca_pcie *pcie)
{
	u32 val;

	val = ca_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_ENABLE_0);

	val |= (INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
		INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);

	ca_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_ENABLE_0);
}

static void ca_pcie_mask_intx_irq(struct irq_data *irqd)
{
	struct ca_pcie *pcie = irq_data_get_irq_chip_data(irqd);
	u32 val;

	val = ca_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_ENABLE_0);

	val &= ~(INT_RADM_INTA_ASSERTED | INT_RADM_INTB_ASSERTED |
		 INT_RADM_INTC_ASSERTED | INT_RADM_INTD_ASSERTED);

	ca_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_ENABLE_0);
}

static void ca_pcie_unmask_intx_irq(struct irq_data *irqd)
{
	struct ca_pcie *pcie = irq_data_get_irq_chip_data(irqd);
	ca_pcie_intx_enable(pcie);
}

static struct irq_chip ca_pcie_intx_irq_chip = {
	.name = "PCI-INTx",
	.irq_enable = ca_pcie_unmask_intx_irq,
	.irq_disable = ca_pcie_mask_intx_irq,
	.irq_mask = ca_pcie_mask_intx_irq,
	.irq_unmask = ca_pcie_unmask_intx_irq,
};

static int ca_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			    irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &ca_pcie_intx_irq_chip,
				 handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);
	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = ca_pcie_intx_map,
	.xlate = pci_irqd_intx_xlate,
};

static int ca_pcie_serdes_phy_init(struct ca_pcie *ca_pcie)
{
	u32 off, val;
	int i, lane;
	struct dw_pcie *pci = ca_pcie->pci;
	struct device *dev = pci->dev;

	for (lane = 0; lane < ca_pcie->lanes; lane++) {
		if (ca_pcie->cfg_cnt[lane] <= 0 ||
		    !ca_pcie->cfg[lane]) {
			dev_warn(dev, "lane %d no serdes cfg!\n",
				 lane);
			continue;
		}

		for (i = 0; i < ca_pcie->cfg_cnt[lane]; i++) {
			off = (phys_addr_t)ca_pcie->cfg[lane][i].addr - ca_pcie->serdes_addr;
			val = ca_pcie->cfg[lane][i].val;

			if (off < ca_pcie->serdes_size)
				writel(val, ca_pcie->serdes_base + off);
		}
	}

	return 0;
}

static int ca_pcie_host_reset(struct ca_pcie *pcie)
{
	int i;
	gpiod_set_value_cansleep(pcie->gpio_reset, 1);
	usleep_range(10000, 20000);

	reset_control_assert(pcie->core_reset);
	usleep_range(10000, 20000);

	reset_control_assert(pcie->phy_reset);
	usleep_range(10000, 20000);

	ca_pcie_serdes_phy_init(pcie);

	for (i = 0; i < pcie->lanes; i++)
		phy_power_on(pcie->phy[i]);
	usleep_range(10000, 20000);

	reset_control_deassert(pcie->phy_reset);
	usleep_range(10000, 20000);

	reset_control_deassert(pcie->core_reset);
	usleep_range(10000, 20000);

	// calibration
	writel(0x500c, pcie->serdes_base + 0x0024);
	usleep_range(100, 200);
	writel(0x520c, pcie->serdes_base + 0x0024);
	usleep_range(100, 200);
	writel(0x500c, pcie->serdes_base + 0x0124);
	usleep_range(100, 200);
	writel(0x520c, pcie->serdes_base + 0x0124);
	usleep_range(100, 200);

	if (pcie->lanes == 2) {
		writel(0x500c, pcie->serdes_base + 0x1024);
		usleep_range(100, 200);
		writel(0x520c, pcie->serdes_base + 0x1024);
		usleep_range(100, 200);
		writel(0x500c, pcie->serdes_base + 0x1124);
		usleep_range(100, 200);
		writel(0x520c, pcie->serdes_base + 0x1124);
		usleep_range(100, 200);
	}

	gpiod_set_value_cansleep(pcie->gpio_reset, 0);
	if (!ca_pcie_serdes_ber_notify(pcie))
		dev_err(pcie->pci->dev, "BER check fail!\n");
	else
		dev_info(pcie->pci->dev, "BER check pass!\n");
	msleep(pcie->device_ready_time);

	return 0;
}

static int ca_pcie_host_setup(struct ca_pcie *pcie)
{
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
	return 0;
}

static void ca_pcie_stop_link(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);

	ca_pcie_writel(pcie, PCIE_LINK_DOWN_RST, PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
	ca_pcie_writel(pcie, 0x0,  PCIE_GLBL_CORE_CONFIG_REG);
	usleep_range(100, 200);
}

static int ca_pcie_establish_link(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);
	if (dw_pcie_link_up(pci))
		return 0;

	/* assert LTSSM enable */
	ca_pcie_writel(pcie, PCIE_LTSSM_ENABLE,
		       PCIE_GLBL_CORE_CONFIG_REG);

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
	u32 val;
	val = ca_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_ENABLE_0);
	val |= INT_MSI_CTR_INT;
	ca_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_ENABLE_0);
}

static void ca_pcie_enable_interrupts(struct ca_pcie *pcie)
{
	ca_pcie_misc_enable(pcie);
	ca_pcie_msi_enable(pcie);
}

/* MSI int handler */
irqreturn_t ca_handle_msi_irq(struct dw_pcie_rp *pp)
{
	int i, pos;
	unsigned long val;
	u32 status;
	irqreturn_t ret = IRQ_NONE;
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);

	for (i = 0; i < MAX_MSI_CTRLS; i++) {
		status = dw_pcie_readl_dbi(pci,
					   PCIE_MSI_INTR0_STATUS +
					   (i * MSI_REG_CTRL_BLOCK_SIZE));
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

static irqreturn_t ca_pcie_irq_handler(int irq, void *arg)
{
	struct ca_pcie *pcie = arg;
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	u32 val;
	val = ca_pcie_readl(pcie, PCIE_GLBL_INTERRUPT_0);

	if (val & INT_MSI_CTR_INT)
		ca_handle_msi_irq(pp);
	if (val & INT_RADM_INTA_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_irq_domain, 0));
	if (val & INT_RADM_INTB_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_irq_domain, 1));
	if (val & INT_RADM_INTC_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_irq_domain, 2));
	if (val & INT_RADM_INTD_ASSERTED)
		generic_handle_irq(irq_find_mapping(pcie->intx_irq_domain, 3));
	if (val & INT_LINK_DOWN) {
		int ltssm = ca_pcie_ltssm(pcie);

		dev_err(pci->dev,
			"Link Down!!!(ltssm = 0x%x - %s)\n",
			ltssm, ltssm_str[ltssm]);
		ca_pcie_writel(pcie, PCIE_LINK_DOWN_RST,
			       PCIE_GLBL_CORE_CONFIG_REG);
	}

	ca_pcie_writel(pcie, val, PCIE_GLBL_INTERRUPT_0);
	return IRQ_HANDLED;
}

static int ca_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node = of_get_next_child(node, NULL);
	int ret;

	if (IS_ERR(pcie_intc_node)) {
		ret = PTR_ERR(pcie_intc_node);
		dev_err(dev, "No PCIe Intc node found(%d)\n", ret);
		return ret;
	}

	pp->irq_domain = irq_domain_add_linear(pcie_intc_node,
					       MAX_INTX_HOST_IRQS,
					       &intx_domain_ops, pp);
	if (IS_ERR(pp->irq_domain)) {
		ret = PTR_ERR(pp->irq_domain);
		dev_err(dev, "Failed to get a INTx IRQ domain(%d)\n", ret);
		of_node_put(pcie_intc_node);
		return ret;
	}

	of_node_put(pcie_intc_node);
	return 0;
}

void dw_pcie_link_check(struct dw_pcie *pci, u8 *speed, u8 *lanes)
{
	u32 val;
	u32 exp_cap_off = dw_pcie_find_capability(pci, PCI_CAP_ID_EXP);

	val = readb(pci->dbi_base + exp_cap_off + PCI_EXP_LNKSTA);
	*speed = val & PCI_EXP_LNKSTA_CLS;
	*lanes = (val & PCI_EXP_LNKSTA_NLW) >> 4;
}

static int ca_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct ca_pcie *pcie = to_ca_pcie(pci);
	int ret;
	u8 speed, lanes;

	/* disable_interrupts */
	ca_pcie_writel(pcie, 0,
		       PCIE_GLBL_INTERRUPT_ENABLE_0);

	ca_pcie_host_reset(pcie);
	ca_pcie_host_setup(pcie);
	ca_pcie_establish_link(pci);

	ret = dw_pcie_wait_for_link(pci);
	if (ret) {
		int ltssm = ca_pcie_ltssm(pcie);

		dev_err(pci->dev,
			"Link Fail!!!(ltssm = 0x%x - %s)\n",
			ltssm, ltssm_str[ltssm]);
		return (ltssm == LTSSM_DETECT_QUIET ?
			-ENODEV : -EIO);
	} else {
		dw_pcie_link_check(pci, &speed, &lanes);
		dev_info(pci->dev, "Speed Gen%d Lanes x%d", speed, lanes);
	}

	ca_pcie_enable_interrupts(pcie);
	ca_pcie_init_irq_domain(pp);

	return 0;
}

static const struct dw_pcie_host_ops ca_pcie_host_ops = {
	.host_init = ca_pcie_host_init,
	//.msi_host_init = ca_pcie_msi_host_init,
};

static int ca_pcie_link_up(struct dw_pcie *pci)
{
	struct ca_pcie *pcie = to_ca_pcie(pci);
	u32 reg, val;

	reg = PCIE_GLBL_PM_INFO_RESET_VOLT_LOW_PWR_STATUS;
	val = ca_pcie_readl(pcie, reg);
	val &= PWR_STATUS_RDLH_LINK_UP;

	return val ? 1 :  0;
}

static const struct dw_pcie_ops ca_pcie_ops = {
	.link_up = ca_pcie_link_up,
	.start_link = ca_pcie_establish_link,
	.stop_link = ca_pcie_stop_link,
};

static int __init ca_add_pcie_port(struct ca_pcie *pcie, struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = pci->dev;
	struct resource *res;
	int ret;
	u32 base_addr;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	ret = devm_request_irq(dev, pp->irq, ca_pcie_irq_handler,
			       IRQF_SHARED | IRQF_PROBE_SHARED,
			       "ca-pcie", pcie);
	if (ret) {
		dev_err(dev, "fail to request irq\n");
		return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbi");
	pci->dbi_base = devm_ioremap_resource(dev, res);
	if (!pci->dbi_base)
		return -ENOMEM;

	pcie->dbi_start = res->start;
	pcie->dbi_end = res->end;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iatu");
	pcie->iatu_unroll_base = devm_ioremap_resource(dev, res);
	if (pcie->iatu_unroll_base) {
		pci->atu_base = pcie->iatu_unroll_base;
		pcie->iatu_unroll_start = res->start;
		base_addr = (u32)pcie->iatu_unroll_start & RC_IATU_BASE_ADDR_MASK;
	}

	pp->ops = &ca_pcie_host_ops;

	// config for separate CA/DWC path
	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host(%d)\n", ret);
		return ret;
	}
	return 0;
}

static void ca_serdes_probe(struct device *dev, struct device_node *np, struct ca_pcie *pcie)
{
	int i, size, cnt;
	char name[MAX_NAME_LEN];

	size = sizeof(struct serdes_cfg);

	for (i = 0; i < pcie->lanes; i++) {
		sprintf(name, "serdes-cfg%d", i);
		pcie->cfg_cnt[i] = of_property_count_elems_of_size(np, name,
								   size);
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

static int ca_pcie_probe(struct platform_device *pdev)
{
	struct dw_pcie *pci;
	struct ca_pcie *pcie;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *reg_res;
	int lanes, i, ret;
	char name[MAX_NAME_LEN];

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &ca_pcie_ops;

	pcie->pci = pci;
	pcie->bus_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(pcie->bus_clk)) {
		dev_err(dev, "Failed to get pcie bus clk\n");
		return PTR_ERR(pcie->bus_clk);
	}

	ret = clk_prepare_enable(pcie->bus_clk);
	if (ret)
		return ret;
	reg_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					       "glbl_regs");
	pcie->reg_base = devm_ioremap_resource(dev, reg_res);
	if (IS_ERR(pcie->reg_base)) {
		ret = PTR_ERR(pcie->reg_base);
		goto fail_bus_clk;
	}

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

	pcie->serdes_addr = reg_res->start;
	pcie->serdes_size = resource_size(reg_res);

	for (i = 0; i < MAX_LANE_NUM; i++)
		pcie->cfg[i] = NULL;
	ca_serdes_probe(dev, np, pcie);

	pcie->core_reset = of_reset_control_get(np, "core_reset");
	if (IS_ERR(pcie->core_reset)) {
		dev_err(dev, "failed to get core_reset (error %ld)\n", PTR_ERR(pcie->core_reset));
		ret = PTR_ERR(pcie->core_reset);
		goto fail_bus_clk;
	}

	pcie->phy_reset = of_reset_control_get(np, "phy_reset");
	if (IS_ERR(pcie->phy_reset)) {
		dev_err(dev, "failed to get phy_reset (error %ld)\n", PTR_ERR(pcie->phy_reset));
		ret = PTR_ERR(pcie->phy_reset);
		goto fail_bus_clk;
	}

	pcie->gpio_reset = devm_gpiod_get(dev, "reset", 0);
	if (IS_ERR(pcie->gpio_reset)) {
		dev_err(dev, "Failed to get reset-gpios\n");
		goto fail_bus_clk;
	}

	gpiod_direction_output(pcie->gpio_reset, 0);

	for (i = 0; i < MAX_LANE_NUM; i++) {
		sprintf(name, "pcie-phy%d", i);
		pcie->phy[i] = devm_phy_get(&pdev->dev, name);
		if (IS_ERR(pcie->phy[i])) {
			ret = PTR_ERR(pcie->phy[i]);
			pcie->phy[i] = NULL;
		}
	}

	for (i = 0; i < pcie->lanes; i++) {
		if (!pcie->phy[i]) {
			dev_err(dev, "failed to get phy %d\n", i);
			goto fail_bus_clk;
		}
	}

	ret = of_property_read_u16(np, "ready-time",
				   &pcie->device_ready_time);
	if (ret)
		pcie->device_ready_time = 0;

	platform_set_drvdata(pdev, pcie);
	ret = ca_add_pcie_port(pcie, pdev);
	if (ret < 0)
		goto fail_bus_clk;
	return 0;

fail_bus_clk:
	if (pcie->phy_reset)
		reset_control_put(pcie->phy_reset);
	if (pcie->core_reset)
		reset_control_put(pcie->core_reset);
	for (i = 0; i < pcie->lanes; i++)
		phy_power_off(pcie->phy[i]);

	clk_disable_unprepare(pcie->bus_clk);

	return ret;
}

static int __exit ca_pcie_remove(struct platform_device *pdev)
{
	struct ca_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int i;

	dw_pcie_host_deinit(pp);

	for (i = 0; i < pcie->lanes; i++)
		phy_power_off(pcie->phy[i]);

	clk_disable_unprepare(pcie->bus_clk);

	if (pcie->phy_reset)
		reset_control_put(pcie->phy_reset);
	if (pcie->core_reset)
		reset_control_put(pcie->core_reset);

	return 0;
}

static const struct of_device_id ca_pcie_of_match[] = {
	{
		.compatible = "cortina-access,venus-pcie",
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
