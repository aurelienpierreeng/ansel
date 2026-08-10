/*
    This file is part of darktable,
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011 johannes hanika.
    Copyright (C) 2011, 2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022, 2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025 Alynx Zhou.

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

/** @file common/debug.h
 *
 * @brief Call tracing for functions you do not want to edit every caller of.
 *
 * @note This header used to also carry the `DT_DEBUG_SQLITE3_*` family, so a file wanting
 * ::DT_DEBUG_TRACE_WRAPPER -- which has nothing to do with SQL -- got `database.h` and
 * `<sqlite3.h>` with it. Those macros live in `database/sql_debug.h` now, next to the
 * connection they call into.
 */

#ifndef DT_COMMON_DEBUG_H
#define DT_COMMON_DEBUG_H

/* Self-containment: DT_DEBUG_TRACE_WRAPPER expands to dt_vprint() and the
 * dt_debug_thread_t enumeration, both from common/logging.h. */

#include "common/logging.h"

#ifdef __cplusplus
extern "C" {
#endif

// Use this to re-define a function to trace it, so you don't need to modify all
// callers. `thread` should be `dt_debug_thread_t`. This requires verbose mode.
//
// Example:
// void dt_dev_pixelpipe_update_history_main_real(dt_develop_t *dev);
// #define dt_dev_pixelpipe_update_history_main(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_history_main_real, (dev))
#define DT_DEBUG_TRACE_WRAPPER(thread, function, ...)                    \
  do {                                                                   \
    dt_vprint((thread), "[debug_trace] %s is called from %s at %s:%d\n", \
              #function, __FUNCTION__, __FILE__, __LINE__);              \
    function(__VA_ARGS__);                                               \
  } while (0)

// Same, for a function taking no arguments. ISO C wants at least one argument for a
// variadic macro's `...`, so DT_DEBUG_TRACE_WRAPPER cannot be handed an empty list
// without relying on a GNU extension.
#define DT_DEBUG_TRACE_WRAPPER_VOID(thread, function)                    \
  do {                                                                   \
    dt_vprint((thread), "[debug_trace] %s is called from %s at %s:%d\n", \
              #function, __FUNCTION__, __FILE__, __LINE__);              \
    function();                                                          \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_DEBUG_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
