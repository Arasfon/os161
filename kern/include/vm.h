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

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */


#include <machine/vm.h>
#include <spinlock.h>

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

#define KVADDR_TO_PADDR(vaddr) ((vaddr) - MIPS_KSEG0)

/* Coremap */

enum cm_state {
	CM_FREE, /* page available */
	CM_FIXED, /* kernel / coremap / other wired page */
	CM_USER, /* page owned by a user address-space */
	CM_EVICTING /* page is currently being evicted to swap */
};

struct coremap_entry {
	enum cm_state state; /* allocation state */
	uint16_t chunk_len; /* run length if first page; else 0 */
	struct addrspace *as; /* owning address-space (CM_USER) */
	uint32_t vpn; /* user virtual page number */
};

/* Swap space management */
struct swapmap {
	struct bitmap *swap_bitmap; /* Tracks used/free swap slots */
	struct spinlock swap_lock; /* Lock for swap operations */
	struct vnode *swap_vnode; /* VNode for the swap device */
	unsigned swap_size; /* Total number of swap slots */
};

/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);

unsigned alloc_upage(struct addrspace *as, vaddr_t vaddr);
void free_upage(unsigned idx);

/* Coremap dump for statistics */
void coremap_dump(void);

/*
 * Return amount of memory (in bytes) used by allocated coremap pages.  If
 * there are ongoing allocations, this value could change after it is returned
 * to the caller. But it should have been correct at some point in time.
 */
unsigned int coremap_used_bytes(void);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *);

/* Invalidate TLB entry for specific vaddr */
void tlb_invalidate(vaddr_t vaddr);

/* Swap initialization and operations */
int swap_init(void);
int swap_alloc(unsigned *idx);
void swap_free(unsigned idx);
int swap_out(paddr_t paddr, unsigned idx);
int swap_in(paddr_t paddr, unsigned idx);

/* Functions for page eviction to swap */
int vm_mark_page_evicting(unsigned idx);
void vm_eviction_finished(unsigned idx);
int vm_find_eviction_victim(unsigned *idx_ret);

/* Evict a page and free up physical memory */
int vm_evict_page(unsigned *idx_ret);

// Convert page-frame index to physical address
static
inline
paddr_t
idx_to_pa(unsigned idx)
{
	return (paddr_t)idx * PAGE_SIZE;
}

// Convert physical address → page-frame index
static
inline
unsigned
pa_to_idx(paddr_t pa)
{
	return (unsigned)(pa / PAGE_SIZE);
}

#endif /* _VM_H_ */
