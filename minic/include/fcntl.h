/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minic: open flags.  As on every 64-bit arch, arm64 does not use the
 * asm-generic values for O_DIRECT/O_LARGEFILE/O_DIRECTORY/O_NOFOLLOW; the
 * values here match the arm64 uapi (and glibc's aarch64 headers).
 */
#ifndef _MINIC_FCNTL_H
#define _MINIC_FCNTL_H

#include <sys/types.h>

#define O_RDONLY	00000000
#define O_WRONLY	00000001
#define O_RDWR		00000002

#define O_CREAT		00000100
#define O_EXCL		00000200
#define O_NOCTTY	00000400
#define O_TRUNC		00001000
#define O_APPEND	00002000
#define O_NONBLOCK	00004000
#define O_DSYNC		0010000
#define O_ASYNC		0020000
#define O_DIRECT	0200000
#define O_LARGEFILE	0
#define O_DIRECTORY	040000
#define O_NOFOLLOW	0100000
#define O_NOATIME	01000000
#define O_CLOEXEC	02000000
#define O_SYNC		04010000
#define O_PATH		010000000
#define O_TMPFILE	(020000000 | O_DIRECTORY)

#define AT_FDCWD	-100

int open(const char *path, int flags, ...);

#endif /* _MINIC_FCNTL_H */
