/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2018 Matthew Macy
 * Copyright (c) 2020 Brandon Bergren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Common static routines shared by MMU implementations.
 */

/***************************************************
 * page management routines.
 ***************************************************/

CTASSERT(sizeof(struct pv_chunk) == PAGE_SIZE);

static __inline struct pv_chunk *
pv_to_chunk(pv_entry_t pv)
{

	return ((struct pv_chunk *)((uintptr_t)pv & ~(uintptr_t)PAGE_MASK));
}

#define PV_PMAP(pv) (pv_to_chunk(pv)->pc_pmap)

/* XXX Remove me after Book-E is adapted */
#if defined(NEEDS_PV_PMAP)

#if defined(__powerpc64__)
CTASSERT(_NPCM == 2);
CTASSERT(_NPCPV == 126);

#define PC_FREE0	0xfffffffffffffffful
#define PC_FREE1	0x3ffffffffffffffful
static const pc_bitmap_t pc_freemask[_NPCM] = { PC_FREE0, PC_FREE1 };

#else
CTASSERT(_NPCM == 8);
CTASSERT(_NPCPV == 252);

#define	PC_FREE0_6	0xfffffffful
#define	PC_FREE7	0x0ffffffful
static const pc_bitmap_t pc_freemask[_NPCM] = {
	PC_FREE0_6, PC_FREE0_6, PC_FREE0_6, PC_FREE0_6,
	PC_FREE0_6, PC_FREE0_6, PC_FREE0_6, PC_FREE7
};

#else
/* XXX END remove me */

#if defined(__powerpc64__)
CTASSERT(_NPCM == 3);
CTASSERT(_NPCPV == 168);

#define	PC_FREE0	0xfffffffffffffffful
#define	PC_FREE1	0x3ffffffffffffffful

static const uint64_t pc_freemask[_NPCM] = { PC_FREE0, PC_FREE1 };

#else
CTASSERT(_NPCM == 11);
CTASSERT(_NPCPV == 336);

#define	PC_FREE0_9	0xfffffffful
#define	PC_FREE10	0x0000fffful

static const uint32_t pc_freemask[_NPCM] = {
	PC_FREE0_9, PC_FREE0_9, PC_FREE0_9,
	PC_FREE0_9, PC_FREE0_9, PC_FREE0_9,
	PC_FREE0_9, PC_FREE0_9, PC_FREE0_9,
	PC_FREE0_9, PC_FREE10
};

#endif


#ifdef NUMA
#define	PMAP_MEMDOM	MAXMEMDOM
#else
#define	PMAP_MEMDOM	1
#endif


