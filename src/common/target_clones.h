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
#ifndef DT_COMMON_TARGET_CLONES_H
#define DT_COMMON_TARGET_CLONES_H

/* Function multi-versioning (target_clones) for SIMD compute functions.
 *
 * This is THE canonical definition of __DT_CLONE_TARGETS__: common/darktable.h
 * inherits it from here (it used to carry a deliberate duplicate until the
 * de-glueing landed). Self-contained on purpose — low-level compute units
 * (common/nn_model.c and friends) include this without pulling anything else.
 *
 * The guard keeps the definition inert if some translation unit ends up seeing
 * it twice through different paths.
 */

#ifndef __DT_CLONE_TARGETS__

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

#endif // __DT_CLONE_TARGETS__
#endif // DT_COMMON_TARGET_CLONES_H
