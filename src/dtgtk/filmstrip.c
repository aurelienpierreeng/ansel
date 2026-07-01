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
 * @file filmstrip.c
 * @brief FILMSTRIP frontend of the thumbtable: the horizontal strip used by darkroom, map and
 *        print, and its layout-ops vtable. The shared engine lives in thumbtable.c; see
 *        thumbtable_internal.h for the ops contract.
 */

#include "common/darktable.h"
#include "control/control.h"
#include "control/signal.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/thumbtable_internal.h"
#include "dtgtk/thumbnail.h"
#include "gui/gtk.h"
#include "views/view.h"


// --- Content widget ---------------------------------------------------------------------------

static GtkWidget *_filmstrip_create_content_widget(void)
{
  // GtkLayout implements GtkScrollable, so the GtkScrolledWindow drives it directly with NO
  // implicit GtkViewport. That viewport was the root of issue #877: at the default ~120px strip
  // height it collapsed the content to the viewport width, disabling horizontal scrolling so
  // thumbnails positioned past the first screen never drew (they only appeared on hover, when a
  // pointer event forced a repaint). With a GtkLayout the content extent is declared explicitly
  // via gtk_layout_set_size(), independent of any child-size negotiation, so it never collapses.
  return gtk_layout_new(NULL, NULL);
}


// --- Geometry ---------------------------------------------------------------------------------

static void _filmstrip_configure_dims(dt_thumbtable_t *table, int *new_width, int *new_height,
                                      int *per_row, int *thumb_width, int *thumb_height)
{
  gint sb_spacing = 0;
  gtk_widget_style_get(table->scroll_window, "scrollbar-spacing", &sb_spacing, NULL);

  // Don't use GtkAdjustment page sizes here: in filmstrip, the scrolled window height can
  // be influenced by its (resized) children during initial layout, which may cause a
  // feedback loop where thumbnails keep growing.
  int width = gtk_widget_get_allocated_width(table->parent_overlay);
  int height = gtk_widget_get_allocated_height(table->parent_overlay);
  GtkWidget *h_scroll = gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
  int h_scroll_h = 0;
  if(h_scroll)
  {
    h_scroll_h = gtk_widget_get_allocated_height(h_scroll);
    if(h_scroll_h > 0) h_scroll_h += sb_spacing;
  }

  // Clamp to the explicit panel size request when present to enforce "container drives child size".
  int req_w = -1, req_h = -1;
  gtk_widget_get_size_request(table->parent_overlay, &req_w, &req_h);
  if(req_h > 0)
    height = MIN(height, req_h);

  height -= h_scroll_h;

  const int deco = dt_thumbtable_thumb_cell_decoration();

  *new_width = width;
  *new_height = height;
  *per_row = 1;
  *thumb_height = height - deco;
  *thumb_width = *thumb_height;
}

static void _filmstrip_rowid_to_position(dt_thumbtable_t *table, int rowid, int *x, int *y)
{
  *x = rowid * table->thumb_width;
  *y = 0;
}

static int _filmstrip_position_to_rowid(dt_thumbtable_t *table, const double x, const double y)
{
  return x + (table->view_width / 2.) / table->thumb_width;
}

static void _filmstrip_get_row_ids(dt_thumbtable_t *table, int *rowid_min, int *rowid_max)
{
  float page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
  float position = gtk_adjustment_get_value(table->h_scrollbar);

  // Preload the previous and next pages too because thumbnails are typically small
  int row_min = (position - page_size) / table->thumb_width;
  int row_max = (position + 2.f * page_size) / table->thumb_width;

  *rowid_min = row_min * table->thumbs_per_row;
  *rowid_max = row_max * table->thumbs_per_row;
}

static gboolean _filmstrip_is_rowid_visible(dt_thumbtable_t *table, int rowid)
{
  int page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
  int position = gtk_adjustment_get_value(table->h_scrollbar);
  int page_right = page_size + position;

  int img_left = rowid * table->thumb_height;
  int img_right = img_left + table->thumb_width;
  return img_left >= position && img_right <= page_right;
}

static void _filmstrip_update_content_size(dt_thumbtable_t *table)
{
  // GtkLayout carries its own scrollable extent (which drives the scrollbar adjustments) instead
  // of a widget size request: width spans the whole collection, height matches the viewport so
  // there is never a vertical scroll.
  guint current_w = 0, current_h = 0;
  gtk_layout_get_size(GTK_LAYOUT(table->grid), &current_w, &current_h);

  const guint width = (guint)MAX(table->collection_count * table->thumb_width, 0);
  const guint height = (guint)MAX(table->view_height, 0);
  if(current_w != width || current_h != height)
  {
    gtk_layout_set_size(GTK_LAYOUT(table->grid), width, height);
    dt_print(DT_DEBUG_LIGHTTABLE, "Configuring grid size main dimension: %u\n", width);
  }
}


// --- Group borders ----------------------------------------------------------------------------

static void _filmstrip_group_borders(dt_thumbtable_t *table, dt_thumbnail_t *thumb, dt_thumbnail_border_t *borders)
{
  const int32_t rowid = thumb->rowid;
  const int32_t groupid = thumb->info.group_id;

  *borders |= DT_THUMBNAIL_BORDER_BOTTOM | DT_THUMBNAIL_BORDER_TOP;

  if(table->lut[CLAMP_ROW(rowid - 1)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid - 1))
    *borders |= DT_THUMBNAIL_BORDER_LEFT;

  if(table->lut[CLAMP_ROW(rowid + 1)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid + 1))
    *borders |= DT_THUMBNAIL_BORDER_RIGHT;
}


// --- Child placement --------------------------------------------------------------------------

static void _filmstrip_place_child(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  gtk_layout_put(GTK_LAYOUT(table->grid), thumb->widget, thumb->x, thumb->y);
}

static void _filmstrip_move_child(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  gtk_layout_move(GTK_LAYOUT(table->grid), thumb->widget, thumb->x, thumb->y);
}


// --- Scrollbars -------------------------------------------------------------------------------

static gboolean _filmstrip_wants_scroll_value(dt_thumbtable_t *table, GtkAdjustment *adjustment)
{
  return adjustment == table->h_scrollbar;
}

static gboolean _filmstrip_wants_page_size_notify(dt_thumbtable_t *table, GObject *object)
{
  // Page size is only used to size the filemanager/grid. Filmstrip uses its parent allocation.
  return FALSE;
}

static gboolean _filmstrip_relevant_scrollbar_changed(dt_thumbtable_t *table, GtkWidget *widget, GtkAllocation *allocation)
{
  // Filmstrip height depends on the horizontal scrollbar height. When it gets realized/allocated,
  // we need to recompute thumbnail sizes even if the parent size didn't change.
  GtkWidget *h_scroll = gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
  if(widget != h_scroll) return FALSE;
  if(allocation->height == table->last_h_scrollbar_height) return FALSE;
  table->last_h_scrollbar_height = allocation->height;
  return TRUE;
}


// --- Per-thumbnail state ----------------------------------------------------------------------

// The filmstrip highlights the active/developed image(s), NOT the lighttable selection (which the
// darkroom clears on image change) - see issue #954.
static gboolean _filmstrip_is_thumb_highlighted(dt_thumbtable_t *table, int32_t imgid)
{
  return dt_view_active_images_has_imgid(imgid);
}

static void _filmstrip_on_thumbnail_added(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  dt_thumbnail_update_selection(thumb, _filmstrip_is_thumb_highlighted(table, thumb->info.id));
  thumb->disable_actions = TRUE;
}

static void _filmstrip_on_drag_begin(dt_thumbtable_t *table, int32_t imgid)
{
  if(imgid > UNKNOWN_IMAGE)
  {
    /* Views that need drags to commit the hovered image must do it before
     * dt_act_on_get_images() snapshots the payload. */
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_DRAG_BEGIN, imgid);
  }
}


// --- Parent -----------------------------------------------------------------------------------

static void _filmstrip_setup_parent(dt_thumbtable_t *table)
{
  // The filmstrip is a static sibling of the heavily, continuously repainted darkroom center.
  // As an overlay child its own offscreen GdkWindow goes stale/blank on Wayland until a pointer
  // event invalidates it (thumbnails only appear on hover, issue #877). Make scroll_window the
  // overlay's MAIN child so it draws into the overlay's own window and stays painted.
  //
  // The main child drives the overlay's size request, which would pin the panel height to the
  // grid and break the resize-handle shrink + _parent_overlay_size_allocate reconfigure flow
  // (the regression seen when the grid couldn't be downsized). Counter that with the vertical
  // EXTERNAL policy + min_content_height(1) + propagate_natural_height(FALSE) recipe so the
  // panel can still be freely shrunk; filmstrip thumb height is derived from the panel's
  // allocation (see dt_thumbtable_configure), not from the scrolled window's own height request.
  gtk_container_add(GTK_CONTAINER(table->parent_overlay), table->scroll_window);
  gtk_widget_set_name(table->grid, "thumbtable-filmstrip");
  dt_gui_add_help_link(table->grid, dt_get_help_url("filmstrip"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_ALWAYS, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(table->scroll_window), 1);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(table->scroll_window), FALSE);
}


// --- Vtable -----------------------------------------------------------------------------------

const dt_thumbtable_layout_ops_t *dt_thumbtable_filmstrip_ops(void)
{
  static const dt_thumbtable_layout_ops_t ops = {
    .create_content_widget = _filmstrip_create_content_widget,
    .configure_dims = _filmstrip_configure_dims,
    .rowid_to_position = _filmstrip_rowid_to_position,
    .position_to_rowid = _filmstrip_position_to_rowid,
    .get_row_ids = _filmstrip_get_row_ids,
    .is_rowid_visible = _filmstrip_is_rowid_visible,
    .update_content_size = _filmstrip_update_content_size,
    .group_borders = _filmstrip_group_borders,
    .place_child = _filmstrip_place_child,
    .move_child = _filmstrip_move_child,
    .wants_scroll_value = _filmstrip_wants_scroll_value,
    .wants_page_size_notify = _filmstrip_wants_page_size_notify,
    .relevant_scrollbar_changed = _filmstrip_relevant_scrollbar_changed,
    .is_thumb_highlighted = _filmstrip_is_thumb_highlighted,
    .on_thumbnail_added = _filmstrip_on_thumbnail_added,
    .on_drag_begin = _filmstrip_on_drag_begin,
    .setup_parent = _filmstrip_setup_parent,
    .grab_focus = NULL,     // filmstrip never grabs keyboard focus for its content widget
    .handle_key = NULL,     // filmstrip has no grid-only navigation/selection keys
    .pre_activate = NULL,   // filmstrip activates without pre-selecting
  };
  return &ops;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
