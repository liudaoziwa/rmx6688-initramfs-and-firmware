// SPDX-License-Identifier: GPL-2.0
/*
 * minic: a bump allocator on top of mmap.
 *
 * The init makes exactly one allocation (a 4 MB log buffer for the CCCI
 * modem logger) and never frees it, so this is as small as it can get: each
 * block carries a 16-byte header (size + padding for alignment), free()
 * simply unmaps it again.
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct hdr {
	size_t size;
	void *raw;	/* start of the mapping (== hdr, kept for clarity) */
};

#define HDR_SIZE 16

static size_t map_len(size_t size)
{
	return (size + HDR_SIZE + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
}

void *malloc(size_t size)
{
	struct hdr *h;

	if (!size)
		size = 1;
	h = mmap(NULL, map_len(size), PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (h == MAP_FAILED)
		return NULL;
	h->size = size;
	h->raw = h;
	return (char *)h + HDR_SIZE;
}

void free(void *ptr)
{
	struct hdr *h;

	if (!ptr)
		return;
	h = (struct hdr *)((char *)ptr - HDR_SIZE);
	munmap(h, map_len(h->size));
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	void *p;

	if (nmemb && total / nmemb != size) {
		errno = ENOMEM;
		return NULL;
	}
	p = malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}

void *realloc(void *ptr, size_t size)
{
	struct hdr *h;
	void *p;

	if (!ptr)
		return malloc(size);
	if (!size) {
		free(ptr);
		return NULL;
	}
	h = (struct hdr *)((char *)ptr - HDR_SIZE);
	p = malloc(size);
	if (!p)
		return NULL;
	memcpy(p, ptr, h->size < size ? h->size : size);
	free(ptr);
	return p;
}
