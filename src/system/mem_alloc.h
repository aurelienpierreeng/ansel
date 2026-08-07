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
#ifndef DT_SYSTEM_MEM_ALLOC_H
#define DT_SYSTEM_MEM_ALLOC_H

/* Cacheline-aligned memory: alignment constants/attributes and the dt_alloc/dt_free
 * family. Self-contained on purpose: low-level compute units include this instead of
 * darktable.h. The pixelpipe-cache-tracked allocators
 * (dt_pixelpipe_cache_alloc_*) are NOT here — they reference the darktable global and
 * stay with the orchestrator. */

#include "common/macros.h"

#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Helper to force stack vectors to be aligned on DT_CACHELINE_BYTES blocks to enable AVX2 */
#define DT_IS_ALIGNED(x) __builtin_assume_aligned(x, DT_CACHELINE_BYTES)

// Configure the size of a CPU cacheline in bytes, floats, and pixels.  On most current architectures,
// a cacheline contains 64 bytes, but Apple Silicon (M-series processors) uses 128-byte cache lines.
#if defined(__APPLE__) && defined(__aarch64__)
  #define DT_CACHELINE_BYTES 128
  #define DT_CACHELINE_FLOATS 32
  #define DT_CACHELINE_PIXELS 8
#else
  #define DT_CACHELINE_BYTES 64
  #define DT_CACHELINE_FLOATS 16
  #define DT_CACHELINE_PIXELS 4
#endif /* __APPLE__ && __aarch64__ */

// Helper to force heap vectors to be aligned on 64 byte blocks to enable AVX2
// If this is applied to a struct member and the struct is allocated on the heap, then it must be allocated
// on a 64 byte boundary to avoid crashes or undefined behavior because of unaligned memory access.
#define DT_ALIGNED_ARRAY __attribute__((aligned(DT_CACHELINE_BYTES)))
#define DT_ALIGNED_PIXEL __attribute__((aligned(16)))

static inline gboolean dt_is_aligned(const void *pointer, size_t byte_count)
{
    return (uintptr_t)pointer % byte_count == 0;
}

static inline size_t dt_round_size(const size_t size, const size_t alignment)
{
  // Round the size of a buffer to the closest higher multiple
  return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
}

static inline size_t dt_round_size_sse(const size_t size)
{
  // Round the size of a buffer to the closest 64 higher multiple
  return dt_round_size(size, 64);
}

static inline void *dt_alloc_align_internal(size_t size)
{
  const size_t alignment = DT_CACHELINE_BYTES;
  const size_t aligned_size = dt_round_size(size, alignment);
#if defined(__FreeBSD_version) && __FreeBSD_version < 700013
  return malloc(aligned_size);
#elif defined(_WIN32)
  return _aligned_malloc(aligned_size, alignment);
#else
  void *ptr = NULL;
  if(posix_memalign(&ptr, alignment, aligned_size)) return NULL;
  return ptr;
#endif
}

void *dt_alloc_align(size_t size);

#define dt_free(ptr)           \
  if(!IS_NULL_PTR(ptr))        \
  {                            \
    g_free((void *)(ptr));     \
    *(void **)(&(ptr)) = NULL; \
  }

static inline void dt_free_gpointer(gpointer ptr)
{
  g_free(ptr);
  ptr = NULL;
}

#ifdef _WIN32
  static inline void dt_free_align_ptr(void *mem)
  {
    _aligned_free(mem);
  }
#else
  static inline void dt_free_align_ptr(void *mem)
  {
    dt_free(mem);
  }
#endif

#define dt_free_align(ptr)            \
  if(!IS_NULL_PTR(ptr))               \
  {                                   \
    dt_free_align_ptr((void *)(ptr)); \
    *(void **)(&(ptr)) = NULL;        \
  }

static inline void* dt_calloc_align(size_t size)
{
  void *buf = dt_alloc_align(size);
  if(buf) memset(buf, 0, size);
  return buf;
}
static inline float *dt_alloc_align_float(size_t pixels)
{
  return (float*)__builtin_assume_aligned(dt_alloc_align(pixels * sizeof(float)), DT_CACHELINE_BYTES);
}
static inline float *dt_calloc_align_float(size_t pixels)
{
  float *const buf = (float*)dt_alloc_align(pixels * sizeof(float));
  if(buf) memset(buf, 0, pixels * sizeof(float));
  return (float*)__builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
}
static inline void * dt_check_sse_aligned(void * pointer)
{
  if(dt_is_aligned(pointer, DT_CACHELINE_BYTES))
    return __builtin_assume_aligned(pointer, DT_CACHELINE_BYTES);
  else
    return NULL;
}

/**
 * @brief Set the memory buffer to zero as a pack of unsigned char
 *
 * @param buffer void buffer
 * @param size size of the memory stride. NEEDS TO BE A MULTIPLE OF 8.
 */
static inline void memset_zero(void *const buffer, size_t size)
{
  // Same as memset_s in C11. memset might be optimized away by compilers, this will not.
  // Not parallelized or vectorized since it's applied only on "small" tiles.
  for(size_t k = 0; k < size / sizeof(unsigned char); k++) {
    unsigned char *const item = (unsigned char *const)buffer + k;
    *item = 0;
  }
}

#ifdef __cplusplus
}
#endif

#endif // DT_SYSTEM_MEM_ALLOC_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
