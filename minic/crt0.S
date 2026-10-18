/* SPDX-License-Identifier: GPL-2.0 */
/*
 * minic: entry point and raw syscall shims for the freestanding init.
 *
 * AArch64 Linux syscall ABI: number in x8, arguments in x0-x5, result in
 * x0.  __syscall6() takes (nr, a1..a6) in x0-x6 and repairs the register
 * layout; syscall() is the variadic shim for the one direct caller
 * (finit_module).  The kernel only looks at as many argument registers as
 * the syscall takes, so unset x3-x6 on a short call are harmless.
 *
 * __syscall6() returns the raw kernel result -- the C wrappers run it
 * through __syscall_ret() to set errno.  syscall() translates the result
 * itself because it is the libc entry point, so it sets errno like glibc.
 */

	.section .text
	.global _start
	.type _start, %function
_start:
	mov	x29, xzr		/* end the frame-pointer chain */
	mov	x30, xzr
	ldr	x0, [sp]		/* argc */
	add	x1, sp, #8		/* argv */
	bl	main
	mov	x8, #94			/* __NR_exit_group */
	svc	#0
1:	b	1b
	.size _start, . - _start

	.global __syscall6
	.type __syscall6, %function
__syscall6:
	mov	x8, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	mov	x4, x5
	mov	x5, x6
	svc	#0
	ret
	.size __syscall6, . - __syscall6

	.global syscall
	.type syscall, %function
syscall:
	mov	x8, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	mov	x4, x5
	mov	x5, x6
	svc	#0
	b	__syscall_ret
	.size syscall, . - syscall

	.section .note.GNU-stack, "", %progbits
