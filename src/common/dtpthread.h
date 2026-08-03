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

#pragma once

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

#ifdef _DEBUG

// copied from darktable.h so we don't need to include the header
#include <sys/time.h>
static inline double dt_pthread_get_wtime()
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}


#define TOPN 3
typedef struct CAPABILITY("mutex") dt_pthread_mutex_t
{
  pthread_mutex_t mutex;
  char name[256];
  double time_locked;
  double time_sum_wait;
  double time_sum_locked;
  char top_locked_name[TOPN][256];
  double top_locked_sum[TOPN];
  char top_wait_name[TOPN][256];
  double top_wait_sum[TOPN];
} CAPABILITY("mutex") dt_pthread_mutex_t;

typedef struct dt_pthread_rwlock_t
{
  pthread_rwlock_t lock;
  int cnt;
  pthread_t writer;
  int writer_depth;
  char name[256];
} dt_pthread_rwlock_t;

static inline int dt_pthread_mutex_destroy(dt_pthread_mutex_t *mutex)
{
  const int ret = pthread_mutex_destroy(&(mutex->mutex));
  assert(!ret);

#if 0
  printf("\n[mutex] stats for mutex `%s':\n", mutex->name);
  printf("[mutex] total time locked: %.3f secs\n", mutex->time_sum_locked);
  printf("[mutex] total wait time  : %.3f secs\n", mutex->time_sum_wait);
  printf("[mutex] top %d lockers   :\n", TOPN);
  for(int k=0; k<TOPN; k++) printf("[mutex]  %.3f secs : `%s'\n", mutex->top_locked_sum[k],
  mutex->top_locked_name[k]);
  printf("[mutex] top %d waiters   :\n", TOPN);
  for(int k=0; k<TOPN; k++) printf("[mutex]  %.3f secs : `%s'\n", mutex->top_wait_sum[k],
  mutex->top_wait_name[k]);
#endif

  return ret;
}

#define dt_pthread_mutex_init(A, B) dt_pthread_mutex_init_with_caller(A, B, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_init_with_caller(dt_pthread_mutex_t *mutex,
                                                    const pthread_mutexattr_t *attr, const char *file,
                                                    const int line, const char *function)
{
  memset(mutex, 0x0, sizeof(dt_pthread_mutex_t));
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
#if defined(__OpenBSD__)
  if(IS_NULL_PTR(attr))
  {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_NORMAL);
    const int ret = pthread_mutex_init(&(mutex->mutex), &a);
    pthread_mutexattr_destroy(&a);
    return ret;
  }
#endif
  const int ret = pthread_mutex_init(&(mutex->mutex), attr);
  assert(!ret);
  return ret;
}

#define dt_pthread_mutex_lock(A) dt_pthread_mutex_lock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_lock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                    const int line, const char *function)
  ACQUIRE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  const double t0 = dt_pthread_get_wtime();
  const int ret = pthread_mutex_lock(&(mutex->mutex));
  assert(!ret);
  mutex->time_locked = dt_pthread_get_wtime();
  double wait = mutex->time_locked - t0;
  mutex->time_sum_wait += wait;
  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  // TODO: have a -d thread option
  //fprintf(stdout, "Thread lock %s acquired\n", mutex->name);
  int min_wait_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_wait_sum[k] < mutex->top_wait_sum[min_wait_slot]) min_wait_slot = k;
    if(!strncmp(name, mutex->top_wait_name[k], 256))
    {
      mutex->top_wait_sum[k] += wait;
      return ret;
    }
  }
  g_strlcpy(mutex->top_wait_name[min_wait_slot], name, sizeof(mutex->top_wait_name[min_wait_slot]));
  mutex->top_wait_sum[min_wait_slot] = wait;
  return ret;
}

#define dt_pthread_mutex_trylock(A) dt_pthread_mutex_trylock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_trylock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                       const int line, const char *function)
  TRY_ACQUIRE(0, mutex)
{
  const double t0 = dt_pthread_get_wtime();
  const int ret = pthread_mutex_trylock(&(mutex->mutex));
  assert(!ret || (ret == EBUSY));
  if(ret) return ret;
  mutex->time_locked = dt_pthread_get_wtime();
  double wait = mutex->time_locked - t0;
  mutex->time_sum_wait += wait;
  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  int min_wait_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_wait_sum[k] < mutex->top_wait_sum[min_wait_slot]) min_wait_slot = k;
    if(!strncmp(name, mutex->top_wait_name[k], 256))
    {
      mutex->top_wait_sum[k] += wait;
      return ret;
    }
  }
  g_strlcpy(mutex->top_wait_name[min_wait_slot], name, sizeof(mutex->top_wait_name[min_wait_slot]));
  mutex->top_wait_sum[min_wait_slot] = wait;
  return ret;
}

#define dt_pthread_mutex_unlock(A) dt_pthread_mutex_unlock_with_caller(A, __FILE__, __LINE__, __FUNCTION__)
static inline int dt_pthread_mutex_unlock_with_caller(dt_pthread_mutex_t *mutex, const char *file,
                                                      const int line, const char *function)
  RELEASE(mutex) NO_THREAD_SAFETY_ANALYSIS
{
  const double t0 = dt_pthread_get_wtime();
  const double locked = t0 - mutex->time_locked;
  mutex->time_sum_locked += locked;

  char *name = mutex->name;
  snprintf(mutex->name, sizeof(mutex->name), "%s:%d (%s)", file, line, function);
  // TODO: have a -d thread debug arg
  //fprintf(stdout, "Thread lock %s released\n", mutex->name);
  int min_locked_slot = 0;
  for(int k = 0; k < TOPN; k++)
  {
    if(mutex->top_locked_sum[k] < mutex->top_locked_sum[min_locked_slot]) min_locked_slot = k;
    if(!strncmp(name, mutex->top_locked_name[k], 256))
    {
      mutex->top_locked_sum[k] += locked;
      min_locked_slot = -1;
      break;
    }
  }
  if(min_locked_slot >= 0)
  {
    g_strlcpy(mutex->top_locked_name[min_locked_slot], name, sizeof(mutex->top_locked_name[min_locked_slot]));
    mutex->top_locked_sum[min_locked_slot] = locked;
  }

  // need to unlock last, to shield our internal data.
  const int ret = pthread_mutex_unlock(&(mutex->mutex));
  assert(!ret);
  return ret;
}

static inline int dt_pthread_cond_wait(pthread_cond_t *cond, dt_pthread_mutex_t *mutex)
{
  return pthread_cond_wait(cond, &(mutex->mutex));
}


static inline int dt_pthread_rwlock_init(dt_pthread_rwlock_t *lock,
    const pthread_rwlockattr_t *attr)
{
  memset(lock, 0, sizeof(dt_pthread_rwlock_t));
  lock->cnt = 0;
  lock->writer_depth = 0;
  const int res = pthread_rwlock_init(&lock->lock, attr);
  assert(!res);
  return res;
}

// In this debug variant the name field is diagnostic scratch, overwritten
// with the caller's file:line on every lock operation; accept the label
// anyway so callers compile identically in both variants (the release
// variant uses it to opt into wait-time diagnostics).
static inline void dt_pthread_rwlock_set_name(dt_pthread_rwlock_t *lock, const char *name)
{
  snprintf(lock->name, sizeof(lock->name), "%s", name);
}

static inline int dt_pthread_rwlock_destroy(dt_pthread_rwlock_t *lock)
{
  if(lock->writer_depth != 0)
    fprintf(stderr, "ERROR: destroying rwlock still owned by writer (depth=%d cnt=%d last=%s)\n",
            lock->writer_depth, lock->cnt, lock->name);
  assert(lock->writer_depth == 0);
  snprintf(lock->name, sizeof(lock->name), "destroyed with cnt %d", lock->cnt);
  const int res = pthread_rwlock_destroy(&lock->lock);
  assert(!res);
  return res;
}

static inline pthread_t dt_pthread_rwlock_get_writer(dt_pthread_rwlock_t *lock)
{
  return lock->writer;
}

#define dt_pthread_rwlock_unlock(A) dt_pthread_rwlock_unlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_unlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth > 1)
  {
    __sync_fetch_and_sub(&(rwlock->cnt), 1);
    rwlock->writer_depth--;
    assert(rwlock->writer_depth >= 1);
    assert(rwlock->cnt >= 0);
    snprintf(rwlock->name, sizeof(rwlock->name), "nu:%s:%d", file, line);
    fprintf(stdout, "Warning: thread lock owned by the same thread is unlocked again by %s\n", rwlock->name);
    return 0;
  }

  const gboolean writer_was_self = pthread_equal(rwlock->writer, pthread_self());
  const int res = pthread_rwlock_unlock(&rwlock->lock);
  assert(!res);
  __sync_fetch_and_sub(&(rwlock->cnt), 1);
  assert(rwlock->cnt >= 0);
  __sync_bool_compare_and_swap(&(rwlock->writer), pthread_self(), 0);
  if(writer_was_self) rwlock->writer_depth = 0;
  if(!res) snprintf(rwlock->name, sizeof(rwlock->name), "u:%s:%d", file, line);
  return res;
}

#define dt_pthread_rwlock_rdlock(A) dt_pthread_rwlock_rdlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_rdlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    rwlock->writer_depth++;
    snprintf(rwlock->name, sizeof(rwlock->name), "wr:%s:%d", file, line);
    fprintf(stdout, "Warning: thread lock owned by the same thread is locked again by %s\n", rwlock->name);
    return 0;
  }

  const int res = pthread_rwlock_rdlock(&rwlock->lock);
  assert(!res);
  __sync_fetch_and_add(&(rwlock->cnt), 1);
  if(!res)
    snprintf(rwlock->name, sizeof(rwlock->name), "r:%s:%d", file, line);
  return res;
}
#define dt_pthread_rwlock_wrlock(A) dt_pthread_rwlock_wrlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_wrlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    rwlock->writer_depth++;
    snprintf(rwlock->name, sizeof(rwlock->name), "ww:%s:%d", file, line);
    fprintf(stdout, "Warning: thread lock owned by the same thread is locked again by %s\n", rwlock->name);
    return 0;
  }

  const int res = pthread_rwlock_wrlock(&rwlock->lock);
  assert(!res);
  __sync_fetch_and_add(&(rwlock->cnt), 1);
  if(!res)
  {
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    rwlock->writer_depth = 1;
    snprintf(rwlock->name, sizeof(rwlock->name), "w:%s:%d", file, line);
  }
  return res;
}
#define dt_pthread_rwlock_tryrdlock(A) dt_pthread_rwlock_tryrdlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_tryrdlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  // IMPORTANT: try* locks are often used as "is it locked by anyone?" probes (e.g. caches).
  // Returning success when the current thread already holds the write lock breaks that contract
  // and can lead to freeing data still in use. Treat it as busy.
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1) return EBUSY;

  const int res = pthread_rwlock_tryrdlock(&rwlock->lock);
  assert(!res || (res == EBUSY));
  if(!res)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    snprintf(rwlock->name, sizeof(rwlock->name), "tr:%s:%d", file, line);
  }
  return res;
}
#define dt_pthread_rwlock_trywrlock(A) dt_pthread_rwlock_trywrlock_with_caller(A, __FILE__, __LINE__)
static inline int dt_pthread_rwlock_trywrlock_with_caller(dt_pthread_rwlock_t *rwlock, const char *file, int line)
{
  // See dt_pthread_rwlock_tryrdlock_with_caller(): don't make trywrlock succeed recursively.
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1) return EBUSY;

  const int res = pthread_rwlock_trywrlock(&rwlock->lock);
  assert(!res || (res == EBUSY));
  if(!res)
  {
    __sync_fetch_and_add(&(rwlock->cnt), 1);
    __sync_lock_test_and_set(&(rwlock->writer), pthread_self());
    rwlock->writer_depth = 1;
    snprintf(rwlock->name, sizeof(rwlock->name), "tw:%s:%d", file, line);
  }
  return res;
}

#undef TOPN
#else

typedef struct CAPABILITY("mutex") dt_pthread_mutex_t
{
  pthread_mutex_t mutex;
} CAPABILITY("mutex") dt_pthread_mutex_t;

// *please* do use these;
static inline int dt_pthread_mutex_init(dt_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr)
{
  return pthread_mutex_init(&mutex->mutex, mutexattr);
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

static inline int dt_pthread_cond_wait(pthread_cond_t *cond, dt_pthread_mutex_t *mutex)
{
  return pthread_cond_wait(cond, &mutex->mutex);
};

// Same-thread recursive write-lock tracking, mirroring the _DEBUG implementation above.
// A thread that already holds the write lock cannot race itself: no other thread can be
// touching the protected data while the write lock is held, so letting that same thread
// re-enter (as reader or writer) is safe for data validity. Without this, glibc's default
// PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP policy self-deadlocks such a thread as soon as
// a second thread is queued waiting for the write lock (new readers, including from the
// writer's own thread, are blocked once a writer is waiting).
typedef struct dt_pthread_rwlock_t
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
} dt_pthread_rwlock_t;

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

static inline int dt_pthread_rwlock_unlock(dt_pthread_rwlock_t *rwlock)
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
// header cycle, can include common/darktable.h and call dt_print(DT_DEBUG_HISTORY, ...) there).
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

static inline int dt_pthread_rwlock_rdlock(dt_pthread_rwlock_t *rwlock)
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

static inline int dt_pthread_rwlock_wrlock(dt_pthread_rwlock_t *rwlock)
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

static inline int dt_pthread_rwlock_tryrdlock(dt_pthread_rwlock_t *rwlock)
{
  // Keep try* locks honest as "is it locked by anyone?" probes: do NOT report success just
  // because the current thread already holds the write lock (see dt_pthread_rwlock_rdlock
  // above for the blocking-call recursion, which is the case this is not).
  if(pthread_equal(rwlock->writer, pthread_self()) && rwlock->writer_depth >= 1) return EBUSY;
  return pthread_rwlock_tryrdlock(&rwlock->lock);
}

static inline int dt_pthread_rwlock_trywrlock(dt_pthread_rwlock_t *rwlock)
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

#endif

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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
