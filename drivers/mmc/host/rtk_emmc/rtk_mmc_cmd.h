#ifndef __RTK_MMC_CMD_H
#define __RTK_MMC_CMD_H

/* The command code should not have any overlap with linux/mmc/mmc.h */
/* Standard MMC commands (4.1)		type  argument     response */
/* class 1 */
#define MMC_SET_BLOCKLEN	 16   /* ac   [31:16] RCA            */
#define MMC_TUNING               21   /* adtc [31:0] data addr   R1  */
#define MMC_MICRON_60        	 60
#define MMC_MICRON_61            61
#define MMC_MICRON_62            62
#define MMC_MICRON_63            63
#define MANU_ID_MICRON1          0xfe
#define MANU_ID_MICRON2          0x13

/*
 * EXT_CSD fields
 */
#define EXT_CSD_ENH_START_ADDR          136     /* R/W, 4 bytes */
#define EXT_CSD_ENH_SIZE_MULT           140     /* R/W, 3 bytes */
#define EXT_CSD_PARTITION_SETTING_COMP  155     /* R/W */
#define EXT_CSD_MAX_ENH_SIZE_MULT       157     /* R/W, 3 bytes */
#define EXT_CSD_WR_REL_SET              167     /* R/W ifHS_CTRL_REL=1 */
#define EXT_CSD_CMD_SET			191	/* RO */
#define EXT_CSD_ACC_SIZE		225	/* RO */
#define EXT_CSD_BOOT_INFO		228	/* RO */
#define EXT_CSD_MIN_PERF_DDR_R_8_52	234	/* RO */
#define EXT_CSD_MIN_PERF_DDR_W_8_52	235	/* RO */
#define EXT_CSD_INI_TIMEOUT_AP		241	/* RO */
#define EXT_CSD_CORRECTLY_PRG_SECTORS_NUM	242	/* RO */

#endif /* __RTK_MMC_CMD_H */
