/* SPDX-License-Identifier: GPL-2.0 */
/* minic: utsname */
#ifndef _MINIC_SYS_UTSNAME_H
#define _MINIC_SYS_UTSNAME_H

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

int uname(struct utsname *buf);

#endif /* _MINIC_SYS_UTSNAME_H */
