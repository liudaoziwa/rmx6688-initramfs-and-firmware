/* SPDX-License-Identifier: GPL-2.0 */
/* minic: prctl (only what the init passes) */
#ifndef _MINIC_SYS_PRCTL_H
#define _MINIC_SYS_PRCTL_H

#define PR_SET_NAME		15
#define PR_GET_NAME		16
#define PR_SET_PDEATHSIG	1
#define PR_SET_DUMPABLE		4
#define PR_SET_NO_NEW_PRIVS	38

int prctl(int option, ...);

#endif /* _MINIC_SYS_PRCTL_H */
