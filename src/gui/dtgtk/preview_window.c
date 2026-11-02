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
  GdkMonitor *monitor;      // monitor the window sits on (compared by pointer, never dereferenced)
  GdkMonitor *previous_monitor; // the one before: the OS may have moved the window away before telling us
  gboolean evacuate;        // its monitor was unplugged: to be reset as a first preview
  gboolean maximized;       // maximized when last shown, to restore it when the OS shows it again
} dt_preview_window_t;

/** Client size of a first preview. Wide enough for the title (image name and version). */
#define DT_PREVIEW_DEFAULT_SIZE 420

/**
 * Geometry the user last gave to a preview, reused by the next ones until the user changes it.
 * x, y are those of the outer frame (title bar included) in screen coordinates, so that they also say
 * which monitor it was on. Not valid before the first preview and after its monitor is unplugged.
 */
static struct
{
  gboolean valid;
  gboolean maximized;
  int x, y, width, height;
} _last = { 0 };

/** Open previews, in opening order: closing one gives the focus back to the most recent of the others. */
static GList *_previews = NULL;
static guint _evacuate_source = 0;

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

/**
 * @brief Remember the position and size the user gave to the window, for the next previews, and the monitor
 * it sits on. Called on every configure event, a drag included: a few reads and stores, the last event of a
 * drag is the final geometry. Maximized and fullscreen geometries are not the user's layout: skipped.
 */
static gboolean _preview_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  GdkWindow *gdk_window = gtk_widget_get_window(window);

  // Minimizing Ansel hides its previews and the OS parks them at a fake position (-32000 on Windows):
  // that is not a layout, nor a monitor change.
  if(!gdk_window_is_visible(gdk_window) || (gdk_window_get_state(gdk_window) & GDK_WINDOW_STATE_ICONIFIED))
    return FALSE;

  GdkMonitor *monitor = gdk_display_get_monitor_at_window(gtk_widget_get_display(window), gdk_window);
  if(monitor != preview->monitor)
  {
    preview->previous_monitor = preview->monitor;
    preview->monitor = monitor;
  }

  if(gdk_window_get_state(gdk_window) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)) return FALSE;

  GdkRectangle frame;
  gdk_window_get_frame_extents(gdk_window, &frame);
  _last.x = frame.x;
  _last.y = frame.y;
  gtk_window_get_size(GTK_WINDOW(window), &_last.width, &_last.height);
  _last.valid = TRUE;
  return FALSE;
}

/** @brief Remember the maximized state and, on Windows, prevent the taskbar or Win+D from minimizing the preview. */
static gboolean _preview_window_state_event(GtkWidget *window, GdkEventWindowState *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;

  // Windows hides the previews with Ansel when it is minimized, and shows them again unmaximized, wherever
  // they were. Maximize them again: GTK then picks the monitor they sit on.
  if(event->changed_mask & GDK_WINDOW_STATE_WITHDRAWN)
  {
    if(!(event->new_window_state & GDK_WINDOW_STATE_WITHDRAWN) && preview->maximized)
      gtk_window_maximize(GTK_WINDOW(window));
    return FALSE;
  }

  if(!(event->new_window_state & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_ICONIFIED)))
  {
    preview->maximized = (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    _last.maximized = preview->maximized;
  }

#ifdef GDK_WINDOWING_WIN32
  if(event->new_window_state & GDK_WINDOW_STATE_ICONIFIED)
  {
    gtk_window_deiconify(GTK_WINDOW(window));
    return TRUE;
  }
#endif
  return FALSE;
}

/**
 * @brief Reopen the previews that sat on an unplugged monitor as first previews: default size, not maximized,
 * centered on Ansel's window. Done from an idle callback, not from the monitor signal, because the OS is
 * rearranging windows at that point.
 */
static gboolean _preview_evacuate(gpointer user_data)
{
  _evacuate_source = 0;
#ifdef GDK_WINDOWING_WIN32
  // Windows minimizes Ansel on a monitor change, and hides the previews it owns with it.
  gtk_window_present(GTK_WINDOW(dt_gui_main_window()));
#endif

  GdkRectangle main_frame;
  gdk_window_get_frame_extents(gtk_widget_get_window(dt_gui_main_window()), &main_frame);

  // Loop on the open previews, looking for those marked.
  for(GList *l = _previews; l; l = g_list_next(l))
  {
    dt_preview_window_t *preview = (dt_preview_window_t *)l->data;
    if(!preview->evacuate) continue;

    preview->evacuate = FALSE;
    gtk_window_unmaximize(GTK_WINDOW(preview->window));
    gtk_window_resize(GTK_WINDOW(preview->window), DT_PREVIEW_DEFAULT_SIZE, DT_PREVIEW_DEFAULT_SIZE);
    gtk_window_move(GTK_WINDOW(preview->window), main_frame.x + (main_frame.width - DT_PREVIEW_DEFAULT_SIZE) / 2,
                    main_frame.y + (main_frame.height - DT_PREVIEW_DEFAULT_SIZE) / 2);
    gtk_window_present(GTK_WINDOW(preview->window));
  }
  return G_SOURCE_REMOVE;
}

/**
 * @brief Mark the previews that sat on the unplugged monitor, and forget the saved geometry, which belongs
 * to it or may be maximized there. The OS may already have moved a window away before this signal, hence
 * the previous monitor too. Monitors are compared by pointer: the OS also rearranges the others' geometry.
 */
static void _preview_monitor_removed(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  gboolean found = FALSE;
  for(GList *l = _previews; l; l = g_list_next(l))
  {
    dt_preview_window_t *preview = (dt_preview_window_t *)l->data;
    if(monitor != preview->monitor && monitor != preview->previous_monitor) continue;
    preview->evacuate = TRUE;
    found = TRUE;
  }
  if(!found) return;

  memset(&_last, 0, sizeof(_last));
  if(_evacuate_source == 0) _evacuate_source = g_idle_add(_preview_evacuate, NULL);
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
  gchar *name = g_strdup_printf(_("Ansel - Preview: %s [%d]"), img->filename, img->version);
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

  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_gui_main_window()));
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);

  // Reuse the geometry the user last gave to a preview, on whichever monitor it was, provided its center
  // still lies on a connected monitor. Otherwise open as a first preview: default size, centered on Ansel.
  GdkDisplay *display = gtk_widget_get_display(dialog);
  gboolean placed = FALSE;
  const int n_monitors = gdk_display_get_n_monitors(display);
  // Loop on the connected monitors, looking for the one holding the center of the saved geometry.
  for(int i = 0; _last.valid && !placed && i < n_monitors; i++)
  {
    GdkRectangle geometry;
    gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &geometry);
    const GdkPoint center = { _last.x + _last.width / 2, _last.y + _last.height / 2 };
    placed = center.x >= geometry.x && center.x < geometry.x + geometry.width
             && center.y >= geometry.y && center.y < geometry.y + geometry.height;
  }

  if(placed)
  {
    gtk_window_set_default_size(GTK_WINDOW(dialog), _last.width, _last.height);
    gtk_window_move(GTK_WINDOW(dialog), _last.x, _last.y);
    // Placed first, so that the window manager maximizes it on the monitor it was on.
    if(_last.maximized) gtk_window_maximize(GTK_WINDOW(dialog));
  }
  else
  {
    gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PREVIEW_DEFAULT_SIZE, DT_PREVIEW_DEFAULT_SIZE);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  }

  static gboolean monitors_connected = FALSE;
  if(!monitors_connected)
  {
    g_signal_connect(display, "monitor-removed", G_CALLBACK(_preview_monitor_removed), NULL);
    monitors_connected = TRUE;
  }

  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_close), NULL);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_close), NULL);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), preview);
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
#ifdef GDK_WINDOWING_WIN32
  // On Windows the minimize box follows the GDK decorations, not the window functions. With GDK_DECOR_ALL the
  // listed decorations are the ones left out: everything but the minimize button, which would also minimize Ansel.
  gtk_widget_realize(dialog);
  gdk_window_set_decorations(gtk_widget_get_window(dialog), GDK_DECOR_ALL | GDK_DECOR_MINIMIZE);
#endif
  gtk_widget_show_all(dialog);
  // The centering hint is for the first mapping only: GTK applies it again whenever the window is shown after
  // being hidden, as Windows does with the previews when Ansel is minimized and restored.
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_NONE);
}
