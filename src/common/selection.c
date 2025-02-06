/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/signal.h"
#include "gui/gtk.h"
#include "views/view.h"

typedef struct dt_selection_t
{
  /* the collection clone used for selection */
  const dt_collection_t *collection;

  /* length of selection. 0 means no selection, -1 means it needs to be updated */
  uint32_t length;

  /* this stores the last single clicked image id indicating
     the start of a selection range */
  uint32_t last_single_id;

  /* GList of ids of all images in selection */
  GList *ids;
} dt_selection_t;

const dt_collection_t *dt_selection_get_collection(struct dt_selection_t *selection)
{
  return selection->collection;
}

int dt_selection_get_first_id(struct dt_selection_t *selection)
{
  return selection->last_single_id;
}

/* updates the internal collection of an selection */
static void _selection_update_collection(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, uint32_t next,
                                         gpointer user_data);

static void _reset_ids_list(dt_selection_t *selection)
{
  g_list_free(g_steal_pointer(&selection->ids));
  selection->ids = NULL;
  selection->length = 0;
  selection->last_single_id = -1;
}

static void _update_last_ids(dt_selection_t *selection)
{
  GList *last = g_list_last(selection->ids);
  if(last)
    selection->last_single_id = GPOINTER_TO_INT(last->data);
  else
    selection->last_single_id = -1;
}

static void _update_ids_list(dt_selection_t *selection)
{
  // Update the cache
  _reset_ids_list(selection);
  selection->ids = dt_collection_get_selected(darktable.collection, -1);
  selection->length = g_list_length(selection->ids);
  _update_last_ids(selection);
}

static void _remove_id_link(dt_selection_t *selection, uint32_t imgid)
{
  GList *link = g_list_find(selection->ids, GINT_TO_POINTER(imgid));
  if(link)
  {
    selection->ids = g_list_delete_link(selection->ids, link);
    --selection->length;
  }
  _update_last_ids(selection);
}

static void _add_id_link(dt_selection_t *selection, uint32_t imgid)
{
  selection->ids = g_list_append(selection->ids, GINT_TO_POINTER(imgid));
  ++selection->length;
  selection->last_single_id = imgid;
}

GList *dt_selection_get_list(struct dt_selection_t *selection)
{
  if(!selection->ids) _update_ids_list(selection);

  return selection->ids;
}

int dt_selection_get_length(struct dt_selection_t *selection)
{
  if(!selection) return 0;
  if(!selection->ids) _update_ids_list(selection);
  if(!selection->ids) return 0;

  return selection->length;
}


static void _selection_select(dt_selection_t *selection, uint32_t imgid)
{
  if(imgid != -1)
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      const uint32_t img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      gchar *query = NULL;
      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id
         || !selection->collection)
      {
        query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
      }
      else
      {
        // clang-format off
        query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images"
                                "  SELECT id"
                                "  FROM main.images "
                                "  WHERE group_id = %d AND id IN (%s)",
                                img_group_id, dt_collection_get_query_no_group(selection->collection));
        // clang-format on
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }
}

void _selection_deselect(dt_selection_t *selection, uint32_t imgid)
{
  if(imgid != -1)
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      const uint32_t img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      gchar *query = NULL;
      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id)
      {
        query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid = %d", imgid);
      }
      else
      {
        // clang-format off
        query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid IN "
                                "(SELECT id FROM main.images WHERE group_id = %d)",
                                img_group_id);
        // clang-format on
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }
}

void _selection_update_collection(gpointer instance, dt_collection_change_t query_change,
                                  dt_collection_properties_t changed_property, gpointer imgs, uint32_t next,
                                  gpointer user_data)
{
  dt_selection_t *selection = (dt_selection_t *)user_data;

  /* free previous collection copy if any */
  if(selection->collection) dt_collection_free(selection->collection);

  /* create a new fresh copy of darktable collection */
  selection->collection = dt_collection_new(darktable.collection);

  /* remove limit part of local collection */
  dt_collection_set_query_flags(selection->collection, (dt_collection_get_query_flags(selection->collection)
                                                        & (~(COLLECTION_QUERY_USE_LIMIT))));
  dt_collection_update(selection->collection);
}

void dt_push_selection()
{
  // Backup current selection
  if(!darktable.gui->selection_stacked)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.selected_backup", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO memory.selected_backup"
                                                         " SELECT * FROM main.selected_images", NULL, NULL, NULL);
    darktable.gui->selection_stacked = TRUE;
  }
  else
  {
    // If we already have a backup, don't do anything.
    // TODO: maybe store a full stack with history index so we can re-select back in time ?
    // In that case, make darktable.gui->selection_stacked a uint32_t, increment it on each push
    // and store it too in a column of the memory.selected_backup table for each row.
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_pop_selection()
{
  // Restore current selection
  if(darktable.gui->selection_stacked)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO main.selected_images"
                                                         " SELECT * FROM memory.selected_backup", NULL, NULL, NULL);
    darktable.gui->selection_stacked = FALSE;
  }
  else
  {
    // If we don't have a backup, nothing to pop
  }


  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

const dt_selection_t *dt_selection_new()
{
  dt_selection_t *s = g_malloc0(sizeof(dt_selection_t));

  /* initialize the collection copy */
  _selection_update_collection(NULL, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL, -1, (gpointer)s);

  /* populate our local cache */
  _update_ids_list(s);

  /* setup signal handler for darktable collection update
   to update the uint32_ternal collection of the selection */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_selection_update_collection), (gpointer)s);

  return s;
}

void dt_selection_free(dt_selection_t *selection)
{
  g_list_free(selection->ids);
  g_free(selection);
}

void dt_selection_invert(dt_selection_t *selection)
{
  if(!selection->collection) return;

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query(selection->collection));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.tmp_selection SELECT imgid FROM main.selected_images", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.selected_images WHERE imgid IN (SELECT imgid FROM memory.tmp_selection)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);

  g_free(fullq);

  _update_ids_list(selection);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_clear(dt_selection_t *selection)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);

  _reset_ids_list(selection);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_select(dt_selection_t *selection, uint32_t imgid)
{
  if(imgid == -1) return;
  _selection_select(selection, imgid);
  _add_id_link(selection, imgid);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_deselect(dt_selection_t *selection, uint32_t imgid)
{
  if(imgid == -1) return;
  _selection_deselect(selection, imgid);
  _remove_id_link(selection, imgid);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_select_single(dt_selection_t *selection, uint32_t imgid)
{
  if(imgid == -1) return;
  dt_selection_clear(selection);
  dt_selection_select(selection, imgid);
}

void dt_selection_toggle(dt_selection_t *selection, uint32_t imgid)
{
  sqlite3_stmt *stmt;
  gboolean exists = FALSE;

  if(imgid == -1) return;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) exists = TRUE;

  sqlite3_finalize(stmt);

  if(exists)
    dt_selection_deselect(selection, imgid);
  else
    dt_selection_select(selection, imgid);
}

void dt_selection_select_all(dt_selection_t *selection)
{
  if(!selection->collection) return;

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  _update_ids_list(selection);

  g_free(fullq);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_select_range(dt_selection_t *selection, uint32_t imgid)
{
  if(!selection->collection) return;

  // selecting a range requires at least one image to be selected already
  if(!dt_collection_get_selected_count(darktable.collection)) return;

  /* get start and end rows for range selection */
  sqlite3_stmt *stmt;
  uint32_t rc = 0;
  uint32_t sr = -1, er = -1;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), dt_collection_get_query_no_group(selection->collection),
                              -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t id = sqlite3_column_int(stmt, 0);
    if(id == selection->last_single_id) sr = rc;

    if(id == imgid) er = rc;

    if(sr != -1 && er != -1) break;

    rc++;
  }
  sqlite3_finalize(stmt);

  // if imgid not in collection, nothing to do
  if(er == -1) return;

  // if last_single_id not in collection, we either use last selected image or first collected one
  uint32_t srid = selection->last_single_id;
  if(sr == -1)
  {
    sr = 0;
    srid = -1;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT m.rowid, m.imgid FROM memory.collected_images AS m, main.selected_images AS s"
        " WHERE m.imgid=s.imgid"
        " ORDER BY m.rowid DESC"
        " LIMIT 1",
        -1, &stmt, NULL);
    // clang-format on
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sr = sqlite3_column_int(stmt, 0);
      srid = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
  }

  /* select the images in range from start to end */
  const uint32_t old_flags = dt_collection_get_query_flags(selection->collection);

  /* use the limit to select range of images */
  dt_collection_set_query_flags(selection->collection, (old_flags | COLLECTION_QUERY_USE_LIMIT));

  dt_collection_update(selection->collection);

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), fullq, -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, MIN(sr, er));
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, (MAX(sr, er) - MIN(sr, er)) + 1);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* reset filter */
  dt_collection_set_query_flags(selection->collection, old_flags);
  dt_collection_update(selection->collection);

  // The logic above doesn't handle groups, so explicitly select the beginning and end to make sure those are selected properly
  dt_selection_select(selection, srid);
  dt_selection_select(selection, imgid);

  _update_ids_list(selection);

  g_free(fullq);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_select_filmroll(dt_selection_t *selection)
{
  // clear at start, too, just to be sure:
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.tmp_selection SELECT imgid FROM main.selected_images", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE film_id IN "
                        "(SELECT film_id FROM main.images AS a JOIN memory.tmp_selection AS "
                        "b ON a.id = b.imgid)",
                        NULL, NULL, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);

  dt_collection_update(selection->collection);

  _update_ids_list(selection);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

void dt_selection_select_unaltered(dt_selection_t *selection)
{
  if(!selection->collection) return;

  /* set unaltered collection filter and update query */
  const uint32_t old_filter_flags = dt_collection_get_filter_flags(selection->collection);
  dt_collection_set_filter_flags(selection->collection, (dt_collection_get_filter_flags(selection->collection)
                                                         | COLLECTION_FILTER_UNALTERED));
  dt_collection_update(selection->collection);

  char *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                dt_collection_get_query(selection->collection));

  /* clean current selection and select unaltered images */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  /* restore collection filter and update query */
  dt_collection_set_filter_flags(selection->collection, old_filter_flags);
  dt_collection_update(selection->collection);

  g_free(fullq);

  _update_ids_list(selection);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}


void dt_selection_select_list(struct dt_selection_t *selection, GList *list)
{
  if(!list) return;
  while(list)
  {
    uint32_t count = 1;
    uint32_t imgid = GPOINTER_TO_INT(list->data);
    selection->last_single_id = imgid;
    gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
    list = g_list_next(list);
    while(list && count < 400)
    {
      imgid = GPOINTER_TO_INT(list->data);
      count++;
      _add_id_link(selection, imgid);
      query = dt_util_dstrcat(query, ",(%d)", imgid);
      list = g_list_next(list);
    }
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

    g_free(query);
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

gchar *dt_selection_ids_to_string(struct dt_selection_t *selection)
{
  // In case we didn't already init the selection
  if(!selection->ids) _update_ids_list(selection);

  // There is no selection even after init, abort
  if(!selection->ids) return NULL;

  gchar **ids = g_malloc0_n(selection->length + 1, 9 * sizeof(char *));
  uint32_t i = 0;

  // Build the array of uint32_tegers as charaters
  for(GList *id = g_list_first(selection->ids); id; id = g_list_next(id))
  {
    ids[i] = g_strdup_printf("%i", GPOINTER_TO_INT(id->data));
    i++;
  }

  // ids needs to be null-terminated for strjoinv
  ids[i] = NULL;

  // Concatenate with blank comas within
  gchar *result = g_strjoinv(",", ids);

  g_strfreev(ids);

  return result;
}

gboolean dt_selection_is_id_selected(struct dt_selection_t *selection, uint32_t imgid)
{
  if(!selection) return FALSE;
  if(!selection->ids) _update_ids_list(selection);
  if(!selection->ids) return FALSE;
  return (g_list_find(selection->ids, GINT_TO_POINTER(imgid)) != NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
