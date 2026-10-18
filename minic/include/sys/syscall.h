/* SPDX-License-Identifier: GPL-2.0 */
/* minic: syscall() variadic shim (implemented in crt0.S) */
#ifndef _MINIC_SYS_SYSCALL_H
#define _MINIC_SYS_SYSCALL_H

#define SYS_finit_module 273

long syscall(long nr, ...);

#endif /* _MINIC_SYS_SYSCALL_H */
