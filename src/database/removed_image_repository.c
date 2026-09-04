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

#include "database/removed_image_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <sqlite3.h>

/* Every table a removed image owns rows in, listed in the order a restore has to put them
 * back: the film roll before the image whose film_id references it, the image before
 * everything keyed on its id. `filter` is how the table is matched to one image while
 * staging -- a film roll is reached through the image rather than by a column of its own,
 * and `meta_data` spells the image column `id` where the rest spell it `imgid`.
 *
 * `child_key` names that column again for the tables the restore must CLEAR before it
 * copies back, and is NULL for the two that are not the image's children -- its film roll,
 * which other images share, and the image row itself, which a restore must never delete.
 * Clearing matters because the schema does not cascade uniformly: `module_order`,
 * `color_labels` and `meta_data` carry no foreign key on images(id), so their rows outlive
 * the removal, and `color_labels` has no unique constraint either -- copying the staged row
 * on top of a survivor would duplicate it, and duplicate it again on every further
 * remove/undo cycle. Clearing first makes the restore idempotent and independent of which
 * tables the schema happens to cascade. */
typedef struct _removed_table_t
{
  const char *name;
  const char *filter;
  const char *child_key;
} _removed_table_t;

static const _removed_table_t _removed_tables[]
    = { { "film_rolls",    "id IN (SELECT film_id FROM main.images WHERE id = ?2)", NULL },
        { "images",        "id = ?2",     NULL },
        { "history",       "imgid = ?2",  "imgid" },
        { "masks_history", "imgid = ?2",  "imgid" },
        { "module_order",  "imgid = ?2",  "imgid" },
        { "tagged_images", "imgid = ?2",  "imgid" },
        { "color_labels",  "imgid = ?2",  "imgid" },
        { "meta_data",     "id = ?2",     "id" },
        { "history_hash",  "imgid = ?2",  "imgid" } };

/* The columns of `memory.removed_<table>` as a comma-separated list, minus the two this
 * module prepends. Asking the database rather than restating them here is the whole reason
 * the twins are declared as `CREATE TABLE ... AS SELECT`: a schema migration then reaches
 * the copies below without an edit in this file. */
static gchar *_columns_of(const char *table)
{
  gchar *pragma = g_strdup_printf("PRAGMA memory.table_info('removed_%s')", table);
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), pragma, -1, &stmt, NULL);
  dt_free(pragma);
  if(IS_NULL_PTR(stmt)) return NULL;

  GString *columns = g_string_new(NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    if(!g_strcmp0(name, "snap_id") || !g_strcmp0(name, "undo_imgid")) continue;
    if(columns->len > 0) g_string_append_c(columns, ',');
    g_string_append(columns, name);
  }
  sqlite3_finalize(stmt);

  // an empty list means the twin table is missing, which no query below can survive
  const gboolean empty = (columns->len == 0);
  return g_string_free(columns, empty);
}

/* Run one `?1 = snap_id, ?2 = imgid` statement to completion. Every query in this file has
 * that shape, which is what makes the file short. */
static gboolean _run(const char *query, const int snap_id, const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return FALSE;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

int dt_removed_image_repository_next_id(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT MAX(snap_id) FROM memory.removed_images WHERE undo_imgid = ?1",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return 0;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  int snap_id = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) snap_id = sqlite3_column_int(stmt, 0) + 1;
  sqlite3_finalize(stmt);
  return snap_id;
}

gboolean dt_removed_image_repository_create(const int snap_id, const int32_t imgid)
{
  gboolean all_ok = TRUE;

  dt_database_start_transaction();

  for(gsize i = 0; i < G_N_ELEMENTS(_removed_tables) && all_ok; i++)
  {
    gchar *columns = _columns_of(_removed_tables[i].name);
    if(IS_NULL_PTR(columns))
    {
      all_ok = FALSE;
      break;
    }

    gchar *query = g_strdup_printf("INSERT INTO memory.removed_%s (snap_id, undo_imgid, %s)"
                                   "  SELECT ?1, ?2, %s FROM main.%s WHERE %s",
                                   _removed_tables[i].name, columns, columns,
                                   _removed_tables[i].name, _removed_tables[i].filter);
    all_ok = _run(query, snap_id, imgid);
    dt_free(query);
    dt_free(columns);
  }

  /* Who else is in this image's group. dt_grouping_remove_from_group() hands the group to a
   * new leader on the way out, rewriting the group_id of images that are not being removed
   * at all -- so that rewrite lives in no table above, and undoing the removal has to undo
   * it too. */
  all_ok = all_ok && _run("INSERT INTO memory.removed_groups (snap_id, undo_imgid, id, group_id)"
                          "  SELECT ?1, ?2, id, group_id FROM main.images"
                          "  WHERE group_id = (SELECT group_id FROM main.images WHERE id = ?2)",
                          snap_id, imgid);

  if(all_ok)
    dt_database_release_transaction();
  else
    dt_database_rollback_transaction();

  return all_ok;
}

gboolean dt_removed_image_repository_restore(const int snap_id, const int32_t imgid)
{
  dt_database_start_transaction();

  /* A whole film roll removed in one go comes back one image per undo record, in whatever
   * order the undo list holds, so an image's group leader is regularly still missing when
   * its own row goes back in. Deferring the foreign keys to the commit is what lets the
   * restore run in table order; SQLite clears the pragma at the commit on its own. */
  sqlite3_exec(dt_database_get_sqlite3_global(), "PRAGMA defer_foreign_keys = ON", NULL, NULL, NULL);

  gboolean all_ok = TRUE;

  for(gsize i = 0; i < G_N_ELEMENTS(_removed_tables) && all_ok; i++)
  {
    gchar *columns = _columns_of(_removed_tables[i].name);
    if(IS_NULL_PTR(columns))
    {
      all_ok = FALSE;
      break;
    }

    // whatever survived the removal in this table goes, so the staged copy is the only one
    if(!IS_NULL_PTR(_removed_tables[i].child_key))
    {
      gchar *clear = g_strdup_printf("DELETE FROM main.%s WHERE %s = ?2",
                                     _removed_tables[i].name, _removed_tables[i].child_key);
      all_ok = _run(clear, snap_id, imgid);
      dt_free(clear);
    }

    /* OR IGNORE because the film roll is regularly still there: only the roll's LAST image
     * takes it down, and every image of the roll staged a copy of it. */
    gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.%s (%s)"
                                   "  SELECT %s FROM memory.removed_%s"
                                   "  WHERE snap_id = ?1 AND undo_imgid = ?2",
                                   _removed_tables[i].name, columns, columns,
                                   _removed_tables[i].name);
    all_ok = all_ok && _run(query, snap_id, imgid);
    dt_free(query);
    dt_free(columns);
  }

  // give the group its leader back -- those rows were never deleted, so this is an update
  all_ok = all_ok && _run("UPDATE main.images SET group_id ="
                          "  (SELECT g.group_id FROM memory.removed_groups AS g"
                          "    WHERE g.snap_id = ?1 AND g.undo_imgid = ?2 AND g.id = main.images.id)"
                          "  WHERE id IN (SELECT id FROM memory.removed_groups"
                          "               WHERE snap_id = ?1 AND undo_imgid = ?2)",
                          snap_id, imgid);

  /* Anything the previous statement pointed at an image that has not come back -- because
   * its own undo record has not been popped, or never will be -- is repointed at itself:
   * main.images.group_id has a foreign key on main.images.id and the commit would refuse it.
   * A group left split by a partial undo is the honest outcome, not something to invent a
   * leader for. */
  all_ok = all_ok && _run("UPDATE main.images SET group_id = id"
                          "  WHERE id IN (SELECT id FROM memory.removed_groups"
                          "               WHERE snap_id = ?1 AND undo_imgid = ?2)"
                          "    AND group_id NOT IN (SELECT id FROM main.images)",
                          snap_id, imgid);

  if(all_ok)
    dt_database_release_transaction();
  else
    dt_database_rollback_transaction();

  return all_ok;
}

void dt_removed_image_repository_clear(const int snap_id, const int32_t imgid)
{
  for(gsize i = 0; i < G_N_ELEMENTS(_removed_tables); i++)
  {
    gchar *query = g_strdup_printf("DELETE FROM memory.removed_%s WHERE snap_id = ?1 AND undo_imgid = ?2",
                                   _removed_tables[i].name);
    _run(query, snap_id, imgid);
    dt_free(query);
  }

  _run("DELETE FROM memory.removed_groups WHERE snap_id = ?1 AND undo_imgid = ?2", snap_id, imgid);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
