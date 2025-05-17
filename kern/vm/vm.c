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
#include <current.h>

static struct coremap_entry *coremap = NULL; /* KSEG0 pointer */
static unsigned coremap_pages = 0; /* total entries */
static struct spinlock cm_lock = SPINLOCK_INITIALIZER;
static bool vm_ready = false;

/* Must be callable with interrupts on; panics if caller is in an IRQ
 * or already holding a spinlock. Same semantics as dumbvm_can_sleep. */
static
void
vm_can_sleep(void)
{
	if (CURCPU_EXISTS()) {
		KASSERT(curcpu->c_spinlocks == 0);
		KASSERT(curthread->t_in_interrupt == 0);
	}
}

void
coremap_dump(void)
{
	unsigned free = 0, fixed = 0, user = 0, evicting = 0;

	for (unsigned i = 0; i < coremap_pages; i++) {
		switch (coremap[i].state) {
		case CM_FREE:
			free++;
			break;
		case CM_FIXED:
			fixed++;
			break;
		case CM_USER:
			user++;
			break;
		case CM_EVICTING:
			evicting++;
			break;
		}
	}

	kprintf("coremap: %u pages total | %u free  %u kernel  %u user  %u evicting\n",
		coremap_pages, free, fixed, user, evicting);
}

void
vm_bootstrap(void)
{
	/* Physical memory layout
	 * [0 ..... kernel_end)  : kernel+ELF sections
	 * [kernel_end ..... ?)  : UNUSED at boot
	 */

	paddr_t ram_top = ram_getsize(); // Bytes (exclusive)
	paddr_t first_free = ram_getfirstfree(); // Bytes (inclusive)

	// How many total physical pages exist?
	coremap_pages = ram_top / PAGE_SIZE;

	// Bytes required for coremap[] and round up to full pages.
	size_t cm_bytes = coremap_pages * sizeof(struct coremap_entry);
	cm_bytes = ROUNDUP(cm_bytes, PAGE_SIZE);

	// Place coremap immediately after the kernel.
	paddr_t cm_paddr = first_free;
	coremap = (struct coremap_entry *)PADDR_TO_KVADDR(cm_paddr);

	// Physical address that will become the first truly free page.
	paddr_t free_base = cm_paddr + cm_bytes;

	// Initialise every entry
	for (unsigned i = 0; i < coremap_pages; i++) {
		paddr_t page_addr = idx_to_pa(i);

		coremap[i].chunk_len = 0;
		coremap[i].as = NULL;
		coremap[i].vpn = 0;

		if (page_addr < free_base) {
			coremap[i].state = CM_FIXED; /* kernel or coremap itself */
		} else {
			coremap[i].state = CM_FREE;
		}
	}

	vm_ready = true;

	unsigned free_pages = (ram_top - free_base) / PAGE_SIZE;
	unsigned mib = (free_pages * PAGE_SIZE) / 1024;
	kprintf("VM: %u / %u pages free (%u KiB)\n", free_pages, coremap_pages, mib);
}

// Returns first index of a run of npages CM_FREE pages, or coremap_pages.
static
unsigned
cm_find_run(unsigned npages)
{
	for (unsigned i = 0; i + npages <= coremap_pages; i++) {
		if (coremap[i].state != CM_FREE) {
			continue;
		}

		bool ok = true;

		for (unsigned j = 1; j < npages; j++) {
			if (coremap[i + j].state != CM_FREE) {
				ok = false;
				i += j; // Skip past the allocated slice
				break;
			}
		}

		if (ok) {
			return i;
		}
	}

	return coremap_pages;
}

vaddr_t
alloc_kpages(unsigned npages)
{
	if (npages == 0) {
		return 0;
	}

	KASSERT(vm_ready); // We initialize VM before everything else (see main.c)
	// Before vm_bootstrap we fall back to ram_stealmem().
	// if (!vm_ready) {
	// 	paddr_t pa = ram_stealmem(npages);
	// 	return (pa == 0) ? 0 : PADDR_TO_KVADDR(pa);
	// }

	vm_can_sleep();

	spinlock_acquire(&cm_lock);

	unsigned idx = cm_find_run(npages);
	if (idx == coremap_pages) {
		spinlock_release(&cm_lock);
		return 0; // Out of memory
	}

	/* Mark allocation */
	coremap[idx].state = CM_FIXED;
	coremap[idx].chunk_len = npages;
	for (unsigned j = 1; j < npages; j++) {
		coremap[idx + j].state = CM_FIXED;
		coremap[idx + j].chunk_len = 0;
	}

	spinlock_release(&cm_lock);

	paddr_t pa = idx_to_pa(idx);
	return PADDR_TO_KVADDR(pa);
}

void
free_kpages(vaddr_t kvaddr)
{
	KASSERT(vm_ready); // We initialize VM before everything else (see main.c)
	// if (!vm_ready) {
	// 	return;
	// }

	paddr_t pa = KVADDR_TO_PADDR(kvaddr);
	unsigned idx = pa_to_idx(pa);

	KASSERT(idx < coremap_pages);

	spinlock_acquire(&cm_lock);

	if (coremap[idx].state != CM_FIXED || coremap[idx].chunk_len == 0) {
		spinlock_release(&cm_lock);
		panic("free_kpages: bad or non-head page @%u\n", idx);
	}

	unsigned run = coremap[idx].chunk_len;
	for (unsigned j = 0; j < run; j++) {
		KASSERT(coremap[idx + j].state == CM_FIXED);
		coremap[idx + j].state = CM_FREE;
		coremap[idx + j].chunk_len = 0;
		coremap[idx + j].as = NULL;
		coremap[idx + j].vpn = 0;
	}

	spinlock_release(&cm_lock);
}

/*
 * Allocate a physical page for user space
 * The page will be mapped to the virtual address vaddr in the address space as
 * Returns the physical frame number, or 0 if allocation fails
 */
unsigned
alloc_upage(struct addrspace *as, vaddr_t vaddr)
{
	vm_can_sleep();

	spinlock_acquire(&cm_lock);

	unsigned idx = cm_find_run(1);
	if (idx == coremap_pages) {
		spinlock_release(&cm_lock);

		return 0; /* Out of memory */
	}

	/* Mark as user page */
	coremap[idx].state = CM_USER;
	coremap[idx].chunk_len = 1;
	coremap[idx].as = as;
	coremap[idx].vpn = VPN(vaddr);

	spinlock_release(&cm_lock);

	return idx;
}

/*
 * Free a physical page for user space
 * The page will be unmapped from the virtual address vaddr in the address space
 */
void
free_upage(unsigned idx)
{
	KASSERT(vm_ready);
	KASSERT(idx < coremap_pages);

	spinlock_acquire(&cm_lock);

	/* Check if the page is being evicted */
	if (coremap[idx].state == CM_EVICTING) {
		/* Page is being evicted, don't free it now */
		spinlock_release(&cm_lock);
		return;
	}

	/* We expect exactly one–page allocations for user pages */
	KASSERT(coremap[idx].state == CM_USER);
	KASSERT(coremap[idx].chunk_len == 1);

	coremap[idx].state = CM_FREE;
	coremap[idx].chunk_len = 0;
	coremap[idx].as = NULL;
	coremap[idx].vpn = 0;

	spinlock_release(&cm_lock);
}

// Returns total bytes not in CM_FREE state
unsigned
coremap_used_bytes(void)
{
	if (!vm_ready) {
		return 0;
	}

	spinlock_acquire(&cm_lock);

	unsigned used = 0;
	for (unsigned i = 0; i < coremap_pages; i++) {
		if (coremap[i].state != CM_FREE) {
			used += PAGE_SIZE;
		}
	}

	spinlock_release(&cm_lock);
	return used;
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm_tlbshootdown: not yet implemented\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	struct addrspace *as;
	struct pte *pte;
	struct region *reg;
	uint32_t ehi, elo;
	unsigned pfn;
	bool need_zero = false;
	bool readonly = false;
	bool in_any = false;

	faultaddress &= PAGE_FRAME;
	if (faultaddress >= MIPS_KSEG0) {
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		return EFAULT;
	}

	if (faulttype == VM_FAULT_READONLY) {
		return EFAULT;
	}

	/*
	 * Scan the region list to see if this vaddr is in one of
	 * the text/data/stack regions. If so, note whether it's
	 * supposed to be writeable or not.
	 */
	for (reg = as->regions; reg != NULL; reg = reg->next) {
		vaddr_t start = reg->vbase;
		vaddr_t end = start + reg->npages * PAGE_SIZE;
		if (faultaddress >= start && faultaddress < end) {
			in_any = true;
			readonly = !reg->writeable;
			break;
		}
	}
	/*
	 * If it wasn't in any region, check the heap.
	 * Heap pages are always writable.
	 */
	if (!in_any
		&& faultaddress >= as->heap_start
		&& faultaddress < as->heap_end)
	{
		in_any = true;
		readonly = false;
	}

	if (!in_any) {
		return EFAULT;
	}

	/*
	 * Try a non-allocating lookup first.
	 */
	pte = pt_get_pte(as, faultaddress, false);

	if (pte == NULL) {

		pte = pt_get_pte(as, faultaddress, true);

		if (pte == NULL) {
			return ENOMEM;
		}

		lock_acquire(pte->pte_lock);

		KASSERT(pte->state == PTE_STATE_UNALLOC);

		pte->state = PTE_STATE_ZERO;
		pte->readonly = readonly;
	} else {
		lock_acquire(pte->pte_lock);

		/* Allow override because of as_prepare_load/as_complete_load */
		readonly = pte->readonly;
	}

	/*
	 * Handle each PTE state.
	 */
	switch (pte->state) {
		case PTE_STATE_UNALLOC:
		case PTE_STATE_ZERO:
			need_zero = true;
			break;
		case PTE_STATE_SWAP:
			lock_release(pte->pte_lock);
			return EFAULT;
		case PTE_STATE_RAM:
			/* nothing special */
			break;
		default:
			lock_release(pte->pte_lock);
			return EFAULT;
	}

	/*
	 * If we already have RAM, just install the TLB entry.
	 */
	if (!need_zero) {
		ehi = faultaddress;
		elo = (pte->pfn << 12) | TLBLO_VALID;

		if (!pte->readonly) {
			elo |= TLBLO_DIRTY;
		}

		pte->referenced = 1;

		lock_release(pte->pte_lock);

		int spl = splhigh();

		tlb_random(ehi, elo);

		splx(spl);

		return 0;
	}

	/*
	 * Otherwise allocate, zero, re-lookup the PTE, and fill in.
	 * We need to release the lock before allocating to avoid deadlocks.
	 */
	lock_release(pte->pte_lock);

	pfn = alloc_upage(as, faultaddress);
	if (pfn == 0) {
		return ENOMEM;
	}

	vaddr_t kva = PADDR_TO_KVADDR(idx_to_pa(pfn));
	bzero((void *)kva, PAGE_SIZE);

	/* Re-lookup the PTE and update it to RAM */
	pte = pt_get_pte(as, faultaddress, false);

	KASSERT(pte != NULL);

	lock_acquire(pte->pte_lock);

	KASSERT(pte->state == PTE_STATE_UNALLOC || pte->state == PTE_STATE_ZERO);

	pte->state = PTE_STATE_RAM;
	pte->pfn = pfn;
	/* Mark as referenced since we're loading it for the first time */
	pte->referenced = 1;

	/* Install in TLB */
	ehi = faultaddress;
	elo = (pfn << 12) | TLBLO_VALID;

	if (!readonly) {
		elo |= TLBLO_DIRTY;
	}

	lock_release(pte->pte_lock);

	int spl = splhigh();

	tlb_random(ehi, elo);

	splx(spl);

	return 0;
}

void tlb_invalidate(vaddr_t vaddr)
{
	int idx = -1;

	int spl = splhigh();

	idx = tlb_probe(vaddr, 0);

	if (idx >= 0) {
		tlb_write(TLBHI_INVALID(idx), TLBLO_INVALID(), idx);
	}

	splx(spl);
}

/*
 * Mark a page as being evicted to swap.
 * Returns:
 *   0 on success
 *   EBUSY if the page is already being evicted
 *   EINVAL if the page is not a user page
 */
int
vm_mark_page_evicting(unsigned idx)
{
	KASSERT(vm_ready);
	KASSERT(idx < coremap_pages);

	spinlock_acquire(&cm_lock);

	if (coremap[idx].state != CM_USER) {
		/* Only user pages can be evicted */
		spinlock_release(&cm_lock);
		return EINVAL;
	}

	if (coremap[idx].state == CM_EVICTING) {
		/* Page is already being evicted */
		spinlock_release(&cm_lock);
		return EBUSY;
	}

	/* Mark the page as being evicted */
	coremap[idx].state = CM_EVICTING;

	spinlock_release(&cm_lock);
	return 0;
}

/*
 * Mark a page eviction as finished.
 * This transitions the page from CM_EVICTING to CM_FREE.
 */
void
vm_eviction_finished(unsigned idx)
{
	KASSERT(vm_ready);
	KASSERT(idx < coremap_pages);

	spinlock_acquire(&cm_lock);

	KASSERT(coremap[idx].state == CM_EVICTING);
	KASSERT(coremap[idx].chunk_len == 1); /* User pages are always 1 page */

	/* Reset the coremap entry */
	coremap[idx].state = CM_FREE;
	coremap[idx].chunk_len = 0;
	coremap[idx].as = NULL;
	coremap[idx].vpn = 0;

	spinlock_release(&cm_lock);
}

/*
 * Find a user page that can be evicted to swap.
 * Implements a simple clock (second-chance) algorithm.
 *
 * Returns:
 *   0 on success with *idx_ret set to the page index
 *   ENOENT if no victim could be found
 */
int
vm_find_eviction_victim(unsigned *idx_ret)
{
	static unsigned victim_next = 0;
	unsigned start_pos;

	KASSERT(vm_ready);
	KASSERT(idx_ret != NULL);

	spinlock_acquire(&cm_lock);

	/* Start from the last position we checked */
	start_pos = victim_next;

	/* First pass: look for pages with reference bit cleared */
	for (unsigned i = 0; i < coremap_pages; i++) {
		unsigned idx = (start_pos + i) % coremap_pages;

		if (coremap[idx].state == CM_USER) {
			/* Found a user page, check if it's not referenced */
			struct addrspace *as = coremap[idx].as;
			vaddr_t vaddr = coremap[idx].vpn * PAGE_SIZE;
			struct pte *pte = pt_get_pte(as, vaddr, false);

			if (pte != NULL) {
				lock_acquire(pte->pte_lock);

				if (pte->referenced == 0) {
					/* This page hasn't been used recently, evict it */
					victim_next = (idx + 1) % coremap_pages;
					*idx_ret = idx;
					lock_release(pte->pte_lock);
					spinlock_release(&cm_lock);
					return 0;
				}

				/* Mark the page as not referenced for next round */
				pte->referenced = 0;
				lock_release(pte->pte_lock);
			}
		}
	}

	/* Second pass: just take any user page */
	for (unsigned i = 0; i < coremap_pages; i++) {
		unsigned idx = (start_pos + i) % coremap_pages;

		if (coremap[idx].state == CM_USER) {
			/* Found a user page, use it */
			victim_next = (idx + 1) % coremap_pages;
			*idx_ret = idx;
			spinlock_release(&cm_lock);
			return 0;
		}
	}

	/* No suitable page found */
	spinlock_release(&cm_lock);
	return ENOENT;
}
