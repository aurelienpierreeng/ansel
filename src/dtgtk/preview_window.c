/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    
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
#include "control/control.h"
#include "common/image_cache.h"
#include "views/view.h"

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct dt_preview_window_t
{
  int32_t imgid;
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
  int width;
  int height;
} dt_preview_window_t;

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);
  dt_free(preview);
}

static void _close_preview_popup(GtkWidget *dialog, gint response_id, gpointer data)
{
  gtk_widget_destroy(dialog);
}

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(!preview || !allocation) return;
  if(allocation->width < 2 || allocation->height < 2) return;
  if(preview->width == allocation->width && preview->height == allocation->height) return;

  preview->width = allocation->width;
  preview->height = allocation->height;

  /* The async fetcher can stop its current pixelpipe through the request-owned
   * shutdown flag. Trigger that as soon as the popup size changes so we don't
   * keep rendering a surface for an obsolete allocation. */
  dt_view_image_surface_fetcher_invalidate(&preview->fetcher, &preview->surface);
  gtk_widget_queue_draw(widget);
}

static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview)) return TRUE;

  const double start = dt_get_wtime();

  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&preview->fetcher, preview->imgid, w, h, &preview->surface, widget, 0);

  if(preview->surface && res == DT_VIEW_SURFACE_OK)
  {
    // The image is immediately available
    int width = cairo_image_surface_get_width(preview->surface);
    int height = cairo_image_surface_get_height(preview->surface);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(preview->surface, &sx, &sy);
    const double logical_width = width / sx;
    const double logical_height = height / sy;

    // we draw the image
    cairo_save(cr);
    double x_offset = (w - logical_width) / 2.;
    double y_offset = (h - logical_height) / 2.;
    cairo_set_source_surface(cr, preview->surface, x_offset, y_offset);

    // get the transparency value
    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // and eventually the image border
    gtk_render_frame(context, cr, 0, 0, w, h);
    cairo_restore(cr);
  }
  else
  {
    dt_control_draw_busy_msg(cr, w, h);
  }

  dt_print(DT_DEBUG_LIGHTTABLE, "Redrawing the preview window for %i in %0.04f sec\n", preview->imgid,
    dt_get_wtime() - start);

  return TRUE;
}


void dt_preview_window_spawn(const int32_t imgid)
{
  dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
  preview->imgid = imgid;
  dt_view_image_surface_fetcher_init(&preview->fetcher);

  GtkWidget *dialog = gtk_dialog_new();

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  gchar *name = g_strdup_printf(_("Ansel - Preview : %s"), img->filename);
  dt_image_cache_read_release(darktable.image_cache, img);
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  dt_free(name);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 350, 350);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_close_preview_popup), NULL);
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);

  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(area, TRUE);
  gtk_widget_set_vexpand(area, TRUE);
  gtk_widget_set_halign(area, GTK_ALIGN_FILL);
  gtk_widget_set_valign(area, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(area, 350, 350);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), area, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(_thumb_draw_image), preview);
  g_signal_connect(G_OBJECT(area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);


  gtk_widget_set_visible(area, TRUE);
  gtk_widget_show_all(dialog);
}
