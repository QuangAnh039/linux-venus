/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_LEDTRIG_CA_PAY_H
#define __LINUX_LEDTRIG_CA_PAY_H

#define LED_SW_ON		0x01	/* = LED_ON */
#define LED_HW_BLINK_RX		0x02
#define LED_HW_BLINK_TX		0x04
#define LED_HW_BLINK_HIGH_RATE	0x10
#define LED_HW_ENABLE		0x80

#define PHY_NORMAL_SPEED	0
#define PHY_LOW_SPEED		1
#define PHY_HIGH_SPEED		2

void ledtrig_ca_phy_ctrl(unsigned int port, unsigned int speed, unsigned int value);
#endif	/* __LINUX_LEDTRIG_CA_PAY_H */
