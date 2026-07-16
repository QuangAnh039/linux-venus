/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CA_KERNEL_HOOK_API_MERCURY_H
#define CA_KERNEL_HOOK_API_MERCURY_H

/***************************************************************************
 * ca_kernel_hook_api_mercury.h
 *
 * This exports hook functions and definition for Linux.
 ***************************************************************************/

#include <linux/netdevice.h>
#include <linux/if_pppox.h>
#include <linux/if_bridge.h>
#include <linux/l2tp.h>
#include <net/neighbour.h>
#include <net/ip_tunnels.h>
#include <net/ip_fib.h>
#include <net/ip6_fib.h>
#include <net/ip6_tunnel.h>
#include <net/netfilter/nf_conntrack_tuple.h>

/***************************************************************************
 * proc 'hook'
 ***************************************************************************/

#define CA_KH_HOOK_IPV4_FORWARD			0x00000001
#define CA_KH_HOOK_IPV6_FORWARD			0x00000002
#define CA_KH_HOOK_BRIDGE_MULTICAST		0x00000004
#define CA_KH_HOOK_IP_MULTICAST			0x00000008
#define CA_KH_HOOK_NF_DROP			0x00000010
#define CA_KH_HOOK_PPPOE_KERNEL			0x00000020
#define CA_KH_HOOK_IPSEC			0x00000040
#define CA_KH_HOOK_L2TP				0x00000080
#define CA_KH_HOOK_IP6_TUNNEL			0x00000100
#define CA_KH_HOOK_6RD				0x00000200
#define CA_KH_HOOK_GRE				0x00000400
#define CA_KH_HOOK_VXLAN			0x00000800
#define CA_KH_HOOK_MACSEC			0x00001000
#define CA_KH_HOOK_IPV4_IPSEC_PASSTHROUGH	0x00010000
#define CA_KH_HOOK_IPV6_IPSEC_PASSTHROUGH	0x00020000
#define CA_KH_HOOK_GRE_PASSTHROUGH		0x00040000
#define CA_KH_HOOK_L2TPV3_OVER_IP_PASSTHROUGH	0x00080000
#define CA_KH_HOOK_IPV4_IN_IPV4_PASSTHROUGH	0x00100000
#define CA_KH_HOOK_IPV6_IN_IPV4_PASSTHROUGH	0x00200000 /* 6RD */
#define CA_KH_HOOK_IPV4_IN_IPV6_PASSTHROUGH	0x00400000 /* DS-Lite, MAP-E */
#define CA_KH_HOOK_IPV6_IN_IPV6_PASSTHROUGH	0x00800000

extern u32 ca_kh_hook;

/***************************************************************************
 * proc 'debug'
 ***************************************************************************/

#define CA_KH_DEBUG_INTERFACE			0x00000001
#define CA_KH_DEBUG_ARP				0x00000002
#define CA_KH_DEBUG_ROUTE			0x00000004
#define CA_KH_DEBUG_NAT				0x00000008
#define CA_KH_DEBUG_TUNNEL			0x00000010
#define CA_KH_DEBUG_MCAST			0x00000020
#define CA_KH_DEBUG_VLAN			0x00000040
#define CA_KH_DEBUG_BYPASS_PORTLIST		0x00000080
#define CA_KH_DEBUG_PORT_BINDING		0x00000100
#define CA_KH_DEBUG_ARP_NOTICE			0x00000200
#define CA_KH_DEBUG_DATAPATH			0x00000400

extern u32 ca_kh_debug;

/***************************************************************************
 * proc 'udp_offload_occasion'
 ***************************************************************************/

enum {
	CA_KH_UDP_OFFLOAD_WHEN_ASSURED = 0,
	CA_KH_UDP_OFFLOAD_WHEN_CONFIRMED = 1,
};

extern u32 ca_kh_udp_offload_occasion;

/***************************************************************************
 * proc 'bypass portlist'
 ***************************************************************************/

enum {
	CA_BYPASS_CNT_INVALID,
	CA_BYPASS_CNT_VALID,
};

/***************************************************************************
 * common
 ***************************************************************************/

#define __CA_ETH_ADDR_LEN	6

#define CA_MCAST_MAX_ADDRESS	8

enum __ca_ip_afi {
	__CA_IPV4 = 0,
	__CA_IPV6 = 1,
};

enum __ca_mcast_filtermode {
	__CA_MCAST_EXCLUDE = 0,
	__CA_MCAST_INCLUDE = 1,
};

union __ca_l3_ip_addr {
	u32	addr[4];
	u32	ipv4_addr;
	u32	ipv6_addr[4];
};

struct __ca_ip_address {
	enum __ca_ip_afi	afi;		/* address family identifier */
	u8			addr_len;	/* length in bits */
	union __ca_l3_ip_addr	ip_addr;	/* IP address, host order */
};

struct __ca_l2_mcast_entry {
	u16				mcast_vlan;
	u8				grp_mac_addr[__CA_ETH_ADDR_LEN];
	enum __ca_mcast_filtermode	filter_mode;
	u16				src_num;	/* number of SA in list */
	struct __ca_ip_address		src_ip_address_list[CA_MCAST_MAX_ADDRESS];
};

/* from OpenWRT package 'nat46' */
enum ca_nat46_khook_xlate_style {
	CA_NAT46_XLATE_NONE = 0,
	CA_NAT46_XLATE_MAP,
	CA_NAT46_XLATE_MAP0,
	CA_NAT46_XLATE_RFC6052,
};

/* from OpenWRT package 'nat46' */
struct ca_nat46_khook_rule {
	enum ca_nat46_khook_xlate_style style;
	struct in6_addr ipv6_prefix;
	u32 ipv6_prefix_len;
	struct in_addr ipv4_prefix;
	u32 ipv4_prefix_len;
	u32 ea_len;
	u32 psid_offset;
	bool fmr_flag;
};

/* from OpenWRT package 'nat46' */
struct ca_nat46_khook_rule_pair {
	struct ca_nat46_khook_rule local;
	struct ca_nat46_khook_rule remote;
};

struct __net_bridge_port {
	struct net_device	*dev;
	struct list_head	list;
};

struct ca_l2tp_khook_cfg {
	/* from "struct l2tp_tunnel" in <linux>/net/l2tp/l2tp_core.h */
	struct net		*net;
	u32		tunnel_id;
	u32		peer_tunnel_id;
	int			version;
	enum l2tp_encap_type	encap;

	bool			is_ipv6;
	struct in_addr		local_ip;
	struct in_addr		peer_ip;
#if IS_ENABLED(CONFIG_IPV6)
	struct in6_addr		local_ip6;
	struct in6_addr		peer_ip6;
#endif

	u16		local_udp_port;
	u16		peer_udp_port;
	unsigned int		use_udp_checksums:1,
				udp6_zero_tx_checksums:1,
				udp6_zero_rx_checksums:1;

	/* from "struct l2tp_session" in <linux>/net/l2tp/l2tp_core.h */
	u32		session_id;
	u32		peer_session_id;
	u8		cookie[8];
	int			cookie_len;
	u8		peer_cookie[8];
	int			peer_cookie_len;
	u16		l2specific_len;
	u16		l2specific_type;
	u32		*nr;	/* addr of session NR state (receive) */
	u32		*ns;	/* addr of session NR state (send) */
	unsigned int		recv_seq : 1;	/* expect receive packets with
						 * sequence numbers?
						 */
	unsigned int		send_seq : 1;	/* send packets with sequence
						 * numbers?
						 */
	unsigned int		lns_mode : 1;	/* behave as LNS? LAC enables
						 * sequence numbers under
						 * control of LNS.
						 */
	int			mtu;
	enum l2tp_pwtype	pwtype;
};

struct kernel_hook_ops {
	int (*kho_l3_intf_add)(u16 device_id, struct net_device *dev,
			       struct __ca_ip_address *ip_addr,
			       u8 tunnel_id);
	int (*kho_l3_intf_update_ip_addr)(u16 device_id, int ifindex,
					  struct __ca_ip_address *ip_addr);
	int (*kho_l3_intf_update_mtu)(u16 device_id, int ifindex, int mtu);
	int (*kho_l3_intf_update_mac_addr)(u16 device_id, int ifindex,
					   u8 *mac_addr, int addr_len);
	int (*kho_l3_intf_delete)(u16 device_id, struct net_device *dev);

	int (*kho_l3_route_add_ipv4_static)(u16 device_id,
					    struct net_device *dev,
					    struct fib_config *cfg);
	int (*kho_l3_route_del_ipv4_ifdown)(u16 device_id,
					    struct net_device *dev);
	int (*kho_l3_route_del_ipv4_static)(u16 device_id,
					    struct net_device *dev,
					    struct fib_config *cfg);
	int (*kho_l3_route_add_ipv6_static)(u16 device_id,
					    struct net_device *dev,
					    struct fib6_config *cfg);
	int (*kho_l3_route_del_ipv6_static)(u16 device_id, int ifindex,
					    struct in6_addr *dst, int dst_plen,
					    struct in6_addr *gateway);
	int (*kho_l3_route_del_ipv6_ifdown)(u16 device_id,
					    struct net_device *dev);
	int (*kho_l3_nexthop_aging_timer_set)(u16 device_id,
					      u32 time);

	int (*kho_neigh_add)(u16 device_id, struct net_device *dev,
			     u8 *da_mac, struct __ca_ip_address *ip_addr,
			     int child_ifindex, bool is_static);
	int (*kho_neigh_delete)(u16 device_id, struct net_device *dev,
				struct __ca_ip_address *ip_addr, bool is_static);
	int (*kho_neigh_update)(u16 device_id, struct net_device *dev,
				u8 *da_mac, struct __ca_ip_address *ip_addr,
				int child_ifindex, bool is_static);
	int (*kho_neigh_update_neighbour)(u16 device_id,
					  struct neighbour *neigh);
	int (*kho_neigh_timer_restart)(u16 device_id,
				       struct neighbour *neigh);

	int (*kho_nat_entry_add)(u16 device_id, u8 ip_proto,
				 int is_trans_src_ip, struct in_addr *old_src_ip,
				 struct in_addr *new_src_ip, struct in_addr *old_dst_ip,
				 struct in_addr *new_dst_ip, __be16 old_src_port,
				 __be16 new_src_port, __be16 old_dst_port,
				 __be16 new_dst_port, struct sk_buff *skb);
	int (*kho_nat_entry_delete)(u16 device_id, struct in_addr *ipv4_addr);
	int (*kho_nat_entry_session_delete)(u16 device_id, u8 ip_proto,
					    __be32 old_src_ip,
					    __be32 old_dst_ip,
					    __be16 old_src_port,
					    __be16 old_dst_port);
	int (*kho_nat_entry_session_delete_force)(u16 device_id,
						  struct nf_conntrack_tuple *tuple);
	int (*kho_nat_entry_delete_by_port)(u16 device_id, u16 port);
	int (*kho_nat_entry_timer_refresh)(u16 device_id,
					   struct nf_conntrack_tuple *tuple1,
					   struct nf_conntrack_tuple *tuple2,
					   u32 *aging_time);
	int (*kho_nat_entry_hit_time_refresh)(u16 device_id,
					      struct nf_conntrack_tuple *tuple1,
					      struct nf_conntrack_tuple *tuple2,
					      struct timespec64 *last_hit_time);

	int (*kho_nat6_entry_add)(u16 device_id, u8 ip_proto,
				  int is_trans_src_ip, struct in6_addr *old_src_ip,
				  struct in6_addr *new_src_ip, struct in6_addr *old_dst_ip,
				  struct in6_addr *new_dst_ip, __be16 old_src_port,
				  __be16 new_src_port, __be16 old_dst_port,
				  __be16 new_dst_port, struct sk_buff *skb);
	int (*kho_nat6_entry_delete)(u16 device_id, struct in6_addr *ipv6_addr);

	int (*kho_ip6tnl_add)(u16 device_id, int ifindex,
			      struct __ip6_tnl_parm *p, u8 *ret_tunnel_id);
	int (*kho_ip6tnl_delete)(u16 device_id, struct net_device *dev,
				 u8 kept_tunnel_id);
	int (*kho_ip6tnl_delete_intf_only)(u16 device_id,
					   struct net_device *dev);

	int (*kho_nat46_add)(u16 device_id, int ifindex,
			     struct ca_nat46_khook_rule_pair *p, u8 *ret_tunnel_id);
	int (*kho_nat46_delete)(u16 device_id, struct net_device *dev,
				u8 kept_tunnel_id);
	int (*kho_nat46_delete_intf_only)(u16 device_id,
					  struct net_device *dev);

	int (*kho_6rd_add)(u16 device_id, int ifindex, struct ip_tunnel *t,
			   u8 *ret_tunnel_id);
	int (*kho_6rd_delete)(u16 device_id, struct net_device *dev,
			      u8 kept_tunnel_id);
	int (*kho_6rd_update)(u16 device_id, int ifindex, struct ip_tunnel *t);
	int (*kho_6rd_delete_intf_only)(u16 device_id, struct net_device *dev);

	int (*kho_v4gre_add)(u16 device_id,
			     struct net_device *dev,
			     struct ip_tunnel *t,
			     u8 *ret_tunnel_id);
	int (*kho_v4gre_delete)(u16 device_id, struct net_device *dev,
				u8 kept_tunnel_id);

	int (*kho_v6gre_add)(u16 device_id,
			     struct net_device *dev,
			     struct ip6_tnl *t,
			     u8 *ret_tunnel_id);
	int (*kho_v6gre_delete)(u16 device_id, struct net_device *dev,
				u8 kept_tunnel_id);

	int (*kho_map_e_add)(u16 device_id, int ifindex,
			     struct __ip6_tnl_parm *p,
			     u8 *ret_tunnel_id);
	int (*kho_map_e_delete)(u16 device_id, struct net_device *dev,
				u8 kept_tunnel_id);
	int (*kho_map_e_delete_intf_only)(u16 device_id,
					  struct net_device *dev);

	int (*kho_l2tp_add)(u16 device_id,
			    struct net_device *dev,
			    struct ca_l2tp_khook_cfg *ca_l2tp_cfg,
			    u8 *ret_tunnel_id);
	int (*kho_l2tp_delete)(u16 device_id, struct net_device *dev,
			       u8 kept_tunnel_id);

	int (*kho_vxlan_open)(u16 device_id,
			      struct net_device *dev,
			      u16 dst_l4_port,
			      u32 vni);
	int (*kho_v4vxlan_add)(u16 device_id,
			       struct net_device *dev,
			       struct net_device *dst_dev,
			       u32 src_ip,
			       u32 dst_ip,
			       u16 src_l4_port,
			       u16 dst_l4_port,
			       u32 vni,
			       u8 tos,
			       u8 ttl);

	int (*kho_vxlan_delete)(u16 device_id,
				struct net_device *dev,
				sa_family_t sa_family);
	int (*kho_v6vxlan_add)(u16 device_id,
			       struct net_device *dev,
			       struct net_device *dst_dev,
			       u32 *src_ip,
			       u32 *dst_ip,
			       u16 src_l4_port,
			       u16 dst_l4_port,
			       u32 vni,
			       u8 tos,
			       u8 ttl,
			       u32 flow_label);

	int (*kho_macsec_sa_add)(u16 device_id, struct net_device *dev,
				 bool direction, u8 assoc_num, void *rx_sa_p);
	int (*kho_macsec_sc_add)(u16 device_id, struct net_device *dev,
				 void *rx_sa_p);
	int (*kho_macsec_delete)(u16 device_id, struct net_device *dev);
	int (*kho_macsec_open)(u16 device_id, struct net_device *dev);

	int (*kho_pppoe_add)(u16 device_id, struct net_device *ppp_dev,
			     struct net_device *eth_dev, struct pppox_sock *po);
	int (*kho_pppoe_delete)(u16 device_id, struct net_device *ppp_dev);
	int (*kho_pppoe_update)(u16 device_id, struct net_device *ppp_dev);

	int (*kho_ipsec_add)(u16 device_id, struct xfrm_state *x_in,
			     struct xfrm_policy *xp_in, struct xfrm_state *x_out,
			     struct xfrm_policy *xp_out);
	int (*kho_ipsec_delete)(u16 device_id, struct xfrm_policy *x_in,
				struct xfrm_policy *x_out);
	int (*kho_ipsec_redirect_to_pe)(struct sk_buff *skb, struct xfrm_state *x,
					enum __ca_ip_afi afi, unsigned char dir);

	int (*kho_bypass_tcp_portlist_add)(u16 dev_id, u16 port,
					   u8 flag);
	int (*kho_bypass_tcp_portlist_delete)(u16 dev_id, u16 port,
					      u8 flag);
	void (*kho_bypass_tcp_portlist_dump)(void);
	int (*kho_bypass_tcp_portlist_exist)(u16 port);

	int (*kho_bypass_udp_portlist_add)(u16 dev_id, u16 port,
					   u8 flag);
	int (*kho_bypass_udp_portlist_delete)(u16 dev_id, u16 port,
					      u8 flag);
	void (*kho_bypass_udp_portlist_dump)(void);
	int (*kho_bypass_udp_portlist_exist)(u16 port);

	int (*kho_8021q_pcp_handler)(struct sk_buff *skb);

	int (*kho_port_binding_check)(u16 device_id, struct sk_buff *skb);

	int (*kho_l2_vlan_add)(u16 device_id, struct net_device *parent_dev,
			       int vid);
	int (*kho_l2_vlan_delete)(u16 device_id, struct net_device *dev);
	int (*kho_l2_br_add_del_if)(u16 device_id, struct net_device *br_dev,
				    struct __net_bridge_port *br_ports, bool is_add,
				    struct net_device *target_dev);

	int (*kho_offload_permit)(u16 device_id,
				  struct sk_buff *skb,
				  bool offload);
	int (*kho_offload_permit_by_packet_count)(u16 device_id,
						  struct sk_buff *skb);
	int (*kho_offload_tcp)(u16 device_id,
			       struct sk_buff *skb,
			       unsigned char pf);
	int (*kho_offload_udp)(u16 device_id,
			       struct sk_buff *skb,
			       unsigned char pf);
	int (*kho_offload_l2gre)(u16 device_id,
				 struct sk_buff *skb);
	int (*kho_offload_generic)(u16 device_id,
				   struct sk_buff *skb,
				   unsigned char pf);

	int (*kho_br_ipa_mdb_add)(void *mdb);
	int (*kho_br_ipa_mdb_del)(void *mdb);
	int (*kho_br_ipa_port_mdb_add)(void *mdb, void *bpg);
	int (*kho_br_ipa_port_mdb_del)(void *mdb, void *bpg);
	int (*kho_ip_mc_vif_add)(void *in_dev, void *vif, bool ipv4, int vifi);
	int (*kho_ip_mc_vif_del)(void *in_dev, void *vif, bool ipv4, int vifi);
	int (*kho_http_cls_add)(unsigned int port);
	int (*kho_http_cls_del)(unsigned int port);
};

extern struct kernel_hook_ops ca_kernel_hook_ops;
int neigh_get_child_ifindex_by_skb(struct sk_buff *skb);

#endif /* CA_KERNEL_HOOK_API_MERCURY_H */
