// SPDX-License-Identifier: GPL-2.0
/*
 * Cortina Access Peripheral WDT driver
 *
 * Copyright (C) 2017 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 * Based on cadence_wdt.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>

#define CA_WDT_DEFAULT_TIMEOUT		30
#define CA_WDT_DEFAULT_HIGH_RESOLUTION	0

/* Supports 1 - 2^32 or 2^32/1000 sec */
#define CA_WDT_MIN_TIMEOUT	1

static int wdt_timeout = CA_WDT_DEFAULT_TIMEOUT;
static int nowayout = WATCHDOG_NOWAYOUT;
static int high_resolution = CA_WDT_DEFAULT_HIGH_RESOLUTION;

module_param(wdt_timeout, int, 0);
MODULE_PARM_DESC(wdt_timeout,
		 "Watchdog time in seconds. (default="
		 __MODULE_STRING(CA_WDT_DEFAULT_TIMEOUT) ")");

module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

module_param(high_resolution, int, 0);

/**
 * struct ca_wdt - Watchdog device structure
 * @regs: baseaddress of device
 * @rst: reset flag
 * @clk: struct clk * of a clock source
 * @prescaler: for saving prescaler value
 * @ctrl_clksel: counter clock prescaler selection
 * @io_lock: spinlock for IO register access
 * @ca_wdt_device: watchdog device structure
 * @ca_wdt_notifier: notifier structure
 *
 * Structure containing parameters specific to cadence watchdog.
 */
struct ca_wdt {
	void __iomem		*regs;
	void __iomem		*rst_reg;
	struct clk		*clk;
	u32			prescaler;
	u16			ps_div;
	u32			delay_reset;
	bool			rst;
	bool			ctrl_clksel;
	spinlock_t		io_lock;	/* the lock for io operations */
	struct watchdog_device	ca_wdt_device;
	struct notifier_block	ca_wdt_notifier;
};

/* Read access to Registers */
static inline u32 ca_wdt_readreg(struct ca_wdt *wdt, u32 offset)
{
	return readl(wdt->regs + offset);
}

/* Write access to Registers */
static inline void ca_wdt_writereg(struct ca_wdt *wdt, u32 offset, u32 val)
{
	writel_relaxed(val, wdt->regs + offset);
}

/*************************Register Map**************************************/

/* PER_WDT Registers */
#define	  CA_WDT_CTRL		  0x00		/* control */
#define   WDT_CTRL_WDTEN	  BIT(0)
#define   WDT_CTRL_RSTEN	  BIT(1)
#define   WDT_CTRL_CLKSEL	  BIT(2)
#define   WDT_CTRL_DELAY_SHFT	  12
#define   WDT_CTRL_MAX_DELAY	  0x000FFFFF
#define   CA_WDT_PS		  0x04		/* pre-scaler load value */
#define   CA_WDT_DIV		  0x08		/* divider for the pre-scaler */
#define   CA_WDT_LD		  0x0C		/* load value */
#define   CA_WDT_LOADE		  0x10		/* load enable */
#define   WDT_LOADE_WDT		  BIT(0)
#define   WDT_LOADE_PRE		  BIT(1)
#define   CA_WDT_CNT		  0x14		/* instantaneous value */
#define   CA_WDT_IE_0		  0x18		/* interrupt enable */
#define   WDT_IE_WDTE		  BIT(0)
#define   CA_WDT_INT_0		  0x1C		/* interrupt */
#define   WDT_INT_WDTI		  BIT(0)
#define	  CA_WDT_STAT_0		  0x20		/* interrupt status */
#define   WDT_STAT_WDTS		  BIT(0)

/* Global Config Register */
#define WD_RESET_SUBSYS_ENABLE	BIT(4)
#define WD_RESET_ALL_BLOCKS	BIT(6)
#define WD_RESET_REMAP		BIT(7)
#define WD_RESET_EXT_RESET	BIT(8)
#define EXT_RESET		BIT(9)

/**
 * ca_wdt_stop - Stop the watchdog.
 *
 * @wdd: watchdog device
 *
 * Read the contents of the ZMR register, clear the WDEN bit
 * in the register and set the access key for successful write.
 *
 * Return: always 0
 */
static int ca_wdt_stop(struct watchdog_device *wdd)
{
	struct ca_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;

	spin_lock(&wdt->io_lock);

	val = ca_wdt_readreg(wdt, CA_WDT_CTRL);
	val &= ~WDT_CTRL_WDTEN;
	ca_wdt_writereg(wdt, CA_WDT_CTRL, val);

	spin_unlock(&wdt->io_lock);

	return 0;
}

/**
 * ca_wdt_reload - Reload the watchdog timer (i.e. pat the watchdog).
 *
 * @wdd: watchdog device
 *
 * Write the restart key value (0x00001999) to the restart register.
 *
 * Return: always 0
 */
static int ca_wdt_reload(struct watchdog_device *wdd)
{
	struct ca_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;

	spin_lock(&wdt->io_lock);

	val = WDT_LOADE_WDT | WDT_LOADE_PRE;
	ca_wdt_writereg(wdt, CA_WDT_LOADE, val);

	spin_unlock(&wdt->io_lock);

	return 0;
}

/**
 * ca_wdt_start - Enable and start the watchdog.
 *
 * @wdd: watchdog device
 *
 * The counter value is calculated according to the formula:
 *		calculated count = (timeout * clock) / prescaler + 1.
 * The calculated count is divided by 0x1000 to obtain the field value
 * to write to counter control register.
 * Clears the contents of prescaler and counter reset value. Sets the
 * prescaler to 4096 and the calculated count and access key
 * to write to CCR Register.
 * Sets the WDT (WDEN bit) and either the Reset signal(RSTEN bit)
 * or Interrupt signal(IRQEN) with a specified cycles and the access
 * key to write to ZMR Register.
 *
 * Return: always 0
 */
static int ca_wdt_start(struct watchdog_device *wdd)
{
	struct ca_wdt *wdt = watchdog_get_drvdata(wdd);
	unsigned int data = 0;
	unsigned short count;
	unsigned int clock_rate, delay_cycle;

	count = high_resolution ? wdd->timeout * 1000 : wdd->timeout;

	spin_lock(&wdt->io_lock);

	ca_wdt_writereg(wdt, CA_WDT_PS, wdt->prescaler);
	ca_wdt_writereg(wdt, CA_WDT_DIV, wdt->ps_div);
	ca_wdt_writereg(wdt, CA_WDT_LD, count);

	ca_wdt_writereg(wdt, CA_WDT_IE_0, WDT_IE_WDTE);

	/* Reset on timeout if specified in device tree. */
	if (wdt->rst)
		data |= WDT_CTRL_RSTEN;
	if (wdt->ctrl_clksel)
		data |= WDT_CTRL_CLKSEL;
	data |= WDT_CTRL_WDTEN;
	if (wdt->delay_reset) {
		clock_rate = clk_get_rate(wdt->clk);
		delay_cycle = wdt->delay_reset * (clock_rate / 16000);
		if (delay_cycle >= WDT_CTRL_MAX_DELAY) {
			/* Set to maximum delay if overflow */
			delay_cycle = WDT_CTRL_MAX_DELAY;
		}
		data |= delay_cycle << WDT_CTRL_DELAY_SHFT;
	}
	ca_wdt_writereg(wdt, CA_WDT_CTRL, data);

	spin_unlock(&wdt->io_lock);

	return 0;
}

/**
 * ca_wdt_settimeout - Set a new timeout value for the watchdog device.
 *
 * @wdd: watchdog device
 * @new_time: new timeout value that needs to be set
 * Return: 0 on success
 *
 * Update the watchdog_device timeout with new value which is used when
 * ca_wdt_start is called.
 */
static int ca_wdt_settimeout(struct watchdog_device *wdd, unsigned int new_time)
{
	wdd->timeout = new_time;

	return ca_wdt_start(wdd);
}

/**
 * ca_wdt_settimeout - Set a new timeout value for the watchdog device.
 *
 * @wdd: watchdog device
 * @new_time: new timeout value that needs to be set
 * Return: 0 on success
 *
 * Update the watchdog_device timeout with new value which is used when
 * ca_wdt_start is called.
 */
static unsigned int ca_wdt_gettimeleft(struct watchdog_device *wdd)
{
	struct ca_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;

	spin_lock(&wdt->io_lock);

	val = ca_wdt_readreg(wdt, CA_WDT_CNT);

	spin_unlock(&wdt->io_lock);

	return high_resolution ? val / 1000 : val;
}

/**
 * ca_wdt_irq_handler - Notifies of watchdog timeout.
 *
 * @irq: interrupt number
 * @dev_id: pointer to a platform device structure
 * Return: IRQ_HANDLED
 *
 * The handler is invoked when the watchdog times out and a
 * reset on timeout has not been enabled.
 */
static irqreturn_t ca_wdt_irq_handler(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct ca_wdt *wdt = platform_get_drvdata(pdev);

	spin_lock(&wdt->io_lock);

	ca_wdt_writereg(wdt, CA_WDT_INT_0, WDT_INT_WDTI);
	ca_wdt_writereg(wdt, CA_WDT_IE_0, 0);

	spin_unlock(&wdt->io_lock);

	dev_info(&pdev->dev,
		 "Watchdog timed out. Internal reset not enabled\n");

	return IRQ_HANDLED;
}

/*
 * Info structure used to indicate the features supported by the device
 * to the upper layers. This is defined in watchdog.h header file.
 */
static const struct watchdog_info ca_wdt_info = {
	.identity	= "cdns_wdt watchdog",
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

/* Watchdog Core Ops */
static const struct watchdog_ops ca_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ca_wdt_start,
	.stop = ca_wdt_stop,
	.ping = ca_wdt_reload,
	.set_timeout = ca_wdt_settimeout,
	.get_timeleft =	ca_wdt_gettimeleft,
};

/**
 * ca_wdt_notify_sys - Notifier for reboot or shutdown.
 *
 * @this: handle to notifier block
 * @code: turn off indicator
 * @unused: unused
 * Return: NOTIFY_DONE
 *
 * This notifier is invoked whenever the system reboot or shutdown occur
 * because we need to disable the WDT before system goes down as WDT might
 * reset on the next boot.
 */
static int ca_wdt_notify_sys(struct notifier_block *this,
			     unsigned long code, void *unused)
{
	struct ca_wdt *wdt = container_of(this, struct ca_wdt, ca_wdt_notifier);

	if (code == SYS_DOWN || code == SYS_HALT)
		ca_wdt_stop(&wdt->ca_wdt_device);

	return NOTIFY_DONE;
}

/************************Platform Operations*****************************/
/**
 * ca_wdt_probe - Probe call for the device.
 *
 * @pdev: handle to the platform device structure.
 * Return: 0 on success, negative error otherwise.
 *
 * It does all the memory allocation and registration for the device.
 */
static int ca_wdt_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret, irq;
	unsigned long clock_f;
	struct ca_wdt *wdt;
	struct watchdog_device *ca_wdt_device;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	ca_wdt_device = &wdt->ca_wdt_device;
	ca_wdt_device->info = &ca_wdt_info;
	ca_wdt_device->ops = &ca_wdt_ops;
	ca_wdt_device->timeout = CA_WDT_DEFAULT_TIMEOUT;
	ca_wdt_device->min_timeout = CA_WDT_MIN_TIMEOUT;
	if (high_resolution)
		ca_wdt_device->max_timeout = U32_MAX / 1000;
	else
		ca_wdt_device->max_timeout = U32_MAX;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wdt->regs = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (IS_ERR(wdt->regs))
		return PTR_ERR(wdt->regs);

	/* Delay between the WDT interrupt firing and the actual reset */
	ret = of_property_read_u32(pdev->dev.of_node, "delay-reset",
				   &wdt->delay_reset);
	if (ret != 0)
		wdt->delay_reset = 0;

	/* Register the interrupt */
	wdt->rst = of_property_read_bool(pdev->dev.of_node, "reset-on-timeout");
	irq = platform_get_irq(pdev, 0);
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, ca_wdt_irq_handler,
				       0, pdev->name, pdev);
		if (ret) {
			dev_err(&pdev->dev,
				"cannot register interrupt handler err=%d\n",
				ret);
			return ret;
		}
	}
	if (wdt->rst) {
		u32 val;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		wdt->rst_reg = devm_ioremap(&pdev->dev, res->start,
					    resource_size(res));
		if (IS_ERR(wdt->rst_reg))
			return PTR_ERR(wdt->rst_reg);

		val = readl(wdt->rst_reg);
		val |= WD_RESET_SUBSYS_ENABLE | WD_RESET_ALL_BLOCKS |
		       WD_RESET_REMAP | WD_RESET_EXT_RESET;
		writel_relaxed(val, wdt->rst_reg);
	}

	/* Initialize the members of ca_wdt structure */
	ca_wdt_device->parent = &pdev->dev;

	ret = watchdog_init_timeout(ca_wdt_device, wdt_timeout,
				    &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "unable to set timeout value\n");
		return ret;
	}

	watchdog_set_nowayout(ca_wdt_device, nowayout);
	watchdog_set_drvdata(ca_wdt_device, wdt);

	wdt->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(wdt->clk)) {
		dev_err(&pdev->dev, "input clock not found\n");
		ret = PTR_ERR(wdt->clk);
		return ret;
	}

	ret = clk_prepare_enable(wdt->clk);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable clock\n");
		return ret;
	}

	clock_f = clk_get_rate(wdt->clk);
	wdt->prescaler = clock_f / 1000 - 1; /* 1ms */
	if (high_resolution) {
		wdt->ps_div = 0;
		wdt->ctrl_clksel = 1;
	} else {
		wdt->ps_div = 1000; /* 1s */
		wdt->ctrl_clksel = 0;
	}

	spin_lock_init(&wdt->io_lock);

	wdt->ca_wdt_notifier.notifier_call = &ca_wdt_notify_sys;
	ret = register_reboot_notifier(&wdt->ca_wdt_notifier);
	if (ret != 0) {
		dev_err(&pdev->dev, "cannot register reboot notifier err=%d)\n",
			ret);
		goto err_clk_disable;
	}

	ret = watchdog_register_device(ca_wdt_device);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register wdt device\n");
		goto err_clk_disable;
	}
	platform_set_drvdata(pdev, wdt);

	dev_dbg(&pdev->dev, "Cortina-Access Watchdog Timer at %p with timeout %ds%s\n",
		wdt->regs, ca_wdt_device->timeout,
		nowayout ? ", nowayout" : "");

	return 0;

err_clk_disable:
	clk_disable_unprepare(wdt->clk);

	return ret;
}

/**
 * ca_wdt_remove - Probe call for the device.
 *
 * @pdev: handle to the platform device structure.
 * Return: 0 on success, otherwise negative error.
 *
 * Unregister the device after releasing the resources.
 */
static int ca_wdt_remove(struct platform_device *pdev)
{
	struct ca_wdt *wdt = platform_get_drvdata(pdev);

	ca_wdt_stop(&wdt->ca_wdt_device);
	watchdog_unregister_device(&wdt->ca_wdt_device);
	unregister_reboot_notifier(&wdt->ca_wdt_notifier);
	clk_disable_unprepare(wdt->clk);

	return 0;
}

/**
 * ca_wdt_shutdown - Stop the device.
 *
 * @pdev: handle to the platform structure.
 *
 */
static void ca_wdt_shutdown(struct platform_device *pdev)
{
	struct ca_wdt *wdt = platform_get_drvdata(pdev);

	ca_wdt_stop(&wdt->ca_wdt_device);
	clk_disable_unprepare(wdt->clk);
}

/**
 * ca_wdt_suspend - Stop the device.
 *
 * @dev: handle to the device structure.
 * Return: 0 always.
 */
static int __maybe_unused ca_wdt_suspend(struct device *dev)
{
	struct platform_device *pdev = container_of(dev,
			struct platform_device, dev);
	struct ca_wdt *wdt = platform_get_drvdata(pdev);

	ca_wdt_stop(&wdt->ca_wdt_device);
	clk_disable_unprepare(wdt->clk);

	return 0;
}

/**
 * ca_wdt_resume - Resume the device.
 *
 * @dev: handle to the device structure.
 * Return: 0 on success, errno otherwise.
 */
static int __maybe_unused ca_wdt_resume(struct device *dev)
{
	int ret;
	struct platform_device *pdev = container_of(dev,
			struct platform_device, dev);
	struct ca_wdt *wdt = platform_get_drvdata(pdev);

	ret = clk_prepare_enable(wdt->clk);
	if (ret) {
		dev_err(dev, "unable to enable clock\n");
		return ret;
	}
	ca_wdt_start(&wdt->ca_wdt_device);

	return 0;
}

static SIMPLE_DEV_PM_OPS(ca_wdt_pm_ops, ca_wdt_suspend, ca_wdt_resume);

static const struct of_device_id ca_wdt_of_match[] = {
	{ .compatible = "cortina-access,wdt", },
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, ca_wdt_of_match);

/* Driver Structure */
static struct platform_driver ca_wdt_driver = {
	.probe		= ca_wdt_probe,
	.remove		= ca_wdt_remove,
	.shutdown	= ca_wdt_shutdown,
	.driver		= {
		.name	= "ca-wdt",
		.of_match_table = ca_wdt_of_match,
		.pm	= &ca_wdt_pm_ops,
	},
};

module_platform_driver(ca_wdt_driver);

MODULE_DESCRIPTION("Watchdog driver for Cortina-Access Peripheral WDT");
MODULE_LICENSE("GPL");
