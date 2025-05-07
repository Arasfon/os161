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
#include <kern/fcntl.h>
#include <limits.h>
#include <lib.h>
#include <vfs.h>
#include <signal.h>

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
	
	if (exitcode > 0 && exitcode <= _NSIG) {
		p->p_retval = _MKWAIT_SIG(exitcode);
	} else {
		p->p_retval = _MKWAIT_EXIT(exitcode);
	}

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
	if (statusptr) {
		err = copyout(&exitstatus, statusptr, sizeof(int));
		if (err) {
			return err;
		}
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

static
int
execv_core(char *kprogname, int argc, char **kargs, size_t stringspace)
{
	struct vnode *v;
	struct addrspace *oldas;
	struct addrspace *newas;
	vaddr_t entrypoint, stackptr;
	size_t i;
	size_t ptrspace;
	int err;

	// Open the executable
	err = vfs_open(kprogname, O_RDONLY, 0, &v);
	if (err) {
		return err;
	}

	// Destroy old address space
	oldas = proc_getas();
	if (oldas) {
		as_deactivate();
		as_destroy(oldas);
	}

	// Create and activate new address space
	newas = as_create();
	if (!newas) {
		vfs_close(v);
		return ENOMEM;
	}
	proc_setas(newas);
	as_activate();

	// Load the ELF executable
	err = load_elf(v, &entrypoint);
	vfs_close(v);
	if (err) {
		return err;
	}

	// Define the user stack
	err = as_define_stack(newas, &stackptr);
	if (err) {
		return err;
	}

	// Compute pointer space
	ptrspace = (argc + 1) * sizeof(userptr_t);

	// Reserve stack space for strings and argv[]
	stackptr -= stringspace;
	stackptr -= ptrspace;
	stackptr = ROUNDDOWN(stackptr, 4);

	// Copy argument strings to user stack and record addresses
	{
		vaddr_t dest = stackptr + ptrspace;

		for (i = 0; i < (size_t)argc; i++) {
			size_t len = strlen(kargs[i]) + 1;

			err = copyout(kargs[i], (userptr_t)dest, len);
			if (err) {
				return err;
			}

			kargs[i] = (char *)dest;
			dest += ROUNDUP(len, 4);
		}

		kargs[argc] = NULL;
	}

	// Copy argv pointer array
	err = copyout(kargs, (userptr_t)stackptr, ptrspace);
	if (err) {
		return err;
	}

	kfree(kargs);
	kfree(kprogname);

	// Does not return
	enter_new_process(argc, (userptr_t)stackptr, NULL, stackptr, entrypoint);

	panic("execv_core: enter_new_process returned\n");

	return EINVAL;
}

int
sys_execv(userptr_t progname, userptr_t args)
{
	char *kprogname = NULL;
	char **kargs = NULL;
	char *arg_buf = NULL;
	char *tmparg = NULL;
	size_t stringspace = 0;
	int argc = 0;
	int err;
	size_t got;
	userptr_t arg_addr;

	// Copy program name
	kprogname = kmalloc(PATH_MAX);
	if (!kprogname) {
		return ENOMEM;
	}

	err = copyinstr(progname, kprogname, PATH_MAX, &got);
	if (err) {
		kfree(kprogname);
		return err;
	}

	// Temporary buffer for measuring args
	tmparg = kmalloc(ARG_MAX + 1);
	if (!tmparg) {
		kfree(kprogname);
		return ENOMEM;
	}

	// Count args and measure total padded size
	while (true) {
		err = copyin((userptr_t)&((userptr_t*)args)[argc], &arg_addr, sizeof(arg_addr));
		if (err) {
			kfree(kprogname);
			kfree(tmparg);
			return err;
		}

		if (arg_addr == NULL) {
			break;
		}

		err = copyinstr(arg_addr, tmparg, ARG_MAX + 1, &got);
		if (err) {
			kfree(kprogname);
			kfree(tmparg);
			return err;
		}

		size_t padded = ROUNDUP(got, 4);

		if (stringspace + padded < stringspace || stringspace + padded > ARG_MAX) {
			kfree(kprogname);
			kfree(tmparg);
			return E2BIG;
		}

		stringspace += padded;
		argc++;
	}

	kfree(tmparg);

	arg_buf = kmalloc(stringspace);
	if (!arg_buf) {
		err = ENOMEM;
		goto err;
	}

	kargs = kmalloc((argc + 1) * sizeof(char *));
	if (!kargs) {
		err = ENOMEM;
		goto err;
	}

	// Copy each user arg into arg_buf and record in kargs[]
	{
		char *bufpos = arg_buf;
		for (int i = 0; i < argc; i++) {
			err = copyin((userptr_t)&((userptr_t*)args)[i], &arg_addr, sizeof(arg_addr));
			if (err) {
				goto err;
			}

			err = copyinstr(arg_addr, bufpos, stringspace - (bufpos - arg_buf), &got);
			if (err) {
				goto err;
			}

			kargs[i] = bufpos;
			bufpos += ROUNDUP(got, 4);
		}
		kargs[argc] = NULL;
	}

	// Should not return
	err = execv_core(kprogname, argc, kargs, stringspace);

err:
	if (arg_buf) kfree(arg_buf);
	if (kargs) kfree(kargs);
	if (kprogname) kfree(kprogname);

	return err;
}

int
sys_kexecv(char *kprogname, char **kargs)
{
	int argc = 0;
	size_t stringspace = 0;
	
	while (kargs[argc] != NULL) {
		size_t len = strlen(kargs[argc]) + 1;
		stringspace += ROUNDUP(len, 4);
		argc++;
	}
	
	// Should not return
	return execv_core(kprogname, argc, kargs, stringspace);
}
