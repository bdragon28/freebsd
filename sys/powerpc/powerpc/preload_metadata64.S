/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Brandon Bergren <bdragon@FreeBSD.org>
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
 *
 */
#include "assym.inc"

#include <machine/param.h>
#include <machine/asm.h>

/* XXX make sys/linker.h usable from assembly? */

#define MODINFO_END             0x0000          /* End of list */
#define MODINFO_NAME            0x0001          /* Name of module (string) */
#define MODINFO_TYPE            0x0002          /* Type of module (string) */
#define MODINFO_ADDR            0x0003          /* Loaded address */
#define MODINFO_SIZE            0x0004          /* Size of module */
#define MODINFO_EMPTY           0x0005          /* Has been deleted */
#define MODINFO_ARGS            0x0006          /* Parameters string */
#define MODINFO_METADATA        0x8000          /* Module-specfic */

#ifdef _CALL_ELF
.abiversion _CALL_ELF
#endif

#define _PRELOAD_STRING(type, data) .align 2; \
	.long type; \
	.long (1f - 0f); \
	0: .asciz data; 1:

/*
 * Use relocations to provide a reasonable approximation of a mdp when using
 * a generic ELF loader such as petitboot.
 *
 * This is needed so we can use the kernel linker early.
 */

	.data
	.align 2
/* uint32_t __preload_metadata[] */
GLOBAL(__preload_metadata)
_PRELOAD_STRING(MODINFO_NAME, "kernel");
_PRELOAD_STRING(MODINFO_TYPE, "elf kernel");

	.long MODINFO_ADDR
	

/*
	.long MODINFO_NAME
	.long (1f - 0f) 
0:
	.asciz "kernel"
1:
	.align 2
	.long MODINFO_TYPE
	.long (1f - 0f)
0:
	.asciz "elf kernel"
1:
	.align 2
	
*/
