/* SPDX-License-Identifier: GPL-2.0 */
/* minic: the sched bits the init uses */
#ifndef _MINIC_SCHED_H
#define _MINIC_SCHED_H

#include <sys/types.h>

struct sched_param {
	int sched_priority;
};

#define SCHED_OTHER	0
#define SCHED_FIFO	1
#define SCHED_RR	2

int sched_setscheduler(pid_t pid, int policy,
		       const struct sched_param *param);

#endif /* _MINIC_SCHED_H */
