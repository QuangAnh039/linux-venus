// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for Cortinqa ATUs. Somewhat based on qcom_iommu.c
 *
 * Copyright (C) 2024 Cortina Access, Inc.
 *              http://www.cortina-access.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * version: 0.1
 * This driver currently supports:
 *	- Add an entry for DMA hardware coherent based on DTS
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "dma-iommu.h"
#include "ca-atu.h"

struct atu_param {
	u32 input;
	u32 output;
	u32 size;
};

struct atu_table {
	struct	atu_param user;
	bool	enable;
};

struct ca_atu_dev {
	/* IOMMU core code handle */
	struct iommu_device	 iommu;
	struct device		*dev;
	void __iomem		*base;

	int			type;
	u32			comp_mask;
	u32			hi_addr; /* DMA coherent high 32 bit address */
	struct atu_table	table[TLB_COUNT];
	struct atu_param	user;
	bool			keep;
};

struct ca_atu_domain {
	struct mutex		 init_mutex; /* Protects iommu pointer */
	struct iommu_domain	 domain;
	struct ca_atu_dev	*atu_dev;
	struct iommu_fwspec	*fwspec;
};

static ssize_t atu_table_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct ca_atu_dev *atu_dev = dev_get_drvdata(dev);
	char *str = buf;
	int count = 0;
	u32 mask;
	int i;

	for (i = 0; i < TLB_COUNT; i++) {
		if (!atu_dev->table[i].enable)
			continue;

		str += sprintf(str, "TABLE[%d] enable\n", i);
		str += sprintf(str, "TABLE[%d] in   = 0x%08x\n",
			       i, atu_dev->table[i].user.input);
		str += sprintf(str, "TABLE[%d] out  = 0x%08x\n",
			       i, atu_dev->table[i].user.output);
		str += sprintf(str, "TABLE[%d] size = 0x%08x\n",
			       i, atu_dev->table[i].user.size);
		mask = (atu_dev->table[i].user.size - 1) ^ atu_dev->comp_mask;
		str += sprintf(str, "         mask = 0x%08x\n", mask);
		count++;
	}

	str += sprintf(str, "type %d, count %d, unit of 64K\n", atu_dev->type,
		       count);

	return (str - buf);
}
static DEVICE_ATTR_RO(atu_table);

static struct attribute *cortina_atu_attributes[] = {
	&dev_attr_atu_table.attr,
	NULL
};

static const struct attribute_group cortina_atu_attr_group = {
	.attrs = cortina_atu_attributes,
};

static int ca_atu_param_chk(unsigned long input, unsigned long size,
			    unsigned long output)
{
	if ((input % SZ_64K) || (size % SZ_64K) || (output % SZ_64K)) {
		pr_err("ATU - input/size/output!\n");
		return -1;
	}

	if (!is_power_of_2(size)) {
		pr_err("ATU - size must be power of 2!\n");
		return -2;
	}

	if (input % size) {
		pr_err("ATU - input address must be size alignment!\n");
		return -3;
	}

	if (output % size) {
		pr_err("ATU - output address must be size alignment!\n");
		return -4;
	}

	return 0;
}

static int ca_atu_clr(struct ca_atu_dev *atu_dev, int idx)
{
	union atu_ctrl ctrl;
	void __iomem *base = atu_dev->base;
	int type = atu_dev->type;
	int ret;
	u32 val;

	writel(0, base + ATU_INPUT);
	writel(0, base + ATU_MASK);
	writel(0, base + ATU_OUTPUT);

	if (type == T_AXI_STD || type == T_AXI_PLUS) {
		writel(0, base + ACE_CFG);
		writel(0, base + AXI_ARUSER);
		writel(0, base + AXI_AWUSER);
		if (type == T_AXI_PLUS)
			writel(0, base + AXI_UPPER);
	} else {
		writel(0, base + ACE_CFG);
		writel(0, base + AHB_HUSER);
		writel(0, base + AHB_UPPER);
	}
	ctrl.wrd = readl(base + ATU_CTRL);
	ctrl.bf.enable &= ~(0x1 << idx);
	ctrl.bf.idx = idx;
	ctrl.bf.w = 1;
	writel(ctrl.wrd, base + ATU_CTRL);
	ret = readl_poll_timeout(base + ATU_CTRL, val, !(val & ATU_CTRL_W),
				 1, 100000);
	if (ret)
		dev_err(atu_dev->dev, "ATU clear time out\n");

	return ret;
}

static int ca_atu_cfg(struct ca_atu_dev *atu_dev, int idx, bool enable)
{
	union atu_ctrl ctrl;
	void __iomem *base = atu_dev->base;
	int type = atu_dev->type;
	int ret;
	u32 mask, val;

	if (enable) {
		writel(atu_dev->user.input, base + ATU_INPUT);
		mask = (atu_dev->user.size - 1) ^ atu_dev->comp_mask;
		writel(mask, base + ATU_MASK);
		writel(atu_dev->user.output, base + ATU_OUTPUT);

		if (type == T_AXI_STD || type == T_AXI_PLUS) {
			union axi_cfg axi;
			union user_attr aruser, awuser;

			axi.wrd = 0;
			axi.bf.arprot = AXPROT_NONSECURE_ACCESS;
			axi.bf.awprot = AXPROT_NONSECURE_ACCESS;
			axi.bf.prot_en = 1;
			axi.bf.arqos = 0;
			axi.bf.awqos = 0;
			axi.bf.qos_en = 0;
			axi.bf.arcache = AXCACHE_BUFFERABLE |
					 AXCACHE_CACHEABLE |
					 AXCACHE_READ_ALLOCATE |
					 AXCACHE_WRITE_ALLOCATE;
			axi.bf.awcache = AXCACHE_BUFFERABLE |
					 AXCACHE_CACHEABLE |
					 AXCACHE_READ_ALLOCATE |
					 AXCACHE_WRITE_ALLOCATE;
			axi.bf.cache_en = 1;
			axi.bf.user_en = 1;

			aruser.wrd = 0;
			aruser.bf.axuser = 0;
			aruser.bf.axdomain = AXDOMAIN_OUTER_SHAREABLE;
			aruser.bf.axbar = AXBAR_NORMAL_ACCESS_S;
			aruser.bf.axsnoop = ARSNOOP_READONCE;

			awuser.wrd = 0;
			awuser.bf.axuser = 0;
			awuser.bf.axdomain = AXDOMAIN_OUTER_SHAREABLE;
			awuser.bf.axbar = AXBAR_NORMAL_ACCESS_S;
			awuser.bf.axsnoop = AWSNOOP_WRITEUNIQUE;

			writel(axi.wrd, base + ACE_CFG);
			writel(aruser.wrd, base + AXI_ARUSER);
			writel(awuser.wrd, base + AXI_AWUSER);
			if (type == T_AXI_PLUS)
				writel(0, base + AXI_UPPER);
		} else {
			union ahb_cfg ahb;
			union user_attr huser;

			ahb.wrd = 0;
			ahb.bf.hprot = HPROT_PRIVILEGED_ACCESS;
			ahb.bf.prot_en = 1;
			ahb.bf.user_en = 1;

			huser.wrd = 0;
			huser.bf.axuser = 0;
			huser.bf.axdomain = AXDOMAIN_OUTER_SHAREABLE;
			huser.bf.axbar = AXBAR_NORMAL_ACCESS_S;
			huser.bf.axsnoop = AWSNOOP_WRITEUNIQUE;

			writel(ahb.wrd, base + ACE_CFG);
			writel(huser.wrd, base + AHB_HUSER);
			writel(0, base + AHB_UPPER);
		}
	}

	ctrl.wrd = readl(base + ATU_CTRL);
	if (enable)
		ctrl.bf.enable |= (enable << idx);
	else
		ctrl.bf.enable &= ~(0x1 << idx);
	ctrl.bf.idx = idx;
	ctrl.bf.r = 0;
	ctrl.bf.w = 1;
	writel(ctrl.wrd, base + ATU_CTRL);
	ret = readl_poll_timeout(base + ATU_CTRL, val, !(val & ATU_CTRL_W),
				 1, 100000);
	if (ret)
		dev_err(atu_dev->dev, "ATU configure time out\n");

	if (enable) {
		atu_dev->table[idx].user.input = atu_dev->user.input;
		atu_dev->table[idx].user.size = atu_dev->user.size;
		atu_dev->table[idx].user.output = atu_dev->user.output;

		dev_dbg(atu_dev->dev, "enable(%d) input 0x%08x, size 0x%08x, output 0x%08x\n",
			idx, atu_dev->user.input, atu_dev->user.size,
			atu_dev->user.output);
	} else {
		dev_dbg(atu_dev->dev, "disable(%d)\n", idx);
	}
	atu_dev->table[idx].enable = enable;

	return 0;
}

static int ca_atu_sync(struct ca_atu_dev *atu_dev)
{
	union atu_ctrl ctrl;
	void __iomem *base = atu_dev->base;
	int i;

	ctrl.wrd = readl(base + ATU_CTRL);

	for (i = 0; i < TLB_COUNT; i++) {
		atu_dev->table[i].enable = (ctrl.bf.enable >> i) & 0x1;

		if (!atu_dev->table[i].enable)
			continue;

		ctrl.bf.idx = i;
		ctrl.bf.r = 1;
		ctrl.bf.w = 0;
		writel(ctrl.wrd, base + ATU_CTRL);

		atu_dev->table[i].user.input = readl(base + ATU_INPUT);
		atu_dev->table[i].user.size = (readl(base + ATU_MASK) ^
					       atu_dev->comp_mask) + 1;
		atu_dev->table[i].user.output = readl(base + ATU_OUTPUT);
	}

	return 0;
}

/******************************************************************************/
static struct ca_atu_domain *to_ca_atu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct ca_atu_domain, domain);
}

static const struct iommu_ops ca_atu_ops;

static struct ca_atu_dev *to_iommu(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (!fwspec || fwspec->ops != &ca_atu_ops)
		return NULL;

	return dev_iommu_priv_get(dev);
}

static int ca_atu_init_domain(struct iommu_domain *domain,
			      struct ca_atu_dev *atu_dev,
			      struct device *mdev)
{
	struct ca_atu_domain *atu_domain = to_ca_atu_domain(domain);
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(mdev);
	int ret = 0;

	dev_dbg(atu_dev->dev, "%s(d:%ps, f:%ps)\n", __func__, domain, fwspec);

	mutex_lock(&atu_domain->init_mutex);
	if (atu_domain->atu_dev)
		goto out_unlock;

	atu_domain->atu_dev = atu_dev;
	atu_domain->fwspec = fwspec;

out_unlock:
	mutex_unlock(&atu_domain->init_mutex);

	return ret;
}

static struct iommu_domain *ca_atu_domain_alloc(unsigned int type)
{
	struct ca_atu_domain *atu_domain;

	pr_debug("%s(%d)\n", __func__, type);

	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;
	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	atu_domain = kzalloc(sizeof(*atu_domain), GFP_KERNEL);
	if (!atu_domain)
		return NULL;

	if (type == IOMMU_DOMAIN_DMA &&
	    iommu_get_dma_cookie(&atu_domain->domain)) {
		kfree(atu_domain);
		return NULL;
	}

	mutex_init(&atu_domain->init_mutex);

	return &atu_domain->domain;
}

static void ca_atu_domain_free(struct iommu_domain *domain)
{
	struct ca_atu_domain *atu_domain = to_ca_atu_domain(domain);

	pr_debug("%s(%p)\n", __func__, domain);

	iommu_put_dma_cookie(domain);

	if (atu_domain->atu_dev) {
		/*
		 * NOTE: unmap can be called after client device is powered
		 * off, for example, with GPUs or anything involving dma-buf.
		 * So we cannot rely on the device_link.  Make sure the IOMMU
		 * is on to avoid unclocked accesses in the TLB inv path:
		 */
		pm_runtime_get_sync(atu_domain->atu_dev->dev);
//		free_io_pgtable_ops(atu_domain->pgtbl_ops);
		pm_runtime_put_sync(atu_domain->atu_dev->dev);
	}

	kfree(atu_domain);
}

static int ca_atu_attach_dev(struct iommu_domain *domain, struct device *mdev)
{
	struct ca_atu_dev *atu_dev = to_iommu(mdev);
	struct ca_atu_domain *atu_domain = to_ca_atu_domain(domain);
	struct device *dev = atu_dev->dev;
	int ret;

	dev_dbg(dev, "%s(%s)\n", __func__, dev_name(mdev));

	if (!atu_domain) {
		dev_err(dev, "cannot attach to IOMMU, is it on the same bus?\n");
		return -ENXIO;
	}

	/* Ensure that the domain is finalized */
	pm_runtime_get_sync(dev);
	ret = ca_atu_init_domain(domain, atu_dev, mdev);

	if (iommu_is_dma_domain(domain)) {
		ret = iommu_get_dma_cookie(domain);
		if (ret) {
			dev_err(dev, "iommu_get_dma_cookie() failed: %d\n", ret);
			pm_runtime_put_sync(dev);
			return ret;
		}
	}

	if (domain->type == IOMMU_DOMAIN_UNMANAGED) {
		ret = iommu_get_dma_cookie(domain);
		if (ret) {
			dev_err(dev, "iommu_get_dma_cookie failed: %d\n", ret);
			return ret;
		}
	}

	pm_runtime_put_sync(dev);
	if (ret < 0)
		return ret;

	/*
	 * Sanity check the domain. We don't support domains across
	 * different IOMMUs.
	 */
	if (atu_domain->atu_dev != atu_dev) {
		dev_err(dev, "cannot attach to IOMMU %s while already attached to domain on IOMMU %s\n",
			dev_name(atu_domain->atu_dev->dev),
			dev_name(dev));
		return -EINVAL;
	}

	return 0;
}

static int ca_atu_map(struct iommu_domain *domain, unsigned long iova,
		      phys_addr_t paddr, size_t size, size_t pgcount,
		      int prot, gfp_t gfp, size_t *mapped)
{
	struct ca_atu_domain *atu_domain = to_ca_atu_domain(domain);
	struct ca_atu_dev *atu_dev = atu_domain->atu_dev;
	int i = 0;

	dev_dbg(atu_dev->dev, "%s(d:%ps, i:%lx, p:%pa, s:%zx, p:%d)\n",
		__func__, domain, iova, &paddr, size, prot);

	if (ca_atu_param_chk(paddr, size, iova))
		return -EINVAL;

	atu_dev->user.input = paddr / SZ_64K;
	atu_dev->user.size = size / SZ_64K;
	atu_dev->user.output = iova / SZ_64K;

	for (i = 0; i < TLB_COUNT; i++) {
		if (atu_dev->table[i].enable)
			continue;

		ca_atu_cfg(atu_dev, i, true);
		break;
	}

	return 0;
}

static size_t ca_atu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size,
			   size_t pgcount, struct iommu_iotlb_gather *gather)
{
	struct ca_atu_domain *atu_domain = to_ca_atu_domain(domain);
	struct ca_atu_dev *atu_dev = atu_domain->atu_dev;
	int i = 0;

	dev_dbg(atu_dev->dev, "%s(d:%ps, i:%lx, s:%zx)\n", __func__, domain,
		iova, size);

	iova /= SZ_64K;
	size /= SZ_64K;
	for (i = 0; i < TLB_COUNT; i++) {
		if (atu_dev->table[i].user.output == iova &&
		    atu_dev->table[i].user.size == size &&
		    atu_dev->table[i].enable) {
			ca_atu_cfg(atu_dev, i, false);
			break;
		}
	}

	return 0;
}

static phys_addr_t ca_atu_iova_to_phys(struct iommu_domain *domain,
				       dma_addr_t iova)
{
	WARN(1, "Not implement %s(%ps, %pad) now!\n", __func__, domain, &iova);

	return 0;
}

static bool ca_atu_capable(struct device *dev, enum iommu_cap cap)
{
	WARN(1, "Not implement %s(%d) now!\n", __func__, cap);

	return false;
}

static struct iommu_device *ca_atu_probe_device(struct device *mdev)
{
	struct ca_atu_dev *atu_dev = to_iommu(mdev);
	struct device_link *link;
	struct device *dev;
	int i;

	if (!atu_dev)
		return ERR_PTR(-ENODEV);
	dev = atu_dev->dev;

	dev_dbg(dev, "%s(%s)\n", __func__, dev_name(mdev));

	/*
	 * Establish the link between iommu and master, so that the
	 * iommu gets runtime enabled/disabled as per the master's
	 * needs.
	 */
	link = device_link_add(mdev, dev, DL_FLAG_PM_RUNTIME);
	if (!link) {
		dev_err(dev, "Unable to create device link between %s and %s\n",
			dev_name(dev), dev_name(mdev));
		return ERR_PTR(-ENODEV);
	}

	if (!atu_dev->keep) {
		for (i = 0; i < TLB_COUNT; i++)
			ca_atu_clr(atu_dev, i);
	}

	/* Add an entry for HW cache coherence of whole DDR here temporarily */
	if (of_dma_is_coherent(mdev->of_node)) {
		atu_dev->user.input = 0;
		atu_dev->user.size = SZ_2G / SZ_64K;
		atu_dev->user.output = atu_dev->hi_addr << 16;

		ca_atu_cfg(atu_dev, 0, true);
		/* 2nd entry for high memory(> 2GB) */
		atu_dev->user.input = 0 >> 16;
		atu_dev->user.size  = 0xB00000000 >> 16;
		atu_dev->user.output = atu_dev->hi_addr << 16;
		ca_atu_cfg(atu_dev, 1, true);
	}

	return &atu_dev->iommu;
}

static void ca_atu_release_device(struct device *mdev)
{
	struct ca_atu_dev *atu_dev = to_iommu(mdev);

	if (!atu_dev)
		return;

	dev_dbg(atu_dev->dev, "%s(%s)\n", __func__, dev_name(mdev));

	iommu_device_unlink(&atu_dev->iommu, mdev);
	iommu_group_remove_device(mdev);
	iommu_fwspec_free(mdev);
}

static int ca_atu_of_xlate(struct device *mdev, struct of_phandle_args *args)
{
	struct ca_atu_dev *atu_dev;
	struct platform_device *iommu_pdev;
	struct iommu_domain *domain;

	iommu_pdev = of_find_device_by_node(args->np);
	if (WARN_ON(!iommu_pdev))
		return -EINVAL;

	dev_dbg(mdev, "%s() %s\n", __func__, dev_name(&iommu_pdev->dev));

	atu_dev = platform_get_drvdata(iommu_pdev);

	if (!dev_iommu_priv_get(mdev)) {
		dev_iommu_priv_set(mdev, atu_dev);
	} else {
		/* make sure devices iommus dt node isn't referring to
		 * multiple different iommu devices.  Multiple context
		 * banks are ok, but multiple devices are not:
		 */
		if (WARN_ON(atu_dev != dev_iommu_priv_get(mdev))) {
			put_device(&iommu_pdev->dev);
			return -EINVAL;
		}
	}

	domain = iommu_get_domain_for_dev(mdev);
	if (domain && domain->ops == ca_atu_ops.default_domain_ops)
		ca_atu_init_domain(domain, atu_dev, mdev);

	return 0;
}

static int ca_atu_def_domain_type(struct device *dev)
{
	return IOMMU_DOMAIN_UNMANAGED;
}

static const struct iommu_ops ca_atu_ops = {
	.capable	= ca_atu_capable,
	.domain_alloc	= ca_atu_domain_alloc,
	.probe_device	= ca_atu_probe_device,
	.release_device	= ca_atu_release_device,
	.device_group	= generic_device_group,
	.of_xlate	= ca_atu_of_xlate,
	.def_domain_type = ca_atu_def_domain_type,
	.pgsize_bitmap	= 0xffff0000, /* SZ_64K ~ SZ_2G */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= ca_atu_attach_dev,
		.map_pages	= ca_atu_map,
		.unmap_pages	= ca_atu_unmap,
		.iova_to_phys	= ca_atu_iova_to_phys,
		.free		= ca_atu_domain_free,
	}
};

static int ca_atu_device_probe(struct platform_device *pdev)
{
	struct ca_atu_dev *atu_dev;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	u32 val;
	int ret;

	atu_dev = devm_kzalloc(dev, sizeof(*atu_dev), GFP_KERNEL);
	if (!atu_dev)
		return -ENOMEM;

	atu_dev->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		atu_dev->base = devm_ioremap_resource(dev, res);
		dev_dbg(dev, "atu name: %s\n", res->name);
	}

	ret = of_property_read_u32(np, "atu-type", &val);
	if (ret || val > T_AHB_PLUS)
		return -EPERM;
	atu_dev->type = val;
	dev_dbg(dev, "type %d\n", atu_dev->type);
	if (!atu_dev->type && IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT))
		atu_dev->comp_mask = 0x000FFFFF;
	else
		atu_dev->comp_mask = 0x0000FFFF;
	dev_dbg(dev, "comp_mask 0x%08x\n", atu_dev->comp_mask);

	if (of_find_property(np, "keep-init", NULL)) {
		atu_dev->keep = true;
		ca_atu_sync(atu_dev);
	} else {
		atu_dev->keep = false;
	}
	dev_dbg(dev, "keep %d\n", atu_dev->type);

	ret = of_property_read_u32(np, "coherent-hi-addr", &val);
	if (!ret) {
		atu_dev->hi_addr = val;
		dev_dbg(dev, "atu->hi_addr = 0x%08x\n", atu_dev->hi_addr);
	}

	platform_set_drvdata(pdev, atu_dev);

	pm_runtime_enable(dev);

	ret = iommu_device_sysfs_add(&atu_dev->iommu, dev, NULL, "%s",
				     res->name);
	if (ret) {
		dev_err(dev, "Failed to register iommu in sysfs\n");
		return ret;
	}

	ret = iommu_device_register(&atu_dev->iommu, &ca_atu_ops, dev);
	if (ret) {
		dev_err(dev, "Failed to register iommu\n");
		return ret;
	}

	return sysfs_create_group(&dev->kobj, &cortina_atu_attr_group);
}

static int ca_atu_device_remove(struct platform_device *pdev)
{
	struct ca_atu_dev *atu_dev = platform_get_drvdata(pdev);

	pm_runtime_force_suspend(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	iommu_device_sysfs_remove(&atu_dev->iommu);
	iommu_device_unregister(&atu_dev->iommu);

	return 0;
}

static const struct of_device_id ca_atu_of_match[] = {
	{ .compatible = "cortina-access,atu" },
	{ /* sentinel */ }
};

static struct platform_driver ca_atu_driver = {
	.driver	= {
		.name		= "ca-atu",
		.of_match_table	= ca_atu_of_match,
	},
	.probe	= ca_atu_device_probe,
	.remove	= ca_atu_device_remove,
};

static int __init ca_atu_init(void)
{
	return platform_driver_register(&ca_atu_driver);
}
subsys_initcall(ca_atu_init);
