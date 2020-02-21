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
 */
ENTRY(_start)
	/* Use r6 to traverse the stack. */
	mr	%r6, %r1

	/* Set up initial stack frame. */
	addi	%r1, %r1, -16
	/* Ensure we're aligned. */
	clrrwi	%r1, %r1, 4

	li	%r0, 0
	/* Ensure the backchain is disconnected. */
	stw	%r0, 0(%r1)

	lwz	%r3, 0(%r6)     /* argc */
	addi	%r6, %r6, 4     /* next dword */
	mr	%r4, %r6        /* argv */
	mulli	%r5, %r3, 4     /* compute argv size */
	add	%r6, %r6, %r5   /* skip over argv */
	addi	%r6, %r6, 4     /* next dword */
	mr	%r5, %r6        /* envp */
	/*
	 * Don't bother with auxv for now. If we end up needing it, it's a
	 * word past the first null pointer in envp.
	 */
	li	%r6, 0

	/* This appears to be set to &_start on PPC32. We don't care. */
	li	%r7, 0

	bl	main
	/* Exit via syscall. */
	b	host_exit

END(_start)
