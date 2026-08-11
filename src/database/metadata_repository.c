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

#include "database/metadata_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <sqlite3.h>

GList *dt_metadata_repository_get_values(const int32_t imgid, const int keyid)
{
  /* Prepared per call: a cached statement here would be shared across the GUI thread and
   * worker jobs with no lock -- two threads stepping one statement interleave both row
   * cursors, and the lazy first-use prepare is itself a race. */
  sqlite3_stmt *stmt = NULL;

  if(imgid < 0)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT value FROM main.meta_data WHERE id IN "
                                "(SELECT imgid FROM main.selected_images) AND key = ?1 ORDER BY value",
                                -1, &stmt, NULL);
    // clang-format on
    if(IS_NULL_PTR(stmt)) return NULL;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, keyid);
  }
  else // single image under mouse cursor
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT value FROM main.meta_data WHERE id = ?1 AND key = ?2 ORDER BY value",
                                -1, &stmt, NULL);
    // clang-format on
    if(IS_NULL_PTR(stmt)) return NULL;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, keyid);
  }

  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *value = (const char *)sqlite3_column_text(stmt, 0);
    result = g_list_prepend(result, g_strdup(value ? value : "")); // to avoid NULL value
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(result); // list was built in reverse order, so un-reverse it
}

GList *dt_metadata_repository_get_all(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT key, value FROM main.meta_data WHERE id=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *metadata = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *value = (const char *)sqlite3_column_text(stmt, 1);
    gchar *ckey = g_strdup_printf("%d", sqlite3_column_int(stmt, 0));
    gchar *cvalue = g_strdup(value ? value : ""); // to avoid NULL value
    metadata = g_list_append(metadata, (gpointer)ckey);
    metadata = g_list_append(metadata, (gpointer)cvalue);
  }
  sqlite3_finalize(stmt);
  return metadata;
}

void dt_metadata_repository_remove(const int32_t imgid, const char *keyid_list)
{
  if(imgid <= 0 || IS_NULL_PTR(keyid_list)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("DELETE FROM main.meta_data WHERE id = %d AND key IN (%s)",
                                 imgid, keyid_list);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);
}

void dt_metadata_repository_add(const dt_metadata_row_t *rows, const size_t count)
{
  if(IS_NULL_PTR(rows) || count == 0) return;

  /* One statement for the whole batch, as before. Built rather than bound because the
   * number of rows is not known until here, and a prepared statement's placeholder count
   * is fixed. */
  GString *values = g_string_new(NULL);
  for(size_t i = 0; i < count; i++)
  {
    char *escaped = sqlite3_mprintf("%q", rows[i].value ? rows[i].value : "");
    g_string_append_printf(values, "%s(%d,%d,'%s')", (i > 0) ? "," : "",
                           rows[i].imgid, rows[i].keyid, escaped);
    sqlite3_free(escaped);
  }

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("INSERT INTO main.meta_data (id, key, value) VALUES %s", values->str);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);
  g_string_free(values, TRUE);
}

int32_t dt_metadata_repository_find_image_by_value(const char *value)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.meta_data WHERE value=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, value, -1, SQLITE_TRANSIENT);

  int32_t imgid = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > -1)
    imgid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return imgid;
}

void dt_metadata_repository_foreach(const int32_t imgid, dt_metadata_repository_row_cb cb,
                                    void *user_data)
{
  if(!cb) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "SELECT key, value FROM main.meta_data WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
}

void dt_metadata_repository_foreach_selected(dt_metadata_repository_selected_cb cb, void *user_data)
{
  if(!cb) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT m.key, m.value, COUNT(m.id) AS ct"
                              " FROM main.meta_data AS m"
                              " JOIN main.selected_images AS s ON s.imgid = m.id"
                              " GROUP BY m.key, m.value ORDER BY m.value",
                              -1, &stmt, NULL);
  // clang-format on
  if(!stmt) return;

  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1),
       (uint32_t)sqlite3_column_int(stmt, 2));
  sqlite3_finalize(stmt);
}

void dt_metadata_repository_cleanup(void)
{
  /* Nothing cached any more: every statement in this file is prepared and finalised per
   * call. Kept because the connection's close order calls every repository's cleanup. */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
