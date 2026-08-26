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
#ifndef DT_SYSTEM_MACROS_H
#define DT_SYSTEM_MACROS_H

/* Base language-level macros with zero dependencies. This is the bottom of the
 * include graph: anything may include it, it includes nothing of ours. */

#include <stdio.h>
#include <stddef.h>

/* Windows legacy-macro shim. windows.h #defines `near`, `grp2`, `interface` and
 * friends, and win/win.h #undefs them (and orders winsock2 before windows.h). Every
 * TU used to inherit this through darktable.h; now that low-level code no
 * longer includes the orchestrator, the shim has to live at the bottom of the stack
 * -- this header -- or the collisions come back on MinGW only, far from their cause.
 * Harmless on every other platform: the whole block disappears. */
#if defined _WIN32
#include "win/win.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stringify @p x WITHOUT expanding it first. Implementation detail of STR(). */
#undef STR_HELPER
#define STR_HELPER(x) #x

/** @brief Stringify @p x after macro-expanding it, e.g. STR(__LINE__) gives "42".
 * @note The indirection through STR_HELPER() is what forces the expansion; stringifying
 * directly would yield "__LINE__". */
#undef STR
#define STR(x) STR_HELPER(x)

/** @brief Stringify @p x WITHOUT expanding it first. Implementation detail of
 * DT_STRINGIFY(). */
#define DT_STRINGIFY_HELPER(x) #x

/** @brief Expand-then-stringify, identical in behaviour to STR().
 * @note Both spellings exist for historical reasons and neither is deprecated. STR() is
 * #undef'd before definition because windows.h and some third-party headers define a name
 * that short; DT_STRINGIFY() is the collision-proof spelling and is the safer default in
 * new code. */
#define DT_STRINGIFY(x) DT_STRINGIFY_HELPER(x)

/** @brief `restrict` in C, nothing in C++.
 *
 * @details `restrict` is not a C++ keyword, so a header shared with a .cc file cannot spell
 * it directly. Where it survives (C), it is a PROMISE that the pointed-to memory is not
 * reached through any other pointer for that object's lifetime; breaking the promise is
 * undefined behaviour the compiler will happily optimise around, and the symptom is wrong
 * pixels rather than a crash. Note the promise silently disappears in C++ translation
 * units, so a bug it hides can behave differently in .c and .cc code. */
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
 * Rejected at COMPILE time: integers, floats, structs, and arrays -- an array's
 * `__typeof__` cannot initialise a local, which is what stops `IS_NULL_PTR(some_array)`
 * from quietly testing a never-NULL decayed address.
 *
 * ACCEPTED, contrary to what this comment claimed for a long time: function pointers.
 * __builtin_classify_type() reports them as pointers, so `IS_NULL_PTR(callback)` compiles
 * and does the obvious, useful thing. Verified rather than assumed.
 *
 * @param p any pointer expression. Evaluated EXACTLY ONCE -- it is bound to a local first
 * -- so `IS_NULL_PTR(*iter++)` is safe.
 * @return TRUE when @p p is NULL.
 *
 * @note Uses a GNU statement expression and __typeof__, so this header requires GCC or
 * Clang. That is already true of the whole tree, MinGW included.
 */
#define IS_NULL_PTR(p)                                            \
  ({                                                              \
    __typeof__(p) _tmp = (p);                                     \
    (void)sizeof(char[                                            \
      (__builtin_classify_type(_tmp) == 5) ? 1 : -1               \
    ]);                                                           \
    _tmp == NULL;                                                 \
  })

/** @brief Mark a branch as impossible, naming it @p D in the message. @see
 * dt_unreachable_codepath() */
/** @brief Mark a deliberate `switch` fall-through so compilers and analysers stop warning.
 *
 * @details Write it as a statement where the `break` would go:
 * `case 7: b |= ...; DT_FALLTHROUGH;`
 *
 * The conventional "fall through" COMMENT is understood by GCC's -Wimplicit-fallthrough
 * and by clang-tidy, but not by every analyser -- SonarCloud reports each one as `c:S128`
 * ("switch case should end with an unconditional break"), which is 7 findings on one
 * intentionally-unrolled loop in common/hash.h. The attribute is a token the parser sees,
 * so it settles the question everywhere at once.
 *
 * Expands to nothing on a toolchain without the attribute, where the comment convention
 * (or nothing at all) was the only option anyway.
 */
#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define DT_FALLTHROUGH __attribute__((fallthrough))
#endif
#endif
#ifndef DT_FALLTHROUGH
#define DT_FALLTHROUGH ((void)0)
#endif

#define dt_unreachable_codepath_with_desc(D)                                                                 \
  dt_unreachable_codepath_with_caller(D, __FILE__, __LINE__, __FUNCTION__)

/** @brief Mark a branch as impossible.
 *
 * @warning This is an ASSERTION OF FACT, not a guard. It ends in
 * __builtin_unreachable(), which tells the optimiser the branch cannot be entered; if it
 * ever is, the program has undefined behaviour and the printed message is no guarantee --
 * the compiler may already have deleted the surrounding test. Use it only where the
 * impossibility is structural. To HANDLE an unexpected value, return an error instead.
 */
#define dt_unreachable_codepath() dt_unreachable_codepath_with_caller("unreachable", __FILE__, __LINE__, __FUNCTION__)

/** @brief Back-end of the dt_unreachable_codepath() macros; call those, so the call site
 * is recorded automatically.
 *
 * @param description short label for the impossible branch.
 * @param file,line,function the call site, supplied by the macros.
 * @return never: the function does not come back.
 *
 * @note Writes to stderr WITHOUT a trailing newline, so the message runs into whatever is
 * printed next -- and stderr is unbuffered, so it does survive a crash immediately after,
 * which is the point.
 */
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

#endif // DT_SYSTEM_MACROS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
