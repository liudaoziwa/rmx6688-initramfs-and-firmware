/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minic: dirent (linux_dirent64 layout; readdir returns entries untouched,
 * unlike glibc which reorders them).
 */
#ifndef _MINIC_DIRENT_H
#define _MINIC_DIRENT_H

#include <stdint.h>

struct dirent {
	uint64_t	d_ino;
	int64_t		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[256];
};

typedef struct __dirstream DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif /* _MINIC_DIRENT_H */
