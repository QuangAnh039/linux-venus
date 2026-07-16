// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>

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

#define MAGIC_NUM	0x7533967

struct pelog_header {
	u32 magic_num;
	u32 log_overwrite;
	u32 write_offset;
	char data[];
} __packed;

struct pelog_module {
	void __iomem	*pelog_shm_vaddr;
	resource_size_t pelog_shm_paddr;
	resource_size_t shm_size;
	u32		u32_read_offset;
	char		*rebuild_msg_buf;
};

struct pelog_module pelog_module0, pelog_module1;

MODULE_LICENSE("GPL");
#ifdef CONFIG_OF
/* Match table for of_platform binding */
static const struct of_device_id ca_pelog_of_match[] = {
	{ .compatible = "cortina-access,pelog",},
	{},
};
MODULE_DEVICE_TABLE(of, ca_pelog_of_match);
#endif

int is_pelog_init_sucess0 = 0, is_pelog_init_sucess1 = 0;

static int ca_pelog_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct resource mem_resource;
	const struct of_device_id *match;
	int ret = -ENOMEM;

	/* assign DT node pointer */
	//np = pdev->dev.of_node;

	/* search DT for a match */
	match = of_match_device(ca_pelog_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np) {
		pr_err("No %s specified\n", "memory-region");
		return ret;
	}

	/* get "pelog0 shm" from DT and convert to platform mem address resource */
	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret) {
		pr_err("%s:pelog of_address_to_resource return %d\n", __func__, ret);
		return ret;
	}

	pelog_module0.shm_size = resource_size(&mem_resource) / 2;
	pelog_module0.pelog_shm_vaddr =
		devm_ioremap(&pdev->dev, mem_resource.start, pelog_module0.shm_size);

	if (!pelog_module0.pelog_shm_vaddr) {
		pr_err("pelog0: devm_ioremap fail\n");
		return -ENOMEM;
	}

	pelog_module0.pelog_shm_paddr = mem_resource.start;
	pelog_module0.u32_read_offset = 0;

	pr_err("pelog0: shm physical address : 0x%lx\n",
	       (unsigned long)pelog_module0.pelog_shm_paddr);
	pr_err("pelog0: shm virtual address : 0x%lx\n",
	       (unsigned long)pelog_module0.pelog_shm_vaddr);
	pr_err("pelog0: shm size: 0x%lx\n", (unsigned long)pelog_module0.shm_size);

	pelog_module0.rebuild_msg_buf = kmalloc(pelog_module0.shm_size, GFP_KERNEL);
	if (!pelog_module0.rebuild_msg_buf)
		return -ENOMEM;

	is_pelog_init_sucess0 = 1;

	/* get "pelog1 shm" from DT and convert to platform mem address resource */
	//ret = of_address_to_resource(np, 1, &mem_resource);
	//if (ret) {
	//	pr_err("%s:pelog1 of_address_to_resource return %d\n", __func__, ret);
	//	return ret;
	//}

	pelog_module1.shm_size = resource_size(&mem_resource) / 2;
	pelog_module1.pelog_shm_vaddr =
		devm_ioremap(&pdev->dev, (mem_resource.start + pelog_module0.shm_size),
			     pelog_module1.shm_size);

	if (!pelog_module1.pelog_shm_vaddr) {
		pr_err("pelog1: devm_ioremap fail\n");
		return -ENOMEM;
	}

	pelog_module1.pelog_shm_paddr = mem_resource.start + pelog_module0.shm_size;
	pelog_module1.u32_read_offset = 0;

	pr_err("pelog1: shm physical address : 0x%lx\n",
	       (unsigned long)pelog_module1.pelog_shm_paddr);
	pr_err("pelog1: shm virtual address : 0x%lx\n",
	       (unsigned long)pelog_module1.pelog_shm_vaddr);
	pr_err("pelog1: shm size: 0x%lx\n", (unsigned long)pelog_module1.shm_size);

	pelog_module1.rebuild_msg_buf = kmalloc(pelog_module1.shm_size, GFP_KERNEL);
	if (!pelog_module1.rebuild_msg_buf)
		return -ENOMEM;

	is_pelog_init_sucess1 = 1;

	return 0;
}

static int ca_pelog_remove(struct platform_device *op)
{
	return 0;
}

static struct platform_driver ca_pelog_driver = {
	.probe = ca_pelog_probe,
	.remove = ca_pelog_remove,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "ca_pelog",
		   .of_match_table = of_match_ptr(ca_pelog_of_match),
	},
};

static int __init ca_pelog_init(void)
{
	//return init_procfs_pelog();
	return platform_driver_register(&ca_pelog_driver);
}

static void __exit ca_pelog_exit(void)
{
	platform_driver_unregister(&ca_pelog_driver);
}

module_init(ca_pelog_init);
module_exit(ca_pelog_exit);

static int pelog0_proc_show(struct seq_file *m, void *v)
{
	struct pelog_header *pelog = (struct pelog_header *)pelog_module0.pelog_shm_vaddr;
	u64 header_size = (uint64_t)(pelog->data) - (uint64_t)pelog;
	u64 msg_buf_size = (uint64_t)pelog_module0.shm_size - (uint64_t)header_size;

	u32 log_offset = pelog->write_offset;
	u32 log_overwrite = pelog->log_overwrite;

	log_offset = log_offset - log_offset % 4; //4 bytes alignment

	if (is_pelog_init_sucess0 == 0) {
		pr_err("Pelog0 init fail\n");
		return 0;
	}

	if (pelog->magic_num != MAGIC_NUM) {
		pr_err("PEDSP0's Pelog not enable\n");
		return 0;
	}

	if (log_offset == pelog_module0.u32_read_offset && log_overwrite == 0) {
		pr_err("PEDSP0's Pelog no log\n");
		return 0;
	}

	memset(pelog_module0.rebuild_msg_buf, 0, pelog_module0.shm_size);

	if (log_overwrite == 0) {
		//pr_err("%s %d\n",__FUNCTION__, __LINE__);
		if (pelog_module0.u32_read_offset > log_offset) { //DSP reboot case
			pr_err("PEDSP0 Pelog log reset\n");
			memcpy_fromio(pelog_module0.rebuild_msg_buf, (pelog->data), log_offset);
			pelog_module0.u32_read_offset = log_offset;
		} else {
			memcpy_fromio(pelog_module0.rebuild_msg_buf,
				      (pelog->data) + pelog_module0.u32_read_offset,
				      log_offset - pelog_module0.u32_read_offset);
			pelog_module0.u32_read_offset = log_offset;
		}
	} else if (log_overwrite == 1) {
		if (pelog_module0.u32_read_offset > log_offset) {
			memcpy_fromio(pelog_module0.rebuild_msg_buf,
				      (pelog->data) + pelog_module0.u32_read_offset,
				      (msg_buf_size - pelog_module0.u32_read_offset));
			memcpy_fromio((pelog_module0.rebuild_msg_buf) +
				      (msg_buf_size - pelog_module0.u32_read_offset),
				      (pelog->data), log_offset);
			pelog->log_overwrite = 0;
			pelog_module0.u32_read_offset = log_offset;
		} else {
			pr_err("PEDSP0 Pelog log overwrite\n");
			memcpy_fromio(pelog_module0.rebuild_msg_buf, (pelog->data) + log_offset,
				      (msg_buf_size - log_offset));
			memcpy_fromio((pelog_module0.rebuild_msg_buf) + (msg_buf_size - log_offset),
				      (pelog->data), log_offset);
			pelog->log_overwrite = 0;
			pelog_module0.u32_read_offset = log_offset;
		}

		//memcpy(pelog_module.rebuild_msg_buf, (pelog->data) + log_offset,
		//	 (msg_buf_size - log_offset));
		//memcpy((pelog_module.rebuild_msg_buf) + (msg_buf_size-log_offset),
		//	 (pelog->data),log_offset);
		//pr_err("%s %d\n",__FUNCTION__, __LINE__);
	} else {
		pr_err("PEDSP0 Pelog log overwrite more then once\n");
		memcpy_fromio(pelog_module0.rebuild_msg_buf, (pelog->data) + log_offset,
			      (msg_buf_size - log_offset));
		memcpy_fromio((pelog_module0.rebuild_msg_buf) + (msg_buf_size - log_offset),
			      (pelog->data), log_offset);
		pelog->log_overwrite = 0;
		pelog_module0.u32_read_offset = log_offset;
	}

	seq_printf(m, (char *)pelog_module0.rebuild_msg_buf);

	return 0;
}

static int pelog0_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pelog0_proc_show, NULL);
}

static const struct proc_ops pelog0_proc_fops = {
	.proc_open	= pelog0_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int pelog1_proc_show(struct seq_file *m, void *v)
{
	struct pelog_header *pelog = (struct pelog_header *)pelog_module1.pelog_shm_vaddr;
	u64 header_size = (uint64_t)(pelog->data) - (uint64_t)pelog;
	u64 msg_buf_size = (uint64_t)pelog_module1.shm_size - (uint64_t)header_size;

	u32 log_offset = pelog->write_offset;
	u32 log_overwrite = pelog->log_overwrite;

	if (is_pelog_init_sucess1 == 0) {
		pr_err("Pelog1 init fail\n");
		return 0;
	}

	if (pelog->magic_num != MAGIC_NUM) {
		pr_err("PEDSP1's Pelog not enable\n");
		return 0;
	}

	if (log_offset == pelog_module1.u32_read_offset && log_overwrite == 0) {
		pr_err("PEDSP1's Pelog no log\n");
		return 0;
	}

	memset(pelog_module1.rebuild_msg_buf, 0, pelog_module1.shm_size);

	if (log_overwrite == 0) {
		//pr_err("%s %d\n",__FUNCTION__, __LINE__);
		if (pelog_module1.u32_read_offset > log_offset) { //DSP reboot case
			pr_err("PEDSP1 Pelog log reset\n");
			memcpy_fromio(pelog_module1.rebuild_msg_buf, (pelog->data), log_offset);
			pelog_module1.u32_read_offset = log_offset;
		} else {
			memcpy_fromio(pelog_module1.rebuild_msg_buf,
				      (pelog->data) + pelog_module1.u32_read_offset,
				      log_offset - pelog_module1.u32_read_offset);
			pelog_module1.u32_read_offset = log_offset;
		}
	} else if (log_overwrite == 1) {
		if (pelog_module1.u32_read_offset > log_offset) {
			memcpy_fromio(pelog_module1.rebuild_msg_buf,
				      (pelog->data) + pelog_module1.u32_read_offset,
				      (msg_buf_size - pelog_module1.u32_read_offset));
			memcpy_fromio((pelog_module1.rebuild_msg_buf) +
				      (msg_buf_size - pelog_module1.u32_read_offset), (pelog->data),
				      log_offset);
			pelog->log_overwrite = 0;
			pelog_module1.u32_read_offset = log_offset;
		} else {
			pr_err("PEDSP1 Pelog log overwrite\n");
			memcpy_fromio(pelog_module1.rebuild_msg_buf, (pelog->data) + log_offset,
				      (msg_buf_size - log_offset));
			memcpy_fromio((pelog_module1.rebuild_msg_buf) + (msg_buf_size - log_offset),
				      (pelog->data), log_offset);
			pelog->log_overwrite = 0;
			pelog_module1.u32_read_offset = log_offset;
		}

		//memcpy(pelog_module.rebuild_msg_buf, (pelog->data) + log_offset,
		//	 (msg_buf_size - log_offset));
		//memcpy((pelog_module.rebuild_msg_buf) + (msg_buf_size-log_offset), (pelog->data),
		//	 log_offset);
		//pr_err("%s %d\n",__FUNCTION__, __LINE__);
	} else {
		pr_err("PEDSP1 Pelog log overwrite more then once\n");
		memcpy_fromio(pelog_module1.rebuild_msg_buf, (pelog->data) + log_offset,
			      (msg_buf_size - log_offset));
		memcpy_fromio((pelog_module1.rebuild_msg_buf) + (msg_buf_size - log_offset),
			      (pelog->data), log_offset);
		pelog->log_overwrite = 0;
		pelog_module1.u32_read_offset = log_offset;
	}

	seq_printf(m, (char *)pelog_module1.rebuild_msg_buf);

	return 0;
}

static int pelog1_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, pelog1_proc_show, NULL);
}

static const struct proc_ops pelog1_proc_fops = {
	.proc_open	= pelog1_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init proc_pelog_init(void)
{
	proc_create("pelog0", 0, NULL, &pelog0_proc_fops);
	proc_create("pelog1", 0, NULL, &pelog1_proc_fops);
	return 0;
}
fs_initcall(proc_pelog_init);
