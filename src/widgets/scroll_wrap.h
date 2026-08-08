/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_WIDGETS_SCROLL_WRAP_H
#define DT_WIDGETS_SCROLL_WRAP_H

/* Vertically resizable containers with a drag grip, and the height persistence behind them.
 *
 * Two shapes, both giving the user a grip on the bottom edge and remembering where they left
 * it: a scrolled window around scrollable content (tree views, text views), and a bare
 * height-request around a self-drawing area (graphs, scopes). */

#include <gtk/gtk.h>

G_BEGIN_DECLS

enum
{
  TREE_LIST_MIN_ROWS = 3,
  TREE_LIST_MAX_ROWS = 11
};

typedef enum dt_ui_resize_mode_t
{
  // Auto-fit: the area shrinks to its content (up to the user/max height). Best for widgets
  // updated rarely; their height following content is helpful, not disruptive.
  DT_UI_RESIZE_DYNAMIC = 0,
  // Fixed: the area keeps the user-set (or default) height regardless of content, so it never
  // shifts the surrounding layout when its content changes. Best for widgets that refresh on
  // hover/selection (tags, notes, metadata) and the collection/library list.
  DT_UI_RESIZE_STATIC
} dt_ui_resize_mode_t;

typedef struct dt_gui_widget_auto_height_t
{
  char *config_str;   // conf key persisting the user-chosen height (px); owned
  int min_size;       // minimum height floor in device pixels
  int last_height;    // last applied bare (pre-padding) height, shared with the drag handle
  dt_ui_resize_mode_t mode;
  GtkTreeModel *model;
  GtkTextBuffer *buffer;
  gulong model_row_inserted;
  gulong model_row_deleted;
  gulong model_row_changed;
  gulong model_rows_reordered;
  gulong model_row_expanded;
  gulong model_row_collapsed;
  gulong buffer_changed;
} dt_gui_widget_auto_height_t;

/**
 * @brief Wrap a scrollable widget in a recessed, vertically resizable scrolled window with a
 * drag handle.
 *
 * Compatible with GtkTreeView, GtkTextView and any other content widget. A drag grip floats on
 * the scrolled window's bottom edge (invisible until hovered); the chosen height is persisted
 * under @p config_str. Returns the wrapper overlay, not the scrolled window.
 *
 * @param w content widget.
 * @param min_size minimum height floor, in device-independent pixels (rescaled by
 *                 DT_PIXEL_APPLY_DPI). In DT_UI_RESIZE_STATIC mode it also serves as the
 *                 default height before the user drags.
 * @param config_str conf key persisting the user-chosen height (copied internally).
 * @param mode DT_UI_RESIZE_DYNAMIC to auto-fit content, or DT_UI_RESIZE_STATIC to keep a fixed
 *             height regardless of content (avoids layout shifts for hover-/selection-driven
 *             widgets).
 */
GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str, dt_ui_resize_mode_t mode);

/**
 * @brief Return the inner GtkScrolledWindow of a dt_ui_scroll_wrap() wrapper, or NULL.
 */
GtkWidget *dt_ui_scroll_wrap_get_scrolled_window(GtkWidget *wrapper);

/**
 * @brief Make a self-drawing widget (typically a GtkDrawingArea graph or scope) vertically
 * resizable.
 *
 * The widget is given a fixed height-request (persisted under @p config_str) and a drag grip
 * floating on its bottom edge -- the same grip used by panels, scroll wrappers and the
 * histogram scope. The content is not scrolled: it keeps drawing to its live allocation, only
 * the height-request changes. Returns a wrapper overlay to pack in place of @p area.
 *
 * @param area the drawing widget (its callbacks/refs stay valid; pack the returned overlay).
 * @param config_str conf key persisting the user-chosen height (copied internally).
 * @param default_height default height in device-independent px (rescaled by DT_PIXEL_APPLY_DPI).
 * @param min_height minimum height floor in device-independent px.
 */
GtkWidget *dt_ui_resizable_drawing_area(GtkWidget *area, char *config_str, int default_height, int min_height);

G_END_DECLS

#endif // DT_WIDGETS_SCROLL_WRAP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
