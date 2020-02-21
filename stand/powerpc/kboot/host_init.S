/*
 *
 * $FreeBSD$
 */

#include <machine/asm.h>

/*
 * Linux-style entry point:
 * r1: Initial sp
 * r2: Intialized by ENTRY macro
 * r3: Need to load argc
 * r4: Need to load argv
 * r5: Need to load envp
 * r6: Need to load auxv
 * r7: atexit pointer or 0
 * r12: &_start
 */
ENTRY(_start)
	/* Use r6 to traverse the stack. */
	mr	%r6, %r1

	/* Align stack pointer. */
	rldicr	%r1, %r1, 0, 59
	/* Allocate zero frame. */
	stdu	%r1, -32(%r1)
	li	%r0, 0
	/* Disassociate stack chain. */
	std	%r0, 0(%r1)

	ld	%r3, 0(%r6)     /* argc */
	addi	%r6, %r6, 8     /* next dword */
	mr	%r4, %r6        /* argv */
	mulli	%r5, %r3, 8     /* compute argv size */
	add	%r6, %r6, %r5   /* skip over argv */
	addi	%r6, %r6, 8     /* next dword */
	mr	%r5, %r6        /* envp */
	/*
	 * Don't bother with auxv for now. If we end up needing it, it's a
	 * word past the NULL at the end of the envp array.
	 */
	li	%r6, 0

	b	main
	/* Trap if we return to this frame -- exit should be via syscall. */
	trap

