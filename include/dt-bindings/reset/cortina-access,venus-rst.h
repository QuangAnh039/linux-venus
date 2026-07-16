/*
 * This header provides block reset mapping.
 *
 */

#ifndef _DT_BINDINGS_RESET_CORTINA_ACCESS_VENUS_RST_MGR_H
#define _DT_BINDINGS_RESET_CORTINA_ACCESS_VENUS_RST_MGR_H

/* BLOCK RESET */
#define NI_RESET		0
#define L2FE_RESET		1
#define L2TM_RESET		2
#define L3FE_RESET		3
#define SDRAM_RESET		4
#define TQM_RESET		5
#define PCI0_RESET		6
#define PCI1_RESET		7
#define PCI2_RESET		8
#define SATA_RESET		9
#define GIC400_RESET		10
#define USB_RESET		13
#define FLASH_RESET		14
#define PER_RESET		15
#define DMA_RESET		16
#define RTC_RESET		17
#define PE0_RESET               18
#define PE1_RESET               19
#define RCPU0_RESET		20
#define RCPU1_RESET		21
#define SADB_RESET		22
#define RCRYPTO_RESET		24
#define LDMA_RESET		25
#define FBM_RESET		26
#define SD_RESET		28

/*** SATURN Specific ***/
#define CPU_AP_RESET            18
#define CPU_DSP_RESET           19
#define TAROKO0_ATU_RESET       23
#define TAROKO1_ATU_RESET       24
#define IIC_SLV_RESET           30

/*** VENUS Specific ***/
#define L3FE_REO_RESET          11
#define QM_REO_RESET            12
#define RESET_UVLO_RESET        23
#define DMA_REO_RESET           27
#define PTP_TIMER_RESET         30
#define QXGMII_RESET            31

/* GLOBAL_PON_CNTL */
#define PON_SERDES_RESET        1
#define PSDS_REG_RESET          2
#define PTP_RESET               3
#define PUC_RESET               8
#define PDC_RESET               9

/* GLOBAL_CONFIG */
#define EXT_RESET               9

/* DPHY RESET */
#define PHY_S0_PCIE_RESET	0
#define PHY_S1_PCIE_RESET	1
#define PHY_S2_PCIE_RESET	2
#define PHY_S2_USB_RESET	3
#define PHY_S3_USB_RESET	4
#define PHY_SATA_FORCE_RESET	5
#define PHY_SATA_RESET		6
#define PHY_SDSIF_RESET		7
#define PHY_SGMII_RESET		8

/*** VENUS Specific ***/
#define PHY_S3_SGMII_RESET	5

/* SD_DLL_CTRL */
#define SD_CLK_SEL              0
#define SD_PHASE_RESET_OVERRIDE 14

#endif
