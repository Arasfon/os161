#include <types.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <addrspace.h>
#include <kern/wait.h>
#include <kern/errno.h>
#include <copyinout.h>

int
sys__exit(int exitcode)
{
	struct proc *p = curproc;
	struct addrspace *as;

	// Close all open file descriptors
	if (p->p_fdtable) {
		fdtable_destroy(p);
	}

	// Release current working directory
	if (p->p_cwd) {
		VOP_DECREF(p->p_cwd);
		p->p_cwd = NULL;
	}

	// Tear down address space
	as = proc_setas(NULL);
	as_deactivate();
	if (as) {
		as_destroy(as);
	}

	// Record exit status and wake up any waiters
	lock_acquire(p->p_cv_lock);
	
	p->p_retval = _MKWAIT_EXIT(exitcode);
	p->p_has_exited = true;
	cv_broadcast(p->p_cv, p->p_cv_lock);

	lock_release(p->p_cv_lock);

	// We will not free the pid here because zombie processes exist
	// Process will be destroyed in sys_waitpid()

	// Does not return
	thread_exit();

	panic("sys__exit: thread_exit returned\n");
	return 0; // Returning just to be consistent with other syscalls
}

int
sys_waitpid(pid_t pid, userptr_t statusptr, int options, int *retval)
{
	struct proc *child;
	int exitstatus;
	int err;

	// We only support options==0 for now
	if (options != 0) {
		return EINVAL;
	}

	// Lookup the child in the PID table
	child = pid_table_lookup(pid);
	if (child == NULL) {
		return ESRCH;
	}

	// Verify it really is our child
	if (child->p_parent != curproc) {
		return ECHILD;
	}

	// Wait for it to exit
	lock_acquire(child->p_cv_lock);

	while (!child->p_has_exited) {
		cv_wait(child->p_cv, child->p_cv_lock);
	}

	exitstatus = child->p_retval;

	lock_release(child->p_cv_lock);

	// Copy the exit status out to userspace
	err = copyout(&exitstatus, statusptr, sizeof(int));
	if (err) {
		return err;
	}

	proc_destroy(child);

	*retval = pid;
	return 0;
}

int
sys_getpid(int *retval)
{
	*retval = curproc->p_pid;
	return 0;
}
