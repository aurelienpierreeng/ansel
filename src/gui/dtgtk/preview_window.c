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
#include "gui/dtgtk/thumbtable.h"
#include "control/control.h"
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
  int width;
  int height;
  GtkWidget *window;
  GtkWidget *main_window;
  gulong main_window_state_handler;
  gulong main_window_destroy_handler;
  gboolean iconified_with_main;
  gboolean geometry_tracking;
  gboolean restore_move_pending;
  int restore_x;
  int restore_y;
  gchar *monitor_name;
  gchar *return_monitor_name;
  int x_ratio;
  int y_ratio;
  int width_ratio;
  int height_ratio;
  gboolean geometry_valid;
  gboolean applying_geometry;
  guint monitor_update_source;
  guint geometry_apply_source;
  GdkScreen *screen;
  GdkDisplay *display;
  gulong monitors_changed_handler;
  gulong monitor_removed_handler;
  guint geometry_tracking_source;
  guint64 window_id;
} dt_preview_window_t;

void _preview_redraw(gpointer instance, dt_preview_window_t *preview);

static guint64 _preview_next_window_id = 0;
static GList *_preview_windows = NULL;
static dt_preview_window_t *_preview_focused_window = NULL;
static gboolean _preview_geometry_valid = FALSE;
static int _preview_x = 0;
static int _preview_y = 0;
static int _preview_width = 420;
static int _preview_height = 420;

static gboolean _preview_finish_monitor_geometry(gpointer user_data);

/**
 * @brief Identify a monitor for this session without relying on its enumeration index.
 */
static gchar *_preview_monitor_name(GdkMonitor *monitor)
{
  const char *manufacturer = gdk_monitor_get_manufacturer(monitor);
  const char *model = gdk_monitor_get_model(monitor);
  GdkRectangle geometry = { 0 };
  gdk_monitor_get_geometry(monitor, &geometry);
  gchar *name = g_strdup_printf("%s_%s@%d_%d",
                                IS_NULL_PTR(manufacturer) ? "" : manufacturer,
                                IS_NULL_PTR(model) ? "Unknown_monitor" : model,
                                geometry.x, geometry.y);
  g_strdelimit(name, " /", '_');
  return name;
}

/**
 * @brief Keep the current preview position and size for later previews this session.
 */
static void _preview_save_geometry(GtkWidget *window, dt_preview_window_t *preview)
{
  if(!preview->geometry_tracking) return;
  GdkWindow *gdk_window = gtk_widget_get_window(window);
  if(!IS_NULL_PTR(gdk_window))
  {
    GdkDisplay *display = gtk_widget_get_display(window);
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gdk_window);
    if(!IS_NULL_PTR(monitor))
    {
      gchar *monitor_name = _preview_monitor_name(monitor);
      if(g_strcmp0(preview->monitor_name, monitor_name) != 0)
      {
        dt_free(preview->monitor_name);
        preview->monitor_name = monitor_name;
      }
      else
        dt_free(monitor_name);
    }
  }

  if(preview->applying_geometry || gtk_window_is_maximized(GTK_WINDOW(window))) return;
  int x = 0, y = 0, width = 0, height = 0;
  gtk_window_get_position(GTK_WINDOW(window), &x, &y);
  gtk_window_get_size(GTK_WINDOW(window), &width, &height);
  if(width < 120 || height < 120) return;

  if(!IS_NULL_PTR(gdk_window))
  {
    GdkDisplay *display = gtk_widget_get_display(window);
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gdk_window);
    if(!IS_NULL_PTR(monitor))
    {
      GdkRectangle workarea = { 0 };
      gdk_monitor_get_workarea(monitor, &workarea);
      if(workarea.width > 0 && workarea.height > 0)
      {
        preview->x_ratio = CLAMP((gint64)(x - workarea.x) * 10000 / workarea.width, 0, 10000);
        preview->y_ratio = CLAMP((gint64)(y - workarea.y) * 10000 / workarea.height, 0, 10000);
        preview->width_ratio = CLAMP((gint64)width * 10000 / workarea.width, 1, 10000);
        preview->height_ratio = CLAMP((gint64)height * 10000 / workarea.height, 1, 10000);
        preview->geometry_valid = TRUE;
      }
    }
  }

  _preview_x = x;
  _preview_y = y;
  _preview_width = width;
  _preview_height = height;
  _preview_geometry_valid = TRUE;
}

/**
 * @brief Apply one preview's saved relative layout to Ansel's current monitor.
 */
static void _preview_apply_monitor_geometry(dt_preview_window_t *preview, GdkMonitor *monitor)
{
  GdkRectangle workarea = { 0 };
  gdk_monitor_get_workarea(monitor, &workarea);
  if(workarea.width <= 0 || workarea.height <= 0) return;

  int width = preview->geometry_valid
                  ? MAX(120, (gint64)workarea.width * preview->width_ratio / 10000)
                  : _preview_width;
  int height = preview->geometry_valid
                   ? MAX(120, (gint64)workarea.height * preview->height_ratio / 10000)
                   : _preview_height;
  width = MIN(width, workarea.width);
  height = MIN(height, workarea.height);
  int x = workarea.x + (preview->geometry_valid
                            ? (gint64)workarea.width * preview->x_ratio / 10000
                            : (workarea.width - width) / 2);
  int y = workarea.y + (preview->geometry_valid
                            ? (gint64)workarea.height * preview->y_ratio / 10000
                            : (workarea.height - height) / 2);
  x = CLAMP(x, workarea.x, workarea.x + workarea.width - width);
  y = CLAMP(y, workarea.y, workarea.y + workarea.height - height);

  preview->applying_geometry = TRUE;
  gtk_window_resize(GTK_WINDOW(preview->window), width, height);
  gtk_window_move(GTK_WINDOW(preview->window), x, y);
  _preview_x = x;
  _preview_y = y;
  _preview_width = width;
  _preview_height = height;
  _preview_geometry_valid = TRUE;

  gchar *monitor_name = _preview_monitor_name(monitor);
  dt_free(preview->monitor_name);
  preview->monitor_name = monitor_name;
  if(preview->geometry_apply_source != 0)
    g_source_remove(preview->geometry_apply_source);
  preview->geometry_apply_source =
      g_timeout_add(350, _preview_finish_monitor_geometry, preview);
}

/**
 * @brief Finish a monitor move after GTK and the window manager process its geometry requests.
 */
static gboolean _preview_finish_monitor_geometry(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->geometry_apply_source = 0;
  preview->applying_geometry = FALSE;
  _preview_save_geometry(preview->window, preview);
  return G_SOURCE_REMOVE;
}

/**
 * @brief Transfer a preview from a disconnected monitor to the monitor containing Ansel.
 */
static gboolean _preview_monitor_update_idle(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->monitor_update_source = 0;
  GdkMonitor *target = NULL;
  GdkWindow *main_gdk_window = gtk_widget_get_window(dt_gui_main_window());
  if(!IS_NULL_PTR(main_gdk_window))
    target = gdk_display_get_monitor_at_window(preview->display, main_gdk_window);
  if(IS_NULL_PTR(target))
  {
    target = gdk_display_get_primary_monitor(preview->display);
    if(IS_NULL_PTR(target) && gdk_display_get_n_monitors(preview->display) > 0)
      target = gdk_display_get_monitor(preview->display, 0);
  }

  if(!IS_NULL_PTR(target))
    _preview_apply_monitor_geometry(preview, target);
  dt_free(preview->return_monitor_name);
  preview->return_monitor_name = NULL;
  return G_SOURCE_REMOVE;
}

/**
 * @brief Coalesce monitor topology notifications before moving preview windows.
 */
static void _preview_schedule_monitor_update(dt_preview_window_t *preview)
{
  if(preview->monitor_update_source == 0)
    preview->monitor_update_source = g_timeout_add(150, _preview_monitor_update_idle, preview);
}

static void _preview_monitors_changed(GdkScreen *screen, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(!IS_NULL_PTR(preview->return_monitor_name))
    _preview_schedule_monitor_update(preview);
}

static void _preview_monitor_removed(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  gchar *removed_monitor_name = _preview_monitor_name(monitor);
  if(g_strcmp0(preview->monitor_name, removed_monitor_name) == 0)
  {
    dt_free(preview->return_monitor_name);
    preview->return_monitor_name = removed_monitor_name;
    removed_monitor_name = NULL;
    _preview_schedule_monitor_update(preview);
  }
  dt_free(removed_monitor_name);
}

/**
 * @brief Center a new preview over the lighttable when no saved geometry exists.
 */
static gboolean _preview_center_on_lighttable(const int window_width, const int window_height,
                                              int *x, int *y)
{
  GtkWidget *main_window = dt_gui_main_window();
  GtkWidget *lighttable = dt_gui_get_ui()->thumbtable_lighttable->parent_overlay;
  GdkWindow *main_gdk_window = gtk_widget_get_window(main_window);
  if(IS_NULL_PTR(main_gdk_window)) return FALSE;

  int lighttable_x = 0, lighttable_y = 0;
  if(!gtk_widget_translate_coordinates(lighttable, main_window, 0, 0, &lighttable_x, &lighttable_y))
    return FALSE;

  int main_x = 0, main_y = 0;
  gdk_window_get_origin(main_gdk_window, &main_x, &main_y);
  const int lighttable_width = gtk_widget_get_allocated_width(lighttable);
  const int lighttable_height = gtk_widget_get_allocated_height(lighttable);
  *x = main_x + lighttable_x + (lighttable_width - window_width) / 2;
  *y = main_y + lighttable_y + (lighttable_height - window_height) / 2;
  return TRUE;
}

/**
 * @brief Minimize and restore independent previews with Ansel's main window.
 * @details Previews remain non-transient so they can be switched behind Ansel from the taskbar.
 */
static gboolean _preview_main_window_state_event(GtkWidget *window, GdkEventWindowState *event,
                                                 gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(!(event->changed_mask & GDK_WINDOW_STATE_ICONIFIED)) return FALSE;

  if(event->new_window_state & GDK_WINDOW_STATE_ICONIFIED)
  {
    GdkWindow *preview_window = gtk_widget_get_window(preview->window);
    if(gtk_widget_get_visible(preview->window) && !IS_NULL_PTR(preview_window)
       && !(gdk_window_get_state(preview_window) & GDK_WINDOW_STATE_ICONIFIED))
    {
      preview->iconified_with_main = TRUE;
      gtk_window_iconify(GTK_WINDOW(preview->window));
    }
  }
  else if(preview->iconified_with_main)
  {
    preview->iconified_with_main = FALSE;
    gtk_window_deiconify(GTK_WINDOW(preview->window));
  }
  return FALSE;
}

/**
 * @brief Use the last-focused preview as the placement source for the next preview.
 */
static gboolean _preview_focus_in_event(GtkWidget *window, GdkEventFocus *event,
                                        gpointer user_data)
{
  _preview_focused_window = (dt_preview_window_t *)user_data;
  return FALSE;
}

/**
 * @brief Forget the main-window signal connections if it is destroyed first.
 */
static void _preview_main_window_destroy(GtkWidget *window, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->main_window = NULL;
  preview->main_window_state_handler = 0;
  preview->main_window_destroy_handler = 0;
}

static gboolean _preview_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _preview_save_geometry(window, preview);
  return FALSE;
}

/**
 * @brief Let the window manager apply restored placement before saving preview geometry.
 */
static gboolean _preview_start_geometry_tracking(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->geometry_tracking_source = 0;
  if(preview->restore_move_pending)
  {
    // Re-apply after mapping: some window managers ignore the pre-map position request.
    gtk_window_move(GTK_WINDOW(preview->window), preview->restore_x, preview->restore_y);
    preview->restore_move_pending = FALSE;
    preview->geometry_tracking_source =
        g_timeout_add(250, _preview_start_geometry_tracking, preview);
    return G_SOURCE_REMOVE;
  }

  preview->geometry_tracking = TRUE;
  _preview_save_geometry(preview->window, preview);
  return G_SOURCE_REMOVE;
}

/**
 * @brief Close the floating preview when Escape is pressed.
 */
static gboolean _preview_key_press_event(GtkWidget *window, GdkEventKey *event, gpointer user_data)
{
  if(event->keyval != GDK_KEY_Escape) return FALSE;
  gtk_widget_destroy(window);
  return TRUE;
}

static gboolean _preview_delete_event(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  gtk_widget_destroy(window);
  return TRUE;
}

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _preview_save_geometry(dialog, preview);
  if(preview->geometry_tracking_source != 0)
    g_source_remove(preview->geometry_tracking_source);
  if(preview->monitor_update_source != 0)
    g_source_remove(preview->monitor_update_source);
  if(preview->geometry_apply_source != 0)
    g_source_remove(preview->geometry_apply_source);
  if(!IS_NULL_PTR(preview->screen) && preview->monitors_changed_handler != 0)
    g_signal_handler_disconnect(preview->screen, preview->monitors_changed_handler);
  if(!IS_NULL_PTR(preview->display) && preview->monitor_removed_handler != 0)
    g_signal_handler_disconnect(preview->display, preview->monitor_removed_handler);
  if(!IS_NULL_PTR(preview->main_window))
  {
    if(preview->main_window_state_handler != 0)
      g_signal_handler_disconnect(preview->main_window, preview->main_window_state_handler);
    if(preview->main_window_destroy_handler != 0)
      g_signal_handler_disconnect(preview->main_window, preview->main_window_destroy_handler);
  }

  _preview_windows = g_list_remove(_preview_windows, preview);
  if(_preview_focused_window == preview)
    _preview_focused_window = NULL;

  // Disconnect before freeing: otherwise the handler can fire with a dangling preview pointer.
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_preview_redraw), preview);
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);
  dt_free(preview->monitor_name);
  dt_free(preview->return_monitor_name);
  dt_free(preview);
}

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview) || IS_NULL_PTR(allocation)) return;
  if(allocation->width < 2 || allocation->height < 2) return;
  if(preview->width == allocation->width && preview->height == allocation->height) return;

  preview->width = allocation->width;
  preview->height = allocation->height;

  /* Stop rendering a surface for an obsolete allocation as soon as the popup size changes. */
  dt_view_image_surface_fetcher_invalidate(&preview->fetcher, &preview->surface);
  gtk_widget_queue_draw(widget);
}

void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_colorprofiles_xyz_to_display(XYZ, RGB);
}

static gboolean _thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview)) return TRUE;

  const double start = dt_get_wtime();
  int w = gtk_widget_get_allocated_width(widget);
  int h = gtk_widget_get_allocated_height(widget);

  dt_aligned_pixel_t bg_color;
  _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);
  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  int borders = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  w -= 2 * borders;
  h -= 2 * borders;

  // FIXME: the preview surface is still color-managed with the main window's monitor profile.
  const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&preview->fetcher, preview->imgid, w, h, &preview->surface, widget, 0);

  if(preview->surface && res == DT_VIEW_SURFACE_OK)
  {
    const int width = cairo_image_surface_get_width(preview->surface);
    const int height = cairo_image_surface_get_height(preview->surface);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(preview->surface, &sx, &sy);
    const double logical_width = width / sx;
    const double logical_height = height / sy;

    cairo_save(cr);
    const double x_offset = (w - logical_width) / 2. + borders;
    const double y_offset = (h - logical_height) / 2. + borders;
    cairo_set_source_surface(cr, preview->surface, x_offset, y_offset);

    GdkRGBA im_color;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
    cairo_paint_with_alpha(cr, im_color.alpha);
    gtk_render_frame(context, cr, borders, borders, w, h);
    cairo_restore(cr);
  }
  else
    dt_control_draw_busy_msg(cr, w, h);

  dt_print(DT_DEBUG_LIGHTTABLE, "Redrawing the preview window for %i in %0.04f sec\n",
           preview->imgid, dt_get_wtime() - start);
  return TRUE;
}

void _preview_redraw(gpointer instance, dt_preview_window_t *preview)
{
  gtk_widget_queue_draw(preview->area);
}

/**
 * @brief Open an image preview using shared session geometry or centered 420×420 geometry.
 * @param imgid Image to display.
 */
void dt_preview_window_spawn(const int32_t imgid)
{
  dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
  if(IS_NULL_PTR(preview)) return;
  preview->imgid = imgid;
  dt_view_image_surface_fetcher_init(&preview->fetcher);

  GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  preview->window = dialog;
  preview->window_id = ++_preview_next_window_id;
  gchar *role = g_strdup_printf("ansel-preview-%" G_GUINT64_FORMAT, preview->window_id);
  gtk_window_set_role(GTK_WINDOW(dialog), role);
  dt_free(role);

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  gchar *name = g_strdup_printf(_("Ansel - Preview: %s [%d]"), img->filename, img->version);
  dt_image_cache_read_release(img);
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  dt_free(name);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_decorated(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_NORMAL);

  GtkWidget *main_window = dt_gui_main_window();
  GdkMonitor *maximized_monitor = NULL;
  int x = _preview_x, y = _preview_y;
  int width = _preview_geometry_valid ? _preview_width : 420;
  int height = _preview_geometry_valid ? _preview_height : 420;
  gboolean has_focused_geometry = FALSE;
  dt_preview_window_t *focused_preview = _preview_focused_window;
  if(!IS_NULL_PTR(focused_preview) && gtk_widget_get_visible(focused_preview->window))
  {
    GdkWindow *focused_gdk_window = gtk_widget_get_window(focused_preview->window);
    if(!IS_NULL_PTR(focused_gdk_window))
    {
      GdkMonitor *focused_monitor =
          gdk_display_get_monitor_at_window(focused_preview->display, focused_gdk_window);
      if(!IS_NULL_PTR(focused_monitor))
      {
        if(gtk_window_is_maximized(GTK_WINDOW(focused_preview->window)))
        {
          maximized_monitor = focused_monitor;
          has_focused_geometry = TRUE;
        }
        else
        {
          gtk_window_get_position(GTK_WINDOW(focused_preview->window), &x, &y);
          gtk_window_get_size(GTK_WINDOW(focused_preview->window), &width, &height);
          has_focused_geometry = width >= 120 && height >= 120;
        }
      }
    }
  }

  int preview_width = width, preview_height = height;
  if(!IS_NULL_PTR(maximized_monitor))
  {
    GdkRectangle workarea = { 0 };
    gdk_monitor_get_workarea(maximized_monitor, &workarea);
    preview_width = MIN(preview_width, workarea.width);
    preview_height = MIN(preview_height, workarea.height);
    x = workarea.x + MAX(0, (workarea.width - preview_width) / 2);
    y = workarea.y + MAX(0, (workarea.height - preview_height) / 2);
  }
  const gboolean has_saved_geometry = _preview_geometry_valid;
  gtk_window_set_default_size(GTK_WINDOW(dialog), preview_width, preview_height);
  const gboolean has_center_position =
      IS_NULL_PTR(maximized_monitor) && !has_focused_geometry && !has_saved_geometry
      && _preview_center_on_lighttable(preview_width, preview_height, &x, &y);
  preview->restore_move_pending =
      IS_NULL_PTR(maximized_monitor)
      && (has_focused_geometry || has_saved_geometry || has_center_position);
  preview->restore_x = x;
  preview->restore_y = y;
  if(preview->restore_move_pending || !IS_NULL_PTR(maximized_monitor))
    gtk_window_move(GTK_WINDOW(dialog), x, y);

  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_delete_event), preview);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), preview);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_key_press_event), preview);
  g_signal_connect(G_OBJECT(dialog), "focus-in-event",
                   G_CALLBACK(_preview_focus_in_event), preview);

  preview->area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(preview->area, TRUE);
  gtk_widget_set_vexpand(preview->area, TRUE);
  gtk_widget_set_halign(preview->area, GTK_ALIGN_FILL);
  gtk_widget_set_valign(preview->area, GTK_ALIGN_FILL);
  gtk_widget_set_can_focus(preview->area, TRUE);
  gtk_widget_set_size_request(preview->area, 120, 120);
  gtk_container_add(GTK_CONTAINER(dialog), preview->area);
  g_signal_connect(G_OBJECT(preview->area), "draw", G_CALLBACK(_thumb_draw_image), preview);
  g_signal_connect(G_OBJECT(preview->area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED,
                                 G_CALLBACK(_preview_redraw), preview);

  preview->main_window = main_window;
  preview->display = gtk_widget_get_display(dialog);
  preview->screen = gtk_widget_get_screen(dialog);
  preview->monitor_removed_handler =
      g_signal_connect(preview->display, "monitor-removed",
                       G_CALLBACK(_preview_monitor_removed), preview);
  preview->monitors_changed_handler =
      g_signal_connect(preview->screen, "monitors-changed",
                       G_CALLBACK(_preview_monitors_changed), preview);
  // Keep the preview in Ansel's window stack so taskbar activation remains with the application.
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main_window));
  preview->main_window_state_handler =
      g_signal_connect(main_window, "window-state-event",
                       G_CALLBACK(_preview_main_window_state_event), preview);
  preview->main_window_destroy_handler =
      g_signal_connect(main_window, "destroy", G_CALLBACK(_preview_main_window_destroy), preview);

  if(!IS_NULL_PTR(maximized_monitor))
    gtk_window_maximize(GTK_WINDOW(dialog));
  gtk_widget_show_all(dialog);
  _preview_windows = g_list_append(_preview_windows, preview);
  gtk_widget_grab_focus(preview->area);
  gtk_window_present(GTK_WINDOW(dialog));
  preview->geometry_tracking_source =
      g_timeout_add(250, _preview_start_geometry_tracking, preview);
}
