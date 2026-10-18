/* SPDX-License-Identifier: GPL-2.0 */
/* minic: ioctl (the request encoding is left to the caller) */
#ifndef _MINIC_SYS_IOCTL_H
#define _MINIC_SYS_IOCTL_H

int ioctl(int fd, unsigned long request, ...);

#endif /* _MINIC_SYS_IOCTL_H */
