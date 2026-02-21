/*
    This file is part of the Ansel project.
    Copyright (C) 2020-2022 Aldric Renaudin.
    Copyright (C) 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Mark-64.
    Copyright (C) 2020, 2022 Nicolas Auffray.
    Copyright (C) 2020-2022 Pascal Obry.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2020 Roman Lebedev.
    Copyright (C) 2020 Tianhao Chai.
    Copyright (C) 2020 Tobias Ellinghaus.
    Copyright (C) 2020 U-DESKTOP-HQME86J\marco.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 Fabio Heer.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2022 Sakari Kapanen.
    Copyright (C) 2023 Ricky Moon.
    Copyright (C) 2026 Guillaume Stutin.
    
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
/** this is the thumbnail class for the lighttable module.  */

#include "common/extra_optimizations.h"

#include "dtgtk/thumbnail.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/thumbtable_info.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/datetime.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/focus.h"
#include "common/focus_peaking.h"
#include "common/grouping.h"
#include "common/image_cache.h"
#include "common/database.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/variables.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/preview_window.h"
#include "dtgtk/thumbnail_btn.h"
#include "gui/drag_and_drop.h"

#include "views/view.h"

#include <glib-object.h>
#include <sqlite3.h>

/**
 * @file thumbnail.c
 *
 * WARNING: because we create and destroy thumbnail objects dynamically when scrolling,
 * and we don't manually cleanup the Gtk signal handlers attached to widgets,
 * some callbacks/handlers can be left hanging, still record events, but send them
 * to non-existing objects. This is why we need to ensure user_data is not NULL everywhere.
 */
#define thumb_return_if_fails(thumb, ...) { if(!thumb || !thumb->widget || !thumb->w_main) return  __VA_ARGS__; }


static void _set_flag(GtkWidget *w, GtkStateFlags flag, gboolean activate)
{
  if(activate)
    gtk_widget_set_state_flags(w, flag, FALSE);
  else
    gtk_widget_unset_state_flags(w, flag);
}

static void _image_update_group_tooltip(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb);
  if(!dt_thumbtable_info_is_grouped(thumb->info))
  {
    gtk_widget_set_has_tooltip(thumb->w_group, FALSE);
    return;
  }

  gchar *tt = NULL;
  int nb = 0;

  // the group leader
  if(thumb->info.id == thumb->info.group_id)
    tt = g_strdup_printf("\n\u2022 <b>%s (%s)</b>", _("current"), _("leader"));
  else
  {
    dt_image_t leader = { 0 };
    if(dt_thumbtable_get_thumbnail_info(thumb->table, thumb->info.group_id, &leader))
      tt = g_strdup_printf("%s\n\u2022 <b>%s (%s)</b>", _("\nclick here to set this image as group leader\n"), leader.filename, _("leader"));
  }

  // and the other images
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, version, filename"
                              " FROM main.images"
                              " WHERE group_id = ?1", -1, &stmt,
                              NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->info.group_id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    nb++;
    const int id = sqlite3_column_int(stmt, 0);
    const int v = sqlite3_column_int(stmt, 1);

    if(id != thumb->info.group_id)
    {
      if(id == thumb->info.id)
        tt = dt_util_dstrcat(tt, "\n\u2022 %s", _("current"));
      else
      {
        tt = dt_util_dstrcat(tt, "\n\u2022 %s", sqlite3_column_text(stmt, 2));
        if(v > 0) tt = dt_util_dstrcat(tt, " v%d", v);
      }
    }
  }
  sqlite3_finalize(stmt);

  // and the number of grouped images
  gchar *ttf = g_strdup_printf("%d %s\n%s", nb, _("grouped images"), tt);
  g_free(tt);

  // let's apply the tooltip
  gtk_widget_set_tooltip_markup(thumb->w_group, ttf);
  g_free(ttf);
}

static void _thumb_update_rating_class(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb);
  for(int i = DT_VIEW_DESERT; i <= DT_VIEW_REJECT; i++)
  {
    gchar *cn = g_strdup_printf("dt_thumbnail_rating_%d", i);
    if(thumb->info.rating == i)
      dt_gui_add_class(thumb->w_main, cn);
    else
      dt_gui_remove_class(thumb->w_main, cn);
    g_free(cn);
  }
}

static void _thumb_write_extension(dt_thumbnail_t *thumb)
{
  // fill the file extension label
  thumb_return_if_fails(thumb);
  if(!thumb->info.filename[0]) return;
  const char *ext = thumb->info.filename + strlen(thumb->info.filename);
  while(ext > thumb->info.filename && *ext != '.') ext--;
  ext++;
  gchar *uext = dt_view_extend_modes_str(ext, thumb->info.is_hdr, thumb->info.is_bw, thumb->info.is_bw_flow);
  gchar *label = g_strdup_printf("%s #%i", uext, thumb->rowid + 1);
  gtk_label_set_text(GTK_LABEL(thumb->w_ext), label);
  g_free(uext);
  g_free(label);
}

static GtkWidget *_gtk_menu_item_new_with_markup(const char *label, GtkWidget *menu,
                                                 void (*activate_callback)(GtkWidget *widget,
                                                                           dt_thumbnail_t *thumb),
                                                 dt_thumbnail_t *thumb)
{
  GtkWidget *menu_item = gtk_menu_item_new_with_label("");
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(menu_item));
  gtk_label_set_markup(GTK_LABEL(child), label);
  gtk_menu_item_set_reserve_indicator(GTK_MENU_ITEM(menu_item), FALSE);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  if(activate_callback) g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(activate_callback), thumb);

  return menu_item;
}

static GtkWidget *_menuitem_from_text(const char *label, const char *value, GtkWidget *menu,
                                      void (*activate_callback)(GtkWidget *widget, dt_thumbnail_t *thumb),
                                      dt_thumbnail_t *thumb)
{
  gchar *text = g_strdup_printf("%s%s", label, value);
  GtkWidget *menu_item = _gtk_menu_item_new_with_markup(text, menu, activate_callback, thumb);
  g_free(text);
  return menu_item;
}

static void _color_label_callback(GtkWidget *widget, dt_thumbnail_t *thumb)
{
  int color = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "custom-data"));
  dt_colorlabels_toggle_label_on_list(g_list_append(NULL, GINT_TO_POINTER(thumb->info.id)), color, TRUE);
}

static void _preview_window_open(GtkWidget *widget, dt_thumbnail_t *thumb)
{
  dt_preview_window_spawn(thumb->info.id);
}

static void _active_modules_popup(GtkWidget *widget, dt_thumbnail_t *thumb)
{
  (void)widget;
  if(!thumb) return;

  sqlite3 *handle = dt_database_get(darktable.db);
  if(!handle) return;

  static const char *sql =
      "SELECT MIN(num) AS num, operation, multi_name "
      "FROM main.history "
      "WHERE imgid = ?1 AND enabled = 1 "
      "GROUP BY operation, multi_name "
      "ORDER BY MIN(num) ASC";

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(handle, sql, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, thumb->info.id);

  GString *text = g_string_new(NULL);
  g_string_append_printf(text, "image id: %d\nfile: %s\n\n", thumb->info.id,
                         (thumb->info.fullpath[0]) ? thumb->info.fullpath : thumb->info.filename);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int num = sqlite3_column_int(stmt, 0);
    const char *op = (const char *)sqlite3_column_text(stmt, 1);
    const char *multi = (const char *)sqlite3_column_text(stmt, 2);
    const gboolean has_multi = (multi && multi[0] && strcmp(multi, " "));

    if(has_multi)
      g_string_append_printf(text, "%d. %s (%s)\n", num, op ? op : "?", multi);
    else
      g_string_append_printf(text, "%d. %s\n", num, op ? op : "?");
  }
  sqlite3_finalize(stmt);

  if(text->len == 0) g_string_assign(text, _("No active modules"));

  // Use the real application window as transient parent, not the popup menu toplevel.
  // Standalone dialog (no transient parent) to avoid GTK parent warnings from popup menus.
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Active modules"),
                                                  NULL, GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_Close"), GTK_RESPONSE_CLOSE,
                                                  NULL);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);
  gtk_widget_set_size_request(scrolled, 420, 260);
  gtk_container_set_border_width(GTK_CONTAINER(scrolled), 4);

  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text->str, -1);
  gtk_container_add(GTK_CONTAINER(scrolled), view);

  gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);
  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);

  g_string_free(text, TRUE);
}

static GtkWidget *_create_menu(dt_thumbnail_t *thumb)
{
  // Always re-create the menu when we show it because we don't bother updating info during the lifetime of the thumbnail
  GtkWidget *menu = gtk_menu_new();

  // Filename: insensitive header to mean that the context menu is for this picture only
  GtkWidget *menu_item = _gtk_menu_item_new_with_markup(thumb->info.filename, menu, NULL, thumb);
  gtk_widget_set_sensitive(menu_item, FALSE);

  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  /** image info */
  menu_item = _gtk_menu_item_new_with_markup(_("Image info"), menu, NULL, thumb);
  GtkWidget *sub_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), sub_menu);

  _menuitem_from_text(_("Folder : "), thumb->info.folder, sub_menu, NULL, thumb);
  _menuitem_from_text(_("Date : "), thumb->info.datetime, sub_menu, NULL, thumb);
  _menuitem_from_text(_("Camera : "), thumb->info.camera_makermodel, sub_menu, NULL, thumb);
  _menuitem_from_text(_("Lens : "), thumb->info.exif_lens, sub_menu, NULL, thumb);

  sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  /** color labels  */
  menu_item = _gtk_menu_item_new_with_markup(_("Assign color labels"), menu, NULL, thumb);
  sub_menu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), sub_menu);

  menu_item = _gtk_menu_item_new_with_markup("<span foreground='#BB2222'>\342\254\244</span> Red", sub_menu, _color_label_callback, thumb);
  g_object_set_data(G_OBJECT(menu_item), "custom-data", GINT_TO_POINTER(0));

  menu_item = _gtk_menu_item_new_with_markup("<span foreground='#BBBB22'>\342\254\244</span> Yellow", sub_menu, _color_label_callback, thumb);
  g_object_set_data(G_OBJECT(menu_item), "custom-data", GINT_TO_POINTER(1));

  menu_item = _gtk_menu_item_new_with_markup("<span foreground='#22BB22'>\342\254\244</span> Green", sub_menu, _color_label_callback, thumb);
  g_object_set_data(G_OBJECT(menu_item), "custom-data", GINT_TO_POINTER(2));

  menu_item = _gtk_menu_item_new_with_markup("<span foreground='#2222BB'>\342\254\244</span> Blue", sub_menu, _color_label_callback, thumb);
  g_object_set_data(G_OBJECT(menu_item), "custom-data", GINT_TO_POINTER(3));

  menu_item = _gtk_menu_item_new_with_markup("<span foreground='#BB22BB'>\342\254\244</span> Purple", sub_menu, _color_label_callback, thumb);
  g_object_set_data(G_OBJECT(menu_item), "custom-data", GINT_TO_POINTER(4));

  menu_item = _gtk_menu_item_new_with_markup(_("Open in preview window…"), menu, _preview_window_open, thumb);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  menu_item = _gtk_menu_item_new_with_markup(_("Show active modules…"), menu, _active_modules_popup, thumb);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  gtk_widget_show_all(menu);

  return menu;
}

static gboolean _event_cursor_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);

  GtkStateFlags state = gtk_widget_get_state_flags(thumb->w_cursor);
  GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_cursor);
  GdkRGBA col;
  gtk_style_context_get_color(context, state, &col);

  cairo_set_source_rgba(cr, col.red, col.green, col.blue, col.alpha);
  cairo_line_to(cr, gtk_widget_get_allocated_width(widget), 0);
  cairo_line_to(cr, gtk_widget_get_allocated_width(widget) / 2, gtk_widget_get_allocated_height(widget));
  cairo_line_to(cr, 0, 0);
  cairo_close_path(cr);
  cairo_fill(cr);

  return TRUE;
}


static void _free_image_surface(dt_thumbnail_t *thumb)
{
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
    cairo_surface_destroy(thumb->img_surf);

  thumb->img_surf = NULL;
}

static gboolean _main_context_queue_draw(GtkWidget *widget)
{
  if(GTK_IS_WIDGET(widget))
  {
    gtk_widget_queue_draw(widget);

    // Gtk redraws may get deferred until the next GDK event; force processing now
    // to ensure background thumbnail jobs repaint as soon as they complete.
    GdkWindow *window = gtk_widget_get_window(widget);
    if(window)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gdk_window_process_updates(window, TRUE);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
  }

  return G_SOURCE_REMOVE;
}

static int _finish_buffer_thread(dt_thumbnail_t *thumb, gboolean success)
{
  thumb_return_if_fails(thumb, 1);

  dt_pthread_mutex_lock(&thumb->lock);
  thumb->image_inited = success;
  thumb->job = NULL;
  dt_pthread_mutex_unlock(&thumb->lock);

  // Redraw events need to be sent from the main GUI thread
  // though we may not have a target widget anymore...
  if(!dt_atomic_get_int(&thumb->destroying) && thumb->w_image)
  {
    GMainContext *context = g_main_context_default();
    g_main_context_invoke_full(context, G_PRIORITY_DEFAULT,
                               (GSourceFunc)_main_context_queue_draw,
                               g_object_ref(thumb->w_image),
                               (GDestroyNotify)g_object_unref);
    g_main_context_wakeup(context);
  }
  
  return 0;
}


int32_t _get_image_buffer(dt_job_t *job)
{  
  // WARNING: the target thumbnail GUI widget can be destroyed at any time during this
  // control flow in the GUI mainthread.
  dt_thumbnail_t *thumb = dt_control_job_get_params(job);
  thumb_return_if_fails(thumb, 1);
  if(dt_atomic_get_int(&thumb->destroying)) return 1;

  // The job was cancelled on the queue. Good chances of having thumb destroyed anytime soon.
  if(!thumb->job || thumb->job != job || dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) return 1;

  // Read and cache the thumb data now, while we have it. And lock it.
  dt_pthread_mutex_lock(&thumb->lock);

  // These are the sizes of the widget bounding box
  int img_w = thumb->img_w;
  int img_h = thumb->img_h;

  const int zoom = (thumb->table) ? thumb->table->zoom : DT_THUMBTABLE_ZOOM_FIT;
  const gboolean show_focus_peaking = (thumb->table && thumb->table->focus_peaking);
  const gboolean show_focus_clusters = (thumb->table && thumb->table->focus_regions);
  const gboolean zoom_in = (thumb->table && thumb->table->zoom > DT_THUMBTABLE_ZOOM_FIT);
  const int32_t imgid = thumb->info.id;

  dt_pthread_mutex_unlock(&thumb->lock);

  // These are the sizes of the actual image. Can be larger than the widget bounding box.
  int img_width = 0;
  int img_height = 0;

  double zoomx = 0.;
  double zoomy = 0.;
  float x_center = 0.f;
  float y_center = 0.f;

  // From there, never read thumb->... directly since it might get destroyed in mainthread anytime.
  dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] fetching or computing thumbnail %i\n", thumb->info.id);

  // Get the actual image content. This typically triggers a rendering pipeline,
  // and can possibly take a long time.
  cairo_surface_t *surface = NULL;
  dt_view_surface_value_t res = dt_view_image_get_surface(imgid, img_w, img_h, &surface, zoom);
  if(surface && res == DT_VIEW_SURFACE_OK)
  {
    // The image is immediately available
    img_width = cairo_image_surface_get_width(surface);
    img_height = cairo_image_surface_get_height(surface);
  }
  else
  {
    _finish_buffer_thread(thumb, FALSE);
    return 0;
  }

  if(zoom > DT_THUMBTABLE_ZOOM_FIT || show_focus_peaking)
  {
    // Note: we compute the "sharpness density" unconditionnaly if the image is zoomed-in
    // in order to get the details barycenter to init centering.
    // Actual density are drawn only if the focus peaking mode is enabled.
    cairo_t *cri = cairo_create(surface);
    unsigned char *rgbbuf = cairo_image_surface_get_data(surface);
    if(rgbbuf)
    {
      if(dt_focuspeaking(cri, rgbbuf, img_width, img_height, show_focus_peaking, &x_center, &y_center) != 0)
      {
        cairo_destroy(cri);
        return 1;
      }
    }
    cairo_destroy(cri);

    // Init the zoom offset using the barycenter of details, to center
    // the zoomed-in image on content that matters: details.
    // Offset is expressed from the center of the image
    if(zoom_in && x_center > 0.f && y_center > 0.f && thumb)
    {
      zoomx = (double)img_width / 2. - x_center;
      zoomy = (double)img_height / 2. - y_center;
    }
  }

  // if needed we compute and draw here the big rectangle to show focused areas
  if(show_focus_clusters)
  {
    uint8_t *full_res_thumb = NULL;
    int32_t full_res_thumb_wd, full_res_thumb_ht;
    dt_colorspaces_color_profile_type_t color_space;
    if(!dt_imageio_large_thumbnail(thumb->info.fullpath, &full_res_thumb, &full_res_thumb_wd, &full_res_thumb_ht, &color_space, img_width, img_height))
    {
      // we look for focus areas
      dt_focus_cluster_t full_res_focus[49];
      const int frows = 5, fcols = 5;
      dt_focus_create_clusters(full_res_focus, frows, fcols, full_res_thumb, full_res_thumb_wd,
                                full_res_thumb_ht);
      // and we draw them on the image
      cairo_t *cri = cairo_create(surface);
      dt_focus_draw_clusters(cri, img_width, img_height, imgid, full_res_thumb_wd,
                              full_res_thumb_ht, full_res_focus, frows, fcols, 1.0, 0, 0);
      cairo_destroy(cri);
    }
    dt_pixelpipe_cache_free_align(full_res_thumb);
  }

  // The job was cancelled on the queue. Good chances of having thumb destroyed anytime soon.
  if(!thumb->job || thumb->job != job 
     || dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED 
     || dt_atomic_get_int(&thumb->destroying))
  {
    cairo_surface_destroy(surface);
    return 1;
  }

  // Write temporary surface into actual image surface if we still have a widget to paint on
  if(thumb && thumb->widget && thumb->w_main)
  {
    dt_pthread_mutex_lock(&thumb->lock);
    thumb->img_width = img_width;
    thumb->img_height = img_height;
    thumb->zoomx = zoomx;
    thumb->zoomy = zoomy;
    thumb->img_surf = surface;
    dt_pthread_mutex_unlock(&thumb->lock);

    _finish_buffer_thread(thumb, TRUE);
  }
  else 
  {
    // Lost thumbnail to paint on
    cairo_surface_destroy(surface);
    return 1;
  }

  return 0;
}

int dt_thumbnail_get_image_buffer(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb, 1);

  // - image inited: the cached buffer has a valid size. Invalid this flag when size changes.
  // - img_surf: we have a cached buffer (cairo surface), regardless of its validity.
  // - a rendering job has already been started
  if((thumb->image_inited && thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0))
    return 0;

  // Nuke the image surface in GUI mainthread.
  // Note: if background thumbnail thread gets ditched,
  // this may never be recreated.
  _free_image_surface(thumb);

  // Get thumbnail GUI requested size now (in GUI mainthread). 
  gtk_widget_get_size_request(thumb->w_image, &thumb->img_w, &thumb->img_h);
  thumb->img_w = MAX(thumb->img_w, 32);
  thumb->img_h = MAX(thumb->img_h, 32);

  // Drawing the focus peaking and doing the color conversions
  // can be expensive on large thumbnails. Do it in a background job,
  // so the thumbtable stays responsive.
  thumb->job = dt_control_job_create(&_get_image_buffer, "get image %i", thumb->info.id);
  dt_control_job_set_params(thumb->job, thumb, NULL);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, thumb->job);

  return 0;
}

static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);

  int w = 0;
  int h = 0;
  gtk_widget_get_size_request(thumb->w_image, &w, &h);

  dt_pthread_mutex_lock(&thumb->lock);
  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
  {
    /*
    fprintf(stdout, "surface: %ix%i, widget: %fx%f\n", thumb->img_width, thumb->img_height,
            darktable.gui->ppd * w, darktable.gui->ppd * h);
    */
    // If the size of the image buffer is smaller than the widget surface, we need a new image
    if(thumb->img_width < (int)roundf(darktable.gui->ppd * w) &&
      thumb->img_height < (int)roundf(darktable.gui->ppd * h))
      thumb->image_inited = FALSE;
  }
  dt_pthread_mutex_unlock(&thumb->lock);

  dt_thumbnail_get_image_buffer(thumb);

  dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] redrawing thumbnail %i\n", thumb->info.id);

  dt_pthread_mutex_lock(&thumb->lock);
  if(thumb->image_inited && thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
  {
    // we draw the image
    cairo_save(cr);
    const float scaler = 1.0f / darktable.gui->ppd;
    cairo_scale(cr, scaler, scaler);

    // Correct allocation size for HighDPI scaling
    w *= darktable.gui->ppd;
    h *= darktable.gui->ppd;
    double x_offset = (w - thumb->img_width) / 2.;
    double y_offset = (h - thumb->img_height) / 2.;

    // Sanitize zoom offsets
    if(thumb->table && thumb->table->zoom > DT_THUMBTABLE_ZOOM_FIT)
    {
      thumb->zoomx = CLAMP(thumb->zoomx, -fabs(x_offset), fabs(x_offset));
      thumb->zoomy = CLAMP(thumb->zoomy, -fabs(y_offset), fabs(y_offset));
    }
    else
    {
      thumb->zoomx = 0.;
      thumb->zoomy = 0.;
    }

    cairo_set_source_surface(cr, thumb->img_surf, thumb->zoomx + x_offset, thumb->zoomy + y_offset);

    // Paint background with CSS transparency
    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_image);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(thumb->w_image), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // Paint CSS borders
    gtk_render_frame(context, cr, 0, 0, w, h);
    cairo_restore(cr);
  }
  else
  {
    dt_control_draw_busy_msg(cr, w, h);
  }
  dt_pthread_mutex_unlock(&thumb->lock);

  return TRUE;
}

#define DEBUG 0

static void _thumb_update_icons(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb);
  if(!thumb->widget) return;

  gboolean show = (thumb->over > DT_THUMBNAIL_OVERLAYS_NONE);

  gtk_widget_set_visible(thumb->w_local_copy, (thumb->info.has_localcopy && show) || DEBUG);
  gtk_widget_set_visible(thumb->w_altered, (dt_thumbtable_info_is_altered(thumb->info) && show) || DEBUG);
  gtk_widget_set_visible(thumb->w_group, (dt_thumbtable_info_is_grouped(thumb->info) && show) || DEBUG);
  gtk_widget_set_visible(thumb->w_audio, (thumb->info.has_audio && show) || DEBUG);
  gtk_widget_set_visible(thumb->w_color, show || DEBUG);
  gtk_widget_set_visible(thumb->w_bottom_eb, show || DEBUG);
  gtk_widget_set_visible(thumb->w_reject, show || DEBUG);
  gtk_widget_set_visible(thumb->w_ext, show || DEBUG);
  gtk_widget_show(thumb->w_cursor);

  _set_flag(thumb->w_main, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->widget, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);

  _set_flag(thumb->w_reject, GTK_STATE_FLAG_ACTIVE, (thumb->info.rating == DT_VIEW_REJECT));

  for(int i = 0; i < MAX_STARS; i++)
  {
    gtk_widget_set_visible(thumb->w_stars[i], show || DEBUG);
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, (thumb->info.rating > i && thumb->info.rating < DT_VIEW_REJECT));
  }

  _set_flag(thumb->w_group, GTK_STATE_FLAG_ACTIVE, (thumb->info.id == thumb->info.group_id));
  _set_flag(thumb->w_main, GTK_STATE_FLAG_SELECTED, thumb->selected);
  _set_flag(thumb->widget, GTK_STATE_FLAG_SELECTED, thumb->selected);
}

static gboolean _event_main_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(!gtk_widget_is_visible(thumb->widget)) return TRUE;

  // Ensure mouse_over_id is set because that's what darkroom uses to open a picture.
  // NOTE: Duplicate module uses that fucking thumbnail without a table...
  if(thumb->table)
    dt_thumbtable_dispatch_over(thumb->table, event->type, thumb->info.id);
  else
    dt_control_set_mouse_over_id(thumb->info.id);

  // raise signal on double click
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    thumb->dragging = FALSE;
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, thumb->info.id);
    return TRUE;
  }
  else if(event->button == GDK_BUTTON_SECONDARY && event->type == GDK_BUTTON_PRESS)
  {
    GtkWidget *menu = _create_menu(thumb);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
  }

  return FALSE;
}

static gboolean _event_main_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  thumb->dragging = FALSE;

  // select on single click only in filemanager mode. Filmstrip mode only raises ACTIVATE signals.
  if(event->button == 1
     && thumb->table && thumb->table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    if(dt_modifier_is(event->state, 0))
      dt_selection_select_single(darktable.selection, thumb->info.id);
    else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      dt_selection_toggle(darktable.selection, thumb->info.id);
    else if(dt_modifier_is(event->state, GDK_SHIFT_MASK) && thumb->table)
      dt_thumbtable_select_range(thumb->table, thumb->rowid);
    // Because selection might include several images, we handle styling globally
    // in the thumbtable scope, catching the SELECTION_CHANGED signal.
    return TRUE;
  }

  return FALSE;
}

static gboolean _event_rating_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->disable_actions) return FALSE;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;

  if(event->button == 1)
  {
    dt_view_image_over_t rating = DT_VIEW_DESERT;
    if(widget == thumb->w_reject)
      rating = DT_VIEW_REJECT;
    else if(widget == thumb->w_stars[0])
      rating = DT_VIEW_STAR_1;
    else if(widget == thumb->w_stars[1])
      rating = DT_VIEW_STAR_2;
    else if(widget == thumb->w_stars[2])
      rating = DT_VIEW_STAR_3;
    else if(widget == thumb->w_stars[3])
      rating = DT_VIEW_STAR_4;
    else if(widget == thumb->w_stars[4])
      rating = DT_VIEW_STAR_5;

    if(rating != DT_VIEW_DESERT)
      dt_ratings_apply_on_image(thumb->info.id, rating, TRUE, TRUE, TRUE);
  }
  return TRUE;
}

static gboolean _event_grouping_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->disable_actions) return FALSE;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;
  dt_grouping_change_representative(thumb->info.id);
  return FALSE;
}

static gboolean _event_audio_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->disable_actions) return FALSE;
  if(dtgtk_thumbnail_btn_is_hidden(widget)) return FALSE;

  if(event->button == 1)
  {
    gboolean start_audio = TRUE;
    if(darktable.view_manager->audio.audio_player_id != -1)
    {
      // don't start the audio for the image we just killed it for
      if(darktable.view_manager->audio.audio_player_id == thumb->info.id) start_audio = FALSE;
      dt_view_audio_stop(darktable.view_manager);
    }

    if(start_audio)
    {
      dt_view_audio_start(darktable.view_manager, thumb->info.id);
    }
  }
  return FALSE;
}



void dt_thumbnail_update_selection(dt_thumbnail_t *thumb, gboolean selected)
{
  thumb_return_if_fails(thumb);
  if(selected != thumb->selected)
  {
    thumb->selected = selected;
    _thumb_update_icons(thumb);
  }
}


// All the text info that we don't have room to display around the image
void _create_alternative_view(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb);
  gtk_label_set_text(GTK_LABEL(thumb->w_filename), thumb->info.filename);
  gtk_label_set_text(GTK_LABEL(thumb->w_datetime), thumb->info.datetime);
  gtk_label_set_text(GTK_LABEL(thumb->w_folder), thumb->info.folder);

  const gchar *exposure_field = g_strdup_printf("%.0f ISO - f/%.1f - %s",
                                                thumb->info.exif_iso,
                                                thumb->info.exif_aperture,
                                                dt_util_format_exposure(thumb->info.exif_exposure));

  gtk_label_set_text(GTK_LABEL(thumb->w_exposure_bias), g_strdup_printf("%+.1f EV", thumb->info.exif_exposure_bias));
  gtk_label_set_text(GTK_LABEL(thumb->w_exposure), exposure_field);
  gtk_label_set_text(GTK_LABEL(thumb->w_camera), thumb->info.camera_makermodel);
  gtk_label_set_text(GTK_LABEL(thumb->w_lens), thumb->info.exif_lens);
  gtk_label_set_text(GTK_LABEL(thumb->w_focal),
                     g_strdup_printf("%0.f mm @ %.2f m", thumb->info.exif_focal_length,
                                     thumb->info.exif_focus_distance));
}


void dt_thumbnail_alternative_mode(dt_thumbnail_t *thumb, gboolean enable)
{
  thumb_return_if_fails(thumb);
  if(thumb->alternative_mode == enable) return;
  thumb->alternative_mode = enable;
  if(enable)
  {
    gtk_widget_set_no_show_all(thumb->w_alternative, FALSE);
    gtk_widget_show_all(thumb->w_alternative);
  }
  else
  {
    gtk_widget_set_no_show_all(thumb->w_alternative, TRUE);
    gtk_widget_hide(thumb->w_alternative);
  }
  gtk_widget_queue_draw(thumb->widget);
}


static gboolean _event_star_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->disable_actions) return TRUE;
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, TRUE);

  // we prelight all stars before the current one
  gboolean pre = TRUE;
  for(int i = 0; i < MAX_STARS; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, pre);

    // We don't want the active state to overlap the prelight one because
    // it makes the feature hard to read/understand.
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, FALSE);

    if(thumb->w_stars[i] == widget) pre = FALSE;
  }
  return TRUE;
}


static gboolean _event_star_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->disable_actions) return TRUE;

  for(int i = 0; i < MAX_STARS; i++)
  {
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_PRELIGHT, FALSE);

    // restore active state
    _set_flag(thumb->w_stars[i], GTK_STATE_FLAG_ACTIVE, i < thumb->info.rating && thumb->info.rating < DT_VIEW_REJECT);
  }
  return TRUE;
}


gboolean _event_expose(GtkWidget *self, cairo_t *cr, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  return FALSE;
}

static gboolean _event_main_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(!gtk_widget_is_visible(thumb->widget)) return TRUE;
  if(!thumb->mouse_over)
  {
    // Thumbnails send leave-notify when in the thumbnail frame but over the image.
    // If we lost the mouse-over in this case, grab it again from mouse motion.
    // Be conservative with sending mouse_over_id events/signal because many
    // places in the soft listen to them and refresh stuff from DB, so it's expensive.
    if(thumb->table)
      dt_thumbtable_dispatch_over(thumb->table, event->type, thumb->info.id);
    else
      dt_control_set_mouse_over_id(thumb->info.id);

    dt_thumbnail_set_mouseover(thumb, TRUE);
  }
  return FALSE;
}

static gboolean _event_main_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(!gtk_widget_is_visible(thumb->widget)) return TRUE;

  if(thumb->table)
    dt_thumbtable_dispatch_over(thumb->table, event->type, thumb->info.id);
  else
    dt_control_set_mouse_over_id(thumb->info.id);

  dt_thumbnail_set_mouseover(thumb, TRUE);
  return FALSE;
}

static gboolean _event_main_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(!gtk_widget_is_visible(thumb->widget)) return TRUE;

  if(thumb->table)
    dt_thumbtable_dispatch_over(thumb->table, event->type, -1);
  else
    dt_control_set_mouse_over_id(-1);

  dt_thumbnail_set_mouseover(thumb, FALSE);
  return FALSE;
}

// lazy-load the history tooltip only when mouse enters the button
static gboolean _altered_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(dt_thumbtable_info_is_altered(thumb->info))
  {
    char *tooltip = dt_history_get_items_as_string(thumb->info.id);
    if(tooltip)
    {
      gtk_widget_set_tooltip_text(thumb->w_altered, tooltip);
      g_free(tooltip);
    }
  }
  return FALSE;
}


static gboolean _group_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  _image_update_group_tooltip(thumb);
  return FALSE;
}


static gboolean _event_image_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);

  if(event->button == 1 && thumb->table && thumb->table->zoom > DT_THUMBTABLE_ZOOM_FIT)
  {
    thumb->dragging = TRUE;
    thumb->drag_x_start = event->x;
    thumb->drag_y_start = event->y;
  }

  return FALSE;
}

static gboolean _event_image_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  if(thumb->dragging)
  {
    const double delta_x = (event->x - thumb->drag_x_start) * darktable.gui->ppd;
    const double delta_y = (event->y - thumb->drag_y_start) * darktable.gui->ppd;
    const gboolean global_shift = dt_modifier_is(event->state, GDK_SHIFT_MASK) && thumb->table;

    if(global_shift)
    {
      // Offset all thumbnails by this amount
      dt_thumbtable_offset_zoom(thumb->table, delta_x, delta_y);
    }
    else
    {
      // Offset only the current thumbnail
      thumb->zoomx += delta_x;
      thumb->zoomy += delta_y;
    }

    // Reset drag origin
    thumb->drag_x_start = event->x;
    thumb->drag_y_start = event->y;

    if(!global_shift)
      gtk_widget_queue_draw(thumb->w_image);

    return TRUE;
  }
  return FALSE;
}

static gboolean _event_image_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  thumb_return_if_fails(thumb, TRUE);
  thumb->dragging = FALSE;
  return FALSE;
}

GtkWidget *dt_thumbnail_create_widget(dt_thumbnail_t *thumb)
{
  // Let the background event box capture all user events from its children first,
  // so we don't have to wire leave/enter events to all of them individually.
  // Children buttons will mostly only use button pressed/released events
  thumb->widget = gtk_event_box_new();
  dt_gui_add_class(thumb->widget, "thumb-cell");
  gtk_widget_set_events(thumb->widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK | GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

  // this is only here to ensure that mouse-over value is updated correctly
  // all dragging actions take place inside thumbatble.c
  gtk_drag_dest_set(thumb->widget, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_object_set_data(G_OBJECT(thumb->widget), "thumb", thumb);
  gtk_widget_show(thumb->widget);

  g_signal_connect(G_OBJECT(thumb->widget), "button-press-event", G_CALLBACK(_event_main_press), thumb);
  g_signal_connect(G_OBJECT(thumb->widget), "button-release-event", G_CALLBACK(_event_main_release), thumb);
  g_signal_connect(G_OBJECT(thumb->widget), "enter-notify-event", G_CALLBACK(_event_main_enter), thumb);
  g_signal_connect(G_OBJECT(thumb->widget), "leave-notify-event", G_CALLBACK(_event_main_leave), thumb);
  g_signal_connect(G_OBJECT(thumb->widget), "motion-notify-event", G_CALLBACK(_event_main_motion), thumb);
  g_signal_connect(G_OBJECT(thumb->widget), "draw", G_CALLBACK(_event_expose), thumb);

  // Main widget
  thumb->w_main = gtk_overlay_new();
  dt_gui_add_class(thumb->w_main, "thumb-main");
  gtk_widget_set_valign(thumb->w_main, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(thumb->w_main, GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(thumb->widget), thumb->w_main);
  gtk_widget_show(thumb->w_main);

  thumb->w_background = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(thumb->w_background, "thumb-background");
  gtk_widget_set_valign(thumb->w_background, GTK_ALIGN_FILL);
  gtk_widget_set_halign(thumb->w_background, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_background);
  gtk_widget_show(thumb->w_background);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_background, TRUE);

  // triangle to indicate current image(s) in filmstrip
  thumb->w_cursor = gtk_drawing_area_new();
  dt_gui_add_class(thumb->w_cursor, "thumb-cursor");
  gtk_widget_set_valign(thumb->w_cursor, GTK_ALIGN_START);
  gtk_widget_set_halign(thumb->w_cursor, GTK_ALIGN_CENTER);
  g_signal_connect(G_OBJECT(thumb->w_cursor), "draw", G_CALLBACK(_event_cursor_draw), thumb);
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_cursor);

  // the image drawing area
  thumb->w_image = gtk_drawing_area_new();
  dt_gui_add_class(thumb->w_image, "thumb-image");
  gtk_widget_set_valign(thumb->w_image, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(thumb->w_image, GTK_ALIGN_CENTER);
  gtk_widget_set_events(thumb->w_image, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(thumb->w_image), "draw", G_CALLBACK(_thumb_draw_image), thumb);
  g_signal_connect(G_OBJECT(thumb->w_image), "button-press-event", G_CALLBACK(_event_image_press), thumb);
  g_signal_connect(G_OBJECT(thumb->w_image), "button-release-event", G_CALLBACK(_event_image_release), thumb);
  g_signal_connect(G_OBJECT(thumb->w_image), "motion-notify-event", G_CALLBACK(_event_image_motion), thumb);
  gtk_widget_show(thumb->w_image);
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_image);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(thumb->w_main), thumb->w_image, TRUE);

  thumb->w_bottom_eb = gtk_event_box_new();
  gtk_widget_set_valign(thumb->w_bottom_eb, GTK_ALIGN_END);
  gtk_widget_set_halign(thumb->w_bottom_eb, GTK_ALIGN_FILL);
  gtk_widget_show(thumb->w_bottom_eb);
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_bottom_eb);

  GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(bottom_box, "thumb-bottom");
  gtk_container_add(GTK_CONTAINER(thumb->w_bottom_eb), bottom_box);
  gtk_widget_show(bottom_box);

  // the reject icon
  thumb->w_reject = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_reject, 0, NULL);
  dt_gui_add_class(thumb->w_reject, "thumb-reject");
  gtk_widget_set_valign(thumb->w_reject, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(thumb->w_reject, GTK_ALIGN_START);
  gtk_widget_show(thumb->w_reject);
  g_signal_connect(G_OBJECT(thumb->w_reject), "button-release-event", G_CALLBACK(_event_rating_release), thumb);
  gtk_box_pack_start(GTK_BOX(bottom_box), thumb->w_reject, FALSE, FALSE, 0);

  GtkWidget *stars_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), stars_box, TRUE, TRUE, 0);
  gtk_widget_set_valign(stars_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(stars_box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(stars_box, TRUE);
  gtk_widget_show(stars_box);

  // the stars
  for(int i = 0; i < MAX_STARS; i++)
  {
    thumb->w_stars[i] = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_star, 0, NULL);
    g_signal_connect(G_OBJECT(thumb->w_stars[i]), "enter-notify-event", G_CALLBACK(_event_star_enter), thumb);
    g_signal_connect(G_OBJECT(thumb->w_stars[i]), "leave-notify-event", G_CALLBACK(_event_star_leave), thumb);
    g_signal_connect(G_OBJECT(thumb->w_stars[i]), "button-release-event", G_CALLBACK(_event_rating_release),
                      thumb);
    dt_gui_add_class(thumb->w_stars[i], "thumb-star");
    gtk_widget_set_valign(thumb->w_stars[i], GTK_ALIGN_CENTER);
    gtk_widget_set_halign(thumb->w_stars[i], GTK_ALIGN_CENTER);
    gtk_widget_show(thumb->w_stars[i]);
    gtk_box_pack_start(GTK_BOX(stars_box), thumb->w_stars[i], FALSE, FALSE, 0);
  }

  // the color labels
  thumb->w_color = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_label_flower, thumb->info.color_labels, NULL);
  dt_gui_add_class(thumb->w_color, "thumb-colorlabels");
  gtk_widget_set_valign(thumb->w_color, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(thumb->w_color, GTK_ALIGN_END);
  gtk_widget_set_no_show_all(thumb->w_color, TRUE);
  gtk_box_pack_start(GTK_BOX(bottom_box), thumb->w_color, FALSE, FALSE, 0);

  thumb->w_top_eb = gtk_event_box_new();
  gtk_widget_set_valign(thumb->w_top_eb, GTK_ALIGN_START);
  gtk_widget_set_halign(thumb->w_top_eb, GTK_ALIGN_FILL);
  gtk_widget_show(thumb->w_top_eb);
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_top_eb);

  GtkWidget *top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(top_box, "thumb-top");
  gtk_container_add(GTK_CONTAINER(thumb->w_top_eb), top_box);
  gtk_widget_show(top_box);

  // the file extension label
  thumb->w_ext = gtk_label_new("");
  dt_gui_add_class(thumb->w_ext, "thumb-ext");
  gtk_widget_set_valign(thumb->w_ext, GTK_ALIGN_CENTER);
  gtk_widget_show(thumb->w_ext);
  gtk_box_pack_start(GTK_BOX(top_box), thumb->w_ext, FALSE, FALSE, 0);

  // the local copy indicator
  thumb->w_local_copy = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_local_copy, 0, NULL);
  dt_gui_add_class(thumb->w_local_copy, "thumb-localcopy");
  gtk_widget_set_tooltip_text(thumb->w_local_copy, _("This picture is locally copied on your disk cache"));
  gtk_widget_set_valign(thumb->w_local_copy, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(thumb->w_local_copy, TRUE);
  gtk_box_pack_start(GTK_BOX(top_box), thumb->w_local_copy, FALSE, FALSE, 0);

  // the altered icon
  thumb->w_altered = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_altered, 0, NULL);
  g_signal_connect(G_OBJECT(thumb->w_altered), "enter-notify-event", G_CALLBACK(_altered_enter), thumb);
  dt_gui_add_class(thumb->w_altered, "thumb-altered");
  gtk_widget_set_valign(thumb->w_altered, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(thumb->w_altered, TRUE);
  gtk_box_pack_end(GTK_BOX(top_box), thumb->w_altered, FALSE, FALSE, 0);

  // the group bouton
  thumb->w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, 0, NULL);
  dt_gui_add_class(thumb->w_group, "thumb-group");
  g_signal_connect(G_OBJECT(thumb->w_group), "button-release-event", G_CALLBACK(_event_grouping_release), thumb);
  g_signal_connect(G_OBJECT(thumb->w_group), "enter-notify-event", G_CALLBACK(_group_enter), thumb);
  gtk_widget_set_valign(thumb->w_group, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(thumb->w_group, TRUE);
  gtk_box_pack_end(GTK_BOX(top_box), thumb->w_group, FALSE, FALSE, 0);

  // the sound icon
  thumb->w_audio = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_audio, 0, NULL);
  dt_gui_add_class(thumb->w_audio, "thumb-audio");
  g_signal_connect(G_OBJECT(thumb->w_audio), "button-release-event", G_CALLBACK(_event_audio_release), thumb);
  gtk_widget_set_valign(thumb->w_audio, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(thumb->w_audio, TRUE);
  gtk_box_pack_end(GTK_BOX(top_box), thumb->w_audio, FALSE, FALSE, 0);

  thumb->w_alternative = gtk_overlay_new();
  gtk_overlay_add_overlay(GTK_OVERLAY(thumb->w_main), thumb->w_alternative);
  gtk_widget_set_halign(thumb->w_alternative, GTK_ALIGN_FILL);
  gtk_widget_set_valign(thumb->w_alternative, GTK_ALIGN_FILL);
  gtk_widget_hide(thumb->w_alternative);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(thumb->w_alternative), box);
  dt_gui_add_class(box, "thumb-alternative");

  GtkWidget *bbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(bbox, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), bbox, TRUE, TRUE, 0);
  thumb->w_filename = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(thumb->w_filename), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_filename, FALSE, FALSE, 0);
  thumb->w_datetime = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_datetime, FALSE, FALSE, 0);
  thumb->w_folder = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(thumb->w_folder), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_folder, FALSE, FALSE, 0);

  bbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(bbox, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(box), bbox, TRUE, TRUE, 0);
  thumb->w_exposure = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_exposure, FALSE, FALSE, 0);
  thumb->w_exposure_bias = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_exposure_bias, FALSE, FALSE, 0);

  bbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(bbox, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(box), bbox, TRUE, TRUE, 0);
  thumb->w_camera = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_camera, FALSE, FALSE, 0);
  thumb->w_lens = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(thumb->w_lens), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_lens, FALSE, FALSE, 0);
  thumb->w_focal = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(bbox), thumb->w_focal, FALSE, FALSE, 0);
  gtk_widget_set_no_show_all(thumb->w_alternative, TRUE);

  return thumb->widget;
}

void dt_thumbnail_resync_info(dt_thumbnail_t *thumb, const dt_image_t *const info)
{
  if(!thumb || !info) return;

  dt_thumbtable_copy_image(&thumb->info, info);

  if(!thumb->widget || !thumb->w_main) return;

  _thumb_update_rating_class(thumb);
  _thumb_update_icons(thumb);
  _create_alternative_view(thumb);
  _thumb_write_extension(thumb);

  if(thumb->w_color)
  {
    GtkDarktableThumbnailBtn *btn = (GtkDarktableThumbnailBtn *)thumb->w_color;
    btn->icon_flags = thumb->info.color_labels;
  }
}

dt_thumbnail_t *dt_thumbnail_new(int rowid, dt_thumbnail_overlay_t over, dt_thumbtable_t *table, dt_image_t *info)
{
  dt_thumbnail_t *thumb = calloc(1, sizeof(dt_thumbnail_t));

  thumb->rowid = rowid;
  thumb->over = over;
  thumb->table = table;
  thumb->zoomx = 0.;
  thumb->zoomy = 0.;
  thumb->job = NULL;
  thumb->img_h = 0;
  thumb->img_w = 0;
  dt_atomic_set_int(&thumb->destroying, FALSE);

  dt_pthread_mutex_init(&thumb->lock, NULL);

  dt_thumbnail_create_widget(thumb);

  dt_thumbnail_resync_info(thumb, info);
  dt_thumbnail_update_gui(thumb);

  // This will then only run on "selection_changed" event
  dt_thumbnail_update_selection(thumb, dt_selection_is_id_selected(darktable.selection, thumb->info.id));

  return thumb;
}

int dt_thumbnail_destroy(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb, 0);

  dt_atomic_set_int(&thumb->destroying, TRUE);

  // Wait for background jobs to finish before deleting the buffers they write in
  dt_pthread_mutex_lock(&thumb->lock);

  thumb->job = NULL;

  // remove multiple delayed gtk_widget_queue_draw triggers
  while(g_idle_remove_by_data(thumb->widget))
  ;
  while(g_idle_remove_by_data(thumb->w_image))
  ;

  if(thumb->img_surf && cairo_surface_get_reference_count(thumb->img_surf) > 0)
    cairo_surface_destroy(thumb->img_surf);
  thumb->img_surf = NULL;

  if(thumb->widget)
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->widget)), thumb->widget);
  thumb->widget = NULL;

  dt_pthread_mutex_unlock(&thumb->lock);

  dt_pthread_mutex_destroy(&thumb->lock);

  free(thumb);
  thumb = NULL;

  return 0;
}

void dt_thumbnail_update_gui(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb);
  _thumb_update_rating_class(thumb);
  GtkDarktableThumbnailBtn *btn = (GtkDarktableThumbnailBtn *)thumb->w_color;
  btn->icon_flags = thumb->info.color_labels;
  _thumb_write_extension(thumb);
  _thumb_update_icons(thumb);
  _create_alternative_view(thumb);
}

void dt_thumbnail_set_overlay(dt_thumbnail_t *thumb, dt_thumbnail_overlay_t mode)
{
  thumb_return_if_fails(thumb);
  thumb->over = mode;
}

// if update, the internal width and height, minus margins and borders, are written back in input
void _widget_set_size(GtkWidget *w, int *parent_width, int *parent_height, const gboolean update)
{
  GtkStateFlags state = gtk_widget_get_state_flags(w);
  GtkStyleContext *context = gtk_widget_get_style_context(w);

  GtkBorder margins;
  gtk_style_context_get_margin(context, state, &margins);

  int width = *parent_width - margins.left - margins.right;
  int height = *parent_height - margins.top - margins.bottom;

  if(width > 0 && height > 0)
  {
    gtk_widget_set_size_request(w, width, height);

    // unvisible widgets need to be allocated to be able to measure the size of flexible boxes.
    GtkAllocation alloc = { .x = 0, .y = 0, .width = width, .height = height };
    gtk_widget_size_allocate(w, &alloc);
  }

  if(update)
  {
    *parent_width = width;
    *parent_height = height;
  }
}


static int _thumb_resize_overlays(dt_thumbnail_t *thumb, int width, int height)
{
  thumb_return_if_fails(thumb, 0);

  // we need to squeeze reject + space + stars + space + colorlabels icons on a thumbnail width
  // that means a width of 4 + MAX_STARS icons size
  // all icons and spaces having a width of 2 * r1
  // inner margins are defined in css (margin_* values)

  // retrieves the size of the main icons in the top panel, thumbtable overlays shall not exceed that
  const float r1 = fminf(DT_PIXEL_APPLY_DPI(20) / 2., (float)width / (2.5 * (4 + MAX_STARS)));
  int icon_size = roundf(2 * r1);

  // reject icon
  gtk_widget_set_size_request(thumb->w_reject, icon_size, icon_size);

  // stars
  for(int i = 0; i < MAX_STARS; i++)
    gtk_widget_set_size_request(thumb->w_stars[i], icon_size, icon_size);

  // the color labels
  gtk_widget_set_size_request(thumb->w_color, icon_size, icon_size);

  // the local copy indicator
  _set_flag(thumb->w_local_copy, GTK_STATE_FLAG_ACTIVE, FALSE);
  gtk_widget_set_size_request(thumb->w_local_copy, icon_size, icon_size);

  // the altered icon
  gtk_widget_set_size_request(thumb->w_altered, icon_size, icon_size);

  // the group bouton
  gtk_widget_set_size_request(thumb->w_group, icon_size, icon_size);

  // the sound icon
  gtk_widget_set_size_request(thumb->w_audio, icon_size, icon_size);

  // the filmstrip cursor
  gtk_widget_set_size_request(thumb->w_cursor, 6.0 * r1, 1.5 * r1);

  // extension text
  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_size_new_absolute(icon_size * PANGO_SCALE * 0.9);
  pango_attr_list_insert(attrlist, attr);
  gtk_label_set_attributes(GTK_LABEL(thumb->w_ext), attrlist);
  pango_attr_list_unref(attrlist);

  return icon_size;
}

// This function is called only from the thumbtable, when the grid size changed.
// NOTE: thumb->widget is a grid cell. It should not get styled, especially not with margins/padding.
// Styling starts at thumb->w_main, aka .thumb-main in CSS, which gets centered in the grid cell.
// Overlays need to be set prior to calling this function because they can change internal sizings.
// It is expected that this function is called only when needed, that is if the size requirements actually
// changed, meaning this check needs to be done upstream because we internally nuke the image surface on every call.
void dt_thumbnail_resize(dt_thumbnail_t *thumb, int width, int height)
{
  thumb_return_if_fails(thumb);
  //fprintf(stdout, "calling resize on %i with overlay %i\n", thumb->info.id, thumb->over);

  if(width < 1 || height < 1) return;

  // widget resizing
  thumb->width = width;
  thumb->height = height;
  _widget_set_size(thumb->widget, &width, &height, TRUE);

  // Apply margins & borders on the main widget
  _widget_set_size(thumb->w_main, &width, &height, TRUE);

  // Update show/hide status for overlays now, because we pack them in boxes
  // so the children need to be sized before their parents for the boxes to have proper size.
  gtk_widget_show_all(thumb->widget);
  _thumb_update_icons(thumb);

  // Proceed with overlays resizing
  int icon_size = _thumb_resize_overlays(thumb, width, height);

  // Finish with updating the image size
  if(thumb->over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL)
  {
    // Persistent overlays shouldn't overlap with image, so resize it.
    // NOTE: this is why we need to allocate above
    int margin_bottom = gtk_widget_get_allocated_height(thumb->w_bottom_eb);
    int margin_top = gtk_widget_get_allocated_height(thumb->w_top_eb);
    height -= 2 * MAX(MAX(margin_top, margin_bottom), icon_size);
    // In case top and bottom bars of overlays have different sizes,
    // we resize symetrically to the largest.
  }
  _widget_set_size(thumb->w_image, &width, &height, FALSE);

  dt_thumbnail_image_refresh_real(thumb);
}

void dt_thumbnail_set_group_border(dt_thumbnail_t *thumb, dt_thumbnail_border_t border)
{
  thumb_return_if_fails(thumb);

  if(border == DT_THUMBNAIL_BORDER_NONE)
  {
    dt_gui_remove_class(thumb->widget, "dt_group_left");
    dt_gui_remove_class(thumb->widget, "dt_group_top");
    dt_gui_remove_class(thumb->widget, "dt_group_right");
    dt_gui_remove_class(thumb->widget, "dt_group_bottom");
    thumb->group_borders = DT_THUMBNAIL_BORDER_NONE;
    return;
  }
  if(border & DT_THUMBNAIL_BORDER_LEFT)
    dt_gui_add_class(thumb->widget, "dt_group_left");
  if(border & DT_THUMBNAIL_BORDER_TOP)
    dt_gui_add_class(thumb->widget, "dt_group_top");
  if(border & DT_THUMBNAIL_BORDER_RIGHT)
    dt_gui_add_class(thumb->widget, "dt_group_right");
  if(border & DT_THUMBNAIL_BORDER_BOTTOM)
    dt_gui_add_class(thumb->widget, "dt_group_bottom");

  thumb->group_borders |= border;
}

void dt_thumbnail_set_mouseover(dt_thumbnail_t *thumb, gboolean over)
{
  thumb_return_if_fails(thumb);

  if(thumb->mouse_over == over) return;
  thumb->mouse_over = over;
  if(thumb->table) thumb->table->rowid = thumb->rowid;

  _set_flag(thumb->widget, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_bottom_eb, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);
  _set_flag(thumb->w_main, GTK_STATE_FLAG_PRELIGHT, thumb->mouse_over);

  _thumb_update_icons(thumb);
}

// set if the thumbnail should react (mouse_over) to drag and drop
// note that it's just cosmetic as dropping occurs in thumbtable in any case
void dt_thumbnail_set_drop(dt_thumbnail_t *thumb, gboolean accept_drop)
{
  thumb_return_if_fails(thumb);

  if(accept_drop)
    gtk_drag_dest_set(thumb->w_main, GTK_DEST_DEFAULT_MOTION, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  else
    gtk_drag_dest_unset(thumb->w_main);
}

// Apply new mipmap on thumbnail
int dt_thumbnail_image_refresh_real(dt_thumbnail_t *thumb)
{
  thumb_return_if_fails(thumb, G_SOURCE_REMOVE);
  thumb->image_inited = FALSE;
  // Queue redraw on the drawing area itself: it's the widget that requests/regenerates the cairo surface.
  // Queueing only the parent overlay may not invalidate the drawing area's window, leaving stale (too small)
  // cached surfaces until some pointer event happens.
  if(thumb->w_image) gtk_widget_queue_draw(thumb->w_image);
  if(thumb->w_main) gtk_widget_queue_draw(thumb->w_main);
  return G_SOURCE_REMOVE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
