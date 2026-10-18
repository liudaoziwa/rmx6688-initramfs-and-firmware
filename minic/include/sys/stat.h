/* SPDX-License-Identifier: GPL-2.0 */
/* minic: mkdir (the init uses nothing else from stat) */
#ifndef _MINIC_SYS_STAT_H
#define _MINIC_SYS_STAT_H

#include <sys/types.h>

int mkdir(const char *path, mode_t mode);

#endif /* _MINIC_SYS_STAT_H */
