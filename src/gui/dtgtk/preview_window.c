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
#include <stdio.h>
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
  GdkScreen *screen;
  gulong monitors_changed_handler;
  GdkDisplay *display;
  gulong monitor_added_handler;
  gulong monitor_removed_handler;
  gchar *monitor_name;
  gchar *return_monitor_name;
  int x_ratio;
  int y_ratio;
  int width_ratio;
  int height_ratio;
  gboolean maximized;
  gboolean geometry_valid;
  gboolean applying_geometry;
  gboolean geometry_tracking;
  gboolean user_geometry_set;
  gboolean persist_geometry;
  guint geometry_apply_source;
  int tracked_x;
  int tracked_y;
  int tracked_width;
  int tracked_height;
  guint geometry_tracking_source;
  guint monitor_update_source;
  guint64 window_id;
} dt_preview_window_t;

void _preview_redraw(gpointer instance, dt_preview_window_t *preview);

static guint64 _preview_next_window_id = 0;
static GList *_preview_windows = NULL;
static guint _preview_raise_source = 0;

static gboolean _preview_raise_all(gpointer user_data)
{
  _preview_raise_source = 0;
  // Restore previews after Ansel's input event finishes, without activating each window.
  for(GList *item = _preview_windows; !IS_NULL_PTR(item); item = g_list_next(item))
  {
    dt_preview_window_t *preview = (dt_preview_window_t *)item->data;
    GdkWindow *gdk_window = gtk_widget_get_window(preview->window);
    if(gtk_widget_get_visible(preview->window) && !IS_NULL_PTR(gdk_window))
      gdk_window_raise(gdk_window);
  }
  return G_SOURCE_REMOVE;
}

void dt_preview_window_raise_all(void)
{
  if(IS_NULL_PTR(_preview_windows)) return;
  if(_preview_raise_source != 0)
    g_source_remove(_preview_raise_source);
  _preview_raise_source = g_timeout_add(150, _preview_raise_all, NULL);
}

/**
 * @brief Return a monitor identity that stays independent of GDK's enumeration order.
 * @details Spaces become underscores because the identity is stored in anselrc.
 */
static gchar *_preview_monitor_name(GdkMonitor *monitor)
{
  const char *manufacturer = gdk_monitor_get_manufacturer(monitor);
  const char *model = gdk_monitor_get_model(monitor);
  gchar *name = NULL;

  if(!IS_NULL_PTR(manufacturer) && manufacturer[0] != '\0' && !IS_NULL_PTR(model) && model[0] != '\0')
    name = g_strdup_printf("%s %s", manufacturer, model);
  else if(!IS_NULL_PTR(model) && model[0] != '\0')
    name = g_strdup(model);
  else if(!IS_NULL_PTR(manufacturer) && manufacturer[0] != '\0')
    name = g_strdup(manufacturer);
  else
    name = g_strdup("Unknown monitor");

  g_strdelimit(name, " ", '_');
  return name;
}

static gchar *_preview_geometry_key(const char *monitor_name)
{
  return g_strdup_printf("plugins/lighttable/preview/window/%s", monitor_name);
}

/**
 * @brief Find the currently connected monitor with the saved manufacturer/model identity.
 * @details The scan compares names, never saved monitor indices, so reordered displays still match.
 */
static GdkMonitor *_preview_find_monitor(GdkDisplay *display, const char *monitor_name)
{
  const int n_monitors = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n_monitors; i++)
  {
    GdkMonitor *monitor = gdk_display_get_monitor(display, i);
    gchar *name = _preview_monitor_name(monitor);
    const gboolean found = g_strcmp0(name, monitor_name) == 0;
    dt_free(name);
    if(found) return monitor;
  }
  return NULL;
}

/**
 * @brief Choose a usable fallback screen when the saved monitor is disconnected.
 * @details Prefer a monitor other than the main window's, then the main or primary monitor.
 */
static GdkMonitor *_preview_fallback_monitor(GdkDisplay *display, GtkWidget *main_window)
{
  GdkMonitor *main_monitor = NULL;
  GdkWindow *main_gdk_window = gtk_widget_get_window(main_window);
  if(!IS_NULL_PTR(main_gdk_window))
    main_monitor = gdk_display_get_monitor_at_window(display, main_gdk_window);

  const int n_monitors = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n_monitors; i++)
  {
    GdkMonitor *monitor = gdk_display_get_monitor(display, i);
    if(monitor != main_monitor) return monitor;
  }

  if(!IS_NULL_PTR(main_monitor)) return main_monitor;
  GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
  if(!IS_NULL_PTR(monitor)) return monitor;
  return n_monitors > 0 ? gdk_display_get_monitor(display, 0) : NULL;
}

/**
 * @brief Center a first-time preview over the lighttable's central image area.
 */
static void _preview_center_on_lighttable(GtkWidget *window, GdkMonitor *monitor,
                                          GdkMonitor *lighttable_monitor)
{
  GtkWidget *main_window = dt_gui_main_window();
  GtkWidget *lighttable = dt_gui_get_ui()->thumbtable_lighttable->parent_overlay;
  GdkWindow *main_gdk_window = gtk_widget_get_window(main_window);
  if(IS_NULL_PTR(main_gdk_window)) return;

  int lighttable_x = 0, lighttable_y = 0;
  if(!gtk_widget_translate_coordinates(lighttable, main_window, 0, 0, &lighttable_x, &lighttable_y))
    return;

  int main_x = 0, main_y = 0, window_width = 0, window_height = 0;
  gdk_window_get_origin(main_gdk_window, &main_x, &main_y);
  gtk_window_get_size(GTK_WINDOW(window), &window_width, &window_height);
  GdkRectangle workarea = { 0 };
  gdk_monitor_get_workarea(monitor, &workarea);
  window_width = MIN(window_width, workarea.width);
  window_height = MIN(window_height, workarea.height);
  gtk_window_resize(GTK_WINDOW(window), window_width, window_height);
  const int lighttable_width = gtk_widget_get_allocated_width(lighttable);
  const int lighttable_height = gtk_widget_get_allocated_height(lighttable);
  int x, y;
  if(monitor == lighttable_monitor)
  {
    x = main_x + lighttable_x + (lighttable_width - window_width) / 2;
    y = main_y + lighttable_y + (lighttable_height - window_height) / 2;
  }
  else
  {
    x = workarea.x + (workarea.width - window_width) / 2;
    y = workarea.y + (workarea.height - window_height) / 2;
  }
  x = CLAMP(x, workarea.x, workarea.x + workarea.width - window_width);
  y = CLAMP(y, workarea.y, workarea.y + workarea.height - window_height);
  gtk_window_move(GTK_WINDOW(window), x, y);
}

/**
 * @brief Read globally persisted geometry for one named monitor.
 */
static gboolean _preview_read_geometry(dt_preview_window_t *preview, const char *monitor_name,
                                       GdkMonitor *monitor)
{
  gchar *key = _preview_geometry_key(monitor_name);
  gchar *value = dt_conf_get_string(key);
  int x_ratio = 0, y_ratio = 0, width_ratio = 0, height_ratio = 0, maximized = 0, format = 0;
  int fields = IS_NULL_PTR(value)
                   ? 0
                   : sscanf(value, "%d,%d,%d,%d,%d,%d", &x_ratio, &y_ratio, &width_ratio,
                            &height_ratio, &maximized, &format);
  const gboolean valid = fields == 6 && (format == 4 || format == 5 || format == 6 || format == 7);
  gboolean valid_values =
      x_ratio >= 0 && x_ratio <= 10000 && y_ratio >= 0 && y_ratio <= 10000
      && width_ratio > 0 && width_ratio <= 10000
      && height_ratio > 0 && height_ratio <= 10000;

  if(valid && valid_values && format == 6 && !IS_NULL_PTR(monitor))
  {
    GdkRectangle geometry = { 0 }, workarea = { 0 };
    gdk_monitor_get_geometry(monitor, &geometry);
    gdk_monitor_get_workarea(monitor, &workarea);
    if(geometry.width > 0 && geometry.height > 0 && workarea.width > 0 && workarea.height > 0)
    {
      x_ratio = (int)(((gint64)geometry.width * x_ratio / 10000
                       + geometry.x - workarea.x) * 10000 / workarea.width);
      y_ratio = (int)(((gint64)geometry.height * y_ratio / 10000
                       + geometry.y - workarea.y) * 10000 / workarea.height);
      width_ratio = (int)((gint64)geometry.width * width_ratio / workarea.width);
      height_ratio = (int)((gint64)geometry.height * height_ratio / workarea.height);
      x_ratio = CLAMP(x_ratio, 0, 10000);
      y_ratio = CLAMP(y_ratio, 0, 10000);
      width_ratio = CLAMP(width_ratio, 1, 10000);
      height_ratio = CLAMP(height_ratio, 1, 10000);
    }
    else
      valid_values = FALSE;
  }

  if(valid && valid_values)
  {
    preview->x_ratio = CLAMP(x_ratio, 0, 10000);
    preview->y_ratio = CLAMP(y_ratio, 0, 10000);
    preview->width_ratio = CLAMP(width_ratio, 1, 10000);
    preview->height_ratio = CLAMP(height_ratio, 1, 10000);
    preview->maximized = maximized != 0;
    preview->geometry_valid = TRUE;
  }

  dt_free(value);
  dt_free(key);
  return valid && valid_values;
}

static gboolean _preview_finish_geometry_apply(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->geometry_apply_source = 0;
  preview->applying_geometry = FALSE;
  if(!IS_NULL_PTR(preview->return_monitor_name)
     && g_strcmp0(preview->monitor_name, preview->return_monitor_name) == 0
     && !IS_NULL_PTR(_preview_find_monitor(gtk_widget_get_display(preview->window),
                                           preview->return_monitor_name)))
  {
    dt_free(preview->return_monitor_name);
    preview->return_monitor_name = NULL;
  }
  return G_SOURCE_REMOVE;
}

static void _preview_defer_geometry_tracking(dt_preview_window_t *preview)
{
  if(preview->geometry_apply_source != 0)
    g_source_remove(preview->geometry_apply_source);
  preview->applying_geometry = TRUE;
  preview->geometry_apply_source =
      g_timeout_add(350, _preview_finish_geometry_apply, preview);
}

/**
 * @brief Restore geometry relative to the usable work area.
 * @details Work-area ratios account for taskbars and other reserved edges when displays differ.
 */
static void _preview_apply_geometry(GtkWidget *window, GdkMonitor *monitor, dt_preview_window_t *preview)
{
  GdkRectangle workarea = { 0 };
  gdk_monitor_get_workarea(monitor, &workarea);
  if(workarea.width < 1 || workarea.height < 1)
    return;

  if(!preview->geometry_valid)
  {
    const int width = MIN(420, workarea.width);
    const int height = MIN(420, workarea.height);
    preview->x_ratio = 5000;
    preview->y_ratio = 5000;
    preview->width_ratio = MAX(1, (int)((gint64)width * 10000 / workarea.width));
    preview->height_ratio = MAX(1, (int)((gint64)height * 10000 / workarea.height));
    preview->maximized = FALSE;
    preview->geometry_valid = TRUE;
  }

  int width = (int)((gint64)workarea.width * preview->width_ratio / 10000);
  int height = (int)((gint64)workarea.height * preview->height_ratio / 10000);
  width = CLAMP(width, MIN(120, workarea.width), workarea.width);
  height = CLAMP(height, MIN(120, workarea.height), workarea.height);

  const int x = workarea.x + (int)((gint64)workarea.width * preview->x_ratio / 10000) - width / 2;
  const int y = workarea.y + (int)((gint64)workarea.height * preview->y_ratio / 10000) - height / 2;
  const int clamped_x = CLAMP(x, workarea.x, workarea.x + workarea.width - width);
  const int clamped_y = CLAMP(y, workarea.y, workarea.y + workarea.height - height);
  _preview_defer_geometry_tracking(preview);
  gtk_window_unmaximize(GTK_WINDOW(window));
  gtk_window_resize(GTK_WINDOW(window), width, height);
  gtk_window_move(GTK_WINDOW(window), clamped_x, clamped_y);
  if(preview->maximized) gtk_window_maximize(GTK_WINDOW(window));
}

/**
 * @brief Store format-7 work-area-relative geometry for one named monitor.
 */
static void _preview_write_geometry(const char *monitor_name, const dt_preview_window_t *preview)
{
  gchar *key = _preview_geometry_key(monitor_name);
  gchar *value = g_strdup_printf("%d,%d,%d,%d,%d,7", preview->x_ratio, preview->y_ratio,
                                 preview->width_ratio, preview->height_ratio,
                                 preview->maximized ? 1 : 0);
  dt_conf_set_string(key, value);
  dt_free(value);
  dt_free(key);
}

/**
 * @brief Persist center and size relative to the usable monitor work area.
 */
static void _preview_save_geometry(GtkWidget *window, dt_preview_window_t *preview,
                                   const int x, const int y, const int width, const int height)
{
  if(width < 2 || height < 2) return;
  GdkWindow *gdk_window = gtk_widget_get_window(window);
  if(IS_NULL_PTR(gdk_window)) return;

  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gdk_window);
  if(IS_NULL_PTR(monitor)) return;

  gchar *monitor_name = _preview_monitor_name(monitor);
  if(preview->applying_geometry)
  {
    dt_free(monitor_name);
    return;
  }

  if(IS_NULL_PTR(preview->return_monitor_name)
     && !IS_NULL_PTR(preview->monitor_name)
     && g_strcmp0(preview->monitor_name, monitor_name) != 0
     && IS_NULL_PTR(_preview_find_monitor(display, preview->monitor_name)))
  {
    preview->return_monitor_name = g_strdup(preview->monitor_name);
    dt_free(monitor_name);
    return;
  }

  if(!IS_NULL_PTR(preview->return_monitor_name)
     && g_strcmp0(preview->monitor_name, monitor_name) != 0)
  {
    dt_free(monitor_name);
    return;
  }
  if(!IS_NULL_PTR(preview->return_monitor_name))
  {
    dt_free(monitor_name);
    return;
  }

  GdkRectangle workarea = { 0 };
  gdk_monitor_get_workarea(monitor, &workarea);
  if(workarea.width < 1 || workarea.height < 1)
  {
    dt_free(monitor_name);
    return;
  }

  dt_free(preview->monitor_name);
  preview->monitor_name = g_strdup(monitor_name);

  preview->x_ratio = (int)((gint64)CLAMP(x + width / 2 - workarea.x, 0, workarea.width)
                            * 10000 / workarea.width);
  preview->y_ratio = (int)((gint64)CLAMP(y + height / 2 - workarea.y, 0, workarea.height)
                            * 10000 / workarea.height);
  preview->width_ratio = CLAMP((int)((gint64)width * 10000 / workarea.width), 1, 10000);
  preview->height_ratio = CLAMP((int)((gint64)height * 10000 / workarea.height), 1, 10000);
  preview->maximized = gtk_window_is_maximized(GTK_WINDOW(window));
  preview->geometry_valid = TRUE;
  if(preview->persist_geometry && preview->user_geometry_set)
  {
    _preview_write_geometry(monitor_name, preview);
    if(IS_NULL_PTR(preview->return_monitor_name))
      dt_conf_set_string("plugins/lighttable/preview/last_monitor", monitor_name);
  }
  dt_free(monitor_name);
}

static gboolean _preview_configure_event(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(preview->applying_geometry)
    _preview_defer_geometry_tracking(preview);
  if(preview->geometry_tracking)
  {
    const gboolean changed = event->x != preview->tracked_x || event->y != preview->tracked_y
                             || event->width != preview->tracked_width
                             || event->height != preview->tracked_height;
    if(changed && !preview->applying_geometry)
    {
      GdkWindow *gdk_window = gtk_widget_get_window(window);
      GdkDisplay *display = gtk_widget_get_display(window);
      GdkMonitor *monitor = IS_NULL_PTR(gdk_window)
                                ? NULL
                                : gdk_display_get_monitor_at_window(display, gdk_window);
      gchar *monitor_name = IS_NULL_PTR(monitor) ? NULL : _preview_monitor_name(monitor);
      const gboolean monitor_was_removed =
          !IS_NULL_PTR(preview->monitor_name)
          && g_strcmp0(preview->monitor_name, monitor_name) != 0
          && IS_NULL_PTR(_preview_find_monitor(display, preview->monitor_name));
      if(monitor_was_removed)
      {
        if(IS_NULL_PTR(preview->return_monitor_name))
          preview->return_monitor_name = g_strdup(preview->monitor_name);
        dt_free(monitor_name);
        return FALSE;
      }
      else
        preview->user_geometry_set = TRUE;
      dt_free(monitor_name);
    }
    preview->tracked_x = event->x;
    preview->tracked_y = event->y;
    preview->tracked_width = event->width;
    preview->tracked_height = event->height;
  }
  _preview_save_geometry(window, preview, event->x, event->y, event->width, event->height);
  return FALSE;
}

static gboolean _preview_window_state_event(GtkWidget *window, GdkEventWindowState *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  int x = 0, y = 0, width = 0, height = 0;
  gtk_window_get_position(GTK_WINDOW(window), &x, &y);
  gtk_window_get_size(GTK_WINDOW(window), &width, &height);
  if(preview->geometry_tracking && (event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED))
    preview->user_geometry_set = TRUE;
  _preview_save_geometry(window, preview, x, y, width, height);
  return FALSE;
}

/**
 * @brief Start tracking after the window manager has settled the initial placement.
 */
static gboolean _preview_start_geometry_tracking(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->geometry_tracking_source = 0;
  gtk_window_get_position(GTK_WINDOW(preview->window), &preview->tracked_x, &preview->tracked_y);
  gtk_window_get_size(GTK_WINDOW(preview->window), &preview->tracked_width, &preview->tracked_height);
  if(!preview->applying_geometry)
    _preview_save_geometry(preview->window, preview, preview->tracked_x, preview->tracked_y,
                           preview->tracked_width, preview->tracked_height);
  preview->geometry_tracking = TRUE;
  return G_SOURCE_REMOVE;
}

/**
 * @brief Keep the preview on its named display or move it when that display is unplugged.
 * @details The in-memory ratios describe the removed monitor, so they are applied to the
 * fallback display instead of loading that display's unrelated saved geometry.
 */
static void _preview_update_monitor_configuration(dt_preview_window_t *preview, GdkDisplay *display)
{
  if(!gtk_widget_get_visible(preview->window)) return;

  if(!IS_NULL_PTR(preview->return_monitor_name))
  {
    GdkMonitor *return_monitor = _preview_find_monitor(display, preview->return_monitor_name);
    if(!IS_NULL_PTR(return_monitor))
    {
      dt_free(preview->monitor_name);
      preview->monitor_name = g_strdup(preview->return_monitor_name);
      _preview_apply_geometry(preview->window, return_monitor, preview);
      gtk_window_present(GTK_WINDOW(preview->window));
      return;
    }
  }

  GdkMonitor *monitor = _preview_find_monitor(display, preview->monitor_name);
  if(!IS_NULL_PTR(monitor)) return;
  if(IS_NULL_PTR(preview->return_monitor_name))
    preview->return_monitor_name = g_strdup(preview->monitor_name);

  monitor = _preview_fallback_monitor(display, dt_gui_main_window());
  if(IS_NULL_PTR(monitor)) return;

  gchar *monitor_name = _preview_monitor_name(monitor);
  dt_free(preview->monitor_name);
  preview->monitor_name = monitor_name;
  _preview_apply_geometry(preview->window, monitor, preview);
  gtk_window_present(GTK_WINDOW(preview->window));
}

static gboolean _preview_monitor_update_idle(gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  preview->monitor_update_source = 0;
  _preview_update_monitor_configuration(preview, gtk_widget_get_display(preview->window));
  return G_SOURCE_REMOVE;
}

static void _preview_schedule_monitor_update(dt_preview_window_t *preview)
{
  if(preview->monitor_update_source == 0)
    preview->monitor_update_source = g_timeout_add(100, _preview_monitor_update_idle, preview);
}

static void _preview_monitors_changed(GdkScreen *screen, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _preview_schedule_monitor_update(preview);
}

static void _preview_monitor_added(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _preview_schedule_monitor_update(preview);
}

static void _preview_monitor_removed(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  gchar *removed_monitor_name = _preview_monitor_name(monitor);
  if(IS_NULL_PTR(preview->return_monitor_name)
     && g_strcmp0(preview->monitor_name, removed_monitor_name) == 0)
    preview->return_monitor_name = g_strdup(removed_monitor_name);
  dt_free(removed_monitor_name);
  _preview_schedule_monitor_update(preview);
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
  int x = 0, y = 0, width = 0, height = 0;
  gtk_window_get_position(GTK_WINDOW(dialog), &x, &y);
  gtk_window_get_size(GTK_WINDOW(dialog), &width, &height);
  _preview_save_geometry(dialog, preview, x, y, width, height);
  if(!IS_NULL_PTR(preview->screen) && preview->monitors_changed_handler != 0)
    g_signal_handler_disconnect(preview->screen, preview->monitors_changed_handler);
  if(!IS_NULL_PTR(preview->display) && preview->monitor_added_handler != 0)
    g_signal_handler_disconnect(preview->display, preview->monitor_added_handler);
  if(!IS_NULL_PTR(preview->display) && preview->monitor_removed_handler != 0)
    g_signal_handler_disconnect(preview->display, preview->monitor_removed_handler);
  if(preview->geometry_tracking_source != 0)
    g_source_remove(preview->geometry_tracking_source);
  if(preview->geometry_apply_source != 0)
    g_source_remove(preview->geometry_apply_source);
  if(preview->monitor_update_source != 0)
    g_source_remove(preview->monitor_update_source);
  _preview_windows = g_list_remove(_preview_windows, preview);
  if(IS_NULL_PTR(_preview_windows))
  {
    if(_preview_raise_source != 0)
      g_source_remove(_preview_raise_source);
    _preview_raise_source = 0;
  }

  // Disconnect from DARKROOM_UI_CHANGED before freeing: otherwise the handler keeps
  // firing with a dangling `preview` pointer and crashes on the next UI change
  // (e.g. moving the border-size slider).
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

  /* The async fetcher can stop its current pixelpipe through the request-owned
   * shutdown flag. Trigger that as soon as the popup size changes so we don't
   * keep rendering a surface for an obsolete allocation. */
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

  // FIXME: the preview surface is still color-managed with the main window's monitor profile.
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


/**
 * @brief Open an image preview using default or globally persisted geometry.
 * @param imgid Image to display.
 * @param restore_saved_geometry TRUE to restore the global preview layout.
 */
void dt_preview_window_spawn(const int32_t imgid, const gboolean restore_saved_geometry)
{
  dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
  preview->imgid = imgid;
  dt_view_image_surface_fetcher_init(&preview->fetcher);

  GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  preview->window = dialog;
  preview->window_id = ++_preview_next_window_id;
  gchar *role = g_strdup_printf("ansel-preview-%" G_GUINT64_FORMAT, preview->window_id);
  gtk_window_set_role(GTK_WINDOW(dialog), role);
  dt_free(role);

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  gchar *name = g_strdup_printf(_("Ansel - Preview: %s [v%d]"), img->filename, img->version);
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
  gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 420);
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_delete_event), preview);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), preview);
  g_signal_connect(G_OBJECT(dialog), "window-state-event", G_CALLBACK(_preview_window_state_event), preview);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_key_press_event), preview);
  preview->persist_geometry = restore_saved_geometry;

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
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED, G_CALLBACK(_preview_redraw), preview);

  GdkDisplay *display = gtk_widget_get_display(dialog);
  GtkWidget *main_window = dt_gui_main_window();
  gchar *last_monitor = restore_saved_geometry
                            ? dt_conf_get_string("plugins/lighttable/preview/last_monitor")
                            : NULL;
  GdkMonitor *saved_monitor =
      restore_saved_geometry ? _preview_find_monitor(display, last_monitor) : NULL;
  GdkWindow *main_gdk_window = gtk_widget_get_window(main_window);
  GdkMonitor *main_monitor = IS_NULL_PTR(main_gdk_window)
                                 ? NULL
                                 : gdk_display_get_monitor_at_window(display, main_gdk_window);
  GdkMonitor *monitor = saved_monitor;
  if(IS_NULL_PTR(monitor) && !IS_NULL_PTR(last_monitor))
    monitor = _preview_fallback_monitor(display, main_window);
  else if(IS_NULL_PTR(monitor))
    monitor = !IS_NULL_PTR(main_monitor) ? main_monitor : _preview_fallback_monitor(display, main_window);
  if(!IS_NULL_PTR(monitor))
  {
    preview->monitor_name = _preview_monitor_name(monitor);
    if(!IS_NULL_PTR(saved_monitor))
      _preview_read_geometry(preview, last_monitor, saved_monitor);
    else if(restore_saved_geometry && !IS_NULL_PTR(last_monitor))
    {
      preview->return_monitor_name = g_strdup(last_monitor);
      _preview_read_geometry(preview, last_monitor, NULL);
    }
    if(restore_saved_geometry && !preview->geometry_valid)
      _preview_read_geometry(preview, preview->monitor_name, monitor);
    if(restore_saved_geometry && !preview->geometry_valid)
    {
      dt_free(preview->return_monitor_name);
      preview->return_monitor_name = NULL;
      monitor = !IS_NULL_PTR(main_monitor) ? main_monitor : _preview_fallback_monitor(display, main_window);
      dt_free(preview->monitor_name);
      preview->monitor_name = IS_NULL_PTR(monitor) ? NULL : _preview_monitor_name(monitor);
    }
    if(preview->geometry_valid)
      _preview_apply_geometry(dialog, monitor, preview);
  }
  dt_free(last_monitor);

  preview->screen = gtk_widget_get_screen(dialog);
  preview->monitors_changed_handler =
      g_signal_connect(preview->screen, "monitors-changed", G_CALLBACK(_preview_monitors_changed), preview);
  preview->display = display;
  preview->monitor_added_handler =
      g_signal_connect(display, "monitor-added", G_CALLBACK(_preview_monitor_added), preview);
  preview->monitor_removed_handler =
      g_signal_connect(display, "monitor-removed", G_CALLBACK(_preview_monitor_removed), preview);

  gtk_widget_set_visible(preview->area, TRUE);
  gtk_widget_show_all(dialog);
  if((!restore_saved_geometry || !preview->geometry_valid) && !IS_NULL_PTR(monitor))
  {
    _preview_defer_geometry_tracking(preview);
    _preview_center_on_lighttable(dialog, monitor, main_monitor);
  }
  _preview_windows = g_list_append(_preview_windows, preview);
  gtk_widget_grab_focus(preview->area);
  gtk_window_present(GTK_WINDOW(dialog));
  dt_preview_window_raise_all();
  preview->geometry_tracking_source = g_timeout_add(500, _preview_start_geometry_tracking, preview);
}
