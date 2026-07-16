/*
 * Realtek MMC/SD/SDIO driver
 *
 * Authors:
 * Copyright (C) 2008-2009 Realtek Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/mbus.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/reset.h>
#include <asm/unaligned.h>

#include <linux/sched.h>	//liao
#include <linux/wait.h>		//liao
#include <linux/slab.h>		//liao
#include <linux/semaphore.h>	//liao
#include <linux/mmc/card.h>	//liao
#include <linux/mmc/host.h>	//liao
#include <linux/mmc/mmc.h>	//liao
#include <linux/mmc/sd.h>	//liao
#include <linux/workqueue.h>	//liao
#include <linux/completion.h>	//liao
/*
#include <rbus/reg_mmc.h>               //liao
#include <rbus/reg_iso.h>               //liao
#include <rbus/reg_sys.h>               //liao
#include <mach/cpu.h>
*/
#include "rtkemmc.h"		//liao
#include "rtksd_ops.h"		//liao
#include "rtk_mmc_debug.h"	//liao
#include "../../core/card.h"
#include "../../core/core.h"
/*
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>

#ifdef CONFIG_REALTEK_PCBMGR
#include <mach/pcbMgr.h>
#ifdef CONFIG_REALTEK_GPIO
#include <mach/venus_gpio.h>
#define EMMC_SHOUTDOWN_PROTECT
#endif
#endif

#include <asm/system_info.h>
*/

#define DRIVER_NAME "rtk_emmc"
#define BANNER      "Realtek eMMC Driver"
#define VERSION     "$Id: rtkemmc.c Phoenix 2014-03-26 10:00 $"
//#define MAX_CMD_RETRY_COUNT 2
#define	RTK_97F_MMC_IRQ 22

#ifdef MONI_MEM_TRASH
#define MAX_BLK_NUM 0x01
#endif

#ifndef CONFIG_MMC_RTKEMMC_HK_ATTR
#define CONFIG_MMC_RTKEMMC_HK_ATTR
#endif

//TBD : if system & card enter suspend, resume can't wakeup correctly
//#define REAL_SUSPEND


int mmc_select_hs200(struct mmc_card *card);
//int mmc_select_ddr50(struct mmc_card *card);
//static int maxfreq = RTKSD_CLOCKRATE_MAX;
//static int nodma;
struct mmc_host *mmc_host_local = NULL;
static u32 rtk_emmc_bus_wid = 0;
volatile u8 g_cmd[6];
static volatile struct backupRegs gRegTbl;
static volatile int g_bResuming;
static int bSendCmd0 = 0;
volatile unsigned int gCurrentBootMode = MODE_SD20;
volatile unsigned int gPreventRetry = 0;
static void *pRSP = NULL;
static void *pRSP_org = NULL;
static volatile unsigned int g_crinit = 0;
static struct rw_semaphore cr_rw_sem;

static void rtksd_request(struct mmc_host *host, struct mmc_request *mrq);
static int rtksd_get_ro(struct mmc_host *mmc);
static void rtksd_set_ios(struct mmc_host *host, struct mmc_ios *ios);

static void set_cmd_info(struct mmc_card *card, struct mmc_command *cmd,
			 struct sd_cmd_pkt *cmd_info, u32 opcode, u32 arg,
			 u8 rsp_para);

static int rtksd_stop_transmission(struct mmc_card *card, int bIngore);
static int rtksd_send_status(struct mmc_card *card, u8 *state, u8 divider,
			     int bIgnore);
static int rtksd_wait_status(struct mmc_card *card, u8 state, u8 divider,
			     int bIgnore);

static void rtkcr_set_speed(struct rtksd_host *sdport, u8 level);
//static int mmc_ds_delay_tuning(struct rtksd_host *sdport);
static int mmc_tuning_sdr50(struct rtksd_host *sdport);
static int mmc_tuning_ddr50(struct rtksd_host *sdport);
static int mmc_tuning_hs200(struct rtksd_host *sdport);
static int mmc_tuning_hs400(struct rtksd_host *sdport);
static int rtksd_execute_tuning(struct mmc_host *host, u32 opcode);
static void rtksd_hs400_complete(struct mmc_host *host);
static void rtksd_hw_reset(struct rtksd_host *sdport);
static void rtk_emmc_hw_init(struct rtksd_host *host);
inline void rtk_emmc_get_clk_reg(struct rtksd_host *sdport);

typedef void (*set_gpio_func_t) (u32 gpio_num, u8 dir, u8 level);

static inline u32 rtl_reg_mask(void *addr, u32 mask, u32 new_val)
{
	u32 val;

	val = readl(addr);
	val &= ~mask;
	val |= new_val & mask;
	writel(val, addr);
	val = readl(addr);	/* flush write to the hardware */

	return val;
}

/*
static struct resource rtkemmc_resources[] = {
    [0] = {
        .start  = GET_MAPPED_RBUS_ADDR((u32)EM_BASE_ADDR),
        .end    = GET_MAPPED_RBUS_ADDR((u32)EM_BASE_ADDR + 0x200),
        .flags  = IORESOURCE_MEM,
    },
    [1] = {
        .start  = IRQ_CR,
        .end    = IRQ_CR,
        .flags  = IORESOURCE_IRQ,
    },
};
*/

static const struct mmc_host_ops rtkemmc_ops = {
	.request = rtksd_request,
	.get_ro = rtksd_get_ro,
	.set_ios = rtksd_set_ios,
	.execute_tuning = rtksd_execute_tuning,
	.hs400_complete = rtksd_hs400_complete,
};

#define UNSTUFF_BITS(resp,start,size)					\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})


static void rtksd_hold_card(struct rtksd_host *sdport)
{
	void *iobase = sdport->base;
	u32 loop = 1000;

	printk(KERN_ERR "%s(%u)", __func__, __LINE__);

	while (loop--) {
		//printk(KERN_ERR "wait eMMC END_STATE!!!\n");
		if (cr_readb(iobase + SD_TRANSFER) & END_STATE)
			break;
		rtkcr_mdelay(1);
	}

	//TBD, chk if we need to do sth related to CR GPIO
#if 0
	//pull low 10us eMMC reset
	rtkcr_set_mis_gpio(20, GPIO_OUT, GPIO_LOW);

	//keep DATA[7:0] floating
	rtkcr_set_mis_gpio(16, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(17, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(18, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(21, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(22, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(23, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(24, GPIO_IN, GPIO_LOW);
	rtkcr_set_mis_gpio(25, GPIO_IN, GPIO_LOW);

	//hold eMMC CMD high
	rtkcr_set_mis_gpio(7, GPIO_OUT, GPIO_HIGH);

	rtkcr_mdelay(1);
	//hold eMMC clock high
	rtkcr_set_mis_gpio(19, GPIO_OUT, GPIO_HIGH);

	rtkcr_mdelay(1);
	//pull high eMMC reset and keep
	rtkcr_set_mis_gpio(20, GPIO_OUT, GPIO_HIGH);
#endif
}

static void rtkemmc_set_crt_muxpad(struct rtksd_host *sdport)
{
	u32 reg_val = 0;

	MMCPRINTF("\n");

	//set default i/f to cr
	//reg_val = cr_readl(SYS_muxpad0);
	reg_val &= ~0xFFFF0FFF;
	reg_val |= 0xaaaa0aa8;
	//cr_writel(reg_val, SYS_muxpad0);
/*
    if (soc_is_rtk1195() && (realtek_rev() >= RTK1195_REV_B))
    {
	cr_writel(0xe0003, PLL_EMMC1); //LDO1.8v
	cr_writel(0x0, CR_PAD_CTL); //PAD to 1.8v
	printk(KERN_INFO "detect 1195 >= REV_B \n");
    }
*/
}

u32 swap_endian(u32 input)
{
	u32 output;
	output = (input & 0xff000000) >> 24 |
	    (input & 0x00ff0000) >> 8 |
	    (input & 0x0000ff00) << 8 | (input & 0x000000ff) << 24;
	return output;
}

/*
 * card power have ON/OFF item.
 * if want to power on card,the rtksd_host is necessary.
 * but power off is always working, no matter rtksd_host wether exist.
 */
//static void rtkcr_card_power(struct rtksd_host *sdport,u8 status)
//{
//    /* nothing to do for eMMC */
//}

static void rtksd_read_rsp(struct rtksd_host *sdport, u32 *rsp, int reg_count)
{
	void *iobase = sdport->base;

	MMCPRINTF("rsp addr=0x%p; reg_count=%u\n", rsp, reg_count);

	if (reg_count == 6) {
		rsp[0] = rsp[1] = 0;
		rsp[0] = cr_readb(iobase + SD_CMD1) << 24 |
		    cr_readb(iobase + SD_CMD2) << 16 |
		    cr_readb(iobase + SD_CMD3) << 8 |
		    cr_readb(iobase + SD_CMD4);
		//rsp[1] = cr_readb(iobase+SD_CMD4) << 24 |
		//         cr_readb(iobase+SD_CMD5) << 16;
		MMCPRINTF("rsp[0]=0x%08x, rsp[1]=0x%08x\n", rsp[0], rsp[1]);

	} else if (reg_count == 16) {
		rsp[0] = swap_endian(rsp[0]);
		rsp[1] = swap_endian(rsp[1]);
		rsp[2] = swap_endian(rsp[2]);
		rsp[3] = swap_endian(rsp[3]);
		MMCPRINTF
		    ("rsp[0]=0x%08x, rsp[1]=0x%08x, rsp[2]=0x%08x, rsp[3]=0x%08x\n",
		     rsp[0], rsp[1], rsp[2], rsp[3]);
	} else
		MMCPRINTF("rsp[0]=0x%08x\n", rsp[0]);
}

/*******************************************************
 *  *
 *   *******************************************************/
static void rtkcr_set_mode_selection(struct rtksd_host *sdport,
				     unsigned int set_bit)
{
	void *iobase = sdport->base;
	unsigned int tmp_bits;
	unsigned long flags;

	MMCPRINTF("%s: switch to 0x%08x\n", __func__, set_bit);
	gCurrentBootMode = set_bit;
	spin_lock_irqsave(&sdport->lock, flags);
	tmp_bits = cr_readb(iobase + SD_CONFIGURE1) & ~MASK_MODE_SELECT;
	cr_writeb((unsigned char)(tmp_bits | set_bit), iobase + SD_CONFIGURE1);
	//sync();
	spin_unlock_irqrestore(&sdport->lock, flags);
	MMCPRINTF(KERN_INFO
		  "%s: set mode to 0x%02x, SD_CONFIGURE1 = %02x, SD_CONFIGURE2 = 0x%02x, SD_CONFIGURE3 = 0x%02x\n",
		  DRIVER_NAME, tmp_bits | set_bit,
		  cr_readb(iobase + SD_CONFIGURE1),
		  cr_readb(iobase + SD_CONFIGURE2),
		  cr_readb(iobase + SD_CONFIGURE3));
}

static int rtkcr_get_mode_selection(struct rtksd_host *sdport)
{
	void *iobase = sdport->base;
	int tmp_bits;
	unsigned long flags;

	MMCPRINTF("%s - start\n", __func__);
	spin_lock_irqsave(&sdport->lock, flags);
	tmp_bits = cr_readb(iobase + SD_CONFIGURE1) & MASK_MODE_SELECT;
	//sync();
	spin_unlock_irqrestore(&sdport->lock, flags);
	MMCPRINTF("mmccr_set_mode_selection; switch to 0x%08x\n", tmp_bits);
	return tmp_bits;
}

#if 0 //FIXME unused
//rtkcr_set_ldo is useless in 97F, because the PLL is different
static void rtkcr_set_ldo(struct rtksd_host *sdport, u32 set_ldo)
{
	u32 tmp_val = 0;
	unsigned long flags;

	spin_lock_irqsave(&sdport->lock, flags);
	tmp_val = (cr_readl(SYS_PLL_EMMC3) & 0xffff) | (set_ldo << 16);
	cr_writel(tmp_val, SYS_PLL_EMMC3);
	sync();
	spin_unlock_irqrestore(&sdport->lock, flags);

	MMCPRINTF(KERN_INFO
		  "%s: set ldo to 0x%02x, PLL_EMM3 - 0x180001f8=%08x\n",
		  DRIVER_NAME, tmp_val, cr_readl(SYS_PLL_EMMC3));
}
#endif

void rtkcr_set_pad_driving(struct rtksd_host *sdport, unsigned int mode,
			   unsigned char CLOCK_DRIVING,
			   unsigned char CMD_DRIVING,
			   unsigned char DATA_DRIVING)
{
	static unsigned char card_pad_val = 0, cmd_pad_val = 0, data_pad_val =
	    0;

	switch (mode) {
	case MMC_IOS_GET_PAD_DRV:
		card_pad_val = cr_readb(sdport->base + EMMC_CARD_PAD_DRV);
		cmd_pad_val = cr_readb(sdport->base + EMMC_CMD_PAD_DRV);
		data_pad_val = cr_readb(sdport->base + EMMC_DATA_PAD_DRV);
		MMCPRINTF("backup card=0x%02x, cmd=0x%02x, data=0x%02x\n",
			  card_pad_val, cmd_pad_val, data_pad_val);
		MMCPRINTF("read : card=0x%02x, cmd=0x%02x, data=0x%02x\n",
			  cr_readb(sdport->base + EMMC_CARD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_CMD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_DATA_PAD_DRV));
		break;
	case MMC_IOS_SET_PAD_DRV:
		cr_writeb(CLOCK_DRIVING, sdport->base + EMMC_CARD_PAD_DRV);	//clock pad driving
		cr_writeb(CMD_DRIVING, sdport->base + EMMC_CMD_PAD_DRV);	//cmd pad driving
		cr_writeb(DATA_DRIVING, sdport->base + EMMC_DATA_PAD_DRV);	//data pads driving
		//sync();
		MMCPRINTF("set card=0x%02x, cmd=0x%02x, data=0x%02x\n",
			  cr_readb(sdport->base + EMMC_CARD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_CMD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_DATA_PAD_DRV));
		break;
	case MMC_IOS_RESTORE_PAD_DRV:
		cr_writeb(CLOCK_DRIVING, sdport->base + EMMC_CARD_PAD_DRV);	//clock pad driving
		cr_writeb(CMD_DRIVING, sdport->base + EMMC_CMD_PAD_DRV);	//cmd pad driving
		cr_writeb(DATA_DRIVING, sdport->base + EMMC_DATA_PAD_DRV);	//data pads driving
		//sync();
		MMCPRINTF("val card=0x%02x, cmd=0x%02x, data=0x%02x\n",
			  card_pad_val, cmd_pad_val, data_pad_val);
		MMCPRINTF("restore card=0x%02x, cmd=0x%02x, data=0x%02x\n",
			  cr_readb(sdport->base + EMMC_CARD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_CMD_PAD_DRV),
			  cr_readb(sdport->base + EMMC_DATA_PAD_DRV));
		break;
	}
	//sync();
	//mdelay(10);
}

static void rtkcr_set_div(struct rtksd_host *sdport, u32 set_div)
{
	void *iobase = sdport->base;
	u32 tmp_div;
	unsigned long flags;

	spin_lock_irqsave(&sdport->lock, flags);
	tmp_div = cr_readb(iobase + SD_CONFIGURE1) & ~MASK_CLOCK_DIV;
	cr_writeb(tmp_div | set_div, iobase + SD_CONFIGURE1);
	//sync();
	spin_unlock_irqrestore(&sdport->lock, flags);

	MMCPRINTF(KERN_INFO "%s: set div to 0x%02x, SD_CONFIGURE1 = %08x\n",
		  DRIVER_NAME, tmp_div | set_div,
		  cr_readb(iobase + SD_CONFIGURE1));
}

static void rtksd_set_bits(struct rtksd_host *sdport, u8 set_bit)
{
	void *iobase = sdport->base;
	u32 tmp_bits;
	unsigned long flags;

	spin_lock_irqsave(&sdport->lock, flags);
	tmp_bits = cr_readb(iobase + SD_CONFIGURE1);
	if ((tmp_bits & MASK_BUS_WIDTH) != set_bit) {
		tmp_bits &= ~MASK_BUS_WIDTH;
		MMCPRINTF("tmp_bits=0x%02x, set_bit=0x%02x\n", tmp_bits,
			  set_bit);
		cr_writeb(tmp_bits | set_bit, iobase + SD_CONFIGURE1);
	}
	//sync();
	spin_unlock_irqrestore(&sdport->lock, flags);
	MMCPRINTF(KERN_INFO "%s: change to %s mode, SD_CONFIGURE1 = 0x%02x\n",
		  DRIVER_NAME, bit_tlb[set_bit],
		  cr_readb(iobase + SD_CONFIGURE1));
}

static void rtkcr_set_speed(struct rtksd_host *sdport, u8 level)
{
	void *iobase = sdport->base;
	unsigned long flags;

	spin_lock_irqsave(&sdport->lock, flags);

	switch (level) {
	case 0:		//ddr52 , highest speed
		cr_writel(CLK_DIV_1 | CRC_CLK_VP0 | PUSH_CLK_SSC | SAMPLE_CLK_VP1
				,iobase + EMMC_CKGEN_CTL);
		break;
	case 1:
		cr_writel(CLK_DIV_2 | CRC_CLK_VP0 | PUSH_CLK_SSC | SAMPLE_CLK_VP1
				,iobase + EMMC_CKGEN_CTL);
		break;
	case 2:
		cr_writel(CLK_DIV_4 | CRC_CLK_VP0 | PUSH_CLK_SSC | SAMPLE_CLK_VP1
				,iobase + EMMC_CKGEN_CTL);
		break;
	default:
		cr_writel(CLK_DIV_8 | CRC_CLK_VP0 | PUSH_CLK_SSC | SAMPLE_CLK_VP1
				,iobase + EMMC_CKGEN_CTL);
		break;
	}
	//sync();
	spin_unlock_irqrestore(&sdport->lock, flags);

	MMCPRINTF(KERN_INFO "%s: change speed level to %d\n",
		  DRIVER_NAME, level);
}

static void rtkemmc_bus_speed_down(struct rtksd_host *sdport)
{
	cr_writeb(0x8, sdport->base + SD_SAMPLE_POINT_CTL);	//sample point = SDCLK / 4
	cr_writeb(0x10, sdport->base + SD_PUSH_POINT_CTL);	//output ahead SDCLK /4
}

u8 rtksd_get_rsp_len(u8 rsp_para)
{
	MMCPRINTF("\n");
	switch (rsp_para & 0x3) {
	case 0:
		return 0;
	case 1:
		return 6;
	case 2:
		return 16;
	default:
		return 0;
	}
}

static u32 rtksd_get_cmd_timeout(struct sd_cmd_pkt *cmd_info)
{
	struct rtksd_host *sdport = cmd_info->sdport;
	u16 block_count = cmd_info->block_count;
	u32 tmout = 0;

	MMCPRINTF("\n");
	if (cmd_info->cmd->data) {
		tmout = msecs_to_jiffies(200);
		if (block_count > 0x100) {
			tmout = tmout + msecs_to_jiffies(block_count >> 1);
		}
	} else
		tmout = msecs_to_jiffies(80);

#ifdef CONFIG_ANDROID
	tmout += msecs_to_jiffies(100);
#endif

	cmd_info->timeout = sdport->tmout = tmout;
	return 0;
}

#define SD_ALLOC_LENGTH     2048
static int rtksd_allocate_dma_buf(struct rtksd_host *sdport,
				  struct mmc_command *cmd)
{
	if (!pRSP)
		pRSP_org = pRSP =
		    dma_alloc_coherent(sdport->dev, SD_ALLOC_LENGTH,
				       &sdport->paddr, GFP_KERNEL);
	else
		return 0;

	if (!pRSP) {
		WARN_ON(1);
		cmd->error = -ENOMEM;
		return 0;
	}

	MMCPRINTF("allocate rtk dma buf : dma addr=0x%08x, phy addr=0x%08x\n",
		  pRSP, sdport->paddr);
	return 1;
}

static int rtksd_free_dma_buf(struct rtksd_host *sdport)
{
	if (pRSP_org)
		dma_free_coherent(sdport->dev, SD_ALLOC_LENGTH, pRSP_org,
				  sdport->paddr);
	else
		return 0;

	MMCPRINTF("free rtk dma buf \n");
	return 1;
}

#if 0 //FIXME unused
static void rtksd_get_next_block(void)
{
	if (pRSP_org) {
		if ((pRSP + 0x200) >= (pRSP_org + SD_ALLOC_LENGTH))
			pRSP = pRSP_org;
		else
			pRSP += 0x200;
	}
}
#endif

static unsigned char *rtksd_get_buffer_start_addr(void)
{
	if (pRSP_org)
		return pRSP_org;
	else
		return NULL;
}

static void rtksd_set_rspparam(struct rtksd_host *sdport,
			      struct sd_cmd_pkt *cmd_info)
{
	volatile char rsp_para1 = 0, rsp_para2 = 0, rsp_para3 = 0;
	unsigned char cfg3 = 0, cfg1 = 0;

	MMCPRINTF
	    ("rsp param reset : idx=0x%02x, sdport=0x%08x,cfg1=0x%02x,cfg2=0x%02x,cfg3=0x%02x\n",
	     cmd_info->cmd->opcode, sdport,
	     cr_readb(sdport->base + SD_CONFIGURE1),
	     cr_readb(sdport->base + SD_CONFIGURE2),
	     cr_readb(sdport->base + SD_CONFIGURE3));

	rsp_para1 = 0;
	rsp_para2 = 0;
	rsp_para3 = 0;
	cfg3 = cr_readb(sdport->base + SD_CONFIGURE3) | SD_CMD_RSP_TO;
	cfg1 =
	    cr_readb(sdport->base +
		     SD_CONFIGURE1) & (MASK_CLOCK_DIV | MASK_BUS_WIDTH |
				       MASK_MODE_SELECT);

	//correct emmc setting for rt1195
	switch (cmd_info->cmd->opcode) {
	case MMC_GO_IDLE_STATE:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 =
		    (IGN_WR_CRC_ERR_EN | CRC16_CAL_DIS) | SD_R0;
		cmd_info->rsp_para3 = 0;
		break;
	case MMC_SEND_OP_COND:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R3 | CRC7_CHK_DIS;
		cmd_info->rsp_para3 = 0;
		/* if (soc_is_rtk1195() && (realtek_rev() >= RTK1195_REV_B))
		   {
		   cmd_info->cmd->arg = MMC_SECTOR_ADDR|MMC_VDD_165_195;
		   }
		   else
		   {
		   cmd_info->cmd->arg = MMC_VDD_30_31|MMC_VDD_31_32|MMC_VDD_32_33|MMC_VDD_33_34|MMC_SECTOR_ADDR; //sector mode
		   } */
		//cmd_info->cmd->arg = MMC_VDD_30_31|MMC_VDD_31_32|MMC_VDD_32_33|MMC_VDD_33_34|MMC_SECTOR_ADDR; //sector mode
		cmd_info->cmd->arg = MMC_VDD_165_195 | MMC_VDD_30_31 | MMC_VDD_31_32 | MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_SECTOR_ADDR;	//sector mode
		break;
	case MMC_ALL_SEND_CID:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R2;
		//cmd_info->rsp_para3 = SD2_R0;
		cmd_info->rsp_para3 = cfg3;
		break;
	case MMC_SET_RELATIVE_ADDR:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R1 | CRC16_CAL_DIS;
		cmd_info->rsp_para3 = cfg3;
		cmd_info->cmd->arg = 0x10000;
		break;
	case MMC_SEND_CSD:
	case MMC_SEND_CID:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R2;
		cmd_info->rsp_para3 = cfg3;
		cmd_info->cmd->arg = 0x10000;
		break;
	case MMC_SEND_EXT_CSD:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R1;
		cmd_info->rsp_para3 = cfg3;
		break;
	case MMC_SLEEP_AWAKE:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->rsp_para2 = SD_R1b;
		cmd_info->rsp_para3 = SD_CMD_RSP_TO;
		cmd_info->cmd->arg = cmd_info->cmd->arg;
		printk(KERN_INFO "%s : cmd5 arg=0x%08x\n", __func__,
			cmd_info->cmd->arg);
		break;
	case MMC_SELECT_CARD:
		cmd_info->rsp_para1 = cfg1 | SD1_R0;
		cmd_info->cmd->arg = cmd_info->cmd->arg;
		if (cmd_info->cmd->flags == (MMC_RSP_NONE | MMC_CMD_AC)) {
			printk(KERN_INFO "%s : cmd7 with rsp none\n", __func__);
			cmd_info->rsp_para2 = RESP_TYPE_NON;
		} else {
			printk(KERN_INFO "%s : cmd7 with rsp\n", __func__);
			cmd_info->rsp_para2 = SD_R1b | CRC16_CAL_DIS;
		}
		cmd_info->rsp_para3 = -1;
		break;
	case MMC_SWITCH:
		cmd_info->rsp_para1 = cfg1;
		cmd_info->rsp_para2 = SD_R1 | CRC16_CAL_DIS;
		cmd_info->rsp_para3 = SD_CMD_RSP_TO;
		break;
	case MMC_SEND_STATUS:
		cmd_info->rsp_para1 = -1;
		cmd_info->rsp_para2 = SD_R1 | CRC16_CAL_DIS;
		cmd_info->rsp_para3 = SD_CMD_RSP_TO;
		cmd_info->cmd->arg = 0x10000;
		break;
	case MMC_STOP_TRANSMISSION:
		cmd_info->rsp_para1 = -1;
		cmd_info->rsp_para2 = SD_R1 | CRC16_CAL_DIS;
		cmd_info->rsp_para3 = -1;
		break;
	case MMC_READ_MULTIPLE_BLOCK:
		cmd_info->rsp_para1 = -1;
		cmd_info->rsp_para3 = (SD_CMD_RSP_TO | ADDR_BYTE_MODE);
		break;
	case MMC_WRITE_MULTIPLE_BLOCK:
		cmd_info->rsp_para1 = -1;
		cmd_info->rsp_para3 = (SD_CMD_RSP_TO | ADDR_BYTE_MODE);
		break;
	default:
		cmd_info->rsp_para1 = -1;	//don't update
		cmd_info->rsp_para3 = -1;	//don't update
		break;
	}
}

static int SD_SendCMDGetRSP_Cmd(struct sd_cmd_pkt *cmd_info, int bIgnore)
{
	volatile u8 cmd_idx = cmd_info->cmd->opcode;
	volatile u32 sd_arg;
	volatile s8 rsp_para1 = 0;
	volatile s8 rsp_para2 = 0;
	volatile s8 rsp_para3 = 0;
	u8 rsp_len = cmd_info->rsp_len;
	u32 *rsp = (u32 *) & cmd_info->cmd->resp;
	struct rtksd_host *sdport = cmd_info->sdport;
	struct mmc_host *host = sdport->mmc;
	void *iobase = sdport->base;
	int err, retry_count = 0;
	u32 byte_count = 0x200, block_count = 1, sa = 0;
	u32 buf_ptr = 0;
	u8 state = 0;

	rtksd_set_rspparam(sdport, cmd_info);	//for 119x
	sd_arg = cmd_info->cmd->arg;
	rsp_para1 = cmd_info->rsp_para1;
	rsp_para2 = cmd_info->rsp_para2;
	rsp_para3 = cmd_info->rsp_para3;

RET_CMD:

	if (rsp == NULL) {
		BUG_ON(1);
	}
	if ((g_crinit == 0) && (cmd_idx > MMC_SET_RELATIVE_ADDR)) {
		printk
		    ("%s: ignore cmd:0x%02x since we're still in emmc init stage\n",
		     DRIVER_NAME, cmd_idx);
		return CR_TRANSFER_FAIL;
	}

	if (rsp_para1 != -1)
		cr_writeb(rsp_para1, iobase + SD_CONFIGURE1);
	cr_writeb(rsp_para2, iobase + SD_CONFIGURE2);
	if (rsp_para3 != -1)
		cr_writeb(rsp_para3, iobase + SD_CONFIGURE3);
	g_cmd[0] = (0x40 | cmd_idx);
	g_cmd[1] = (sd_arg >> 24) & 0xff;
	g_cmd[2] = (sd_arg >> 16) & 0xff;
	g_cmd[3] = (sd_arg >> 8) & 0xff;
	g_cmd[4] = sd_arg & 0xff;
	g_cmd[5] = 0x00;

	cr_writeb((0x40 | cmd_idx), iobase + SD_CMD0);
	cr_writeb((sd_arg >> 24) & 0xff, iobase + SD_CMD1);
	cr_writeb((sd_arg >> 16) & 0xff, iobase + SD_CMD2);
	cr_writeb((sd_arg >> 8) & 0xff, iobase + SD_CMD3);
	cr_writeb(sd_arg & 0xff, iobase + SD_CMD4);
	cr_writeb(g_cmd[5], iobase + SD_CMD5);
	sdport->cmd_opcode = cmd_idx;
	//sync();
	if (bSendCmd0)
		pr_info("0 cmd0:0x%02x,cmd1:0x%02x,cmd2:0x%02x,cmd3:0x%02x,cmd4:0x%02x,cmd5:0x%02x\n",
		     readb(iobase + SD_CMD0), cr_readb(iobase + SD_CMD1),
		     readb(iobase + SD_CMD2), cr_readb(iobase + SD_CMD3),
		     readb(iobase + SD_CMD4), cr_readb(iobase + SD_CMD5));
	MMCPRINTF
	    ("Send  cmd0:0x%02x,cmd1:0x%02x,cmd2:0x%02x,cmd3:0x%02x,cmd4:0x%02x,cmd5:0x%02x\n",
	     cr_readb(iobase + SD_CMD0), cr_readb(iobase + SD_CMD1),
	     cr_readb(iobase + SD_CMD2), cr_readb(iobase + SD_CMD3),
	     cr_readb(iobase + SD_CMD4), cr_readb(iobase + SD_CMD5));
	rtksd_get_cmd_timeout(cmd_info);
#if 0
	if (sdport->rtflags & RTKCR_FOPEN_LOG) {
		pr_info("%s: cmd_idx=%u, rsp_addr=0x%08p\n \
		       sd_arg=0x%08x; rsp_para1=0x%02x rsp_para2=0x%02x\n \
		       rsp_para3=0x%02x rsp_len=0x%x cfg1=0x%02x,cfg2=0x%02x,cfg3=0x%02x\n",
		       __func__, cmd_idx, (uintptr_t)rsp, sd_arg, rsp_para1, rsp_para2,
		       rsp_para3, rsp_len,
		       readb(sdport->base + SD_CONFIGURE1),
		       readb(sdport->base + SD_CONFIGURE2),
		       readb(sdport->base + SD_CONFIGURE3));
	}
#endif
	if (RESP_TYPE_17B & rsp_para2) {
		//remap the resp dst buffer to un-cache
#if 1
		pRSP = rtksd_get_buffer_start_addr();
		buf_ptr = ((u32) sdport->paddr & ~0xff);
		sa = buf_ptr / 8;
		MMCPRINTF("cmdx tmp physical buf addr : 0x%08x\n",
			  sdport->paddr);
		MMCPRINTF("cmdx pRSP addr : 0x%08x\n", pRSP);
		MMCPRINTF("chg buf_ptr addr to : 0x%08x\n", buf_ptr);
		MMCPRINTF("final dma src addr : 0x%08x\n", sa);
#else
		sa = (u32) rsp / 8;
#endif

		MMCPRINTF("-----rsp 17B-----\n");
		MMCPRINTF("DMA_sa=0x%08x DMA_len=0x%08x, byte_cnt = 0x%x, blk_cnt = 0x%x\n", sa, 1,
				byte_count, block_count);
		cr_writeb(byte_count, iobase + SD_BYTE_CNT_L);	//0x24
		cr_writeb(byte_count >> 8, iobase + SD_BYTE_CNT_H);	//0x28
		cr_writeb(block_count, iobase + SD_BLOCK_CNT_L);	//0x2C
		cr_writeb(block_count >> 8, iobase + SD_BLOCK_CNT_H);	//0x30
		rtkemmc_set_dma(iobase, sa, 1, RSP17_SEL | DDR_WR | DMA_XFER);
		//rtksd_get_next_block();
	} else if (RESP_TYPE_6B & rsp_para2) {
		MMCPRINTF("-----rsp 6B-----\n");
		//cr_writel( 0x00, iobase+EMMC_CPU_ACC); //97F not support CPU direct access DMA
	}

	MMCPRINTF
	    ("1 cmd0:0x%02x,cmd1:0x%02x,cmd2:0x%02x,cmd3:0x%02x,cmd4:0x%02x,cmd5:0x%02x\n",
	     g_cmd[0], g_cmd[1], g_cmd[2], g_cmd[3], g_cmd[4], g_cmd[5]);

	//sync();
	err =
	    rtkcr_wait_opt_end(DRIVER_NAME, sdport, EMMC_SENDCMDGETRSP,
			       bIgnore);

	if (err == CR_TRANS_OK) {
		//sync();
		if (buf_ptr != 0) {
			//ignore start pattern
			pRSP = (void *)((uintptr_t) pRSP & ~0xff);
			pRSP++;
			rtksd_read_rsp(sdport, (u32 *) pRSP, rsp_len);
			if (cmd_idx == MMC_SEND_EXT_CSD)
				memcpy(rsp, (u32 *) pRSP, 16);
			else
				memcpy(rsp, (u32 *) pRSP, 16);
			memset(pRSP, 0, SD_ALLOC_LENGTH);
		} else
			rtksd_read_rsp(sdport, rsp, rsp_len);

		//sync();
		if (cmd_idx == MMC_SET_RELATIVE_ADDR) {
			g_crinit = 1;
			MMCPRINTF("emmc init done ...\n");
		}
		//get cmd7 status
		if ((cmd_info->cmd->flags == (MMC_RSP_NONE | MMC_CMD_AC))
		    && (cmd_idx == MMC_SELECT_CARD)) {
			printk(KERN_INFO "get status =>\n");
			rtksd_send_status(host->card, &state, 0, 0);
		}
	} else {
		if (!sdport->is_tuning) {
			pr_warn("%s: %s cmd trans fail, err=%d, ignore=%d, gPreventRetry=%d, gCurrentBootMode=%d, cmd_idx=%d\n",
					DRIVER_NAME, __func__, err, bIgnore, gPreventRetry, gCurrentBootMode, cmd_idx);
		}
#ifdef MMC_DEBUG
		printk("[LY]snd gCurrentBootMode =%d\n", gCurrentBootMode);
#endif
		if (gPreventRetry) {
#if 0
			if (!sdport->is_tuning) {
			pr_warn("[LY]error when card in uninit state, err=%d\n",
					gCurrentBootMode, err);
			}
#endif
			return err;
		}
		if (gCurrentBootMode >= MODE_DDR)
			return err;
		if (cmd_idx == MMC_SEND_STATUS)	//prevent dead lock looping
			return err;
		if (retry_count++ < MAX_CMD_RETRY_COUNT) {
			printk(KERN_WARNING "retry %d ---->\n", retry_count);
			err = error_handling(sdport, cmd_idx, bIgnore);
			goto RET_CMD;
		}
	}
	return err;
}

static int SD_SendCMDGetRSP(struct sd_cmd_pkt *cmd_info)
{
	int rc;
	MMCPRINTF("\n");

	rc = SD_SendCMDGetRSP_Cmd(cmd_info, 0);

	return rc;
}

static void duplicate_pkt(struct sd_cmd_pkt *sour, struct sd_cmd_pkt *dist)
{
	dist->sdport = sour->sdport;
	dist->cmd = sour->cmd;
	dist->data = sour->data;

	dist->dma_buffer = sour->dma_buffer;
	dist->byte_count = sour->byte_count;
	dist->block_count = sour->block_count;

	dist->flags = sour->flags;
	dist->rsp_para1 = sour->rsp_para1;
	dist->rsp_para2 = sour->rsp_para2;
	dist->rsp_para3 = sour->rsp_para3;
	dist->rsp_len = sour->rsp_len;
	dist->timeout = sour->timeout;
}

static int rtksd_err_handle(u16 cmdcode, struct sd_cmd_pkt *cmd_info)
{
	struct mmc_host *host = cmd_info->sdport->mmc;
	//struct rtksd_host *sdport   = cmd_info->sdport;
	//u8 cmd_idx                  = cmd_info->cmd->opcode;
	//u32 iobase                  = sdport->base;
	u8 state = 0;
	int err = 0;

	MMCPRINTF("(%s:%d) : cmd=0x%02x\n", __func__, __LINE__, cmdcode);
	if (host->card) {
		if (cmdcode == EMMC_AUTOWRITE2) {
			if (cmd_info->cmd->opcode == 18 ||
			    cmd_info->cmd->opcode == 25) {
				int stop_loop = 5;
				while (stop_loop--) {

					err =
					    rtksd_stop_transmission(host->card,
								    0);
					if (err) {
						mdelay(1);
						rtksd_send_status(host->card,
								  &state, 0, 0);
						if (state == STATE_TRAN)
							break;
					} else {
						break;
					}
				}
			}
		}
		MMCPRINTF("(%s:%d) - cmd=0x%02x, before polling TRAN state\n",
			  __func__, __LINE__, cmdcode);
		err = rtksd_wait_status(host->card, STATE_TRAN, 0, 0);
	}
	return err;
}

static int SD_Stream_Cmd34(u16 cmdcode, struct sd_cmd_pkt *cmd_info)
{
	u16 byte_count = cmd_info->byte_count;
	u16 block_count = cmd_info->block_count;
	void *data = cmd_info->dma_buffer;
	struct rtksd_host *sdport = cmd_info->sdport;
	void *iobase = sdport->base;
	int err;
	u32 cpu_mode = 0;
	u32 sa = 0;

	if (data == NULL)
		BUG_ON(1);

	if (cmdcode != EMMC_AUTOREAD3 && cmdcode != EMMC_AUTOREAD4 &&
	    cmdcode != EMMC_AUTOWRITE3 && cmdcode != EMMC_AUTOWRITE4) {
		BUG_ON(1);
	}

	sa = ((u32)(uintptr_t)data) / 8;

	if (cmd_info->cmd->data->flags & MMC_DATA_READ) {
		MMCPRINTF("-----mmc data ddr read-----\n");
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DDR_WR | DMA_XFER);
	} else if (cmd_info->cmd->data->flags & MMC_DATA_WRITE) {
		MMCPRINTF("-----mmc data write-----\n");
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DMA_XFER);
	} else {
		MMCPRINTF("-----mmc data sram read (cpu mode)-----\n");
		MMCPRINTF("CR_CPU_ACC(0x18012080) = 0x%x\n", CPU_MODE_EN);
		cpu_mode = 1;
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DDR_WR | DMA_XFER);
	}
	cr_writeb(byte_count, iobase + SD_BYTE_CNT_L);	//0x24
	cr_writeb(byte_count >> 8, iobase + SD_BYTE_CNT_H);	//0x28
	cr_writeb(block_count, iobase + SD_BLOCK_CNT_L);	//0x2C
	cr_writeb(block_count >> 8, iobase + SD_BLOCK_CNT_H);	//0x30

	sdport->cmd_opcode = 0xf1;
	rtksd_get_cmd_timeout(cmd_info);
	err = rtkcr_wait_opt_end(DRIVER_NAME, sdport, cmdcode, cpu_mode);
	if (err)
		mmcmsg3(KERN_WARNING "%s: %s fail\n", DRIVER_NAME, __func__);

	return err;
}

static int SD_Stream_Cmd(u16 cmdcode, struct sd_cmd_pkt *cmd_info,
			 unsigned int bIgnore)
{
	u8 cmd_idx = cmd_info->cmd->opcode;
	u32 sd_arg = cmd_info->cmd->arg;
	s8 rsp_para1 = cmd_info->rsp_para1;
	s8 rsp_para2 = cmd_info->rsp_para2;
	s8 rsp_para3 = cmd_info->rsp_para3;
	int rsp_len = cmd_info->rsp_len;
	u32 *rsp = (u32 *) & cmd_info->cmd->resp;
	u16 byte_count = cmd_info->byte_count;
	u16 block_count = cmd_info->block_count;
	struct rtksd_host *sdport = cmd_info->sdport;
	void *iobase = sdport->base;
	int err;
	u8 *data = cmd_info->dma_buffer;
	u32 cpu_mode = 0;
	u32 sa = 0, retry_count = 0;
#if 0
	if (sdport->rtflags & RTKCR_FOPEN_LOG) {
		pr_info("%s strm: cmd_idx=%u, rsp_addr=0x%08x\nsd_arg=0x%08x; rsp_para1=0x%02x \
			rsp_para2=0x%02x rsp_para3=0x%02x rsp_len=0x%x cfg1=0x%02x,cfg2=0x%02x,cfg3=0x%02x, DMA=0x%08x\n",
		       DRIVER_NAME, cmd_idx, rsp, sd_arg, rsp_para1, rsp_para2, rsp_para3, rsp_len, cr_readb(sdport->base + SD_CONFIGURE1),
		       cr_readb(sdport->base + SD_CONFIGURE2), cr_readb(sdport->base + SD_CONFIGURE3), cr_readb(sdport->base + EMMC_DMA_CTL1));
	}
#endif
	if (data == NULL) {
		BUG_ON(1);
	}
	if (rsp == NULL) {
		BUG_ON(1);
	}
#ifdef TEST_POWER_RESCYCLE
	cmd_info->sdport->test_count++;
	mmcspec("test_count=%d\n", cmd_info->sdport->test_count);
#endif
	if ((g_crinit == 0) && (cmd_idx > MMC_SET_RELATIVE_ADDR)) {
		printk
		    ("%s : ignore cmd:0x%02x since we're still in emmc init stage\n",
		     DRIVER_NAME, cmd_idx);
		return CR_TRANSFER_FAIL;
	}

	rtksd_set_rspparam(sdport, cmd_info);	//for 119x
	sd_arg = cmd_info->cmd->arg;
	if (rsp_para1 != -1)
		rsp_para1 = cmd_info->rsp_para1;
	rsp_para2 = cmd_info->rsp_para2;
	if (rsp_para3 != -1)
		rsp_para3 = cmd_info->rsp_para3;
	sa = ((u32)(uintptr_t)data) / 8;

	if ((cmdcode == EMMC_NORMALWRITE) || (cmdcode == EMMC_NORMALREAD)) {
		//rsp_para2|=CRC16_CAL_DIS;
		byte_count = 512;
	}
STR_CMD_RET:
	g_cmd[0] = (0x40 | cmd_idx);
	g_cmd[1] = (sd_arg >> 24) & 0xff;
	g_cmd[2] = (sd_arg >> 16) & 0xff;
	g_cmd[3] = (sd_arg >> 8) & 0xff;
	g_cmd[4] = sd_arg & 0xff;
	g_cmd[5] = 0x00;

	cr_writeb(g_cmd[0], iobase + SD_CMD0);	//0x10
	cr_writeb(g_cmd[1], iobase + SD_CMD1);	//0x14
	cr_writeb(g_cmd[2], iobase + SD_CMD2);	//0x18
	cr_writeb(g_cmd[3], iobase + SD_CMD3);	//0x1C
	cr_writeb(g_cmd[4], iobase + SD_CMD4);	//0x20
	cr_writeb(g_cmd[5], iobase + SD_CMD5);	//0x20

	if (rsp_para1 != -1) {
		MMCPRINTF("reset configure1 : 0x%02x\n", rsp_para1);
		cr_writeb(rsp_para1, iobase + SD_CONFIGURE1);	//0x0C
		MMCPRINTF("read configure1 : 0x%02x\n",
			  cr_readb(iobase + SD_CONFIGURE1));
	}
	cr_writeb(rsp_para2, iobase + SD_CONFIGURE2);	//0x0C
	if (rsp_para3 != -1) {
		MMCPRINTF("reset configure3 : 0x%02x\n", rsp_para3);
		cr_writeb(rsp_para3, iobase + SD_CONFIGURE3);	//0x0C
		MMCPRINTF("read configure3 : 0x%02x\n",
			  cr_readb(iobase + SD_CONFIGURE3));
	}
	cr_writeb(byte_count, iobase + SD_BYTE_CNT_L);	//0x24
	cr_writeb(byte_count >> 8, iobase + SD_BYTE_CNT_H);	//0x28
	cr_writeb(block_count, iobase + SD_BLOCK_CNT_L);	//0x2C
	cr_writeb(block_count >> 8, iobase + SD_BLOCK_CNT_H);	//0x30

	if (cmd_info->cmd->data->flags & MMC_DATA_READ) {
		MMCPRINTF("-----mmc data ddr read-----\n");
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DDR_WR | DMA_XFER);
	} else if (cmd_info->cmd->data->flags & MMC_DATA_TUNING) {
		MMCPRINTF("-----mmc data ddr tuning-----\n");
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DAT64_SEL | DDR_WR | DMA_XFER);
	} else if ((cmd_info->cmd->data->flags & MMC_DATA_WRITE)
		   || (cmd_info->cmd->data->flags & MMC_MICRON_60)) {
		MMCPRINTF("-----mmc data write-----\n");
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DMA_XFER);
	} else {
		MMCPRINTF("-----mmc data sram read (cpu mode)-----\n");
		MMCPRINTF("CR_CPU_ACC(0x18012080) = 0x%x\n", CPU_MODE_EN);

		cpu_mode = 1;
		rtkemmc_set_dma(iobase, (u32)sa, block_count, DDR_WR | DMA_XFER);
	}

	sdport->cmd_opcode = cmd_idx;

	rtksd_get_cmd_timeout(cmd_info);

	err = rtkcr_wait_opt_end(DRIVER_NAME, sdport, cmdcode, bIgnore);

#ifdef FORCE_CHG_SDR50_MODE
	//printk("hacker cmd_arg=%08x\n", cmd_arg);
	if (cmd_arg == 0x9999) {
		ret_err = -1;
	}
#endif

	if (err == RTK_SUCC) {
		if ((cmdcode == EMMC_AUTOREAD1) || (cmdcode == EMMC_AUTOWRITE1)) {
			MMCPRINTF("AUTO READ/WRITE 1 skip response~\n");
		} else {
			rtksd_read_rsp(sdport, rsp, rsp_len);
			MMCPRINTF("---stream cmd done---\n");
		}
#if 0
		if ((cmd_idx > 17) && (!bIgnore))
			BUG();
#endif
	} else {
		MMCPRINTF("strm cmd_idx=%d,ret_err=%d,bIgnore=%d\n", cmd_idx,
			  err, bIgnore);
		mmcmsg3(KERN_WARNING "%s: %s fail\n", DRIVER_NAME, __func__);
		if (bIgnore)
			return err;
#if 0
		BUG();
#endif
		if (retry_count++ < MAX_CMD_RETRY_COUNT) {
			err = error_handling(sdport, cmd_idx, bIgnore);
#ifdef FORCE_CHG_SDR50_MODE
			if ((cmd_arg == 0x9999) && (!err))
				return RTK_SUCC;
#endif
			goto STR_CMD_RET;
		}
	}
	return err;
}

static u32 rtkcr_chk_cmdcode(struct mmc_command *cmd)
{
	u32 cmdcode;

	if (cmd->opcode < 56) {
		cmdcode = (u32) rtk_sd_cmdcode[cmd->opcode][0];
		WARN_ON(cmd->data == NULL);
		if (cmd->data->flags & MMC_DATA_WRITE) {
			if (cmd->opcode == 42)
				cmdcode = EMMC_NORMALWRITE;
			else if (cmd->opcode == 56)
				cmdcode = EMMC_AUTOWRITE2;
		}
	} else {
		cmdcode = EMMC_CMD_UNKNOW;
	}

	return cmdcode;

}

static u32 rtkcr_chk_r1_type(struct mmc_command *cmd)
{
	return 0;
}

static u8 rtk_get_rsp_type(struct mmc_command *cmd)
{
	u32 rsp_type;
	if (mmc_resp_type(cmd) == MMC_RSP_R1) {
		rsp_type = SD_R1;
	} else if (mmc_resp_type(cmd) == MMC_RSP_R1B) {
		rsp_type = SD_R1b;
	} else if (mmc_resp_type(cmd) == MMC_RSP_R2) {
		rsp_type = SD_R2;
	} else if (mmc_resp_type(cmd) == MMC_RSP_R3) {
		rsp_type = SD_R3;
	} else if (mmc_resp_type(cmd) == MMC_RSP_R6) {
		rsp_type = SD_R6;
	} else if (mmc_resp_type(cmd) == MMC_RSP_R7) {
		rsp_type = SD_R7;
	} else {
		rsp_type = SD_R0;
	}
	return rsp_type;
}

//#define USE_TMP_BUF
static int SD_Stream(struct sd_cmd_pkt *cmd_info)
{
	int err = 0;
	u32 i;
	struct scatterlist *sg;
	u32 dir = 0;
	u32 dma_nents = 0;
	u32 dma_leng;
	dma_addr_t dma_addr;
	dma_addr_t dma_addr_sys = 0;
	u32 old_arg;
	u8 one_blk = 0;
	u8 f_in_dma = 0;
	u16 cmdcode = 0;
	//enum dma_data_direction   direction;
	//dma_addr_t                dma_addr = 0;

	struct mmc_host *host = cmd_info->sdport->mmc;
	struct rtksd_host *sdport = cmd_info->sdport;

	rtksd_set_rspparam(sdport, cmd_info);	//for 119x

	MMCPRINTF("\n");
	if (cmd_info->data->flags & MMC_DATA_READ) {
		dir = DMA_FROM_DEVICE;
	} else {
		// KWarning: checked ok by alexkh@realtek.com
		if (host->card && mmc_card_cmd24_err(host->card)) {
			one_blk = 1;
		}

		dir = DMA_TO_DEVICE;
	}

	cmd_info->data->bytes_xfered = 0;
	dma_nents = dma_map_sg(mmc_dev(host), cmd_info->data->sg,
			       cmd_info->data->sg_len, dir);
	sg = cmd_info->data->sg;

#ifdef SHOW_MMC_PRD
	printk("sg_len:%u\n", cmd_info->data->sg_len);
	printk("sg:0x%p; dma_nents:%u\n", sg, dma_nents);
#endif
	old_arg = cmd_info->cmd->arg;

	for (i = 0; i < dma_nents; i++, sg++) {
		dma_leng = sg_dma_len(sg);

		if (dma_leng & 0x1ff) {
			dma_addr_sys = sg_dma_address(sg);

			if (sdport->tmp_buf) {	//use tmp buffer
				f_in_dma = 1;
				dma_addr = sdport->tmp_buf_phy_addr;
#ifndef USE_TMP_BUF
				WARN_ON(1);
#endif
			} else {	//use default buffer
				dma_addr = dma_addr_sys;
			}
		} else {
			dma_addr = sg_dma_address(sg);
		}

		//dma_addr = dma_map_page(host->parent, sg_page(sg), 0,
		//                              PAGE_SIZE, dir);
#ifdef SHOW_MMC_PRD
		printk("dma_addr:0x%x; dma_leng:0x%x\n", dma_addr, dma_leng);
		mmcinfo("host=%p\n", host);
		if (host->card) {
			mmcinfo("card=%p\n", host->card);
			if (mmc_card_blockaddr(host->card))
				printk("arg:0x%x blk\n", cmd_info->cmd->arg);
			else
				printk("arg:0x%x byte\n", cmd_info->cmd->arg);
		}
#endif

		if (one_blk) {	/* occur at write case only */
			u8 i;
			u32 blk_cnt;
			struct sd_cmd_pkt tmp_pkt;

			blk_cnt = dma_leng / BYTE_CNT;
			if (blk_cnt == 0) {
				WARN_ON(1);
				blk_cnt = 1;
			}
			duplicate_pkt(cmd_info, &tmp_pkt);

			tmp_pkt.byte_count = BYTE_CNT;
			tmp_pkt.block_count = 1;
			tmp_pkt.dma_buffer = (unsigned char *)dma_addr;

			if (tmp_pkt.cmd->opcode == 25) {
				tmp_pkt.cmd->opcode = 24;
			}

			if (tmp_pkt.cmd->opcode == 18) {
				tmp_pkt.cmd->opcode = 17;
			}

			cmdcode = sdport->ops->chk_cmdcode(tmp_pkt.cmd);
			for (i = 0; i < blk_cnt; i++) {

				err = SD_Stream_Cmd(cmdcode, &tmp_pkt, 0);

				if (err == 0) {
					if (host->card) {
						if (tmp_pkt.cmd->opcode == 24)
							host->card->state &=
							    ~MMC_STATE_CMD24_ERR;
					}

					if (host->card
					    && mmc_card_blockaddr(host->card))
						tmp_pkt.cmd->arg += 1;
					else
						tmp_pkt.cmd->arg += BYTE_CNT;

					tmp_pkt.dma_buffer += BYTE_CNT;
					tmp_pkt.data->bytes_xfered += BYTE_CNT;

				} else {
					//TBD
					if (rtksd_err_handle(cmdcode, &tmp_pkt)) {

						if (tmp_pkt.cmd->opcode == 24) {
							int err34 = 0;

							err34 =
							    SD_Stream_Cmd34
							    (EMMC_AUTOWRITE3,
							     &tmp_pkt);
							if (err34) {
								if (rtksd_err_handle(EMMC_AUTOWRITE3, &tmp_pkt)) {
									mmcmsg3
									    (KERN_INFO
									     "%s: error recover fail 1\n",
									     DRIVER_NAME);
								}
							} else {
								rtksd_wait_status
								    (host->card,
								     STATE_TRAN,
								     0, 0);
							}

						} else {
							mmcmsg3(KERN_INFO
								"%s: error recover fail 2\n",
								DRIVER_NAME);
						}
					}
					break;
				}
			}
//liaokh            dma_cache_wback(KSEG0ADDR(dma_addr), dma_leng);
			//dma_unmap_page(host->parent, dma_addr, PAGE_SIZE, dir);
		} else {
			u32 blk_cnt;
			cmd_info->byte_count = BYTE_CNT;	//rtk HW limite, one trigger 512 byte pass.
			blk_cnt = dma_leng / BYTE_CNT;

			if (blk_cnt == 0 && dma_leng) {
				blk_cnt = 1;
			}

			cmd_info->block_count = blk_cnt;
			cmd_info->dma_buffer = (unsigned char *)dma_addr;

			//if(cmd_info->data->flags & MMC_DATA_READ){
//liaokh                dma_cache_inv(KSEG0ADDR(dma_addr), dma_leng);
			//}
			//else
			//{
			//rtksd_wait_status(host->card,STATE_TRAN,0,0);
			//}

			cmdcode = sdport->ops->chk_cmdcode(cmd_info->cmd);
			err = SD_Stream_Cmd(cmdcode, cmd_info, 0);

			//if(cmd_info->data->flags & MMC_DATA_WRITE){
//liaokh                dma_cache_wback(KSEG0ADDR(dma_addr), dma_leng);
			//}
			//dma_unmap_page(host->parent, dma_addr, PAGE_SIZE, dir);

			if (err == 0) {
				if (host->card) {
					if ((cmd_info->cmd->opcode == 25) &&
					    (cmdcode == EMMC_AUTOWRITE2)) {
						int stop_err;

						stop_err =
						    rtksd_stop_transmission
						    (host->card, 0);
						if (stop_err) {
							MMCPRINTF
							    ("rtksd_stop_transmission fail\n");
							goto ERR_HANDLE;
						}
					}
				}

				if (host->card
				    && mmc_card_blockaddr(host->card))
					cmd_info->cmd->arg +=
					    cmd_info->block_count;
				else
					cmd_info->cmd->arg += dma_leng;

				cmd_info->data->bytes_xfered += dma_leng;

			} else {
ERR_HANDLE:
				//TBD
				if (rtksd_err_handle(cmdcode, cmd_info)) {
					if (cmd_info->cmd->opcode == 18
					    || cmd_info->cmd->opcode == 25) {
						if (host->card) {
							int stop_err;
							stop_err =
							    rtksd_stop_transmission
							    (host->card, 0);
							if (stop_err) {
								MMCPRINTF
								    ("rtksd_stop_transmission fail\n");
								//goto ERR_HANDLE;
							}
							rtksd_wait_status(host->
									  card,
									  STATE_TRAN,
									  0, 0);
						}

					} else if (cmd_info->cmd->opcode == 24) {
						int err34 = 0;
						err34 =
						    SD_Stream_Cmd34
						    (EMMC_AUTOWRITE3, cmd_info);
						if (err34) {
							if (rtksd_err_handle
							    (EMMC_AUTOWRITE3,
							     cmd_info)) {
								mmcmsg3
								    (KERN_INFO
								     "%s: error recover fail 3\n",
								     DRIVER_NAME);
							}
						} else {
							if (host->card)
								rtksd_wait_status
								    (host->card,
								     STATE_TRAN,
								     0, 0);
						}

					} else {
						mmcmsg3(KERN_INFO
							"%s: error recover fail 4\n",
							DRIVER_NAME);
					}
				}
			}
		}

		if (err) {
			cmd_info->cmd->arg = old_arg;
			break;
		}

		if (f_in_dma) {
			u32 i;
			u8 *ptr1 = phys_to_virt(dma_addr_sys);
			u8 *ptr2 = phys_to_virt(dma_addr);
			f_in_dma = 0;
			for (i = 0; i < dma_leng; i++)
				*ptr1 = *ptr2;
		}
	}
	dma_unmap_sg(mmc_dev(host), cmd_info->data->sg,
		     cmd_info->data->sg_len, dir);

	//reset cmd0
	g_cmd[0] = 0x00;

	return err;

}

static void rtkcr_req_end_tasklet(unsigned long param)
{
	struct rtksd_host *sdport;
	struct mmc_request *mrq;
	unsigned long flags;

	sdport = (struct rtksd_host *)param;
	spin_lock_irqsave(&sdport->lock, flags);

	mrq = sdport->mrq;
	sdport->mrq = NULL;

	spin_unlock_irqrestore(&sdport->lock, flags);
	mmc_request_done(sdport->mmc, mrq);
}

static void rtksd_send_command(struct rtksd_host *sdport,
			       struct mmc_command *cmd)
{
	int rc = 0;
	struct sd_cmd_pkt cmd_info;
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	if (!sdport || !cmd) {
		printk(KERN_ERR "%s: sdport or cmd is null\n", DRIVER_NAME);
		return;
	}

	cmd_info.cmd = cmd;
	cmd_info.sdport = sdport;
	cmd_info.rsp_para2 = rtk_get_rsp_type(cmd_info.cmd);
	cmd_info.rsp_len = rtksd_get_rsp_len(cmd_info.rsp_para2);

	if (cmd->data) {
		cmd_info.data = cmd->data;
		if (cmd->data->flags & MMC_DATA_READ) {
			/* do nothing */
		} else if (cmd->data->flags & MMC_DATA_WRITE) {
			if (sdport->wp == 1) {
				pr_warn("%s: card is locked!",
				       DRIVER_NAME);
				rc = -1;
				cmd->retries = 0;
				goto err_out;
			}
		} else {
			printk(KERN_ERR "error: cmd->data->flags=%d\n",
			       cmd->data->flags);
			cmd->error = -MMC_ERR_INVALID;
			cmd->retries = 0;
			goto err_out;
		}
		rc = SD_Stream(&cmd_info);

	} else {
		rc = SD_SendCMDGetRSP(&cmd_info);
	}

	if (cmd->opcode == MMC_SWITCH) {
		if ((cmd->arg & 0xffff00ff) == 0x03b30001) {
			if ((cmd->arg & 0x0000ff00) == 0) {
				sdport->rtflags |= RTKCR_USER_PARTITION;
			} else {
				sdport->rtflags &= ~RTKCR_USER_PARTITION;
			}
		}
	}
	MMCPRINTF("%s: cmd->opcode=0x%02x\n", __func__, cmd->opcode);
#if 0
	if (cmd->opcode == MMC_SELECT_CARD) {
		MMCPRINTF("to DIV_none, then tuning at this speed\n");
		//rtkcr_set_div(sdport,EMMC_CLOCK_DIV_NON);
		mmc_tuning_sdr50(&cmd_info);
	}
#endif

err_out:
	if (rc) {
		if (rc == -RTK_RMOV)
			cmd->retries = 0;
		if (cmd->data)
			cmd->data->error = rc;
		else
			cmd->error = rc;
	}
	tasklet_schedule(&sdport->req_end_tasklet);
}

static void rtksd_fix_unaligned_sg(struct rtksd_host *sdport, struct mmc_data *data)
{
	if (data->flags & MMC_DATA_WRITE) {
		if (data->blksz * data->blocks > PAGE_SIZE) {
			dev_err(sdport->dev, "Unaligned data size exceed PAGE_SIZE\n");
			data->error = -EINVAL;
			return;
		}
		sg_copy_to_buffer(data->sg, data->sg_len, sdport->aligned_buf,
				data->blksz * data->blocks);
		sg_init_one(&sdport->aligned_sg, sdport->aligned_buf,
				data->sg->length);
		data->sg = &sdport->aligned_sg;
	} else
		data->sg->offset = 0;
}

static void rtksd_request(struct mmc_host *host, struct mmc_request *mrq)
{
	struct rtksd_host *sdport;
	struct mmc_command *cmd;
	//struct mmc_card  *card;
#ifdef USE_TMP_BUF
	unsigned char *tmp_buf = NULL;
#endif

	MMCPRINTF("\n");

	sdport = mmc_priv(host);
	BUG_ON(sdport->mrq != NULL);

	//spin_lock_irqsave(&sdport->lock,flags);
	down_write(&cr_rw_sem);
	cmd = mrq->cmd;
	sdport->mrq = mrq;

	if (!(sdport->rtflags & RTKCR_FCARD_DETECTED)) {
		cmd->error = -MMC_ERR_RMOVE;
		cmd->retries = 0;
		goto done;
	}

	if (mrq->data) {
		if (mrq->data->sg->offset & 7) {
			dev_info(sdport->dev, "Unaligned Bad SG: %ux%u @ offset %u sg_len = %u\n",
				mrq->data->blksz, mrq->data->blocks,
				mrq->data->sg->offset, mrq->data->sg->length);
			rtksd_fix_unaligned_sg(sdport, mrq->data);
		}
	}

/*
 * the "tmp_buf" should used for sg_init_one() in blksz small than 512 byte case.
 * because RTK DMA HW issue,the samllest DMA size is 512 byte
 */
#ifdef USE_TMP_BUF
	if (cmd->data) {

		if (cmd->data->blksz < 512) {
			WARN_ON(1);
			printk("blksz=%d\n", cmd->data->blksz);

			tmp_buf = dma_alloc_coherent(sdport->dev, 512,
						     &sdport->tmp_buf_phy_addr,
						     GFP_KERNEL);

			if (!tmp_buf) {
				WARN_ON(1);
				cmd->error = -ENOMEM;
				goto done;
			}
			memset(tmp_buf, 0, 512);
			sdport->tmp_buf = tmp_buf;
			//printk("%s(%u)buf=0x%p phy_addr=0x%x\n",
			//        __func__,__LINE__,
			//        sdport->tmp_buf,sdport->tmp_buf_phy_addr);
		} else {
			sdport->tmp_buf = NULL;
		}
	}
#endif //#ifdef USE_TMP_BUF

	if (sdport && cmd) {
		rtksd_allocate_dma_buf(sdport, cmd);
		rtksd_send_command(sdport, cmd);
	} else {
done:
		tasklet_schedule(&sdport->req_end_tasklet);
	}
	up_write(&cr_rw_sem);

	//spin_unlock_irqrestore(&sdport->lock, flags);
}

static int rtksd_execute_tuning(struct mmc_host *host, u32 opcode)
{
	struct rtksd_host *sdport;
	struct sd_cmd_pkt cmd_info;

	sdport = mmc_priv(host);
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	cmd_info.sdport = sdport;

	MMCPRINTF("%s : opcode=0x%08x, mode=0x%08x\n", __func__,
			opcode, sdport->speed_step);

	sdport->is_tuning = 1;
	if (host->caps2 & MMC_CAP2_HS400)
		mmc_tuning_hs200(sdport);
	else if (host->caps2 & MMC_CAP2_HS200)
		mmc_tuning_hs200(sdport);
	else if (host->caps & MMC_CAP_DDR)
		mmc_tuning_ddr50(sdport);
	else
		mmc_tuning_sdr50(sdport);
	sdport->is_tuning = 0;

	return 0;
}

static void rtksd_hs400_complete(struct mmc_host *host)
{
	struct rtksd_host *sdport = mmc_priv(host);

	sdport->is_tuning = 1;
	mmc_tuning_hs400(sdport);
	sdport->is_tuning = 0;
	dev_info(sdport->dev, "After HS400 tuning delay chain is 0x%x\n",
			cr_readb(sdport->base + DS_DELAY_CFG));
}

static void rtksd_hw_reset(struct rtksd_host *sdport)
{	
	rtk_emmc_hw_init(sdport);
	sdport->ops->chk_card_insert(sdport);
}

/* liao ********************
 * check read only func
 *
 *
 ***************************/
static int rtksd_get_ro(struct mmc_host *mmc)
{
	//struct rtksd_host *sdport = mmc_priv(mmc);
	//struct mmc_card *card = mmc->card;

	return 0;
}

static void rtksd_set_ios(struct mmc_host *host, struct mmc_ios *ios)
{
	struct rtksd_host *sdport;
	sdport = mmc_priv(host);

	MMCPRINTF("ios->bus_mode = %u\n", ios->bus_mode);
	MMCPRINTF("ios->clock = %u\n", ios->clock);
	MMCPRINTF("ios->bus_width = %u\n", ios->bus_width);
	MMCPRINTF("ios->timing = %u\n", ios->timing);

	//down_write(&cr_rw_sem);
	if (!g_bResuming) {
		if (ios->clock == 52000000) {
			sdport->ops->set_clk(sdport, CLK_100MHZ);
		} else if (ios->clock == 200000000) {
			sdport->ops->set_clk(sdport, CLK_200MHZ);
		} else {
			sdport->ops->set_clk(sdport, CLK_50MHZ);
		}

		switch (ios->timing) {
		case MMC_TIMING_LEGACY:
			rtkcr_set_mode_selection(sdport, MODE_SD20);
			sdport->ops->set_clk(sdport, CLK_50MHZ);
			break;
		case MMC_TIMING_MMC_HS:
			rtkcr_set_mode_selection(sdport, MODE_SD20);
			sdport->ops->set_clk(sdport, CLK_100MHZ);
			break;
		case MMC_TIMING_UHS_DDR50:
		case MMC_TIMING_MMC_DDR52:
			rtkcr_set_mode_selection(sdport, MODE_DDR);
			sdport->ops->set_clk(sdport, CLK_100MHZ);
			break;
		case MMC_TIMING_MMC_HS200:
			rtkcr_set_mode_selection(sdport, MODE_SD30);
			//sdport->ops->set_clk(sdport, CLK_200MHZ);
			break;
		case MMC_TIMING_MMC_HS400:
			rtkcr_set_mode_selection(sdport, MODE_HS400);
			//sdport->ops->set_clk(sdport, CLK_200MHZ);
			cr_writeb(0xE0, sdport->base + SD_AUTO_RST_FIFO);
			cr_writeb(0x00, sdport->base + SD_PUSH_POINT_CTL);
			break;
		default:
			rtkcr_set_mode_selection(sdport, MODE_SD20);
			sdport->ops->set_clk(sdport, CLK_50MHZ);
			dev_info(sdport->dev,
				"%s: timing not match, set mode to LEGACY clk to 25Mhz\n", __func__);
		}
	}

	if (ios->power_mode == MMC_POWER_OFF) {
		dev_info(sdport->dev, "%s: MMC power off\n", __func__);
		rtksd_hw_reset(sdport);
	}
	//rtk_emmc_get_clk_reg(sdport);

	if (ios->bus_width == MMC_BUS_WIDTH_8) {
		MMCPRINTF("%s: set bus width 8\n", __func__);
		rtksd_set_bits(sdport, BUS_WIDTH_8);
	} else if (ios->bus_width == MMC_BUS_WIDTH_4) {
		MMCPRINTF("%s: set bus width 4\n", __func__);
		rtksd_set_bits(sdport, BUS_WIDTH_4);
	} else {
		MMCPRINTF("%s: set bus width 1\n", __func__);
		rtksd_set_bits(sdport, BUS_WIDTH_1);
	}

	if (ios->bus_mode == MMC_BUSMODE_PUSHPULL) {
		MMCPRINTF("ios busmode = pushpull\n");
		//rtkcr_set_div(sdport,EMMC_CLOCK_DIV_NON);
	} else {		//MMC_BUSMODE_OPENDRAIN
		MMCPRINTF("ios busmode != pushpull\n");
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_128);
		rtksd_set_bits(sdport, BUS_WIDTH_1);
	}

	//up_write(&cr_rw_sem);
}

static void rtkemmc_dump_registers(struct rtksd_host *sdport)
{
	MMCPRINTF(KERN_INFO "%s : \n", __func__);
	MMCPRINTF("card_select=0x%02x \n", gRegTbl.card_select);
	MMCPRINTF("sample_point_ctl=0x%02x \n", gRegTbl.sample_point_ctl);
	MMCPRINTF("push_point_ctl=0x%02x \n", gRegTbl.push_point_ctl);
	//MMCPRINTF("sys_pll_emmc3=0x%08x \n", gRegTbl.sys_pll_emmc3);
	//MMCPRINTF("pll_emmc1=0x%08x \n", gRegTbl.pll_emmc1);
	MMCPRINTF("sd_configure1=0x%02x \n", gRegTbl.sd_configure1);
	MMCPRINTF("sd_configure2=0x%02x \n", gRegTbl.sd_configure2);
	MMCPRINTF("sd_configure3=0x%02x \n", gRegTbl.sd_configure3);
	MMCPRINTF("EMMC_CARD_PAD_DRV=0x%02x \n",
		  cr_readb(sdport->base + EMMC_CARD_PAD_DRV));
	MMCPRINTF("EMMC_CMD_PAD_DRV=0x%02x \n",
		  cr_readb(sdport->base + EMMC_CMD_PAD_DRV));
	MMCPRINTF("EMMC_DATA_PAD_DRV=0x%02x \n",
		  cr_readb(sdport->base + EMMC_DATA_PAD_DRV));
	//sync();
}

static void rtkemmc_restore_registers(struct rtksd_host *sdport)
{
	MMCPRINTF(KERN_INFO "%s : \n", __func__);
	rtkcr_set_pad_driving(sdport, MMC_IOS_RESTORE_PAD_DRV, 0x66, 0x64,
			      0x66);
	cr_writeb(gRegTbl.card_select, sdport->base + CARD_SELECT);
	cr_writeb(gRegTbl.sample_point_ctl, sdport->base + SD_SAMPLE_POINT_CTL);
	cr_writeb(gRegTbl.push_point_ctl, sdport->base + SD_PUSH_POINT_CTL);
	//cr_writel(gRegTbl.sys_pll_emmc3, SYS_PLL_EMMC3);
	//cr_writel(gRegTbl.pll_emmc1, PLL_EMMC1);
	cr_writeb(gRegTbl.sd_configure1, sdport->base + SD_CONFIGURE1);
	cr_writeb(gRegTbl.sd_configure2, sdport->base + SD_CONFIGURE2);
	cr_writeb(gRegTbl.sd_configure3, sdport->base + SD_CONFIGURE3);
	//sync();
	rtkemmc_dump_registers(sdport);
}

static void rtkemmc_backup_registers(struct rtksd_host *sdport)
{
	MMCPRINTF(KERN_INFO "%s : \n", __func__);
	rtkcr_set_pad_driving(sdport, MMC_IOS_GET_PAD_DRV, 0x66, 0x64, 0x66);
	gRegTbl.card_select = cr_readb(sdport->base + CARD_SELECT);
	gRegTbl.sample_point_ctl = cr_readb(sdport->base + SD_SAMPLE_POINT_CTL);
	gRegTbl.push_point_ctl = cr_readb(sdport->base + SD_PUSH_POINT_CTL);
	//gRegTbl.sys_pll_emmc3    = cr_readl(SYS_PLL_EMMC3);
	//gRegTbl.pll_emmc1        = cr_readl(PLL_EMMC1);
	//MMCPRINTF(KERN_INFO "%s : pll_emmc1=0x%08x, reg_pll_emmc1=0x%08x\n", __func__,gRegTbl.pll_emmc1,cr_readl(PLL_EMMC1));

	gRegTbl.sd_configure1 = cr_readb(sdport->base + SD_CONFIGURE1);
	gRegTbl.sd_configure2 = cr_readb(sdport->base + SD_CONFIGURE2);
	gRegTbl.sd_configure3 = cr_readb(sdport->base + SD_CONFIGURE3);
	//sync();
	rtkemmc_dump_registers(sdport);
}

inline void rtk_emmc_get_clk_reg(struct rtksd_host *sdport)
{
	printk("\tSSCPLL_CTRL1 = 0x%x, SSCPLL_CTRL2 = 0x%x, SD_CONFIGURE1 = 0x%x, CKGEN_CTL = 0x%x\n",
		cr_readl(sdport->pll_base + SSCPLL_CTRL1), cr_readl(sdport->pll_base + SSCPLL_CTRL2),
		cr_readb(sdport->base + SD_CONFIGURE1), cr_readl(sdport->base + EMMC_CKGEN_CTL));
}

void rtkemmc_set_dma(void *base, u32 src, u32 len, u32 cfg)
{
	MMCPRINTF("%s: src_addr = 0x%x, len = 0x%x, cfg = 0x%x\n",
			__func__);

	cr_writel(src, base + EMMC_DMA_CTL1);
	cr_writel(len, base + EMMC_DMA_CTL2);
	cr_writel(cfg, base + EMMC_DMA_CTL3);
}

static void rtkemmc_set_clk(struct rtksd_host *sdport, u32 mmc_clk)
{
	// set OC_EN/FLAG_INITIAL/EN_SSC = 0
	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL0, REG_OC_EN, ~REG_OC_EN);
	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL0, FLAG_INIT, ~FLAG_INIT);
	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, ~EN_SSC);
	// wait 100ns, set PHRT0 = 0
	usleep_range(1, 2);
	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL0, PHRT0, ~PHRT0);

	// wait 100ns, set NCODE_SSC/FCODE_SSC/NCODE_T_SCPU/FCODE_T_SCPU
	usleep_range(1, 2);

	switch (mmc_clk) {
	case CLK_200KHZ:
		writel(0x2004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x4000008, sdport->pll_base + SSCPLL_CTRL4);
		usleep_range(1, 2);
		writel(0x2004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(1, 2);
		writel(0x78b002, sdport->pll_base + SSCPLL_CTRL1);
		writel(0x3bd6a3d, sdport->pll_base + SSCPLL_CTRL2);
		writel(0x184f, sdport->pll_base + SSCPLL_CTRL3);

		usleep_range(1, 2);
		writel(0x6004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(19, 20);
		writel(0x6004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x185f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);
		rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, EN_SSC);
		usleep_range(1, 2);

		rtkcr_set_speed(sdport, 0);
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_128);
		break;
	case CLK_50MHZ:
		writel(0x2004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x4000008, sdport->pll_base + SSCPLL_CTRL4);
		usleep_range(1, 2);
		writel(0x2004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(1, 2);

		writel(0x78b002, sdport->pll_base + SSCPLL_CTRL1);
		writel(0x3bd6a3d, sdport->pll_base + SSCPLL_CTRL2);
		writel(0x184f, sdport->pll_base + SSCPLL_CTRL3);

		usleep_range(1, 2);
		writel(0x6004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(19, 20);
		writel(0x6004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x185f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);
		rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, EN_SSC);
		usleep_range(1, 2);
		rtkcr_set_speed(sdport, 0);
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_NON);
		break;
	case CLK_100MHZ:
		writel(0x82004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x84004008, sdport->pll_base + SSCPLL_CTRL4);
		usleep_range(1, 2);
		writel(0x82004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(1, 2);

		writel(0x2078b006, sdport->pll_base + SSCPLL_CTRL1);
		writel(0x3bd6000, sdport->pll_base + SSCPLL_CTRL2);
		writel(0x384f, sdport->pll_base + SSCPLL_CTRL3);

		usleep_range(1, 2);
		writel(0x86004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(19, 20);
		writel(0x86004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x385f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);
		rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, EN_SSC);
		usleep_range(1, 2);
		rtkcr_set_speed(sdport, 0);
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_NON);
		break;
	case CLK_200MHZ:
		writel(0x82004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x84004008, sdport->pll_base + SSCPLL_CTRL4);
		usleep_range(1, 2);
		writel(0x82004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(1, 2);

		writel(0x4078b007, sdport->pll_base + SSCPLL_CTRL1);
		writel(0x3bd6000, sdport->pll_base + SSCPLL_CTRL2);
		writel(0x784f, sdport->pll_base + SSCPLL_CTRL3);

		usleep_range(1, 2);
		writel(0x86004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(19, 20);
		writel(0x86004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x785f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);
		rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, EN_SSC);
		usleep_range(1, 2);
		rtkcr_set_speed(sdport, 0);
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_NON);
		rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0xff, 0xff, 0xff);
		break;
	case CLK_26MHZ:
	default:
		writel(0x2004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x4000008, sdport->pll_base + SSCPLL_CTRL4);
		usleep_range(1, 2);
		writel(0x2004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(1, 2);

		writel(0x878a000, sdport->pll_base + SSCPLL_CTRL1);
		writel(0x3bd6a3d, sdport->pll_base + SSCPLL_CTRL2);
		writel(0x184f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);

		writel(0x6004001, sdport->pll_base + SSCPLL_CTRL0);
		usleep_range(19, 20);
		writel(0x6004003, sdport->pll_base + SSCPLL_CTRL0);
		writel(0x185f, sdport->pll_base + SSCPLL_CTRL3);
		usleep_range(1, 2);
		rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL4, EN_SSC, EN_SSC);
		usleep_range(1, 2);
		rtkcr_set_speed(sdport, 0);
		rtkcr_set_div(sdport, EMMC_CLOCK_DIV_NON);
	}
	// wait 100ns, set OC_EN=1
	//usleep_range(1, 2);
	//rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL0, REG_OC_EN, REG_OC_EN);

	// wait 20us, set PHRT0=1
	//usleep_range(20, 21);
	//rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL0, PHRT0, PHRT0);

	// wait 100ns, enable clk_sd30_en
	//usleep_range(1, 2);
	//rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL3, CLK_EN_SD30, CLK_EN_SD30);

	// complete
	usleep_range(1, 2);

	MMCPRINTF("%s: Set clk freq to %s\n", __func__, clk_tlb[mmc_clk]);
}

static void rtkemmc_chk_card_insert(struct rtksd_host *sdport)
{
	struct mmc_host *host = sdport->mmc;

	MMCPRINTF("%s : \n", __func__);
	rtksd_set_bits(sdport, BUS_WIDTH_1);
	rtkcr_set_mode_selection(sdport, MODE_SD20);
	sdport->ops->set_clk(sdport, CLK_200KHZ);
	cr_writeb(0x58, sdport->base + SD_SAMPLE_POINT_CTL);	//sample point = SDCLK / 4
	cr_writeb(0x0, sdport->base + SD_PUSH_POINT_CTL);	//output ahead SDCLK /4

	host->ops = &rtkemmc_ops;
	sdport->rtflags |= RTKCR_FCARD_DETECTED;
	MMCPRINTF("%s : \n", __func__);
}

static void rtksd_timeout_timer(struct timer_list *t)
{
	struct rtksd_host *sdport;
	u8 wflag = 0;
	u32 int_status = 0;
	u32 sd_status1 = 0, sd_status2 = 0, bus_status = 0;
	u32 sd_trans = 0, dma_trans = 0;
	unsigned long flags;

	sdport = from_timer(sdport, t, timer);
	MMCPRINTF("rtksd_timeout_timer fired ...\n");
	MMCPRINTF("%s - int_wait=%08x\n", __func__, sdport->int_waiting);
	spin_lock_irqsave(&sdport->lock, flags);
	//down_write(&cr_rw_sem);
#ifdef ENABLE_EMMC_INT_MODE
	if (sdport->int_waiting)
#else
	if (1)
#endif
	{
		MMCPRINTF("0. get sd trans \n");
		wflag = 0x01;
#ifdef ENABLE_EMMC_INT_MODE
		MMCPRINTF("========== C1 ==========\n");
		rtkcr_hold_int_dec(sdport->base);
		rtkcr_clr_int_sta(sdport->base);
		//sync();
		rtkcr_get_int_sta(sdport->base, &int_status);
		sdport->int_status = int_status;
#endif
		MMCPRINTF("1. get sd trans \n");
		MMCPRINTF("baseio=0x%08x\n", sdport->base);
		rtkcr_get_sd_trans(sdport->base, &sd_trans);
		rtkcr_get_dma_trans(sdport->base, &dma_trans);

		MMCPRINTF("2. get sd status \n");
		rtkcr_get_sd_sta(sdport->base, &sd_status1, &sd_status2,
				 &bus_status);

		sdport->sd_trans = sd_trans;
		sdport->sd_status1 = sd_status1;
		sdport->sd_status2 = sd_status2;
		sdport->bus_status = bus_status;
		sdport->dma_trans = dma_trans;
		MMCPRINTF
		    ("int sts : 0x%08x sd_trans : 0x%08x, sd_st1 : 0x%08x\n",
		     sdport->int_status, sdport->sd_trans, sdport->sd_status1);
		MMCPRINTF
		    ("int st2 : 0x%08x bus_sts : 0x%08x dma_trans : 0x%08x\n",
		     sdport->sd_status2, sdport->bus_status, sdport->dma_trans);
#ifdef ENABLE_EMMC_INT_MODE
		rtkcr_en_int(sdport->base);
#endif
	} else {
		WARN_ON(1);
	}

	if (sdport->int_waiting)
		rtk_op_complete(sdport);

	spin_unlock_irqrestore(&sdport->lock, flags);
	//up_write(&cr_rw_sem);
#ifdef ENABLE_EMMC_INT_MODE
	MMCPRINTF(KERN_WARNING "%s: %s %s card access time out!\n",
		  DRIVER_NAME,
		  (wflag & 0x01) ? "int error;" : "",
		  (wflag & 0x10) ? "dma error;" : "");
#endif

}

#ifdef EMMC_SHOUTDOWN_PROTECT
static void rtksd_gpio_isr(VENUS_GPIO_ID gid, unsigned char assert, void *dev)
{
	struct rtksd_host *sdport = dev;
	u32 reg_tmp;
	u32 bit_tmp;
	u32 reginfo;
	unsigned char assert_tmp;

	printk("%s(%u) sdport=0x%p (0x%x) [assert:%x]\n",
	       __func__, __LINE__, sdport, gid, assert);

	reginfo = GET_PIN_TYPE(sdport->gpio_isr_info);
	bit_tmp = GET_PIN_INDEX(sdport->gpio_isr_info);

	reg_tmp = ((bit_tmp & 0xffffffe0) >> 5) << 2;
	bit_tmp = (bit_tmp & 0x1fUL);

	switch (reginfo) {
	case PCB_PIN_TYPE_GPIO:
		reg_tmp += GP0DATI;
		printk(KERN_ERR "PIN type is PCB_PIN_TYPE_GPIO\n");
		break;
	case PCB_PIN_TYPE_ISO_GPIO:
		reg_tmp += ISO_GPDATI;
		printk(KERN_ERR "PIN type is PCB_PIN_TYPE_ISO_GPIO\n");
		break;
	default:
		printk(KERN_ERR "PIN group not match\n");
		WARN_ON(1);
		return;
	}

	reginfo = cr_readl(reg_tmp);
	assert_tmp =
	    (unsigned char)((reginfo & (0x01UL << bit_tmp)) >> bit_tmp);
	WARN_ON(assert_tmp != assert);

	if ((reginfo & (0x01UL << bit_tmp)) == 0) {
		printk(KERN_ERR "%s(%u)Hold eMMC RSENT!!!\n",
		       __func__, __LINE__);

		rtksd_hold_card(sdport);
	}
}

#endif

static irqreturn_t rtksd_irq(int irq, void *dev)
{
	struct rtksd_host *sdport = dev;

	int irq_handled = 0;
	u32 int_status = 0;
	u32 sd_trans = 0, dma_trans = 0;
	u32 sd_status1 = 0, sd_status2 = 0, bus_status = 0;

	MMCPRINTF("rtksd_irq interrupted ...\n");

	rtkcr_hold_int_dec(sdport->base);
	rtkcr_get_int_sta(sdport->base, &int_status);
	rtkcr_clr_int_sta(sdport->base);
	//sync();

	//down_write(&cr_rw_sem);
	//spin_lock(&sdport->lock);

	MMCPRINTF("int_status =%08x, int_waiting=%08x\n", int_status,
		  sdport->int_waiting);
	if (int_status & (ISRSTA_INT1 | ISRSTA_INT2 | ISRSTA_INT4))	//card_end ?
	{
		MMCPRINTF("========== C2 ==========\n");
		rtkcr_get_sd_trans(sdport->base, &sd_trans);
		rtkcr_get_dma_trans(sdport->base, &dma_trans);
		rtkcr_get_sd_sta(sdport->base, &sd_status1, &sd_status2,
				 &bus_status);
		//sync();

		MMCPRINTF("we get int end \n");

#ifdef  SHOW_INT_STATUS
		printk("INT=0x%08x sdport=0x%p\n", int_status, sdport);
#endif
		sdport->int_status = int_status;
		sdport->sd_trans = sd_trans;
		sdport->sd_status1 = sd_status1;
		sdport->sd_status2 = sd_status2;
		sdport->bus_status = bus_status;
		sdport->dma_trans = dma_trans;
		if (sdport->int_waiting) {
			MMCPRINTF("int wait clear 1\n");
			del_timer(&sdport->timer);
			rtk_op_complete(sdport);
			MMCPRINTF("int wait clear 2\n");
		} else {
			MMCPRINTF("No int_waiting!!!\n");
		}
		//rtkcr_en_int(sdport->base);
		irq_handled = 1;
	} else {
		printk(KERN_ERR "INT no END_STATE!!!\n");
	}

	sync();

	//up_write(&cr_rw_sem);
	//spin_unlock(&sdport->lock);

	if (irq_handled)
		return IRQ_HANDLED;	//liao
	else
		return IRQ_NONE;

}

static int rtksd_wait_status(struct mmc_card *card, u8 state, u8 divider,
			     int bIgnore)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	unsigned long timeend;
	int err, bMalloc = 0;
	struct mmc_host *mmc = mmc_host_local;

	MMCPRINTF("\n");
	timeend = jiffies + msecs_to_jiffies(100);	/* wait 100ms */

	if (card == NULL) {
		bMalloc = 1;
		card =
		    (struct mmc_card *)kmalloc(sizeof(struct mmc_card),
					       GFP_KERNEL);
		card->host = mmc;
	}

	do {
		memset(&cmd, 0, sizeof(struct mmc_command));
		memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

		set_cmd_info(card, &cmd, &cmd_info,
			     MMC_SEND_STATUS,
			     (card->rca) << RCA_SHIFTER, SD_R1 | divider);
		err = SD_SendCMDGetRSP_Cmd(&cmd_info, bIgnore);

		if (err) {
			MMCPRINTF("wait %s fail\n", state_tlb[state]);
			break;
		} else {
			u8 cur_state = R1_CURRENT_STATE(cmd.resp[0]);
			MMCPRINTF("resp[0]=0x%08x,cur_state=%s\n", cmd.resp[0],
				  state_tlb[cur_state]);
			err = -1;
			if (cur_state == state) {
				if (cmd.resp[0] & R1_READY_FOR_DATA) {
					err = 0;
					break;
				}
			}
		}

	} while (time_before(jiffies, timeend));

	if (bMalloc) {
		kfree(card);
		card = NULL;
	}
	return err;
}

#ifdef RESERVE
static int rtkcr_send_cmd13(struct rtksd_host *sdport, u8 *state)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	int err = 0;

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	cmd.opcode = MMC_SEND_STATUS;
	cmd.arg = (1 << RCA_SHIFTER);
	cmd_info.cmd = &cmd;
	cmd_info.sdport = sdport;
	cmd_info.rsp_para2 = SD_R1;
	cmd_info.rsp_len = rtksd_get_rsp_len(SD_R1);

	gPreventRetry = 1;
	err = SD_SendCMDGetRSP(&cmd_info);
	gPreventRetry = 0;

	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_SEND_STATUS fail\n", DRIVER_NAME);
	} else {
		u8 cur_state = R1_CURRENT_STATE(cmd.resp[0]);
		*state = cur_state;
		mmcmsg1("cur_state=%s\n", state_tlb[cur_state]);
	}

	return err;
}
#endif
static int rtksd_send_status(struct mmc_card *card, u8 *state, u8 divider,
			     int bIgnore)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	int err = 0;
	int bMalloc = 0;
	struct mmc_host *mmc = mmc_host_local;

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	if (card == NULL) {
		bMalloc = 1;
		card =
		    (struct mmc_card *)kmalloc(sizeof(struct mmc_card),
					       GFP_KERNEL);
		card->host = mmc;
	}

	set_cmd_info(card, &cmd, &cmd_info,
		     MMC_SEND_STATUS,
		     (card->rca) << RCA_SHIFTER, SD_R1 | divider);
	err = SD_SendCMDGetRSP(&cmd_info);

	if (bMalloc) {
		kfree(card);
		card = NULL;
	}

	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_SEND_STATUS fail\n", DRIVER_NAME);
	} else {
		u8 cur_state = R1_CURRENT_STATE(cmd.resp[0]);
		*state = cur_state;
		MMCPRINTF(KERN_INFO "cur_state=%s\n", state_tlb[cur_state]);
	}

	return err;
}

static int rtksd_switch_user_partition(struct mmc_card *card)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	int err = 0;

	MMCPRINTF("\n");

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	set_cmd_info(card, &cmd, &cmd_info, MMC_SWITCH, 0x03b30001, SD_R1b);
	err = SD_SendCMDGetRSP_Cmd(&cmd_info, 0);

	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_SWITCH fail\n", DRIVER_NAME);
	}
	return err;

}

#ifdef CONFIG_MMC_RTKEMMC_HK_ATTR
//#define HACK_BOOT_PART_RW

#define NORMAL_PART 0
#define BOOT1_PART  1
#define BOOT2_PART  2
#define GP1_PART    3
#define GP2_PART    4
#define GP3_PART    5
#define GP4_PART    6

#ifdef HACK_BOOT_PART_RW
static int rtksd_switch_partition(struct mmc_card *card, u8 acc_part)
{

	rtksd_switch(card,
		     MMC_SWITCH_MODE_WRITE_BYTE,
		     EXT_CSD_PART_CONFIG, acc_part, EXT_CSD_CMD_SET_NORMAL);

	return 0;

}
#endif

static int rtksd_switch(struct mmc_card *card,
			u8 acc_mod, u8 index, u8 value, u8 cmd_set)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	u32 arg = 0;
	int err = 0;
	int bMalloc = 0;
	struct mmc_host *mmc = mmc_host_local;

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	if (card == NULL) {
		bMalloc = 1;
		card =
		    (struct mmc_card *)kmalloc(sizeof(struct mmc_card),
					       GFP_KERNEL);
		card->host = mmc;
	}

	arg = (acc_mod << 24) | (index << 16) | (value << 8) | (cmd_set);

	MMCPRINTF("%s : arg=0x%08x\n", __func__, arg);
	set_cmd_info(card, &cmd, &cmd_info, MMC_SWITCH, arg, SD_R1b);

	err = SD_SendCMDGetRSP_Cmd(&cmd_info, 0);
	if (bMalloc) {
		kfree(card);
		card = NULL;
	}

	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_SWITCH fail\n", DRIVER_NAME);
	}
	return err;

}
#endif

/****************************************************************
 *  tuning area
****************************************************************/
void mmc_CRT_reset(struct rtksd_host *sdport)
{
	void *iobase = sdport->base;

	if (!sdport)
		BUG();

	rtkemmc_backup_registers(sdport);
	/* 97F no need
	   cr_writel(cr_readl(SYS_SOFT_RESET2)&((u32)~(1<<11)), SYS_SOFT_RESET2);          //reset emmc
	   cr_writel(cr_readl(SYS_CLOCK_ENABLE1)&((u32)~(1<<24)), SYS_CLOCK_ENABLE1);      //disable emmc clk
	   udelay(100);
	   cr_writel(cr_readl(SYS_CLOCK_ENABLE1)|(1<<24), SYS_CLOCK_ENABLE1);      //disable emmc clk
	   cr_writel(cr_readl(SYS_SOFT_RESET2)|(1<<11), SYS_SOFT_RESET2);          //reset emmc
	 */

	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL3, CLK_EN_SD30, ~CLK_EN_SD30);
	udelay(100);
	rtl_reg_mask(sdport->pll_base + SSCPLL_CTRL3, CLK_EN_SD30, CLK_EN_SD30);

	rtl_reg_mask(iobase + EMMC_SRAM_CTL, MAP_SEL, MAP_SEL);

	rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0xff, 0xff, 0xff);
	rtkemmc_restore_registers(sdport);
	//sync();
}

void mmc_phase_adjust(struct rtksd_host *sdport, u32 VP0, u32 VP1)
{
	void *iobase = sdport->base;

//phase selection
	if ((VP0 == 0xff) & (VP1 == 0xff)) {
#ifdef MMC_DEBUG
		MMCPRINTF("Phase VP0 and VP1 no change \n");
#endif
	} else if ((VP0 != 0xff) & (VP1 == 0xff)) {
#ifdef MMC_DEBUG
		MMCPRINTF("Phase VP0=%x, VP1 no change \n", VP0);
#endif
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) | 0x70000, iobase + EMMC_CKGEN_CTL);	//change clk to 4Mhz
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xfffffffd, sdport->pll_base + SSCPLL_CTRL0);	//reset pll
		cr_writel(((cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xffffff07) | (VP0 << 3)), sdport->pll_base + SSCPLL_CTRL0);	//vp0 phase:0x0~0x1f
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) | 0x2, sdport->pll_base + SSCPLL_CTRL0);	//release reset pll
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) & 0xfff8ffff, iobase + EMMC_CKGEN_CTL);	//change clock to PLL
		cr_writel(cr_readl(iobase + SD_CONFIGURE1) & 0xef,
			  iobase + SD_CONFIGURE1);
	} else if ((VP0 == 0xff) & (VP1 != 0xff)) {
#ifdef MMC_DEBUG
		MMCPRINTF("Phase VP0 no change, VP1=%x \n", VP1);
#endif
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) | 0x70000, iobase + EMMC_CKGEN_CTL);	//change clk to 4Mhz
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xfffffffd, sdport->pll_base + SSCPLL_CTRL0);	//reset pll
		cr_writel(((cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xffffe0ff) | (VP1 << 8)), sdport->pll_base + SSCPLL_CTRL0);	//vp0 phase:0x0~0x1f
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) | 0x2, sdport->pll_base + SSCPLL_CTRL0);	//release reset pll
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) & 0xfff8ffff, iobase + EMMC_CKGEN_CTL);	//change clock to PLL
		cr_writel(cr_readl(iobase + SD_CONFIGURE1) & 0xef,
			  iobase + SD_CONFIGURE1);
	} else {
#ifdef MMC_DEBUG
		MMCPRINTF("Phase VP0=%x, VP1=%x \n", VP0, VP1);
#endif
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) | 0x70000, iobase + EMMC_CKGEN_CTL);	//change clk to 4Mhz
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xfffffffd, sdport->pll_base + SSCPLL_CTRL0);	//reset pll
		cr_writel(((cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xffffff07) | (VP0 << 3)), sdport->pll_base + SSCPLL_CTRL0);	//vp0 phase:0x0~0x1f
		cr_writel(((cr_readl(sdport->pll_base + SSCPLL_CTRL0) & 0xffffe0ff) | (VP1 << 8)), sdport->pll_base + SSCPLL_CTRL0);	//vp1 phase:0x0~0x1f
		cr_writel(cr_readl(sdport->pll_base + SSCPLL_CTRL0) | 0x2, sdport->pll_base + SSCPLL_CTRL0);	//release reset pll
		cr_writel(cr_readl(iobase + EMMC_CKGEN_CTL) & 0xfff8ffff, iobase + EMMC_CKGEN_CTL);	//change clock to PLL
		cr_writel(cr_readl(iobase + SD_CONFIGURE1) & 0xef,
			  iobase + SD_CONFIGURE1);
	}
	udelay(300);
	//sync();
}

int SEARCH_BEST(u32 window)
{
	int i, j, k, max;
	int window_temp[32];
	int window_start[32];
	int window_end[32];
	int window_max = 0;
	int window_best = 0;
	int parse_end = 1;
	for (i = 0; i < 0x20; i++) {
		window_temp[i] = 0;
		window_start[i] = 0;
		window_end[i] = -1;
	}
	j = 1;
	i = 0;
	k = 0;
	max = 0;
	while ((i < 0x1f) && (k < 0x1f)) {
		parse_end = 0;
		for (i = window_end[j - 1] + 1; i < 0x20; i++) {
			if (((window >> i) & 1) == 1) {
				window_start[j] = i;
//                              parse_end=1;
//                              printk("window_start=0x%x \n", window_start[j]);
				break;
			}
		}
//              printk("i=0x%x \n", i);
		if (i == 0x20) {
			break;
		}
		for (k = window_start[j] + 1; k < 0x20; k++) {
			if (((window >> k) & 1) == 0) {
				window_end[j] = k - 1;
				parse_end = 1;
				//                      printk("test \n");
				break;
			}
		}
		if (parse_end == 0) {
			window_end[j] = 0x1f;
		}
//                      printk("window_end=0x%x \n", window_end[j]);
		j++;
	}
	for (i = 1; i < j; i++) {
		window_temp[i] = window_end[i] - window_start[i] + 1;
//              printk("window temp=0x%x \n", window_temp[i]);
	}
	if (((window & 1) == 1) && (((window >> 0x1f) & 1) == 1)) {
		window_temp[1] = window_temp[1] + window_temp[j - 1];
		window_start[1] = window_start[j - 1];
//              printk("merge \n");
	}
	for (i = 1; i < j; i++) {
		if (window_temp[i] > window_max) {
			window_max = window_temp[i];
			max = i;
		}
	}
	if ((((window & 1) == 1) && (((window >> 0x1f) & 1) == 1))
	    && (max == 1)) {
		window_best =
		    ((window_start[max] + window_end[max] + 0x20) / 2) & 0x1f;

	} else {
		window_best =
		    ((window_start[max] + window_end[max]) / 2) & 0x1f;
	}
	MMCPRINTF("window start=0x%x \n", window_start[max]);
	MMCPRINTF("window end=0x%x \n", window_end[max]);
	MMCPRINTF("window best=0x%x \n", window_best);
	return window_best;
}

#ifdef RESERVE
static int mmc_ds_delay_tuning(struct rtksd_host *host)
{
	int i;
	int ds_best = 0xff;
	u64 ds_window = ~(0);

	for (i = 0; i < 64; i++) {
		cr_writeb(i, host->base + DS_DELAY_CFG);
		MMCPRINTF("phase =0x%x \n", i);

		if (rtkcr_send_cmd18(host) != 0) {
		//if (mmc_send_tuning(host->mmc, MMC_SEND_TUNING_BLOCK_HS200, NULL) != 0) {
			ds_window = ds_window & (~((u64)1 << i));
		}
	}

	ds_best = SEARCH_BEST(ds_window);
	dev_info(host->dev, "%s: DS_WINDOW=0x%llx, ds_best=0x%x\n",
	       __func__, ds_window, ds_best);

	if (ds_window == 0x0) {
		dev_err(host->dev, "%s: Data Strobe tuning fail\n", __func__);
		printk("SD_CONFIGURE1 = 0x%x\n", cr_readb(host->base + SD_CONFIGURE1));
		printk("SD_SAMPLE_POINT = 0x%x\n", cr_readb(host->base + SD_SAMPLE_POINT_CTL));
		printk("SD_PUSH_POINT = 0x%x\n", cr_readb(host->base + SD_PUSH_POINT_CTL));
		printk("SD_AUTO_RST_FIFO = 0x%x\n", cr_readb(host->base + SD_AUTO_RST_FIFO));
		printk("DS_DELAY_CFG = 0x%x\n", cr_readb(host->base + DS_DELAY_CFG));
		return -EINVAL;
	}
	cr_writeb(ds_best, host->base + DS_DELAY_CFG);

	return 0;
}

static int mmc_rx_phase_tuning(struct rtksd_host *host)
{
	int i;
	int rx_best = 0xff;
	u32 rx_window = 0xffffffff;
	u8 cmd_err;

	for (i = 0x0; i < 0x20; i++) {
		mmc_phase_adjust(host, 0xff, i);
		MMCPRINTF("phase =0x%x \n", i);

		//if (rtkcr_send_cmd18(host) != 0) {
		if (rtkcr_send_cmd13(host, &cmd_err) != 0) {
		//if (mmc_send_tuning(host->mmc, MMC_SEND_TUNING_BLOCK_HS200, NULL)) {
			rx_window = rx_window & (~(1 << i));
		}
	}

	rx_best = SEARCH_BEST(rx_window);
	dev_info(host->dev, "%s: RX_WINDOW=0x%x, rx_best=0x%x\n",
	       __func__, rx_window, rx_best);

	if (rx_window == 0x0) {
		dev_err(host->dev, "%s: RX tuning fail\n", __func__);
		return -EINVAL;
	}
	mmc_phase_adjust(host, 0xff, rx_best);

	return 0;
}

static int mmc_tx_phase_tuning(struct rtksd_host *host)
{
	int i;
	int tx_best = 0xff;
	u32 tx_window = 0xffffffff;
	u8 cmd_err;

	for (i = 0x0; i < 0x20; i++) {
		mmc_phase_adjust(host, i, 0xff);
		MMCPRINTF("phase =0x%x \n", i);

		//if(mmc_send_cmd21() == 99)
		//if (rtkcr_send_cmd25(host) != 0) {
		if (rtkcr_send_cmd13(host, &cmd_err) != 0) {
			tx_window = tx_window & (~(1 << i));
		}
	}

	tx_best = SEARCH_BEST(tx_window);
	dev_info(host->dev, "%s: TX_WINDOW=0x%x, tx_best=0x%x\n",
	       __func__, tx_window, tx_best);

	if (tx_window == 0x0) {
		dev_err(host->dev, "%s: TX tuning fail\n", __func__);
		return -EINVAL;
	}

	if (gCurrentBootMode == MODE_HS400) {
		dev_info(host->dev, "Current mode is HS400, shift forward half cycle\n");
		tx_best = tx_best - 8;
	}
	mmc_phase_adjust(host, tx_best, 0xff);

	return 0;
}

static void mmc_phase_tuning(struct rtksd_host *host)
{
	mmc_phase_adjust(host, 0, 0);
	mdelay(5);
	//sync();
	MMCPRINTF("============== Start TX Tuning ===============\n");
	mmc_tx_phase_tuning(host);

	MMCPRINTF("++++++++++++++ Start RX Tuning +++++++++++++++\n");
	mmc_rx_phase_tuning(host);

}
#endif

static int mmc_tuning_hs400(struct rtksd_host *sdport)
{
	if (!g_bResuming)
		gCurrentBootMode = MODE_HS400;

	dev_info(sdport->dev, "HS400: starting tuning process.\n");
	//sdport->ops->set_clk(sdport, CLK_200MHZ);
	rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0xff, 0xff, 0xff);

	cr_writeb(10, sdport->base + DS_DELAY_CFG);
	//mmc_ds_delay_tuning(sdport);
	dev_info(sdport->dev, "Current mode is HS400, shift forward half cycle\n");
	mmc_phase_adjust(sdport, 0x1f - 8, 0x1);
	//mmc_phase_tuning(sdport);

	mmc_CRT_reset(sdport);

	return 0;
}

static int mmc_tuning_hs200(struct rtksd_host *sdport)
{
	if (!g_bResuming)
		gCurrentBootMode = MODE_SD30;

	dev_info(sdport->dev, "HS200: starting tuning process.\n");
	sdport->ops->set_clk(sdport, CLK_200MHZ);
	rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0xff, 0xff, 0xff);

	mmc_phase_adjust(sdport, 0x1f, 0x1);
	//mmc_phase_tuning(sdport);
	mmc_CRT_reset(sdport);

	return 0;
}

static int mmc_tuning_ddr50(struct rtksd_host *sdport)
{
	volatile u32 TX_window = 0xffffffff;
	volatile int TX_best = 0xff;
	volatile u32 RX_window = 0xffffffff;
	volatile int RX_best = 0xff;
	int i = 0;

	//set current boot mode
	if (!g_bResuming)
		gCurrentBootMode = MODE_DDR;
	return 0; //fix me
	printk("[LY]ddr50 gCurrentBootMode =%d\n", gCurrentBootMode);

	sdport->ops->set_clk(sdport, CLK_100MHZ);
	rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0x66, 0x64, 0x66);
	cr_writeb(0xa8, sdport->base + SD_SAMPLE_POINT_CTL);
	//Using phase-shift clock for DDR mode command/data sample point selection

	cr_writeb(0x90, sdport->base + SD_PUSH_POINT_CTL);
	//Using phase-shift clock for DDR mode command/data push point selection
	mdelay(5);
	//sync();
	MMCPRINTF("==============Start DDR50 TX Tuning ==================\n");
	for (i = 0x0; i < 0x20; i++) {
		mmc_phase_adjust(sdport, i, 0xff);
#ifdef MMC_DEBUG
		MMCPRINTF("phase (%d) - VP=0x%08x\n", i, REG32(sdport->pll_base + SSCPLL_CTRL0));
#endif
		if (rtkcr_send_cmd25(sdport) != 0) {
			TX_window = TX_window & (~(1 << i));
		}
	}
	TX_best = SEARCH_BEST(TX_window);
	MMCPRINTF("TX_WINDOW= 0x%x \n", TX_window);
	MMCPRINTF("TX_best= 0x%x \n", TX_best);
	printk(KERN_INFO "[%s:DDR50] TX_WINDOW=0x%x, TX_best=0x%x \n",
	       DRIVER_NAME, TX_window, TX_best);
	if (TX_window == 0x0) {
		pr_warn("[%s:DDR50] TX tuning fail \n",
		       DRIVER_NAME);
		return -1;
	}
	mmc_phase_adjust(sdport, TX_best, 0xff);

	//sync();
	MMCPRINTF("++++++++++++++Start DDR50 RX Tuning ++++++++++++++++++\n");
	i = 0;
	for (i = 0x0; i < 0x20; i++) {
		mmc_phase_adjust(sdport, 0xff, i);
#ifdef MMC_DEBUG
		MMCPRINTF("phase =0x%x \n", i);
#endif
		//if(rtksd_send_cmd8(sdport, 0)!=0)
		if (rtkcr_send_cmd18(sdport) != 0) {
			RX_window = RX_window & (~(1 << i));
		}
	}
	RX_best = SEARCH_BEST(RX_window);
	MMCPRINTF("RX_best= 0x%x \n", RX_best);
	printk(KERN_INFO "[%s:DDR50] RX_WINDOW=0x%x, RX_best=0x%x \n",
	       DRIVER_NAME, RX_window, RX_best);
	//sync();
	if (RX_window == 0x0) {
		pr_warn("[%s:DDR50] RX tuning fail \n",
		       DRIVER_NAME);
		return -2;
	}
	mmc_phase_adjust(sdport, 0xff, RX_best);
	return 0;
}

static int mmc_tuning_sdr50(struct rtksd_host *sdport)
{
	int err = 0;
	void *iobase = sdport->base;

	if (!g_bResuming)
		gCurrentBootMode = MODE_SD20;
	return 0; //fix me
	MMCPRINTF("[LY]sdr gCurrentBootMode =%d\n", gCurrentBootMode);

	cr_writeb(0x0, sdport->base + SD_SAMPLE_POINT_CTL);	//sample point = SDCLK / 4
	cr_writeb(0x0, sdport->base + SD_PUSH_POINT_CTL);	//output ahead SDCLK /4
	sdport->ops->set_clk(sdport, CLK_100MHZ);
	//for sdr50, don't set pad driving
	//rtkcr_set_pad_driving(sdport,MMC_IOS_SET_PAD_DRV, 0,0,0);
	rtkcr_set_div(sdport, EMMC_CLOCK_DIV_NON);
	//sync();
	mdelay(5);
	MMCPRINTF("==============Select SDR50 Push Point ==================\n");
	if ((err = rtkcr_send_cmd25(sdport)) == 0) {
		MMCPRINTF("Output at FALLING edge of SDCLK \n");
	} else {
		cr_writeb(0x10, iobase + SD_PUSH_POINT_CTL);	//Push point =output at falling edge
		//sync();
		//if ((err = rtkcr_send_cmd13(sdport,&ret_state))==0)
		if ((err = rtkcr_send_cmd25(sdport)) == 0) {
			MMCPRINTF("Output is ahead by 1/4 SDCLK period \n");
		} else {
			pr_err("sdr tuning : No good push point \n");
			return -1;
		}
	}

	MMCPRINTF
	    ("++++++++++++++Select SDR50 Sample Point ++++++++++++++++++\n");
	if ((err = rtkcr_send_cmd18(sdport)) == 0) {
		MMCPRINTF("Sample at RISING edge of SDCLK \n");
	} else {
		cr_writeb(8, iobase + SD_SAMPLE_POINT_CTL);	//sample point is delayed by 1/4 SDCLK period
		//sync();
		if ((err = rtkcr_send_cmd18(sdport)) == 0) {
			MMCPRINTF
			    ("sample point is delayed by 1/4 SDCLK period \n");
		} else {
			pr_err("sdr tuning : No good Sample point \n");
			return -2;
		}
	}
	//sync();
	MMCPRINTF(KERN_INFO "SDR select (sample/push) : 0x%02x/0x%02x\n",
		  cr_readb(sdport->base + SD_SAMPLE_POINT_CTL),
		  cr_readb(sdport->base + SD_PUSH_POINT_CTL));

	return 0;
}

void sample_ctl_sdr(struct rtksd_host *sdport, u8 status)
{
	void *base = sdport->base;

	switch (sdport->clk_shft) {
	case 0:
		cr_writeb(SP_SDR_DEFAULT, base + SD_SAMPLE_POINT_CTL);
		cr_writeb(PP_SDR_AHEAD, base + SD_PUSH_POINT_CTL);
		break;
	case 1:
		cr_writeb(SP_SDR_DELAY, base + SD_SAMPLE_POINT_CTL);
		cr_writeb(PP_SDR_AHEAD, base + SD_PUSH_POINT_CTL);
		break;
	case 2:
		cr_writeb(SP_SDR_DELAY, base + SD_SAMPLE_POINT_CTL);
		cr_writeb(PP_SDR_DEFAULT, base + SD_PUSH_POINT_CTL);
		break;
	case 3:
		cr_writeb(SP_SDR_DEFAULT, base + SD_SAMPLE_POINT_CTL);
		cr_writeb(PP_SDR_DEFAULT, base + SD_PUSH_POINT_CTL);
		break;
	}

	sdport->clk_shft = (sdport->clk_shft + 1) % 4;

	dev_info(sdport->dev, "%s: new point s:0x%02x, p:0x%02x\n", __func__,
			cr_readb(base + SD_SAMPLE_POINT_CTL),
			cr_readb(base + SD_PUSH_POINT_CTL));
}

void sample_ctl_ddr(struct rtksd_host *sdport, u8 status)
{
	void *base = sdport->base;
	u8 val;

	if (status & CRC7_STATUS) {
		val = cr_readb(base + SD_SAMPLE_POINT_CTL);
		val = val ^ SP_DDR_CMD_DELAY;
		cr_writeb(val, base + SD_SAMPLE_POINT_CTL);
	} else if (status & CRC16_STATUS) {
		val = cr_readb(base + SD_SAMPLE_POINT_CTL);
		val = val ^ SP_DDR_DATA_DELAY;
		cr_writeb(val, base + SD_SAMPLE_POINT_CTL);
	} else if (status & WRT_ERR_BIT) {
		val = cr_readb(base + SD_PUSH_POINT_CTL);
		val = val ^ PP_DDR_DATA_AHEAD;
		cr_writeb(val, base + SD_PUSH_POINT_CTL);
	} else {
		val = cr_readb(base + SD_PUSH_POINT_CTL);
		val = val ^ PP_DDR_CMD_AHEAD;
		cr_writeb(val, base + SD_PUSH_POINT_CTL);
	}

	dev_info(sdport->dev, "%s: new point s:0x%02x, p:0x%02x\n", __func__,
			cr_readb(base + SD_SAMPLE_POINT_CTL),
			cr_readb(base + SD_PUSH_POINT_CTL));
}

int sample_ctl_switch(struct rtksd_host *sdport, int cmd_idx, int bIgnore, u8 status)
{
	int err = 0;
	int mode_val = 0;
	void *iobase = sdport->base;

	mode_val = rtkcr_get_mode_selection(sdport);
	pr_info("cur point: s:0x%02x, p:0x%02x, mode=0x%02x\n",
		  cr_readb(sdport->base + SD_SAMPLE_POINT_CTL),
		  cr_readb(sdport->base + SD_PUSH_POINT_CTL), mode_val);

	if (mode_val == MODE_SD20) {
		sample_ctl_sdr(sdport, status);
	} else {
		sample_ctl_ddr(sdport, status);
	}

	mdelay(5);
	cr_writeb(0x14, iobase + CR_CARD_STOP);
	mdelay(5);

	if ((cmd_idx != MMC_SET_BLOCKLEN) && (cmd_idx > MMC_SET_RELATIVE_ADDR)) {
		err = polling_to_tran_state(sdport, cmd_idx, bIgnore);
#if 0
		while (retry_cnt-- && err) {
			err =
			    rtksd_send_status(host->card, &ret_state, 0,
					      bIgnore);
		}
		if (ret_state == STATE_DATA) {
#ifdef MMC_DEBUG
			MMCPRINTF("before send stop\n");
#endif
			mmc_send_stop(host->card);
			err = mmccr_wait_status(STATE_TRAN);
#ifdef MMC_DEBUG
			MMCPRINTF("before send stop: err=%d\n", err);
#endif
		} else if (ret_state == STATE_RCV) {
#ifdef MMC_DEBUG
			MMCPRINTF("before send stop\n");
#endif
			mmc_send_stop(host->card);
			err = mmccr_wait_status(STATE_TRAN);
#ifdef MMC_DEBUG
			MMCPRINTF("before send stop: err=%d\n", err);
#endif
		}
#endif
	}

	return err;
}

/****************************************************************
 *  error handling area
 *
****************************************************************/
int error_handling(struct rtksd_host *sdport, unsigned int cmd_idx,
		   unsigned int bIgnore)
{
	void *iobase = sdport->base;
	unsigned char sts1_val = 0;
	int err = 0;

	sts1_val = cr_readb(iobase + SD_STATUS1);
	MMCPRINTF(KERN_INFO
		  "%s : status1 val=%02x, cmd_idx=0x%02x, gCurrentBootMode=0x%02x\n",
		  __func__, sts1_val, cmd_idx, gCurrentBootMode);
	host_card_stop(sdport);
	if (cmd_idx > MMC_SET_RELATIVE_ADDR) {
#if 0
		if ((sts1_val & WRT_ERR_BIT) || (sts1_val & CRC16_STATUS)
		    || (sts1_val & CRC7_STATUS)) {
			if ((cmd_idx != MMC_READ_SINGLE_BLOCK)
			    && (cmd_idx != MMC_WRITE_BLOCK))
				rtksd_stop_transmission(host->card, bIgnore);
		}
#endif
		polling_to_tran_state(sdport, cmd_idx, bIgnore);
	}

	if (bIgnore)
		return 0;
	//till here, we are good to go
	if (gCurrentBootMode > MODE_DDR) {
		MMCPRINTF(KERN_INFO "change mode from %d to %d ---> \n",
			  gCurrentBootMode, MODE_SD20);
		//flush_cache((unsigned long)g_ext_csd, CSD_ARRAY_SIZE);
#ifdef MMC_DEBUG
		mmc_show_ext_csd(g_ext_csd);
#endif
		if (gCurrentBootMode == MODE_SD30) {
			//TBD, way to change to HS / SDR
			//ddr50
			//err = mmc_select_ddr50(mmc,g_ext_csd);
			//mmc->boot_caps &= ~MMC_MODE_HSDDR_52MHz;
		} else {	//sdr50
			//err = mmc_select_sdr50(mmc,g_ext_csd);
			//mmc->boot_caps &= ~MMC_MODE_HS200;
		}
		if (err)
			printk(KERN_ERR " change mode result ==> fail\n");
		else {
			printk(KERN_INFO
			       " change mode result ==> successful\n");
		}
	} else {
		MMCPRINTF("%s : cmd_idx = 0x%x, bIgnore = %u\n", __func__,
			  cmd_idx, bIgnore);
		err = sample_ctl_switch(sdport, cmd_idx, bIgnore, sts1_val);
	}
	if (err)
		printk(KERN_ERR "[LY] error handling fail\n");
	return err;
}

int polling_to_tran_state(struct rtksd_host *sdport, unsigned int cmd_idx,
			  int bIgnore)
{
	int err = 1, retry_cnt = 5;
	u8 ret_state = 0;
	struct mmc_host *host = sdport->mmc;

	MMCPRINTF("%s : \n", __func__);

	err = 1;
	retry_cnt = 3;
	while (retry_cnt-- && err) {
		err = rtksd_send_status(host->card, &ret_state, 0, bIgnore);
	}
	if (ret_state == STATE_DATA) {
#ifdef MMC_DEBUG
		printk("before send stop\n");
#endif
		if ((cmd_idx != MMC_READ_SINGLE_BLOCK)
		    && (cmd_idx != MMC_WRITE_BLOCK))
			rtksd_stop_transmission(host->card, 1);
		err = rtksd_wait_status(host->card, STATE_TRAN, 0, bIgnore);
#ifdef MMC_DEBUG
		MMCPRINTF("before send stop: err=%d\n", err);
#endif
	} else if (ret_state == STATE_RCV) {
#ifdef MMC_DEBUG
		MMCPRINTF("before send stop\n");
#endif
		if ((cmd_idx != MMC_READ_SINGLE_BLOCK)
		    && (cmd_idx != MMC_WRITE_BLOCK))
			rtksd_stop_transmission(host->card, 1);
		err = rtksd_wait_status(host->card, STATE_TRAN, 0, bIgnore);
#ifdef MMC_DEBUG
		MMCPRINTF("before send stop: err=%d\n", err);
#endif
	}
	return err;
}

/*
//int rtkcr_send_cmd8(struct sd_cmd_pkt * cmd_info, unsigned int bIgnore)
int rtkcr_send_cmd8(struct rtksd_host *sdport, unsigned int bIgnore)
{
	int ret_err=0;
	struct sd_cmd_pkt cmd_info;
	struct mmc_host *host = sdport->mmc;
	volatile int err=1,retry_cnt=5;
	int rsp_para1, rsp_para2, rsp_para3;
	unsigned char cfg3=0,cfg1=0;
	int sts1_val=0;
	unsigned char *crd_ext_csd=NULL;
	u32 iobase = sdport->base;
	struct mmc_data *data=NULL;
	struct mmc_command *cmd=NULL;

	memset(&cmd_info, 0x00, sizeof(struct sd_cmd_pkt));

	crd_ext_csd = rtksd_get_buffer_start_addr();
	if (crd_ext_csd == NULL)
	{
		printk(KERN_ERR "%s,%s : crd_ext_csd == NULL\n",DRIVER_NAME,__func__);
		return -5;
	}

	rsp_para1 = 0;
	rsp_para2 = 0;
	rsp_para3 = 0;
	cfg3 = cr_readb(iobase+SD_CONFIGURE3)|SD_CMD_RSP_TO;
	cfg1 = cr_readb(iobase+SD_CONFIGURE1)&(MASK_CLOCK_DIV|MASK_BUS_WIDTH|MASK_MODE_SELECT);
	rsp_para1 = cfg1|SD1_R0;
	rsp_para2 = SD_R1;
	rsp_para3 = cfg3;

	if (cmd_info.cmd == NULL)
	{
		cmd  = (struct mmc_command*) kmalloc(sizeof(struct mmc_command),GFP_KERNEL);
		memset(cmd, 0x00, sizeof(struct mmc_command));
		cmd_info.cmd  = (struct mmc_command*) cmd;
	}
	cmd_info.sdport = sdport;
	cmd_info.cmd->arg=0;
	cmd_info.cmd->opcode = MMC_SEND_EXT_CSD;
	cmd_info.rsp_para1	  = rsp_para1;
	cmd_info.rsp_para2	  = rsp_para2;
	cmd_info.rsp_para3	  = rsp_para3;
	cmd_info.rsp_len	 = rtksd_get_rsp_len(rsp_para2);
	cmd_info.byte_count  = 0x200;
	cmd_info.block_count = 1;
	cmd_info.dma_buffer = crd_ext_csd;
	if (cmd_info.cmd->data == NULL)
	{
		data  = (struct mmc_data*) kmalloc(sizeof(struct mmc_data),GFP_KERNEL);
		memset(data, 0x00, sizeof(struct mmc_data));
		cmd_info.cmd->data = data;
		data->flags = MMC_DATA_READ;
	}
	else
		cmd_info.cmd->data->flags = MMC_DATA_READ;
	sync();
	MMCPRINTF("\n*** %s %s %d, cmdidx=0x%02x(%d), resp_type=0x%08x, host=0x%08x, card=0x%08x -------\n", \
		__FILE__, __FUNCTION__, __LINE__, cmd_info.cmd->opcode, cmd_info.cmd->opcode, cmd_info.cmd->flags, host, host->card);
	ret_err = SD_Stream_Cmd( EMMC_NORMALREAD, &cmd_info, bIgnore);
	if (ret_err)
	{
		if (bIgnore)
		{
			goto err8;
		}
		sts1_val = cr_readb(iobase+SD_STATUS1);
		MMCPRINTF("[LY] status1 val=%02x\n", sts1_val);
		host_card_stop(sdport);
		if ((sts1_val&CRC16_STATUS)||(sts1_val&CRC7_STATUS))
		{
			if ((cmd_info.cmd->opcode!= MMC_READ_SINGLE_BLOCK)&&(cmd_info.cmd->opcode != MMC_WRITE_BLOCK))
				rtksd_stop_transmission(host->card,1);
		}
		polling_to_tran_state(sdport,cmd_info.cmd->opcode,1);
        }
err8:
	if (cmd)
	{
		cmd_info.cmd = NULL;
		kfree(cmd);
		cmd=NULL;
	}
	if (data)
	{
		//cmd_info->cmd->data = NULL;
		kfree(data);
		data=NULL;
	}
	sync();
	return ret_err;
}*/
int rtkcr_send_cmd18(struct rtksd_host *sdport)
{
	int ret_err = 0;
	struct sd_cmd_pkt cmd_info;
	struct mmc_host *host = sdport->mmc;
	int sts1_val = 0;
	unsigned char *crd_tmp_buffer = NULL;
	void *iobase = sdport->base;
	struct mmc_data *data = NULL;
	struct mmc_command *cmd = NULL;

	memset(&cmd_info, 0x00, sizeof(struct sd_cmd_pkt));

	crd_tmp_buffer = rtksd_get_buffer_start_addr();
	if (crd_tmp_buffer == NULL) {
		printk(KERN_ERR "%s,%s : crd_ext_csd == NULL\n", DRIVER_NAME,
		       __func__);
		return -5;
	}

	if (cmd_info.cmd == NULL) {
		cmd =
		    (struct mmc_command *)kmalloc(sizeof(struct mmc_command),
						  GFP_KERNEL);
		memset(cmd, 0x00, sizeof(struct mmc_command));
		cmd_info.cmd = (struct mmc_command *)cmd;
	}
	cmd_info.sdport = sdport;
	cmd_info.cmd->arg = 0x100;
	cmd_info.cmd->opcode = MMC_READ_MULTIPLE_BLOCK;
	cmd_info.rsp_para1 = -1;
	cmd_info.rsp_para2 = SD_R1;
	cmd_info.rsp_para3 = -1;
	cmd_info.rsp_len = rtksd_get_rsp_len(SD_R1);
	cmd_info.byte_count = 0x200;
	cmd_info.block_count = 2;
	cmd_info.dma_buffer = crd_tmp_buffer;
	if (cmd_info.cmd->data == NULL) {
		data =
		    (struct mmc_data *)kmalloc(sizeof(struct mmc_data),
					       GFP_KERNEL);
		memset(data, 0x00, sizeof(struct mmc_data));
		cmd_info.cmd->data = data;
		data->flags = MMC_DATA_READ;
	} else
		cmd_info.cmd->data->flags = MMC_DATA_READ;
	MMCPRINTF
	    ("\n*** %s %s %d, cmdidx=0x%02x(%d), resp_type=0x%08x, host=0x%08x, card=0x%08x -------\n",
	     __FILE__, __FUNCTION__, __LINE__, cmd_info.cmd->opcode,
	     cmd_info.cmd->opcode, cmd_info.cmd->flags, host, host->card);
	ret_err = SD_Stream_Cmd(EMMC_AUTOREAD1, &cmd_info, 1);
	if (ret_err) {
		sts1_val = cr_readb(iobase + SD_STATUS1);
		MMCPRINTF("[LY] status1 val=%02x\n", sts1_val);
		host_card_stop(sdport);
		if ((sts1_val & WRT_ERR_BIT) || (sts1_val & CRC16_STATUS)
		    || (sts1_val & CRC7_STATUS)
		    || (sts1_val & CRC_TIMEOUT_ERR)) {
			if ((cmd_info.cmd->opcode != MMC_READ_SINGLE_BLOCK)
			    && (cmd_info.cmd->opcode != MMC_WRITE_BLOCK))
				rtksd_stop_transmission(host->card, 1);
		}
		polling_to_tran_state(sdport, cmd_info.cmd->opcode, 1);
	}

	if (cmd) {
		kfree(cmd);
		cmd_info.cmd = NULL;
		cmd = NULL;
	}
	if (data) {
		kfree(data);
		//cmd_info.cmd->data = NULL;
		data = NULL;
	}
	return ret_err;
}

int rtkcr_send_cmd25(struct rtksd_host *sdport)
{
	int ret_err = 0, i = 0;
	struct sd_cmd_pkt cmd_info;
	struct mmc_host *host = sdport->mmc;
	int sts1_val = 0;
	char *crd_tmp_buffer = NULL;
	void *iobase = sdport->base;
	struct mmc_data *data = NULL;
	struct mmc_command *cmd = NULL;

	memset(&cmd_info, 0x00, sizeof(struct sd_cmd_pkt));

	crd_tmp_buffer = rtksd_get_buffer_start_addr();
	if (crd_tmp_buffer == NULL) {
		printk(KERN_ERR "%s,%s : crd_ext_csd == NULL\n", DRIVER_NAME,
		       __func__);
		return -5;
	}

	for (i = 0; i < 0x400; i++)
		crd_tmp_buffer[i] = 0;

	//spin_lock_irqsave(&sdport->lock,flags);
	if (cmd_info.cmd == NULL) {
		cmd =
		    (struct mmc_command *)kmalloc(sizeof(struct mmc_command),
						  GFP_KERNEL);
		memset(cmd, 0x00, sizeof(struct mmc_command));
		cmd_info.cmd = (struct mmc_command *)cmd;
	}
	//spin_unlock(&sdport->lock);
	cmd_info.sdport = sdport;
	cmd_info.cmd->arg = 0xfe;
	cmd_info.cmd->opcode = MMC_WRITE_MULTIPLE_BLOCK;
	cmd_info.rsp_para1 = -1;
	cmd_info.rsp_para2 = SD_R1;
	cmd_info.rsp_para3 = -1;
	cmd_info.rsp_len = rtksd_get_rsp_len(SD_R1);
	cmd_info.byte_count = 0x200;
	cmd_info.block_count = 2;
	cmd_info.dma_buffer = crd_tmp_buffer;
	//spin_lock_irqsave(&sdport->lock,flags);
	if (cmd_info.cmd->data == NULL) {
		data =
		    (struct mmc_data *)kmalloc(sizeof(struct mmc_data),
					       GFP_KERNEL);
		memset(data, 0x00, sizeof(struct mmc_data));
		cmd_info.cmd->data = data;
		data->flags = MMC_DATA_WRITE;
	} else
		cmd_info.cmd->data->flags = MMC_DATA_WRITE;
	//spin_unlock(&sdport->lock);
	MMCPRINTF
	    ("\n*** %s %s %d, cmdidx=0x%02x(%d), resp_type=0x%08x, host=0x%08x, card=0x%08x , cmd=0x%08x, data=0x%08x-------\n",
	     __FILE__, __FUNCTION__, __LINE__, cmd_info.cmd->opcode,
	     cmd_info.cmd->opcode, cmd_info.cmd->flags, host, host->card, cmd,
	     data);
	ret_err = SD_Stream_Cmd(EMMC_AUTOWRITE1, &cmd_info, 1);
	if (ret_err) {
		sts1_val = cr_readb(iobase + SD_STATUS1);
		MMCPRINTF("[LY] status1 val=%02x\n", sts1_val);
		host_card_stop(sdport);
		if ((sts1_val & WRT_ERR_BIT) || (sts1_val & CRC16_STATUS)
		    || (sts1_val & CRC7_STATUS)) {
			if ((cmd_info.cmd->opcode != MMC_READ_SINGLE_BLOCK)
			    && (cmd_info.cmd->opcode != MMC_WRITE_BLOCK))
				rtksd_stop_transmission(host->card, 1);
		}
		polling_to_tran_state(sdport, cmd_info.cmd->opcode, 1);
	}

	MMCPRINTF
	    ("\n*** %s %s %d, cmdidx=0x%02x(%d), resp_type=0x%08x, host=0x%08x, card=0x%08x , cmd=0x%08x, data=0x%08x-------\n",
	     __FILE__, __FUNCTION__, __LINE__, cmd_info.cmd->opcode,
	     cmd_info.cmd->opcode, cmd_info.cmd->flags, host, host->card, cmd,
	     data);
#if 1
	//spin_lock_irqsave(&sdport->lock,flags);
	if (cmd) {
		cmd_info.cmd = NULL;
		kfree(cmd);
		cmd = NULL;
	}
	if (data) {
		//cmd_info.cmd->data = NULL;
		kfree(data);
		data = NULL;
	}
	//spin_unlock(&sdport->lock);
#endif
	//sync();
	return ret_err;
}

void host_card_stop(struct rtksd_host *sdport)
{
	MMCPRINTF("[LY] host_card_stop \n");
	mmc_CRT_reset(sdport);
	//sync();
}

/***************************************************************
 *
 ***************************************************************/
static int rtksd_go_idle(struct mmc_card *card)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	int err = 0;

	MMCPRINTF("\n");

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	down_write(&cr_rw_sem);
	bSendCmd0 = 1;
	set_cmd_info(card, &cmd, &cmd_info, MMC_GO_IDLE_STATE, 0x00, SD_R1);
	err = SD_SendCMDGetRSP_Cmd(&cmd_info, 0);
	bSendCmd0 = 0;
	up_write(&cr_rw_sem);

	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_GO_IDLE fail\n", DRIVER_NAME);
	}
	return err;

}

static int rtksd_stop_transmission(struct mmc_card *card, int bIgnore)
{
	struct mmc_command cmd;
	struct sd_cmd_pkt cmd_info;
	int err = 0;
	int bMalloc = 0;
	struct mmc_host *mmc = mmc_host_local;

	MMCPRINTF("%s : \n", __func__);

	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&cmd_info, 0, sizeof(struct sd_cmd_pkt));

	if (card == NULL) {
		bMalloc = 1;
		card =
		    (struct mmc_card *)kmalloc(sizeof(struct mmc_card),
					       GFP_KERNEL);
		card->host = mmc;
	}

	set_cmd_info(card, &cmd, &cmd_info,
		     MMC_STOP_TRANSMISSION, 0x00, SD_R1 | CRC16_CAL_DIS);
	err = SD_SendCMDGetRSP_Cmd(&cmd_info, bIgnore);

	if (bMalloc) {
		kfree(card);
		card = NULL;
	}
	if (err) {
		mmcmsg3(KERN_WARNING "%s: MMC_STOP_TRANSMISSION fail\n",
			DRIVER_NAME);
	}
	return err;

}

static void set_cmd_info(struct mmc_card *card, struct mmc_command *cmd,
			 struct sd_cmd_pkt *cmd_info, u32 opcode, u32 arg,
			 u8 rsp_para)
{
	memset(cmd, 0, sizeof(struct mmc_command));
	memset(cmd_info, 0, sizeof(struct sd_cmd_pkt));

	cmd->opcode = opcode;
	cmd->arg = arg;
	cmd_info->cmd = cmd;
	cmd_info->sdport = mmc_priv(card->host);
	cmd_info->rsp_para2 = rsp_para;
	cmd_info->rsp_len = rtksd_get_rsp_len(rsp_para);
}

static struct rtk_host_ops emmc_ops = {
	.func_irq = NULL,
	.re_init_proc = NULL,
	.card_det = NULL,
	.card_power = NULL,
	.chk_card_insert = rtkemmc_chk_card_insert,
	.set_crt_muxpad = rtkemmc_set_crt_muxpad,
	.set_clk = rtkemmc_set_clk,
	//we don't need to do the rst
	.reset_card = NULL,
	.reset_host = NULL,
	.bus_speed_down = rtkemmc_bus_speed_down,
	.get_cmdcode = NULL,	//rtk_get_emmc_cmdcode,
	.get_r1_type = NULL,	//rtk_emmc_get_r1_type,
	.chk_cmdcode = rtkcr_chk_cmdcode,
	.chk_r1_type = rtkcr_chk_r1_type,
	.backup_regs = rtkemmc_backup_registers,
	.restore_regs = rtkemmc_restore_registers,
};

#ifdef CONFIG_MMC_RTKEMMC_HK_ATTR
#if 0
static int mmc_send_data_cmd(unsigned int hc_cmd,
			     unsigned int cmd_arg,
			     unsigned int blk_cnt, unsigned char *buffer)
{
	int err = 0;
	//unsigned int rw_mode;
	struct mmc_host *mmc = mmc_host_local;
	unsigned long flags;

	printk(KERN_ERR "cmd=%u, arg=0x%08x, size=0x%08x, buf=0x%p\n",
	       hc_cmd, cmd_arg, blk_cnt, buffer);
	if (mmc) {
		struct rtksd_host *sdport = mmc_priv(mmc);
		struct mmc_card *card = mmc->card;
		struct mmc_request mrq = { 0 };
		struct mmc_command cmd = { 0 };
		struct mmc_data data = { 0 };
		struct scatterlist sg;

		sdport = mmc_priv(mmc);
		mmc_claim_host(mmc);
		spin_lock_irqsave(&sdport->lock, flags);

		cmd.opcode = hc_cmd;
		cmd.arg = cmd_arg;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		if (blk_cnt) {	//date info setting
			data.sg = &sg;
			data.sg_len = 1;
			data.blksz = 512;
			data.blocks = blk_cnt;

			sg_init_one(data.sg, buffer, (blk_cnt << 9));

			if (hc_cmd == MMC_WRITE_MULTIPLE_BLOCK ||
			    hc_cmd == MMC_WRITE_BLOCK) {
				data.flags = MMC_DATA_WRITE;
			} else if (hc_cmd == MMC_SEND_EXT_CSD ||
				   hc_cmd == MMC_READ_MULTIPLE_BLOCK ||
				   hc_cmd == MMC_READ_SINGLE_BLOCK) {
				data.flags = MMC_DATA_READ;
			} else {
				printk(KERN_ERR "not supported command");
				spin_unlock_irqrestore(&sdport->lock, flags);
				goto ERR_OUT;
			}

			mmc_set_data_timeout(&data, card);
		}

		data.mrq = &mrq;
		cmd.mrq = &mrq;
		cmd.data = &data;
		cmd.retries = 3;

CMD_RETRY:
		mrq.data = &data;
		mrq.cmd = &cmd;

		if (sdport->mrq) {
			sdport->mrq = NULL;
		}

		cmd.error = 0;
		data.error = 0;

		spin_unlock_irqrestore(&sdport->lock, flags);
		mmc_wait_for_req(mmc, &mrq);
		spin_lock_irqsave(&sdport->lock, flags);

		err = cmd.error;

		if (err && cmd.retries) {
			printk(KERN_ERR "%s(%u)last retry %d counter.\n",
			       __func__, __LINE__, cmd.retries);
			cmd.retries--;
			goto CMD_RETRY;
		}
		spin_unlock_irqrestore(&sdport->lock, flags);
		mmc_release_host(mmc);
	} else {
		err = -ENODEV;
	}

ERR_OUT:
	if (err)
		printk(KERN_ERR "err=%d\n", err);
	return err;
}
#endif
#if 0
static void show_ext_csd(u8 * ext_csd)
{
	int i;
	for (i = 0; i < 512; i += 8) {
		printk(KERN_ERR
		       "[%03u]=%02x [%03u]=%02x [%03u]=%02x [%03u]=%02x "
		       "[%03u]=%02x [%03u]=%02x [%03u]=%02x [%03u]=%02x\n",
		       i, *(ext_csd + i), i + 1, *(ext_csd + i + 1), i + 2,
		       *(ext_csd + i + 2), i + 3, *(ext_csd + i + 3), i + 4,
		       *(ext_csd + i + 4), i + 5, *(ext_csd + i + 5), i + 6,
		       *(ext_csd + i + 6), i + 7, *(ext_csd + i + 7));
	}
	printk(KERN_ERR "\n");
}
#endif
#define PF2_SHOW_EXT_CSD 0x01UL
#define PF2_FULL_PARAM   0x02UL
#define PF2_SET_EXT_CSD  0x04UL

#ifdef HACK_BOOT_PART_RW
/*
Note : this function not complete yet.
*/
#if 0
static void hk_fill_bp(struct device *dev,
		       size_t p_count, unsigned char *cr_param)
{
	u8 *emmc_buf;
	//u32 i;
	u32 param1;
	u32 param2;
	u32 param3;
	u32 buf_size;
	u8 part_config;
	struct mmc_host *host;
	struct mmc_card *card;

	printk(KERN_ERR "%s(%u)3013/08/15 17:30\n", __func__, __LINE__);

	host = dev_get_drvdata(dev);
	if (host)
		card = host->card;

	if (card == NULL) {
		printk(KERN_ERR "card is not exist.\n");
		goto ERR_OUT;
	}

	rtkcr_chk_param(&param1, 8, cr_param);
	rtkcr_chk_param(&param2, 8, cr_param + 9);
	rtkcr_chk_param(&param3, 8, cr_param + 18);

	printk(KERN_ERR "param1=0x%x\n", param1);	//enh_start_addr
	printk(KERN_ERR "param2=0x%x\n", param2);	//enh_block_cnt
	printk(KERN_ERR "param3=0x%x\n", param3);

	buf_size = 512;
	part_config = card->ext_csd.part_config;

	if (host && card) {
		int idx;
		printk(KERN_ERR "erased byte = 0x%x\n", card->erased_byte);
		printk(KERN_ERR "part_config=0x%x\n", part_config);
		printk(KERN_ERR "card's partition information\n");
		for (idx = 0; idx < card->nr_parts; idx++) {
			if (card->part[idx].size) {
				printk(KERN_ERR "\n===> \nPART %u; name:%s\n",
				       idx, card->part[idx].name);
				printk(KERN_ERR
				       "    part_cfg = 0x%x; size:0x%x\n",
				       card->part[idx].part_cfg,
				       card->part[idx].size >> 9);
				printk(KERN_ERR
				       "    force_ro = 0x%x; area_type = 0x%x\n",
				       card->part[idx].force_ro,
				       card->part[idx].area_type);
				if (card->part[idx].part_cfg <
				    EXT_CSD_PART_CONFIG_ACC_GP0) {
					printk(KERN_ERR
					       "cfg: EXT_CSD_PART_CONFIG_ACC_BOOT%x\n",
					       (card->part[idx].part_cfg) -
					       EXT_CSD_PART_CONFIG_ACC_BOOT0);
				} else {
					printk(KERN_ERR
					       "cfg: EXT_CSD_PART_CONFIG_ACC_GP%u\n",
					       (card->part[idx].part_cfg) -
					       EXT_CSD_PART_CONFIG_ACC_GP0);
				}
				if (card->part[idx].area_type ==
				    MMC_BLK_DATA_AREA_MAIN)
					printk(KERN_ERR
					       "type: MMC_BLK_DATA_AREA_MAIN\n");
				if (card->part[idx].area_type ==
				    MMC_BLK_DATA_AREA_BOOT)
					printk(KERN_ERR
					       "type: MMC_BLK_DATA_AREA_BOOT\n");
				if (card->part[idx].area_type ==
				    MMC_BLK_DATA_AREA_GP)
					printk(KERN_ERR
					       "type: MMC_BLK_DATA_AREA_GP\n");
			}
		}
	}

	buf_size = 512 * 16;
	emmc_buf = kmalloc(buf_size, GFP_KERNEL);
	if (!emmc_buf) {
		printk(KERN_ERR "emmc_buf is NULL\n");
		goto ERR_OUT;
	}

	memset(emmc_buf, 0, buf_size);

	printk(KERN_ERR "emmc_buf=0x%p\n", emmc_buf);

	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "[EXT_CSD] :\n");
	show_ext_csd(emmc_buf);

	rtksd_switch_partition(card, BOOT1_PART);

	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "{F} [EXT_CSD] :\n");
	show_ext_csd(emmc_buf);

	if (*(emmc_buf + EXT_CSD_PART_CONFIG) & 0x01) {
		u32 boot_part_blk_cnt;

		boot_part_blk_cnt = (*(emmc_buf + EXT_CSD_BOOT_MULT)) << 8;
		printk(KERN_ERR
		       "have changed to boot partition 1. [%x block]\n",
		       boot_part_blk_cnt);

		//mmc_send_data_cmd(MMC_READ_MULTIPLE_BLOCK,
		//              0,boot_part_blk_cnt,emmc_buf);

	}

	kfree(emmc_buf);

ERR_OUT:
	return;
}
#endif
#endif
#if 0
static void hk_set_wr_rel(struct device *dev,
			  size_t p_count, unsigned char *cr_param)
{
	u8 *emmc_buf;
	u8 acc_mod;
	u8 index;
	u8 value;
	u8 i;
	u8 idx_lop;
	u8 cmd_set;
	u32 param1;
	u32 param2;
	u32 param3;
	u32 buf_size;
	struct mmc_host *host;
	struct mmc_card *card = NULL;

	printk(KERN_ERR "%s(%u)3013/08/15 17:30\n", __func__, __LINE__);

	if (p_count != 38) {
		printk(KERN_ERR "Command format:\n"
		       "    echo set_wr_rel=param1,param2,param3 > emmc_hacking;\n"
		       "        param1[7:0] is value in byte you want to set.\n"
		       "        param2 :\n"
		       "            [bit0] TO show all ext_csd.\n"
		       "            [bit1] Send CMD6. if this bit is set.\n"
		       "                   param1 change to 32 bits parameter of argument of AMD6.\n"
		       "        param3 must be 2379beef to make sure you want to do this.\n"
		       "ex:\n"
		       "    echo set_wr_rel=00000001,00000000,2379beef > emmc_hacking;\n"
		       "        param1=0x00000001 enable user data area write reliability\n"
		       "        param2=0x00020000 do not show all ext_csd.\n"
		       "        param3=2379beef make sure that want to do this.\n"
		       "\n"
		       "    echo set_wr_rel=03a71f01,00000002,2379beef > emmc_hacking;\n"
		       "        param1=0x03a71f01 set ext_cse[0xa7], value=0x1f, cmd_set=1\n"
		       "        param2=0x00000002 send CMD6. param1 is argument in 32 bits.\n"
		       "        param3=2379beef make sure that want to do this.\n");
		goto ERR_OUT;
	}

	host = dev_get_drvdata(dev);
	if (host)
		card = host->card;

	// KWarning: checked ok by alexkh@realtek.com
	if (card == NULL) {
		printk(KERN_ERR "card is not exist.\n");
		goto ERR_OUT;
	}

	rtkcr_chk_param(&param1, 8, cr_param);
	rtkcr_chk_param(&param2, 8, cr_param + 9);
	rtkcr_chk_param(&param3, 8, cr_param + 18);

	printk(KERN_ERR "param1=0x%x\n", param1);
	printk(KERN_ERR "param2=0x%x\n", param2);
	printk(KERN_ERR "param3=0x%x\n", param3);

	if (param2 & PF2_FULL_PARAM) {
		printk(KERN_ERR "Send CMD6 alert\n");
		acc_mod = (u8) ((param1 >> 24) & 0xff);
		index = (u8) ((param1 >> 16) & 0xff);
		value = (u8) ((param1 >> 8) & 0xff);
		cmd_set = (u8) (param1 & 0xff);
	} else {
		printk(KERN_ERR "Enable Write Reliability\n");
		acc_mod = MMC_SWITCH_MODE_WRITE_BYTE;
		index = EXT_CSD_WR_REL_SET;
		value = (u8) param1;
		cmd_set = 1;
	}

	printk(KERN_ERR "acc_mod=0x%x; index=%u; value=0x%x; cmd_set=0x%x\n",
	       acc_mod, index, value, cmd_set);

	buf_size = 512;
	emmc_buf = kmalloc(buf_size, GFP_KERNEL);
	if (!emmc_buf) {
		printk(KERN_ERR "emmc_buf is NULL\n");
		goto ERR_OUT;
	}
	//printk(KERN_ERR "emmc_buf=0x%p\n",emmc_buf);

	memset(emmc_buf, 0, 512);
	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "[EXT_CSD] :\n");
	if (param2) {
		if (param2 & PF2_SHOW_EXT_CSD)
			show_ext_csd(emmc_buf);
		if (param2 & PF2_FULL_PARAM) {
			if (index == 249 || index == 242 ||
			    index == 212 || index == 136)
				idx_lop = 4;
			else if (index == 157 || index == 140)
				idx_lop = 3;
			else if (index == 143)
				idx_lop = 12;
			else
				idx_lop = 1;

			for (i = 0; i < idx_lop; i++)
				printk(KERN_ERR "    [%03u]=%02x\n", index + i,
				       *(emmc_buf + index + i));
		}

	} else {
		printk(KERN_ERR "    [%03u]=%02x [%03u]=%02x\n",
		       166, *(emmc_buf + 166), 167, *(emmc_buf + 167));
	}

	if (index == EXT_CSD_WR_REL_SET) {
		if ((*(emmc_buf + EXT_CSD_WR_REL_PARAM) & 0x05) == 0x05) {
			if (*(emmc_buf + EXT_CSD_PARTITION_SETTING_COMP) & 0x01) {
				printk(KERN_ERR
				       "This chip PARTITION configuration have completed\n");
				printk(KERN_ERR "  ENH_SATRT_ADDR = 0x%08x\n",
				       (*(emmc_buf + EXT_CSD_ENH_START_ADDR + 3)
					<< 24) | (*(emmc_buf +
						    EXT_CSD_ENH_START_ADDR +
						    2) << 16) | (*(emmc_buf +
								   EXT_CSD_ENH_START_ADDR
								   +
								   1) << 8) |
				       (*(emmc_buf + EXT_CSD_ENH_START_ADDR)));
				printk(KERN_ERR "  ENH_SIZE_MULT  = 0x%06x\n",
				       (*(emmc_buf + EXT_CSD_ENH_SIZE_MULT + 2)
					<< 16) | (*(emmc_buf +
						    EXT_CSD_ENH_SIZE_MULT +
						    1) << 8) | (*(emmc_buf +
								  EXT_CSD_ENH_SIZE_MULT)));
				printk(KERN_ERR
				       "  PARTITION_ATTRIBUTE  = 0x%06x\n",
				       (*
					(emmc_buf +
					 EXT_CSD_PARTITION_ATTRIBUTE)));
				printk(KERN_ERR "  WR_REL_SET  = 0x%x\n",
				       (*(emmc_buf + EXT_CSD_WR_REL_SET)));
				goto FINISH_OUT;
			}

		} else {
			printk(KERN_ERR
			       "Device not support setting write reliability\n");
			printk(KERN_ERR "  WR_REL_PARAM = 0x%x\n",
			       *(emmc_buf + EXT_CSD_WR_REL_PARAM));
			goto FINISH_OUT;
		}
	}

	if (param3 == 0x2379beef)
		rtksd_switch(card, acc_mod, index, value, cmd_set);
	else {
		printk(KERN_ERR "param3 != 0x2379beef skip command.\n");
		goto FINISH_OUT;
	}

	memset(emmc_buf, 0, 512);
	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "{F} [EXT_CSD] :\n");
	if (param2) {
		if (param2 & PF2_SHOW_EXT_CSD)
			show_ext_csd(emmc_buf);
		if (param2 & PF2_FULL_PARAM) {
			if (index == 249 || index == 242 ||
			    index == 212 || index == 136)
				idx_lop = 4;
			else if (index == 157 || index == 140)
				idx_lop = 3;
			else if (index == 143)
				idx_lop = 12;
			else
				idx_lop = 1;

			for (i = 0; i < idx_lop; i++)
				printk(KERN_ERR "    [%03u]=%02x\n", index + i,
				       *(emmc_buf + index + i));
		}
	} else {
		printk(KERN_ERR "    [%03u]=%02x [%03u]=%02x\n",
		       166, *(emmc_buf + 166), 167, *(emmc_buf + 167));
	}

FINISH_OUT:
	kfree(emmc_buf);

ERR_OUT:
	return;

}
#endif
#if 0
static void hk_red_ext_csd(struct device *dev, size_t p_count,	//39
			   unsigned char *cr_param)
{
	u8 *emmc_buf;
	u32 param1;		//target emmc address
	u32 param2;		//block number
	u32 param3;		//1: write; 0: read
	u32 buf_size;
	struct mmc_host *host;
	struct mmc_card *card = NULL;

	printk(KERN_ERR "%s(%u)3013/08/15 17:30\n", __func__, __LINE__);
	host = dev_get_drvdata(dev);
	if (host)
		card = host->card;

	// KWarning: checked ok by alexkh@realtek.com
	if (card == NULL) {
		printk(KERN_ERR "card is not exist.\n");
		goto ERR_OUT;
	}

	rtkcr_chk_param(&param1, 8, cr_param);
	rtkcr_chk_param(&param2, 8, cr_param + 9);
	rtkcr_chk_param(&param3, 8, cr_param + 18);

	printk(KERN_ERR "param1=0x%x\n", param1);	//enh_start_addr
	printk(KERN_ERR "param2=0x%x\n", param2);	//enh_block_cnt
	printk(KERN_ERR "param3=0x%x\n", param3);

	buf_size = 512;
	emmc_buf = kmalloc(buf_size, GFP_KERNEL);
	if (!emmc_buf) {
		printk(KERN_ERR "emmc_buf is NULL\n");
		goto ERR_OUT;
	}
	printk(KERN_ERR "emmc_buf=0x%p\n", emmc_buf);

	memset(emmc_buf, 0, 512);
	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "[EXT_CSD] :\n");

	if (param1 && !(param2 & PF2_SHOW_EXT_CSD)) {
		u8 i;
		u8 item_cnt;
		for (i = 0; i < 4; i++) {
			item_cnt = param1 >> (i * 8);
			printk(KERN_ERR "    [%03u]=%02x ", item_cnt,
			       *(emmc_buf + item_cnt));
		}
		printk(KERN_ERR "\n");
	} else {
		show_ext_csd(emmc_buf);
	}
	kfree(emmc_buf);

ERR_OUT:
	return;
}
#endif
#if 0
static void hk_set_enh_user_area(struct device *dev,
				 size_t p_count, unsigned char *cr_param)
{
	u8 *emmc_buf;
	u32 param1;
	u32 param2;
	u32 param3;
	u32 buf_size;
	struct mmc_host *host;
	struct mmc_card *card = NULL;

	printk(KERN_ERR "%s(%u)3013/08/15 17:30\n", __func__, __LINE__);
	host = dev_get_drvdata(dev);
	if (host)
		card = host->card;

	// KWarning: checked ok by alexkh@realtek.com
	if (card == NULL) {
		printk(KERN_ERR "card is not exist.\n");
		goto ERR_OUT;
	}
	rtkcr_chk_param(&param1, 8, cr_param);
	rtkcr_chk_param(&param2, 8, cr_param + 9);
	rtkcr_chk_param(&param3, 8, cr_param + 18);

	printk(KERN_ERR "param1=0x%x\n", param1);	//enh_start_addr
	printk(KERN_ERR "param2=0x%x\n", param2);	//enh_block_cnt
	printk(KERN_ERR "param3=0x%x\n", param3);

	buf_size = 512;
	emmc_buf = kmalloc(buf_size, GFP_KERNEL);
	if (!emmc_buf) {
		printk(KERN_ERR "emmc_buf is NULL\n");
		goto ERR_OUT;
	}
	printk(KERN_ERR "emmc_buf=0x%p\n", emmc_buf);

	memset(emmc_buf, 0, 512);
	mmc_send_data_cmd(MMC_SEND_EXT_CSD, 0, 1, emmc_buf);

	printk(KERN_ERR "[EXT_CSD] :\n");
	if ((*(emmc_buf + EXT_CSD_PARTITION_SUPPORT) & 0x03) == 0x03) {
		u32 i;
		//u8 item_cnt;
		u32 max_enh_size_mult;
		u32 enh_size_base_blk;
		u32 sec_cnt;
		u8 hc_wp_grp_size;
		u8 hc_erase_grp_size;

		hc_wp_grp_size = *(emmc_buf + EXT_CSD_HC_WP_GRP_SIZE);
		hc_erase_grp_size = *(emmc_buf + EXT_CSD_HC_ERASE_GRP_SIZE);

		sec_cnt = (*(emmc_buf + EXT_CSD_SEC_CNT + 3) << 24) |
		    (*(emmc_buf + EXT_CSD_SEC_CNT + 2) << 16) |
		    (*(emmc_buf + EXT_CSD_SEC_CNT + 1) << 8) |
		    (*(emmc_buf + EXT_CSD_SEC_CNT));

		max_enh_size_mult =
		    (*(emmc_buf + EXT_CSD_MAX_ENH_SIZE_MULT + 2) << 16) |
		    (*(emmc_buf + EXT_CSD_MAX_ENH_SIZE_MULT + 1) << 8) |
		    (*(emmc_buf + EXT_CSD_MAX_ENH_SIZE_MULT));

		enh_size_base_blk = (u32) hc_wp_grp_size *
		    (u32) hc_erase_grp_size *1024;

		printk(KERN_ERR "  ERASE_GROUP_DEF = 0x%x\n",
		       *(emmc_buf + EXT_CSD_ERASE_GROUP_DEF));
		printk(KERN_ERR "  HC_WP_GRP_SIZE = 0x%x\n", hc_wp_grp_size);
		printk(KERN_ERR "  HC_ERASE_GRP_SIZE = 0x%x\n",
		       hc_erase_grp_size);
		printk(KERN_ERR "  SEC_CNT = 0x%x\n", sec_cnt);
		printk(KERN_ERR "  MAX_ENH_SIZE_MULT = 0x%x\n",
		       max_enh_size_mult);
		printk(KERN_ERR "Enhanced User Data Area base=\n"
		       "            %u kBytes(0x%x block)\n",
		       (enh_size_base_blk >> 1), enh_size_base_blk);
		printk(KERN_ERR "Enhanced User Data Area x Size=\n"
		       "            ENH_SIZE_MULT x HC_WP_GRP_SIZE x\n"
		       "            HC_ERASE_GRP_SIZE x 512 kBytes\n");

		if (*(emmc_buf + EXT_CSD_PARTITION_SETTING_COMP) & 0x01) {
			printk(KERN_ERR
			       "This chip PARTITION configuration have completed\n");
			printk(KERN_ERR "  ENH_SATRT_ADDR = 0x%08x\n",
			       (*(emmc_buf + EXT_CSD_ENH_START_ADDR + 3) << 24)
			       | (*(emmc_buf + EXT_CSD_ENH_START_ADDR + 2) <<
				  16) | (*(emmc_buf + EXT_CSD_ENH_START_ADDR +
					   1) << 8) | (*(emmc_buf +
							 EXT_CSD_ENH_START_ADDR)));
			printk(KERN_ERR "  ENH_SIZE_MULT  = 0x%06x\n",
			       (*(emmc_buf + EXT_CSD_ENH_SIZE_MULT + 2) << 16) |
			       (*(emmc_buf + EXT_CSD_ENH_SIZE_MULT + 1) << 8) |
			       (*(emmc_buf + EXT_CSD_ENH_SIZE_MULT)));
			printk(KERN_ERR "  PARTITION_ATTRIBUTE  = 0x%06x\n",
			       (*(emmc_buf + EXT_CSD_PARTITION_ATTRIBUTE)));

			goto FINISH_OUT;
		}

		if (param2 > enh_size_base_blk) {
			i = param2 % enh_size_base_blk;
			param2 = (param2 / enh_size_base_blk);
			if (i)
				param2 += 1;
		} else {
			param2 = 1;
		}
		if (param2 > max_enh_size_mult)
			param2 = max_enh_size_mult;

		printk(KERN_ERR "ENH_SATRT_ADDR = 0x%08x\n", param1);
		printk(KERN_ERR "ENH_SIZE_MULT  = 0x%06x\n", param2);

		if (p_count == 38 && (param3 == 0x2379beef)) {
			u32 part_switch_time;
			/* doing set enhance user data param to ext_csd */
			/* setting ENH_SATRT_ADDR */
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_START_ADDR,
				     (u8) (param1 & 0xff), 1);
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_START_ADDR + 1,
				     (u8) ((param1 >> 8) & 0xff), 1);
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_START_ADDR + 2,
				     (u8) ((param1 >> 16) & 0xff), 1);
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_START_ADDR + 3,
				     (u8) ((param1 >> 24) & 0xff), 1);
			/* setting ENH_SIZE_MULT */
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_SIZE_MULT,
				     (u8) (param2 & 0xff), 1);
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_SIZE_MULT + 1,
				     (u8) ((param2 >> 8) & 0xff), 1);
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_ENH_SIZE_MULT + 2,
				     (u8) ((param2 >> 16) & 0xff), 1);
			/* settig user data area to enhance mode */
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_PARTITION_ATTRIBUTE, 0x01, 1);
			/* setting partition configuration complete */
			rtksd_switch(card, MMC_SWITCH_MODE_WRITE_BYTE,
				     EXT_CSD_PARTITION_SETTING_COMP, 0x01, 1);

			part_switch_time =
			    *(emmc_buf + EXT_CSD_PART_SWITCH_TIME);
			printk(KERN_ERR "Partition Switch Time is %u0ms.\n",
			       part_switch_time);
			mdelay(10 * (part_switch_time + 10));
		} else {
			printk(KERN_ERR "Command format:\n"
			       "    echo set_ehuser=param1,param2,param3 > emmc_hacking;\n"
			       "        param1 is enhance user data area start address in block(8 hex).\n"
			       "        param2 is enhance user data area size in block(8 hex).\n"
			       "        param3 must be 2379beef to make sure you want to do this.\n"
			       "ex:\n"
			       "    echo set_ehuser=00000000,00020000,2379beef > emmc_hacking;\n"
			       "        param1=0x00000000 EUDA start address is 0.\n"
			       "        param2=0x00020000 EUDA size is 0x2000.\n"
			       "        param3=2379beef make sure that want to do this.\n");
		}

	} else {
		printk(KERN_ERR
		       "device do not support enhance uaer data area.\n");
	}
FINISH_OUT:
	kfree(emmc_buf);

ERR_OUT:
	return;
}
#endif

#endif

/* ========================================================
 * blk_addr    : eMMC read/write target address, block base.
 * data_size   : tarnsfer data size, block base.
 * buffer      : DMA address
 * rw_mode     : fast read or fast write
 ========================================================== */
#define FAST_READ   0x1278
#define FAST_WRITE  0x3478
static int mmc_fast_rw(unsigned int blk_addr,
		       unsigned int data_size,
		       unsigned char *buffer, unsigned int rw_mode)
{
	int err = 0;
	struct mmc_host *mmc = mmc_host_local;
	unsigned long flags;

	if (mmc) {
		struct rtksd_host *sdport;
		struct mmc_card *card;
		struct mmc_request mrq = { 0 };
		struct mmc_command cmd = { 0 };
		struct mmc_data data = { 0 };
		struct scatterlist sg;

		sdport = mmc_priv(mmc);
		mmc_claim_host(mmc);
		spin_lock_irqsave(&sdport->lock, flags);

		card = mmc->card;

		if ((sdport->rtflags & RTKCR_USER_PARTITION) == 0) {
			int try_loop = 3;
			do {
				err = 0;
				spin_unlock_irqrestore(&sdport->lock, flags);
				err = rtksd_switch_user_partition(card);
				spin_lock_irqsave(&sdport->lock, flags);

			} while (try_loop-- && err);
			if (err) {
				spin_unlock_irqrestore(&sdport->lock, flags);
				mmc_release_host(mmc);
				goto ERR_OUT;
			} else
				sdport->rtflags |= RTKCR_USER_PARTITION;
		}

		if (data_size > 1) {

			if (rw_mode == FAST_WRITE)
				cmd.opcode = MMC_WRITE_MULTIPLE_BLOCK;
			else
				cmd.opcode = MMC_READ_MULTIPLE_BLOCK;

		} else {

			if (rw_mode == FAST_WRITE)
				cmd.opcode = MMC_WRITE_BLOCK;
			else
				cmd.opcode = MMC_READ_SINGLE_BLOCK;
		}

		cmd.arg = blk_addr;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

		if (data_size) {	//date info setting
			data.sg = &sg;
			data.sg_len = 1;
			data.blksz = 512;
			data.blocks = data_size;

			sg_init_one(data.sg, buffer, (data_size << 9));

			if (rw_mode == FAST_WRITE)
				data.flags = MMC_DATA_WRITE;
			else
				data.flags = MMC_DATA_READ;

			mmc_set_data_timeout(&data, card);
		}

		data.mrq = &mrq;
		cmd.mrq = &mrq;
		cmd.data = &data;
		cmd.retries = 5;

CMD_RETRY:
		mrq.data = &data;
		mrq.cmd = &cmd;

		if (sdport->mrq) {
			sdport->mrq = NULL;
		}
		cmd.error = 0;
		data.error = 0;
		spin_unlock_irqrestore(&sdport->lock, flags);
		mmc_wait_for_req(mmc, &mrq);
		spin_lock_irqsave(&sdport->lock, flags);
		err = cmd.error;
		if ((err == 0) && (rw_mode == FAST_WRITE)) {
			spin_unlock_irqrestore(&sdport->lock, flags);
			err = rtksd_wait_status(mmc->card, STATE_TRAN, 0, 0);
			spin_lock_irqsave(&sdport->lock, flags);
		}
		if (err && cmd.retries) {
			printk("%s(%u)last retry %d counter.\n",
			       __func__, __LINE__, cmd.retries);
			cmd.retries--;
			goto CMD_RETRY;
		}
		spin_unlock_irqrestore(&sdport->lock, flags);
		mmc_release_host(mmc);
	} else {
		err = -ENODEV;
	}

ERR_OUT:
	if (err)
		printk("err=%d\n", err);
	return err;
}

#define MAX_XFER_BLK_A    0x100
#define MAX_XFER_BLK_B    0x400
static int mmc_fast_rw_loop(unsigned int blk_addr,
			    unsigned int data_size,
			    unsigned char *buffer, unsigned int rw_mode)
{
	int err = 0;

	unsigned int tmp_addr = blk_addr;
	unsigned int tmp_size = data_size;
	unsigned int org_size = data_size;
	unsigned char *tmp_buf = buffer;
	unsigned int max_xfer_blk;
	do {
		/* max 1M bytes read/write per transfer */

		max_xfer_blk = MAX_XFER_BLK_B;

		if (data_size > max_xfer_blk) {
			tmp_size = max_xfer_blk;
		} else {
			tmp_size = data_size;
		}

		err = mmc_fast_rw(tmp_addr, tmp_size, tmp_buf, rw_mode);

		if (err)
			break;

		if (data_size > max_xfer_blk) {
			tmp_addr += max_xfer_blk;
			data_size -= max_xfer_blk;
			tmp_buf += (max_xfer_blk << 9);
		} else {
			data_size = 0;
		}

	} while (data_size);

	if (err == 0)
		return org_size - data_size;
	else
		return err;
}

int mmc_fast_read(unsigned int blk_addr,
		  unsigned int data_size, unsigned char *buffer)
{
	int err = 0;
	err = mmc_fast_rw_loop(blk_addr, data_size, buffer, FAST_READ);
	return err;
}

EXPORT_SYMBOL(mmc_fast_read);

int mmc_fast_write(unsigned int blk_addr,
		   unsigned int data_size, unsigned char *buffer)
{
	int err = 0;
	if (blk_addr < 0x20000) {
		printk("target small then save area.\n");
		err = -1;
	}
	err = mmc_fast_rw_loop(blk_addr, data_size, buffer, FAST_WRITE);
	return err;
}

EXPORT_SYMBOL(mmc_fast_write);

int mmc_send_cmd0(void)
{
	int err = 0;
	//struct mmc_host * mmc = mmc_host_local;
	struct mmc_card *card = NULL;
	struct mmc_host *mmc = mmc_host_local;

	MMCPRINTF("%s(%u)\n", __func__, __LINE__);

#if 1
	if (mmc) {
		mmc_claim_host(mmc);
		card = mmc->card;
	} else {
		printk("%s(%u) mmc == NULL\n", __func__, __LINE__);
		return -6;
	}
#else
	host = dev_get_drvdata(dev);
	if (host)
		card = host->card;
#endif

	// KWarning: checked ok by alexkh@realtek.com
	if (card == NULL) {
		printk(KERN_ERR "card is not exist.\n");
		err = -ENODEV;
		goto ERR_OUT;
	}
	err = rtksd_go_idle(card);

ERR_OUT:
	return -5;
}

EXPORT_SYMBOL(mmc_send_cmd0);

/* mmc device attribute *********************************************************** */
#define TEST_BLK_SIZE 10
void rtk_hexdump(const char *str, const void *buf, unsigned int length)
{
	unsigned int i;
	char *ptr = (char *)buf;

	if ((buf == NULL) || (length == 0)) {
		printk("NULL\n");
		return;
	}
	printk(str == NULL ? __FUNCTION__ : str);
	printk(" (0x%08x)\n", (unsigned int)(uintptr_t)buf);

	for (i = 0; i < length; i++) {
		printk("0x%02x", (unsigned int)(ptr[i]));

		if ((i & 0xf) == 0xf)
			printk("\n");
		else
			printk(" ");
	}
	printk("\n");
}

EXPORT_SYMBOL(rtk_hexdump);
#if 0
static ssize_t
cr_send_cmd0_dev_show(struct device *dev,
		      struct device_attribute *attr,
		      const char *buf, size_t count)
{
	printk("%s(%u)\n", __func__, __LINE__);
	return sprintf(buf, "send cmd0\n");
}

static ssize_t
cr_send_cmd0_dev_store(struct device *dev,
		       struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int err = 0;

	//err = mmc_send_cmd0(dev);
	err = mmc_send_cmd0();
	return err;
}

DEVICE_ATTR(cr_send_cmd0, S_IRUGO | S_IWUSR,
	    cr_send_cmd0_dev_show, cr_send_cmd0_dev_store);
#endif
#if 0
static ssize_t
cr_fast_RW_dev_show(struct device *dev, struct device_attribute *attr,
		    char *buf)
{
	struct mmc_host *host = dev_get_drvdata(dev);

	printk("%s(%u)\n", __func__, __LINE__);

	if (host && host->card) {
		rtksd_switch_user_partition(host->card);
	}
	return sprintf(buf, "send SWITCH command\n");
}

static ssize_t
cr_fast_RW_dev_store(struct device *dev,
		     struct device_attribute *attr,
		     const char *buf, size_t count)
{

	unsigned char *cr_param;

	printk("%s(%u)\n", __func__, __LINE__);
	printk("%s\n", buf);
	printk("count=%zu\n", count);

	cr_param = (char *)rtkcr_parse_token(buf, "cr_param");

	if (cr_param) {
		u8 *emmc_buf;
		u32 param1;	//target emmc address
		u32 param2;	//block number
		u32 param3;	//1: write; 0: read

		rtkcr_chk_param(&param1, 8, cr_param);
		rtkcr_chk_param(&param2, 8, cr_param + 9);
		rtkcr_chk_param(&param3, 8, cr_param + 18);
		printk("param1=0x%x param2=0x%x param3=0x%x\n",
		       param1, param2, param3);

		emmc_buf = kmalloc(BYTE_CNT * param2, GFP_KERNEL);
		if (!emmc_buf) {
			printk("emmc_buf is NULL\n");
			goto ERR_OUT;
		}

		printk("emmc_buf=0x%p\n", emmc_buf);

		if (param3 == 1) {
			mmc_fast_write(param1, param2, emmc_buf);
		} else {
			mmc_fast_read(param1, param2, emmc_buf);
		}
		kfree(emmc_buf);
	} else {
		printk("have no parameter searched.\n");
	}

ERR_OUT:
	/*
	   return value must be equare or big then "count"
	   to finish this attribute
	 */

	return count;
}

DEVICE_ATTR(cr_fast_RW, S_IRUGO | S_IWUSR,
	    cr_fast_RW_dev_show, cr_fast_RW_dev_store);
#endif
#if 0
static ssize_t
em_open_log_dev_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct mmc_host *host = dev_get_drvdata(dev);
	struct rtksd_host *sdport = mmc_priv(host);

	if (sdport->rtflags & RTKCR_FOPEN_LOG) {
		sdport->rtflags &= ~RTKCR_FOPEN_LOG;
	} else {
		sdport->rtflags |= RTKCR_FOPEN_LOG;
	}

	return sprintf(buf, "%s log %s\n",
		       DRIVER_NAME,
		       (sdport->rtflags & RTKCR_FOPEN_LOG) ? "open" : "close");
}

static ssize_t
em_open_log_dev_store(struct device *dev,
		      struct device_attribute *attr,
		      const char *buf, size_t count)
{

	printk("%s(%u)Not thing to do.\n", __func__, __LINE__);

	return count;
}

DEVICE_ATTR(em_open_log, S_IRUGO | S_IWUSR,
	    em_open_log_dev_show, em_open_log_dev_store);
#endif
#if 0
def CONFIG_MMC_RTKEMMC_HK_ATTR
static ssize_t
emmc_hacking_dev_show(struct device *dev, struct device_attribute *attr,
		      char *buf)
{
	//struct mmc_host * host = dev_get_drvdata(dev);
	//struct mmc_card *card = host->card;

	printk(KERN_ERR "%s(%u)3013/08/12 10:55\n", __func__, __LINE__);

	printk(KERN_ERR "Supported hacking below:\n");
#ifdef HACK_BOOT_PART_RW
	printk(KERN_ERR "    fill_par    : fill data  to specific partition\n");
#endif
	printk(KERN_ERR "    set_wr_rel  : enable write reliability.\n");
	printk(KERN_ERR "    red_ext_csd : show ext_csd.\n");
	printk(KERN_ERR "    set_ehuser  : enable enhance user date area.\n");

	return sprintf(buf, "emmc_hacking_dev_show\n");
}

static ssize_t
emmc_hacking_dev_store(struct device *dev,
		       struct device_attribute *attr,
		       const char *buf, size_t count)
{
	unsigned char *cr_param;

	printk(KERN_ERR "%s(%u)2013/08/15 17:30\n", __func__, __LINE__);
	//printk("%s\n",buf);
	printk(KERN_ERR "count=%zu\n", count);

#ifdef HACK_BOOT_PART_RW
	cr_param = (char *)rtkcr_parse_token(buf, "fill_par");
	if (cr_param) {
		hk_fill_bp(dev, count, cr_param);
		goto FINISH_OUT;
	}
#endif //#ifdef HACK_BOOT_PART_RW

	cr_param = (char *)rtkcr_parse_token(buf, "set_wr_rel");
	if (cr_param) {
		hk_set_wr_rel(dev, count, cr_param);
		goto FINISH_OUT;
	}

	cr_param = (char *)rtkcr_parse_token(buf, "red_ext_csd");
	if (cr_param) {
		hk_red_ext_csd(dev, count, cr_param);
		goto FINISH_OUT;
	}

	cr_param = (char *)rtkcr_parse_token(buf, "set_ehuser");
	if (cr_param) {
		hk_set_enh_user_area(dev, count, cr_param);
		goto FINISH_OUT;
	}

	printk(KERN_ERR "have no match command!!\n");

FINISH_OUT:
	/*
	   return value must be equare or big then "count"
	   to finish this attribute
	 */
	return count;
}

DEVICE_ATTR(emmc_hacking, S_IRUGO | S_IWUSR,
	    emmc_hacking_dev_show, emmc_hacking_dev_store);

#endif
#if 0
static ssize_t
emmc_id_dev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mmc_host *host = dev_get_drvdata(dev);
	struct mmc_card *card = host->card;
	struct rtksd_host *sdport = mmc_priv(host);

	//MMCPRINTF(KERN_INFO "%s(%u)%s %s\n", __func__, __LINE__, __DATE__,
	//	  __TIME__);
	return sprintf(buf, "emmcid=0x%02x%02x\ncfg1=0x%02x,cfg2=0x%02x,cfg3=0x%02x,sts1=0x%02x, \
		sts2=0x%02x,bus_sts=0x%02x\nsample_pnt=0x%02x,push_pnt=0x%02x,trans=0x%02x,pad_ctl=0x%02x,\
		ckgen_ctl=0x%02x\nCARD_SELECT=0x%02x,SYS_PLL_EMMC3=0x%08x,PLL_EMMC1=0x%08x\n",
		(unsigned char)(card->cid.manfid), (unsigned char)(card->cid.oemid),
		cr_readb(sdport->base + SD_CONFIGURE1), cr_readb(sdport->base + SD_CONFIGURE2),
		cr_readb(sdport->base + SD_CONFIGURE3), cr_readb(sdport->base + SD_STATUS1),
		cr_readb(sdport->base + SD_STATUS2), cr_readb(sdport->base + SD_BUS_STATUS),
		cr_readb(sdport->base + SD_TRANSFER), cr_readb(sdport->base + SD_SAMPLE_POINT_CTL),
		cr_readb(sdport->base + SD_PUSH_POINT_CTL), cr_readb(sdport->base + CR_SD_PAD_CTL),
		cr_readb(sdport->base + CR_SD_CKGEN_CTL), cr_readb(sdport->base + CARD_SELECT),
		cr_readl(sdport->pll_base + SSCPLL_CTRL2), cr_readl(sdport->pll_base + SSCPLL_CTRL0));
}

static ssize_t
emmc_id_dev_store(struct device *dev,
		  struct device_attribute *attr, const char *buf, size_t count)
{
	printk(KERN_ERR "%s(%u)Nothing to do\n", __func__, __LINE__);

	/*
	   return value must be equare or big then "count"
	   to finish this attribute
	 */
	return count;
}

DEVICE_ATTR(emmc_id, S_IRUGO | S_IWUSR, emmc_id_dev_show, emmc_id_dev_store);
#endif
#if 0
static void rtk_emmc_create_dev_attr(struct device *dev)
{
	int att_err;

	att_err = device_create_file(dev, &dev_attr_cr_send_cmd0);
	att_err = device_create_file(dev, &dev_attr_cr_fast_RW);
	att_err = device_create_file(dev, &dev_attr_em_open_log);
	att_err = device_create_file(dev, &dev_attr_emmc_id);
#ifdef CONFIG_MMC_RTKEMMC_HK_ATTR
	att_err = device_create_file(dev, &dev_attr_emmc_hacking);
#endif
}
#endif
static int rtk_emmc_get_resource(struct platform_device *pdev, struct rtksd_host *sdport)
{
	struct device *dev = &pdev->dev;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sdport->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(sdport->base)) {
		dev_err(dev, "base register ioremap failed %ld\n",
		       PTR_ERR(sdport->base));
		return -ENOMEM;
	}
	dev_info(dev, "resource - %pr mapped at 0x%px\n", res, sdport->base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	sdport->pll_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(sdport->pll_base)) {
		dev_err(dev, "pll register ioremap failed %ld\n",
			PTR_ERR(sdport->pll_base));
		return -ENOMEM;
	}
	dev_info(dev, "resource - %pr mapped at 0x%px\n", res, sdport->pll_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	sdport->blk_base = ioremap(res->start,  resource_size(res));
	if (IS_ERR(sdport->blk_base)) {
		dev_err(dev, "block reset register ioremap failed %ld\n",
			PTR_ERR(sdport->blk_base));
		return -ENOMEM;
	}
	dev_info(dev, "resource - %pr mapped at 0x%px\n", res, sdport->blk_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	sdport->sd_io_pull_base = ioremap(res->start,  resource_size(res));
	if (IS_ERR(sdport->sd_io_pull_base)) {
		dev_err(dev, "sd_io_pull_base register ioremap failed %ld\n",
			PTR_ERR(sdport->sd_io_pull_base));
		return -ENOMEM;
	}
	dev_info(dev, "resource - %pr mapped at 0x%p\n", res, sdport->sd_io_pull_base);

	return 0;
}

static const struct of_device_id rtk_rtkemmc_ids[] = {
	{.compatible = "realtek,rtl8198xb-mmc"},
	{.compatible = "Realtek,rtk119x-emmc"},
	{ /* Sentinel */ },
};

static void rtk_emmc_hw_init(struct rtksd_host *host)
{
	void __iomem *emmc_base = host->base;
	void __iomem *pll_base = host->pll_base;
	void __iomem *blk_base = host->blk_base;
	void __iomem *sd_io_pull_base = host->sd_io_pull_base;
	int retry_cnt;

	if (host->sd_rst) {
		reset_control_assert(host->sd_rst);

		rtl_reg_mask(blk_base, EN_SD, ~EN_SD);
		// reset PLL default value
		writel(0x22004000, pll_base + SSCPLL_CTRL0);
		writel(0x4078b004, pll_base + SSCPLL_CTRL1);
		writel(0x3bd68f5, pll_base + SSCPLL_CTRL2);
		writel(0x7ca0, pll_base + SSCPLL_CTRL3);
		writel(0x80000008, pll_base + SSCPLL_CTRL4);
		usleep_range(10, 50);
		reset_control_deassert(host->sd_rst);
	}
// obsolete
#if 0
	/* Enable LDO and fix divided fractional */
	writel(readl(pll_base + SSCPLL_CTRL1) | SSCLDO_EN | SSC_DIV_F_SEL,
			pll_base + SSCPLL_CTRL1);

	/* Enable SSC PLL clk */
	writel(SSC_RSTB | SSC_PLL_RSTB | SSC_PLL_POW | CLK_EN_SD30,
			pll_base + SSCPLL_CTRL3);

	/* Enable SD/EMMC IP */
	rtl_reg_mask(blk_base, EN_SD, EN_SD);
	writel(0x019FF100, blk_base + IO_PULL_CTRL);

	/* Set voltage to 1.8v */
	rtl_reg_mask(pll_base + LDO_CTRL, TUNE_VO, LDO_1P8V);
	rtl_reg_mask(pll_base + LDO_CTRL, LDO_EN, LDO_EN);
	writel(PAD_1P8V, emmc_base + EMMC_PAD_CTL);
#else
	writel(PAD_1P8V, emmc_base + EMMC_PAD_CTL);
	/*
	 * 3. wait 300ns, en_sd30 = 1
	 */
	usleep_range(1, 2);
	rtl_reg_mask(blk_base, EN_SD, EN_SD);
	writel(0x019FF100, sd_io_pull_base);

	/*
	 * 4. wait 100ns, modify sscpll_icp / sscpll_rs / sscpll_pdiv /
	 * NCODE_T_SCPU / FCODE_T_SCPU / NCODE_SSC / FCODE_SSC /
	 * reg_doc_gran / reg_time0_ck / reg_bypass_pi / reg_pi_ibselh
	 */
	usleep_range(1, 2);
	writel(0x2004000, pll_base + SSCPLL_CTRL0);
	writel(0x878a000, pll_base + SSCPLL_CTRL1);
	writel(0x3bd6a3d, pll_base + SSCPLL_CTRL2);
	writel(0x1840, pll_base + SSCPLL_CTRL3);
	writel(0x8, pll_base + SSCPLL_CTRL4);

	/*
	 * 5. wait 100ns, SSC_RSTB = 1, OC_EN = 1, BIAS_EN =1
	 */
	usleep_range(1, 2);
	writel(0x6004001, pll_base + SSCPLL_CTRL0);
	writel(0x1841, pll_base + SSCPLL_CTRL3);

	/*
	 * 6. wait 500ns, release SSC_PLL_POW/SSC_PLL_RSTB/PLL_EN
	 */
	usleep_range(1, 2);
	writel(0x184f, pll_base + SSCPLL_CTRL3);
	writel(0x4000008, pll_base + SSCPLL_CTRL4);

	/*
	 * 7. wait 45us, let OC_EN = 0
	 */
	usleep_range(45, 46);
	writel(0x2004001, pll_base + SSCPLL_CTRL0);
	usleep_range(1, 2);
	writel(0x6004001, pll_base + SSCPLL_CTRL0);
	/*
	 * 9. check CLKRDY = 1
	 */
	for (retry_cnt = 0; retry_cnt < 3; retry_cnt++) {
		usleep_range(1, 2);
		if (readl(emmc_base + EMMC_PLL_DEBUG1) & PLL_DEBUG1_CLKRDY) {
				pr_info("EMMC PLL ready\n");
				break;
		} else {
				pr_warn("EMMC PLL not ready yet\n");
				continue;
		}
	}

	/*
	 * 10. wait 20us, let PHRT0 = 1
	 */
	usleep_range(20, 21);
	writel(0x6004003, pll_base + SSCPLL_CTRL0);

	/*
	 * 11. wait 100ns, clk_sd30_en
	 */
	usleep_range(1, 2);
	writel(0x185f, pll_base + SSCPLL_CTRL3);

	/*
	 * 12. PLL initialization complete. PLL clk is 26MHz
	 */
	usleep_range(1, 2);

#endif
	rtl_reg_mask(emmc_base + EMMC_SRAM_CTL, MAP_SEL, MAP_SEL);
	writeb(STOP_SD_MMC, emmc_base + CR_CARD_STOP);
	writeb(SD_SEL, emmc_base + CARD_SELECT);
	writeb(SD_MMC, emmc_base + CR_CARD_OE);
}

int rtk_emmc_parse_dt(struct platform_device *pdev, struct rtksd_host *sdport)
{
	struct device *dev = &pdev->dev;
	int ret = 0;

	/* Get register base from DT */
	if (rtk_emmc_get_resource(pdev, sdport))
		return -ENOMEM;

	/* Get IRQ num */
	sdport->irq = platform_get_irq(pdev, 0);
	if (sdport->irq <= 0) {
		dev_err(dev, "fail to parse of irq.\n");
		return -ENXIO;
	}

	sdport->sd_rst = of_reset_control_get(dev->of_node, "sd_reset");
	if (IS_ERR(sdport->sd_rst)) {
		dev_err(dev, "Get sd_reset failed %ld\n", PTR_ERR(sdport->sd_rst));
		return -ENXIO;
	}

	ret = mmc_of_parse(sdport->mmc);

	return ret;
}

static int rtkemmc_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc = NULL;
	struct rtksd_host *sdport = NULL;
	struct device *dev = &pdev->dev;
	int ret;

#ifdef EMMC_SHOUTDOWN_PROTECT
	u64 rtk_tmp_gpio;
#endif
	// remove for compile
	//rtk_emmc_create_dev_attr(dev);

	mmc = mmc_alloc_host(sizeof(struct rtksd_host), dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto out;
	}
	mmc_host_local = mmc;

	sdport = mmc_priv(mmc);
	memset((void *)sdport, 0, sizeof(struct rtksd_host));

	sdport->mmc = mmc;
	sdport->dev = dev;
	sdport->ops = &emmc_ops;

	sema_init(&sdport->sem, 1);
	sema_init(&sdport->sem_op_end, 1);

	ret = rtk_emmc_parse_dt(pdev, sdport);
	if (ret)
		goto out;

	mmc->ocr_avail = MMC_VDD_30_31
		| MMC_VDD_31_32
		| MMC_VDD_32_33
		| MMC_VDD_33_34
		| MMC_VDD_165_195;

	mmc->f_min = 10000000 >> 8;
	mmc->f_max = 200000000;
	mmc->max_blk_count = 0x400;
	mmc->max_seg_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;

	spin_lock_init(&sdport->lock);
	init_rwsem(&cr_rw_sem);
	tasklet_init(&sdport->req_end_tasklet, rtkcr_req_end_tasklet,
		     (unsigned long)sdport);

	//Force enable dbg log
#ifdef MMC_DBG
	sdport->rtflags |= RTKCR_FOPEN_LOG;
#endif

	if (rtk_emmc_bus_wid == 9 || rtk_emmc_bus_wid == 5) {
		sdport->rtflags |= RTKCR_FOPEN_LOG;
	}

#ifdef ENABLE_EMMC_INT_MODE
	rtkcr_hold_int_dec(sdport->base);	/* hold status interrupt */
	rtkcr_clr_int_sta(sdport->base);
#endif

	ret = request_irq(sdport->irq, rtksd_irq, IRQF_SHARED, DRIVER_NAME, sdport);
	if (ret) {
		printk(KERN_ERR "%s: cannot assign irq %d\n", DRIVER_NAME, sdport->irq);
		goto out;
	}
	timer_setup(&sdport->timer, rtksd_timeout_timer, 0);

	rtk_emmc_hw_init(sdport);

	//sdport->ops->set_crt_muxpad(sdport);
	if (sdport->ops->reset_card)
		sdport->ops->reset_card(sdport);
	sdport->ops->chk_card_insert(sdport);

#ifdef ENABLE_EMMC_INT_MODE
	//rtkcr_en_int(sdport->base);
	writel(0x00000006, sdport->base + CR_SD_ISR);	//enable interrupt
	writel(0x00000007, sdport->base + CR_SD_ISREN);
#endif

	platform_set_drvdata(pdev, mmc);

	ret = mmc_add_host(mmc);
	if (ret)
		goto out;

#ifdef CONFIG_MMC_FOR_RTK_FPGA
	cr_writel(0x200000, CR_DUMMY_SYS);
	cr_writel(0xf2100, sdport->base + EMMC_CKGEN_CTL);
#endif
	cr_writeb(0x2, sdport->base + CARD_SELECT);	//for emmc, select SD ip
	//rtkcr_set_pad_driving(sdport,MMC_IOS_GET_PAD_DRV, 0x66,0x64,0x66);
	rtkcr_set_pad_driving(sdport, MMC_IOS_SET_PAD_DRV, 0x66, 0x64, 0x66);
	//sync();
	memset((void *)g_cmd, 0x00, 6);
	memset((void *)&gRegTbl, 0x00, sizeof(struct backupRegs));
	gCurrentBootMode = MODE_SD20;
	MMCPRINTF("\ncard sample ctl : 0x%08x\n",
		  cr_readb(sdport->base + SD_SAMPLE_POINT_CTL));
	MMCPRINTF("\ncard push point ctl : 0x%08x\n",
		  cr_readb(sdport->base + SD_PUSH_POINT_CTL));
	g_crinit = 0;
	gPreventRetry = 0;
	g_bResuming = 0;

	sdport->aligned_buf = (u8 *)__get_free_page(GFP_KERNEL | GFP_DMA);

	printk(KERN_NOTICE "%s: %s driver initialized\n",
	       mmc_hostname(mmc), DRIVER_NAME);

	return 0;

out:
	if (sdport) {
		if (sdport->irq)
			free_irq(sdport->irq, sdport);

		if (sdport->base)
			iounmap(sdport->base);
	}
	if (mmc)
		mmc_free_host(mmc);
	return ret;
}

static int __exit rtksd_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	MMCPRINTF("\n");
#if 0
	device_remove_file(&pdev->dev, &dev_attr_cr_send_cmd0);
	device_remove_file(&pdev->dev, &dev_attr_cr_fast_RW);
	device_remove_file(&pdev->dev, &dev_attr_em_open_log);
	device_remove_file(&pdev->dev, &dev_attr_emmc_id);

#ifdef CONFIG_MMC_RTKEMMC_HK_ATTR
	device_remove_file(&pdev->dev, &dev_attr_emmc_hacking);
#endif
#endif
	if (mmc) {
		struct rtksd_host *sdport = mmc_priv(mmc);

		//flush_workqueue(); //fix later

		rtksd_free_dma_buf(sdport);

		mmc_remove_host(mmc);
		if (!mmc) {
			printk("eMMC host have removed.\n");
			mmc_host_local = NULL;
		}
		free_irq(sdport->irq, sdport);

		del_timer_sync(&sdport->timer);
		iounmap(sdport->base);

		mmc_free_host(mmc);
	}
	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM
static int rtksd_suspend(struct platform_device *dev, pm_message_t state)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	//int ret = 0;
	struct rtksd_host *sdport = NULL;

	if (mmc == NULL) {
		printk(KERN_INFO "[%s:%s] mmc == NULL...\n", DRIVER_NAME,
		       __func__);
		dump_stack();
		return 0;
	}

	if (!mmc)
		mmc = mmc_host_local;

	sdport = mmc_priv(mmc);
	if (!sdport)
		BUG();

	printk(KERN_INFO "%s: Prepare to suspend...\n", DRIVER_NAME);
	dump_stack();

	down_write(&cr_rw_sem);
	sdport->ops->backup_regs(sdport);
	up_write(&cr_rw_sem);
#ifndef REAL_SUSPEND
	return 0;
#endif

	if (mmc) {
		//struct rtksd_host *sdport = mmc_priv(mmc);
		if (sdport->ins_event) {
			printk(KERN_INFO "%s: waiting ins_event...\n",
			       DRIVER_NAME);
			rtkcr_mdelay(50);
		}
		down_write(&cr_rw_sem);
		sdport->ops->backup_regs(sdport);
		up_write(&cr_rw_sem);
		//ret = mmc_suspend_host(mmc); FIXME deprecated in Kernel 5.10
		/* should turn of plug timer */
	} else {
		printk("%s(%u)suspend fail , mmc == NULL\n", __func__,
		       __LINE__);
		return -1;
	}
	// reset eMMC flow ***
	printk(KERN_INFO "%s: Holding eMMC reset pin...\n", DRIVER_NAME);
	if (mmc) {
		struct rtksd_host *sdport = mmc_priv(mmc);
		rtksd_hold_card(sdport);
	}
	return 0;
}

static int rtksd_resume(struct platform_device *dev)
{
	struct mmc_host *mmc = platform_get_drvdata(dev);
	//int ret = 0;
	struct rtksd_host *sdport = NULL;
	struct mmc_host *host = NULL;

	if (mmc == NULL) {
		printk(KERN_INFO "[%s:%s] mmc == NULL...\n", DRIVER_NAME,
		       __func__);
		dump_stack();
		return 0;
	}

	if (!mmc)
		mmc = mmc_host_local;
	sdport = mmc_priv(mmc);
	if (!sdport)
		BUG();
	host = sdport->mmc;
	host->card->host = mmc;
	host->card->host->claimed = 1;
	g_bResuming = 1;
	printk(KERN_INFO "%s: wake up to resume...\n", DRIVER_NAME);
	dump_stack();

	rtksd_switch(host->card,
		     MMC_SWITCH_MODE_WRITE_BYTE,
		     EXT_CSD_HS_TIMING, 1, EXT_CSD_CMD_SET_NORMAL);
	rtksd_execute_tuning(host, MMC_SEND_TUNING_BLOCK_HS200);
	//if (gCurrentBootMode == MODE_SD30)
	//	mmc_select_hs200(host->card);	// FIXME mmc_select_hs200 is static kernel func, don't want to hack kernel for now.
	//else if (gCurrentBootMode == MODE_DDR) // FIXME can find mmc_select_ddr50 implementation
	//	mmc_select_ddr50(host->card);
	sdport->ops->restore_regs(sdport);
	host->card->host->claimed = 0;
	g_bResuming = 0;
#ifndef REAL_SUSPEND
	return 0;
#endif

	if (mmc) {
		//struct rtksd_host *sdport = mmc_priv(mmc);

		emmc_show_config123(sdport);

		down_write(&cr_rw_sem);
		//sdport->ops->set_crt_muxpad(sdport);
		if (sdport->ops->reset_card)
			sdport->ops->reset_card(sdport);
		sdport->ops->restore_regs(sdport);
		sdport->ins_event = EVENT_NON;
		up_write(&cr_rw_sem);
		//ret = mmc_resume_host(mmc); //FIXME deprecated in Kernel 5.10
	} else {
		printk("%s(%u)resume fail , mmc == NULL\n", __func__, __LINE__);
		return -1;
	}

	printk("%s(%u)resume finish~~\n", __func__, __LINE__);
	return 0;
}

#if 0
const struct dev_pm_ops rtk_emmc_pm_ops = {
	.suspend = rtksd_suspend,
	.resume = rtksd_resume,
	.poweroff = NULL,
	.restore = NULL,
};

#define RTK_EMMC_PM_OPS (&rtk_emmc_pm_ops)
#endif
#else
#define RTK_EMMC_PM_OPS NULL
#endif

/*****************************************************************************************/
/* driver / device attache area */
/*****************************************************************************************/

static struct platform_driver rtkemmc_driver = {
	.probe = rtkemmc_probe,
	.remove_new = __exit_p(rtksd_remove),
	.driver = {
		   .name = "rtk_emmc",
		   //.pm       = RTK_EMMC_PM_OPS,
		   .owner = THIS_MODULE,
		   .of_match_table = rtk_rtkemmc_ids,
		   },
#ifdef CONFIG_PM
	.suspend = rtksd_suspend,
	.resume = rtksd_resume,
#endif
};
#if 0
static void rtkcr_display_version(void)
{
	const __u8 *revision;
	const __u8 *date;
	const __u8 *time;
	char *running = (__u8 *) VERSION;

	strsep(&running, " ");
	strsep(&running, " ");
	revision = strsep(&running, " ");
	date = strsep(&running, " ");
	time = strsep(&running, " ");
	printk(BANNER " Rev:%s (%s %s)\n", revision, date, time);
	//printk("%s: build at %s %s\n", DRIVER_NAME, __DATE__, __TIME__);

#ifdef CONFIG_MMC_BLOCK_BOUNCE
	printk("%s: CONFIG_MMC_BLOCK_BOUNCE enable\n", DRIVER_NAME);
#else
	printk("%s: CONFIG_MMC_BLOCK_BOUNCE disable\n", DRIVER_NAME);
#endif

#ifdef CONFIG_SMP
	printk("%s: ##### CONFIG_SMP alert!! #####\n", DRIVER_NAME);
#else
	printk("%s: ##### CONFIG_SMP disable!! #####\n", DRIVER_NAME);
#endif
	//strcpy(&media_type[0], "emmc");
}
#endif
#if 0
static int rtkemmc_set_bus_width(char *buf)
{
	/*
	   get eMMC bus width setting by bootcode parameter, like below
	   bootargs=console=ttyS0,115200 earlyprintk emmc_bus=8
	   the keyword is "emmc_bus"
	   the getted parameter is hex.
	   example:
	   emmc_bus=8
	 */

	rtkcr_chk_param(&rtk_emmc_bus_wid, 1, buf + 1);
	printk("%s: setting bus width is %u-bit\n",
	       DRIVER_NAME, rtk_emmc_bus_wid);
	return 0;
}
#endif
#if 0
static int __init rtkemmc_init(void)
{
	int rc = 0;

	MMCPRINTF("\n");

	rtkcr_display_version();

#ifdef CONFIG_ANDROID
	printk(KERN_INFO "%s: Android timming setting\n", DRIVER_NAME);
#endif

	rc = platform_driver_register(&rtkemmc_driver);

	if (rc < 0) {
		printk(KERN_INFO
		       "Realtek EMMC Controller Driver installation fails.\n\n");
		return -ENODEV;
	} else {
#ifdef ENABLE_EMMC_INT_MODE
		printk(KERN_INFO
		       "Realtek EMMC Controller Driver is running interrupt mode.\n\n");
#endif
		printk(KERN_INFO
		       "Realtek EMMC Controller Driver is successfully installing.\n\n");
		return 0;
	}

	return rc;
}
#endif
#if 0
static void __exit rtkemmc_exit(void)
{
	MMCPRINTF("\n");
	platform_driver_unregister(&rtkemmc_driver);
}
#endif
// allow emmc driver initialization earlier
//module_init(rtkemmc_init);
//module_exit(rtkemmc_exit);
module_platform_driver(rtkemmc_driver);

/* maximum card clock frequency (default 50MHz) */
//module_param(maxfreq, int, 0);

/* force PIO transfers all the time */
//module_param(nodma, int, 0);

MODULE_AUTHOR("Elbereth");
MODULE_DESCRIPTION("Realtek EMMC Host Controller driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rtkemmc");

//__setup("emmc_bus", rtkemmc_set_bus_width);
