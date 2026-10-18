// SPDX-License-Identifier: GPL-2.0
/* minic: the handful of string/memory routines the init uses. */
#include <stddef.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	char *d = dst;
	const char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	char *d = dst;
	const char *s = src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		while (n--)
			d[n] = s[n];
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	char *d = dst;

	while (n--)
		*d++ = (char)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *pa = a;
	const unsigned char *pb = b;

	while (n--) {
		if (*pa != *pb)
			return (int)*pa - (int)*pb;
		pa++;
		pb++;
	}
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (!n)
		return 0;
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++))
		;
	return dst;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

size_t strcspn(const char *s, const char *reject)
{
	size_t n = 0;
	const char *r;

	for (; s[n]; n++)
		for (r = reject; *r; r++)
			if (*r == s[n])
				return n;
	return n;
}

char *strstr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return (char *)hay;
	for (; *hay; hay++)
		if (strncmp(hay, needle, n) == 0)
			return (char *)hay;
	return NULL;
}
