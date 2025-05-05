#include <types.h>
#include <syscall.h>
#include <proc.h>
#include <current.h>
#include <vnode.h>
#include <addrspace.h>
#include <kern/wait.h>
#include <kern/errno.h>
#include <copyinout.h>
#include <mips/trapframe.h>

int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct proc *child;
	struct addrspace *child_as;
	int err, i;
	struct trapframe *child_tf;

	// Create the child proc structure
	child = proc_create(curproc->p_name);
	if (child == NULL) {
		return ENPROC;
	}

	// Record parent/child relationship
	child->p_parent = curproc;
	spinlock_acquire(&curproc->p_lock);
	procarray_add(curproc->p_children, child, NULL);
	spinlock_release(&curproc->p_lock);

	// Duplicate the address space
	err = as_copy(curproc->p_addrspace, &child_as);
	if (err) {
		proc_destroy(child);
		return err;
	}
	child->p_addrspace = child_as;

	// Inherit current working directory
	if (curproc->p_cwd) {
		VOP_INCREF(curproc->p_cwd);
		child->p_cwd = curproc->p_cwd;
	}

	// Duplicate the file-descriptor table
	err = fdtable_init(child);
	if (err) {
		proc_destroy(child);
		return err;
	}
	for (i = 0; i < curproc->p_fdtable_size; i++) {
		struct file_handle *fh = curproc->p_fdtable[i].fd_file;
		int flags = curproc->p_fdtable[i].fd_flags;
		if (fh) {
			// Bump handle refcount and install into child
			fh_acquire(fh);
			child->p_fdtable[i].fd_file = fh;
			child->p_fdtable[i].fd_flags = flags;
		}
	}

	// Prepare the child's trapframe
	child_tf = kmalloc(sizeof(*child_tf));
	if (child_tf == NULL) {
		proc_destroy(child);
		return ENOMEM;
	}
	*child_tf = *tf;	// Copy all registers

	// Fork a thread in the child process
	err = thread_fork(curthread->t_name,
					  child,
					  enter_forked_process,
					  (void*)child_tf,	// Child's trapframe as a parameter
					  1);		// nargs
	if (err) {
		kprintf("sys_fork: thread_fork failed\n");
		kfree(child_tf);
		proc_destroy(child);
		return err;
	}

	// Return child's pid in parent
	*retval = child->p_pid;
	return 0;
}

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

	KASSERT(child->p_has_exited);

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
