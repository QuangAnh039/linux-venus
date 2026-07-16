/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CA_OFFLOAD_INFO_H__
#define __CA_OFFLOAD_INFO_H__

#include <linux/in.h>
#include <linux/in6.h>

#include <soc/cortina-access/ca_packet_info.h>

#define CA_MAGIC	0x0D06F00D	/* dog food */

enum ca_xfrm_state {
	XFRM_DISABLED,
	XFRM_TX,
	XFRM_RX
};

struct ca_offload_info {
	/* Set ingress_magic to CA_MAGIC packet is received by Cortina NI Rx.
	 * If the value is not CA_MGIC, pcaket may come from CPU self, WiFi,
	 * or other Ethernet driver.
	 */
	unsigned int ingress_magic;

	/* store ingress (original) packet contents */
	struct ca_packet_info ingress_pkt_info;
	bool ingress_pkt_info_valid;

	/* Set egress_magic to CA_MAGIC packet would be transmitted by Cortina NI Tx.
	 * If the value is not CA_MGIC, pcaket may be sent to peer from WiFi
	 * or other Ethernet driver.
	 */
	unsigned int egress_magic;

	/* store egress packet contents */
	struct ca_packet_info egress_pkt_info;
	bool egress_pkt_info_valid;

	/* Policy-based routing includes source routing.
	 * egress_ifindex is valid when policy_route is true.
	 * ipv4_nh is valid when policy_route and pkt_info.is_ipv4 are both true.
	 * ipv6_nh is valid when policy_route and pkt_info.is_ipv6 are both true.
	 */
	bool policy_route;
	int egress_ifindex;
	union {
		struct in_addr ipv4_nh;
		struct in6_addr ipv6_nh;
	};

	bool sw_only;
	enum ca_xfrm_state xfrm;
	u8 xfrm_proto;
	unsigned long lifetime; /* jiffies */

#if defined(CONFIG_CA_KERNEL_HOOK)
	struct nf_conn *ct;
	bool ct_valid;
#endif

	/* record rx_virt_addr for cpu port 6 and port 7 shared with offload and
	 * normal path
	 */
	void *rx_virt_addr;
};

#endif /* __CA_OFFLOAD_INFO_H__ */
