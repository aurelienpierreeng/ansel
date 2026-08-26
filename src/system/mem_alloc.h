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

#include "system/macros.h"

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

/** @brief Promise the compiler that @p x is already cacheline-aligned, and return it.
 *
 * @warning This ASSERTS, it does not align: the compiler is free to emit aligned loads
 * and stores on the strength of the promise. Applying it to a pointer that is not in
 * fact aligned is undefined behaviour, and typically shows up as a SIGSEGV in vectorised
 * code far from the allocation. Only use it on memory from dt_alloc_align() and friends,
 * or on a stack object declared DT_ALIGNED_ARRAY. Use dt_check_sse_aligned() when the
 * alignment is not known statically.
 */
#define DT_IS_ALIGNED(x) __builtin_assume_aligned(x, DT_CACHELINE_BYTES)

/* Size of a CPU cacheline, in bytes, floats and 4-float pixels.
 *
 * These are COMPILE-TIME constants chosen per target, not the running machine's real
 * cacheline: a binary built for x86-64 and run under emulation on Apple Silicon keeps 64.
 * They set the alignment of every dt_alloc_align() buffer and of DT_ALIGNED_ARRAY, so a
 * value smaller than the hardware's costs performance, and one larger costs memory --
 * neither is a correctness problem, which is why the mismatch is easy to miss.
 */
#if defined(__APPLE__) && defined(__aarch64__)
  #define DT_CACHELINE_BYTES 128
  #define DT_CACHELINE_FLOATS 32
  #define DT_CACHELINE_PIXELS 8
#else
  #define DT_CACHELINE_BYTES 64
  #define DT_CACHELINE_FLOATS 16
  #define DT_CACHELINE_PIXELS 4
#endif /* __APPLE__ && __aarch64__ */

/** @brief Align an object on a cacheline boundary, so AVX2 can load it whole.
 *
 * @warning On a STRUCT MEMBER this attribute constrains the member's offset within the
 * struct -- it cannot constrain where the struct itself lands. A struct carrying one of
 * these must therefore be allocated by dt_alloc_align() (or live on the stack, where the
 * compiler honours it); plain malloc()/g_malloc() guarantee far less alignment, and the
 * member then straddles a cacheline. The result is a crash or silently wrong pixels in
 * vectorised code, at the point of USE, with nothing wrong at the point of allocation.
 */
#define DT_ALIGNED_ARRAY __attribute__((aligned(DT_CACHELINE_BYTES)))

/** @brief Align a 4-float pixel on 16 bytes, enough for SSE. Same struct-member caveat as
 * DT_ALIGNED_ARRAY, with a weaker requirement that malloc() happens to meet on most
 * platforms -- which is exactly why misuse survives testing here and not there. */
#define DT_ALIGNED_PIXEL __attribute__((aligned(16)))

/** @brief Is @p pointer aligned on a @p byte_count boundary? Pure test, no side effect. */
static inline gboolean dt_is_aligned(const void *pointer, size_t byte_count)
{
    return (uintptr_t)pointer % byte_count == 0;
}

/** @brief Round @p size UP to the next multiple of @p alignment.
 *
 * @param size byte count to round. Zero rounds to zero.
 * @param alignment must be non-zero; zero divides by zero.
 * @return the rounded size, always >= @p size.
 */
static inline size_t dt_round_size(const size_t size, const size_t alignment)
{
  // Round the size of a buffer to the closest higher multiple
  return ((size % alignment) == 0) ? size : ((size - 1) / alignment + 1) * alignment;
}

/** @brief Round @p size up to the next multiple of 64.
 *
 * @note Hardcoded 64, NOT DT_CACHELINE_BYTES: on Apple Silicon a cacheline is 128, so this
 * rounds to half a cacheline there. It is named for the SSE register width it was written
 * for; do not substitute it for dt_round_size(size, DT_CACHELINE_BYTES).
 */
static inline size_t dt_round_size_sse(const size_t size)
{
  // Round the size of a buffer to the closest 64 higher multiple
  return dt_round_size(size, 64);
}

/** @brief Platform back-end for dt_alloc_align(). Call dt_alloc_align() instead.
 *
 * @return cacheline-aligned memory of at least @p size bytes (rounded up), or NULL if the
 * allocation failed. Must be released with dt_free_align().
 */
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

/** @brief Allocate cacheline-aligned memory.
 *
 * @details The single entry point for buffers that vectorised code will touch. The
 * returned block is aligned on DT_CACHELINE_BYTES and its usable size is @p size rounded
 * UP to that boundary, so writing the rounded size is safe and reading it is defined.
 *
 * @param size requested byte count.
 *
 * @return the block, or **NULL when the allocation fails** -- which happens in practice on
 * the pixel path, where buffers are hundreds of megabytes. Callers must check.
 *
 * @warning Release ONLY with dt_free_align(). On Windows this comes from _aligned_malloc()
 * and passing it to free()/g_free()/dt_free() corrupts the heap; on POSIX the two paths
 * happen to coincide, which is precisely why such a mismatch passes every Linux test and
 * fails on Windows only.
 *
 * @see dt_calloc_align() for the zeroed variant, dt_alloc_align_float() for float counts.
 */
void *dt_alloc_align(size_t size);

/** @brief g_free() @p ptr and set it to NULL, skipping both if it is already NULL.
 *
 * @param ptr an **lvalue** holding a pointer. It is assigned NULL, so a temporary, a cast
 * expression or a function result does not compile -- deliberately: the whole point is
 * that no caller is left holding a dangling copy.
 *
 * @warning Pairs with g_malloc()/g_strdup() and the rest of the GLib family, NOT with
 * dt_alloc_align(). Use dt_free_align() for that.
 *
 * @warning Expands to a bare `if` with no `do { } while(0)` wrapper, so
 * `if(c) dt_free(p); else ...` fails to compile with "else without a previous if". Brace
 * the branch. It is a compile error rather than a silent misbinding, so it cannot reach
 * runtime -- but it does force braces where none should be needed.
 */
#define dt_free(ptr)           \
  if(!IS_NULL_PTR(ptr))        \
  {                            \
    g_free((void *)(ptr));     \
    *(void **)(&(ptr)) = NULL; \
  }

/** @brief g_free() one pointer, with the signature GDestroyNotify wants.
 *
 * @param ptr taken BY VALUE, so unlike the dt_free() macro this cannot NULL the caller's
 * variable -- the assignment in the body only clears the local copy and is dead. Use it
 * where a GLib container needs a free function, not as a general-purpose free.
 */
static inline void dt_free_gpointer(gpointer ptr)
{
  g_free(ptr);
  ptr = NULL;
}

/** @brief Platform back-end for dt_free_align(). Call dt_free_align() instead: this one
 * takes the pointer by value and so cannot NULL the caller's variable. */
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

/** @brief Release memory from dt_alloc_align() and set @p ptr to NULL.
 *
 * @param ptr an **lvalue**, for the same reason as dt_free(). NULL is accepted and ignored.
 *
 * @warning Must be used for -- and only for -- dt_alloc_align(), dt_calloc_align(),
 * dt_alloc_align_float() and dt_calloc_align_float(). See dt_alloc_align() for what
 * crossing the two families costs on Windows.
 *
 * @warning Same bare-`if` expansion as dt_free(): brace the branch in an if/else.
 */
#define dt_free_align(ptr)            \
  if(!IS_NULL_PTR(ptr))               \
  {                                   \
    dt_free_align_ptr((void *)(ptr)); \
    *(void **)(&(ptr)) = NULL;        \
  }

/** @brief dt_alloc_align() followed by a zero fill.
 * @return the zeroed block, or NULL on failure. Release with dt_free_align().
 * @note Only the @p size bytes asked for are zeroed, not the padding dt_alloc_align()
 * rounds the block up to. */
static inline void* dt_calloc_align(size_t size)
{
  void *buf = dt_alloc_align(size);
  if(buf) memset(buf, 0, size);
  return buf;
}
/** @brief Allocate @p pixels floats, cacheline-aligned and marked as such.
 * @return the block, or NULL on failure. Release with dt_free_align().
 * @warning @p pixels is a COUNT OF FLOATS, not a byte count, and the multiplication is not
 * checked for overflow -- validate suspicious dimensions before calling. */
static inline float *dt_alloc_align_float(size_t pixels)
{
  return (float*)__builtin_assume_aligned(dt_alloc_align(pixels * sizeof(float)), DT_CACHELINE_BYTES);
}
/** @brief dt_alloc_align_float() followed by a zero fill.
 * @return the zeroed block, or NULL on failure. Release with dt_free_align(). */
static inline float *dt_calloc_align_float(size_t pixels)
{
  float *const buf = (float*)dt_alloc_align(pixels * sizeof(float));
  if(buf) memset(buf, 0, pixels * sizeof(float));
  return (float*)__builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
}
/** @brief Runtime-checked counterpart to DT_IS_ALIGNED().
 * @return @p pointer marked aligned for the compiler when it really is cacheline-aligned,
 * and **NULL when it is not** -- the return value is the test result, so ignoring it and
 * using @p pointer anyway defeats the purpose. Does not allocate; ownership is unchanged. */
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
 * @param buffer destination, at least @p size bytes. Not NULL-checked.
 * @param size number of bytes to clear. The body writes one unsigned char at a time, so
 * any size is handled; the "multiple of 8" this once required is no longer a constraint.
 *
 * @note The intent is a zero fill a compiler may not elide, as memset_s() guarantees in
 * C11. A plain byte loop is not that guarantee -- an optimiser is allowed to recognise it
 * and call memset() anyway -- so do not rely on this to scrub secrets.
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
