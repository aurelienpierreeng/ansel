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


/* ---------------------------------------------------------------------------------------
 *  Identity and lifecycle
 * ------------------------------------------------------------------------------------- */

/* Run a one-text-parameter query and return the first integer column, or 0. */
static guint _first_id_for_text(const char *query, const char *value)
{
  if(IS_NULL_PTR(value)) return 0;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, value, -1, SQLITE_TRANSIENT);

  guint id = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return id;
}

/* Run a one-integer-parameter statement to completion. */
static void _run_for_id(const char *query, const guint id)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

guint dt_tag_repository_find_by_name(const char *name)
{
  return _first_id_for_text("SELECT id FROM data.tags WHERE name = ?1", name);
}

guint dt_tag_repository_find_by_name_nocase(const char *name)
{
  // clang-format off
  return _first_id_for_text("SELECT T.id, T.flags FROM data.tags AS T "
                            "WHERE LOWER(T.name) = LOWER(?1)", name);
  // clang-format on
}

guint dt_tag_repository_insert(const char *name)
{
  if(IS_NULL_PTR(name)) return 0;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO data.tags (id, name) VALUES (NULL, ?1)", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* Read the id back rather than taking sqlite3_last_insert_rowid(): that is what this
   * always did, and the two differ if anything else on this connection inserts in
   * between. */
  return dt_tag_repository_find_by_name(name);
}

gchar *dt_tag_repository_get_name(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT name FROM data.tags WHERE id= ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  gchar *name = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW) name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return name;
}

void dt_tag_repository_rename(const guint tagid, const char *new_name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.tags SET name = ?2 WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

int dt_tag_repository_count_attachments(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(*) FROM main.tagged_images WHERE tagid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  int count = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

void dt_tag_repository_delete(const guint tagid)
{
  _run_for_id("DELETE FROM data.tags WHERE id=?1", tagid);
  _run_for_id("DELETE FROM main.tagged_images WHERE tagid=?1", tagid);
  _run_for_id("DELETE FROM memory.darktable_tags WHERE tagid=?1", tagid);
}

void dt_tag_repository_delete_batch(const char *id_list)
{
  if(IS_NULL_PTR(id_list)) return;

  sqlite3_stmt *stmt = NULL;
  gchar *query = g_strdup_printf("DELETE FROM data.tags WHERE id IN (%s)", id_list);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);

  query = g_strdup_printf("DELETE FROM main.tagged_images WHERE tagid IN (%s)", id_list);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);
}

void dt_tag_repository_mark_internal(const guint tagid)
{
  _run_for_id("INSERT INTO memory.darktable_tags (tagid) VALUES (?1)", tagid);
}

void dt_tag_repository_rebuild_internal(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.darktable_tags",
                        NULL, NULL, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "INSERT INTO memory.darktable_tags (tagid)"
                        " SELECT DISTINCT id FROM data.tags WHERE name LIKE 'darktable|%'",
                        NULL, NULL, NULL);
  // clang-format on
}

/* ---------------------------------------------------------------------------------------
 *  Flags and synonyms
 * ------------------------------------------------------------------------------------- */

gint dt_tag_repository_get_flags(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT flags FROM data.tags WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  gint flags = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) flags = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return flags;
}

void dt_tag_repository_set_flags(const guint tagid, const gint flags)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.tags SET flags = ?2 WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, flags);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_tag_repository_update_flags(const guint tagid, const gint set, const gint keep_mask)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.tags SET flags = (IFNULL(flags, 0) & ?3) | ?2 WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, set);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, keep_mask);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gchar *dt_tag_repository_get_synonyms(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT synonyms FROM data.tags WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  gchar *synonyms = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    synonyms = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return synonyms;
}

void dt_tag_repository_set_synonyms(const guint tagid, const char *synonyms)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.tags SET synonyms = ?2 WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, synonyms, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

/* ---------------------------------------------------------------------------------------
 *  Attachments
 * ------------------------------------------------------------------------------------- */

gboolean dt_tag_repository_is_attached(const guint tagid, const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT imgid FROM main.tagged_images WHERE imgid = ?1 AND tagid = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);

  const gboolean attached = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return attached;
}

static GList *_collect_imgids(sqlite3_stmt *stmt)
{
  GList *ids = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  return ids;
}

GList *dt_tag_repository_get_images(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT imgid FROM main.tagged_images WHERE tagid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  return _collect_imgids(stmt);
}

GList *dt_tag_repository_get_images_in_list(const guint tagid, const char *imgid_list)
{
  if(IS_NULL_PTR(imgid_list)) return NULL;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT imgid FROM main.tagged_images"
                                 " WHERE tagid = %d AND imgid IN (%s)", tagid, imgid_list);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  dt_free(query);
  return _collect_imgids(stmt);
}

uint32_t dt_tag_repository_count_distinct_images(const guint tagid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(DISTINCT imgid) AS imgnb FROM main.tagged_images WHERE tagid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  uint32_t count = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

void dt_tag_repository_detach_batch(const int32_t imgid, const char *tagid_list)
{
  if(imgid <= 0 || IS_NULL_PTR(tagid_list)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("DELETE FROM main.tagged_images WHERE imgid = %d AND tagid IN (%s)",
                                 imgid, tagid_list);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);
}

void dt_tag_repository_attach_batch(const char *values)
{
  if(IS_NULL_PTR(values)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("INSERT INTO main.tagged_images (imgid, tagid, position) VALUES %s",
                                 values);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_free(query);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
