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

		vaddr_t free_start = ROUNDUP(new_break, PAGE_SIZE);
		vaddr_t free_end = ROUNDDOWN(old_break, PAGE_SIZE);

		for (vaddr_t va = free_start; va < free_end; va += PAGE_SIZE) {
			spinlock_release(&as->pt_lock);
			struct pte *pte = pt_get_pte(as, va, false);
			spinlock_acquire(&as->pt_lock);

			if (pte != NULL && pte->state == PTE_STATE_RAM) {
				free_upage(pte->pfn);
				pte->state = PTE_STATE_UNALLOC;
				tlb_invalidate(va);
			}
		}
	}

	as->heap_end = new_break;

	spinlock_release(&as->pt_lock);

	*retval = (int32_t)old_break;

	return 0;
}
