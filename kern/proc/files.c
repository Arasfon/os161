#include <types.h>
#include <proc.h>
#include <vnode.h>
#include <limits.h>
#include <kern/errno.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <synch.h>

int
fh_create(struct vnode *vn, int flags, struct file_handle **retfh)
{
	struct file_handle *fh;

	fh = kmalloc(sizeof(*fh));
	if (fh == NULL) {
		return ENOMEM;
	}

	fh->fh_vnode = vn;
	fh->fh_offset = 0;
	fh->fh_flags = flags;
	fh->fh_refcount = 1;

	fh->fh_lock = lock_create("file_handle");
	if (fh->fh_lock == NULL) {
		kfree(fh);
		vfs_close(vn);
		return ENOMEM;
	}

	*retfh = fh;
	return 0;
}

void
fh_acquire(struct file_handle *fh)
{
	KASSERT(fh != NULL);

	lock_acquire(fh->fh_lock);

	fh->fh_refcount++;

	lock_release(fh->fh_lock);
}

void
fh_release(struct file_handle *fh)
{
	bool do_destroy = false;

	KASSERT(fh != NULL);

	lock_acquire(fh->fh_lock);

	KASSERT(fh->fh_refcount > 0);

	fh->fh_refcount--;

	if (fh->fh_refcount == 0) {
		do_destroy = true;
	}

	lock_release(fh->fh_lock);

	if (do_destroy) {
		fh_destroy(fh);
	}
}

void
fh_destroy(struct file_handle *fh)
{
	KASSERT(fh != NULL);
	KASSERT(fh->fh_refcount == 0);

	vfs_close(fh->fh_vnode);
	lock_destroy(fh->fh_lock);
	kfree(fh);
}

static
int
open_console(int openflags, struct file_handle **fh_out)
{
	struct vnode *vn;
	char buf[] = "con:";
	
	int err = vfs_open(buf, openflags, 0, &vn);
	if (err) return err;

	err = fh_create(vn, openflags, fh_out);
	if (err) {
		vfs_close(vn);
	}

	return err;
}

int
fdtable_init(struct proc *p)
{
	int err;

	p->p_fdtable_lock = lock_create("fdtable_lock");
	if (!p->p_fdtable_lock) return ENOMEM;

	p->p_fdtable_size = OPEN_MAX;
	p->p_fdtable = kmalloc(sizeof(*p->p_fdtable) * p->p_fdtable_size);
	if (!p->p_fdtable)
	{
		lock_destroy(p->p_fdtable_lock);
		return ENOMEM;
	}

	lock_acquire(p->p_fdtable_lock);
	for (int i = 0; i < p->p_fdtable_size; i++) {
		p->p_fdtable[i].fd_file = NULL;
		p->p_fdtable[i].fd_flags = 0;
	}
	lock_release(p->p_fdtable_lock);

	struct file_handle *fh[3];
	int modes[3] = { O_RDONLY, O_WRONLY, O_WRONLY };

	for (int i = 0; i < 3; i++) {
		err = open_console(modes[i], &fh[i]);
		if (err) {
			// Clean up any earlier handles
			for (int j = 0; j < i; j++) {
				fh_release(fh[j]);
			}
			lock_destroy(p->p_fdtable_lock);
			kfree(p->p_fdtable);
			return err;
		}
	}

	lock_acquire(p->p_fdtable_lock);

	for (int i = 0; i < 3; i++) {
		p->p_fdtable[i].fd_file  = fh[i];
		p->p_fdtable[i].fd_flags = 0;
	}

	lock_release(p->p_fdtable_lock);

	return 0;
}

int
fdtable_destroy(struct proc *p)
{
	fdtable_closeall(p);

	kfree(p->p_fdtable);
	p->p_fdtable = NULL;
	p->p_fdtable_size = 0;

	lock_destroy(p->p_fdtable_lock);
	p->p_fdtable_lock = NULL;

	return 0;
}

int fdtable_alloc(struct proc *p, struct file_handle *fh, int *retfd)
{
	int i;

	lock_acquire(p->p_fdtable_lock);

	for (i = 0; i < p->p_fdtable_size; i++) {
		if (p->p_fdtable[i].fd_file == NULL) {
			p->p_fdtable[i].fd_file = fh;
			p->p_fdtable[i].fd_flags = 0;

			lock_release(p->p_fdtable_lock);

			*retfd = i;

			return 0;
		}
	}

	lock_release(p->p_fdtable_lock);

	return EMFILE;
}

int
fdtable_free(struct proc *p, int fd)
{
	if (fd < 0 || fd >= p->p_fdtable_size) {
		return EBADF;
	}

	lock_acquire(p->p_fdtable_lock);

	// Already closed?
	if (p->p_fdtable[fd].fd_file == NULL) {
		lock_release(p->p_fdtable_lock);
		return EBADF;
	}

	struct file_handle *fh = p->p_fdtable[fd].fd_file;
	p->p_fdtable[fd].fd_file = NULL;
	p->p_fdtable[fd].fd_flags = 0;

	lock_release(p->p_fdtable_lock);

	// Drop the handle reference (this will close when refcount decrements to 0)
	fh_release(fh);

	return 0;
}

struct file_handle *
fdtable_get(struct proc *p, int fd, int *err)
{
	struct file_handle *fh;

	if (fd < 0 || fd >= p->p_fdtable_size) {
		*err = EBADF;
		return NULL;
	}

	lock_acquire(p->p_fdtable_lock);

	fh = p->p_fdtable[fd].fd_file;
	if (fh) {
		// Bump so it cannot vanish under us
		fh_acquire(fh);
	}

	lock_release(p->p_fdtable_lock);

	if (!fh) {
		*err = EBADF;
		return NULL;
	}

	*err = 0;
	return fh;
}

int
fdtable_dup(struct proc *p, int oldfd, int newfd)
{
	struct file_handle *oldfh, *old2;
	int flags;

	if (oldfd<0 || oldfd>=p->p_fdtable_size ||
		newfd<0 || newfd>=p->p_fdtable_size) {
		return EBADF;
	}

	if (oldfd == newfd) {
		// POSIX says dup2(fd,fd) is a no‑op (but check validity)
		lock_acquire(p->p_fdtable_lock);

		oldfh = p->p_fdtable[oldfd].fd_file;

		lock_release(p->p_fdtable_lock);

		return oldfh ? 0 : EBADF;
	}

	// Take table lock once
	lock_acquire(p->p_fdtable_lock);

	oldfh = p->p_fdtable[oldfd].fd_file;

	if (!oldfh) {
		lock_release(p->p_fdtable_lock);
		return EBADF;
	}

	flags = p->p_fdtable[oldfd].fd_flags;

	// If newfd in use, snatch it out
	old2 = p->p_fdtable[newfd].fd_file;
	p->p_fdtable[newfd].fd_file = NULL;
	p->p_fdtable[newfd].fd_flags = 0;

	// Install the old handle into newfd
	fh_acquire(oldfh);
	p->p_fdtable[newfd].fd_file  = oldfh;
	p->p_fdtable[newfd].fd_flags = flags;

	lock_release(p->p_fdtable_lock);

	// Now that table‑lock is dropped, release the old newfd handle
	if (old2) {
		fh_release(old2);
	}

	return 0;
}

int
fdtable_setflags(struct proc *p, int fd, int flags)
{
	if (fd < 0 || fd >= p->p_fdtable_size) {
		return EBADF;
	}

	lock_acquire(p->p_fdtable_lock);

	if (p->p_fdtable[fd].fd_file == NULL) {
		lock_release(p->p_fdtable_lock);
		return EBADF;
	}

	p->p_fdtable[fd].fd_flags = flags;

	lock_release(p->p_fdtable_lock);

	return 0;
}

void
fdtable_closeall(struct proc *p)
{
	int i, n = p->p_fdtable_size;
	struct file_handle **to_close;

	// Allocate temporary array to stash handles
	to_close = kmalloc(sizeof(*to_close) * n);

	// When no spare memory to use, close in‑place under lock (less ideal)
	if (to_close == NULL) {
		lock_acquire(p->p_fdtable_lock);

		for (i = 0; i < n; i++) {
			if (p->p_fdtable[i].fd_file) {
				fh_release(p->p_fdtable[i].fd_file);
				p->p_fdtable[i].fd_file = NULL;
				p->p_fdtable[i].fd_flags = 0;
			}
		}

		lock_release(p->p_fdtable_lock);

		return;
	}

	// Pluck them out under lock
	lock_acquire(p->p_fdtable_lock);

	int m = 0;
	for (i = 0; i < n; i++) {
		if (p->p_fdtable[i].fd_file) {
			to_close[m++] = p->p_fdtable[i].fd_file;
			p->p_fdtable[i].fd_file = NULL;
			p->p_fdtable[i].fd_flags = 0;
		}
	}

	lock_release(p->p_fdtable_lock);

	// Drop references outside the table lock
	for (i = 0; i < m; i++) {
		fh_release(to_close[i]);
	}

	kfree(to_close);
}
