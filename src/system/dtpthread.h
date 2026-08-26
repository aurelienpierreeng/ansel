/*
    This file is part of darktable,
    Copyright (C) 2010-2011, 2014 johannes hanika.
    Copyright (C) 2011, 2014, 2016-2017, 2020 Tobias Ellinghaus.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Stuart Henderson.
    Copyright (C) 2014-2017 Roman Lebedev.
    Copyright (C) 2017 Christian Tellefsen.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2020 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023-2025 Aurélien PIERRE.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_SYSTEM_DTPTHREAD_H
#define DT_SYSTEM_DTPTHREAD_H

#include "external/ThreadSafetyAnalysis.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One implementation, always.
 *
 * There used to be a second one behind #ifdef _DEBUG: ~320 lines carrying per-lock names,
 * acquisition timing, a top-3 contention table and printf warnings. It is gone, for the
 * reason its own neighbours documented. caches/cache.c still carries the epitaph of the
 * last time this file had two arms:
 *
 *   "There used to be an #ifdef _DEBUG split here ... the non-_DEBUG arm stopped compiling
 *    -- and nobody found out, because this whole block only exists under AddressSanitizer
 *    and nothing in CI builds with it."
 *
 * A second implementation that no test exercises does not verify anything; it only gives
 * the analysers a second, unreachable body to reason about. All nine BLOCKER findings
 * SonarCloud reported against this file were in that arm, and none of them described code
 * that ships.
 *
 * What is kept is what earns its place at compile time or at run time:
 *
 *   - the CAPABILITY/ACQUIRE/RELEASE annotations, which drive clang's -Wthread-safety
 *     (enabled in cmake/compiler-warnings.cmake). A bare pthread_mutex_t cannot carry
 *     them: the wrapper struct is what makes lock discipline checkable at all.
 *   - dt_pthread_rwlock_t's same-thread recursive-writer tracking, which is not
 *     instrumentation but a deadlock fix -- see the comment on the type below.
 */


typedef struct CAPABILITY("mutex") dt_pthread_mutex_t
{
  pthread_mutex_t mutex;
} CAPABILITY("mutex") dt_pthread_mutex_t;

// *please* do use these;
/** @brief Initialise a mutex. With @p mutexattr NULL -- which is how 54 of the 56 call
 * sites in this tree spell it -- the mutex is RECURSIVE.
 *
 * @details A thread re-entering a lock it already holds cannot race itself: while it holds
 * the lock no other thread is inside the critical section, so the data it protects is as
 * safe at depth 2 as at depth 1. What a non-recursive mutex does in that situation is
 * deadlock -- turning a harmless nesting into a frozen application, which is a worse
 * outcome than the thing it is meant to prevent.
 *
 * The codebase had already reached that conclusion one lock at a time: darktable.c makes
 * the exiv2 mutex recursive so "a public exif function can hold it across its whole
 * critical section while inner helpers or re-entrant calls re-lock it without deadlocking."
 * This makes it the default instead of a per-lock rediscovery.
 *
 * @param mutex the mutex to initialise.
 * @param mutexattr NULL for the recursive default, or an explicit attribute to override it.
 *
 * @warning Recursion is NOT free of consequence, and there is exactly one place it bites:
 * pthread_cond_wait() releases the mutex ONCE. A thread that waits while holding a
 * recursive mutex at depth > 1 therefore never releases it and the wait cannot be signalled
 * -- POSIX calls the combination undefined. At depth 1 it behaves exactly as before. The
 * eight cond-wait sites in this tree all take their mutex once at the top of a wait loop;
 * see dt_pthread_cond_wait() below, which repeats this where it would be noticed.
 *
 * @note Recursion also hides a real class of bug that a deadlock would have exposed: a
 * function that breaks an invariant, then calls something that re-enters and reads the
 * half-updated state. That trade is deliberate -- a rare, quiet correctness risk in place
 * of a frequent, total hang.
 */
static inline int dt_pthread_mutex_init(dt_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
  if(mutexattr) return pthread_mutex_init(&mutex->mutex, mutexattr);

  pthread_mutexattr_t recursive;
  int res = pthread_mutexattr_init(&recursive);
  if(res) return res;
  res = pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
  if(!res) res = pthread_mutex_init(&mutex->mutex, &recursive);
  pthread_mutexattr_destroy(&recursive);
  return res;
};

static inline int dt_pthread_mutex_lock(dt_pthread_mutex_t *mutex) ACQUIRE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  return pthread_mutex_lock(&mutex->mutex);
};

static inline int dt_pthread_mutex_trylock(dt_pthread_mutex_t *mutex) TRY_ACQUIRE(0, mutex)
{
  return pthread_mutex_trylock(&mutex->mutex);
};

static inline int dt_pthread_mutex_unlock(dt_pthread_mutex_t *mutex) RELEASE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  return pthread_mutex_unlock(&mutex->mutex);
};

static inline int dt_pthread_mutex_destroy(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_destroy(&mutex->mutex);
};

/** @brief Wait on @p cond, releasing @p mutex for the duration.
 *
 * @warning @p mutex must be held EXACTLY ONCE by the calling thread. Mutexes from
 * dt_pthread_mutex_init(..., NULL) are recursive, and pthread_cond_wait() releases a mutex
 * once regardless of how deep the caller holds it: waiting at depth > 1 leaves it held, so
 * no other thread can take it to signal, and the wait never returns. POSIX calls this
 * undefined; in practice it is a silent hang.
 *
 * Every wait in this tree takes its mutex at the top of a loop and waits at depth 1, which
 * is the shape this is safe in. If you ever need to wait from inside a nested critical
 * section, unwind to depth 1 first -- do not add a "recursive wait".
 */
static inline int dt_pthread_cond_wait(pthread_cond_t *cond, dt_pthread_mutex_t *mutex)
{
  return pthread_cond_wait(cond, &mutex->mutex);
};

// Same-thread recursive write-lock tracking.
/* Annotated as a clang CAPABILITY so -Wthread-safety can check it. The point of doing so is
 * GUARDED_BY on the DATA these locks protect: clang's analysis is declarative -- it proves
 * that every access to an annotated field happens while the named lock is held -- which is a
 * different and stronger thing than symbolic execution guessing at lock state. It also does
 * not care that our writers are recursive, because it is not counting.
 *
 * Each accessor carries NO_THREAD_SAFETY_ANALYSIS on its own body: the body deliberately
 * acquires or releases without a matching partner, which is exactly what the attribute is
 * for. The ACQUIRE/RELEASE annotation is what callers are checked against.
 */
// A thread that already holds the write lock cannot race itself: no other thread can be
// touching the protected data while the write lock is held, so letting that same thread
// re-enter (as reader or writer) is safe for data validity. Without this, glibc's default
// PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP policy self-deadlocks such a thread as soon as
// a second thread is queued waiting for the write lock (new readers, including from the
// writer's own thread, are blocked once a writer is waiting).
typedef struct CAPABILITY("rwlock") dt_pthread_rwlock_t
{
  pthread_rwlock_t lock;
  pthread_t writer;
  int writer_depth;
  // Temporary diagnostic hook (find_history_mutex_blocker): NULL for every lock except the
  // ones explicitly named via dt_pthread_rwlock_set_name(). Zero overhead for unnamed locks
  // (one pointer compare). last_holder_tid is best-effort/racy by design -- it's a debug hint
  // for "who was probably holding this right before I blocked", not a correctness primitive.
  const char *name;
  long last_holder_tid;
  gboolean last_holder_was_writer;
  // Tracks the reader holding this lock the longest, to tell apart "the writer is queued
  // behind a reader that grabbed it a moment ago" from "some reader has been sitting on this
  // for the whole wait" -- the single last_holder_tid above can't distinguish those, since it
  // reflects whoever *most recently acquired*, not whoever has been holding it longest.
  int active_reader_count;
  double oldest_active_reader_since;
  long oldest_active_reader_tid;
} CAPABILITY("rwlock") dt_pthread_rwlock_t;

static inline int dt_pthread_rwlock_init(dt_pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
{
  lock->writer = 0;
  lock->writer_depth = 0;
  lock->name = NULL;
  lock->last_holder_tid = 0;
  lock->last_holder_was_writer = FALSE;
  lock->active_reader_count = 0;
  lock->oldest_active_reader_since = 0.0;
  lock->oldest_active_reader_tid = 0;
  return pthread_rwlock_init(&lock->lock, attr);
}

// Opt-in: call once after dt_pthread_rwlock_init() to enable wait-time diagnostics on this
// specific lock instance. Temporary, for find_history_mutex_blocker -- remove once resolved.
static inline void dt_pthread_rwlock_set_name(dt_pthread_rwlock_t *lock, const char *name)
{
  lock->name = name;
}

static inline int dt_pthread_rwlock_destroy(dt_pthread_rwlock_t *lock)
{
  return pthread_rwlock_destroy(&lock->lock);
}

static inline int dt_pthread_rwlock_unlock(dt_pthread_rwlock_t *rwlock) RELEASE_GENERIC(rwlock) NO_THREAD_SAFETY_ANALYSIS
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth > 1)
  {
    rwlock->writer_depth--;
    return 0;
  }
  const gboolean writer_was_self = pthread_equal(rwlock->writer, pthread_self());
  if(rwlock->name && !writer_was_self)
  {
    // A reader unlocking (writer_was_self is only true for a thread that took the write lock;
    // recursive-writer-as-reader re-entry already returned above via writer_depth).
    if(__sync_fetch_and_sub(&rwlock->active_reader_count, 1) == 1)
    {
      rwlock->oldest_active_reader_since = 0.0;
      rwlock->oldest_active_reader_tid = 0;
    }
  }
  const int res = pthread_rwlock_unlock(&rwlock->lock);
  if(writer_was_self)
  {
    rwlock->writer_depth = 0;
    __sync_bool_compare_and_swap(&(rwlock->writer), pthread_self(), 0);
  }
  return res;
}

// Diagnostic-only. This header is included too early in the chain (darktable.h includes it
// before declaring dt_debug_thread_t/darktable_t/dt_print) for dt_print() to be callable
// directly from the inline functions below -- so the actual logging is delegated to these two
// non-inline helpers, implemented in dtpthread.c (which, being a .c file and not part of the
// header cycle, can include darktable.h and call dt_print(DT_DEBUG_HISTORY, ...) there).
// This makes the traces respect `-d history` like every other diagnostic instead of always
// firing via a raw fprintf. Only ever called for locks opted in via dt_pthread_rwlock_set_name();
// zero-cost (one pointer compare) for every other lock. Temporary, for
// find_history_mutex_blocker -- remove once resolved.
void _dt_pthread_rwlock_diag_log_rdlock(const char *name, unsigned long tid, double wait_ms,
                                         unsigned long prev_holder, gboolean prev_was_writer);
void _dt_pthread_rwlock_diag_log_wrlock(const char *name, unsigned long tid, double wait_ms,
                                         unsigned long prev_holder, gboolean prev_was_writer,
                                         int active_readers, unsigned long oldest_reader_tid,
                                         double oldest_reader_age_ms);

static inline double _dt_pthread_rwlock_diag_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline int dt_pthread_rwlock_rdlock(dt_pthread_rwlock_t *rwlock) ACQUIRE_SHARED(rwlock) NO_THREAD_SAFETY_ANALYSIS
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1)
  {
    rwlock->writer_depth++;
    return 0;
  }
  if(rwlock->name)
  {
    const double _start = _dt_pthread_rwlock_diag_now();
    const long _prev_holder = rwlock->last_holder_tid;
    const gboolean _prev_was_writer = rwlock->last_holder_was_writer;
    const int res = pthread_rwlock_rdlock(&rwlock->lock);
    const double _wait_ms = (_dt_pthread_rwlock_diag_now() - _start) * 1000.0;
    if(_wait_ms > 1.0)
      _dt_pthread_rwlock_diag_log_rdlock(rwlock->name, (unsigned long)pthread_self(), _wait_ms,
                                          (unsigned long)_prev_holder, _prev_was_writer);
    if(!res)
    {
      rwlock->last_holder_tid = (long)pthread_self();
      rwlock->last_holder_was_writer = FALSE;
      if(__sync_fetch_and_add(&rwlock->active_reader_count, 1) == 0)
      {
        rwlock->oldest_active_reader_since = _dt_pthread_rwlock_diag_now();
        rwlock->oldest_active_reader_tid = (long)pthread_self();
      }
    }
    return res;
  }
  return pthread_rwlock_rdlock(&rwlock->lock);
}

static inline int dt_pthread_rwlock_wrlock(dt_pthread_rwlock_t *rwlock) ACQUIRE(rwlock) NO_THREAD_SAFETY_ANALYSIS
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1)
  {
    rwlock->writer_depth++;
    return 0;
  }
  if(rwlock->name)
  {
    const double _start = _dt_pthread_rwlock_diag_now();
    const long _prev_holder = rwlock->last_holder_tid;
    const gboolean _prev_was_writer = rwlock->last_holder_was_writer;
    const int _readers_now = rwlock->active_reader_count;
    const long _oldest_reader_tid = rwlock->oldest_active_reader_tid;
    const double _oldest_reader_age_ms
        = _readers_now > 0 ? (_start - rwlock->oldest_active_reader_since) * 1000.0 : 0.0;
    const int res = pthread_rwlock_wrlock(&rwlock->lock);
    const double _wait_ms = (_dt_pthread_rwlock_diag_now() - _start) * 1000.0;
    if(_wait_ms > 1.0)
      _dt_pthread_rwlock_diag_log_wrlock(rwlock->name, (unsigned long)pthread_self(), _wait_ms,
                                          (unsigned long)_prev_holder, _prev_was_writer, _readers_now,
                                          (unsigned long)_oldest_reader_tid, _oldest_reader_age_ms);
    if(!res)
    {
      __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
      rwlock->writer_depth = 1;
      rwlock->last_holder_tid = (long)pthread_self();
      rwlock->last_holder_was_writer = TRUE;
    }
    return res;
  }
  const int res = pthread_rwlock_wrlock(&rwlock->lock);
  if(!res)
  {
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    rwlock->writer_depth = 1;
  }
  return res;
}

static inline int dt_pthread_rwlock_tryrdlock(dt_pthread_rwlock_t *rwlock) TRY_ACQUIRE_SHARED(0, rwlock) NO_THREAD_SAFETY_ANALYSIS
{
  // Keep try* locks honest as "is it locked by anyone?" probes: do NOT report success just
  // because the current thread already holds the write lock (see dt_pthread_rwlock_rdlock
  // above for the blocking-call recursion, which is the case this is not).
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1) return EBUSY;
  return pthread_rwlock_tryrdlock(&rwlock->lock);
}

static inline int dt_pthread_rwlock_trywrlock(dt_pthread_rwlock_t *rwlock) TRY_ACQUIRE(0, rwlock) NO_THREAD_SAFETY_ANALYSIS
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1) return EBUSY;
  const int res = pthread_rwlock_trywrlock(&rwlock->lock);
  if(!res)
  {
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    rwlock->writer_depth = 1;
  }
  return res;
}

#define dt_pthread_rwlock_rdlock_with_caller(A,B,C) dt_pthread_rwlock_rdlock(A)
#define dt_pthread_rwlock_wrlock_with_caller(A,B,C) dt_pthread_rwlock_wrlock(A)
#define dt_pthread_rwlock_tryrdlock_with_caller(A,B,C) dt_pthread_rwlock_tryrdlock(A)
#define dt_pthread_rwlock_trywrlock_with_caller(A,B,C) dt_pthread_rwlock_trywrlock(A)


// if at all possible, do NOT use.
static inline int dt_pthread_mutex_BAD_lock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_lock(&mutex->mutex);
};

static inline int dt_pthread_mutex_BAD_trylock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_trylock(&mutex->mutex);
};

static inline int dt_pthread_mutex_BAD_unlock(dt_pthread_mutex_t *mutex)
{
  return pthread_mutex_unlock(&mutex->mutex);
};

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg, const gboolean realtime);

void dt_pthread_setname(const char *name);

#endif // DT_SYSTEM_DTPTHREAD_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
