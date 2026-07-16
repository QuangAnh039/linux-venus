// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/freezer.h>
#include <linux/sched.h>
#include <dt-bindings/clock/ca8299-clock.h>
#include <soc/cortina-access/registers.h>
#include "ca-mmc-phase.h"

#define to_ca8299_pll_clk(_hw) container_of(_hw, struct ca8299_pll_clk, hw)
#define to_ca8299_cpu_daemon(_hw) \
		container_of(_hw, struct ca8299_cpu_daemon, hw)
#define clk_mask(width) ((1 << (width)) - 1)

enum {
	CA_CLK_PLL,
	CA_CLK_DIVIDER,
	CA_CLK_MUX,
	CA_CLK_GATE,
	CA_CLK_FIXED_FACTOR,
	CA_CLK_PE_DAEMON,
	CA_CLK_CPU_DAEMON,
	CA_CLK_MMC_PHASE,
};

enum {
	PED_REF,
	PED_FPLL,
	PED_DIV_F,
	PED_MUX_PE,
	PED_CLKS_NUM,
};

enum {
	CPUD_REF,
	CPUD_CPLL,
	CPUD_DIV_F2C,
	CPUD_MUX_CPU,
	CPUD_DIV_CORTEX,
	CPUD_CLKS_NUM,
};

enum EN_CLK_EPLL_TYPE {
	CLK_EPLL_1P5G,
	CLK_EPLL_1P2G,
	CLK_EPLL_1G,
	CLK_EPLL_NUM,
};

#define DIVN_BASE		4
#define DIVN_SHIFT		0
#define DIVN_WIDTH		8
#define PREDIV_BASE		2
#define PREDIV_SHIFT		10
#define PREDIV_WIDTH		2
#define PLL_RSTB		9

// delay for pll lock should be more than 160us
#define MIN_DELAY_PLL_FOR_LOCK		200
#define MIN_DELAY_PLL_FOR_SWITCH	10

struct ca8299_clk_reg {
	void __iomem *regbase;
	spinlock_t regs_lock;	/* register access lock */
};

struct ca8299_clk_param {
	int type;
	u32 reg;
};

struct ca8299_pll_clk {
	void __iomem *reg;
	spinlock_t *lock;	/* register access lock */
	struct clk_hw hw;
};

struct ca8299_pll_param {
	unsigned int    pll;
	unsigned int    prediv;
	unsigned int    divn;
};

struct ca8299_cpu_strap_speed {
	unsigned int    pll;
	unsigned int    div;
	unsigned int    ncode_t;
};

struct ca8299_pe_speed {
	unsigned int    pll;
	unsigned int    div;
};

struct ca8299_cpu_daemon {
	void __iomem *reg;
	struct mutex mlock;	/* daemon lock */
	spinlock_t *lock;	/* register access lock */
	struct clk_hw hw;
	struct clk *clks[CPUD_CLKS_NUM];
};

static enum EN_CLK_EPLL_TYPE epll_rate = CLK_EPLL_NUM;

static const struct ca8299_pll_param ca8299_pll_param_table[] = {
	{ .pll = 40, .prediv = 5, .divn = 200, },	/* CPU */
	{ .pll = 44, .prediv = 5, .divn = 220, },	/* CPU */
	{ .pll = 48, .prediv = 5, .divn = 240, },	/* CPU */
	{ .pll = 50, .prediv = 5, .divn = 250, },	/* CPU */
	{ .pll = 56, .prediv = 3, .divn = 168, },	/* CPU */
	{ .pll = 64, .prediv = 3, .divn = 192, },	/* FPLL/CPU */
	{ .pll = 72, .prediv = 3, .divn = 216, },	/* CPU */
	{ .pll = 80, .prediv = 2, .divn = 160, },	/* EPLL */
	{ } /* sentinel */
};

static const struct clk_div_table ca8299_sd_div_table[] = {
	{ .val = 0, .div = 40, },
	{ .val = 1, .div = 20, },
	{ .val = 2, .div = 10, },
	{ } /* sentinel */
};

static const struct ca8299_cpu_strap_speed ca8299_cpu_strap_speed_table[] = {
	{ .pll = 48, .div = 3, .ncode_t = 12, }, /* 400000000 */
	{ .pll = 50, .div = 2, .ncode_t = 21, }, /* 625000000 */
	{ .pll = 56, .div = 2, .ncode_t = 24, }, /* 700000000 */
	{ .pll = 64, .div = 2, .ncode_t = 28, }, /* 800000000 */
	{ .pll = 72, .div = 2, .ncode_t = 32, }, /* 900000000 */
	{ .pll = 40, .div = 1, .ncode_t = 36, }, /* 1000000000 */
	{ .pll = 44, .div = 1, .ncode_t = 40, }, /* 1100000000 */
	{ .pll = 48, .div = 1, .ncode_t = 44, }, /* 1200000000 */
	{ .pll = 56, .div = 1, .ncode_t = 52, }, /* 1400000000 */
	{ .pll = 64, .div = 1, .ncode_t = 60, }, /* 1600000000 */
	{ .pll = 72, .div = 1, .ncode_t = 68, }, /* 1800000000 */
	{ .pll = 80, .div = 1, .ncode_t = 76, }, /* 2000000000 */
	{ }
};

static const struct ca8299_pe_speed ca8299_pe_speed_table[] = {
	{ .pll = 64, .div = 2, }, /* 800000000 */
	{ }
};

struct ca8299_clk_param pll_cpll     = {CA_CLK_PLL, GLOBAL_CPLL0};
struct ca8299_clk_param pll_epll_1p5g = {CA_CLK_PLL, GLOBAL_EPLL0};
struct ca8299_clk_param pll_fpll     = {CA_CLK_PLL, GLOBAL_FPLL0};
struct ca8299_clk_param pll_epll_1p2g = {CA_CLK_PLL, GLOBAL_EPLL0};
struct ca8299_clk_param pll_epll_1g  = {CA_CLK_PLL, GLOBAL_EPLL0};
struct ca8299_clk_param mux_cplldiv  = {CA_CLK_MUX, GLOBAL_CPLLDIV};
struct ca8299_clk_param mux_pediv    = {CA_CLK_MUX, GLOBAL_PEDIV};
struct ca8299_clk_param div_cplldiv  = {CA_CLK_DIVIDER, GLOBAL_CPLLDIV};
struct ca8299_clk_param div_fplldiv  = {CA_CLK_DIVIDER, GLOBAL_PEDIV};
struct ca8299_clk_param div_eplldiv  = {CA_CLK_DIVIDER, GLOBAL_EPLLDIV};
struct ca8299_clk_param div_eplldiv2 = {CA_CLK_DIVIDER, GLOBAL_EPLLDIV2};
struct ca8299_clk_param gate_config  = {CA_CLK_GATE, GLOBAL_GLOBAL_CONFIG};
struct ca8299_clk_param fixed_factor = {CA_CLK_FIXED_FACTOR, 0};
struct ca8299_clk_param pe_daemon    = {CA_CLK_PE_DAEMON, 0};
struct ca8299_clk_param cpu_daemon   = {CA_CLK_CPU_DAEMON, 0};
struct ca8299_clk_param div_trcdiv   = {CA_CLK_DIVIDER, GLOBAL_CPLLDIV};

static const struct of_device_id ca8299_soc_clks_ids[] __initconst = {
	{ .compatible = "cortina-access,ca8299-pll-cpll-clk",
	  .data = (void *)&pll_cpll, },
	{ .compatible = "cortina-access,ca8299-pll-fpll-clk",
	  .data = (void *)&pll_fpll, },
	{ .compatible = "cortina-access,ca8299-pll-epll-1p5g-clk",
	  .data = (void *)&pll_epll_1p5g, },
	{ .compatible = "cortina-access,ca8299-pll-epll-1p2g-clk",
	  .data = (void *)&pll_epll_1p2g, },
	{ .compatible = "cortina-access,ca8299-pll-epll-1g-clk",
	  .data = (void *)&pll_epll_1g, },
	{ .compatible = "cortina-access,mux-cpu-clk",
	  .data = (void *)&mux_cplldiv, },
	{ .compatible = "cortina-access,mux-pe-clk",
	  .data = (void *)&mux_pediv, },
	{ .compatible = "cortina-access,div-cpll-clk",
	  .data = (void *)&div_cplldiv, },
	{ .compatible = "cortina-access,div-fpll-clk",
	  .data = (void *)&div_fplldiv, },
	{ .compatible = "cortina-access,div-epll-clk",
	  .data = (void *)&div_eplldiv, },
	{ .compatible = "cortina-access,div-epll2-clk",
	  .data = (void *)&div_eplldiv2, },
	{ .compatible = "cortina-access,gate-config-clk",
	  .data = (void *)&gate_config, },
	{ .compatible = "cortina-access,fixed-factor-clk",
	  .data = (void *)&fixed_factor, },
	{ .compatible = "cortina-access,ca8299-pe-daemon-clk",
	  .data = (void *)&pe_daemon, },
	{ .compatible = "cortina-access,ca8299-cpu-daemon-clk",
	  .data = (void *)&cpu_daemon, },
	{},
};

static bool readonly;

unsigned long ca8299_pll_recalc_rate(struct clk_hw *hw,
				     unsigned long parent_rate)
{
	struct ca8299_pll_clk *pll_clk = to_ca8299_pll_clk(hw);
	u32 val, divn;

	val = readl(pll_clk->reg) >> DIVN_SHIFT;
	val &= clk_mask(DIVN_WIDTH);
	divn = val + DIVN_BASE;

	if (epll_rate == CLK_EPLL_1P2G)
		return (parent_rate * divn) / 5 * 4;
	else if (epll_rate == CLK_EPLL_1G)
		return (parent_rate * divn) / 3 * 2;
	else /* Include epll_rate == CLK_EPLL_1P5G*/
		return (parent_rate * divn);
}

long ca8299_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long *parent_rate)
{
	unsigned long mult;
	int i;

	if ((rate % (*parent_rate)) != 0)
		goto NOT_SUPPORT;
	mult = rate / (*parent_rate);

	for (i = 0; i < ARRAY_SIZE(ca8299_pll_param_table); i++) {
		if (ca8299_pll_param_table[i].pll == mult)
			return rate;
	}

NOT_SUPPORT:
	return *parent_rate;
}

int ca8299_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct ca8299_pll_clk *pll_clk = to_ca8299_pll_clk(hw);
	unsigned long mult;
	unsigned long flags = 0;
	u32 val, divn, prediv;
	int i;

	if ((rate % parent_rate) != 0)
		return 0;

	mult = rate / parent_rate;
	prediv = 0;
	divn = 0;
	for (i = 0; i < ARRAY_SIZE(ca8299_pll_param_table); i++) {
		if (ca8299_pll_param_table[i].pll == mult) {
			prediv = ca8299_pll_param_table[i].prediv;
			divn = ca8299_pll_param_table[i].divn;
		}
	}
	if (mult != (divn / prediv))
		return 0;
	spin_lock_irqsave(pll_clk->lock, flags);
	val = readl(pll_clk->reg);
	val &= ~(clk_mask(DIVN_WIDTH) << DIVN_SHIFT);
	val |= (divn - DIVN_BASE) << DIVN_SHIFT;
	val &= ~(clk_mask(PREDIV_WIDTH) << PREDIV_SHIFT);
	val |= (prediv - PREDIV_BASE) << PREDIV_SHIFT;
	writel(val, pll_clk->reg);
	spin_unlock_irqrestore(pll_clk->lock, flags);
	return rate;
}

static const struct clk_ops ca8299_pll_ops = {
	.recalc_rate	= ca8299_pll_recalc_rate,
	.round_rate	= ca8299_pll_round_rate,
	.set_rate	= ca8299_pll_set_rate,
};

int ca8299_clk_pll(struct device_node *np, struct ca8299_clk_reg *clk_reg,
		   struct ca8299_clk_param *clk_param)
{
	const char *parent_name;
	void __iomem *reg;
	struct clk_init_data init;
	struct ca8299_pll_clk *pll_clk;
	struct clk *soc_clk;

	parent_name = of_clk_get_parent_name(np, 0);
	if (!strcmp(np->name, "pll_epll_1p5g"))
		epll_rate = CLK_EPLL_1P5G;
	else if (!strcmp(np->name, "pll_epll_1p2g"))
		epll_rate = CLK_EPLL_1P2G;
	else if (!strcmp(np->name, "pll_epll_1g"))
		epll_rate = CLK_EPLL_1G;
	else
		epll_rate = CLK_EPLL_NUM;

	reg = clk_reg->regbase + clk_param->reg - GLOBAL_JTAG_ID;

	pll_clk = kzalloc(sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return -ENOMEM;

	init.name = np->name;
	init.ops = &ca8299_pll_ops;
	init.flags = 0;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);

	pll_clk->hw.init = &init;
	pll_clk->reg = reg;
	pll_clk->lock = &clk_reg->regs_lock;

	soc_clk = clk_register(NULL, &pll_clk->hw);
	if (!IS_ERR(soc_clk))
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);
	else
		kfree(pll_clk);

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

int ca8299_clk_divider(struct device_node *np, struct ca8299_clk_reg *clk_reg,
		       struct ca8299_clk_param *clk_param)
{
	u8 op_shift, op_width;
	const char *parent_name;
	void __iomem *reg;
	struct clk *soc_clk;

	if (of_property_read_u8(np, "operate-shift", &op_shift))
		return -EPERM;
	if (op_shift >= 32)
		return -EINVAL;
	if (of_property_read_u8(np, "operate-width", &op_width))
		return -EPERM;
	if (op_width >= 32)
		return -EINVAL;

	parent_name = of_clk_get_parent_name(np, 0);
	reg = clk_reg->regbase + clk_param->reg - GLOBAL_JTAG_ID;

	soc_clk = clk_register_divider(NULL, np->name, parent_name, 0,
				       reg, op_shift + 1, op_width - 1,
				       CLK_DIVIDER_ONE_BASED | CLK_DIVIDER_ALLOW_ZERO,
				       &clk_reg->regs_lock);

	if (!IS_ERR(soc_clk))
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

int ca8299_clk_mux(struct device_node *np, struct ca8299_clk_reg *clk_reg,
		   struct ca8299_clk_param *clk_param)
{
	u8 op_shift, num_parents;
	void __iomem *reg;
	const char *parent_names[2];
	struct clk *soc_clk;
	int i;

	if (of_property_read_u8(np, "operate-shift", &op_shift))
		return -EPERM;
	if (op_shift >= 32)
		return -EINVAL;
	num_parents = of_clk_get_parent_count(np);
	if (num_parents != 2)
		return -EINVAL;

	for (i = 0; i < num_parents; i++)
		parent_names[i] = of_clk_get_parent_name(np, i);

	reg = clk_reg->regbase + clk_param->reg - GLOBAL_JTAG_ID;

	soc_clk = clk_register_mux(NULL, np->name, parent_names, num_parents,
				   0, reg, op_shift, 1, 0, &clk_reg->regs_lock);
	if (!IS_ERR(soc_clk))
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

int ca8299_clk_gate(struct device_node *np, struct ca8299_clk_reg *clk_reg,
		    struct ca8299_clk_param *clk_param)
{
	u8 op_shift, gate_flags;
	void __iomem *reg;
	struct clk *soc_clk;

	if (of_property_read_u8(np, "operate-shift", &op_shift))
		return -EPERM;
	if (op_shift >= 32)
		return -EINVAL;

	if (of_property_read_bool(np, "active-low"))
		gate_flags = CLK_GATE_SET_TO_DISABLE;
	else
		gate_flags = 0;

	reg = clk_reg->regbase + clk_param->reg - GLOBAL_JTAG_ID;

	soc_clk = clk_register_gate(NULL, np->name, NULL, 0, reg, op_shift,
				    gate_flags, &clk_reg->regs_lock);
	if (!IS_ERR(soc_clk))
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

int ca8299_clk_fixed_factor(struct device_node *np,
			    struct ca8299_clk_reg *clk_reg,
			    struct ca8299_clk_param *clk_param)
{
	unsigned int mult, div;
	const char *parent_name;
	struct clk *soc_clk;

	if (of_property_read_u32(np, "fixed-mult", &mult))
		mult = 1;
	if (of_property_read_u32(np, "fixed-div", &div))
		div = 1;

	parent_name = of_clk_get_parent_name(np, 0);

	soc_clk = clk_register_fixed_factor(NULL, np->name, parent_name, 0,
					    mult, div);
	if (!IS_ERR(soc_clk))
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

int ca8299_clk_pe_daemon(struct device_node *np,
			 struct ca8299_clk_reg *clk_reg,
			 struct ca8299_clk_param *clk_param)
{
	/* keep FPLL and PEDIV untainted */

	return 0;
}

int ca8299_clk_mmc_register(struct device_node *np,
			    struct ca8299_clk_reg *clk_reg,
			    struct ca8299_clk_param *clk_param)
{
	u8 op_shift, op_width;
	void __iomem *reg;
	struct clk *soc_clk;
	const char *parent_names;

	if (of_property_read_u8(np, "operate-shift", &op_shift))
		return -EPERM;
	if (of_property_read_u8(np, "operate-width", &op_width))
		return -EPERM;
	if (op_shift >= 32)
		return -EINVAL;

	parent_names = of_clk_get_parent_name(np, 0);

	reg = clk_reg->regbase + clk_param->reg - GLOBAL_JTAG_ID;

	soc_clk = ca_mmc_phase_clk_register(np->name, &parent_names,
					    &clk_reg->regs_lock, reg,
					    op_shift, op_width);

	if (!IS_ERR(soc_clk)) {
		of_clk_add_provider(np, of_clk_src_simple_get, soc_clk);
		pr_info("%s %s OK\n", __func__, np->name);
	} else {
		pr_info("%s %s NG\n", __func__, np->name);
	}

	return IS_ERR(soc_clk) ? PTR_ERR(soc_clk) : 0;
}

unsigned long ca8299_cpu_recalc_rate(struct clk_hw *hw,
				     unsigned long parent_rate)
{
	struct ca8299_cpu_daemon *cpud = to_ca8299_cpu_daemon(hw);
	unsigned long ref_rate;
	GLOBAL_CPLL0_t cpll0;
	void __iomem *cpll0_reg;
	u8 mult;

	cpll0_reg = cpud->reg + GLOBAL_CPLL0 - GLOBAL_JTAG_ID;

	cpll0.wrd = readl(cpll0_reg);
	mult = cpll0.bf.ncode_t + DIVN_BASE;

	ref_rate = clk_get_rate(cpud->clks[CPUD_REF]);
	return ref_rate * mult;
}

long ca8299_cpu_round_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long *parent_rate)
{
	struct ca8299_cpu_daemon *cpud = to_ca8299_cpu_daemon(hw);
	unsigned long ref_rate, target;
	int i;

	ref_rate = clk_get_rate(cpud->clks[CPUD_REF]);
	for (i = 0; i < ARRAY_SIZE(ca8299_cpu_strap_speed_table); i++) {
		target = ref_rate * (ca8299_cpu_strap_speed_table[i].ncode_t + 4);
		if (rate == target)
			return target;
	}

	return *parent_rate;
}

bool is_idle_thread(struct task_struct *task)
{
	return task->__state == TASK_IDLE;
}

int ca8299_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct ca8299_cpu_daemon *cpud = to_ca8299_cpu_daemon(hw);
	/* Lower CPU PLL to avoid rush current
	 * Configure CPU to 2000MHz according to HW suggestion.(example)
	 * CLK = 25M * [(ncode + 4) + (fcode/2048)]
	 *
	 * Adjust Flow(per Allen-Tsai):
	 *	1. Set PLL_SSC_DIG_RSTB=1'b1, PLL_SSC_DIG_OC_EN = 1'b0
	 *	2. Set New target frequency: PLL_SSC_DIG_NCODE_T[7:0], FCODE_T[10:0]
	 *	3. Set PLL_SSC_DIG_OC_EN = 1'b1 (OC/UC start)
	 *	4. Wait PLL_SSC_DIG_OC_DONE = 1'b1
	 */
	GLOBAL_CPLL0_t cpll0;
	GLOBAL_CPLL_SSC1_t cpll_ssc1;
	GLOBAL_CPLL_SSC_STATE_t cpll_ssc_state;
	unsigned long ref_rate, target;
	void __iomem *reg_cpll0, *reg_ssc1, *reg_ssc;
	int i, check_loop = 0;

	ref_rate = clk_get_rate(cpud->clks[CPUD_REF]);
	reg_cpll0 = cpud->reg + GLOBAL_CPLL0 - GLOBAL_JTAG_ID;
	cpll0.wrd = readl(reg_cpll0);
	cpll0.bf.ssc_rstb = 1;
	cpll0.bf.ssc_oc_en = 0;
	writel(cpll0.wrd, reg_cpll0);

	for (i = 0; i < ARRAY_SIZE(ca8299_cpu_strap_speed_table); i++) {
		target = ref_rate * (ca8299_cpu_strap_speed_table[i].ncode_t + 4);
		if (rate == target)
			goto MATCH;
	}
	return 0;
MATCH:
	cpll0.bf.ncode_t = ca8299_cpu_strap_speed_table[i].ncode_t;
	writel(cpll0.wrd, reg_cpll0);

	reg_ssc1 = cpud->reg + GLOBAL_CPLL_SSC1 - GLOBAL_JTAG_ID;
	cpll_ssc1.wrd = readl(reg_ssc1);
	cpll_ssc1.bf.fcode_t = 0;
	writel(cpll_ssc1.wrd, reg_ssc1);
	cpll0.bf.ssc_oc_en = 1;
	writel(cpll0.wrd, reg_cpll0);

	reg_ssc = cpud->reg + GLOBAL_CPLL_SSC_STATE - GLOBAL_JTAG_ID;
	cpll_ssc_state.wrd = readl(reg_ssc);
	while ((cpll_ssc_state.bf.ssc_reg_oc_done == 0) && (check_loop++ < 50)) {
		//confirm not in idle state
		if (is_idle_thread(current)) {
			pr_err("Cannot sleep in idle thread\n");
			break;
		}
		usleep_range(10, 15);
		cpll_ssc_state.wrd = readl(reg_ssc);
	}
	pr_info("Change CPU frequency to %ld MHz\n", rate / 1000000);

	return rate;
}

int ca8299_cpu_init(struct clk_hw *hw)
{
	struct ca8299_cpu_daemon *cpud = to_ca8299_cpu_daemon(hw);
	void __iomem *cpllmux_reg;

	cpllmux_reg = cpud->reg + GLOBAL_CPLLMUX - GLOBAL_JTAG_ID;

	/* magic values received from HW team		*/
	/* prevent cpu stall while changing frequency	*/
	writel(0xff000000, cpllmux_reg);

	return 0;
}

static const struct clk_ops ca8299_cpu_ops = {
	.recalc_rate	= ca8299_cpu_recalc_rate,
	.round_rate	= ca8299_cpu_round_rate,
	.set_rate	= ca8299_cpu_set_rate,
	.init		= ca8299_cpu_init,
};

int ca8299_clk_cpu_daemon(struct device_node *np,
			  struct ca8299_clk_reg *clk_reg,
			  struct ca8299_clk_param *clk_param)
{
	const char *parent_names[5];
	struct clk *ref_clk;
	unsigned long  ref_rate, min_rate, max_rate;
	int i;
	struct clk *cpu_clk;
	struct clk_init_data init;
	struct ca8299_cpu_daemon *cpud;
	size_t max_index = ARRAY_SIZE(ca8299_cpu_strap_speed_table) - 2;

	readonly = of_property_read_bool(np, "read-only");
	cpud = kzalloc(sizeof(*cpud), GFP_KERNEL);
	if (!cpud)
		return -ENOMEM;

	for (i = 0; i < CPUD_CLKS_NUM; i++) {
		parent_names[i] = of_clk_get_parent_name(np, i);
		cpud->clks[i] = __clk_lookup(parent_names[i]);
	}
	ref_clk = __clk_lookup(parent_names[CPUD_REF]);
	ref_rate = clk_get_rate(ref_clk);
	min_rate = ref_rate * (ca8299_cpu_strap_speed_table[0].ncode_t + 4);
	max_rate = ref_rate * (ca8299_cpu_strap_speed_table[max_index].ncode_t + 4);

	init.name = np->name;
	init.ops = &ca8299_cpu_ops;
	init.flags = 0;
	init.parent_names = &parent_names[CPUD_DIV_CORTEX];
	init.num_parents = 1;
	mutex_init(&cpud->mlock);
	cpud->reg = clk_reg->regbase;
	cpud->lock = &clk_reg->regs_lock;
	cpud->hw.init = &init;

	cpu_clk = clk_register(NULL, &cpud->hw);
	if (!IS_ERR(cpu_clk)) {
		of_clk_add_provider(np, of_clk_src_simple_get, cpu_clk);
		clk_set_min_rate(cpu_clk, min_rate);
		clk_set_max_rate(cpu_clk, max_rate);
	} else {
		kfree(cpud);
	}
	return IS_ERR(cpu_clk) ? PTR_ERR(cpu_clk) : 0;
}

int (*ca8299_clk_setup_fns[])(struct device_node *np,
			      struct ca8299_clk_reg *clk_reg,
			      struct ca8299_clk_param *clk_param) = {
	[CA_CLK_PLL]          = ca8299_clk_pll,
	[CA_CLK_DIVIDER]      = ca8299_clk_divider,
	[CA_CLK_MUX]          = ca8299_clk_mux,
	[CA_CLK_GATE]         = ca8299_clk_gate,
	[CA_CLK_FIXED_FACTOR] = ca8299_clk_fixed_factor,
	[CA_CLK_PE_DAEMON]    = ca8299_clk_pe_daemon,
	[CA_CLK_CPU_DAEMON]   = ca8299_clk_cpu_daemon,
	[CA_CLK_MMC_PHASE]    = ca8299_clk_mmc_register,
};

static void __init ca8299_soc_clks_setup(struct device_node *np)
{
	struct ca8299_clk_reg *clk_reg;
	struct device_node *childnp;
	const struct of_device_id *clk_id;
	struct ca8299_clk_param *clk_param;

	clk_reg = kzalloc(sizeof(*clk_reg), GFP_KERNEL);
	if (!clk_reg)
		return;

	clk_reg->regbase = of_iomap(np, 0);
	if (!clk_reg->regbase) {
		kfree(clk_reg);
		return;
	}
	spin_lock_init(&clk_reg->regs_lock);
	for_each_child_of_node(np, childnp) {
		clk_id = of_match_node(ca8299_soc_clks_ids, childnp);
		if (!clk_id)
			continue;

		clk_param = (struct ca8299_clk_param *)clk_id->data;
		if (ca8299_clk_setup_fns[clk_param->type](childnp, clk_reg,
							  clk_param))
			pr_err("ca8299_clk_setup_fns(%s) fail!\n", np->name);
	}
	epll_rate = CLK_EPLL_NUM;
	pr_info("%s: clk setup done\n", __func__);
}

module_param(readonly, bool, 0644);
MODULE_PARM_DESC(readonly, "CPU daemon is readonly");

CLK_OF_DECLARE(ca8299_soc_clks, "cortina-access,ca8299-soc-clks",
	       ca8299_soc_clks_setup);
