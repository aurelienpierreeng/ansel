#include "common/darktable.h"
#include "control/control.h"
#include "common/image_cache.h"
#include "views/view.h"

#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


static void _dt_mipmaps_updated_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(!user_data) return;
  GtkWidget *area = (GtkWidget *)user_data;
  gtk_widget_queue_draw(area);
}


void _close_preview_popup(GtkWidget *dialog, gint response_id, gpointer data)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_mipmaps_updated_callback), data);
}


// Note: the only event that will trigger a redraw is resizing the window,
// or getting a new mipmap when the cache pipeline finishes,
// which will all require refreshing the image surface.
// So we don't do clever double-buffering, caching and invalidation here.
static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{

  while(gtk_events_pending())
  {
     gtk_main_iteration();
  }

  const double start = dt_get_wtime();

  int32_t imgid = GPOINTER_TO_INT(user_data);
  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  cairo_surface_t *surface = NULL;
  dt_view_surface_value_t res = dt_view_image_get_surface(imgid, w, h, &surface, 0);

  if(surface && res == DT_VIEW_SURFACE_OK)
  {
    // The image is immediately available
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);

    // we draw the image
    cairo_save(cr);
    const float scaler = 1.0f / darktable.gui->ppd;
    cairo_scale(cr, scaler, scaler);

    double x_offset = (w * darktable.gui->ppd - width) / 2.;
    double y_offset = (h * darktable.gui->ppd - height) / 2.;
    cairo_set_source_surface(cr, surface, x_offset, y_offset);

    // get the transparency value
    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // and eventually the image border
    gtk_render_frame(context, cr, 0, 0, w * darktable.gui->ppd, h * darktable.gui->ppd);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
  }
  else
  {
    dt_control_draw_busy_msg(cr, w, h);
  }

  dt_print(DT_DEBUG_LIGHTTABLE, "Redrawing the preview window for %i in %0.04f sec\n", imgid,
    dt_get_wtime() - start);

  return TRUE;
}


void dt_preview_window_spawn(const int32_t imgid)
{
  GtkWidget *dialog = gtk_dialog_new();

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  gchar *name = g_strdup_printf(_("Ansel - Preview : %s"), img->filename);
  dt_image_cache_read_release(darktable.image_cache, img);
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  g_free(name);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 350, 350);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_close_preview_popup), dialog);

  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(area, TRUE);
  gtk_widget_set_vexpand(area, TRUE);
  gtk_widget_set_halign(area, GTK_ALIGN_FILL);
  gtk_widget_set_valign(area, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(area, 350, 350);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), area, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(area), "draw", G_CALLBACK(_thumb_draw_image), GINT_TO_POINTER(imgid));


  gtk_widget_set_visible(area, TRUE);
  gtk_widget_show_all(dialog);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                                  G_CALLBACK(_dt_mipmaps_updated_callback), dialog);
}
