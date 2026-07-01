/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @file thumbtable_internal.h
 * @brief Private interface shared between the thumbtable engine (thumbtable.c) and its two
 *        frontends (filemanager.c, filmstrip.c).
 *
 * The engine owns the collection LUT, the thumbnail hash, populate/resize, DnD, selection and
 * signal wiring; every place where the old code branched on @ref dt_thumbtable_mode_t is now a
 * call through the @ref dt_thumbtable_layout_ops_t vtable that each frontend provides.
 *
 * This header is NOT part of the public API. Only the three thumbtable translation units include
 * it; external callers keep using dtgtk/thumbtable.h.
 */

#pragma once

#include "dtgtk/thumbtable.h"
#include "dtgtk/thumbnail.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>


// Clamp a row id to the valid collection range, and detect collection edges.
// Both reference the enclosing scope's `table`, matching how they were used inline.
#define CLAMP_ROW(rowid) CLAMP(rowid, 0, table->collection_count - 1)
#define IS_COLLECTION_EDGE(rowid) (rowid < 0 || rowid >= table->collection_count)


typedef enum dt_thumbtable_direction_t
{
  DT_TT_MOVE_UP,
  DT_TT_MOVE_DOWN,
  DT_TT_MOVE_LEFT,
  DT_TT_MOVE_RIGHT,
  DT_TT_MOVE_PREVIOUS_PAGE,
  DT_TT_MOVE_NEXT_PAGE,
  DT_TT_MOVE_START,
  DT_TT_MOVE_END
} dt_thumbtable_direction_t;


/**
 * @struct dt_thumbtable_layout_ops_t
 * @brief Per-mode layout strategy. One instance per frontend, shared (const) by all tables of
 *        that mode and stored on `table->ops`. Every method receives the table it operates on.
 *
 * Methods documented as "nullable" are only meaningful in one mode; the engine checks for NULL
 * before dispatching. All others must be provided by both frontends.
 */
typedef struct dt_thumbtable_layout_ops_t
{
  // Create the content widget stored in table->grid. The grid uses a GtkFixed (pinned to the
  // viewport width, vertical scroll only); the filmstrip uses a GtkLayout, which implements
  // GtkScrollable so it goes straight into the GtkScrolledWindow with no implicit GtkViewport -
  // that viewport is what collapsed the strip width and broke horizontal scrolling (issue #877).
  GtkWidget *(*create_content_widget)(void);

  // Compute the viewport size + per-row count + individual thumbnail size from the parent
  // allocation. Fills all five out-params; the engine handles the "too small" reset and the
  // change detection. (was: dt_thumbtable_configure per-mode block)
  void (*configure_dims)(dt_thumbtable_t *table, int *new_width, int *new_height,
                         int *per_row, int *thumb_width, int *thumb_height);

  // Map a rowid to the north-west pixel corner of its cell, and back. (was: _rowid_to_position /
  // _position_to_rowid)
  void (*rowid_to_position)(dt_thumbtable_t *table, int rowid, int *x, int *y);
  int (*position_to_rowid)(dt_thumbtable_t *table, const double x, const double y);

  // Visible rowid range at the current scroll step, and single-rowid visibility test. The engine
  // wraps these with the shared "configured + scrollbars present" guard. (was: _get_row_ids /
  // _is_rowid_visible)
  void (*get_row_ids)(dt_thumbtable_t *table, int *rowid_min, int *rowid_max);
  gboolean (*is_rowid_visible)(dt_thumbtable_t *table, int rowid);

  // Set the virtual size of the content widget so the scrollbars span the whole collection.
  // (was: _update_grid_area per-mode block)
  void (*update_content_size)(dt_thumbtable_t *table);

  // Add the mode-specific group-border flags for a grouped thumbnail. The engine has already
  // reset the flags and checked that the image is grouped and borders are enabled.
  // (was: _add_thumbnail_group_borders per-mode block)
  void (*group_borders)(dt_thumbtable_t *table, dt_thumbnail_t *thumb, dt_thumbnail_border_t *borders);

  // Put a freshly-added thumbnail widget in the content widget / move an existing one. thumb->x,y
  // are already computed. (was: gtk_fixed_put / gtk_fixed_move)
  void (*place_child)(dt_thumbtable_t *table, dt_thumbnail_t *thumb);
  void (*move_child)(dt_thumbtable_t *table, dt_thumbnail_t *thumb);

  // Scrollbar predicates. The engine performs the actual update scheduling; these only decide
  // whether an event matters for this mode (and record scrollbar geometry as a side effect for
  // relevant_scrollbar_changed). (was: _scrollbar_value_changed / _scrollbar_page_size_notify /
  // _scrollbar_widget_size_allocate branches)
  gboolean (*wants_scroll_value)(dt_thumbtable_t *table, GtkAdjustment *adjustment);
  gboolean (*wants_page_size_notify)(dt_thumbtable_t *table, GObject *object);
  gboolean (*relevant_scrollbar_changed)(dt_thumbtable_t *table, GtkWidget *widget, GtkAllocation *allocation);

  // The mode's source of truth for the "highlighted" (selected-looking) thumbnail state: the
  // lighttable selection for the grid, the active/developed image(s) for the filmstrip. The engine
  // repaints highlights from this on BOTH selection and active-image changes, so whichever the mode
  // actually tracks stays in sync (issue #954: clearing the selection on darkroom entry must not
  // clear the filmstrip's developed-image marker).
  gboolean (*is_thumb_highlighted)(dt_thumbtable_t *table, int32_t imgid);

  // Per-thumbnail selection / action state applied when a thumbnail enters the viewport.
  // (was: _add_thumbnail_at_rowid selection block)
  void (*on_thumbnail_added)(dt_thumbtable_t *table, dt_thumbnail_t *thumb);

  // Commit the hovered image at drag-begin (filmstrip raises a signal, grid extends selection).
  // (was: _event_dnd_begin branch)
  void (*on_drag_begin)(dt_thumbtable_t *table, int32_t imgid);

  // Build the parent overlay scroll stack, widget name, help link and scroll policy.
  // (was: dt_thumbtable_set_parent per-mode block)
  void (*setup_parent)(dt_thumbtable_t *table);

  // Nullable. Grab keyboard focus for the content widget before scroll-to-selection.
  // (was: _grab_focus FILEMANAGER block)
  void (*grab_focus)(dt_thumbtable_t *table);

  // Nullable. Handle a mode-specific navigation/selection key. Return TRUE if consumed. Shared
  // keys (Left/Right/Page/Home/End/Return/Alt/Delete) are handled by the engine.
  // (was: dt_thumbtable_key_pressed_grid FILEMANAGER-only cases)
  gboolean (*handle_key)(dt_thumbtable_t *table, GdkEventKey *event, guint key, int32_t imgid);

  // Nullable. Select the image about to be opened by a Return/activate key (grid only).
  void (*pre_activate)(dt_thumbtable_t *table, int32_t imgid);
} dt_thumbtable_layout_ops_t;


// Frontend vtable accessors (defined in filemanager.c / filmstrip.c).
const dt_thumbtable_layout_ops_t *dt_thumbtable_grid_ops(void);
const dt_thumbtable_layout_ops_t *dt_thumbtable_filmstrip_ops(void);


// --- Engine internals shared with the frontends -----------------------------------------------

// Extra extent one .thumb-cell adds beyond the layout stride (see thumbtable.c for the rationale).
int dt_thumbtable_thumb_cell_decoration(void);

// Coalesced focus/scroll-to-selection scheduling (used by grid-only zoom & grid-config API).
void dt_thumbtable_schedule_focus(dt_thumbtable_t *table, const gint priority);

// LUT lookup: collection index of an imgid, or UNKNOWN_IMAGE. Caller MUST hold table->lock
// (matches every existing call site; the helper does not lock internally).
int dt_thumbtable_find_rowid_from_imgid(dt_thumbtable_t *table, const int32_t imgid);

// Keyboard navigation step inside the collection (used by the grid frontend's handle_key).
void dt_thumbtable_move_in_grid(dt_thumbtable_t *table, GdkEventKey *event,
                                dt_thumbtable_direction_t direction, int origin_imgid);
