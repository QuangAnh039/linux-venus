/*
 * This header provides block reset mapping.
 *
 */

#ifndef _DT_BINDINGS_RESET_CORTINA_ACCESS_MERCUR_REST_H
#define _DT_BINDINGS_RESET_CORTINA_ACCESS_MERCUR_REST_H

/* BLOCK RESET */
#define NI_RESET         0
#define L2FE_RESET       1
#define L2TM_RESET       2
#define L3TM_RESET       3
#define L3FE_RESET       4
#define SDRAM_RESET      5
#define TQM_RESET        6
#define GIC400_RESET     7
#define L3FE_REO_RESET   8
#define QM_REO_RESET     9
#define FLASH_RESET      10
#define PER_RESET        11
#define DMA_RESET        12
#define PE0_RESET        14
#define PE1_RESET        15
#define PE2_RESET        16
#define PE3_RESET        17
#define DOE_RESET        18
#define UVLO_RESET       19
#define RCRYPTO0_RESET   20
#define RCRYPTO1_RESET   21
#define LDMA_RESET       22
#define DMA_REO_RESET    23
#define SD_RESET         24
#define OTP_RESET        25
#define PTP_TMR_RESET    26
#define QXGMII_RESET     27
#define QXGMII_SDS_RESET 28
#define DXGMII_RESET     29
#define DXGMII_SDS_RESET  30

/* Block RESET1 */
#define PCI0_RESET        0
#define PCI1_RESET        1
#define PCI2_RESET        2
#define PCI7_RESET        3
#define PCI8_RESET        4
#define USB_S2_RESET      5
#define USB_S6_RESET      6
#define USB_S8_RESET      7
#define USB_S9_RESET      8
#define RCPU0_RESET       9
#define RCPU1_RESET       10
#define RCPU2_RESET       11
#define RCPU3_RESET       12
#define RCPU4_RESET       13
#define FBM_RESET         14

/* Block RESET EXT */
#define I2S_RESET          0
#define GDMA_RESET         1
#define SSI0_RESET         2
#define UART0_RESET        3
#define UART1_RESET        4
#define USB_OTG_RESET      5
/* Why is enable function here?? */
#define GDMA_EN_RESET      8
#define SSI0_EN_RESET      9
#define UART0_EN_RESET     10
#define UART1_EN_RESET     11
#define I2S_EN_RESET       12
#define S0_EN_RESET        13
#define S1_EN_RESET        14
#define S2_MA_EN_RESET     15
#define S7_EN_RESET        16
#define S8_EN_RESET        17
#define SD_EN_RESET        18

/* GLOBAL_PON_CNTL */
#define PON_SERDES_RESET        1
#define PSDS_REG_RESET          2
#define PTP_RESET               3
#define PUC_RESET               8
#define PDC_RESET               9
#define PSDS_WTG_RESET         10

/* GLOBAL_CONFIG */
#define EXT_RESET               9

/* DPHY RESET */
#define PHY_S0_PCIE_RESET    0
#define PHY_S1_PCIE_RESET    1
#define PHY_S2_PCIE_RESET    2
#define PHY_S2_SGMII_RESET   3
#define PHY_S7_PCIE_RESET    4
#define PHY_S7_SGMII_RESET   5
#define PHY_S8_PCIE_RESET    6
#define PHY_S8_SGMII_RESET   7
#define PHY_S9_PCIE_RESET    8
#define PHY_S9_SGMII_RESET   9

/*** FABIRC_RESET ***/
#define AXI_RESET             0
#define CAPSRAM128_RESET      1
#define CAPSRAM128_REG_RESET  2
#define CAPSRAM640_RESET      3
#define CAPSRAM640_REG_RESET  4
#define CCIREG_RESET          5
#define HSIO_RESET            6
#define LSIO_RESET            7
#define PE_RESET              8
#define LSAXI_RESET           9

#endif
