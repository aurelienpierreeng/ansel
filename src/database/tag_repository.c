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

#include "database/tag_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <sqlite3.h>

GList *dt_tag_repository_get_attached_names(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;

  if(imgid < 0)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                "i.tagid = t.id WHERE imgid IN "
                                "(SELECT imgid FROM main.selected_images)",
                                -1, &stmt, NULL);
    // clang-format on
  }
  else // single image under mouse cursor
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                "i.tagid = t.id WHERE imgid = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  }

  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    result = g_list_prepend(result, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);

  return g_list_reverse(result);
}

int dt_tag_repository_count_images(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT (*)"
                              "  FROM main.tagged_images"
                              "  WHERE tagid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  int count = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

void dt_tag_count_free(gpointer data)
{
  dt_tag_count_t *t = (dt_tag_count_t *)data;
  if(IS_NULL_PTR(t)) return;
  dt_free(t->name);
  dt_free(t);
}

GList *dt_tag_repository_get_by_path_with_counts(const char *path, const char *path_prefix)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT t.id, t.name, ti.count"
                              "  FROM data.tags AS t"
                              "  LEFT JOIN (SELECT tagid,"
                              "               COUNT(DISTINCT imgid) AS count"
                              "             FROM main.tagged_images"
                              "             GROUP BY tagid) AS ti"
                              "  ON ti.tagid = t.id"
                              "  WHERE name = ?1 OR SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, path, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, path_prefix, -1, SQLITE_TRANSIENT);

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    dt_tag_count_t *t = g_malloc0(sizeof(dt_tag_count_t));
    if(t)
    {
      t->id = sqlite3_column_int(stmt, 0);
      t->name = g_strdup(name ? name : "");
      t->count = sqlite3_column_int(stmt, 2);
      tags = g_list_prepend(tags, t);
    }
  }
  sqlite3_finalize(stmt);

  return tags; // not reversed: matches the previous prepend-only order
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
