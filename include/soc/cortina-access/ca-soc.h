#ifndef __CORTINA_ACCESS_SOC_H__
#define __CORTINA_ACCESS_SOC_H__

#include <linux/types.h>

#define VID_CORTINA_ACCESS		0x8F3

#define CHIP_CA7742			0x010C	/* G3 */
#define CHIP_CA8279			0x010D	/* G3HGU */
#define CHIP_CA8271			0xA17C	/* Saturn SFU */
#define CHIP_CA8271S			0xA17D	/* Saturn SFP+ */
#define CHIP_CA8289			0xA18C	/* Venus */
#define CHIP_CA8271N			0xA19C	/* Saturn2 SFU */
#define CHIP_CA8271NS			0xA19D	/* Saturn2 SFP+ */
#define CHIP_CA8272NI			0xA19E	/* Saturn2 8272NI */
#define CHIP_CA8299			0x898D	/* Mercury 19X19 */
#define CHIP_CA8299S			0x898F	/* Mercury 17X17 */

#define CA_SOC_VENDOR_ID(jtag_id)	((jtag_id) & 0xFFF)
#define CA_SOC_CHIP_ID(jtag_id)		(((jtag_id) >> 12) & 0xFFFF)
#define CA_SOC_CHIP_REV(jtag_id)	(((jtag_id) >> 28) & 0xF)

#define CA7742_NAME			"CA7742(G3)"
#define CA8279_NAME			"CA8279(G3HGU)"
#define CA8271_NAME			"CA8271(Saturn SFU)"
#define CA8271S_NAME			"CA8271S(Saturn SFU+)"
#define CA8289_NAME			"CA8289(Venus)"
#define CA8271N_NAME			"CA8271N(Saturn2 SFU)"
#define CA8271NS_NAME			"CA8271NS(Saturn2 SFP+)"
#define CA8272NI_NAME			"CA8272NI(Saturn2 8272NI)"
#define CA8299_NAME			"CA8299(Mercury)"
#define CA8299S_NAME			"CA8299S(Mercury)"

struct ca_soc_data {
	u32 vendor_id;
	u32 chip_id;
	u32 part_no;
	u32 chip_revision;
	u32 pon_revision;

	u32 cpll;
	u32 epll;
	u32 fpll;
	u32 cpu_clk;
	u32 cci_clk;
	u32 lsaxi_clk;
	u32 hsaxi_clk;
	u32 core_clk;
	u32 crypto_clk;
	u32 eaxi_clk;
	u32 atb_clk;
	u32 pe_clk;
	u32 peaxi_clk;
	u32 per_clk;

	u32 io_clk;
	u32 eth_ref2_clk;
};

extern struct proc_dir_entry *ca_proc_dir;

#ifndef CONFIG_SOC_BUS_EXTEND
/* A deprecated leggacy API. Please use soc_device_one_match() in place of it.
 */
int ca_soc_data_get(struct ca_soc_data *data);
#endif

#endif
