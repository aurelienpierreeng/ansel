/*
    This file is part of the Ansel project.
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

#include "gui/splash.h"

#include "common/darktable.h"
#include "common/file_location.h"
#include "common/l10n.h"
#include "gui/gtk.h"

#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
typedef struct dt_splash_t
{
  GtkWidget *window;
  GtkWidget *drawing;
  GtkWidget *label_message;
  GtkWidget *logo;
  gchar *logo_path;
  int logo_scale_factor;
  GdkPixbuf *slide_pixbuf;
  int slide_cache_width;
  int slide_cache_height;
  int slide_cache_scale;
  int slide_cache_index;
  gboolean shown;
  GtkCssProvider *css;

  GPtrArray *authors;
  GPtrArray *slides;
  gint current_slide;
  guint slide_timeout_id;
} dt_splash_t;

typedef struct dt_splash_slide_t
{
  gchar *path;
  gchar *author;
} dt_splash_slide_t;

static dt_splash_t *splash = NULL;

static gboolean _splash_env_is_truthy(const char *value)
{
  if(!value) return FALSE;
  if(value[0] == '\0') return TRUE;
  if(g_ascii_strcasecmp(value, "0") == 0) return FALSE;
  if(g_ascii_strcasecmp(value, "false") == 0) return FALSE;
  if(g_ascii_strcasecmp(value, "no") == 0) return FALSE;
  if(g_ascii_strcasecmp(value, "off") == 0) return FALSE;
  return TRUE;
}

static gboolean _splash_is_disabled(void)
{
  return _splash_env_is_truthy(g_getenv("ANSEL_NO_SPLASH"))
      || _splash_env_is_truthy(g_getenv("ANSEL_DISABLE_SPLASH"))
      || _splash_env_is_truthy(g_getenv("DARKTABLE_NO_SPLASH"))
      || _splash_env_is_truthy(g_getenv("DARKTABLE_DISABLE_SPLASH"));
}

void dt_gui_splash_set_transient_for(GtkWidget *parent)
{
  if(!splash || !splash->window || !parent) return;
  gtk_window_set_transient_for(GTK_WINDOW(splash->window), GTK_WINDOW(parent));
  gtk_window_set_keep_above(GTK_WINDOW(splash->window), TRUE);
}

static void _splash_force_show(void)
{
  if(!splash || !splash->window || splash->shown) return;

  gtk_widget_show_all(splash->window);
  gtk_window_present(GTK_WINDOW(splash->window));
  gtk_widget_show_now(splash->window);
  gtk_widget_queue_draw(splash->drawing);
  gtk_widget_queue_draw(splash->window);

  GdkDisplay *display = gdk_display_get_default();
  if(display) gdk_display_flush(display);

  splash->shown = TRUE;
}

static void _splash_clear_slide_cache(void)
{
  if(!splash) return;
  if(splash->slide_pixbuf)
  {
    g_object_unref(splash->slide_pixbuf);
    splash->slide_pixbuf = NULL;
  }
  splash->slide_cache_index = -1;
  splash->slide_cache_width = 0;
  splash->slide_cache_height = 0;
  splash->slide_cache_scale = 0;
}

static void _splash_add_css(const char *data)
{
  if(!splash || !splash->css) return;
  gtk_css_provider_load_from_data(splash->css, data, -1, NULL);
}

static gchar *_splash_build_data_path(const char *subpath)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  return g_build_filename(datadir, "pixmaps", "splash", subpath, NULL);
}

static gchar *_splash_build_author_list(guint max_names)
{
  if(!splash || !splash->authors || splash->authors->len == 0)
    return g_strdup(_("Contributors"));

  GString *buf = g_string_new(NULL);
  guint used = 0;
  g_string_append(buf, "© ");

  for(guint i = 0; i < splash->authors->len && used < max_names; i++)
  {
    const gchar *name = (const gchar *)splash->authors->pdata[i];
    if(!name || !name[0]) continue;
    used++;

    g_string_append(buf, name);

    if(used < max_names)
      g_string_append(buf, ", ");
  }

  g_string_append(buf, "… and all contributors.");

  return g_string_free(buf, FALSE);
}

static GtkWidget *_splash_shadow_label_new(const gchar *text, const gchar *name, const gchar *shadow_name,
                                           GtkWidget **out_main)
{
  GtkWidget *fixed = gtk_fixed_new();
  GtkWidget *shadow1 = gtk_label_new(text);
  GtkWidget *shadow2 = gtk_label_new(text);
  GtkWidget *label = gtk_label_new(text);

  if(shadow_name) gtk_widget_set_name(shadow1, shadow_name);
  if(shadow_name) gtk_widget_set_name(shadow2, shadow_name);
  if(name) gtk_widget_set_name(label, name);

  PangoAttrList *shadow_attrs = pango_attr_list_new();
  PangoAttribute *fg = pango_attr_foreground_new(0, 0, 0);
  PangoAttribute *alpha = pango_attr_foreground_alpha_new((guint16)(0.75 * 65535));
  pango_attr_list_insert(shadow_attrs, fg);
  pango_attr_list_insert(shadow_attrs, alpha);
  gtk_label_set_attributes(GTK_LABEL(shadow1), shadow_attrs);
  gtk_label_set_attributes(GTK_LABEL(shadow2), shadow_attrs);
  pango_attr_list_unref(shadow_attrs);

  PangoAttrList *main_attrs = pango_attr_list_new();
  PangoAttribute *main_fg = pango_attr_foreground_new(65535, 65535, 65535);
  PangoAttribute *main_alpha = pango_attr_foreground_alpha_new(65535);
  pango_attr_list_insert(main_attrs, main_fg);
  pango_attr_list_insert(main_attrs, main_alpha);
  gtk_label_set_attributes(GTK_LABEL(label), main_attrs);
  pango_attr_list_unref(main_attrs);

  gtk_fixed_put(GTK_FIXED(fixed), shadow1, 1, 1);
  gtk_fixed_put(GTK_FIXED(fixed), shadow2, 2, 2);
  gtk_fixed_put(GTK_FIXED(fixed), label, 0, 0);

  GList *shadows = NULL;
  shadows = g_list_append(shadows, shadow1);
  shadows = g_list_append(shadows, shadow2);
  g_object_set_data_full(G_OBJECT(label), "splash-shadow-labels", shadows, (GDestroyNotify)g_list_free);

  if(out_main) *out_main = label;

  return fixed;
}

static void _splash_shadow_label_set_text(GtkWidget *label, const gchar *text)
{
  if(!label) return;
  gtk_label_set_text(GTK_LABEL(label), text);
  GList *shadows = (GList *)g_object_get_data(G_OBJECT(label), "splash-shadow-labels");
  for(GList *iter = shadows; iter; iter = g_list_next(iter))
    gtk_label_set_text(GTK_LABEL(iter->data), text);
}

static dt_splash_slide_t *_splash_slide_new(const gchar *path, const gchar *author)
{
  dt_splash_slide_t *slide = g_malloc0(sizeof(dt_splash_slide_t));
  slide->path = g_strdup(path);
  slide->author = author ? g_strdup(author) : NULL;
  return slide;
}

static void _splash_slide_free(gpointer data)
{
  dt_splash_slide_t *slide = (dt_splash_slide_t *)data;
  if(!slide) return;
  g_free(slide->path);
  g_free(slide->author);
  g_free(slide);
}

static gboolean _splash_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!splash || !splash->slides || splash->slides->len == 0) return FALSE;

  GtkAllocation alloc;
  gtk_widget_get_allocation(widget, &alloc);
  const int width = alloc.width;
  const int height = alloc.height;

  dt_splash_slide_t *slide = (dt_splash_slide_t *)splash->slides->pdata[splash->current_slide % splash->slides->len];
  if(!slide || !slide->path) return FALSE;

  int scale_factor = gtk_widget_get_scale_factor(widget);
  if(scale_factor < 1) scale_factor = 1;
  const int dev_width = width * scale_factor;
  const int dev_height = height * scale_factor;

  const int slide_index = splash->current_slide % splash->slides->len;
  const gboolean cache_ok = splash->slide_pixbuf
    && splash->slide_cache_index == slide_index
    && splash->slide_cache_width == dev_width
    && splash->slide_cache_height == dev_height
    && splash->slide_cache_scale == scale_factor;

  if(!cache_ok)
  {
    _splash_clear_slide_cache();

    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(slide->path, &error);
    if(!pixbuf)
    {
      if(error) g_error_free(error);
      return FALSE;
    }

    const int img_w = gdk_pixbuf_get_width(pixbuf);
    const int img_h = gdk_pixbuf_get_height(pixbuf);
    const double scale = fmax((double)dev_width / img_w, (double)dev_height / img_h);
    const int scaled_w = (int)ceil(img_w * scale);
    const int scaled_h = (int)ceil(img_h * scale);

    splash->slide_pixbuf = gdk_pixbuf_scale_simple(pixbuf, scaled_w, scaled_h, GDK_INTERP_HYPER);
    g_object_unref(pixbuf);
    splash->slide_cache_index = slide_index;
    splash->slide_cache_width = dev_width;
    splash->slide_cache_height = dev_height;
    splash->slide_cache_scale = scale_factor;
  }

  const int scaled_w = gdk_pixbuf_get_width(splash->slide_pixbuf);
  const int scaled_h = gdk_pixbuf_get_height(splash->slide_pixbuf);
  const int offset_x = (dev_width - scaled_w) / 2;
  const int offset_y = (dev_height - scaled_h) / 2;

  cairo_save(cr);
  cairo_scale(cr, 1.0 / scale_factor, 1.0 / scale_factor);
  cairo_rectangle(cr, 0, 0, dev_width, dev_height);
  cairo_clip(cr);

  gdk_cairo_set_source_pixbuf(cr, splash->slide_pixbuf, offset_x, offset_y);
  cairo_paint(cr);

  cairo_restore(cr);

  if(slide->author && slide->author[0])
  {
    gchar *label = g_strdup_printf(_("© %s"), slide->author);
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, label);
    PangoFontDescription *desc = pango_font_description_from_string("14px Roboto");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (width - 32) * PANGO_SCALE);

    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    const int pad = 6;
    const int margin = 0;
    int box_w = text_w + pad * 2;
    int box_h = text_h + pad * 2;
    int x = width - box_w - margin;
    int y = margin;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, box_w, box_h);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
    cairo_move_to(cr, x + pad, y + pad);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
    g_free(label);
  }

  return FALSE;
}

static gboolean _splash_slide_advance(gpointer user_data)
{
  if(!splash || !splash->drawing) return G_SOURCE_REMOVE;
  if(!splash->slides || splash->slides->len == 0) return G_SOURCE_CONTINUE;
  splash->current_slide = (splash->current_slide + 1) % splash->slides->len;
  _splash_clear_slide_cache();
  gtk_widget_queue_draw(splash->drawing);
  return G_SOURCE_CONTINUE;
}

static void _splash_load_authors(void)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  gchar *path = g_build_filename(datadir, "AUTHORS", NULL);
  gchar *content = NULL;
  gsize len = 0;

  if(g_file_get_contents(path, &content, &len, NULL) && content)
  {
    gchar **lines = g_strsplit(content, "\n", -1);
    for(gint i = 0; lines[i]; i++)
    {
      gchar *trim = g_strstrip(lines[i]);
      if(trim[0] == '\0') continue;
      if(trim[0] == '*') continue;
      g_ptr_array_add(splash->authors, g_strdup(trim));
    }
    g_strfreev(lines);
    g_free(content);
  }
  g_free(path);

  if(splash->authors->len == 0)
  {
    g_ptr_array_add(splash->authors, g_strdup(_("Darktable & Ansel contributors")));
  }
}

static void _splash_load_slides(void)
{
  const gchar *default_author = _("Boilerplate image");
  gchar *list_path = _splash_build_data_path("slides.txt");
  gchar *content = NULL;
  gsize len = 0;
  if(g_file_get_contents(list_path, &content, &len, NULL) && content)
  {
    gchar **lines = g_strsplit(content, "\n", -1);
    for(gint i = 0; lines[i]; i++)
    {
      gchar *line = g_strstrip(lines[i]);
      if(line[0] == '\0' || line[0] == '#') continue;

      gchar **parts = g_strsplit(line, "|", 2);
      const gchar *name = parts[0] ? g_strstrip(parts[0]) : NULL;
      const gchar *author = (parts[1] && parts[1][0]) ? g_strstrip(parts[1]) : default_author;
      if(name && name[0])
      {
        gchar *path = NULL;
        if(g_path_is_absolute(name))
          path = g_strdup(name);
        else
          path = _splash_build_data_path(name);

        if(g_file_test(path, G_FILE_TEST_EXISTS))
          g_ptr_array_add(splash->slides, _splash_slide_new(path, author));
        g_free(path);
      }
      g_strfreev(parts);
    }
    g_strfreev(lines);
    g_free(content);
  }
  g_free(list_path);
}

static void _splash_update_message(const gchar *message)
{
  if(!splash || !splash->label_message) return;
  _splash_force_show();
  _splash_shadow_label_set_text(splash->label_message, message);
  gtk_widget_queue_draw(splash->label_message);
  gtk_widget_queue_draw(splash->drawing);
  for(int i = 0; i < 2; i++)
    gtk_main_iteration_do(FALSE);
}

static gchar *_splash_capitalize_name(const char *name)
{
  if(!name || !name[0]) return g_strdup("");
  gchar *out = g_strdup(name);
  out[0] = g_ascii_toupper(out[0]);
  return out;
}

static gboolean _splash_logo_set_from_path(GtkWidget *logo, const char *path, int target_size, int scale_factor)
{
  if(!logo || !path) return FALSE;
  if(scale_factor < 1) scale_factor = 1;

  const int target_px = target_size * scale_factor;
  GError *error = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, target_px, target_px, TRUE, &error);
  if(!pixbuf)
  {
    if(error) g_error_free(error);
    return FALSE;
  }

  cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale_factor, NULL);
  g_object_unref(pixbuf);
  if(!surface) return FALSE;

  gtk_image_set_from_surface(GTK_IMAGE(logo), surface);
  cairo_surface_destroy(surface);
  return TRUE;
}

static GtkWidget *_splash_create_logo(int target_size, int scale_factor, gchar **out_path)
{
  char datadir[PATH_MAX] = { 0 };
  char sharedir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));

  // Best effort to find a logo
  GtkWidget *image = gtk_image_new();
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(paths, g_build_filename(datadir, "pixmaps", "scalable", "ansel.svg", NULL));
  g_ptr_array_add(paths, g_build_filename(sharedir, "icons", "hicolor", "scalable", "apps", "ansel.svg", NULL));
  const char *sizes[] = { "256x256", "128x128", "64x64", NULL };
  for(guint i = 0; sizes[i]; i++)
  {
    g_ptr_array_add(paths, g_build_filename(datadir, "pixmaps", sizes[i], "ansel.png", NULL));
    g_ptr_array_add(paths, g_build_filename(sharedir, "icons", "hicolor", sizes[i], "apps", "ansel.png", NULL));
  }

  gboolean loaded = FALSE;
  for(guint i = 0; i < paths->len && !loaded; i++)
  {
    const gchar *path = (const gchar *)paths->pdata[i];
    if(path && g_file_test(path, G_FILE_TEST_EXISTS))
    {
      if(_splash_logo_set_from_path(image, path, target_size, scale_factor))
      {
        if(out_path) *out_path = g_strdup(path);
        loaded = TRUE;
      }
    }
  }

  if(!loaded)
  {
    gtk_image_set_from_icon_name(GTK_IMAGE(image), "ansel", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), target_size);
  }

  g_ptr_array_free(paths, TRUE);
  return image;
}

static void _splash_update_logo_for_scale(void)
{
  if(!splash || !splash->logo || !splash->logo_path) return;

  GtkWidget *scale_widget = splash->window ? splash->window : splash->logo;
  int scale_factor = gtk_widget_get_scale_factor(scale_widget);
  if(scale_factor < 1) scale_factor = 1;
  if(scale_factor == splash->logo_scale_factor) return;

  const int target_size = 128;
  if(_splash_logo_set_from_path(splash->logo, splash->logo_path, target_size, scale_factor))
    splash->logo_scale_factor = scale_factor;
}

static void _splash_scale_factor_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void)object;
  (void)pspec;
  (void)user_data;
  _splash_update_logo_for_scale();
}

void dt_gui_splash_init(void)
{
  if(splash) return;
  if(_splash_is_disabled()) return;

  splash = calloc(1, sizeof(dt_splash_t));
  splash->authors = g_ptr_array_new_with_free_func(g_free);
  splash->slides = g_ptr_array_new_with_free_func(_splash_slide_free);
  splash->slide_cache_index = -1;
  splash->shown = FALSE;

  _splash_load_authors();
  _splash_load_slides();

  splash->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(splash->window), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(splash->window), FALSE);
  gtk_window_set_position(GTK_WINDOW(splash->window), GTK_WIN_POS_CENTER);
  gtk_window_set_type_hint(GTK_WINDOW(splash->window), GDK_WINDOW_TYPE_HINT_SPLASHSCREEN);
  gtk_window_set_keep_above(GTK_WINDOW(splash->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(splash->window), 960, 600);
  gtk_widget_set_app_paintable(splash->window, TRUE);
  gtk_widget_set_name(splash->window, "ansel-splash");
  g_signal_connect(G_OBJECT(splash->window), "notify::scale-factor",
                   G_CALLBACK(_splash_scale_factor_changed), NULL);

  GtkWidget *overlay = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(splash->window), overlay);

  splash->drawing = gtk_drawing_area_new();
  gtk_widget_set_hexpand(splash->drawing, TRUE);
  gtk_widget_set_vexpand(splash->drawing, TRUE);
  gtk_widget_set_name(splash->drawing, "splash-background");
  gtk_container_add(GTK_CONTAINER(overlay), splash->drawing);
  g_signal_connect(G_OBJECT(splash->drawing), "draw", G_CALLBACK(_splash_draw), NULL);

  GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(info_box, "splash-info");
  gtk_widget_set_halign(info_box, GTK_ALIGN_START);
  gtk_widget_set_valign(info_box, GTK_ALIGN_END);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), info_box);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(header, "splash-header");
  gtk_box_pack_start(GTK_BOX(info_box), header, FALSE, FALSE, 0);

  splash->logo_scale_factor = 1;
  splash->logo_path = NULL;
  splash->logo = _splash_create_logo(128, 1, &splash->logo_path);
  if(splash->logo)
  {
    gtk_widget_set_name(splash->logo, "splash-logo");
    gtk_widget_set_size_request(splash->logo, 128, 128);
    gtk_widget_set_halign(splash->logo, GTK_ALIGN_START);
    gtk_widget_set_valign(splash->logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(header), splash->logo, FALSE, FALSE, 0);
  }

  GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(title_box, "splash-title-box");
  gtk_box_pack_start(GTK_BOX(header), title_box, FALSE, FALSE, 0);

  gchar *app_name = _splash_capitalize_name(PACKAGE_NAME);
  GtkWidget *title_fixed = _splash_shadow_label_new(app_name, "splash-title", "splash-title-shadow", NULL);
  g_free(app_name);
  gtk_box_pack_start(GTK_BOX(title_box), title_fixed, FALSE, FALSE, 0);

  GtkWidget *version_fixed = _splash_shadow_label_new(darktable_package_string, "splash-version",
                                                      "splash-version-shadow", NULL);
  gtk_box_pack_start(GTK_BOX(title_box), version_fixed, FALSE, FALSE, 0);

  gchar *authors_line = _splash_build_author_list(5);
  GtkWidget *authors_fixed = _splash_shadow_label_new(authors_line, "splash-authors", "splash-authors-shadow", NULL);
  g_free(authors_line);
  gtk_box_pack_start(GTK_BOX(title_box), authors_fixed, FALSE, FALSE, 0);

  GtkWidget *ticker_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(ticker_box, "splash-ticker");
  gtk_widget_set_halign(ticker_box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(ticker_box, GTK_ALIGN_END);
  gtk_widget_set_hexpand(ticker_box, TRUE);
  gtk_widget_set_size_request(ticker_box, -1, 28);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ticker_box);

  GtkWidget *message_fixed = _splash_shadow_label_new(_("Starting..."), "splash-message",
                                                      "splash-message-shadow",
                                                      &splash->label_message);
  gtk_box_pack_start(GTK_BOX(ticker_box), message_fixed, FALSE, FALSE, 0);

  splash->css = gtk_css_provider_new();
  _splash_add_css(
    "#ansel-splash {"
    "  background-color: #777777;"
    "}"
    "#splash-info {"
    "  background-color: transparent;"
    "  background-image: none;"
    "  box-shadow: none;"
    "  margin: 12px 0;"
    "  padding: 12px 0;"
    "  -GtkBox-spacing: 12px;"
    "}"
    "#splash-header {"
    "  -GtkBox-spacing: 12px;"
    "  margin: 12px 0;"
    "  padding: 12px 0;"
    "}"
    "#splash-title-box {"
    "  -GtkBox-spacing: 12px;"
    "  padding-top: 18px;"
    "}"
    "#splash-logo {"
    "  padding: 0;"
    "  margin: 0;"
    "}"
    "#splash-title {"
    "  color: #f2f2f2;"
    "  font: 700 40px \"Roboto\";"
    "}"
    "#splash-title-shadow {"
    "  color: rgba(0,0,0,0.75);"
    "  font: 700 40px \"Roboto\";"
    "}"
    "#splash-version {"
    "  color: rgb(255,255,255);"
    "  font: 16px \"Roboto\";"
    "}"
    "#splash-version-shadow {"
    "  color: rgba(0,0,0,0.75);"
    "  font: 16px \"Roboto\";"
    "}"
    "#splash-message {"
    "  color: rgba(255,255,255,0.9);"
    "  font: 16px \"Roboto\";"
    "}"
    "#splash-message-shadow {"
    "  color: rgba(0,0,0,0.75);"
    "  font: 16px \"Roboto\";"
    "}"
    "#splash-ticker {"
    "  background: transparent;"
    "  padding: 24px;"
    "  background-image: none;"
    "  box-shadow: none;"
    "  margin: 0;"
    "}"
    "#splash-authors {"
    "  color: rgba(255,255,255,0.92);"
    "  font: 16px \"Roboto\";"
    "  margin: 6px 0;"
    "}"
    "#splash-authors-shadow {"
    "  color: rgba(0,0,0,0.75);"
    "  font: 16px \"Roboto\";"
    "  margin: 6px 0;"
    "}"
  );

  GdkScreen *screen = gdk_screen_get_default();
  if(screen)
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(splash->css),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 2);

  _splash_force_show();
  _splash_update_logo_for_scale();

  splash->slide_timeout_id = g_timeout_add(4000, _splash_slide_advance, NULL);
}

void dt_gui_splash_update(const char *message)
{
  if(!splash || !message) return;
  _splash_update_message(message);
}

void dt_gui_splash_updatef(const char *format, ...)
{
  if(!splash || !format) return;
  va_list ap;
  va_start(ap, format);
  gchar *buf = g_strdup_vprintf(format, ap);
  va_end(ap);
  _splash_update_message(buf);
  g_free(buf);
}

void dt_gui_splash_close(void)
{
  if(!splash) return;

  if(splash->slide_timeout_id) g_source_remove(splash->slide_timeout_id);

  gtk_widget_destroy(splash->window);
  GdkScreen *screen = gdk_screen_get_default();
  if(screen)
    gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(splash->css));
  g_object_unref(splash->css);

  g_ptr_array_free(splash->authors, TRUE);
  g_ptr_array_free(splash->slides, TRUE);
  g_free(splash->logo_path);
  _splash_clear_slide_cache();

  free(splash);
  splash = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
