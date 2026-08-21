/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

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

#include "database/selection_repository.h"

#include "common/image.h"   // for UNKNOWN_IMAGE
#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <sqlite3.h>

/* The read is on the hot path -- every selection change re-reads the whole table to
 * rebuild the in-memory mirror -- so its statement is cached. The writes are one per user
 * action and are not. */

void dt_selection_repository_select(const int32_t imgid)
{
  if(imgid < 0) return;

  gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

void dt_selection_repository_deselect(const int32_t imgid)
{
  if(imgid < 0) return;

  gchar *query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid = %d", imgid);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

void dt_selection_repository_select_list(const char *ids)
{
  if(IS_NULL_PTR(ids)) return;

  gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES %s", ids);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

void dt_selection_repository_deselect_list(const char *ids)
{
  if(IS_NULL_PTR(ids)) return;

  gchar *query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid IN (%s)", ids);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

void dt_selection_repository_clear(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM main.selected_images",
                        NULL, NULL, NULL);
}

GList *dt_selection_repository_get_all(void)
{
  /* Prepared per call: a cached statement here would be shared GUI-thread/worker-thread
   * state with no lock, and two threads stepping one statement interleave both row
   * cursors. Same rule as the rest of this module's uncached functions. */
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT imgid FROM main.selected_images ORDER BY imgid DESC",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return NULL;

  GList *list = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    list = g_list_prepend(list, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));

  sqlite3_finalize(stmt);

  /* Not reversed: the query orders DESC and prepending flips it, so the caller gets
   * ascending order. That was the previous behaviour and the selection's in-memory mirror
   * depends on it. */
  return list;
}

void dt_selection_repository_drop_uncollected(void)
{
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "DELETE FROM main.selected_images"
                        " WHERE imgid NOT IN"
                        " (SELECT imgid FROM memory.collected_images)", NULL, NULL, NULL);
  // clang-format on
}

void dt_selection_repository_push(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.selected_backup",
                        NULL, NULL, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "INSERT INTO memory.selected_backup"
                        " SELECT * FROM main.selected_images", NULL, NULL, NULL);
  // clang-format on
}

void dt_selection_repository_pop(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM main.selected_images",
                        NULL, NULL, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "INSERT INTO main.selected_images"
                        " SELECT * FROM memory.selected_backup", NULL, NULL, NULL);
  // clang-format on
}

int32_t dt_selection_repository_get_lowest_id(void)
{
  int32_t imgid = UNKNOWN_IMAGE;
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return imgid;

  if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return imgid;
}

void dt_selection_repository_cleanup(void)
{
  /* Nothing cached any more: every statement in this file is prepared and finalised per
   * call. Kept because the connection's close order calls every repository's cleanup. */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
