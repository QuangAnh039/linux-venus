// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Cortina Access Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "ca-mmc-phase.h"

static unsigned long ca_mmc_recalc(struct clk_hw *hw,
				   unsigned long parent_rate)
{
	return parent_rate;
}

static int ca_mmc_get_phase(struct clk_hw *hw)
{
	struct ca_mmc_clock *mmc_clock = to_mmc_clock(hw);

	return readl(mmc_clock->reg) >> mmc_clock->shift;
}

static int ca_mmc_set_phase(struct clk_hw *hw, int degrees)
{
	struct ca_mmc_clock *mmc_clock = to_mmc_clock(hw);
	u32 val = readl(mmc_clock->reg);
	unsigned long flags;

	spin_lock_irqsave(mmc_clock->lock, flags);
	val &= ~(mmc_clock->mask << mmc_clock->shift);
	val |= (degrees << mmc_clock->shift);
	val |= (1 << MMC_PHASE_RESET_OVERRIDE);
	writel(val, mmc_clock->reg);
	spin_unlock_irqrestore(mmc_clock->lock, flags);

	usleep_range(10, 11);

	spin_lock_irqsave(mmc_clock->lock, flags);
	val = readl(mmc_clock->reg);
	val &= ~(1 << MMC_PHASE_RESET_OVERRIDE);
	writel(val, mmc_clock->reg);
	spin_unlock_irqrestore(mmc_clock->lock, flags);

	return 0;
}

static int ca_mmc_init(struct clk_hw *hw)
{
	struct ca_mmc_clock *mmc_clock = to_mmc_clock(hw);

	writel(SD_DLL_DEFAULT, mmc_clock->reg);

	return 0;
}

const struct clk_ops ca_mmc_clk_ops = {
	.recalc_rate    = ca_mmc_recalc,
	.get_phase	= ca_mmc_get_phase,
	.set_phase	= ca_mmc_set_phase,
	.init		= ca_mmc_init,
};

struct clk *ca_mmc_phase_clk_register(const char *name,
				      const char *const *parent_name,
				      spinlock_t *lock,
				      void __iomem *reg, int shift,
				      int width)
{
	struct clk_init_data init;
	struct ca_mmc_clock *mmc_clock;
	struct clk *clk;

	mmc_clock = kmalloc(sizeof(*mmc_clock), GFP_KERNEL);
	if (!mmc_clock)
		return NULL;

	init.name = name;
	init.flags = 0;
	init.num_parents = parent_name ? 1 : 0;
	init.parent_names = parent_name ? parent_name : NULL;
	init.ops = &ca_mmc_clk_ops;

	mmc_clock->hw.init = &init;
	mmc_clock->reg = reg;
	mmc_clock->shift = shift;
	mmc_clock->lock = lock;
	mmc_clock->mask = (1 << width) - 1;

	clk = clk_register(NULL, &mmc_clock->hw);
	if (IS_ERR(clk))
		goto err_free;

	return clk;

err_free:
	kfree(mmc_clock);
	return NULL;
}
