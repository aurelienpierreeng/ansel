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
#ifndef DT_COMMON_LOGGING_H
#define DT_COMMON_LOGGING_H

/* Debug-channel flags and the dt_print() family. The implementations live in
 * darktable.c (they read the runtime `darktable.unmuted` mask), but the
 * DECLARATIONS need nothing from the application: low-level compute units include
 * this instead of darktable.h to be able to log. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Debug channels, selected at runtime with -d <channel>.
 *
 * @details These are MASK BITS, combined into the runtime mask dt_get_debug_flags()
 * returns, with two exceptions worth knowing before adding one:
 *
 *   - DT_DEBUG_ALWAYS is 0, so it matches nothing under a bitwise AND. The dt_print()
 *     family special-cases it by equality. A new channel must never be 0.
 *   - bit 6 (1 << 6) is FREE -- the list jumps from 1 << 5 to 1 << 7. The values are not
 *     contiguous and nothing may assume they are.
 *
 * Bit 31 cannot be spelled as an enumerator (1 << 31 does not fit an `int`), so
 * DT_DEBUG_SUPERVISOR is a macro below. The mask is an int32_t and is now FULL: a new
 * channel needs bit 6, or a wider mask.
 */
typedef enum dt_debug_thread_t
{
  DT_DEBUG_ALWAYS        = 0,       // always print regardless of debug flags
  // powers of two, masking
  DT_DEBUG_CACHE          = 1 <<  0,
  DT_DEBUG_CONTROL        = 1 <<  1,
  DT_DEBUG_DEV            = 1 <<  2,
  DT_DEBUG_GTK            = 1 <<  3,
  DT_DEBUG_PERF           = 1 <<  4,
  DT_DEBUG_PIPECACHE      = 1 <<  5,
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
  DT_DEBUG_SHORTCUTS      = 1 << 23,
  DT_DEBUG_TILING         = 1 << 24,
  DT_DEBUG_HISTORY        = 1 << 25,
  DT_DEBUG_PIPE           = 1 << 26,
  DT_DEBUG_IMPORT         = 1 << 27,
  DT_DEBUG_VERBOSE        = 1 << 28,
  DT_DEBUG_COLORPROFILE   = 1 << 29,
  DT_DEBUG_NOCACHE_REUSE  = 1 << 30,
} dt_debug_thread_t;

// Uses the top (sign) bit of the int32_t `unmuted` mask. Defined as a macro
// because 1 << 31 is not representable as an `int` enumerator. Enables the
// high-level event supervisor (NDJSON tracing of history/pipeline/cache state).
// See develop/supervisor.h.
#define DT_DEBUG_SUPERVISOR ((int32_t)(1u << 31))

/* Runtime debug-channel mask (the application's `darktable.unmuted`). Accessor
 * declared here, implemented by the orchestrator (darktable.c, next to
 * dt_print()) so header inlines can gate on debug channels without importing
 * the application struct. Test with `dt_get_debug_flags() & DT_DEBUG_XXX`. */
int32_t dt_get_debug_flags(void);

/**
 * @brief Print to stdout when @p thread is enabled, prefixed with seconds since startup.
 *
 * @param thread the channel to gate on, or DT_DEBUG_ALWAYS to print unconditionally.
 * @param msg printf format string; the call is format-checked at compile time.
 *
 * @note Flushes stdout on every call, so output survives a crash immediately after -- the
 * reason to use this rather than a bare fprintf(stderr) when tracing a crash.
 *
 * @note Format strings are checked against the GNU conversions on every platform,
 * Windows included: src/CMakeLists.txt builds with __USE_MINGW_ANSI_STDIO=1, which is what
 * makes `%z` and friends work under MinGW rather than falling back to the MS runtime's
 * narrower set. Dropping that definition would silently change what these format strings
 * mean on Windows only.
 */
void dt_print(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));

/** @brief dt_print() without the timestamp prefix (nts = no time stamp). */
void dt_print_nts(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));

/** @brief dt_print() that additionally requires DT_DEBUG_VERBOSE to be enabled, i.e. both
 * `-d <channel>` and `-d verbose`.
 * @note The `v` is for VERBOSE, not for va_list -- this is variadic like the others, not a
 * vprintf()-style counterpart. */
void dt_vprint(dt_debug_thread_t thread, const char *msg, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_LOGGING_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
