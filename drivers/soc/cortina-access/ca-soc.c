// SPDX-License-Identifier: GPL-2.0-only

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/nvmem-consumer.h>
#if defined(CONFIG_ARM64)
  #include <linux/arm-smccc.h>
  #include <soc/cortina-access/ca-smc.h>
#endif
#include <soc/cortina-access/ca-soc.h>
#include <soc/cortina-access/registers.h>


#define USB_ID_REV	BIT(0)
#define PON_REV		BIT(1)
#define PON_CHIP_REV	BIT(2)
#define VENUS_REV	BIT(3)

#define DIV_BASE 4

#define MHZ		(1000000)
#define MHZDP(HZ)	(((HZ) / 10000) % 100)

#define VER_STR_LEN 20
struct rom_version {
	unsigned char tapeout_version;
	uintptr_t ver_str_addr; /* Address of version string */
	unsigned char ver_str[VER_STR_LEN]; /* Version string */
};

struct ca_soc_ext {
	u32 flag;
	u32 rom_addr;
	struct rom_version *pon_str;
};

struct ca_socinfo {
	struct soc_device *soc_dev;
	struct soc_device_attribute attr;
	struct ca_soc_ext *soc_ext;
	struct ca_soc_data soc_data;
};

struct ca_soc_data *ca_soc;

struct proc_dir_entry *ca_proc_dir;
EXPORT_SYMBOL(ca_proc_dir);

unsigned int rotpk_hash;

#define CA_SOC_RD(reg)			readl(jtag_base + ((reg) - GLOBAL_JTAG_ID))
#define CA_SOC_WR(val, reg)	writel(val, jtag_base + ((reg) - GLOBAL_JTAG_ID))
void __iomem *jtag_base;

#ifndef CONFIG_SOC_BUS_EXTEND
/* A deprecated leggacy API. Please use soc_device_one_match() in place of it.
 */
int ca_soc_data_get(struct ca_soc_data *data)
{
	if (!data)
		return -EINVAL;
	if (!ca_soc)
		return -EPERM;

	memcpy(data, ca_soc, sizeof(*ca_soc));

	return 0;
}
EXPORT_SYMBOL(ca_soc_data_get);
#endif

static int proc_rotpk_check_show(struct seq_file *m, void *v)
{
	seq_puts(m, (rotpk_hash == 1) ? "enabled\n" : "disabled\n");
	return 0;
}

static int proc_rotpk_check_open(struct inode *inode, struct  file *file)
{
	return single_open(file, proc_rotpk_check_show, NULL);
}

static const struct proc_ops proc_rotpk_check_fops = {
	.proc_open = proc_rotpk_check_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

#if defined(CONFIG_ARM64)
u64 ca_smc_call(unsigned int smc_id, unsigned long x1, unsigned long x2,
		unsigned long x3, unsigned long x4)
{
	struct arm_smccc_res res;

	if (x1 == 0) {
		pr_err("Illegal SMC call(Address 0)!\n");
		while (1)
			;
	}

	arm_smccc_smc(smc_id, x1, x2, x3, 0, 0, 0, 0, &res);
	return res.a0;
}
EXPORT_SYMBOL(ca_smc_call);
#endif

#ifdef CONFIG_ARCH_CORTINA_ACCESS_SMC_ROTPK
static int rotpk_check(void)
{
	return ca_smc_call(CA_SVC_OTP_ROTPK_STAT, 1, 0, 0, 0);
}
#else
static int rotpk_check(void)
{
	return 0;
}
#endif

static int ca_proc_init(void)
{
	ca_proc_dir = proc_mkdir("driver/cortina-access", NULL);

	rotpk_hash = rotpk_check();

	if (proc_create("ROTPK_Hash", 0444, ca_proc_dir,
			&proc_rotpk_check_fops) == NULL)
		pr_err("Fail to create proc entry ROTPK_Hash.\n");

	return 0;
}

static int ca_soc_data_dump(struct platform_device *pdev, struct ca_socinfo *cs)
{
	struct device *dev = &pdev->dev;
	struct ca_soc_data *soc_data = &cs->soc_data;
	unsigned int part_no;

	part_no = CA_SMC_CALL_CHIP_ID();
	dev_info(dev, "PART#: 0x%06X\n", part_no);
	dev_info(dev, "Vendor ID: 0x%03X\n", soc_data->vendor_id);
	dev_info(dev, "Chip ID: 0x%04X\n", soc_data->chip_id);
	dev_info(dev, "Chip Rev: %c\n", soc_data->chip_revision);
	if (soc_data->pon_revision)
		dev_info(dev, "PON Rev: %c\n", soc_data->pon_revision);

	if (soc_data->cpll != 0)
		dev_info(dev, "CPLL: %u.%02u MHZ\n",
			 soc_data->cpll / MHZ,
			 MHZDP(soc_data->cpll));
	if (soc_data->epll != 0)
		dev_info(dev, "EPLL: %u.%02u MHZ\n",
			 soc_data->epll / MHZ,
			 MHZDP(soc_data->epll));
	if (soc_data->fpll != 0)
		dev_info(dev, "FPLL: %u.%02u MHZ\n",
			 soc_data->fpll / MHZ,
			 MHZDP(soc_data->fpll));
	if (soc_data->cpu_clk != 0)
		dev_info(dev, "CPU Clock: %u.%02u MHZ\n",
			 soc_data->cpu_clk / MHZ,
			 MHZDP(soc_data->cpu_clk));
	if (soc_data->core_clk != 0)
		dev_info(dev, "Core Clock: %u.%02u MHZ\n",
			 soc_data->core_clk / MHZ,
			 MHZDP(soc_data->core_clk));
	if (soc_data->eaxi_clk != 0)
		dev_info(dev, "EAXI Clock: %u.%02u MHZ\n",
			 soc_data->eaxi_clk / MHZ,
			 MHZDP(soc_data->eaxi_clk));
	if (soc_data->lsaxi_clk != 0)
		dev_info(dev, "LSAXI Clock: %u.%02u MHZ\n",
			 soc_data->lsaxi_clk / MHZ,
			 MHZDP(soc_data->lsaxi_clk));
	if (soc_data->hsaxi_clk != 0)
		dev_info(dev, "HSAXI Clock: %u.%02u MHZ\n",
			 soc_data->hsaxi_clk / MHZ,
			 MHZDP(soc_data->hsaxi_clk));
	if (soc_data->cci_clk != 0)
		dev_info(dev, "CCI Clock: %u.%02u MHZ\n",
			 soc_data->cci_clk / MHZ,
			 MHZDP(soc_data->cci_clk));
	if (soc_data->crypto_clk != 0)
		dev_info(dev, "Crypto Clock: %u.%02u MHZ\n",
			 soc_data->crypto_clk / MHZ,
			 MHZDP(soc_data->crypto_clk));
	if (soc_data->atb_clk != 0)
		dev_info(dev, "ATB Clock: %u.%02u MHZ\n",
			 soc_data->atb_clk / MHZ,
			 MHZDP(soc_data->atb_clk));
	if (soc_data->pe_clk != 0)
		dev_info(dev, "PE Clock: %u.%02u MHZ\n",
			 soc_data->pe_clk / MHZ,
			 MHZDP(soc_data->pe_clk));
	if (soc_data->peaxi_clk != 0)
		dev_info(dev, "PE AXI Clock: %u.%02u MHZ\n",
			 soc_data->peaxi_clk / MHZ,
			 MHZDP(soc_data->peaxi_clk));
	if (soc_data->per_clk != 0)
		dev_info(dev, "PER Clock: %u.%02u MHZ\n",
			 soc_data->per_clk / MHZ,
			 MHZDP(soc_data->per_clk));
	if (soc_data->io_clk != 0)
		dev_info(dev, "I/O Clock: %u.%02u MHZ\n",
			 soc_data->io_clk / MHZ,
			 MHZDP(soc_data->io_clk));
	if (soc_data->eth_ref2_clk != 0)
		dev_info(dev, "Eth Ref 2 Clock: %u.%02u MHZ\n",
			 soc_data->eth_ref2_clk / MHZ,
			 MHZDP(soc_data->eth_ref2_clk));

	return 0;
}

static ssize_t
data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	char *bp = buf;
	unsigned int part_no;

	part_no = CA_SMC_CALL_CHIP_ID();
	bp += sprintf(bp, "PART#: 0x%06X\n", part_no);
	bp += sprintf(bp, "Vendor ID: 0x%03X\n", ca_soc->vendor_id);
	bp += sprintf(bp, "Chip ID: 0x%04X\n", ca_soc->chip_id);
	bp += sprintf(bp, "Chip Rev: %c\n", ca_soc->chip_revision);
	if (ca_soc->pon_revision)
		bp += sprintf(bp, "PON Rev: %c\n", ca_soc->pon_revision);

	if (ca_soc->cpll != 0)
		bp += sprintf(bp, "CPLL: %u.%02u MHZ\n",
			      ca_soc->cpll / MHZ,
			      MHZDP(ca_soc->cpll));
	if (ca_soc->epll != 0)
		bp += sprintf(bp, "EPLL: %u.%02u MHZ\n",
			      ca_soc->epll / MHZ,
			      MHZDP(ca_soc->epll));
	if (ca_soc->fpll != 0)
		bp += sprintf(bp, "FPLL: %u.%02u MHZ\n",
			      ca_soc->fpll / MHZ,
			      MHZDP(ca_soc->fpll));
	if (ca_soc->cpu_clk != 0)
		bp += sprintf(bp, "CPU Clock: %u.%02u MHZ\n",
			      ca_soc->cpu_clk / MHZ,
			      MHZDP(ca_soc->cpu_clk));
	if (ca_soc->core_clk != 0)
		bp += sprintf(bp, "Core Clock: %u.%02u MHZ\n",
			      ca_soc->core_clk / MHZ,
			      MHZDP(ca_soc->core_clk));
	if (ca_soc->eaxi_clk != 0)
		bp += sprintf(bp, "EAXI Clock: %u.%02u MHZ\n",
			      ca_soc->eaxi_clk / MHZ,
			      MHZDP(ca_soc->eaxi_clk));
	if (ca_soc->lsaxi_clk != 0)
		bp += sprintf(bp, "LSAXI Clock: %u.%02u MHZ\n",
			      ca_soc->lsaxi_clk / MHZ,
			      MHZDP(ca_soc->lsaxi_clk));
	if (ca_soc->hsaxi_clk != 0)
		bp += sprintf(bp, "HSAXI Clock: %u.%02u MHZ\n",
			      ca_soc->hsaxi_clk / MHZ,
			      MHZDP(ca_soc->hsaxi_clk));
	if (ca_soc->cci_clk != 0)
		bp += sprintf(bp, "CCI Clock: %u.%02u MHZ\n",
			      ca_soc->cci_clk / MHZ,
			      MHZDP(ca_soc->cci_clk));
	if (ca_soc->crypto_clk != 0)
		bp += sprintf(bp, "Crypto Clock: %u.%02u MHZ\n",
			      ca_soc->crypto_clk / MHZ,
			      MHZDP(ca_soc->crypto_clk));
	if (ca_soc->atb_clk != 0)
		bp += sprintf(bp, "ATB Clock: %u.%02u MHZ\n",
			      ca_soc->atb_clk / MHZ,
			      MHZDP(ca_soc->atb_clk));
	if (ca_soc->pe_clk != 0)
		bp += sprintf(bp, "PE Clock: %u.%02u MHZ\n",
			      ca_soc->pe_clk / MHZ,
			      MHZDP(ca_soc->pe_clk));
	if (ca_soc->peaxi_clk != 0)
		bp += sprintf(bp, "PE AXI Clock: %u.%02u MHZ\n",
			      ca_soc->peaxi_clk / MHZ,
			      MHZDP(ca_soc->peaxi_clk));
	if (ca_soc->per_clk != 0)
		bp += sprintf(bp, "PER Clock: %u.%02u MHZ\n",
			      ca_soc->per_clk / MHZ,
			      MHZDP(ca_soc->per_clk));
	if (ca_soc->io_clk != 0)
		bp += sprintf(bp, "I/O Clock: %u.%02u MHZ\n",
			      ca_soc->io_clk / MHZ,
			      MHZDP(ca_soc->io_clk));
	if (ca_soc->eth_ref2_clk != 0)
		bp += sprintf(bp, "Eth Ref 2 Clock: %u.%02u MHZ\n",
			      ca_soc->eth_ref2_clk / MHZ,
			      MHZDP(ca_soc->eth_ref2_clk));

	return bp - buf;
}

static DEVICE_ATTR_RO(data);

static struct attribute *ca_soc_attrs[] = {
	&dev_attr_data.attr,
	NULL
};

ATTRIBUTE_GROUPS(ca_soc);

static int ca_soc_device_init(struct platform_device *pdev, struct ca_socinfo *cs,
			      u32 jtag_id, char *name)
{
	struct soc_device_attribute *attr = &cs->attr;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	u32 val;

	np = of_find_node_by_path("/");
	of_property_read_string(np, "model", &attr->machine);
	of_node_put(np);
#ifdef CONFIG_SOC_BUS_EXTEND
	attr->vendor = devm_kasprintf(dev, GFP_KERNEL, "%s", "Cortina Access");
#endif
	attr->family = devm_kasprintf(dev, GFP_KERNEL, "%s", name);
	attr->revision = devm_kasprintf(dev, GFP_KERNEL, "%c", cs->soc_data.chip_revision);
	if (!nvmem_cell_read_u32(dev, "uuid", &val))
		attr->serial_number = devm_kasprintf(dev, GFP_KERNEL, "%08X", val);

	attr->soc_id = devm_kasprintf(dev, GFP_KERNEL, "%03X-%04X-0%X",
				      CA_SOC_VENDOR_ID(jtag_id),
				      CA_SOC_CHIP_ID(jtag_id),
				      CA_SOC_CHIP_REV(jtag_id));
	attr->data = &cs->soc_data;
	attr->custom_attr_group = ca_soc_groups[0];

	return 0;
}


static int ca_soc_clk_init(struct device_node *np, struct ca_soc_data *soc_data)
{
	struct clk *soc_clk;

	soc_clk = of_clk_get_by_name(np, "cpll");
	if (!IS_ERR(soc_clk)) {
		soc_data->cpll = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "epll");
	if (!IS_ERR(soc_clk)) {
		soc_data->epll = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "fpll");
	if (!IS_ERR(soc_clk)) {
		soc_data->fpll = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "cpu_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->cpu_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "cci_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->cci_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "lsaxi_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->lsaxi_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "hsaxi_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->hsaxi_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "core_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->core_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "crypto_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->crypto_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "eaxi_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->eaxi_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "atb_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->atb_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "pe_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->pe_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "peaxi_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->peaxi_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "per_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->per_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "io_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->io_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}
	soc_clk = of_clk_get_by_name(np, "eth_ref2_clk");
	if (!IS_ERR(soc_clk)) {
		soc_data->eth_ref2_clk = clk_get_rate(soc_clk);
		clk_put(soc_clk);
	}

	return 0;
}

struct rom_version hgu_pon_rev_info[] = {
	{'C', 0xf3954080, "v1.3(release):957a6b"},	/* Saturn REV-C */
	{'B', 0xf39539c0, "v1.3(release):be66b2"}	/* Saturn REV-B */
};

struct rom_version saturn_pon_rev_info[] = {
	{'C', 0x42054080, "v1.3(release):957a6b"},	/* Saturn REV-C */
	{'B', 0x420539c0, "v1.3(release):be66b2"}	/* Saturn REV-B */
};

unsigned char get_venus_revision(struct ca_soc_ext *soc_ext)
{
	unsigned int revision = 'B';

#ifdef	CONFIG_CORTINA_ACCESS_SMCC
	/* Since OTP/ROM memory are secure-only, we need go through SMC to
	 * get revision.
	 */
	 revision = CA_SMC_CALL_ROM_VERSION();
#else
	unsigned int *rom_base;
	rom_base = ioremap(soc_ext->rom_addr, 4);
	if (!rom_base) {
		pr_err("Unable to map ROM area!\n");
		return 'A';
	}
	revision = *rom_base;
#endif

	if (revision == 0)
		return 'A';
	else
		return revision - 0x0B + 'B';

	return (unsigned char)revision;
}

unsigned char get_saturn_revision(struct ca_soc_ext *soc_ext)
{
	int i, entry_num;
	struct rom_version *asic_rom_ver;
	unsigned char *rom_base, revision;

	/* Chip revision locates at end of ROM after REV-D */
	rom_base = ioremap(soc_ext->rom_addr, 4);
	if (!rom_base) {
		pr_err("Unable to Map SSB area!\n");
		return 'C';
	}
	if (*rom_base != 0) {
		revision = 'D' + *rom_base - 0xd;
		iounmap(rom_base);
		pr_err("Saturn revision=%x\n", revision);
		return revision;
	}

	/* Compare string */
	entry_num = sizeof(saturn_pon_rev_info) / sizeof(struct rom_version);
	for (i = 0; i < entry_num; i++) {
		asic_rom_ver = &soc_ext->pon_str[i];
		rom_base = ioremap(asic_rom_ver->ver_str_addr, VER_STR_LEN);
		if (!rom_base) {
			pr_err("Unable to Map SSB area!\n");
			return 'C';
		}
		if (memcmp(rom_base, asic_rom_ver->ver_str, VER_STR_LEN) == 0) {
			iounmap(rom_base);
			return asic_rom_ver->tapeout_version;
		}
		iounmap(rom_base);
	}

	pr_info("Default C\n");
	return 'C';
}

static int ca_soc_data_init(struct platform_device *pdev, struct ca_socinfo *cs,
			    u32 jtag_id)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct ca_soc_ext *soc_ext = cs->soc_ext;
	struct ca_soc_data *soc_data = &cs->soc_data;

	soc_data->vendor_id = CA_SOC_VENDOR_ID(jtag_id);
	soc_data->chip_id = CA_SOC_CHIP_ID(jtag_id);
	soc_data->chip_revision = CA_SOC_CHIP_REV(jtag_id);
	soc_data->part_no = CA_SMC_CALL_CHIP_ID();

	if (soc_ext->flag & USB_ID_REV) {
		void __iomem *usb2_id_base = of_iomap(np, 1);

		if (!usb2_id_base) {
			dev_err(dev, "cannot ioremap resource USB2_ID\n");
			return -ENODEV;
		}

		if (readl(usb2_id_base) == 0x01100020)	/* Synopsis vendor ID */
			soc_data->chip_revision = 'D';

		iounmap(usb2_id_base);
	}

	if (soc_ext->flag & PON_REV) {
		soc_data->pon_revision = get_saturn_revision(soc_ext);

		if (soc_ext->flag & PON_CHIP_REV)
			soc_data->chip_revision = soc_data->pon_revision;
	}

	if (soc_ext->flag & VENUS_REV)
		soc_data->chip_revision = get_venus_revision(soc_ext);

	ca_soc_clk_init(np, soc_data);

	return 0;
}

static char *ca_soc_find(u32 jtag_id)
{
	if (CA_SOC_VENDOR_ID(jtag_id) != VID_CORTINA_ACCESS)
		return NULL;

	/* check CHIP_ID */
	switch (CA_SOC_CHIP_ID(jtag_id)) {
	case CHIP_CA7742:	return CA7742_NAME;
	case CHIP_CA8279:	return CA8279_NAME;
	case CHIP_CA8271:	return CA8271_NAME;
	case CHIP_CA8271S:	return CA8271S_NAME;
	case CHIP_CA8289:	return CA8289_NAME;
	case CHIP_CA8271N:	return CA8271N_NAME;
	case CHIP_CA8271NS:	return CA8271NS_NAME;
	case CHIP_CA8272NI:	return CA8272NI_NAME;
	case CHIP_CA8299:	return CA8299_NAME;
	case CHIP_CA8299S:	return CA8299S_NAME;
	default:		return NULL;
	}
}

static int ca_socinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ca_socinfo *cs;
	struct ca_soc_ext *cse;
	struct resource *res;

	u32 jtag_id;
	char *name;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	cse = (struct ca_soc_ext *)of_device_get_match_data(dev);
	if (IS_ERR(cse)) {
		dev_err(dev, "missing private data\n");
		return -ENODEV;
	}
	cs->soc_ext = cse;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "JTAG_ID");
	jtag_base = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(jtag_base))
		return PTR_ERR(jtag_base);

	jtag_id = readl(jtag_base);
	name = ca_soc_find(jtag_id);
	if (!name)
		return -ENODEV;

	ca_soc_data_init(pdev, cs, jtag_id);
	ca_soc_device_init(pdev, cs, jtag_id, name);

	cs->soc_dev = soc_device_register(&cs->attr);
	if (IS_ERR(cs->soc_dev))
		return PTR_ERR(cs->soc_dev);

	platform_set_drvdata(pdev, cs);

	ca_soc_data_dump(pdev, cs);

	/* struct soc_device is not defined in header file. There is no way to
	 * get data of attr. Use global ca_soc to keeps the value.
	 */
	ca_soc = &cs->soc_data;

	ca_proc_init();

	return 0;
}

static int ca_socinfo_remove(struct platform_device *pdev)
{
	struct ca_socinfo *cs = platform_get_drvdata(pdev);

	soc_device_unregister(cs->soc_dev);

	return 0;
}

static const struct ca_soc_ext soc_exts[] = {
	{ .flag = USB_ID_REV,			/* G3 */
	  .rom_addr = 0,
	  .pon_str = NULL },
	{ .flag = USB_ID_REV | PON_REV,		/* G3HGU */
	  .rom_addr = 0xf395fffc,
	  .pon_str = hgu_pon_rev_info },
	{ .flag = PON_REV | PON_CHIP_REV,	/* Saturn series */
	  .rom_addr = 0x4205fffc,
	  .pon_str = saturn_pon_rev_info },
	{ .flag = VENUS_REV,			/* VENUS */
	  .rom_addr = 0xfffdfffc,
	  .pon_str = NULL },
	{ .flag = 0,				/* MERCURY */
	  .rom_addr = 0,
	  .pon_str = NULL },
	{ /* sentinel */ },
};

static const struct of_device_id ca_soc_match[] = {
	{ .compatible = "cortina-access,g3",
	  .data = (const void *)&soc_exts[0] },
	{ .compatible = "cortina-access,g3hgu",
	  .data = (const void *)&soc_exts[1] },
	{ .compatible = "cortina-access,saturn-sfu",
	  .data = (const void *)&soc_exts[2] },
	{ .compatible = "cortina-access,venus",
	  .data = (const void *)&soc_exts[3] },
	{ .compatible = "cortina-access,mercury",
	  .data = (const void *)&soc_exts[4] },
	{ },
};
MODULE_DEVICE_TABLE(of, ca_soc_match);

static struct platform_driver ca_socinfo_driver = {
	.probe = ca_socinfo_probe,
	.remove = ca_socinfo_remove,
	.driver  = {
		.name = "ca-socinfo",
		.of_match_table = ca_soc_match,
	},
};

static int __init ca_socinfo_init(void)
{
	int ret;

	ret = platform_driver_register(&ca_socinfo_driver);
	if (ret) {
		pr_err("Failed to register SoC driver\n");
		return ret;
	}

	return 0;
}

static void __exit ca_socinfo_exit(void)
{
	return platform_driver_unregister(&ca_socinfo_driver);
}

subsys_initcall_sync(ca_socinfo_init);
module_exit(ca_socinfo_exit);

MODULE_DESCRIPTION("Cortina-Access SoC Info driver");
MODULE_LICENSE("GPL v2");
