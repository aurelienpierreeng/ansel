/*
    This file is part of darktable,
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2013 Jérémy Rosen.
    Copyright (C) 2013, 2018-2021 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 Rikard Öxler.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020, 2022 Philippe Weyland.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 solarer.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Luca Zulberti.
    
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
#include "common/selection.h"
#include "common/darktable.h"
#include "control/signal.h"
#include "gui/gtk.h"
#include "views/view.h"

static sqlite3_stmt *_selection_database_to_glist_stmt = NULL;

typedef struct dt_selection_t
{
  /* length of selection. 0 means no selection, -1 means it needs to be updated */
  uint32_t length;

  /* this stores the last single clicked image id indicating
     the start of a selection range */
  int32_t last_single_id;

  /* GList of ids of all images in selection */
  GList *ids;
} dt_selection_t;


// Signal the GUI that selection got changed and trigger a selected images counter update
static void _update_gui()
{
  dt_collection_hint_message(darktable.collection);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}


int32_t dt_selection_get_first_id(struct dt_selection_t *selection)
{
  return selection->last_single_id;
}


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

// Drop selected imgids that are not in the current collection
// WARNING: that doesn't take care of visible/unvisible image group members in GUI
static void _clean_missing_ids(dt_selection_t *selection)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.selected_images"
                        " WHERE imgid NOT IN"
                        " (SELECT imgid FROM memory.collected_images)", NULL, NULL, NULL);
}

// Unroll DB imgids to GList
static GList *_selection_database_to_glist(dt_selection_t *selection)
{
  GList *list = NULL;

  if(!_selection_database_to_glist_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT imgid FROM main.selected_images ORDER BY imgid DESC",
                                -1, &_selection_database_to_glist_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _selection_database_to_glist_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t imgid = sqlite3_column_int(stmt, 0);
    list = g_list_prepend(list, GINT_TO_POINTER(imgid));
  }

  // Don't reverse the GList since we ordered SQL rows by descending order
  // and prepend to the GList.
  return list;
}

 void dt_selection_reload_from_database_real(dt_selection_t *selection)
{
  _reset_ids_list(selection);
  selection->ids = _selection_database_to_glist(selection);
  selection->length = g_list_length(selection->ids);
  _update_last_ids(selection);
}

/* On collection change events, ensure the selection is only a subset of the current collection,
 * aka it doesn't contain dangling imgids that can't be found in current collection
 */
static void _selection_update_collection(gpointer instance, dt_collection_change_t query_change,
  dt_collection_properties_t changed_property, gpointer imgs, uint32_t next,
  dt_selection_t *selection)
{
  _clean_missing_ids(selection);
  dt_selection_reload_from_database(selection);
  _update_gui();
}


static void _remove_id_link(dt_selection_t *selection, int32_t imgid)
{
  GList *link = g_list_find(selection->ids, GINT_TO_POINTER(imgid));
  if(link)
  {
    selection->ids = g_list_delete_link(selection->ids, link);
    --selection->length;
  }
  _update_last_ids(selection);
}

static void _add_id_link(dt_selection_t *selection, int32_t imgid)
{
  if(!g_list_find(selection->ids, GINT_TO_POINTER(imgid)))
  {
    selection->ids = g_list_append(selection->ids, GINT_TO_POINTER(imgid));
    ++selection->length;
  }
  selection->last_single_id = imgid;
}

GList *dt_selection_get_list(struct dt_selection_t *selection)
{
  if(!selection->ids) return NULL;

  return g_list_copy(selection->ids);
}

int dt_selection_get_length(struct dt_selection_t *selection)
{
  if(!selection || !selection->ids) return 0;

  return selection->length;
}

static void _selection_select(dt_selection_t *selection, int32_t imgid)
{
  if(imgid < 0) return;

  gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
  g_free(query);
}

static void _selection_deselect(dt_selection_t *selection, int32_t imgid)
{
  if(imgid < 0) return;

  gchar *query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid = %d", imgid);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
  g_free(query);
}

void dt_selection_push(dt_selection_t *selection)
{
  // Backup current selection
  if(!darktable.gui->selection_stacked)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.selected_backup", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO memory.selected_backup"
                                                         " SELECT * FROM main.selected_images", NULL, NULL, NULL);
    darktable.gui->selection_stacked = TRUE;

    // Commit from DB to GList of imgids
    dt_selection_reload_from_database(selection);
  }

  _update_gui();
}

void dt_selection_pop(dt_selection_t *selection)
{
  // Restore current selection
  if(darktable.gui->selection_stacked)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO main.selected_images"
                                                         " SELECT * FROM memory.selected_backup", NULL, NULL, NULL);
    darktable.gui->selection_stacked = FALSE;

    // Commit from DB to GList of imgids
    dt_selection_reload_from_database(selection);
  }

  _update_gui();
}

dt_selection_t *dt_selection_new()
{
  dt_selection_t *selection = g_malloc0(sizeof(dt_selection_t));

  /* populate our local cache */
  dt_selection_reload_from_database(selection);

  /* setup signal handler for collection update to sanitize selection imgids */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_selection_update_collection), (gpointer)selection);

  return selection;
}

void dt_selection_free(dt_selection_t *selection)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_selection_update_collection),
                                     (gpointer)selection);
  g_list_free(selection->ids);
  if(_selection_database_to_glist_stmt)
  {
    sqlite3_finalize(_selection_database_to_glist_stmt);
    _selection_database_to_glist_stmt = NULL;
  }
  g_free(selection);
}

void dt_selection_clear(dt_selection_t *selection)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  _reset_ids_list(selection);
  _update_gui();
}

void dt_selection_select(dt_selection_t *selection, int32_t imgid)
{
  if(imgid == UNKNOWN_IMAGE) return;
  _selection_select(selection, imgid);
  _add_id_link(selection, imgid);
  _update_gui();
}

void dt_selection_deselect(dt_selection_t *selection, int32_t imgid)
{
  if(imgid == UNKNOWN_IMAGE) return;
  _selection_deselect(selection, imgid);
  _remove_id_link(selection, imgid);
  _update_gui();
}

void dt_selection_select_single(dt_selection_t *selection, int32_t imgid)
{
  if(imgid == UNKNOWN_IMAGE) return;
  dt_selection_clear(selection);
  dt_selection_select(selection, imgid);
}

void dt_selection_toggle(dt_selection_t *selection, int32_t imgid)
{
  if(imgid == UNKNOWN_IMAGE) return;

  if(g_list_find(selection->ids, GINT_TO_POINTER(imgid)))
    dt_selection_deselect(selection, imgid);
  else
    dt_selection_select(selection, imgid);
}

static int32_t _list_iterate(struct dt_selection_t *selection, GList **list, int *count, const gboolean add)
{
  *count += 1;
  int32_t imgid = GPOINTER_TO_INT((*list)->data);

  if(add)
    _add_id_link(selection, imgid);
  else
    _remove_id_link(selection, imgid);

  *list = g_list_next(*list);
  return imgid;
}

void dt_selection_select_list(struct dt_selection_t *selection, const GList *const l)
{
  if(!l) return;
  GList *list = (GList *)l;

  // Send SQL queries by batches of 400 imgids for performance
  while(list)
  {
    int count = 0;
    gchar *ids = g_strdup("");
    while(list && count < 400)
    {
      int32_t imgid = _list_iterate(selection, &list, &count, TRUE);
      ids = dt_util_dstrcat(ids, (ids[0] != '\0') ? ", (%i)" : "(%i)", imgid);
    }
    gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES %s", ids);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
    g_free(query);
  }

  _update_gui();
}

void dt_selection_deselect_list(struct dt_selection_t *selection, const GList *const l)
{
  if(!l) return;
  GList *list = (GList *)l;

  // Send SQL queries by batches of 400 imgids for performance
  while(list)
  {
    int count = 0;
    gchar *ids = g_strdup("");
    while(list && count < 400)
    {
      int32_t imgid = _list_iterate(selection, &list, &count, FALSE);
      ids = dt_util_dstrcat(ids, (ids[0] != '\0') ? ", %i" : "%i", imgid);
    }
    gchar *query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid IN (%s)", ids);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
    g_free(query);
    g_free(ids);
  }

  _update_gui();
}

gchar *dt_selection_ids_to_string(struct dt_selection_t *selection)
{
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

gboolean dt_selection_is_id_selected(struct dt_selection_t *selection, int32_t imgid)
{
  if(!selection || !selection->ids) return FALSE;
  return (g_list_find(selection->ids, GINT_TO_POINTER(imgid)) != NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
