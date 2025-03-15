/*
 * All the contents of this file are overwritten during automated
 * testing. Please consider this before changing anything in this file.
 */

#include <types.h>
#include <lib.h>
#include <clock.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/test161.h>
#include <spinlock.h>
#include <current.h>

/*
 * Readers-writer lock tests.
 */

#define CREATELOOPS 8
#define NTHREADS 32
#define NREADERLOCKLOOPS 100
#define NWRITEPERLOOPS 25 // each 25 reader loops 1 writer will be called
#define NWRITERLOCKLOOPS (NREADERLOCKLOOPS/NWRITEPERLOOPS)

static struct semaphore *donesem = NULL;
static struct rwlock *testrwlock = NULL;
static struct cv *writerscv = NULL;
static struct lock *writerscvlock = NULL;

static volatile unsigned long testval1;
static volatile unsigned long testval2;
static volatile unsigned long testval3;

struct spinlock status_lock;
static volatile unsigned max_readers;
static bool test_status = TEST161_FAIL;

static
bool
failif(bool condition) {
	if (condition) {
		spinlock_acquire(&status_lock);
		test_status = TEST161_FAIL;
		spinlock_release(&status_lock);
	}
	return condition;
}

static
void
count_max_readers() {
	spinlock_acquire(&status_lock);
	unsigned cur_readers = threadarray_num(testrwlock->rwlock_active_readers);
	if (cur_readers > max_readers) {
		max_readers = cur_readers;
	}
	spinlock_release(&status_lock);
}

static
void
readertestthread(void *junk, unsigned long num) {
	(void)junk;
	(void)num;

	int i;

	for (i = 0; i < NREADERLOCKLOOPS; i++) {
		kprintf_t(".");

		if (i % NWRITEPERLOOPS == 0) {
			lock_acquire(writerscvlock);
			cv_signal(writerscv, writerscvlock);
			cv_signal(writerscv, writerscvlock);
			lock_release(writerscvlock);
		}

		rwlock_acquire_read(testrwlock);
		kprintf("reading (%lu-%d)\n", num, i);
		random_yielder(4);

		unsigned long local1 = testval1;
		unsigned long local2 = testval2;
		unsigned long local3 = testval3;

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);

		if (local1 != testval1) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);

		if (local2 != testval2) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);

		if (local2 != testval1 * testval1) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);
		
		if (local3 != testval3) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);
		
		if (local3 != testval1 % 3) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == NULL);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) > 0);

		count_max_readers();
		kprintf("read (%lu-%d)\n", num, i);
		rwlock_release_read(testrwlock);
	}

	V(donesem);
	return;

fail:
	rwlock_release_read(testrwlock);
	failif(true);
	V(donesem);
	return;
}

static
void
writertestthread(void *junk, unsigned long num) {
	(void)junk;

	int i;

	for (i = 0; i < NWRITERLOCKLOOPS; i++)
	{
		lock_acquire(writerscvlock);

		cv_wait(writerscv, writerscvlock);
		
		lock_release(writerscvlock);

		kprintf("*** writer acquiring (%lu)\n", num);
		rwlock_acquire_write(testrwlock);
		kprintf("!!! writing (%lu)\n", num);

		random_yielder(4);

		testval1 = num;
		testval2 = num * num;
		testval3 = num % 3;

		random_yielder(4);

		unsigned long local1 = testval1;
		unsigned long local2 = testval2;
		unsigned long local3 = testval3;

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);

		if (local1 != testval1) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);

		if (local1 != num) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);

		if (local2 != testval2) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);

		if (local2 != num * num) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);
		
		if (local3 != testval3) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);
		
		if (local3 != num % 3) {
			goto fail;
		}

		random_yielder(4);
		KASSERT(testrwlock->rwlock_active_writer == curthread);
		KASSERT(threadarray_num(testrwlock->rwlock_active_readers) == 0);

		kprintf("!!! wrote (%lu)\n", num);
		rwlock_release_write(testrwlock);
	}

	V(donesem);
	return;

fail:
	rwlock_release_write(testrwlock);
	failif(true);
	V(donesem);
	return;
}

int rwtest(int nargs, char **args) {
	(void)nargs;
	(void)args;

	int i, result;

	kprintf_n("Starting rwt1...\n");
	for (i=0; i<CREATELOOPS; i++) {
		kprintf_t(".");
		testrwlock = rwlock_create("testrwlock");
		if (testrwlock == NULL) {
			panic("rwt1: rwlock_create failed\n");
		}
		writerscv = cv_create("testrwlock_cv");
		if (writerscv == NULL) {
			panic("rwt1: cv_create failed\n");
		}
		writerscvlock = lock_create("testrwlock_cvlock");
		if (writerscvlock == NULL) {
			panic("rwt1: lock_create failed\n");
		}
		donesem = sem_create("donesem", 0);
		if (donesem == NULL) {
			panic("rwt1: sem_create failed\n");
		}
		if (i != CREATELOOPS - 1) {
			lock_destroy(writerscvlock);
			cv_destroy(writerscv);
			rwlock_destroy(testrwlock);
			sem_destroy(donesem);
		}
	}
	spinlock_init(&status_lock);
	test_status = TEST161_SUCCESS;

	testval1 = 161;
	testval2 = testval1 * testval1;
	testval3 = testval1 % 3;

	max_readers = 0;

	for (i = 0; i < NTHREADS; i++) {
		kprintf_t(".");

		result = thread_fork("rwlocktestwriter", NULL, writertestthread, NULL, i);
		if (result) {
			panic("rwt1: thread_fork failed: %s\n", strerror(result));
		}

		result = thread_fork("rwlocktestreader", NULL, readertestthread, NULL, i);
		if (result) {
			panic("rwt1: thread_fork failed: %s\n", strerror(result));
		}
	}

	for (i = 0; i < (NTHREADS * 2); i++) {
		kprintf_t(".");
		P(donesem);
	}

	kprintf("max readers: %u", max_readers);

	if (max_readers < NTHREADS) {
		test_status = TEST161_FAIL;
	}

	lock_destroy(writerscvlock);
	cv_destroy(writerscv);
	rwlock_destroy(testrwlock);
	sem_destroy(donesem);
	writerscvlock = NULL;
	writerscv = NULL;
	testrwlock = NULL;
	donesem = NULL;

	kprintf_t("\n");
	success(test_status, SECRET, "rwt1");

	return 0;
}

int rwtest2(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("Starting rwt2...\n");
	kprintf_n("(This test panics on success!)\n");

	testrwlock = rwlock_create("testrwlock");
	if (testrwlock == NULL) {
		panic("rwt2: rwlock_create failed\n");
	}

	secprintf(SECRET, "Should panic...", "rwt2");
	rwlock_release_read(testrwlock);

	/* Should not get here on success. */

	success(TEST161_FAIL, SECRET, "rwt2");

	rwlock_destroy(testrwlock);

	return 0;
}

int rwtest3(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("Starting rwt3...\n");
	kprintf_n("(This test panics on success!)\n");

	testrwlock = rwlock_create("testrwlock");
	if (testrwlock == NULL) {
		panic("rwt3: rwlock_create failed\n");
	}

	secprintf(SECRET, "Should panic...", "rwt3");
	rwlock_release_write(testrwlock);

	/* Should not get here on success. */

	success(TEST161_FAIL, SECRET, "rwt3");

	rwlock_destroy(testrwlock);

	return 0;
}

int rwtest4(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("Starting rwt4...\n");
	kprintf_n("(This test panics on success!)\n");

	testrwlock = rwlock_create("testrwlock");
	if (testrwlock == NULL) {
		panic("rwt4: rwlock_create failed\n");
	}

	secprintf(SECRET, "Shouldn't hang...", "rwt4");
	rwlock_acquire_read(testrwlock);
	rwlock_release_read(testrwlock);
	rwlock_acquire_write(testrwlock);
	rwlock_release_write(testrwlock);
	rwlock_acquire_read(testrwlock);
	secprintf(SECRET, "Should panic...", "rwt4");
	rwlock_destroy(testrwlock);

	/* Should not get here on success. */

	success(TEST161_FAIL, SECRET, "rwt4");

	rwlock_destroy(testrwlock);

	return 0;
}

int rwtest5(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("Starting rwt5...\n");
	kprintf_n("(This test panics on success!)\n");

	testrwlock = rwlock_create("testrwlock");
	if (testrwlock == NULL) {
		panic("rwt5: rwlock_create failed\n");
	}

	secprintf(SECRET, "Shouldn't hang...", "rwt5");
	rwlock_acquire_read(testrwlock);
	rwlock_release_read(testrwlock);
	rwlock_acquire_write(testrwlock);
	rwlock_release_write(testrwlock);
	rwlock_acquire_write(testrwlock);
	secprintf(SECRET, "Should panic...", "rwt5");
	rwlock_destroy(testrwlock);

	/* Should not get here on success. */

	success(TEST161_FAIL, SECRET, "rwt5");

	rwlock_destroy(testrwlock);

	return 0;
}
