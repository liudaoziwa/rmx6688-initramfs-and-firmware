// SPDX-License-Identifier: GPL-2.0
/*
 * minic: opendir/readdir/closedir.
 *
 * The getdents64 records come back in kernel order; the uid/gid/ino
 * reordering glibc does is not needed by the init, so readdir returns them
 * as-is.  One 32 KB buffer is enough for /dev and /sys/class/block.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "minic.h"

#define DIRBUF_SIZE 32768

struct __dirstream {
	int fd;
	size_t pos;
	size_t end;
	char buf[DIRBUF_SIZE];
};

DIR *opendir(const char *name)
{
	DIR *d;
	int fd;

	fd = open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	d = malloc(sizeof(*d));
	if (!d) {
		close(fd);
		return NULL;
	}
	d->fd = fd;
	d->pos = 0;
	d->end = 0;
	return d;
}

struct dirent *readdir(DIR *d)
{
	for (;;) {
		struct dirent *e;

		if (d->pos >= d->end) {
			long n = __syscall_ret(__syscall6(MINIC_NR_getdents64,
							  d->fd,
							  (long)d->buf,
							  DIRBUF_SIZE,
							  0, 0, 0));

			if (n <= 0)
				return NULL;
			d->pos = 0;
			d->end = (size_t)n;
		}
		e = (struct dirent *)(void *)(d->buf + d->pos);
		d->pos += e->d_reclen;
		if (e->d_ino)
			return e;
	}
}

int closedir(DIR *d)
{
	int ret = close(d->fd);

	free(d);
	return ret;
}
