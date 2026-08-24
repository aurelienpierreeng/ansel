/*
    This file is part of darktable,
    Copyright (C) 2009-2014, 2016 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2014 Henrik Andersson.
    Copyright (C) 2010, 2013-2014, 2016 Pascal de Bruijn.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010-2019 Tobias Ellinghaus.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011-2012, 2015 Jérémy Rosen.
    Copyright (C) 2011 Moritz Lipp.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2013 Simon Spannagel.
    Copyright (C) 2012, 2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012-2017, 2019-2020 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Jochem Kossen.
    Copyright (C) 2013, 2015, 2017-2022 Pascal Obry.
    Copyright (C) 2013-2016, 2019-2020 Roman Lebedev.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2015 Edouard Gomez.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2015, 2019 Ulrich Pegelow.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2017-2018, 2021 Dan Torop.
    Copyright (C) 2017-2018 Matthieu Moy.
    Copyright (C) 2017-2018 Rikard Öxler.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2019-2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 emeikei.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 vacaboja.
    Copyright (C) 2020 Bill Ferguson.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2021 David-Tillmann Schaefer.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2020, 2022 Miloš Komarčević.
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2020 quovadit.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 darkelectron.
    Copyright (C) 2021 lhietal.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2022 Roman Neuhauser.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2023 Maurizio Paglia.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025 Guillaume Stutin.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "darktable.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "metadata/colorlabels.h"   // DT_COLORLABELS_*
#include "gui/screen_metrics.h"
#include "widgets/widget_settings.h"
#include "widgets/resize_handle.h"
#include "colorprofiles/colorspaces.h"
#include "common/l10n.h"
#include "common/file_location.h"
#include "common/utility.h"
#include "gui/guides.h"
#include "widgets/expander.h"

#include "gui/application.h"
#include "common/thumbnail_notify.h"
#include "gui/common/film_gui.h"
#include "gui/common/folder_survey_gui.h"
#include "gui/develop/history_merge_gui.h"
#include "gui/common/collection_gui.h"
#include "common/startup_progress.h"
#include "gui/dtgtk/thumbtable.h"
#include "gui/splash.h"

#include "common/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "gui/presets.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef _WIN32
#include <gdk/gdkwin32.h>
#endif
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "widgets/container.h"
#include "widgets/widget_style.h"
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/*
 * NEW UI API
 */

/* generic callback for redraw widget signals */

/* widgets/ carries its own colour-label indices so it needs no application header. This is
 * the only place both enums are visible, so this is where they are pinned together. */
_Static_assert((int)DT_WIDGET_COLORLABEL_RED == (int)DT_COLORLABELS_RED
                   && (int)DT_WIDGET_COLORLABEL_PURPLE == (int)DT_COLORLABELS_PURPLE
                   && (int)DT_WIDGET_COLORLABEL_COUNT == (int)DT_COLORLABELS_LAST,
               "widget colour-label indices drifted from dt_colorlabels_enum");

static void _update_display_profile(void);
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget);
/* callback for redraw log signals */
static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget);
static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget);

// set class function to add CSS classes with just a simple line call



/* ------------------------------------------------------------------------------------------
 * Widget-callback suppression depth (see darktable.h for the rationale and API).
 * ------------------------------------------------------------------------------------------ */
/* Sub-handle accessors for the GUI singleton (declared in gui/application.h). The orchestrator
 * binds darktable.gui via dt_gui_get_global(); these narrow it to the parts callers
 * actually want, so they stop walking the application struct. */
struct dt_ui_t *dt_gui_get_ui(void)
{
  return dt_gui_get_global()->ui;
}

struct dt_accels_t *dt_gui_get_accels(void)
{
  return dt_gui_get_global()->accels;
}

GtkWidget *dt_gui_main_window(void)
{
  return dt_ui_main_window(dt_gui_get_global()->ui);
}

GtkWidget *dt_gui_center_widget(void)
{
  return dt_ui_center(dt_gui_get_global()->ui);
}





/*
 * OLD UI API
 */
static void _init_widgets(dt_gui_gtk_t *gui);
static gboolean _configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data);




static gboolean _draw(GtkWidget *da, cairo_t *cr, gpointer user_data)
{
  dt_control_expose(NULL);
  if(darktable.gui->surface)
  {
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
  }
  return TRUE;
}


static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    return dt_view_manager_scrolled(darktable.view_manager, event->x, event->y,
                                    delta_y < 0,
                                    event->state, delta_y);
  }

  return FALSE;
}


int dt_gui_gtk_write_config()
{
  dt_pthread_mutex_lock(&darktable.gui->mutex);
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  const GdkWindowState window_state = gdk_window_get_state(gtk_widget_get_window(widget));
  dt_conf_set_bool("ui_last/maximized", (window_state & GDK_WINDOW_STATE_MAXIMIZED));
  int width, height;
  gtk_window_get_size(GTK_WINDOW(widget), &width, &height);
  dt_conf_set_int("ui_last/window_width", width);
  dt_conf_set_int("ui_last/window_height", height);

  gboolean save_window_position = TRUE;
#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay *display = gtk_widget_get_display(widget);
  if(GDK_IS_WAYLAND_DISPLAY(display))
    save_window_position = FALSE;
#endif

  if(save_window_position)
  {
    GdkWindow *gdk_window = gtk_widget_get_window(widget);
    GdkDisplay *window_display = gtk_widget_get_display(widget);
    if(!IS_NULL_PTR(gdk_window) && !IS_NULL_PTR(window_display))
    {
      GdkMonitor *monitor = gdk_display_get_monitor_at_window(window_display, gdk_window);
      if(!IS_NULL_PTR(monitor))
      {
        const int n_monitors = gdk_display_get_n_monitors(window_display);
        int monitor_index = -1;
        for(int i = 0; i < n_monitors; i++)
        {
          if(gdk_display_get_monitor(window_display, i) == monitor)
          {
            monitor_index = i;
            break;
          }
        }
        if(monitor_index >= 0)
          dt_conf_set_int("ui_last/window_monitor", monitor_index);
      }
    }

    if(!(window_state & GDK_WINDOW_STATE_MAXIMIZED))
    {
      int x, y;
      gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
      dt_conf_set_int("ui_last/window_x", x);
      dt_conf_set_int("ui_last/window_y", y);
    }
  }

  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  return 0;
}


void dt_gui_gtk_quit()
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_gui_add_class(win, "dt_gui_quit");
  gtk_window_set_title(GTK_WINDOW(win), _("closing Ansel..."));

  dt_ui_cleanup_titlebar(darktable.gui->ui);

  // Write out windows dimension
  dt_gui_gtk_write_config();

  // hide main window
  gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));
}

gboolean dt_gui_quit_callback(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_control_quit();
  return TRUE;
}

void dt_gui_store_last_preset(const char *name)
{
  dt_free(darktable.gui->last_preset);
  darktable.gui->last_preset = g_strdup(name);
}

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_quit_callback(GtkOSXApplication *OSXapp, gpointer user_data)
#else
static gboolean _osx_quit_callback(GtkosxApplication *OSXapp, gpointer user_data)
#endif
{
  GList *windows, *window;
  windows = gtk_window_list_toplevels();
  for(window = windows; !IS_NULL_PTR(window); window = g_list_next(window))
    if(gtk_window_get_modal(GTK_WINDOW(window->data)) && gtk_widget_get_visible(GTK_WIDGET(window->data)))
      break;
  if(IS_NULL_PTR(window)) dt_control_quit();
  g_list_free(windows);
  windows = NULL;
  return TRUE;
}

#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_openfile_callback(GtkOSXApplication *OSXapp, gchar *path, gpointer user_data)
#else
static gboolean _osx_openfile_callback(GtkosxApplication *OSXapp, gchar *path, gpointer user_data)
#endif
{
  return dt_load_from_string(path, TRUE, NULL) > 0;
}
#endif

static gboolean _configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  static int oldw = 0;
  static int oldh = 0;
  // make our selves a properly sized pixmap if our window has been resized
  if(oldw != event->width || oldh != event->height)
  {
    // create our new pixmap with the correct size.
    cairo_surface_t *tmpsurface
        = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, event->width, event->height);
    // copy the contents of the old pixmap to the new pixmap.  This keeps ugly uninitialized
    // pixmaps from being painted upon resize
    //     int minw = oldw, minh = oldh;
    //     if(event->width  < minw) minw = event->width;
    //     if(event->height < minh) minh = event->height;

    cairo_t *cr = cairo_create(tmpsurface);
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    // we're done with our old pixmap, so we can get rid of it and replace it with our properly-sized one.
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = tmpsurface;
    // maybe we are on another screen now with > 50% of the area
    _update_display_profile();
  }
  oldw = event->width;
  oldh = event->height;

#ifndef GDK_WINDOWING_QUARTZ
  dt_configure_ppd_dpi((dt_gui_gtk_t *) user_data);
#endif

  return dt_control_configure(da, event, user_data);
}

static gboolean _window_configure(GtkWidget *da, GdkEvent *event, gpointer user_data)
{
  static int oldx = 0;
  static int oldy = 0;
  if(oldx != event->configure.x || oldy != event->configure.y)
  {
    // maybe we are on another screen now with > 50% of the area
    _update_display_profile();
    oldx = event->configure.x;
    oldy = event->configure.y;
  }
  return FALSE;
}

typedef struct dt_tablet_motion_state_t
{
  gboolean valid;
  double x;
  double y;
  guint32 time_ms;
  double speed_px_s;
  gboolean have_pressure;
  double pressure;
  gboolean have_tilt;
  double tilt_x;
  double tilt_y;
} dt_tablet_motion_state_t;

static dt_tablet_motion_state_t _tablet_motion_state = { 0 };

static inline double _clamp01d(const double value)
{
  return MIN(1.0, MAX(0.0, value));
}

static const gdouble *_event_axes(const GdkEvent *event)
{
  if(IS_NULL_PTR(event)) return NULL;
  switch(event->type)
  {
    case GDK_MOTION_NOTIFY:
      return ((const GdkEventMotion *)event)->axes;
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_3BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
      return ((const GdkEventButton *)event)->axes;
    default:
      return NULL;
  }
}

static gboolean _get_axis_value_for_source(const GdkEvent *event, GdkDevice *source_device,
                                           const GdkAxisUse axis, double *value, gboolean *from_source_map)
{
  if(from_source_map) *from_source_map = FALSE;
  if(IS_NULL_PTR(value)) return FALSE;

  const gdouble *axes = _event_axes(event);
  if(source_device && axes)
  {
    double source_value = 0.0;
    if(gdk_device_get_axis(source_device, (gdouble *)axes, axis, &source_value))
    {
      *value = source_value;
      if(from_source_map) *from_source_map = TRUE;
      return TRUE;
    }
  }

  return gdk_event_get_axis((GdkEvent *)event, axis, value);
}

static gboolean _sample_axis_from_device_state(GdkWindow *window, GdkDevice *device,
                                                const GdkAxisUse axis, double *value)
{
  if(IS_NULL_PTR(window) || IS_NULL_PTR(device) || IS_NULL_PTR(value)) return FALSE;
  if(gdk_device_get_source(device) == GDK_SOURCE_KEYBOARD) return FALSE;
  if(gdk_device_get_device_type(device) == GDK_DEVICE_TYPE_SLAVE)
  {
    GdkDisplay *display = gdk_device_get_display(device);
    if(!display || !gdk_display_device_is_grabbed(display, device)) return FALSE;
  }

  const int n_axes = gdk_device_get_n_axes(device);
  if(n_axes <= 0) return FALSE;

  double *axes = g_newa(double, n_axes);
  memset(axes, 0, sizeof(double) * n_axes);
  GdkModifierType modifiers = 0;
  gdk_device_get_state(device, window, axes, &modifiers);
  return gdk_device_get_axis(device, axes, axis, value);
}

static gboolean _sample_tablet_state_from_devices(const GdkEvent *event,
                                                  double *pressure, gboolean *have_pressure,
                                                  double *tilt_x, double *tilt_y, gboolean *have_tilt,
                                                  const char **picked_device_name)
{
  if(pressure) *pressure = 0.0;
  if(have_pressure) *have_pressure = FALSE;
  if(tilt_x) *tilt_x = 0.0;
  if(tilt_y) *tilt_y = 0.0;
  if(have_tilt) *have_tilt = FALSE;
  if(picked_device_name) *picked_device_name = NULL;

  if(IS_NULL_PTR(darktable.gui)) return FALSE;
  GdkWindow *window = gdk_event_get_window((GdkEvent *)event);
  if(IS_NULL_PTR(window)) window = gtk_widget_get_window(dt_ui_center(darktable.gui->ui));
  if(IS_NULL_PTR(window)) return FALSE;

  int best_score = -1;
  double best_p = 0.0;
  gboolean best_have_p = FALSE;
  double best_tx = 0.0;
  double best_ty = 0.0;
  gboolean best_have_t = FALSE;
  const char *best_name = NULL;

  GdkSeat *seat = gdk_display_get_default() ? gdk_display_get_default_seat(gdk_display_get_default()) : NULL;
  GList *runtime_devices = seat ? gdk_seat_get_slaves(seat, GDK_SEAT_CAPABILITY_ALL_POINTING) : NULL;

  for(GList *l = runtime_devices; l; l = g_list_next(l))
  {
    GdkDevice *device = (GdkDevice *)l->data;
    if(IS_NULL_PTR(device)) continue;
    const GdkInputSource source = gdk_device_get_source(device);
    if(source == GDK_SOURCE_KEYBOARD) continue;
    if(gdk_device_get_device_type(device) == GDK_DEVICE_TYPE_SLAVE)
    {
      GdkDisplay *display = gdk_device_get_display(device);
      /* gdk_device_get_state() requires slave devices to be grabbed.
       * Skip non-grabbed slaves to avoid GTK/GDK criticals. */
      if(!display || !gdk_display_device_is_grabbed(display, device)) continue;
    }

    const GdkAxisFlags axis_flags = gdk_device_get_axes(device);
    const gboolean supports_pressure = (axis_flags & GDK_AXIS_FLAG_PRESSURE) != 0;
    const gboolean supports_x_tilt = (axis_flags & GDK_AXIS_FLAG_XTILT) != 0;
    const gboolean supports_y_tilt = (axis_flags & GDK_AXIS_FLAG_YTILT) != 0;
    const gboolean pen_like = (source == GDK_SOURCE_PEN || source == GDK_SOURCE_ERASER || source == GDK_SOURCE_CURSOR);
    if(!supports_pressure && !supports_x_tilt && !supports_y_tilt && !pen_like) continue;

    const int n_axes = gdk_device_get_n_axes(device);
    if(n_axes <= 0) continue;
    double *axes = g_newa(double, n_axes);
    memset(axes, 0, sizeof(double) * n_axes);
    GdkModifierType modifiers = 0;
    gdk_device_get_state(device, window, axes, &modifiers);

    double p = 0.0;
    gboolean have_p = FALSE;
    double tx = 0.0;
    double ty = 0.0;
    gboolean have_tx = FALSE;
    gboolean have_ty = FALSE;

    for(int i = 0; i < n_axes; i++)
    {
      const GdkAxisUse use = gdk_device_get_axis_use(device, i);
      if(use == GDK_AXIS_PRESSURE)
      {
        p = axes[i];
        have_p = TRUE;
      }
      else if(use == GDK_AXIS_XTILT)
      {
        tx = axes[i];
        have_tx = TRUE;
      }
      else if(use == GDK_AXIS_YTILT)
      {
        ty = axes[i];
        have_ty = TRUE;
      }
    }

    const gboolean have_t = have_tx || have_ty;
    const int score = (have_p ? 2 : 0) + (have_t ? 2 : 0) + (p > 1e-4 ? 4 : 0)
                      + ((hypot(tx, ty) > 1e-4) ? 3 : 0) + (pen_like ? 1 : 0);
    if(score <= best_score) continue;

    best_score = score;
    best_p = p;
    best_have_p = have_p;
    best_tx = tx;
    best_ty = ty;
    best_have_t = have_t;
    best_name = gdk_device_get_name(device);
  }
  if(runtime_devices)
  {
    g_list_free(runtime_devices);
    runtime_devices = NULL;
  }

  if(best_score < 0) return FALSE;
  if(pressure) *pressure = best_p;
  if(have_pressure) *have_pressure = best_have_p;
  if(tilt_x) *tilt_x = best_tx;
  if(tilt_y) *tilt_y = best_ty;
  if(have_tilt) *have_tilt = best_have_t;
  if(picked_device_name) *picked_device_name = best_name;
  return TRUE;
}

static dt_control_pointer_input_t _extract_pointer_input(const GdkEvent *event, const double x, const double y,
                                                         const guint32 time_ms, const gboolean reset_kinematics,
                                                         const char *tag)
{
  dt_control_pointer_input_t input = { 0 };
  input.x = x;
  input.y = y;
  input.time_ms = time_ms;
  GdkDevice *source_device = gdk_event_get_source_device((GdkEvent *)event);
  GdkDevice *event_device = gdk_event_get_device((GdkEvent *)event);
  GdkDevice *device = source_device ? source_device : event_device;
  const GdkInputSource source = device ? gdk_device_get_source(device) : GDK_SOURCE_MOUSE;
  const GdkAxisFlags axis_flags = device ? gdk_device_get_axes(device) : 0;
  const gboolean supports_pressure = (axis_flags & GDK_AXIS_FLAG_PRESSURE) != 0;
  const gboolean supports_x_tilt = (axis_flags & GDK_AXIS_FLAG_XTILT) != 0;
  const gboolean supports_y_tilt = (axis_flags & GDK_AXIS_FLAG_YTILT) != 0;
  gboolean read_pressure = FALSE;
  gboolean read_x_tilt = FALSE;
  gboolean read_y_tilt = FALSE;
  gboolean map_pressure_source = FALSE;
  gboolean map_xtilt_source = FALSE;
  gboolean map_ytilt_source = FALSE;
  gboolean state_pressure_source = FALSE;
  gboolean state_pressure_event = FALSE;
  gboolean state_xtilt_source = FALSE;
  gboolean state_ytilt_source = FALSE;
  gboolean fallback_pressure = FALSE;
  gboolean fallback_tilt = FALSE;
  const char *fallback_device_name = NULL;
  GdkDeviceTool *tool = gdk_event_get_device_tool((GdkEvent *)event);
  const int tool_type = tool ? (int)gdk_device_tool_get_tool_type(tool) : -1;
  const gboolean tool_is_stylus
      = tool && (tool_type == GDK_DEVICE_TOOL_TYPE_PEN || tool_type == GDK_DEVICE_TOOL_TYPE_ERASER
                 || tool_type == GDK_DEVICE_TOOL_TYPE_BRUSH || tool_type == GDK_DEVICE_TOOL_TYPE_PENCIL
                 || tool_type == GDK_DEVICE_TOOL_TYPE_AIRBRUSH);
  gboolean is_tablet_like = supports_pressure || supports_x_tilt || supports_y_tilt || tool_is_stylus;
  GdkWindow *window = gdk_event_get_window((GdkEvent *)event);
  if(IS_NULL_PTR(window) && darktable.gui) window = gtk_widget_get_window(dt_ui_center(darktable.gui->ui));

  {
    double pressure = 0.0;
    if(_get_axis_value_for_source(event, source_device, GDK_AXIS_PRESSURE, &pressure, &map_pressure_source))
    {
      read_pressure = TRUE;
      input.pressure = _clamp01d(pressure);
      input.has_pressure = TRUE;
      _tablet_motion_state.have_pressure = TRUE;
      _tablet_motion_state.pressure = input.pressure;
    }
    else if(is_tablet_like && _tablet_motion_state.have_pressure)
    {
      input.pressure = _tablet_motion_state.pressure;
      input.has_pressure = TRUE;
    }
    else if(!is_tablet_like)
    {
      _tablet_motion_state.have_pressure = FALSE;
    }
  }

  if(input.has_pressure && input.pressure <= 0.0 && window)
  {
    double p_state = 0.0;
    if(source_device && _sample_axis_from_device_state(window, source_device, GDK_AXIS_PRESSURE, &p_state))
    {
      input.pressure = _clamp01d(p_state);
      state_pressure_source = TRUE;
    }
    else if(event_device
            && _sample_axis_from_device_state(window, event_device, GDK_AXIS_PRESSURE, &p_state))
    {
      input.pressure = _clamp01d(p_state);
      state_pressure_event = TRUE;
    }
  }

  {
    double x_tilt = 0.0;
    double y_tilt = 0.0;
    const gboolean has_x_tilt
        = _get_axis_value_for_source(event, source_device, GDK_AXIS_XTILT, &x_tilt, &map_xtilt_source);
    const gboolean has_y_tilt
        = _get_axis_value_for_source(event, source_device, GDK_AXIS_YTILT, &y_tilt, &map_ytilt_source);
    read_x_tilt = has_x_tilt;
    read_y_tilt = has_y_tilt;
    if(has_x_tilt || has_y_tilt)
    {
      input.tilt_x = CLAMP(x_tilt, -1.0, 1.0);
      input.tilt_y = CLAMP(y_tilt, -1.0, 1.0);
      input.has_tilt = TRUE;
      _tablet_motion_state.have_tilt = TRUE;
      _tablet_motion_state.tilt_x = input.tilt_x;
      _tablet_motion_state.tilt_y = input.tilt_y;
    }
    else if(is_tablet_like && _tablet_motion_state.have_tilt)
    {
      input.tilt_x = _tablet_motion_state.tilt_x;
      input.tilt_y = _tablet_motion_state.tilt_y;
      input.has_tilt = TRUE;
    }
    else if(!is_tablet_like)
    {
      _tablet_motion_state.have_tilt = FALSE;
    }
  }

  if(!input.has_tilt && window)
  {
    double tx_state = 0.0, ty_state = 0.0;
    const gboolean has_tx = source_device && _sample_axis_from_device_state(window, source_device, GDK_AXIS_XTILT, &tx_state);
    const gboolean has_ty = source_device && _sample_axis_from_device_state(window, source_device, GDK_AXIS_YTILT, &ty_state);
    if(has_tx || has_ty)
    {
      input.tilt_x = CLAMP(tx_state, -1.0, 1.0);
      input.tilt_y = CLAMP(ty_state, -1.0, 1.0);
      input.has_tilt = TRUE;
      _tablet_motion_state.have_tilt = TRUE;
      _tablet_motion_state.tilt_x = input.tilt_x;
      _tablet_motion_state.tilt_y = input.tilt_y;
      state_xtilt_source = has_tx;
      state_ytilt_source = has_ty;
    }
  }

  if(input.has_tilt)
    input.tilt = _clamp01d(hypot(input.tilt_x, input.tilt_y));
  else
    input.tilt = 0.0;

  if(!input.has_pressure || !input.has_tilt)
  {
    double fb_pressure = 0.0;
    gboolean fb_have_pressure = FALSE;
    double fb_tilt_x = 0.0;
    double fb_tilt_y = 0.0;
    gboolean fb_have_tilt = FALSE;
    if(_sample_tablet_state_from_devices(event, &fb_pressure, &fb_have_pressure,
                                         &fb_tilt_x, &fb_tilt_y, &fb_have_tilt,
                                         &fallback_device_name))
    {
      if(!input.has_pressure && fb_have_pressure)
      {
        input.pressure = _clamp01d(fb_pressure);
        input.has_pressure = TRUE;
        fallback_pressure = TRUE;
      }
      if(!input.has_tilt && fb_have_tilt)
      {
        input.tilt_x = CLAMP(fb_tilt_x, -1.0, 1.0);
        input.tilt_y = CLAMP(fb_tilt_y, -1.0, 1.0);
        input.tilt = _clamp01d(hypot(input.tilt_x, input.tilt_y));
        input.has_tilt = TRUE;
        fallback_tilt = TRUE;
      }
    }
  }

  if(input.has_pressure || input.has_tilt) is_tablet_like = TRUE;

  if(reset_kinematics)
  {
    _tablet_motion_state.valid = TRUE;
    _tablet_motion_state.x = x;
    _tablet_motion_state.y = y;
    _tablet_motion_state.time_ms = time_ms;
    _tablet_motion_state.speed_px_s = 0.0;
    input.acceleration = 0.0;
    return input;
  }

  if(_tablet_motion_state.valid && time_ms > _tablet_motion_state.time_ms)
  {
    const double dt_s = MAX((double)(time_ms - _tablet_motion_state.time_ms), 1.0) * 1e-3;
    const double dx = x - _tablet_motion_state.x;
    const double dy = y - _tablet_motion_state.y;
    const double speed_px_s = hypot(dx, dy) / dt_s;
    const double accel_px_s2 = fabs(speed_px_s - _tablet_motion_state.speed_px_s) / dt_s;
    /* Normalize acceleration for stylus mapping. 25000 px/s² keeps a useful
     * dynamic range while clipping extreme event jitter. */
    input.acceleration = _clamp01d(accel_px_s2 / 25000.0);
    _tablet_motion_state.speed_px_s = speed_px_s;
  }
  else
  {
    input.acceleration = 0.0;
    _tablet_motion_state.speed_px_s = 0.0;
  }

  _tablet_motion_state.valid = TRUE;
  _tablet_motion_state.x = x;
  _tablet_motion_state.y = y;
  _tablet_motion_state.time_ms = time_ms;

  dt_print(DT_DEBUG_INPUT,
           "[tablet] %s dev='%s' src_dev='%s' evt_dev='%s' src=%d tablet=%d tool=%d supports[p=%d xt=%d yt=%d] read[p=%d xt=%d yt=%d] map_src[p=%d xt=%d yt=%d] state[p_src=%d p_evt=%d xt_src=%d yt_src=%d] fallback[p=%d t=%d dev='%s'] values[p=%.4f tx=%.4f ty=%.4f t=%.4f a=%.4f] xy=(%.1f, %.1f) t_ms=%u reset=%d\n",
           tag ? tag : "event",
           device ? gdk_device_get_name(device) : "<none>",
           source_device ? gdk_device_get_name(source_device) : "<none>",
           event_device ? gdk_device_get_name(event_device) : "<none>",
           (int)source,
           is_tablet_like ? 1 : 0,
           tool_type,
           supports_pressure ? 1 : 0,
           supports_x_tilt ? 1 : 0,
           supports_y_tilt ? 1 : 0,
           read_pressure ? 1 : 0,
           read_x_tilt ? 1 : 0,
           read_y_tilt ? 1 : 0,
           map_pressure_source ? 1 : 0,
           map_xtilt_source ? 1 : 0,
           map_ytilt_source ? 1 : 0,
           state_pressure_source ? 1 : 0,
           state_pressure_event ? 1 : 0,
           state_xtilt_source ? 1 : 0,
           state_ytilt_source ? 1 : 0,
           fallback_pressure ? 1 : 0,
           fallback_tilt ? 1 : 0,
           fallback_device_name ? fallback_device_name : "<none>",
           input.has_pressure ? input.pressure : -1.0,
           input.has_tilt ? input.tilt_x : 0.0,
           input.has_tilt ? input.tilt_y : 0.0,
           input.has_tilt ? input.tilt : 0.0,
           input.acceleration,
           x, y,
           time_ms,
           reset_kinematics ? 1 : 0);

  return input;
}

static gboolean _button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  /* Reset Gtk focus */
  dt_widget_set_scroll_focus(NULL);
  gtk_widget_grab_focus(w);

  const dt_control_pointer_input_t input = _extract_pointer_input((const GdkEvent *)event, event->x, event->y,
                                                                  event->time, TRUE, "button-press");
  dt_control_set_pointer_input(&input);
  const double pressure = input.has_pressure ? input.pressure : 1.0;
  dt_control_button_pressed(event->x, event->y, pressure, event->button, event->type, event->state & 0xf);
  return FALSE;
}

static gboolean _button_released(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  const dt_control_pointer_input_t input = _extract_pointer_input((const GdkEvent *)event, event->x, event->y,
                                                                  event->time, FALSE, "button-release");
  dt_control_set_pointer_input(&input);
  dt_control_button_released(event->x, event->y, event->button, event->state & 0xf);

  return TRUE;
}

static gboolean _mouse_moved(GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  const dt_control_pointer_input_t input = _extract_pointer_input((const GdkEvent *)event, event->x, event->y,
                                                                  event->time, FALSE, "motion");
  dt_control_set_pointer_input(&input);
  dt_control_mouse_moved(event->x, event->y, input.has_pressure ? input.pressure : 1.0, event->state & 0xf);
  return FALSE;
}

#ifdef _WIN32
/* Arbitrary stable subclass identifier encoded as ASCII "ASNN".
 * It only needs to stay unique within this process for SetWindowSubclass(). */
#define DT_WIN32_CURSOR_SUBCLASS_CENTER ((UINT_PTR)0x41534e4e)

static LRESULT CALLBACK _center_win32_cursor_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param,
                                                  UINT_PTR subclass_id, DWORD_PTR ref_data)
{
  /* On Win32, DefSubclassProc() answers WM_SETCURSOR for the drawing area in center view by
   * restoring the window-class default arrow on every mouse move. The center
   * view already selected the proper cursor through GDK, so swallow the
   * client-area reset and keep the current cursor unchanged until the view
   * requests another explicit cursor change. */
  if(subclass_id == DT_WIN32_CURSOR_SUBCLASS_CENTER && message == WM_SETCURSOR && LOWORD(l_param) == HTCLIENT)
    return TRUE;

  return DefSubclassProc(hwnd, message, w_param, l_param);
}

static void _center_realize(GtkWidget *widget, gpointer user_data)
{
  GdkWindow *center_window = gtk_widget_get_window(widget);
  HWND center_hwnd = center_window ? (HWND)gdk_win32_window_get_handle(center_window) : NULL;
  if(!IS_NULL_PTR(center_hwnd))
    SetWindowSubclass(center_hwnd, _center_win32_cursor_proc, DT_WIN32_CURSOR_SUBCLASS_CENTER, (DWORD_PTR)widget);
}

static void _center_unrealize(GtkWidget *widget, gpointer user_data)
{
  GdkWindow *center_window = gtk_widget_get_window(widget);
  HWND center_hwnd = center_window ? (HWND)gdk_win32_window_get_handle(center_window) : NULL;
  if(!IS_NULL_PTR(center_hwnd))
    RemoveWindowSubclass(center_hwnd, _center_win32_cursor_proc, DT_WIN32_CURSOR_SUBCLASS_CENTER);
}
#endif

static gboolean _key_pressed(GtkWidget *w, GdkEventKey *event)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  dt_control_key_pressed(event);
  return TRUE;
}

static gboolean _center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}

static gboolean _center_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_enter();
  return TRUE;
}

static const char* _get_source_name(int pos)
{
  static const gchar *SOURCE_NAMES[]
    = { "GDK_SOURCE_MOUSE",    "GDK_SOURCE_PEN",         "GDK_SOURCE_ERASER",   "GDK_SOURCE_CURSOR",
        "GDK_SOURCE_KEYBOARD", "GDK_SOURCE_TOUCHSCREEN", "GDK_SOURCE_TOUCHPAD", "GDK_SOURCE_TRACKPOINT",
        "GDK_SOURCE_TABLET_PAD" };
  if(pos >= G_N_ELEMENTS(SOURCE_NAMES)) return "<UNKNOWN>";
  return SOURCE_NAMES[pos];
}

static const char* _get_mode_name(int pos)
{
  static const gchar *MODE_NAMES[] = { "GDK_MODE_DISABLED", "GDK_MODE_SCREEN", "GDK_MODE_WINDOW" };
  if(pos >= G_N_ELEMENTS(MODE_NAMES)) return "<UNKNOWN>";
  return MODE_NAMES[pos];
}

static const char* _get_axis_name(int pos)
{
  static const gchar *AXIS_NAMES[]
    = { "GDK_AXIS_IGNORE",   "GDK_AXIS_X",      "GDK_AXIS_Y",     "GDK_AXIS_PRESSURE",
        "GDK_AXIS_XTILT",    "GDK_AXIS_YTILT",  "GDK_AXIS_WHEEL", "GDK_AXIS_DISTANCE",
        "GDK_AXIS_ROTATION", "GDK_AXIS_SLIDER", "GDK_AXIS_LAST" };
  if(pos >= G_N_ELEMENTS(AXIS_NAMES)) return "<UNKNOWN>";
  return AXIS_NAMES[pos];
}


/* ---- Host hooks for the shortcut system (widgets/accelerators.c) --------------------- */
#define DT_ACCEL_SEARCH_RECENT_KEY "plugins/accel_search/recent_entries"

static GtkWidget *_widget_root_window(void)
{
  return dt_gui_main_window();
}

static gint _widget_natural_width(GtkWidget *widget)
{
  if(IS_NULL_PTR(dt_gui_get_ui())) return -1;
  if(dt_ui_panel_ancestor(dt_gui_get_ui(), DT_UI_PANEL_RIGHT, widget))
    return dt_ui_panel_get_size(dt_gui_get_ui(), DT_UI_PANEL_RIGHT);
  if(dt_ui_panel_ancestor(dt_gui_get_ui(), DT_UI_PANEL_LEFT, widget))
    return dt_ui_panel_get_size(dt_gui_get_ui(), DT_UI_PANEL_LEFT);
  return -1;
}

/* dt_control_change_cursor() and dt_toast_log() are macros, so the widget hooks need real
 * functions to point at. */
// The host owns the preferences; widgets only know the key they were given.
static gboolean _widget_stored_int(const char *key, int *value)
{
  if(!dt_conf_key_exists(key)) return FALSE;
  *value = dt_conf_get_int(key);
  return TRUE;
}

static void _widget_store_int(const char *key, int value)
{
  dt_conf_set_int(key, value);
}

static gboolean _widget_stored_bool(const char *key)
{
  return dt_conf_get_bool(key);
}

static void _widget_store_bool(const char *key, gboolean value)
{
  dt_conf_set_bool(key, value);
}

// The display profile can only be probed once the control loop is up: before that there is no
// realised window to read an X atom or a colord property from. common/colorspaces.c does not
// know what a control loop is, so the check lives here, with the three call sites it guards.
static void _update_display_profile(void)
{
  if(!dt_control_running()) return;
  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY, dt_gui_center_widget());
}

// Relay a display-profile change onto the signal bus. colorspaces/ raises nothing itself.
static void _notify_profile_changed(void)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_CHANGED);
}

static void _notebook_page_changed(gpointer owner)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_NOTEBOOK_TAB_CHANGED, owner);
}

static void _widget_cursor(GdkCursorType cursor)
{
  dt_control_change_cursor(cursor);
}

static void _widget_message(const char *message)
{
  dt_toast_log("%s", message);
}

static gint _accels_top_offset(void)
{
  if(IS_NULL_PTR(dt_gui_get_global()) || IS_NULL_PTR(dt_gui_get_ui())) return 0;
  GtkWidget *top_panel = dt_gui_get_ui()->panels[DT_UI_PANEL_TOP];
  if(IS_NULL_PTR(top_panel) || !gtk_widget_get_visible(top_panel)) return 0;
  return gtk_widget_get_allocated_height(top_panel);
}

static gchar *_accels_recent_get(int index)
{
  gchar *key = g_strdup_printf("%s/%d", DT_ACCEL_SEARCH_RECENT_KEY, index);
  gchar *value = dt_conf_key_exists(key) ? dt_conf_get_string(key) : NULL;
  dt_free(key);
  return value;
}

static void _accels_recent_set(int index, const char *value)
{
  gchar *key = g_strdup_printf("%s/%d", DT_ACCEL_SEARCH_RECENT_KEY, index);
  dt_conf_set_string(key, value ? value : "");
  dt_free(key);
}

/* common/ reports startup progress; opening the splash on the first message is display
 * state, so it lives here rather than in whatever subsystem happens to be slow. */
static void _gui_startup_progress(const char *message)
{
  // dt_gui_splash_init() already no-ops once the splash exists, so no "have I opened it?"
  // flag is needed here -- which is the same flag common/opencl.c used to carry.
  dt_gui_splash_init();
  dt_gui_splash_updatef("%s", message);
}

/* GUI-thread half of _gui_refresh_thumbnail(). Ends in gtk_widget_queue_draw(), so it must
 * not run anywhere else. */
static void _refresh_thumbnail_widgets(int32_t imgid, gboolean refresh_filmstrip)
{
  struct dt_ui_t *ui = dt_gui_get_ui();
  if(IS_NULL_PTR(ui)) return;

  dt_thumbtable_refresh_thumbnail(ui->thumbtable_lighttable, imgid, TRUE);

  // Best-effort: refreshing the filmstrip spawns an export thread that competes with the
  // realtime darkroom main preview, so darkroom write paths ask for it to be skipped.
  if(refresh_filmstrip)
    dt_thumbtable_refresh_thumbnail(ui->thumbtable_filmstrip, imgid, TRUE);
}

/* What crosses the thread boundary: two values, and deliberately nothing else. A request
 * carrying a dt_thumbnail_t* or a GtkWidget* would need that object refcounted, because the
 * GUI thread can destroy it between the worker posting the request and the main loop running
 * it -- the hazard libs/backgroundjobs.c already pays a refcount for. An image id cannot go
 * stale; the widget lookup happens on the GUI thread, where the answer is valid or absent. */
typedef struct _thumbnail_refresh_request_t
{
  int32_t imgid;
  gboolean refresh_filmstrip;
} _thumbnail_refresh_request_t;

static gboolean _refresh_thumbnail_on_gui_thread(gpointer user_data)
{
  const _thumbnail_refresh_request_t *request = (const _thumbnail_refresh_request_t *)user_data;
  _refresh_thumbnail_widgets(request->imgid, request->refresh_filmstrip);
  return G_SOURCE_REMOVE;
}

/* common/ announces that an image's thumbnail is stale; this turns that into the two
 * widget refreshes. Registered at the end of dt_gui_gtk_init(), so headless runs never
 * have a handler and the notification is a no-op there.
 *
 * The announcement arrives on whatever thread changed the image, and most of them are not
 * this one: dt_image_history_changed() is reached from the flip/rotate job, from the import
 * job and from style application, all of which run in the job queue. The refresh ends in
 * gtk_widget_queue_draw(), which walks the widget hierarchy and edits the toplevel's
 * invalidation region -- structures the GUI thread is reading at the same time. That is
 * Sentry 142561119: an access violation inside GTK's own memmove, on the job thread, with
 * dt_control_work() still on the stack underneath it.
 *
 * So the work is marshalled onto the main loop unless we are already on it. Running inline
 * when we are keeps the ordering every existing GUI-thread caller already relies on. */
static void _gui_refresh_thumbnail(int32_t imgid, gboolean refresh_filmstrip)
{
  if(dt_widget_on_gui_thread())
  {
    _refresh_thumbnail_widgets(imgid, refresh_filmstrip);
    return;
  }

  _thumbnail_refresh_request_t *request = g_malloc(sizeof(_thumbnail_refresh_request_t));
  request->imgid = imgid;
  request->refresh_filmstrip = refresh_filmstrip;
  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, _refresh_thumbnail_on_gui_thread, request, g_free);
}

int dt_gui_gtk_init(dt_gui_gtk_t *gui)
{
  /* lets zero mem */
  memset(gui, 0, sizeof(dt_gui_gtk_t));

  dt_pthread_mutex_init(&gui->mutex, NULL);

  // force gtk3 to use normal scroll bars instead of the popup thing. they get in the way of controls
  // the alternative would be to gtk_scrolled_window_set_overlay_scrolling(..., FALSE); every single widget
  // that might have scroll bars
  g_setenv("GTK_OVERLAY_SCROLLING", "0", 0);

  // same for ubuntus overlay-scrollbar-gtk3
  g_setenv("LIBOVERLAY_SCROLLBAR", "0", 0);

  // unset gtk rc from kde:
  char path[DT_PATH_MAX] = { 0 }, datadir[DT_PATH_MAX] = { 0 }, sharedir[DT_PATH_MAX] = { 0 }, configdir[DT_PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  const char *css_theme = dt_conf_get_string_const("ui_last/theme");
  if(css_theme)
    g_strlcpy(gui->gtkrc, css_theme, sizeof(gui->gtkrc));
  else
    g_snprintf(gui->gtkrc, sizeof(gui->gtkrc), "ansel");

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  GtkOSXApplication *OSXApp = g_object_new(GTK_TYPE_OSX_APPLICATION, NULL);
  gtk_osxapplication_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#else
  GtkosxApplication *OSXApp = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
  gtkosx_application_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#endif
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationBlockTermination", G_CALLBACK(_osx_quit_callback), NULL);
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationOpenFile", G_CALLBACK(_osx_openfile_callback), NULL);
#endif

  GtkWidget *widget;
  gui->ui = g_malloc0(sizeof(dt_ui_t));
  gui->surface = NULL;
  gui->center_tooltip = 0;
  gui->culling_mode = FALSE;
  gui->presets_popup_menu = NULL;
  gui->last_preset = NULL;
  gui->export_popup.window = NULL;
  gui->export_popup.module = NULL;
  gui->styles_popup.window = NULL;
  gui->styles_popup.module = NULL;

  // smooth scrolling must be enabled to handle trackpad/touch events
  dt_widget_set_scroll_mask(GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

  // Emulates the same feature as Gtk focus but for scrolling events
  // The GtkWidget capturing scrolling events will write its address in this pointer
  dt_widget_set_scroll_focus(NULL);

  // Init global accels. We localize the config because accels pathes use translated GUI labels.
  // User switching between languages may loose their custom shortcuts if we didn't localize them.
  // NOTE: needs to be inited before widgets, more specifically before the global menu
  gchar *keyboardrc = g_strdup_printf("keyboardrc.%s", dt_l10n_get_current_lang(dt_l10n_get_global()));
  gchar *keyboardrc_path = g_build_filename(configdir, keyboardrc, NULL);

  GtkAccelFlags flags = 0;
  if(dt_conf_get_bool("accels/mask")) flags |= GTK_ACCEL_MASK;
  gui->accels = dt_accels_init(keyboardrc_path, flags);
  dt_free(keyboardrc);
  dt_free(keyboardrc_path);

  // Load the user's saved shortcuts right away, before _init_widgets() below builds the
  // main window and enters the default view. Views call dt_accels_connect_accels() from
  // their own enter() (see e.g. views/lighttable.c), which happens well before darktable.c's
  // own later, single dt_accels_connect_accels() call -- without this earlier load, that
  // first connect reconciles every shortcut against an empty accel map (key=0, mods=0) and
  // permanently records it as a deliberate "user cleared this shortcut" override, before the
  // real saved value has had a chance to be read at all. This must be the ONLY call to
  // dt_accels_load_user_config(): calling it again anywhere later would re-read the
  // still-on-disk (unsaved until exit) file and clobber any normalization applied to the
  // live GtkAccelMap since this first load.
  dt_accels_load_user_config(gui->accels);

  // Initializing widgets
  _init_widgets(gui);

  //init overlay colors
  dt_guides_set_overlay_colors();

  dt_concat_path_file(path, datadir, "icons");
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);
  dt_concat_path_file(path, sharedir, "icons");
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);

  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  widget = center;

  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_visible(widget, TRUE);
  gtk_widget_grab_focus(widget);
  gtk_widget_add_events(widget, GDK_PROXIMITY_IN_MASK | GDK_PROXIMITY_OUT_MASK | GDK_TABLET_PAD_MASK);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_configure), gui);
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw), NULL);
  g_signal_connect(G_OBJECT(widget), "motion-notify-event", G_CALLBACK(_mouse_moved), NULL);
  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(_key_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "leave-notify-event", G_CALLBACK(_center_leave), NULL);
  g_signal_connect(G_OBJECT(widget), "enter-notify-event", G_CALLBACK(_center_enter), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_button_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(_button_released), NULL);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(_scrolled), NULL);
#ifdef _WIN32
  g_signal_connect(G_OBJECT(widget), "realize", G_CALLBACK(_center_realize), NULL);
  g_signal_connect(G_OBJECT(widget), "unrealize", G_CALLBACK(_center_unrealize), NULL);
  if(gtk_widget_get_realized(widget))
    _center_realize(widget, NULL);
#endif

  dt_gui_presets_init();

  _update_display_profile();
  // update the profile when the window is moved. resize is already handled in configure()
  widget = dt_ui_main_window(darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_window_configure), NULL);

  dt_gui_freeze_reset();

  // load theme
  dt_gui_load_theme(gui->gtkrc);

  // let's try to support pressure sensitive input devices like tablets for mask drawing
  dt_print(DT_DEBUG_INPUT, "[input device] Input devices found:\n\n");

  GList *input_devices
      = gdk_seat_get_slaves(gdk_display_get_default_seat(gdk_display_get_default()), GDK_SEAT_CAPABILITY_ALL);
  const int manager_slave_count = 0;
  const int manager_floating_count = 0;
  GList *stylus_devices
      = gdk_seat_get_slaves(gdk_display_get_default_seat(gdk_display_get_default()), GDK_SEAT_CAPABILITY_TABLET_STYLUS);
  dt_print(DT_DEBUG_INPUT, "[input device] seat capabilities bitmask: %u\n",
           (unsigned int)gdk_seat_get_capabilities(gdk_display_get_default_seat(gdk_display_get_default())));
  dt_print(DT_DEBUG_INPUT, "[input device] stylus-capable devices reported by seat: %d\n", g_list_length(stylus_devices));
  dt_print(DT_DEBUG_INPUT, "[input device] manager fallback devices: slave=%d floating=%d merged_total=%d\n",
           manager_slave_count, manager_floating_count, g_list_length(input_devices));
  for(GList *l = stylus_devices; !IS_NULL_PTR(l); l = g_list_next(l))
  {
    GdkDevice *device = (GdkDevice *)l->data;
    if(IS_NULL_PTR(device)) continue;
    dt_print(DT_DEBUG_INPUT, "  [tablet seat] %s source=%s axes_flags=%u n_axes=%d\n",
             gdk_device_get_name(device), _get_source_name(gdk_device_get_source(device)),
             (unsigned int)gdk_device_get_axes(device), gdk_device_get_n_axes(device));
  }
  if(stylus_devices)
  {
    g_list_free(stylus_devices);
    stylus_devices = NULL;
  }
  for(GList *l = input_devices; !IS_NULL_PTR(l); l = g_list_next(l))
  {
    GdkDevice *device = (GdkDevice *)l->data;
    if(IS_NULL_PTR(device)) continue;
    const GdkInputSource source = gdk_device_get_source(device);
    const gint n_axes = (source == GDK_SOURCE_KEYBOARD ? 0 : gdk_device_get_n_axes(device));

    // force-enable everything we find in screen mode.
    // TODO: make that an user param ?
    gdk_device_set_mode(device, GDK_MODE_SCREEN);

    dt_print(DT_DEBUG_INPUT, "%s (%s), source: %s, mode: %s, %d axes, %d keys\n", gdk_device_get_name(device),
             (source != GDK_SOURCE_KEYBOARD) && gdk_device_get_has_cursor(device) ? "with cursor" : "no cursor",
             _get_source_name(source),
             _get_mode_name(gdk_device_get_mode(device)), n_axes,
             source != GDK_SOURCE_KEYBOARD ? gdk_device_get_n_keys(device) : 0);

    for(int i = 0; i < n_axes; i++)
    {
      dt_print(DT_DEBUG_INPUT, "  %s\n", _get_axis_name(gdk_device_get_axis_use(device, i)));
    }
    dt_print(DT_DEBUG_INPUT, "\n");
  }
  if(input_devices)
  {
    g_list_free(input_devices);
    input_devices = NULL;
  }

  // Gtk seems to capture some reserved shortcuts (Tab). We need to bypass it entirely
  // by hacking all events.
  gtk_widget_add_events(dt_ui_main_window(gui->ui), dt_widget_scroll_mask());
  g_signal_connect(G_OBJECT(dt_ui_main_window(gui->ui)), "event", G_CALLBACK(dt_accels_dispatch), gui->accels);

  // finally set the cursor to be the default.
  // for some reason this is needed on some systems to pick up the correctly themed cursor
  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_widget_set_mouse_radius(DT_UI_SCALE_DEVICE(15.0f), DT_UI_SCALE_DEVICE(15.0f));

  // Tell common/ how to repaint a stale thumbnail. The backend announces staleness and
  // knows nothing about thumbtables; this is the only place the two are connected.
  // Tell the widget layer which thread owns widget state, and how the user wants scrolling.
  if(dt_control_get_global()) dt_widget_set_gui_thread(dt_control_get_global()->gui_thread);
  dt_widget_set_scroll_reversed(dt_conf_get_bool("scroll/reverse_x"), dt_conf_get_bool("scroll/reverse_y"));

  dt_widget_set_root_window_handler(_widget_root_window);
  dt_widget_set_natural_width_handler(_widget_natural_width);
  dt_widget_set_cursor_handler(_widget_cursor);
  dt_widget_set_refocus_handler(dt_gui_refocus_center);
  dt_colorspaces_set_profile_changed_handler(_notify_profile_changed);
  dt_widget_set_debug_overlays((dt_get_debug_flags() & DT_DEBUG_MASKS) != 0);
  dt_widget_set_storage_handlers(_widget_stored_int, _widget_store_int,
                                 _widget_stored_bool, _widget_store_bool);
  dt_widget_set_notebook_page_handler(_notebook_page_changed);
  dt_widget_set_message_handler(_widget_message);
  dt_accels_set_global(gui->accels);
  dt_accels_set_top_offset_handler(_accels_top_offset);
  dt_accels_set_refocus_handler(dt_gui_refocus_center);
  dt_accels_set_recent_handlers(_accels_recent_get, _accels_recent_set);

  dt_thumbnail_notify_set_handler(_gui_refresh_thumbnail);
  dt_startup_progress_set_handler(_gui_startup_progress);
  dt_film_gui_register_handlers();
  dt_collection_gui_register_handlers();
  dt_folder_survey_gui_register_handlers();
  dt_history_merge_gui_register_handlers();

  return 0;
}

void dt_gui_gtk_run(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  if(darktable.gui->surface)
  {
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = NULL;
  }

  darktable.gui->surface
      = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  /* Pre-configure the views so a draw arriving before the first configure-event has a valid
   * size to work with.
   *
   * The size handed over must be THE SAME size the configure-event handler will report for the
   * same widget, and it was not: this passed the allocation minus twice an 8 px `tabborder',
   * while dt_control_configure() passes the event's width and height untouched. Two callers,
   * one widget, two answers 16 px apart -- so the view was configured at one size here and at
   * another as soon as GTK delivered an allocation, and a view that recomputes on a size change
   * (the darkroom pipeline does) paid for both.
   *
   * The border was not drawn by anything: `tabborder' was written on the line above and read on
   * the line below, and nowhere else in the program. It is gone. */
  dt_view_manager_configure(darktable.view_manager, allocation.width, allocation.height);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  gtk_osxapplication_ready(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL));
#else
  gtkosx_application_ready(g_object_new(GTKOSX_TYPE_APPLICATION, NULL));
#endif
#endif
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_focus_window();
#endif
  /* start the event loop */
  gtk_main();

  if (darktable.gui->surface)
  {
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = NULL;
  }
  dt_cleanup();
}


void dt_configure_ppd_dpi(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = gui->ui->main_window;

  gui->ppd = dt_get_system_gui_ppd(widget);
  dt_widget_set_ppd(gui->ppd);
  gui->filter_image = CAIRO_FILTER_GOOD;
  dt_widget_set_image_filter(gui->filter_image);

  // get the screen resolution
  const float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");
  if(screen_dpi_overwrite > 0.0)
  {
    gui->dpi = screen_dpi_overwrite;
    gdk_screen_set_resolution(gtk_widget_get_screen(widget), screen_dpi_overwrite);
    dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi as specified in "
                               "the configuration file\n",
             screen_dpi_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_autoset_dpi(widget);
#endif
    gui->dpi = gdk_screen_get_resolution(gtk_widget_get_screen(widget));
    if(gui->dpi < 0.0)
    {
      gui->dpi = 96.0;
      gdk_screen_set_resolution(gtk_widget_get_screen(widget), 96.0);
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to the default 96 dpi\n");
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi\n", gui->dpi);
  }
  gui->dpi_factor
      = gui->dpi / 96;
  dt_screen_set_dpi(gui->dpi); // the raw resolution, for whoever reports it rather than scales by it
  dt_widget_set_dpi_factor(gui->dpi_factor); // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default

  // em depends on the screen DPI (point -> px), so refresh it here too.
  dt_gui_update_em();
}

// Last DT_GUI_BOX_SPACING value actually applied to containers. Used to retarget exactly
// the containers that carry the standard spacing when em changes, leaving deliberate 0-spacing
// and custom-spacing containers untouched. Seeded with the pre-em reference (10px).
static gint _last_box_spacing = 10;

typedef struct _spacing_ctx_t { gint old_s, new_s; } _spacing_ctx_t;

// Recursively retarget GtkBox/GtkGrid/GtkFlowBox children whose spacing still equals the
// previously-applied standard spacing. Setting spacing inside gtk_container_foreach() doesn't
// mutate the child list, so the walk is safe.
static void _refresh_container_spacing(GtkWidget *w, gpointer user_data)
{
  const _spacing_ctx_t *c = (const _spacing_ctx_t *)user_data;

  if(GTK_IS_BOX(w))
  {
    if(gtk_box_get_spacing(GTK_BOX(w)) == c->old_s) gtk_box_set_spacing(GTK_BOX(w), c->new_s);
  }
  else if(GTK_IS_FLOW_BOX(w))
  {
    if((gint)gtk_flow_box_get_row_spacing(GTK_FLOW_BOX(w)) == c->old_s)
      gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(w), c->new_s);
    if((gint)gtk_flow_box_get_column_spacing(GTK_FLOW_BOX(w)) == c->old_s)
      gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(w), c->new_s);
  }
  else if(GTK_IS_GRID(w))
  {
    if((gint)gtk_grid_get_row_spacing(GTK_GRID(w)) == c->old_s)
      gtk_grid_set_row_spacing(GTK_GRID(w), c->new_s);
    if((gint)gtk_grid_get_column_spacing(GTK_GRID(w)) == c->old_s)
      gtk_grid_set_column_spacing(GTK_GRID(w), c->new_s);
  }

  if(GTK_IS_CONTAINER(w))
    gtk_container_foreach(GTK_CONTAINER(w), _refresh_container_spacing, user_data);
}

// Propagate a new DT_GUI_BOX_SPACING to already-built containers across every toplevel, so a
// runtime font/DPI change updates the inner gutters live (gtk_*_set_spacing bakes the value into
// the widget at creation time, so reloading the CSS alone is not enough).
static void _refresh_all_container_spacing(void)
{
  const gint new_s = DT_GUI_BOX_SPACING;
  if(new_s == _last_box_spacing) return;

  _spacing_ctx_t c = { _last_box_spacing, new_s };
  GList *toplevels = gtk_window_list_toplevels(); // list owned by us, elements not reffed
  for(GList *l = toplevels; l; l = l->next)
    _refresh_container_spacing(GTK_WIDGET(l->data), &c);
  g_list_free(toplevels);

  _last_box_spacing = new_s;
}

void dt_gui_update_em(void)
{
  dt_gui_gtk_t *gui = darktable.gui;
  if(!gui || !gui->ui || !gui->ui->main_window) return;

  GtkStyleContext *ctx = gtk_widget_get_style_context(gui->ui->main_window);
  PangoFontDescription *desc = NULL;
  gtk_style_context_get(ctx, gtk_style_context_get_state(ctx), GTK_STYLE_PROPERTY_FONT, &desc, NULL);
  if(!desc) return;

  const gint size = pango_font_description_get_size(desc);
  if(size > 0)
  {
    if(pango_font_description_get_size_is_absolute(desc))
      // already device-independent px
      gui->em = (double)size / PANGO_SCALE;
    else
      // points -> px at the screen DPI, matching how GTK renders point-sized fonts
      gui->em = (double)size / PANGO_SCALE * gui->dpi / 72.0;

    dt_widget_set_em_size(gui->em);
  }
  pango_font_description_free(desc);

  // The new em may change DT_GUI_BOX_SPACING; push it to existing containers so the change is live.
  _refresh_all_container_spacing();
}

void dt_gui_set_pango_resolution(PangoLayout *layout)
{
  if(IS_NULL_PTR(layout) || !darktable.gui) return;
  // Cairo-drawn text is laid out in points; the screen DPI converts those to device-independent px,
  // matching how GTK renders the rest of the UI. Centralized here so call sites never hand-write the DPI.
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
}


static gboolean _focus_in_out_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_window_set_urgency_hint(GTK_WINDOW(widget), FALSE);
  return FALSE;
}

static gboolean _ui_log_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static gboolean _ui_toast_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static void _init_widgets(dt_gui_gtk_t *gui)
{
  GtkWidget *container;
  GtkWidget *widget;

  // Creating the main window
  gui->ui->main_window  = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(gui->ui->main_window , "main_window");
  gtk_window_set_role(GTK_WINDOW(gui->ui->main_window ), "main-app");
  gtk_window_set_icon_name(GTK_WINDOW(gui->ui->main_window ), "ansel");
  gtk_window_set_title(GTK_WINDOW(gui->ui->main_window ), "Ansel");

  // Init the titlebar ASAP because we replace the desktop titlebar & decoration with ours
  dt_ui_init_titlebar(gui->ui);

  dt_configure_ppd_dpi(gui);

  gtk_window_set_default_size(GTK_WINDOW(gui->ui->main_window), DT_PIXEL_APPLY_DPI(1200), DT_PIXEL_APPLY_DPI(800));

  // NOTE: allowing full-screen on startup shits the bed with MacOS
  if(dt_conf_get_bool("ui_last/maximized"))
  {
    gboolean restore_window_position = TRUE;
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay *display = gtk_widget_get_display(gui->ui->main_window);
    if(GDK_IS_WAYLAND_DISPLAY(display))
      restore_window_position = FALSE;
#endif

    if(restore_window_position)
    {
      GdkDisplay *window_display = gtk_widget_get_display(gui->ui->main_window);
      GdkMonitor *monitor = NULL;

      if(!IS_NULL_PTR(window_display))
      {
        if(dt_conf_key_exists("ui_last/window_monitor"))
        {
          const int monitor_index = dt_conf_get_int("ui_last/window_monitor");
          if(monitor_index >= 0 && monitor_index < gdk_display_get_n_monitors(window_display))
            monitor = gdk_display_get_monitor(window_display, monitor_index);
        }

        if(IS_NULL_PTR(monitor)
           && dt_conf_key_exists("ui_last/window_x")
           && dt_conf_key_exists("ui_last/window_y"))
        {
          const int x = dt_conf_get_int("ui_last/window_x");
          const int y = dt_conf_get_int("ui_last/window_y");
          monitor = gdk_display_get_monitor_at_point(window_display, x, y);
        }

        if(IS_NULL_PTR(monitor))
          monitor = gdk_display_get_primary_monitor(window_display);
        if(IS_NULL_PTR(monitor) && gdk_display_get_n_monitors(window_display) > 0)
          monitor = gdk_display_get_monitor(window_display, 0);
      }

      if(!IS_NULL_PTR(monitor))
      {
        GdkRectangle workarea = { 0 };
        gdk_monitor_get_workarea(monitor, &workarea);
        gtk_window_move(GTK_WINDOW(gui->ui->main_window), workarea.x, workarea.y);
      }
    }

    gtk_window_maximize(GTK_WINDOW(gui->ui->main_window));
  }
  else
  {
    int width = dt_conf_get_int("ui_last/window_width");
    int height = dt_conf_get_int("ui_last/window_height");
    gtk_window_resize(GTK_WINDOW(gui->ui->main_window), width, height);

    gboolean restore_window_position = TRUE;
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay *display = gtk_widget_get_display(gui->ui->main_window);
    if(GDK_IS_WAYLAND_DISPLAY(display))
      restore_window_position = FALSE;
#endif

    if(restore_window_position
       && dt_conf_key_exists("ui_last/window_x")
       && dt_conf_key_exists("ui_last/window_y"))
    {
      const int x = dt_conf_get_int("ui_last/window_x");
      const int y = dt_conf_get_int("ui_last/window_y");

      int clamped_x = x;
      int clamped_y = y;
      GdkDisplay *window_display = gtk_widget_get_display(gui->ui->main_window);
      GdkMonitor *monitor = NULL;

      if(!IS_NULL_PTR(window_display))
      {
        if(dt_conf_key_exists("ui_last/window_monitor"))
        {
          const int monitor_index = dt_conf_get_int("ui_last/window_monitor");
          if(monitor_index >= 0 && monitor_index < gdk_display_get_n_monitors(window_display))
            monitor = gdk_display_get_monitor(window_display, monitor_index);
        }

        if(IS_NULL_PTR(monitor))
          monitor = gdk_display_get_monitor_at_point(window_display, x + width / 2, y + height / 2);
        if(IS_NULL_PTR(monitor))
          monitor = gdk_display_get_primary_monitor(window_display);
        if(IS_NULL_PTR(monitor) && gdk_display_get_n_monitors(window_display) > 0)
          monitor = gdk_display_get_monitor(window_display, 0);
      }

      if(!IS_NULL_PTR(monitor))
      {
        GdkRectangle workarea = { 0 };
        gdk_monitor_get_workarea(monitor, &workarea);

        const int max_x = workarea.x + MAX(0, workarea.width - width);
        const int max_y = workarea.y + MAX(0, workarea.height - height);
        clamped_x = CLAMP(x, workarea.x, max_x);
        clamped_y = CLAMP(y, workarea.y, max_y);
      }

      gtk_window_move(GTK_WINDOW(gui->ui->main_window), clamped_x, clamped_y);
    }
  }

  dt_gui_splash_set_transient_for(gui->ui->main_window);

  g_signal_connect(G_OBJECT(gui->ui->main_window ), "delete_event", G_CALLBACK(dt_gui_quit_callback), NULL);
  g_signal_connect(G_OBJECT(gui->ui->main_window ), "focus-in-event", G_CALLBACK(_focus_in_out_event), NULL);
  g_signal_connect(G_OBJECT(gui->ui->main_window ), "focus-out-event", G_CALLBACK(_focus_in_out_event), NULL);
  g_signal_connect_after(G_OBJECT(gui->ui->main_window ), "key-press-event", G_CALLBACK(_key_pressed), NULL);

  container = gui->ui->main_window;

  // Adding the outermost vbox
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  /* connect to signal redraw all */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_REDRAW_ALL,
                            G_CALLBACK(_ui_widget_redraw_callback), gui->ui->main_window);

  container = widget;

  // Initializing the main table
  dt_ui_init_main_table(container, gui->ui);

  /* the log message */
  GtkWidget *eb = gtk_event_box_new();
  darktable.gui->ui->log_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_log_button_press_event),
                   darktable.gui->ui->log_msg);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->log_msg), PANGO_ELLIPSIZE_MIDDLE);
  dt_gui_add_class(darktable.gui->ui->log_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->log_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb);
  //gtk_overlay_reorder_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb, -1);

  /* the toast message */
  eb = gtk_event_box_new();
  darktable.gui->ui->toast_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_toast_button_press_event),
                   darktable.gui->ui->toast_msg);
  gtk_widget_set_events(eb, GDK_BUTTON_PRESS_MASK | dt_widget_scroll_mask());
  g_signal_connect(G_OBJECT(eb), "scroll-event", G_CALLBACK(_scrolled), NULL);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->toast_msg), PANGO_ELLIPSIZE_MIDDLE);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  gtk_label_set_attributes(GTK_LABEL(darktable.gui->ui->toast_msg), attrlist);
  pango_attr_list_unref(attrlist);

  dt_gui_add_class(darktable.gui->ui->toast_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->toast_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_START);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb);
  //gtk_overlay_reorder_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb, -1);

  /* update log message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_LOG_REDRAW, G_CALLBACK(_ui_log_redraw_callback),
                            darktable.gui->ui->log_msg);

  /* update toast message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_TOAST_REDRAW, G_CALLBACK(_ui_toast_redraw_callback),
                            darktable.gui->ui->toast_msg);


  // Showing everything
  gtk_widget_show_all(dt_ui_main_window(gui->ui));

  gtk_widget_set_visible(dt_ui_log_msg(gui->ui), FALSE);
  gtk_widget_set_visible(dt_ui_toast_msg(gui->ui), FALSE);
}

void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));

  if(GTK_WIDGET(ui->containers[c]) != gtk_widget_get_parent(w)) return;

  gtk_container_set_focus_child(GTK_CONTAINER(ui->containers[c]), w);
  gtk_widget_queue_draw(ui->containers[c]);
}

void dt_ui_container_foreach(dt_ui_t *ui, const dt_ui_container_t c, GtkCallback callback)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_foreach(GTK_CONTAINER(ui->containers[c]), callback, (gpointer)ui->containers[c]);
}

void dt_ui_container_destroy_children(dt_ui_t *ui, const dt_ui_container_t c)
{
  dt_gui_container_destroy_children(GTK_CONTAINER(ui->containers[c]));
}

void dt_ui_notify_user()
{
  if(darktable.gui && !gtk_window_is_active(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui))))
  {
    gtk_window_set_urgency_hint(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), TRUE);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
    gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
    gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif
  }
}

/* this is called as a signal handler, the signal raising logic asserts the gdk lock. */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget)
{
   gtk_widget_queue_draw(widget);
}

static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw log message, if any
  dt_control_t *const control = dt_control_get_global();
  dt_pthread_mutex_lock(&control->log_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&control->log_mutex);
    return;
  }
  if(dt_control_get_global()->log_ack != dt_control_get_global()->log_pos)
  {
    if(strcmp(dt_control_get_global()->log_message[dt_control_get_global()->log_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), dt_control_get_global()->log_message[dt_control_get_global()->log_ack]);
    gtk_widget_show(widget);
  }
  else
  {
    gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&control->log_mutex);
}

static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw toast message, if any
  dt_control_t *const control = dt_control_get_global();
  dt_pthread_mutex_lock(&control->toast_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&control->toast_mutex);
    return;
  }
  if(dt_control_get_global()->toast_ack != dt_control_get_global()->toast_pos)
  {
    if(strcmp(dt_control_get_global()->toast_message[dt_control_get_global()->toast_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), dt_control_get_global()->toast_message[dt_control_get_global()->toast_ack]);
    if(!gtk_widget_get_visible(widget))
    {
      const int h = gtk_widget_get_allocated_height(dt_ui_center_base(darktable.gui->ui));
      gtk_widget_set_margin_bottom(gtk_widget_get_parent(widget), 0.15 * h - DT_PIXEL_APPLY_DPI(10));
      gtk_widget_show(widget);
    }
  }
  else
  {
    if(gtk_widget_get_visible(widget)) gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&control->toast_mutex);
}











// TODO: should that go to another place than gtk.c?
void dt_gui_add_help_link(GtkWidget *widget, char *link)
{
  g_object_set_data_full(G_OBJECT(widget), "dt-help-url", link, g_free);
  gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
}

// load a CSS theme
void dt_gui_load_theme(const char *theme)
{
  char theme_css[DT_PATH_MAX] = { 0 };
  g_snprintf(theme_css, sizeof(theme_css), "%s.css", theme);

  if(!dt_conf_key_exists("use_system_font"))
    dt_conf_set_bool("use_system_font", TRUE);

  //set font size
  if(dt_conf_get_bool("use_system_font"))
    gtk_settings_reset_property(gtk_settings_get_default(), "gtk-font-name");
  else
  {
    //font name can only use period as decimal separator
    //but printf format strings use comma for some locales, so replace comma with period
    gchar *font_size = g_strdup_printf(_("%.1f"), dt_conf_get_float("font_size"));
    gchar *font_size_updated = dt_util_str_replace(font_size, ",", ".");
    gchar *font_name = g_strdup_printf(_("Sans %s"), font_size_updated);
    g_object_set(gtk_settings_get_default(), "gtk-font-name", font_name, NULL);
    dt_free(font_size_updated);
    dt_free(font_size);
    dt_free(font_name);
  }

  gchar *path, *usercsspath;
  char datadir[DT_PATH_MAX] = { 0 }, configdir[DT_PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  // user dir theme
  path = g_build_filename(configdir, "themes", theme_css, NULL);
  if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    // dt dir theme
    dt_free(path);
    path = g_build_filename(datadir, "themes", theme_css, NULL);
    if(!g_file_test(path, G_FILE_TEST_EXISTS))
    {
      // fallback to default theme
      dt_free(path);
      path = g_build_filename(datadir, "themes", "ansel.css", NULL);
      dt_conf_set_string("ui_last/theme", "ansel");
    }
    else
      dt_conf_set_string("ui_last/theme", theme);
  }
  else
    dt_conf_set_string("ui_last/theme", theme);

  GError *error = NULL;

  GtkStyleProvider *themes_style_provider = GTK_STYLE_PROVIDER(gtk_css_provider_new());
  gtk_style_context_add_provider_for_screen
    (gdk_screen_get_default(), themes_style_provider, GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

  usercsspath = g_build_filename(configdir, "user.css", NULL);

  gchar *path_uri = g_filename_to_uri(path, NULL, &error);
  if(IS_NULL_PTR(path_uri))
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, path, error->message);

  gchar *usercsspath_uri = g_filename_to_uri(usercsspath, NULL, &error);
  if(IS_NULL_PTR(usercsspath_uri))
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, usercsspath, error->message);

  gchar *themecss = NULL;
  if(dt_conf_get_bool("themes/usercss") && g_file_test(usercsspath, G_FILE_TEST_EXISTS))
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri,
                                           "'); @import url('", usercsspath_uri, "');", NULL);
  }
  else
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri, "');", NULL);
  }

  dt_free(path_uri);
  dt_free(usercsspath_uri);
  dt_free(path);
  dt_free(usercsspath);

  if(dt_conf_get_bool("ui/hide_tooltips"))
  {
    gchar *newcss = g_strjoin(NULL, themecss, " tooltip {opacity: 0; background: transparent;}", NULL);
    dt_free(themecss);
    themecss = newcss;
  }

  if(!gtk_css_provider_load_from_data(GTK_CSS_PROVIDER(themes_style_provider), themecss, -1, &error))
  {
    fprintf(stderr, "%s: error parsing combined CSS %s: %s\n", G_STRFUNC, themecss, error->message);
    g_clear_error(&error);
  }

  dt_free(themecss);

  g_object_unref(themes_style_provider);

  // setup the colors

  GdkRGBA *c = dt_widget_colors();
  GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_widget_get_style_context(main_window);

  c[DT_GUI_COLOR_BG] = (GdkRGBA){ 0.1333, 0.1333, 0.1333, 1.0 };

  struct color_init
  {
    const char *name;
    GdkRGBA default_col;
  } init[DT_GUI_COLOR_LAST] = {
    [DT_GUI_COLOR_DARKROOM_BG] = { "darkroom_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_DARKROOM_PREVIEW_BG] = { "darkroom_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_BG] = { "lighttable_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG] = { "lighttable_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_FONT] = { "lighttable_bg_font_color", { .7, .7, .7, 1.0 } },
    [DT_GUI_COLOR_PRINT_BG] = { "print_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_BRUSH_CURSOR] = { "brush_cursor", { 1., 1., 1., 0.9 } },
    [DT_GUI_COLOR_BRUSH_TRACE] = { "brush_trace", { 0., 0., 0., 0.8 } },
    [DT_GUI_COLOR_BUTTON_FG] = { "button_fg", { 0.7, 0.7, 0.7, 0.55 } },
    [DT_GUI_COLOR_THUMBNAIL_BG] = { "thumbnail_bg_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BG] = { "thumbnail_selected_bg_color", { 0.8, 0.8, 0.8, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_BG] = { "thumbnail_hover_bg_color", { 0.65, 0.65, 0.65, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_OUTLINE] = { "thumbnail_outline_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE] = { "thumbnail_selected_outline_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE] = { "thumbnail_hover_outline_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_FONT] = { "thumbnail_font_color", { 0.425, 0.425, 0.425, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT] = { "thumbnail_selected_font_color", { 0.5, 0.5, 0.5, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_FONT] = { "thumbnail_hover_font_color", { 0.7, 0.7, 0.7, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_BORDER] = { "thumbnail_border_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER] = { "thumbnail_selected_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_FILMSTRIP_BG] = { "filmstrip_bg_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_PREVIEW_HOVER_BORDER] = { "preview_hover_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_LOG_BG] = { "log_bg_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_LOG_FG] = { "log_fg_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_SAME_LOC] = { "map_count_same_loc_color", { 1.0, 1.0, 1.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_DIFF_LOC] = { "map_count_diff_loc_color", { 1.0, 0.85, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_BG] = { "map_count_bg_color", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH] = { "map_count_circle_color_h", { 1.0, 1.0, 0.8, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_LOW] = { "map_count_circle_color_l", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_DEF] = { "map_count_circle_color_d", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_WARNING] = { "warning_color", { 1.0, 0.647, 0.0, 1.0 } },
  };

  // starting from 1 as DT_GUI_COLOR_BG is not part of this table
  for(int i = 1; i < DT_GUI_COLOR_LAST; i++)
  {
    if(!gtk_style_context_lookup_color(ctx, init[i].name, &c[i]))
    {
      c[i] = init[i].default_col;
    }
  }

  dt_widget_set_theme_loaded(TRUE);

  // The active theme/font may change the root font size, so refresh the cached em
  // that drives DT_GUI_BOX_SPACING.
  dt_gui_update_em();
}





















































// draw rounded rectangle


















void dt_gui_refocus_center()
{
  // Refocus window, useful if we just closed a popup/modal/transient
  gtk_window_present_with_time(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GDK_CURRENT_TIME);

  // Desperate measure to refocus the window
  gtk_grab_add(dt_ui_main_window(darktable.gui->ui));
  gtk_grab_remove(dt_ui_main_window(darktable.gui->ui));
  gtk_widget_grab_focus(dt_ui_main_window(darktable.gui->ui));

  // dt_view_manager_name() returns the translated display name (e.g. "Table lumineuse"), not the
  // internal id: compare against module_name, which is stable and untranslated.
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  if(!IS_NULL_PTR(current_view) && !strcmp(current_view->module_name, "lighttable"))
  {
    gtk_widget_grab_focus(darktable.gui->ui->thumbtable_lighttable->grid);
  }
  else
  {
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }

  // Be sure to re-enable accelerators
  dt_accels_disable(darktable.gui->accels, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
