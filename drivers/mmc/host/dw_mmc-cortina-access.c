// SPDX-License-Identifier: GPL-2.0
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

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"
#include "dw_mmc-cortina-access.h"

static int dw_mci_ca_set_clock(struct dw_mci *host, struct mmc_ios *ios)
{
	struct dw_mci_ca_priv_data *priv = host->priv;
	u32 bus_hz;
	u32 io_drv_val = readl((void *)priv->g_io_drv_ctrl);
	int ret;

	if (ios->clock >= 100000000 && ios->clock < 20000000) {
		ret = clk_set_rate(host->ciu_clk, 10000000);
		io_drv_val &= ~(SD_DS_MASK << SD_DS_SHIFT);
		io_drv_val |= 0x77 << SD_DS_SHIFT;
	} else if (ios->clock >= 200000000) {
		ret = clk_set_rate(host->ciu_clk, 200000000);
		io_drv_val &= ~(SD_DS_MASK << SD_DS_SHIFT);
		io_drv_val |= 0x77 << SD_DS_SHIFT;
	} else {
		ret = clk_set_rate(host->ciu_clk, 50000000);
		io_drv_val &= ~(SD_DS_MASK << SD_DS_SHIFT);
		io_drv_val |= 0x33 << SD_DS_SHIFT;
		if (!IS_ERR(priv->drv_clk)) {
			if (!IS_ERR(priv->drv_clk))
				clk_set_phase(priv->drv_clk, 0x14);
			if (!IS_ERR(priv->smpl_clk))
				clk_set_phase(priv->smpl_clk, 0);
		}
	}

	if (ret)
		dev_warn(host->dev, "failed to set rate %uHz\n", ios->clock);

	writel(io_drv_val, (void *)priv->g_io_drv_ctrl);
	usleep_range(10, 15);

	bus_hz = clk_get_rate(host->ciu_clk);
	if (bus_hz != host->bus_hz) {
		host->bus_hz = bus_hz;
		host->current_speed = 0;
	}

	dev_dbg(host->dev, "bus_hz %uHz timing = %u, clock %u, io_drv %x\n",
		host->bus_hz, ios->timing, ios->clock, io_drv_val);

	return 0;
}

static void dw_mci_ca_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	struct dw_mci_ca_priv_data *priv = host->priv;

	if (ios->timing == MMC_TIMING_UHS_SDR50 ||
	    ios->timing == MMC_TIMING_UHS_SDR104) {
		set_bit(DW_MMC_CARD_NO_USE_HOLD, &host->slot->flags);
	} else {
		clear_bit(DW_MMC_CARD_NO_USE_HOLD, &host->slot->flags);
	}

	priv->ios = ios;

	dw_mci_ca_set_clock(host, ios);
}

static int dw_mci_ca_get_best_phase(u64 tuning_flag, int phase_step)
{
	int i;
	u64 val;
	unsigned int len;
	unsigned int range_start = 0;
	unsigned int range_length = 0;
	unsigned int middle_phase = 0;

	if (!tuning_flag)
		return -EIO;

	i = ffs(tuning_flag) - 1;

	while (i < phase_step) {
		val = ror64(tuning_flag, i);
		len = ffs(~val) - 1;

		if (len > range_length) {
			range_length = len;
			range_start = i;
		}

		i += len;
		i++;
	}

	// if range too small
	if (range_length < 3)
		return -EIO;

	middle_phase = range_start + range_length / 2;

	return middle_phase;
}

static int dw_mci_ca_clk_smpl_phase_tuning(struct dw_mci_slot *slot,
					   u32 opcode)
{
	struct dw_mci *host = slot->host;
	struct mmc_host *mmc = slot->mmc;
	struct dw_mci_ca_priv_data *priv = host->priv;

	int i = 0;
	int phase_step = 0;
	int best_smpl = 0;
	u64 tuning_flag = 0;

	if (IS_ERR(priv->smpl_clk)) {
		dev_err(host->dev, "smpl clk not defined\n");
		return -EIO;
	}

	if (priv->ios->clock == 200000000) {
		phase_step = 20;
	} else if (priv->ios->clock == 100000000) {
		phase_step = 40;
	} else {
		dev_err(host->dev, "can't support frequency %u\n",
			priv->ios->clock);
		return -EIO;
	}

	for (i = 0; i < phase_step; i++) {
		clk_set_phase(priv->smpl_clk, i);

		if (!mmc_send_tuning(mmc, opcode, NULL))
			tuning_flag |= 1 << i;
	}

	best_smpl = dw_mci_ca_get_best_phase(tuning_flag, phase_step);
	if (best_smpl < 0) {
		dev_err(host->dev, "fail to find smpl phase\n");
		return -EIO;
	}

	clk_set_phase(priv->smpl_clk, best_smpl);

	priv->cur_phase_smpl = best_smpl;

	return 0;
}

static int dw_mci_ca_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	int ret = 0;

	ret = dw_mci_ca_clk_smpl_phase_tuning(slot, opcode);
	if (ret) {
		dev_err(slot->host->dev, "tuning fail\n");
		goto tuning_error;
	}

tuning_error:
	return ret;
}

static int dw_mci_ca_init(struct dw_mci *host)
{
	struct dw_mci_ca_priv_data *priv;
	struct device_node *node = host->dev->of_node;
	u32 regs = 0;

	priv  = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	of_property_read_u32(node, "g_io_drv_ctrl", &regs);
	priv->g_io_drv_ctrl = ioremap(regs, 4);
	if (IS_ERR(priv->g_io_drv_ctrl)) {
		dev_err(host->dev, "fail to get drv ctrl\n");
		goto ERR;
	}

	priv->smpl_clk = of_clk_get_by_name(node, "sd-smpl");
	if (IS_ERR(priv->smpl_clk)) {
		dev_err(host->dev, "fail to get smpl clk\n");
		goto ERR;
	}

	host->priv = priv;

	return 0;
ERR:
	iounmap(priv->g_io_drv_ctrl);

	return -EINVAL;
}

#ifdef CONFIG_PM_SLEEP
static int dw_mci_ca_suspend(struct device *dev)
{
	return dw_mci_runtime_suspend(dev);
}

static int dw_mci_ca_resume(struct device *dev)
{
	return dw_mci_runtime_resume(dev);
}

#else
#define dw_mci_ca_suspend		NULL
#define dw_mci_ca_resume		NULL
#endif /* CONFIG_PM_SLEEP */

static const struct dw_mci_drv_data ca_drv_data = {
	.init = dw_mci_ca_init,
	.execute_tuning = dw_mci_ca_execute_tuning,
	.set_ios = dw_mci_ca_set_ios,
};

static const struct of_device_id dw_mci_ca_match[] = {
	{
		.compatible = "cortina-access,dw-mshc",
		.data = &ca_drv_data,
	},

	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_ca_match);

static int dw_mci_ca_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data;
	const struct of_device_id *match;
	int ret;

	match = of_match_node(dw_mci_ca_match, pdev->dev.of_node);
	drv_data = match->data;

	ret = dw_mci_pltfm_register(pdev, drv_data);
	if (ret)
		pr_err("%s: can't register dw_mmc-ca\n", __func__);

	return ret;
}

static const struct dev_pm_ops dw_mci_ca_pmops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw_mci_ca_suspend, dw_mci_ca_resume)
};

static struct platform_driver dw_mci_ca_pltfm_driver = {
	.probe		= dw_mci_ca_probe,
	.remove_new	= dw_mci_pltfm_remove,
	.driver		= {
		.name		= "dwmmc_cortina_access",
		.of_match_table	= dw_mci_ca_match,
		.pm		= &dw_mci_ca_pmops,
	},
};

module_platform_driver(dw_mci_ca_pltfm_driver);

MODULE_DESCRIPTION("Cortina-Access Specific DW-MSHC Driver Extension");
MODULE_AUTHOR("Jway Lin<jway.lin@cortina-access.com");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dwmmc_cortina_access");
