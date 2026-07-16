// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pin control driver for Cortina-Access CA82xx SoCs
 *
 * Copyright (C) 2024 Cortina Access, Inc.
 *		 http://www.cortina-access.com
 *
 * Based on pinctrl-digicolor.c
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/spinlock.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "pinctrl-utils.h"
#include "core.h"

#define DRIVER_NAME	"pinctrl-ca"

/* Mercury has more PIN_MUX REG */
#if defined(CONFIG_ARCH_CA_MERCURY)
  #define PIN_MUX_OFFSET	0x00
  #define PIN_MUX_LED_OFS	0x04
  #define PIN_MUX_NEW_OFS	0x08
  #define IO_DRV_OFFSET		0x10
  #define GPIO0_MUX_OFFSET	0x14
  #define GPIO1_MUX_OFFSET	0x18
  #define GPIO2_MUX_OFFSET	0x1C
  #define GPIO3_MUX_OFFSET	0x20
  #define GPIO4_MUX_OFFSET	0x24
  #define GPIO5_MUX_OFFSET	0x28
#else
  #define PIN_MUX_OFFSET	0x00
  #define IO_DRV_OFFSET		0x04
  #define GPIO0_MUX_OFFSET	0x08
  #define GPIO1_MUX_OFFSET	0x0C
  #define GPIO2_MUX_OFFSET	0x10
  #define GPIO3_MUX_OFFSET	0x14
  #define GPIO4_MUX_OFFSET	0x18
#endif

/* G3/G3HGU equips 5 GPIO, but 4 for SFU/SFP */
#if defined(CONFIG_ARCH_CA_VENUS)
  #define GPIO_GROUP			5
  #define MUX_REG_CNT			1
#elif defined(CONFIG_ARCH_CA_MERCURY)
  #define GPIO_GROUP			6
  #define MUX_REG_CNT			3
#else		/* Saturn */
  #define GPIO_GROUP			4
  #define MUX_REG_CNT			1
#endif

#define PIN_COLLECTIONS		(GPIO_GROUP + MUX_REG_CNT)	/* GPIO_MUX_+ PIN_MUX REG */
#define PINS_PER_COLLECTION	32
#define PINS_COUNT		(PIN_COLLECTIONS * PINS_PER_COLLECTION)

struct ca_pinmap {
	void __iomem		*regs;
	unsigned int		physical_addr;
	struct device		*dev;
	struct pinctrl_dev	*pctl;

	struct pinctrl_desc	*desc;
	const char		*pin_names[PINS_COUNT];

	struct gpio_chip	chip;
};

static int ca_get_groups_count(struct pinctrl_dev *pctldev)
{
	return PINS_COUNT;
}

static const char *ca_get_group_name(struct pinctrl_dev *pctldev,
				     unsigned int selector)
{
	struct ca_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	/* Exactly one group per pin */
	return pmap->desc->pins[selector].name;
}

static int ca_get_group_pins(struct pinctrl_dev *pctldev, unsigned int selector,
			     const unsigned int **pins,
			     unsigned int *num_pins)
{
	struct ca_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	*pins = &pmap->desc->pins[selector].number;
	*num_pins = 1;

	return 0;
}

static const struct pinctrl_ops ca_pinctrl_ops = {
	.get_groups_count	= ca_get_groups_count,
	.get_group_name		= ca_get_group_name,
	.get_group_pins		= ca_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_pin,
	.dt_free_map		= pinctrl_utils_free_map,
};

const char *const ca_functions[] = {
	"gpio",
	"multi-fn",
};

static int ca_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(ca_functions);
}

static const char *ca_get_fname(struct pinctrl_dev *pctldev, unsigned int selector)
{
	return ca_functions[selector];
}

static int ca_get_groups(struct pinctrl_dev *pctldev, unsigned int selector,
			 const char * const **groups,
			 unsigned * const num_groups)
{
	struct ca_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);

	*groups = pmap->pin_names;
	*num_groups = PINS_COUNT;

	return 0;
}

static void ca_client_sel(int pin_num, int *reg, int *bit)
{
	*bit = (pin_num % PINS_PER_COLLECTION);

	if (pin_num >= (PINS_PER_COLLECTION * GPIO_GROUP)) {	/* PIN_MUX */
		pin_num -= PINS_PER_COLLECTION * GPIO_GROUP;
		*reg = (pin_num / PINS_PER_COLLECTION) * 4 + PIN_MUX_OFFSET;
	} else {			/* Skip IO_DRIVE_CONTROL */
		*reg = (pin_num / PINS_PER_COLLECTION) * 4 + GPIO0_MUX_OFFSET;
	}
}

static int ca_set_mux(struct pinctrl_dev *pctldev, unsigned int selector,
		      unsigned int group)
{
	struct ca_pinmap *pmap = pinctrl_dev_get_drvdata(pctldev);
	int bit_off, reg_off;
	u32 reg;

	ca_client_sel(group, &reg_off, &bit_off);

	reg = readl_relaxed(pmap->regs + reg_off);
	if (selector) {
		/* multi-fn */
		if (reg_off == PIN_MUX_OFFSET) {
			/* PIN_MUX set 1 as multi-function */
			reg |= (1 << bit_off);
		} else {
			/* GPIO_MUX set 0 as multi-function */
			reg &= ~(1 << bit_off);
		}
	} else {
		/* GPIO */
		if (reg_off == PIN_MUX_OFFSET) {
			/* PIN_MUX set 0 as GPIO */
			reg &= ~(1 << bit_off);
		} else {
			/* GPIO_MUX set 1 as GPIO */
			reg |= (1 << bit_off);
		}
	}

	writel_relaxed(reg, pmap->regs + reg_off);

	return 0;
}

static int ca_pmx_request_gpio(struct pinctrl_dev *pcdev,
			       struct pinctrl_gpio_range *range,
			       unsigned int offset)
{
	struct ca_pinmap *pmap = pinctrl_dev_get_drvdata(pcdev);
	int bit_off, reg_off;
	u32 reg;

	ca_client_sel(offset, &reg_off, &bit_off);

	reg = readl_relaxed(pmap->regs + reg_off);
	if (reg_off < GPIO0_MUX_OFFSET) {	/* Overflow! */
		dev_err(pcdev->dev, "ERROR! request invalid GPIO pin %d\n", offset);
		return -EBUSY;
	}

	/* GPIO_MUX set 0 as multi-function */
	reg |= (1 << bit_off);
	writel_relaxed(reg, pmap->regs + reg_off);

	return 0;
}

static const struct pinmux_ops ca_pmxops = {
	.get_functions_count	= ca_get_functions_count,
	.get_function_name	= ca_get_fname,
	.get_function_groups	= ca_get_groups,
	.set_mux		= ca_set_mux,
	.gpio_request_enable	= ca_pmx_request_gpio,
};

static int ca_pinctrl_probe(struct platform_device *pdev)
{
	struct ca_pinmap *pmap;
	struct resource *r;
	struct pinctrl_pin_desc *pins;
	struct pinctrl_desc *pctl_desc;
	char *pin_names;
	int name_len = strlen("GPIO_xxx") + 1;	/* or PMUX_xxx */
	int i, j;

	pmap = devm_kzalloc(&pdev->dev, sizeof(*pmap), GFP_KERNEL);
	if (!pmap)
		return -ENOMEM;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pmap->regs = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(pmap->regs))
		return PTR_ERR(pmap->regs);

	pins = devm_kzalloc(&pdev->dev, sizeof(*pins) * PINS_COUNT, GFP_KERNEL);
	if (!pins)
		return -ENOMEM;
	pin_names = devm_kzalloc(&pdev->dev, name_len * PINS_COUNT,
				 GFP_KERNEL);
	if (!pin_names)
		return -ENOMEM;

	for (i = 0; i < GPIO_GROUP; i++) {	/* GPIO_MUX */
		for (j = 0; j < PINS_PER_COLLECTION; j++) {
			int pin_id = i * PINS_PER_COLLECTION + j;
			char *name = &pin_names[pin_id * name_len];

			snprintf(name, name_len, "GPIO_%c%c%c",
				 '0' + (i), '0' + (j / 10), '0' + (j % 10));
			pins[pin_id].number = pin_id;
			pins[pin_id].name = name;
			pmap->pin_names[pin_id] = name;
		}
	}

	for (i = GPIO_GROUP; i < PIN_COLLECTIONS; i++) {	/* PIN_MUX */
		for (j = 0; j < PINS_PER_COLLECTION; j++) {
			int pin_id = i * PINS_PER_COLLECTION + j;
			char *name = &pin_names[pin_id * name_len];

#if defined(CONFIG_ARCH_CA_MERCURY)
			snprintf(name, name_len, "PMUX_%c%c%c",
				 '0' + (i - GPIO_GROUP), '0' + (j / 10), '0' + (j % 10));
#else
			snprintf(name, name_len, "PMUX_%c%c%c",
				 '0', '0' + (j / 10), '0' + (j % 10));
#endif
			pins[pin_id].number = pin_id;
			pins[pin_id].name = name;
			pmap->pin_names[pin_id] = name;
		}
	}

	pctl_desc = devm_kzalloc(&pdev->dev, sizeof(*pctl_desc), GFP_KERNEL);
	if (!pctl_desc)
		return -ENOMEM;

	pctl_desc->name	= DRIVER_NAME,
	pctl_desc->owner = THIS_MODULE,
	pctl_desc->pctlops = &ca_pinctrl_ops,
	pctl_desc->pmxops = &ca_pmxops,
	pctl_desc->npins = PINS_COUNT;
	pctl_desc->pins = pins;
	pmap->desc = pctl_desc;

	pmap->dev = &pdev->dev;

	pmap->pctl = pinctrl_register(pctl_desc, &pdev->dev, pmap);
	if (IS_ERR(pmap->pctl)) {
		dev_err(&pdev->dev, "pinctrl driver registration failed\n");
		return PTR_ERR(pmap->pctl);
	}

#ifdef CONFIG_LEDS_CA77XX_PHY_2DIR
	{
	u32 reg;

	reg = readl_relaxed(pmap->regs + IO_DRV_OFFSET);
	reg |= BIT(9);	/* flash_ds for PHY Tx/Rx monitor */
	writel_relaxed(reg, pmap->regs + IO_DRV_OFFSET);
	}
#endif

	return 0;
}

static int ca_pinctrl_remove(struct platform_device *pdev)
{
	struct ca_pinmap *pmap = platform_get_drvdata(pdev);

	pinctrl_unregister(pmap->pctl);
	gpiochip_remove(&pmap->chip);

	return 0;
}

static const struct of_device_id ca_pinctrl_ids[] = {
	{ .compatible = "cortina-access,ca8289-pinctrl" },
	{ .compatible = "cortina-access,ca8299-pinctrl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ca_pinctrl_ids);

static struct platform_driver ca_pinctrl_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = ca_pinctrl_ids,
	},
	.probe = ca_pinctrl_probe,
	.remove = ca_pinctrl_remove,
};
module_platform_driver(ca_pinctrl_driver);
