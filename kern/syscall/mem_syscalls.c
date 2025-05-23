#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <current.h>
#include <syscall.h>
#include <spinlock.h>

/*
 * System call to extend or shrink the heap (program break)
 */
int
sys_sbrk(intptr_t amount, int32_t *retval)
{
	struct addrspace *as;
	vaddr_t old_break, new_break;

	as = proc_getas();
	if (as == NULL) {
		return EFAULT;
	}

	/* We need the global lock for heap_end/heap_start */
	spinlock_acquire(&as->pt_lock);
	old_break = as->heap_end;

	if (amount == 0) {
		*retval = (int32_t)old_break;
		spinlock_release(&as->pt_lock);
		return 0;
	}

	if (amount > 0) {
		new_break = old_break + amount;
		vaddr_t heap_limit = USERSTACK - STACKPAGES * PAGE_SIZE;

		if (new_break > heap_limit) {
			spinlock_release(&as->pt_lock);
			return ENOMEM;
		}

		as->heap_end = new_break;
		spinlock_release(&as->pt_lock);
		*retval = (int32_t)old_break;
		return 0;
	} else {
		if (old_break < (vaddr_t)-amount) {
			spinlock_release(&as->pt_lock);
			return EINVAL; // Overflow
		}

		new_break = old_break + amount;

		if (new_break < as->heap_start) {
			spinlock_release(&as->pt_lock);
			return EINVAL; // Overflow
		}

		/*
		 * Store these values before releasing the spinlock
		 * so they remain consistent during our processing
		 */
		vaddr_t free_start = ROUNDUP(new_break, PAGE_SIZE);
		vaddr_t free_end = ROUNDDOWN(old_break + PAGE_SIZE - 1, PAGE_SIZE);

		/* Update heap_end while still holding the spinlock */
		as->heap_end = new_break;
		spinlock_release(&as->pt_lock);

		/* Skip the loop if no pages to free */
		if (free_start >= free_end) {
			*retval = (int32_t)old_break;
			return 0;
		}

		/* Now free pages with individual PTE locks */
		for (vaddr_t va = free_start; va < free_end; va += PAGE_SIZE) {
			struct pte *pte = pt_get_pte(as, va, false);

			if (pte != NULL) {
				/* Acquire the lock for this specific PTE */
				lock_acquire(pte->pte_lock);

				if (pte->state == PTE_STATE_RAM) {
					free_upage(pte->pfn);
					pte->state = PTE_STATE_UNALLOC;
					tlb_invalidate(va);
				}
				else if (pte->state == PTE_STATE_SWAP) {
					swap_free(pte->swap_slot);
					pte->swap_slot = 0;
					pte->state = PTE_STATE_UNALLOC;
				}
				else if (pte->state == PTE_STATE_ZERO) {
					pte->state = PTE_STATE_UNALLOC;
				}

				lock_release(pte->pte_lock);
			}
		}

		/* Don't need to reacquire the spinlock since we've already updated heap_end */
		*retval = (int32_t)old_break;
		return 0;
	}
}
