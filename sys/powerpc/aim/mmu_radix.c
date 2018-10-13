/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2018 Matthew Macy
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


#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");


#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/bitstring.h>
#include <sys/queue.h>
#include <sys/cpuset.h>
#include <sys/endian.h>
#include <sys/kerneldump.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/syslog.h>
#include <sys/msgbuf.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vmem.h>
#include <sys/vmmeter.h>
#include <sys/smp.h>

#include <sys/kdb.h>

#include <dev/ofw/openfirm.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <vm/vm_pageout.h>
#include <vm/vm_phys.h>
#include <vm/vm_reserv.h>
#include <vm/uma.h>

#include <machine/_inttypes.h>
#include <machine/cpu.h>
#include <machine/platform.h>
#include <machine/frame.h>
#include <machine/md_var.h>
#include <machine/psl.h>
#include <machine/bat.h>
#include <machine/hid.h>
#include <machine/pte.h>
#include <machine/sr.h>
#include <machine/trap.h>
#include <machine/mmuvar.h>

#ifdef INVARIANTS
#include <vm/uma_dbg.h>
#endif

/*
 *
 */
static int nkpt = 64;
SYSCTL_INT(_machdep, OID_AUTO, nkpt, CTLFLAG_RD, &nkpt, 0,
    "Number of kernel page table pages allocated on bootup");


static SYSCTL_NODE(_vm, OID_AUTO, pmap, CTLFLAG_RD, 0, "VM/pmap parameters");

static int pg_ps_enabled = 1;
SYSCTL_INT(_vm_pmap, OID_AUTO, pg_ps_enabled, CTLFLAG_RDTUN | CTLFLAG_NOFETCH,
    &pg_ps_enabled, 0, "Are large page mappings enabled?");

static u_int64_t	KPTphys;	/* phys addr of kernel level 1 */

//static vm_paddr_t	KERNend;	/* phys addr of end of bootstrap data */

static vm_offset_t qframe = 0;
static struct mtx qframe_mtx;

#ifndef MMU_DIRECT
#include "mmu_oea64.h"
#include "mmu_if.h"
#include "moea64_if.h"

/*
 * Kernel MMU interface
 */
static void mmu_radix_advise(mmu_t, pmap_t, vm_offset_t, vm_offset_t, int);
static void mmu_radix_align_superpage(mmu_t, vm_object_t, vm_ooffset_t,
	vm_offset_t *, vm_size_t);
	
static void	mmu_radix_bootstrap(mmu_t mmup, 
		    vm_offset_t kernelstart, vm_offset_t kernelend);
static void mmu_radix_clear_modify(mmu_t, vm_page_t);
static int mmu_radix_change_attr(mmu_t, vm_offset_t, vm_size_t, vm_memattr_t);

static void mmu_radix_copy(mmu_t, pmap_t, pmap_t, vm_offset_t,
    vm_size_t, vm_offset_t);
static void mmu_radix_copy_page(mmu_t, vm_page_t, vm_page_t);
static void mmu_radix_copy_pages(mmu_t mmu, vm_page_t *ma, vm_offset_t a_offset,
    vm_page_t *mb, vm_offset_t b_offset, int xfersize);
static int mmu_radix_enter(mmu_t, pmap_t, vm_offset_t, vm_page_t, vm_prot_t,
    u_int flags, int8_t psind);
static void mmu_radix_enter_object(mmu_t, pmap_t, vm_offset_t, vm_offset_t, vm_page_t,
    vm_prot_t);
static void mmu_radix_enter_quick(mmu_t, pmap_t, vm_offset_t, vm_page_t, vm_prot_t);
static vm_paddr_t mmu_radix_extract(mmu_t, pmap_t, vm_offset_t);
static vm_page_t mmu_radix_extract_and_hold(mmu_t, pmap_t, vm_offset_t, vm_prot_t);
static void mmu_radix_growkernel(mmu_t, vm_offset_t);
static void mmu_radix_init(mmu_t);
static int mmu_radix_mincore(mmu_t, pmap_t, vm_offset_t, vm_paddr_t *);
static boolean_t mmu_radix_is_modified(mmu_t, vm_page_t);
static boolean_t mmu_radix_is_prefaultable(mmu_t, pmap_t, vm_offset_t);
static boolean_t mmu_radix_is_referenced(mmu_t, vm_page_t);
static void mmu_radix_kremove(mmu_t mmup, vm_offset_t va);
static int mmu_radix_ts_referenced(mmu_t, vm_page_t);
static vm_offset_t mmu_radix_map(mmu_t, vm_offset_t *, vm_paddr_t, vm_paddr_t, int);
static boolean_t mmu_radix_page_exists_quick(mmu_t, pmap_t, vm_page_t);
static void mmu_radix_page_init(mmu_t, vm_page_t);
static void mmu_radix_object_init_pt(mmu_t, pmap_t, vm_offset_t, vm_object_t,
	vm_pindex_t, vm_size_t);
static int mmu_radix_page_wired_mappings(mmu_t, vm_page_t);
static void mmu_radix_pinit(mmu_t, pmap_t);
static void mmu_radix_pinit0(mmu_t, pmap_t);
static void mmu_radix_protect(mmu_t, pmap_t, vm_offset_t, vm_offset_t, vm_prot_t);
static void mmu_radix_qenter(mmu_t, vm_offset_t, vm_page_t *, int);
static void mmu_radix_qremove(mmu_t, vm_offset_t, int);
static void mmu_radix_release(mmu_t, pmap_t);
static void mmu_radix_remove(mmu_t, pmap_t, vm_offset_t, vm_offset_t);
static void mmu_radix_remove_pages(mmu_t, pmap_t);
static void mmu_radix_remove_all(mmu_t, vm_page_t);
static void mmu_radix_remove_write(mmu_t, vm_page_t);
static void mmu_radix_unwire(mmu_t, pmap_t, vm_offset_t, vm_offset_t);
static void mmu_radix_zero_page(mmu_t, vm_page_t);
static void mmu_radix_zero_page_area(mmu_t, vm_page_t, int, int);
static void mmu_radix_activate(mmu_t, struct thread *);
static void mmu_radix_deactivate(mmu_t, struct thread *);
static void *mmu_radix_mapdev(mmu_t, vm_paddr_t, vm_size_t);
static void *mmu_radix_mapdev_attr(mmu_t, vm_paddr_t, vm_size_t, vm_memattr_t);
static void mmu_radix_unmapdev(mmu_t, vm_offset_t, vm_size_t);
static vm_paddr_t mmu_radix_kextract(mmu_t, vm_offset_t);
static void mmu_radix_page_set_memattr(mmu_t, vm_page_t m, vm_memattr_t ma);
static void mmu_radix_kenter_attr(mmu_t, vm_offset_t, vm_paddr_t, vm_memattr_t ma);
static void mmu_radix_kenter(mmu_t, vm_offset_t, vm_paddr_t);
static boolean_t mmu_radix_dev_direct_mapped(mmu_t, vm_paddr_t, vm_size_t);
static void mmu_radix_sync_icache(mmu_t, pmap_t, vm_offset_t, vm_size_t);
static void mmu_radix_dumpsys_map(mmu_t mmu, vm_paddr_t pa, size_t sz,
    void **va);
static void mmu_radix_scan_init(mmu_t mmu);
static vm_offset_t mmu_radix_quick_enter_page(mmu_t mmu, vm_page_t m);
static void mmu_radix_quick_remove_page(mmu_t mmu, vm_offset_t addr);
static int mmu_radix_map_user_ptr(mmu_t mmu, pmap_t pm,
    volatile const void *uaddr, void **kaddr, size_t ulen, size_t *klen);
static int mmu_radix_decode_kernel_ptr(mmu_t mmu, vm_offset_t addr,
    int *is_user, vm_offset_t *decoded_addr);

static mmu_method_t mmu_radix_methods[] = {
	MMUMETHOD(mmu_activate,		mmu_radix_activate),
	MMUMETHOD(mmu_advise,	mmu_radix_advise),
	MMUMETHOD(mmu_align_superpage,	mmu_radix_align_superpage),
	MMUMETHOD(mmu_bootstrap,	mmu_radix_bootstrap),
	MMUMETHOD(mmu_change_attr,	mmu_radix_change_attr),
	MMUMETHOD(mmu_clear_modify,	mmu_radix_clear_modify),
	MMUMETHOD(mmu_copy,	mmu_radix_copy),
	MMUMETHOD(mmu_copy_page,	mmu_radix_copy_page),
	MMUMETHOD(mmu_copy_pages,	mmu_radix_copy_pages),
	MMUMETHOD(mmu_deactivate,      	mmu_radix_deactivate),
	MMUMETHOD(mmu_enter,		mmu_radix_enter),
	MMUMETHOD(mmu_enter_object,	mmu_radix_enter_object),
	MMUMETHOD(mmu_enter_quick,	mmu_radix_enter_quick),
	MMUMETHOD(mmu_extract,		mmu_radix_extract),
	MMUMETHOD(mmu_extract_and_hold,	mmu_radix_extract_and_hold),
	MMUMETHOD(mmu_growkernel,		mmu_radix_growkernel),
	MMUMETHOD(mmu_init,		mmu_radix_init),
	MMUMETHOD(mmu_is_modified,	mmu_radix_is_modified),
	MMUMETHOD(mmu_is_prefaultable,	mmu_radix_is_prefaultable),
	MMUMETHOD(mmu_is_referenced,	mmu_radix_is_referenced),
	MMUMETHOD(mmu_kremove,	mmu_radix_kremove),
	MMUMETHOD(mmu_map,     		mmu_radix_map),
	MMUMETHOD(mmu_mincore,     		mmu_radix_mincore),
	MMUMETHOD(mmu_object_init_pt,     		mmu_radix_object_init_pt),
	MMUMETHOD(mmu_page_exists_quick,mmu_radix_page_exists_quick),
	MMUMETHOD(mmu_page_init,	mmu_radix_page_init),
	MMUMETHOD(mmu_page_wired_mappings,mmu_radix_page_wired_mappings),
	MMUMETHOD(mmu_pinit,		mmu_radix_pinit),
	MMUMETHOD(mmu_pinit0,		mmu_radix_pinit0),
	MMUMETHOD(mmu_protect,		mmu_radix_protect),
	MMUMETHOD(mmu_qenter,		mmu_radix_qenter),
	MMUMETHOD(mmu_qremove,		mmu_radix_qremove),
	MMUMETHOD(mmu_release,		mmu_radix_release),
	MMUMETHOD(mmu_remove,		mmu_radix_remove),
	MMUMETHOD(mmu_remove_pages,	mmu_radix_remove_pages),
	MMUMETHOD(mmu_remove_all,      	mmu_radix_remove_all),
	MMUMETHOD(mmu_remove_write,	mmu_radix_remove_write),
	MMUMETHOD(mmu_sync_icache,	mmu_radix_sync_icache),
	MMUMETHOD(mmu_ts_referenced,	mmu_radix_ts_referenced),
	MMUMETHOD(mmu_unwire,		mmu_radix_unwire),
	MMUMETHOD(mmu_zero_page,       	mmu_radix_zero_page),
	MMUMETHOD(mmu_zero_page_area,	mmu_radix_zero_page_area),
	MMUMETHOD(mmu_page_set_memattr,	mmu_radix_page_set_memattr),
	MMUMETHOD(mmu_quick_enter_page, mmu_radix_quick_enter_page),
	MMUMETHOD(mmu_quick_remove_page, mmu_radix_quick_remove_page),

	MMUMETHOD(mmu_mapdev,		mmu_radix_mapdev),
	MMUMETHOD(mmu_mapdev_attr,	mmu_radix_mapdev_attr),
	MMUMETHOD(mmu_unmapdev,		mmu_radix_unmapdev),
	MMUMETHOD(mmu_kextract,		mmu_radix_kextract),
	MMUMETHOD(mmu_kenter,		mmu_radix_kenter),
	MMUMETHOD(mmu_kenter_attr,	mmu_radix_kenter_attr),
	MMUMETHOD(mmu_dev_direct_mapped,mmu_radix_dev_direct_mapped),
	MMUMETHOD(mmu_scan_init,	mmu_radix_scan_init),
	MMUMETHOD(mmu_dumpsys_map,	mmu_radix_dumpsys_map),
	MMUMETHOD(mmu_map_user_ptr,	mmu_radix_map_user_ptr),
	MMUMETHOD(mmu_decode_kernel_ptr, mmu_radix_decode_kernel_ptr),
	{ 0, 0 }
};

MMU_DEF(mmu_radix, MMU_TYPE_RADIX, mmu_radix_methods, 0);

#define METHOD(m) mmu_radix_ ## m(mmu_t mmup, 
#define METHODVOID(m) mmu_radix_ ## m(mmu_t mmup)
#define DUMPMETHOD(m) mmu_radix_dumpsys_ ## m(mmu_t mmup,
#define DUMPMETHODVOID(m) mmu_radix_dumpsys_ ## m(mmu_t mmup)
#define VISIBILITY static
#else
#define METHOD(m) pmap_ ## m(
#define METHODVOID(m) pmap_ ## m(void)
#define DUMPMETHOD(m) dumpsys_ ## m(
#define DUMPMETHODVOID(m) dumpsys_ ## m(void)
#define VISIBILITY

struct pmap kernel_pmap_store;

#endif

static void mmu_radix_pmap_remove(pmap_t pmap, vm_offset_t start, vm_offset_t end);
static boolean_t pmap_demote_l3e_locked(pmap_t pmap, pml3_entry_t *l3e, vm_offset_t va,
	struct rwlock **lockp);
static boolean_t pmap_demote_l3e(pmap_t pmap, pml3_entry_t *pde, vm_offset_t va);
static int pmap_unuse_pt(pmap_t, vm_offset_t, pml3_entry_t, struct spglist *);
static int pmap_remove_l3e(pmap_t pmap, pml3_entry_t *pdq, vm_offset_t sva,
    struct spglist *free, struct rwlock **lockp);
static int pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t sva,
    pml3_entry_t ptepde, struct spglist *free, struct rwlock **lockp);
static vm_page_t pmap_remove_pt_page(pmap_t pmap, vm_offset_t va);
static void pmap_remove_page(pmap_t pmap, vm_offset_t va, pml3_entry_t *pde,
    struct spglist *free);
static bool	pmap_remove_ptes(pmap_t pmap, vm_offset_t sva, vm_offset_t eva,
		    pml3_entry_t *l3e, struct spglist *free,
		    struct rwlock **lockp);

static bool	pmap_pv_insert_l3e(pmap_t pmap, vm_offset_t va, pml3_entry_t l3e,
		    u_int flags, struct rwlock **lockp);
#if VM_NRESERVLEVEL > 0
static void	pmap_pv_promote_l3e(pmap_t pmap, vm_offset_t va, vm_paddr_t pa,
		    struct rwlock **lockp);
#endif
static void	pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va);
static int pmap_insert_pt_page(pmap_t pmap, vm_page_t mpte);
static vm_page_t pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va, vm_page_t m,
	vm_prot_t prot, vm_page_t mpte, struct rwlock **lockp);

static bool	pmap_enter_2mpage(pmap_t pmap, vm_offset_t va, vm_page_t m,
		    vm_prot_t prot, struct rwlock **lockp);
static int	pmap_enter_l3e(pmap_t pmap, vm_offset_t va, pml3_entry_t newpde,
		    u_int flags, vm_page_t m, struct rwlock **lockp);

static vm_page_t reclaim_pv_chunk(pmap_t locked_pmap, struct rwlock **lockp);
static void free_pv_chunk(struct pv_chunk *pc);
static vm_page_t _pmap_allocpte(pmap_t pmap, vm_pindex_t ptepindex, struct rwlock **lockp);
static vm_page_t pmap_allocl3e(pmap_t pmap, vm_offset_t va,
		struct rwlock **lockp);
#ifdef notyet
static vm_page_t pmap_allocpte(pmap_t pmap, vm_offset_t va,
		struct rwlock **lockp);
#endif
static void _pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m,
    struct spglist *free);
static boolean_t pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m, struct spglist *free);


/*
 * Internal flags for pmap_enter()'s helper functions.
 */
#define	PMAP_ENTER_NORECLAIM	0x1000000	/* Don't reclaim PV entries. */
#define	PMAP_ENTER_NOREPLACE	0x2000000	/* Don't replace mappings. */
#define PMAP_LOG_WIRE(m)  printf("%s:%d wire %p pa: %lx\n", __func__, __LINE__, (m), (m)->phys_addr)

#define	curpmap		PCPU_GET(curpmap)


#define UNIMPLEMENTED() panic("%s not implemented", __func__)

#define RIC_FLUSH_TLB 0
#define RIC_FLUSH_PWC 1
#define RIC_FLUSH_ALL 2

#define POWER9_TLB_SETS_RADIX	128	/* # sets in POWER9 TLB Radix mode */

#define PPC_INST_TLBIE			0x7c000264
#define PPC_INST_TLBIEL			0x7c000224
#define PPC_INST_SLBIA			0x7c0003e4

#define TLBIEL_INVAL_SEL_MASK	0xc00	/* invalidation selector */
#define  TLBIEL_INVAL_PAGE	0x000	/* invalidate a single page */
#define  TLBIEL_INVAL_SET_LPID	0x800	/* invalidate a set for current LPID */
#define  TLBIEL_INVAL_SET	0xc00	/* invalidate a set for all LPIDs */

#define ___PPC_RA(a)	(((a) & 0x1f) << 16)
#define ___PPC_RB(b)	(((b) & 0x1f) << 11)
#define ___PPC_RS(s)	(((s) & 0x1f) << 21)
#define ___PPC_RT(t)	___PPC_RS(t)
#define ___PPC_R(r)	(((r) & 0x1) << 16)
#define ___PPC_PRS(prs)	(((prs) & 0x1) << 17)
#define ___PPC_RIC(ric)	(((ric) & 0x3) << 18)

#define PPC_SLBIA(IH)	__XSTRING(.long PPC_INST_SLBIA | \
				       ((IH & 0x7) << 21))
#define	PPC_TLBIE_5(rb,rs,ric,prs,r)				\
	__XSTRING(.long PPC_INST_TLBIE |								\
			  ___PPC_RB(rb) | ___PPC_RS(rs) |						\
			  ___PPC_RIC(ric) | ___PPC_PRS(prs) |					\
			  ___PPC_R(r))

#define	PPC_TLBIEL(rb,rs,ric,prs,r) \
	 __XSTRING(.long PPC_INST_TLBIEL | \
			   ___PPC_RB(rb) | ___PPC_RS(rs) |			\
			   ___PPC_RIC(ric) | ___PPC_PRS(prs) |		\
			   ___PPC_R(r))

#define PPC_INVALIDATE_ERAT		PPC_SLBIA(7)

/* Number of supported PID bits */
static unsigned int isa3_pid_bits;

/* PID to start allocating from */
static unsigned int isa3_base_pid;

#define PROCTAB_SIZE_SHIFT	(isa3_pid_bits + 4)
#define PROCTAB_ENTRIES	(1ul << isa3_pid_bits)


/*
 * Map of physical memory regions.
 */
static struct	mem_region *regions;
static struct	mem_region *pregions;
static u_int	phys_avail_count;
static int	regions_sz, pregions_sz;
static struct pate *isa3_parttab;
static struct prte *isa3_proctab;

#define	RADIX_PGD_SIZE_SHIFT	16
#define RADIX_PGD_SIZE	(1UL << RADIX_PGD_SIZE_SHIFT)

#define	RADIX_PGD_INDEX_SHIFT	(RADIX_PGD_SIZE_SHIFT-3)
#define NL2EPG (PAGE_SIZE/sizeof(pml2_entry_t))
#define NL3EPG (PAGE_SIZE/sizeof(pml3_entry_t))

#define	NUPML1E		(RADIX_PGD_SIZE/sizeof(uint64_t))	/* number of userland PML1 pages */
#define	NUPDPE		(NUPML1E*NL2EPG)/* number of userland PDP pages */
#define	NUPDE		(NUPDPE*NL3EPG)	/* number of userland PD entries */

/* POWER9 only permits a 64k partition table size. */
#define	PARTTAB_SIZE_SHIFT	16
#define PARTTAB_SIZE	(1UL << PARTTAB_SIZE_SHIFT)

#define PARTTAB_HR		(1UL << 63) /* host uses radix */
#define PARTTAB_GR		(1UL << 63) /* guest uses radix must match host */

/* TLB flush actions. Used as argument to tlbiel_all() */
enum {
	TLB_INVAL_SCOPE_LPID = 0,	/* invalidate TLBs for current LPID */
	TLB_INVAL_SCOPE_GLOBAL = 1,	/* invalidate all TLBs */
};

#define	NPV_LIST_LOCKS	MAXCPU
static int pmap_initialized;
static vm_paddr_t proctab0pa;
static vm_paddr_t parttab_phys;

/*
 * Data for the pv entry allocation mechanism.
 * Updates to pv_invl_gen are protected by the pv_list_locks[]
 * elements, but reads are not.
 */
static TAILQ_HEAD(pch, pv_chunk) pv_chunks = TAILQ_HEAD_INITIALIZER(pv_chunks);
static struct mtx __exclusive_cache_line pv_chunks_mutex;
static struct rwlock __exclusive_cache_line pv_list_locks[NPV_LIST_LOCKS];
static struct md_page *pv_table;
static struct md_page pv_dummy;

#ifdef PV_STATS
#define PV_STAT(x)	do { x ; } while (0)
#else
#define PV_STAT(x)	do { } while (0)
#endif

#define	pa_index(pa)	((pa) >> PDRSHIFT)
#define	pa_to_pvh(pa)	(&pv_table[pa_index(pa)])

#define	PHYS_TO_PV_LIST_LOCK(pa)	\
			(&pv_list_locks[pa_index(pa) % NPV_LIST_LOCKS])

#define	CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, pa)	do {	\
	struct rwlock **_lockp = (lockp);		\
	struct rwlock *_new_lock;			\
							\
	_new_lock = PHYS_TO_PV_LIST_LOCK(pa);		\
	if (_new_lock != *_lockp) {			\
		if (*_lockp != NULL)			\
			rw_wunlock(*_lockp);		\
		*_lockp = _new_lock;			\
		rw_wlock(*_lockp);			\
	}						\
} while (0)

#define	CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m)	\
			CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, VM_PAGE_TO_PHYS(m))

#define	RELEASE_PV_LIST_LOCK(lockp)		do {	\
	struct rwlock **_lockp = (lockp);		\
							\
	if (*_lockp != NULL) {				\
		rw_wunlock(*_lockp);			\
		*_lockp = NULL;				\
	}						\
} while (0)

#define	VM_PAGE_TO_PV_LIST_LOCK(m)	\
			PHYS_TO_PV_LIST_LOCK(VM_PAGE_TO_PHYS(m))

/*
 * We support 52 bits, hence:
 * bits 52 - 31 = 21, 0b10101
 * RTS encoding details
 * bits 0 - 3 of rts -> bits 6 - 8 unsigned long
 * bits 4 - 5 of rts -> bits 62 - 63 of unsigned long
 */
#define RTS_SIZE ((0x2UL << 61) | (0x5UL << 5))


static int powernv_enabled = 1;


static inline void
tlbiel_radix_set_isa300(uint32_t set, uint32_t is,
	uint32_t pid, uint32_t ric, uint32_t prs)
{
	uint64_t rb;
	uint64_t rs;

	rb = PPC_BITLSHIFT_VAL(set, 51) | PPC_BITLSHIFT_VAL(is, 53);
	rs = PPC_BITLSHIFT_VAL((uint64_t)pid, 31);

	__asm __volatile(PPC_TLBIEL(%0, %1, %2, %3, 1)
		     : : "r"(rb), "r"(rs), "i"(ric), "i"(prs)
		     : "memory");
}

static void
tlbiel_flush_isa3(uint32_t num_sets, uint32_t is)
{
	uint32_t set;

	__asm __volatile("ptesync": : :"memory");

	/*
	 * Flush the first set of the TLB, and the entire Page Walk Cache
	 * and partition table entries. Then flush the remaining sets of the
	 * TLB.
	 */
	tlbiel_radix_set_isa300(0, is, 0, RIC_FLUSH_ALL, 0);
	for (set = 1; set < num_sets; set++)
		tlbiel_radix_set_isa300(set, is, 0, RIC_FLUSH_TLB, 0);

	/* Do the same for process scoped entries. */
	tlbiel_radix_set_isa300(0, is, 0, RIC_FLUSH_ALL, 1);
	for (set = 1; set < num_sets; set++)
		tlbiel_radix_set_isa300(set, is, 0, RIC_FLUSH_TLB, 1);

	__asm __volatile("ptesync": : :"memory");
}

static void
mmu_radix_tlbiel_flush(int scope)
{
	int is;

	MPASS(scope == TLB_INVAL_SCOPE_LPID ||
		  scope == TLB_INVAL_SCOPE_GLOBAL);
	is = scope + 2;

	tlbiel_flush_isa3(POWER9_TLB_SETS_RADIX, is);
	__asm __volatile(PPC_INVALIDATE_ERAT "; isync" : : :"memory");
}

static void
mmu_radix_init_amor(void)
{
	/*
	* In HV mode, we init AMOR (Authority Mask Override Register) so that
	* the hypervisor and guest can setup IAMR (Instruction Authority Mask
	* Register), enable key 0 and set it to 1.
	*
	* AMOR = 0b1100 .... 0000 (Mask for key 0 is 11)
	*/
	mtspr(SPR_AMOR, (3ul << 62));
}

static void
mmu_radix_init_iamr(void)
{
	/*
	 * Radix always uses key0 of the IAMR to determine if an access is
	 * allowed. We set bit 0 (IBM bit 1) of key0, to prevent instruction
	 * fetch.
	 */
	mtspr(SPR_IAMR, (1ul << 62));
}

static void
mmu_radix_pid_set(pmap_t pmap)
{
	mtspr(SPR_PID, pmap->pm_pid);
	isync();
}

/* Quick sort callout for comparing physical addresses. */
static int
pa_cmp(const void *a, const void *b)
{
	const vm_paddr_t *pa = a, *pb = b;

	if (*pa < *pb)
		return (-1);
	else if (*pa > *pb)
		return (1);
	else
		return (0);
}

#define L1_PAGE_SIZE_SHIFT 39
#define L1_PAGE_SIZE (1UL<<L1_PAGE_SIZE_SHIFT)
#define L1_PAGE_MASK (L1_PAGE_SIZE-1)

#define L2_PAGE_SIZE_SHIFT 30
#define L2_PAGE_SIZE (1UL<<L2_PAGE_SIZE_SHIFT)
#define L2_PAGE_MASK (L2_PAGE_SIZE-1)

#define L3_PAGE_SIZE_SHIFT 21
#define L3_PAGE_SIZE (1UL<<L3_PAGE_SIZE_SHIFT)
#define L3_PAGE_MASK (L3_PAGE_SIZE-1)

#define RPTE_SHIFT 9
#define NLS_MASK ((1UL<<5)-1)
#define RPTE_ENTRIES (1UL<<RPTE_SHIFT)
#define RPTE_MASK (RPTE_ENTRIES-1)

#define NLB_SHIFT 0
#define NLB_MASK (((1UL<<52)-1) << 8)

#define	pte_load_store(ptep, pte)	atomic_swap_long(ptep, pte)
#define	pte_load_clear(ptep)		atomic_swap_long(ptep, 0)	
#define	pte_store(ptep, pte) do {	   \
	*(u_long *)(ptep) = (u_long)(pte); \
} while (0)
#define	pde_store(ptep, pa) do {	   \
		*(u_long *)(ptep) = (u_long)(pa|RPTE_VALID|RPTE_SHIFT); \
} while (0)

#define	pte_clear(ptep)			pte_store(ptep, 0)

#define	PTESYNC()	__asm __volatile("ptesync");
#define	TLBSYNC()	__asm __volatile("tlbsync; ptesync");
#define	SYNC()		__asm __volatile("sync");
#define	EIEIO()		__asm __volatile("eieio");

#define PG_W	RPTE_WIRED
#define PG_V	RPTE_VALID
#define PG_MANAGED	RPTE_MANAGED
#define PG_PROMOTED	RPTE_PROMOTED
#define PG_M	RPTE_C
#define PG_A	RPTE_R
#define PG_X	RPTE_EAA_X
#define PG_RW	(RPTE_EAA_R| RPTE_EAA_W)
#define PG_PS	RPTE_LEAF
#define PG_PTE_CACHE RPTE_ATTR_MASK


#define	PMAP_PDE_SUPERPAGE	(1 << 8)	/* supports 2MB superpages */

/*
 * Promotion to a 2MB (PDE) page mapping requires that the corresponding 4KB
 * (PTE) page mappings have identical settings for the following fields:
 */
#define	PG_PTE_PROMOTE	(PG_X | PG_MANAGED | PG_W | PG_PTE_CACHE | \
	    PG_M | PG_A | RPTE_EAA_P | PG_RW | PG_V)


static __inline void
TLBIE(uint64_t vpn) {
	__asm __volatile("tlbie %0" :: "r"(vpn) : "memory");
	__asm __volatile("eieio; tlbsync; ptesync" ::: "memory");
}

static __inline int
cntlzd(uint64_t word)
{
	uint64_t result;
	__asm __volatile("cntlzd %0,%1" : "=r"(result) : "r"(word));

	return (int)result;
}

static __inline int
cnttzd(uint64_t word)
{
	uint64_t result;
	__asm __volatile("cnttzd %0,%1" : "=r"(result) : "r"(word));

	return (int)result;
}

static __inline void
pagezero(void *page)
{
	/* XXX optimize me */
	memset(page, 0, PAGE_SIZE);
}


#if 0
static __inline int
ffsl(uint64_t word)
{
	int lz;

	lz = cntlzd(word);
	if (lz == 64)
		return (0);
	return (lz+1);
}
#endif

static bool
pmap_not_in_di(void)
{

	return (curthread->td_md.md_invl_gen.gen == 0);
}

#define	PMAP_ASSERT_NOT_IN_DI() \
    KASSERT(pmap_not_in_di(), ("DI already started"))

static void
pmap_delayed_invl_started(void)
{
	/* XXX */
}

static void
pmap_delayed_invl_finished(void)
{
	/* XXX */
}

static void
pmap_delayed_invl_page(vm_page_t m)
{
	/* XXX */
}

/* Return various clipped indexes for a given VA */
static __inline vm_pindex_t
pmap_pte_index(vm_offset_t va)
{

	return ((va >> PAGE_SHIFT) & RPTE_MASK);
}

static int pmap_debug = 0;

static __inline vm_pindex_t
pmap_l3e_pindex(vm_offset_t va)
{
	if (pmap_debug)
		printf("l4idx: %lu\n", ((va & PG_FRAME) >> L3_PAGE_SIZE_SHIFT));
	return ((va & PG_FRAME) >> L3_PAGE_SIZE_SHIFT);
}

static __inline vm_pindex_t
pmap_pml3e_index(vm_offset_t va)
{
	if (pmap_debug)
		printf("l3idx: %lu\n", ((va >> L3_PAGE_SIZE_SHIFT) & RPTE_MASK));
	return ((va >> L3_PAGE_SIZE_SHIFT) & RPTE_MASK);
}

static __inline vm_pindex_t
pmap_pml2e_index(vm_offset_t va)
{
	if (pmap_debug)
		printf("l2idx: %lu\n", ((va >> L2_PAGE_SIZE_SHIFT) & RPTE_MASK));
	return ((va >> L2_PAGE_SIZE_SHIFT) & RPTE_MASK);
}

static __inline vm_pindex_t
pmap_pml1e_index(vm_offset_t va)
{
	if (pmap_debug)
		printf("l1idx(%lx): %lu\n", va, ((va & PG_FRAME) >> L1_PAGE_SIZE_SHIFT));
	return ((va & PG_FRAME) >> L1_PAGE_SIZE_SHIFT);
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_l3e_to_pte(pt_entry_t *l3e, vm_offset_t va)
{
	pt_entry_t *pte;
	vm_paddr_t ptepa;

	ptepa = (*l3e & NLB_MASK);
	pte = (pt_entry_t *)PHYS_TO_DMAP(ptepa);
	return (&pte[pmap_pte_index(va)]);
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_l2e_to_l3e(pt_entry_t *l2e, vm_offset_t va)
{
	pt_entry_t *l3e;
	vm_paddr_t l3pa;

	l3pa = (*l2e & NLB_MASK);
	l3e = (pml3_entry_t *)PHYS_TO_DMAP(l3pa);
	return (&l3e[pmap_pml3e_index(va)]);
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_l1e_to_l2e(pt_entry_t *l1e, vm_offset_t va)
{
	pt_entry_t *l2e;
	vm_paddr_t l2pa;

	l2pa = (*l1e & NLB_MASK);
	
	l2e = (pml2_entry_t *)PHYS_TO_DMAP(l2pa);
	return (&l2e[pmap_pml2e_index(va)]);
}

static __inline pml1_entry_t *
pmap_pml1e(pmap_t pmap, vm_offset_t va)
{

	return (&pmap->pm_pml1[pmap_pml1e_index(va)]);
}

static pt_entry_t *
pmap_pml2e(pmap_t pmap, vm_offset_t va)
{
	pt_entry_t *l1e;

	l1e = pmap_pml1e(pmap, va);
	if ((*l1e & RPTE_VALID) == 0)
		return (NULL);
	return (pmap_l1e_to_l2e(l1e, va));
}

static pt_entry_t *
pmap_pml3e(pmap_t pmap, vm_offset_t va)
{
	pt_entry_t *l2e;

	l2e = pmap_pml2e(pmap, va);
	if ((*l2e & RPTE_VALID) == 0)
		return (NULL);
	return (pmap_l2e_to_l3e(l2e, va));
}

static __inline void
pmap_resident_count_inc(pmap_t pmap, int count)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	pmap->pm_stats.resident_count += count;
}

static __inline void
pmap_resident_count_dec(pmap_t pmap, int count)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT(pmap->pm_stats.resident_count >= count,
	    ("pmap %p resident count underflow %ld %d", pmap,
	    pmap->pm_stats.resident_count, count));
	pmap->pm_stats.resident_count -= count;
}

static uint64_t
allocpages(vm_paddr_t *firstaddr, int n)
{
	u_int64_t ret;

	ret = *firstaddr;
	bzero((void *)PHYS_TO_DMAP(ret), n * PAGE_SIZE);
	*firstaddr += n * PAGE_SIZE;
	return (ret);
}

static pt_entry_t *
vtopte(vm_offset_t va)
{
	pt_entry_t *l3e;

	l3e = pmap_pml3e(curpmap, va);
	if ((*l3e & RPTE_VALID) == 0)
		return (NULL);
	return (pmap_l3e_to_pte(l3e, va));
}

static pt_entry_t *
kvtopte(vm_offset_t va)
{
	pt_entry_t *l3e;

	l3e = pmap_pml3e(kernel_pmap, va);
	if ((*l3e & RPTE_VALID) == 0)
		return (NULL);
	return (pmap_l3e_to_pte(l3e, va));
}

static __inline void
mmu_radix_pmap_kenter(vm_offset_t va, vm_paddr_t pa)
{
	pt_entry_t *pte;

	pte = kvtopte(va);
	MPASS(pte != NULL);
	*pte = pa | RPTE_VALID | RPTE_LEAF | RPTE_EAA_R | RPTE_EAA_W | RPTE_EAA_P | PG_M | PG_A;
}

/*
 * Determine the appropriate bits to set in a PTE or PDE for a specified
 * caching mode.
 */

#define pmap_cache_bits(pmap, mode, is_pde) (0)

static void
pmap_invalidate_page(pmap_t pmap, vm_offset_t start)
{
	TLBIE(start);
}

static void
pmap_invalidate_range(pmap_t pmap, vm_offset_t start, vm_offset_t end)
{

	while (start != end) {
		TLBIE(start);
		start += PAGE_SIZE;
	}
}

static void
pmap_invalidate_all(pmap_t pmap)
{
	if (pmap == kernel_pmap)
		mmu_radix_tlbiel_flush(TLB_INVAL_SCOPE_GLOBAL);
	else
		mmu_radix_tlbiel_flush(TLB_INVAL_SCOPE_LPID);
}

static void
pmap_invalidate_l3e_page(pmap_t pmap, vm_offset_t va, pml3_entry_t l3e)
{

	/*
	 * When the PDE has PG_PROMOTED set, the 2MB page mapping was created
	 * by a promotion that did not invalidate the 512 4KB page mappings
	 * that might exist in the TLB.  Consequently, at this point, the TLB
	 * may hold both 4KB and 2MB page mappings for the address range [va,
	 * va + L3_PAGE_SIZE).  Therefore, the entire range must be invalidated here.
	 * In contrast, when PG_PROMOTED is clear, the TLB will not hold any
	 * 4KB page mappings for the address range [va, va + L3_PAGE_SIZE), and so a
	 * single INVLPG suffices to invalidate the 2MB page mapping from the
	 * TLB.
	 */
	if ((l3e & PG_PROMOTED) != 0)
		pmap_invalidate_range(pmap, va, va + L3_PAGE_SIZE - 1);
	else
		pmap_invalidate_page(pmap, va);
}

static __inline struct pv_chunk *
pv_to_chunk(pv_entry_t pv)
{

	return ((struct pv_chunk *)((uintptr_t)pv & ~(uintptr_t)PAGE_MASK));
}

#define PV_PMAP(pv) (pv_to_chunk(pv)->pc_pmap)

#define	PC_FREE0	0xfffffffffffffffful
#define	PC_FREE1	0xfffffffffffffffful
#define	PC_FREE2	0x000000fffffffffful

static const uint64_t pc_freemask[_NPCM] = { PC_FREE0, PC_FREE1, PC_FREE2 };

/*
 * Ensure that the number of spare PV entries in the specified pmap meets or
 * exceeds the given count, "needed".
 *
 * The given PV list lock may be released.
 */
static void
reserve_pv_entries(pmap_t pmap, int needed, struct rwlock **lockp)
{
	struct pch new_tail;
	struct pv_chunk *pc;
	vm_page_t m;
	int avail, free;
	bool reclaimed;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT(lockp != NULL, ("reserve_pv_entries: lockp is NULL"));

	/*
	 * Newly allocated PV chunks must be stored in a private list until
	 * the required number of PV chunks have been allocated.  Otherwise,
	 * reclaim_pv_chunk() could recycle one of these chunks.  In
	 * contrast, these chunks must be added to the pmap upon allocation.
	 */
	TAILQ_INIT(&new_tail);
retry:
	avail = 0;
	TAILQ_FOREACH(pc, &pmap->pm_pvchunk, pc_list) {
		//		if ((cpu_feature2 & CPUID2_POPCNT) == 0)
		bit_count((bitstr_t *)pc->pc_map, 0,
				  sizeof(pc->pc_map) * NBBY, &free);
#if 0
		free = popcnt_pc_map_pq(pc->pc_map);
#endif		
		if (free == 0)
			break;
		avail += free;
		if (avail >= needed)
			break;
	}
	for (reclaimed = false; avail < needed; avail += _NPCPV) {
		m = vm_page_alloc(NULL, 0, VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ |
		    VM_ALLOC_WIRED);
		if (m == NULL) {
			m = reclaim_pv_chunk(pmap, lockp);
			if (m == NULL)
				goto retry;
			reclaimed = true;
		}
		PV_STAT(atomic_add_int(&pc_chunk_count, 1));
		PV_STAT(atomic_add_int(&pc_chunk_allocs, 1));
		dump_add_page(m->phys_addr);
		pc = (void *)PHYS_TO_DMAP(m->phys_addr);
		pc->pc_pmap = pmap;
		pc->pc_map[0] = PC_FREE0;
		pc->pc_map[1] = PC_FREE1;
		pc->pc_map[2] = PC_FREE2;
		TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		TAILQ_INSERT_TAIL(&new_tail, pc, pc_lru);
		PV_STAT(atomic_add_int(&pv_entry_spare, _NPCPV));

		/*
		 * The reclaim might have freed a chunk from the current pmap.
		 * If that chunk contained available entries, we need to
		 * re-count the number of available entries.
		 */
		if (reclaimed)
			goto retry;
	}
	if (!TAILQ_EMPTY(&new_tail)) {
		mtx_lock(&pv_chunks_mutex);
		TAILQ_CONCAT(&pv_chunks, &new_tail, pc_lru);
		mtx_unlock(&pv_chunks_mutex);
	}
}

/*
 * First find and then remove the pv entry for the specified pmap and virtual
 * address from the specified pv list.  Returns the pv entry if found and NULL
 * otherwise.  This operation can be performed on pv lists for either 4KB or
 * 2MB page mappings.
 */
static __inline pv_entry_t
pmap_pvh_remove(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	TAILQ_FOREACH(pv, &pvh->pv_list, pv_next) {
		if (pmap == PV_PMAP(pv) && va == pv->pv_va) {
			TAILQ_REMOVE(&pvh->pv_list, pv, pv_next);
			pvh->pv_gen++;
			break;
		}
	}
	return (pv);
}

/*
 * After demotion from a 2MB page mapping to 512 4KB page mappings,
 * destroy the pv entry for the 2MB page mapping and reinstantiate the pv
 * entries for each of the 4KB page mappings.
 */
static void
pmap_pv_demote_l3e(pmap_t pmap, vm_offset_t va, vm_paddr_t pa,
    struct rwlock **lockp)
{
	struct md_page *pvh;
	struct pv_chunk *pc;
	pv_entry_t pv;
	vm_offset_t va_last;
	vm_page_t m;
	int bit, field;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((pa & L3_PAGE_MASK) == 0,
	    ("pmap_pv_demote_pde: pa is not 2mpage aligned"));
	CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, pa);

	/*
	 * Transfer the 2mpage's pv entry for this mapping to the first
	 * page's pv list.  Once this transfer begins, the pv list lock
	 * must not be released until the last pv entry is reinstantiated.
	 */
	pvh = pa_to_pvh(pa);
	va = trunc_2mpage(va);
	pv = pmap_pvh_remove(pvh, pmap, va);
	KASSERT(pv != NULL, ("pmap_pv_demote_pde: pv not found"));
	m = PHYS_TO_VM_PAGE(pa);
	TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
	m->md.pv_gen++;
	/* Instantiate the remaining NPTEPG - 1 pv entries. */
	PV_STAT(atomic_add_long(&pv_entry_allocs, NPTEPG - 1));
	va_last = va + L3_PAGE_SIZE - PAGE_SIZE;
	for (;;) {
		pc = TAILQ_FIRST(&pmap->pm_pvchunk);
		KASSERT(pc->pc_map[0] != 0 || pc->pc_map[1] != 0 ||
		    pc->pc_map[2] != 0, ("pmap_pv_demote_pde: missing spare"));
		for (field = 0; field < _NPCM; field++) {
			while (pc->pc_map[field]) {
				bit = cntlzd(pc->pc_map[field]);
				pc->pc_map[field] &= ~(1ul << bit);
				pv = &pc->pc_pventry[field * 64 + bit];
				va += PAGE_SIZE;
				pv->pv_va = va;
				m++;
				KASSERT((m->oflags & VPO_UNMANAGED) == 0,
			    ("pmap_pv_demote_pde: page %p is not managed", m));
				TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
				m->md.pv_gen++;
				if (va == va_last)
					goto out;
			}
		}
		TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
		TAILQ_INSERT_TAIL(&pmap->pm_pvchunk, pc, pc_list);
	}
out:
	if (pc->pc_map[0] == 0 && pc->pc_map[1] == 0 && pc->pc_map[2] == 0) {
		TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
		TAILQ_INSERT_TAIL(&pmap->pm_pvchunk, pc, pc_list);
	}
	PV_STAT(atomic_add_long(&pv_entry_count, NPTEPG - 1));
	PV_STAT(atomic_subtract_int(&pv_entry_spare, NPTEPG - 1));
}

static void
reclaim_pv_chunk_leave_pmap(pmap_t pmap, pmap_t locked_pmap, bool start_di)
{

	if (pmap == NULL)
		return;
	pmap_invalidate_all(pmap);
	if (pmap != locked_pmap)
		PMAP_UNLOCK(pmap);
	if (start_di)
		pmap_delayed_invl_finished();
}

/*
 * We are in a serious low memory condition.  Resort to
 * drastic measures to free some pages so we can allocate
 * another pv entry chunk.
 *
 * Returns NULL if PV entries were reclaimed from the specified pmap.
 *
 * We do not, however, unmap 2mpages because subsequent accesses will
 * allocate per-page pv entries until repromotion occurs, thereby
 * exacerbating the shortage of free pv entries.
 */
static vm_page_t
reclaim_pv_chunk(pmap_t locked_pmap, struct rwlock **lockp)
{
	struct pv_chunk *pc, *pc_marker, *pc_marker_end;
	struct pv_chunk_header pc_marker_b, pc_marker_end_b;
	struct md_page *pvh;
	pml3_entry_t *pde;
	pmap_t next_pmap, pmap;
	pt_entry_t *pte, tpte;
	pv_entry_t pv;
	vm_offset_t va;
	vm_page_t m, m_pc;
	struct spglist free;
	uint64_t inuse;
	int bit, field, freed;
	bool start_di;
	static int active_reclaims = 0;

	PMAP_LOCK_ASSERT(locked_pmap, MA_OWNED);
	KASSERT(lockp != NULL, ("reclaim_pv_chunk: lockp is NULL"));
	pmap = NULL;
	m_pc = NULL;
	SLIST_INIT(&free);
	bzero(&pc_marker_b, sizeof(pc_marker_b));
	bzero(&pc_marker_end_b, sizeof(pc_marker_end_b));
	pc_marker = (struct pv_chunk *)&pc_marker_b;
	pc_marker_end = (struct pv_chunk *)&pc_marker_end_b;

	/*
	 * A delayed invalidation block should already be active if
	 * pmap_advise() or pmap_remove() called this function by way
	 * of pmap_demote_l3e_locked().
	 */
	start_di = pmap_not_in_di();

	mtx_lock(&pv_chunks_mutex);
	active_reclaims++;
	TAILQ_INSERT_HEAD(&pv_chunks, pc_marker, pc_lru);
	TAILQ_INSERT_TAIL(&pv_chunks, pc_marker_end, pc_lru);
	while ((pc = TAILQ_NEXT(pc_marker, pc_lru)) != pc_marker_end &&
	    SLIST_EMPTY(&free)) {
		next_pmap = pc->pc_pmap;
		if (next_pmap == NULL) {
			/*
			 * The next chunk is a marker.  However, it is
			 * not our marker, so active_reclaims must be
			 * > 1.  Consequently, the next_chunk code
			 * will not rotate the pv_chunks list.
			 */
			goto next_chunk;
		}
		mtx_unlock(&pv_chunks_mutex);

		/*
		 * A pv_chunk can only be removed from the pc_lru list
		 * when both pc_chunks_mutex is owned and the
		 * corresponding pmap is locked.
		 */
		if (pmap != next_pmap) {
			reclaim_pv_chunk_leave_pmap(pmap, locked_pmap,
			    start_di);
			pmap = next_pmap;
			/* Avoid deadlock and lock recursion. */
			if (pmap > locked_pmap) {
				RELEASE_PV_LIST_LOCK(lockp);
				PMAP_LOCK(pmap);
				if (start_di)
					pmap_delayed_invl_started();
				mtx_lock(&pv_chunks_mutex);
				continue;
			} else if (pmap != locked_pmap) {
				if (PMAP_TRYLOCK(pmap)) {
					if (start_di)
						pmap_delayed_invl_started();
					mtx_lock(&pv_chunks_mutex);
					continue;
				} else {
					pmap = NULL; /* pmap is not locked */
					mtx_lock(&pv_chunks_mutex);
					pc = TAILQ_NEXT(pc_marker, pc_lru);
					if (pc == NULL ||
					    pc->pc_pmap != next_pmap)
						continue;
					goto next_chunk;
				}
			} else if (start_di)
				pmap_delayed_invl_started();
		}

		/*
		 * Destroy every non-wired, 4 KB page mapping in the chunk.
		 */
		freed = 0;
		for (field = 0; field < _NPCM; field++) {
			for (inuse = ~pc->pc_map[field] & pc_freemask[field];
			    inuse != 0; inuse &= ~(1UL << bit)) {
				bit = cntlzd(inuse);
				pv = &pc->pc_pventry[field * 64 + bit];
				va = pv->pv_va;
				pde = pmap_pml3e(pmap, va);
				if ((*pde & PG_PS) != 0)
					continue;
				pte = pmap_l3e_to_pte(pde, va);
				if ((*pte & PG_W) != 0)
					continue;
				tpte = pte_load_clear(pte);
				m = PHYS_TO_VM_PAGE(tpte & PG_FRAME);
				if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
					vm_page_dirty(m);
				if ((tpte & PG_A) != 0)
					vm_page_aflag_set(m, PGA_REFERENCED);
				CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
				TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
				m->md.pv_gen++;
				if (TAILQ_EMPTY(&m->md.pv_list) &&
				    (m->flags & PG_FICTITIOUS) == 0) {
					pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
					if (TAILQ_EMPTY(&pvh->pv_list)) {
						vm_page_aflag_clear(m,
						    PGA_WRITEABLE);
					}
				}
				pmap_delayed_invl_page(m);
				pc->pc_map[field] |= 1UL << bit;
				pmap_unuse_pt(pmap, va, *pde, &free);
				freed++;
			}
		}
		if (freed == 0) {
			mtx_lock(&pv_chunks_mutex);
			goto next_chunk;
		}
		/* Every freed mapping is for a 4 KB page. */
		pmap_resident_count_dec(pmap, freed);
		PV_STAT(atomic_add_long(&pv_entry_frees, freed));
		PV_STAT(atomic_add_int(&pv_entry_spare, freed));
		PV_STAT(atomic_subtract_long(&pv_entry_count, freed));
		TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
		if (pc->pc_map[0] == PC_FREE0 && pc->pc_map[1] == PC_FREE1 &&
		    pc->pc_map[2] == PC_FREE2) {
			PV_STAT(atomic_subtract_int(&pv_entry_spare, _NPCPV));
			PV_STAT(atomic_subtract_int(&pc_chunk_count, 1));
			PV_STAT(atomic_add_int(&pc_chunk_frees, 1));
			/* Entire chunk is free; return it. */
			m_pc = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
			dump_drop_page(m_pc->phys_addr);
			mtx_lock(&pv_chunks_mutex);
			TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
			break;
		}
		TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		mtx_lock(&pv_chunks_mutex);
		/* One freed pv entry in locked_pmap is sufficient. */
		if (pmap == locked_pmap)
			break;
next_chunk:
		TAILQ_REMOVE(&pv_chunks, pc_marker, pc_lru);
		TAILQ_INSERT_AFTER(&pv_chunks, pc, pc_marker, pc_lru);
		if (active_reclaims == 1 && pmap != NULL) {
			/*
			 * Rotate the pv chunks list so that we do not
			 * scan the same pv chunks that could not be
			 * freed (because they contained a wired
			 * and/or superpage mapping) on every
			 * invocation of reclaim_pv_chunk().
			 */
			while ((pc = TAILQ_FIRST(&pv_chunks)) != pc_marker) {
				MPASS(pc->pc_pmap != NULL);
				TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
				TAILQ_INSERT_TAIL(&pv_chunks, pc, pc_lru);
			}
		}
	}
	TAILQ_REMOVE(&pv_chunks, pc_marker, pc_lru);
	TAILQ_REMOVE(&pv_chunks, pc_marker_end, pc_lru);
	active_reclaims--;
	mtx_unlock(&pv_chunks_mutex);
	reclaim_pv_chunk_leave_pmap(pmap, locked_pmap, start_di);
	if (m_pc == NULL && !SLIST_EMPTY(&free)) {
		m_pc = SLIST_FIRST(&free);
		SLIST_REMOVE_HEAD(&free, plinks.s.ss);
		/* Recycle a freed page table page. */
		m_pc->wire_count = 1;
	}
	vm_page_free_pages_toq(&free, true);
	return (m_pc);
}

/*
 * free the pv_entry back to the free list
 */
static void
free_pv_entry(pmap_t pmap, pv_entry_t pv)
{
	struct pv_chunk *pc;
	int idx, field, bit;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PV_STAT(atomic_add_long(&pv_entry_frees, 1));
	PV_STAT(atomic_add_int(&pv_entry_spare, 1));
	PV_STAT(atomic_subtract_long(&pv_entry_count, 1));
	pc = pv_to_chunk(pv);
	idx = pv - &pc->pc_pventry[0];
	field = idx / 64;
	bit = idx % 64;
	pc->pc_map[field] |= 1ul << bit;
	if (pc->pc_map[0] != PC_FREE0 || pc->pc_map[1] != PC_FREE1 ||
	    pc->pc_map[2] != PC_FREE2) {
		/* 98% of the time, pc is already at the head of the list. */
		if (__predict_false(pc != TAILQ_FIRST(&pmap->pm_pvchunk))) {
			TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
			TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		}
		return;
	}
	TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
	free_pv_chunk(pc);
}

static void
free_pv_chunk(struct pv_chunk *pc)
{
	vm_page_t m;

	mtx_lock(&pv_chunks_mutex);
 	TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	PV_STAT(atomic_subtract_int(&pv_entry_spare, _NPCPV));
	PV_STAT(atomic_subtract_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_frees, 1));
	/* entire chunk is free, return it */
	m = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
	dump_drop_page(m->phys_addr);
	vm_page_unwire(m, PQ_NONE);
	vm_page_free(m);
}

/*
 * Returns a new PV entry, allocating a new PV chunk from the system when
 * needed.  If this PV chunk allocation fails and a PV list lock pointer was
 * given, a PV chunk is reclaimed from an arbitrary pmap.  Otherwise, NULL is
 * returned.
 *
 * The given PV list lock may be released.
 */
static pv_entry_t
get_pv_entry(pmap_t pmap, struct rwlock **lockp)
{
	int bit, field;
	pv_entry_t pv;
	struct pv_chunk *pc;
	vm_page_t m;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PV_STAT(atomic_add_long(&pv_entry_allocs, 1));
retry:
	pc = TAILQ_FIRST(&pmap->pm_pvchunk);
	if (pc != NULL) {
		for (field = 0; field < _NPCM; field++) {
			if (pc->pc_map[field]) {
				bit = cntlzd(pc->pc_map[field]);
				break;
			}
		}
		if (field < _NPCM) {
			pv = &pc->pc_pventry[field * 64 + bit];
			pc->pc_map[field] &= ~(1ul << bit);
			/* If this was the last item, move it to tail */
			if (pc->pc_map[0] == 0 && pc->pc_map[1] == 0 &&
			    pc->pc_map[2] == 0) {
				TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
				TAILQ_INSERT_TAIL(&pmap->pm_pvchunk, pc,
				    pc_list);
			}
			PV_STAT(atomic_add_long(&pv_entry_count, 1));
			PV_STAT(atomic_subtract_int(&pv_entry_spare, 1));
			return (pv);
		}
	}
	/* No free items, allocate another chunk */
	m = vm_page_alloc(NULL, 0, VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED);
	if (m == NULL) {
		if (lockp == NULL) {
			PV_STAT(pc_chunk_tryfail++);
			return (NULL);
		}
		m = reclaim_pv_chunk(pmap, lockp);
		if (m == NULL)
			goto retry;
	}
	PV_STAT(atomic_add_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_allocs, 1));
	dump_add_page(m->phys_addr);
	pc = (void *)PHYS_TO_DMAP(m->phys_addr);
	pc->pc_pmap = pmap;
	pc->pc_map[0] = PC_FREE0 & ~1ul;	/* preallocated bit 0 */
	pc->pc_map[1] = PC_FREE1;
	pc->pc_map[2] = PC_FREE2;
	mtx_lock(&pv_chunks_mutex);
	TAILQ_INSERT_TAIL(&pv_chunks, pc, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	pv = &pc->pc_pventry[0];
	TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
	PV_STAT(atomic_add_long(&pv_entry_count, 1));
	PV_STAT(atomic_add_int(&pv_entry_spare, _NPCPV - 1));
	return (pv);
}

#if VM_NRESERVLEVEL > 0
/*
 * After promotion from 512 4KB page mappings to a single 2MB page mapping,
 * replace the many pv entries for the 4KB page mappings by a single pv entry
 * for the 2MB page mapping.
 */
static void
pmap_pv_promote_l3e(pmap_t pmap, vm_offset_t va, vm_paddr_t pa,
    struct rwlock **lockp)
{
	struct md_page *pvh;
	pv_entry_t pv;
	vm_offset_t va_last;
	vm_page_t m;

	KASSERT((pa & L3_PAGE_MASK) == 0,
	    ("pmap_pv_promote_pde: pa is not 2mpage aligned"));
	CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, pa);

	/*
	 * Transfer the first page's pv entry for this mapping to the 2mpage's
	 * pv list.  Aside from avoiding the cost of a call to get_pv_entry(),
	 * a transfer avoids the possibility that get_pv_entry() calls
	 * reclaim_pv_chunk() and that reclaim_pv_chunk() removes one of the
	 * mappings that is being promoted.
	 */
	m = PHYS_TO_VM_PAGE(pa);
	va = trunc_2mpage(va);
	pv = pmap_pvh_remove(&m->md, pmap, va);
	KASSERT(pv != NULL, ("pmap_pv_promote_pde: pv not found"));
	pvh = pa_to_pvh(pa);
	TAILQ_INSERT_TAIL(&pvh->pv_list, pv, pv_next);
	pvh->pv_gen++;
	/* Free the remaining NPTEPG - 1 pv entries. */
	va_last = va + L2_PAGE_SIZE - PAGE_SIZE;
	do {
		m++;
		va += PAGE_SIZE;
		pmap_pvh_free(&m->md, pmap, va);
	} while (va < va_last);
}
#endif /* VM_NRESERVLEVEL > 0 */

/*
 * First find and then destroy the pv entry for the specified pmap and virtual
 * address.  This operation can be performed on pv lists for either 4KB or 2MB
 * page mappings.
 */
static void
pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	pv = pmap_pvh_remove(pvh, pmap, va);
	KASSERT(pv != NULL, ("pmap_pvh_free: pv not found"));
	free_pv_entry(pmap, pv);
}

/*
 * Conditionally create the PV entry for a 4KB page mapping if the required
 * memory can be allocated without resorting to reclamation.
 */
static boolean_t
pmap_try_insert_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m,
    struct rwlock **lockp)
{
	pv_entry_t pv;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	/* Pass NULL instead of the lock pointer to disable reclamation. */
	if ((pv = get_pv_entry(pmap, NULL)) != NULL) {
		pv->pv_va = va;
		CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
		m->md.pv_gen++;
		return (TRUE);
	} else
		return (FALSE);
}


#define DMAP_PAGE_BITS (RPTE_VALID | RPTE_LEAF | RPTE_EAA_MASK)

static vm_paddr_t
alloc_pt_page(void)
{
	vm_paddr_t page;

	page = moea64_bootstrap_alloc(PAGE_SIZE, PAGE_SIZE);
	pagezero((void*)PHYS_TO_DMAP(page));
	return (page);
}

static void
mmu_radix_dmap_range(vm_paddr_t start, vm_paddr_t end)
{
	pt_entry_t *pte, pteval;
	vm_paddr_t page;

	printf("dmap %lx -> %lx\n", start, end);
	while (start < end) {
		pteval = start | DMAP_PAGE_BITS;

		pte = pmap_pml2e(kernel_pmap, PHYS_TO_DMAP(start));
		if ((start & L2_PAGE_MASK) == 0 &&
			end - start >= L2_PAGE_SIZE) {
			start += L2_PAGE_SIZE;
			goto done;
		} else if ((*pte & RPTE_VALID) == 0) {
			page = alloc_pt_page();
			pde_store(pte, page);
		}

		pte = pmap_l2e_to_l3e(pte, PHYS_TO_DMAP(start));
		if ((start & L3_PAGE_MASK) == 0 &&
			end - start >= L3_PAGE_SIZE) {
			start += L3_PAGE_SIZE;
			goto done;
		} else if ((*pte & RPTE_VALID) == 0) {
			page = alloc_pt_page();
			pde_store(pte, page);
		}
		pte = pmap_l3e_to_pte(pte, PHYS_TO_DMAP(start));
		start += PAGE_SIZE;
	done:
		pte_store(pte, pteval);
	}
}

static void
mmu_radix_dmap_populate(vm_size_t hwphyssz)
{
	u_long l2pages;
	vm_paddr_t pages, start, end;

	l2pages = ((physmem << PAGE_SHIFT) + L1_PAGE_MASK) >> L1_PAGE_SIZE_SHIFT;
	pages = moea64_bootstrap_alloc(l2pages*PAGE_SIZE, PAGE_SIZE);
	printf("l2pages=%lu l2phys=%lx hwphyssz=%lx\n", l2pages, pages, hwphyssz);
	for (int i = 0; i < l2pages; i++, pages += PAGE_SIZE) {
		pagezero((void*)PHYS_TO_DMAP(pages));
		printf("pml1[%d]= %lx\n", i, pages);
		kernel_pmap->pm_pml1[i] = (pages | RPTE_VALID | RPTE_SHIFT);
	}
	for (int i = 0; i < pregions_sz; i++) {
		start = pregions[i].mr_start;
		end = start + pregions[i].mr_size;
		if (hwphyssz && start >= hwphyssz)
			break;
		if (hwphyssz && hwphyssz < end)
			end = hwphyssz;
		mmu_radix_dmap_range(start, end);
	}
}

static vm_paddr_t
mmu_radix_setup_pagetables(vm_size_t hwphyssz)
{
	vm_paddr_t ptpages, pages;
	pt_entry_t *pte;
	vm_paddr_t allocoff, l1phys;

	bzero(kernel_pmap, sizeof(struct pmap));
	PMAP_LOCK_INIT(kernel_pmap);
	CPU_FILL(&kernel_pmap->pm_active);

	ptpages = moea64_bootstrap_alloc(2*PAGE_SIZE, PAGE_SIZE);
	memset((void*)PHYS_TO_DMAP(ptpages), 0, 2*PAGE_SIZE);
	l1phys = moea64_bootstrap_alloc(RADIX_PGD_SIZE, RADIX_PGD_SIZE);
	printf("l1phys=%lx\n", l1phys);
	MPASS((l1phys & (RADIX_PGD_SIZE-1)) == 0);
	kernel_pmap->pm_pml1 = (pml1_entry_t *)PHYS_TO_DMAP(l1phys);
	memset(kernel_pmap->pm_pml1, 0, RADIX_PGD_SIZE);

	mmu_radix_dmap_populate(hwphyssz);

	/*
	 * Create page tables for first 128MB of KVA
	 */
	pages = ptpages;
	pte = pmap_pml1e(kernel_pmap, VM_MIN_KERNEL_ADDRESS);
	*pte = (pages | RPTE_VALID | RPTE_SHIFT);
	pages += PAGE_SIZE;
	pte = pmap_l1e_to_l2e(pte, VM_MIN_KERNEL_ADDRESS);
	*pte = (pages | RPTE_VALID | RPTE_SHIFT);
	pages += PAGE_SIZE;
	pte = pmap_l2e_to_l3e(pte, VM_MIN_KERNEL_ADDRESS);
	/*
	 * the kernel page table pages need to be preserved in
	 * phys_avail and not overlap with previous  allocations
	 */
	allocoff = 0;
	for (int j = 0; j < 2*phys_avail_count; j+=2)
		printf("phys_avail[%d]=%08lx - phys_avail[%d]=%08lx\n",
			   j, phys_avail[j], j+1, phys_avail[j+1]);

	for (int i = 0; phys_avail[i + 1] != 0; i += 2) {
		int64_t delta = (int64_t)(phys_avail[i+1] - phys_avail[i]);

		printf("delta = %ld\n", delta);
		if (delta > 128*PAGE_SIZE) {
			allocoff = phys_avail[i];
			break;
		}
	}
	MPASS(allocoff != 0);
	printf("allocoff=%lx\n", allocoff);
	
	pages = allocpages(&allocoff, nkpt);
	KPTphys = pages;
	for (int i = 0; i < nkpt; i++, pte++, pages += PAGE_SIZE)
		*pte = (pages | RPTE_VALID | RPTE_SHIFT);
	kernel_vm_end = VM_MIN_KERNEL_ADDRESS + nkpt*L3_PAGE_SIZE;
	printf("kernel_pmap pml1 %p\n", kernel_pmap->pm_pml1);
	return (allocoff);
}

static void
mmu_radix_ofw_mappings_check(void)
{
	ihandle_t	mmui;
	phandle_t	chosen;
	phandle_t	mmu;
	ssize_t		sz;

	/*
	 * Set up the Open Firmware pmap and add its mappings if not in real
	 * mode.
	 */

	printf("%s enter\n", __func__);
	chosen = OF_finddevice("/chosen");
	printf("chosen=%x\n", chosen);
	if (chosen != -1 && OF_getencprop(chosen, "mmu", &mmui, 4) != -1) {
		mmu = OF_instance_to_package(mmui);
		printf("mmu=%x\n", mmu);
		if (mmu == -1)
			sz = 0;
		else if ((sz = OF_getproplen(mmu, "translations")) == -1)
			sz = 0;
		printf("sz=%lu\n", sz);
		if (sz > 6144 /* tmpstksz - 2 KB headroom */)
			panic("moea64_bootstrap: too many ofw translations");
#ifdef notyet
		if (sz > 0)
			moea64_add_ofw_mappings(mmup, mmu, sz);
#else
		if (sz > 0)
			panic("too many: %lu ofw mappings\n", sz);
#endif
	}
}

static vm_paddr_t
mmu_radix_early_bootstrap(vm_offset_t start, vm_offset_t end)
{
	vm_paddr_t	kpstart, kpend;
	vm_size_t	physsz, hwphyssz;
	//uint64_t	l2virt;
	int		rm_pavail, proctab_size;
	int		i, j;

	kpstart = start & ~DMAP_BASE_ADDRESS;
	kpend = end & ~DMAP_BASE_ADDRESS;

	/* Get physical memory regions from firmware */
	mem_regions(&pregions, &pregions_sz, &regions, &regions_sz);
	CTR0(KTR_PMAP, "mmu_radix_early_bootstrap: physical memory");

	if (2 * VM_PHYSSEG_MAX < regions_sz)
		panic("mmu_radix_early_bootstrap: phys_avail too small");
	for (int i = 0; i < pregions_sz; i++)
		printf("pregions[%d].mr_start=%lx pregions[%d].mr_size=%lx\n",
			   i, pregions[i].mr_start, i, pregions[i].mr_size);

	phys_avail_count = 0;
	physsz = 0;
	hwphyssz = 0;
	TUNABLE_ULONG_FETCH("hw.physmem", (u_long *) &hwphyssz);
	for (i = 0, j = 0; i < regions_sz; i++) {

		if ((regions[i].mr_start & PAGE_MASK) ||
			regions[i].mr_size < PAGE_SIZE)
			continue;

		CTR3(KTR_PMAP, "region: %#zx - %#zx (%#zx)",
		    regions[i].mr_start, regions[i].mr_start +
		    regions[i].mr_size, regions[i].mr_size);
		if (hwphyssz != 0 &&
		    (physsz + regions[i].mr_size) >= hwphyssz) {
			if (physsz < hwphyssz) {
				phys_avail[j] = regions[i].mr_start;
				phys_avail[j + 1] = regions[i].mr_start +
				    hwphyssz - physsz;
				physsz = hwphyssz;
				phys_avail_count++;
			}
			break;
		}
		phys_avail[j] = regions[i].mr_start;
		phys_avail[j + 1] = regions[i].mr_start + regions[i].mr_size;
		phys_avail_count++;
		physsz += regions[i].mr_size;
		j += 2;
	}

	/* Check for overlap with the kernel and exception vectors */
	rm_pavail = 0;
	for (j = 0; j < 2*phys_avail_count; j+=2) {
		if (phys_avail[j] < EXC_LAST)
			phys_avail[j] += EXC_LAST;

		if (phys_avail[j] >= kpstart &&
		    phys_avail[j+1] <= kpend) {
			phys_avail[j] = phys_avail[j+1] = ~0;
			rm_pavail++;
			continue;
		}

		if (kpstart >= phys_avail[j] &&
		    kpstart < phys_avail[j+1]) {
			if (kpend < phys_avail[j+1]) {
				phys_avail[2*phys_avail_count] =
				    (kpend & ~PAGE_MASK) + PAGE_SIZE;
				phys_avail[2*phys_avail_count + 1] =
				    phys_avail[j+1];
				phys_avail_count++;
			}

			phys_avail[j+1] = kpstart & ~PAGE_MASK;
		}

		if (kpend >= phys_avail[j] &&
		    kpend < phys_avail[j+1]) {
			if (kpstart > phys_avail[j]) {
				phys_avail[2*phys_avail_count] = phys_avail[j];
				phys_avail[2*phys_avail_count + 1] =
				    kpstart & ~PAGE_MASK;
				phys_avail_count++;
			}

			phys_avail[j] = (kpend & ~PAGE_MASK) +
			    PAGE_SIZE;
		}
	}
	for (j = 0; j < 2*phys_avail_count; j+=2)
		printf("phys_avail[%d]=%08lx - phys_avail[%d]=%08lx\n",
			   j, phys_avail[j], j+1, phys_avail[j+1]);

	/* Remove physical available regions marked for removal (~0) */
	if (rm_pavail) {
		qsort(phys_avail, 2*phys_avail_count, sizeof(phys_avail[0]),
			pa_cmp);
		phys_avail_count -= rm_pavail;
		for (i = 2*phys_avail_count;
		     i < 2*(phys_avail_count + rm_pavail); i+=2)
			phys_avail[i] = phys_avail[i+1] = 0;
	}
	physmem = btoc(physsz);	

	/* XXX assume we're running non-virtualized and
	 * we don't support BHYVE
	 */
	if (isa3_pid_bits == 0)
		isa3_pid_bits = 20;
	parttab_phys = moea64_bootstrap_alloc(PARTTAB_SIZE, PARTTAB_SIZE);

	proctab_size = 1UL << PROCTAB_SIZE_SHIFT;
	proctab0pa = moea64_bootstrap_alloc(proctab_size, proctab_size);

	return (mmu_radix_setup_pagetables(hwphyssz));
}

static void
mmu_radix_late_bootstrap(vm_paddr_t allocoff, vm_offset_t start, vm_offset_t end)
{
	int		i;
	vm_paddr_t	pa;
	void		*dpcpu;
	vm_offset_t va;

	/*
	 * Set up the Open Firmware pmap and add its mappings if not in real
	 * mode.
	 */

	printf("%s enter\n", __func__);
	mmu_radix_ofw_mappings_check();

	/*
	 * Calculate the last available physical address.
	 */
	Maxmem = 0;
	for (i = 0; phys_avail[i + 2] != 0; i += 2)
		Maxmem = max(Maxmem, powerpc_btop(phys_avail[i + 1]));

	mtmsr(mfmsr() | PSL_DR | PSL_IR);
	/*
	 * Set the start and end of kva.
	 */
	virtual_avail = VM_MIN_KERNEL_ADDRESS;
	virtual_end = VM_MAX_SAFE_KERNEL_ADDRESS; 
#if 0
	/*
	 * Remap any early IO mappings (console framebuffer, etc.)
	 */
	bs_remap_earlyboot();
#endif
	/*
	 * Allocate a kernel stack with a guard page for thread0 and map it
	 * into the kernel page map.
	 */
	pa = allocpages(&allocoff, kstack_pages);
	va = virtual_avail + KSTACK_GUARD_PAGES * PAGE_SIZE;
	virtual_avail = va + kstack_pages * PAGE_SIZE;
	CTR2(KTR_PMAP, "moea64_bootstrap: kstack0 at %#x (%#x)", pa, va);
	thread0.td_kstack = va;
	for (i = 0; i < kstack_pages; i++) {
		mmu_radix_pmap_kenter(va, pa);
		pa += PAGE_SIZE;
		va += PAGE_SIZE;
	}
	thread0.td_kstack_pages = kstack_pages;
	printf("%s set kstack\n", __func__);

	/*
	 * Allocate virtual address space for the message buffer.
	 */
	pa = msgbuf_phys = allocpages(&allocoff, (msgbufsize + PAGE_MASK)  >> PAGE_SHIFT);
	msgbufp = (struct msgbuf *)PHYS_TO_DMAP(pa);
	printf("%s set msgbuf\n", __func__);
	/*
	 * Allocate virtual address space for the dynamic percpu area.
	 */
	pa = allocpages(&allocoff, (msgbufsize + PAGE_MASK) >> PAGE_SHIFT);
	dpcpu = (void *)PHYS_TO_DMAP(pa);
	dpcpu_init(dpcpu, curcpu);
}

static void
mmu_parttab_init(void)
{
	uint64_t ptcr;

	isa3_parttab = (struct pate *)PHYS_TO_DMAP(parttab_phys);

	memset(isa3_parttab, 0, PARTTAB_SIZE);
	printf("%s parttab: %p\n", __func__, isa3_parttab);
	ptcr = parttab_phys | (PARTTAB_SIZE_SHIFT-12);
	printf("setting ptcr %lx\n", ptcr);
	mtspr(SPR_PTCR, ptcr);
	printf("set ptcr\n");
#if 0
	/* functional simulator claims MCE on this */
	powernv_set_nmmu_ptcr(ptcr);
	printf("set nested mmu ptcr\n");
#endif	
}

static void
mmu_parttab_update(uint64_t lpid, uint64_t pagetab, uint64_t proctab)
{
	uint64_t prev;
	
	printf("%s isa3_parttab %p lpid %lx pagetab %lx proctab %lx\n", __func__, isa3_parttab,
		   lpid, pagetab, proctab);
	prev = be64toh(isa3_parttab[lpid].pagetab);
	isa3_parttab[lpid].pagetab = htobe64(pagetab);
	isa3_parttab[lpid].proctab = htobe64(proctab);

	if (prev & PARTTAB_HR) {
		__asm __volatile(PPC_TLBIE_5(%0,%1,2,0,1) : :
			     "r" (TLBIEL_INVAL_SET_LPID), "r" (lpid));
		__asm __volatile(PPC_TLBIE_5(%0,%1,2,1,1) : :
			     "r" (TLBIEL_INVAL_SET_LPID), "r" (lpid));
	} else {
		__asm __volatile(PPC_TLBIE_5(%0,%1,2,0,0) : :
			     "r" (TLBIEL_INVAL_SET_LPID), "r" (lpid));
	}
	__asm __volatile("eieio; tlbsync; ptesync" : : : "memory");
	printf("%s done\n", __func__);
}

static void
mmu_radix_parttab_init(void)
{
	uint64_t pagetab;

	mmu_parttab_init();
	printf("%s construct pagetab\n", __func__);
	pagetab = RTS_SIZE | DMAP_TO_PHYS((vm_offset_t)kernel_pmap->pm_pml1) | \
		         RADIX_PGD_INDEX_SHIFT | PARTTAB_HR;
	printf("%s install pagetab %lx\n", __func__, pagetab);
	mmu_parttab_update(0, pagetab, 0);
	printf("parttab inited\n");
}

static void
mmu_radix_proctab_register(vm_paddr_t proctabpa, uint64_t table_size)
{
	uint64_t pagetab, proctab;

	pagetab = be64toh(isa3_parttab[0].pagetab);
	proctab = proctabpa | table_size | PARTTAB_GR;
	mmu_parttab_update(0, pagetab, proctab);
}

static void
mmu_radix_proctab_init(void)
{
	uint64_t proctab_size;

	isa3_base_pid = 1;

	proctab_size = 1UL << PROCTAB_SIZE_SHIFT;
	isa3_proctab = (void*)PHYS_TO_DMAP(proctab0pa);
	memset(isa3_proctab, 0, proctab_size);
	isa3_proctab->proctab0 = htobe64(RTS_SIZE | DMAP_TO_PHYS((vm_offset_t)kernel_pmap->pm_pml1) | \
									 RADIX_PGD_INDEX_SHIFT);

	mmu_radix_proctab_register(proctab0pa, PROCTAB_SIZE_SHIFT - 12);

	__asm __volatile("ptesync" : : : "memory");
	__asm __volatile(PPC_TLBIE_5(%0,%1,2,1,1) : :
		     "r" (TLBIEL_INVAL_SET_LPID), "r" (0));
	__asm __volatile("eieio; tlbsync; ptesync" : : : "memory");
	printf("process table %p and kernel radix PDE: %p\n",
		   isa3_proctab, kernel_pmap->pm_pml1);
	mtmsr(mfmsr() | PSL_DR );
	mtmsr(mfmsr() &  ~PSL_DR);
	printf("bounced data translation\n");
	kernel_pmap->pm_pid = isa3_base_pid;
	isa3_base_pid++;
}

VISIBILITY void
METHOD(advise) pmap_t pmap, vm_offset_t start, vm_offset_t end, int advice)
{
	UNIMPLEMENTED();
}

/*
 * Routines used in machine-dependent code
 */
VISIBILITY void
METHOD(bootstrap) vm_offset_t start, vm_offset_t end)
{
	uint64_t lpcr;
	vm_paddr_t allocoff;

	printf("%s\n", __func__);
	hw_direct_map = 1;
	allocoff = mmu_radix_early_bootstrap(start, end);
	printf("early bootstrap complete\n");
	if (powernv_enabled) {
		lpcr = mfspr(SPR_LPCR);
		mtspr(SPR_LPCR, lpcr | LPCR_UPRT | LPCR_HR);
		mmu_radix_parttab_init();
		mmu_radix_init_amor();
		printf("powernv init complete\n");
	} else {
		/* XXX assume we're virtualized - QEMU doesn't support radix on powernv */
		/* radix_init_pseries() */
	}
	mmu_radix_init_iamr();
	mmu_radix_proctab_init();
	mmu_radix_pid_set(kernel_pmap);
	DELAY(10000);
	printf("set pid\n");
	/* XXX assume CPU_FTR_HVMODE */
	mmu_radix_tlbiel_flush(TLB_INVAL_SCOPE_GLOBAL);
	DELAY(10000);

	mmu_radix_late_bootstrap(allocoff, start, end);
	//psl_kernset &= ~(PSL_IR | PSL_DR);
	__pcpu[0].pc_curpmap = kernel_pmap;

	printf("%s done\n", __func__);
}

static SYSCTL_NODE(_vm_pmap, OID_AUTO, l3e, CTLFLAG_RD, 0,
    "2MB page mapping counters");

static u_long pmap_l3e_demotions;
SYSCTL_ULONG(_vm_pmap_l3e, OID_AUTO, demotions, CTLFLAG_RD,
    &pmap_l3e_demotions, 0, "2MB page demotions");

static u_long pmap_l3e_mappings;
SYSCTL_ULONG(_vm_pmap_l3e, OID_AUTO, mappings, CTLFLAG_RD,
    &pmap_l3e_mappings, 0, "2MB page mappings");

static u_long pmap_l3e_p_failures;
SYSCTL_ULONG(_vm_pmap_l3e, OID_AUTO, p_failures, CTLFLAG_RD,
    &pmap_l3e_p_failures, 0, "2MB page promotion failures");

static u_long pmap_l3e_promotions;
SYSCTL_ULONG(_vm_pmap_l3e, OID_AUTO, promotions, CTLFLAG_RD,
    &pmap_l3e_promotions, 0, "2MB page promotions");

static SYSCTL_NODE(_vm_pmap, OID_AUTO, l2e, CTLFLAG_RD, 0,
    "1GB page mapping counters");

static u_long pmap_l2e_demotions;
SYSCTL_ULONG(_vm_pmap_l2e, OID_AUTO, demotions, CTLFLAG_RD,
    &pmap_l2e_demotions, 0, "1GB page demotions");

bool
pmap_ps_enabled(pmap_t pmap)
{
	return (pg_ps_enabled && (pmap->pm_flags & PMAP_PDE_SUPERPAGE) != 0);
}

VISIBILITY void
METHOD(clear_modify) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
}

VISIBILITY void
METHOD(copy) pmap_t dst_pmap, pmap_t src_pmap, vm_offset_t dst_addr,
    vm_size_t len, vm_offset_t src_addr)
{

	CTR6(KTR_PMAP, "%s(%p, %p, %#x, %#x, %#x)", __func__, dst_pmap,
	    src_pmap, dst_addr, len, src_addr);
}

VISIBILITY void
METHOD(copy_page) vm_page_t src, vm_page_t dst)
{

	CTR3(KTR_PMAP, "%s(%p, %p)", __func__, src, dst);
}

VISIBILITY void
METHOD(copy_pages) vm_page_t ma[], vm_offset_t a_offset, vm_page_t mb[],
    vm_offset_t b_offset, int xfersize)
{

	CTR6(KTR_PMAP, "%s(%p, %#x, %p, %#x, %#x)", __func__, ma,
	    a_offset, mb, b_offset, xfersize);
}

#if VM_NRESERVLEVEL > 0
/*
 * Tries to promote the 512, contiguous 4KB page mappings that are within a
 * single page table page (PTP) to a single 2MB page mapping.  For promotion
 * to occur, two conditions must be met: (1) the 4KB page mappings must map
 * aligned, contiguous physical memory and (2) the 4KB page mappings must have
 * identical characteristics. 
 */
static void
pmap_promote_l3e(pmap_t pmap, pml3_entry_t *pde, vm_offset_t va,
    struct rwlock **lockp)
{
	pml3_entry_t newpde;
	pt_entry_t *firstpte, oldpte, pa, *pte;
	vm_page_t mpte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * Examine the first PTE in the specified PTP.  Abort if this PTE is
	 * either invalid, unused, or does not map the first 4KB physical page
	 * within a 2MB page. 
	 */
	firstpte = (pt_entry_t *)PHYS_TO_DMAP(*pde & PG_FRAME);
setpde:
	newpde = *firstpte;
	if ((newpde & ((PG_FRAME & L2_PAGE_MASK) | PG_A | PG_V)) != (PG_A | PG_V)) {
		atomic_add_long(&pmap_l3e_p_failures, 1);
		CTR2(KTR_PMAP, "pmap_promote_l3e: failure for va %#lx"
		    " in pmap %p", va, pmap);
		return;
	}
	if ((newpde & (PG_M | PG_RW)) == PG_RW) {
		/*
		 * When PG_M is already clear, PG_RW can be cleared without
		 * a TLB invalidation.
		 */
		if (!atomic_cmpset_long(firstpte, newpde, newpde & ~PG_RW))
			goto setpde;
		newpde &= ~PG_RW;
	}

	/*
	 * Examine each of the other PTEs in the specified PTP.  Abort if this
	 * PTE maps an unexpected 4KB physical page or does not have identical
	 * characteristics to the first PTE.
	 */
	pa = (newpde & (PG_PS_FRAME | PG_A | PG_V)) + L2_PAGE_SIZE - PAGE_SIZE;
	for (pte = firstpte + NPTEPG - 1; pte > firstpte; pte--) {
setpte:
		oldpte = *pte;
		if ((oldpte & (PG_FRAME | PG_A | PG_V)) != pa) {
			atomic_add_long(&pmap_l3e_p_failures, 1);
			CTR2(KTR_PMAP, "pmap_promote_l3e: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return;
		}
		if ((oldpte & (PG_M | PG_RW)) == PG_RW) {
			/*
			 * When PG_M is already clear, PG_RW can be cleared
			 * without a TLB invalidation.
			 */
			if (!atomic_cmpset_long(pte, oldpte, oldpte & ~PG_RW))
				goto setpte;
			oldpte &= ~PG_RW;
			CTR2(KTR_PMAP, "pmap_promote_l3e: protect for va %#lx"
			    " in pmap %p", (oldpte & PG_FRAME & L3_PAGE_MASK) |
			    (va & ~L3_PAGE_MASK), pmap);
		}
		if ((oldpte & PG_PTE_PROMOTE) != (newpde & PG_PTE_PROMOTE)) {
			atomic_add_long(&pmap_l3e_p_failures, 1);
			CTR2(KTR_PMAP, "pmap_promote_l3e: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return;
		}
		pa -= PAGE_SIZE;
	}

	/*
	 * Save the page table page in its current state until the PDE
	 * mapping the superpage is demoted by pmap_demote_pde() or
	 * destroyed by pmap_remove_pde(). 
	 */
	mpte = PHYS_TO_VM_PAGE(*pde & PG_FRAME);
	KASSERT(mpte >= vm_page_array &&
	    mpte < &vm_page_array[vm_page_array_size],
	    ("pmap_promote_l3e: page table page is out of range"));
	KASSERT(mpte->pindex == pmap_l3e_pindex(va),
	    ("pmap_promote_l3e: page table page's pindex is wrong"));
	if (pmap_insert_pt_page(pmap, mpte)) {
		atomic_add_long(&pmap_l3e_p_failures, 1);
		CTR2(KTR_PMAP,
		    "pmap_promote_l3e: failure for va %#lx in pmap %p", va,
		    pmap);
		return;
	}

	/*
	 * Promote the pv entries.
	 */
	if ((newpde & PG_MANAGED) != 0)
		pmap_pv_promote_l3e(pmap, va, newpde & PG_PS_FRAME, lockp);

	pde_store(pde, PG_PROMOTED | PG_PS | newpde);

	atomic_add_long(&pmap_l3e_promotions, 1);
	CTR2(KTR_PMAP, "pmap_promote_l3e: success for va %#lx"
	    " in pmap %p", va, pmap);
}
#endif /* VM_NRESERVLEVEL > 0 */

VISIBILITY int
METHOD(enter) pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot,
    u_int flags, int8_t psind)
{
	struct rwlock *lock;
	pml3_entry_t *l3e;
	pt_entry_t *pte;
	pt_entry_t newpte, origpte;
	pv_entry_t pv;
	vm_paddr_t opa, pa;
	vm_page_t mpte, om;
	int rv;
	boolean_t nosleep;

#if 0	
	printf("pmap_enter(%p, %#lx, %p, %#x, %x, %d)\n", pmap, va,
	    m, prot, flags, psind);
#endif
	va = trunc_page(va);
	CTR6(KTR_PMAP, "pmap_enter(%p, %#lx, %p, %#x, %x, %d)", pmap, va,
	    m, prot, flags, psind);
	KASSERT(va <= VM_MAX_KERNEL_ADDRESS, ("pmap_enter: toobig"));
	KASSERT((m->oflags & VPO_UNMANAGED) != 0 || va < kmi.clean_sva ||
	    va >= kmi.clean_eva,
	    ("pmap_enter: managed mapping within the clean submap"));
	if ((m->oflags & VPO_UNMANAGED) == 0 && !vm_page_xbusied(m))
		VM_OBJECT_ASSERT_LOCKED(m->object);
	KASSERT((flags & PMAP_ENTER_RESERVED) == 0,
	    ("pmap_enter: flags %u has reserved bits set", flags));
	pa = VM_PAGE_TO_PHYS(m);
	newpte = (pt_entry_t)(pa | PG_A | PG_V | RPTE_LEAF);
	if ((flags & VM_PROT_WRITE) != 0)
		newpte |= PG_M;
	if ((prot & VM_PROT_WRITE) != 0)
		newpte |= PG_RW;
	KASSERT((newpte & (PG_M | PG_RW)) != PG_M,
	    ("pmap_enter: flags includes VM_PROT_WRITE but prot doesn't"));
	if (prot & VM_PROT_EXECUTE)
		newpte |= PG_X;
	if ((flags & PMAP_ENTER_WIRED) != 0)
		newpte |= PG_W;
	if (va > DMAP_MIN_ADDRESS)
		newpte |= RPTE_EAA_P;
#if 0
	newpte |= pmap_cache_bits(pmap, m->md.pat_mode, psind > 0);
#endif
	/*
	 * Set modified bit gratuitously for writeable mappings if
	 * the page is unmanaged. We do not want to take a fault
	 * to do the dirty bit accounting for these mappings.
	 */
	if ((m->oflags & VPO_UNMANAGED) != 0) {
		if ((newpte & PG_RW) != 0)
			newpte |= PG_M;
	} else
		newpte |= PG_MANAGED;

	lock = NULL;
	PMAP_LOCK(pmap);
	if (psind == 1) {
		/* Assert the required virtual and physical alignment. */
		KASSERT((va & L3_PAGE_MASK) == 0, ("pmap_enter: va unaligned"));
		KASSERT(m->psind > 0, ("pmap_enter: m->psind < psind"));
		rv = pmap_enter_l3e(pmap, va, newpte | RPTE_LEAF, flags, m, &lock);
		goto out;
	}
	mpte = NULL;

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
retry:
	l3e = pmap_pml3e(pmap, va);
	if (l3e != NULL && (*l3e & PG_V) != 0 && ((*l3e & RPTE_LEAF) == 0 ||
	    pmap_demote_l3e_locked(pmap, l3e, va, &lock))) {
		pte = pmap_l3e_to_pte(l3e, va);
		if (va < VM_MAXUSER_ADDRESS && mpte == NULL) {
			mpte = PHYS_TO_VM_PAGE(*l3e & PG_FRAME);

			PMAP_LOG_WIRE(mpte);
			mpte->wire_count++;
		}
	} else if (va < VM_MAXUSER_ADDRESS) {
		/*
		 * Here if the pte page isn't mapped, or if it has been
		 * deallocated.
		 */
		nosleep = (flags & PMAP_ENTER_NOSLEEP) != 0;
		mpte = _pmap_allocpte(pmap, pmap_l3e_pindex(va),
		    nosleep ? NULL : &lock);
		if (mpte == NULL && nosleep) {
			rv = KERN_RESOURCE_SHORTAGE;
			goto out;
		}
		goto retry;
	} else
		panic("pmap_enter: invalid page directory va=%#lx", va);

	origpte = *pte;
	pv = NULL;

	/*
	 * Is the specified virtual address already mapped?
	 */
	if ((origpte & PG_V) != 0) {
		/*
		 * Wiring change, just update stats. We don't worry about
		 * wiring PT pages as they remain resident as long as there
		 * are valid mappings in them. Hence, if a user page is wired,
		 * the PT page will be also.
		 */
		if ((newpte & PG_W) != 0 && (origpte & PG_W) == 0)
			pmap->pm_stats.wired_count++;
		else if ((newpte & PG_W) == 0 && (origpte & PG_W) != 0)
			pmap->pm_stats.wired_count--;

		/*
		 * Remove the extra PT page reference.
		 */
		if (mpte != NULL) {
			mpte->wire_count--;
			KASSERT(mpte->wire_count > 0,
			    ("pmap_enter: missing reference to page table page,"
			     " va: 0x%lx", va));
		}

		/*
		 * Has the physical page changed?
		 */
		opa = origpte & PG_FRAME;
		if (opa == pa) {
			/*
			 * No, might be a protection or wiring change.
			 */
			if ((origpte & PG_MANAGED) != 0 &&
			    (newpte & PG_RW) != 0)
				vm_page_aflag_set(m, PGA_WRITEABLE);
			if (((origpte ^ newpte) & ~(PG_M | PG_A)) == 0)
				goto unchanged;
			goto validate;
		}

		/*
		 * The physical page has changed.  Temporarily invalidate
		 * the mapping.  This ensures that all threads sharing the
		 * pmap keep a consistent view of the mapping, which is
		 * necessary for the correct handling of COW faults.  It
		 * also permits reuse of the old mapping's PV entry,
		 * avoiding an allocation.
		 *
		 * For consistency, handle unmanaged mappings the same way.
		 */
		origpte = pte_load_clear(pte);
		KASSERT((origpte & PG_FRAME) == opa,
		    ("pmap_enter: unexpected pa update for %#lx", va));
		if ((origpte & PG_MANAGED) != 0) {
			om = PHYS_TO_VM_PAGE(opa);

			/*
			 * The pmap lock is sufficient to synchronize with
			 * concurrent calls to pmap_page_test_mappings() and
			 * pmap_ts_referenced().
			 */
			if ((origpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(om);
			if ((origpte & PG_A) != 0)
				vm_page_aflag_set(om, PGA_REFERENCED);
			CHANGE_PV_LIST_LOCK_TO_PHYS(&lock, opa);
			pv = pmap_pvh_remove(&om->md, pmap, va);
			if ((newpte & PG_MANAGED) == 0)
				free_pv_entry(pmap, pv);
			if ((om->aflags & PGA_WRITEABLE) != 0 &&
			    TAILQ_EMPTY(&om->md.pv_list) &&
			    ((om->flags & PG_FICTITIOUS) != 0 ||
			    TAILQ_EMPTY(&pa_to_pvh(opa)->pv_list)))
				vm_page_aflag_clear(om, PGA_WRITEABLE);
		}
		if ((origpte & PG_A) != 0)
			pmap_invalidate_page(pmap, va);
		origpte = 0;
	} else {
		/*
		 * Increment the counters.
		 */
		if ((newpte & PG_W) != 0)
			pmap->pm_stats.wired_count++;
		pmap_resident_count_inc(pmap, 1);
	}

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((newpte & PG_MANAGED) != 0) {
		if (pv == NULL) {
			pv = get_pv_entry(pmap, &lock);
			pv->pv_va = va;
		}
		CHANGE_PV_LIST_LOCK_TO_PHYS(&lock, pa);
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
		m->md.pv_gen++;
		if ((newpte & PG_RW) != 0)
			vm_page_aflag_set(m, PGA_WRITEABLE);
	}

	/*
	 * Update the PTE.
	 */
	if ((origpte & PG_V) != 0) {
validate:
		origpte = pte_load_store(pte, newpte);
		KASSERT((origpte & PG_FRAME) == pa,
		    ("pmap_enter: unexpected pa update for %#lx", va));
		if ((newpte & PG_M) == 0 && (origpte & (PG_M | PG_RW)) ==
		    (PG_M | PG_RW)) {
			if ((origpte & PG_MANAGED) != 0)
				vm_page_dirty(m);

			/*
			 * Although the PTE may still have PG_RW set, TLB
			 * invalidation may nonetheless be required because
			 * the PTE no longer has PG_M set.
			 */
		} else if ((origpte & PG_X) != 0 || (newpte & PG_X) == 0) {
			/*
			 * This PTE change does not require TLB invalidation.
			 * XXX true on POWER?
			 */
			goto unchanged;
		}
		if ((origpte & PG_A) != 0)
			pmap_invalidate_page(pmap, va);
	} else
		pte_store(pte, newpte);
unchanged:

#if VM_NRESERVLEVEL > 0
	/*
	 * If both the page table page and the reservation are fully
	 * populated, then attempt promotion.
	 */
	if ((mpte == NULL || mpte->wire_count == NPTEPG) &&
	    pmap_ps_enabled(pmap) &&
	    (m->flags & PG_FICTITIOUS) == 0 &&
	    vm_reserv_level_iffullpop(m) == 0)
		pmap_promote_l3e(pmap, l3e, va, &lock);
#endif

	rv = KERN_SUCCESS;
out:
	if (lock != NULL)
		rw_wunlock(lock);
	PMAP_UNLOCK(pmap);
	return (rv);
}


/*
 * Tries to create a read- and/or execute-only 2MB page mapping.  Returns true
 * if successful.  Returns false if (1) a page table page cannot be allocated
 * without sleeping, (2) a mapping already exists at the specified virtual
 * address, or (3) a PV entry cannot be allocated without reclaiming another
 * PV entry.
 */
static bool
pmap_enter_2mpage(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot,
    struct rwlock **lockp)
{
	pml3_entry_t newpde;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	newpde = VM_PAGE_TO_PHYS(m) | /* pmap_cache_bits(pmap, m->md.pat_mode, 1) | */
	    RPTE_LEAF | PG_V;
	if ((m->oflags & VPO_UNMANAGED) == 0)
		newpde |= PG_MANAGED;
	if (prot & VM_PROT_EXECUTE)
		newpde |= PG_X;
	if (va > DMAP_MIN_ADDRESS)
		newpde |= RPTE_EAA_P;
	return (pmap_enter_l3e(pmap, va, newpde, PMAP_ENTER_NOSLEEP |
	    PMAP_ENTER_NOREPLACE | PMAP_ENTER_NORECLAIM, NULL, lockp) ==
	    KERN_SUCCESS);
}

/*
 * Tries to create the specified 2MB page mapping.  Returns KERN_SUCCESS if
 * the mapping was created, and either KERN_FAILURE or KERN_RESOURCE_SHORTAGE
 * otherwise.  Returns KERN_FAILURE if PMAP_ENTER_NOREPLACE was specified and
 * a mapping already exists at the specified virtual address.  Returns
 * KERN_RESOURCE_SHORTAGE if PMAP_ENTER_NOSLEEP was specified and a page table
 * page allocation failed.  Returns KERN_RESOURCE_SHORTAGE if
 * PMAP_ENTER_NORECLAIM was specified and a PV entry allocation failed.
 *
 * The parameter "m" is only used when creating a managed, writeable mapping.
 */
static int
pmap_enter_l3e(pmap_t pmap, vm_offset_t va, pml3_entry_t newpde, u_int flags,
    vm_page_t m, struct rwlock **lockp)
{
	struct spglist free;
	pml3_entry_t oldpde, *pde;
	vm_page_t mt, pdpg;

	KASSERT((newpde & (PG_M | PG_RW)) != PG_RW,
	    ("pmap_enter_pde: newpde is missing PG_M"));
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	if ((pdpg = pmap_allocl3e(pmap, va, (flags & PMAP_ENTER_NOSLEEP) != 0 ?
	    NULL : lockp)) == NULL) {
		CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
		    " in pmap %p", va, pmap);
		return (KERN_RESOURCE_SHORTAGE);
	}
	pde = (pml3_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(pdpg));
	pde = &pde[pmap_pml3e_index(va)];
	oldpde = *pde;
	if ((oldpde & PG_V) != 0) {
		KASSERT(pdpg->wire_count > 1,
		    ("pmap_enter_pde: pdpg's wire count is too low"));
		if ((flags & PMAP_ENTER_NOREPLACE) != 0) {
			pdpg->wire_count--;
			CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return (KERN_FAILURE);
		}
		/* Break the existing mapping(s). */
		SLIST_INIT(&free);
		if ((oldpde & RPTE_LEAF) != 0) {
			/*
			 * The reference to the PD page that was acquired by
			 * pmap_allocl3e() ensures that it won't be freed.
			 * However, if the PDE resulted from a promotion, then
			 * a reserved PT page could be freed.
			 */
			(void)pmap_remove_l3e(pmap, pde, va, &free, lockp);
		} else {
			pmap_delayed_invl_started();
			if (pmap_remove_ptes(pmap, va, va + L2_PAGE_SIZE, pde, &free,
			    lockp))
		               pmap_invalidate_all(pmap);
			pmap_delayed_invl_finished();
		}
		vm_page_free_pages_toq(&free, true);
		if (va >= VM_MAXUSER_ADDRESS) {
			mt = PHYS_TO_VM_PAGE(*pde & PG_FRAME);
			if (pmap_insert_pt_page(pmap, mt)) {
				/*
				 * XXX Currently, this can't happen because
				 * we do not perform pmap_enter(psind == 1)
				 * on the kernel pmap.
				 */
				panic("pmap_enter_pde: trie insert failed");
			}
		} else
			KASSERT(*pde == 0, ("pmap_enter_pde: non-zero pde %p",
			    pde));
	}
	if ((newpde & PG_MANAGED) != 0) {
		/*
		 * Abort this mapping if its PV entry could not be created.
		 */
		if (!pmap_pv_insert_l3e(pmap, va, newpde, flags, lockp)) {
			SLIST_INIT(&free);
			if (pmap_unwire_ptp(pmap, va, pdpg, &free)) {
				/*
				 * Although "va" is not mapped, paging-
				 * structure caches could nonetheless have
				 * entries that refer to the freed page table
				 * pages.  Invalidate those entries.
				 */
				pmap_invalidate_page(pmap, va);
				vm_page_free_pages_toq(&free, true);
			}
			CTR2(KTR_PMAP, "pmap_enter_pde: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return (KERN_RESOURCE_SHORTAGE);
		}
		if ((newpde & PG_RW) != 0) {
			for (mt = m; mt < &m[L2_PAGE_SIZE / PAGE_SIZE]; mt++)
				vm_page_aflag_set(mt, PGA_WRITEABLE);
		}
	}

	/*
	 * Increment counters.
	 */
	if ((newpde & PG_W) != 0)
		pmap->pm_stats.wired_count += L2_PAGE_SIZE / PAGE_SIZE;
	pmap_resident_count_inc(pmap, L2_PAGE_SIZE / PAGE_SIZE);

	/*
	 * Map the superpage.  (This is not a promoted mapping; there will not
	 * be any lingering 4KB page mappings in the TLB.)
	 */
	pde_store(pde, newpde);

	atomic_add_long(&pmap_l3e_mappings, 1);
	CTR2(KTR_PMAP, "pmap_enter_pde: success for va %#lx"
	    " in pmap %p", va, pmap);
	return (KERN_SUCCESS);
}

VISIBILITY void
METHOD(enter_object) pmap_t pmap, vm_offset_t start, vm_offset_t end,
    vm_page_t m_start, vm_prot_t prot)
{

	struct rwlock *lock;
	vm_offset_t va;
	vm_page_t m, mpte;
	vm_pindex_t diff, psize;

	VM_OBJECT_ASSERT_LOCKED(m_start->object);

	CTR6(KTR_PMAP, "%s(%p, %#x, %#x, %p, %#x)", __func__, pmap, start,
	    end, m_start, prot);

	psize = atop(end - start);
	mpte = NULL;
	m = m_start;
	lock = NULL;
	PMAP_LOCK(pmap);
	while (m != NULL && (diff = m->pindex - m_start->pindex) < psize) {
		va = start + ptoa(diff);
		if ((va & L2_PAGE_MASK) == 0 && va + L2_PAGE_SIZE <= end &&
		    m->psind == 1 && pmap_ps_enabled(pmap) &&
		    pmap_enter_2mpage(pmap, va, m, prot, &lock))
			m = &m[L2_PAGE_SIZE / PAGE_SIZE - 1];
		else
			mpte = pmap_enter_quick_locked(pmap, va, m, prot,
			    mpte, &lock);
		m = TAILQ_NEXT(m, listq);
	}
	if (lock != NULL)
		rw_wunlock(lock);
	PMAP_UNLOCK(pmap);
}

static vm_page_t
pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va, vm_page_t m,
    vm_prot_t prot, vm_page_t mpte, struct rwlock **lockp)
{
	struct spglist free;
	pt_entry_t *pte;
	vm_paddr_t pa;

	KASSERT(va < kmi.clean_sva || va >= kmi.clean_eva ||
	    (m->oflags & VPO_UNMANAGED) != 0,
	    ("pmap_enter_quick_locked: managed mapping within the clean submap"));
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS) {
		vm_pindex_t ptepindex;
		pml3_entry_t *ptepa;

		/*
		 * Calculate pagetable page index
		 */
		ptepindex = pmap_l3e_pindex(va);
		if (mpte && (mpte->pindex == ptepindex)) {
			PMAP_LOG_WIRE(mpte);
			mpte->wire_count++;
		} else {
			/*
			 * Get the page directory entry
			 */
			ptepa = pmap_pml3e(pmap, va);

			/*
			 * If the page table page is mapped, we just increment
			 * the hold count, and activate it.  Otherwise, we
			 * attempt to allocate a page table page.  If this
			 * attempt fails, we don't retry.  Instead, we give up.
			 */
			if (ptepa && (*ptepa & PG_V) != 0) {
				if (*ptepa & PG_PS)
					return (NULL);
				mpte = PHYS_TO_VM_PAGE(*ptepa & PG_FRAME);
				PMAP_LOG_WIRE(mpte);
				mpte->wire_count++;
			} else {
				/*
				 * Pass NULL instead of the PV list lock
				 * pointer, because we don't intend to sleep.
				 */
				mpte = _pmap_allocpte(pmap, ptepindex, NULL);
				if (mpte == NULL)
					return (mpte);
			}
		}
		pte = (pt_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mpte));
		pte = &pte[pmap_pte_index(va)];
	} else {
		mpte = NULL;
		pte = vtopte(va);
	}
	if (*pte) {
		if (mpte != NULL) {
			mpte->wire_count--;
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((m->oflags & VPO_UNMANAGED) == 0 &&
	    !pmap_try_insert_pv_entry(pmap, va, m, lockp)) {
		if (mpte != NULL) {
			SLIST_INIT(&free);
			if (pmap_unwire_ptp(pmap, va, mpte, &free)) {
				/*
				 * Although "va" is not mapped, paging-
				 * structure caches could nonetheless have
				 * entries that refer to the freed page table
				 * pages.  Invalidate those entries.
				 */
				pmap_invalidate_page(pmap, va);
				vm_page_free_pages_toq(&free, true);
			}
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Increment counters
	 */
	pmap_resident_count_inc(pmap, 1);

	pa = VM_PAGE_TO_PHYS(m) | pmap_cache_bits(pmap, m->md.pat_mode, 0);
	if (prot & VM_PROT_EXECUTE)
		pa |= PG_X;

	/*
	 * Now validate mapping with RO protection
	 */
	if ((m->oflags & VPO_UNMANAGED) != 0)
		pte_store(pte, pa | PG_V);
	else
		pte_store(pte, pa | PG_V | PG_MANAGED);
	return (mpte);
}

VISIBILITY void
METHOD(enter_quick) pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot)
{

	CTR5(KTR_PMAP, "%s(%p, %#x, %p, %#x)", __func__, pmap, va, m, prot);

}

VISIBILITY vm_paddr_t
METHOD(extract) pmap_t pmap, vm_offset_t va)
{

	CTR3(KTR_PMAP, "%s(%p, %#x)", __func__, pmap, va);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY vm_page_t
METHOD(extract_and_hold) pmap_t pmap, vm_offset_t va, vm_prot_t prot)
{

	CTR4(KTR_PMAP, "%s(%p, %#x, %#x)", __func__, pmap, va, prot);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY void
METHOD(growkernel) vm_offset_t addr)
{
	vm_paddr_t paddr;
	vm_page_t nkpg;
	pml3_entry_t *l3e;
	pml2_entry_t *l2e;

	CTR2(KTR_PMAP, "%s(%#x)", __func__, va);
	if (VM_MIN_KERNEL_ADDRESS < addr &&
		addr < (VM_MIN_KERNEL_ADDRESS + nkpt*L3_PAGE_SIZE))
		return;

	addr = roundup2(addr, L3_PAGE_SIZE);
	if (addr - 1 >= vm_map_max(kernel_map))
		addr = vm_map_max(kernel_map);
	while (kernel_vm_end < addr) {
		l2e = pmap_pml2e(kernel_pmap, kernel_vm_end);
		if ((*l2e & PG_V) == 0) {
			/* We need a new PDP entry */
			nkpg = vm_page_alloc(NULL, kernel_vm_end >> L2_PAGE_SIZE_SHIFT,
			    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ |
			    VM_ALLOC_WIRED | VM_ALLOC_ZERO);
			if (nkpg == NULL)
				panic("pmap_growkernel: no memory to grow kernel");
			if ((nkpg->flags & PG_ZERO) == 0)
				pmap_zero_page(nkpg);
			paddr = VM_PAGE_TO_PHYS(nkpg);
			pde_store(l2e, paddr);
			continue; /* try again */
		}
		l3e = pmap_l2e_to_l3e(l2e, kernel_vm_end);
		if ((*l3e & PG_V) != 0) {
			kernel_vm_end = (kernel_vm_end + L3_PAGE_SIZE) & ~L3_PAGE_MASK;
			if (kernel_vm_end - 1 >= vm_map_max(kernel_map)) {
				kernel_vm_end = vm_map_max(kernel_map);
				break;
			}
			continue;
		}

		nkpg = vm_page_alloc(NULL, pmap_l3e_pindex(kernel_vm_end),
		    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ | VM_ALLOC_WIRED |
		    VM_ALLOC_ZERO);
		if (nkpg == NULL)
			panic("pmap_growkernel: no memory to grow kernel");
		if ((nkpg->flags & PG_ZERO) == 0)
			pmap_zero_page(nkpg);
		paddr = VM_PAGE_TO_PHYS(nkpg);
		pde_store(l3e, paddr);

		kernel_vm_end = (kernel_vm_end + L3_PAGE_SIZE) & ~L3_PAGE_MASK;
		if (kernel_vm_end - 1 >= vm_map_max(kernel_map)) {
			kernel_vm_end = vm_map_max(kernel_map);
			break;
		}
	}
}

static MALLOC_DEFINE(M_RADIX_PGD, "radix_pgd", "radix page table root directory");
static uma_zone_t zone_radix_pgd;

static int
radix_pgd_import(void *arg __unused, void **store, int count, int domain __unused,
    int flags)
{
	printf("%s(%p, %p, %d, %d, %x)\n",
		   __func__, arg, store, count, domain, flags);
	
	for (int i = 0; i < count; i++) {
		vm_page_t m = vm_page_alloc_contig(NULL, 0, VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ |
			VM_ALLOC_WIRED | VM_ALLOC_ZERO | VM_ALLOC_WAITOK, RADIX_PGD_SIZE/PAGE_SIZE,
		    0, (vm_paddr_t)-1, RADIX_PGD_SIZE, L1_PAGE_SIZE, VM_MEMATTR_DEFAULT);
		store[i] = (void *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));
	}
	printf("%s done\n", __func__);
	return (count);
}

static void
radix_pgd_release(void *arg __unused, void **store, int count)
{
	vm_page_t m;
	struct spglist free;
	int page_count;

	SLIST_INIT(&free);
	page_count = RADIX_PGD_SIZE/PAGE_SIZE;

	for (int i = 0; i < count; i++) {
		m = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)store[i]));
		for (int j = page_count-1; j >= 0; j--)
			SLIST_INSERT_HEAD(&free, &m[j], plinks.s.ss); 
		vm_page_free_pages_toq(&free, false);
	}
}

VISIBILITY void
METHODVOID(init)
{
	vm_page_t mpte;
	vm_size_t s;
	int error, i, pv_npg;

	/* L1TF, reserve page @0 unconditionally */
	vm_page_blacklist_add(0, bootverbose);
	
	zone_radix_pgd = uma_zcache_create("radix_pgd_cache",
		RADIX_PGD_SIZE, NULL, NULL,
#ifdef INVARIANTS
	    trash_init, trash_fini,
#else
	    NULL, NULL,
#endif
		radix_pgd_import, radix_pgd_release,
		NULL, UMA_ZONE_NOBUCKET);

	/*
	 * Initialize the vm page array entries for the kernel pmap's
	 * page table pages.
	 */ 
	PMAP_LOCK(kernel_pmap);
	for (i = 0; i < nkpt; i++) {
		mpte = PHYS_TO_VM_PAGE(KPTphys + (i << PAGE_SHIFT));
		KASSERT(mpte >= vm_page_array &&
		    mpte < &vm_page_array[vm_page_array_size],
				("pmap_init: page table page is out of range size: %lu",
				 vm_page_array_size));
		mpte->pindex = pmap_l3e_pindex(VM_MIN_KERNEL_ADDRESS) + i;
		mpte->phys_addr = KPTphys + (i << PAGE_SHIFT);
		pmap_insert_pt_page(kernel_pmap, mpte);
		PMAP_LOG_WIRE(mpte);
		mpte->wire_count = 1;
	}
	PMAP_UNLOCK(kernel_pmap);
	vm_wire_add(nkpt);
	
	CTR1(KTR_PMAP, "%s()", __func__);
	TAILQ_INIT(&pv_dummy.pv_list);

	/*
	 * Are large page mappings enabled?
	 */
	TUNABLE_INT_FETCH("vm.pmap.pg_ps_enabled", &pg_ps_enabled);
	if (pg_ps_enabled) {
		KASSERT(MAXPAGESIZES > 1 && pagesizes[1] == 0,
		    ("pmap_init: can't assign to pagesizes[1]"));
		pagesizes[1] = L3_PAGE_SIZE;
	}

	/*
	 * Initialize the pv chunk list mutex.
	 */
	mtx_init(&pv_chunks_mutex, "pmap pv chunk list", NULL, MTX_DEF);

	/*
	 * Initialize the pool of pv list locks.
	 */
	for (i = 0; i < NPV_LIST_LOCKS; i++)
		rw_init(&pv_list_locks[i], "pmap pv list");

	/*
	 * Calculate the size of the pv head table for superpages.
	 */
	pv_npg = howmany(vm_phys_segs[vm_phys_nsegs - 1].end, L3_PAGE_SIZE);

	/*
	 * Allocate memory for the pv head table for superpages.
	 */
	s = (vm_size_t)(pv_npg * sizeof(struct md_page));
	s = round_page(s);
	pv_table = (struct md_page *)kmem_malloc(s, M_WAITOK | M_ZERO);
	for (i = 0; i < pv_npg; i++)
		TAILQ_INIT(&pv_table[i].pv_list);
	TAILQ_INIT(&pv_dummy.pv_list);

	pmap_initialized = 1;
	mtx_init(&qframe_mtx, "qfrmlk", NULL, MTX_SPIN);
	error = vmem_alloc(kernel_arena, PAGE_SIZE, M_BESTFIT | M_WAITOK,
	    (vmem_addr_t *)&qframe);
	if (error != 0)
		panic("qframe allocation failed");
}

VISIBILITY boolean_t
METHOD(is_modified) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
	return false;
}

VISIBILITY boolean_t
METHOD(is_prefaultable) pmap_t pmap, vm_offset_t va)
{

	CTR3(KTR_PMAP, "%s(%p, %#x)", __func__, pmap, va);
	UNIMPLEMENTED();
	return false;
}

VISIBILITY boolean_t
METHOD(is_referenced) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY boolean_t
METHOD(ts_referenced) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY vm_offset_t
METHOD(map) vm_offset_t *virt __unused, vm_paddr_t start, vm_paddr_t end, int prot __unused)
{

	CTR5(KTR_PMAP, "%s(%p, %#x, %#x, %#x)", __func__, virt, start, end,
	    prot);
	for (vm_paddr_t sva = start; sva < end; sva += PAGE_SIZE)
		pagezero((void*)PHYS_TO_DMAP(sva));
	return (PHYS_TO_DMAP(start));
}

VISIBILITY void
METHOD(object_init_pt) pmap_t pmap, vm_offset_t addr, vm_object_t object,
    vm_pindex_t pindex, vm_size_t size)
{

	CTR6(KTR_PMAP, "%s(%p, %#x, %p, %u, %#x)", __func__, pmap, addr,
	    object, pindex, size);
	UNIMPLEMENTED();
}

VISIBILITY boolean_t
METHOD(page_exists_quick) pmap_t pmap, vm_page_t m)
{

	CTR3(KTR_PMAP, "%s(%p, %p)", __func__, pmap, m);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY void
METHOD(page_init) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	TAILQ_INIT(&m->md.pv_list);
	m->md.md_attr = VM_MEMATTR_DEFAULT;
}

VISIBILITY int
METHOD(page_wired_mappings) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	return (0);
}

static inline int
mmu_radix_pmap_pinit(pmap_t pmap)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, pmap);

	/*
	 * allocate the page directory page
	 */
	pmap->pm_pml1 = uma_zalloc(zone_radix_pgd, M_WAITOK);

	for (int j = 0; j <  RADIX_PGD_SIZE/PAGE_SIZE; j++)
			pagezero(pmap->pm_pml1 + j*PAGE_SIZE);
	pmap->pm_root.rt_root = 0;
	CPU_ZERO(&pmap->pm_active);
	TAILQ_INIT(&pmap->pm_pvchunk);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
	pmap->pm_flags = PMAP_PDE_SUPERPAGE;
	return (1);
}

#ifdef MMU_DIRECT
int
pmap_pinit(pmap_t pmap)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, pmap);
	return (mmu_radix_pmap_pinit(pmap));
}
#else
static inline void
mmu_radix_pinit(mmu_t mmu, pmap_t pmap)
{
	(void)mmu_radix_pmap_pinit(pmap);
}
#endif


/*
 * This routine is called if the desired page table page does not exist.
 *
 * If page table page allocation fails, this routine may sleep before
 * returning NULL.  It sleeps only if a lock pointer was given.
 *
 * Note: If a page allocation fails at page table level two or three,
 * one or two pages may be held during the wait, only to be released
 * afterwards.  This conservative approach is easily argued to avoid
 * race conditions.
 */
static vm_page_t
_pmap_allocpte(pmap_t pmap, vm_pindex_t ptepindex, struct rwlock **lockp)
{
	vm_page_t m, pdppg, pdpg;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * Allocate a page table page.
	 */
	if ((m = vm_page_alloc(NULL, ptepindex, VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED | VM_ALLOC_ZERO)) == NULL) {
		if (lockp != NULL) {
			RELEASE_PV_LIST_LOCK(lockp);
			PMAP_UNLOCK(pmap);
			PMAP_ASSERT_NOT_IN_DI();
			vm_wait(NULL);
			PMAP_LOCK(pmap);
		}

		/*
		 * Indicate the need to retry.  While waiting, the page table
		 * page may have been allocated.
		 */
		return (NULL);
	}
	if ((m->flags & PG_ZERO) == 0)
		pmap_zero_page(m);

	/*
	 * Map the pagetable page into the process address space, if
	 * it isn't already there.
	 */

	if (ptepindex >= (NUPDE + NUPDPE)) {
		pml4_entry_t *pml1;
		vm_pindex_t pml1index;

		/* Wire up a new PDPE page */
		pml1index = ptepindex - (NUPDE + NUPDPE);
		pml1 = &pmap->pm_pml1[pml1index];
		*pml1 = VM_PAGE_TO_PHYS(m) | RPTE_VALID | RPTE_SHIFT;

	} else if (ptepindex >= NUPDE) {
		vm_pindex_t pml1index;
		vm_pindex_t pdpindex;
		pml1_entry_t *pml1;
		pml2_entry_t *pdp;

		/* Wire up a new PDE page */
		pdpindex = ptepindex - NUPDE;
		pml1index = pdpindex >> RADIX_PGD_INDEX_SHIFT;

		pml1 = &pmap->pm_pml1[pml1index];
		if ((*pml1 & PG_V) == 0) {
			/* Have to allocate a new pdp, recurse */
			if (_pmap_allocpte(pmap, NUPDE + NUPDPE + pml1index,
			    lockp) == NULL) {
				vm_page_unwire_noq(m);
				vm_page_free_zero(m);
				return (NULL);
			}
		} else {
			/* Add reference to pdp page */
			pdppg = PHYS_TO_VM_PAGE(*pml1 & PG_FRAME);
			PMAP_LOG_WIRE(pdppg);
			pdppg->wire_count++;
		}
		pdp = (pml2_entry_t *)PHYS_TO_DMAP(*pml1 & PG_FRAME);

		/* Now find the pdp page */
		pdp = &pdp[pdpindex & ((1ul << RPTE_SHIFT) - 1)];
		*pdp = VM_PAGE_TO_PHYS(m) | RPTE_VALID | RPTE_SHIFT;

	} else {
		vm_pindex_t pml1index;
		vm_pindex_t pdpindex;
		pml1_entry_t *pml1;
		pml2_entry_t *pdp;
		pml3_entry_t *pd;

		/* Wire up a new PTE page */
		pdpindex = ptepindex >> RPTE_SHIFT;
		pml1index = pdpindex >> RADIX_PGD_INDEX_SHIFT;

		/* First, find the pdp and check that its valid. */
		pml1 = &pmap->pm_pml1[pml1index];
		if ((*pml1 & PG_V) == 0) {
			/* Have to allocate a new pd, recurse */
			if (_pmap_allocpte(pmap, NUPDE + pdpindex,
			    lockp) == NULL) {
				vm_page_unwire_noq(m);
				vm_page_free_zero(m);
				return (NULL);
			}
			pdp = (pml2_entry_t *)PHYS_TO_DMAP(*pml1 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << RPTE_SHIFT) - 1)];
		} else {
			pdp = (pml2_entry_t *)PHYS_TO_DMAP(*pml1 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << RPTE_SHIFT) - 1)];
			if ((*pdp & PG_V) == 0) {
				/* Have to allocate a new pd, recurse */
				if (_pmap_allocpte(pmap, NUPDE + pdpindex,
				    lockp) == NULL) {
					vm_page_unwire_noq(m);
					vm_page_free_zero(m);
					return (NULL);
				}
			} else {
				/* Add reference to the pd page */
				pdpg = PHYS_TO_VM_PAGE(*pdp & PG_FRAME);
				PMAP_LOG_WIRE(pdpg);
				pdpg->wire_count++;
			}
		}
		pd = (pml3_entry_t *)PHYS_TO_DMAP(*pdp & PG_FRAME);

		/* Now we know where the page directory page is */
		pd = &pd[ptepindex & ((1ul << RPTE_SHIFT) - 1)];
		*pd = VM_PAGE_TO_PHYS(m) | RPTE_VALID | RPTE_SHIFT;
	}

	pmap_resident_count_inc(pmap, 1);

	return (m);
}
static vm_page_t
pmap_allocl3e(pmap_t pmap, vm_offset_t va, struct rwlock **lockp)
{
	vm_pindex_t pdpindex, ptepindex;
	pml2_entry_t *pdpe;
	vm_page_t pdpg;

retry:
	pdpe = pmap_pml2e(pmap, va);
	if (pdpe != NULL && (*pdpe & PG_V) != 0) {
		/* Add a reference to the pd page. */
		pdpg = PHYS_TO_VM_PAGE(*pdpe & PG_FRAME);
		PMAP_LOG_WIRE(pdpg);
		pdpg->wire_count++;
	} else {
		/* Allocate a pd page. */
		ptepindex = pmap_l3e_pindex(va);
		pdpindex = ptepindex >> RPTE_SHIFT;
		pdpg = _pmap_allocpte(pmap, NUPDE + pdpindex, lockp);
		if (pdpg == NULL && lockp != NULL)
			goto retry;
	}
	return (pdpg);
}
#ifdef notyet
static vm_page_t
pmap_allocpte(pmap_t pmap, vm_offset_t va, struct rwlock **lockp)
{
	vm_pindex_t ptepindex;
	pml3_entry_t *pd;
	vm_page_t m;

	/*
	 * Calculate pagetable page index
	 */
	ptepindex = pmap_l3e_pindex(va);
retry:
	/*
	 * Get the page directory entry
	 */
	pd = pmap_pml3e(pmap, va);

	/*
	 * This supports switching from a 2MB page to a
	 * normal 4K page.
	 */
	if (pd != NULL && (*pd & (PG_PS | PG_V)) == (PG_PS | PG_V)) {
		if (!pmap_demote_l3e_locked(pmap, pd, va, lockp)) {
			/*
			 * Invalidation of the 2MB page mapping may have caused
			 * the deallocation of the underlying PD page.
			 */
			pd = NULL;
		}
	}

	/*
	 * If the page table page is mapped, we just increment the
	 * hold count, and activate it.
	 */
	if (pd != NULL && (*pd & PG_V) != 0) {
		m = PHYS_TO_VM_PAGE(*pd & PG_FRAME);
		PMAP_LOG_WIRE(m);
		m->wire_count++;
	} else {
		/*
		 * Here if the pte page isn't mapped, or if it has been
		 * deallocated.
		 */
		m = _pmap_allocpte(pmap, ptepindex, lockp);
		if (m == NULL && lockp != NULL)
			goto retry;
	}
	return (m);
}
#endif
VISIBILITY void
METHOD(pinit0) pmap_t pmap)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, pmap);
	PMAP_LOCK_INIT(pmap);
	pmap->pm_pml1 = kernel_pmap->pm_pml1;
	pmap->pm_root.rt_root = 0;
	CPU_ZERO(&pmap->pm_active);
	TAILQ_INIT(&pmap->pm_pvchunk);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
	pmap->pm_flags = PMAP_PDE_SUPERPAGE;
}
/*
 * pmap_protect_l3e: do the things to protect a 2mpage in a process
 */
static boolean_t
pmap_protect_l3e(pmap_t pmap, pt_entry_t *l3e, vm_offset_t sva, vm_prot_t prot)
{
	pt_entry_t newpde, oldpde;
	vm_offset_t eva, va;
	vm_page_t m;
	boolean_t anychanged;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((sva & L3_PAGE_MASK) == 0,
	    ("pmap_protect_l3e: sva is not 2mpage aligned"));
	anychanged = FALSE;
retry:
	oldpde = newpde = *l3e;
	if ((oldpde & (PG_MANAGED | PG_M | PG_RW)) ==
	    (PG_MANAGED | PG_M | PG_RW)) {
		eva = sva + L3_PAGE_SIZE;
		for (va = sva, m = PHYS_TO_VM_PAGE(oldpde & PG_PS_FRAME);
		    va < eva; va += PAGE_SIZE, m++)
			vm_page_dirty(m);
	}
	if ((prot & VM_PROT_WRITE) == 0)
		newpde &= ~(PG_RW | PG_M);
	if (prot & VM_PROT_EXECUTE)
		newpde |= PG_X;
	if (newpde != oldpde) {
		/*
		 * As an optimization to future operations on this PDE, clear
		 * PG_PROMOTED.  The impending invalidation will remove any
		 * lingering 4KB page mappings from the TLB.
		 */
		if (!atomic_cmpset_long(l3e, oldpde, newpde & ~PG_PROMOTED))
			goto retry;
		anychanged = TRUE;
	}
	return (anychanged);
}


VISIBILITY void
METHOD(protect) pmap_t pmap, vm_offset_t sva, vm_offset_t eva, vm_prot_t prot)
{
	vm_offset_t va_next;
	pml1_entry_t *l1e;
	pml2_entry_t *l2e;
	pml3_entry_t ptpaddr, *l3e;
	pt_entry_t *pte;
	boolean_t anychanged;

	CTR5(KTR_PMAP, "%s(%p, %#x, %#x, %#x)", __func__, pmap, start, end,
	    prot);

	KASSERT((prot & ~VM_PROT_ALL) == 0, ("invalid prot %x", prot));
	if (prot == VM_PROT_NONE) {
		mmu_radix_pmap_remove(pmap, sva, eva);
		return;
	}

	if ((prot & (VM_PROT_WRITE|VM_PROT_EXECUTE)) ==
	    (VM_PROT_WRITE|VM_PROT_EXECUTE))
		return;

	anychanged = FALSE;

	/*
	 * Although this function delays and batches the invalidation
	 * of stale TLB entries, it does not need to call
	 * pmap_delayed_invl_started() and
	 * pmap_delayed_invl_finished(), because it does not
	 * ordinarily destroy mappings.  Stale TLB entries from
	 * protection-only changes need only be invalidated before the
	 * pmap lock is released, because protection-only changes do
	 * not destroy PV entries.  Even operations that iterate over
	 * a physical page's PV list of mappings, like
	 * pmap_remove_write(), acquire the pmap lock for each
	 * mapping.  Consequently, for protection-only changes, the
	 * pmap lock suffices to synchronize both page table and TLB
	 * updates.
	 *
	 * This function only destroys a mapping if pmap_demote_l3e()
	 * fails.  In that case, stale TLB entries are immediately
	 * invalidated.
	 */
	
	PMAP_LOCK(pmap);
	for (; sva < eva; sva = va_next) {

		l1e = pmap_pml1e(pmap, sva);
		if ((*l1e & PG_V) == 0) {
			va_next = (sva + L1_PAGE_SIZE) & ~L1_PAGE_MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		l2e = pmap_l1e_to_l2e(l1e, sva);
		if ((*l2e & PG_V) == 0) {
			va_next = (sva + L2_PAGE_SIZE) & ~L2_PAGE_MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		va_next = (sva + L3_PAGE_SIZE) & ~L3_PAGE_MASK;
		if (va_next < sva)
			va_next = eva;

		l3e = pmap_l2e_to_l3e(l2e, sva);
		ptpaddr = *l3e;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Check for large page.
		 */
		if ((ptpaddr & RPTE_LEAF) != 0) {
			/*
			 * Are we protecting the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			printf("protect or demote: %lx\n", *l3e);
			if (sva + L3_PAGE_SIZE == va_next && eva >= va_next) {
				if (pmap_protect_l3e(pmap, l3e, sva, prot))
					anychanged = TRUE;
				continue;
			} else if (!pmap_demote_l3e(pmap, l3e, sva)) {
				/*
				 * The large page mapping was destroyed.
				 */
				continue;
			}
		}

		if (va_next > eva)
			va_next = eva;

		for (pte = pmap_l3e_to_pte(l3e, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			pt_entry_t obits, pbits;
			vm_page_t m;

retry:
			obits = pbits = *pte;
			if ((pbits & PG_V) == 0)
				continue;

			if ((prot & VM_PROT_WRITE) == 0) {
				if ((pbits & (PG_MANAGED | PG_M | PG_RW)) ==
				    (PG_MANAGED | PG_M | PG_RW)) {
					m = PHYS_TO_VM_PAGE(pbits & PG_FRAME);
					vm_page_dirty(m);
				}
				pbits &= ~(PG_RW | PG_M);
			}
			if (prot & VM_PROT_EXECUTE)
				pbits |= PG_X;

			if (pbits != obits) {
				if (!atomic_cmpset_long(pte, obits, pbits))
					goto retry;
				anychanged = TRUE;
			}
		}
	}
	if (anychanged)
		pmap_invalidate_all(pmap);
	PMAP_UNLOCK(pmap);
}

VISIBILITY void
METHOD(qenter) vm_offset_t sva, vm_page_t *ma, int count)
{

	CTR4(KTR_PMAP, "%s(%#x, %p, %d)", __func__, sva, m, count);
	pt_entry_t *endpte, oldpte, pa, *pte;
	vm_page_t m;
	uint64_t cache_bits, attr_bits;

	oldpte = 0;
	pte = vtopte(sva);
	endpte = pte + count;
	cache_bits = 0;
	attr_bits = RPTE_VALID | RPTE_LEAF | RPTE_EAA_R | RPTE_EAA_W | RPTE_EAA_P;
	while (pte < endpte) {
		m = *ma++;
#if 0
		cache_bits = pmap_cache_bits(kernel_pmap, m->md.pat_mode, 0);
#endif	
		pa = VM_PAGE_TO_PHYS(m) | cache_bits | attr_bits;
		if (*pte != pa) {
			oldpte |= *pte;
			pte_store(pte, pa);
		}
		pte++;
	}

	if (__predict_false((oldpte & RPTE_VALID) != 0))
		pmap_invalidate_range(kernel_pmap, sva, sva + count *
		    PAGE_SIZE);
}

VISIBILITY void
METHOD(qremove) vm_offset_t sva, int count)
{
	vm_offset_t va;

	CTR3(KTR_PMAP, "%s(%#x, %d)", __func__, sva, count);

	va = sva;
	while (count-- > 0) {
		KASSERT(va >= VM_MIN_KERNEL_ADDRESS, ("usermode or dmap va %lx", va));
		pmap_kremove(va);
		va += PAGE_SIZE;
	}
	pmap_invalidate_range(kernel_pmap, sva, va);
}

/***************************************************
 * Page table page management routines.....
 ***************************************************/
/*
 * Schedule the specified unused page table page to be freed.  Specifically,
 * add the page to the specified list of pages that will be released to the
 * physical memory manager after the TLB has been updated.
 */
static __inline void
pmap_add_delayed_free_list(vm_page_t m, struct spglist *free,
    boolean_t set_PG_ZERO)
{

	if (set_PG_ZERO)
		m->flags |= PG_ZERO;
	else
		m->flags &= ~PG_ZERO;
	SLIST_INSERT_HEAD(free, m, plinks.s.ss);
}
	
/*
 * Inserts the specified page table page into the specified pmap's collection
 * of idle page table pages.  Each of a pmap's page table pages is responsible
 * for mapping a distinct range of virtual addresses.  The pmap's collection is
 * ordered by this virtual address range.
 */
static __inline int
pmap_insert_pt_page(pmap_t pmap, vm_page_t mpte)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	return (vm_radix_insert(&pmap->pm_root, mpte));
}

/*
 * Removes the page table page mapping the specified virtual address from the
 * specified pmap's collection of idle page table pages, and returns it.
 * Otherwise, returns NULL if there is no page table page corresponding to the
 * specified virtual address.
 */
static __inline vm_page_t
pmap_remove_pt_page(pmap_t pmap, vm_offset_t va)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	return (vm_radix_remove(&pmap->pm_root, pmap_l3e_pindex(va)));
}

/*
 * Decrements a page table page's wire count, which is used to record the
 * number of valid page table entries within the page.  If the wire count
 * drops to zero, then the page table page is unmapped.  Returns TRUE if the
 * page table page was unmapped and FALSE otherwise.
 */
static inline boolean_t
pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m, struct spglist *free)
{

	--m->wire_count;
	if (m->wire_count == 0) {
		_pmap_unwire_ptp(pmap, va, m, free);
		return (TRUE);
	} else
		return (FALSE);
}

static void
_pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m, struct spglist *free)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	/*
	 * unmap the page table page
	 */
	if (m->pindex >= (NUPDE + NUPDPE)) {
		/* PDP page */
		pml1_entry_t *pml1;
		pml1 = pmap_pml1e(pmap, va);
		*pml1 = 0;
	} else if (m->pindex >= NUPDE) {
		/* PD page */
		pml2_entry_t *l2e;
		l2e = pmap_pml2e(pmap, va);
		*l2e = 0;
	} else {
		/* PTE page */
		pml3_entry_t *l3e;
		l3e = pmap_pml3e(pmap, va);
		*l3e = 0;
	}
	pmap_resident_count_dec(pmap, 1);
	if (m->pindex < NUPDE) {
		/* We just released a PT, unhold the matching PD */
		vm_page_t pdpg;

		pdpg = PHYS_TO_VM_PAGE(*pmap_pml2e(pmap, va) & PG_FRAME);
		pmap_unwire_ptp(pmap, va, pdpg, free);
	}
	if (m->pindex >= NUPDE && m->pindex < (NUPDE + NUPDPE)) {
		/* We just released a PD, unhold the matching PDP */
		vm_page_t pdppg;

		pdppg = PHYS_TO_VM_PAGE(*pmap_pml1e(pmap, va) & PG_FRAME);
		pmap_unwire_ptp(pmap, va, pdppg, free);
	}

	/* 
	 * Put page on a list so that it is released after
	 * *ALL* TLB shootdown is done
	 */
	pmap_add_delayed_free_list(m, free, TRUE);
}

/*
 * After removing a page table entry, this routine is used to
 * conditionally free the page, and manage the hold/wire counts.
 */
static int
pmap_unuse_pt(pmap_t pmap, vm_offset_t va, pml3_entry_t ptepde,
    struct spglist *free)
{
	vm_page_t mpte;

	if (va >= VM_MAXUSER_ADDRESS)
		return (0);
	KASSERT(ptepde != 0, ("pmap_unuse_pt: ptepde != 0"));
	mpte = PHYS_TO_VM_PAGE(ptepde & PG_FRAME);
	return (pmap_unwire_ptp(pmap, va, mpte, free));
}

VISIBILITY void
METHOD(release) pmap_t pmap)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, pmap);
	UNIMPLEMENTED();
}

/*
 * Create the PV entry for a 2MB page mapping.  Always returns true unless the
 * flag PMAP_ENTER_NORECLAIM is specified.  If that flag is specified, returns
 * false if the PV entry cannot be allocated without resorting to reclamation.
 */
static bool
pmap_pv_insert_l3e(pmap_t pmap, vm_offset_t va, pml3_entry_t pde, u_int flags,
    struct rwlock **lockp)
{
	struct md_page *pvh;
	pv_entry_t pv;
	vm_paddr_t pa;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	/* Pass NULL instead of the lock pointer to disable reclamation. */
	if ((pv = get_pv_entry(pmap, (flags & PMAP_ENTER_NORECLAIM) != 0 ?
	    NULL : lockp)) == NULL)
		return (false);
	pv->pv_va = va;
	pa = pde & PG_PS_FRAME;
	CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, pa);
	pvh = pa_to_pvh(pa);
	TAILQ_INSERT_TAIL(&pvh->pv_list, pv, pv_next);
	pvh->pv_gen++;
	return (true);
}

/*
 * Fills a page table page with mappings to consecutive physical pages.
 */
static void
pmap_fill_ptp(pt_entry_t *firstpte, pt_entry_t newpte)
{
	pt_entry_t *pte;

	for (pte = firstpte; pte < firstpte + NPTEPG; pte++) {
		*pte = newpte;
		newpte += PAGE_SIZE;
	}
}

static boolean_t
pmap_demote_l3e(pmap_t pmap, pml3_entry_t *pde, vm_offset_t va)
{
	struct rwlock *lock;
	boolean_t rv;

	lock = NULL;
	rv = pmap_demote_l3e_locked(pmap, pde, va, &lock);
	if (lock != NULL)
		rw_wunlock(lock);
	return (rv);
}

static boolean_t
pmap_demote_l3e_locked(pmap_t pmap, pml3_entry_t *l3e, vm_offset_t va,
    struct rwlock **lockp)
{
	pml3_entry_t newpde, oldpde;
	pt_entry_t *firstpte, newpte;
	vm_paddr_t mptepa;
	vm_page_t mpte;
	struct spglist free;
	vm_offset_t sva;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	oldpde = *l3e;
	KASSERT((oldpde & (RPTE_LEAF | PG_V)) == (RPTE_LEAF | PG_V),
			("pmap_demote_l3e: oldpde is missing RPTE_LEAF and/or PG_V %lx", oldpde));
	if ((oldpde & PG_A) == 0 || (mpte = pmap_remove_pt_page(pmap, va)) ==
	    NULL) {
		KASSERT((oldpde & PG_W) == 0,
		    ("pmap_demote_l3e: page table page for a wired mapping"
		    " is missing"));

		/*
		 * Invalidate the 2MB page mapping and return "failure" if the
		 * mapping was never accessed or the allocation of the new
		 * page table page fails.  If the 2MB page mapping belongs to
		 * the direct map region of the kernel's address space, then
		 * the page allocation request specifies the highest possible
		 * priority (VM_ALLOC_INTERRUPT).  Otherwise, the priority is
		 * normal.  Page table pages are preallocated for every other
		 * part of the kernel address space, so the direct map region
		 * is the only part of the kernel address space that must be
		 * handled here.
		 */
		if ((oldpde & PG_A) == 0 || (mpte = vm_page_alloc(NULL,
		    pmap_l3e_pindex(va), (va >= DMAP_MIN_ADDRESS && va <
		    DMAP_MAX_ADDRESS ? VM_ALLOC_INTERRUPT : VM_ALLOC_NORMAL) |
		    VM_ALLOC_NOOBJ | VM_ALLOC_WIRED)) == NULL) {
			SLIST_INIT(&free);
			sva = trunc_2mpage(va);
			pmap_remove_l3e(pmap, l3e, sva, &free, lockp);
			pmap_invalidate_l3e_page(pmap, sva, oldpde);
			vm_page_free_pages_toq(&free, true);
			CTR2(KTR_PMAP, "pmap_demote_l3e: failure for va %#lx"
			    " in pmap %p", va, pmap);
			return (FALSE);
		}
		if (va < VM_MAXUSER_ADDRESS)
			pmap_resident_count_inc(pmap, 1);
	}
	mptepa = VM_PAGE_TO_PHYS(mpte);
	firstpte = (pt_entry_t *)PHYS_TO_DMAP(mptepa);
	newpde = mptepa | PG_M | PG_A | (oldpde & RPTE_EAA_P) | PG_RW | PG_V;
	KASSERT((oldpde & PG_A) != 0,
	    ("pmap_demote_l3e: oldpde is missing PG_A"));
	KASSERT((oldpde & (PG_M | PG_RW)) != PG_RW,
	    ("pmap_demote_l3e: oldpde is missing PG_M"));
	newpte = oldpde & ~PG_PS;

	/*
	 * If the page table page is new, initialize it.
	 */
	if (mpte->wire_count == 1) {
		mpte->wire_count = NPTEPG;
		pmap_fill_ptp(firstpte, newpte);
	}
	KASSERT((*firstpte & PG_FRAME) == (newpte & PG_FRAME),
	    ("pmap_demote_l3e: firstpte and newpte map different physical"
	    " addresses"));

	/*
	 * If the mapping has changed attributes, update the page table
	 * entries.
	 */
	if ((*firstpte & PG_PTE_PROMOTE) != (newpte & PG_PTE_PROMOTE))
		pmap_fill_ptp(firstpte, newpte);

	/*
	 * The spare PV entries must be reserved prior to demoting the
	 * mapping, that is, prior to changing the PDE.  Otherwise, the state
	 * of the PDE and the PV lists will be inconsistent, which can result
	 * in reclaim_pv_chunk() attempting to remove a PV entry from the
	 * wrong PV list and pmap_pv_demote_l3e() failing to find the expected
	 * PV entry for the 2MB page mapping that is being demoted.
	 */
	if ((oldpde & PG_MANAGED) != 0)
		reserve_pv_entries(pmap, NPTEPG - 1, lockp);

	/*
	 * Demote the mapping.  This pmap is locked.  The old PDE has
	 * PG_A set.  If the old PDE has PG_RW set, it also has PG_M
	 * set.  Thus, there is no danger of a race with another
	 * processor changing the setting of PG_A and/or PG_M between
	 * the read above and the store below. 
	 */
	pte_store(l3e, newpde);

	/*
	 * Invalidate a stale recursive mapping of the page table page.
	 */
	if (va >= VM_MAXUSER_ADDRESS)
		pmap_invalidate_page(pmap, (vm_offset_t)vtopte(va));

	/*
	 * Demote the PV entry.
	 */
	if ((oldpde & PG_MANAGED) != 0)
		pmap_pv_demote_l3e(pmap, va, oldpde & PG_PS_FRAME, lockp);

	atomic_add_long(&pmap_l3e_demotions, 1);
	CTR2(KTR_PMAP, "pmap_demote_l3e: success for va %#lx"
	    " in pmap %p", va, pmap);
	return (TRUE);
}

/*
 * pmap_remove_kernel_pde: Remove a kernel superpage mapping.
 */
static void
pmap_remove_kernel_l3e(pmap_t pmap, pml3_entry_t *l3e, vm_offset_t va)
{
	pml3_entry_t newpde;
	vm_paddr_t mptepa;
	vm_page_t mpte;

	KASSERT(pmap == kernel_pmap, ("pmap %p is not kernel_pmap", pmap));
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	mpte = pmap_remove_pt_page(pmap, va);
	if (mpte == NULL)
		panic("pmap_remove_kernel_pde: Missing pt page.");

	mptepa = VM_PAGE_TO_PHYS(mpte);
	newpde = mptepa | PG_M | PG_A | PG_RW | PG_V;

	/*
	 * Initialize the page table page.
	 */
	pagezero((void *)PHYS_TO_DMAP(mptepa));

	/*
	 * Demote the mapping.
	 */
	pte_store(l3e, newpde);

	/*
	 * Invalidate a stale recursive mapping of the page table page.
	 */
	pmap_invalidate_page(pmap, (vm_offset_t)vtopte(va));
}

/*
 * pmap_remove_l3e: do the things to unmap a superpage in a process
 */
static int
pmap_remove_l3e(pmap_t pmap, pml3_entry_t *pdq, vm_offset_t sva,
    struct spglist *free, struct rwlock **lockp)
{
	struct md_page *pvh;
	pml3_entry_t oldpde;
	vm_offset_t eva, va;
	vm_page_t m, mpte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((sva & L3_PAGE_MASK) == 0,
	    ("pmap_remove_l3e: sva is not 2mpage aligned"));
	oldpde = pte_load_clear(pdq);
	if (oldpde & PG_W)
		pmap->pm_stats.wired_count -= (L3_PAGE_SIZE / PAGE_SIZE);
	pmap_resident_count_dec(pmap, L3_PAGE_SIZE / PAGE_SIZE);
	if (oldpde & PG_MANAGED) {
		CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, oldpde & PG_PS_FRAME);
		pvh = pa_to_pvh(oldpde & PG_PS_FRAME);
		pmap_pvh_free(pvh, pmap, sva);
		eva = sva + L3_PAGE_SIZE;
		for (va = sva, m = PHYS_TO_VM_PAGE(oldpde & PG_PS_FRAME);
		    va < eva; va += PAGE_SIZE, m++) {
			if ((oldpde & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(m);
			if (oldpde & PG_A)
				vm_page_aflag_set(m, PGA_REFERENCED);
			if (TAILQ_EMPTY(&m->md.pv_list) &&
			    TAILQ_EMPTY(&pvh->pv_list))
				vm_page_aflag_clear(m, PGA_WRITEABLE);
			pmap_delayed_invl_page(m);
		}
	}
	if (pmap == kernel_pmap) {
		pmap_remove_kernel_l3e(pmap, pdq, sva);
	} else {
		mpte = pmap_remove_pt_page(pmap, sva);
		if (mpte != NULL) {
			pmap_resident_count_dec(pmap, 1);
			KASSERT(mpte->wire_count == NPTEPG,
			    ("pmap_remove_l3e: pte page wire count error"));
			mpte->wire_count = 0;
			pmap_add_delayed_free_list(mpte, free, FALSE);
		}
	}
	return (pmap_unuse_pt(pmap, sva, *pmap_pml2e(pmap, sva), free));
}


/*
 * pmap_remove_pte: do the things to unmap a page in a process
 */
static int
pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t va, 
    pml3_entry_t ptepde, struct spglist *free, struct rwlock **lockp)
{
	struct md_page *pvh;
	pt_entry_t oldpte;
	vm_page_t m;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	oldpte = pte_load_clear(ptq);
	if (oldpte & RPTE_WIRED)
		pmap->pm_stats.wired_count -= 1;
	pmap_resident_count_dec(pmap, 1);
	if (oldpte & RPTE_MANAGED) {
		m = PHYS_TO_VM_PAGE(oldpte & PG_FRAME);
		if ((oldpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			vm_page_dirty(m);
		if (oldpte & PG_A)
			vm_page_aflag_set(m, PGA_REFERENCED);
		CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
		pmap_pvh_free(&m->md, pmap, va);
		if (TAILQ_EMPTY(&m->md.pv_list) &&
		    (m->flags & PG_FICTITIOUS) == 0) {
			pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
			if (TAILQ_EMPTY(&pvh->pv_list))
				vm_page_aflag_clear(m, PGA_WRITEABLE);
		}
		pmap_delayed_invl_page(m);
	}
	return (pmap_unuse_pt(pmap, va, ptepde, free));
}

/*
 * Remove a single page from a process address space
 */
static void
pmap_remove_page(pmap_t pmap, vm_offset_t va, pml3_entry_t *l3e,
    struct spglist *free)
{
	struct rwlock *lock;
	pt_entry_t *pte;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if ((*l3e & RPTE_VALID) == 0)
		return;
	pte = pmap_l3e_to_pte(l3e, va);
	if ((*pte & RPTE_VALID) == 0)
		return;
	lock = NULL;
	pmap_remove_pte(pmap, pte, va, *l3e, free, &lock);
	if (lock != NULL)
		rw_wunlock(lock);
	pmap_invalidate_page(pmap, va);
}

/*
 * Removes the specified range of addresses from the page table page.
 */
static bool
pmap_remove_ptes(pmap_t pmap, vm_offset_t sva, vm_offset_t eva,
    pml3_entry_t *l3e, struct spglist *free, struct rwlock **lockp)
{
	pt_entry_t *pte;
	vm_offset_t va;
	bool anyvalid;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	anyvalid = false;
	va = eva;
	for (pte = pmap_l3e_to_pte(l3e, sva); sva != eva; pte++,
	    sva += PAGE_SIZE) {
		if (*pte == 0) {
			if (va != eva) {
				pmap_invalidate_range(pmap, va, sva);
				va = eva;
			}
			continue;
		}
		if (va == eva)
			va = sva;
		if (pmap_remove_pte(pmap, pte, sva, *l3e, free, lockp)) {
			sva += PAGE_SIZE;
			break;
		}
	}
	if (va != eva)
		pmap_invalidate_range(pmap, va, sva);
	return (anyvalid);
}


static void
mmu_radix_pmap_remove(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	struct rwlock *lock;
	vm_offset_t va_next;
	pml1_entry_t *l1e;
	pml2_entry_t *l2e;
	pml3_entry_t ptpaddr, *l3e;
	struct spglist free;
	int anyvalid;

	CTR4(KTR_PMAP, "%s(%p, %#x, %#x)", __func__, pmap, start, end);

	/*
	 * Perform an unsynchronized read.  This is, however, safe.
	 */
	if (pmap->pm_stats.resident_count == 0)
		return;

	anyvalid = 0;
	SLIST_INIT(&free);

	pmap_delayed_invl_started();
	PMAP_LOCK(pmap);

	/*
	 * special handling of removing one page.  a very
	 * common operation and easy to short circuit some
	 * code.
	 */
	if (sva + PAGE_SIZE == eva) {
		l3e = pmap_pml3e(pmap, sva);
		if (l3e && (*l3e & RPTE_LEAF) == 0) {
			pmap_remove_page(pmap, sva, l3e, &free);
			goto out;
		}
	}

	lock = NULL;
	for (; sva < eva; sva = va_next) {

		if (pmap->pm_stats.resident_count == 0)
			break;

		l1e = pmap_pml1e(pmap, sva);
		if ((*l1e & PG_V) == 0) {
			va_next = (sva + L1_PAGE_SIZE) & ~L1_PAGE_MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		l2e = pmap_l1e_to_l2e(l1e, sva);
		if ((*l2e & PG_V) == 0) {
			va_next = (sva + L2_PAGE_SIZE) & ~L2_PAGE_MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		/*
		 * Calculate index for next page table.
		 */
		va_next = (sva + L3_PAGE_SIZE) & ~L3_PAGE_MASK;
		if (va_next < sva)
			va_next = eva;

		l3e = pmap_l2e_to_l3e(l2e, sva);
		ptpaddr = *l3e;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

		/*
		 * Check for large page.
		 */
		if ((ptpaddr & RPTE_LEAF) != 0) {
			/*
			 * Are we removing the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			printf("valid leaf on remove: %lx\n", ptpaddr);
			if (sva + L3_PAGE_SIZE == va_next && eva >= va_next) {
				pmap_remove_l3e(pmap, l3e, sva, &free, &lock);
				continue;
			} else if (!pmap_demote_l3e_locked(pmap, l3e, sva,
			    &lock)) {
				/* The large page mapping was destroyed. */
				continue;
			} else
				ptpaddr = *l3e;
		}

		/*
		 * Limit our scan to either the end of the va represented
		 * by the current page table page, or to the end of the
		 * range being removed.
		 */
		if (va_next > eva)
			va_next = eva;

		if (pmap_remove_ptes(pmap, sva, va_next, l3e, &free, &lock))
			anyvalid = 1;
	}
	if (lock != NULL)
		rw_wunlock(lock);
out:
	if (anyvalid)
		pmap_invalidate_all(pmap);
	PMAP_UNLOCK(pmap);
	pmap_delayed_invl_finished();
	vm_page_free_pages_toq(&free, true);
}

VISIBILITY void
METHOD(remove) pmap_t pmap, vm_offset_t start, vm_offset_t end)
{

	mmu_radix_pmap_remove(pmap, start, end);
}

VISIBILITY void
METHOD(remove_all) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(remove_pages) pmap_t pmap)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, pmap);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(remove_write) vm_page_t m)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(unwire) pmap_t pmap, vm_offset_t start, vm_offset_t end)
{

	CTR4(KTR_PMAP, "%s(%p, %#x, %#x)", __func__, pmap, start, end);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(zero_page) vm_page_t m)
{
	void *addr;

	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	addr = (void*)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));
	pagezero(addr);
}

VISIBILITY void
METHOD(zero_page_area) vm_page_t m, int off, int size)
{

	CTR4(KTR_PMAP, "%s(%p, %d, %d)", __func__, m, off, size);
	UNIMPLEMENTED();
}

VISIBILITY int
METHOD(mincore) pmap_t pmap, vm_offset_t addr, vm_paddr_t *locked_pa)
{

	CTR3(KTR_PMAP, "%s(%p, %#x)", __func__, pmap, addr);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY void
METHOD(activate) struct thread *td)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, td);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(deactivate) struct thread *td)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, td);
	UNIMPLEMENTED();
}

/*
 *	Increase the starting virtual address of the given mapping if a
 *	different alignment might result in more superpage mappings.
 */
VISIBILITY void
METHOD(align_superpage) vm_object_t object, vm_ooffset_t offset,
    vm_offset_t *addr, vm_size_t size)
{

	CTR5(KTR_PMAP, "%s(%p, %#x, %p, %#x)", __func__, object, offset, addr,
	    size);
	vm_offset_t superpage_offset;

	if (size < L3_PAGE_SIZE)
		return;
	if (object != NULL && (object->flags & OBJ_COLORED) != 0)
		offset += ptoa(object->pg_color);
	superpage_offset = offset & L3_PAGE_MASK;
	if (size - ((L3_PAGE_SIZE - superpage_offset) & L3_PAGE_MASK) < L3_PAGE_SIZE ||
	    (*addr & L3_PAGE_MASK) == superpage_offset)
		return;
	if ((*addr & L3_PAGE_MASK) < superpage_offset)
		*addr = (*addr & ~L3_PAGE_MASK) + superpage_offset;
	else
		*addr = ((*addr + L3_PAGE_MASK) & ~L3_PAGE_MASK) + superpage_offset;
}

VISIBILITY void *
METHOD(mapdev) vm_paddr_t pa, vm_size_t size)
{

	CTR3(KTR_PMAP, "%s(%#x, %#x)", __func__, pa, size);
	UNIMPLEMENTED();
	return (NULL);
}

VISIBILITY void *
METHOD(mapdev_attr) vm_paddr_t pa, vm_size_t size, vm_memattr_t attr)
{

	CTR4(KTR_PMAP, "%s(%#x, %#x, %#x)", __func__, pa, size, attr);
	UNIMPLEMENTED();
	return (NULL);
}

VISIBILITY void
METHOD(page_set_memattr) vm_page_t m, vm_memattr_t ma)
{

	CTR3(KTR_PMAP, "%s(%p, %#x)", __func__, m, ma);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(unmapdev) vm_offset_t va, vm_size_t size)
{

	CTR3(KTR_PMAP, "%s(%#x, %#x)", __func__, va, size);
	UNIMPLEMENTED();
}

VISIBILITY vm_paddr_t
METHOD(kextract) vm_offset_t va)
{
	pml3_entry_t l3e;
	vm_paddr_t pa;

	CTR2(KTR_PMAP, "%s(%#x)", __func__, va);
	if (va >= DMAP_MIN_ADDRESS && va < DMAP_MAX_ADDRESS) {
		pa = DMAP_TO_PHYS(va);
	} else {
		l3e = *pmap_pml3e(kernel_pmap, va);
		if (l3e & RPTE_LEAF) {
			pa = (l3e & PG_PS_FRAME) | (va & L3_PAGE_MASK);
		} else {
			/*
			 * Beware of a concurrent promotion that changes the
			 * PDE at this point!  For example, vtopte() must not
			 * be used to access the PTE because it would use the
			 * new PDE.  It is, however, safe to use the old PDE
			 * because the page table page is preserved by the
			 * promotion.
			 */
			pa = *pmap_l3e_to_pte(&l3e, va);
			pa = (pa & PG_FRAME) | (va & PAGE_MASK);
		}
	}
	return (pa);
}

VISIBILITY void
METHOD(kenter) vm_offset_t va, vm_paddr_t pa)
{

	CTR3(KTR_PMAP, "%s(%#x, %#x)", __func__, va, pa);
	mmu_radix_pmap_kenter(va, pa);
}

VISIBILITY void
METHOD(kenter_attr) vm_offset_t va, vm_paddr_t pa, vm_memattr_t ma)
{

	CTR4(KTR_PMAP, "%s(%#x, %#x, %#x)", __func__, va, pa, ma);
	UNIMPLEMENTED();
}

VISIBILITY void
METHOD(kremove) vm_offset_t va)
{
	pt_entry_t *pte;

	CTR2(KTR_PMAP, "%s(%#x)", __func__, va);

	pte = kvtopte(va);
	pte_clear(pte);
}

VISIBILITY int
METHOD(map_user_ptr) pmap_t pm, volatile const void *uaddr, void **kaddr,
    size_t ulen, size_t *klen)
{

	CTR2(KTR_PMAP, "%s(%p)", __func__, uaddr);
	UNIMPLEMENTED();
}

VISIBILITY int
METHOD(decode_kernel_ptr) vm_offset_t addr, int *is_user, vm_offset_t *decoded)
{

	CTR2(KTR_PMAP, "%s(%#jx)", __func__, (uintmax_t)addr);
	*decoded = addr;
	*is_user = (addr < DMAP_MIN_ADDRESS);
	return (0);
}

VISIBILITY boolean_t
METHOD(dev_direct_mapped) vm_paddr_t pa, vm_size_t size)
{

	CTR3(KTR_PMAP, "%s(%#x, %#x)", __func__, pa, size);
	UNIMPLEMENTED();
	return (false);
}

VISIBILITY void
METHOD(sync_icache) pmap_t pm, vm_offset_t va, vm_size_t sz)
{
 
	CTR4(KTR_PMAP, "%s(%p, %#x, %#x)", __func__, pm, va, sz);
	UNIMPLEMENTED();
}

#ifdef MMU_DIRECT
VISIBILITY void
dumpsys_map_chunk(vm_paddr_t pa, size_t sz, void **va)
{

	CTR4(KTR_PMAP, "%s(%#jx, %#zx, %p)", __func__, (uintmax_t)pa, sz, va);
	UNIMPLEMENTED();
}

void
dumpsys_unmap_chunk(vm_paddr_t pa, size_t sz, void *va)
{

	CTR4(KTR_PMAP, "%s(%#jx, %#zx, %p)", __func__, (uintmax_t)pa, sz, va);
	UNIMPLEMENTED();
}

void
dumpsys_pa_init(void)
{

	CTR1(KTR_PMAP, "%s()", __func__);
	UNIMPLEMENTED();
}
#else
static void
mmu_radix_scan_init(mmu_t mmup)
{

	CTR1(KTR_PMAP, "%s()", __func__);
	UNIMPLEMENTED();
}

static void
mmu_radix_dumpsys_map(mmu_t mmu, vm_paddr_t pa, size_t sz,
	void **va)
{
	CTR4(KTR_PMAP, "%s(%#jx, %#zx, %p)", __func__, (uintmax_t)pa, sz, va);
	UNIMPLEMENTED();	
}

#endif

VISIBILITY vm_offset_t
METHOD(quick_enter_page) vm_page_t m)
{
	CTR2(KTR_PMAP, "%s(%p)", __func__, m);
	UNIMPLEMENTED();
	return (0);
}

VISIBILITY void
METHOD(quick_remove_page) vm_offset_t addr)
 {

	CTR2(KTR_PMAP, "%s(%#x)", __func__, addr);
	UNIMPLEMENTED();
}

VISIBILITY int
METHOD(change_attr) vm_offset_t addr, vm_size_t size, vm_memattr_t mode)
{
	CTR4(KTR_PMAP, "%s(%#x, %#zx, %d)", __func__, addr, size, mode);
	UNIMPLEMENTED();
	return (0);
}

#ifdef MMU_DIRECT
boolean_t
pmap_is_valid_memattr(pmap_t pmap __unused, vm_memattr_t mode)
{

	switch (mode) {
	case VM_MEMATTR_DEFAULT:
	case VM_MEMATTR_UNCACHEABLE:
	case VM_MEMATTR_CACHEABLE:
	case VM_MEMATTR_WRITE_COMBINING:
	case VM_MEMATTR_WRITE_BACK:
	case VM_MEMATTR_WRITE_THROUGH:
	case VM_MEMATTR_PREFETCHABLE:
		return (TRUE);
	default:
		return (FALSE);
	}
}
#endif
