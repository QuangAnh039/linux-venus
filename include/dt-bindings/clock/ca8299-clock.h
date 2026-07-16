/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2017-2018 Cortina Access Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __DT_BINDINGS_CLOCK_CA8299_H
#define __DT_BINDINGS_CLOCK_CA8299_H

#define CA8299_CLK_DUMMY				0
#define CA8299_CLK_OSC					1

/* Cortex CPU Virtual Fixed Freq inputs, READ-ONLY Global Strap Speed */
#define CA8299_CLK_CORTEX_FIXED_400MHZ_RATE		2
#define CA8299_CLK_CORTEX_FIXED_625MHZ_RATE		3
#define CA8299_CLK_CORTEX_FIXED_700MHZ_RATE		4
#define CA8299_CLK_CORTEX_FIXED_800MHZ_RATE		5
#define CA8299_CLK_CORTEX_FIXED_900MHZ_RATE		6
#define CA8299_CLK_CORTEX_FIXED_1000MHZ_RATE		7
#define CA8299_CLK_CORTEX_FIXED_1100MHZ_RATE		8
#define CA8299_CLK_CORTEX_FIXED_1200MHZ_RATE		9

/* Cortex CPU Virtual Input Mux for Fixed Freq inputs, */
/* READ-ONLY from Global Strap Reg. */
#define CA8299_CLK_CORTEX_FIXED_MODE			10

/* PLL PRE Divider */
#define CA8299_CLK_CPLL_PRE_DIV				11
#define CA8299_CLK_EPLL_PRE_DIV				12
#define CA8299_CLK_FPLL_PRE_DIV				13

/* PLL Prediv Bypass or Prediv Output Mux */
#define CA8299_CLK_CPLL_DIVN_SRC			14
#define CA8299_CLK_EPLL_DIVN_SRC			15
#define CA8299_CLK_FPLL_DIVN_SRC			16

/* PLL Multiplier */
#define CA8299_CLK_CPLL_DIVN				17
#define CA8299_CLK_EPLL_DIVN				18
#define CA8299_CLK_FPLL_DIVN				19

/* used fixed CPLL PREDIV and DIVN or dynamic values */
#define CA8299_CLK_CPLL_BUS_SRC				20

/* PLL Output */
#define CA8299_CLK_CPLL_BUS				21
#define CA8299_CLK_EPLL_BUS				22
#define CA8299_CLK_FPLL_BUS				23

/* Cortex CPU Post Dividers for CPLL or FPLL */
#define CA8299_CLK_CORTEX_DYNAMIC_DIVSEL_CPLL		24
#define CA8299_CLK_CORTEX_DIVSEL_CPLL			25
#define CA8299_CLK_CORTEX_DIVSEL_FPLL			26

/* Cortex CPU Input Mux, CPLL or FPLL  */
#define CA8299_CLK_CORTEX_SRC				27

/* Cortex CCI cache speed of  1/2 of Cortex SRC */
#define CA8299_CLK_CORTEX_CCI_DIV2			28

/* Cortex CCI cache speed Mux,  1:1 or 1/2 */
#define CA8299_CLK_CORTEX_CCI_SRC			29

/* Final Cortex and CCI Speed */
#define CA8299_CLK_CPU					30

#define CA8299_CLK_END					31

#define CA8299_CFG_CLKEN_PCIE0				10
#define CA8299_CFG_CLKEN_SATA				11
#define CA8299_CFG_CLKEN_PCIE1				12
#define CA8299_CFG_CLKEN_PCIE2				14
#define CA8299_CFG_CLKEN_PCIE7                          15
#define CA8299_CFG_CLKEN_PCIE8                          16
#define CA8299_CFG_PD_L3FE				27
#define CA8299_CFG_PD_OFFLOAD0				28
#define CA8299_CFG_PD_OFFLOAD1				29
#define CA8299_CFG_PD_CRYPTO				30
#define CA8299_CFG_PD_CORE				31

#define CA8299_CPLLDIV_CF_SEL				15
#define CA8299_PEDIV_OFFLOAD_SEL			6
#define CA8299_PEDIV_FULLSPEED_SEL			22

#define CA8299_CPLLDIV_CORTEX_OFFSET			0
#define CA8299_CPLLDIV_CORTEX_SIZE			6
#define CA8299_CPLLDIV_F2C_OFFSET			7
#define CA8299_CPLLDIV_F2C_SIZE				4
#define CA8299_CPLLDIV_TRC_OFFSET			12
#define CA8299_CPLLDIV_TRC_SIZE				7
#define CA8299_CPLLDIV_CFG_PILCS_PER_OFFSET		19
#define CA8299_CPLLDIV_CFG_PILCS_PER_SIZE		6
#define CA8299_CPLLDIV_CFG_EXT_VSFC_OFFSET		25
#define CA8299_CPLLDIV_CFG_EXT_VSFC_SIZE		6

#define CA8299_EPLLDIV_DW_PER_OFFSET			0
#define CA8299_EPLLDIV_DW_PER_SIZE			7
#define CA8299_EPLLDIV_CORE_OFFSET			8
#define CA8299_EPLLDIV_CORE_SIZE			7
#define CA8299_EPLLDIV_LSAXI_OFFSET			24
#define CA8299_EPLLDIV_LSAXI_SIZE			7
#define CA8299_EPLLDIV2_PON_CORE_OFFSET			0
#define CA8299_EPLLDIV2_PON_CORE_SIZE			7
#define CA8299_EPLLDIV2_CCI_OFFSET			8
#define CA8299_EPLLDIV2_CCI_SIZE			7

#define CA8299_FPLLDIV_FPLL_OFFSET			0
#define CA8299_FPLLDIV_FPLL_SIZE			6
#define CA8299_FPLLDIV_C2F_OFFSET			8
#define CA8299_FPLLDIV_C2F_SIZE				6
#define CA8299_FPLLDIV_PEAXI_OFFSET			16
#define CA8299_FPLLDIV_PEAXI_SIZE			6

#endif /* __DT_BINDINGS_CLOCK_CA8299_H */
