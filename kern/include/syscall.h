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

#ifndef _SYSCALL_H_
#define _SYSCALL_H_


#include <cdefs.h> /* for __DEAD */
struct trapframe; /* from <machine/trapframe.h> */

/*
 * The system call dispatcher.
 */

void syscall(struct trapframe *tf);

/*
 * Support functions.
 */

/* Helper for fork(). */
void enter_forked_process(void* data, unsigned long ndata);

/* Enter user mode. Does not return. */
__DEAD void enter_new_process(int argc, userptr_t argv, userptr_t env,
		       vaddr_t stackptr, vaddr_t entrypoint);


/*
 * Prototypes for IN-KERNEL entry points for system call implementations.
 */

int sys_reboot(int code);

int sys___time(userptr_t user_seconds, userptr_t user_nanoseconds);

int sys_open(userptr_t pathname, int flags, mode_t mode, int *retval);
int sys_dup2(int oldfd, int newfd, int *retval);
int sys_close(int fd);
int sys_read(int fd, userptr_t buf, size_t nbytes, int *retval);
int sys_write(int fd, userptr_t buf, size_t nbytes, int *retval);
int sys_lseek(int fd, off_t offset, int whence, int64_t *retval);
int sys_remove(userptr_t pathname);
int sys_chdir(userptr_t pathname);
int sys___getcwd(userptr_t buf, size_t buflen, int *retval);

int sys_fork(struct trapframe *tf, pid_t *retval);
int sys_execv(userptr_t progname, userptr_t args);
int sys_kexecv(char *kprogname, char **kargs);
int sys__exit(int status);
int sys_kwaitpid(pid_t pid, int options, int *statuscode);
int sys_waitpid(pid_t pid, userptr_t statusptr, int options, int *retval);
int sys_getpid(int *retval);

int sys_sbrk(intptr_t amount, int32_t *retval);

#endif /* _SYSCALL_H_ */
