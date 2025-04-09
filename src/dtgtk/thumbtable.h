/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.
    Copyright (C) 2022-2025 Aurélien PIERRE.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */
#include "dtgtk/thumbnail.h"
#include "common/dtpthread.h"
#include "common/darktable.h"
#include "common/debug.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>

#pragma once

typedef enum dt_thumbtable_mode_t
{
  DT_THUMBTABLE_MODE_NONE,
  DT_THUMBTABLE_MODE_FILEMANAGER,
  DT_THUMBTABLE_MODE_FILMSTRIP
} dt_thumbtable_mode_t;


typedef enum dt_thumbtable_zoom_t
{
  DT_THUMBTABLE_ZOOM_FIT = 0,
  DT_THUMBTABLE_ZOOM_HALF = 1,
  DT_THUMBTABLE_ZOOM_FULL = 2,
  DT_THUMBTABLE_ZOOM_TWICE = 3
} dt_thumbtable_zoom_t;

typedef struct dt_thumbtable_cache_t
{
  int32_t imgid;          // image ID as found in database
  int32_t groupid;        // image group ID as found in database
  dt_thumbnail_t *thumb;  // reference to the thumbnail object
  uint32_t group_members; // numbers of images in the same group
  uint32_t history_items;
} dt_thumbtable_cache_t;

typedef struct dt_thumbtable_t
{
  dt_thumbtable_mode_t mode;
  dt_thumbnail_overlay_t overlays;

  GtkWidget *grid; // GtkGrid

  // Store the current number of columns in grid
  int grid_cols;

  // list of thumbnails loaded inside main widget (dt_thumbnail_t)
  // for filmstrip and filemanager, this is all the images drawn at screen (even partially)
  // for zoommable, this is all the images in the row drawn at screen. We don't load laterals images on fly.
  GList *list;

  int thumbs_per_row; // number of image in a row (1 for filmstrip ; MAX_ZOOM for zoomable)
  int thumb_width;              // demanded thumb size (real size can differ of 1 due to rounding)
  int thumb_height;
  int view_width, view_height; // last main widget size

  gboolean dragging;
  int last_x, last_y;         // last position of cursor during move
  int drag_dx, drag_dy;       // distance of move of the current dragging session
  dt_thumbnail_t *drag_thumb; // thumb currently dragged (under the mouse)

  // when performing a drag, we store the list of items to drag here
  // as this can change during the drag and drop (esp. because of the image_over_id)
  GList *drag_list;

  // nb of thumbnails loaded
  uint32_t thumb_nb;

  // Set to TRUE once the current collection has been loaded into thumbnails,
  // reset to FALSE on collection changed events.
  // When TRUE, we bypass (re)-init of the thumbnails.
  gboolean collection_inited;
  gboolean thumbs_inited;
  gboolean configured;

  // Checksum of the collection query for caching
  uint64_t collection_hash;
  int collection_count;

  int min_row_id;
  int max_row_id;

  // Our LUT of collection, mapping rowid (index) to imgid (content)
  dt_thumbtable_cache_t *lut;

  GtkWidget *scroll_window;

  // References to the scrollbar adjustments belonging to the parent widget
  GtkAdjustment *v_scrollbar;
  GtkAdjustment *h_scrollbar;
  double x_position;
  double y_position;

  // Overlay in which we insert the grid, in central view and filmstrip
  GtkWidget *parent_overlay;

  // Since GUI and background signals can init/delete/populate/iterate over the same stuff,
  // ensure iterations don't happen on stuff being deleted at the same time.
  // Protect only the loops of dynamic size
  dt_pthread_mutex_t lock;

  // signal that the current collection needs to be entirely flushed unconditionnaly
  gboolean reset_collection;

  // show extended overlays while holding alt key
  gboolean alternate_mode;

  // The rowid (aka index in thumbnail sequence) of the last active thumbnail
  // used as a fallback for missing imgid to sync scrolling when an image is evicted
  // from current collection
  int rowid;

  // Set to TRUE to only display the group leader image
  gboolean collapse_groups;

  // Thumbnails inner zoom level
  dt_thumbtable_zoom_t zoom;

  // Show focus regions on thumbnails
  gboolean focus_regions;
  gboolean focus_peaking;

  gboolean draw_group_borders;

  // Gtk signal id for the redraw event
  unsigned long draw_signal_id;
  gboolean no_drawing;

} dt_thumbtable_t;


dt_thumbtable_t *dt_thumbtable_new(dt_thumbtable_mode_t mode);
void dt_thumbtable_cleanup(dt_thumbtable_t *table);
void dt_thumbtable_configure(dt_thumbtable_t *table);
void dt_thumbtable_update(dt_thumbtable_t *table);
void dt_thumbtable_set_parent(dt_thumbtable_t *table, dt_thumbtable_mode_t mode);
void dt_thumbtable_update_parent(dt_thumbtable_t *table);

// drag & drop receive function - handles dropping of files in the center view (files are added to the library)
void dt_thumbtable_event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data, guint target_type, guint time, gpointer user_data);

// change the type of overlays that should be shown (over or under the image)
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, dt_thumbnail_overlay_t over);

// set zoom level
void dt_thumbtable_set_zoom(dt_thumbtable_t *table, dt_thumbtable_zoom_t level);
dt_thumbtable_zoom_t dt_thumbtable_get_zoom(dt_thumbtable_t *table);

// offset all the zoomed thumbnails by the same amount
void dt_thumbtable_offset_zoom(dt_thumbtable_t *table, const double delta_x, const double delta_y);

void dt_thumbtable_set_focus_regions(dt_thumbtable_t *table, gboolean enable);
gboolean dt_thumbtable_get_focus_regions(dt_thumbtable_t *table);

void dt_thumbtable_set_focus_peaking(dt_thumbtable_t *table, gboolean enable);
gboolean dt_thumbtable_get_focus_peaking(dt_thumbtable_t *table);

void dt_thumbtable_set_draw_group_borders(dt_thumbtable_t *table, gboolean enable);
gboolean dt_thumbtable_get_draw_group_borders(dt_thumbtable_t *table);

// signal that the current collection needs to be flushed entirely before being reloaded
void dt_thumbtable_reset_collection(dt_thumbtable_t *table);

gboolean dt_thumbtable_key_released_grid(GtkWidget *self, GdkEventKey *event, gpointer user_data);
gboolean dt_thumbtable_key_pressed_grid(GtkWidget *self, GdkEventKey *event, gpointer user_data);

// call this when the history of an image is changed and mipmap cache needs updating.
// reinit = TRUE will force-flush the existing thumbnail. imgid = -1 applies on all thumbnails in thumbtable.
void dt_thumbtable_refresh_thumbnail_real(dt_thumbtable_t *table, int32_t imgid, gboolean reinit);
#define dt_thumbtable_refresh_thumbnail(table, imgid, reinit) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_LIGHTTABLE, dt_thumbtable_refresh_thumbnail_real, (table), (imgid), (reinit))

// select all images from the current collection through the GUI list of thumbnails
void dt_thumbtable_select_all(dt_thumbtable_t *table);

// select all images from the current collection that lie between the closest current selection bound
// and the specified rowid index.
void dt_thumbtable_select_range(dt_thumbtable_t *table, const int rowid);

// invert the selection of images from the current collection through the GUI list of thumbnails
void dt_thumbtable_invert_selection(dt_thumbtable_t *table);

// set mouse_over imgid while resolving conflicts between mouse and keyboard events
void dt_thumbtable_dispatch_over(dt_thumbtable_t *table, GdkEventType type, int32_t imgid);

int dt_thumbtable_scroll_to_imgid(dt_thumbtable_t *table, int32_t imgid);
int dt_thumbtable_scroll_to_active_rowid(dt_thumbtable_t *table);
int dt_thumbtable_scroll_to_selection(dt_thumbtable_t *table);

// Internally update the first visible thumbnail index based on current scrolling position
void dt_thumbtable_set_active_rowid(dt_thumbtable_t *table);

/**
 * Gtk quick wrappers/helpers
 */

static inline void dt_thumbtable_redraw_real(dt_thumbtable_t *table)
{
  gtk_widget_queue_draw(table->grid);
}

#define dt_thumbtable_redraw(table) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_LIGHTTABLE, dt_thumbtable_redraw_real, (table))

static inline void dt_thumbtable_show(dt_thumbtable_t *table)
{
  gtk_widget_show(table->parent_overlay);
  gtk_widget_show(table->scroll_window);

  // Thumbtable is prevented to configure and update, for
  // as long as it's hidden. We need to force the update now.
  dt_thumbtable_redraw(table);
}

static inline void dt_thumbtable_hide(dt_thumbtable_t *table)
{
  gtk_widget_hide(table->parent_overlay);
  gtk_widget_hide(table->scroll_window);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
