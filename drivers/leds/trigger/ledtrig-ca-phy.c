// SPDX-License-Identifier: GPL-2.0

/*
 * Camera Flash and Torch On/Off Trigger
 *
 * based on ledtrig-ide-disk.c
 *
 * Copyright 2013 Texas Instruments
 *
 * Author: Milo(Woogyom) Kim <milo.kim@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/ledtrig-ca-phy.h>

#if defined(CONFIG_ARCH_CA_MERCURY)
#if defined(CONFIG_LEDS_CA_PHY_2DIR)
#define NUM_CA_PHY_LED	10
#elif defined(CONFIG_LEDS_CA_PHY_2CTRL)
#define NUM_CA_PHY_LED	20
#elif defined(CONFIG_LEDS_CA_PHY_3CTRL)
#define NUM_CA_PHY_LED	30
#else
#define NUM_CA_PHY_LED	20
#endif
#define MAX_CA_PHY_PORT	9
#else
#if defined(CONFIG_LEDS_CA_PHY_2DIR)
#define NUM_CA_PHY_LED	8
#elif defined(CONFIG_LEDS_CA_PHY_2CTRL)
#define NUM_CA_PHY_LED	16
#elif defined(CONFIG_LEDS_CA_PHY_3CTRL)
#define NUM_CA_PHY_LED	24
#else
#define NUM_CA_PHY_LED	16
#endif
#define MAX_CA_PHY_PORT	7
#endif

DEFINE_LED_TRIGGER(ledtrig_ca_phy[NUM_CA_PHY_LED]);

static char *ledtrig_name[NUM_CA_PHY_LED] = {
#if defined(CONFIG_LEDS_CA_PHY_2DIR)
	"ca_phy0",
	"ca_phy1",
	"ca_phy2",
	"ca_phy3",
	"ca_phy4",
	"ca_phy5",
	"ca_phy6",
	"ca_phy7",
#if defined(CONFIG_ARCH_CA_MERCURY)
	"ca_phy8",
	"ca_phy9",
#endif
#elif defined(CONFIG_LEDS_CA_PHY_2CTRL)
	"ca_phy0_link",
	"ca_phy0",
	"ca_phy1_link",
	"ca_phy1",
	"ca_phy2_link",
	"ca_phy2",
	"ca_phy3_link",
	"ca_phy3",
	"ca_phy4_link",
	"ca_phy4",
	"ca_phy5_link",
	"ca_phy5",
	"ca_phy6_link",
	"ca_phy6",
	"ca_phy7_link",
	"ca_phy7",
#if defined(CONFIG_ARCH_CA_MERCURY)
	"ca_phy8_link",
	"ca_phy8",
	"ca_phy9_link",
	"ca_phy9",
#endif
#elif defined(CONFIG_LEDS_CA_PHY_3CTRL)
	"ca_phy0_link",
	"ca_phy0_rx",
	"ca_phy0_tx",
	"ca_phy1_link",
	"ca_phy1_rx",
	"ca_phy1_tx",
	"ca_phy2_link",
	"ca_phy2_rx",
	"ca_phy2_tx",
	"ca_phy3_link",
	"ca_phy3_rx",
	"ca_phy3_tx",
	"ca_phy4_link",
	"ca_phy4_rx",
	"ca_phy4_tx",
	"ca_phy5_link",
	"ca_phy5_rx",
	"ca_phy5_tx",
	"ca_phy6_link",
	"ca_phy6_rx",
	"ca_phy6_tx",
	"ca_phy7_link",
	"ca_phy7_rx",
	"ca_phy7_tx",
#if defined(CONFIG_ARCH_CA_MERCURY)
	"ca_phy8_link",
	"ca_phy8_rx",
	"ca_phy8_tx",
	"ca_phy9_link",
	"ca_phy9_rx",
	"ca_phy9_tx",
#endif
#else
	"ca_phy0_rx",
	"ca_phy0_tx",
	"ca_phy1_rx",
	"ca_phy1_tx",
	"ca_phy2_rx",
	"ca_phy2_tx",
	"ca_phy3_rx",
	"ca_phy3_tx",
	"ca_phy4_rx",
	"ca_phy4_tx",
	"ca_phy5_rx",
	"ca_phy5_tx",
	"ca_phy6_rx",
	"ca_phy6_tx",
	"ca_phy7_rx",
	"ca_phy7_tx",
#if defined(CONFIG_ARCH_CA_MERCURY)
	"ca_phy8_rx",
	"ca_phy8_tx",
	"ca_phy9_rx",
	"ca_phy9_tx"
#endif
#endif
};

void ledtrig_ca_phy_ctrl(unsigned int port, unsigned int speed,
			 unsigned int value)
{
	u8 rx_val;
#if !(defined(CONFIG_LEDS_CA_PHY_2DIR) || \
	defined(CONFIG_LEDS_CA_PHY_2CTRL))
	u8 tx_val;
#endif
#if (defined(CONFIG_LEDS_CA_PHY_3CTRL) || \
	defined(CONFIG_LEDS_CA_PHY_2CTRL)) && \
	(!defined(CONFIG_LEDS_CA_PHY_2DIR))
	u8 link;
#endif
	if (port > MAX_CA_PHY_PORT)
		return;

#if defined(CONFIG_LEDS_CA_PHY_2DIR)
	if (!value) {
		rx_val = 0;
	} else {
		/* Set Tx bit to support Tx/Rx trigger */
		rx_val = LED_HW_ENABLE | LED_HW_BLINK_TX | LED_SW_ON;

		if (speed == PHY_HIGH_SPEED)
			rx_val |= LED_HW_BLINK_HIGH_RATE;
	}

	led_trigger_event(ledtrig_ca_phy[port], rx_val);
#elif defined(CONFIG_LEDS_CA_PHY_2CTRL)
	if (!value) {
		link = 0;
		rx_val = 0;
	} else {
		link = LED_HW_ENABLE | LED_SW_ON;
		rx_val = LED_HW_ENABLE | LED_HW_BLINK_TX | LED_HW_BLINK_RX;

		if (speed == PHY_HIGH_SPEED)
			rx_val |= LED_HW_BLINK_HIGH_RATE;
	}

	led_trigger_event(ledtrig_ca_phy[port * 2], link);
	led_trigger_event(ledtrig_ca_phy[port * 2 + 1], rx_val);

#elif defined(CONFIG_LEDS_CA_PHY_3CTRL)
	if (!value) {
		link = 0;
		rx_val = 0;
		tx_val = 0;
	} else {
		link = LED_HW_ENABLE | LED_SW_ON;
		rx_val = LED_HW_ENABLE | LED_HW_BLINK_RX;
		tx_val = LED_HW_ENABLE | LED_HW_BLINK_TX;

		if (speed == PHY_HIGH_SPEED) {
			rx_val |= LED_HW_BLINK_HIGH_RATE;
			tx_val |= LED_HW_BLINK_HIGH_RATE;
		}
	}

	led_trigger_event(ledtrig_ca_phy[port * 3], link);
	led_trigger_event(ledtrig_ca_phy[port * 3 + 1], rx_val);
	led_trigger_event(ledtrig_ca_phy[port * 3 + 2], tx_val);

#else
	if (!value) {
		rx_val = 0;
		tx_val = 0;
	} else {
		rx_val = LED_HW_ENABLE | LED_HW_BLINK_RX | LED_SW_ON;
		tx_val = LED_HW_ENABLE | LED_HW_BLINK_TX | LED_SW_ON;

		if (speed == PHY_HIGH_SPEED) {
			rx_val |= LED_HW_BLINK_HIGH_RATE;
			tx_val |= LED_HW_BLINK_HIGH_RATE;
		}
	}

	led_trigger_event(ledtrig_ca_phy[port * 2], rx_val);
	led_trigger_event(ledtrig_ca_phy[port * 2 + 1], tx_val);
#endif
}
EXPORT_SYMBOL_GPL(ledtrig_ca_phy_ctrl);

static int __init ledtrig_ca_phy_init(void)
{
	int i;

	for (i = 0; i < NUM_CA_PHY_LED; i++)
		led_trigger_register_simple(ledtrig_name[i],
					    &ledtrig_ca_phy[i]);

	return 0;
}
module_init(ledtrig_ca_phy_init);

static void __exit ledtrig_ca_phy_exit(void)
{
	int i;

	for (i = 0; i < NUM_CA_PHY_LED; i++)
		led_trigger_unregister_simple(ledtrig_ca_phy[i]);
}
module_exit(ledtrig_ca_phy_exit);

MODULE_DESCRIPTION("LED Trigger for Cortina Access Internal PHYs");
MODULE_LICENSE("GPL");
