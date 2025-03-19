/*
 * Copyright (c) 2001, 2002, 2009
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
 * Driver code is in kern/tests/synchprobs.c We will
 * replace that file. This file is yours to modify as you see fit.
 *
 * You should implement your solution to the whalemating problem below.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

static struct semaphore *male_sem;
static struct semaphore *female_sem;
static struct semaphore *matchmaker_sem;
static struct lock *gl_lock;

static volatile unsigned male_count;
static volatile unsigned female_count;
static volatile unsigned matchmaker_count;

/*
 * Called by the driver during initialization.
 */

void whalemating_init() {
	male_sem = sem_create("male_sem", 0);
	female_sem = sem_create("female_sem", 0);
	matchmaker_sem = sem_create("matchmaker_sem", 0);
	gl_lock = lock_create("global_lock");

	male_count = 0;
	female_count = 0;
	matchmaker_count = 0;
}

/*
 * Called by the driver during teardown.
 */

void
whalemating_cleanup() {
	matchmaker_count = 0;
	female_count = 0;
	male_count = 0;

	lock_destroy(gl_lock);
	sem_destroy(matchmaker_sem);
	sem_destroy(female_sem);
	sem_destroy(male_sem);

	gl_lock = NULL;
	matchmaker_sem = NULL;
	female_sem = NULL;
	male_sem = NULL;
}

void
male(uint32_t index)
{
	male_start(index);

	lock_acquire(gl_lock);

	if (female_count > 0 && matchmaker_count > 0) {
		female_count--;
		matchmaker_count--;
		lock_release(gl_lock);

		V(female_sem);
		V(matchmaker_sem);
	} else {
		male_count++;
		lock_release(gl_lock);
		P(male_sem);
	}

	male_end(index);
}

void
female(uint32_t index)
{
	female_start(index);

	lock_acquire(gl_lock);

	if (male_count > 0 && matchmaker_count > 0) {
		male_count--;
		matchmaker_count--;
		lock_release(gl_lock);

		V(male_sem);
		V(matchmaker_sem);
	} else {
		female_count++;
		lock_release(gl_lock);
		P(female_sem);
	}

	female_end(index);
}

void
matchmaker(uint32_t index)
{
	matchmaker_start(index);

	lock_acquire(gl_lock);

	if (male_count > 0 && female_count > 0) {
		male_count--;
		female_count--;
		lock_release(gl_lock);

		V(male_sem);
		V(female_sem);
	} else {
		matchmaker_count++;
		lock_release(gl_lock);
		P(matchmaker_sem);
	}

	matchmaker_end(index);
}
