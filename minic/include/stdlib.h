/* SPDX-License-Identifier: GPL-2.0 */
/* minic: malloc/calloc/realloc/free (mmap-backed) */
#ifndef _MINIC_STDLIB_H
#define _MINIC_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

void _exit(int status) __attribute__((noreturn));

#endif /* _MINIC_STDLIB_H */
