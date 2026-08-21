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
#ifndef DT_SYSTEM_OPENMP_H
#define DT_SYSTEM_OPENMP_H

/* OpenMP wrappers: the pragma shorthands used across the pixel code, with
 * single-threaded fallbacks when OpenMP is disabled. Self-contained on purpose:
 * low-level compute units include this instead of darktable.h. */

#ifdef _OPENMP
# include <omp.h>

#ifndef dt_omp_nontemporal
// Clang 10+ supports the nontemporal() OpenMP directive
// GCC 9 recognizes it as valid, but does not do anything with it
// GCC 10+ ???
#if (__clang__+0 >= 10 || __GNUC__ >= 9)
#  define dt_omp_nontemporal(...) nontemporal(__VA_ARGS__)
#else
// GCC7/8 only support OpenMP 4.5, which does not have the nontemporal() directive.
#  define dt_omp_nontemporal(var, ...)
#endif
#endif /* dt_omp_nontemporal */

#define OMP_PRAGMA(x) _Pragma(#x)
#define __OMP_PARALLEL__(...) OMP_PRAGMA(omp parallel default(firstprivate) __VA_ARGS__)
#define __OMP_PARALLEL_FOR__(...) OMP_PRAGMA(omp parallel for default(firstprivate) schedule(static) __VA_ARGS__)
#define __OMP_PARALLEL_FOR_SIMD__(...) OMP_PRAGMA(omp parallel for simd default(firstprivate) schedule(simd:static) __VA_ARGS__)
#define __OMP_FOR_SIMD__(...) OMP_PRAGMA(omp for simd schedule(simd:static) __VA_ARGS__)
#define __OMP_FOR__(...) OMP_PRAGMA(omp for schedule(static) __VA_ARGS__)
#define __OMP_SIMD__(...) OMP_PRAGMA(omp simd __VA_ARGS__)
#define __OMP_DECLARE_SIMD__(...) OMP_PRAGMA(omp declare simd __VA_ARGS__)

// CLang 20 supports OpenMP 5.1 default(firstprivate) but only for C files.
// C++ files still need to use default(none) until further notice.
// Change that when baseline CLang is upgraded.
#define __OMP_PARALLEL_FOR_CPP__(...) OMP_PRAGMA(omp parallel for default(none) schedule(static) __VA_ARGS__)

// TRUE while the caller runs inside a parallel region. Diagnostics belong outside one: a message
// printed per thread floods the log and interleaves mid-line with the other threads' output.
#define dt_omp_in_parallel() (omp_in_parallel() != 0)

#else /* _OPENMP */

# define omp_get_max_threads() 1
# define omp_get_thread_num() 0
# define dt_omp_in_parallel() 0

#define __OMP_PARALLEL__(...)
#define __OMP_PARALLEL_FOR__(...)
#define __OMP_PARALLEL_FOR_SIMD__(...)
#define __OMP_FOR_SIMD__(...)
#define __OMP_FOR__(...)
#define __OMP_SIMD__(...)
#define __OMP_DECLARE_SIMD__(...)

#define __OMP_PARALLEL_FOR_CPP__(...)

#endif /* _OPENMP */

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Number of OpenMP threads the application decided to use. DECLARED here so
 * low-level compute code can size per-thread buffers without importing
 * darktable.h; BOUND by the orchestrator (darktable.c). */
int dt_get_num_openmp_threads(void);

static inline int dt_get_thread_num()
{
#ifdef _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

// after writing data using copy_pixel_nontemporal, it is necessary to
// ensure that the writes have completed before attempting reads from
// a different core.  This function produces the required memory
// fence to ensure proper visibility
static inline void dt_sfence()
{
#if defined(__x86_64__) || defined(__i386__)
  _mm_sfence();
#else
  // the following generates an MFENCE instruction on x86/x64.  We
  // only really need SFENCE, which is less expensive, but none of the
  // other memory orders generate *any* fence instructions on x64.
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// if the copy_pixel_nontemporal() writes were inside an OpenMP
// parallel loop, the OpenMP parallelization will have performed a
// memory fence before resuming single-threaded operation, so a
// dt_sfence would be superfluous.  But if compiled without OpenMP
// parallelization, we should play it safe and emit a memory fence.
// This function should be used right after a parallelized for loop,
// where it will produce a barrier only if needed.
#ifdef _OPENMP
#define dt_omploop_sfence()
#else
#define dt_omploop_sfence() dt_sfence()
#endif

#ifdef __cplusplus
}
#endif

#endif // DT_SYSTEM_OPENMP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
