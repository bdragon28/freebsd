/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Brandon Bergren
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
 * $FreeBSD$
 */

/*
 * Common parts of locore.
 */

#ifdef _CALL_ELF
.abiversion _CALL_ELF
#endif

/*
 * AIM PowerNV boot stub
 *
 * Entry point for bootloaders that do not fully implement ELF and start
 * at the beginning of the image (kexec, notably). In its own section so
 * that it ends up before any linker-generated call stubs and actually at
 * the beginning of the image. kexec on some systems also enters at
 * (start of image) + 0x60, so put a spin loop there.
 */
        .section ".text.kboot", "x", @progbits
kbootentry:
        b __start
. = kbootentry + 0x40   /* Magic address used in platform layer */
        .global smp_spin_sem
ap_kexec_spin_sem:
        .long   -1
. = kbootentry + 0x60   /* Entry point for kexec APs */
ap_kexec_start:         /* At 0x60 past start, copied to 0x60 by kexec */
        /* r3 set to CPU ID by kexec */

        /* Invalidate icache for low-memory copy and jump there */
        li      %r0,0x80
        dcbst   0,%r0
        sync
        icbi    0,%r0
        isync
        ba      0x80                    /* Absolute branch to next inst */

. = kbootentry + 0x80                   /* Aligned to cache line */
1:      or      31,31,31                /* yield */
        sync
        lwz     %r1,0x40(0)             /* Spin on ap_kexec_spin_sem */
        cmpw    %r1,%r3                 /* Until it equals our CPU ID */
        bne     1b
        
        /* Released */
        or      2,2,2                   /* unyield */

        /* Make sure that it will be software reset. Clear SRR1 */
        li      %r1,0
        mtsrr1  %r1
        ba      EXC_RST

	.text
	.align 12
.global b

ENTRY(get_spr)
