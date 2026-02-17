/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010-2011, 2013-2014, 2016-2018, 2020 Tobias Ellinghaus.
    Copyright (C) 2011 Karl Mikaelsson.
    Copyright (C) 2012 johannes hanika.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2015-2016 Bernd Steinhauser.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 Marcus Rückert.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
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
// CMake uses config.cmake.h to generate config.h within the build folder.
#pragma once

#include <stddef.h>

// clang-format off
// it butchers @@ and ${} :(

#define PACKAGE_NAME "@CMAKE_PROJECT_NAME@"
#define PACKAGE_BUGREPORT "https://github.com/aurelienpierreeng/ansel/issues"

// these will be defined in build/bin/version_gen.c
extern const char darktable_package_version[];
extern const char darktable_package_string[];
extern const char darktable_last_commit_year[];

static const char *dt_supported_extensions[] __attribute__((unused)) = {"@DT_SUPPORTED_EXTENSIONS_STRING@", NULL};

#define GETTEXT_PACKAGE "ansel"

// Those are used to find needed dirs runtime, so they needs to be relative.
#define DARKTABLE_LOCALEDIR "@REL_BIN_TO_LOCALEDIR@"
#define DARKTABLE_MODULEDIR "@REL_BIN_TO_MODULEDIR@"
#define DARKTABLE_DATADIR   "@REL_BIN_TO_DATADIR@"
#define DARKTABLE_SHAREDIR  "@REL_BIN_TO_SHAREDIR@"
#define DARKTABLE_KERNELSDIR "@REL_BIN_TO_DATADIR@/kernels"

#define SHARED_MODULE_PREFIX "@CMAKE_SHARED_MODULE_PREFIX@"
#define SHARED_MODULE_SUFFIX "@CMAKE_SHARED_MODULE_SUFFIX@"

#define WANTED_STACK_SIZE (@WANTED_STACK_SIZE@)
#define WANTED_THREADS_STACK_SIZE (@WANTED_THREADS_STACK_SIZE@)

#define ISO_CODES_LOCATION "@IsoCodes_LOCATION@"
#define ISO_CODES_LOCALEDIR "@IsoCodes_LOCALEDIR@"

// clang-format on

#ifndef __GNUC_PREREQ
// on OSX, gcc-4.6 and clang chokes if this is not here.
#if defined __GNUC__ && defined __GNUC_MINOR__
#define __GNUC_PREREQ(maj, min) ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

#if defined(_OPENMP) && __GNUC_PREREQ(4, 9)
#define OPENMP_SIMD_
#define SIMD() simd
#else
#define SIMD()
#endif

// see http://clang.llvm.org/docs/LanguageExtensions.html
#ifndef __has_feature      // Optional of course.
#define __has_feature(x) 0 // Compatibility with non-clang compilers.
#endif
#ifndef __has_extension
#define __has_extension __has_feature // Compatibility with pre-3.0 compilers.
#endif

// see https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

#cmakedefine HAVE_CPUID_H 1
#cmakedefine HAVE___GET_CPUID 1

#cmakedefine HAVE_OMP_FIRSTPRIVATE_WITH_CONST 1

#cmakedefine HAVE_THREAD_RWLOCK_ARCH_T_READERS 1

#cmakedefine HAVE_THREAD_RWLOCK_ARCH_T_NR_READERS 1

/******************************************************************************
 * OpenCL target settings
 *****************************************************************************/

// OpenCL 3.0 is the highest version supported by Nvidia drivers as of 2025
// and AMD caught up to 2.0.
#define CL_TARGET_OPENCL_VERSION 200

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
