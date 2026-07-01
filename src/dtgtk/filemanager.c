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
 * @file filemanager.c
 * @brief FILEMANAGER frontend of the thumbtable: the lighttable grid layout, its layout-ops
 *        vtable, and the grid-only public API (zoom + grid reconfiguration). The shared engine
 *        lives in thumbtable.c; see thumbtable_internal.h for the ops contract.
 */

#include "common/darktable.h"
#include "common/collection.h"
#include "common/selection.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/thumbtable_internal.h"
#include "dtgtk/thumbnail.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include <math.h>


// --- Content widget ---------------------------------------------------------------------------

static GtkWidget *_grid_create_content_widget(void)
{
  // GtkFixed: the grid pins its width to the viewport (horizontal policy NEVER) and only ever
  // scrolls vertically, so the implicit GtkViewport it gets inside the GtkScrolledWindow is fine.
  return gtk_fixed_new();
}


// --- Geometry ---------------------------------------------------------------------------------

static void _grid_configure_dims(dt_thumbtable_t *table, int *new_width, int *new_height,
                                 int *per_row, int *thumb_width, int *thumb_height)
{
  // GtkScrolledWindow reserves a "scrollbar-spacing" gutter between the content and the scrollbar.
  // It is a legacy GtkWidget *style property* (default 3px), not a CSS box property - invisible in the
  // GTK Inspector's CSS pane. We zero it via CSS (#thumbtable-scroll, see ansel.css) but still read its
  // real value here so the maths stays exact whatever the theme/DPI yields.
  gint sb_spacing = 0;
  gtk_widget_style_get(table->scroll_window, "scrollbar-spacing", &sb_spacing, NULL);

  // Use actual widget allocations for sizing: GtkAdjustment page sizes are not reliably updated
  // during shrinking, which can prevent thumbnails from downscaling until another resize happens.
  int width = gtk_widget_get_allocated_width(table->parent_overlay);
  int height = gtk_widget_get_allocated_height(table->parent_overlay);

  GtkWidget *v_scroll = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
  const int v_scroll_w = v_scroll ? gtk_widget_get_allocated_width(v_scroll) : 0;
  if(v_scroll_w > 0) width -= v_scroll_w + sb_spacing;

  const int cols = dt_conf_get_int("plugins/lighttable/images_in_row");

  // Reserve the per-cell decoration surplus on the cross axis so the whole grid (cols*thumb_width plus
  // the last cell's protruding border) stays within the viewport. Under the NEVER scroll policy a
  // GtkFixed wider than the viewport would drag the scrolled window (and scrollbar) past the parent.
  const int deco = dt_thumbtable_thumb_cell_decoration();

  *new_width = width;
  *new_height = height;
  *per_row = cols;
  *thumb_width = (int)floorf((float)(width - deco) / (float)MAX(cols, 1));
  *thumb_height = (cols == 1) ? height : *thumb_width;
}

static void _grid_rowid_to_position(dt_thumbtable_t *table, int rowid, int *x, int *y)
{
  int row = rowid / table->thumbs_per_row; // euclidean division
  int col = rowid % table->thumbs_per_row;
  *x = col * table->thumb_width;
  *y = row * table->thumb_height;
}

static int _grid_position_to_rowid(dt_thumbtable_t *table, const double x, const double y)
{
  // Attempt to get the image rowid sitting in the center of the middle row
  return (y + table->view_height / 2) / table->thumb_height * table->thumbs_per_row
    + table->thumbs_per_row / 2 - 1;
}

static void _grid_get_row_ids(dt_thumbtable_t *table, int *rowid_min, int *rowid_max)
{
  // Pixel coordinates of the viewport:
  float page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
  float position = gtk_adjustment_get_value(table->v_scrollbar);

  // what is currently visible lies between position and position + page_size.
  // don't preload next/previous rows because, when in 1 thumb/column,
  // that can be quite slow
  int row_min = floorf(position / (float)table->thumb_height);
  int row_max = ceilf((position + page_size) / (float)table->thumb_height);

  // rowid is the positional ID of the image in the SQLite collection, indexed from 0.
  // SQLite indexes from 1 but then be use our own array to cache results.
  *rowid_min = row_min * table->thumbs_per_row;
  *rowid_max = row_max * table->thumbs_per_row;
}

static gboolean _grid_is_rowid_visible(dt_thumbtable_t *table, int rowid)
{
  // Pixel coordinates of the viewport:
  int page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
  int position = gtk_adjustment_get_value(table->v_scrollbar);
  int page_bottom = page_size + position;

  int img_top = (rowid / table->thumbs_per_row) * table->thumb_height;
  int img_bottom = img_top + table->thumb_height;
  return img_top >= position && img_bottom <= page_bottom;
}

static void _grid_update_content_size(dt_thumbtable_t *table)
{
  int current_w = 0, current_h = 0;
  gtk_widget_get_size_request(table->grid, &current_w, &current_h);

  const int height = (int)ceilf((float)table->collection_count / (float)table->thumbs_per_row) * table->thumb_height;
  // Pin the cross-axis (width) to the viewport so the grid reaches the scrollbar instead of stopping
  // at the floor-rounded column total. Safe under NEVER: the grid content already fits view_width
  // (decoration budgeted in configure), so this lands on a stable fixpoint (grid = view_width,
  // scrolled window = parent) without perturbing the cross-axis adjustment.
  if(current_h != height || current_w != table->view_width)
  {
    gtk_widget_set_size_request(table->grid, table->view_width, height);
    dt_print(DT_DEBUG_LIGHTTABLE, "Configuring grid size main dimension: %.f\n", (double)height);
  }
}


// --- Group borders ----------------------------------------------------------------------------

static void _grid_group_borders(dt_thumbtable_t *table, dt_thumbnail_t *thumb, dt_thumbnail_border_t *borders)
{
  const int32_t rowid = thumb->rowid;
  const int32_t groupid = thumb->info.group_id;

  if(table->lut[CLAMP_ROW(rowid - table->thumbs_per_row)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid - table->thumbs_per_row))
    *borders |= DT_THUMBNAIL_BORDER_TOP;

  if(table->lut[CLAMP_ROW(rowid + table->thumbs_per_row)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid + table->thumbs_per_row))
    *borders |= DT_THUMBNAIL_BORDER_BOTTOM;

  if(table->lut[CLAMP_ROW(rowid - 1)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid - 1))
    *borders |= DT_THUMBNAIL_BORDER_LEFT;

  if(table->lut[CLAMP_ROW(rowid + 1)].groupid != groupid
    || IS_COLLECTION_EDGE(rowid + 1))
    *borders |= DT_THUMBNAIL_BORDER_RIGHT;

  // If the group spans over more than a full row,
  // close the row ends. Otherwise, we leave orphans opened at the row ends.
  if(table->lut[rowid].thumb->info.group_members > table->thumbs_per_row)
  {
    if(rowid % table->thumbs_per_row == 0)
      *borders |= DT_THUMBNAIL_BORDER_LEFT;
    if(rowid % table->thumbs_per_row == table->thumbs_per_row - 1)
      *borders |= DT_THUMBNAIL_BORDER_RIGHT;
  }
}


// --- Child placement --------------------------------------------------------------------------

static void _grid_place_child(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  gtk_fixed_put(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
}

static void _grid_move_child(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  gtk_fixed_move(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
}


// --- Scrollbars -------------------------------------------------------------------------------

static gboolean _grid_wants_scroll_value(dt_thumbtable_t *table, GtkAdjustment *adjustment)
{
  return adjustment == table->v_scrollbar;
}

static gboolean _grid_wants_page_size_notify(dt_thumbtable_t *table, GObject *object)
{
  // Page size is only used to size the filemanager/grid. Only react to the scroll-axis (vertical)
  // adjustment. The cross-axis adjustment's page size is driven by the grid width we set during
  // configure; reacting to it would re-enter configure and can spin in a resize/redraw loop.
  return object == (GObject *)table->v_scrollbar;
}

static gboolean _grid_relevant_scrollbar_changed(dt_thumbtable_t *table, GtkWidget *widget, GtkAllocation *allocation)
{
  // Filemanager fallback sizing subtracts the vertical scrollbar width.
  GtkWidget *v_scroll = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
  if(widget != v_scroll) return FALSE;
  if(allocation->width == table->last_v_scrollbar_width) return FALSE;
  table->last_v_scrollbar_width = allocation->width;
  return TRUE;
}


// --- Per-thumbnail state ----------------------------------------------------------------------

// The grid highlights the lighttable selection.
static gboolean _grid_is_thumb_highlighted(dt_thumbtable_t *table, int32_t imgid)
{
  return dt_selection_is_id_selected(darktable.selection, imgid);
}

static void _grid_on_thumbnail_added(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  dt_thumbnail_update_selection(thumb, _grid_is_thumb_highlighted(table, thumb->info.id));
  thumb->disable_actions = FALSE;
}

static void _grid_on_drag_begin(dt_thumbtable_t *table, int32_t imgid)
{
  // Ensure the image that collects the drag event is properly part of the selection
  if(imgid > UNKNOWN_IMAGE)
    dt_selection_select(darktable.selection, imgid);
}


// --- Parent / focus ---------------------------------------------------------------------------

static void _grid_setup_parent(dt_thumbtable_t *table)
{
  // The filemanager grid overlays the center canvas and floats as an OVERLAY child so its size
  // request stays decoupled from the panel (the outer GtkOverlay drives its allocation). It is
  // not affected by the Wayland blank-until-hover issue because it is the focused content
  // of the lighttable view, not a static sibling of a continuously repainted surface.
  gtk_overlay_add_overlay(GTK_OVERLAY(table->parent_overlay), table->scroll_window);
  gtk_widget_set_name(table->grid, "thumbtable-filemanager");
  dt_gui_add_help_link(table->grid, dt_get_help_url("lighttable_filemanager"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
}

static void _grid_grab_focus(dt_thumbtable_t *table)
{
  GtkWidget *focused = NULL;
  GtkWidget *toplevel = gtk_widget_get_toplevel(table->grid);
  if(!IS_NULL_PTR(toplevel) && GTK_IS_WINDOW(toplevel))
    focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

  // Grab focus here otherwise, on first click over the grid,
  // scrolled window gets scrolled all the way to the top and it's annoying.
  // This can work only if the grid is mapped and realized, which we ensure
  // by wrapping that in a g_idle() method.
  if(IS_NULL_PTR(focused) || (!GTK_IS_EDITABLE(focused) && !GTK_IS_TEXT_VIEW(focused)))
    gtk_widget_grab_focus(table->grid);
}


// --- Keyboard ---------------------------------------------------------------------------------

static gboolean _grid_handle_key(dt_thumbtable_t *table, GdkEventKey *event, guint key, int32_t imgid)
{
  switch(key)
  {
    case GDK_KEY_Up:
      dt_thumbtable_move_in_grid(table, event, DT_TT_MOVE_UP, imgid);
      return TRUE;
    case GDK_KEY_Down:
      dt_thumbtable_move_in_grid(table, event, DT_TT_MOVE_DOWN, imgid);
      return TRUE;
    case GDK_KEY_space:
    {
      if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      {
        dt_pthread_mutex_lock(&table->lock);
        int rowid = dt_thumbtable_find_rowid_from_imgid(table, imgid);
        dt_pthread_mutex_unlock(&table->lock);
        dt_thumbtable_select_range(table, rowid);
      }
      else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
        dt_selection_toggle(darktable.selection, imgid);
      else
        dt_selection_select_single(darktable.selection, imgid);
      return TRUE;
    }
    case GDK_KEY_nobreakspace:
    {
      // Shift + space is decoded as nobreakspace on BÉPO keyboards
      dt_pthread_mutex_lock(&table->lock);
      int rowid = dt_thumbtable_find_rowid_from_imgid(table, imgid);
      dt_pthread_mutex_unlock(&table->lock);
      dt_thumbtable_select_range(table, rowid);
      return TRUE;
    }
  }
  return FALSE;
}

static void _grid_pre_activate(dt_thumbtable_t *table, int32_t imgid)
{
  // This is only to be consistent with mouse events:
  // opening to darkroom happens with double click (aka ACTIVATE event),
  // but the first click always select the clicked thumbnail before.
  dt_selection_select_single(darktable.selection, imgid);
}


// --- Vtable -----------------------------------------------------------------------------------

const dt_thumbtable_layout_ops_t *dt_thumbtable_grid_ops(void)
{
  static const dt_thumbtable_layout_ops_t ops = {
    .create_content_widget = _grid_create_content_widget,
    .configure_dims = _grid_configure_dims,
    .rowid_to_position = _grid_rowid_to_position,
    .position_to_rowid = _grid_position_to_rowid,
    .get_row_ids = _grid_get_row_ids,
    .is_rowid_visible = _grid_is_rowid_visible,
    .update_content_size = _grid_update_content_size,
    .group_borders = _grid_group_borders,
    .place_child = _grid_place_child,
    .move_child = _grid_move_child,
    .wants_scroll_value = _grid_wants_scroll_value,
    .wants_page_size_notify = _grid_wants_page_size_notify,
    .relevant_scrollbar_changed = _grid_relevant_scrollbar_changed,
    .is_thumb_highlighted = _grid_is_thumb_highlighted,
    .on_thumbnail_added = _grid_on_thumbnail_added,
    .on_drag_begin = _grid_on_drag_begin,
    .setup_parent = _grid_setup_parent,
    .grab_focus = _grid_grab_focus,
    .handle_key = _grid_handle_key,
    .pre_activate = _grid_pre_activate,
  };
  return &ops;
}


// --- Grid-only public API ---------------------------------------------------------------------

void dt_thumbtable_set_zoom(dt_thumbtable_t *table, dt_thumbtable_zoom_t level)
{
  table->zoom = level;
  dt_thumbtable_set_active_rowid(table);
  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);
  dt_thumbtable_schedule_focus(table, G_PRIORITY_DEFAULT_IDLE);
}

dt_thumbtable_zoom_t dt_thumbtable_get_zoom(dt_thumbtable_t *table)
{
  return table->zoom;
}

void dt_thumbtable_offset_zoom(dt_thumbtable_t *table, const double delta_x, const double delta_y)
{
  dt_pthread_mutex_lock(&table->lock);
  GHashTableIter iter;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, table->list);
  while(g_hash_table_iter_next(&iter, NULL, &value))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)value;
    thumb->zoomx += delta_x;
    thumb->zoomy += delta_y;
    gtk_widget_queue_draw(thumb->w_image);
  }
  dt_pthread_mutex_unlock(&table->lock);
}

/**
 * @brief Idle callback for applying grid configuration
 *
 * This handler is used when grid configuration changes (like column count).
 * It handles the grid reconfiguration and thumbnail updates, then schedules
 * a follow-up idle callback for scrolling to ensure proper GTK widget state.
 */
static gboolean _thumbtable_idle_apply_grid_configuration(gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(IS_NULL_PTR(table)) return G_SOURCE_REMOVE;

  table->idle_update_id = 0;

  // Reconfigure the grid with new column settings from config
  dt_thumbtable_configure(table);

  // Update and populate visible thumbnails at new sizes
  dt_thumbtable_update(table);

  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);

  // Queue redraw for any unpopulated areas
  if(table->thumb_nb == 0) gtk_widget_queue_draw(table->grid);

  // Schedule scrolling as a follow-up idle callback with lower priority.
  // This ensures the GTK widget grid is fully mapped and realized before we attempt to scroll.
  // We use a lower priority (G_PRIORITY_LOW) to let the GTK layout pass complete first.
  dt_thumbtable_schedule_focus(table, G_PRIORITY_LOW);

  return G_SOURCE_REMOVE;
}

void dt_thumbtable_apply_grid_configuration(dt_thumbtable_t *table)
{
  if(IS_NULL_PTR(table)) return;
  if(table->scroll_window && !gtk_widget_is_visible(table->scroll_window)) return;

  // Cancel any pending standard idle update to coalesce configuration changes
  if(table->idle_update_id)
  {
    g_source_remove(table->idle_update_id);
    table->idle_update_id = 0;
  }

  // Ensure we have the current active image so we can scroll back to it after grid size change
  dt_thumbtable_set_active_rowid(table);

  // Schedule the coordinated grid configuration with higher priority to ensure
  // it runs before other pending updates
  table->idle_update_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                          (GSourceFunc)_thumbtable_idle_apply_grid_configuration,
                                          table, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
