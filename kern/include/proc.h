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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <synch.h>
#include <array.h>

struct addrspace;
struct thread;
struct vnode;

#define ITEMINLINE
DECLARRAY_BYTYPE(procarray, struct proc, ITEMINLINE);

/* Definition of a file handle */
struct file_handle {
	struct vnode *fh_vnode;		/* The vnode this file refers to */
	off_t fh_offset;			/* Current position in the file */
	unsigned fh_refcount;		/* Number of references to this file handle */
	int fh_flags;				/* Open flags (O_RDONLY, etc.) */
	struct lock *fh_lock;		/* Lock for this file handle */
};

/* File descriptor table entry */
struct file_descriptor {
	struct file_handle *fd_file;		/* File handle if in use, NULL if free */
	int fd_flags;						/* Flags for the file descriptor */
};

/*
 * Process structure.
 *
 * Note that we only count the number of threads in each process.
 * (And, unless you implement multithreaded user processes, this
 * number will not exceed 1 except in kproc.) If you want to know
 * exactly which threads are in the process, e.g. for debugging, add
 * an array and a sleeplock to protect it. (You can't use a spinlock
 * to protect an array because arrays need to be able to call
 * kmalloc.)
 *
 * You will most likely be adding stuff to this structure, so you may
 * find you need a sleeplock in here for other reasons as well.
 * However, note that p_addrspace must be protected by a spinlock:
 * thread_switch needs to be able to fetch the current address space
 * without sleeping.
 */
struct proc {
	char *p_name;			/* Name of this process */
	struct spinlock p_lock;		/* Lock for general operations */
	unsigned p_numthreads;		/* Number of threads in this process */

	pid_t p_pid;		/* PID */
	int p_retval;		/* The exit code */
	bool p_has_exited;		/* Has process exited? */
	struct cv *p_cv;			/* For parent to wait on */
	struct lock *p_cv_lock;		/* Lock for cv */
	struct proc *p_parent;		/* Parent (or NULL) */
	struct procarray *p_children;		/* Children */
	struct lock *p_children_lock;		/* Lock for children */

	/* VM */
	struct addrspace *p_addrspace;	/* virtual address space */

	/* VFS */
	struct vnode *p_cwd;		/* current working directory */

	/* File descriptor table */
	struct file_descriptor *p_fdtable;	/* Open file table */
	int p_fdtable_size;					/* Size of file table */
	struct lock *p_fdtable_lock;		/* Lock for file table */
};

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a new process for use by fork(). */
struct proc *proc_create(const char *name);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);

/* PID HADNLING */

/* Initialize the PID table. Should be called early in the boot process. */
void pid_table_bootstrap(void);

/* Destroy the PID table. Should it be called in the shutdown process? */
void pid_table_destroy(void);

/* Lookup a process by PID. */
struct proc *pid_table_lookup(pid_t pid);

/* Allocate a PID. */
int pid_alloc(struct proc *proc, pid_t *pid);

/* Free a PID. */
int pid_free(pid_t pid);

/* FILE HANDLING */

/* Allocate, initialize, return a new file_handle with refcount==1 (or error) */
int fh_create(struct vnode *vn, int flags, struct file_handle **retfh);

/* Bump the refcount (for dup/fork) */
void fh_acquire(struct file_handle *fh);

/* Drop one reference; when it hits zero: vfs_close(vn), destroy lock, free() */
void fh_release(struct file_handle *fh);

/* Destroy immediately (only if you know refcount==0) */
void fh_destroy(struct file_handle *fh);

/* Init file descriptor table */
int fdtable_init(struct proc *p);

/* Init console in the file descriptor table */
int fdtable_init_console(struct proc *p);

/* Destroy file descriptor table */
int fdtable_destroy(struct proc *p);

/* Allocate a file descriptor */
int fdtable_alloc(struct proc *p, struct file_handle *fh, int *retfd);

/* Free a file descriptor */
int fdtable_free(struct proc *p, int fd);

/* Return the file_handle for ‘fd’, or NULL + set error code if invalid/free */
struct file_handle *fdtable_get(struct proc *p, int fd, int *err);

/* Duplicate one slot to another (for dup2): bumps fh refcount appropriately */
int fdtable_dup(struct proc *p, int oldfd, int newfd);

/* Change the per‐fd flags (e.g. CLOEXEC) */
int fdtable_setflags(struct proc *p, int fd, int flags);

/* Close all descriptors (e.g. on exec or proc exit) */
void fdtable_closeall(struct proc *p);

#endif /* _PROC_H_ */
