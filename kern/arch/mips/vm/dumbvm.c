/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

#ifdef OPT_A3
#include <syscall.h>
#endif

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground. You should replace all of this
 * code while doing the VM assignment. In fact, starting in that
 * assignment, this file is not included in your kernel!
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#ifdef OPT_A3
// indicated whether the vm has been bootstrapped
static bool vm_bootstrapped = false;
struct coremap_val *coremap = NULL;
paddr_t first_free_paddr;
int num_frames;
int pages_used = 0;
#endif

void
vm_bootstrap(void)
{
#ifdef OPT_A3
	paddr_t first_paddr, last_paddr;
	ram_getsize(&first_paddr, &last_paddr);

	// some fumbling calculations needed here.. 
	// 512 MB max memory ==> 
	// 2 frames for virtual pointer addresses
	int reserved = 4;
	num_frames = (last_paddr - first_paddr) / PAGE_SIZE - reserved;

	DEBUG(DB_AWESOME_VM, "PAGES: %u\n", num_frames);
	DEBUG(DB_AWESOME_VM, "BOOTSTRAP: %u %u\n", first_paddr, last_paddr);

	// initialize the coremap
	coremap = (struct coremap_val *)PADDR_TO_KVADDR(first_paddr);
	for (int i =0; i<num_frames; i++) {
		coremap[i].addrspace = NULL;
		coremap[i].used = false;
		coremap[i].continuous = 0;
	}

	// the first free available physical address
	first_free_paddr = first_paddr + reserved * PAGE_SIZE;
	DEBUG(DB_AWESOME_VM, "FIRST FREE: %u\n", first_free_paddr);

	vm_bootstrapped = true;
#else
	/* Do nothing. */
#endif
}

#ifdef OPT_A3
// allocate some continuous physical frames!
static
paddr_t
get_frames(long npages)
{
	spinlock_acquire(&stealmem_lock);	
	
	// no more memory
	if (pages_used + npages >= num_frames) {
		spinlock_release(&stealmem_lock);
		return 0;
	}

	int start_frame = 0;
	int continuous_pages = 0;

	while (continuous_pages < npages) {
		int end_frame = start_frame + continuous_pages;

		if (end_frame == num_frames) {
			continuous_pages = 0;
			break;
		}

		if (coremap[end_frame].used) {
			continuous_pages = 0;
			start_frame = end_frame + 1;
			continue;
		}

		continuous_pages ++;
	}

	// no continuous memory segment found
	if (continuous_pages == 0) {
		spinlock_release(&stealmem_lock);
		return 0;
	}

	pages_used += npages;

	struct addrspace *addrspace = curproc_getas();

	DEBUG(DB_AWESOME_VM, "%u FRAMES FOUND: %u TO %u - PROC %x\n", (int)npages, start_frame, start_frame + continuous_pages, (int)addrspace);

	// mark this frame as used
	for(int i = 0; i < continuous_pages; i++) {
		coremap[start_frame + i].used = true;
		coremap[start_frame + i].addrspace = addrspace;

		if (i == 0) {
			coremap[start_frame + i].continuous = npages;
		} else {
			coremap[start_frame + i].continuous = 0;
		}
	}

	paddr_t paddr = first_free_paddr + PAGE_SIZE * start_frame;
	
	spinlock_release(&stealmem_lock);
	return paddr;
}

static
void
free_frames(paddr_t paddr) {
	spinlock_acquire(&stealmem_lock);

	KASSERT((paddr - first_free_paddr) % PAGE_SIZE == 0);

	unsigned int frame = (paddr - first_free_paddr) / PAGE_SIZE;

	for (unsigned int i =0; i < coremap[frame].continuous; i++) {
		// clear the frame
		coremap[frame].addrspace = NULL;
		coremap[frame].used = false; 
		coremap[frame].continuous = 0;
		pages_used -= coremap[frame].continuous;
	}
	spinlock_release(&stealmem_lock);
}

/* Allocates some memory used for the virtual -> page frame mapping
 * 
*/
static
paddr_t*
make_page_table(int npages)
{
	paddr_t *table;
	// alloc mem for table pointers
	table = (paddr_t *)kmalloc(npages * sizeof(paddr_t *));

	// get a free frame, does not have to be contiguous physically
	for (int i =0; i<npages; i++) {
		table[i] = get_frames(1);

		if (table[i] == 0) {
			// free the memory
			for (int k=0; k<i; k++) {
				free_frames(table[i]);
			}
			kfree(table);
			return 0;
		}
	}

	return table;
}

#endif

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;

#ifdef OPT_A3
	if (vm_bootstrapped) {
		pa = get_frames(npages);
	} else {
		pa = getppages(npages);
	}
#else
	pa = getppages(npages);
#endif

	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
#ifdef OPT_A3
	// since the physical address for the kernel is - 0x80000000 
	free_frames(KVADDR_TO_PADDR(addr));
#else
	/* nothing - leak the memory. */

	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

#ifdef OPT_A3
	bool can_write = true;
#endif

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#ifdef OPT_A3
	    // exit the process
	    sys__exit(0);
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
#ifdef OPT_A3
	//KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	/*
	for (unsigned int i = 0; i< as->as_npages1; i++) {
		KASSERT((as->page_table1[i] & PAGE_FRAME) == as->page_table1[i]);
	}
	*/
#else
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
#endif

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {

#ifdef OPT_A3
		int page_table_idx = (faultaddress - vbase1) / PAGE_SIZE;
		paddr = as->page_table1[page_table_idx];
		can_write = false; // not dirtiable
#else
		paddr = (faultaddress - vbase1) + as->as_pbase1;
#endif
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		
#ifdef OPT_A3
		int page_table_idx = (faultaddress - vbase2) / PAGE_SIZE;
		paddr = as->page_table2[page_table_idx];
#else
		paddr = (faultaddress - vbase2) + as->as_pbase2;
#endif
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		
#ifdef OPT_A3
		int page_table_idx = (faultaddress - stackbase) / PAGE_SIZE;
		paddr = as->page_table_stack[page_table_idx];
#else
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
#endif
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
#ifdef OPT_A3
	// whether it can write or not; true if it's loading the segments
	int dirty_mask = (can_write || as->is_loading) ? TLBLO_DIRTY : 0;
#endif

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
#ifdef OPT_A3
		elo = paddr | dirty_mask | TLBLO_VALID;
#else
		elo = paddr | TLBLO_DIRTY |TLBLO_VALID;
#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

#ifdef OPT_A3
	// invalidate a TLB entry to store the new entry
	elo = paddr | dirty_mask | TLBLO_VALID;
	tlb_random(faultaddress, elo);
	splx(spl);
	return 0;
#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}


#ifdef OPT_A3
	as->as_vbase1 = 0;
	as->as_npages1 = 0;

	as->as_vbase2 = 0;
	as->as_npages2 = 0;

	as->is_loading = false;
	as->page_table1 = NULL;
	as->page_table2 = NULL;
	as->page_table_stack = NULL;
#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{

#ifdef OPT_A3
	spinlock_acquire(&stealmem_lock);	

	DEBUG(DB_AWESOME_VM, "Freeing address at 0x%x\n", (int)as);

	for (int i =0; i< num_frames; i++) {
		if (coremap[i].addrspace == as) {
			coremap[i].addrspace = NULL;
			coremap[i].used = false;
			coremap[i].continuous = 0;
			pages_used --;
		}
	}
	spinlock_release(&stealmem_lock);

	kfree(as->page_table1);
	kfree(as->page_table2);
	kfree(as->page_table_stack);

	as->page_table1 = NULL;
	as->page_table2 = NULL;
	as->page_table_stack = NULL;

#else
	kfree(as);
#endif
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t *paddr_table, unsigned npages)
{
#ifdef OPT_A3
	for (unsigned int i =0; i<npages; i++) {
		bzero((void *)PADDR_TO_KVADDR(paddr_table[i]), PAGE_SIZE);
	}
#else
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
#endif
}

int
as_prepare_load(struct addrspace *as)
{

#ifdef OPT_A3

	KASSERT(as->page_table1 == NULL);
	KASSERT(as->page_table2 == NULL);
	KASSERT(as->page_table_stack == NULL);

	as->page_table1 = make_page_table(as->as_npages1);
	if (as->page_table1 == 0) {
		return ENOMEM;
	}

	as->page_table2 = make_page_table(as->as_npages2);
	if (as->page_table2 == 0) {
		return ENOMEM;
	}

	as->page_table_stack = make_page_table(DUMBVM_STACKPAGES);
	if (as->page_table_stack == 0) {
		return ENOMEM;
	}

	as_zero_region(as->page_table1, as->as_npages1);
	as_zero_region(as->page_table2, as->as_npages2);
	as_zero_region(as->page_table_stack, DUMBVM_STACKPAGES);

	as->is_loading = true;

#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
#ifdef OPT_A3
	as->is_loading = false;
#else
	(void)as;
#endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#ifdef OPT_A3
	KASSERT(as->page_table_stack != NULL);
#else
	KASSERT(as->as_stackpbase != 0);
#endif

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

#ifdef OPT_A3
	// TODO

	// move page by page
	KASSERT(new->page_table1 != 0);
	KASSERT(new->page_table2 != 0);
	KASSERT(new->page_table_stack != 0);

	for (unsigned int page = 0; page < old->as_npages1; page ++) {
		memmove((void *)PADDR_TO_KVADDR(new->page_table1[page]),
			(const void *)PADDR_TO_KVADDR(old->page_table1[page]),
			PAGE_SIZE);
	}

	for (unsigned int page = 0; page < old->as_npages2; page ++) {
		memmove((void *)PADDR_TO_KVADDR(new->page_table2[page]),
			(const void *)PADDR_TO_KVADDR(old->page_table2[page]),
			PAGE_SIZE);
	}

	for (unsigned int page = 0; page < DUMBVM_STACKPAGES; page ++) {
		memmove((void *)PADDR_TO_KVADDR(new->page_table_stack[page]),
			(const void *)PADDR_TO_KVADDR(old->page_table_stack[page]),
			PAGE_SIZE);
	}

#else
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif

	*ret = new;
	return 0;
}
