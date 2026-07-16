// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/module.h>

u32 ca_kh_hook = 0xfff8ffff;
EXPORT_SYMBOL(ca_kh_hook);

u32 ca_kh_debug;
EXPORT_SYMBOL(ca_kh_debug);

u32 ca_kh_udp_offload_occasion;
EXPORT_SYMBOL(ca_kh_udp_offload_occasion);
