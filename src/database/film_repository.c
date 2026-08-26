/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2025-2026 Aurélien PIERRE.

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

#include "database/film_repository.h"
#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

int32_t dt_film_repository_find_by_folder(const char *folder)
{
  if(IS_NULL_PTR(folder)) return -1;

  int32_t filmroll_id = -1;
  sqlite3_stmt *stmt;
  // Hoisted out of the macro call: a preprocessing directive inside the argument list of a
  // function-like macro is undefined behaviour (C11 6.10.3p11). GCC and Clang accept it,
  // which is exactly why it survives until a toolchain bump.
#ifdef _WIN32
  // Windows paths are matched case-insensitively, which LIKE gives us and = does not.
  const char *query = "SELECT id FROM main.film_rolls WHERE folder LIKE ?1";
#else
  const char *query = "SELECT id FROM main.film_rolls WHERE folder = ?1";
#endif
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, folder, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) filmroll_id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return filmroll_id;
}

char *dt_film_repository_get_folder(const int32_t id)
{
  char *folder = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder"
                              " FROM main.film_rolls"
                              " WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    folder = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
  return folder;
}

gboolean dt_film_repository_insert(const char *folder)
{
  if(IS_NULL_PTR(folder)) return FALSE;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO main.film_rolls (id, access_timestamp, folder)"
                              "  VALUES (NULL, strftime('%s', 'now'), ?1)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, folder, -1, SQLITE_STATIC);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_film_repository_touch_access(const int32_t id)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.film_rolls"
                              " SET access_timestamp = strftime('%s', 'now')"
                              " WHERE id = ?1", -1, &stmt,
                              NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_film_repository_set_folder(const int32_t id, const char *folder)
{
  if(IS_NULL_PTR(folder)) return FALSE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.film_rolls SET folder=?1 WHERE id=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, folder, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_film_repository_delete(const int32_t id)
{
  sqlite3_stmt *stmt;
  // due to foreign keys, all images with references to the film roll are deleted,
  // and likewise all entries with references to those images
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.film_rolls WHERE id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_film_repository_has_images(const int32_t id)
{
  gboolean has = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW) has = TRUE;
  sqlite3_finalize(stmt);
  return has;
}

GList *dt_film_repository_get_image_ids(const int32_t filmid)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    result = g_list_prepend(result, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

void dt_film_repository_foreach(dt_film_repository_row_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder FROM main.film_rolls",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
}

void dt_film_repository_foreach_empty(dt_film_repository_row_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id,folder"
                              " FROM main.film_rolls AS B"
                              " WHERE (SELECT COUNT(*)"
                              "        FROM main.images AS A"
                              "        WHERE A.film_id=B.id) = 0",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
}

void dt_film_repository_foreach_under(const char *path, dt_film_repository_row_cb cb,
                                      void *user_data)
{
  if(IS_NULL_PTR(path) || IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  gchar *like = g_strdup_printf("%s%%", path);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder FROM main.film_rolls WHERE folder LIKE ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, like, -1, SQLITE_TRANSIENT);
  g_free(like);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
}

void dt_film_repository_folder_status_clear(void)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM memory.film_folder",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_film_repository_folder_status_set(const int32_t id, const gboolean present)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.film_folder (id, status) "
                              "VALUES (?1, ?2)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, present ? 1 : 0);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
