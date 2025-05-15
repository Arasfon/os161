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

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

/*
 * Address space structure and operations.
 */


#include <vm.h>
#include <spinlock.h>
#include "opt-dumbvm.h"

struct vnode;

/* Page table entry state constants */
#define PTE_STATE_UNALLOC	0	/* Page not yet allocated; any reference forces it to be allocated and zero filled */
#define PTE_STATE_ZERO		1	/* Allocated but has never been written; can be satisfied by zero-fill */
#define PTE_STATE_RAM		2	/* Page resident in memory, pte.pfn valid */
#define PTE_STATE_SWAP		3	/* Non-resident; contents live in swap_slot */

/* VPN extraction macros */
#define VPN(vaddr) ((vaddr) >> 12)
#define L1_INDEX(vaddr) (VPN(vaddr) >> 10)
#define L2_INDEX(vaddr) (VPN(vaddr) & 0x3FF)

/* Page table entry structure - each entry is 32 bits */
struct pte {
	uint32_t pfn;	/* Physical frame number when in RAM */
	uint32_t swap_slot;	/* Swap slot number when swapped */
	uint8_t state;	/* UNALLOC/ZERO/RAM/SWAP */
	uint8_t dirty;	/* Set when page is modified */
	uint8_t readonly;	/* Set for read-only pages */
};

/* Size of first and second level page tables */
#define PT_L1_SIZE 1024 /* 10 bits -> 4 KiB L1 table */
#define PT_L2_SIZE 1024 /* 10 bits -> 4 KiB L2 page */

/* Number of pages in user stack */
#define STACKPAGES 18 /* 16 pages = 64KB, 2 extra pages to allow 64KB argv in the tests */

/* Region flags */
struct region {
	vaddr_t vbase; /* Base virtual address */
	size_t npages; /* Number of pages in region */
	int readable; /* Read permission */
	int writeable; /* Write permission */
	int executable; /* Execute permission */
	struct region *next; /* Next region in linked list */
};

/*
 * Address space - data structure associated with the virtual memory
 * space of a process.
 */
struct addrspace {
#if OPT_DUMBVM
        vaddr_t as_vbase1;
        paddr_t as_pbase1;
        size_t as_npages1;
        vaddr_t as_vbase2;
        paddr_t as_pbase2;
        size_t as_npages2;
        paddr_t as_stackpbase;
#else
		/* Two-level page table - dynamically allocated */
		struct pte **pt_l1; /* First level page table */

		/* List of memory regions */
		struct region *regions;

		/* Heap management */
		vaddr_t heap_start; /* Start address of heap */
		vaddr_t heap_end; /* Current end address of heap (break) */

		/* Lock for page table operations */
		struct spinlock pt_lock;
#endif
};

/*
 * Functions to manage page tables
 */
struct pte *pt_get_pte(struct addrspace *as, vaddr_t vaddr, bool create);
int pt_alloc_l2(struct addrspace *as, int l1_index);

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_copy   - create a new address space that is an exact copy of
 *                an old one. Probably calls as_create to get a new
 *                empty address space and fill it in, but that's up to
 *                you.
 *
 *    as_activate - make curproc's address space the one currently
 *                "seen" by the processor.
 *
 *    as_deactivate - unload curproc's address space so it isn't
 *                currently "seen" by the processor. This is used to
 *                avoid potentially "seeing" it while it's being
 *                destroyed.
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_prepare_load - this is called before actually loading from an
 *                executable into the address space.
 *
 *    as_complete_load - this is called when loading from an executable
 *                is complete.
 *
 *    as_define_stack - set up the stack region in the address space.
 *                (Normally called *after* as_complete_load().) Hands
 *                back the initial stack pointer for the new process.
 *
 * Note that when using dumbvm, addrspace.c is not used and these
 * functions are found in dumbvm.c.
 */

struct addrspace *as_create(void);
int               as_copy(struct addrspace *src, struct addrspace **ret);
void              as_activate(void);
void              as_deactivate(void);
void              as_destroy(struct addrspace *);

int               as_define_region(struct addrspace *as,
                                   vaddr_t vaddr, size_t sz,
                                   int readable,
                                   int writeable,
                                   int executable);
int               as_prepare_load(struct addrspace *as);
int               as_complete_load(struct addrspace *as);
int               as_define_stack(struct addrspace *as, vaddr_t *initstackptr);


/*
 * Functions in loadelf.c
 *    load_elf - load an ELF user program executable into the current
 *               address space. Returns the entry point (initial PC)
 *               in the space pointed to by ENTRYPOINT.
 */

int load_elf(struct vnode *v, vaddr_t *entrypoint);

#endif /* _ADDRSPACE_H_ */
