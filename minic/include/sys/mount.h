/* SPDX-License-Identifier: GPL-2.0 */
/* minic: mount/umount (asm-generic MS_* values) */
#ifndef _MINIC_SYS_MOUNT_H
#define _MINIC_SYS_MOUNT_H

#define MS_RDONLY	1
#define MS_NOSUID	2
#define MS_NODEV	4
#define MS_NOEXEC	8
#define MS_SYNCHRONOUS	16
#define MS_REMOUNT	32
#define MS_MANDLOCK	64
#define MS_DIRSYNC	128

#define MS_NOSYMFOLLOW	256
#define MS_NOATIME	1024
#define MS_NODIRATIME	2048
#define MS_BIND		4096
#define MS_MOVE		8192
#define MS_REC		16384
#define MS_SILENT	32768
#define MS_POSIXACL	(1 << 16)
#define MS_UNBINDABLE	(1 << 17)
#define MS_PRIVATE	(1 << 18)
#define MS_SLAVE	(1 << 19)
#define MS_SHARED	(1 << 20)
#define MS_RELATIME	(1 << 21)

int mount(const char *src, const char *target, const char *fstype,
	  unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#endif /* _MINIC_SYS_MOUNT_H */
