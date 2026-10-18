/* SPDX-License-Identifier: GPL-2.0 */
/* minic: string/memory routines */
#ifndef _MINIC_STRING_H
#define _MINIC_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strchr(const char *s, int c);
size_t strcspn(const char *s, const char *reject);
char *strstr(const char *hay, const char *needle);

char *strerror(int err);

#endif /* _MINIC_STRING_H */
