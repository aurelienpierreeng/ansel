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

/** @file database/sql_debug.h
 *
 * @brief Checked wrappers around the `sqlite3_*` calls: trace the statement, assert the
 * return code, report the connection's error message when it is not SQLITE_OK.
 *
 * @warning **This header is scaffolding, and it is counted.** It exists because SQL is
 * still written outside `src/database` -- every file that issues a query includes it, and
 * every one of those is a translation unit holding a raw `sqlite3 *` that the module cannot
 * account for. `tools/check_module_boundaries.sh` ratchets the number of such files
 * downwards; the header is deleted when the count reaches zero and the macros become
 * private to the repositories.
 *
 * So: **do not include this from new code.** A new query belongs in a repository under
 * `src/database/`, reached through a named function. See `src/database/README.md`.
 *
 * @note It used to live in `common/debug.h`, which meant that a file wanting nothing but
 * ::DT_DEBUG_TRACE_WRAPPER -- a generic tracing helper with no relation to SQL -- pulled
 * in `database.h` and `<sqlite3.h>` as well. That was 42 translation units, of which 5
 * never issued a query. `common/debug.h` now carries only the tracing wrapper.
 */

#ifndef DT_DATABASE_SQL_DEBUG_H
#define DT_DATABASE_SQL_DEBUG_H

/* Self-containment: the macros below expand to dt_database_get_sqlite3_global()
 * (database/database.h), dt_print()/DT_DEBUG_SQL (common/logging.h) and fprintf()
 * (<stdio.h>). Include them here rather than making every consumer of the macros do it. */

#include "common/logging.h"
#include "database/database.h"

#include <sqlite3.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// define this to see all sql queries passed to prepare and exec at compile time, or a variable name
// warning:
// there are some direct calls to sqlite3_exec and sqlite3_prepare_v2 which are missing here. grep manually.
// #define DEBUG_SQL_QUERIES

#ifdef DEBUG_SQL_QUERIES
  #define __STRINGIFY(TEXT) #TEXT
  #define MESSAGE(VALUE) __STRINGIFY(message __STRINGIFY(SQLDEBUG: VALUE))
  #define __DT_DEBUG_SQL_QUERY__(value) _Pragma(MESSAGE(value))
#else
  #define __DT_DEBUG_SQL_QUERY__(value)
#endif


#ifdef _DEBUG
#include <assert.h> // conditional-ok: assert() is used only in this arm; the release arm below reports and continues
#define __DT_DEBUG_ASSERT__(xin)                                                                                  \
  {                                                                                                               \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") const int x = xin;              \
    if(x != SQLITE_OK)                                                                                            \
    {                                                                                                             \
      fprintf(stderr, "sqlite3 error: %s:%d, function %s(): %s\n", __FILE__, __LINE__, __FUNCTION__,              \
              sqlite3_errmsg(dt_database_get_sqlite3_global()));                                                     \
    }                                                                                                             \
    assert(x == SQLITE_OK);                                                                                       \
    _Pragma("GCC diagnostic pop")                                                                                 \
  }
#define __DT_DEBUG_ASSERT_WITH_QUERY__(xin, query)                                                                \
  {                                                                                                               \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") const int x = xin;              \
    if(x != SQLITE_OK)                                                                                            \
    {                                                                                                             \
      fprintf(stderr, "sqlite3 error: %s:%d, function %s(), query \"%s\": %s\n", __FILE__, __LINE__, __FUNCTION__,\
              (query), sqlite3_errmsg(dt_database_get_sqlite3_global()));                                            \
    }                                                                                                             \
    assert(x == SQLITE_OK);                                                                                       \
    _Pragma("GCC diagnostic pop")                                                                                 \
  }
#else
#define __DT_DEBUG_ASSERT__(xin)                                                                                  \
  {                                                                                                               \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") const int x = xin;              \
    if(x != SQLITE_OK)                                                                                            \
    {                                                                                                             \
      fprintf(stderr, "sqlite3 error: %s:%d, function %s(): %s\n", __FILE__, __LINE__, __FUNCTION__,              \
              sqlite3_errmsg(dt_database_get_sqlite3_global()));                                                     \
    }                                                                                                             \
    _Pragma("GCC diagnostic pop")                                                                                 \
  }
#define __DT_DEBUG_ASSERT_WITH_QUERY__(xin, query)                                                                \
  {                                                                                                               \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"") const int x = xin;              \
    if(x != SQLITE_OK)                                                                                            \
    {                                                                                                             \
      fprintf(stderr, "sqlite3 error: %s:%d, function %s(), query \"%s\": %s\n", __FILE__, __LINE__, __FUNCTION__,\
              (query), sqlite3_errmsg(dt_database_get_sqlite3_global()));                                            \
    }                                                                                                             \
    _Pragma("GCC diagnostic pop")                                                                                 \
  }

#endif

#define DT_DEBUG_SQLITE3_EXEC(a, b, c, d, e)                                                                      \
  do                                                                                                              \
  {                                                                                                               \
    dt_print(DT_DEBUG_SQL, "[sql] %s:%d, function %s(): exec \"%s\"\n", __FILE__, __LINE__, __FUNCTION__, (b));   \
    __DT_DEBUG_ASSERT_WITH_QUERY__(sqlite3_exec(a, b, c, d, e), (b));                                             \
    __DT_DEBUG_SQL_QUERY__(b)                                                                                     \
  } while(0)

#define DT_DEBUG_SQLITE3_PREPARE_V2(a, b, c, d, e)                                                                \
  do                                                                                                              \
  {                                                                                                               \
    dt_print(DT_DEBUG_SQL, "[sql] %s:%d, function %s(): prepare \"%s\"\n", __FILE__, __LINE__, __FUNCTION__, (b));\
    __DT_DEBUG_ASSERT_WITH_QUERY__(sqlite3_prepare_v2(a, b, c, d, e), (b));                                       \
    __DT_DEBUG_SQL_QUERY__(b)                                                                                     \
  } while(0)

#define DT_DEBUG_SQLITE3_BIND_INT(a, b, c) __DT_DEBUG_ASSERT__(sqlite3_bind_int(a, b, c))
#define DT_DEBUG_SQLITE3_BIND_INT64(a, b, c) __DT_DEBUG_ASSERT__(sqlite3_bind_int64(a, b, c))
#define DT_DEBUG_SQLITE3_BIND_DOUBLE(a, b, c) __DT_DEBUG_ASSERT__(sqlite3_bind_double(a, b, c))
#define DT_DEBUG_SQLITE3_BIND_TEXT(a, b, c, d, e) __DT_DEBUG_ASSERT__(sqlite3_bind_text(a, b, c, d, e))
#define DT_DEBUG_SQLITE3_BIND_BLOB(a, b, c, d, e) __DT_DEBUG_ASSERT__(sqlite3_bind_blob(a, b, c, d, e))
#define DT_DEBUG_SQLITE3_CLEAR_BINDINGS(a) __DT_DEBUG_ASSERT__(sqlite3_clear_bindings(a))
#define DT_DEBUG_SQLITE3_RESET(a) __DT_DEBUG_ASSERT__(sqlite3_reset(a))

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_SQL_DEBUG_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
