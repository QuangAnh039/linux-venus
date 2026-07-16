/* SPDX-License-Identifier: GPL-2.0  */
/*
 * Cortina-Access Specific Extensions for Synopsys DW Multimedia Card Interface driver
 *
 * Copyright (C) 2025, Cortina-Accss Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _DW_MMC_CA_H_
#define _DW_MMC_CA_H_

#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/clk.h>

#define CLK_SMPL_PHASE_CRTL (24)
#define CLK_SMPL_PHASE_MASK (0x3f)
#define CLK_DRV_PHASE_CRTL (16)
#define CLK_DRV_PHASE_MASK (0x3f)
#define PHASE_RESET_OVERRIDE (14)
#define SMPL_PHASE_SHIFT (64)
#define DRV_PHASE_SHIFT (64)
#define SD_DLL_RESET_OVERRIDE_SHIFT (14)

#define UHS_REG_EXT (0x108)
#define REG_EXT_SMPL_CTRL (16)
#define REG_EXT_SMPL_PHASE_LVL (16)

#define SD_DS_MASK (0xff)
#define SD_DS_SHIFT (16)

struct dw_mci_ca_priv_data {
	u32 cur_phase_drv;
	u32 cur_phase_smpl;
	struct mmc_ios *ios;
	u32 *sd_dll_ctrl;
	u32 *g_io_drv_ctrl;
	u8 debug_flag;
	struct clk *smpl_clk;
	struct clk *drv_clk;
};

#endif
