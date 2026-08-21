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

#include "database/colorlabel_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"

#include <sqlite3.h>

/* Every statement is prepared and finalised per call. These four were cached "because the
 * lighttable asks for an image's labels on every thumbnail" -- that stopped being true when
 * the thumbtable moved to one bulk query computing the label mask inline; the remaining
 * callers run at import/click frequency, and an unlocked cached statement shared between
 * the GUI thread and worker jobs is a data race, not an optimisation. */
static sqlite3_stmt *_prepared(const char *query)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  return stmt;
}

int dt_colorlabel_repository_get(const int32_t imgid)
{
  sqlite3_stmt *stmt = _prepared("SELECT color FROM main.color_labels WHERE imgid = ?1");
  if(IS_NULL_PTR(stmt)) return 0;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  int colors = 0;
  // Colors are int between 0 and 5, turn them into octal bitmask
  while(sqlite3_step(stmt) == SQLITE_ROW)
    colors |= (1 << sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);

  return colors;
}

void dt_colorlabel_repository_set(const int32_t imgid, const int color)
{
  sqlite3_stmt *stmt = _prepared("INSERT OR IGNORE INTO main.color_labels (imgid, color) VALUES (?1, ?2)");
  if(IS_NULL_PTR(stmt)) return;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabel_repository_remove(const int32_t imgid, const int color)
{
  sqlite3_stmt *stmt = _prepared("DELETE FROM main.color_labels WHERE imgid=?1 AND color=?2");
  if(IS_NULL_PTR(stmt)) return;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabel_repository_remove_all(const int32_t imgid)
{
  sqlite3_stmt *stmt = _prepared("DELETE FROM main.color_labels WHERE imgid=?1");
  if(IS_NULL_PTR(stmt)) return;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_colorlabel_repository_has(const int32_t imgid, const int color)
{
  if(imgid <= 0) return FALSE;

  /* Not cached: this one is asked once per user action, not once per thumbnail. */
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT * FROM main.color_labels WHERE imgid=?1 AND color=?2 LIMIT 1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);

  const gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return found;
}

GList *dt_colorlabel_repository_get_list(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;

  if(imgid < 0)
  {
    /* No ORDER BY on this branch and ORDER BY color on the other. That asymmetry is
     * inherited from dt_metadata_get(); ordering a selection by colour would group the
     * same colour from different images together, which is not what the caller counts. */
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT color FROM main.color_labels WHERE imgid IN "
                                "(SELECT imgid FROM main.selected_images)",
                                -1, &stmt, NULL);
    // clang-format on
  }
  else
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT color FROM main.color_labels WHERE imgid=?1 ORDER BY color",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  }

  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    result = g_list_prepend(result, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  return g_list_reverse(result);
}

void dt_colorlabel_repository_foreach(const int32_t imgid, void (*cb)(void *, const int),
                                      void *user_data)
{
  if(!cb) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
}

void dt_colorlabel_repository_cleanup(void)
{
  /* Nothing cached any more: every statement in this file is prepared and finalised per
   * call. Kept because the connection's close order calls every repository's cleanup. */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
