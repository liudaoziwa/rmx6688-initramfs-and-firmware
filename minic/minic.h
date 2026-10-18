/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minic: private plumbing shared between the library sources (not a public
 * header; the standard-named headers live in include/).
 */
#ifndef MINIC_H
#define MINIC_H

/* AArch64 syscall numbers used by the library (asm-generic table). */
#define MINIC_NR_ioctl			29
#define MINIC_NR_mkdirat		34
#define MINIC_NR_umount2		39
#define MINIC_NR_mount			40
#define MINIC_NR_faccessat		48
#define MINIC_NR_chdir			49
#define MINIC_NR_chroot			51
#define MINIC_NR_openat			56
#define MINIC_NR_close			57
#define MINIC_NR_getdents64		61
#define MINIC_NR_read			63
#define MINIC_NR_write			64
#define MINIC_NR_pread64		67
#define MINIC_NR_fsync			82
#define MINIC_NR_exit_group		94
#define MINIC_NR_nanosleep		101
#define MINIC_NR_sched_setscheduler	119
#define MINIC_NR_setsid			157
#define MINIC_NR_uname			160
#define MINIC_NR_prctl			167
#define MINIC_NR_munmap			215
#define MINIC_NR_clone			220
#define MINIC_NR_execve			221
#define MINIC_NR_mmap			222
#define MINIC_NR_finit_module		273

long __syscall6(long nr, long a1, long a2, long a3, long a4, long a5,
		long a6);
long __syscall_ret(long ret);

#endif /* MINIC_H */
