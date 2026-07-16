/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CA_EFUSE_CFG_H_
#define _CA_EFUSE_CFG_H_

#if defined(CONFIG_ARCH_CORTINA_SATURN2) || defined(CONFIG_ARCH_CA_VENUS)
struct ca_phy_param_s {
	uint32_t reserved3_0	: 4;
	uint32_t gphy_cal_cmplt : 1;
	uint32_t reserved3_1	: 3;

	uint32_t sw_patch	: 1;
	uint32_t phy_cmd	: 2;
	uint32_t reserved3_2	: 2;

	uint32_t dgac_lb_dn_up	: 1;
	uint32_t spd_chg	: 1;
	uint32_t reserved3_3	: 1;

	uint32_t en_ado_cal	: 1;
	uint32_t en_rc_cal	: 1;
	uint32_t en_r_cal	: 1;

	uint32_t en_amp_cal	: 1;
	uint32_t disable_500m	: 1;
	uint32_t reserved3_4	: 11;
};
#else
struct ca_phy_param_s {
	uint32_t sw_patch       : 1;
	uint32_t phy_cmd        : 2;
	uint32_t reserved3_0    : 1;

	uint32_t gphy_cal_cmplt : 1;
	uint32_t dgac_lb_dn_up  : 1;
	uint32_t spd_chg        : 1;
	uint32_t reserved3_1    : 9;

	uint32_t en_ado_cal     : 1;
	uint32_t en_rc_cal      : 1;
	uint32_t en_r_cal       : 1;

	uint32_t en_amp_cal     : 1;
	uint32_t disable_500m   : 1;
	uint32_t reserved3_2    : 11;
};
#endif

struct ca_phy_k_s {
	uint32_t rc_cal_len	: 16;
	uint32_t amp_cal	: 16;
	uint32_t adc0_cal	: 16;
	uint32_t r_cal		: 4;
	uint32_t reserved	: 12;
};

struct ca_efuse_cfg_s {
	u32 uuid;
	uint32_t hvsid		: 8;
	uint32_t reserved1_0	: 24;
	u32 reserved2;

	struct ca_phy_param_s param;

	u32 phy_patch[4];

	struct ca_phy_k_s phy_k_data[4];
};

#define EFUSE_PHY_PARAM_OFFSET	0x0C
#define EFUSE_PHY_PATH_OFFSET	0x10
#define EFUSE_PHY_CALIB_OFFSET	0x20

#endif /* _CA_EFUSE_CFG_H_ */
