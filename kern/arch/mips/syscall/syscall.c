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

#include <types.h>
#include <kern/errno.h>
#include <kern/syscall.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <endian.h>
#include <copyinout.h>
#include <addrspace.h>


/*
 * System call dispatcher.
 *
 * A pointer to the trapframe created during exception entry (in
 * exception-*.S) is passed in.
 *
 * The calling conventions for syscalls are as follows: Like ordinary
 * function calls, the first 4 32-bit arguments are passed in the 4
 * argument registers a0-a3. 64-bit arguments are passed in *aligned*
 * pairs of registers, that is, either a0/a1 or a2/a3. This means that
 * if the first argument is 32-bit and the second is 64-bit, a1 is
 * unused.
 *
 * This much is the same as the calling conventions for ordinary
 * function calls. In addition, the system call number is passed in
 * the v0 register.
 *
 * On successful return, the return value is passed back in the v0
 * register, or v0 and v1 if 64-bit. This is also like an ordinary
 * function call, and additionally the a3 register is also set to 0 to
 * indicate success.
 *
 * On an error return, the error code is passed back in the v0
 * register, and the a3 register is set to 1 to indicate failure.
 * (Userlevel code takes care of storing the error code in errno and
 * returning the value -1 from the actual userlevel syscall function.
 * See src/user/lib/libc/arch/mips/syscalls-mips.S and related files.)
 *
 * Upon syscall return the program counter stored in the trapframe
 * must be incremented by one instruction; otherwise the exception
 * return code will restart the "syscall" instruction and the system
 * call will repeat forever.
 *
 * If you run out of registers (which happens quickly with 64-bit
 * values) further arguments must be fetched from the user-level
 * stack, starting at sp+16 to skip over the slots for the
 * registerized values, with copyin().
 */
void
syscall(struct trapframe *tf)
{
	int callno;
	int32_t retval;
	int64_t retval64;
	bool is64bit_retval = false;
	int err;

	KASSERT(curthread != NULL);
	KASSERT(curthread->t_curspl == 0);
	KASSERT(curthread->t_iplhigh_count == 0);

	callno = tf->tf_v0;

	/*
	 * Initialize retval to 0. Many of the system calls don't
	 * really return a value, just 0 for success and -1 on
	 * error. Since retval is the value returned on success,
	 * initialize it to 0 by default; thus it's not necessary to
	 * deal with it except for calls that return other values,
	 * like write.
	 */

	retval = 0;
	retval64 = 0;

	switch (callno) {
	    case SYS_reboot: // 119
		err = sys_reboot(tf->tf_a0);
		break;

	    case SYS___time: // 113
		err = sys___time((userptr_t)tf->tf_a0,
				 (userptr_t)tf->tf_a1);
		break;

	    /* Add stuff here */
		case SYS_open: // 45
		err = sys_open((userptr_t)tf->tf_a0, tf->tf_a1, tf->tf_a2, &retval);
		break;
		case SYS_dup2: // 48
		err = sys_dup2(tf->tf_a0, tf->tf_a1, &retval);
		break;
		case SYS_close: // 49
		err = sys_close(tf->tf_a0);
		break;
		case SYS_read: // 50
		err = sys_read(tf->tf_a0, (userptr_t)tf->tf_a1, tf->tf_a2, &retval);
		break;
		case SYS_write: // 55
		err = sys_write(tf->tf_a0, (userptr_t)tf->tf_a1, tf->tf_a2, &retval);
		break;
		case SYS_lseek: // 59
		{
			is64bit_retval = true;

			int fd = tf->tf_a0;

			// The offset is passed in a2 and a3 as 64-bit value
			// The whence is passed on the stack
			int whence;
			err = copyin((userptr_t)(tf->tf_sp + 16), &whence, sizeof(whence));
			if (err) {
				break;
			}

			// Construct the 64-bit offset from a2 and a3
			uint64_t offset;
			join32to64(tf->tf_a2, tf->tf_a3, &offset);

			err = sys_lseek(fd, offset, whence, &retval64);

			break;
		}
		case SYS_remove: // 68
		err = sys_remove((userptr_t)tf->tf_a0);
		break;
		case SYS_chdir: // 74
		err = sys_chdir((userptr_t)tf->tf_a0);
		break;
		case SYS___getcwd: // 76
		err = sys___getcwd((userptr_t)tf->tf_a0, tf->tf_a1, &retval);
		break;

		case SYS_fork: // 0
		err = sys_fork(tf, &retval);
		break;
		case SYS_execv: // 2
		err = sys_execv((userptr_t)tf->tf_a0, (userptr_t)tf->tf_a1);
		break;
		case SYS__exit: // 3
		err = sys__exit(tf->tf_a0);
		break;
		case SYS_waitpid: // 4
		err = sys_waitpid(tf->tf_a0, (userptr_t)tf->tf_a1, tf->tf_a2, &retval);
		break;
		case SYS_getpid: // 5
		err = sys_getpid(&retval);
		break;

		case SYS_sbrk: // 9
		err = sys_sbrk((intptr_t)tf->tf_a0, &retval);
		break;

	    default:
		kprintf("Unknown syscall %d\n", callno);
		err = ENOSYS;
		break;
	}


	if (err) {
		/*
		 * Return the error code. This gets converted at
		 * userlevel to a return value of -1 and the error
		 * code in errno.
		 */
		tf->tf_v0 = err;
		tf->tf_a3 = 1;      /* signal an error */
	}
	else {
		/* Success. */
		if (is64bit_retval) {
			uint32_t v0, v1;
			split64to32(retval64, &v0, &v1);

			tf->tf_v0 = v0;
			tf->tf_v1 = v1;
		} else {
			tf->tf_v0 = retval;
		}
		tf->tf_a3 = 0;      /* signal no error */
	}

	/*
	 * Now, advance the program counter, to avoid restarting
	 * the syscall over and over again.
	 */

	tf->tf_epc += 4;

	/* Make sure the syscall code didn't forget to lower spl */
	KASSERT(curthread->t_curspl == 0);
	/* ...or leak any spinlocks */
	KASSERT(curthread->t_iplhigh_count == 0);
}

/*
 * Enter user mode for a newly forked process.
 */
void
enter_forked_process(void* data, unsigned long ndata)
{
	KASSERT(ndata == 1);

	struct trapframe *tf = (struct trapframe *)data;

    // Copy into stack
    struct trapframe tf_stack = *tf;
    kfree(tf);

	// In the child, fork() returns 0
	tf_stack.tf_v0 = 0;
	tf_stack.tf_a3 = 0;			// signal no error
	tf_stack.tf_epc += 4;		// advance past the syscall

	as_activate();

	// Switch to user mode with this trapframe
	mips_usermode(&tf_stack);
}
