/*
 * Copyright (c) Cortina-Access Limited 2015.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>

#define PER_INTERRUPT_STATUS	0x00
#define PER_INTERRUPT_ENABLE	0x04

static DEFINE_RAW_SPINLOCK(irq_controller_lock);

static void ca_per_ictl_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int n;

	chained_irq_enter(chip, desc);

	for (n = 0; n < d->revmap_size; n += 32) {
		struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, n);
		u32 stat = readl_relaxed(gc->reg_base + PER_INTERRUPT_STATUS);
		u32 enable = readl_relaxed(gc->reg_base + PER_INTERRUPT_ENABLE);

		stat = stat & enable;

		while (stat) {
			u32 hwirq = ffs(stat) - 1;
			u32 virq = irq_find_mapping(d, gc->irq_base + hwirq);

			if (virq)
			{
				generic_handle_irq(virq);
			}
			stat &= ~(1 << hwirq);
		}
	}

	chained_irq_exit(chip, desc);
}

static int ca_peri_irq_set_wake(struct irq_data *data, unsigned int on)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	if (on)
		irq_gc_mask_set_bit(data);
	else
		irq_gc_mask_clr_bit(data);
	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	irq_chip_set_wake_parent(data, on);
#endif
	return 0;
}

int __init ca_per_ictl_init(struct device_node *np,
			    struct device_node *parent)
{
	unsigned int clr = IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN;
	struct resource r;
	struct irq_domain *domain;
	struct irq_chip_generic *gc;
	void __iomem *iobase;
	int ret, nrirqs, irq;

	/* Map the parent interrupt for the chained handler */
	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("%s: unable to parse irq\n", np->full_name);
		return -EINVAL;
	}

	ret = of_address_to_resource(np, 0, &r);
	if (ret) {
		pr_err("%s: unable to get resource\n", np->full_name);
		return ret;
	}

	if (!request_mem_region(r.start, resource_size(&r), np->full_name)) {
		pr_err("%s: unable to request mem region\n", np->full_name);
		return -ENOMEM;
	}

	iobase = ioremap(r.start, resource_size(&r));
	if (!iobase) {
		pr_err("%s: unable to map resource\n", np->full_name);
		ret = -ENOMEM;
		goto err_release;
	}

	/* mask all interrupts */
	writel_relaxed(0, iobase + PER_INTERRUPT_ENABLE);

	nrirqs = 32;

	domain = irq_domain_add_linear(np, nrirqs, &irq_generic_chip_ops, NULL);
	if (!domain) {
		pr_err("%s: unable to add irq domain\n", np->full_name);
		ret = -ENOMEM;
		goto err_unmap;
	}

	ret = irq_alloc_domain_generic_chips(domain, 32, (nrirqs > 32) ? 2 : 1,
					     np->name, handle_level_irq, clr, 0,
					     IRQ_GC_INIT_MASK_CACHE);
	if (ret) {
		pr_err("%s: unable to alloc irq domain gc\n", np->full_name);
		goto err_unmap;
	}

	gc = irq_get_domain_generic_chip(domain, 0);
	gc->private = domain;
	gc->reg_base = iobase;

	gc->chip_types[0].regs.ack = PER_INTERRUPT_STATUS;
	gc->chip_types[0].regs.mask = PER_INTERRUPT_ENABLE;

	/* 1: Enable,
	 * 0: Disable
	 * Just swap mask/unmask and re-use irq_gc framework.
	 */
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_clr_bit;
	gc->chip_types[0].chip.irq_unmask = irq_gc_mask_set_bit;
	gc->chip_types[0].chip.irq_set_wake = ca_peri_irq_set_wake;

	irq_set_chained_handler_and_data(irq, ca_per_ictl_handler, domain);
	return 0;

err_unmap:
	iounmap(iobase);
err_release:
	release_mem_region(r.start, resource_size(&r));
	return ret;
}

IRQCHIP_DECLARE(ca_per_ictl, "cortina-access,per-ictl", ca_per_ictl_init);
