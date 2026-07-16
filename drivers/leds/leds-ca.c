// SPDX-License-Identifier: GPL-2.0

/*
 * Driver for CA77XX memory-mapped serial LEDs
 *
 * Copyright (C) 2017 Cortina Access, Inc.
 *		http://www.cortina-access.com
 *
 * based on leds-bcm6328.c
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#ifdef CONFIG_LEDS_TRIGGER_CA_PHY
#include <linux/ledtrig-ca-phy.h>
#endif

#define CA_LED_MAX_HW_BLINK		127
#if defined(CONFIG_ARCH_CA_MERCURY)
#define CA_LED_MAX_COUNT		20
#define CA_LED_MAX_PORT		10
#else
#define CA_LED_MAX_COUNT		16
#define CA_LED_MAX_PORT		8
#endif

/* LED_CONTROL fields */
#define CA_LED_BLINK_RATE1_OFFSET	0
#define CA_LED_BLINK_RATE1_MASK	0xFF
#define CA_LED_BLINK_RATE2_OFFSET	8
#define CA_LED_BLINK_RATE2_MASK	0xFF
#define CA_LED_CLK_POLARITY		BIT(17)
#define CA_LED_CLK_TEST_MODE	BIT(16)
#define CA_LED_CLK_TEST_RX_TEST	BIT(30)
#define CA_LED_CLK_TEST_TX_TEST	BIT(31)

/* LED_CONFIG fields */
#define CA_LED_EVENT_ON_OFFSET	0
#define CA_LED_EVENT_ON_MASK	0x7
#define CA_LED_EVENT_BLINK_OFFSET	3
#define CA_LED_EVENT_BLINK_MASK	0x7
#define CA_LED_EVENT_OFF_OFFSET	6
#define CA_LED_EVENT_OFF_MASK	0x7
#define CA_LED_OFF_ON_OFFSET	9
#define CA_LED_OFF_ON_MASK		0x3
#define CA_LED_PORT_OFFSET		11
#if defined(CONFIG_ARCH_CA_MERCURY)
#define CA_LED_PORT_MASK		0xf
#define CA_LED_OFF_VAL		BIT(15)
#define CA_LED_SW_EVENT		BIT(16)
#define CA_LED_BLINK_SEL		BIT(17)
#define CA_LED_BLINK_OR		BIT(18)
#else
#define CA_LED_PORT_MASK		0x7
#define CA_LED_OFF_VAL		BIT(14)
#define CA_LED_SW_EVENT		BIT(15)
#define CA_LED_BLINK_SEL		BIT(16)
#define CA_LED_BLINK_OR		BIT(17)
#endif

#define TRIGGER_HW_RX			0
#define TRIGGER_HW_TX			1
#define TRIGGER_SW			2
#define TRIGGER_NONE			3

#define LED_TRIGGER_HW_RX			BIT(TRIGGER_HW_RX)
#define LED_TRIGGER_HW_TX			BIT(TRIGGER_HW_TX)
#define LED_TRIGGER_SW				BIT(TRIGGER_SW)
#define LED_TRIGGER_NONE			0

#define BLINK_RATE1			0
#define BLINK_RATE2			1

/**
 * struct ca_led_cfg - configuration for LEDs
 * @cdev: LED class device for this LED
 * @mem: memory resource
 * @lock: memory lock
 * @idx: LED index number
 * @active_low: LED is active low
 * @off_event: off triggered by rx/tx/sw event
 * @blink_event: blinking triggered by rx/tx/sw event
 * @on_event: on triggered by rx/tx/sw event
 * @port: monitor port
 * @blink: haredware blink rate select
 * @enable: LED is enabled/disabled
 */
struct ca_led_cfg {
	struct led_classdev cdev;
	void __iomem *mem;
	spinlock_t *lock;	/* protect LED resource access */
	int idx;
	bool active_low;
#ifdef CONFIG_LEDS_TRIGGER_CA_PHY
	bool phy_trig;
#endif
	int off_event;
	int blink_event;
	int on_event;
	int port;
	int blink;
	int enable;
	struct device *dev;
	bool blink_or;
};

/**
 * struct ca_led_cfg - control for LEDs
 * @mem: memory resource
 * @lock: memory lock
 * @test_mode: enter test mode
 * @clk_low: clock polarity
 * @blink_rate1: haredware blink rate 1
 * @blink_rate2: haredware blink rate 2
 * @CORTINA_LED: configuration for LEDs
 */
struct ca_led_ctrl {
	void __iomem *mem;
	spinlock_t *lock;	/* protect LED resource access */

	int test_mode;
	int clk_low;
	u16 blink_rate1;
	u16 blink_rate2;

	struct device *dev;

	struct ca_led_cfg *led_cfg[CA_LED_MAX_COUNT];
};

static char *trigger_str[] = {
	[TRIGGER_HW_RX] = "rx",
	[TRIGGER_HW_TX] = "tx",
	[TRIGGER_SW] = "sw",
	[TRIGGER_NONE] = "none",
};

static char *rate_str[] = {
	[BLINK_RATE1] = "blink_rate1",
	[BLINK_RATE2] = "blink_rate2",
};

static struct ca_led_ctrl *glb_led_ctrl;

static void ca_led_write(void __iomem *reg, unsigned long data)
{
	iowrite32(data, reg);
}

static unsigned long ca_led_read(void __iomem *reg)
{
	return ioread32(reg);
}

#if (!(defined(CONFIG_LEDS_CA_PHY_2DIR) && \
	(defined(CONFIG_ARCH_CA_VENUS) || \
	defined(CONFIG_ARCH_CA_MERCURY)))) && \
	defined(CONFIG_LEDS_TRIGGER_CA_PHY)
static int ca_led_sw_on(int idx, int on)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (idx >= CA_LED_MAX_COUNT)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];

	spin_lock_irqsave(led_cfg->lock, flags);
	val = ca_led_read(led_cfg->mem);
	if (on)
		val |= CA_LED_SW_EVENT;
	else
		val &= ~CA_LED_SW_EVENT;

	ca_led_write(led_cfg->mem, val);
	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}
#endif

#if (defined(CONFIG_LEDS_CA_PHY_2DIR) || \
	defined(CONFIG_LEDS_CA_PHY_2CTRL)) && \
	(defined(CONFIG_ARCH_CA_VENUS) || \
	defined(CONFIG_ARCH_CA_MERCURY))
static int ca_led_sw_on_wr(int idx, int on)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (idx >= CA_LED_MAX_COUNT)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];

	spin_lock_irqsave(led_cfg->lock, flags);
	val = ca_led_read(led_cfg->mem);
	if (on) {
		val &= ~(CA_LED_OFF_ON_MASK << CA_LED_OFF_ON_OFFSET);
	} else {
		val &= ~(CA_LED_OFF_ON_MASK << CA_LED_OFF_ON_OFFSET);
		val |= 0x1 << CA_LED_OFF_ON_OFFSET;
	}

	ca_led_write(led_cfg->mem, val);
	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}

/**
 * ca_led_config_wr - venus workaround
 */
static int ca_led_config_wr(int idx, int active_low, uint off_event,
			    uint blink_event, uint on_event, int port,
			    int blink, int blink_or)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (idx >= CA_LED_MAX_COUNT)
		return -EINVAL;
	if (port >= CA_LED_MAX_PORT)
		return -EINVAL;
	if (blink > BLINK_RATE2)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];

	spin_lock_irqsave(led_cfg->lock, flags);

	val = ca_led_read(led_cfg->mem);

	if (active_low)
		val |= CA_LED_OFF_VAL;
	else
		val &= ~CA_LED_OFF_VAL;

	if (blink_or) {
		led_cfg->blink_or = true;
		val |= CA_LED_BLINK_OR;
	} else {
		led_cfg->blink_or = false;
		val &= ~CA_LED_BLINK_OR;
	}

	led_cfg->off_event = (int)off_event;
	led_cfg->blink_event = (int)blink_event;
	led_cfg->on_event = (int)on_event;
	led_cfg->blink = blink;

	if (blink == BLINK_RATE1)
		val &= ~CA_LED_BLINK_SEL;
	else if (blink == BLINK_RATE2)
		val |= CA_LED_BLINK_SEL;

	val &= ~(CA_LED_EVENT_OFF_MASK << CA_LED_EVENT_OFF_OFFSET);
	//if (off_event != TRIGGER_NONE)
	val |= off_event << CA_LED_EVENT_OFF_OFFSET;

	val &= ~(CA_LED_EVENT_BLINK_MASK << CA_LED_EVENT_BLINK_OFFSET);
	val |= blink_event << CA_LED_EVENT_BLINK_OFFSET;

	val &= ~(CA_LED_EVENT_ON_MASK << CA_LED_EVENT_ON_OFFSET);
	val |= on_event << CA_LED_EVENT_ON_OFFSET;

	led_cfg->port = port;
	val &= ~(CA_LED_PORT_MASK << CA_LED_PORT_OFFSET);
	val |= port << CA_LED_PORT_OFFSET;

	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}
#endif

/**
 * ca_led_enable - switch LED to off or triggered by event
 */
static int ca_led_enable(int idx, int enable)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (idx >= CA_LED_MAX_COUNT)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];

	spin_lock_irqsave(led_cfg->lock, flags);

	val = ca_led_read(led_cfg->mem);

	if (enable) {
		led_cfg->enable = 1;

		/* driven by event */
		val &= ~(CA_LED_OFF_ON_MASK << CA_LED_OFF_ON_OFFSET);
	} else {
		led_cfg->enable = 0;

		/* force off */
		val &= ~(CA_LED_OFF_ON_MASK << CA_LED_OFF_ON_OFFSET);
		val |= 0x3 << CA_LED_OFF_ON_OFFSET;
	}

	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}

/**
 * ca_led_config - config LED trigger behavior
 */
static int ca_led_config(int idx, int active_low, int off_event,
			 int blink_event, int on_event, int port,
			 int blink, int blink_or)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	u32 val;

	if (idx >= CA_LED_MAX_COUNT)
		return -EINVAL;
	if (off_event > TRIGGER_NONE)
		return -EINVAL;
	if (blink_event > TRIGGER_NONE)
		return -EINVAL;
	if (on_event > TRIGGER_NONE)
		return -EINVAL;
	if (port >= CA_LED_MAX_PORT)
		return -EINVAL;
	if (blink > BLINK_RATE2)
		return -EINVAL;

	led_cfg = glb_led_ctrl->led_cfg[idx];

	spin_lock_irqsave(led_cfg->lock, flags);

	val = ca_led_read(led_cfg->mem);

	if (active_low) {
		led_cfg->active_low = true;
		val |= CA_LED_OFF_VAL;
	} else {
		led_cfg->active_low = false;
		val &= ~CA_LED_OFF_VAL;
	}

	if (blink_or) {
		led_cfg->blink_or = true;
		val |= CA_LED_BLINK_OR;
	} else {
		led_cfg->blink_or = false;
		val &= ~CA_LED_BLINK_OR;
	}

	led_cfg->off_event = off_event;
	led_cfg->blink_event = blink_event;
	led_cfg->on_event = on_event;
	led_cfg->blink = blink;

	if (blink == BLINK_RATE1)
		val &= ~CA_LED_BLINK_SEL;
	else if (blink == BLINK_RATE2)
		val |= CA_LED_BLINK_SEL;

	val &= ~(CA_LED_EVENT_OFF_MASK << CA_LED_EVENT_OFF_OFFSET);
	if (off_event != TRIGGER_NONE)
		val |= BIT(off_event) << CA_LED_EVENT_OFF_OFFSET;

	val &= ~(CA_LED_EVENT_BLINK_MASK << CA_LED_EVENT_BLINK_OFFSET);
	if (blink_event != TRIGGER_NONE)
		val |= BIT(blink_event) << CA_LED_EVENT_BLINK_OFFSET;

	val &= ~(CA_LED_EVENT_ON_MASK << CA_LED_EVENT_ON_OFFSET);
	if (on_event != TRIGGER_NONE)
		val |= BIT(on_event) << CA_LED_EVENT_ON_OFFSET;

	led_cfg->port = port;
	val &= ~(CA_LED_PORT_MASK << CA_LED_PORT_OFFSET);
	val |= port << CA_LED_PORT_OFFSET;

	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}

/**
 * ca_led_test_mode - switch all LEDs into/from test mode
 */
int ca_led_test_mode(int enable)
{
	struct ca_led_cfg *led_cfg;
	struct led_classdev *led_cdev;
	unsigned long flags;
	u32 val;
	int i;

	if (enable) {
		spin_lock_irqsave(glb_led_ctrl->lock, flags);

		glb_led_ctrl->test_mode = 1;

		/* hardware generate tx/rx event automatically */
		for (i = 0; i < CA_LED_MAX_COUNT; i++) {
			if (!glb_led_ctrl->led_cfg[i])
				continue;

			led_cfg = glb_led_ctrl->led_cfg[i];
			led_cdev = &led_cfg->cdev;

			/* change to hw rx trigger with blink */
			mutex_lock(&led_cdev->led_access);
			led_sysfs_disable(led_cdev);
			mutex_unlock(&led_cdev->led_access);

			val = ca_led_read(led_cfg->mem);
			val &= ~(CA_LED_EVENT_ON_MASK <<
				 CA_LED_EVENT_ON_OFFSET);
			val &= ~(CA_LED_EVENT_BLINK_MASK <<
				 CA_LED_EVENT_BLINK_OFFSET);
			val |= BIT(0) << CA_LED_EVENT_BLINK_OFFSET;
			val |= CA_LED_BLINK_SEL;
			ca_led_write(led_cfg->mem, val);
		}

		val = ca_led_read(glb_led_ctrl->mem);
		val |= CA_LED_CLK_TEST_MODE |
		       CA_LED_CLK_TEST_RX_TEST |
		       CA_LED_CLK_TEST_TX_TEST;
		ca_led_write(glb_led_ctrl->mem, val);

		spin_unlock_irqrestore(glb_led_ctrl->lock, flags);
	} else {
		spin_lock_irqsave(glb_led_ctrl->lock, flags);

		glb_led_ctrl->test_mode = 0;

		val = ca_led_read(glb_led_ctrl->mem);
		val &= ~(CA_LED_CLK_TEST_MODE |
			 CA_LED_CLK_TEST_RX_TEST |
			 CA_LED_CLK_TEST_TX_TEST);
		ca_led_write(glb_led_ctrl->mem, val);

		spin_unlock_irqrestore(glb_led_ctrl->lock, flags);

		for (i = 0; i < CA_LED_MAX_COUNT; i++) {
			led_cfg = glb_led_ctrl->led_cfg[i];

			ca_led_config(i, led_cfg->active_low,
				      led_cfg->off_event,
				      led_cfg->blink_event,
				      led_cfg->on_event, led_cfg->port,
				      led_cfg->blink, led_cfg->blink_or);
		}
	}

	return 0;
}
EXPORT_SYMBOL(ca_led_test_mode);

/**
 * ca_led_set - set on/off software event
 */
static void ca_led_set(struct led_classdev *led_cdev,
		       enum led_brightness value)
{
	struct ca_led_cfg *led_cfg =
		container_of(led_cdev, struct ca_led_cfg, cdev);

	unsigned long flags;
	u32 val;

#ifdef CONFIG_LEDS_TRIGGER_CA_PHY

	if (led_cfg->phy_trig) {
		if (value & LED_HW_ENABLE) {
			u32 blink_event = TRIGGER_NONE;
			int blink_rate = BLINK_RATE1;
			int active_low;

			if (value & LED_HW_BLINK_TX)
				blink_event = TRIGGER_HW_TX;
			if (value & LED_HW_BLINK_RX)
				blink_event = TRIGGER_HW_RX;

			if (!(value & LED_HW_BLINK_HIGH_RATE))
				blink_rate = BLINK_RATE2;

			val = (value & LED_SW_ON) ? 1 : 0;
			active_low = (led_cfg->active_low) ? 1 : 0;
	#if defined(CONFIG_LEDS_CA_PHY_2DIR) && \
	    (defined(CONFIG_ARCH_CA_VENUS) || \
		defined(CONFIG_ARCH_CA_MERCURY))
			blink_event = (LED_TRIGGER_HW_TX | LED_TRIGGER_HW_RX |
				       LED_TRIGGER_SW);
			active_low = (led_cfg->active_low) ? 0 : 1;
			ca_led_config_wr(led_cfg->idx, active_low,
					 LED_TRIGGER_NONE, blink_event,
					 LED_TRIGGER_NONE, led_cfg->port,
					 blink_rate, 1);
			ca_led_sw_on_wr(led_cfg->idx, val);
	#elif defined(CONFIG_LEDS_CA_PHY_2CTRL) && \
	      (defined(CONFIG_ARCH_CA_VENUS) || \
		  defined(CONFIG_ARCH_CA_MERCURY))
			if (blink_event == TRIGGER_NONE) {
				ca_led_config(led_cfg->idx, active_low,
					      TRIGGER_NONE, blink_event,
					      TRIGGER_SW, led_cfg->port,
					      blink_rate, 0);
				ca_led_sw_on(led_cfg->idx, val);
				ca_led_enable(led_cfg->idx, 1);

			} else {
				blink_event = (LED_TRIGGER_HW_TX |
					       LED_TRIGGER_HW_RX | LED_TRIGGER_SW);
				active_low = (led_cfg->active_low) ? 0 : 1;
				ca_led_config_wr(led_cfg->idx, active_low,
						 LED_TRIGGER_NONE, blink_event,
						 LED_TRIGGER_NONE, led_cfg->port,
						 blink_rate, 1);
				ca_led_sw_on_wr(led_cfg->idx, val);
			}
	#else
			ca_led_config(led_cfg->idx, led_cfg->active_low,
				      TRIGGER_NONE, blink_event,
				      TRIGGER_SW,
				      led_cfg->port, blink_rate, 0);
			ca_led_sw_on(led_cfg->idx, val);
			ca_led_enable(led_cfg->idx, 1);
	#endif

		} else {
	#if (defined(CONFIG_LEDS_CA_PHY_2DIR) || \
		defined(CONFIG_LEDS_CA_PHY_2CTRL)) && \
		(defined(CONFIG_ARCH_CA_VENUS) || \
		defined(CONFIG_ARCH_CA_MERCURY))
			ca_led_config_wr(led_cfg->idx, led_cfg->active_low,
					 LED_TRIGGER_NONE,
					 LED_TRIGGER_NONE,
					 LED_TRIGGER_NONE, led_cfg->port,
					 0, 0);
	#endif
			ca_led_enable(led_cfg->idx, 0);
		}

		return;
	}
#endif

	spin_lock_irqsave(led_cfg->lock, flags);
	val = ca_led_read(led_cfg->mem);
	if (value == LED_OFF)
		val &= ~CA_LED_SW_EVENT;
	else
		val |= CA_LED_SW_EVENT;
	ca_led_write(led_cfg->mem, val);
	spin_unlock_irqrestore(led_cfg->lock, flags);
}

static int ca_led_blink_set(struct led_classdev *led_cdev,
			    unsigned long *delay_on, unsigned long *delay_off)
{
	struct ca_led_cfg *led_cfg =
		container_of(led_cdev, struct ca_led_cfg, cdev);
	struct ca_led_ctrl *glb_led;
	unsigned long delay, flags;
	u32 val;

	/* limit sw not to use hardware blink */
	if (led_cfg->blink_event == TRIGGER_SW)
		return -EINVAL;

	glb_led = dev_get_drvdata(led_cfg->dev);

	if (*delay_on != *delay_off) {
		dev_dbg(led_cdev->dev,
			"fallback to soft blinking (delay_on != delay_off)\n");
		return -EINVAL;
	}

	if (*delay_on == ((glb_led->blink_rate1 + 1) * 16)) {
		delay = 0;
	} else if (*delay_on == ((glb_led->blink_rate2 + 1) * 16)) {
		delay = 1;
	} else {
		dev_dbg(led_cdev->dev, "fallback to soft blinking\n");
		return -EINVAL;
	}

	spin_lock_irqsave(led_cfg->lock, flags);
	val = ca_led_read(led_cfg->mem);
	if (delay)
		val |= CA_LED_BLINK_SEL;
	else
		val &= ~CA_LED_BLINK_SEL;
	ca_led_write(led_cfg->mem, val);
	spin_unlock_irqrestore(led_cfg->lock, flags);

	return 0;
}

/**
 * ca_led_show_hw_event - show hardware blink event
 */
static ssize_t blink_event_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(led_cfg->lock, flags);

	for (i = 0; i <= TRIGGER_NONE; i++) {
		if (i == led_cfg->blink_event)
			len += sprintf(buf + len, "[%s] ", trigger_str[i]);
		else
			len += sprintf(buf + len, "%s ", trigger_str[i]);
	}

	spin_unlock_irqrestore(led_cfg->lock, flags);

	len += sprintf(buf + len, "\n");
	return len;
}

/**
 * ca_led_store_hw_event - set hardware blink event
 */
static ssize_t blink_event_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	u32 val;
	int i;

	for (i = 0; i <= TRIGGER_NONE; i++) {
		if (!strncmp(buf, trigger_str[i], strlen(trigger_str[i])))
			break;
	}
	if (i > TRIGGER_NONE)
		return size;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->blink_event = i;

	val = ca_led_read(led_cfg->mem);

	val &= ~(CA_LED_EVENT_BLINK_MASK << CA_LED_EVENT_BLINK_OFFSET);
	if (led_cfg->blink_event == TRIGGER_HW_RX ||
	    led_cfg->blink_event == TRIGGER_HW_TX) {
		val |= BIT(led_cfg->blink_event) <<
			       CA_LED_EVENT_BLINK_OFFSET;
	} else if (led_cfg->blink_event == TRIGGER_SW) {
		val |= BIT(led_cfg->blink_event) <<
			CA_LED_EVENT_BLINK_OFFSET;
	} else if (led_cfg->blink_event == TRIGGER_NONE) {
		val &= ~(CA_LED_EVENT_BLINK_MASK
				<< CA_LED_EVENT_BLINK_OFFSET);
	}

	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	mutex_lock(&led_cdev->led_access);
	if (led_cfg->blink_event == TRIGGER_SW)
		led_sysfs_enable(&led_cfg->cdev);
	//else
	//	led_sysfs_disable(&led_cfg->cdev);
	mutex_unlock(&led_cdev->led_access);
	return size;
}

static DEVICE_ATTR_RW(blink_event);

static ssize_t hw_port_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->port);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}

static ssize_t hw_port_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags, param;

	u32 val;

	if (kstrtoul(buf, 10, &param))
		return 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->port = param & CA_LED_PORT_MASK;

	val = ca_led_read(led_cfg->mem);
	val &= ~(CA_LED_PORT_MASK << CA_LED_PORT_OFFSET);
	val |= led_cfg->port << CA_LED_PORT_OFFSET;
	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return size;
}

static DEVICE_ATTR_RW(hw_port);

static ssize_t hw_blink_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(led_cfg->lock, flags);

	for (i = 0; i <= BLINK_RATE2; i++) {
		if (i == led_cfg->blink)
			len += sprintf(buf + len, "[%s] ", rate_str[i]);
		else
			len += sprintf(buf + len, "%s ", rate_str[i]);
	}

	spin_unlock_irqrestore(led_cfg->lock, flags);

	len += sprintf(buf + len, "\n");
	return len;
}

static ssize_t hw_blink_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	u32 val;
	int i;

	for (i = 0; i <= BLINK_RATE2; i++) {
		if (!strncmp(buf, rate_str[i], strlen(rate_str[i])))
			break;
	}
	if (i > BLINK_RATE2)
		return size;

	spin_lock_irqsave(led_cfg->lock, flags);

	led_cfg->blink = i;

	val = ca_led_read(led_cfg->mem);
	if (led_cfg->blink == BLINK_RATE1)
		val &= ~CA_LED_BLINK_SEL;
	else if (led_cfg->blink == BLINK_RATE2)
		val |= CA_LED_BLINK_SEL;
	ca_led_write(led_cfg->mem, val);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return size;
}

static DEVICE_ATTR_RW(hw_blink);

static ssize_t hw_enable_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->enable);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}

static ssize_t hw_enable_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long param;

	if (kstrtoul(buf, 10, &param))
		return 0;

	ca_led_enable(led_cfg->idx, param);

	return size;
}

static DEVICE_ATTR_RW(hw_enable);

static ssize_t hw_id_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct ca_led_cfg *led_cfg =
			container_of(led_cdev, struct ca_led_cfg, cdev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_cfg->lock, flags);

	ret += sprintf(buf, "%d\n", led_cfg->idx);

	spin_unlock_irqrestore(led_cfg->lock, flags);

	return ret;
}

static ssize_t hw_id_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t size)
{
	return size;
}

static DEVICE_ATTR_RW(hw_id);

static struct attribute *CA_LED_attrs[] = {
	&dev_attr_hw_id.attr,
	&dev_attr_blink_event.attr,
	&dev_attr_hw_port.attr,
	&dev_attr_hw_blink.attr,
	&dev_attr_hw_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(CA_LED);

static int ca_led_probe(struct ca_led_ctrl *led_ctrl,
			struct device_node *nc, u32 reg, void __iomem *mem,
			spinlock_t *lock)
{
	struct ca_led_cfg *led_cfg;
	unsigned long flags;
	const char *state;
	u32 val;
	int rc, active_low, off_event, blink_event, on_event, port, blink, blink_or;

	led_cfg = devm_kzalloc(led_ctrl->dev, sizeof(*led_cfg), GFP_KERNEL);
	if (!led_cfg)
		return -ENOMEM;
	led_ctrl->led_cfg[reg] = led_cfg;

	led_cfg->idx = reg;
	led_cfg->mem = mem;
	led_cfg->lock = lock;
	led_cfg->dev = led_ctrl->dev;

	led_cfg->cdev.name = of_get_property(nc, "label", NULL) ? : nc->name;
	led_cfg->cdev.default_trigger = of_get_property(nc,
							"linux,default-trigger",
							NULL);

	if (of_property_read_bool(nc, "active-low"))
		active_low = 1;
	else
		active_low = 0;

	if (of_property_read_bool(nc, "blink-or"))
		blink_or = 1;
	else
		blink_or = 0;

	/* priority : off > blink > on */
	if (of_property_read_bool(nc, "off,hw-rx"))
		off_event = TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "off,hw-tx"))
		off_event = TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "off,sw"))
		off_event = TRIGGER_SW;
	else
		off_event = TRIGGER_NONE;

	if (of_property_read_bool(nc, "blinking,hw-rx"))
		blink_event = TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "blinking,hw-tx"))
		blink_event = TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "blinking,sw"))
		blink_event = TRIGGER_SW;
	else
		blink_event = TRIGGER_NONE;

	if (of_property_read_bool(nc, "on,hw-rx"))
		on_event = TRIGGER_HW_RX;
	else if (of_property_read_bool(nc, "on,hw-tx"))
		on_event = TRIGGER_HW_TX;
	else if (of_property_read_bool(nc, "on,sw"))
		on_event = TRIGGER_SW;
	else
		on_event = TRIGGER_NONE;

	if (!of_property_read_u32(nc, "port", &val))
		port = val;
	else
		port = 0;

	if (of_property_read_bool(nc, "blink-rate1"))
		blink = BLINK_RATE1;
	else
		blink = BLINK_RATE2;

	ca_led_config(led_cfg->idx, active_low, off_event, blink_event,
		      on_event, port, blink, blink_or);

	spin_lock_irqsave(lock, flags);

	if (!of_property_read_string(nc, "default-state", &state)) {
		if (!strcmp(state, "disabled")) {
			led_cfg->enable = 0;
			led_cfg->cdev.brightness = LED_OFF;
		} else {
			led_cfg->enable = 1;

			if (!strcmp(state, "on"))
				led_cfg->cdev.brightness = LED_FULL;
		}
	} else {
		led_cfg->enable = 1;
		led_cfg->cdev.brightness = LED_OFF;
	}

#ifdef CONFIG_LEDS_TRIGGER_CA_PHY
	if (led_cfg->cdev.default_trigger) {
#if defined(CONFIG_LEDS_CA_PHY_2DIR)
		char str[16];

		sprintf(str, "ca_phy%d", led_cfg->port);

		if (!strcmp(str, led_cfg->cdev.default_trigger))
			led_cfg->phy_trig = true;
#elif defined(CONFIG_LEDS_CA_PHY_2CTRL)
		char str[16], link_str[16];

		sprintf(link_str, "ca_phy%d_link", led_cfg->port);

		sprintf(str, "ca_phy%d", led_cfg->port);

		if ((!strcmp(link_str, led_cfg->cdev.default_trigger)) ||
		    (!strcmp(str, led_cfg->cdev.default_trigger)))
			led_cfg->phy_trig = true;

#elif defined(CONFIG_LEDS_CA_PHY_3CTRL)
		char link_str[16], rx_str[16], tx_str[16];

		sprintf(link_str, "ca_phy%d_link", led_cfg->port);
		sprintf(rx_str, "ca_phy%d_rx", led_cfg->port);
		sprintf(tx_str, "ca_phy%d_tx", led_cfg->port);

		if ((!strcmp(link_str, led_cfg->cdev.default_trigger)) ||
		    (!strcmp(rx_str, led_cfg->cdev.default_trigger)) ||
		    (!strcmp(tx_str, led_cfg->cdev.default_trigger)))
			led_cfg->phy_trig = true;
#else
		char rx_str[16], tx_str[16];

		sprintf(rx_str, "ca_phy%d_rx", led_cfg->port);
		sprintf(tx_str, "ca_phy%d_tx", led_cfg->port);

		if ((!strcmp(rx_str, led_cfg->cdev.default_trigger)) ||
		    (!strcmp(tx_str, led_cfg->cdev.default_trigger)))
			led_cfg->phy_trig = true;
#endif
	}
#endif
	spin_unlock_irqrestore(lock, flags);

	if (led_cfg->enable)
		ca_led_set(&led_cfg->cdev, led_cfg->cdev.brightness);
	ca_led_enable(led_cfg->idx, led_cfg->enable);

	led_cfg->cdev.brightness_set = ca_led_set;
	led_cfg->cdev.blink_set = ca_led_blink_set;
	led_cfg->cdev.groups = CA_LED_groups;

	rc = devm_led_classdev_register(led_ctrl->dev, &led_cfg->cdev);
	if (rc < 0)
		return rc;

	dev_set_drvdata(led_cfg->cdev.dev, led_cfg);

	dev_dbg(led_ctrl->dev, "registered LED %s\n", led_cfg->cdev.name);

	if (led_cfg->on_event == TRIGGER_SW) {
		mutex_lock(&led_cfg->cdev.led_access);
		led_sysfs_enable(&led_cfg->cdev);
		mutex_unlock(&led_cfg->cdev.led_access);
	} else {
		mutex_lock(&led_cfg->cdev.led_access);
		led_sysfs_disable(&led_cfg->cdev);
		mutex_unlock(&led_cfg->cdev.led_access);
#ifdef CONFIG_LEDS_TRIGGER_CA_PHY
		if (!led_cfg->phy_trig)
			led_cfg->cdev.default_trigger = "none";
#else
		led_cfg->cdev.default_trigger = "none";
#endif
	}

	return 0;
}

static ssize_t test_mode_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", led_ctrl->test_mode);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t test_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned long param;

	if (kstrtoul(buf, 10, &param))
		return 0;

	ca_led_test_mode(param);

	return count;
}
static DEVICE_ATTR_RW(test_mode);

static ssize_t clk_low_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", led_ctrl->clk_low);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t clk_low_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return 0;

	spin_lock_irqsave(led_ctrl->lock, flags);
	val = ca_led_read(led_ctrl->mem);
	if (param) {
		led_ctrl->clk_low = 1;
		val |= CA_LED_CLK_POLARITY;
	} else {
		led_ctrl->clk_low = 0;
		val &= ~CA_LED_CLK_POLARITY;
	}
	ca_led_write(led_ctrl->mem, val);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}
static DEVICE_ATTR_RW(clk_low);

static ssize_t blink_rate1_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", (led_ctrl->blink_rate1 + 1) * 16);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t blink_rate1_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return 0;

	if (param >= 16) {
		param = param / 16 - 1;
		if (param > CA_LED_MAX_HW_BLINK)
			param = CA_LED_MAX_HW_BLINK;
	} else {
		param = 0;
	}

	spin_lock_irqsave(led_ctrl->lock, flags);
	led_ctrl->blink_rate1 = param;
	val = ca_led_read(led_ctrl->mem);
	val &= ~(CA_LED_BLINK_RATE1_MASK << CA_LED_BLINK_RATE1_OFFSET);
	val |= led_ctrl->blink_rate1 << CA_LED_BLINK_RATE1_OFFSET;
	ca_led_write(led_ctrl->mem, val);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}

static DEVICE_ATTR_RW(blink_rate1);

static ssize_t blink_rate2_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t ret = 0;

	spin_lock_irqsave(led_ctrl->lock, flags);

	ret += sprintf(buf, "%d\n", (led_ctrl->blink_rate2 + 1) * 16);

	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return ret;
}

static ssize_t blink_rate2_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct ca_led_ctrl *led_ctrl = dev_get_drvdata(dev);
	unsigned long flags, param;
	u32 val;

	if (kstrtoul(buf, 10, &param))
		return 0;

	param = param / 16 - 1;
	if (param > CA_LED_MAX_HW_BLINK)
		param = CA_LED_MAX_HW_BLINK;

	spin_lock_irqsave(led_ctrl->lock, flags);
	led_ctrl->blink_rate2 = param;
	val = ca_led_read(led_ctrl->mem);
	val &= ~(CA_LED_BLINK_RATE2_MASK << CA_LED_BLINK_RATE2_OFFSET);
	val |= led_ctrl->blink_rate2 << CA_LED_BLINK_RATE2_OFFSET;
	ca_led_write(led_ctrl->mem, val);
	spin_unlock_irqrestore(led_ctrl->lock, flags);

	return count;
}

static DEVICE_ATTR_RW(blink_rate2);

static struct attribute *ca_leds_attributes[] = {
	&dev_attr_test_mode.attr,
	&dev_attr_clk_low.attr,
	&dev_attr_blink_rate1.attr,
	&dev_attr_blink_rate2.attr,
	NULL
};

static const struct attribute_group ca_leds_attr_group = {
	.attrs = ca_leds_attributes,
};

static int ca_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct resource *mem_r;
	void __iomem *mem;
	spinlock_t *lock;	/* protect LED resource access */
	u32 val, reg;
	unsigned long flags;

	glb_led_ctrl = devm_kzalloc(dev, sizeof(struct ca_led_ctrl),
				    GFP_KERNEL);
	if (!glb_led_ctrl)
		return -ENOMEM;
	glb_led_ctrl->dev = dev;

	mem_r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem_r)
		return -EINVAL;

	mem = devm_ioremap_resource(dev, mem_r);
	if (IS_ERR(mem))
		return PTR_ERR(mem);
	dev_info(dev, "resource - %pr mapped at 0x%pK\n", mem_r, mem);
	glb_led_ctrl->mem = mem;

	lock = devm_kzalloc(dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	spin_lock_init(lock);
	glb_led_ctrl->lock = lock;

	reg = 0;
	if (!of_property_read_u32(np, "clk-low", &val)) {
		glb_led_ctrl->clk_low = 1;
		reg |= CA_LED_CLK_POLARITY;
	} else {
		reg &= ~CA_LED_CLK_POLARITY;
	}

	if (!of_property_read_u32(np, "hw-blink-rate1", &val)) {
		val = val / 16 - 1;
		glb_led_ctrl->blink_rate1 = val > CA_LED_MAX_HW_BLINK ?
					    CA_LED_MAX_HW_BLINK : val;
	}
	reg |= (glb_led_ctrl->blink_rate1 & CA_LED_BLINK_RATE1_MASK) <<
	       CA_LED_BLINK_RATE1_OFFSET;

	if (!of_property_read_u32(np, "hw-blink-rate2", &val)) {
		val = val / 16 - 1;
		glb_led_ctrl->blink_rate2 = val > CA_LED_MAX_HW_BLINK ?
					    CA_LED_MAX_HW_BLINK : val;
	}
	reg |= (glb_led_ctrl->blink_rate2 & CA_LED_BLINK_RATE2_MASK) <<
	       CA_LED_BLINK_RATE2_OFFSET;

	spin_lock_irqsave(lock, flags);
	ca_led_write(glb_led_ctrl->mem, reg);
	spin_unlock_irqrestore(lock, flags);

	mem += 4;
	for_each_available_child_of_node(np, child) {
		int rc;
		u32 reg;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg >= CA_LED_MAX_COUNT) {
			dev_err(dev, "invalid LED (%u >= %d)\n", reg,
				CA_LED_MAX_COUNT);
			continue;
		}

		rc = ca_led_probe(glb_led_ctrl, child, reg, mem + reg * 4,
				  lock);
		if (rc < 0) {
			of_node_put(child);
			return rc;
		}
	}

	dev_set_drvdata(dev, glb_led_ctrl);

	return sysfs_create_group(&dev->kobj, &ca_leds_attr_group);
}

static int ca_leds_revome(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &ca_leds_attr_group);

	return 0;
}

static const struct of_device_id ca_leds_of_match[] = {
	{ .compatible = "cortina-access,leds", },
	{ },
};
MODULE_DEVICE_TABLE(of, ca_leds_of_match);

static struct platform_driver ca_leds_driver = {
	.probe = ca_leds_probe,
	.remove = ca_leds_revome,
	.driver = {
		.name = "leds-cortina-access",
		.of_match_table = ca_leds_of_match,
	},
};

module_platform_driver(ca_leds_driver);

MODULE_DESCRIPTION("LED driver for cortina access led controllers");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-cortina_access");
