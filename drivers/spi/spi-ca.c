// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Cortina Access SPI Controllers
 *
 * This driver is inspired by:
 * spi-bcm2835.c, Copyright (C) 2015 Martin Sperl
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/spi/spi.h>
#include <linux/iopoll.h>

#define REG_CLK_LOAD_MASK	0x0000FFFF

/* Bitfields in REG_CFG */
#define REG_CFG_CC		0x00000001
#define REG_CFG_WR		0x00000002
#define REG_CFG_MW_OFF		2
#define REG_CFG_SS_OFF		8
#define REG_CFG_SC_OFF		16
#define REG_CFG_PSD_OFF		24

/* Bitfields in REG_MODE */
#define REG_MODE_CPOL		0x00000001
#define REG_MODE_CPHA		0x00000002
#define REG_MODE_CMDS		0x00000004
#define REG_MODE_ISAM		0x00000008

/* Bitfields in REG_CTRL */
#define REG_CTRL_DONE		0x00000001
#define REG_CTRL_START		0x00000080

/* Bitfields in REG_IE/REG_INT */
#define REG_INT_INT		0x00000001

#define CA_SPI_FIFO_SIZE	8
#define CA_SPI_NUM_CS		8  /* raise as necessary */
#define CA_SPI_MODE_BITS	(SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_NO_CS)

#define CA_CTRL_MODE_ISAM	BIT(0)
#define CA_CTRL_CS_WORD		BIT(1)

/* counter_load must be 1 ~ 65535 in register REG_CLK
 * target SPI clock = PER_CLK / (2 * ( counter_load + 1 ))
 */
#define CA_SPI_MIN_DIVISOR	4
#define CA_SPI_MAX_DIVISOR	131072

#define DRV_NAME	"spi-ca"

/* define polling limits */
static unsigned int polling_limit_us = 30;
module_param(polling_limit_us, uint, 0664);
MODULE_PARM_DESC(polling_limit_us,
		 "time in us to run a transfer in polling mode\n");

/* Register Definitions */
enum ca_spi_registers {
	REG_CLK,
	REG_CFG,
	REG_CS_CFG,  /* Only for rtl9617d */
	REG_MODE,
	REG_CTRL,
	REG_CA0,
	REG_CA1,
	REG_CA2,
	REG_WDAT1,
	REG_WDAT0,
	REG_RDAT1,
	REG_RDAT0,
	REG_IE0,
	REG_INT0,
	REG_IE1,
	REG_INT1,
	REG_STAT,
	REG_MAX
};

struct ca_spi_data {
	u8 regs[REG_MAX];	/* Register offsets */
	u8 hw_cs_num;
	u8 cfg_cs_cnt;		/* cs in REG_CFG */
	struct {
		u8 ss_offset;
		u8 mw_offset;
	} cs_cfg;
};

struct cfg_mode {
	u32 clk;	/* for sel_ssp_cs_extend/micro_wire_cs_sel_extend in REG_CLK */
	u32 cfg;
	u32 cs_cfg;
	u32 mode;

	u32 spi_mode;
	u32 spi_speed;
	u32 spi_ctrl_data;
};

struct cortina_spi_cdata {
	bool isam_mode;
	bool cs_word;
};

/**
 * struct ca_spi - Cortina Access SPI controller
 * @regs: base address of register map
 * @clk: core clock, divided to calculate serial clock
 * @clk_hz: core clock cached speed
 * @irq: interrupt, signals TX FIFO empty or RX FIFO ? full
 * @ctlr: SPI controller reverse lookup
 * @spi: pointer handling spi device for debug
 * @xfer: pointer handling spi transfer for debug
 * @tx_buf: pointer whence next transmitted byte is read
 * @rx_buf: pointer where next received byte is written
 * @tx_len: remaining bytes to transmit
 * @rx_len: remaining bytes to receive
 * @prepare_cm: precalculated CFG/MODE registers value for ->prepare_message()
 * @debugfs_dir: the debugfs directory - neede to remove debugfs when
 *      unloading the module
 * @count_transfer_polling: count of how often polling mode is used
 * @count_transfer_irq: count of how often interrupt mode is used
 * @count_transfer_atomic: count of how often atomic mode is used
 * @hw_data:
 * @chip_select: SPI target currently selected
 */
struct ca_spi {
	void __iomem *regs;
	struct clk *clk;
	unsigned long clk_hz;
	int irq;
	struct spi_controller *ctlr;
	struct spi_device *spi;
	struct spi_transfer *xfer;
	const u8 *tx_buf;
	u8 *rx_buf;
	int tx_len;
	int rx_len;
	struct cfg_mode prepare_cm[CA_SPI_NUM_CS];

	struct dentry *debugfs_dir;
	u64 count_transfer_polling;
	u64 count_transfer_irq;
	u64 count_transfer_atomic;
	u32 effective_speed_hz;

	const struct ca_spi_data *hw_data;
	bool polling;
};

static inline int __ca_spi_setup(struct spi_device *spi);

#if defined(CONFIG_DEBUG_FS)
static void ca_debugfs_create(struct ca_spi *cs,
			      const char *dname)
{
	char name[64];
	struct dentry *dir;

	/* get full name */
	snprintf(name, sizeof(name), "spi-ca-%s", dname);

	/* the base directory */
	dir = debugfs_create_dir(name, NULL);
	cs->debugfs_dir = dir;

	/* the counters */
	debugfs_create_u64("count_transfer_polling", 0444, dir,
			   &cs->count_transfer_polling);
	debugfs_create_u64("count_transfer_irq", 0444, dir,
			   &cs->count_transfer_irq);
	debugfs_create_u64("count_transfer_atomic", 0444, dir,
			   &cs->count_transfer_atomic);
	debugfs_create_u32("effective_speed_hz", 0444, dir,
			   &cs->effective_speed_hz);
}

static void ca_debugfs_remove(struct ca_spi *cs)
{
	debugfs_remove_recursive(cs->debugfs_dir);
	cs->debugfs_dir = NULL;
}
#else
static void ca_debugfs_create(struct ca_spi *cs,
			      const char *dname)
{
}

static void ca_debugfs_remove(struct ca_spi *cs)
{
}
#endif /* CONFIG_DEBUG_FS */

static inline u32 ca_rd(struct ca_spi *cs, enum ca_spi_registers reg)
{
	u32 val = readl(cs->regs + cs->hw_data->regs[reg]);

	dev_dbg(&cs->ctlr->dev, "R, reg=0x%02x, val=0x%08x\n",
		cs->hw_data->regs[reg], val);

	return val;
}

static inline void ca_wr(struct ca_spi *cs, enum ca_spi_registers reg, u32 val)
{
	dev_dbg(&cs->ctlr->dev, "W, reg=0x%02x, val=0x%08x\n",
		cs->hw_data->regs[reg], val);

	writel(val, cs->regs + cs->hw_data->regs[reg]);
}

static inline void ca_flush_rx_fifo(struct ca_spi *cs)
{
	ca_wr(cs, REG_RDAT0, 0);
	ca_wr(cs, REG_RDAT1, 0);
}

static inline void ca_flush_tx_fifo(struct ca_spi *cs)
{
	ca_wr(cs, REG_CA0, 0);
	ca_wr(cs, REG_CA1, 0);
}

static inline void ca_rd_rx_fifo(struct ca_spi *cs)
{
	int rx_len;
	int loop, i;
	u32 val;

	if (!cs->rx_len)
		return;

	if (cs->prepare_cm[cs->spi->chip_select].spi_ctrl_data & CA_CTRL_CS_WORD)
		rx_len = cs->spi->bits_per_word / 8;
	else
		rx_len = min(cs->rx_len, CA_SPI_FIFO_SIZE);

	if (!cs->rx_buf) {
		cs->rx_len -= rx_len;
		return;
	}

REMAINDER:
	val = ca_rd(cs, rx_len > 4 ? REG_RDAT1 : REG_RDAT0);
	loop = ((rx_len % 4) ? : 4) - 1;
	for (i = loop; i >= 0; i--)
		*cs->rx_buf++ = (u8)((val >> (i * 8)) & 0xFF);

	cs->rx_len -= (loop + 1);

	if (rx_len > 4) {
		rx_len = 4;
		goto REMAINDER;
	}
}

static inline void ca_wr_tx_fifo(struct ca_spi *cs)
{
	int reg[3] = {REG_CA0, REG_CA1, REG_CA2};
	int tx_len;
	u32 val;
	int i;

	if (!cs->tx_len)
		return;

	if (cs->prepare_cm[cs->spi->chip_select].spi_ctrl_data & CA_CTRL_CS_WORD)
		tx_len = cs->spi->bits_per_word / 8;
	else
		tx_len = min(cs->tx_len, CA_SPI_FIFO_SIZE);

	if (!cs->tx_buf) {
		cs->tx_len -= tx_len;
		return;
	}

	for (i = 0; i < tx_len; i++) {
		if ((i % 4) == 0)
			val = 0;

		val |= ((u32)(*cs->tx_buf++)) << (24 - 8 * (i % 4));

		if ((i & 3) == 3)
			ca_wr(cs, reg[i / 4], val);
	}
	if (i % 4)
		ca_wr(cs, reg[i / 4], val);

	cs->tx_len -= tx_len;
}

static void ca_spi_reset_hw(struct ca_spi *cs)
{
	ca_wr(cs, REG_IE0, 0);
	if (ca_rd(cs, REG_INT0))
		ca_wr(cs, REG_INT0, REG_INT_INT);

	ca_flush_rx_fifo(cs);
	ca_flush_tx_fifo(cs);
}

static irqreturn_t ca_spi_interrupt(int irq, void *dev_id)
{
	struct ca_spi *cs = dev_id;
	struct spi_device *spi = cs->spi;
	struct spi_transfer *xfer = cs->xfer;
	int len = xfer->len;
	u32 val = ca_rd(cs, REG_INT0);

	if (!(val & REG_INT_INT))
		return IRQ_NONE;

	ca_wr(cs, REG_CTRL, REG_CTRL_DONE);
	ca_wr(cs, REG_INT0, REG_INT_INT);

	ca_rd_rx_fifo(cs);

	if (cs->rx_len) {
		ca_wr_tx_fifo(cs);
		ca_wr(cs, REG_CTRL, REG_CTRL_START);
	} else {
		ca_spi_reset_hw(cs);

		if (xfer->tx_buf && xfer->rx_buf) {
			dev_dbg(&spi->dev, "len=%d tx=[%*phD] rx=[%*phD]\n",
				len, len, xfer->tx_buf, len, xfer->rx_buf);
		} else if (xfer->tx_buf) {
			dev_dbg(&spi->dev, "len=%d tx=[%*phD]\n",
				len, len, xfer->tx_buf);
		} else if (xfer->rx_buf) {
			dev_dbg(&spi->dev, "len=%d rx=[%*phD]\n",
				len, len, xfer->rx_buf);
		}

		spi_finalize_current_transfer(cs->ctlr);
	}

	return IRQ_HANDLED;
}

static int ca_spi_transfer_one_irq(struct spi_controller *ctlr,
				   struct spi_device *spi,
				   struct spi_transfer *xfer)
{
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);

	/* update usage statistics */
	cs->count_transfer_irq++;

	ca_wr_tx_fifo(cs);

	ca_wr(cs, REG_IE0, REG_INT_INT);

	ca_wr(cs, REG_CTRL, REG_CTRL_START);

	/* signal that we need to wait for completion */
	return 1;
}

static int ca_spi_transfer_one_poll(struct spi_controller *ctlr,
				    struct spi_device *spi,
				    struct spi_transfer *xfer)
{
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);
	unsigned long timeout_us = spi->rt ? 2000 : 22000;
	int len = xfer->len;
	u32 reg;
	int ret;

	/* update usage statistics */
	if (spi->rt)
		cs->count_transfer_atomic++;
	else
		cs->count_transfer_polling++;

	while (cs->rx_len) {
		if (spi->rt && spi->cs_gpiod)
			gpiod_set_value(spi->cs_gpiod, 1);

		ca_wr_tx_fifo(cs);

		ca_wr(cs, REG_CTRL, REG_CTRL_START);

		if (spi->rt) {
			ret = read_poll_timeout_atomic(ca_rd, reg, (reg & 1), 1, timeout_us,
						       false, cs, REG_CTRL);
		} else {
			ret = read_poll_timeout(ca_rd, reg, (reg & 1), 3, timeout_us,
						false, cs, REG_CTRL);
		}
		if (ret) {
			dev_err(&spi->dev, "poll timeout %lu\n", timeout_us);
			return ret;
		}

		ca_wr(cs, REG_CTRL, REG_CTRL_DONE);

		ca_rd_rx_fifo(cs);

		if (spi->rt && spi->cs_gpiod)
			gpiod_set_value(spi->cs_gpiod, 0);
	}

	ca_spi_reset_hw(cs);

	if (xfer->tx_buf && xfer->rx_buf) {
		dev_dbg(&spi->dev, "len=%d tx=[%*phD] rx=[%*phD]\n",
			len, len, xfer->tx_buf, len, xfer->rx_buf);
	} else if (xfer->tx_buf) {
		dev_dbg(&spi->dev, "len=%d tx=[%*phD]\n",
			len, len, xfer->tx_buf);
	} else if (xfer->rx_buf) {
		dev_dbg(&spi->dev, "len=%d rx=[%*phD]\n",
			len, len, xfer->rx_buf);
	}

	return 0;
}

static int ca_spi_transfer_one(struct spi_controller *ctlr,
			       struct spi_device *spi,
			       struct spi_transfer *xfer)
{
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);
	unsigned long cdiv;
	u32 bits_cnt;

	if (xfer->speed_hz != cs->prepare_cm[spi->chip_select].spi_speed) {
		u32 desired_divisor = DIV_ROUND_UP(cs->clk_hz, xfer->speed_hz);

		if (desired_divisor < CA_SPI_MIN_DIVISOR) {
			cdiv = CA_SPI_MIN_DIVISOR;
		} else {
			cdiv = desired_divisor;

			if (cdiv % 2)
				cdiv++;
			if (cdiv > CA_SPI_MAX_DIVISOR)
				cdiv = CA_SPI_MAX_DIVISOR;
		}

		cs->prepare_cm[spi->chip_select].clk &= ~REG_CLK_LOAD_MASK;
		cs->prepare_cm[spi->chip_select].clk += cdiv / 2 - 1;
		ca_wr(cs, REG_CLK, cs->prepare_cm[spi->chip_select].clk);

		cs->prepare_cm[spi->chip_select].spi_speed = xfer->speed_hz;
	} else {
		cdiv = ((cs->prepare_cm[spi->chip_select].clk & REG_CLK_LOAD_MASK) + 1) * 2;
	}
	xfer->effective_speed_hz = cs->clk_hz / cdiv;

	if (cs->prepare_cm[spi->chip_select].spi_ctrl_data & CA_CTRL_CS_WORD) {
		bits_cnt = xfer->bits_per_word - 1;
	} else {
		if (xfer->len >= CA_SPI_FIFO_SIZE)
			bits_cnt = CA_SPI_FIFO_SIZE * 8 - 1;
		else
			bits_cnt = xfer->len * 8 - 1;
	}
	ca_wr(cs, REG_CFG, cs->prepare_cm[spi->chip_select].cfg | (bits_cnt << REG_CFG_SC_OFF));

	cs->spi = spi;
	cs->xfer = xfer;
	cs->tx_buf = xfer->tx_buf;
	cs->rx_buf = xfer->rx_buf;
	cs->tx_len = xfer->len;
	cs->rx_len = xfer->len;
	cs->effective_speed_hz = xfer->effective_speed_hz;

	if (cs->polling || spi->rt)
		return ca_spi_transfer_one_poll(ctlr, spi, xfer);

	return ca_spi_transfer_one_irq(ctlr, spi, xfer);
}

static int ca_spi_prepare_message(struct spi_controller *ctlr,
				  struct spi_message *msg)
{
	struct spi_device *spi = msg->spi;
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);
	struct spi_transfer *first_xfer;
	u64 access_time;

	if (spi->mode != cs->prepare_cm[spi->chip_select].spi_mode ||
	    spi->max_speed_hz != cs->prepare_cm[spi->chip_select].spi_speed) {
		if (__ca_spi_setup(spi))
			return -EINVAL;
	}

	ca_wr(cs, REG_CLK, cs->prepare_cm[spi->chip_select].clk);
	/* Not update REG_CFG, lack of cmd_cnt/dat_cnt */
	if (cs->hw_data->regs[REG_CS_CFG])
		ca_wr(cs, REG_CS_CFG, cs->prepare_cm[spi->chip_select].cs_cfg);
	ca_wr(cs, REG_MODE, cs->prepare_cm[spi->chip_select].mode);

	/* Determines the operation mode used for one message access. The operation
	 * mode remains unchanged to keep the design simple.
	 * Assume that the speed of each transfer is the same. We use the first one
	 * to calculate the whole access time. Each transfer takes extra 1 clock for
	 * cs start + stop.
	 * access time(us) = (msg->frame_length * 8 + 1) / speed_hz * 10^6
	 */
	first_xfer = list_first_entry(&msg->transfers, struct spi_transfer,
				      transfer_list);
	access_time = (msg->frame_length * 8 + 1) * 1000000;

	if (msg->frame_length > CA_SPI_FIFO_SIZE ||
	    access_time > (u64)polling_limit_us * first_xfer->speed_hz)
		cs->polling = false;
	else
		cs->polling = true;

	return 0;
}

/* CS Configuration */
static inline u32 __ca_spi_cfg_cs(struct ca_spi *cs, struct spi_device *spi)
{
	u32 val = 0;
	u8 cs_num = spi->chip_select;
	const struct ca_spi_data *hw = cs->hw_data;

	if (spi->mode & SPI_NO_CS)
		return 0;

	if (spi->cs_gpiod)
		return 0;

	if (hw->regs[REG_CS_CFG]) {
		if (spi->mode & SPI_CS_HIGH)
			val |= BIT(cs_num) << hw->cs_cfg.mw_offset;
		else
			val |= BIT(cs_num) << hw->cs_cfg.ss_offset;
	} else if (cs_num >= hw->cfg_cs_cnt) {
		u8 ext_cs = cs_num - hw->cfg_cs_cnt;

		if (spi->mode & SPI_CS_HIGH)
			val |= BIT(ext_cs) << hw->cs_cfg.mw_offset;
		else
			val |= BIT(ext_cs) << hw->cs_cfg.ss_offset;
	} else {
		if (spi->mode & SPI_CS_HIGH)
			val |= BIT(cs_num) << REG_CFG_MW_OFF;
		else
			val |= BIT(cs_num) << REG_CFG_SS_OFF;
	}

	if (cs->hw_data->regs[REG_CS_CFG])
		cs->prepare_cm[spi->chip_select].cs_cfg = val;
	else if (spi->chip_select >= cs->hw_data->cfg_cs_cnt)
		cs->prepare_cm[spi->chip_select].clk = val;
	else
		return val;

	return 0;
}


static struct cortina_spi_cdata *
cortina_spi_cdata_from_dt(struct spi_device *spi)
{
	struct cortina_spi_cdata *cdata;
	struct device_node *np;

	np = spi->dev.of_node;
	if (!np)
		return NULL;

	cdata = kzalloc(sizeof(*cdata), GFP_KERNEL);
	if (!cdata)
		return NULL;

	cdata->isam_mode = of_property_read_bool(np, "cortina-access,isam-mode");
	if (cdata->isam_mode)
		dev_info(&spi->dev, "ISAM mode enabled\n");

	cdata->cs_word = of_property_read_bool(np, "cortina-access,cs-word");
	if (cdata->cs_word)
		dev_info(&spi->dev, "CS word mode enabled\n");

	return cdata;
}

static inline int __ca_spi_setup(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);
	struct cortina_spi_cdata *cdata = spi->controller_data;
	unsigned long cdiv;
	u32 val, desired_divisor;
 
	/* chip select could cross registers must be set first */
	if (!cdata) {
		cdata = cortina_spi_cdata_from_dt(spi);
		spi->controller_data = cdata;
	}
	val = __ca_spi_cfg_cs(cs, spi);
	val |= REG_CFG_CC | REG_CFG_WR;
	cs->prepare_cm[spi->chip_select].cfg = val;

	desired_divisor = DIV_ROUND_UP(cs->clk_hz, spi->max_speed_hz);
	if (desired_divisor < CA_SPI_MIN_DIVISOR) {
		cdiv = CA_SPI_MIN_DIVISOR;
	} else {
		cdiv = desired_divisor;

		if (cdiv % 2)
			cdiv++;
		if (cdiv > CA_SPI_MAX_DIVISOR)
			cdiv = CA_SPI_MAX_DIVISOR;
	}
	cs->prepare_cm[spi->chip_select].clk &= ~REG_CLK_LOAD_MASK;
	cs->prepare_cm[spi->chip_select].clk += cdiv / 2 - 1;

	val = 0;
	if (spi->mode & SPI_CPOL)
		val |= REG_MODE_CPOL;
	if (spi->mode & SPI_CPHA)
		val |= REG_MODE_CPHA;
	if (cdata && cdata->isam_mode)
		val |= REG_MODE_ISAM;
	cs->prepare_cm[spi->chip_select].mode = val | REG_MODE_CMDS;

	cs->prepare_cm[spi->chip_select].spi_mode = spi->mode;
	cs->prepare_cm[spi->chip_select].spi_speed = spi->max_speed_hz;
	if (cdata && cdata->cs_word)
		cs->prepare_cm[spi->chip_select].spi_ctrl_data = CA_CTRL_CS_WORD;

	return 0;
}

static int ca_spi_setup(struct spi_device *spi)
{
	return __ca_spi_setup(spi);
}

static void _spi_transfer_delay_ns(u32 ns)
{
	if (!ns)
		return;
	if (ns <= NSEC_PER_USEC) {
		ndelay(ns);
	} else {
		u32 us = DIV_ROUND_UP(ns, NSEC_PER_USEC);

		udelay(us);
	}
}

static void cortina_spi_cleanup(struct spi_device *spi)
{
	struct cortina_spi_cdata *cdata = spi->controller_data;

	spi->controller_data = NULL;
	if (spi->dev.of_node)
		kfree(cdata);
}

extern int spi_queued_transfer(struct spi_device *spi, struct spi_message *msg);
static int ca_spi_transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct spi_transfer *xfer;
	int delay;
	int ret;

	/* SPI framework code path */
	if (!spi->rt)
		return spi_queued_transfer(spi, msg);

	spi->controller->cur_msg = msg;

	/* expand and simplify __spi_pump_messages() in spi.c to the
	 * minimum necessary operations with complex delay.
	 */
	ca_spi_prepare_message(spi->controller, msg);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		ret = ca_spi_transfer_one(spi->controller, spi, xfer);
		if (ret) {
			dev_err(&msg->spi->dev, "SPI transfer failed: %d\n", ret);
			break;
		}

		delay = spi_delay_to_ns(&xfer->delay, xfer);
		if (delay < 0)
			return delay;

		_spi_transfer_delay_ns(delay);
	}

	spi_finalize_current_message(spi->controller);
	spi->controller->cur_msg = NULL;

	msg->status = ret;
	if (msg->complete)
		msg->complete(msg->context);

	return ret;
}

static size_t ca_spi_max_transfer_size(struct spi_device *spi)
{
	if (spi->cs_gpiod || (spi->mode & SPI_NO_CS))
		return SIZE_MAX;
	else
		return CA_SPI_FIFO_SIZE;
}

static int ca_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct spi_controller *ctlr;
	struct ca_spi *cs;
	int err;

	ctlr = devm_spi_alloc_host(dev, sizeof(*cs));
	if (!ctlr)
		return -ENOMEM;

	platform_set_drvdata(pdev, ctlr);

	cs = spi_controller_get_devdata(ctlr);
	cs->ctlr = ctlr;
	cs->hw_data = of_device_get_match_data(dev);

	cs->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(cs->regs))
		return dev_err_probe(dev, PTR_ERR(cs->regs), "failed to map register memory\n");

	cs->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(cs->clk))
		return dev_err_probe(dev, PTR_ERR(cs->clk), "could not get clk\n");
	if (clk_get_rate(cs->clk) == 0) {
		dev_warn(dev, "core clock rate is zero\n");
		return -EINVAL;
	}

	cs->irq = platform_get_irq(pdev, 0);
	if (cs->irq <= 0)
		return cs->irq ? cs->irq : -ENODEV;

	err = clk_prepare_enable(cs->clk);
	if (err)
		return dev_err_probe(dev, err, "failed to enable clock\n");
	cs->clk_hz = clk_get_rate(cs->clk);

	err = devm_request_irq(dev, cs->irq, ca_spi_interrupt, 0,
			       dev_name(dev), cs);
	if (err) {
		dev_err_probe(dev, err, "could not request IRQ\n");
		goto out_clk_disable;
	}

	ctlr->dev.of_node = dev->of_node;
	ctlr->num_chipselect = CA_SPI_NUM_CS;	// supporting native + GPIO
	ctlr->mode_bits = CA_SPI_MODE_BITS;
	/* Now only support multiple 8-bits to simplify the driver implementation */
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16) |
				   SPI_BPW_MASK(24) | SPI_BPW_MASK(32);
	ctlr->max_speed_hz = cs->clk_hz / CA_SPI_MIN_DIVISOR;	/* necessary for __spi_validate() */
	ctlr->min_speed_hz = cs->clk_hz / CA_SPI_MAX_DIVISOR;
	ctlr->max_transfer_size = ca_spi_max_transfer_size;
	ctlr->setup = ca_spi_setup;
	ctlr->cleanup = cortina_spi_cleanup;
	ctlr->prepare_message = ca_spi_prepare_message;
	ctlr->transfer_one = ca_spi_transfer_one;
	ctlr->use_gpio_descriptors = true;
	ctlr->max_native_cs = cs->hw_data->hw_cs_num;

	err = spi_register_controller(ctlr);
	if (err) {
		dev_err_probe(dev, err, "could not register SPI controller");
		goto out_clk_disable;
	}
	/* to support spi_device::rt = 1 for transfer on atomic context */
	ctlr->transfer = ca_spi_transfer;

	ca_debugfs_create(cs, dev_name(dev));

	dev_info(dev, "Cortina Access SPI Controller driver(at %pr, irq %d, FIFOs %d)\n",
		 res, cs->irq, CA_SPI_FIFO_SIZE);

	return 0;

out_clk_disable:
	clk_disable_unprepare(cs->clk);

	return err;
}

static void ca_spi_remove(struct platform_device *pdev)
{
	struct spi_controller *ctlr = platform_get_drvdata(pdev);
	struct ca_spi *cs = spi_controller_get_devdata(ctlr);

	ca_debugfs_remove(cs);

	spi_unregister_controller(ctlr);

	ca_spi_reset_hw(cs);

	clk_disable_unprepare(cs->clk);
}

static const struct ca_spi_data venus_data = {
	.regs = {
		[REG_CLK]   = 0x00,
		[REG_CFG]   = 0x04,
		[REG_MODE]  = 0x08,
		[REG_CTRL]  = 0x0C,
		[REG_CA0]   = 0x10,
		[REG_CA1]   = 0x14,
		[REG_CA2]   = 0x18,
		[REG_WDAT1] = 0x1C,
		[REG_WDAT0] = 0x20,
		[REG_RDAT1] = 0x24,
		[REG_RDAT0] = 0x28,
		[REG_IE0]   = 0x2C,
		[REG_INT0]  = 0x30,
		[REG_IE1]   = 0x34,
		[REG_INT1]  = 0x38,
		[REG_STAT]  = 0x3C
	},
	.hw_cs_num = 5,
	.cfg_cs_cnt = 5,
};

/* Mercury (Extended CS in CFG) */
static const struct ca_spi_data mercury_data = {
	.regs = {
		[REG_CLK]   = 0x00,
		[REG_CFG]   = 0x04,
		[REG_MODE]  = 0x08,
		[REG_CTRL]  = 0x0C,
		[REG_CA0]   = 0x10,
		[REG_CA1]   = 0x14,
		[REG_CA2]   = 0x18,
		[REG_WDAT1] = 0x1C,
		[REG_WDAT0] = 0x20,
		[REG_RDAT1] = 0x24,
		[REG_RDAT0] = 0x28,
		[REG_IE0]   = 0x2C,
		[REG_INT0]  = 0x30,
		[REG_IE1]   = 0x34,
		[REG_INT1]  = 0x38,
		[REG_STAT]  = 0x3C
	},
	.hw_cs_num = 8,
	.cfg_cs_cnt = 5,
	.cs_cfg = {
		.ss_offset = 24,
		.mw_offset = 27
	}
};

static const struct of_device_id ca_spi_match[] = {
	{ .compatible = "cortina-access,venus-spi", .data = &venus_data},
	{ .compatible = "cortina-access,mercury-spi", .data = &mercury_data},
	{}
};
MODULE_DEVICE_TABLE(of, ca_spi_match);

static struct platform_driver ca_spi_driver = {
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= ca_spi_match,
	},
	.probe		= ca_spi_probe,
	.remove_new	= ca_spi_remove,
	.shutdown	= ca_spi_remove,
};
module_platform_driver(ca_spi_driver);

MODULE_DESCRIPTION("SPI controller driver for Cortina Access");
MODULE_LICENSE("GPL");
