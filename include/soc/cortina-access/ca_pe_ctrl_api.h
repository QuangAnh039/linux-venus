/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CA_PE_CTRL_H__
#define __CA_PE_CTRL_H__

#define PE_CTRL_IOC_MAGIC  'U'
#define PE_CTRL_IOCTL_RESET_PE0   _IOWR(PE_CTRL_IOC_MAGIC, 1, int)
#define PE_CTRL_IOCTL_ENABLE_PE0  _IOWR(PE_CTRL_IOC_MAGIC, 2, int)
#define PE_CTRL_IOCTL_RESET_PE1   _IOWR(PE_CTRL_IOC_MAGIC, 3, int)
#define PE_CTRL_IOCTL_ENABLE_PE1  _IOWR(PE_CTRL_IOC_MAGIC, 4, int)

#endif /* __CA_PE_CTRL_H__ */
