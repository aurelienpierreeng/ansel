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

  // Row order, NOT reverse-row: dt_map_location_get_locations_by_path() walks this list and
  // prepends into its own, and that second prepend is what reproduces the single prepend the
  // original did straight off the cursor. Returning reverse-row here would flip the result.
  return g_list_reverse(tags);
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
  sqlite3_stmt *stmt = NULL;
  /* `%%` is two wildcards, not a printf escape -- this query is never format-expanded.
   * Two consecutive `%` match exactly what one does, so it is redundant rather than
   * wrong, and it is kept verbatim so the text is unchanged. */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.darktable_tags (tagid)"
                              " SELECT DISTINCT id"
                              " FROM data.tags"
                              " WHERE name LIKE 'darktable|%%'",
                              -1, &stmt, NULL);
  // clang-format on
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
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
  // built in reverse order by prepending, so un-reverse it -- both callers returned
  // row order and their consumers walk the list in it
  return g_list_reverse(ids);
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


/* ---------------------------------------------------------------------------------------
 *  Attached-tag listings
 * ------------------------------------------------------------------------------------- */

/* Four queries for one question, because the SQL genuinely differs on two axes: one
 * image (bind an id) versus the selection (join `main.selected_images`), and whether
 * internal tags are excluded. Indexed [selection][ignore_internal] so the pair of booleans
 * picks the query rather than four copies of the surrounding code doing it. Prepared per
 * call: a cached statement here would be shared GUI-thread/worker-thread state with no
 * lock, and the tag panel and batch tag jobs both come through this. */
static const char *const _attached_query[2][2] = {
  /* one image */
  { "SELECT DISTINCT I.tagid, T.name, T.flags, T.synonyms,"
    " COUNT(DISTINCT I.imgid) AS inb"
    " FROM main.tagged_images AS I"
    " JOIN data.tags AS T ON T.id = I.tagid"
    " WHERE I.imgid = ?1"
    " GROUP BY I.tagid "
    " ORDER by T.name",
    "SELECT DISTINCT I.tagid, T.name, T.flags, T.synonyms,"
    " COUNT(DISTINCT I.imgid) AS inb"
    " FROM main.tagged_images AS I"
    " JOIN data.tags AS T ON T.id = I.tagid"
    " WHERE I.imgid = ?1 AND T.id NOT IN memory.darktable_tags"
    " GROUP BY I.tagid "
    " ORDER by T.name" },
  /* the selection */
  { "SELECT DISTINCT I.tagid, T.name, T.flags, T.synonyms,"
    " COUNT(DISTINCT I.imgid) AS inb"
    " FROM main.tagged_images AS I"
    " JOIN data.tags AS T ON T.id = I.tagid"
    " JOIN main.selected_images AS S ON S.imgid = I.imgid"
    " GROUP BY I.tagid "
    " ORDER by T.name",
    "SELECT DISTINCT I.tagid, T.name, T.flags, T.synonyms,"
    " COUNT(DISTINCT I.imgid) AS inb"
    " FROM main.tagged_images AS I"
    " JOIN data.tags AS T ON T.id = I.tagid"
    " JOIN main.selected_images AS S ON S.imgid = I.imgid"
    " WHERE T.id NOT IN memory.darktable_tags"
    " GROUP BY I.tagid "
    " ORDER by T.name" },
};

/* Fill the fields that come from a row. `leave` and `select` are the caller's. */
static dt_tag_t *_tag_from_row(sqlite3_stmt *stmt, const gboolean with_count)
{
  dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
  if(IS_NULL_PTR(t)) return NULL;

  t->id = sqlite3_column_int(stmt, 0);
  t->tag = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  t->flags = sqlite3_column_int(stmt, 2);
  t->synonym = g_strdup((const char *)sqlite3_column_text(stmt, 3));
  if(with_count) t->count = sqlite3_column_int(stmt, 4);
  return t;
}

GList *dt_tag_repository_get_attached(const int32_t imgid, const gboolean ignore_internal)
{
  const int sel = (imgid > 0) ? 0 : 1;
  const int ign = ignore_internal ? 1 : 0;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), _attached_query[sel][ign],
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return NULL;
  if(sel == 0) DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = _tag_from_row(stmt, TRUE);
    if(t) tags = g_list_prepend(tags, t);
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(tags); // the ORDER BY is the point
}

GList *dt_tag_repository_get_attached_for_export(const int32_t imgid)
{
  if(!(imgid > 0)) return NULL;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT DISTINCT T.id, T.name, T.flags, T.synonyms"
                              " FROM data.tags AS T"
                              // tags attached to image(s), not dt tag, ordered by name
                              " JOIN (SELECT DISTINCT I.tagid, T.name"
                              "       FROM main.tagged_images AS I"
                              "       JOIN data.tags AS T ON T.id = I.tagid"
                              "       WHERE I.imgid = ?1 AND T.id NOT IN memory.darktable_tags"
                              "       ORDER by T.name) AS T1"
                              // keep also tags in the path to be able to check category in path
                              " ON T.id = T1.tagid"
                              "    OR (T.name = SUBSTR(T1.name, 1, LENGTH(T.name))"
                              "       AND SUBSTR(T1.name, LENGTH(T.name) + 1, 1) = '|')",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = _tag_from_row(stmt, FALSE);
    if(t) tags = g_list_prepend(tags, t);
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(tags);
}

GList *dt_tag_repository_get_ids_for_images(const char *imgid_list, const dt_tag_kind_t kind)
{
  if(IS_NULL_PTR(imgid_list)) return NULL;

  sqlite3_stmt *stmt = NULL;
  char query[256] = { 0 };
  /* The `IN (%s)` is the caller's id list and the trailing fragment selects the kind.
   * Both are composed here rather than bound because neither is a value: one is a list of
   * unknown length, the other a clause. */
  // clang-format off
  snprintf(query, sizeof(query), "SELECT DISTINCT T.id"
                                 "  FROM main.tagged_images AS I"
                                 "  JOIN data.tags T on T.id = I.tagid"
                                 "  WHERE I.imgid IN (%s) %s",
           imgid_list, kind == DT_TAG_KIND_ANY ? "" :
                       kind == DT_TAG_KIND_INTERNAL ? "AND T.id IN memory.darktable_tags" :
                                                      "AND NOT T.id IN memory.darktable_tags");
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    tags = g_list_prepend(tags, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  return tags; // prepend-only, as before
}

GList *dt_tag_repository_get_with_usage(const uint32_t nb_selected)
{
  sqlite3_stmt *stmt = NULL;

  /* Select tags that are similar to the keyword and are actually used to tag images*/
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.taglist (id, count)"
                              "  SELECT tagid, COUNT(*)"
                              "  FROM main.tagged_images"
                              "  GROUP BY tagid",
                              -1, &stmt, NULL);
  // clang-format on
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* Now put all the bits together */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT T.name, T.id, MT.count, CT.imgnb, T.flags, T.synonyms"
                              "  FROM data.tags T "
                              "  LEFT JOIN memory.taglist MT ON MT.id = T.id "
                              "  LEFT JOIN (SELECT tagid, COUNT(DISTINCT imgid) AS imgnb"
                              "             FROM main.tagged_images "
                              "             WHERE imgid IN (SELECT imgid FROM main.selected_images) GROUP BY tagid) AS CT "
                              "    ON CT.tagid = T.id"
                              "  WHERE T.id NOT IN memory.darktable_tags "
                              "  ORDER BY T.name ",
                              -1, &stmt, NULL);
  // clang-format on

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    if(t)
    {
      t->tag = g_strdup((const char *)sqlite3_column_text(stmt, 0));
      t->id = sqlite3_column_int(stmt, 1);
      t->count = sqlite3_column_int(stmt, 2);
      const uint32_t imgnb = sqlite3_column_int(stmt, 3);
      t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                  (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                  (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
      t->flags = sqlite3_column_int(stmt, 4);
      t->synonym = g_strdup((const char *)sqlite3_column_text(stmt, 5));
      tags = g_list_prepend(tags, t);
    }
  }
  sqlite3_finalize(stmt);

  /* memory.taglist is scratch shared by several listings, so it is emptied on the way out
   * rather than on the way in -- whoever runs next finds it clean. */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.taglist", NULL, NULL, NULL);

  return g_list_reverse(tags); // the ORDER BY is the point
}

GList *dt_tag_repository_get_collection_tags(void)
{
  sqlite3_stmt *stmt = NULL;
  /* Tags attached to at least one image of the current collection */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT DISTINCT T.name, T.id"
                              "  FROM data.tags T"
                              "  JOIN main.tagged_images TI ON TI.tagid = T.id"
                              "  WHERE TI.imgid IN (SELECT imgid FROM memory.collected_images)"
                              "    AND T.id NOT IN memory.darktable_tags"
                              "  ORDER BY T.name",
                              -1, &stmt, NULL);
  // clang-format on

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    if(t)
    {
      t->tag = g_strdup((const char *)sqlite3_column_text(stmt, 0));
      t->id = sqlite3_column_int(stmt, 1);
      tags = g_list_prepend(tags, t);
    }
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(tags); // the ORDER BY is the point
}

GList *dt_tag_repository_get_names_under(const int32_t imgid, const char *category)
{
  if(IS_NULL_PTR(category)) return NULL;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
          "SELECT DISTINCT T.name FROM main.tagged_images AS I "
          "INNER JOIN data.tags AS T "
          "ON T.id = I.tagid AND SUBSTR(T.name, 1, LENGTH(?2)) = ?2 "
          "WHERE I.imgid = ?1",
          -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, category, -1, SQLITE_TRANSIENT);

  GList *names = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    names = g_list_prepend(names, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);

  return g_list_reverse(names); // query order, which is what the caller walked
}

/* Only select tags that are equal or child to the one we are looking for once. */
static void _fill_similar_tags(const char *keyword)
{
  gchar *keyword_expr = g_strdup_printf("%s|", keyword);

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.similar_tags (tagid)"
                              "  SELECT id"
                              "    FROM data.tags"
                              "    WHERE name = ?1 OR SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, keyword, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, keyword_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_free(keyword_expr);
}

static void _clear_similar_tags(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.similar_tags",
                        NULL, NULL, NULL);
}

/* Run a no-parameter query and return its first integer column. */
static int _scalar(const char *query)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  const int v = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return v;
}

void dt_tag_repository_count_similar(const char *keyword, int *tag_count, int *img_count)
{
  if(IS_NULL_PTR(tag_count) || IS_NULL_PTR(img_count)) return;
  *tag_count = 0;
  *img_count = 0;
  if(IS_NULL_PTR(keyword)) return;

  _fill_similar_tags(keyword);

  *tag_count = _scalar("SELECT COUNT(DISTINCT tagid) FROM memory.similar_tags");
  // clang-format off
  *img_count = _scalar("SELECT COUNT(DISTINCT ti.imgid)"
                       "  FROM main.tagged_images AS ti "
                       "  JOIN memory.similar_tags AS st"
                       "    ON st.tagid = ti.tagid");
  // clang-format on

  _clear_similar_tags();
}

void dt_tag_repository_get_similar(const char *keyword, GList **tags, GList **imgids)
{
  if(IS_NULL_PTR(keyword) || IS_NULL_PTR(tags) || IS_NULL_PTR(imgids)) return;

  _fill_similar_tags(keyword);

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT ST.tagid, T.name"
                              " FROM memory.similar_tags ST"
                              " JOIN data.tags T"
                              "   ON T.id = ST.tagid ",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    if(t)
    {
      t->id = sqlite3_column_int(stmt, 0);
      t->tag = g_strdup((const char *)sqlite3_column_text(stmt, 1));
      *tags = g_list_append(*tags, t);
    }
  }
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT DISTINCT ti.imgid"
                              " FROM main.tagged_images AS ti"
                              " JOIN memory.similar_tags AS st"
                              "   ON st.tagid = ti.tagid",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
    *imgids = g_list_append(*imgids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  _clear_similar_tags();
}


GList *dt_tag_repository_get_suggestions(const uint32_t nb_selected, const int confidence,
                                         const char *recent_tags, const int nb_recent)
{
  sqlite3_stmt *stmt = NULL;

  // get attached tags with how many times they are attached in db and on selected images
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.taglist (id, count, count2)"
                              "  SELECT S.tagid, COUNT(imgid) AS count,"
                              "    CASE WHEN count2 IS NULL THEN 0 ELSE count2 END AS count2"
                              "  FROM main.tagged_images AS S"
                              "  LEFT JOIN ("
                              "    SELECT tagid, COUNT(imgid) AS count2"
                              "    FROM main.tagged_images"
                              "    WHERE imgid IN main.selected_images"
                              "    GROUP BY tagid) AS at"
                              "  ON at.tagid = S.tagid"
                              "  WHERE S.tagid NOT IN memory.darktable_tags"
                              "  GROUP BY S.tagid",
                              -1, &stmt, NULL);
  // clang-format on
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  char *query = NULL;
  if(confidence != 100)
    query = g_strdup_printf("SELECT td.name, tagid2, t21.count, t21.count2,"
                            " td.flags, td.synonyms FROM ("
                            // get tags with required confidence
                            "  SELECT DISTINCT tagid2 FROM ("
                            "    SELECT tagid2 FROM ("
                            // get how many times (tag1, tag2) are attached together (c12)
                            "      SELECT tagid1, tagid2, count(*) AS c12"
                            "      FROM ("
                            "        SELECT DISTINCT tagid AS tagid1, imgid FROM main.tagged_images"
                            "        JOIN memory.taglist AS t00"
                            "        ON t00.id = tagid1 AND t00.count2 > 0) AS t1"
                            "      JOIN ("
                            "        SELECT DISTINCT tagid AS tagid2, imgid FROM main.tagged_images"
                            "        WHERE tagid NOT IN memory.darktable_tags) AS t2"
                            "      ON t2.imgid = t1.imgid AND tagid1 != tagid2"
                            "      GROUP BY tagid1, tagid2)"
                            "    JOIN memory.taglist AS t01"
                            "    ON t01.id = tagid1"
                            "    JOIN memory.taglist AS t02"
                            "    ON t02.id = tagid2"
                            // filter by confidence and reject tags attached on all selected images
                            "    WHERE (t01.count-t01.count2) != 0"
                            "      AND (100 * c12 / (t01.count-t01.count2) >= %d)"
                            "      AND t02.count2 != %d) "
                            "  UNION"
                            // get recent list tags
                            "  SELECT * FROM ("
                            "    SELECT tn.id AS tagid2 FROM data.tags AS tn"
                            "    JOIN memory.taglist AS t02"
                            "    ON t02.id = tn.id"
                            "    WHERE tn.name IN (\'%s\')"
                            // reject tags attached on all selected images and keep the required number
                            "      AND t02.count2 != %d LIMIT %d)) "
                            "LEFT JOIN memory.taglist AS t21 "
                            "ON t21.id = tagid2 "
                            "LEFT JOIN data.tags as td ON td.id = tagid2 ",
                            confidence, nb_selected, recent_tags, nb_selected, nb_recent);
    // clang-format on
  else
    query = g_strdup_printf("SELECT tn.name, tn.id, count, count2,"
                            "  tn.flags, tn.synonyms "
                            // get recent list tags
                            "FROM data.tags AS tn "
                            "JOIN memory.taglist AS t02 "
                            "ON t02.id = tn.id "
                            "WHERE tn.name IN (\'%s\')"
                            // reject tags attached on all selected images and keep the required number
                            "  AND t02.count2 != %d LIMIT %d",
                            recent_tags, nb_selected, nb_recent);
    // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query,
                              -1, &stmt, NULL);

  GList *tags = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    if(t)
    {
      t->tag = g_strdup((const char *)sqlite3_column_text(stmt, 0));
      t->id = sqlite3_column_int(stmt, 1);
      t->count = sqlite3_column_int(stmt, 2);
      const uint32_t imgnb = sqlite3_column_int(stmt, 3);
      t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                  (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                  (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
      t->flags = sqlite3_column_int(stmt, 4);
      t->synonym = g_strdup((const char *)sqlite3_column_text(stmt, 5));
      tags = g_list_prepend(tags, t);
    }
  }

  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.taglist", NULL, NULL, NULL);
  dt_free(query);

  return g_list_reverse(tags); // query order, which is what the caller appended in
}

gboolean dt_tag_repository_attach(const guint tagid, const int32_t imgid)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO main.tagged_images (tagid, imgid, position)"
                              "  VALUES (?1, ?2,"
                              "    (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000) + (1 << 32)"
                              "      FROM main.tagged_images))",
                               -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

void dt_tag_repository_get_agreement(GList *imgids, gboolean *same_tags, gboolean *same_categories)
{
  gboolean tags_agree = TRUE;
  gboolean categories_agree = TRUE;

  const int count = (int)g_list_length(imgids);
  char *set = NULL;
  if(count > 0)
  {
    GString *ids = g_string_new(NULL);
    for(GList *l = imgids; l; l = g_list_next(l))
    {
      if(l != imgids) g_string_append_c(ids, ',');
      g_string_append_printf(ids, "%d", GPOINTER_TO_INT(l->data));
    }
    set = g_string_free(ids, FALSE);
  }

  if(set)
  {
    // clang-format off
    char *query = g_strdup_printf("SELECT flags, COUNT(DISTINCT imgid) "
                                  "FROM main.tagged_images "
                                  "JOIN data.tags "
                                  "ON data.tags.id = main.tagged_images.tagid AND name NOT LIKE 'darktable|%%' "
                                  "WHERE imgid in (%s) GROUP BY tagid", set);
    // clang-format on
    sqlite3_stmt *stmt = NULL;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
    dt_free(query);

    if(stmt)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        if(sqlite3_column_int(stmt, 0) & DT_TF_CATEGORY)
          categories_agree &= (sqlite3_column_int(stmt, 1) == count);
        else
          tags_agree &= (sqlite3_column_int(stmt, 1) == count);
      }
      sqlite3_finalize(stmt);
    }
    dt_free(set);
  }

  if(same_tags) *same_tags = tags_agree;
  if(same_categories) *same_categories = categories_agree;
}

void dt_tag_repository_cleanup(void)
{
  /* Nothing cached any more: every statement in this file is prepared and finalised per
   * call. Kept because the connection's close order calls every repository's cleanup. */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
