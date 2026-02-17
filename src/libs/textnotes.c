/*
    This file is part of Ansel,
    Copyright (C) 2026 Ansel developers.

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

#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/variables.h"
#include "control/control.h"
#include "control/signal.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "libs/lib.h"
#include "views/view.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef HAVE_HTTP_SERVER
#include <libsoup/soup.h>
#endif

#ifdef HAVE_CMARK
#include <cmark.h>
#endif
DT_MODULE(1)

typedef struct dt_lib_textnotes_t
{
  dt_lib_module_t *self;
  GtkWidget *root;
  GtkWidget *stack;
  GtkTextView *edit_view;
  GtkTextView *preview_view;
  GtkWidget *preview_sw;
  GtkWidget *mode_toggle;
  GtkWidget *mtime_label;
  GtkWidget *completion_popover;
  GtkWidget *completion_tree;
  GtkListStore *completion_model;
  GtkTextMark *completion_mark;
  int preview_alloc_width;
  gchar *path;
  gchar *height_setting;
  int32_t imgid;
  gboolean loading;
  gboolean dirty;
  gboolean rendering;
  guint resize_idle_id;
  guint save_timeout_id;
#ifdef HAVE_HTTP_SERVER
  GHashTable *download_inflight;
#endif
} dt_lib_textnotes_t;

const char *name(dt_lib_module_t *self)
{
  return _("Notes");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "darkroom", "lighttable", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 875;
}

static void _save_now(dt_lib_module_t *self);
static void _render_preview(dt_lib_textnotes_t *d, const char *text);
static void _update_for_current_image(dt_lib_module_t *self);

static gchar *_get_buffer_text(GtkTextBuffer *buffer)
{
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

static gchar *_get_edit_text(dt_lib_textnotes_t *d)
{
  if(!d || !d->edit_view) return g_strdup("");
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  return _get_buffer_text(buffer);
}

static void _set_edit_text(dt_lib_textnotes_t *d, const char *text)
{
  if(!d || !d->edit_view) return;
  d->loading = TRUE;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  gtk_text_buffer_set_text(buffer, text ? text : "", -1);
  d->loading = FALSE;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  if(size) *size = 0;
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->edit_view) return NULL;

  gchar *text = _get_edit_text(d);
  if(!text) return NULL;

  if(size) *size = strlen(text) + 1;
  return text;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params || size <= 0) return 1;

  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->edit_view) return 1;

  gchar *text = g_strndup((const gchar *)params, size);
  _set_edit_text(d, text);
  d->dirty = TRUE;
  _save_now(self);

  g_free(text);
  return 0;
}

void init_presets(dt_lib_module_t *self)
{
  static const char default_text[] =
    "## Todo\n"
    "\n"
    "- [ ] Normalize illuminant & colors\n"
    "- [ ] Normalize contrast & dynamic range\n"
    "- [ ] Fix lens distortion and noise\n"
    "- [ ] Enhance colors\n"
    "\n"
    "## Resources\n"
    "\n"
    "- [Documentation](https://ansel.photos/en/doc)\n"
    "\n"
    "## Lifecycle\n"
    "\n"
    "- Shot: $(EXIF.YEAR)-$(EXIF.MONTH)-$(EXIF.DAY) $(EXIF.HOUR):$(EXIF.MINUTE)\n"
    "- Imported: $(IMPORT.DATE)\n"
    "- Last edited: $(CHANGE.DATE)\n"
    "- Exported: $(EXPORT.DATE)\n"  
    "\n"
    "![](https://images.unsplash.com/photo-1514888286974-6c03e2ca1dba)";

  dt_lib_presets_add(_("Default"), self->plugin_name, self->version(),
                     default_text, sizeof(default_text), TRUE);
}

static int _preview_text_window_width_px(dt_lib_textnotes_t *d)
{
  if(!d || !d->preview_view) return 0;

  GtkTextView *tv = d->preview_view;
  GdkWindow *tw = gtk_text_view_get_window(tv, GTK_TEXT_WINDOW_TEXT);
  if(!tw) return 0;

  return gdk_window_get_width(tw);
}

static void _render_preview_from_edit(dt_lib_textnotes_t *d)
{
  if(!d) return;
  gchar *text = _get_edit_text(d);
  _render_preview(d, text);
  g_free(text);
}

static void _completion_hide(dt_lib_textnotes_t *d)
{
  if(!d) return;
  if(d->completion_popover)
    gtk_widget_hide(d->completion_popover);
  if(d->completion_mark && d->edit_view)
  {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
    gtk_text_buffer_delete_mark(buffer, d->completion_mark);
    d->completion_mark = NULL;
  }
}

static gboolean _completion_match(const char *item, const char *prefix)
{
  if(!prefix || !*prefix) return TRUE;
  if(!item) return FALSE;

  gchar *norm_item = g_utf8_normalize(item, -1, G_NORMALIZE_ALL);
  gchar *norm_prefix = g_utf8_normalize(prefix, -1, G_NORMALIZE_ALL);
  if(!norm_item || !norm_prefix)
  {
    g_free(norm_item);
    g_free(norm_prefix);
    return FALSE;
  }

  gchar *case_item = g_utf8_casefold(norm_item, -1);
  gchar *case_prefix = g_utf8_casefold(norm_prefix, -1);
  const gboolean match = case_item && case_prefix && g_str_has_prefix(case_item, case_prefix);
  g_free(case_item);
  g_free(case_prefix);
  g_free(norm_item);
  g_free(norm_prefix);
  return match;
}

static void _completion_fill(dt_lib_textnotes_t *d, const char *prefix)
{
  if(!d || !d->completion_model) return;
  gtk_list_store_clear(d->completion_model);

  const dt_gtkentry_completion_spec *list = dt_gtkentry_get_default_path_compl_list();
  GtkTreeIter iter;
  for(const dt_gtkentry_completion_spec *l = list; l && l->varname; l++)
  {
    if(!_completion_match(l->varname, prefix)) continue;
    gtk_list_store_append(d->completion_model, &iter);
    gtk_list_store_set(d->completion_model, &iter, COMPL_VARNAME, l->varname,
                       COMPL_DESCRIPTION, _(l->description), -1);
  }
}

static gboolean _completion_find_prefix(dt_lib_textnotes_t *d, GtkTextIter *cursor,
                                        GtkTextIter *start_iter, gchar **prefix_out)
{
  if(!d || !d->edit_view || !cursor || !start_iter || !prefix_out) return FALSE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  GtkTextIter line_start = *cursor;
  gtk_text_iter_set_line_offset(&line_start, 0);

  gchar *line = gtk_text_buffer_get_text(buffer, &line_start, cursor, FALSE);
  if(!line) return FALSE;

  gchar *match = g_strrstr(line, "$(");
  if(!match)
  {
    g_free(line);
    return FALSE;
  }

  if(strchr(match, ')'))
  {
    g_free(line);
    return FALSE;
  }

  gchar *prefix = match + 2;
  for(const gchar *p = prefix; *p; p++)
  {
    if(g_ascii_isspace(*p))
    {
      g_free(line);
      return FALSE;
    }
  }

  const int byte_offset = (int)(match - line);
  const int char_offset = g_utf8_strlen(line, byte_offset);
  *start_iter = line_start;
  gtk_text_iter_set_line_offset(start_iter, char_offset + 2);

  *prefix_out = g_strdup(prefix);
  g_free(line);
  return TRUE;
}

static gboolean _completion_apply_selected(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->completion_tree || !d->edit_view || !d->completion_mark) return FALSE;

  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->completion_tree));
  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(sel, &model, &iter)) return FALSE;

  gchar *varname = NULL;
  gtk_tree_model_get(model, &iter, COMPL_VARNAME, &varname, -1);
  if(!varname) return FALSE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  GtkTextIter start, end;
  gtk_text_buffer_get_iter_at_mark(buffer, &start, d->completion_mark);
  gtk_text_buffer_get_iter_at_mark(buffer, &end, gtk_text_buffer_get_insert(buffer));
  gtk_text_buffer_delete(buffer, &start, &end);

  gchar *insert = g_strdup_printf("%s)", varname);
  gtk_text_buffer_insert(buffer, &start, insert, -1);
  g_free(insert);
  g_free(varname);

  _completion_hide(d);
  return TRUE;
}

static void _completion_update(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->edit_view || !d->completion_popover) return;
  if(!gtk_widget_get_visible(GTK_WIDGET(d->edit_view)))
  {
    _completion_hide(d);
    return;
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  GtkTextIter cursor;
  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));

  GtkTextIter start_iter;
  gchar *prefix = NULL;
  if(!_completion_find_prefix(d, &cursor, &start_iter, &prefix))
  {
    _completion_hide(d);
    return;
  }

  _completion_fill(d, prefix);
  g_free(prefix);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->completion_tree));
  if(!model || gtk_tree_model_iter_n_children(model, NULL) <= 0)
  {
    _completion_hide(d);
    return;
  }

  GtkTreeIter first;
  if(gtk_tree_model_get_iter_first(model, &first))
  {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->completion_tree));
    gtk_tree_selection_select_iter(sel, &first);
  }

  if(d->completion_mark)
    gtk_text_buffer_move_mark(buffer, d->completion_mark, &start_iter);
  else
    d->completion_mark = gtk_text_buffer_create_mark(buffer, NULL, &start_iter, TRUE);

  GdkRectangle rect = { 0 };
  gtk_text_view_get_iter_location(d->edit_view, &cursor, &rect);
  gtk_text_view_buffer_to_window_coords(d->edit_view, GTK_TEXT_WINDOW_WIDGET,
                                        rect.x, rect.y + rect.height, &rect.x, &rect.y);
  GtkWidget *anchor = d->root ? d->root : GTK_WIDGET(d->edit_view);
  gtk_popover_set_relative_to(GTK_POPOVER(d->completion_popover), anchor);
  if(anchor != GTK_WIDGET(d->edit_view))
  {
    int ax = 0, ay = 0;
    if(gtk_widget_translate_coordinates(GTK_WIDGET(d->edit_view), anchor, rect.x, rect.y, &ax, &ay))
    {
      rect.x = ax;
      rect.y = ay;
    }
  }
  if(rect.width <= 0) rect.width = 1;
  rect.height = 1;
  gtk_popover_set_pointing_to(GTK_POPOVER(d->completion_popover), &rect);
  gtk_widget_show_all(d->completion_popover);
#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_popover_popup(GTK_POPOVER(d->completion_popover));
#endif
}

static gboolean _completion_focus_out_idle(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_textnotes_t *d = self ? (dt_lib_textnotes_t *)self->data : NULL;
  if(!d) return G_SOURCE_REMOVE;

  if(d->completion_popover && gtk_widget_get_visible(d->completion_popover))
  {
    GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(d->edit_view));
    if(GTK_IS_WINDOW(toplevel))
    {
      GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(toplevel));
      if(focus && gtk_widget_is_ancestor(focus, d->completion_popover))
        return G_SOURCE_REMOVE;
    }
  }

  _completion_hide(d);
  _save_now(self);
  return G_SOURCE_REMOVE;
}

static gboolean _edit_key_press(GtkWidget *widget, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->completion_popover) return FALSE;
  if(!gtk_widget_get_visible(d->completion_popover)) return FALSE;

  if(event->keyval == GDK_KEY_Escape)
  {
    _completion_hide(d);
    return TRUE;
  }

  if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter || event->keyval == GDK_KEY_Tab)
  {
    if(_completion_apply_selected(self))
      return TRUE;
  }

  (void)widget;
  return FALSE;
}

static gboolean _edit_key_release(GtkWidget *widget, GdkEventKey *event, dt_lib_module_t *self)
{
  _completion_update(self);
  (void)widget;
  (void)event;
  return FALSE;
}

static gboolean _edit_button_release(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _completion_update(self);
  (void)widget;
  (void)event;
  return FALSE;
}

static void _completion_row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column,
                                      dt_lib_module_t *self)
{
  _completion_apply_selected(self);
  (void)tree;
  (void)path;
  (void)column;
}

static void _setup_completion(dt_lib_module_t *self, GtkWidget *textview)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !textview) return;

  d->completion_model = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkWidget *completion_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->completion_model));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(completion_tree), FALSE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("variable"), renderer,
                                                                     "text", COMPL_DESCRIPTION, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(completion_tree), col);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(completion_tree));
  gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
  g_signal_connect(completion_tree, "row-activated", G_CALLBACK(_completion_row_activated), self);

  GtkWidget *completion_sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(completion_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(completion_sw), completion_tree);
  gtk_widget_set_size_request(completion_sw, 360, 200);

  d->completion_popover = gtk_popover_new(NULL);
  gtk_popover_set_position(GTK_POPOVER(d->completion_popover), GTK_POS_BOTTOM);
  gtk_popover_set_relative_to(GTK_POPOVER(d->completion_popover), textview);
  gtk_container_add(GTK_CONTAINER(d->completion_popover), completion_sw);
  d->completion_tree = completion_tree;
}

static gboolean _alloc_row_buffers(const int width, guchar **row_in, guchar **row_out)
{
  *row_in = g_malloc((size_t)width * 4);
  *row_out = g_malloc((size_t)width * 4);
  if(!*row_in || !*row_out)
  {
    g_free(*row_in);
    g_free(*row_out);
    *row_in = NULL;
    *row_out = NULL;
    return FALSE;
  }
  return TRUE;
}

static void _free_row_buffers(guchar *row_in, guchar *row_out)
{
  g_free(row_in);
  g_free(row_out);
}

static void _colorcorrect_row(cmsHTRANSFORM transform, guchar *src, const int width,
                              const int n_channels, const gboolean has_alpha,
                              guchar *row_in, guchar *row_out)
{
  for(int x = 0; x < width; x++)
  {
    const int s = x * n_channels;
    const int d = x * 4;
    row_in[d + 0] = src[s + 0];
    row_in[d + 1] = src[s + 1];
    row_in[d + 2] = src[s + 2];
    row_in[d + 3] = has_alpha ? src[s + 3] : 255;
  }

  cmsDoTransform(transform, row_in, row_out, width);

  for(int x = 0; x < width; x++)
  {
    const int s = x * 4;
    const int d = x * n_channels;
    src[d + 0] = row_out[s + 2];
    src[d + 1] = row_out[s + 1];
    src[d + 2] = row_out[s + 0];
    if(has_alpha)
      src[d + 3] = row_out[s + 3];
  }
}

static void _colorcorrect_pixbuf(GdkPixbuf *pixbuf)
{
  if(!pixbuf) return;

  cmsHTRANSFORM transform = NULL;
  pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
  if(darktable.color_profiles->transform_srgb_to_display)
    transform = darktable.color_profiles->transform_srgb_to_display;

  if(!transform)
  {
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    return;
  }

  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  const int n_channels = gdk_pixbuf_get_n_channels(pixbuf);
  if(width <= 0 || height <= 0 || n_channels < 3)
  {
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    return;
  }

  guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
  if(!pixels)
  {
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    return;
  }

  const gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);

#ifdef _OPENMP
  const int nthreads = omp_get_max_threads();
  guchar **rows_in = g_malloc0((size_t)nthreads * sizeof(*rows_in));
  guchar **rows_out = g_malloc0((size_t)nthreads * sizeof(*rows_out));
  gboolean ok = TRUE;
  for(int i = 0; i < nthreads; i++)
  {
    if(!_alloc_row_buffers(width, &rows_in[i], &rows_out[i]))
    {
      ok = FALSE;
      break;
    }
  }
  if(!ok)
  {
    for(int i = 0; i < nthreads; i++)
      _free_row_buffers(rows_in[i], rows_out[i]);
    g_free(rows_in);
    g_free(rows_out);
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    return;
  }

#pragma omp parallel default(none) \
  dt_omp_firstprivate(pixels, rowstride, n_channels, width, height, transform, has_alpha, rows_in, rows_out)
  {
    const int tid = omp_get_thread_num();
    guchar *row_in = rows_in[tid];
    guchar *row_out = rows_out[tid];

#pragma omp for schedule(static)
    for(int y = 0; y < height; y++)
    {
      guchar *src = pixels + (size_t)y * rowstride;
      _colorcorrect_row(transform, src, width, n_channels, has_alpha, row_in, row_out);
    }
  }

  for(int i = 0; i < nthreads; i++)
    _free_row_buffers(rows_in[i], rows_out[i]);
  g_free(rows_in);
  g_free(rows_out);
#else
  guchar *row_in = NULL;
  guchar *row_out = NULL;
  if(!_alloc_row_buffers(width, &row_in, &row_out))
  {
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    return;
  }
  for(int y = 0; y < height; y++)
  {
    guchar *src = pixels + (size_t)y * rowstride;
    _colorcorrect_row(transform, src, width, n_channels, has_alpha, row_in, row_out);
  }
  _free_row_buffers(row_in, row_out);
#endif

  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
}

static void _toggle_mode(GtkToggleButton *button, dt_lib_module_t *self);
static void _load_for_image(dt_lib_module_t *self, const int32_t imgid);
static gboolean _refresh_preview_idle(gpointer user_data);

static void _open_uri(const char *uri)
{
  if(!uri || !*uri) return;

  GtkWindow *win = NULL;
  if(darktable.gui && darktable.gui->ui)
    win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));

  GError *error = NULL;
  const gboolean ok = gtk_show_uri_on_window(win, uri, GDK_CURRENT_TIME, &error);
  if(!ok && error)
  {
    dt_control_log(_("could not open link: %s"), error->message);
    g_clear_error(&error);
  }
}

static gchar *_expand_text_for_preview(dt_lib_textnotes_t *d, const char *source_text)
{
  if(!d || d->imgid <= 0) return NULL;

  dt_variables_params_t *vp;
  dt_variables_params_init(&vp);

  char input_dir[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(d->imgid, input_dir, sizeof(input_dir), &from_cache, __FUNCTION__);

  vp->filename = input_dir;
  vp->jobcode = "textnotes";
  vp->imgid = d->imgid;
  vp->sequence = 0;
  vp->escape_markup = FALSE;

  gchar *tmp = g_strdup(source_text ? source_text : "");
  gchar *expanded = dt_variables_expand(vp, tmp, TRUE);
  g_free(tmp);
  dt_variables_params_destroy(vp);
  return expanded;
}

#ifdef HAVE_CMARK
typedef struct dt_textnotes_list_state_t
{
  gboolean ordered;
  int index;
} dt_textnotes_list_state_t;

typedef struct dt_textnotes_image_state_t
{
  gboolean suppress_text;
  gboolean tag_added;
} dt_textnotes_image_state_t;

static void _buffer_append_newline(GtkTextBuffer *buffer)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  if(gtk_text_iter_is_start(&end)) return;
  GtkTextIter it = end;
  if(gtk_text_iter_backward_char(&it) && gtk_text_iter_get_char(&it) != '\n')
    gtk_text_buffer_insert(buffer, &end, "\n", 1);
}

static void _buffer_append_blankline(GtkTextBuffer *buffer)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);
  if(gtk_text_iter_is_start(&end)) return;

  GtkTextIter it = end;
  if(gtk_text_iter_backward_char(&it))
  {
    if(gtk_text_iter_get_char(&it) == '\n')
    {
      GtkTextIter it2 = it;
      if(gtk_text_iter_backward_char(&it2) && gtk_text_iter_get_char(&it2) == '\n') return;
      gtk_text_buffer_insert(buffer, &end, "\n", 1);
      return;
    }
  }

  gtk_text_buffer_insert(buffer, &end, "\n\n", 2);
}

static void _insert_with_tags(GtkTextBuffer *buffer, const char *text, GPtrArray *tags)
{
  if(!text || !*text) return;
  GtkTextIter start, end;
  gtk_text_buffer_get_end_iter(buffer, &start);
  GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE);
  end = start;
  gtk_text_buffer_insert(buffer, &end, text, -1);
  gtk_text_buffer_get_iter_at_mark(buffer, &start, mark);
  for(guint i = 0; i < tags->len; i++)
    gtk_text_buffer_apply_tag(buffer, g_ptr_array_index(tags, i), &start, &end);
  gtk_text_buffer_delete_mark(buffer, mark);
}

static void _emit_list_prefix(GtkTextBuffer *buffer, GArray *list_stack, const gboolean checkbox,
                              const gboolean checked, const int checklist_line)
{
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buffer, &end);

  const int depth = list_stack->len;
  for(int i = 1; i < depth; i++) gtk_text_buffer_insert(buffer, &end, "  ", 2);

  dt_textnotes_list_state_t *st = NULL;
  if(depth > 0)
    st = &g_array_index(list_stack, dt_textnotes_list_state_t, depth - 1);

  if(checkbox)
  {
    GtkTextTag *checkbox_tag = gtk_text_buffer_create_tag(buffer, NULL, "scale", 1.1, NULL);
    if(checklist_line > 0)
      g_object_set_data(G_OBJECT(checkbox_tag), "checklist_line", GINT_TO_POINTER(checklist_line));
    GtkTextIter start = end;
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE);
    gtk_text_buffer_insert(buffer, &end, checked ? "\u2611" : "\u2610", -1);
    gtk_text_buffer_get_iter_at_mark(buffer, &start, mark);
    gtk_text_buffer_apply_tag(buffer, checkbox_tag, &start, &end);
    gtk_text_buffer_delete_mark(buffer, mark);
    gtk_text_buffer_insert(buffer, &end, " ", 1);
    if(st && st->ordered) st->index++;
    return;
  }

  if(st && st->ordered)
  {
    gchar *num = g_strdup_printf("%d. ", st->index);
    gtk_text_buffer_insert(buffer, &end, num, -1);
    g_free(num);
    st->index++;
  }
  else
  {
    gtk_text_buffer_insert(buffer, &end, "- ", 2);
  }
}

static void _collect_text_tag(GtkTextTag *tag, gpointer user_data)
{
  GPtrArray *tags = (GPtrArray *)user_data;
  g_ptr_array_add(tags, tag);
}

static void _clear_tag_table(GtkTextBuffer *buffer)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  GPtrArray *tags = g_ptr_array_new();
  gtk_text_tag_table_foreach(table, _collect_text_tag, tags);
  for(guint i = 0; i < tags->len; i++)
    gtk_text_tag_table_remove(table, g_ptr_array_index(tags, i));
  g_ptr_array_free(tags, TRUE);
}

typedef struct dt_textnotes_tags_t
{
  GtkTextTag *bold;
  GtkTextTag *italic;
  GtkTextTag *mono;
  GtkTextTag *h1;
  GtkTextTag *h2;
  GtkTextTag *h3;
} dt_textnotes_tags_t;

static dt_textnotes_tags_t _create_preview_tags(GtkTextBuffer *buffer)
{
  dt_textnotes_tags_t tags = { 0 };
  tags.bold = gtk_text_buffer_create_tag(buffer, "tn_bold", "weight", PANGO_WEIGHT_BOLD, NULL);
  tags.italic = gtk_text_buffer_create_tag(buffer, "tn_italic", "style", PANGO_STYLE_ITALIC, NULL);
  tags.mono = gtk_text_buffer_create_tag(buffer, "tn_mono", "family", "monospace", NULL);
  tags.h1 = gtk_text_buffer_create_tag(buffer, "tn_h1", "weight", PANGO_WEIGHT_BOLD, "scale", 1.4, NULL);
  tags.h2 = gtk_text_buffer_create_tag(buffer, "tn_h2", "weight", PANGO_WEIGHT_BOLD, "scale", 1.25, NULL);
  tags.h3 = gtk_text_buffer_create_tag(buffer, "tn_h3", "weight", PANGO_WEIGHT_BOLD, "scale", 1.15, NULL);
  return tags;
}

static void _pop_active_tag(GPtrArray *active_tags)
{
  if(active_tags->len > 0)
    g_ptr_array_remove_index(active_tags, active_tags->len - 1);
}

static void _push_link_tag(GtkTextBuffer *buffer, GPtrArray *active_tags, const char *url)
{
  GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, NULL,
                                               "underline", PANGO_UNDERLINE_SINGLE,
                                               NULL);
  if(url && *url)
    g_object_set_data_full(G_OBJECT(tag), "href", g_strdup(url), g_free);
  g_ptr_array_add(active_tags, tag);
}

static void _insert_mono_text(GtkTextBuffer *buffer, GtkTextTag *tag_mono, const char *lit)
{
  if(!lit || !*lit) return;
  GtkTextIter start, end;
  gtk_text_buffer_get_end_iter(buffer, &start);
  GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE);
  end = start;
  gtk_text_buffer_insert(buffer, &end, lit, -1);
  gtk_text_buffer_get_iter_at_mark(buffer, &start, mark);
  gtk_text_buffer_apply_tag(buffer, tag_mono, &start, &end);
  gtk_text_buffer_delete_mark(buffer, mark);
}

static void _emit_pending_list_prefix(GtkTextBuffer *buffer, GArray *list_stack,
                                      gboolean *item_pending_prefix)
{
  if(*item_pending_prefix)
  {
    _emit_list_prefix(buffer, list_stack, FALSE, FALSE, 0);
    *item_pending_prefix = FALSE;
  }
}

static const char *_handle_list_text_prefix(GtkTextBuffer *buffer, GArray *list_stack,
                                            gboolean *item_pending_prefix,
                                            const char *lit, const int line_no)
{
  if(!lit || !*lit) return lit;

  if(*item_pending_prefix && lit[0] == '[' && lit[2] == ']'
     && (lit[1] == ' ' || lit[1] == 'x' || lit[1] == 'X'))
  {
    const gboolean checked = (lit[1] == 'x' || lit[1] == 'X');
    _emit_list_prefix(buffer, list_stack, TRUE, checked, line_no);
    *item_pending_prefix = FALSE;
    int offset = 3;
    if(lit[3] == ' ') offset = 4;
    return lit + offset;
  }

  if(*item_pending_prefix)
  {
    _emit_list_prefix(buffer, list_stack, FALSE, FALSE, 0);
    *item_pending_prefix = FALSE;
  }

  return lit;
}

static void _list_push(GArray *list_stack, cmark_node *node)
{
  dt_textnotes_list_state_t st = {
    .ordered = (cmark_node_get_list_type(node) == CMARK_ORDERED_LIST),
    .index = cmark_node_get_list_start(node)
  };
  g_array_append_val(list_stack, st);
}

static void _list_pop(GtkTextBuffer *buffer, GArray *list_stack)
{
  if(list_stack->len > 0) g_array_remove_index(list_stack, list_stack->len - 1);
  _buffer_append_blankline(buffer);
}

static void _list_item_enter(GtkTextBuffer *buffer, gboolean *in_list_item, gboolean *item_pending_prefix)
{
  _buffer_append_newline(buffer);
  *in_list_item = TRUE;
  *item_pending_prefix = TRUE;
}

static void _list_item_leave(GtkTextBuffer *buffer, gboolean *in_list_item, gboolean *item_pending_prefix)
{
  _buffer_append_newline(buffer);
  *in_list_item = FALSE;
  *item_pending_prefix = FALSE;
}

static gboolean _is_remote_url(const char *url)
{
  if(!url || !*url) return FALSE;
  return g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://");
}

static gchar *_remote_cache_path(const char *url)
{
  if(!url || !*url) return NULL;

  gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url, -1);
  if(!hash) return NULL;

  const char *end = strchr(url, '?');
  if(!end) end = url + strlen(url);
  const char *slash = end;
  while(slash > url && *slash != '/') slash--;
  if(*slash == '/') slash++;
  const char *dot = NULL;
  for(const char *p = end - 1; p > slash; p--)
  {
    if(*p == '.')
    {
      dot = p;
      break;
    }
  }

  gchar *filename = NULL;
  if(dot && (end - dot) <= 8)
    filename = g_strconcat(hash, dot, NULL);
  else
    filename = g_strdup(hash);

  g_free(hash);

  gchar *cache_dir = g_build_filename(g_get_user_cache_dir(), "ansel", "downloads", NULL);
  gchar *path = g_build_filename(cache_dir, filename, NULL);
  g_free(cache_dir);
  g_free(filename);
  return path;
}

#ifdef HAVE_HTTP_SERVER
typedef struct dt_textnotes_fetch_t
{
  dt_lib_module_t *self;
  dt_lib_textnotes_t *d;
  gchar *url;
  gchar *path;
} dt_textnotes_fetch_t;

static SoupSession *_textnotes_soup_session(void)
{
  static SoupSession *session = NULL;
  if(session) return session;
  session = soup_session_new();
  if(session)
    g_object_set(session, "timeout", 10, "user-agent", "Ansel", NULL);
  return session;
}

static void _finish_remote_download(dt_textnotes_fetch_t *fetch, gboolean ok)
{
  if(fetch->d && fetch->d->download_inflight && fetch->url)
    g_hash_table_remove(fetch->d->download_inflight, fetch->url);

  if(ok && fetch->self)
    g_idle_add(_refresh_preview_idle, fetch->self);

  g_free(fetch->url);
  g_free(fetch->path);
  g_free(fetch);
}

#if LIBSOUP_VERSION_MAJOR >= 3
static void _remote_download_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
  dt_textnotes_fetch_t *fetch = (dt_textnotes_fetch_t *)user_data;
  SoupSession *session = SOUP_SESSION(source);
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(session, res, &error);

  if(!bytes || error)
  {
    if(error) g_clear_error(&error);
    if(bytes) g_bytes_unref(bytes);
    _finish_remote_download(fetch, FALSE);
    return;
  }

  const gsize len = g_bytes_get_size(bytes);
  const void *data = g_bytes_get_data(bytes, NULL);
  gboolean ok = FALSE;
  if(fetch->path && data && len > 0)
    ok = g_file_set_contents(fetch->path, data, (gssize)len, NULL);

  g_bytes_unref(bytes);
  _finish_remote_download(fetch, ok);
}
#else
static void _remote_download_cb(SoupSession *session, SoupMessage *msg, gpointer user_data)
{
  dt_textnotes_fetch_t *fetch = (dt_textnotes_fetch_t *)user_data;
  gboolean ok = FALSE;

  if(msg->status_code == SOUP_STATUS_OK && msg->response_body && msg->response_body->data)
  {
    ok = g_file_set_contents(fetch->path,
                             msg->response_body->data,
                             (gssize)msg->response_body->length,
                             NULL);
  }

  _finish_remote_download(fetch, ok);
}
#endif

static void _queue_remote_download(dt_lib_module_t *self, dt_lib_textnotes_t *d,
                                   const char *url, const char *path)
{
  if(!self || !d || !url || !path) return;
  if(!d->download_inflight)
    d->download_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if(g_hash_table_contains(d->download_inflight, url)) return;

  gchar *cache_dir = g_build_filename(darktable.cachedir, "downloads", NULL);
  g_mkdir_with_parents(cache_dir, 0700);
  g_free(cache_dir);

  SoupSession *session = _textnotes_soup_session();
  if(!session) return;

  SoupMessage *msg = soup_message_new("GET", url);
  if(!msg) return;

  dt_textnotes_fetch_t *fetch = g_new0(dt_textnotes_fetch_t, 1);
  fetch->self = self;
  fetch->d = d;
  fetch->url = g_strdup(url);
  fetch->path = g_strdup(path);

  g_hash_table_add(d->download_inflight, g_strdup(url));

#if LIBSOUP_VERSION_MAJOR >= 3
  soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL, _remote_download_cb, fetch);
#else
  soup_session_queue_message(session, msg, _remote_download_cb, fetch);
#endif
}
#endif

static gchar *_resolve_image_path(const char *url, const char *base_dir)
{
  if(!url || !*url) return NULL;
  if(_is_remote_url(url) || g_str_has_prefix(url, "ftp://"))
    return NULL;

  if(g_str_has_prefix(url, "file://"))
    return g_filename_from_uri(url, NULL, NULL);

  if(g_path_is_absolute(url))
  {
    gchar *unescaped = g_uri_unescape_string(url, NULL);
    return unescaped ? unescaped : g_strdup(url);
  }

  if(!base_dir || !*base_dir)
    return NULL;

  gchar *unescaped = g_uri_unescape_string(url, NULL);
  gchar *result = g_build_filename(base_dir, unescaped ? unescaped : url, NULL);
  g_free(unescaped);
  return result;
}

static gchar *_get_image_base_dir(dt_lib_textnotes_t *d)
{
  if(!d || d->imgid <= 0) return NULL;

  char *txt_path = dt_image_get_text_path(d->imgid);
  if(txt_path)
  {
    gchar *dir = g_path_get_dirname(txt_path);
    g_free(txt_path);
    return dir;
  }

  gboolean from_cache = FALSE;
  char image_path[PATH_MAX] = { 0 };
  dt_image_full_path(d->imgid, image_path, sizeof(image_path), &from_cache, __FUNCTION__);
  if(image_path[0] == '\0') return NULL;

  return g_path_get_dirname(image_path);
}

static void _queue_preview_refresh(dt_lib_textnotes_t *d, const guint delay_ms)
{
  if(!d || d->resize_idle_id) return;
  d->resize_idle_id = g_timeout_add(delay_ms, _refresh_preview_idle, d->self);
}

static int _get_preview_scale(dt_lib_textnotes_t *d)
{
  int scale = 1;
  if(d && d->preview_view)
    scale = gtk_widget_get_scale_factor(GTK_WIDGET(d->preview_view));
  if(scale <= 0) scale = 1;
  return scale;
}

static int _compute_max_image_width(dt_lib_textnotes_t *d, const int scale, gboolean *have_device)
{
  int device_w = _preview_text_window_width_px(d);
  if(have_device) *have_device = (device_w > 0);

  int max_w = 0;
  if(device_w > 0)
  {
    const int dpad = (scale > 1) ? 3 : 2; // slightly tighter on HiDPI
    if(device_w > dpad) device_w -= dpad;
    max_w = device_w / scale;
    if(max_w < 1) max_w = 1;
  }

  if(max_w <= 0 && d->preview_view)
  {
    GdkRectangle rect = { 0 };
    gtk_text_view_get_visible_rect(d->preview_view, &rect);
    if(rect.width > 0) max_w = rect.width;
  }
  if(max_w <= 0 && d->preview_view)
    max_w = gtk_widget_get_allocated_width(GTK_WIDGET(d->preview_view));
  if(max_w <= 0 && d->preview_sw)
    max_w = gtk_widget_get_allocated_width(GTK_WIDGET(d->preview_sw));
  if(max_w <= 0 && d->root)
    max_w = gtk_widget_get_allocated_width(d->root);

  if((!have_device || !*have_device) && d->preview_view)
  {
    const int margin = gtk_text_view_get_left_margin(d->preview_view)
                       + gtk_text_view_get_right_margin(d->preview_view);
    if(margin > 0 && max_w > margin) max_w -= margin;

    GtkStyleContext *ctx = gtk_widget_get_style_context(GTK_WIDGET(d->preview_view));
    GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(d->preview_view));
    GtkBorder padding = { 0 }, border = { 0 };
    gtk_style_context_get_padding(ctx, state, &padding);
    gtk_style_context_get_border(ctx, state, &border);
    const int chrome = padding.left + padding.right + border.left + border.right;
    if(chrome > 0 && max_w > chrome) max_w -= chrome;
  }

  if(max_w > 2) max_w -= 2;
  return max_w;
}

static GdkPixbuf *_load_scaled_pixbuf(const char *path, const int target_w, GError **error)
{
  if(target_w > 0)
    return gdk_pixbuf_new_from_file_at_scale(path, target_w, -1, TRUE, error);
  return gdk_pixbuf_new_from_file(path, error);
}

static void _insert_pixbuf_widget(dt_lib_textnotes_t *d, GtkTextBuffer *buffer,
                                  GdkPixbuf *pixbuf, const int max_w)
{
  GtkWidget *image = gtk_image_new_from_pixbuf(pixbuf);
  if(max_w > 0)
    gtk_widget_set_size_request(image, max_w, -1);
  gtk_widget_set_halign(image, GTK_ALIGN_START);
  gtk_widget_set_margin_top(image, 2);
  gtk_widget_set_margin_bottom(image, 6);

  GtkTextIter iter;
  gtk_text_buffer_get_end_iter(buffer, &iter);
  GtkTextChildAnchor *anchor = gtk_text_buffer_create_child_anchor(buffer, &iter);
  gtk_text_view_add_child_at_anchor(d->preview_view, image, anchor);
  gtk_widget_show(image);
}

static gboolean _insert_markdown_image(dt_lib_textnotes_t *d, GtkTextBuffer *buffer,
                                       const char *url, const char *fallback_url,
                                       const char *base_dir)
{
  const char *remote_url = NULL;
  if(_is_remote_url(url)) remote_url = url;
  else if(_is_remote_url(fallback_url)) remote_url = fallback_url;

  gchar *path = NULL;
  if(remote_url)
  {
    path = _remote_cache_path(remote_url);
#ifdef HAVE_HTTP_SERVER
    if(path && !g_file_test(path, G_FILE_TEST_EXISTS))
      _queue_remote_download(d->self, d, remote_url, path);
#endif
  }
  else
  {
    path = _resolve_image_path(url, base_dir);
    if(!path && fallback_url && (!url || g_strcmp0(url, fallback_url) != 0))
      path = _resolve_image_path(fallback_url, base_dir);
  }
  if(!path) return FALSE;

  if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    g_free(path);
    return FALSE;
  }

  const int scale = _get_preview_scale(d);
  gboolean have_device = FALSE;
  int max_w = _compute_max_image_width(d, scale, &have_device);
  if(!have_device) _queue_preview_refresh(d, 60);
  if(max_w <= 0)
  {
    _queue_preview_refresh(d, 60);
    g_free(path);
    return TRUE;
  }

  const int target_w = max_w * scale;
  GError *error = NULL;
  GdkPixbuf *pixbuf = _load_scaled_pixbuf(path, target_w, &error);
  if(!pixbuf)
  {
    if(error) g_clear_error(&error);
    g_free(path);
    return FALSE;
  }

  _colorcorrect_pixbuf(pixbuf);
  _insert_pixbuf_widget(d, buffer, pixbuf, max_w);
  g_object_unref(pixbuf);

  g_free(path);
  return TRUE;
}

static GArray *_build_line_offsets(const char *text)
{
  GArray *offsets = g_array_new(FALSE, FALSE, sizeof(gsize));
  gsize off = 0;
  g_array_append_val(offsets, off);
  if(!text) return offsets;
  for(const char *p = text; *p; p++, off++)
  {
    if(*p == '\n')
    {
      gsize next = off + 1;
      g_array_append_val(offsets, next);
    }
  }
  return offsets;
}

static gchar *_normalize_markdown_images(const char *text)
{
  if(!text) return g_strdup("");

  GString *out = g_string_sized_new(strlen(text) + 16);
  const char *p = text;
  while(*p)
  {
    if(p[0] == '!' && p[1] == '[')
    {
      const char *alt_end = strchr(p + 2, ']');
      if(alt_end && alt_end[1] == '(')
      {
        const char *dest_start = alt_end + 2;
        const char *line_end = strchr(dest_start, '\n');
        if(!line_end) line_end = dest_start + strlen(dest_start);
        const char *close_paren = memchr(dest_start, ')', (size_t)(line_end - dest_start));
        if(close_paren && close_paren > dest_start)
        {
          const char *s = dest_start;
          const char *e = close_paren;
          while(s < e && g_ascii_isspace(*s)) s++;
          while(e > s && g_ascii_isspace(*(e - 1))) e--;

          gboolean has_space = FALSE;
          gboolean has_quote = FALSE;
          for(const char *q = s; q < e; q++)
          {
            if(g_ascii_isspace(*q)) has_space = TRUE;
            if(*q == '"' || *q == '\'') has_quote = TRUE;
          }

          if(has_space && !has_quote && s < e && *s != '<')
          {
            g_string_append_len(out, p, (gsize)(dest_start - p));
            g_string_append_c(out, '<');
            g_string_append_len(out, s, (gsize)(e - s));
            g_string_append(out, ">)");
            p = close_paren + 1;
            continue;
          }
        }
      }
    }

    g_string_append_c(out, *p);
    p++;
  }

  return g_string_free(out, FALSE);
}

static gchar *_extract_image_dest_from_source(const char *text, const GArray *offsets, cmark_node *node)
{
  if(!text || !offsets || offsets->len == 0) return NULL;
  const int sl = cmark_node_get_start_line(node);
  const int sc = cmark_node_get_start_column(node);
  if(sl <= 0 || sc <= 0 || sl > (int)offsets->len) return NULL;

  const gsize line_start = g_array_index(offsets, gsize, sl - 1);
  const gsize line_end = (sl < (int)offsets->len)
                           ? g_array_index(offsets, gsize, sl) - 1
                           : strlen(text);
  if(line_start >= line_end) return NULL;

  gsize start = line_start + (gsize)(sc - 1);
  if(start >= line_end) start = line_start;

  const char *line = text + line_start;
  const gsize line_len = line_end - line_start;
  const char *p = line + (start - line_start);
  const char *line_endp = line + line_len;

  const char *open_paren = NULL;
  for(const char *q = p; q < line_endp; q++)
  {
    if(*q == '(')
    {
      open_paren = q;
      break;
    }
  }
  if(!open_paren || open_paren + 1 >= line_endp) return NULL;

  const char *dest_start = open_paren + 1;
  const char *dest_end = NULL;

  if(*dest_start == '<')
  {
    const char *close = strchr(dest_start + 1, '>');
    if(close && close < line_endp) dest_end = close;
  }
  else
  {
    for(const char *q = line_endp - 1; q > dest_start; q--)
    {
      if(*q == ')')
      {
        dest_end = q;
        break;
      }
    }
  }

  if(!dest_end || dest_end <= dest_start) return NULL;

  gchar *raw = g_strndup(dest_start, dest_end - dest_start);
  if(!raw) return NULL;

  gchar *trimmed = g_strstrip(raw);
  if(trimmed[0] == '<' && trimmed[strlen(trimmed) - 1] == '>')
  {
    trimmed[strlen(trimmed) - 1] = '\0';
    trimmed++;
  }

  GString *out = g_string_new(NULL);
  for(const char *q = trimmed; *q; q++)
  {
    if(*q == '\\' && q[1] != '\0')
    {
      q++;
      g_string_append_c(out, *q);
    }
    else
    {
      g_string_append_c(out, *q);
    }
  }

  gchar *result = g_string_free(out, FALSE);
  g_free(raw);
  return result;
}
#endif

static void _render_preview(dt_lib_textnotes_t *d, const char *text)
{
  d->rendering = TRUE;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->preview_view);
  gtk_text_buffer_set_text(buffer, "", -1);

#ifdef HAVE_CMARK
  _clear_tag_table(buffer);
  dt_textnotes_tags_t tags = _create_preview_tags(buffer);
  GPtrArray *active_tags = g_ptr_array_new();

  const char *source_text = text ? text : "";
  gchar *expanded = _expand_text_for_preview(d, source_text);
  const char *render_text = expanded ? expanded : source_text;

  gchar *normalized = _normalize_markdown_images(render_text);
  cmark_node *doc = cmark_parse_document(normalized,
                                         strlen(normalized),
                                         CMARK_OPT_DEFAULT | CMARK_OPT_SOURCEPOS);
  if(!doc)
  {
    g_ptr_array_free(active_tags, TRUE);
    g_free(normalized);
    g_free(expanded);
    d->rendering = FALSE;
    return;
  }

  cmark_iter *it = cmark_iter_new(doc);
  GArray *list_stack = g_array_new(FALSE, FALSE, sizeof(dt_textnotes_list_state_t));
  GArray *image_stack = g_array_new(FALSE, FALSE, sizeof(dt_textnotes_image_state_t));
  gboolean in_list_item = FALSE;
  gboolean item_pending_prefix = FALSE;

  GArray *line_offsets = _build_line_offsets(render_text);
  gchar *base_dir = _get_image_base_dir(d);

  for(cmark_event_type ev = cmark_iter_next(it); ev != CMARK_EVENT_DONE; ev = cmark_iter_next(it))
  {
    cmark_node *node = cmark_iter_get_node(it);
    const cmark_node_type t = cmark_node_get_type(node);
    const gboolean entering = (ev == CMARK_EVENT_ENTER);

    switch(t)
    {
      case CMARK_NODE_PARAGRAPH:
        if(!entering)
        {
          if(in_list_item) _buffer_append_newline(buffer);
          else _buffer_append_blankline(buffer);
        }
        break;
      case CMARK_NODE_TEXT:
        if(entering)
        {
          const char *lit = cmark_node_get_literal(node);
          if(!lit) break;
          if(image_stack->len > 0)
          {
            dt_textnotes_image_state_t *st =
              &g_array_index(image_stack, dt_textnotes_image_state_t, image_stack->len - 1);
            if(st->suppress_text) break;
          }
          lit = _handle_list_text_prefix(buffer, list_stack, &item_pending_prefix,
                                         lit, cmark_node_get_start_line(node));
          _insert_with_tags(buffer, lit, active_tags);
        }
        break;
      case CMARK_NODE_SOFTBREAK:
      case CMARK_NODE_LINEBREAK:
        if(entering)
        {
          GtkTextIter it_end;
          gtk_text_buffer_get_end_iter(buffer, &it_end);
          gtk_text_buffer_insert(buffer, &it_end, "\n", 1);
        }
        break;
      case CMARK_NODE_EMPH:
        if(entering)
          g_ptr_array_add(active_tags, tags.italic);
        else
          _pop_active_tag(active_tags);
        break;
      case CMARK_NODE_STRONG:
        if(entering)
          g_ptr_array_add(active_tags, tags.bold);
        else
          _pop_active_tag(active_tags);
        break;
      case CMARK_NODE_CODE:
        if(entering)
        {
          _emit_pending_list_prefix(buffer, list_stack, &item_pending_prefix);
          _insert_mono_text(buffer, tags.mono, cmark_node_get_literal(node));
        }
        break;
      case CMARK_NODE_CODE_BLOCK:
        if(entering)
        {
          _buffer_append_blankline(buffer);
          _emit_pending_list_prefix(buffer, list_stack, &item_pending_prefix);
          _insert_mono_text(buffer, tags.mono, cmark_node_get_literal(node));
          _buffer_append_blankline(buffer);
        }
        break;
      case CMARK_NODE_HEADING:
        if(entering)
        {
          GtkTextTag *tag = tags.h3;
          const int level = cmark_node_get_heading_level(node);
          if(level <= 1) tag = tags.h1;
          else if(level == 2) tag = tags.h2;
          g_ptr_array_add(active_tags, tag);
        }
        else
        {
          _pop_active_tag(active_tags);
          _buffer_append_blankline(buffer);
        }
        break;
      case CMARK_NODE_LINK:
        if(entering)
          _push_link_tag(buffer, active_tags, cmark_node_get_url(node));
        else
          _pop_active_tag(active_tags);
        break;
      case CMARK_NODE_IMAGE:
        if(entering)
        {
          _emit_pending_list_prefix(buffer, list_stack, &item_pending_prefix);
          const char *url = cmark_node_get_url(node);
          gchar *fallback = _extract_image_dest_from_source(render_text, line_offsets, node);
          const gboolean inlined = _insert_markdown_image(d, buffer, url, fallback, base_dir);
          g_free(fallback);
          dt_textnotes_image_state_t st = { .suppress_text = inlined, .tag_added = FALSE };
          if(!inlined)
          {
            _push_link_tag(buffer, active_tags, url);
            st.tag_added = TRUE;
          }
          g_array_append_val(image_stack, st);
        }
        else if(image_stack->len > 0)
        {
          dt_textnotes_image_state_t st =
            g_array_index(image_stack, dt_textnotes_image_state_t, image_stack->len - 1);
          if(st.tag_added) _pop_active_tag(active_tags);
          g_array_remove_index(image_stack, image_stack->len - 1);
        }
        break;
      case CMARK_NODE_LIST:
        if(entering)
          _list_push(list_stack, node);
        else
          _list_pop(buffer, list_stack);
        break;
      case CMARK_NODE_ITEM:
        if(entering)
          _list_item_enter(buffer, &in_list_item, &item_pending_prefix);
        else
          _list_item_leave(buffer, &in_list_item, &item_pending_prefix);
        break;
      default:
        break;
    }
  }

  g_array_free(list_stack, TRUE);
  g_array_free(image_stack, TRUE);
  g_ptr_array_free(active_tags, TRUE);
  cmark_iter_free(it);
  cmark_node_free(doc);
  g_array_free(line_offsets, TRUE);
  g_free(base_dir);
  g_free(normalized);
  g_free(expanded);
#else
  const char *source_text = text ? text : "";
  gchar *expanded = _expand_text_for_preview(d, source_text);
  gtk_text_buffer_set_text(buffer, expanded ? expanded : source_text, -1);
  g_free(expanded);
#endif
  d->rendering = FALSE;
}

static void _clear_mtime_label(dt_lib_textnotes_t *d)
{
  if(!d || !d->mtime_label) return;
  gtk_label_set_text(GTK_LABEL(d->mtime_label), "");
  gtk_widget_set_visible(d->mtime_label, FALSE);
}

static void _update_mtime_label(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d->mtime_label) return;

  char *path = (d->imgid > 0) ? dt_image_get_text_path(d->imgid) : NULL;
  if(!path)
  {
    _clear_mtime_label(d);
    return;
  }

  GStatBuf statbuf;
  if(g_stat(path, &statbuf) != 0)
  {
    _clear_mtime_label(d);
    g_free(path);
    return;
  }

  GDateTime *gdt = g_date_time_new_from_unix_local((gint64)statbuf.st_mtime);
  char local[128] = { 0 };
  if(gdt && dt_datetime_gdatetime_to_local(local, sizeof(local), gdt, FALSE, FALSE))
  {
    gchar *text = g_strdup_printf(_("Last modified: %s"), local);
    gchar *markup = g_markup_printf_escaped("<i>%s</i>", text);
    gtk_label_set_markup(GTK_LABEL(d->mtime_label), markup);
    gtk_widget_set_visible(d->mtime_label, TRUE);
    g_free(markup);
    g_free(text);
  }
  else
  {
    _clear_mtime_label(d);
  }

  if(gdt) g_date_time_unref(gdt);
  g_free(path);
}

static void _toggle_checklist_at_line(dt_lib_module_t *self, const int line_no)
{
  if(line_no < 1) return;

  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(d->edit_view);
  GtkTextIter line_start, line_end;
  gtk_text_buffer_get_iter_at_line(buffer, &line_start, line_no - 1);
  line_end = line_start;
  gtk_text_iter_forward_to_line_end(&line_end);

  GtkTextIter s_space, e_space, s_x, e_x, s_X, e_X;
  gboolean f_space = gtk_text_iter_forward_search(&line_start, "[ ]", 0, &s_space, &e_space, &line_end);
  gboolean f_x = gtk_text_iter_forward_search(&line_start, "[x]", 0, &s_x, &e_x, &line_end);
  gboolean f_X = gtk_text_iter_forward_search(&line_start, "[X]", 0, &s_X, &e_X, &line_end);

  if(!f_space && !f_x && !f_X) return;

  GtkTextIter *s = NULL;
  GtkTextIter *e = NULL;
  gboolean checked = FALSE;

  if(f_space)
  {
    s = &s_space; e = &e_space; checked = FALSE;
  }
  if(f_x && (!s || gtk_text_iter_get_offset(&s_x) < gtk_text_iter_get_offset(s)))
  {
    s = &s_x; e = &e_x; checked = TRUE;
  }
  if(f_X && (!s || gtk_text_iter_get_offset(&s_X) < gtk_text_iter_get_offset(s)))
  {
    s = &s_X; e = &e_X; checked = TRUE;
  }

  if(!s || !e) return;

  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_delete(buffer, s, e);
  gtk_text_buffer_insert(buffer, s, checked ? "[ ]" : "[x]", -1);
  gtk_text_buffer_end_user_action(buffer);

  GtkTextBuffer *edit_buffer = gtk_text_view_get_buffer(d->edit_view);
  gchar *text = _get_buffer_text(edit_buffer);
  if(d->mode_toggle && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->mode_toggle)))
  {
    _toggle_mode(GTK_TOGGLE_BUTTON(d->mode_toggle), self);
  }
  else
  {
    _render_preview(d, text);
  }
  g_free(text);
}

static gboolean _preview_button_press(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;

  GtkTextView *view = GTK_TEXT_VIEW(widget);
  gint bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_TEXT,
                                        (gint)event->x, (gint)event->y, &bx, &by);
  GtkTextIter iter;
  gtk_text_view_get_iter_at_location(view, &iter, bx, by);

  GSList *tags = gtk_text_iter_get_tags(&iter);
  for(GSList *t = tags; t; t = g_slist_next(t))
  {
    GtkTextTag *tag = t->data;
    gpointer linep = g_object_get_data(G_OBJECT(tag), "checklist_line");
    if(linep)
    {
      _toggle_checklist_at_line(self, GPOINTER_TO_INT(linep));
      g_slist_free(tags);
      return TRUE;
    }
  }

  for(GSList *t = tags; t; t = g_slist_next(t))
  {
    GtkTextTag *tag = t->data;
    const char *href = g_object_get_data(G_OBJECT(tag), "href");
    if(href && *href)
    {
      _open_uri(href);
      g_slist_free(tags);
      return TRUE;
    }
  }

  g_slist_free(tags);

  GtkTextIter line_start = iter;
  gtk_text_iter_set_line_offset(&line_start, 0);
  GtkTextIter line_end = line_start;
  gtk_text_iter_forward_to_line_end(&line_end);

  GtkTextIter scan = line_start;
  while(TRUE)
  {
    GSList *ltags = gtk_text_iter_get_tags(&scan);
    for(GSList *t = ltags; t; t = g_slist_next(t))
    {
      GtkTextTag *tag = t->data;
      gpointer linep = g_object_get_data(G_OBJECT(tag), "checklist_line");
      if(linep)
      {
        _toggle_checklist_at_line(self, GPOINTER_TO_INT(linep));
        g_slist_free(ltags);
        return TRUE;
      }
    }
    g_slist_free(ltags);
    if(gtk_text_iter_compare(&scan, &line_end) >= 0) break;
    if(!gtk_text_iter_forward_char(&scan)) break;
  }

  return FALSE;
}

static gboolean _refresh_preview_idle(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self) return G_SOURCE_REMOVE;
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->edit_view) return G_SOURCE_REMOVE;
  d->resize_idle_id = 0;
  if(d->mode_toggle && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->mode_toggle)))
    return G_SOURCE_REMOVE;

  _render_preview_from_edit(d);
  return G_SOURCE_REMOVE;
}

static void _preview_map(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !d->preview_view || !d->edit_view) return;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->mode_toggle))) return;

  _render_preview_from_edit(d);

  if(!d->resize_idle_id)
    d->resize_idle_id = g_timeout_add(80, _refresh_preview_idle, self);
}

static void _preview_size_allocate(GtkWidget *widget, GtkAllocation *allocation, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d || !allocation) return;

  const int width = allocation->width;
  if(width > 0 && width != d->preview_alloc_width)
  {
    d->preview_alloc_width = width;
    if(d->mode_toggle && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->mode_toggle)))
    {
      if(!d->resize_idle_id && !d->rendering)
        d->resize_idle_id = g_timeout_add(60, _refresh_preview_idle, self);
    }
  }

  (void)widget;
}

static gboolean _initial_load_idle(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d) return G_SOURCE_REMOVE;
  if(d->imgid > 0) return G_SOURCE_REMOVE;
  if(!darktable.develop) return G_SOURCE_CONTINUE;
  _load_for_image(self, darktable.develop->image_storage.id);
  return G_SOURCE_REMOVE;
}

static void _ensure_has_txt_flag(const int32_t imgid)
{
  if(imgid <= 0) return;

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!img) return;

  if(!(img->flags & DT_IMAGE_HAS_TXT))
    img->flags |= DT_IMAGE_HAS_TXT;

  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
}

static char *_text_sidecar_save_path(const int32_t imgid)
{
  if(imgid <= 0) return NULL;

  gboolean from_cache = FALSE;
  char image_path[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, image_path, sizeof(image_path), &from_cache, __FUNCTION__);

  if(image_path[0] == '\0' || !g_file_test(image_path, G_FILE_TEST_EXISTS))
  {
    from_cache = TRUE;
    dt_image_full_path(imgid, image_path, sizeof(image_path), &from_cache, __FUNCTION__);
  }

  if(image_path[0] == '\0') return NULL;

  return dt_image_build_text_path_from_path(image_path);
}

static void _save_and_render(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  gchar *text = _get_edit_text(d);

  _render_preview(d, text);

  if(d->dirty && d->path && d->imgid > 0)
  {
    GError *error = NULL;
    if(!g_file_set_contents(d->path, text, -1, &error))
    {
      dt_control_log(_("failed to save text notes to %s: %s"), d->path, error->message);
      g_clear_error(&error);
    }
    else
    {
      _ensure_has_txt_flag(d->imgid);
      d->dirty = FALSE;
    }
  }

  _update_mtime_label(self);
  g_free(text);
}

static gboolean _save_timeout_cb(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  d->save_timeout_id = 0;
  _save_and_render(self);
  return G_SOURCE_REMOVE;
}

static void _save_now(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(d->save_timeout_id)
  {
    g_source_remove(d->save_timeout_id);
    d->save_timeout_id = 0;
  }
  _save_and_render(self);
}

static void _textbuffer_changed(GtkTextBuffer *buffer, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(d->loading) return;
  d->dirty = TRUE;

  if(d->save_timeout_id)
  {
    g_source_remove(d->save_timeout_id);
    d->save_timeout_id = 0;
  }

  d->save_timeout_id = g_timeout_add(750, _save_timeout_cb, self);

  _completion_update(self);
}

static gboolean _textview_focus_out(GtkWidget *widget, GdkEventFocus *event, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  (void)d;
  g_idle_add(_completion_focus_out_idle, self);
  return FALSE;
}

static void _toggle_mode(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  const gboolean preview = gtk_toggle_button_get_active(button);
  gtk_stack_set_visible_child_name(GTK_STACK(d->stack), preview ? "preview" : "edit");
  gtk_button_set_label(GTK_BUTTON(d->mode_toggle), preview ? _("Edit") : _("Preview"));

  if(preview)
  {
    _completion_hide(d);
    gchar *text = _get_edit_text(d);
    _render_preview(d, text);
    g_free(text);
  }
}

static void _load_for_image(dt_lib_module_t *self, const int32_t imgid)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;

  if(d->save_timeout_id)
  {
    g_source_remove(d->save_timeout_id);
    d->save_timeout_id = 0;
  }

  d->imgid = imgid;
  g_free(d->path);
  d->path = NULL;

  if(imgid <= 0)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(d->edit_view), FALSE);
    gtk_widget_set_sensitive(d->mode_toggle, FALSE);
  }
  else
  {
    gtk_widget_set_sensitive(GTK_WIDGET(d->edit_view), TRUE);
    gtk_widget_set_sensitive(d->mode_toggle, TRUE);
    d->path = _text_sidecar_save_path(imgid);
  }

  gchar *text = NULL;
  char *existing_path = (imgid > 0) ? dt_image_get_text_path(imgid) : NULL;
  if(existing_path)
  {
    g_file_get_contents(existing_path, &text, NULL, NULL);
    _ensure_has_txt_flag(imgid);
  }
  if(!text) text = g_strdup("");

  _set_edit_text(d, text);
  d->dirty = FALSE;

  if(d->mode_toggle && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->mode_toggle)))
  {
    _toggle_mode(GTK_TOGGLE_BUTTON(d->mode_toggle), self);
  }
  else
  {
    _render_preview(d, text);
  }

  g_free(existing_path);
  g_free(text);
  _update_mtime_label(self);
}

static void _image_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _update_for_current_image(self);
}

static void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _update_for_current_image(self);
}

static void _update_for_current_image(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;
  if(!d) return;

  if(!d->loading) _save_now(self);

  int32_t img_id = dt_control_get_mouse_over_id();
  if(img_id > -1)
    ;
  else if(dt_act_on_get_first_image() > -1)
    img_id = dt_act_on_get_first_image();

  if(img_id == d->imgid) return; // nothing to update, spare the SQL queries

  _load_for_image(self, img_id);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)calloc(1, sizeof(dt_lib_textnotes_t));
  self->data = (void *)d;
  d->self = self;

  d->imgid = -1;
  d->height_setting = g_strdup("plugins/darkroom/textnotes/text_height");

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->widget = vbox;
  d->root = vbox;

  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

  d->mode_toggle = gtk_toggle_button_new_with_label(_("preview"));
  gtk_widget_set_tooltip_text(d->mode_toggle, _("toggle Markdown preview"));
  gtk_box_pack_end(GTK_BOX(toolbar), d->mode_toggle, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(d->mode_toggle), "toggled", G_CALLBACK(_toggle_mode), self);

  d->mtime_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->mtime_label), 0.0f);
  gtk_widget_set_halign(d->mtime_label, GTK_ALIGN_START);
  gtk_widget_set_visible(d->mtime_label, FALSE);
  gtk_box_pack_start(GTK_BOX(toolbar), d->mtime_label, TRUE, TRUE, 0);

  d->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(d->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_box_pack_start(GTK_BOX(vbox), d->stack, TRUE, TRUE, 0);

  GtkWidget *textview = gtk_text_view_new();
  dt_accels_disconnect_on_text_input(textview);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(textview), FALSE);
  gtk_widget_set_hexpand(textview, TRUE);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  g_signal_connect(buffer, "changed", G_CALLBACK(_textbuffer_changed), self);
  g_signal_connect(textview, "focus-out-event", G_CALLBACK(_textview_focus_out), self);
  g_signal_connect(textview, "key-press-event", G_CALLBACK(_edit_key_press), self);
  g_signal_connect(textview, "key-release-event", G_CALLBACK(_edit_key_release), self);
  g_signal_connect(textview, "button-release-event", G_CALLBACK(_edit_button_release), self);

  d->edit_view = GTK_TEXT_VIEW(textview);
  _setup_completion(self, textview);

  GtkWidget *edit_sw = dt_ui_scroll_wrap(textview, 140, d->height_setting);
  gtk_widget_set_hexpand(edit_sw, TRUE);
  gtk_widget_set_vexpand(edit_sw, TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(edit_sw), FALSE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(edit_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_stack_add_named(GTK_STACK(d->stack), edit_sw, "edit");

  GtkWidget *preview_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(preview_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(preview_view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(preview_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(preview_view), FALSE);
  gtk_widget_set_hexpand(preview_view, TRUE);
  gtk_widget_add_events(preview_view, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(preview_view), "button-press-event",
                   G_CALLBACK(_preview_button_press), self);
  g_signal_connect(G_OBJECT(preview_view), "map", G_CALLBACK(_preview_map), self);
  g_signal_connect(G_OBJECT(preview_view), "size-allocate", G_CALLBACK(_preview_size_allocate), self);
  gtk_widget_set_hexpand(preview_view, TRUE);
  gtk_widget_set_vexpand(preview_view, TRUE);
  d->preview_view = GTK_TEXT_VIEW(preview_view);

  GtkWidget *preview_sw = dt_ui_scroll_wrap(preview_view, 140, d->height_setting);
  d->preview_sw = preview_sw;
  gtk_widget_set_hexpand(preview_sw, TRUE);
  gtk_widget_set_vexpand(preview_sw, TRUE);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(preview_sw), FALSE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(preview_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_stack_add_named(GTK_STACK(d->stack), preview_sw, "preview");
  gtk_stack_set_visible_child_name(GTK_STACK(d->stack), "preview");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->mode_toggle), TRUE);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_image_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_image_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                                  G_CALLBACK(_mouse_over_image_callback), self);

  gtk_widget_show_all(self->widget);

  _update_for_current_image(self);
  g_idle_add(_initial_load_idle, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_textnotes_t *d = (dt_lib_textnotes_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);

  if(d->save_timeout_id)
  {
    g_source_remove(d->save_timeout_id);
    d->save_timeout_id = 0;
  }

  if(d->resize_idle_id)
  {
    g_source_remove(d->resize_idle_id);
    d->resize_idle_id = 0;
  }

#ifdef HAVE_HTTP_SERVER
  if(d->download_inflight)
  {
    g_hash_table_destroy(d->download_inflight);
    d->download_inflight = NULL;
  }
#endif

  if(d->completion_popover)
  {
    gtk_widget_destroy(d->completion_popover);
    d->completion_popover = NULL;
  }
  if(d->completion_model)
  {
    g_object_unref(d->completion_model);
    d->completion_model = NULL;
  }

  g_free(d->path);
  g_free(d->height_setting);
  free(d);
  self->data = NULL;
}
