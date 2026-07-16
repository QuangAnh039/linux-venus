// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>

#define CA_EPLLDIV_MIN		0x7f007f7f
#define CA_EPLLDIV2_MIN		0x7f7f

struct ca_pll_clk_info {
	struct device *dev;
	void __iomem *glb_eplldiv_reg;
	void __iomem *glb_eplldiv2_reg;
	u32 glb_eplldiv_prev;
	u32 glb_eplldiv2_prev;
	/* mutex lock for pll reg */
	struct mutex *lock;
};

static struct ca_pll_clk_info *ca_pll_clk_pdata;

static unsigned long ca_pll_clk_read(void __iomem *reg)
{
	return ioread32(reg);
}

static void ca_pll_clk_write(void __iomem *reg, unsigned long data)
{
	iowrite32(data, reg);
}

static void ca_pll_clk_s2idle_end(void)
{
	struct device *dev = ca_pll_clk_pdata->dev;

	dev_info(dev, "%s: line %d\n", __func__, __LINE__);
}

static void ca_pll_clk_restore_early(void)
{
	struct device *dev = ca_pll_clk_pdata->dev;
	void __iomem *div_reg = ca_pll_clk_pdata->glb_eplldiv_reg;
	void __iomem *div2_reg = ca_pll_clk_pdata->glb_eplldiv2_reg;
	u32 div_prev = ca_pll_clk_pdata->glb_eplldiv_prev;
	u32 div2_prev = ca_pll_clk_pdata->glb_eplldiv2_prev;
	struct mutex *lock = ca_pll_clk_pdata->lock;

	/* Resume clock rates */
	mutex_lock(lock);
	ca_pll_clk_write(div_reg, div_prev);
	ca_pll_clk_write(div2_reg, div2_prev);
	mutex_unlock(lock);

	dev_info(dev, "%s: restore peripheral clocks\n", __func__);
}

static int ca_pll_clk_prepare_late(void)
{
	struct device *dev = ca_pll_clk_pdata->dev;
	void __iomem *div_reg = ca_pll_clk_pdata->glb_eplldiv_reg;
	void __iomem *div2_reg = ca_pll_clk_pdata->glb_eplldiv2_reg;
	u32 *div_prev = &ca_pll_clk_pdata->glb_eplldiv_prev;
	u32 *div2_prev = &ca_pll_clk_pdata->glb_eplldiv2_prev;
	struct mutex *lock = ca_pll_clk_pdata->lock;

	mutex_lock(lock);
	*div_prev = ca_pll_clk_read(div_reg);
	*div2_prev = ca_pll_clk_read(div2_reg);

	/* Decrease clock rates */
	ca_pll_clk_write(div_reg, CA_EPLLDIV_MIN);
	ca_pll_clk_write(div2_reg, CA_EPLLDIV2_MIN);
	mutex_unlock(lock);

	dev_info(dev, "%s: scale peripheral clocks\n", __func__);

	return 0;
}

static int ca_pll_clk_s2idle_begin(void)
{
	struct device *dev = ca_pll_clk_pdata->dev;

	dev_info(dev, "%s: line %d\n", __func__, __LINE__);
	return 0;
}

static const struct platform_s2idle_ops ca_pll_clk_s2idle_ops = {
	.begin		= ca_pll_clk_s2idle_begin,
	.prepare_late	= ca_pll_clk_prepare_late,
	.restore_early	= ca_pll_clk_restore_early,
	.end		= ca_pll_clk_s2idle_end,
};

static int ca_pll_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *mem_r;
	void __iomem *mem;
	/* mutex lock for pll reg */
	struct mutex *lock;

	ca_pll_clk_pdata = devm_kzalloc(dev, sizeof(struct ca_pll_clk_info),
					GFP_KERNEL);
	if (!ca_pll_clk_pdata)
		return -ENOMEM;

	ca_pll_clk_pdata->dev = dev;

	/* Map GLOBAL_EPLLDIV */
	mem_r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem_r)
		return -EINVAL;

	mem = devm_ioremap_resource(dev, mem_r);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	ca_pll_clk_pdata->glb_eplldiv_reg = mem;

	/* Map GLOBAL_EPLLDIV2 */
	mem_r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!mem_r)
		return -EINVAL;

	mem = devm_ioremap_resource(dev, mem_r);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	ca_pll_clk_pdata->glb_eplldiv2_reg = mem;

	/* Initialize lock */
	lock = devm_kzalloc(dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	mutex_init(lock);
	ca_pll_clk_pdata->lock = lock;

	/* Register s2idle ops */
	s2idle_set_ops(&ca_pll_clk_s2idle_ops);

	dev_set_drvdata(dev, ca_pll_clk_pdata);
	return 0;
}

static int ca_pll_clk_remove(struct platform_device *pdev)
{
	/* Unregister ops */
	s2idle_set_ops(NULL);

	return 0;
}

static const struct of_device_id ca_pll_clk_match[] = {
	{ .compatible = "cortina-access,ca8299-pll" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ca_pll_clk_match);

static struct platform_driver ca_pll_clk_driver = {
	.probe = ca_pll_clk_probe,
	.remove = ca_pll_clk_remove,
	.driver = {
		.name = "ca8299-pll-clk",
		.of_match_table = ca_pll_clk_match,
	},
};

module_platform_driver(ca_pll_clk_driver);

MODULE_DESCRIPTION("Cortina PLL suspend-to-idle driver");
MODULE_LICENSE("GPL");
