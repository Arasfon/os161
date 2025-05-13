/*
 * Copyright (c) 2013
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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <synch.h>
#include <limits.h>
#include <kern/errno.h>
#include <spinlock.h>
#include <array.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

static struct proc *pid_table[PID_MAX];
static struct spinlock pid_table_lock;
static pid_t next_pid = PID_MIN;

#ifndef ITEMINLINE
#define ITEMINLINE INLINE
#endif
DEFARRAY_BYTYPE(procarray, struct proc, ITEMINLINE);

static
bool
procarray_removefirst(struct procarray *a, struct proc *val) {
	for (unsigned i = 0; i < procarray_num(a); i++) {
		if (procarray_get(a, i) == val) {
			procarray_remove(a, i);
			return true;
		}
	}

	return false;
}

/*
 * Create a proc structure.
 */
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);
	proc->p_cv_lock = lock_create("proc_cv_lock");
	KASSERT(proc->p_cv_lock);

	pid_t pid;
	int err = pid_alloc(proc, &pid);
	if (err) {
		panic("pid_alloc failed: %d\n", err);
		return NULL;
	}

	proc->p_pid = pid;
	proc->p_retval = 0;
	proc->p_has_exited = false;
	proc->p_cv = cv_create("proc_cv");
	KASSERT(proc->p_cv);
	proc->p_parent = NULL;
	proc->p_children = procarray_create();
	KASSERT(proc->p_children);
	proc->p_children_lock = lock_create("proc_children_lock");

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	/* File descriptor table */
	proc->p_fdtable = NULL;
	proc->p_fdtable_size = 0;
	proc->p_fdtable_lock = NULL;

	return proc;
}

static
struct proc *
proc_create_sys(const char *name, pid_t pid) {
	KASSERT(pid >= 0 && pid < PID_MIN);

	pid_t old_next_pid = next_pid;

	struct proc *proc = proc_create(name);

	pid_free(proc->p_pid);

	// Manually register the process
	spinlock_acquire(&pid_table_lock);
	proc->p_pid = pid;

	pid_table[pid] = proc;
	spinlock_release(&pid_table_lock);

	// Technically by the time we get here, the next_pid could have changed
	// but it doesn't matter much because we will still get to the good pid
	// albeit with more iterations
	next_pid = old_next_pid;

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	if (proc->p_parent != NULL) {
		lock_acquire(proc->p_parent->p_children_lock);
		procarray_removefirst(proc->p_parent->p_children, proc);
		lock_release(proc->p_parent->p_children_lock);
	}

	pid_free(proc->p_pid);
	lock_destroy(proc->p_children_lock);
	procarray_destroy(proc->p_children);
	cv_destroy(proc->p_cv);

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);
	lock_destroy(proc->p_cv_lock);

	/* Destroy the file descriptor table */
	if (proc->p_fdtable) {
		fdtable_destroy(proc);
	}

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create_sys("[kernel]", 0);
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	/* Initialize the file descriptor table */
	int err = fdtable_init(newproc);
	if (err) {
		panic("runprogram: could not init fdtable: %d\n", err);
	}
	err = fdtable_init_console(newproc);
	if (err) {
		panic("runprogram: could not init console in fdtable: %d\n", err);
	}

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

/* PID HANDLING */

void
pid_table_bootstrap(void)
{
    spinlock_init(&pid_table_lock);

    spinlock_acquire(&pid_table_lock);

    for (int i = 0; i < PID_MAX; i++) {
        pid_table[i] = NULL;
    }

    next_pid = PID_MIN;

    spinlock_release(&pid_table_lock);
}

void
pid_table_destroy(void)
{
    spinlock_cleanup(&pid_table_lock);
}

struct proc *
pid_table_lookup(pid_t pid)
{
    struct proc *p = NULL;

    spinlock_acquire(&pid_table_lock);

	// Ignore the kernel process
    if (pid > 0 && pid < PID_MAX) {
        p = pid_table[pid];
    }

    spinlock_release(&pid_table_lock);

    return p;
}

int
pid_alloc(struct proc *proc, pid_t *pid)
{
    KASSERT(pid != NULL);

    spinlock_acquire(&pid_table_lock);

    for (int i = 0; i < PID_MAX-1; i++) {

        pid_t candidate = (next_pid + i) % PID_MAX;

        if (candidate < PID_MIN) {
            continue;
        }

        if (pid_table[candidate] == NULL) {
            pid_table[candidate] = proc;

            next_pid = (candidate + 1) % PID_MAX;

            *pid = candidate;

            spinlock_release(&pid_table_lock);

            return 0;
        }
    }

    spinlock_release(&pid_table_lock);

    return ENPROC;
}

int
pid_free(pid_t pid)
{
    if (pid < PID_MIN || pid >= PID_MAX) {
        return EINVAL;
    }

    spinlock_acquire(&pid_table_lock);

    if (pid_table[pid] == NULL) {
        spinlock_release(&pid_table_lock);

        return EINVAL;
    }

    pid_table[pid] = NULL;

    spinlock_release(&pid_table_lock);

    return 0;
}
