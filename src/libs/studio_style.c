/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

/** Studio Capture: style module. "Pool" is the ordered treeview of styles
    actually applied, on import (first entry replaces the fresh history, the
    rest stack on top) or via the manual apply button. "Styles" below it is a
    tree of every available style, grouped into categories split on "|" in
    the style name (same convention as the library's own style panel). Both
    treeviews carry per-row action icons in dedicated columns (up/down/remove
    on pool rows, add on style leaves), matching the click-by-column pattern
    already used for mask group rows in develop/blend_gui.c. */

#include "common/macros.h"
#include "common/mem_alloc.h"
#include "common/module_versioning.h"
#include "control/signal.h"
#include "common/folder_survey.h"
#include "common/history_merge.h"
#include "common/image.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

DT_MODULE(1)

typedef enum dt_studio_pool_cols_t
{
  POOL_COL_NAME = 0,
  POOL_NUM_COLS
} dt_studio_pool_cols_t;

typedef enum dt_studio_styles_cols_t
{
  STYLES_COL_NAME = 0,
  STYLES_COL_FULLNAME, // set only on leaves (actual styles, not category nodes)
  STYLES_NUM_COLS
} dt_studio_styles_cols_t;

typedef struct dt_lib_studio_style_t
{
  GtkWidget *pool_treeview;         // flat, ordered list of applied style names
  GtkTreeViewColumn *pool_up_col;
  GtkTreeViewColumn *pool_down_col;
  GtkTreeViewColumn *pool_remove_col;

  GtkWidget *styles_treeview;       // categories (split on "|") with styles as leaves
  GtkTreeViewColumn *styles_add_col;
  // A treeview cell rendered by GtkCellRenderer is not its own CSS node, so
  // the theme's `*:disabled { opacity }` rule never reaches it: "sensitive"
  // bindings render identically whether true or false. Graying a leaf
  // already in the pool therefore uses two direct mechanisms computed live
  // in cell_data_func callbacks instead: an explicit "foreground" color for
  // the name, and swapping in a pre-dimmed pixbuf for the add icon.
  GdkPixbuf *add_icon_enabled;
  GdkPixbuf *add_icon_disabled;

  GList *pool; // ordered list of g_strdup'd style names: the source of truth
} dt_lib_studio_style_t;

const char *name(dt_lib_module_t *self)
{
  return _("Auto style");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "studio_capture", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 980;
}

static gboolean _studio_style_pool_contains(dt_lib_studio_style_t *d, const char *name)
{
  for(GList *l = d->pool; l; l = g_list_next(l))
    if(!g_strcmp0((const char *)l->data, name)) return TRUE;
  return FALSE;
}

/**
 * @brief Persist the pool, in order, to conf.
 */
static void _studio_style_save(dt_lib_studio_style_t *d)
{
  GString *conf = g_string_new(NULL);
  for(GList *l = d->pool; l; l = g_list_next(l))
  {
    if(conf->len) g_string_append(conf, DT_FOLDER_SURVEY_STYLES_SEPARATOR);
    g_string_append(conf, (const char *)l->data);
  }

  dt_conf_set_string(DT_FOLDER_SURVEY_STYLES_CONF_KEY, conf->str);
  g_string_free(conf, TRUE);
}

/**
 * @brief Rebuild the pool treeview, in order, from d->pool.
 */
static void _studio_style_rebuild_pool_ui(dt_lib_studio_style_t *d)
{
  GtkListStore *store = gtk_list_store_new(POOL_NUM_COLS, G_TYPE_STRING);
  GtkTreeIter iter;
  for(GList *l = d->pool; l; l = g_list_next(l))
  {
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, POOL_COL_NAME, (const char *)l->data, -1);
  }
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->pool_treeview), GTK_TREE_MODEL(store));
  g_object_unref(store);
}

/**
 * @brief Drop pool entries whose style no longer exists.
 */
static void _studio_style_prune_pool(dt_lib_studio_style_t *d, GList *all_styles)
{
  GList *l = d->pool;
  while(l)
  {
    GList *next = g_list_next(l);
    gboolean found = FALSE;
    for(GList *s = all_styles; s && !found; s = g_list_next(s))
      found = !g_strcmp0(((dt_style_t *)s->data)->name, (const char *)l->data);
    if(!found)
    {
      dt_free(l->data);
      d->pool = g_list_delete_link(d->pool, l);
    }
    l = next;
  }
}

/**
 * @brief Find, or create as a child of the last found/created sibling, the
 * tree node for one "|"-separated path segment. Mirrors libs/styles.c's own
 * category tree builder so both style browsers behave identically.
 */
static gboolean _studio_style_get_node_for_name(GtkTreeModel *model, const gboolean root, GtkTreeIter *iter,
                                                const gchar *segment_name)
{
  GtkTreeIter parent = *iter;

  if(root)
  {
    if(!gtk_tree_model_get_iter_first(model, iter))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, NULL);
      return FALSE;
    }
  }
  else
  {
    if(!gtk_tree_model_iter_children(model, iter, &parent))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, &parent);
      return FALSE;
    }
  }

  do
  {
    gchar *node_name = NULL;
    gtk_tree_model_get(model, iter, STYLES_COL_NAME, &node_name, -1);
    const gboolean match = !g_strcmp0(node_name, segment_name);
    dt_free(node_name);
    if(match) return TRUE;
  }
  while(gtk_tree_model_iter_next(model, iter));

  gtk_tree_store_append(GTK_TREE_STORE(model), iter, root ? NULL : &parent);
  return FALSE;
}

/**
 * @brief Rebuild the styles tree from every available style, grouping
 * category segments split on "|" in the style name, and prune the pool of
 * any name that no longer matches one.
 */
static void _studio_style_populate_styles_tree(dt_lib_studio_style_t *d)
{
  GList *all_styles = dt_styles_get_list("");
  _studio_style_prune_pool(d, all_styles);

  GtkTreeStore *store = gtk_tree_store_new(STYLES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeIter iter;

  for(GList *s = all_styles; s; s = g_list_next(s))
  {
    const dt_style_t *style = (const dt_style_t *)s->data;
    gchar **segments = g_strsplit(style->name, "|", 0);

    for(int k = 0; segments[k]; k++)
    {
      const gboolean found = _studio_style_get_node_for_name(GTK_TREE_MODEL(store), k == 0, &iter, segments[k]);
      if(!found)
      {
        if(segments[k + 1])
          gtk_tree_store_set(store, &iter, STYLES_COL_NAME, segments[k], -1);
        else
          gtk_tree_store_set(store, &iter, STYLES_COL_NAME, segments[k], STYLES_COL_FULLNAME, style->name, -1);
      }
    }
    g_strfreev(segments);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(d->styles_treeview), GTK_TREE_MODEL(store));
  g_object_unref(store);

  g_list_free_full(all_styles, dt_style_free);
}

/**
 * @brief Gray a leaf's name when its style is already in the pool. Computed
 * live at render time (not stored in the model), mirroring libs/history.c's
 * _lib_history_view_cell_set_foreground for the same reason: cell renderers
 * are not separate CSS nodes, so the theme cannot do this via :disabled.
 */
static void _studio_style_name_cell_data_func(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                              GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  gchar *fullname = NULL;
  gtk_tree_model_get(model, iter, STYLES_COL_FULLNAME, &fullname, -1);

  if(!IS_NULL_PTR(fullname) && _studio_style_pool_contains(d, fullname))
    g_object_set(renderer, "foreground-set", TRUE, "foreground", "#888", NULL);
  else
    g_object_set(renderer, "foreground-set", FALSE, NULL);

  dt_free(fullname);
}

/**
 * @brief Show the add icon only on leaves, swapping in a pre-dimmed pixbuf
 * when the leaf's style is already in the pool. Computed live at render
 * time for the same reason as _studio_style_name_cell_data_func above.
 */
static void _studio_style_add_cell_data_func(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                             GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  gchar *fullname = NULL;
  gtk_tree_model_get(model, iter, STYLES_COL_FULLNAME, &fullname, -1);

  if(IS_NULL_PTR(fullname))
  {
    g_object_set(renderer, "visible", FALSE, NULL);
  }
  else
  {
    const gboolean in_pool = _studio_style_pool_contains(d, fullname);
    g_object_set(renderer, "visible", TRUE, "pixbuf", in_pool ? d->add_icon_disabled : d->add_icon_enabled,
                NULL);
  }

  dt_free(fullname);
}

/**
 * @brief Apply the up/down/remove action for the pool row under a click,
 * identified by which icon column was hit.
 */
static void _studio_style_pool_action_click(dt_lib_studio_style_t *d, GtkTreePath *path, GtkTreeViewColumn *column)
{
  const int index = gtk_tree_path_get_indices(path)[0];

  if(column == d->pool_up_col)
  {
    if(index <= 0) return;
    GList *previous = g_list_nth(d->pool, index - 1);
    GList *current = g_list_next(previous);
    gpointer tmp = previous->data;
    previous->data = current->data;
    current->data = tmp;
  }
  else if(column == d->pool_down_col)
  {
    GList *current = g_list_nth(d->pool, index);
    GList *next = current ? g_list_next(current) : NULL;
    if(IS_NULL_PTR(current) || IS_NULL_PTR(next)) return;
    gpointer tmp = current->data;
    current->data = next->data;
    next->data = tmp;
  }
  else if(column == d->pool_remove_col)
  {
    GList *link = g_list_nth(d->pool, index);
    if(IS_NULL_PTR(link)) return;
    char *removed_name = (char *)link->data;
    d->pool = g_list_delete_link(d->pool, link);
    gtk_widget_queue_draw(d->styles_treeview); // un-gray the corresponding leaf, if visible
    dt_free(removed_name);
  }
  else
    return;

  _studio_style_rebuild_pool_ui(d);
  _studio_style_save(d);
}

/**
 * @brief Left click on a pool row's up/down/remove icon column.
 *
 * @return TRUE when an action column was hit, so the treeview's default
 * handler does not also change the row selection.
 */
static gboolean _studio_style_pool_button_pressed(GtkWidget *treeview, GdkEventButton *event, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  if(IS_NULL_PTR(event) || event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY) return FALSE;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, &column,
                                    NULL, NULL))
    return FALSE;

  const gboolean consumed = column == d->pool_up_col || column == d->pool_down_col
                            || column == d->pool_remove_col;
  if(consumed) _studio_style_pool_action_click(d, path, column);
  gtk_tree_path_free(path);
  return consumed;
}

static gboolean _studio_style_pool_query_tooltip(GtkWidget *treeview, gint x, gint y, gboolean keyboard_tip,
                                                 GtkTooltip *tooltip, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  if(keyboard_tip) return FALSE;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), x, y, &path, &column, NULL, NULL)) return FALSE;
  gtk_tree_path_free(path);

  const char *text = NULL;
  if(column == d->pool_up_col) text = _("Apply this style earlier");
  else if(column == d->pool_down_col) text = _("Apply this style later");
  else if(column == d->pool_remove_col) text = _("Remove this style from the pool");
  if(IS_NULL_PTR(text)) return FALSE;

  gtk_tooltip_set_text(tooltip, text);
  return TRUE;
}

/**
 * @brief Add the style leaf under a click to the end of the pool.
 */
static gboolean _studio_style_add_click(dt_lib_studio_style_t *d, GtkWidget *treeview, GtkTreePath *path)
{
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, path)) return FALSE;

  gchar *fullname = NULL;
  gtk_tree_model_get(model, &iter, STYLES_COL_FULLNAME, &fullname, -1);
  if(IS_NULL_PTR(fullname)) return FALSE; // a category node, not a style

  if(!_studio_style_pool_contains(d, fullname))
  {
    d->pool = g_list_append(d->pool, g_strdup(fullname));
    _studio_style_rebuild_pool_ui(d);
    gtk_widget_queue_draw(d->styles_treeview); // gray out the just-added leaf
    _studio_style_save(d);
  }
  dt_free(fullname);
  return TRUE;
}

static gboolean _studio_style_styles_button_pressed(GtkWidget *treeview, GdkEventButton *event, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  if(IS_NULL_PTR(event) || event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY) return FALSE;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, &column,
                                    NULL, NULL))
    return FALSE;

  gboolean consumed = FALSE;
  if(column == d->styles_add_col) consumed = _studio_style_add_click(d, treeview, path);
  gtk_tree_path_free(path);
  return consumed;
}

static gboolean _studio_style_styles_query_tooltip(GtkWidget *treeview, gint x, gint y, gboolean keyboard_tip,
                                                   GtkTooltip *tooltip, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  if(keyboard_tip) return FALSE;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), x, y, &path, &column, NULL, NULL)) return FALSE;
  gtk_tree_path_free(path);

  if(column != d->styles_add_col) return FALSE;
  gtk_tooltip_set_text(tooltip, _("Add this style to the pool"));
  return TRUE;
}

/**
 * @brief Re-apply the pool, in order, to the displayed image.
 */
static void _studio_style_apply_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;

  const int32_t imgid = dt_view_active_images_get_first();
  if(imgid <= UNKNOWN_IMAGE)
  {
    dt_control_log(_("No image is displayed."));
    return;
  }
  if(IS_NULL_PTR(d->pool))
  {
    dt_control_log(_("The pool is empty."));
    return;
  }

  dt_hm_batch_state_t batch = { 0 };
  int applied = 0;

  for(GList *l = d->pool; l; l = g_list_next(l))
  {
    const char *pool_name = (const char *)l->data;
    const int32_t style_id = dt_styles_get_id_by_name(pool_name);
    if(style_id > 0
       && !dt_styles_apply_to_image_merge(pool_name, style_id, imgid, DT_HISTORY_MERGE_APPEND, &batch))
    {
      applied++;
    }
  }
  dt_hm_batch_state_cleanup(&batch);

  if(applied == 0)
  {
    dt_control_log(_("No style could be applied."));
    return;
  }

  // History went straight to DB: refresh cached metadata, mipmap and thumbnails,
  // then let listeners (studio center view) re-fetch their surfaces.
  dt_image_history_changed(imgid, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_control_log(ngettext("Applied %d style.", "Applied %d styles.", applied), applied);
}

static void _studio_style_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)user_data;
  _studio_style_populate_styles_tree(d);
  _studio_style_rebuild_pool_ui(d);
  _studio_style_save(d);
}

/**
 * @brief Load the persisted, ordered pool from conf, once at startup.
 */
static void _studio_style_load_pool(dt_lib_studio_style_t *d)
{
  char *conf = dt_conf_get_string(DT_FOLDER_SURVEY_STYLES_CONF_KEY);
  gchar **names = g_strsplit(conf && conf[0] ? conf : "", DT_FOLDER_SURVEY_STYLES_SEPARATOR, -1);
  dt_free(conf);

  for(gchar **pool_name = names; *pool_name; pool_name++)
    if((*pool_name)[0] != '\0') d->pool = g_list_append(d->pool, g_strdup(*pool_name));

  g_strfreev(names);
}

/**
 * @brief Append the text column carrying the row's display name. Packed
 * with expand=TRUE so any later-appended icon columns stay flush right.
 */
static GtkTreeViewColumn *_studio_style_add_name_column(GtkWidget *treeview, const int name_model_col,
                                                        GtkCellRenderer **out_renderer)
{
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, (gchar *)0);
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", name_model_col);
  gtk_tree_view_column_set_expand(col, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), col);
  if(out_renderer) *out_renderer = renderer;
  return col;
}

/**
 * @brief Append one fixed-width icon column, for per-row action clicks
 * handled by column identity (see the *_button_pressed handlers above).
 */
static GtkTreeViewColumn *_studio_style_add_icon_column(GtkWidget *treeview, const char *icon_name,
                                                        GtkCellRenderer **out_renderer)
{
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
  g_object_set(renderer, "icon-name", icon_name, "stock-size", GTK_ICON_SIZE_MENU, NULL);
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(col, DT_PIXEL_APPLY_DPI(24));
  gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), col);
  if(out_renderer) *out_renderer = renderer;
  return col;
}

/**
 * @brief Load a themed icon at the menu icon size, optionally pre-blended to
 * a lower alpha. Used for the styles tree's add icon: unlike a "sensitive"
 * binding, a literal alpha blend is guaranteed to render dimmed regardless
 * of theme/CSS quirks around cell renderers (see the struct comment above).
 */
static GdkPixbuf *_studio_style_load_icon(const char *icon_name, const double alpha)
{
  gint width = 16;
  gint height = 16;
  gtk_icon_size_lookup(GTK_ICON_SIZE_MENU, &width, &height);

  GdkPixbuf *base = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), icon_name, width,
                                             GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
  if(IS_NULL_PTR(base) || alpha >= 1.0) return base;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);
  gdk_cairo_set_source_pixbuf(cr, base, 0, 0);
  cairo_paint_with_alpha(cr, alpha);
  cairo_destroy(cr);

  GdkPixbuf *dimmed = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
  cairo_surface_destroy(surface);
  g_object_unref(base);
  return dimmed;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)g_malloc0(sizeof(dt_lib_studio_style_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  /* Pool: the ordered, applied styles, with inline reorder/remove icons. */
  GtkWidget *pool_label = gtk_label_new(_("Pool"));
  gtk_widget_set_halign(pool_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), pool_label, FALSE, FALSE, 0);

  d->pool_treeview = gtk_tree_view_new();
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->pool_treeview), FALSE);
  _studio_style_add_name_column(d->pool_treeview, POOL_COL_NAME, NULL);
  d->pool_up_col = _studio_style_add_icon_column(d->pool_treeview, "go-up-symbolic", NULL);
  d->pool_down_col = _studio_style_add_icon_column(d->pool_treeview, "go-down-symbolic", NULL);
  d->pool_remove_col = _studio_style_add_icon_column(d->pool_treeview, "list-remove-symbolic", NULL);
  g_signal_connect(d->pool_treeview, "button-press-event", G_CALLBACK(_studio_style_pool_button_pressed), d);
  gtk_widget_set_has_tooltip(d->pool_treeview, TRUE);
  g_signal_connect(d->pool_treeview, "query-tooltip", G_CALLBACK(_studio_style_pool_query_tooltip), d);

  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->pool_treeview, 100, "plugins/darkroom/studio_style/poolwindowheight",
                                       DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  GtkWidget *apply = gtk_button_new_with_label(_("Apply to the displayed image"));
  gtk_widget_set_tooltip_text(apply, _("Replace the history of the displayed image with the pool of styles"));
  g_signal_connect(G_OBJECT(apply), "clicked", G_CALLBACK(_studio_style_apply_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), apply, FALSE, FALSE, 0);

  /* Styles: every available style, grouped into categories, with an inline add icon on leaves. */
  GtkWidget *styles_label = gtk_label_new(_("Styles"));
  gtk_widget_set_halign(styles_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), styles_label, FALSE, FALSE, 0);

  d->add_icon_enabled = _studio_style_load_icon("list-add-symbolic", 1.0);
  d->add_icon_disabled = _studio_style_load_icon("list-add-symbolic", 0.35);

  d->styles_treeview = gtk_tree_view_new();
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->styles_treeview), FALSE);
  GtkCellRenderer *name_renderer = NULL;
  GtkTreeViewColumn *name_col
      = _studio_style_add_name_column(d->styles_treeview, STYLES_COL_NAME, &name_renderer);
  gtk_tree_view_column_set_cell_data_func(name_col, name_renderer, _studio_style_name_cell_data_func, d, NULL);

  GtkCellRenderer *add_renderer = NULL;
  d->styles_add_col = _studio_style_add_icon_column(d->styles_treeview, "list-add-symbolic", &add_renderer);
  /* The cell_data_func drives "pixbuf" on every row; clear the static "icon-name" set by
   * _studio_style_add_icon_column() so the two properties never compete. */
  g_object_set(add_renderer, "icon-name", NULL, NULL);
  gtk_tree_view_column_set_cell_data_func(d->styles_add_col, add_renderer, _studio_style_add_cell_data_func, d,
                                          NULL);
  g_signal_connect(d->styles_treeview, "button-press-event", G_CALLBACK(_studio_style_styles_button_pressed), d);
  gtk_widget_set_has_tooltip(d->styles_treeview, TRUE);
  g_signal_connect(d->styles_treeview, "query-tooltip", G_CALLBACK(_studio_style_styles_query_tooltip), d);

  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->styles_treeview, 100,
                                       "plugins/darkroom/studio_style/styleswindowheight", DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  _studio_style_load_pool(d);
  _studio_style_populate_styles_tree(d);
  _studio_style_rebuild_pool_ui(d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_STYLE_CHANGED,
                                  G_CALLBACK(_studio_style_changed_callback), d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_studio_style_changed_callback), self->data);
  dt_lib_studio_style_t *d = (dt_lib_studio_style_t *)self->data;
  if(!IS_NULL_PTR(d))
  {
    g_list_free_full(d->pool, dt_free_gpointer);
    if(!IS_NULL_PTR(d->add_icon_enabled)) g_object_unref(d->add_icon_enabled);
    if(!IS_NULL_PTR(d->add_icon_disabled)) g_object_unref(d->add_icon_disabled);
  }
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
