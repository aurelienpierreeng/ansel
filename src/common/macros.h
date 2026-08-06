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
#ifndef DT_COMMON_MACROS_H
#define DT_COMMON_MACROS_H

/* Base language-level macros with zero dependencies. This is the bottom of the
 * include graph: anything may include it, it includes nothing of ours. */

#include <stdio.h>
#include <stddef.h>

/* Windows legacy-macro shim. windows.h #defines `near`, `grp2`, `interface` and
 * friends, and win/win.h #undefs them (and orders winsock2 before windows.h). Every
 * TU used to inherit this through common/darktable.h; now that low-level code no
 * longer includes the orchestrator, the shim has to live at the bottom of the stack
 * -- this header -- or the collisions come back on MinGW only, far from their cause.
 * Harmless on every other platform: the whole block disappears. */
#if defined _WIN32
#include "win/win.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#undef STR_HELPER
#define STR_HELPER(x) #x

#undef STR
#define STR(x) STR_HELPER(x)

#define DT_STRINGIFY_HELPER(x) #x
#define DT_STRINGIFY(x) DT_STRINGIFY_HELPER(x)

// When included by a C++ file, restrict qualifiers are not allowed
#ifdef __cplusplus
#define DT_RESTRICT
#else
#define DT_RESTRICT restrict
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

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_MACROS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
