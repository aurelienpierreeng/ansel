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

#include "widgets/scroll_wrap.h"

#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"
#include "widgets/resize_handle.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"
#include <glib/gi18n.h>
#include "widgets/container.h"

static const char *const DT_GUI_WIDGET_AUTO_HEIGHT_KEY = "dt-gui-widget-auto-height";

// ---- Resizable drawing area (the histogram-scope paradigm, made reusable) -------------------
// A fixed-pixel-height GtkDrawingArea (or any widget sized by height-request) made vertically
// resizable by the shared grip primitive, with the height persisted to config. Unlike the
// scroll-wrap helper above, the content draws itself and is not scrolled; we only manage its
// height-request, so the drawing code keeps reading the live allocation as usual.

static const char *const DT_UI_RESIZABLE_AREA_KEY = "dt-ui-resizable-area";

typedef struct dt_ui_resizable_area_t
{
  char *config_str;   // conf key persisting the user-chosen height (px); owned
  int min_height;     // minimum height floor, device pixels
  int last_height;    // last applied height, shared with the drag handle
} dt_ui_resizable_area_t;

/* The tallest run of whole rows that fits in `limit`, walked in display order.
 *
 * Rounding to a multiple of a nominal row height cannot answer this once a row is not that
 * height: a separator is three pixels, so a list of four rows plus a separator rounds to five
 * nominal rows and cuts the fifth in half -- which is the very thing the rounding exists to
 * prevent. Returns TRUE once the limit is reached, so the recursion into expanded children can
 * stop the walk. */
static gboolean _treeview_fit_rows(GtkTreeView *treeview, GtkTreeModel *model, GtkTreeIter *parent,
                                   const gint nominal, const gint limit, gint *used)
{
  if(!GTK_IS_TREE_MODEL(model)) return TRUE;

  GtkTreeIter iter;
  gboolean valid = parent ? gtk_tree_model_iter_children(model, &iter, parent)
                          : gtk_tree_model_get_iter_first(model, &iter);

  while(valid)
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);

    GdkRectangle rect = { 0 };
    if(path) gtk_tree_view_get_background_area(treeview, path, NULL, &rect);
    const gint row = rect.height > 0 ? rect.height : nominal;

    if(*used + row > limit)
    {
      if(path) gtk_tree_path_free(path);
      return TRUE;
    }
    *used += row;

    if(path && gtk_tree_model_iter_has_child(model, &iter) && gtk_tree_view_row_expanded(treeview, path)
       && _treeview_fit_rows(treeview, model, &iter, nominal, limit, used))
    {
      gtk_tree_path_free(path);
      return TRUE;
    }

    if(path) gtk_tree_path_free(path);
    valid = gtk_tree_model_iter_next(model, &iter);
  }

  return FALSE;
}

static gint _get_container_row_heigth(GtkWidget *w)
{
  gint height = DT_PIXEL_APPLY_DPI(10);

  if(GTK_IS_TREE_VIEW(w))
  {
    gint row_height = 0;

    const gint num_columns = gtk_tree_view_get_n_columns(GTK_TREE_VIEW(w));
    for(int c = 0; c < num_columns; c++)
    {
      gint cell_height = 0;
      gtk_tree_view_column_cell_get_size(gtk_tree_view_get_column(GTK_TREE_VIEW(w), c),
                                        NULL, NULL, NULL, NULL, &cell_height);
      if(cell_height > row_height) row_height = cell_height;
    }
    GValue separation = { G_TYPE_INT };
    gtk_widget_style_get_property(w, "vertical-separator", &separation);

    if(row_height > 0) height = row_height + g_value_get_int(&separation);
  }
  else if(GTK_IS_TEXT_VIEW(w))
  {
    PangoLayout *layout = gtk_widget_create_pango_layout(w, "X");
    pango_layout_get_pixel_size(layout, NULL, &height);
    g_object_unref(layout);
  }
  else
  {
    GtkWidget *child = dt_gui_container_first_child(GTK_CONTAINER(w));
    if(child)
    {
      height = gtk_widget_get_allocated_height(child);
    }
  }

  return height;
}

// find the scrolled window parent of a treeview, if any
static GtkWidget *_search_parent_scrolled_window(GtkWidget *w)
{
  if(!GTK_IS_WIDGET(w)) return NULL;
  
  GtkWidget *parent = w;
  while(parent)
  {
    if(GTK_IS_SCROLLED_WINDOW(parent)) break;
    parent = gtk_widget_get_parent(parent);
  }

  return GTK_IS_SCROLLED_WINDOW(parent) ? parent : NULL;
}

// Counts only visible items (those whose parents are expanded)
/* The height the visible rows actually occupy.
 *
 * Each row is measured rather than counted, because not every row is one nominal row tall: a
 * GtkTreeView separator is drawn as a few pixels, and counting it as a row would claim a whole
 * one of window height for it. gtk_tree_view_get_background_area() answers 0 for a view that is
 * not realised yet, and those rows fall back to the nominal height -- which is what this did for
 * every row before. */
static int _treeview_visible_height(GtkTreeView *treeview, GtkTreeModel *model, GtkTreeIter *parent,
                                    const gint nominal)
{
  if(!GTK_IS_TREE_MODEL(model)) return 0;

  GtkTreeIter iter;
  gboolean valid = parent ? gtk_tree_model_iter_children(model, &iter, parent)
                          : gtk_tree_model_get_iter_first(model, &iter);
  int height = 0;

  while(valid)
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);

    GdkRectangle rect = { 0 };
    if(path) gtk_tree_view_get_background_area(treeview, path, NULL, &rect);
    height += rect.height > 0 ? rect.height : nominal;

    // If this item is expanded, recursively measure its visible children
    if(path && gtk_tree_model_iter_has_child(model, &iter) && gtk_tree_view_row_expanded(treeview, path))
      height += _treeview_visible_height(treeview, model, &iter, nominal);

    if(path) gtk_tree_path_free(path);

    valid = gtk_tree_model_iter_next(model, &iter);
  }

  return height;
}

static int _textview_count_visible_rows(GtkWidget *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return 0;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  if(!GTK_IS_TEXT_BUFFER(buffer)) return 0;

  // For text views, use the number of logical lines in the buffer.
  return MAX(0, gtk_text_buffer_get_line_count(buffer));
}

static void _widget_auto_disconnect_model(dt_gui_widget_auto_height_t *state, GtkWidget *treeview)
{
  if(IS_NULL_PTR(state)) return;

  GtkTreeModel *model = state->model;
  if(model)
  {
    if(state->model_row_inserted)   g_signal_handler_disconnect(model, state->model_row_inserted);
    if(state->model_row_deleted)    g_signal_handler_disconnect(model, state->model_row_deleted);
    if(state->model_row_changed)    g_signal_handler_disconnect(model, state->model_row_changed);
    if(state->model_rows_reordered) g_signal_handler_disconnect(model, state->model_rows_reordered);
    g_object_remove_weak_pointer(G_OBJECT(model), (gpointer *)&state->model);
  }

  if(GTK_IS_TREE_VIEW(treeview))
  {
    if(state->model_row_expanded)   g_signal_handler_disconnect(treeview, state->model_row_expanded);
    if(state->model_row_collapsed)  g_signal_handler_disconnect(treeview, state->model_row_collapsed);
  }

  state->model = NULL;
  state->model_row_inserted = 0;
  state->model_row_deleted = 0;
  state->model_row_changed = 0;
  state->model_rows_reordered = 0;
  state->model_row_expanded = 0;
  state->model_row_collapsed = 0;
}

static void _widget_auto_disconnect_buffer(dt_gui_widget_auto_height_t *state)
{
  if(IS_NULL_PTR(state)) return;

  GtkTextBuffer *buffer = state->buffer;
  if(buffer)
  {
    if(state->buffer_changed) g_signal_handler_disconnect(buffer, state->buffer_changed);
    g_object_remove_weak_pointer(G_OBJECT(buffer), (gpointer *)&state->buffer);
  }

  state->buffer = NULL;
  state->buffer_changed = 0;
}

/**
 * @brief Window-height ceiling shared by the auto-size rule and the drag handle.
 *
 * @details The full main-window height: a resizable area may grow as tall as the window (the
 * parent panel scrolls to reach it). Content shorter than this still shrinks to fit, so this only
 * bounds how far the user can drag.
 */
static gint _resizable_scroll_max_height(void)
{
  GtkWidget *win = dt_widget_root_window();
  return win ? gtk_widget_get_allocated_height(win) : DT_PIXEL_APPLY_DPI(1000);
}

/**
 * @brief The single sizing rule for every dt_ui_scroll_wrap area.
 *
 * @details Height = clamp(min(content, cap), min_size, 75% window), where the cap is the user's
 * persisted height when set, otherwise the window ceiling so the area auto-grows to its content.
 * This makes small content shrink to fit, lets nested lists grow and have their parent panel scroll
 * until the user drags the handle to cap them, and snaps lists/textviews to whole rows to avoid
 * clipped half-rows. The computed bare height -- before the scrolled window's own padding and
 * border are added back -- is cached for the drag handle.
 */
static void _resizable_scroll_apply(GtkWidget *w)
{
  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkWidget *sw = _search_parent_scrolled_window(w);
  if(!GTK_IS_SCROLLED_WINDOW(sw)) return;

  const gint max_height = _resizable_scroll_max_height();
  const gint min_size = MAX(1, state->min_size);
  int stored = 0;
  const gboolean has_conf = dt_widget_stored_int(state->config_str, &stored);

  gint height;
  gint increment = 0;

  if(state->mode == DT_UI_RESIZE_STATIC)
  {
    // Fixed height: the user's persisted size, or min_size as the default. Independent of content,
    // so a content refresh (e.g. on hovering another thumbnail) never shifts the layout.
    height = CLAMP(has_conf ? stored : min_size, min_size, max_height);
  }
  else
  {
    // Dynamic: fit to content, capped by the user's persisted height (or the window ceiling).
    const gboolean row_based = GTK_IS_TREE_VIEW(w) || GTK_IS_TEXT_VIEW(w);
    increment = row_based ? _get_container_row_heigth(w) : 0;

    gint content = 0;
    if(GTK_IS_TREE_VIEW(w))
    {
      const int measured = _treeview_visible_height(GTK_TREE_VIEW(w), gtk_tree_view_get_model(GTK_TREE_VIEW(w)),
                                                   NULL, increment);
      content = MAX(increment, measured);
    }
    else if(GTK_IS_TEXT_VIEW(w))
    {
      const int rows = _textview_count_visible_rows(w);
      content = MAX(1, rows) * increment;
    }
    else
    {
      gtk_widget_get_preferred_height(w, NULL, &content);
    }

    const gint cap = has_conf ? CLAMP(stored, min_size, max_height) : max_height;
    height = CLAMP(MIN(content, cap), min_size, max_height);

    /* Snap to whole rows to avoid clipped half-rows -- but only when the area is showing less
     * than it holds, which is the only case where a row can be cut. When everything fits, the
     * measured height is already exact, and rounding it up would add most of a row of dead space
     * for any content that is not a whole number of them, such as a list carrying a separator. */
    if(increment > 0 && height < content)
    {
      if(GTK_IS_TREE_VIEW(w))
      {
        gint used = 0;
        _treeview_fit_rows(GTK_TREE_VIEW(w), gtk_tree_view_get_model(GTK_TREE_VIEW(w)), NULL, increment,
                           height, &used);
        if(used > 0) height = used;
      }
      else
      {
        // A textview's lines are all the one height, so a multiple of it is a row boundary.
        height += increment - 1;
        height -= height % increment;
      }
    }
  }
  state->last_height = height;

  // The request covers the scrolled window's whole CSS box, and the viewport only gets what is
  // left of it: padding AND border are both taken out first. Counting one and not the other
  // leaves the content exactly that many pixels short of fitting, and an AUTOMATIC policy
  // answers a two-pixel shortfall with a full scrollbar -- on an area whose whole purpose is to
  // be sized to fit its content. The .dt_recessed_scroll treeviews carry both (2px padding over
  // a 1px border), so this is not a hypothetical.
  GtkStyleContext *sw_context = gtk_widget_get_style_context(sw);
  const GtkStateFlags sw_state = gtk_widget_get_state_flags(sw);
  GtkBorder padding;
  GtkBorder border;
  gtk_style_context_get_padding(sw_context, sw_state, &padding);
  gtk_style_context_get_border(sw_context, sw_state, &border);

  gint old_height = 0;
  gtk_widget_get_size_request(sw, NULL, &old_height);
  const gint new_height = height + padding.top + padding.bottom + border.top + border.bottom
                          + (GTK_IS_TEXT_VIEW(w) ? 2 : 0);
  if(new_height != old_height)
    gtk_widget_set_size_request(sw, -1, new_height);
}

static void _widget_auto_update(GtkWidget *widget)
{
  _resizable_scroll_apply(widget);
}

static gboolean _resizable_scroll_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
  _resizable_scroll_apply(w);
  return FALSE;
}

static void _resizable_scroll_realize(GtkWidget *w, gpointer user_data)
{
  _resizable_scroll_apply(w);
}

// Drag handle accessors: bare (pre-padding) target height, kept consistent with the sizing rule.
static int _resizable_scroll_handle_get_size(gpointer user_data)
{
  GtkWidget *w = GTK_WIDGET(user_data);
  const dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  return state ? state->last_height : 0;
}

static int _resizable_scroll_handle_resize(int requested_size, gboolean finished, gpointer user_data)
{
  GtkWidget *w = GTK_WIDGET(user_data);
  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state) || IS_NULL_PTR(state->config_str)) return requested_size;

  const gint value = CLAMP(requested_size, MAX(1, state->min_size), _resizable_scroll_max_height());
  dt_widget_store_int(state->config_str, value);
  _resizable_scroll_apply(w);
  return state->last_height;
}

static void _widget_auto_model_row_inserted(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                              gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer user_data)
{
  (void)model;
  (void)path;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_changed(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                             gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_rows_reordered(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                                gint *new_order, gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  (void)new_order;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_expanded(GtkTreeView *tree_view, GtkTreeIter *expanded_iter, GtkTreePath *path,
                                              gpointer user_data)
{
  (void)tree_view;
  (void)expanded_iter;
  (void)path;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_collapsed(GtkTreeView *tree_view, GtkTreeIter *collapsed_iter, GtkTreePath *path,
                                               gpointer user_data)
{
  (void)tree_view;
  (void)collapsed_iter;
  (void)path;

  // Recalculate tree view height after loading the data
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
  (void)buffer;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_connect_model(GtkWidget *treeview)
{
  if(!GTK_IS_TREE_VIEW(treeview)) return;

  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(treeview), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  if(model == state->model) return;

  _widget_auto_disconnect_model(state, treeview);
  if(!GTK_IS_TREE_MODEL(model)) return;

  state->model = model;
  g_object_add_weak_pointer(G_OBJECT(model), (gpointer *)&state->model);
  state->model_row_inserted =   g_signal_connect(model, "row-inserted",
                                              G_CALLBACK(_widget_auto_model_row_inserted), treeview);
  state->model_row_deleted =    g_signal_connect(model, "row-deleted",
                                              G_CALLBACK(_widget_auto_model_row_deleted), treeview);
  state->model_row_changed =    g_signal_connect(model, "row-changed",
                                              G_CALLBACK(_widget_auto_model_row_changed), treeview);
  state->model_rows_reordered = g_signal_connect(model, "rows-reordered",
                                              G_CALLBACK(_widget_auto_model_rows_reordered), treeview);
  state->model_row_expanded =   g_signal_connect(treeview, "row-expanded",
                                              G_CALLBACK(_widget_auto_model_row_expanded), treeview);
  state->model_row_collapsed =  g_signal_connect(treeview, "row-collapsed",
                                              G_CALLBACK(_widget_auto_model_row_collapsed), treeview);
}

static void _widget_auto_connect_buffer(GtkWidget *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return;

  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(textview), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  if(buffer == state->buffer) return;

  _widget_auto_disconnect_buffer(state);
  if(!GTK_IS_TEXT_BUFFER(buffer)) return;

  state->buffer = buffer;
  g_object_add_weak_pointer(G_OBJECT(buffer), (gpointer *)&state->buffer);
  state->buffer_changed = g_signal_connect(buffer, "changed",
                                           G_CALLBACK(_widget_auto_text_buffer_changed), textview);
}

static void _widget_auto_on_model_changed(GObject *treeview, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  (void)user_data;
  _widget_auto_connect_model(GTK_WIDGET(treeview));
  _widget_auto_update(GTK_WIDGET(treeview));
}

static void _widget_auto_on_buffer_changed(GObject *textview, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  (void)user_data;
  _widget_auto_connect_buffer(GTK_WIDGET(textview));
  _widget_auto_update(GTK_WIDGET(textview));
}

static void _widget_auto_height_free(gpointer data)
{
  dt_gui_widget_auto_height_t *state = (dt_gui_widget_auto_height_t *)data;
  if(IS_NULL_PTR(state)) return;
  _widget_auto_disconnect_model(state, NULL);
  _widget_auto_disconnect_buffer(state);
  g_free(state->config_str);
  dt_free(state);
}

/**
 * @brief Wrap a scrollable content widget in a recessed, vertically resizable scrolled window.
 *
 * @details Returns an overlay wrapping the scrolled window, with a themed drag grip floating on its
 * bottom edge (the same grip primitive used by panels and the histogram scope). The grip takes no
 * layout space and is invisible until hovered. Sizing follows @p mode: DT_UI_RESIZE_DYNAMIC auto-fits
 * the content up to the user height, DT_UI_RESIZE_STATIC keeps a fixed height (see _resizable_scroll_apply).
 *
 * The returned widget is the wrapper overlay, not the scrolled window; callers needing the inner
 * scrolled window (e.g. to tweak its scroll policy) must use dt_ui_scroll_wrap_get_scrolled_window().
 *
 * @param w content widget (treeview, textview or any container)
 * @param min_size minimum height floor in device pixels (also the static default before the user drags)
 * @param config_str conf key persisting the user-chosen height (copied internally)
 * @param mode DT_UI_RESIZE_DYNAMIC (auto-fit) or DT_UI_RESIZE_STATIC (fixed height)
 */
GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str, dt_ui_resize_mode_t mode)
{
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  if(GTK_IS_TREE_VIEW(w)) dt_gui_add_class(sw, "dt_recessed_scroll");
  gtk_container_add(GTK_CONTAINER(sw), w);

  // Per-widget sizing state, freed with w.
  dt_gui_widget_auto_height_t *state = calloc(1, sizeof(*state));
  state->config_str = g_strdup(config_str);
  state->min_size = MAX(1, DT_PIXEL_APPLY_DPI(min_size));
  state->mode = mode;
  g_object_set_data_full(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY, state, _widget_auto_height_free);

  if(mode == DT_UI_RESIZE_DYNAMIC)
  {
    // Sizing triggers. Lists/textviews recompute from their model/buffer change signals (cheap and
    // exact) plus a one-shot on realize, when row heights become accurate -- we deliberately avoid a
    // per-draw recount, which would re-walk a large model on every redraw. Generic content has no
    // such signals, so it recomputes on draw using its (GTK-cached) preferred height.
    if(GTK_IS_TREE_VIEW(w))
    {
      g_signal_connect(w, "notify::model", G_CALLBACK(_widget_auto_on_model_changed), NULL);
      g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
    }
    else if(GTK_IS_TEXT_VIEW(w))
    {
      g_signal_connect(w, "notify::buffer", G_CALLBACK(_widget_auto_on_buffer_changed), NULL);
      g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
    }
    else
    {
      g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(_resizable_scroll_draw), NULL);
    }
    _widget_auto_connect_model(w);
    _widget_auto_connect_buffer(w);
  }
  else
  {
    // Static: height is content-independent, so we only size once after realization (and on drag).
    g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
  }

  // Drag grip floating on the scrolled window's bottom edge (overlay): it takes no layout space,
  // so the wrapper leaves no margin-like gap and stays aligned with neighbouring widgets. The grip
  // is centered on the bottom border via CSS and is invisible until hovered.
  GtkWidget *handle = dtgtk_resize_handle_new(GTK_ORIENTATION_VERTICAL, FALSE,
                                                   _("Drag to resize"),
                                                   _resizable_scroll_handle_get_size,
                                                   _resizable_scroll_handle_resize, w);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), sw);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);

  _widget_auto_update(w);
  return over;
}

/**
 * @brief Return the inner scrolled window of a dt_ui_scroll_wrap() wrapper, or NULL.
 */
GtkWidget *dt_ui_scroll_wrap_get_scrolled_window(GtkWidget *wrapper)
{
  if(!GTK_IS_CONTAINER(wrapper)) return NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(wrapper));
  GtkWidget *sw = NULL;
  for(GList *l = children; l; l = g_list_next(l))
  {
    if(GTK_IS_SCROLLED_WINDOW(l->data))
    {
      sw = GTK_WIDGET(l->data);
      break;
    }
  }
  g_list_free(children);
  return sw;
}

static void _resizable_area_free(gpointer data)
{
  dt_ui_resizable_area_t *state = (dt_ui_resizable_area_t *)data;
  if(IS_NULL_PTR(state)) return;
  g_free(state->config_str);
  dt_free(state);
}

static int _resizable_area_get_size(gpointer user_data)
{
  GtkWidget *area = GTK_WIDGET(user_data);
  const dt_ui_resizable_area_t *state = g_object_get_data(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY);
  if(state) return state->last_height;
  return gtk_widget_get_allocated_height(area);
}

static int _resizable_area_resize(int requested_size, gboolean finished, gpointer user_data)
{
  GtkWidget *area = GTK_WIDGET(user_data);
  dt_ui_resizable_area_t *state = g_object_get_data(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY);
  if(IS_NULL_PTR(state)) return requested_size;

  const int height = CLAMP(requested_size, MAX(1, state->min_height), _resizable_scroll_max_height());
  state->last_height = height;
  gtk_widget_set_size_request(area, -1, height);
  if(finished) dt_widget_store_int(state->config_str, height);
  return height;
}

GtkWidget *dt_ui_resizable_drawing_area(GtkWidget *area, char *config_str, int default_height, int min_height)
{
  dt_ui_resizable_area_t *state = calloc(1, sizeof(*state));
  state->config_str = g_strdup(config_str);
  state->min_height = MAX(1, DT_PIXEL_APPLY_DPI(min_height));

  int height = DT_PIXEL_APPLY_DPI(default_height);
  dt_widget_stored_int(config_str, &height);
  height = CLAMP(height, state->min_height, _resizable_scroll_max_height());
  state->last_height = height;
  g_object_set_data_full(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY, state, _resizable_area_free);
  gtk_widget_set_size_request(area, -1, height);

  // Drag grip floating on the area's bottom edge (an overlay, not a packed sibling), so it takes
  // no layout space -- the area stays flush with neighbouring widgets, no margin-like gap. It sits
  // over the graph's bottom inset/axis margin (graphs reserve one), invisible until hovered.
  GtkWidget *handle = dtgtk_resize_handle_new(GTK_ORIENTATION_VERTICAL, FALSE,
                                                   _("Drag to resize"),
                                                   _resizable_area_get_size, _resizable_area_resize, area);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), area);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  return over;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
