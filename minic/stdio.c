// SPDX-License-Identifier: GPL-2.0
/*
 * minic: vsnprintf/snprintf.
 *
 * Covers what the init's logging uses: %s (with precision), %c, %d/%i, %u,
 * %x/%X, %o, %p and %%, with width, left-justify and zero padding (width
 * and the %s precision may come from an argument via '*').  Integer
 * precision is not implemented (never used here).  The return value is
 * capped at size - 1 so callers can feed it straight to write() without
 * running past the buffer.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

struct out {
	char *buf;
	size_t left;	/* character slots left before the NUL */
	size_t put;	/* total characters produced */
};

static void emit(struct out *o, char c)
{
	if (o->left) {
		*o->buf++ = c;
		o->left--;
	}
	o->put++;
}

static int digit(unsigned long v, unsigned int base, int upper, char *buf)
{
	char tmp[24];
	int n = 0;
	int i;

	do {
		int d = (int)(v % base);

		tmp[n++] = d < 10 ? (char)('0' + d)
				  : (char)((upper ? 'A' : 'a') + d - 10);
		v /= base;
	} while (v);
	for (i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	return n;
}

static void emit_padded(struct out *o, const char *s, int len, int width,
			int leftjust)
{
	int pad = width > len ? width - len : 0;
	int i;

	if (leftjust) {
		for (i = 0; i < len; i++)
			emit(o, s[i]);
		while (pad--)
			emit(o, ' ');
	} else {
		while (pad--)
			emit(o, ' ');
		for (i = 0; i < len; i++)
			emit(o, s[i]);
	}
}

static void emit_num(struct out *o, unsigned long uv, unsigned int base,
		     int upper, int neg, int width, int zero, int leftjust)
{
	char nbuf[24];
	int nlen = digit(uv, base, upper, nbuf);
	int len = nlen + (neg ? 1 : 0);
	int pad = width > len ? width - len : 0;
	int i;

	if (leftjust) {
		if (neg)
			emit(o, '-');
		for (i = 0; i < nlen; i++)
			emit(o, nbuf[i]);
		while (pad--)
			emit(o, ' ');
	} else {
		if (zero) {
			if (neg)
				emit(o, '-');
			while (pad--)
				emit(o, '0');
		} else {
			while (pad--)
				emit(o, ' ');
			if (neg)
				emit(o, '-');
		}
		for (i = 0; i < nlen; i++)
			emit(o, nbuf[i]);
	}
}

static void format(struct out *o, const char *fmt, va_list ap)
{
	for (;;) {
		char c = *fmt++;
		int leftjust = 0;
		int zero = 0;
		int is_long = 0;
		int width = 0;
		int prec = -1;
		char spec;

		if (!c)
			return;
		if (c != '%') {
			emit(o, c);
			continue;
		}
		for (;; fmt++) {
			if (*fmt == '-')
				leftjust = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt == '+' || *fmt == ' ' || *fmt == '#')
				;	/* accepted, ignored */
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
			if (width < 0) {
				leftjust = 1;
				width = -width;
			}
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			if (fmt[1] == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				fmt += 2;
			} else {
				prec = 0;
				for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++)
					prec = prec * 10 + (*fmt - '0');
			}
		}
		while (*fmt == 'l' || *fmt == 'z' || *fmt == 't' ||
		       *fmt == 'j') {
			is_long = 1;
			fmt++;
		}
		while (*fmt == 'h')
			fmt++;
		spec = *fmt;
		if (spec)
			fmt++;

		switch (spec) {
		case 'd':
		case 'i': {
			long v = is_long ? va_arg(ap, long)
					 : (long)va_arg(ap, int);
			unsigned long uv = (unsigned long)v;
			int neg = 0;

			if (v < 0) {
				neg = 1;
				uv = ~uv + 1;
			}
			emit_num(o, uv, 10, 0, neg, width, zero, leftjust);
			break;
		}
		case 'u':
			emit_num(o, is_long ? va_arg(ap, unsigned long)
					    : va_arg(ap, unsigned int),
				 10, 0, 0, width, zero, leftjust);
			break;
		case 'x':
		case 'X':
			emit_num(o, is_long ? va_arg(ap, unsigned long)
					    : va_arg(ap, unsigned int),
				 16, spec == 'X', 0, width, zero, leftjust);
			break;
		case 'o':
			emit_num(o, is_long ? va_arg(ap, unsigned long)
					    : va_arg(ap, unsigned int),
				 8, 0, 0, width, zero, leftjust);
			break;
		case 'c': {
			char s[1];

			s[0] = (char)va_arg(ap, int);
			emit_padded(o, s, 1, width, leftjust);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len = 0;

			if (!s)
				s = "(null)";
			while (s[len] && (prec < 0 || len < prec))
				len++;
			emit_padded(o, s, len, width, leftjust);
			break;
		}
		case 'p': {
			char pb[24];
			int pl = 2;

			pb[0] = '0';
			pb[1] = 'x';
			pl += digit((unsigned long)va_arg(ap, void *), 16, 0,
				    pb + 2);
			emit_padded(o, pb, pl, width, leftjust);
			break;
		}
		case '%':
			emit(o, '%');
			break;
		default:
			emit(o, '%');
			if (spec)
				emit(o, spec);
		}
	}
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o;
	int ret;

	o.buf = buf;
	o.left = size ? size - 1 : 0;
	o.put = 0;
	format(&o, fmt, ap);
	if (size)
		*o.buf = '\0';
	ret = (int)o.put;
	if (size && (size_t)ret > size - 1)
		ret = (int)(size - 1);
	return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return ret;
}
