// SPDX-License-Identifier: GPL-2.0
/*
 * SPDX-License-Identifier
 * Copyright (c) 2022, Cortina Access. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, chip
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * chip list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/leds.h>
#include <linux/io.h>
#include <linux/mtd/partitions.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/dma-mapping.h>
#include <linux/sizes.h>
#include <linux/nmi.h>
#include <linux/bitfield.h>
#include <linux/spinlock.h>
#include <linux/reset.h>

#ifndef __ASM_ARCH_G3_FLASH_H
#define __ASM_ARCH_G3_FLASH_H

enum {
	ECC_HAM_256_3B = 0,
	ECC_HAM_512_3B = 1,
	ECC_BCH_8 = 2,
	ECC_BCH_16 = 3,
	ECC_BCH_24 = 4,
	ECC_BCH_40 = 5
};

struct ca_nand_host {
	struct nand_controller controller;
	struct nand_chip *nand_chip;
	void __iomem *io_base;
	unsigned int bch_cap;
	struct device *dev;
	unsigned int flash_mem_base;

	/* legacy data. need to be merged to framework */
	int pagebuf;
	u8 *calc_buf;
	u8 *code_buf;
};

struct tx_descriptor_t {
	unsigned int buf_adr; /* Buff addr */
	unsigned int buf_adr_hi :  8 ; /* bits 7:0 */
	unsigned int buf_len    :  16 ;  /* bits 23:8 */
	unsigned int sgm	:  1 ;  /* bits 24 */
	unsigned int rsrvd      :  6 ;  /* bits 30:25 */
	unsigned int own	:  1 ;  /* bits 31:31 */
};

struct rx_descriptor_t {
	unsigned int buf_adr; /* Buff addr */
	unsigned int buf_adr_hi :  8 ; /* bits 7:0 */
	unsigned int buf_len    : 16 ;  /* bits 23:8 */
	unsigned int rsrvd      :  7 ;  /* bits 30:24 */
	unsigned int own	:  1 ;  /* bits 31:31 */
};

/* Masks */
#define NSTATE_MASK		0xF00
#define FLASH_COMPLETE_MASK	0x1
#define FIFO_COMPLETE_MASK	0x400
#define BCH_COMPLETE_MASK	0x80000000
#define RPTR_INDEX_MASK		0x1FFF
#define WPTR_INDEX_MASK		0x1FFF
#define TX_INTERRUPT_MASK	BIT(0)
#define RX_INTERRUPT_MASK	BIT(0)

/* P-NAND controller register definition */
#define FLASH_STATUS			0x008
#define FLASH_TYPE			0x00c
#define   SET_FLASH_PIN			BIT(15)
#define   PAGE_512			(0x4 << 12)
#define   PAGE_2K			(0x5 << 12)
#define   PAGE_4K			(0x6 << 12)
#define   PAGE_8K			(0x7 << 12)
#define   CONFIGURABLE_OOB_SIZE		(0x0 << 9)
#define FLASH_FLASH_ACCESS_START	0x010
#define   FLASH_RD			(0x2 << 12)
#define   FLASH_WT			(0x3 << 12)
#define   FLASH_FIFO_GO			BIT(2)
#define   FLASH_GO			BIT(0)
#define FLASH_FLASH_INTERRUPT		0x014
#define   REGIRQ			BIT(0)
#define FLASH_FIFO_CONTROL		0x01c
#define   FIFO_FLASH_RD			0x2
#define   FIFO_FLASH_WT			0x3
#define FLASH_NF_ACCESS			0x060
#define   NFLASH_CHIP0_EN		(0x0 << 15)
#define   NFLASH_CHIP1_EN		BIT(15)
#define   NFLASH_REG_WIDTH8		(0x0 << 10)
#define   NFLASH_REG_WIDTH16		(0x1 << 10)
#define   NFLASH_REG_WIDTH32		(0x2 << 10)
#define FLASH_NF_COUNT			0x064
#define   NCNT_EMPTY_OOB		(0x3FF << 22)
#define   NCNT_EMPTY_DATA		(0x3FFF << 8)
#define   NCNT_512P_DATA		(0x1FF << 8)
#define   NCNT_DATA_1			(0x0 << 8)
#define   NCNT_DATA_2			(0x1 << 8)
#define   NCNT_DATA_3			(0x2 << 8)
#define   NCNT_DATA_4			(0x3 << 8)
#define   NCNT_DATA_5			(0x4 << 8)
#define   NCNT_DATA_6			(0x5 << 8)
#define   NCNT_DATA_7			(0x6 << 8)
#define   NCNT_DATA_8			(0x7 << 8)
#define   NCNT_EMPTY_ADDR		(0x7 << 4)
#define   NCNT_ADDR_5			(0x4 << 4)
#define   NCNT_ADDR_4			(0x3 << 4)
#define   NCNT_ADDR_3			(0x2 << 4)
#define   NCNT_ADDR_2			(0x1 << 4)
#define   NCNT_ADDR_1			(0x0 << 4)
#define FLASH_NF_COMMAND		0x068
#define   NCNT_EMPTY_CMD		0x3
#define   NCNT_CMD_3			0x2
#define   NCNT_CMD_2			0x1
#define   NCNT_CMD_1			0x0
#define FLASH_NF_ADDRESS_1		0x06c
#define FLASH_NF_ADDRESS_2		0x070
#define FLASH_NF_DATA			0x074
#define FLASH_NF_TIMING			0x078
#define   NF_CLKWIDTH_MSK		(0x7 << 24)
#define   NF_CLK_06_AXI			(0x7 << 24)
#define   NF_CLK_07_AXI			(0x6 << 24)
#define   NF_CLK_08_AXI			(0x5 << 24)
#define   NF_CLK_09_AXI			(0x4 << 24)
#define   NF_CLK_10_AXI			(0x3 << 24)
#define   NF_CLK_12_AXI			(0x2 << 24)
#define   NF_CLK_14_AXI			(0x1 << 24)
#define   NF_CLK_16_AXI			(0x0 << 24)
#define FLASH_NF_ECC_STATUS		0x07c
#define FLASH_NF_ECC_CONTROL		0x080
#define FLASH_NF_ECC_OOB		0x084
#define FLASH_NF_ECC_GEN0		0x088
#define FLASH_NF_ECC_RESET		0x0c8
#define   NF_RESET			BIT(2)
#define   FIFO_CLR			BIT(1)
#define   ECC_CLR			BIT(0)
#define FLASH_NF_BCH_CONTROL		0x0cc
#define   BCH_ENABLE			BIT(8)
#define   BCH_DECODE			BIT(1)
#define   BCH_ENCODE			(0x0 << 1)
#define FLASH_NF_BCH_STATUS		0x0d0
#define FLASH_NF_BCH_ERROR_LOC01	0x0d4
#define FLASH_NF_BCH_OOB0		0x124
#define FLASH_NF_BCH_GEN0_0		0x16c
#define FLASH_NF_BCH_GEN0_1		0x170
#define FLASH_NF_BCH_GEN1_0		0x1b4

/* DMA control registers */
#define DMA_SEC_DMA_GLB_DMA_LSO_CTRL	    0x00
#define DMA_SEC_DMA_GLB_DMA_SSP_RX_CTRL	 0x64
#define   CHECK_OWN			     BIT(1)
#define   DMA_EN				BIT(0)
#define DMA_SEC_DMA_GLB_DMA_SSP_TX_CTRL	 0x68
#define DMA_SEC_DMA_SSP_Q_RXQ_CONTROL	   0x00
#define DMA_SEC_DMA_SSP_Q_RXQ_BASE_DEPTH	0x04
#define DMA_SEC_DMA_SSP_Q_RXQ_BASE	      0x08
#define DMA_SEC_DMA_SSP_Q_RXQ_RPTR	      0X10
#define DMA_SEC_DMA_SSP_Q_TXQ_CONTROL	   0x18
#define   DISABLE_TXQ			   0x00
#define   ENABLE_TXQ			    BIT(0)
#define DMA_SEC_DMA_SSP_Q_TXQ_BASE_DEPTH	0x1c
#define DMA_SEC_DMA_SSP_Q_TXQ_BASE	      0x20
#define DMA_SEC_DMA_SSP_Q_TXQ_WPTR	      0x24
#define DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT    0x50
#define DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT    0x58

#define FLASH_NF_BCH_CONTROL__BCH_COMPARE       BIT(0)
#define FLASH_NF_BCH_CONTROL__BCH_CODE_SEL      GENMASK(6, 4)

#define FLASH_NF_BCH_STATUS__BCH_DEC_STATUS     GENMASK(1, 0)
#define FLASH_NF_BCH_STATUS__BCH_ERR_NUM	GENMASK(13, 8)
#define FLASH_NF_BCH_STATUS__BCH_DEC_DONE       BIT(30)

// BCH ECC comparison result,
#define BCH_UNCORRECTABLE       0x3
#define BCH_CORRECTABLE_ERR     0x2

#define BCH_DATA_UNIT	   1024

//define DMA parameter
#define FDMA_DEPTH      3
#define FDMA_DESC_NUM   BIT(FDMA_DEPTH)

#define OWN_DMA 0
#define OWN_SW  1

/* Macro to access flash controller register */
#define read_flash_ctrl_reg(reg) readl(flash_ctl_base + (reg))
#define write_flash_ctrl_reg(reg, data) writel(data, flash_ctl_base + (reg))

/* Macro to access global DMA register */
#define read_dma_glb_ctrl_reg(reg) readl(dma_ctl_glb_base + (reg))
#define write_dma_glb_ctrl_reg(reg, data) writel(data, dma_ctl_glb_base + (reg))

/* Macro to access DMA controller register */
#define read_dma_ctrl_reg(reg) readl(dma_ctl_base + (reg))
#define write_dma_ctrl_reg(reg, data) writel(data, dma_ctl_base + (reg))
#endif

static void __iomem *flash_ctl_base;
static void __iomem *flash_mem_base;
static void __iomem *dma_ctl_glb_base;
static void __iomem *dma_ctl_base;

struct ca_nand_host *ca_host;
static int nand_page, nand_col;
static bool force_ace;
static u8 *tmp_buf;

static struct tx_descriptor_t *tx_desc;
static struct rx_descriptor_t *rx_desc;
static unsigned int CHIP_EN;
static unsigned int *pread, *pwrite;

static void check_flash_ctrl_status(void)
{
	unsigned long timeo;
	u32 flash_status;

	timeo = jiffies + HZ;
	do {
		flash_status = read_flash_ctrl_reg(FLASH_STATUS);
		if (!(flash_status & NSTATE_MASK))
			return;
	} while (time_before(jiffies, timeo));

	pr_err("FLASH_STATUS ERROR: %x\n", flash_status);
}

static void ca_nand_reset_controller(void)
{
	u32 flash_nf_ecc_reset = 0;
	u32 reg_v, iter = 0;

	reg_v = read_flash_ctrl_reg(FLASH_STATUS);
	/* Wait RDY pin */
	while (((reg_v & NSTATE_MASK) == 0x800) && (iter++ < 100)) {
		udelay(1);
		reg_v = read_flash_ctrl_reg(FLASH_STATUS);
	}

	flash_nf_ecc_reset = ECC_CLR | FIFO_CLR | NF_RESET;
	write_flash_ctrl_reg(FLASH_NF_ECC_RESET, flash_nf_ecc_reset);

	do {
		flash_nf_ecc_reset = read_flash_ctrl_reg(FLASH_NF_ECC_RESET);
	} while (flash_nf_ecc_reset != 0);
}

static void ca_rst_ecc_bch_registers(void)
{
	u32 flash_nf_ecc_reset = 0, flash_flash_interrupt = 0;

	flash_nf_ecc_reset = ECC_CLR | FIFO_CLR;
	write_flash_ctrl_reg(FLASH_NF_ECC_RESET, flash_nf_ecc_reset);

	flash_flash_interrupt = REGIRQ;
	write_flash_ctrl_reg(FLASH_FLASH_INTERRUPT, flash_flash_interrupt);

	flash_nf_ecc_reset = ECC_CLR;
	write_flash_ctrl_reg(FLASH_NF_ECC_RESET, flash_nf_ecc_reset);

	/*  Disable ECC function */
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, 0);
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0);
}

/**
 * ca_nand_read_oob_std - [REPLACEABLE] the most common OOB data read function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @page:	page number to read
 */
static int ca_nand_read_oob_std(struct nand_chip *chip,
				int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int i;
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;

	ca_nand_reset_controller();

	check_flash_ctrl_status();

	/* disable ecc gen */
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0x0);

	flash_nf_count |= ((mtd->oobsize - 1) << 22) | NCNT_EMPTY_DATA;

	if (nanddev_target_size(&chip->base) < (32 << 20)) {
		flash_nf_count |= NCNT_ADDR_3 | NCNT_CMD_1;
		if (mtd->writesize > SZ_512)
			flash_nf_command = NAND_CMD_READ0;
		else
			flash_nf_command = NAND_CMD_READOOB;

		flash_nf_address_1 = ((page & 0x00ffffff) << 8);
		flash_nf_address_2 = ((page & 0xff000000) >> 24);
	} else if ((nanddev_target_size(&chip->base) >= (32 << 20)) &&
		   (nanddev_target_size(&chip->base) <= (128 << 20))) {
		flash_nf_count |= NCNT_ADDR_4 | NCNT_CMD_1;
		flash_nf_command = NAND_CMD_READ0;

		/*  Jeneng */
		if (mtd->writesize > SZ_512) {
			flash_nf_count |= NCNT_CMD_2;
			flash_nf_command = (NAND_CMD_READSTART << 8);
		}
		flash_nf_address_1 =
		    (((page & 0xffff) << 16) + (mtd->writesize & 0xffff));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);

	} else {		/* if((nanddev_target_size(&chip->base) > (128 << 20)) )) */

		flash_nf_count |= NCNT_ADDR_5 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_READ0 | (NAND_CMD_READSTART << 8);
		flash_nf_address_1 =
		    (((page & 0xffff) << 16) + (mtd->writesize & 0xffff));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);
	}

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	/* write read id command */
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	/* write address 0x0 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	pread = (unsigned int *)chip->oob_poi;

	for (i = 0; i < mtd->oobsize / 4; i++) {
		reg_val = CHIP_EN;
		/* nf_access.bf.nflashDirWr = ; */
		reg_val |= NFLASH_REG_WIDTH32;
		write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

		reg_val = FLASH_GO | FLASH_RD;
		/* flash_start.bf.nflash_random_access = RND_ENABLE; */
		write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		while (reg_val & FLASH_COMPLETE_MASK) {
			udelay(1);
			schedule();
			reg_val =
			    read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		}
		pread[i] = read_flash_ctrl_reg(FLASH_NF_DATA);
	}

	if (chip->oob_poi[chip->badblockpos] != 0xff)
		mtd->ecc_stats.failed++;

	return 0;
}

/**
 * ca_nand_write_oob_std - [REPLACEABLE] the most
 * common OOB data write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @page:	page number to write
 */
static int ca_nand_write_oob_std(struct nand_chip *chip,
				 int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int status = 0, i;
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;

	check_flash_ctrl_status();

	chip->legacy.cmdfunc(chip, NAND_CMD_SEQIN, mtd->writesize, page);
	/* chip->legacy.write_buf(mtd, buf, length); */
	/* Send command to program the OOB data */
	chip->legacy.cmdfunc(chip, NAND_CMD_PAGEPROG, -1, -1);

	/* disable ecc gen */
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0x0);

	flash_nf_count = ((mtd->oobsize - 1) << 22) | NCNT_EMPTY_DATA;

	flash_nf_address_2 = 0;

	if (nanddev_target_size(&chip->base) < SZ_32M) {
		flash_nf_count |= NCNT_ADDR_3;

		if (mtd->writesize > SZ_512) {
			flash_nf_count |= NCNT_CMD_2;
			flash_nf_command = NAND_CMD_SEQIN |
					   (NAND_CMD_PAGEPROG << 8);
		} else {
			flash_nf_count |= NCNT_CMD_3;
			flash_nf_command = NAND_CMD_READOOB |
					   (NAND_CMD_SEQIN << 8) |
					   (NAND_CMD_PAGEPROG << 16);
		}

		/* read oob need to add page data size
		 * to match correct oob ddress
		 */
		flash_nf_address_1 = (((page & 0x00ffffff) << 8));
		flash_nf_address_2 = ((page & 0xff000000) >> 24);
	} else if (nanddev_target_size(&chip->base) <= SZ_128M) {
		flash_nf_count |= NCNT_ADDR_4 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 =
		    (((page & 0xffff) << 16) + (mtd->writesize & 0xffff));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);

	} else {		/* if((nanddev_target_size(&chip->base) > (128 << 20)) )) */

		flash_nf_count |= NCNT_ADDR_5 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 =
		    (((page & 0xffff) << 16) + (mtd->writesize & 0xffff));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);
	}

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	/* write read id command */
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	/* write address 0x0 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	pwrite = (unsigned int *)chip->oob_poi;

	for (i = 0; i < ((mtd->oobsize / 4)); i++) {
		reg_val = CHIP_EN | NFLASH_REG_WIDTH32;
		/* nf_access.bf.nflashDirWr = ; */
		write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

		write_flash_ctrl_reg(FLASH_NF_DATA, pwrite[i]);

		reg_val = FLASH_GO | FLASH_WT;
		/* flash_start.bf.nflash_random_access = RND_ENABLE; */
		write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		while (reg_val) {
			udelay(1);
			schedule();
			reg_val =
			    read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		}
		/* pwrite[i] = read_flash_ctrl_reg(FLASH_NF_DATA); */
	}

	status = chip->legacy.waitfunc(chip);

	return status & NAND_STATUS_FAIL ? -EIO : 0;
}

static void fill_bch_oob_data(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	u32 i, j, flash_nf_bch_gen0_0 = 0;
	int eccsteps = chip->ecc.steps;
	u8 *ecc_calc = ca_host->calc_buf;

	const u32 reg_offset = FLASH_NF_BCH_GEN0_1 - FLASH_NF_BCH_GEN0_0;
	const u32 group_offset = FLASH_NF_BCH_GEN1_0 - FLASH_NF_BCH_GEN0_0;

	u32 addr = FLASH_NF_BCH_GEN0_0;
	u8 *ecc_end_pos;

	for (; eccsteps; --eccsteps, addr += group_offset) {
		ecc_end_pos = ecc_calc + chip->ecc.bytes;

		for (i = 0; ecc_calc != ecc_end_pos; ++i) {
			flash_nf_bch_gen0_0 =
			    read_flash_ctrl_reg(addr + reg_offset * i);

			for (j = 0; j < 4 && ecc_calc != ecc_end_pos;
			     ++j, ++ecc_calc)
				*ecc_calc = ((flash_nf_bch_gen0_0 >> (j * 8)) &
					     0xff);
		}
	}

	mtd_ooblayout_set_eccbytes(mtd, ca_host->calc_buf,
				   chip->oob_poi, 0, chip->ecc.total);
}

static inline int flip_bit(u8 *data, u32 err_loc)
{
	u32 offset, bit;

	offset = (err_loc & 0x3fff) >> 3;
	if (offset >= BCH_DATA_UNIT)
		return 0;

	bit = err_loc & 0x07;
	data[offset] ^= (1 << bit);

	return 1;
}

static int do_bits_correction(u8 *data, u32 err_num)
{
	int i;
	u32 err_loc;
	u32 bitflip_cnt = 0;

	for (i = 0; i < (err_num + 1) / 2; i++) {
		err_loc = read_flash_ctrl_reg(FLASH_NF_BCH_ERROR_LOC01 + i * 4);

		if (flip_bit(data, err_loc))
			bitflip_cnt += 1;

		if (((i + 1) * 2) > err_num)
			break;

		if (flip_bit(data, err_loc >> 16))
			bitflip_cnt += 1;
	}

	return bitflip_cnt;
}

static int ca_nand_bch_correct_data(struct mtd_info *mtd, unsigned char *p,
				    unsigned char *ecc_code)
{
	int i, j, count = 0;
	unsigned char *ecc_end_pos;
	struct nand_chip *chip = mtd_to_nand(mtd);
	u32 bch_oob, reg_val;
	int decode_status;
	int codesel;

	/* ugly */
	codesel = (ecc_code - ca_host->code_buf) / chip->ecc.bytes;

	ecc_end_pos = ecc_code + chip->ecc.bytes;
	for (i = 0; i < chip->ecc.bytes; i += 4) {
		bch_oob = 0;
		for (j = 0; j < 4 && ecc_code != ecc_end_pos; ++j, ++ecc_code)
			bch_oob |= (*ecc_code) << (8 * j);

		write_flash_ctrl_reg(FLASH_NF_BCH_OOB0 + i, bch_oob);
	}

	/* enable ecc compare */
	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_CONTROL);
	reg_val &= ~FLASH_NF_BCH_CONTROL__BCH_CODE_SEL;
	reg_val |= FIELD_PREP(FLASH_NF_BCH_CONTROL__BCH_CODE_SEL, codesel);
	reg_val &= ~FLASH_NF_BCH_CONTROL__BCH_COMPARE;
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	reg_val |= FLASH_NF_BCH_CONTROL__BCH_COMPARE;
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	while (!(reg_val & FLASH_NF_BCH_STATUS__BCH_DEC_DONE)) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	}

	decode_status = reg_val & FLASH_NF_BCH_STATUS__BCH_DEC_STATUS;
	if (decode_status == BCH_CORRECTABLE_ERR) {
		count = FIELD_GET(FLASH_NF_BCH_STATUS__BCH_ERR_NUM, reg_val);
		do_bits_correction(p, count);
	} else if (decode_status == BCH_UNCORRECTABLE) {
		count = -EBADMSG;
	}

	/* disable ecc compare */
	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_CONTROL);
	reg_val &= ~FLASH_NF_BCH_CONTROL__BCH_COMPARE;
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	return count;
}

static int bch_correct(struct mtd_info *mtd, struct nand_chip *chip,
		       uint8_t *buf)
{
	int i, eccsize = chip->ecc.size, ret;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	u8 *p = buf;
	unsigned int max_bitflips = 0;
	u8 *ecc_code = ca_host->code_buf;

	eccbytes = chip->ecc.bytes;
	eccsize = chip->ecc.size;

	ret = mtd_ooblayout_get_eccbytes(mtd, ecc_code, chip->oob_poi, 0,
					 chip->ecc.total);
	if (ret)
		return ret;

	eccsteps = chip->ecc.steps;
	p = buf;

	for (i = 0, ret = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		int stat;

		stat = ca_nand_bch_correct_data(mtd, p,  &ecc_code[i]);
		if (stat == -EBADMSG &&
		    (chip->ecc.options & NAND_ECC_GENERIC_ERASED_CHECK)) {
			/* check for empty pages with bitflips */
			stat = nand_check_erased_ecc_chunk(p, eccsize,
							   &ecc_code[i],
							   eccbytes,
							   NULL, 0,
							   chip->ecc.strength);
		}

		if (stat < 0) {
			mtd->ecc_stats.failed++;
			ret = -EIO;
			break;
		}

		mtd->ecc_stats.corrected += stat;
		max_bitflips = max_t(unsigned int, max_bitflips, stat);
	}
	return ret ? ret : max_bitflips;
}

static int do_ca_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				u8 *buf, int page)
{
	unsigned int addr;
	u8 *vaddr;
	unsigned long page_64 = (unsigned long)page;
	unsigned long buf_adr, pa;
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;
	u32 dma_rx_rptr_index = 0;
	u32 dma_tx_wptr_index = 0;
	u32 unmap_index = 0;

	check_flash_ctrl_status();
	/* disable txq5 */
	reg_val = DISABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);
	/* for indirect access with DMA, because DMA not ready  */
	flash_nf_count = ((mtd->oobsize - 1) << 22) |
			  ((mtd->writesize - 1) << 8);

	if (nanddev_target_size(&chip->base) < SZ_32M) {
		flash_nf_count |= NCNT_ADDR_3 | NCNT_CMD_1;
		flash_nf_command = NAND_CMD_READ0;
		flash_nf_address_1 = (((page & 0x00ffffff) << 8));
		flash_nf_address_2 = ((page & 0xff000000) >> 24);
	} else if (nanddev_target_size(&chip->base) <= SZ_128M) {
		flash_nf_count |= NCNT_ADDR_4;
		flash_nf_command = NAND_CMD_READ0;
		if (mtd->writesize > SZ_512) {
			flash_nf_count |= NCNT_CMD_2;
			flash_nf_command = (NAND_CMD_READSTART << 8);
		} else {
			flash_nf_count |= NCNT_ADDR_4 | NCNT_CMD_1;
		}
		flash_nf_address_1 = (((page & 0xffff) << 16));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);
	} else {		/* if((nanddev_target_size(&chip->base) > SZ_128M ) )) */
		flash_nf_count |= NCNT_ADDR_5 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_READ0 | (NAND_CMD_READSTART << 8);
		flash_nf_address_1 = (((page_64 & 0xffff) << 16));
		flash_nf_address_2 = ((page_64 & 0xffff0000) >> 16);
	}

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	/* write read id command */
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	/* write address 0x0 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	flash_nf_count = read_flash_ctrl_reg(FLASH_NF_COUNT);
	flash_nf_command = read_flash_ctrl_reg(FLASH_NF_COMMAND);
	flash_nf_address_1 = read_flash_ctrl_reg(FLASH_NF_ADDRESS_1);
	flash_nf_address_2 = read_flash_ctrl_reg(FLASH_NF_ADDRESS_2);

	reg_val = CHIP_EN | NFLASH_REG_WIDTH8 |
		((page_64 << chip->page_shift) / SZ_128M);
	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

	reg_val = read_flash_ctrl_reg(FLASH_NF_ACCESS);

	addr =
	    (page_64 << chip->page_shift) % SZ_128M +
	    ca_host->flash_mem_base;
	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= (uint8_t *)high_memory) {
		struct page *p1;
		if (((size_t)buf & PAGE_MASK) !=
		    ((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);
	}
	goto out_copy_done;

 out_copy:
	vaddr = buf;
	ca_host->pagebuf = -1;
	chip->pagecache.page = -1;
	buf = chip->data_buf;
 out_copy_done:

	dma_tx_wptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR);
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->writesize;
	tx_desc[dma_tx_wptr_index].buf_adr = addr;

	/* page data rx desc */
	dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR);
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->writesize;
	buf_adr = dma_map_single(ca_host->dev, (void *)buf,
				 mtd->writesize, DMA_FROM_DEVICE);
	rx_desc[dma_rx_rptr_index].buf_adr = (unsigned int)buf_adr;
	rx_desc[dma_rx_rptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);

	unmap_index = dma_rx_rptr_index;

	wmb(); /* make sure the descriptor value is updated */

	/* set axi_bus_len = 8 */

	/* set fifo control */
	reg_val = FIFO_FLASH_RD;
	write_flash_ctrl_reg(FLASH_FIFO_CONTROL, reg_val);

	reg_val = FLASH_FIFO_GO;
	/* flash_start.bf.nflashRegCmd = FLASH_RD; */
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	/* update tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);
	/* dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR); */

	/* enable txq5 */
	reg_val = ENABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);

	//#define DMA_DMA_SSP_TXQ5_RPTR		    0xf009046c

	while (!(reg_val & RX_INTERRUPT_MASK)) {	//444 + 2
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {	//46c +2
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}
	pa = rx_desc[unmap_index].buf_adr | ((unsigned long)rx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->writesize, DMA_FROM_DEVICE);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	if (vaddr != 0) {
		memcpy(vaddr, buf, mtd->writesize);
		buf = vaddr;
		vaddr = 0;
	}

	/* oob tx desc */
	//dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;

	//addr +=  mtd->writesize;
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->oobsize;
	tx_desc[dma_tx_wptr_index].buf_adr = (addr + mtd->writesize);

	/* oob rx desc */
	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->oobsize;
	buf_adr = dma_map_single(ca_host->dev, (void *)chip->oob_poi,
				 mtd->oobsize, DMA_FROM_DEVICE);
	rx_desc[dma_rx_rptr_index].buf_adr = (unsigned int)buf_adr;
	rx_desc[dma_rx_rptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);

	unmap_index = dma_rx_rptr_index;

	wmb();  /* make sure the descriptor value is updated */
	/* set axi_bus_len = 8 */

	/* update tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);
	/* dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR); */

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	//#define DMA_DMA_SSP_TXQ5_RPTR		    0xf009046c

	while (!(reg_val & RX_INTERRUPT_MASK)) {	//444 + 2
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {	//46c +2
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}

	reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	while (reg_val & FIFO_COMPLETE_MASK) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	}
	pa = rx_desc[unmap_index].buf_adr | ((unsigned long)rx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->oobsize, DMA_FROM_DEVICE);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR, dma_rx_rptr_index);

	wmb(); /* make sure the dma RX pointer value is updated */

	return 0;
}

static int ca_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
			     u8 *buf, int page)
{
	int ret;

	if (!((unsigned long)buf & 0xf))
		return do_ca_nand_read_page(mtd, chip, buf, page);

	/*  not aligned */
	ret = do_ca_nand_read_page(mtd, chip, tmp_buf, page);
	memcpy(buf, tmp_buf, mtd->writesize);

	return ret;
}

/**
 * ca_nand_read_page_hwecc - [REPLACEABLE] hardware ecc based page read function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	buffer to store read data
 * @page:	page number to read
 *
 * Not for syndrome calculating ecc controllers which need a special oob layout
 */
static int ca_nand_read_page_hwecc(struct nand_chip *chip,
				   u8 *buf, int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int bitflips = 0;
	u32 reg_val = 0;

	ca_rst_ecc_bch_registers();
	reg_val = BCH_ENABLE | BCH_DECODE | (ca_host->bch_cap << 9);
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	ca_nand_read_page(mtd, chip, buf, page);

	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	while (!(reg_val & BCH_COMPLETE_MASK)) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	}
#ifdef	NAND_ECC_TEST
	memset(tr, 0, NAND_MAX_PAGESIZE);
	memcpy(tr, buf, mtd->writesize);
	addr = ((page << chip->page_shift) % SZ_128M) + ca77x_host->io_base;
#endif

	bitflips = bch_correct(mtd, chip, buf);

	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, 0);
	return bitflips;
}

/**
 * ca_nand_write_page_hwecc - [REPLACEABLE] hardware ecc based
 * page write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	data buffer
 */
static int ca_nand_write_page_hwecc(struct nand_chip *chip, const uint8_t *buf,
				    int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned long page_64;
	int col;
	u32 addr;
	u8 *vaddr;
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;
	u32 dma_rx_rptr_index = 0;
	u32 dma_tx_wptr_index = 0;
	u32 unmap_index = 0;
	unsigned long buf_adr, pa;

	page_64 = (unsigned long)page;
	col = nand_col;

	check_flash_ctrl_status();

	ca_rst_ecc_bch_registers();

	reg_val = DISABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	reg_val = BCH_ENABLE | BCH_ENCODE | (ca_host->bch_cap << 9);
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	flash_nf_count = ((mtd->oobsize - 1) << 22) |
			  ((mtd->writesize - 1) << 8);

	if (nanddev_target_size(&chip->base) < SZ_32M) {
		flash_nf_count |= NCNT_ADDR_3 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0x00ffffff) << 8));
		flash_nf_address_2 = ((page & 0xff000000) >> 24);

	} else if (nanddev_target_size(&chip->base) <= SZ_128M) {//TODO different with raw
		flash_nf_count |= NCNT_ADDR_4 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0xffff) << 16));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);

	} else {		/* if((nanddev_target_size(&chip->base) > SZ_128M ) )) */

		flash_nf_count |= NCNT_ADDR_5 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0xffff) << 16));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);
	}

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	reg_val = CHIP_EN | NFLASH_REG_WIDTH8 |
			((page_64 << chip->page_shift) / SZ_128M);
	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

	addr =
	    (page_64 << chip->page_shift) % SZ_128M +
	    ca_host->flash_mem_base;

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= (uint8_t *)high_memory) {
		struct page *p1;
		if (((size_t)buf & PAGE_MASK) !=
		    ((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);
	}
	goto out_copy_done;
 out_copy:
	vaddr = (uint8_t *)buf;
	ca_host->pagebuf = -1;
	chip->pagecache.page = -1;
	buf = chip->data_buf;
	memcpy((uint8_t *)buf, vaddr, mtd->writesize);
 out_copy_done:

	/* page data tx desc */
	dma_tx_wptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR);
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->writesize;
	buf_adr = dma_map_single(ca_host->dev, (void *)buf,
				 mtd->writesize, DMA_TO_DEVICE);
	tx_desc[dma_tx_wptr_index].buf_adr = (unsigned int)buf_adr;
	tx_desc[dma_tx_wptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);

	unmap_index = dma_tx_wptr_index;

#ifdef	NAND_ECC_TEST
	memset(tw, 0, NAND_MAX_PAGESIZE);
	memcpy(tw, buf, mtd->writesize);
#endif

	dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR) &
					      RPTR_INDEX_MASK;
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->writesize;
	rx_desc[dma_rx_rptr_index].buf_adr = addr;

	/* update page tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);
	/* set axi_bus_len = 8 */
	/* set fifo control */
	reg_val = FIFO_FLASH_WT;
	write_flash_ctrl_reg(FLASH_FIFO_CONTROL, reg_val);

	wmb();  /* make sure the descriptor value is updated */

	reg_val = FLASH_FIFO_GO;
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	/* enable txq5 */
	reg_val = ENABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	/**
	 * if(dma_ssp_rxq5_intsts.bf.rxq5_full)
	  {
	  printk("rxq5_full\n");
	  while(1);
	  }
	 **/
	while (!(reg_val & RX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}
	pa = tx_desc[unmap_index].buf_adr | ((unsigned long)tx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->writesize, DMA_TO_DEVICE);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	while (!(reg_val & BCH_COMPLETE_MASK)) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_STATUS);
	}

	/* disable ecc gen */
	reg_val = read_flash_ctrl_reg(FLASH_NF_BCH_CONTROL);
	reg_val &= ~BCH_ENABLE;
	write_flash_ctrl_reg(FLASH_NF_BCH_CONTROL, reg_val);

	/* printk("write page ecc(page %x) : ", page); */

	fill_bch_oob_data(chip);

	/* oob rx desc */
	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->oobsize;
	rx_desc[dma_rx_rptr_index].buf_adr = (addr + mtd->writesize);

	/* dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR); */
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->oobsize;
	buf_adr = dma_map_single(ca_host->dev, (void *)chip->oob_poi,
				 mtd->oobsize, DMA_TO_DEVICE);
	tx_desc[dma_tx_wptr_index].buf_adr = (unsigned int)buf_adr;
	tx_desc[dma_tx_wptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);

	unmap_index = dma_tx_wptr_index;

	wmb();  /* make sure the descriptor value is updated */

	/* update tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	/**
	 * if(dma_ssp_rxq5_intsts.bf.rxq5_full)
	  {
	  printk("rxq5_full\n");
	  while(1);
	  }
	 **/
	while (!(reg_val & RX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}

	reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	while (reg_val & FIFO_COMPLETE_MASK) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	}
	pa = tx_desc[unmap_index].buf_adr | ((unsigned long)tx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->oobsize, DMA_TO_DEVICE);

	/* update rx read ptr */
	/* dma_rxq5_rptr.wrd = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR); */
	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR, dma_rx_rptr_index);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	if (vaddr != 0) {
		buf = vaddr;
		vaddr = 0;
	}

	usleep_range(250, 350);

	return 0;
}

/**
 * ca_nand_read_page_raw - [Intern] read raw page data without ecc
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	buffer to store read data
 * @oob_required: caller need oob data, read to chip->oob_poi
 * @page:	page number to read
 *
 * Not for syndrome calculating ecc controllers, which use a special oob layout
 */
static int ca_nand_read_page_raw(struct nand_chip *chip,
				 u8 *buf, int oob_requied, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	check_flash_ctrl_status();
	ca_rst_ecc_bch_registers();

	return ca_nand_read_page(mtd, chip, buf, page);
}

/**
 * ca_nand_write_page_raw - [Intern] raw page write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	data buffer
 *
 * Not for syndrome calculating ecc controllers, which use a special oob layout
 */
static int ca_nand_write_page_raw(struct nand_chip *chip,
				  const u8 *buf, int oob_required,
				  int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	unsigned long page_64;
	unsigned long buf_adr, pa;
	u32 addr;
	u8 *vaddr;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;
	u32 reg_val = 0;
	u32 dma_rx_rptr_index = 0, dma_tx_wptr_index = 0;
	u32 unmap_index = 0;

	check_flash_ctrl_status();

	page_64 = (unsigned long)page;

	ca_rst_ecc_bch_registers();

	/* disable txq5 */
	reg_val = DISABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	flash_nf_count |= ((mtd->oobsize - 1) << 22) |
			   ((mtd->writesize - 1) << 8);

	if (nanddev_target_size(&chip->base) < (32 << 20)) {
		flash_nf_count |= NCNT_ADDR_3 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0x00ffffff) << 8));
		flash_nf_address_2 = ((page & 0xff000000) >> 24);

	} else if ((nanddev_target_size(&chip->base) >= (32 << 20)) &&
		   (nanddev_target_size(&chip->base) <= (128 << 20))) {
		flash_nf_count |= NCNT_ADDR_4 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0xffff) << 16));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);

	} else {		/* if((nanddev_target_size(&chip->base) > (128 << 20)) )) */

		flash_nf_count |= NCNT_ADDR_5 | NCNT_CMD_2;
		flash_nf_command = NAND_CMD_SEQIN | (NAND_CMD_PAGEPROG << 8);
		flash_nf_address_1 = (((page & 0xffff) << 16));
		flash_nf_address_2 = ((page & 0xffff0000) >> 16);
	}

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	reg_val = CHIP_EN | NFLASH_REG_WIDTH8 |
		(((page_64 << chip->page_shift) / SZ_128M));
	/* write_flash_ctrl_reg(FLASH_NF_ACCESS, nf_access.wrd); */

	/* write */
	/* prepare dma descriptor */
	/* chip->data_buf */
	/* nf_access.wrd = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START); */
	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

	addr =
	    ((page_64 << chip->page_shift) % SZ_128M) +
	    ca_host->flash_mem_base;

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= (uint8_t *)high_memory) {
		struct page *p1;

		if (((size_t)buf & PAGE_MASK) !=
		    ((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);
	}
	goto out_copy_done;
 out_copy:
	vaddr = (uint8_t *)buf;
	ca_host->pagebuf = -1;
	chip->pagecache.page = -1;
	buf = chip->data_buf;
	memcpy((uint8_t *)buf, vaddr, mtd->writesize);
 out_copy_done:

	/* page data tx desc */
	dma_tx_wptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR) &
					      WPTR_INDEX_MASK;
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->writesize;
	buf_adr = dma_map_single(ca_host->dev, (void *)buf,
				 mtd->writesize, DMA_TO_DEVICE);
	tx_desc[dma_tx_wptr_index].buf_adr = (unsigned int)buf_adr;
	tx_desc[dma_tx_wptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);
	unmap_index = dma_tx_wptr_index;

#ifdef	NAND_ECC_TEST
	memset(tw, 0, NAND_MAX_PAGESIZE);
	memcpy(tw, buf, mtd->writesize);
#endif

	/* page data rx desc */
	dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR);
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->writesize;
	rx_desc[dma_rx_rptr_index].buf_adr = addr;

	/* oob rx desc */
	addr = (uint32_t)addr + (uint32_t)mtd->writesize;

	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	rx_desc[dma_rx_rptr_index].own = OWN_DMA;
	rx_desc[dma_rx_rptr_index].buf_len = mtd->oobsize;
	rx_desc[dma_rx_rptr_index].buf_adr = addr;

	wmb();  /* make sure the descriptor value is updated */

	/* update page tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);
	/* set axi_bus_len = 8 */
	/* set fifo control */
	reg_val = FIFO_FLASH_WT;
	write_flash_ctrl_reg(FLASH_FIFO_CONTROL, reg_val);

	reg_val = FLASH_FIFO_GO;
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	/* enable txq5 */
	reg_val = ENABLE_TXQ;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL, reg_val);

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	while (!(reg_val & RX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}
	pa = tx_desc[unmap_index].buf_adr | ((unsigned long)tx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->writesize, DMA_TO_DEVICE);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	/* dma_rx_rptr_index = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR); */
	tx_desc[dma_tx_wptr_index].own = OWN_DMA;
	tx_desc[dma_tx_wptr_index].buf_len = mtd->oobsize;
	buf_adr = dma_map_single(ca_host->dev, (void *)chip->oob_poi,
				 mtd->oobsize, DMA_TO_DEVICE);
	tx_desc[dma_tx_wptr_index].buf_adr = (unsigned int)buf_adr;
	tx_desc[dma_tx_wptr_index].buf_adr_hi = (unsigned int)(buf_adr >> 32);

	unmap_index = dma_tx_wptr_index;

	wmb(); /* make sure the descriptor value is updated */

	/* update tx write ptr */
	dma_tx_wptr_index = (dma_tx_wptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_WPTR, dma_tx_wptr_index);

	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	while (!(reg_val & RX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	}
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	while (!(reg_val & TX_INTERRUPT_MASK)) {
		udelay(1);
		schedule();
		reg_val =
		    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	}

	reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	while (reg_val & FIFO_COMPLETE_MASK) {
		udelay(1);
		schedule();
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	}
	pa = tx_desc[unmap_index].buf_adr | ((unsigned long)tx_desc[unmap_index].buf_adr_hi << 32);
	dma_unmap_single(ca_host->dev, pa, mtd->oobsize, DMA_TO_DEVICE);

	/* update rx read ptr */
	/* dma_rxq5_rptr.wrd = read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR); */
	dma_rx_rptr_index = (dma_rx_rptr_index + 1) % FDMA_DESC_NUM;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_RPTR, dma_rx_rptr_index);

	/* clr tx/rx eof */
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_COAL_INTERRUPT,
			   reg_val);
	reg_val =
	    read_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT);
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_COAL_INTERRUPT,
			   reg_val);

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	if (vaddr != 0) {
		buf = vaddr;
		vaddr = 0;
	}

	return 0;
}

/**
 * ca_nand_read_subpage - [REPLACEABLE] ECC based sub-page read function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @data_offs: offset of requested data within the page
 * @readlen: data length
 * @bufpoi: buffer to store read data
 * @page: page number to read
 */
static int ca_nand_read_subpage(struct nand_chip *chip,
				u32 data_offs, u32 readlen,
				u8 *bufpoi, int page)
{
	int max_bitflips = 0;

	max_bitflips =
	    chip->ecc.read_page(chip, chip->data_buf, 1, page);
	memcpy(bufpoi, &chip->data_buf[data_offs], readlen);

	return max_bitflips;
}

/**
 * ca_nand_erase_block - [GENERIC] erase a block
 * @mtd:	MTD device structure
 * @page:	page address
 *
 * Erase a block.
 */

static int ca_nand_erase_block(struct nand_chip *chip)
{
	/*      int opcode,tst=0,tst1=0,tst2=0; */
	struct mtd_info *mtd = nand_to_mtd(chip);
	u64 test;
	unsigned long timeo;
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;

	/* Send commands to erase a page */
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0);	/*  */

	flash_nf_count |= NCNT_EMPTY_OOB | NCNT_EMPTY_DATA | NCNT_CMD_2;

	/*
	 * test = nanddev_target_size(&chip->base);
	 * test = test / mtd->writesize;
	 * if((nanddev_target_size(&chip->base)/mtd->writesize) > 0x10000)
	 */

	test = 0x10000UL * mtd->writesize;
	if (nanddev_target_size(&chip->base) > test)
		flash_nf_count |= NCNT_ADDR_3;
	else
		flash_nf_count |= NCNT_ADDR_2;

	flash_nf_command = NAND_CMD_ERASE1 | (NAND_CMD_ERASE2 << 8);
	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);

	reg_val = CHIP_EN | NFLASH_REG_WIDTH8;

	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);
	reg_val = FLASH_GO | FLASH_RD; /* no data access use read.. */
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	timeo = jiffies + HZ * 10;
	do {
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		if (!(reg_val & FLASH_COMPLETE_MASK))
			return 0;
	} while (time_before(jiffies, timeo));

	pr_info("%s: %x\n", __func__, reg_val);
	check_flash_ctrl_status();

	return 0;
}

/* <== Cortina-Access added function */

/**
 * nand_read_byte - [DEFAULT] read one byte from the chip
 * @mtd: MTD device structure
 *
 * Default read function for 8bit buswidth
 */

static int read_byte_index = 5;

static uint8_t ca_nand_read_byte_wait(struct mtd_info *mtd)
{
	u8 opcode;
	unsigned long timeo;
	u32 reg_val = 0;

	reg_val = FLASH_GO | FLASH_RD;
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	timeo = jiffies + HZ;
	do {
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
		if (!(reg_val & FLASH_COMPLETE_MASK))
			break;
	} while (time_before(jiffies, timeo));

	opcode = read_flash_ctrl_reg(FLASH_NF_DATA);
	read_byte_index += 1;
	if (read_byte_index >= sizeof(uint32_t))
		read_byte_index = 0;

	return (opcode >> (read_byte_index << 3)) & 0xff;
}

static uint8_t ca_nand_read_byte(struct nand_chip *chip)
{
	u32 opcode;
	u32 reg_val = 0;

	reg_val = FLASH_GO | FLASH_RD;
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	while (reg_val & FLASH_COMPLETE_MASK) {
		usleep_range(10, 1000);
		reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	}
	opcode = read_flash_ctrl_reg(FLASH_NF_DATA);

	read_byte_index += 1;
	if (read_byte_index >= sizeof(uint32_t))
		read_byte_index = 0;

	return (opcode >> (read_byte_index << 3)) & 0xff;
}

/**
 * ca_nand_select_chip - [DEFAULT] control CE line
 * @mtd: MTD device structure
 * @chipnr: chipnumber to select, -1 for deselect
 *
 * Default select function for 1 chip devices.
 */
static void ca_nand_select_chip(struct nand_chip *chip, int chipnr)
{
	switch (chipnr) {
	case -1:
	case 0:
		CHIP_EN = NFLASH_CHIP0_EN;
		break;
	case 1:
		CHIP_EN = NFLASH_CHIP1_EN;
		break;

	default:
		/* BUG(); */
		CHIP_EN = NFLASH_CHIP0_EN;
	}
}

/**
 * ca_nand_write_buf - [DEFAULT] write buffer to chip
 * @mtd: MTD device structure
 * @buf: data buffer
 * @len: number of bytes to write
 *
 * Default write function for 8bit buswidth.
 */
static void ca_nand_write_buf(struct nand_chip *chip, const uint8_t *buf,
			      int len)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int i, page = 0, col = 0;

	if (len <= (mtd->writesize + mtd->oobsize)) {
		page = nand_page;
		col = nand_col;
		ca_host->pagebuf = -1;
		chip->pagecache.page = -1;
		chip->ecc.read_page(chip, chip->data_buf, 1, page);

		for (i = 0; i < len; i++)
			chip->data_buf[col + i] = buf[i];

		chip->ecc.write_page(chip, chip->data_buf, 1,
				     page);
	}
}

/**
 * ca_nand_read_buf - [DEFAULT] read chip data into buffer
 * @mtd: MTD device structure
 * @buf: buffer to store date
 * @len: number of bytes to read
 *
 * Default read function for 8bit buswidth.
 */
static void ca_nand_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	int page, col;

	if (len <= (mtd->writesize + mtd->oobsize)) {
		page = nand_page;
		col = nand_col;

		chip->ecc.read_page(chip, chip->data_buf, 1, page);
		memcpy(buf, &chip->data_buf[col], len);
	}
}

static void ca_nand_generic_cmd(u32 cmd, int sub_cmd, u32 data_cnt,
				u32 addr_cnt)
{
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;

	ca_nand_reset_controller();
	check_flash_ctrl_status();

	/* disable ecc gen */
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0x0);

	flash_nf_count = (NCNT_EMPTY_OOB | (data_cnt << 8) |
			  NCNT_ADDR_1 | NCNT_CMD_1);

	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);

	flash_nf_command = cmd;
	/* write read id command */
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	flash_nf_address_1 = sub_cmd;
	/* write address 0x00 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	flash_nf_address_2 = 0;
	/* write address 0x00 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);

	/* read maker code */
	reg_val = CHIP_EN | NFLASH_REG_WIDTH8;
	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

	read_byte_index = 5;
}

static void ca_nand_CMD_READID(unsigned int command,
			       int column, int page_addr)
{
	u32 data_cnt;

	switch (column) {
	case 20:		/* subcommand 0x20 for ONFI signature */
		data_cnt = NCNT_DATA_4;
		break;
	case 40:		/* subcommand 0x40 for JEDEC  */
		data_cnt = NCNT_DATA_5;
		break;
	case 0:
	default:
		data_cnt = NCNT_DATA_8;
	}

	return ca_nand_generic_cmd(NAND_CMD_READID, column, data_cnt,
				   NCNT_ADDR_1);
}

static void ca_nand_CMD_PARAM(unsigned int command,
			      int column, int page_addr)
{
	u32 data_cnt = 0;

	if (column == 0)
		data_cnt = 3 * sizeof(struct nand_onfi_params) - 1;
	else if (column == 40)
		data_cnt = sizeof(struct nand_jedec_params) - 1;

	return ca_nand_generic_cmd(NAND_CMD_PARAM, column, data_cnt,
				   NCNT_ADDR_1);
}

static void ca_nand_CMD_STATUS(unsigned int command,
			       int column, int page_addr)
{
	return ca_nand_generic_cmd(NAND_CMD_STATUS, 0, NCNT_ADDR_1,
				   NCNT_EMPTY_ADDR);
}

static void ca_nand_CMD_RESET(struct nand_chip *chip)
{
	u32 reg_val = 0;
	u32 flash_nf_count = 0, flash_nf_command = 0;
	u32 flash_nf_address_1 = 0, flash_nf_address_2 = 0;

	/* Check flash status */
	check_flash_ctrl_status();
	udelay(chip->legacy.chip_delay);

	/* Disable ecc control */
	write_flash_ctrl_reg(FLASH_NF_ECC_CONTROL, 0x0);

	/* Write data, address and command count */
	flash_nf_count = NCNT_EMPTY_OOB | NCNT_EMPTY_DATA |
			  NCNT_EMPTY_ADDR | NCNT_CMD_1;
	write_flash_ctrl_reg(FLASH_NF_COUNT, flash_nf_count);
	/* Write read id command */
	flash_nf_command = NAND_CMD_RESET;
	write_flash_ctrl_reg(FLASH_NF_COMMAND, flash_nf_command);
	/* Write address 0x00 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, flash_nf_address_1);
	/* Write address 0x00 */
	write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, flash_nf_address_2);
	/* Enable flash and set register width */
	reg_val = CHIP_EN | NFLASH_REG_WIDTH8;
	write_flash_ctrl_reg(FLASH_NF_ACCESS, reg_val);

	/* Issue command */
	reg_val = FLASH_GO | FLASH_WT;
	write_flash_ctrl_reg(FLASH_FLASH_ACCESS_START, reg_val);

	reg_val = read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	while (reg_val & FLASH_COMPLETE_MASK) {
		udelay(1);
		schedule();
		reg_val =
		    read_flash_ctrl_reg(FLASH_FLASH_ACCESS_START);
	}
}

/**
 * ca_nand_command
 * @mtd: MTD device structure
 * @command: the command to be sent
 * @column: the column address for chip command, -1 if none
 * @page_addr: the page address for chip command, -1 if none
 */
void ca_nand_command(struct nand_chip *chip, unsigned int command,
		     int column, int page_addr)
{
	/*
	 * program and erase have their own busy handlers
	 * status and sequential in needs no delay
	 */
	switch (command) {
	case NAND_CMD_READID:
		return ca_nand_CMD_READID(command, column, page_addr);
	case NAND_CMD_PARAM:
		return ca_nand_CMD_PARAM(command, column, page_addr);
	case NAND_CMD_PAGEPROG:
	case NAND_CMD_ERASE1:
		/* Check flash status */
		check_flash_ctrl_status();

		/* Write address */
		write_flash_ctrl_reg(FLASH_NF_ADDRESS_1, page_addr);
		write_flash_ctrl_reg(FLASH_NF_ADDRESS_2, 0);
		return;
	case NAND_CMD_ERASE2:
		ca_nand_erase_block(chip);
		return;
	case NAND_CMD_SEQIN:
	case NAND_CMD_STATUS:
		return ca_nand_CMD_STATUS(command, column, page_addr);
	case NAND_CMD_RESET:
		return ca_nand_CMD_RESET(chip);
	default:
		/*
		 * If we don't have access to the busy pin, we apply the given
		 * command delay
		 */
		if (!chip->legacy.dev_ready(chip)) {
			udelay(chip->legacy.chip_delay);
			return;
		}
	}

	/* Apply chip short delay always to ensure that we do wait tWB in
	 * any case on any machine.
	 */
	udelay(chip->legacy.chip_delay * 10);
	nand_wait_ready(chip);
}

/**
 * nand_wait - [DEFAULT] wait until the command is done
 * @mtd: MTD device structure
 * @chip: NAND chip structure
 *
 * Wait for command done. This applies to erase and program only. Erase can
 * take up to 400ms and program up to 20ms according to general NAND and
 * SmartMedia specs.
 */
static int ca_nand_wait(struct nand_chip *chip)
{
	unsigned long timeo = jiffies;
	/* unsigned long	timeo = jiffies + 2; */
	int status;

	timeo += (HZ * 20) / 1000;

	/* wait until command is processed or timeout occures */
	do {
		if (chip->legacy.dev_ready(chip))
			break;
		touch_softlockup_watchdog();
	} while (time_before(jiffies, timeo));

	/* Apply chip short delay always to ensure that we do wait tWB in
	 * any case on any machine.
	 */
	udelay(chip->legacy.chip_delay * 10);

	chip->legacy.cmdfunc(chip, NAND_CMD_STATUS, -1, -1);

	while (time_before(jiffies, timeo)) {
		if (chip->legacy.dev_ready(chip)) {
			if (chip->legacy.dev_ready(chip))
				break;
		} else {
			/* if (chip->legacy.read_byte(mtd) & NAND_STATUS_READY) */
			break;
		}
		cond_resched();
	}

	/* status = (int)chip->legacy.read_byte(mtd); */
	/* return status; */
	status = read_flash_ctrl_reg(FLASH_NF_DATA) & 0xff;
	return status;
}

#define NOTALIGNED(x)	(((x) & (chip->subpagesize - 1)) != 0)

/*
 * is_module_text_address() isn't exported, and it's mostly a pointless
 * test if chip is a module _anyway_ -- they'd have to try _really_ hard
 * to call us from in-kernel code if the core NAND support is modular.
 */
#ifdef MODULE
#define caller_is_module() (1)
#else
#define caller_is_module() \
	is_module_text_address((unsigned long)__builtin_return_address(0))
#endif

static void reconfigure_flash_strap(struct mtd_info *mtd)
{
	u32 flash_type = 0;

	/* Reconfigure flash ype */
	switch (mtd->writesize) {
	case SZ_512:
		flash_type |= PAGE_512;
		break;
	case SZ_2K:
		flash_type |= PAGE_2K;
		break;
	case SZ_4K:
		flash_type |= PAGE_4K;
		break;
	case SZ_8K:
		flash_type |= PAGE_8K;
		break;
	default:
		pr_err("Error: page size not supported! %d\n", mtd->writesize);
		break;
	};
	flash_type |= CONFIGURABLE_OOB_SIZE;	/* configurable oob size */
	if (mtd->size > SZ_128M)
		flash_type |= SET_FLASH_PIN;	/* Flsah size > 128MB */

	write_flash_ctrl_reg(FLASH_TYPE, flash_type);
}

static void ca_nand_write_byte(struct nand_chip *chip, uint8_t byte)
{
}

static void ca_nand_set_defaults(struct nand_chip *chip)
{
	struct nand_ecc_ctrl *ecc = &chip->ecc;
	struct mtd_info *mtd = nand_to_mtd(chip);

	chip->legacy.cmdfunc = ca_nand_command;
	chip->legacy.waitfunc = ca_nand_wait;
	chip->legacy.select_chip = ca_nand_select_chip;
	chip->legacy.read_byte = ca_nand_read_byte;
	chip->legacy.write_buf = ca_nand_write_buf;
	chip->legacy.read_buf = ca_nand_read_buf;

	ecc->read_page = ca_nand_read_page_hwecc;
	ecc->write_page = ca_nand_write_page_hwecc;
	ecc->read_page_raw = ca_nand_read_page_raw;
	ecc->write_page_raw = ca_nand_write_page_raw;
	ecc->read_oob = ca_nand_read_oob_std;
	ecc->write_oob = ca_nand_write_oob_std;
	ecc->read_subpage = ca_nand_read_subpage;

	chip->legacy.write_byte = ca_nand_write_byte;
	mtd->ooblayout = nand_get_large_page_ooblayout();
	chip->legacy.chip_delay = 10;
}

static int ca_calc_ecc_bytes(int step_size, int strength)
{
	return DIV_ROUND_UP(strength * fls(step_size * 8), 16) * 2;
}

NAND_ECC_CAPS_SINGLE(ca_ecc_caps, ca_calc_ecc_bytes,
		     1024, 8, 16, 24, 40);

static int ca_nand_attach_chip(struct nand_chip *chip)
{
	int ret;
	struct mtd_info *mtd = nand_to_mtd(chip);

	/* Two extra bytes are reserved as an erase marker,
	 * located right before the ECC code
	 */
	ret = nand_ecc_choose_conf(chip, &ca_ecc_caps, mtd->oobsize - 2);
	if (ret) {
		pr_err("Failed to configure ECC settings\n");
		return ret;
	}

	pr_info("Chosen ECC settings step = %d, strength = %d, bytes = %d\n",
		chip->ecc.size, chip->ecc.strength, chip->ecc.bytes);
	return 0;
}

static const struct nand_controller_ops ca_nandc_ops = {
	.attach_chip = ca_nand_attach_chip,
};

/**
 * ca_nand_scan - [NAND Interface] Scan for the NAND device
 * @mtd: MTD device structure
 * @maxchips: number of chips to scan for
 *
 * This fills out all the uninitialized function pointers with the defaults.
 * The flash ID is read and the mtd/chip structures are filled with the
 * appropriate values. The mtd->owner field must be set to the module of the
 * caller.
 */
static int ca_nand_scan(struct mtd_info *mtd, int maxchips)
{
	int ret;
	struct nand_chip *chip = mtd_to_nand(mtd);

	/* Many callers got chip wrong, so check for it for a while... */
	if (!mtd->owner && caller_is_module()) {
		pr_crit("%s called with NULL mtd->owner!\n", __func__);
		WARN_ON((!mtd->owner && caller_is_module()));
	}

	ca_nand_set_defaults(chip);

	ret = nand_scan_with_ids(chip, maxchips, NULL);

	reconfigure_flash_strap(mtd);

	tmp_buf = kzalloc(mtd->writesize, GFP_KERNEL);
	if (IS_ERR(tmp_buf)) {
		pr_err("Error: no memory allocated\n");
		return -ENOMEM;
	}
	return ret;
}

/**
 * nand_release - [NAND Interface] Free resources held by the NAND device
 * @mtd: MTD device structure
 */
void ca_nand_release(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	mtd_device_unregister(mtd);

	/* Free bad block table memory */
	kfree(chip->bbt);

	/* Free bad block descriptor memory */
	if (chip->badblock_pattern && chip->badblock_pattern->options
	    & NAND_BBT_DYNAMICSTRUCT)
		kfree(chip->badblock_pattern);
}
EXPORT_SYMBOL_GPL(ca_nand_release);

#ifdef CONFIG_OF
static const struct of_device_id ca_nflash_dt_ids[] = {
	{.compatible = "cortina-access,nflash",},
	{},
};

MODULE_DEVICE_TABLE(of, ca_nflash_dt_ids);
#endif

int init_DMA_SSP(struct platform_device *pdev)
{
	int i;
	u32 rx_glb_dma_ctrl = 0, tx_glb_dma_ctrl = 0;
	u32 rx_base_depth = 0, tx_base_depth = 0;

	dma_addr_t dma_tx_handle;
	dma_addr_t dma_rx_handle;

	rx_glb_dma_ctrl =
	    read_dma_glb_ctrl_reg(DMA_SEC_DMA_GLB_DMA_SSP_RX_CTRL);
	tx_glb_dma_ctrl =
	    read_dma_glb_ctrl_reg(DMA_SEC_DMA_GLB_DMA_SSP_TX_CTRL);

	if (!(rx_glb_dma_ctrl & 0x3)) {
		rx_glb_dma_ctrl |= CHECK_OWN | DMA_EN;
		write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_CONTROL,
				   rx_glb_dma_ctrl);
	}

	if (!(tx_glb_dma_ctrl & 0x3)) {
		tx_glb_dma_ctrl |= CHECK_OWN | DMA_EN;
		write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_CONTROL,
				   tx_glb_dma_ctrl);
	}

	tx_desc = dma_alloc_coherent(&pdev->dev,
				     (sizeof(struct tx_descriptor_t) *
				      FDMA_DESC_NUM), &dma_tx_handle,
				     GFP_KERNEL | GFP_DMA);
	rx_desc =
	    dma_alloc_coherent(&pdev->dev,
			       (sizeof(struct rx_descriptor_t) * FDMA_DESC_NUM),
			       &dma_rx_handle, GFP_KERNEL | GFP_DMA);

	if (!rx_desc || !tx_desc) {
		pr_err("Buffer allocation for failed!\n");
		kfree(rx_desc);
		kfree(tx_desc);

		return 0;
	}
	//printk("tx_desc_v: %p (p: %x), rx_desc_v: %p (p: %x)\n", tx_desc,
	//       dma_tx_handle, rx_desc, dma_rx_handle);

	/* set base address and depth */
	rx_base_depth = dma_rx_handle | FDMA_DEPTH;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_BASE_DEPTH,
			   rx_base_depth);
	if (force_ace)
		write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_RXQ_BASE, 4);

	tx_base_depth = dma_tx_handle | FDMA_DEPTH;
	write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_BASE_DEPTH,
			   tx_base_depth);
	if (force_ace)
		write_dma_ctrl_reg(DMA_SEC_DMA_SSP_Q_TXQ_BASE, 4);

	memset((unsigned char *)tx_desc, 0,
	       (sizeof(struct tx_descriptor_t) * FDMA_DESC_NUM));
	memset((unsigned char *)rx_desc, 0,
	       (sizeof(struct rx_descriptor_t) * FDMA_DESC_NUM));

	for (i = 0; i < FDMA_DESC_NUM; i++) {
		/* set own by sw */
		tx_desc[i].own = OWN_SW;
		/* enable q5 Scatter-Gather memory copy */
		tx_desc[i].sgm = 0x1;
		if (force_ace) {
			/* ACE */
			tx_desc[i].buf_adr_hi = 4;
			rx_desc[i].buf_adr_hi = 4;
		}
	}

	return 1;
}

/*
 *	read device ready pin
 */
int ca_nand_dev_ready(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	u8 status, old_sts;

	check_flash_ctrl_status();

	write_flash_ctrl_reg(FLASH_NF_DATA, 0xffffffff);
	old_sts = read_flash_ctrl_reg(FLASH_STATUS);
	if ((old_sts & 0xffff) != 0)
		pr_info("old_sts : %x      ", old_sts);

	do {
		chip->legacy.cmdfunc(chip, NAND_CMD_STATUS, -1, -1);
		status = ca_nand_read_byte_wait(mtd);
	} while (status == 0xff);

	return (status & NAND_STATUS_READY);
}

static int get_bch_cap(const int strength)
{
	int i;
	const int bch_supports[] = { 8, 16, 24, 40 };

	for (i = 0; i < ARRAY_SIZE(bch_supports); ++i) {
		if (bch_supports[i] == strength)
			return i;
	}

	return -1;
}

int ca_nflash_probe(struct platform_device *pdev)
{
	struct nand_chip *chip;
	struct mtd_info *mtd;
	struct resource mem_resource;
	struct reset_control *flash_rst;
	const struct of_device_id *match;
	struct device_node *np = pdev->dev.of_node;
	int ret, err = 0;
	unsigned int flash_base;
	u32 reg_v, flash_type = 0;

	pr_info("Cortin P-NAND init ...\n");

	match = of_match_device(ca_nflash_dt_ids, &pdev->dev);
	if (!match)
		return -EINVAL;

	force_ace = of_property_read_bool(np, "force-ace-on");
	pr_info("force_ace: %d\n", force_ace);

	flash_rst = of_reset_control_get(np, "flash_reset");
	if (IS_ERR(flash_rst)) {
		dev_err(&pdev->dev, "failed to get flash_reset, NODE(%s)\n", np->full_name);
		return PTR_ERR(flash_rst);
	}

	/* Flash controller base */
	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret) {
		dev_warn(&pdev->dev, "invalid address %d\n", ret);
		return ret;
	}

	flash_ctl_base = devm_ioremap(&pdev->dev, mem_resource.start,
				      resource_size(&mem_resource));

	if (flash_rst) {
		reset_control_assert(flash_rst);
		usleep_range(10, 50);
		reset_control_deassert(flash_rst);
	}

	/* Speed up Flash clock */
	reg_v = read_flash_ctrl_reg(FLASH_NF_TIMING);
	reg_v &= ~NF_CLKWIDTH_MSK;
	reg_v |= NF_CLK_09_AXI;
	write_flash_ctrl_reg(FLASH_NF_TIMING, reg_v);

	/* Flash memory base */
	ret = of_address_to_resource(np, 1, &mem_resource);
	if (ret) {
		dev_warn(&pdev->dev, "invalid address %d\n", ret);
		return ret;
	}

	flash_base = (unsigned int)mem_resource.start;
	flash_mem_base = devm_ioremap(&pdev->dev, mem_resource.start,
				      resource_size(&mem_resource));

	/* DMA global controller */
	ret = of_address_to_resource(np, 2, &mem_resource);
	if (ret) {
		dev_warn(&pdev->dev, "invalid address %d\n", ret);
		return ret;
	}

	dma_ctl_glb_base = devm_ioremap(&pdev->dev, mem_resource.start,
					resource_size(&mem_resource));

	/* DMA controller */
	ret = of_address_to_resource(np, 3, &mem_resource);
	if (ret) {
		dev_warn(&pdev->dev, "invalid address %d\n", ret);
		return ret;
	}

	dma_ctl_base = devm_ioremap(&pdev->dev, mem_resource.start,
				    resource_size(&mem_resource));

	/* Allocate memory for MTD device structure and private data */
	ca_host = kzalloc(sizeof(*ca_host), GFP_KERNEL);
	if (!ca_host) {
		pr_err
		("Unable to allocate ca_host NAND MTD device structure.\n");
		return -ENOMEM;
	}

	ca_host->flash_mem_base = flash_base;

	/* Get pointer to private data */
	/* Allocate memory for MTD device structure and private data */
	ca_host->nand_chip =
	    kzalloc(sizeof(struct nand_chip), GFP_KERNEL);
	if (!ca_host->nand_chip) {
		pr_err
		    ("Unable to allocate Cortina-Access NAND MTD device structure.\n");
		err = -ENOMEM;
		goto err_mtd;
	}

#ifdef CONFIG_ZONE_DMA32
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
#else
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(40));
#endif
	if (ret) {
		pr_err("failed to set DMA mask\n");
		goto err_add;
	}

	/* 32bits DMA descriptor base address*/
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_err("failed to set DMA coherent mask\n");
		goto err_add;
	}

	ca_host->dev = &pdev->dev;
	if (force_ace) {
		/* domain = 2, cache = 2, ace_cmd = 1 */
		writel(0x02008080, dma_ctl_glb_base + 0x18);
		pr_info("Force ACE for NAND DMA\n");
	}

	if (init_DMA_SSP(pdev) == 0)
		goto err_add;

	chip = (struct nand_chip *)(ca_host->nand_chip);
	mtd = nand_to_mtd(chip);

	/* Link the private data with the MTD structure */
	mtd->owner = THIS_MODULE;

	platform_set_drvdata(pdev, ca_host);

	/* Set address of NAND IO lines */
	chip->legacy.IO_ADDR_R = flash_ctl_base;
	chip->legacy.IO_ADDR_W = flash_ctl_base;
	/* Set address of hardware control function */

	chip->legacy.dev_ready = ca_nand_dev_ready;
	/* set eccmode using hardware ECC */
	chip->ecc.options |= NAND_ECC_GENERIC_ERASED_CHECK;

	/* vmalloc buffer cannot used on DMA.
	 * Define NAND_USE_BOUNCE_BUFFER will use temporary buffer for DMA.
	 * And then use memcpy to move to dest buffer.
	 */
	chip->options |= NAND_NO_SUBPAGE_WRITE | NAND_USES_DMA;

	/* Set Nand type temporarily, Nand type will be overwritten later */
	flash_type |= PAGE_2K;	/* Init at 2K page */
	flash_type |= CONFIGURABLE_OOB_SIZE;	/* Give a arbitrary oob size */
	flash_type |= SET_FLASH_PIN;	/* Flsah size > 256MB? */
	write_flash_ctrl_reg(FLASH_TYPE, flash_type);

	mtd_set_of_node(mtd, np);
	mtd->priv = chip;

	chip->controller = &ca_host->controller;
	nand_controller_init(&ca_host->controller);
	ca_host->controller.ops = &ca_nandc_ops;

	/* Scan to find existence of the device */
	err = ca_nand_scan(mtd, 1);
	if (err)
		goto err_scan;

	/* allocate oob buffers */
	ca_host->calc_buf = kmalloc(mtd->oobsize, GFP_KERNEL);
	if (!ca_host->calc_buf) {
		pr_err
		    ("Unable to allocate ecc calcuate buffer.\n");
		err = -ENOMEM;
		goto err_mtd;
	}
	ca_host->code_buf = kzalloc(mtd->oobsize, GFP_KERNEL);
	if (!ca_host->code_buf) {
		pr_err
		    ("Unable to allocate ecc code buffer.\n");
		err = -ENOMEM;
		goto err_mtd;
	}

	ca_host->bch_cap = get_bch_cap(chip->ecc.strength);
	if (ca_host->bch_cap < 0)
		goto err_scan;

	/* Register the partitions */
	mtd->name = "ca_nand_flash";

	/* Parse mtd partition and register to mtd framework */
	err = mtd_device_parse_register(mtd, NULL, NULL, NULL, 0);

	/* Return happy */
	pr_info("Cortina-Access P-NAND init OK.\n");

	return 0;

 err_add:
 err_scan:
	pr_err("Nand driver fail to scan!\n");
	platform_set_drvdata(pdev, NULL);
	iounmap(ca_host->io_base);
 err_mtd:
	pr_err("Nand driver fail to malloc!\n");
	kfree(ca_host->nand_chip);

	pr_err("Nand driver fail to ioremap!\n");
	kfree(ca_host->calc_buf);
	kfree(ca_host->code_buf);

	pr_err("Nand driver fail to ioremap!\n");
	kfree(ca_host);

	return err;
}

int ca_nflash_remove(struct platform_device *pdev)
{
	kfree(tmp_buf);
	return 0;
}

static struct platform_driver ca_nflash_driver = {
	.probe = ca_nflash_probe,
	.remove = ca_nflash_remove,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "ca_nflash",
		   .of_match_table = of_match_ptr(ca_nflash_dt_ids),
		   },
};

static int __init ca_nlfash_init(void)
{
	return platform_driver_register(&ca_nflash_driver);
}

static void __exit ca_nflash_exit(void)
{
	platform_driver_unregister(&ca_nflash_driver);
}

module_init(ca_nlfash_init);
module_exit(ca_nflash_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason Li, Jengfeng Lee, Kate Liu et al.");
MODULE_DESCRIPTION("NAND flash driver for Cortina-Access parallel NAND flash controller");
MODULE_ALIAS("platform:ca_nand");
