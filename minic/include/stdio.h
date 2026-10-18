/* SPDX-License-Identifier: GPL-2.0 */
/* minic: snprintf/vsnprintf only */
#ifndef _MINIC_STDIO_H
#define _MINIC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
	__attribute__((format(printf, 3, 0)));

#endif /* _MINIC_STDIO_H */
