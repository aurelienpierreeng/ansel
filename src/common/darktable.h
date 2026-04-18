/*
    This file is part of darktable,
    Copyright (C) 2009-2012 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010, 2012 Pascal de Bruijn.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010-2020 Tobias Ellinghaus.
    Copyright (C) 2011, 2014-2015 Bruce Guenter.
    Copyright (C) 2011-2013, 2017 Ulrich Pegelow.
    Copyright (C) 2012 Ammon Riley.
    Copyright (C) 2012 Christian Himpel.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 Jean-Sébastien Pédron.
    Copyright (C) 2012-2014 Jérémy Rosen.
    Copyright (C) 2012 Moritz Lipp.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Simon Spannagel.
    Copyright (C) 2013, 2021 Aldric Renaudin.
    Copyright (C) 2013, 2015, 2019-2021 Pascal Obry.
    Copyright (C) 2013-2017 Roman Lebedev.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2015 Matthias Gehre.
    Copyright (C) 2016-2019 Peter Budai.
    Copyright (C) 2016 Stuart Henderson.
    Copyright (C) 2018-2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 parafin.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020 Andreas Schneider.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020-2021 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hubert Figuière.
    Copyright (C) 2021 Paolo DePetrillo.
    Copyright (C) 2021 Robert Bridge.
    Copyright (C) 2021 Roman Khatko.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philippe Weyland.
    Copyright (C) 2023-2025 Alynx Zhou.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    
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


// just to be sure. the build system should set this for us already:
#if defined __DragonFly__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__
#define _WITH_DPRINTF
#define _WITH_GETLINE
#elif !defined _XOPEN_SOURCE && !defined _WIN32
#define _XOPEN_SOURCE 700 // for localtime_r and dprintf
#endif

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

#if !defined(O_BINARY)
// To have portable g_open() on *nix and on Windows
#define O_BINARY 0
#endif

#include "external/ThreadSafetyAnalysis.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/database.h"
#include "common/fp_mode.h"
#include "common/dtpthread.h"
#include "common/utility.h"
#ifdef _WIN32
#include "win/getrusage.h"
#else
#include <sys/resource.h>
#endif
#include <stdint.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include <json-glib/json-glib.h>
#include <lua/lua.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef _RELEASE
#include "common/poison.h"
#endif

#include "common/usermanual_url.h"

// for signal debugging symbols
#include "control/signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DT_MODULE_VERSION 23 // version of dt's module interface

// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 11
#define DT_PERF_INFOSIZE 4096

// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return -DT_MODULE_VERSION;                                                                               \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#else
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return DT_MODULE_VERSION;                                                                                \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#endif

#define DT_MODULE_INTROSPECTION(MODVER, PARAMSTYPE) DT_MODULE(MODVER)

// ..to be able to compare it against this:
static inline int dt_version()
{
#ifdef _DEBUG
  return -DT_MODULE_VERSION;
#else
  return DT_MODULE_VERSION;
#endif
}

// returns the darktable version as <major>.<minor>
char *dt_version_major_minor();

#undef STR_HELPER
#define STR_HELPER(x) #x

#undef STR
#define STR(x) STR_HELPER(x)

#define DT_IMAGE_DBLOCKS 64

// When included by a C++ file, restrict qualifiers are not allowed
#ifdef __cplusplus
#define DT_RESTRICT
#else
#define DT_RESTRICT restrict
#endif

// Default code for imgid meaning the picture is unknown or invalid
#define UNKNOWN_IMAGE -1

#ifdef __cplusplus
}
#endif

/********************************* */

/**
 * We need to include all OS-centric & arch-centric libs here, because
 * they typically contain low-level info useful to plan for SIMD memory use
 * (mem alloc, mem alignment, SSE level, CPU features, etc.).
 */

#if defined _WIN32
#include "win/win.h"
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__)
typedef unsigned int u_int;
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h> // needed for _mm_stream_ps
#endif

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
#define __OMP_PARALLEL_FOR_CPP__(...) \
  OMP_PRAGMA(omp parallel __VA_ARGS__) \
  {  \
    dt_fp_init(DT_FP_MODE_FAST); \
    OMP_PRAGMA(for default(none) schedule(static))

#define __OMP_PARALLEL_FOR_CPP_END__ \
  }


#else /* _OPENMP */

# define omp_get_max_threads() 1
# define omp_get_thread_num() 0

#define __OMP_PARALLEL__(...) 
#define __OMP_PARALLEL_FOR__(...)
#define __OMP_PARALLEL_FOR_SIMD__(...)
#define __OMP_FOR_SIMD__(...)
#define __OMP_FOR__(...)
#define __OMP_SIMD__(...)
#define __OMP_DECLARE_SIMD__(...)

#define __OMP_PARALLEL_FOR_CPP__(...)
#define __OMP_PARALLEL_FOR_CPP_END__

#endif /* _OPENMP */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C is way too permissive with `!=`, `==` and `if(var)` checks, which can mean 
 * too many things depending on what we compare. We force here a semantic NULL check
 * for pointers types that will fail for anything else than pointers,
 * and make the code more explicit about what is checked.
 * This will fail at compile time on function pointers and anything that is not a pointer.
 * 
 */
#define IS_NULL_PTR(p)                                            \
  ({                                                              \
    __typeof__(p) _tmp = (p);                                     \
    (void)sizeof(char[                                            \
      (__builtin_classify_type(_tmp) == 5) ? 1 : -1               \
    ]);                                                           \
    _tmp == NULL;                                                 \
  })


static inline int dt_get_thread_num()
{
#ifdef _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

/* Create cloned functions for various CPU SSE generations */
/* See for instructions https://hannes.hauswedell.net/post/2017/12/09/fmv/ */
/* TL;DR : use only on SIMD functions containing low-level paralellized/vectorized loops */
#if __has_attribute(target_clones) && !defined(_WIN32) && !defined(NATIVE_ARCH) && !defined(_DEBUG)

  /*
   * Apple note:
   * - arm64 vs x86_64 is handled by the universal binary, not target_clones.
   * - target_clones on Apple is only useful within one slice.
   * - the x86-64-v2/v3/v4 strings are not accepted by all Apple Clang versions.
   *
   * Therefore:
   * - on Apple arm64: disable clones here,
   * - on Apple x86_64: require explicit opt-in once the toolchain is validated.
   */

  #if defined(__APPLE__)

    #if defined(__aarch64__) || defined(__arm64__)
      #define __DT_CLONE_TARGETS__

    #elif defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)

      /*
       * Enable this from your build system only after verifying that the local
       * Apple Clang accepts:
       *   target_clones("default","arch=x86-64","arch=x86-64-v2","arch=x86-64-v3","arch=x86-64-v4")
       *
       * Example:
       *   -DDT_APPLE_X86_TARGET_CLONES=1
       */
      #if defined(DT_APPLE_X86_TARGET_CLONES)
        #define __DT_CLONE_TARGETS__ \
          __attribute__((target_clones( \
            "default", \
            "arch=x86-64", \
            "arch=x86-64-v2", \
            "arch=x86-64-v3", \
            "arch=x86-64-v4" \
          )))
      #else
        #define __DT_CLONE_TARGETS__
      #endif

    #else
      #define __DT_CLONE_TARGETS__
    #endif

  #elif defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
    #define __DT_CLONE_TARGETS__ \
      __attribute__((target_clones( \
        "default", \
        "arch=x86-64", \
        "arch=x86-64-v2", \
        "arch=x86-64-v3", \
        "arch=x86-64-v4" \
      )))

  #elif defined(__PPC64__)
    /* __PPC64__ is the only macro tested for in is_supported_platform.h, other macros would fail there anyway. */
    #define __DT_CLONE_TARGETS__ __attribute__((target_clones("default","cpu=power9")))

  #else
    #define __DT_CLONE_TARGETS__
  #endif

#else
  #define __DT_CLONE_TARGETS__
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

struct dt_dev_pixelpipe_cache_t;

#define DT_STRINGIFY_HELPER(x) #x
#define DT_STRINGIFY(x) DT_STRINGIFY_HELPER(x)

void *dt_pixelpipe_cache_alloc_align_cache_impl(struct dt_dev_pixelpipe_cache_t *cache, size_t size, int id,
                                                const char *name);
#define dt_pixelpipe_cache_alloc_align_cache(size, id) \
  dt_pixelpipe_cache_alloc_align_cache_impl(darktable.pixelpipe_cache, (size), (id), __FILE__ ":" DT_STRINGIFY(__LINE__))

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

void dt_pixelpipe_cache_free_align_cache(struct dt_dev_pixelpipe_cache_t *cache, void **mem, const char *message);

#define dt_pixelpipe_cache_free_align(mem) \
  dt_pixelpipe_cache_free_align_cache(darktable.pixelpipe_cache, (void **)&(mem), __FILE__ ":" DT_STRINGIFY(__LINE__));

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

// Most code in dt assumes that the compiler is capable of auto-vectorization.  In some cases, this will yield
// suboptimal code if the compiler in fact does NOT auto-vectorize.  Uncomment the following line for such a
// compiler.
//#define DT_NO_VECTORIZATION

// For some combinations of compiler and architecture, the compiler may actually emit inferior code if given
// a hint to vectorize a loop.  Uncomment the following line if such a combination is the compilation target.
//#define DT_NO_SIMD_HINTS

// utility type to ease declaration of aligned small arrays to hold a pixel (and document their purpose)
typedef DT_ALIGNED_PIXEL float dt_aligned_pixel_t[4];
// SIMD view matching dt_aligned_pixel_t layout, for explicit 4-float vector math.
typedef float dt_aligned_pixel_simd_t __attribute__((vector_size(16), aligned(16)));

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t dt_simd_set1(const float value)
{
  return (dt_aligned_pixel_simd_t){ value, value, value, value };
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_abs(const dt_aligned_pixel_simd_t value)
{
  dt_aligned_pixel_simd_t out = value;
  for(int c = 0; c < 4; c++)
    out[c] = fabsf(value[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_max_zero(const dt_aligned_pixel_simd_t value)
{
  dt_aligned_pixel_simd_t out = value;
  for(int c = 0; c < 4; c++)
    out[c] = (isfinite(value[c])) ? MAX(value[c], 0.0f) : 0.f;
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_copysign(const dt_aligned_pixel_simd_t magnitude, const dt_aligned_pixel_simd_t sign)
{
  dt_aligned_pixel_simd_t out = magnitude;
  for(int c = 0; c < 4; c++)
    out[c] = copysignf(magnitude[c], sign[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_pow(const dt_aligned_pixel_simd_t base, const dt_aligned_pixel_simd_t exponent)
{
  dt_aligned_pixel_simd_t out = base;
  for(int c = 0; c < 4; c++)
    out[c] = powf(base[c], exponent[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_load_simd(const float *const pixel)
{
  dt_aligned_pixel_simd_t out;
  __builtin_memcpy(&out, pixel, sizeof(out));
  return out;
}

static inline __attribute__((always_inline)) void
dt_store_simd(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  __builtin_memcpy(pixel, &value, sizeof(value));
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_load_simd_aligned(const float *const pixel)
{
  const float *const in = (const float *const)__builtin_assume_aligned(pixel, 16);
  return dt_load_simd(in);
}

static inline __attribute__((always_inline)) void
dt_store_simd_aligned(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  float *const out = (float *const)__builtin_assume_aligned(pixel, 16);
  dt_store_simd(out, value);
}

static inline __attribute__((always_inline)) void
dt_store_simd_nontemporal(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  float *const out = (float *const)__builtin_assume_aligned(pixel, 16);

#if defined(__x86_64__) || defined(__i386__)
  const union
  {
    dt_aligned_pixel_simd_t simd;
    __m128 sse;
  } cast = { .simd = value };
  _mm_stream_ps(out, cast.sse);
#elif defined(__aarch64__)
  const union
  {
    dt_aligned_pixel_simd_t simd;
    float32x4_t neon;
  } cast = { .simd = value };
  vst1q_f32(out, cast.neon);
#elif (__clang__+0 > 7) && (__clang__+0 < 10)
  for_each_channel(k,aligned(out:16)) __builtin_nontemporal_store(value[k], out[k]);
#else
  for_each_channel(k,aligned(out:16) dt_omp_nontemporal(out)) out[k] = value[k];
#endif
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_mat3x4_mul_vec4(const dt_aligned_pixel_simd_t in, const dt_aligned_pixel_simd_t row0,
                   const dt_aligned_pixel_simd_t row1, const dt_aligned_pixel_simd_t row2)
{
  // Keep the multiply first in each accumulation step so GCC contracts this
  // into chained FMA instructions in the multiversioned FMA clones too.
  dt_aligned_pixel_simd_t out = row0 * in[0];
  out = row1 * in[1] + out;
  return row2 * in[2] + out;
}

// To be able to vectorize per-pixel loops, we need to operate on all four channels, but if the compiler does
// not auto-vectorize, doing so increases computation by 1/3 for a channel which typically is ignored anyway.
// Select the appropriate number of channels over which to loop to produce the fastest code.
#ifdef DT_NO_VECTORIZATION
#define DT_PIXEL_SIMD_CHANNELS 3
#else
#define DT_PIXEL_SIMD_CHANNELS 4
#endif

// A macro which gives us a configurable shorthand to produce the optimal performance when processing all of the
// channels in a pixel.  Its first argument is the name of the variable to be used inside the 'for' loop it creates,
// while the optional second argument is a set of OpenMP directives, typically specifying variable alignment.
// If indexing off of the begining of any buffer allocated with dt's image or aligned allocation functions, the
// alignment to specify is 64; otherwise, use 16, as there may have been an odd number of pixels from the start.
// Sample usage:
//         for_each_channel(k,aligned(src,dest:16))
//         {
//           src[k] = dest[k] / 3.0f;
//         }
#if defined(_OPENMP) && defined(OPENMP_SIMD_) && !defined(DT_NO_SIMD_HINTS)
//https://stackoverflow.com/questions/45762357/how-to-concatenate-strings-in-the-arguments-of-pragma
#define _DT_Pragma_(x) _Pragma(#x)
#define _DT_Pragma(x) _DT_Pragma_(x)
#define for_each_channel(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < 4; _var++)
#else
#define for_each_channel(_var, ...) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  for (size_t _var = 0; _var < 4; _var++)
#endif


// copy the RGB channels of a pixel using nontemporal stores if
// possible; includes the 'alpha' channel as well if faster due to
// vectorization, but subsequent code should ignore the value of the
// alpha unless explicitly set afterwards (since it might not have
// been copied).  NOTE: nontemporal stores will actually be *slower*
// if we immediately access the pixel again.  This function should
// only be used when processing an entire image before doing anything
// else with the destination buffer.
static inline void copy_pixel_nontemporal(
	float *const __restrict__ out,
        const float *const __restrict__ in)
{
  dt_store_simd_nontemporal(out, dt_load_simd(in));
}


// copy the RGB channels of a pixel; includes the 'alpha' channel as well if faster due to vectorization, but
// subsequent code should ignore the value of the alpha unless explicitly set afterwards (since it might not have
// been copied)
static inline void copy_pixel(float *const __restrict__ out, const float *const __restrict__ in)
{
  for_each_channel(k,aligned(in,out:16)) out[k] = in[k];
}

/********************************* */

struct dt_gui_gtk_t;
struct dt_control_t;
struct dt_develop_t;
struct dt_mipmap_cache_t;
struct dt_image_cache_t;
struct dt_lib_t;
struct dt_conf_t;
struct dt_points_t;
struct dt_imageio_t;
struct dt_bauhaus_t;
struct dt_undo_t;
struct dt_colorspaces_t;
struct dt_l10n_t;

typedef float dt_boundingbox_t[4];  //(x,y) of upperleft, then (x,y) of lowerright

typedef enum dt_debug_thread_t
{
  DT_DEBUG_ALWAYS        = 0,       // always print regardless of debug flags
  // powers of two, masking
  DT_DEBUG_CACHE          = 1 <<  0,
  DT_DEBUG_CONTROL        = 1 <<  1,
  DT_DEBUG_DEV            = 1 <<  2,
  DT_DEBUG_PERF           = 1 <<  4,
  DT_DEBUG_PIPECACHE         = 1 <<  5,
  DT_DEBUG_PWSTORAGE      = 1 <<  6,
  DT_DEBUG_OPENCL         = 1 <<  7,
  DT_DEBUG_SQL            = 1 <<  8,
  DT_DEBUG_MEMORY         = 1 <<  9,
  DT_DEBUG_LIGHTTABLE     = 1 << 10,
  DT_DEBUG_NAN            = 1 << 11,
  DT_DEBUG_MASKS          = 1 << 12,
  DT_DEBUG_LUA            = 1 << 13,
  DT_DEBUG_INPUT          = 1 << 14,
  DT_DEBUG_PRINT          = 1 << 15,
  DT_DEBUG_CAMERA_SUPPORT = 1 << 16,
  DT_DEBUG_IOPORDER       = 1 << 17,
  DT_DEBUG_IMAGEIO        = 1 << 18,
  DT_DEBUG_UNDO           = 1 << 19,
  DT_DEBUG_SIGNAL         = 1 << 20,
  DT_DEBUG_PARAMS         = 1 << 21,
  DT_DEBUG_DEMOSAIC       = 1 << 22,
  DT_DEBUG_SHORTCUTS         = 1 << 23,
  DT_DEBUG_TILING         = 1 << 24,
  DT_DEBUG_HISTORY        = 1 << 25,
  DT_DEBUG_PIPE           = 1 << 26,
  DT_DEBUG_IMPORT         = 1 << 27,
  DT_DEBUG_VERBOSE        = 1 << 28,
  DT_DEBUG_COLORPROFILE   = 1 << 29,
  DT_DEBUG_NOCACHE_REUSE  = 1 << 30,
} dt_debug_thread_t;

typedef struct dt_sys_resources_t
{
  size_t total_memory;     // All RAM on system
  size_t mipmap_memory;    // RAM allocated to mipmap cache
  size_t headroom_memory;  // RAM left to OS & other Apps
  size_t pixelpipe_memory; // RAM used by the pixelpipe cache (approx.)
} dt_sys_resources_t;

typedef struct darktable_t
{
  int32_t num_openmp_threads;

  int32_t unmuted;
  GList *iop;
  GList *iop_order_list;
  GList *iop_order_rules;

  // Keep track of optional features that may depend on environnement
  // ond compiling options : OpenCL, libsecret, kwallet
  GList *capabilities;
  JsonParser *noiseprofile_parser;
  struct dt_conf_t *conf;
  struct dt_develop_t *develop;
  struct dt_lib_t *lib;
  struct dt_view_manager_t *view_manager;
  struct dt_control_t *control;
  struct dt_control_signal_t *signals;
  struct dt_gui_gtk_t *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t *image_cache;
  struct dt_bauhaus_t *bauhaus;
  const struct dt_database_t *db;
  const struct dt_pwstorage_t *pwstorage;
  struct dt_collection_t *collection;
  struct dt_selection_t *selection;
  struct dt_points_t *points;
  struct dt_imageio_t *imageio;
  struct dt_opencl_t *opencl;
  struct dt_dbus_t *dbus;
  struct dt_undo_t *undo;
  struct dt_colorspaces_t *color_profiles;
  struct dt_l10n_t *l10n;
  struct dt_dev_pixelpipe_cache_t *pixelpipe_cache;

  // Protects from concurrent writing at export time
  dt_pthread_mutex_t plugin_threadsafe;

  // Protect appending/removing GList links to the darktable.capabilities list
  dt_pthread_mutex_t capabilities_threadsafe;

  // Exiv2 readMetadata() was not thread-safe prior to 0.27
  // FIXME: Is it now ?
  dt_pthread_mutex_t exiv2_threadsafe;

  // RawSpeed readFile() method is apparently not thread-safe
  dt_pthread_mutex_t readFile_mutex;

  // Prevent concurrent export/thumbnail pipelines from runnnig at the same time
  // It brings no additional performance since the CPU is our bottleneck,
  // and CPU pixel code is already multi-threaded internally through OpenMP
  dt_pthread_mutex_t pipeline_threadsafe;

  // Building SQL transactions through `dt_database_start_transaction_debug()`
  // from "too many" threads (like loading all thumbnails from a new collection)
  // leads to SQL error:
  // `BEGIN": cannot start a transaction within a transaction`
  // Also, we need to ensure that image metadata/history reads & writes
  // happen each in their all time, from all pipeline jobs/threads.
  dt_pthread_rwlock_t database_threadsafe;

  char *progname;
  char *datadir;
  char *sharedir;
  char *moduledir;
  char *localedir;
  char *tmpdir;
  char *configdir;
  char *cachedir;
  char *kerneldir;
  dt_lua_state_t lua_state;
  GList *guides;
  double start_wtime;
  GList *themes;
  int32_t unmuted_signal_dbg_acts;
  gboolean unmuted_signal_dbg[DT_SIGNAL_COUNT];
  GTimeZone *utc_tz;
  GDateTime *origin_gdt;
  struct dt_sys_resources_t dtresources;

  // Working message displayed over the main preview when working
  char *main_message;
} darktable_t;

typedef struct
{
  double clock;
  double user;
} dt_times_t;

extern darktable_t darktable;

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data, lua_State *L);
void dt_cleanup();
void dt_print(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));
/* same as above but without time stamp : nts = no time stamp */
void dt_print_nts(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));
/* same as above but requires additional DT_DEBUG_VERBOSE flag to be true */
void dt_vprint(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));

// Number of workers, on top of reserved workers (1 for main preview, 1 for thumbnail in darkroom)
// This is currently set to 2, so 4 workers total, without user config.
// Workers will process a queue of jobs that they share together (except for reserved ones). 
// It is useless to use more than 2 workers
// since those jobs very often lock some mutex that prevents concurrent running.
// All jobs finding an idle worker will "start" immediately, as far as the OS knows from outside the program,
// but may do nothing internally except for waiting a mutex locked by another worker/thread.
// In that situation, we loose the ability to flush the queue, since jobs are "running".
// So it's better to have few workers with long queues, rather
// than many workers, to be able to control queued jobs.
int dt_worker_threads();

// Get the remaining memory available for pipeline allocations,
// once we subtracted caches memory and headroom from system memory
size_t dt_get_available_mem();

// Get the maximum size for the whole mipmap cache
size_t dt_get_mipmap_mem();

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

// check whether the specified mask of modifier keys exactly matches, among the set Shift+Control+(Alt/Meta).
// ignores the state of any other shifting keys
static inline gboolean dt_modifier_is(const GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in desired_modifier_mask to be the bit for the Cmd key
  return (state & modifiers) == desired_modifier_mask;
}

// check whether the given modifier state includes AT LEAST the specified mask of modifier keys
static inline gboolean dt_modifiers_include(const GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in desired_modifier_mask to be the bit for the Cmd key
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  // check whether all modifier bits of interest are turned on
  return (state & (modifiers & desired_modifier_mask)) == desired_modifier_mask;
}

int dt_capabilities_check(char *capability);
void dt_capabilities_add(char *capability);
void dt_capabilities_remove(char *capability);
void dt_capabilities_cleanup();

static inline double dt_get_wtime(void)
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}

static inline void dt_get_times(dt_times_t *t)
{
  struct rusage ru;

  getrusage(RUSAGE_SELF, &ru);
  t->clock = dt_get_wtime();
  t->user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * (1.0 / 1000000.0);
}

void dt_show_times(const dt_times_t *start, const char *prefix);

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...) __attribute__((format(printf, 3, 4)));

/** \brief check if file is a supported image */
gboolean dt_supported_image(const gchar *filename);

// a few macros and helper functions to speed up certain frequently-used GLib operations
#define g_list_is_singleton(list) ((list) && (!(list)->next))
static inline gboolean g_list_shorter_than(const GList *list, unsigned len)
{
  // instead of scanning the full list to compute its length and then comparing against the limit,
  // bail out as soon as the limit is reached.  Usage: g_list_shorter_than(l,4) instead of g_list_length(l)<4
  while (len-- > 0)
  {
    if (!list) return TRUE;
    list = g_list_next(list);
  }
  return FALSE;
}

// advance the list by one position, unless already at the final node
static inline GList *g_list_next_bounded(GList *list)
{
  return g_list_next(list) ? g_list_next(list) : list;
}

static inline const GList *g_list_next_wraparound(const GList *list, const GList *head)
{
  return g_list_next(list) ? g_list_next(list) : head;
}

static inline const GList *g_list_prev_wraparound(const GList *list)
{
  // return the prior element of the list, unless already on the first element; in that case, return the last
  // element of the list.
  return g_list_previous(list) ? g_list_previous(list) : g_list_last((GList*)list);
}

void dt_print_mem_usage();

void dt_configure_runtime_performance(dt_sys_resources_t *resources, gboolean init_gui);

// helper function which loads whatever image_to_load points to: single image files or whole directories
// it tells you if it was a single image or a directory in single_image (when it's not NULL)
int dt_load_from_string(const gchar *image_to_load, gboolean open_image_in_dr, gboolean *single_image);

#define dt_unreachable_codepath_with_desc(D)                                                                 \
  dt_unreachable_codepath_with_caller(D, __FILE__, __LINE__, __FUNCTION__)
#define dt_unreachable_codepath() dt_unreachable_codepath_with_caller("unreachable", __FILE__, __LINE__, __FUNCTION__)
static inline void dt_unreachable_codepath_with_caller(const char *description, const char *file,
                                                       const int line, const char *function)
{
  fprintf(stderr, "[dt_unreachable_codepath] {%s} %s:%d (%s) - we should not be here. please report this to "
                  "the developers.",
          description, file, line, function);
  __builtin_unreachable();
}

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
  const size_t total_bytes = DT_CACHELINE_BYTES * cache_lines * darktable.num_openmp_threads;
  void *buf = dt_pixelpipe_cache_alloc_align_cache_impl(darktable.pixelpipe_cache, total_bytes, 0, message);
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
  memset(buf, 0, *padded_size * darktable.num_openmp_threads * objsize);
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
// Given the buffer and object count returned by dt_pixelpipe_cache_alloc_perthread and a thread count in 0..darktable.num_openmp_threads,
// return a pointer to the indicated thread's private buffer.
#define dt_get_bythread(buf, padsize, tnum) DT_IS_ALIGNED((buf) + ((padsize) * (tnum)))

// Scramble bits in str to create an (hopefully) unique hash representing the state of str
// Dan Bernstein algo v2 http://www.cse.yorku.ca/~oz/hash.html
// hash should be inited to 5381 if first run, or from a previous hash computed with this function.
static inline uint64_t dt_hash(uint64_t hash, const char *str, size_t size)
{
  for(size_t i = 0; i < size; i++)
    hash = ((hash << 5) + hash) ^ str[i];

  return hash;
}

/** define for max path/filename length */
#define DT_MAX_FILENAME_LEN 256

#ifndef PATH_MAX
/*
 * from /usr/include/linux/limits.h (Linux 3.16.5)
 * Some systems might not define it (e.g. Hurd)
 *
 * We do NOT depend on any specific value of this env variable.
 * If you want constant value across all systems, use DT_MAX_PATH_FOR_PARAMS!
 */
#define PATH_MAX 4096
#endif

/*
 * ONLY TO BE USED FOR PARAMS!!! (e.g. dt_imageio_disk_t)
 *
 * WARNING: this should *NEVER* be changed, as it will break params,
 *          created with previous DT_MAX_PATH_FOR_PARAMS.
 */
#define DT_MAX_PATH_FOR_PARAMS 4096

static inline gchar *dt_string_replace(const char *string, const char *to_replace)
{
  if(IS_NULL_PTR(string) || IS_NULL_PTR(to_replace)) return NULL;
  gchar **split = g_strsplit(string, to_replace, -1);
  gchar *text = g_strjoinv("", split);
  g_strfreev(split);
  return text;
}

// Remove underscore from GUI labels containing mnemonics
static inline gchar *delete_underscore(const char *s)
{
  return dt_string_replace(s, "_");
}

/**
 * @brief Remove Pango/Gtk markup and accels mnemonics from text labels.
 * If the markup parsing fails, fallback to returning a copy of the original string.
 *
 * @param s Original string to clean
 * @return gchar* Newly-allocated string. The caller is responsible for freeing it.
 */
static inline gchar *strip_markup(const char *s)
{
  if(IS_NULL_PTR(s)) return g_strdup("");

  PangoAttrList *attrs = NULL;
  gchar *plain = NULL;

  const gchar *underscore = "_";
  gunichar mnemonic = underscore[0];
  if(!pango_parse_markup(s, -1, mnemonic, &attrs, &plain, NULL, NULL))
    plain = delete_underscore(s);

  pango_attr_list_unref(attrs);
  return plain;
}

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
