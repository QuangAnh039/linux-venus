// SPDX-License-Identifier: GPL-2.0

/*
 * FILE NAME i2c-ca.c
 *
 * BRIEF MODULE DESCRIPTION
 *  Driver for Cortina Access I2C(BIW) controller
 *
 * Copyright (C) 2016 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 *  Based on i2c-mv64xxx.c
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/delay.h>

#include <soc/cortina-access/registers.h>
#if defined(CONFIG_ARCH_CA_MERCURY)
#define PER_BIW_CFG PER_BIW0_CFG
#define PER_BIW_CFG_t PER_BIW0_CFG_t

#define PER_BIW_CTRL PER_BIW0_CTRL
#define PER_BIW_CTRL_t PER_BIW0_CTRL_t

#define PER_BIW_TXR PER_BIW0_TXR
#define PER_BIW_TXR_t PER_BIW0_TXR_t

#define PER_BIW_RXR PER_BIW0_RXR
#define PER_BIW_RXR_t PER_BIW0_RXR_t

#define PER_BIW_ACK PER_BIW0_ACK
#define PER_BIW_ACK_t PER_BIW0_ACK_t

#define PER_BIW_INT_0 PER_BIW0_INT_0
#define PER_BIW_INT_0_t PER_BIW0_INT_0_t
#endif

/* Register defines */
#define CA_BIW_CFG		0x00
#define CA_BIW_CTRL		0x04
#define CA_BIW_TXR		0x08
#define CA_BIW_RXR		0x0C
#define CA_BIW_ACK		0x10
#define CA_BIW_IE0		0x14
#define CA_BIW_INT0		0x18
#define CA_BIW_IE1		0x1C
#define CA_BIW_INT1		0x20
#define CA_BIW_STAT		0x24

/* CA_BIW_CFG BIT */
#define BIW_CFG_CORE_EN		BIT(0)
#define BIW_CFG_PRER_OFF	16
#define BIW_CFG_PRER_MASK	0xFFFF0000

/* CA_BIW_CTRL BIT */
#define BIW_CTRL_DONE		BIT(0)
#define BIW_CTRL_ACK_IN		BIT(3)
#define BIW_CTRL_WRITE		BIT(4)
#define BIW_CTRL_READ		BIT(5)
#define BIW_CTRL_STOP		BIT(6)
#define BIW_CTRL_START		BIT(7)

/* CA_BIW_TXR BIT */
#define BIW_TXR_OFF		0
#define BIW_TXR_MASK		0x000000FF

/* CA_BIW_RXR BIT */
#define BIW_RXR_OFF		0
#define BIW_RXR_MASK		0x000000FF

/* CA_BIW_ACK */
#define BIW_ACK_AL		BIT(0)
#define BIW_ACK_BUSY		BIT(1)
#define BIW_ACK_ACK_OUT		BIT(2)

/* CA_BIW_IE0 BIT */
#define BIW_IE_BIT		BIT(0)

/* CA_BIW_INT0 */
#define BIW_INT_BIT		BIT(0)

/* CA_BIW_STAT */
#define BIW_STAT_BIT		BIT(0)

/* proprietary driver states */
enum {
	I2C_STATE_INVALID,
	I2C_STATE_IDLE,
	I2C_STATE_START,
	I2C_STATE_WAITING_FOR_ADDR_1_ACK,
	I2C_STATE_WAITING_FOR_ADDR_2_ACK,
	I2C_STATE_WAITING_FOR_WRITE_ACK,
	I2C_STATE_WAITING_FOR_READ_ACK,
};

struct ca_i2c_data {
	int			irq;
	u32			state;
	u32			aborting;
	void __iomem		*reg_base;
	u32			addr1;
	u32			addr2;
	u32			bytes_left;
	u32			byte_posn;
	u32			block;
	int			rc;
	u32			ref_freq;
	u32			bus_freq;
	u32			ack;
#if defined(CONFIG_HAVE_CLK)
	struct clk		*clk;
#endif
	wait_queue_head_t	waitq;
	spinlock_t		lock;		/* lock access to i2c_data */
	struct i2c_msg		*msg;
	struct i2c_adapter	adapter;
};

#define CA_I2C_CTLR_NAME	"ca-i2c"

/* i2c Platform Device, Driver Data */
struct ca_i2c_pdata {
	u32			ref_freq;	/* Reference Clock */
	u32			bus_freq;	/* Serial Clock */
	u16			timeout;	/* In milliseconds */
	u16			retries;	/* retry count */
};

#define DEFAULT_BUS_FREQ	100000		/* Hz */
#define FM_BUS_FREQ		400000
#define HSM_BUS_FREQ		3400000
#define DEFAULT_TIMEOUT		1000
#define DEFAULT_RETRIES		3

#define I2C_M_NOSTOP		0x1000

#define REG_ACCESS_RD		0x00000001
#define REG_ACCESS_WR		0x00000002
#define FUNC_ENTRY		0x00000004
#define FSM_TRACE		0x00000008
#define IGNORE_ERR_WARN		0x00000010
/*Turn off error info by default*/
static u32 dbg_flag = 0x00000010;

static inline u32 ca_rd_reg(struct ca_i2c_data *drv_data,
			    unsigned int reg)
{
	if (dbg_flag & REG_ACCESS_RD) {
		u32 val = readl(drv_data->reg_base + reg);

		dev_dbg(&drv_data->adapter.dev, "R, reg=0x%02x, val 0x%08x\n",
			reg, val);

		return val;
	}

	return readl(drv_data->reg_base + reg);
}

static inline void ca_wr_reg(struct ca_i2c_data *drv_data,
			     unsigned int reg, u32 val)
{
	if (dbg_flag & REG_ACCESS_WR)
		dev_dbg(&drv_data->adapter.dev, "W, reg=0x%02x, val 0x%08x\n",
			reg, val);

	writel(val, drv_data->reg_base + reg);
}

static void dump_regs(struct ca_i2c_data *drv_data)
{
	u32 reg;

	reg = CA_BIW_CFG;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_CFG = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_CTRL;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_CTRL = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_TXR;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_TXR = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_RXR;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_RXR = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_ACK;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_ACK = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_IE0;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_IE0 = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_INT0;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_INT0 = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_IE1;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_IE1 = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_INT1;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_INT1 = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
	reg = CA_BIW_STAT;
	dev_info(&drv_data->adapter.dev, "\t0x%02X: BIW_STAT = 0x%x\n",
		 reg, ca_rd_reg(drv_data, reg));
}

/*
 *****************************************************************************
 *
 *	Finite State Machine & Interrupt Routines
 *
 *****************************************************************************
 */

static void ca_i2c_hw_fini(struct ca_i2c_data *drv_data)
{
	ca_wr_reg(drv_data, CA_BIW_IE0, 0);

	ca_wr_reg(drv_data, CA_BIW_CFG, 0);

	drv_data->state = I2C_STATE_INVALID;
}

/* Reset hardware and initialize FSM */
static void ca_i2c_hw_init(struct ca_i2c_data *drv_data)
{
	PER_BIW_CFG_t reg_biw_cfg;

	reg_biw_cfg.wrd = ca_rd_reg(drv_data, CA_BIW_CFG);

	/* reset */
	if (reg_biw_cfg.bf.core_en) {
		reg_biw_cfg.bf.biw_soft_reset = 1;
		ca_wr_reg(drv_data, CA_BIW_CFG, reg_biw_cfg.wrd);

		mdelay(50);

		reg_biw_cfg.bf.biw_soft_reset = 0;
		ca_wr_reg(drv_data, CA_BIW_CFG, reg_biw_cfg.wrd);
	}

	reg_biw_cfg.bf.prer = (drv_data->ref_freq / (5 * drv_data->bus_freq))
			      - 1;

	reg_biw_cfg.bf.core_en = 1;
	ca_wr_reg(drv_data, CA_BIW_CFG, reg_biw_cfg.wrd);

	mdelay(50);

	ca_wr_reg(drv_data, CA_BIW_IE0, 1);

	drv_data->state = I2C_STATE_IDLE;
}

static void ca_i2c_fsm(struct ca_i2c_data *drv_data)
{
	struct i2c_msg *msg = drv_data->msg;
	u32 state = drv_data->state;

	/*
	 * If error result occurs, stop the following actions and reset hardware
	 */
	if ((drv_data->ack & BIW_ACK_ACK_OUT) || (drv_data->ack & BIW_ACK_AL)) {
		if (drv_data->ack & BIW_ACK_AL)
			drv_data->rc = -EAGAIN;
		else
			drv_data->rc = -ENXIO;
		drv_data->state = I2C_STATE_INVALID;
	}

	/* The status from the ctlr [mostly] tells us what to do next */
	switch (drv_data->state) {
	case I2C_STATE_START:
		/* next state */
		drv_data->state = I2C_STATE_WAITING_FOR_ADDR_1_ACK;

		ca_wr_reg(drv_data, CA_BIW_TXR, drv_data->addr1);
		if (drv_data->bytes_left == I2C_SMBUS_QUICK)
			ca_wr_reg(drv_data, CA_BIW_CTRL,
				  BIW_CTRL_START | BIW_CTRL_WRITE |
				  BIW_CTRL_STOP);
		else
			ca_wr_reg(drv_data, CA_BIW_CTRL,
				  BIW_CTRL_START | BIW_CTRL_WRITE);
		break;

	/* Performing a read/write */
	case I2C_STATE_WAITING_FOR_ADDR_1_ACK:
		if ((msg->flags & I2C_M_TEN) &&
		    !(msg->flags & I2C_M_NOSTART)) {
			/* next state */
			drv_data->state = I2C_STATE_WAITING_FOR_ADDR_2_ACK;

			ca_wr_reg(drv_data, CA_BIW_TXR,
				  drv_data->addr2);
			ca_wr_reg(drv_data, CA_BIW_CTRL,
				  BIW_CTRL_WRITE);
			break;
		}
		fallthrough;

	case I2C_STATE_WAITING_FOR_ADDR_2_ACK:
		if (msg->flags & I2C_M_RD) {
			/* next state */
			drv_data->state = I2C_STATE_WAITING_FOR_READ_ACK;

			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & I2C_M_NOSTOP))
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_READ);
			else
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_READ | BIW_CTRL_ACK_IN |
					  BIW_CTRL_STOP);
		} else {
			if (drv_data->bytes_left == I2C_SMBUS_QUICK) {
				drv_data->block = 0;
				wake_up_interruptible(&drv_data->waitq);
				break;
			}

			/* next state */
			drv_data->state = I2C_STATE_WAITING_FOR_WRITE_ACK;

			ca_wr_reg(drv_data, CA_BIW_TXR,
				  msg->buf[drv_data->byte_posn++]);
			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & I2C_M_NOSTOP))
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_WRITE);
			else
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_WRITE | BIW_CTRL_STOP);
		}
		break;

	case I2C_STATE_WAITING_FOR_WRITE_ACK:
		if (drv_data->bytes_left) {
			ca_wr_reg(drv_data, CA_BIW_TXR,
				  msg->buf[drv_data->byte_posn++]);
			drv_data->bytes_left--;

			if (drv_data->bytes_left ||
			    (msg->flags & I2C_M_NOSTOP))
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_WRITE);
			else
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_WRITE | BIW_CTRL_STOP);
		} else {
			drv_data->block = 0;
			wake_up_interruptible(&drv_data->waitq);
		}
		break;

	case I2C_STATE_WAITING_FOR_READ_ACK:
		msg->buf[drv_data->byte_posn++] =
					ca_rd_reg(drv_data, CA_BIW_RXR);

		if (drv_data->bytes_left) {
			drv_data->bytes_left--;

			if (drv_data->bytes_left)
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_READ);
			else
				ca_wr_reg(drv_data, CA_BIW_CTRL,
					  BIW_CTRL_READ | BIW_CTRL_ACK_IN |
					  BIW_CTRL_STOP);
		} else {
			drv_data->block = 0;
			wake_up_interruptible(&drv_data->waitq);
		}
		break;

	case I2C_STATE_INVALID:
	default:
		if (!(dbg_flag & IGNORE_ERR_WARN))
			dev_err(&drv_data->adapter.dev,
				"Ctlr Error -- state: 0x%x, ack: 0x%x, addr: 0x%x, flags: 0x%x, left: 0x%x\n",
				drv_data->state, drv_data->ack,
				msg->addr, msg->flags, drv_data->bytes_left);
		if (!drv_data->rc)
			drv_data->rc = -EIO;

		ca_i2c_hw_init(drv_data);

		drv_data->block = 0;
		wake_up_interruptible(&drv_data->waitq);
	}

	if (dbg_flag & FSM_TRACE)
		dev_dbg(&drv_data->adapter.dev, "state curr %d, next %d\n",
			state, drv_data->state);
}

static irqreturn_t ca_i2c_intr(int irq, void *param)
{
	struct ca_i2c_data *drv_data = param;
	PER_BIW_CTRL_t reg_biw_ctrl;
	PER_BIW_INT_0_t reg_biw_int0;
	irqreturn_t rc = IRQ_NONE;

	/* disable biw interrupt */
	ca_wr_reg(drv_data, CA_BIW_IE0, 0);

	reg_biw_int0.wrd = ca_rd_reg(drv_data, CA_BIW_INT0);
	reg_biw_int0.bf.biwi = 1;
	ca_wr_reg(drv_data, CA_BIW_INT0, reg_biw_int0.wrd);

	reg_biw_ctrl.wrd = ca_rd_reg(drv_data, CA_BIW_CTRL);
	if (reg_biw_ctrl.bf.biwdone != 1)
		if (!(dbg_flag & IGNORE_ERR_WARN))
			dev_err(&drv_data->adapter.dev, "ca: no done signal!\n");
	ca_wr_reg(drv_data, CA_BIW_CTRL, reg_biw_ctrl.wrd);

	drv_data->ack = ca_rd_reg(drv_data, CA_BIW_ACK);

	ca_i2c_fsm(drv_data);
	rc = IRQ_HANDLED;

	/* enable biw interrupt */
	ca_wr_reg(drv_data, CA_BIW_IE0, 1);

	return rc;
}

/*
 *****************************************************************************
 *
 *	I2C Msg Execution Routines
 *
 *****************************************************************************
 */
static void ca_i2c_prepare_for_io(struct ca_i2c_data *drv_data,
				  struct i2c_msg *msg)
{
	u32	dir = 0;

	drv_data->msg = msg;
	drv_data->byte_posn = 0;
	drv_data->bytes_left = msg->len;
	drv_data->aborting = 0;
	drv_data->rc = 0;
	drv_data->ack = 0;

	if (msg->flags & I2C_M_RD)
		dir = 1;

	if (msg->flags & I2C_M_TEN) {
		drv_data->addr1 = 0xf0 | (((u32)msg->addr & 0x300) >> 7) | dir;
		drv_data->addr2 = (u32)msg->addr & 0xff;
	} else {
		drv_data->addr1 = ((u32)msg->addr & 0x7f) << 1 | dir;
		drv_data->addr2 = 0;
	}
}

static void ca_i2c_wait_for_completion(struct ca_i2c_data *drv_data)
{
	long time_left;
	char abort = 0;

	time_left = wait_event_interruptible_timeout(drv_data->waitq,
						     !drv_data->block,
						     drv_data->adapter.timeout);

	spin_lock(&drv_data->lock);
	if (!time_left) { /* Timed out */
		drv_data->rc = -ETIMEDOUT;
		abort = 1;
		dump_regs(drv_data);
	} else if (time_left < 0) { /* Interrupted/Error */
		drv_data->rc = time_left; /* errno value */
		abort = 1;
	}
	spin_unlock(&drv_data->lock);

	if (abort && drv_data->block) {
		spin_lock(&drv_data->lock);
		drv_data->aborting = 1;
		spin_unlock(&drv_data->lock);

		time_left = wait_event_timeout(drv_data->waitq,
					       !drv_data->block,
					       drv_data->adapter.timeout);

		if (time_left <= 0 && drv_data->block) {
			drv_data->state = I2C_STATE_IDLE;
			if (!(dbg_flag & IGNORE_ERR_WARN))
				dev_err(&drv_data->adapter.dev,
					"ca: I2C bus locked, block: %d, time_left: %d\n",
					drv_data->block, (int)time_left);
			ca_i2c_hw_init(drv_data);
			drv_data->block = 0;
		}
	}
}

static int ca_i2c_execute_msg(struct ca_i2c_data *drv_data,
			      struct i2c_msg *msg)
{
	spin_lock(&drv_data->lock);

	ca_i2c_prepare_for_io(drv_data, msg);

	if (unlikely(msg->flags & I2C_M_NOSTART))
		drv_data->state = I2C_STATE_WAITING_FOR_ADDR_2_ACK;
	else
		drv_data->state = I2C_STATE_START;

	drv_data->block = 1;
	ca_i2c_fsm(drv_data);

	spin_unlock(&drv_data->lock);

	ca_i2c_wait_for_completion(drv_data);

	return drv_data->rc;
}

/*
 *****************************************************************************
 *
 *	I2C Core Support Routines (Interface to higher level I2C code)
 *
 *****************************************************************************
 */
static u32 ca_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static int ca_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
		       int num)
{
	struct ca_i2c_data *drv_data = i2c_get_adapdata(adap);
	int	i, rc;

	for (i = 0; i < num; i++) {
		/* check if next msg is continus msg */
		if (i < (num - 1))
			msgs[i].flags |= I2C_M_NOSTOP;

		rc = ca_i2c_execute_msg(drv_data, &msgs[i]);
		if (rc)
			return rc;
	}

	return num;
}

static const struct i2c_algorithm ca_i2c_algo = {
	.master_xfer = ca_i2c_xfer,
	.functionality = ca_i2c_functionality,
};

/*
 *****************************************************************************
 *
 *	Driver Interface & Early Init Routines
 *
 *****************************************************************************
 */
static const struct of_device_id ca_i2c_of_match_table[] = {
	{ .compatible = "cortina-access,venus-i2c", },
	{ .compatible = "cortina-access,mercury-i2c", },
	{ },
};
MODULE_DEVICE_TABLE(of, ca_i2c_of_match_table);

#ifdef CONFIG_OF
static int
ca_of_config(struct ca_i2c_data *drv_data,
	     struct device *dev)
{
	/* CLK is mandatory when using DT to describe the i2c bus. We
	 * need to know tclk in order to calculate bus clock
	 * factors.
	 */
#if !defined(CONFIG_HAVE_CLK)
	/* Have OF but no CLK */
	return -ENODEV;
#else
	const struct of_device_id *device;
	struct device_node *np = dev->of_node;
	u32 bus_freq, timeout, retries;
	int rc = 0;

	if (IS_ERR(drv_data->clk)) {
		rc = -ENODEV;
		goto out;
	}
	drv_data->ref_freq = clk_get_rate(drv_data->clk);

	if (of_property_read_u32(np, "clock-frequency", &bus_freq))
		drv_data->bus_freq = DEFAULT_BUS_FREQ;
	else
		drv_data->bus_freq = bus_freq;
	if (drv_data->bus_freq != DEFAULT_BUS_FREQ &&
	    drv_data->bus_freq != FM_BUS_FREQ &&
	    drv_data->bus_freq != HSM_BUS_FREQ) {
		dev_warn(dev, "WARNING: I2C SCL set to %dKHz!\n",
			 drv_data->bus_freq / 1000);
	}

	if (of_property_read_u32(np, "timeout", &timeout)) {
		dev_err(dev, "invalid timeout on %s\n", np->full_name);
		drv_data->adapter.timeout = msecs_to_jiffies(DEFAULT_TIMEOUT);
	} else {
		drv_data->adapter.timeout = msecs_to_jiffies(timeout);
	}

	if (of_property_read_u32(np, "retries", &retries)) {
		dev_err(dev, "invalid retries on %s\n", np->full_name);
		drv_data->adapter.retries = DEFAULT_RETRIES;
	} else {
		drv_data->adapter.retries = retries;
	}

	drv_data->irq = irq_of_parse_and_map(np, 0);

	device = of_match_device(ca_i2c_of_match_table, dev);
	if (!device)
		return -ENODEV;

out:
	return rc;
#endif
}
#else /* CONFIG_OF */
static int
ca_of_config(struct ca_i2c_data *drv_data,
	     struct device *dev)
{
	return -ENODEV;
}
#endif /* CONFIG_OF */

static int ca_i2c_probe(struct platform_device *pd)
{
	struct ca_i2c_data		*drv_data;
	struct ca_i2c_pdata	*pdata = dev_get_platdata(&pd->dev);
	struct resource	*res;
	int	rc;

	if ((!pdata && !pd->dev.of_node))
		return -ENODEV;

	drv_data = devm_kzalloc(&pd->dev, sizeof(struct ca_i2c_data),
				GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	res = platform_get_resource(pd, IORESOURCE_MEM, 0);
	drv_data->reg_base = devm_ioremap_resource(&pd->dev, res);
	if (IS_ERR(drv_data->reg_base))
		return PTR_ERR(drv_data->reg_base);
	dev_info(&pd->dev, "resource - %pr mapped at 0x%pK\n", res,
		 drv_data->reg_base);

	strscpy(drv_data->adapter.name, CA_I2C_CTLR_NAME " adapter",
		sizeof(drv_data->adapter.name));

	init_waitqueue_head(&drv_data->waitq);
	spin_lock_init(&drv_data->lock);

	drv_data->state = I2C_STATE_INVALID;

#if defined(CONFIG_HAVE_CLK)
	/* Not all platforms have a clk */
	drv_data->clk = devm_clk_get(&pd->dev, NULL);
	if (!IS_ERR(drv_data->clk))
		clk_prepare_enable(drv_data->clk);
#endif

	if (pdata) {
		drv_data->ref_freq = pdata->ref_freq;
		drv_data->bus_freq = pdata->bus_freq;
		drv_data->adapter.timeout = msecs_to_jiffies(pdata->timeout);
		drv_data->adapter.retries = pdata->retries;
		drv_data->irq = platform_get_irq(pd, 0);
	} else if (pd->dev.of_node) {
		rc = ca_of_config(drv_data, &pd->dev);
		if (rc)
			goto exit_clk;
	}
	if (drv_data->irq < 0) {
		rc = -ENXIO;
		goto exit_clk;
	}
	dev_info(&pd->dev, "irq %d\n", drv_data->irq);
	rc = devm_request_irq(&pd->dev, drv_data->irq, ca_i2c_intr, 0,
			      CA_I2C_CTLR_NAME, drv_data);
	if (rc) {
		dev_err(&drv_data->adapter.dev,
			"ca: Can't register intr handler irq%d: %d\n",
			drv_data->irq, rc);
		goto exit_clk;
	}

	drv_data->adapter.dev.parent = &pd->dev;
	drv_data->adapter.algo = &ca_i2c_algo;
	drv_data->adapter.owner = THIS_MODULE;
	drv_data->adapter.class = I2C_CLASS_DEPRECATED;
	drv_data->adapter.nr = pd->id;
	drv_data->adapter.dev.of_node = pd->dev.of_node;
	platform_set_drvdata(pd, drv_data);
	i2c_set_adapdata(&drv_data->adapter, drv_data);

	ca_i2c_hw_init(drv_data);

	rc = i2c_add_numbered_adapter(&drv_data->adapter);
	if (rc != 0) {
		dev_err(&drv_data->adapter.dev,
			"ca: Can't add i2c adapter, rc: %d\n", -rc);
		goto exit_clk;
	}

	return 0;

exit_clk:
#if defined(CONFIG_HAVE_CLK)
	/* Not all platforms have a clk */
	if (!IS_ERR(drv_data->clk)) {
		clk_disable(drv_data->clk);
		clk_unprepare(drv_data->clk);
	}
#endif

	return rc;
}

static int ca_i2c_remove(struct platform_device *dev)
{
	struct ca_i2c_data *drv_data = platform_get_drvdata(dev);

	i2c_del_adapter(&drv_data->adapter);
	ca_i2c_hw_fini(drv_data);
#if defined(CONFIG_HAVE_CLK)
	if (!IS_ERR(drv_data->clk))
		clk_disable_unprepare(drv_data->clk);
#endif

	return 0;
}

static struct platform_driver ca_i2c_driver = {
	.probe	= ca_i2c_probe,
	.remove	= ca_i2c_remove,
	.driver	= {
		.name	= CA_I2C_CTLR_NAME,
		.of_match_table = ca_i2c_of_match_table,
	},
};
module_platform_driver(ca_i2c_driver);

module_param_named(debug, dbg_flag, uint, 0644);

MODULE_DESCRIPTION("Cortina Access I2C controller driver");
MODULE_LICENSE("GPL");
