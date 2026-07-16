/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CORTINA_ACCESS_MMC_PHASE_H
#define __CORTINA_ACCESS_MMC_PHASE_H

#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/delay.h>

struct ca_mmc_clock {
	struct clk_hw hw;
	void __iomem *reg;
	spinlock_t *lock;	/* register access lock */
	int shift;
	u32 mask;
};

#define to_mmc_clock(_hw) container_of(_hw, struct ca_mmc_clock, hw)

#define MMC_PHASE_RESET_OVERRIDE (14)
#define SD_DLL_DEFAULT	(0x143000)

struct clk *ca_mmc_phase_clk_register(const char *name,
				      const char *const *parent_name,
				      spinlock_t *lock,
				      void __iomem *reg, int shift, int width);
#endif
