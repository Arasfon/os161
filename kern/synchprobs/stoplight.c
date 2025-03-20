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
 * Driver code is in kern/tests/synchprobs.c We will replace that file. This
 * file is yours to modify as you see fit.
 *
 * You should implement your solution to the stoplight problem below. The
 * quadrant and direction mappings for reference: (although the problem is, of
 * course, stable under rotation)
 *
 *   |0 |
 * -     --
 *    01  1
 * 3  32
 * --    --
 *   | 2|
 *
 * As way to think about it, assuming cars drive on the right: a car entering
 * the intersection from direction X will enter intersection quadrant X first.
 * The semantics of the problem are that once a car enters any quadrant it has
 * to be somewhere in the intersection until it call leaveIntersection(),
 * which it should call while in the final quadrant.
 *
 * As an example, let's say a car approaches the intersection and needs to
 * pass through quadrants 0, 3 and 2. Once you call inQuadrant(0), the car is
 * considered in quadrant 0 until you call inQuadrant(3). After you call
 * inQuadrant(2), the car is considered in quadrant 2 until you call
 * leaveIntersection().
 *
 * You will probably want to write some helper functions to assist with the
 * mappings. Modular arithmetic can help, e.g. a car passing straight through
 * the intersection entering from direction X will leave to direction (X + 2)
 * % 4 and pass through quadrants X and (X + 3) % 4.  Boo-yah.
 *
 * Your solutions below should call the inQuadrant() and leaveIntersection()
 * functions in synchprobs.c to record their progress.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

static struct cv *quadrant_lock_cv;
static struct lock *global_lock;

static bool quadrant_locked[4];

static
void
lock_turning_right(uint32_t direction) {
	int first_quadrant = direction;

	lock_acquire(global_lock);

	while (quadrant_locked[first_quadrant]) {
		cv_wait(quadrant_lock_cv, global_lock);
	}

	quadrant_locked[first_quadrant] = true;

	lock_release(global_lock);
}

static
void
unlock_turning_right(uint32_t direction, uint32_t current_step) {
	int first_quadrant = direction;

	if (current_step != 0) {
		panic("stoplight: wrong right turn step");
	}

	lock_acquire(global_lock);

	quadrant_locked[first_quadrant] = false;
	cv_broadcast(quadrant_lock_cv, global_lock);

	lock_release(global_lock);
}

static
void
lock_going_straight(uint32_t direction) {
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;

	lock_acquire(global_lock);

	while (quadrant_locked[first_quadrant] || quadrant_locked[second_quadrant]) {
		cv_wait(quadrant_lock_cv, global_lock);
	}

	quadrant_locked[first_quadrant] = true;
	quadrant_locked[second_quadrant] = true;

	lock_release(global_lock);
}

static
void
unlock_going_straight(uint32_t direction, uint32_t current_step) {
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;

	if (current_step > 1) {
		panic("stoplight: wrong straight going step");
	}

	lock_acquire(global_lock);

	switch (current_step)
	{
	case 0:
		quadrant_locked[first_quadrant] = false;
		break;
	case 1:
		quadrant_locked[second_quadrant] = false;
		break;
	}

	cv_broadcast(quadrant_lock_cv, global_lock);

	lock_release(global_lock);
}

static
void
lock_turning_left(uint32_t direction) {
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;
	int third_quadrant = (direction + 2) % 4;

	lock_acquire(global_lock);

	while (quadrant_locked[first_quadrant] || quadrant_locked[second_quadrant] || quadrant_locked[third_quadrant]) {
		cv_wait(quadrant_lock_cv, global_lock);
	}

	quadrant_locked[first_quadrant] = true;
	quadrant_locked[second_quadrant] = true;
	quadrant_locked[third_quadrant] = true;

	lock_release(global_lock);
}

static
void
unlock_turning_left(uint32_t direction, uint32_t current_step) {
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;
	int third_quadrant = (direction + 2) % 4;

	if (current_step > 2) {
		panic("stoplight: wrong left turn step");
	}

	lock_acquire(global_lock);

	switch (current_step)
	{
	case 0:
		quadrant_locked[first_quadrant] = false;
		break;
	case 1:
		quadrant_locked[second_quadrant] = false;
		break;
	case 2:
		quadrant_locked[third_quadrant] = false;
		break;
	}

	cv_broadcast(quadrant_lock_cv, global_lock);

	lock_release(global_lock);
}

/*
 * Called by the driver during initialization.
 */

void
stoplight_init() {
	quadrant_lock_cv = cv_create("stoplight");
	global_lock = lock_create("stoplight_global");

	quadrant_locked[0] = false;
	quadrant_locked[1] = false;
	quadrant_locked[2] = false;
	quadrant_locked[3] = false;
}

/*
 * Called by the driver during teardown.
 */

void stoplight_cleanup() {
	lock_destroy(global_lock);

	cv_destroy(quadrant_lock_cv);

	global_lock = NULL;

	quadrant_lock_cv = NULL;

	quadrant_locked[3] = false;
	quadrant_locked[2] = false;
	quadrant_locked[1] = false;
	quadrant_locked[0] = false;
}

void
turnright(uint32_t direction, uint32_t index)
{
	int first_quadrant = direction;

	lock_turning_right(direction);
	inQuadrant(first_quadrant, index);
	leaveIntersection(index);
	unlock_turning_right(direction, 0);
}

void
gostraight(uint32_t direction, uint32_t index)
{
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;

	lock_going_straight(direction);
	inQuadrant(first_quadrant, index);
	inQuadrant(second_quadrant, index);
	unlock_going_straight(direction, 0);
	leaveIntersection(index);
	unlock_going_straight(direction, 1);
}

void
turnleft(uint32_t direction, uint32_t index)
{
	int first_quadrant = direction;
	int second_quadrant = (direction + 3) % 4;
	int third_quadrant = (direction + 2) % 4;

	lock_turning_left(direction);
	inQuadrant(first_quadrant, index);
	inQuadrant(second_quadrant, index);
	unlock_turning_left(direction, 0);
	inQuadrant(third_quadrant, index);
	unlock_turning_left(direction, 1);
	leaveIntersection(index);
	unlock_turning_left(direction, 2);
}
