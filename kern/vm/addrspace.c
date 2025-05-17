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
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <mips/vm.h>
#include <syscall.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

/*
 * Helper function to round up to the nearest page boundary
 */
static inline vaddr_t
page_align(vaddr_t addr)
{
	return ROUNDUP(addr, PAGE_SIZE);
}

/*
 * Allocate a second-level page table
 */
int
pt_alloc_l2(struct addrspace *as, int l1_index)
{
	struct pte *alloc;
	char lock_name[] = "pte_lock";

	KASSERT(as != NULL);
	KASSERT(as->pt_l1 != NULL);
	KASSERT(l1_index >= 0 && l1_index < PT_L1_SIZE);

	/* Check if already allocated - assumes caller does NOT hold the lock */
	if (as->pt_l1[l1_index] != NULL) {
		return 0; /* Already allocated */
	}

	/* Allocate memory for the L2 page table without holding the lock */
	alloc = kmalloc(PT_L2_SIZE * sizeof(struct pte));
	if (alloc == NULL) {
		return ENOMEM;
	}

	for (int i = 0; i < PT_L2_SIZE; i++) {
		alloc[i].state = PTE_STATE_UNALLOC;
		alloc[i].pfn = 0;
		alloc[i].swap_slot = 0;
		alloc[i].dirty = 0;
		alloc[i].readonly = 0;
		alloc[i].referenced = 0;

		/* Create a lock for each page table entry */
		//snprintf(lock_name, sizeof(lock_name), "pte_lock_%d_%d", l1_index, i);
		alloc[i].pte_lock = lock_create(lock_name);
		if (alloc[i].pte_lock == NULL) {
			/* Clean up already allocated locks */
			for (int j = 0; j < i; j++) {
				lock_destroy(alloc[j].pte_lock);
			}
			kfree(alloc);
			return ENOMEM;
		}
	}

	/* Acquire lock and check if another thread beat us */
	spinlock_acquire(&as->pt_lock);

	if (as->pt_l1[l1_index] != NULL) {
		/* Another thread allocated this L2 table first */
		spinlock_release(&as->pt_lock);

		/* Clean up all locks and free the allocated memory */
		for (int i = 0; i < PT_L2_SIZE; i++) {
			lock_destroy(alloc[i].pte_lock);
		}
		kfree(alloc);

		return 0;
	}

	as->pt_l1[l1_index] = alloc;

	spinlock_release(&as->pt_lock);

	return 0;
}

/*
 * Get a page table entry for a virtual address
 * If create is true, allocate page tables as needed
 */
struct pte *
pt_get_pte(struct addrspace *as, vaddr_t vaddr, bool create)
{
	struct pte **l1ptr;
	int l1_index, l2_index;
	int result;

	KASSERT(as != NULL);

	l1_index = L1_INDEX(vaddr);
	l2_index = L2_INDEX(vaddr);

	/* Check if we need to allocate the L1 page table */
	if (as->pt_l1 == NULL) {
		if (!create) {
			return NULL;
		}

		/* Allocate L1 table - don't hold lock */
		l1ptr = kmalloc(PT_L1_SIZE * sizeof(struct pte *));
		if (l1ptr == NULL) {
			return NULL;
		}

		for (int i = 0; i < PT_L1_SIZE; i++) {
			l1ptr[i] = NULL;
		}

		/* Acquire lock and check if another thread beat us */
		spinlock_acquire(&as->pt_lock);

		if (as->pt_l1 != NULL) {
			/* Another thread created the L1 table first */
			spinlock_release(&as->pt_lock);
			kfree(l1ptr);

			/* Try again with the new L1 table */
			return pt_get_pte(as, vaddr, create);
		}

		as->pt_l1 = l1ptr;

		spinlock_release(&as->pt_lock);
	}

	/* Check if we need to allocate the L2 page table */
	if (as->pt_l1[l1_index] == NULL) {
		if (!create) {
			return NULL;
		}

		result = pt_alloc_l2(as, l1_index);
		if (result) {
			return NULL;
		}
	}

	/*
	 * We still need to use the spinlock to safely access the L1 table,
	 * but afterwards we can use the individual PTE lock for the specific entry
	 */
	spinlock_acquire(&as->pt_lock);

	/* Double check that the L2 table exists after acquiring the lock */
	if (as->pt_l1[l1_index] == NULL) {
		/* Something went wrong - the L2 table should exist by now */
		spinlock_release(&as->pt_lock);
		return NULL;
	}

	struct pte *pte = &as->pt_l1[l1_index][l2_index];

	/* Note that we don't acquire the PTE lock here
	 * This function only returns the PTE, it doesn't modify it
	 * The caller should acquire the PTE lock if it needs to modify the PTE
	 */

	spinlock_release(&as->pt_lock);

	return pte;
}

/*
 * Address space creation
 */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->pt_l1 = NULL;
	as->regions = NULL;
	as->heap_start = 0;
	as->heap_end = 0;
	spinlock_init(&as->pt_lock);

	return as;
}

/*
 * Create a copy of the address space
 */
int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;
	struct region *old_region, *new_region, *prev_region;

	new = as_create();
	if (new == NULL) {
		return ENOMEM;
	}

	new->heap_start = old->heap_start;
	new->heap_end = old->heap_end;

	/* Copy regions */
	prev_region = NULL;
	for (old_region = old->regions; old_region != NULL; old_region = old_region->next) {
		new_region = kmalloc(sizeof(struct region));

		if (new_region == NULL) {
			as_destroy(new);
			return ENOMEM;
		}

		new_region->vbase = old_region->vbase;
		new_region->npages = old_region->npages;
		new_region->readable = old_region->readable;
		new_region->writeable = old_region->writeable;
		new_region->executable = old_region->executable;
		new_region->next = NULL;

		if (prev_region == NULL) {
			new->regions = new_region;
		} else {
			prev_region->next = new_region;
		}

		prev_region = new_region;
	}

	/* Copy page tables */
	if (old->pt_l1 != NULL) {
		/* Allocate L1 page table */
		new->pt_l1 = kmalloc(PT_L1_SIZE * sizeof(struct pte *));
		if (new->pt_l1 == NULL) {
			as_destroy(new);
			return ENOMEM;
		}

		for (int i = 0; i < PT_L1_SIZE; i++) {
			new->pt_l1[i] = NULL;
		}

		/* Copy page tables and allocate pages */
		for (int i = 0; i < PT_L1_SIZE; i++) {
			if (old->pt_l1[i] != NULL) {
				/* Allocate L2 page table (this also initializes all the PTE locks) */
				int result = pt_alloc_l2(new, i);
				if (result) {
					as_destroy(new);
					return result;
				}

				/* Copy page table entries */
				for (int j = 0; j < PT_L2_SIZE; j++) {
					struct pte *old_pte = &old->pt_l1[i][j];
					struct pte *new_pte = &new->pt_l1[i][j];

					/* Lock both old and new PTEs while copying */
					lock_acquire(old_pte->pte_lock);
					lock_acquire(new_pte->pte_lock);

					KASSERT(new_pte->state == PTE_STATE_UNALLOC);

					if (old_pte->state == PTE_STATE_RAM) {
						unsigned idx = alloc_upage(new, i * PT_L2_SIZE * PAGE_SIZE + j * PAGE_SIZE);
						if (idx == 0) {
							/* Release locks before destroying the address space */
							lock_release(new_pte->pte_lock);
							lock_release(old_pte->pte_lock);
							as_destroy(new);
							return ENOMEM;
						}

						new_pte->state = old_pte->state;
						new_pte->readonly = old_pte->readonly;
						new_pte->referenced = old_pte->referenced;

						paddr_t pa_old = old_pte->pfn * PAGE_SIZE;
						vaddr_t kv_old = PADDR_TO_KVADDR(pa_old);

						paddr_t pa_new = idx_to_pa(idx);
						vaddr_t kv_new = PADDR_TO_KVADDR(pa_new);

						memmove((void*)kv_new, (void*)kv_old, PAGE_SIZE);

						new_pte->pfn = idx;
						new_pte->dirty = old_pte->dirty;
					}
					else if (old_pte->state == PTE_STATE_SWAP) {
						/* Allocate a new swap slot for the page */
						unsigned new_swap_idx;
						int result = swap_alloc(&new_swap_idx);
						if (result) {
							lock_release(new_pte->pte_lock);
							lock_release(old_pte->pte_lock);
							as_destroy(new);
							return ENOMEM;
						}

						/* Allocate a temporary physical page to hold the data */
						unsigned idx = alloc_upage(new, i * PT_L2_SIZE * PAGE_SIZE + j * PAGE_SIZE);
						if (idx == 0) {
							swap_free(new_swap_idx);
							lock_release(new_pte->pte_lock);
							lock_release(old_pte->pte_lock);
							as_destroy(new);
							return ENOMEM;
						}

						paddr_t temp_paddr = idx_to_pa(idx);
						result = swap_in(temp_paddr, old_pte->swap_slot);
						if (result) {
							free_upage(idx);
							swap_free(new_swap_idx);
							lock_release(new_pte->pte_lock);
							lock_release(old_pte->pte_lock);
							as_destroy(new);
							return result;
						}

						result = swap_out(temp_paddr, new_swap_idx);
						if (result) {
							free_upage(idx);
							swap_free(new_swap_idx);
							lock_release(new_pte->pte_lock);
							lock_release(old_pte->pte_lock);
							as_destroy(new);
							return result;
						}

						free_upage(idx);

						/* Set up the new PTE to point to swap */
						new_pte->state = PTE_STATE_SWAP;
						new_pte->readonly = old_pte->readonly;
						new_pte->swap_slot = new_swap_idx;
						new_pte->referenced = 0;
					}
					else if (old_pte->state == PTE_STATE_ZERO) {
						new_pte->state = PTE_STATE_ZERO;
						new_pte->readonly = old_pte->readonly;
					}

					/* Release the locks */
					lock_release(new_pte->pte_lock);
					lock_release(old_pte->pte_lock);
				}
			}
		}
	}

	*ret = new;

	return 0;
}

/*
 * Destroy an address space
 */
void
as_destroy(struct addrspace *as)
{
	struct region *reg, *next;

	KASSERT(as != NULL);

	/* Clean up the regions */
	for (reg = as->regions; reg != NULL; reg = next) {
		next = reg->next;
		kfree(reg);
	}

	/* Clean up page tables, freeing memory when needed */
	if (as->pt_l1 != NULL) {
		for (int i = 0; i < PT_L1_SIZE; i++) {
			if (as->pt_l1[i] != NULL) {
				for (int j = 0; j < PT_L2_SIZE; j++) {
					struct pte *pte = &as->pt_l1[i][j];

					if (pte->pte_lock != NULL) {
						lock_acquire(pte->pte_lock);
					}

					/* Free physical memory for pages in RAM */
					if (pte->state == PTE_STATE_RAM) {
						free_upage(pte->pfn);
					}
					/* Free swap slots for pages in swap */
					else if (pte->state == PTE_STATE_SWAP) {
						swap_free(pte->swap_slot);
					}

					/* Free the PTE lock */
					if (pte->pte_lock != NULL) {
						lock_release(pte->pte_lock);
						lock_destroy(pte->pte_lock);
					}
				}
				kfree(as->pt_l1[i]);
			}
		}
		kfree(as->pt_l1);
	}

	spinlock_cleanup(&as->pt_lock);

	kfree(as);
}

/*
 * Activate an address space
 */
void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/* Disable interrupts for TLB operations */
	int spl = splhigh();

	/*
	 * We don't track individual pages here, so we simply invalidate
	 * every TLB slot. That guarantees no stale entries remain from
	 * the previous address space. A more efficient design would
	 * only evict entries for pages we're actually changing.
	 */
	for (int i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	/* Restore interrupts */
	splx(spl);
}

/*
 * Deactivate an address space
 */
void
as_deactivate(void)
{
	/* Nothing to do - we invalidate TLB in as_activate */
}

/*
 * Set up a segment (memory region) in the address space
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	struct region *reg;

	KASSERT(as != NULL);

	/* Adjust the size to page-align */
	sz = page_align(sz + (vaddr & ~(vaddr_t)PAGE_FRAME));
	vaddr &= PAGE_FRAME;

	size_t npages = sz / PAGE_SIZE;

	reg = kmalloc(sizeof(struct region));
	if (reg == NULL) {
		return ENOMEM;
	}

	reg->vbase = vaddr;
	reg->npages = npages;
	reg->readable = readable;
	reg->writeable = writeable;
	reg->executable = executable;

	/* Add to region list */
	reg->next = as->regions;
	as->regions = reg;

	/* Update heap_start if not set yet */
	if (as->heap_start == 0) {
		as->heap_start = vaddr + sz;
		as->heap_end = as->heap_start;
	} else {
		/* Update heap_start if this region ends after current heap_start */
		vaddr_t region_end = vaddr + sz;
		if (region_end > as->heap_start) {
			as->heap_start = region_end;
			as->heap_end = as->heap_start;
		}
	}

	return 0;
}

/*
 * Prepare an address space for loading
 */
int
as_prepare_load(struct addrspace *as)
{
	/*
	* Mark all pages in defined regions as ZERO, but leave them
	* writable during the load. We'll fix up read-only flags
	* later in as_complete_load().
	*/

	KASSERT(as != NULL);

	struct region *reg;
	vaddr_t vaddr;
	size_t i;
	struct pte *pte;

	for (reg = as->regions; reg != NULL; reg = reg->next) {
		vaddr = reg->vbase;

		for (i = 0; i < reg->npages; i++, vaddr += PAGE_SIZE) {
			/* Create PTE */
			pte = pt_get_pte(as, vaddr, true);
			if (pte == NULL) {
				return ENOMEM;
			}

			lock_acquire(pte->pte_lock);

			KASSERT(pte->state == PTE_STATE_UNALLOC || pte->state == PTE_STATE_ZERO);

			pte->state = PTE_STATE_ZERO;
			/* leave writable during load, override later */
			pte->readonly = false;

			lock_release(pte->pte_lock);
		}
	}

	return 0;
}

/*
 * Complete the loading of an address space
 */
int
as_complete_load(struct addrspace *as)
{
	/*
	 * Now that load_segment/load_elf has copied all code & data
	 * into the ZERO pages, walk every region again and set the
	 * readonly flag according to the original as->regions info.
	*/

	KASSERT(as != NULL);

	struct region *reg;
	vaddr_t vaddr;
	size_t i;
	struct pte *pte;

	for (reg = as->regions; reg != NULL; reg = reg->next) {
		vaddr = reg->vbase;

		for (i = 0; i < reg->npages; i++, vaddr += PAGE_SIZE) {
			/* Lookup the PTE; it must already exist */
			pte = pt_get_pte(as, vaddr, false);
			if (pte == NULL) {
				/* shouldn't happen if prepare_load succeeded */
				continue;
			}

			lock_acquire(pte->pte_lock);

			KASSERT(pte->state == PTE_STATE_ZERO || pte->state == PTE_STATE_RAM);

			pte->readonly = !reg->writeable;

			lock_release(pte->pte_lock);
		}
	}

	return 0;
}

/*
 * Set up the stack region in the address space
 */
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
    vaddr_t old_hstart = as->heap_start;
    vaddr_t old_hend = as->heap_end;

	/* Define the user stack region */
	int result = as_define_region(as,
		USERSTACK - STACKPAGES * PAGE_SIZE,
		STACKPAGES * PAGE_SIZE,
		1, /* readable */
		1, /* writable */
		0  /* not executable */);

	if (result) {
		return result;
	}

    as->heap_start = old_hstart;
    as->heap_end = old_hend;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}
