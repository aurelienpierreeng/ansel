/*
    This file is part of darktable,
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Jérémy Rosen.
    Copyright (C) 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philippe Weyland.
    Copyright (C) 2023, 2025 Aurélien PIERRE.
    
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

#pragma once

#include <glib.h>
#include <inttypes.h>

#include "common/debug.h"

/**
 * @file selection.h
 *
 * The old design built selections from collections, through SQL, to ensure selections
 * were always a subset of collections. That didn't work well with the GUI option to show/hide
 * grouped images, because then, every SQL query needed to be modified according to a GUI parameter,
 * which was complicated, hard to debug, tiring to maintain.
 *
 * Collections now are immune to GUI parameters. It is in the thumbtable that we decide to show/hide
 * the thumbnail widgets of grouped images, and collections contain all of them.
 * In order to select grouped images depending on whether they are
 * shown or not, it is also from the thumbtable that batch selection events need to be dispatched, using visible images.
 * It is assumed that users expect selections to include everything visible but only what's visible and nothing more.
 *
 * Because the thumbtable is populated from collections, we know it contains only valid imgid, and contains at most
 * the whole current collection. So we can safely assume any imgid
 * coming from the thumbtable is a valid image ID with regard to the current collection, without additional checks.
 *
 * We synchronize here 2 representations of the selections:
 *  - a memory DB table: memory.selected_images, that is saved to be restored between reboots,
 *  - a GList of imgid, selection->ids, cached, that allows us direct access from C loops.
 *
 * Selections subscribe to the COLLECTION_CHANGED signal to ensure the selected imgids are a subset of the current collection
 * at all time. But that doesn't deal with images that might be hidden from GUI, for example image group members.
 *
 * It is up to the thumbtable code (aka GUI) to resync selection imgids with visible widgets.
 *
 * No insertions or deletions should be made into the `main.selected_images` database table, outside of `selection.c`.
 * Interactions with selections should use the public API here.
 *
 * `SELECT imgid FROM main.seleted_images` should be reserved to SQL JOIN, when fetching metadata from DB for the list of IDs.
 * All other cases should iterate over the GList of imgids returned by `dt_selection_get_list()`
 *
 */

struct dt_selection_t;

struct dt_selection_t *dt_selection_new();
void dt_selection_free(struct dt_selection_t *selection);

/** Get the first imgid of a selection */
int32_t dt_selection_get_first_id(struct dt_selection_t *selection);
/** clears the selection */
void dt_selection_clear(struct dt_selection_t *selection);
/** adds a single imgid to the current selection. use the optimized `dt_selection_select_list()` to process batches. */
void dt_selection_select(struct dt_selection_t *selection, int32_t imgid);
/** removes a single imgid from the current selection. use the optimized `dt_selection_deselect_list()` to process batches. */
void dt_selection_deselect(struct dt_selection_t *selection, int32_t imgid);
/** clears current selection and adds a single imgid */
void dt_selection_select_single(struct dt_selection_t *selection, int32_t imgid);
/** toggles selection of a single image in the current selection */
void dt_selection_toggle(struct dt_selection_t *selection, int32_t imgid);
/** selects a set of images from a list in a fast, optimized fashion. the list is unaltered */
void dt_selection_select_list(struct dt_selection_t *selection, const GList *list);
/** deselects a set of images from a list in a fast, optimized fashion. the list is unaltered */
void dt_selection_deselect_list(struct dt_selection_t *selection, const GList *list);
/** get the list of selected images. Warning: returns a copy, the caller owns it and needs to free it. */
GList *dt_selection_get_list(struct dt_selection_t *selection);

/** backup the current selection to a temp memory database table */
void dt_selection_push(struct dt_selection_t *selection);
/** restore the previous selection from a temp memory database table */
void dt_selection_pop(struct dt_selection_t *selection);

/** get the length of the current selection (number of items) */
int dt_selection_get_length(struct dt_selection_t *selection);

/** concatenate all image ids from the selection as a string, separated with coma, for SQL queries */
gchar *dt_selection_ids_to_string(struct dt_selection_t *selection);

/** see if the imgid is known from the selection */
gboolean dt_selection_is_id_selected(struct dt_selection_t *selection, int32_t imgid);

/** call this right after the selection got changed directly in memory database, to resync the GList representation of the selection */
void dt_selection_reload_from_database_real(struct dt_selection_t *selection);

#define dt_selection_reload_from_database(selection) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_SQL, dt_selection_reload_from_database_real, (selection))

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
