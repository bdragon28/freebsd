/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2004 Suleiman Souhlal
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ucontext.h>

__weak_reference(__makecontext, makecontext);

void _ctx_done(ucontext_t *ucp);
void _ctx_start(void);

void
_ctx_done(ucontext_t *ucp)
{
	if (ucp->uc_link == NULL)
		exit(0);
	else {
		/* invalidate context */
		ucp->uc_mcontext.mc_len = 0;

		setcontext((const ucontext_t *)ucp->uc_link);

		abort(); /* should never return from above call */
	}
}

void
__makecontext(ucontext_t *ucp, void (*start)(void), int argc, ...)
{
	mcontext_t *mc;
	char *sp;
	va_list ap;
	int i, regargs, stackargs;

	/* Sanity checks */
	if ((ucp == NULL) || (argc < 0)
	    || (ucp->uc_stack.ss_sp == NULL)
	    || (ucp->uc_stack.ss_size < MINSIGSTKSZ)) {
		/* invalidate context */
		ucp->uc_mcontext.mc_len = 0;
		return;
	}

	/*
	 * Since we are setting up a stack frame for an arbitrary function,
	 * we *must* allocate a Parameter Save Area on top of the normal
	 * stack frame header, as the callee may try and use it.
	 *
	 * The ELFv1 stack frame header is 6 dwords (48 bytes) and the
	 * ELFv2 stack frame header is 4 dwords (32 bytes).
	 *
	 * Immediately following this is the Parameter Save Area, which
	 * must be a minimum of 8 dwords when it exists.
	 */
	stackargs = (argc > 8) ? argc - 8 : 0;
#if !defined(_CALL_ELF) || _CALL_ELF == 1
	sp = (char *) ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size
	    - sizeof(uintptr_t)*(6 + 8 + stackargs);
#else
	sp = (char *) ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size
	    - sizeof(uintptr_t)*(4 + 8 + stackargs);
#endif
	/* Ensure stack alignment. */
	sp = (char *)((uintptr_t)sp & ~0x1f);

	mc = &ucp->uc_mcontext;

	/*
	 * Up to 8 register args. Assumes all args are 64-bit and
	 * integer only.
	 *
	 * makecontext() itself only defines the behavior for arguments
	 * of type int. As such, we do not need to use the full
	 * algo mandated by the ABI here, we can simply count in dwords.
	 */
	regargs = (argc > 8) ? 8 : argc;
	va_start(ap, argc);
	for (i = 0; i < regargs; i++)
		mc->mc_gpr[3 + i] = va_arg(ap, uint64_t);

	/*
	 * Overflow args go onto the stack
	 */
	if (argc > 8) {
		uint64_t *argp;

		/*
		 * While we don't need to provide a copy of the first 8
		 * params in the stack, we still need to reserve spill
		 * space for the callee to use. Skip an additional 8 dwords
		 * past the end of the header to land on the correct slot.
		 */
#if !defined(_CALL_ELF) || _CALL_ELF == 1
		argp = (uint64_t *)sp + 6 + 8;
#else
		argp = (uint64_t *)sp + 4 + 8;
#endif

		for (i = 0; i < stackargs; i++)
			*argp++ = va_arg(ap, uint64_t);
	}
	va_end(ap);

	/*
	 * Use caller-saved regs 14/15 to hold params that _ctx_start
	 * will use to invoke the user-supplied func
	 */
#if !defined(_CALL_ELF) || _CALL_ELF == 1
	/* Cast to ensure this is treated as a function descriptor. */
	mc->mc_srr0 = *(uintptr_t *)_ctx_start;
#else
	mc->mc_srr0 = (uintptr_t) _ctx_start;
#endif
	mc->mc_gpr[1] = (uintptr_t) sp;		/* new stack pointer */
	mc->mc_gpr[14] = (uintptr_t) start;	/* r14 <- start */
	mc->mc_gpr[15] = (uintptr_t) ucp;	/* r15 <- ucp */
}
