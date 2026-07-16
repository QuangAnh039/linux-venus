/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CORTINA_ATU_H_
#define _CORTINA_ATU_H_

union atu_ctrl {
	struct {
		u32 enable   :  8; /* bits 7:0 */
		u32 rsrvd1   : 16;
		u32 idx      :  6; /* bits 29:24 */
		u32 r        :  1; /* bits 30:30 */
		u32 w        :  1; /* bits 31:31 */
	} bf;
	u32 wrd;
};

union axi_cfg {
	struct {
		u32 arprot     :  3; /* bits 2:0 */
		u32 rsrvd1     :  1;
		u32 awprot     :  3; /* bits 6:4 */
		u32 rsrvd2     :  1;
		u32 arqos      :  4; /* bits 11:8 */
		u32 awqos      :  4; /* bits 15:12 */
		u32 arcache    :  4; /* bits 19:16 */
		u32 awcache    :  4; /* bits 23:20 */
		u32 user_en    :  1; /* bits 24:24 */
		u32 prot_en    :  1; /* bits 25:25 */
		u32 cache_en   :  1; /* bits 26:26 */
		u32 qos_en     :  1; /* bits 27:27 */
		u32 rsrvd3     :  4;
	} bf;
	u32 wrd;
};

union ahb_cfg {
	struct {
		u32 hprot      :  4; /* bits 3:0 */
		u32 rsrvd1     : 20;
		u32 user_en    :  1; /* bits 24:24 */
		u32 prot_en    :  1; /* bits 25:25 */
		u32 rsrvd2     :  6;
	} bf;
	u32 wrd;
};

union user_attr {
	struct {
		u32	axuser	  :  6; /* bits 5:0 */
		u32	axdomain  :  2; /* bits 7:6 */
		u32	axbar	  :  2; /* bits 9:8 */
		u32	axsnoop   :  4; /* bits 13:10 */
		u32	rsrvd	  : 18;
	} bf;
	u32 wrd;
};

#define ATU_CTRL	0x00
#define ATU_INPUT	0x04
#define ATU_MASK	0x08
#define ATU_OUTPUT	0x0C
#define ACE_CFG		0x10
#define AXI_ARUSER	0x14
#define	AXI_AWUSER	0x18
#define	AXI_UPPER	0x1C

#define AHB_HUSER	0x14
#define	AHB_UPPER	0x18

#define ATU_CTRL_W	BIT(31)
#define ATU_CTRL_R	BIT(30)

#define T_AXI_STD	0
#define T_AXI_PLUS	1
#define T_AHB_PLUS	2

#define ACE_S0_ADDR	0x020000	/* unit of 0x10000 */
#define ACE_S1_ADDR	0x040000	/* unit of 0x10000 */
#define ACE_S2_ADDR	0x100000	/* unit of 0x10000 */

#define AXDOMAIN_NON_SHAREABLE   0
#define AXDOMAIN_INNER_SHAREABLE 1
#define AXDOMAIN_OUTER_SHAREABLE 2
#define AXDOMAIN_SYSTEM          3

#define AXBAR_NORMAL_ACCESS_S    0
#define AXBAR_MEMORY             1
#define AXBAR_NORMAL_ACCESS_I    2
#define AXBAR_SYNCHRONIZATION    3

#define ARSNOOP_READNOSNOOP      0x0
#define ARSNOOP_READONCE         0x0
#define ARSNOOP_READSHARED       0x1
#define ARSNOOP_READCLEAN        0x2
#define ARSNOOP_READNOTSHARED    0x3
#define ARSNOOP_READUNIQUE       0x7
#define ARSNOOP_CLEANUNIQUE      0xB
#define ARSNOOP_MAKEUNIQUE       0xC
#define ARSNOOP_CLEANSHARED      0x8
#define ARSNOOP_CLEANINVALID     0x9
#define ARSNOOP_MAKEINVALID      0xD
#define ARSNOOP_BARRIER          0x0
#define ARSNOOP_DVM_COMPLETE     0xE
#define ARSNOOP_DVM_MESSAGE      0xF

#define AWSNOOP_WRITENOSNOOP     0x0
#define AWSNOOP_WRITEUNIQUE      0x0
#define AWSNOOP_WRITELINEUNIQUE  0x1
#define AWSNOOP_WRITECLEAN       0x2
#define AWSNOOP_WRITEBACK        0x3
#define AWSNOOP_EVICT            0x4
#define AWSNOOP_WRITEEVICTA      0x5
#define AWSNOOP_BARRIER          0x0

#define AXCACHE_BUFFERABLE		0x1
#define AXCACHE_CACHEABLE		0x2
#define AXCACHE_READ_ALLOCATE		0x4
#define AXCACHE_WRITE_ALLOCATE		0x8

#define AXPROT_PRIVILEGED_ACCESS	0x1 /* not set - unprivileged access */
#define AXPROT_NONSECURE_ACCESS		0x2 /* not set - secure access */
#define AXPROT_INSTRUCTION_ACCESS	0x4 /* not set - data access */

#define HPROT_DATA_ACCESS		0x1 /* not set - opcode fetch */
#define HPROT_PRIVILEGED_ACCESS		0x2 /* not set - user access */
#define HPROT_BUFFERABLE		0x4 /* not set - not bufferable */
#define HPROT_CACHEABLE			0x8 /* not set - not cacheable */

struct ace_attr {
	u8	arcache;
	u8	awcache;
	u8	arqos;
	u8	awqos;
	u8	arprot;
	u8	awprot;
	u8	hprot;
	u8	cache_en;
	u8	qos_en;
	u8	prot_en;
	u8	user_en;
	union user_attr	aruser;
	union user_attr	awuser;
	union user_attr	huser;
};

#define TLB_COUNT	8

#endif /* _CORTINA_ATU_H_ */
