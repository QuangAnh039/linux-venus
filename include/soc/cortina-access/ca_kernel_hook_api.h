/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CA_KERNEL_HOOK_API_H
#define CA_KERNEL_HOOK_API_H

#if defined(CONFIG_ARCH_CA_MERCURY)
#include <soc/cortina-access/ca_kernel_hook_api_mercury.h>
#elif defined(CONFIG_ARCH_CA_VENUS)
#include <soc/cortina-access/ca_kernel_hook_api_venus.h>
#endif

#endif /* CA_KERNEL_HOOK_API_H */
