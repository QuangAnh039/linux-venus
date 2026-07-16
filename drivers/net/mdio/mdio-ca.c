// SPDX-License-Identifier: GPL-2.0
/*
 * MDIO bus controller driver for Cortina-Access SoCs
 *
 * Copyright (c) 2024 Cortina-Access, Inc.
 *		 http://www.cortina-access.com
 */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock_types.h>

#define MDIO_CFG_REG			0x00
#define MDIO_PRE_SCALE(x)		(((x) & 0xffff) << 16)
#define MDIO_MANUAL				BIT(0)
#define MDIO_ADDR_REG			0x04
#define MDIO_OP_C45(x)			(((x) & 0b11) << 18)
#define MDIO_ST_C45				BIT(16)
#define MDIO_ACCESS_RD_WR		BIT(15)
#define MDIO_REG_ADDR(x)		(((x) & 0x1f) << 8)
#define MDIO_DEV_ADDR(x)		((x) & 0x1f)
#define MDIO_WDDATA_REG			0x08
#define MDIO_RDDATA_REG			0x0c
#define MDIO_CTRL_REG			0x10
#define MDIO_CTRL_START			BIT(7)
#define MDIO_CTRL_DONE			BIT(0)
#define MDIO_AUTO_CFG_REG		0x14
#define MDIO_AUTO_INTV_REG		0x18
#define MDIO_AUTO_RM_REG		0x1c
#define MDIO_AUTO_AADDR_REG		0x20
#define MDIO_ENTAB0_REG			0x24
#define MDIO_ENTAB1_REG			0x28
#define MDIO_ITAB0_REG			0x2c
#define MDIO_ITAB1_REG			0x30
#define MDIO_IE0_REG			0x34
#define MDIO_INT0_REG			0x38
#define MDIO_IE1_REG			0x3c
#define MDIO_INT1_REG			0x40
#define MDIO_STAT_REG			0x44

#define MDIO_TIMEOUT_US			50000   /* 50 ms */
#define MDIO_SLEEP_US			10		/* 10 us */
#define __MDIO_CLOCK_MIN		(1000)	  /* 1 KHz */
#define __MDIO_CLOCK_MAX		(20000000) /* 20 MHz */
//#define __MDIO_CLOCK_DEF		(2500000)  /* 2.5 MHz */
#define MHZ						(1000000)
#define MHZDP(HZ)				(((HZ) / 10000) % 100)

struct ca_mdio_data {
	void __iomem	*membase;
	unsigned int	per_clk;
	unsigned int	bus_clk;
	int (*speed_set)(struct mii_bus *bus, u32 bus_clk);
	int (*speed_get)(struct mii_bus *bus, u32 *bus_clk);
};

struct ca_mdio_data *mdio_data_priv;

enum mdio_st_clause {
	MDIO_ST_CLAUSE_45 = 0,
	MDIO_ST_CLAUSE_22
};

enum mdio_c22_op_seq {
	MDIO_C22_WR = 1,
	MDIO_C22_RD = 2
};

enum mdio_c45_op_seq {
	MDIO_C45_WR_ADDR = 0,
	MDIO_C45_WR_DATA,
	MDIO_C45_RD_INC,
	MDIO_C45_RD
};

enum mdio_io_direction {
	MDIO_WR = 0,
	MDIO_RD = 1
};

static int ca_mdio_wait_ready(struct mii_bus *bus)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 busy;

	/* Do start */
	writel(MDIO_CTRL_START, priv->membase + MDIO_CTRL_REG);
	/* Wait for ready */
	if (readl_poll_timeout(priv->membase + MDIO_CTRL_REG, busy,
			       (busy & MDIO_CTRL_START) == 0,
			       MDIO_SLEEP_US, MDIO_TIMEOUT_US)) {
		return -1;
	}
	/* Clear done bit */
	writel(MDIO_CTRL_DONE, priv->membase + MDIO_CTRL_REG);

	return 0;
}

static int ca_mdio_read_c45(struct mii_bus *bus, int mii_id, int mmd,
			    int reg)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data;

	/* C45 write cfg and start */
	data = MDIO_OP_C45(MDIO_C45_WR_ADDR)
			| MDIO_ST_C45
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(mmd);
	data &= ~MDIO_ACCESS_RD_WR;
	writel(data, priv->membase + MDIO_ADDR_REG);
	dev_dbg(&bus->dev, "%s: 01MDIO_ADDR_REG: 0x%02x\n",
		__func__, readl(priv->membase + MDIO_ADDR_REG));
	/* Fill reg addr */
	writel(reg & 0xffff, priv->membase + MDIO_WDDATA_REG);

	/* Wait c45 write done */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	/* C45 read cfg and start */
	data = MDIO_OP_C45(MDIO_C45_RD)
			| MDIO_ST_C45
			| MDIO_ACCESS_RD_WR
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(mmd);
	writel(data, priv->membase + MDIO_ADDR_REG);
	dev_dbg(&bus->dev, "%s: 02MDIO_ADDR_REG: 0x%02x\n",
		__func__, readl(priv->membase + MDIO_ADDR_REG));

	/* Wait c45 read done */
	if (ca_mdio_wait_ready(bus)) {
		dev_err(&bus->dev, "MDIO bus is busy\n");
		return  -ETIMEDOUT;
	}

	/* Return data */
	return readl(priv->membase + MDIO_RDDATA_REG);
}

static int ca_mdio_read_c22(struct mii_bus *bus, int mii_id, int regnum)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data;

	/* Check for ready */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	/* C22 read cfg and start */
	//data = readl(priv->membase + MDIO_ADDR_REG);
	data = MDIO_OP_C45(MDIO_C45_RD)
			| MDIO_ACCESS_RD_WR
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(regnum);
	data &= ~MDIO_ST_C45;
	writel(data, priv->membase + MDIO_ADDR_REG);
	writel(MDIO_CTRL_START, priv->membase + MDIO_CTRL_REG);

	/* Wait c22 read done */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	/* Return data */
	return readl(priv->membase + MDIO_RDDATA_REG);
}

static int ca_mdio_write_c45(struct mii_bus *bus, int mii_id, int mmd,
			     int reg, u16 value)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data;

	/* C45 write addr and start */
	//data = readl(priv->membase + MDIO_ADDR_REG);
	data = MDIO_OP_C45(MDIO_C45_WR_ADDR)
			| MDIO_ST_C45
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(mmd);
	data &= ~MDIO_ACCESS_RD_WR;
	writel(data, priv->membase + MDIO_ADDR_REG);
	dev_dbg(&bus->dev, "%s: 01MDIO_ADDR_REG: 0x%02x\n",
		__func__, readl(priv->membase + MDIO_ADDR_REG));
	/* Fill reg addr */
	writel(reg & 0xffff, priv->membase + MDIO_WDDATA_REG);

	/* Wait done */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	/* C45 write data and start */
	//data = readl(priv->membase + MDIO_ADDR_REG);
	data = MDIO_OP_C45(MDIO_C45_WR_DATA)
			| MDIO_ST_C45
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(mmd);
	data &= ~MDIO_ACCESS_RD_WR;
	writel(data, priv->membase + MDIO_ADDR_REG);
	dev_dbg(&bus->dev, "%s: 02MDIO_ADDR_REG: 0x%02x\n",
		__func__, readl(priv->membase + MDIO_ADDR_REG));
	/* Config the data needed writing */
	writel(value, priv->membase + MDIO_WDDATA_REG);

	/* Wait done */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	return 0;
}

static int ca_mdio_write_c22(struct mii_bus *bus, int mii_id, int regnum,
			     u16 value)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data;

	/* C22 write data and start */
	//data = readl(priv->membase + MDIO_ADDR_REG);
	data = MDIO_OP_C45(MDIO_C22_WR)
			| MDIO_DEV_ADDR(mii_id)
			| MDIO_REG_ADDR(regnum);
	data &= ~MDIO_ST_C45;
	data &= ~MDIO_ACCESS_RD_WR;
	writel(data, priv->membase + MDIO_ADDR_REG);
	/* Config the data needed writing */
	writel(value, priv->membase + MDIO_WDDATA_REG);

	/* Wait for ready */
	if (ca_mdio_wait_ready(bus))
		return -ETIMEDOUT;

	return 0;
}

static int ca_mdio_speed_set(struct mii_bus *bus, u32 bus_clk)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data, prer;

	if (bus_clk < __MDIO_CLOCK_MIN || bus_clk > __MDIO_CLOCK_MAX)
		return -ENODATA;

	/* Calculate pre scale for MDIO clock adjust */
	prer = (priv->per_clk / (bus_clk << 1)) - 1;
	if ((priv->per_clk % (bus_clk << 1)) > bus_clk)
		prer += 1;

	data = readl(priv->membase + MDIO_CFG_REG);
	data = MDIO_PRE_SCALE(prer) | MDIO_MANUAL;
	writel(data, priv->membase + MDIO_CFG_REG);

	return 0;
}

static int ca_mdio_speed_get(struct mii_bus *bus, u32 *bus_clk)
{
	struct ca_mdio_data *priv = bus->priv;
	u32 data;

	data = readl(priv->membase + MDIO_CFG_REG);
	*bus_clk = (priv->per_clk / (((data >> 16) & 0xffff) + 1)) >> 1;

	return 0;
}

static int ca_mdio_probe(struct platform_device *pdev)
{
	struct ca_mdio_data *priv;
	struct mii_bus *bus;
	unsigned int per_clk, bus_clk;
	int ret;

	if (!pdev) {
		dev_err(NULL, "pdev is NULL!\r\n");
		return -ENODEV;
	}

	bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;

	priv->membase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->membase))
		return PTR_ERR(priv->membase);

	bus->name = "ca_mdio";
	bus->read = ca_mdio_read_c22;
	bus->write = ca_mdio_write_c22;
	bus->read_c45 = ca_mdio_read_c45;
	bus->write_c45 = ca_mdio_write_c45;
	bus->parent = &pdev->dev;

	/* PHY addr(0 ~ 4) be ignored when probing */
	bus->phy_mask = 0x1F;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s%d", pdev->name, pdev->id);
	priv->speed_set = ca_mdio_speed_set;
	priv->speed_get = ca_mdio_speed_get;

	/* Default per clock 125MHz if not specified */
	if (!of_property_read_u32(pdev->dev.of_node, "peri-clock-frequency", &per_clk))
		priv->per_clk = per_clk;
	else
		priv->per_clk = 125000000;

	/* Default MDIO bus clock 2.5MHz if not specified */
	if (!of_property_read_u32(pdev->dev.of_node, "clock-frequency", &bus_clk))
		priv->bus_clk = bus_clk;
	else
		priv->bus_clk = 2500000;

	ca_mdio_speed_set(bus, priv->bus_clk);

	dev_info(&pdev->dev, "per_clk:%u.%02u MHz, MDIO bus speed:%u.%02u MHz\n",
		 priv->per_clk / MHZ,
		 MHZDP(priv->per_clk),
		 priv->bus_clk / MHZ,
		 MHZDP(priv->bus_clk));

	ret = of_mdiobus_register(bus, pdev->dev.of_node);
	if (ret) {
		dev_err(&pdev->dev, "Cannot register CA MDIO bus!\n");
		return ret;
	}
	platform_set_drvdata(pdev, bus);

	dev_info(&pdev->dev, "mii-bus %s has registered\n", bus->id);

	return 0;
}

static int ca_mdio_remove(struct platform_device *pdev)
{
	struct mii_bus *bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus);

	return 0;
}

static const struct of_device_id ca_mdio_dt_ids[] = {
	{.compatible = "cortina-access,mdio"},
	{}
};

static struct platform_driver ca_mdio_driver = {
	.probe = ca_mdio_probe,
	.remove = ca_mdio_remove,
	.driver = {
		.name = "ca-mdio",
		.of_match_table = ca_mdio_dt_ids,
	},
};

module_platform_driver(ca_mdio_driver);

MODULE_AUTHOR("Cortina-Access Co., Ltd.");
MODULE_DESCRIPTION("Cortina-Access MDIO driver");
MODULE_LICENSE("GPL");
