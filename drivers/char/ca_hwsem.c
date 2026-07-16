// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/io.h>

#include <soc/cortina-access/ca_hwsem.h>

MODULE_LICENSE("GPL");

typedef volatile unsigned int ca_reg32;

/* register map */
struct ca_sem_reg {
	ca_reg32 u32semaphore[24];
	ca_reg32 u32semaphore_status[24];
	ca_reg32 u32cortex_axid_mask;
	#define CORTEX_AXID_MASK_INIT_VAL	0x00000000

	ca_reg32 u32taroko_axid_mask;
	#define TAROKO_AXID_MASK_INIT_VAL	0x00000003
};

struct ca_sem_module {
	struct ca_sem_reg *sem_reg_addr;
};

static struct ca_sem_module *ca_sem_context;

int ca_sem_lock(enum ca_sem_id semid, unsigned long flags)
{
	unsigned int u32reg_val;

	/* semaphore is ocupied if u32reg_val = 0x0000_0000 */
	do {
		u32reg_val = readl(&ca_sem_context->sem_reg_addr->u32semaphore[semid]);
	} while (u32reg_val == 0x0);

	return 0;
}
EXPORT_SYMBOL(ca_sem_lock);

int ca_sem_unlock(enum ca_sem_id semid, unsigned long flags)
{
	/* write register to release semaphore */
	writel(0xffffffff, &ca_sem_context->sem_reg_addr->u32semaphore[semid]);

	return 0;
}
EXPORT_SYMBOL(ca_sem_unlock);

/* Match table for of_platform binding */
static const struct of_device_id ca_sem_of_match[] = {
	{ .compatible = "cortina-access,sem", },
	{}
};
MODULE_DEVICE_TABLE(of, ca_sem_of_match);

static int ca_hwsem_probe(struct platform_device *pdev)
{
	int ret;
	struct resource mem_resource;
	const struct of_device_id *match;
	struct device_node *np;

	ca_sem_context = kmalloc(sizeof(*ca_sem_context), GFP_KERNEL);
	if (!ca_sem_context)
		return -EINVAL;

	/* get the resource map from device tree for
	 * hardware semaphore registers and shared memory
	 */

	/* assign DT node pointer */
	np = pdev->dev.of_node;

	/* search DT for a match */
	match = of_match_device(ca_sem_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	/* get "sem register" from DT and convert to platform mem address resource */
	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret) {
		pr_err("%s: of_address_to_resource return %d\n", __func__, ret);
		return ret;
	}

	/* map physical memory of hardware semaphore, and get the virtual device memory */
	ca_sem_context->sem_reg_addr =
	    (struct ca_sem_reg *)devm_ioremap(&pdev->dev, mem_resource.start,
								   resource_size(&mem_resource));

	/* reset axid mask */
	writel(CORTEX_AXID_MASK_INIT_VAL, &ca_sem_context->sem_reg_addr->u32cortex_axid_mask);
	writel(TAROKO_AXID_MASK_INIT_VAL, &ca_sem_context->sem_reg_addr->u32taroko_axid_mask);

	return 0;
}

static int ca_hwsem_remove(struct platform_device *op)
{
	iounmap(ca_sem_context->sem_reg_addr);
	kfree(ca_sem_context);

	return 0;
}

static struct platform_driver ca_hwsem_driver = {
	.probe = ca_hwsem_probe,
	.remove = ca_hwsem_remove,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "ca_hwsem",
		   .of_match_table = of_match_ptr(ca_sem_of_match),
		   },
};

static int __init ca_hwsem_init(void)
{
	//return platform_driver_probe(&ca_dma_driver, ca_dma_probe);
	return platform_driver_register(&ca_hwsem_driver);
}

static void __exit ca_hwsem_exit(void)
{
	platform_driver_unregister(&ca_hwsem_driver);
}

module_init(ca_hwsem_init);
module_exit(ca_hwsem_exit);
