/* SPDX-License-Identifier: GPL-2.0 */
/* minic: timespec + nanosleep */
#ifndef _MINIC_TIME_H
#define _MINIC_TIME_H

#include <sys/types.h>

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* _MINIC_TIME_H */
