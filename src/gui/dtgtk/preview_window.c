/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
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
#include "common/logging.h"
#include "colorprofiles/colorspaces.h"   // dt_colorprofiles_xyz_to_display()
#include "common/colorspaces_inline_conversions.h"   // dt_Lab_to_XYZ
#include "common/conf.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "common/times.h"
#include "control/signal.h"
#include "gui/application.h"
#include "control/control.h"
#include "control/user_message.h"
#include "caches/image_cache.h"
#include "views/view.h"

#include <gtk/gtk.h>
#include "widgets/widget_settings.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct dt_preview_window_t
{
  int32_t imgid;
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
  GtkWidget *area;
  GtkWidget *window;
  int width;
  int height;
  double aspect; // width / height of the displayed image, 0 until it is rendered
} dt_preview_window_t;

/** Client size and maximized state of the last preview that was moved or resized: the next ones open alike. */
static int _last_width = 420;
static int _last_height = 420;
static gboolean _last_maximized = FALSE;

/** Open previews, in opening order: closing one gives the focus back to the most recent of the others. */
static GList *_previews = NULL;

void _preview_redraw(gpointer instance, dt_preview_window_t *preview);

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _previews = g_list_remove(_previews, preview);
  // Disconnect from DARKROOM_UI_CHANGED before freeing: otherwise the handler keeps
  // firing with a dangling `preview` pointer and crashes on the next UI change
  // (e.g. moving the border-size slider).
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_preview_redraw), preview);
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);
  dt_free(preview);
}

/**
 * @brief Close on Escape or on the close button, and focus the most recently opened preview still alive
 * (the main window when none is left), so that repeated Escape peels them off one after the other.
 */
static gboolean _preview_close(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  if(event->type == GDK_KEY_PRESS && event->key.keyval != GDK_KEY_Escape) return FALSE;

  gtk_widget_destroy(window);
  GList *last = g_list_last(_previews);
  GtkWidget *next = IS_NULL_PTR(last) ? dt_gui_main_window() : ((dt_preview_window_t *)last->data)->window;
  gtk_window_present(GTK_WINDOW(next));
  return TRUE;
}

/** Remember the size the user gave to the window, for the next previews. Maximized or fullscreen sizes are not. */
static gboolean _preview_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  const GdkWindowState state = gdk_window_get_state(gtk_widget_get_window(window));
  if(!(state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)))
    gtk_window_get_size(GTK_WINDOW(window), &_last_width, &_last_height);
  return FALSE;
}

/**
 * @brief Remember the maximized state, and release the aspect ratio hint while maximized: the window manager
 * fills the screen whatever the image ratio, and the hint would fight it.
 */
static gboolean _preview_window_state_event(GtkWidget *window, GdkEventWindowState *event, gpointer user_data)
{
  const dt_preview_window_t *preview = (const dt_preview_window_t *)user_data;
  if(!(event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED)) return FALSE;

  const gboolean maximized = (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
  _last_maximized = maximized;
  if(preview->aspect > 0.)
  {
    GdkGeometry hints = { .min_aspect = preview->aspect, .max_aspect = preview->aspect };
    gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, &hints, maximized ? 0 : GDK_HINT_ASPECT);
  }
  return FALSE;
}

/**
 * @brief Give the window the aspect ratio of the rendered image (crop and orientation included), so that no grey
 * margin is left around it, and keep it while the user resizes. The width is kept and the height follows.
 */
static void _preview_fit_aspect(dt_preview_window_t *preview, const double aspect)
{
  preview->aspect = aspect;
  GtkWindow *window = GTK_WINDOW(preview->window);
  if(gdk_window_get_state(gtk_widget_get_window(preview->window)) & GDK_WINDOW_STATE_MAXIMIZED) return;

  int width = 0, height = 0;
  gtk_window_get_size(window, &width, &height);
  // X11 window managers may satisfy a newly installed hint by shrinking the width to the current height:
  // resize without the hint first, so that the width stays the reference for portrait images too.
  gtk_window_set_geometry_hints(window, NULL, NULL, 0);
  gtk_window_resize(window, width, (int)(width / aspect + 0.5));
  GdkGeometry hints = { .min_aspect = aspect, .max_aspect = aspect };
  gtk_window_set_geometry_hints(window, NULL, &hints, GDK_HINT_ASPECT);
}

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview) || IS_NULL_PTR(allocation)) return;
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

// TODO: we color-manage the preview window assuming it sits on the same screen as the main window
// Will need to grab the color profile of the actual monitor where it sits.
void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_colorprofiles_xyz_to_display(XYZ, RGB);
}

static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview)) return TRUE;

  const double start = dt_get_wtime();

  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  // Paint background
  dt_aligned_pixel_t bg_color;
  _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);
  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  int borders = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  w -= 2 * borders;
  h -= 2 * borders;

  // FIXME: this gets the color-managed image surface using the color profile of the monitor
  // where the main window sits. Need to detect the monitor of the preview window instead.
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

    // Ignore tiny surfaces and changes below the rounding noise, so that the ratio does not jitter.
    const double aspect = logical_width / logical_height;
    if(logical_width >= 50. && logical_height >= 50.
       && (preview->aspect <= 0. || fabs(aspect / preview->aspect - 1.) > 0.01))
      _preview_fit_aspect(preview, aspect);

    // we draw the image
    cairo_save(cr);
    double x_offset = (w - logical_width) / 2. + borders;
    double y_offset = (h - logical_height) / 2. + borders;
    cairo_set_source_surface(cr, preview->surface, x_offset, y_offset);

    // get the transparency value
    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);

    // and eventually the image border
    gtk_render_frame(context, cr, borders, borders, w, h);
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


void _preview_redraw(gpointer instance, dt_preview_window_t *preview)
{
  gtk_widget_queue_draw(preview->area);
}


void dt_preview_window_spawn(const int32_t imgid)
{
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img))
  {
    dt_control_log(_("No image to open !"));
    return;
  }
  gchar *name = g_strdup_printf(_("Ansel - Preview : %s"), img->filename);
  dt_image_cache_read_release(img);

  dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
  preview->imgid = imgid;
  dt_view_image_surface_fetcher_init(&preview->fetcher);

  // A plain top-level window rather than a GtkDialog, to get the maximize button.
  GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  preview->window = dialog;
  _previews = g_list_append(_previews, preview);
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  dt_free(name);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_gui_main_window()));
  gtk_window_set_default_size(GTK_WINDOW(dialog), _last_width, _last_height);
  if(_last_maximized) gtk_window_maximize(GTK_WINDOW(dialog));
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_close), NULL);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_close), NULL);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), NULL);
  g_signal_connect(G_OBJECT(dialog), "window-state-event", G_CALLBACK(_preview_window_state_event), preview);

  preview->area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(preview->area, TRUE);
  gtk_widget_set_vexpand(preview->area, TRUE);
  gtk_widget_set_size_request(preview->area, 120, 120);
  gtk_container_add(GTK_CONTAINER(dialog), preview->area);
  g_signal_connect(G_OBJECT(preview->area), "draw", G_CALLBACK(_thumb_draw_image), preview);
  g_signal_connect(G_OBJECT(preview->area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED, G_CALLBACK(_preview_redraw), preview);

  gtk_widget_set_visible(preview->area, TRUE);
  gtk_widget_show_all(dialog);
}
