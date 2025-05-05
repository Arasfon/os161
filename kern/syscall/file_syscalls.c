#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/stat.h>
#include <limits.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>
#include <copyinout.h>
#include <syscall.h>
#include <kern/seek.h>

int
sys_open(userptr_t user_path, int flags, mode_t mode, int *retval)
{
	char pathbuf[PATH_MAX];
	size_t actual;
	int err;

	// Copy pathname from user
	err = copyinstr(user_path, pathbuf, sizeof(pathbuf), &actual);
	if (err) {
		return err;
	}

	struct vnode *vn;
	err = vfs_open(pathbuf, flags, mode, &vn);
	if (err) {
		return err;
	}

	// With refcount == 1
	struct file_handle *fh;
	err = fh_create(vn, flags, &fh);
	if (err) {
		vfs_close(vn);
		return err;
	}

	int fd;
	err = fdtable_alloc(curproc, fh, &fd);
	if (err) {
		fh_release(fh);
		return err;
	}

	*retval = fd;
	return 0;
}

int
sys_dup2(int oldfd, int newfd, int *retval)
{
	int err;
	
	err = fdtable_dup(curproc, oldfd, newfd);
	if (err) return err;

	*retval = newfd;
	return 0;
}

int
sys_close(int fd)
{
	int err;
	
	err = fdtable_free(curproc, fd);
	return err;
}

int
sys_read(int fd, userptr_t user_buf, size_t buflen, int *retval)
{
	int err;

	// Increases refcount
	struct file_handle *fh = fdtable_get(curproc, fd, &err);
	if (fh == NULL) {
		return err;
	}

	// Check that it was opened for reading
	if ((fh->fh_flags & O_ACCMODE) == O_WRONLY) {
		fh_release(fh);
		return EBADF;
	}

	lock_acquire(fh->fh_lock);
	off_t offset = fh->fh_offset;
	lock_release(fh->fh_lock);

	// Set up a kernel‐side uio to describe the transfer
	struct iovec iov;
	struct uio ku;
	uio_uinit(&iov, &ku, user_buf, buflen, offset, UIO_READ);

	// Perform the read
	err = VOP_READ(fh->fh_vnode, &ku);
	if (err) {
		fh_release(fh);
		return err;
	}

	// Compute how many bytes were actually read
	size_t got = buflen - ku.uio_resid;

	lock_acquire(fh->fh_lock);
	fh->fh_offset = ku.uio_offset;
	lock_release(fh->fh_lock);

	fh_release(fh);

	*retval = (int)got;
	return 0;
}

int
sys_write(int fd, userptr_t user_buf, size_t nbytes, int *retval)
{
	int err;

	// Increases refcount
	struct file_handle *fh = fdtable_get(curproc, fd, &err);
	if (fh == NULL) {
		return err;
	}

	// Check that it was opened for writing
	int accmode = fh->fh_flags & O_ACCMODE;
	if (accmode == O_RDONLY) {
		fh_release(fh);
		return EBADF;
	}

	lock_acquire(fh->fh_lock);
	off_t offset = fh->fh_offset;
	lock_release(fh->fh_lock);

	// Set up uio for the write
	struct iovec iov;
	struct uio ku;
	uio_uinit(&iov, &ku, user_buf, nbytes, offset, UIO_WRITE);

	// Perform the write
	err = VOP_WRITE(fh->fh_vnode, &ku);
	if (err) {
		fh_release(fh);
		return err;
	}

	// Compute how many bytes were written
	size_t wrote = nbytes - ku.uio_resid;

	lock_acquire(fh->fh_lock);
	fh->fh_offset = ku.uio_offset;
	lock_release(fh->fh_lock);

	fh_release(fh);

	*retval = (int)wrote;
	return 0;
}

int
sys_lseek(int fd, off_t offset, int whence, int64_t *retval)
{
	int err;
	struct file_handle *fh;
	off_t newpos;

	fh = fdtable_get(curproc, fd, &err);
	if (fh == NULL) return err;

	// Compute new offset
	lock_acquire(fh->fh_lock);

	switch (whence) {
		case SEEK_SET:
			newpos = offset;
			break;
		case SEEK_CUR:
			newpos = fh->fh_offset + offset;
			break;
		case SEEK_END:
			{
				// Get file length (stat)
				struct stat st;

				lock_release(fh->fh_lock);

				err = VOP_STAT(fh->fh_vnode, &st);
				if (err) {
					fh_release(fh);
					return err;
				}

				lock_acquire(fh->fh_lock);

				newpos = st.st_size + offset;
			}
			break;
		default:
			lock_release(fh->fh_lock);
			fh_release(fh);
			return EINVAL;
	}

	if (newpos < 0) {
		lock_release(fh->fh_lock);
		fh_release(fh);
		return EINVAL;
	}

	// Update offset
	fh->fh_offset = newpos;

	lock_release(fh->fh_lock);

	*retval = newpos;
	fh_release(fh);

	return 0;
}

int
sys_chdir(userptr_t path)
{
	char *kpath;
	int err;

	kpath = kmalloc(PATH_MAX);
	if (kpath == NULL) return ENOMEM;

	err = copyinstr(path, kpath, PATH_MAX, NULL);
	if (err) {
		kfree(kpath);
		return err;
	}

	err = vfs_chdir(kpath);
	kfree(kpath);
	return err;
}

int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{
	struct iovec iov;
	struct uio ku;
	int err;

	// Initialize uio to copy out the cwd string
	uio_uinit(&iov, &ku, buf, buflen, 0, UIO_READ);
	ku.uio_space = curproc->p_addrspace;

	err = vfs_getcwd(&ku);
	if (err) return err;

	// Number of bytes copied
	*retval = (int)(buflen - ku.uio_resid);
	return 0;
}
