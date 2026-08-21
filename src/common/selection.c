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

#include "common/utility.h"
#include "database/selection_repository.h"
#include "common/collection.h"
#include "common/selection.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/image.h"
#include "control/signal.h"


typedef struct dt_selection_t
{
  /* length of selection. 0 means no selection, -1 means it needs to be updated */
  uint32_t length;

  /* this stores the last single clicked image id indicating
     the start of a selection range */
  int32_t last_single_id;

  /* GList of ids of all images in selection */
  GList *ids;

  /* TRUE while a selection is parked in memory.selected_backup by dt_selection_push(),
     waiting for the matching dt_selection_pop(). This lived on dt_gui_gtk_t, which is why a
     layer-1 module reached for dt_gui_get_global() to read its own state -- it is not GUI
     state, and no GUI code ever touched it. */
  gboolean stacked;
} dt_selection_t;


// Signal the GUI that selection got changed and trigger a selected images counter update
static void _update_gui()
{
  dt_collection_hint_message(dt_collection_get_global());
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_SELECTION_CHANGED);
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
  dt_selection_repository_drop_uncollected();
}

// Unroll DB imgids to GList
static GList *_selection_database_to_glist(dt_selection_t *selection)
{
  // Don't reverse the GList: the query orders SQL rows descending and the repository
  // prepends, so what comes back is already ascending.
  return dt_selection_repository_get_all();
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
  if(IS_NULL_PTR(selection->ids)) return NULL;

  return g_list_copy(selection->ids);
}

int dt_selection_get_length(struct dt_selection_t *selection)
{
  if(IS_NULL_PTR(selection) || !selection->ids) return 0;

  return selection->length;
}

static void _selection_select(dt_selection_t *selection, int32_t imgid)
{
  if(imgid < 0) return;

  dt_selection_repository_select(imgid);
}

static void _selection_deselect(dt_selection_t *selection, int32_t imgid)
{
  if(imgid < 0) return;

  dt_selection_repository_deselect(imgid);
}

void dt_selection_push(dt_selection_t *selection)
{
  // Backup current selection
  if(!selection->stacked)
  {
    dt_selection_repository_push();
    selection->stacked = TRUE;

    // Commit from DB to GList of imgids
    dt_selection_reload_from_database(selection);
  }

  _update_gui();
}

void dt_selection_pop(dt_selection_t *selection)
{
  // Restore current selection
  if(selection->stacked)
  {
    dt_selection_repository_pop();
    selection->stacked = FALSE;

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
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_selection_update_collection), (gpointer)selection);

  return selection;
}

void dt_selection_free(dt_selection_t *selection)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_selection_update_collection),
                                     (gpointer)selection);
  g_list_free(selection->ids);
  selection->ids = NULL;
  dt_selection_repository_cleanup();
  dt_free(selection);
}

void dt_selection_clear(dt_selection_t *selection)
{
  dt_selection_repository_clear();
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
  if(IS_NULL_PTR(l)) return;
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
    dt_selection_repository_select_list(ids);
    // its sibling below has always freed this; this one never did
    dt_free(ids);
  }

  _update_gui();
}

void dt_selection_deselect_list(struct dt_selection_t *selection, const GList *const l)
{
  if(IS_NULL_PTR(l)) return;
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
    dt_selection_repository_deselect_list(ids);
    dt_free(ids);
  }

  _update_gui();
}

gchar *dt_selection_ids_to_string(struct dt_selection_t *selection)
{
  // There is no selection even after init, abort
  if(IS_NULL_PTR(selection->ids)) return NULL;

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
  if(IS_NULL_PTR(selection) || !selection->ids) return FALSE;
  return (g_list_find(selection->ids, GINT_TO_POINTER(imgid)) != NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
