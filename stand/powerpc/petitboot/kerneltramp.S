/*
 * This is the analog to the kexec "purgatory" code
 *
 * The goal here is to call the actual kernel entry point with the arguments it
 * expects when kexec calls into it with no arguments. The value of the kernel
 * entry point and arguments r3-r7 are copied into the trampoline text (which
 * can be executed from any address) at bytes 8-32. kexec begins execution
 * of APs at 0x60 bytes past the entry point, executing in a copy relocated
 * to the absolute address 0x60. Here we implement a loop waiting on the release
 * of a lock by the kernel at 0x40.
 * 
 * $FreeBSD$
 */

#include <machine/asm.h>

        .globl  CNAME(kerneltramp),CNAME(szkerneltramp)
CNAME(kerneltramp):
	b	1f						/* 0x000 */
1:	bl	2f						/* 0x004 */

/*
 * MUST BE IN SYNC WITH:
 *  struct trampoline_data {
 *   uint32_t	kernel_entry;
 *   uint32_t	dtb;
 *   uint32_t	phys_mem_offset;
 *   uint32_t	of_entry;
 *   uint32_t	mdp;
 *   uint32_t	mdp_size;
 *  };
 */
	.long	0	/* kernel_entry */			/* 0x008 */
	.long	0	/* dtb */				/* 0x00c */
	.long	0	/* phys_mem_offset */			/* 0x010 */
	.long	0	/* of_entry */				/* 0x014 */
	.long	0	/* mdp */				/* 0x018 */
	.long	0	/* mdp_size */				/* 0x020 */

2:	mflr	%r8	/* trampoline_data */			/* 0x024 */
	mfmsr	%r10						/* 0x028 */
	andi.	%r10, %r10, 1	/* test MSR_LE */		/* 0x02c */
	bne	little_endian					/* 0x030 */

big_endian:
	lwz	%r3,4(%r8)					/* 0x034 */
	lwz	%r4,8(%r8)					/* 0x038 */
	b	1f						/* 0x03c */

. = kerneltramp + 0x40	/* AP spinlock */
	.long -1						/* 0x040 */

1:
	lwz	%r5,12(%r8)					/* 0x044 */
	lwz	%r6,16(%r8)					/* 0x048 */
	lwz	%r7,20(%r8)					/* 0x04c */

	lwz	%r10, 0(%r8)					/* 0x050 */
	mtctr	%r10						/* 0x054 */
4:	/* bctr	*/ nop						/* 0x058 */
	/*nop	*/ b 4b						/* 0x05c */

. = kerneltramp + 0x60	/* AP entry point */
	/* R3: My cpu id */

	/* Invalidate icache for low-memory copy and jump there */
	li	%r0,0x80					/* 0x060 */
	dcbst	0,%r0						/* 0x064 */
	sync							/* 0x068 */
	icbi	0,%r0						/* 0x06c */
	isync							/* 0x070 */
	ba	0x80	/* Absolute branch to next inst */	/* 0x074 */
	nop							/* 0x078 */
	nop							/* 0x07c */

. = kerneltramp + 0x80		/* Aligned to cache line */
1:	or	31,31,31	/* yield */			/* 0x080 */
	sync							/* 0x084 */
	lwz	%r1,0x40(0)	/* Spin on ap_kexec_spin_sem */	/* 0x088 */
	cmpw	%r1,%r3		/* Until it is our CPU ID */	/* 0x08c */
	bne	1b						/* 0x090 */

	/* Released */
	or	2,2,2		/* unyield */			/* 0x094 */

	/* Make sure that it will be software reset. Clear SRR1 */
	li	%r1,0						/* 0x098 */
	mtsrr1	%r1						/* 0x09c */
	ba	0x100		/* EXC_RST */			/* 0x0a0 */

/* We're starting in LE */
little_endian:

	/* Entries are BE, swap them during load. */
	li	%r10, 4						/* 0x0a4 */
	lwbrx	%r3, %r8, %r10					/* 0x0a8 */
	li	%r10, 8						/* 0x0ac */
	lwbrx	%r4, %r8, %r10					/* 0x0b0 */
	li	%r10, 12					/* 0x0b4 */
	lwbrx	%r5, %r8, %r10					/* 0x0b8 */
	li	%r10, 16					/* 0x0bc */
	lwbrx	%r6, %r8, %r10					/* 0x0c0 */
	li	%r10, 20					/* 0x0c4 */
	lwbrx	%r7, %r8, %r10					/* 0x0c8 */

	/* Clear MSR_LE flag to enter the BE world */
	mfmsr	%r10						/* 0x0cc */
	clrrdi	%r10, %r10, 1					/* 0x0d0 */
	mtsrr1	%r10						/* 0x0d4 */

	/* Entry is at 0(%r8) */
	li	%r10, 0						/* 0x0d8 */
	lwbrx	%r10, %r8, %r10					/* 0x0dc */
	mtsrr0	%r10						/* 0x0e0 */

	rfid							/* 0x0e4 */
	nop							/* 0x0e8 */
	nop							/* 0x0ec */
	nop							/* 0x0f0 */
	nop							/* 0x0f4 */
	nop							/* 0x0f8 */
	nop							/* 0x0fc */
endkerneltramp:

	.data
CNAME(szkerneltramp):
	.long endkerneltramp - CNAME(kerneltramp)
