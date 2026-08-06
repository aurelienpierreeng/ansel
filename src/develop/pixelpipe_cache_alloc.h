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
#ifndef DT_DEVELOP_PIXELPIPE_CACHE_ALLOC_H
#define DT_DEVELOP_PIXELPIPE_CACHE_ALLOC_H

/* Convenience allocators over the pixelpipe cache, bound to the application-wide
 * cache singleton (and, for the perthread variants, to the application's OpenMP
 * thread count).
 *
 * They moved here from common/darktable.h: they are pipeline-cache API glue, not
 * orchestrator material. The binding goes through the dt_pixelpipe_cache_get_global()
 * and dt_get_num_openmp_threads() accessors — declared by the libs, implemented by
 * the orchestrator (common/darktable.c) — so including this helper does NOT drag
 * common/darktable.h (and with it the whole application) into the translation unit. */

#include "common/macros.h"
#include "common/mem_alloc.h"
#include "common/openmp.h"
#include "develop/pixelpipe_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

#define dt_pixelpipe_cache_alloc_align_cache(size, id) \
  dt_pixelpipe_cache_alloc_align_cache_impl(dt_pixelpipe_cache_get_global(), (size), (id), __FILE__ ":" DT_STRINGIFY(__LINE__))

#ifndef dt_pixelpipe_cache_alloc_align
#define dt_pixelpipe_cache_alloc_align(size, pipe) \
  dt_pixelpipe_cache_alloc_align_cache((size), (pipe)->type)
#endif

#ifndef dt_pixelpipe_cache_alloc_align_float
#define dt_pixelpipe_cache_alloc_align_float(pixels, pipe) \
  ((float *)dt_pixelpipe_cache_alloc_align((size_t)(pixels) * sizeof(float), (pipe)))
#endif

#ifndef dt_pixelpipe_cache_alloc_align_float_cache
#define dt_pixelpipe_cache_alloc_align_float_cache(pixels, id) \
  ((float *)dt_pixelpipe_cache_alloc_align_cache((size_t)(pixels) * sizeof(float), (id)))
#endif

#ifndef dt_pixelpipe_cache_alloc_align_int
#define dt_pixelpipe_cache_alloc_align_int(count, pipe) \
  ((int *)dt_pixelpipe_cache_alloc_align((size_t)(count) * sizeof(int), (pipe)))
#endif

#ifndef dt_pixelpipe_cache_alloc_align_double
#define dt_pixelpipe_cache_alloc_align_double(count, pipe) \
  ((double *)dt_pixelpipe_cache_alloc_align((size_t)(count) * sizeof(double), (pipe)))
#endif

#define dt_pixelpipe_cache_free_align(mem) \
  dt_pixelpipe_cache_free_align_cache(dt_pixelpipe_cache_get_global(), (void **)&(mem), __FILE__ ":" DT_STRINGIFY(__LINE__));

// Allocate a buffer for 'n' objects each of size 'objsize' bytes for each of the program's threads.
// Ensures that there is no false sharing among threads by aligning and rounding up the allocation to
// a multiple of the cache line size.  Returns a pointer to the allocated pool and the adjusted number
// of objects in each thread's buffer.  Use dt_get_perthread or dt_get_bythread (see below) to access
// a specific thread's buffer.
static inline void *dt_pixelpipe_cache_alloc_perthread_impl(const size_t n, const size_t objsize, size_t* padded_size, const char *message)
{
  const size_t alloc_size = n * objsize;
  const size_t cache_lines = (alloc_size + DT_CACHELINE_BYTES - 1) / DT_CACHELINE_BYTES;
  *padded_size = DT_CACHELINE_BYTES * cache_lines / objsize;
  const size_t total_bytes = DT_CACHELINE_BYTES * cache_lines * dt_get_num_openmp_threads();
  void *buf = dt_pixelpipe_cache_alloc_align_cache_impl(dt_pixelpipe_cache_get_global(), total_bytes, 0, message);
  if(IS_NULL_PTR(buf)) return NULL;
  return __builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
}

#ifndef dt_pixelpipe_cache_alloc_perthread
#define dt_pixelpipe_cache_alloc_perthread(n, objsize, padded_size) \
  ((void *)dt_pixelpipe_cache_alloc_perthread_impl((n), (objsize), (padded_size), __FILE__ ":" DT_STRINGIFY(__LINE__)))
#endif

static inline void *dt_pixelpipe_cache_calloc_perthread_impl(const size_t n, const size_t objsize, size_t* padded_size, const char *message)
{
  void *const buf = (float*)dt_pixelpipe_cache_alloc_perthread_impl(n, objsize, padded_size, message);
  if(IS_NULL_PTR(buf)) return NULL;
  memset(buf, 0, *padded_size * dt_get_num_openmp_threads() * objsize);
  return buf;
}

#ifndef dt_pixelpipe_cache_calloc_perthread
#define dt_pixelpipe_cache_calloc_perthread(n, objsize, padded_size) \
  ((void *)dt_pixelpipe_cache_calloc_perthread_impl((n), (objsize), (padded_size), __FILE__ ":" DT_STRINGIFY(__LINE__)))
#endif

// Same as dt_pixelpipe_cache_alloc_perthread, but the object is a float.
static inline float *dt_pixelpipe_cache_alloc_perthread_float_impl(const size_t n, size_t* padded_size, const char *message)
{
  return (float*)dt_pixelpipe_cache_alloc_perthread_impl(n, sizeof(float), padded_size, message);
}

#ifndef dt_pixelpipe_cache_alloc_perthread_float
#define dt_pixelpipe_cache_alloc_perthread_float(n, padded_size) \
  ((float *)dt_pixelpipe_cache_alloc_perthread_float_impl((n), (padded_size), __FILE__ ":" DT_STRINGIFY(__LINE__)))
#endif

// Given the buffer and object count returned by dt_pixelpipe_cache_alloc_perthread, return the current thread's private buffer.
#define dt_get_perthread(buf, padsize) DT_IS_ALIGNED((buf) + ((padsize) * dt_get_thread_num()))
// Given the buffer and object count returned by dt_pixelpipe_cache_alloc_perthread and a thread count in 0..dt_get_num_openmp_threads()-1,
// return a pointer to the indicated thread's private buffer.
#define dt_get_bythread(buf, padsize, tnum) DT_IS_ALIGNED((buf) + ((padsize) * (tnum)))

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_PIXELPIPE_CACHE_ALLOC_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
