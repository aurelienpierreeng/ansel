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

/** @file tests/unittests/testdb.h
 *
 * @brief Shared fixture for the src/database repository tests.
 *
 * Opens the one connection on ":memory:" (dt_database_open() then puts data and memory on
 * ":memory:" too, and skips the lock files), and closes it again after finalising every
 * repository's cached statements -- the same discipline the application follows at
 * shutdown, and the reason dt_database_close() can be called at all.
 *
 * Everything a test writes goes through the repository APIs. That is not a convenience:
 * tools/check_module_boundaries.sh holds the module's raw-connection accessor at ZERO
 * call sites outside src/database, and these tests live outside src/database. A test that
 * cannot set up its rows through the public API has found a hole in the API.
 */

#ifndef DT_TESTS_UNITTESTS_TESTDB_H
#define DT_TESTS_UNITTESTS_TESTDB_H

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include "database/collection_query.h"
#include "database/colorlabel_repository.h"
#include "database/database.h"
#include "database/film_repository.h"
#include "database/history_repository.h"
#include "database/image_repository.h"
#include "database/metadata_repository.h"
#include "database/preset_repository.h"
#include "database/selection_repository.h"
#include "database/style_repository.h"
#include "database/tag_repository.h"

static inline int testdb_setup(void **state)
{
  (void)state;
  const dt_database_params_t params = { .alternative = NULL,
                                        .library = ":memory:",
                                        .load_data = FALSE,
                                        .has_gui = FALSE,
                                        .verbose = FALSE };
  return dt_database_open(&params) == DT_DATABASE_OPEN_OK ? 0 : -1;
}

static inline int testdb_teardown(void **state)
{
  (void)state;
  /* Every cleanup is idempotent and safe for repositories the test never touched. The
   * connection cannot close over a live statement, so this order is load-bearing. */
  dt_collection_query_cleanup();
  dt_colorlabel_repository_cleanup();
  dt_history_repository_cleanup();
  dt_image_repository_cleanup();
  dt_metadata_repository_cleanup();
  dt_preset_repository_cleanup();
  dt_selection_repository_cleanup();
  dt_style_repository_cleanup();
  dt_tag_repository_cleanup();
  dt_database_close();
  return 0;
}

/** A film roll to hang images on, created through the API and returned by id. */
static inline int32_t testdb_make_film(const char *folder)
{
  if(!dt_film_repository_insert(folder)) return -1;
  return dt_film_repository_find_by_folder(folder);
}

/** One imported image row, by id. Everything but the identity is at its default. */
static inline int32_t testdb_make_image(const int32_t film_id, const char *filename)
{
  if(!dt_image_repository_insert_import(film_id, filename, 0, 1000)) return -1;
  return dt_image_repository_find_by_film_and_filename(film_id, filename);
}

#endif // DT_TESTS_UNITTESTS_TESTDB_H
