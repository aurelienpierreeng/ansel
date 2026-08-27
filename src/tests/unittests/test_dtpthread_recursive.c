/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

/** Locks initialised with a NULL attribute are recursive.
 *
 * A thread re-entering a lock it already holds cannot race itself, so the data stays
 * protected; a non-recursive mutex answers that situation with a deadlock, which is a
 * worse outcome than the one it prevents. dt_pthread_mutex_init(..., NULL) therefore
 * produces a recursive mutex, and dt_pthread_rwlock_t tracks same-thread writer depth.
 *
 * These tests probe with TRYLOCK rather than lock(). Both express the property, but a
 * regression caught by trylock is a failed assertion, while a regression caught by lock()
 * is a hung test binary that CI can only kill on timeout. The second acquisition through
 * the real lock() path is exercised too -- after trylock has already established that it
 * cannot block.
 */

#include "external/ThreadSafetyAnalysis.h"
#include "system/dtpthread.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

/*
 * These four exercise re-entrant locking on purpose, which is exactly the pattern clang's
 * thread-safety analysis is built to reject: it models a lock as held or not held, so a
 * second acquisition by the owner reads as "acquiring a lock that is already held" and the
 * matching second release as "releasing a lock that was not held". The behaviour under test
 * is real and deliberate (see dtpthread.h); the analysis simply has no way to express it.
 * Exempting the test bodies keeps the tree's finding count honest -- these are not defects
 * anyone can fix, and left in they would mask ones that are.
 */

/** The property, stated without risking a hang: the owner can take it again. */
static void _a_null_attr_mutex_is_recursive(void **state) NO_THREAD_SAFETY_ANALYSIS
{
  (void)state;
  dt_pthread_mutex_t mutex;
  assert_int_equal(dt_pthread_mutex_init(&mutex, NULL), 0);

  assert_int_equal(dt_pthread_mutex_lock(&mutex), 0);

  // On a non-recursive mutex this returns EBUSY (NORMAL) or EDEADLK (ERRORCHECK).
  assert_int_equal(dt_pthread_mutex_trylock(&mutex), 0);
  assert_int_equal(dt_pthread_mutex_unlock(&mutex), 0);

  assert_int_equal(dt_pthread_mutex_unlock(&mutex), 0);
  assert_int_equal(dt_pthread_mutex_destroy(&mutex), 0);
}

/** The same through the blocking entry point, which is what callers actually use.
 * Safe to run only because the test above already proved it cannot block. */
static void _the_blocking_lock_also_re_enters(void **state) NO_THREAD_SAFETY_ANALYSIS
{
  (void)state;
  dt_pthread_mutex_t mutex;
  assert_int_equal(dt_pthread_mutex_init(&mutex, NULL), 0);

  for(int depth = 0; depth < 4; depth++) assert_int_equal(dt_pthread_mutex_lock(&mutex), 0);
  for(int depth = 0; depth < 4; depth++) assert_int_equal(dt_pthread_mutex_unlock(&mutex), 0);

  // Fully released: a fresh acquisition still succeeds.
  assert_int_equal(dt_pthread_mutex_trylock(&mutex), 0);
  assert_int_equal(dt_pthread_mutex_unlock(&mutex), 0);
  assert_int_equal(dt_pthread_mutex_destroy(&mutex), 0);
}

/** An explicit attribute still wins: this is the override path, not a hardcoded policy. */
static void _an_explicit_attribute_is_honoured(void **state) NO_THREAD_SAFETY_ANALYSIS
{
  (void)state;
  pthread_mutexattr_t errorcheck;
  assert_int_equal(pthread_mutexattr_init(&errorcheck), 0);
  assert_int_equal(pthread_mutexattr_settype(&errorcheck, PTHREAD_MUTEX_ERRORCHECK), 0);

  dt_pthread_mutex_t mutex;
  assert_int_equal(dt_pthread_mutex_init(&mutex, &errorcheck), 0);
  assert_int_equal(dt_pthread_mutex_lock(&mutex), 0);

  // Not recursive, because the caller asked for something else.
  assert_int_not_equal(dt_pthread_mutex_trylock(&mutex), 0);

  assert_int_equal(dt_pthread_mutex_unlock(&mutex), 0);
  assert_int_equal(dt_pthread_mutex_destroy(&mutex), 0);
  pthread_mutexattr_destroy(&errorcheck);
}

/** The rwlock's writer may re-enter, as reader or as writer.
 *
 * This is the deadlock fix rather than a convenience: glibc's default
 * PREFER_WRITER_NONRECURSIVE policy blocks a re-entering thread as soon as another writer
 * is queued, and dt_dev_pixelpipe_change() can re-enter while holding history_mutex. */
static void _an_rwlock_writer_may_re_enter(void **state) NO_THREAD_SAFETY_ANALYSIS
{
  (void)state;
  dt_pthread_rwlock_t lock;
  assert_int_equal(dt_pthread_rwlock_init(&lock, NULL), 0);

  assert_int_equal(dt_pthread_rwlock_wrlock(&lock), 0);
  assert_int_equal(dt_pthread_rwlock_wrlock(&lock), 0);   // same thread, as writer
  assert_int_equal(dt_pthread_rwlock_rdlock(&lock), 0);   // same thread, as reader

  assert_int_equal(dt_pthread_rwlock_unlock(&lock), 0);
  assert_int_equal(dt_pthread_rwlock_unlock(&lock), 0);
  assert_int_equal(dt_pthread_rwlock_unlock(&lock), 0);

  assert_int_equal(dt_pthread_rwlock_destroy(&lock), 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_null_attr_mutex_is_recursive),
    cmocka_unit_test(_the_blocking_lock_also_re_enters),
    cmocka_unit_test(_an_explicit_attribute_is_honoured),
    cmocka_unit_test(_an_rwlock_writer_may_re_enter),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
