/* SPDX-License-Identifier: GPL-2.0 */
/* minic: freestanding stddef.h */
#ifndef _MINIC_STDDEF_H
#define _MINIC_STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* _MINIC_STDDEF_H */
