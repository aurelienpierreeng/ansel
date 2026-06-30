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
#include "common/darktable.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/l10n.h"
#include "common/file_location.h"
#include "common/ratings.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "gui/guides.h"
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/sidepanel.h"

#include "gui/gtk.h"
#include "gui/splash.h"

#include "common/styles.h"
#include "control/conf.h"
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
#include <unistd.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <pthread.h>

/*
 * NEW UI API
 */

/* generic callback for redraw widget signals */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget);
/* callback for redraw log signals */
static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget);
static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget);

// set class function to add CSS classes with just a simple line call
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(!gtk_style_context_has_class(context, class_name))
  {
    gtk_style_context_add_class(context, class_name);
    gtk_widget_queue_draw(widget);
  }
}

void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(gtk_style_context_has_class(context, class_name))
  {
    gtk_style_context_remove_class(context, class_name);
    gtk_widget_queue_draw(widget);
  }
}

/* ------------------------------------------------------------------------------------------
 * Widget-callback suppression depth (see common/darktable.h for the rationale and API).
 * ------------------------------------------------------------------------------------------ */
static inline gboolean _dt_on_gui_thread(void)
{
  // Same idiom as dt_control_signal (control/signal.c): is the caller the GUI/main thread?
  return darktable.control && pthread_equal(darktable.control->gui_thread, pthread_self());
}

gboolean dt_gui_widgets_suppressed(void)
{
  return darktable.gui && darktable.gui->_widget_suppress_depth > 0;
}

void dt_gui_freeze_begin_(const char *file, int line)
{
  // Only the GUI thread owns widget state. Off-thread callers (notably worker-thread
  // reload_defaults during thumbnail/export, which has no widgets to suppress) must not touch
  // the shared depth, or concurrent non-atomic ++/-- drift it and break suppression for the
  // GUI thread. For them this is a deliberate no-op.
  if(!darktable.gui || !_dt_on_gui_thread()) return;
  // MAX(.,0) heals any pre-existing negative drift so the depth is always genuinely suppressing.
  darktable.gui->_widget_suppress_depth = MAX(darktable.gui->_widget_suppress_depth, 0) + 1;
  (void)file;
  (void)line;
}

void dt_gui_freeze_end_(const char *file, int line)
{
  if(!darktable.gui || !_dt_on_gui_thread()) return;
  if(darktable.gui->_widget_suppress_depth <= 0)
  {
    // A bare end with nothing to match: an unbalanced freeze bracket exists. Surface it (with
    // the offending site) instead of letting the counter go negative and silently disable
    // suppression for the rest of the session.
    fprintf(stderr, "[dt_gui_freeze] unbalanced end at %s:%d (depth was %d); "
                    "look for a freeze begin without a matching end.\n",
            file, line, darktable.gui->_widget_suppress_depth);
    darktable.gui->_widget_suppress_depth = 0;
    return;
  }
  darktable.gui->_widget_suppress_depth--;
}

void dt_gui_freeze_reset(void)
{
  if(darktable.gui) darktable.gui->_widget_suppress_depth = 0;
}

/*
 * OLD UI API
 */
static void _init_widgets(dt_gui_gtk_t *gui);
static gboolean _configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data);

gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  gboolean handled = FALSE;
  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? 1.0 : -1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -1.0 : 1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? 1.0 : -1.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -1.0 : 1.0;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      if((delta_x && event->delta_x != 0) || (delta_y && event->delta_y != 0))
      {
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
        if(delta_x) *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x / 50. : event->delta_x / 50.;
        if(delta_y) *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y / 50. : event->delta_y / 50.;
#else
        if(delta_x) *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x : event->delta_x;
        if(delta_y) *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y : event->delta_y;
#endif
        handled = TRUE;
      }
    default:
      break;
    }
  return handled;
}

gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  // accumulates scrolling regardless of source or the widget being scrolled
  static gdouble acc_x = 0.0, acc_y = 0.0;

  gboolean handled = FALSE;

  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? 1 : -1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -1 : 1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? 1 : -1;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -1 : 1;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      // stop events reset accumulated delta
      if(event->is_stop)
      {
        acc_x = acc_y = 0.0;
        break;
      }
      // accumulate trackpad/touch scrolls until they make a unit
      // scroll, and only then tell caller that there is a scroll to
      // handle
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
      acc_x += dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x / 50. : event->delta_x / 50.;
      acc_y += dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y / 50. : event->delta_y / 50.;
#else
      acc_x += dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x : event->delta_x;
      acc_y += dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y : event->delta_y;
#endif
      const gdouble amt_x = trunc(acc_x);
      const gdouble amt_y = trunc(acc_y);
      if(amt_x != 0 || amt_y != 0)
      {
        acc_x -= amt_x;
        acc_y -= amt_y;
        if((delta_x && amt_x != 0) || (delta_y && amt_y != 0))
        {
          if(delta_x) *delta_x = (int)amt_x;
          if(delta_y) *delta_y = (int)amt_y;
          handled = TRUE;
        }
      }
      break;
    default:
      break;
  }
  return handled;
}

gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta)
{
  gdouble delta_x, delta_y;
  if(dt_gui_get_scroll_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta)
{
  int delta_x, delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}


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

#ifdef _DEBUG
void dt_gtk_widget_queue_draw_ext(GtkWidget *widget, const char *name, const char *file, const int line)
{
  if(!GTK_IS_WIDGET(widget))
  {
    dt_print(DT_DEBUG_GTK, "gtk_widget_queue_draw(%s) called with a non-WIDGET or NULL widget at %s:%d (widget=%p)\n",
             name, file, line, widget);
    return;
  }
  else
    dt_print(DT_DEBUG_GTK, "queueing redraw for `%s` (`%s`) at %s:%d\n",
             name, gtk_widget_get_name(widget), file, line);


  (gtk_widget_queue_draw)(widget);
}

void dt_gtk_toggle_button_set_active_ext(GtkToggleButton *toggle_button, const char *name, const gboolean active,
                                         const char *file, const int line)
{
  if(!GTK_IS_TOGGLE_BUTTON(toggle_button))
  {
    dt_print(DT_DEBUG_GTK, "gtk_toggle_button_set_active(%s) called with a non-TOGGLE_BUTTON or NULL widget at %s:%d (toggle_button=%p)\n",
            name, file, line, toggle_button);
    return;
  }
  else
    dt_print(DT_DEBUG_GTK, "setting toggle button `%s` (`%s`) to %s at %s:%d\n", name, gtk_widget_get_name(GTK_WIDGET(toggle_button)),
            active ? "active" : "inactive", file, line);

  (gtk_toggle_button_set_active)(toggle_button, active);
}
#endif

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

void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t color)
{
  const GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgb(cr, bc.red, bc.green, bc.blue);
}

void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t color, float opacity_coef)
{
  GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgba(cr, bc.red, bc.green, bc.blue, bc.alpha * opacity_coef);
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
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
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
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
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
  darktable.gui->has_scroll_focus = NULL;
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
  char path[PATH_MAX] = { 0 }, datadir[PATH_MAX] = { 0 }, sharedir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
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
  gui->selection_stacked = FALSE;
  gui->presets_popup_menu = NULL;
  gui->last_preset = NULL;
  gui->export_popup.window = NULL;
  gui->export_popup.module = NULL;
  gui->styles_popup.window = NULL;
  gui->styles_popup.module = NULL;

  // smooth scrolling must be enabled to handle trackpad/touch events
  gui->scroll_mask = GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK;

  // Emulates the same feature as Gtk focus but for scrolling events
  // The GtkWidget capturing scrolling events will write its address in this pointer
  gui->has_scroll_focus = NULL;

  // Init global accels. We localize the config because accels pathes use translated GUI labels.
  // User switching between languages may loose their custom shortcuts if we didn't localize them.
  // NOTE: needs to be inited before widgets, more specifically before the global menu
  gchar *keyboardrc = g_strdup_printf("keyboardrc.%s", dt_l10n_get_current_lang(darktable.l10n));
  gchar *keyboardrc_path = g_build_filename(configdir, keyboardrc, NULL);

  GtkAccelFlags flags = 0;
  if(dt_conf_get_bool("accels/mask")) flags |= GTK_ACCEL_MASK;
  gui->accels = dt_accels_init(keyboardrc_path, flags);
  dt_free(keyboardrc);
  dt_free(keyboardrc_path);

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

  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY);
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
  gtk_widget_add_events(dt_ui_main_window(gui->ui), gui->scroll_mask);
  g_signal_connect(G_OBJECT(dt_ui_main_window(gui->ui)), "event", G_CALLBACK(dt_accels_dispatch), gui->accels);

  // finally set the cursor to be the default.
  // for some reason this is needed on some systems to pick up the correctly themed cursor
  dt_control_change_cursor(GDK_LEFT_PTR);
  gui->mouse.effect_radius = DT_UI_SCALE_DEVICE(15.0f);

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
  // need to pre-configure views to avoid crash caused by draw coming before configure-event
  darktable.control->tabborder = 8;
  const int tb = darktable.control->tabborder;
  dt_view_manager_configure(darktable.view_manager, allocation.width - 2 * tb, allocation.height - 2 * tb);
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

// refactored function to read current ppd, because gtk for osx has been unreliable
// we use the specific function here. Anyway, if nothing meaningful is found we default back to 1.0
double dt_get_system_gui_ppd(GtkWidget *widget)
{
  double res = 0.0f;
#ifdef GDK_WINDOWING_QUARTZ
  res = dt_osx_get_ppd();
#else
  res = gtk_widget_get_scale_factor(widget);
#endif
  if((res < 1.0f) || (res > 4.0f))
  {
    dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] can't detect system ppd\n");
    return 1.0f;
  }
  dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] system ppd is %f\n", res);
  return res;
}

void dt_configure_ppd_dpi(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = gui->ui->main_window;

  gui->ppd = dt_get_system_gui_ppd(widget);
  gui->filter_image = CAIRO_FILTER_GOOD;

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
      = gui->dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default

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

void dt_gui_cairo_set_font_options(cairo_t *cr, GtkWidget *widget)
{
  if(IS_NULL_PTR(cr)) return;

  // Source GTK's resolved text-rendering options (anti-aliasing, hinting, subpixel order,
  // hint-metrics/kerning), which GTK populates from GtkSettings/Xft/fontconfig. The widget's
  // Pango context is the same source native widgets use; fall back to the main window, then to the
  // screen defaults, so an off-screen/scratch Cairo surface never silently reverts to Cairo's
  // AA-on defaults (which would make our cairo-drawn text look unlike the rest of the UI).
  const cairo_font_options_t *fo = NULL;

  if(widget)
  {
    PangoContext *pc = gtk_widget_get_pango_context(widget);
    if(pc) fo = pango_cairo_context_get_font_options(pc);
  }
  if(!fo && darktable.gui && darktable.gui->ui && darktable.gui->ui->main_window)
  {
    PangoContext *pc = gtk_widget_get_pango_context(darktable.gui->ui->main_window);
    if(pc) fo = pango_cairo_context_get_font_options(pc);
  }
  if(!fo)
  {
    GdkScreen *screen = gdk_screen_get_default();
    if(screen) fo = gdk_screen_get_font_options(screen);
  }

  // cairo_set_font_options() copies internally, so the const pointer's lifetime is not a concern.
  if(fo) cairo_set_font_options(cr, fo);
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
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL,
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
  gtk_widget_set_events(eb, GDK_BUTTON_PRESS_MASK | darktable.gui->scroll_mask);
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
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW, G_CALLBACK(_ui_log_redraw_callback),
                            darktable.gui->ui->log_msg);

  /* update toast message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW, G_CALLBACK(_ui_toast_redraw_callback),
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
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&darktable.control->log_mutex);
    return;
  }
  if(darktable.control->log_ack != darktable.control->log_pos)
  {
    if(strcmp(darktable.control->log_message[darktable.control->log_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->log_message[darktable.control->log_ack]);
    gtk_widget_show(widget);
  }
  else
  {
    gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
}

static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw toast message, if any
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
    return;
  }
  if(darktable.control->toast_ack != darktable.control->toast_pos)
  {
    if(strcmp(darktable.control->toast_message[darktable.control->toast_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->toast_message[darktable.control->toast_ack]);
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
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
}

void dt_ellipsize_combo(GtkComboBox *cbox)
{
  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(cbox));
  for(const GList *it = renderers; it; it = g_list_next(it))
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
  }
  g_list_free(renderers);
  renderers = NULL;
}

typedef struct result_t
{
  enum {RESULT_NONE, RESULT_NO, RESULT_YES} result;
  char *entry_text;
  GtkWidget *window, *entry, *button_yes, *button_no;
} result_t;

static void _gtk_main_quit_safe(GtkWidget *widget, gpointer data)
{
  (void)widget;
  (void)data;
  if(gtk_main_level() > 0) gtk_main_quit();
}

static void _yes_no_button_handler(GtkButton *button, gpointer data)
{
  result_t *result = (result_t *)data;

  if((void *)button == (void *)result->button_yes)
    result->result = RESULT_YES;
  else if((void *)button == (void *)result->button_no)
    result->result = RESULT_NO;

  if(result->entry)
    result->entry_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(result->entry)));
  gtk_widget_destroy(result->window);
  _gtk_main_quit_safe(NULL, NULL);
}

gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  // themes not yet loaded, no CSS add some manual padding
  const int padding = darktable.themes ? 0 : 5;

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  if(!IS_NULL_PTR(darktable.gui) && !IS_NULL_PTR(darktable.gui->ui)
     && !IS_NULL_PTR(darktable.gui->ui->main_window))
  {
    GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
    if(GTK_IS_WINDOW(main_window))
    {
      GtkWindow *win = GTK_WINDOW(main_window);
      gtk_window_set_transient_for(GTK_WINDOW(window), win);
      gtk_window_set_modal(GTK_WINDOW(window), TRUE);
      if(gtk_widget_get_visible(GTK_WIDGET(win)))
      {
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      }
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *mhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), mhbox, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(mhbox), label, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return result.result == RESULT_YES;
}

char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);

  if(!IS_NULL_PTR(darktable.gui) && !IS_NULL_PTR(darktable.gui->ui)
     && !IS_NULL_PTR(darktable.gui->ui->main_window))
  {
    GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
    if(GTK_IS_WINDOW(main_window))
    {
      GtkWindow *win = GTK_WINDOW(main_window);
      gtk_window_set_transient_for(GTK_WINDOW(window), win);
      if(gtk_widget_get_visible(GTK_WIDGET(win)))
      {
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      }
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);
  gtk_widget_set_margin_top(vbox, 7);
  gtk_widget_set_margin_bottom(vbox, 5);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  dt_accels_disconnect_on_text_input(entry);

  g_object_ref(entry);
  if(placeholder)
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_box_pack_start(GTK_BOX(vbox), entry, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_margin_top(hbox, 10);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window, .entry = entry};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  if(result.result == RESULT_YES)
    return result.entry_text;

  dt_free(result.entry_text);
  return NULL;
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
  char theme_css[PATH_MAX] = { 0 };
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
  char datadir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
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

  GdkRGBA *c = darktable.gui->colors;
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
  };

  // starting from 1 as DT_GUI_COLOR_BG is not part of this table
  for(int i = 1; i < DT_GUI_COLOR_LAST; i++)
  {
    if(!gtk_style_context_lookup_color(ctx, init[i].name, &c[i]))
    {
      c[i] = init[i].default_col;
    }
  }

  // The active theme/font may change the root font size, so refresh the cached em
  // that drives DT_GUI_BOX_SPACING.
  dt_gui_update_em();
}

GdkModifierType dt_key_modifier_state()
{
  guint state = 0;
  GdkWindow *window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  gdk_device_get_state(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window))), window, NULL, &state);
  return state;

/* FIXME double check correct way of doing this (merge conflict with Input System NG 20210319)
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  return gdk_keymap_get_modifier_state(keymap) & gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_DEFAULT_MOD_MASK);
*/
}

static void _notebook_size_callback(GtkNotebook *notebook, GdkRectangle *allocation, gpointer *data)
{
  const int n = gtk_notebook_get_n_pages(notebook);
  g_return_if_fail(n > 0);

  GtkRequestedSize *sizes = g_malloc_n(n, sizeof(GtkRequestedSize));

  for(int i = 0; i < n; i++)
  {
    sizes[i].data = gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i));
    sizes[i].minimum_size = 0;
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(sizes[i].data, NULL, &natural_size);
    sizes[i].natural_size = natural_size.width;
  }

  GtkAllocation first, last;
  gtk_widget_get_allocation(sizes[0].data, &first);
  gtk_widget_get_allocation(sizes[n - 1].data, &last);

  const gint total_space = last.x + last.width - first.x; // ignore tab padding; CSS sets padding for label

  if(total_space > 0)
  {
    gtk_distribute_natural_allocation(total_space, n, sizes);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, sizes[i].minimum_size, -1);

    gtk_widget_size_allocate(GTK_WIDGET(notebook), allocation);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, -1, -1);
  }

  dt_free(sizes);
}

// GTK_STATE_FLAG_PRELIGHT does not seem to get set on the label on hover so
// state-flags-changed cannot update darktable.control->element for shortcut mapping
static gboolean _notebook_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation notebook_alloc, label_alloc;
  gtk_widget_get_allocation(widget, &notebook_alloc);

  GtkNotebook *notebook = GTK_NOTEBOOK(widget);
  const int n = gtk_notebook_get_n_pages(notebook);
  for(int i = 0; i < n; i++)
  {
    gtk_widget_get_allocation(gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i)), &label_alloc);
  }

  return FALSE;
}

GtkNotebook *dt_ui_notebook_new()
{
  return GTK_NOTEBOOK(gtk_notebook_new());
}

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip)
{
  gchar *text_cpy = g_strdup(_(text));
  dt_capitalize_label(text_cpy);
  GtkWidget *label = gtk_label_new(text_cpy);
  dt_free(text_cpy);
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  if(strlen(text) > 2)
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(label, tooltip ? tooltip : _(text));
  gtk_widget_set_has_tooltip(GTK_WIDGET(notebook), FALSE);

  gint page_num = gtk_notebook_append_page(notebook, page, label);
  gtk_container_child_set(GTK_CONTAINER(notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  if(page_num == 1 &&
     !g_signal_handler_find(G_OBJECT(notebook), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _notebook_size_callback, NULL))
  {
    g_signal_connect(G_OBJECT(notebook), "size-allocate", G_CALLBACK(_notebook_size_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "motion-notify-event", G_CALLBACK(_notebook_motion_notify_callback), NULL);
  }

  return page;
}

static gint _get_container_row_heigth(GtkWidget *w)
{
  gint height = DT_PIXEL_APPLY_DPI(10);

  if(GTK_IS_TREE_VIEW(w))
  {
    gint row_height = 0;

    const gint num_columns = gtk_tree_view_get_n_columns(GTK_TREE_VIEW(w));
    for(int c = 0; c < num_columns; c++)
    {
      gint cell_height = 0;
      gtk_tree_view_column_cell_get_size(gtk_tree_view_get_column(GTK_TREE_VIEW(w), c),
                                        NULL, NULL, NULL, NULL, &cell_height);
      if(cell_height > row_height) row_height = cell_height;
    }
    GValue separation = { G_TYPE_INT };
    gtk_widget_style_get_property(w, "vertical-separator", &separation);

    if(row_height > 0) height = row_height + g_value_get_int(&separation);
  }
  else if(GTK_IS_TEXT_VIEW(w))
  {
    PangoLayout *layout = gtk_widget_create_pango_layout(w, "X");
    pango_layout_get_pixel_size(layout, NULL, &height);
    g_object_unref(layout);
  }
  else
  {
    GtkWidget *child = dt_gui_container_first_child(GTK_CONTAINER(w));
    if(child)
    {
      height = gtk_widget_get_allocated_height(child);
    }
  }

  return height;
}

static const char *const DT_GUI_WIDGET_AUTO_HEIGHT_KEY = "dt-gui-widget-auto-height";

// find the scrolled window parent of a treeview, if any
static GtkWidget *_search_parent_scrolled_window(GtkWidget *w)
{
  if(!GTK_IS_WIDGET(w)) return NULL;
  
  GtkWidget *parent = w;
  while(parent)
  {
    if(GTK_IS_SCROLLED_WINDOW(parent)) break;
    parent = gtk_widget_get_parent(parent);
  }

  return GTK_IS_SCROLLED_WINDOW(parent) ? parent : NULL;
}

// Counts only visible items (those whose parents are expanded)
static int _treeview_count_visible_rows(GtkTreeView *treeview, GtkTreeModel *model, GtkTreeIter *parent)
{
  if(!GTK_IS_TREE_MODEL(model)) return 0;

  GtkTreeIter iter;
  gboolean valid = parent ? gtk_tree_model_iter_children(model, &iter, parent)
                          : gtk_tree_model_get_iter_first(model, &iter);
  int count = 0;

  while(valid)
  {
    count++;
    
    // If this item is expanded, recursively count its visible children
    if(gtk_tree_model_iter_has_child(model, &iter))
    {
      GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
      if(path)
      {
        if(gtk_tree_view_row_expanded(treeview, path))
        {
          count += _treeview_count_visible_rows(treeview, model, &iter);
        }
        gtk_tree_path_free(path);
      }
    }
    
    valid = gtk_tree_model_iter_next(model, &iter);
  }

  return count;
}

static int _textview_count_visible_rows(GtkWidget *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return 0;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  if(!GTK_IS_TEXT_BUFFER(buffer)) return 0;

  // For text views, use the number of logical lines in the buffer.
  return MAX(0, gtk_text_buffer_get_line_count(buffer));
}

static void _widget_auto_disconnect_model(dt_gui_widget_auto_height_t *state, GtkWidget *treeview)
{
  if(IS_NULL_PTR(state)) return;

  GtkTreeModel *model = state->model;
  if(model)
  {
    if(state->model_row_inserted)   g_signal_handler_disconnect(model, state->model_row_inserted);
    if(state->model_row_deleted)    g_signal_handler_disconnect(model, state->model_row_deleted);
    if(state->model_row_changed)    g_signal_handler_disconnect(model, state->model_row_changed);
    if(state->model_rows_reordered) g_signal_handler_disconnect(model, state->model_rows_reordered);
    g_object_remove_weak_pointer(G_OBJECT(model), (gpointer *)&state->model);
  }

  if(GTK_IS_TREE_VIEW(treeview))
  {
    if(state->model_row_expanded)   g_signal_handler_disconnect(treeview, state->model_row_expanded);
    if(state->model_row_collapsed)  g_signal_handler_disconnect(treeview, state->model_row_collapsed);
  }

  state->model = NULL;
  state->model_row_inserted = 0;
  state->model_row_deleted = 0;
  state->model_row_changed = 0;
  state->model_rows_reordered = 0;
  state->model_row_expanded = 0;
  state->model_row_collapsed = 0;
}

static void _widget_auto_disconnect_buffer(dt_gui_widget_auto_height_t *state)
{
  if(IS_NULL_PTR(state)) return;

  GtkTextBuffer *buffer = state->buffer;
  if(buffer)
  {
    if(state->buffer_changed) g_signal_handler_disconnect(buffer, state->buffer_changed);
    g_object_remove_weak_pointer(G_OBJECT(buffer), (gpointer *)&state->buffer);
  }

  state->buffer = NULL;
  state->buffer_changed = 0;
}

/**
 * @brief Window-height ceiling shared by the auto-size rule and the drag handle.
 *
 * @details The full main-window height: a resizable area may grow as tall as the window (the
 * parent panel scrolls to reach it). Content shorter than this still shrinks to fit, so this only
 * bounds how far the user can drag.
 */
static gint _resizable_scroll_max_height(void)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  return win ? gtk_widget_get_allocated_height(win) : DT_PIXEL_APPLY_DPI(1000);
}

/**
 * @brief The single sizing rule for every dt_ui_scroll_wrap area.
 *
 * @details Height = clamp(min(content, cap), min_size, 75% window), where the cap is the user's
 * persisted height when set, otherwise the window ceiling so the area auto-grows to its content.
 * This makes small content shrink to fit, lets nested lists grow and have their parent panel scroll
 * until the user drags the handle to cap them, and snaps lists/textviews to whole rows to avoid
 * clipped half-rows. The computed bare (pre-padding) height is cached for the drag handle.
 */
static void _resizable_scroll_apply(GtkWidget *w)
{
  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkWidget *sw = _search_parent_scrolled_window(w);
  if(!GTK_IS_SCROLLED_WINDOW(sw)) return;

  const gint max_height = _resizable_scroll_max_height();
  const gint min_size = MAX(1, state->min_size);
  const gboolean has_conf = state->config_str && dt_conf_key_exists(state->config_str);

  gint height;
  gint increment = 0;

  if(state->mode == DT_UI_RESIZE_STATIC)
  {
    // Fixed height: the user's persisted size, or min_size as the default. Independent of content,
    // so a content refresh (e.g. on hovering another thumbnail) never shifts the layout.
    height = CLAMP(has_conf ? dt_conf_get_int(state->config_str) : min_size, min_size, max_height);
  }
  else
  {
    // Dynamic: fit to content, capped by the user's persisted height (or the window ceiling).
    const gboolean row_based = GTK_IS_TREE_VIEW(w) || GTK_IS_TEXT_VIEW(w);
    increment = row_based ? _get_container_row_heigth(w) : 0;

    gint content = 0;
    if(GTK_IS_TREE_VIEW(w))
    {
      const int rows = _treeview_count_visible_rows(GTK_TREE_VIEW(w), gtk_tree_view_get_model(GTK_TREE_VIEW(w)), NULL);
      content = MAX(1, rows) * increment;
    }
    else if(GTK_IS_TEXT_VIEW(w))
    {
      const int rows = _textview_count_visible_rows(w);
      content = MAX(1, rows) * increment;
    }
    else
    {
      gtk_widget_get_preferred_height(w, NULL, &content);
    }

    const gint cap = has_conf ? CLAMP(dt_conf_get_int(state->config_str), min_size, max_height) : max_height;
    height = CLAMP(MIN(content, cap), min_size, max_height);

    // snap to whole rows for lists/textviews to avoid clipped half-rows
    if(increment > 0)
    {
      height += increment - 1;
      height -= height % increment;
    }
  }
  state->last_height = height;

  GtkBorder padding;
  gtk_style_context_get_padding(gtk_widget_get_style_context(sw),
                                gtk_widget_get_state_flags(sw), &padding);

  gint old_height = 0;
  gtk_widget_get_size_request(sw, NULL, &old_height);
  const gint new_height = height + padding.top + padding.bottom + (GTK_IS_TEXT_VIEW(w) ? 2 : 0);
  if(new_height != old_height)
    gtk_widget_set_size_request(sw, -1, new_height);
}

static void _widget_auto_update(GtkWidget *widget)
{
  _resizable_scroll_apply(widget);
}

static gboolean _resizable_scroll_draw(GtkWidget *w, cairo_t *cr, gpointer user_data)
{
  _resizable_scroll_apply(w);
  return FALSE;
}

static void _resizable_scroll_realize(GtkWidget *w, gpointer user_data)
{
  _resizable_scroll_apply(w);
}

// Drag handle accessors: bare (pre-padding) target height, kept consistent with the sizing rule.
static int _resizable_scroll_handle_get_size(gpointer user_data)
{
  GtkWidget *w = GTK_WIDGET(user_data);
  const dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  return state ? state->last_height : 0;
}

static int _resizable_scroll_handle_resize(int requested_size, gboolean finished, gpointer user_data)
{
  GtkWidget *w = GTK_WIDGET(user_data);
  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state) || IS_NULL_PTR(state->config_str)) return requested_size;

  const gint value = CLAMP(requested_size, MAX(1, state->min_size), _resizable_scroll_max_height());
  dt_conf_set_int(state->config_str, value);
  _resizable_scroll_apply(w);
  return state->last_height;
}

static void _widget_auto_model_row_inserted(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                              gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer user_data)
{
  (void)model;
  (void)path;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_changed(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                             gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_rows_reordered(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                                gint *new_order, gpointer user_data)
{
  (void)model;
  (void)path;
  (void)iter;
  (void)new_order;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_expanded(GtkTreeView *tree_view, GtkTreeIter *expanded_iter, GtkTreePath *path,
                                              gpointer user_data)
{
  (void)tree_view;
  (void)expanded_iter;
  (void)path;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_model_row_collapsed(GtkTreeView *tree_view, GtkTreeIter *collapsed_iter, GtkTreePath *path,
                                               gpointer user_data)
{
  (void)tree_view;
  (void)collapsed_iter;
  (void)path;

  // Recalculate tree view height after loading the data
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
  (void)buffer;
  _widget_auto_update(GTK_WIDGET(user_data));
}

static void _widget_auto_connect_model(GtkWidget *treeview)
{
  if(!GTK_IS_TREE_VIEW(treeview)) return;

  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(treeview), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  if(model == state->model) return;

  _widget_auto_disconnect_model(state, treeview);
  if(!GTK_IS_TREE_MODEL(model)) return;

  state->model = model;
  g_object_add_weak_pointer(G_OBJECT(model), (gpointer *)&state->model);
  state->model_row_inserted =   g_signal_connect(model, "row-inserted",
                                              G_CALLBACK(_widget_auto_model_row_inserted), treeview);
  state->model_row_deleted =    g_signal_connect(model, "row-deleted",
                                              G_CALLBACK(_widget_auto_model_row_deleted), treeview);
  state->model_row_changed =    g_signal_connect(model, "row-changed",
                                              G_CALLBACK(_widget_auto_model_row_changed), treeview);
  state->model_rows_reordered = g_signal_connect(model, "rows-reordered",
                                              G_CALLBACK(_widget_auto_model_rows_reordered), treeview);
  state->model_row_expanded =   g_signal_connect(treeview, "row-expanded",
                                              G_CALLBACK(_widget_auto_model_row_expanded), treeview);
  state->model_row_collapsed =  g_signal_connect(treeview, "row-collapsed",
                                              G_CALLBACK(_widget_auto_model_row_collapsed), treeview);
}

static void _widget_auto_connect_buffer(GtkWidget *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return;

  dt_gui_widget_auto_height_t *state = g_object_get_data(G_OBJECT(textview), DT_GUI_WIDGET_AUTO_HEIGHT_KEY);
  if(IS_NULL_PTR(state)) return;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
  if(buffer == state->buffer) return;

  _widget_auto_disconnect_buffer(state);
  if(!GTK_IS_TEXT_BUFFER(buffer)) return;

  state->buffer = buffer;
  g_object_add_weak_pointer(G_OBJECT(buffer), (gpointer *)&state->buffer);
  state->buffer_changed = g_signal_connect(buffer, "changed",
                                           G_CALLBACK(_widget_auto_text_buffer_changed), textview);
}

static void _widget_auto_on_model_changed(GObject *treeview, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  (void)user_data;
  _widget_auto_connect_model(GTK_WIDGET(treeview));
  _widget_auto_update(GTK_WIDGET(treeview));
}

static void _widget_auto_on_buffer_changed(GObject *textview, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  (void)user_data;
  _widget_auto_connect_buffer(GTK_WIDGET(textview));
  _widget_auto_update(GTK_WIDGET(textview));
}

static void _widget_auto_height_free(gpointer data)
{
  dt_gui_widget_auto_height_t *state = (dt_gui_widget_auto_height_t *)data;
  if(IS_NULL_PTR(state)) return;
  _widget_auto_disconnect_model(state, NULL);
  _widget_auto_disconnect_buffer(state);
  g_free(state->config_str);
  dt_free(state);
}

void dt_gui_textview_set_padding(GtkTextView *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return;

  gtk_text_view_set_left_margin(textview, DT_PIXEL_APPLY_DPI(4));
  gtk_text_view_set_right_margin(textview, DT_PIXEL_APPLY_DPI(4));
  gtk_text_view_set_top_margin(textview, DT_PIXEL_APPLY_DPI(2));
  gtk_text_view_set_bottom_margin(textview, DT_PIXEL_APPLY_DPI(2));
}

/**
 * @brief Wrap a scrollable content widget in a recessed, vertically resizable scrolled window.
 *
 * @details Returns an overlay wrapping the scrolled window, with a themed drag grip floating on its
 * bottom edge (the same grip primitive used by panels and the histogram scope). The grip takes no
 * layout space and is invisible until hovered. Sizing follows @p mode: DT_UI_RESIZE_DYNAMIC auto-fits
 * the content up to the user height, DT_UI_RESIZE_STATIC keeps a fixed height (see _resizable_scroll_apply).
 *
 * The returned widget is the wrapper overlay, not the scrolled window; callers needing the inner
 * scrolled window (e.g. to tweak its scroll policy) must use dt_ui_scroll_wrap_get_scrolled_window().
 *
 * @param w content widget (treeview, textview or any container)
 * @param min_size minimum height floor in device pixels (also the static default before the user drags)
 * @param config_str conf key persisting the user-chosen height (copied internally)
 * @param mode DT_UI_RESIZE_DYNAMIC (auto-fit) or DT_UI_RESIZE_STATIC (fixed height)
 */
GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str, dt_ui_resize_mode_t mode)
{
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  if(GTK_IS_TREE_VIEW(w)) dt_gui_add_class(sw, "dt_recessed_scroll");
  gtk_container_add(GTK_CONTAINER(sw), w);

  // Per-widget sizing state, freed with w.
  dt_gui_widget_auto_height_t *state = calloc(1, sizeof(*state));
  state->config_str = g_strdup(config_str);
  state->min_size = MAX(1, DT_PIXEL_APPLY_DPI(min_size));
  state->mode = mode;
  g_object_set_data_full(G_OBJECT(w), DT_GUI_WIDGET_AUTO_HEIGHT_KEY, state, _widget_auto_height_free);

  if(mode == DT_UI_RESIZE_DYNAMIC)
  {
    // Sizing triggers. Lists/textviews recompute from their model/buffer change signals (cheap and
    // exact) plus a one-shot on realize, when row heights become accurate -- we deliberately avoid a
    // per-draw recount, which would re-walk a large model on every redraw. Generic content has no
    // such signals, so it recomputes on draw using its (GTK-cached) preferred height.
    if(GTK_IS_TREE_VIEW(w))
    {
      g_signal_connect(w, "notify::model", G_CALLBACK(_widget_auto_on_model_changed), NULL);
      g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
    }
    else if(GTK_IS_TEXT_VIEW(w))
    {
      g_signal_connect(w, "notify::buffer", G_CALLBACK(_widget_auto_on_buffer_changed), NULL);
      g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
    }
    else
    {
      g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(_resizable_scroll_draw), NULL);
    }
    _widget_auto_connect_model(w);
    _widget_auto_connect_buffer(w);
  }
  else
  {
    // Static: height is content-independent, so we only size once after realization (and on drag).
    g_signal_connect(w, "realize", G_CALLBACK(_resizable_scroll_realize), NULL);
  }

  // Drag grip floating on the scrolled window's bottom edge (overlay): it takes no layout space,
  // so the wrapper leaves no margin-like gap and stays aligned with neighbouring widgets. The grip
  // is centered on the bottom border via CSS and is invisible until hovered.
  GtkWidget *handle = dt_bauhaus_resize_handle_new(GTK_ORIENTATION_VERTICAL, FALSE,
                                                   _("Drag to resize"),
                                                   _resizable_scroll_handle_get_size,
                                                   _resizable_scroll_handle_resize, w);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), sw);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);

  _widget_auto_update(w);
  return over;
}

/**
 * @brief Return the inner scrolled window of a dt_ui_scroll_wrap() wrapper, or NULL.
 */
GtkWidget *dt_ui_scroll_wrap_get_scrolled_window(GtkWidget *wrapper)
{
  if(!GTK_IS_CONTAINER(wrapper)) return NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(wrapper));
  GtkWidget *sw = NULL;
  for(GList *l = children; l; l = g_list_next(l))
  {
    if(GTK_IS_SCROLLED_WINDOW(l->data))
    {
      sw = GTK_WIDGET(l->data);
      break;
    }
  }
  g_list_free(children);
  return sw;
}

// ---- Resizable drawing area (the histogram-scope paradigm, made reusable) -------------------
// A fixed-pixel-height GtkDrawingArea (or any widget sized by height-request) made vertically
// resizable by the shared grip primitive, with the height persisted to config. Unlike the
// scroll-wrap helper above, the content draws itself and is not scrolled; we only manage its
// height-request, so the drawing code keeps reading the live allocation as usual.

static const char *const DT_UI_RESIZABLE_AREA_KEY = "dt-ui-resizable-area";

typedef struct dt_ui_resizable_area_t
{
  char *config_str;   // conf key persisting the user-chosen height (px); owned
  int min_height;     // minimum height floor, device pixels
  int last_height;    // last applied height, shared with the drag handle
} dt_ui_resizable_area_t;

static void _resizable_area_free(gpointer data)
{
  dt_ui_resizable_area_t *state = (dt_ui_resizable_area_t *)data;
  if(IS_NULL_PTR(state)) return;
  g_free(state->config_str);
  dt_free(state);
}

static int _resizable_area_get_size(gpointer user_data)
{
  GtkWidget *area = GTK_WIDGET(user_data);
  const dt_ui_resizable_area_t *state = g_object_get_data(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY);
  if(state) return state->last_height;
  return gtk_widget_get_allocated_height(area);
}

static int _resizable_area_resize(int requested_size, gboolean finished, gpointer user_data)
{
  GtkWidget *area = GTK_WIDGET(user_data);
  dt_ui_resizable_area_t *state = g_object_get_data(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY);
  if(IS_NULL_PTR(state)) return requested_size;

  const int height = CLAMP(requested_size, MAX(1, state->min_height), _resizable_scroll_max_height());
  state->last_height = height;
  gtk_widget_set_size_request(area, -1, height);
  if(finished && state->config_str) dt_conf_set_int(state->config_str, height);
  return height;
}

GtkWidget *dt_ui_resizable_drawing_area(GtkWidget *area, char *config_str, int default_height, int min_height)
{
  dt_ui_resizable_area_t *state = calloc(1, sizeof(*state));
  state->config_str = g_strdup(config_str);
  state->min_height = MAX(1, DT_PIXEL_APPLY_DPI(min_height));

  int height = (config_str && dt_conf_key_exists(config_str)) ? dt_conf_get_int(config_str)
                                                              : DT_PIXEL_APPLY_DPI(default_height);
  height = CLAMP(height, state->min_height, _resizable_scroll_max_height());
  state->last_height = height;
  g_object_set_data_full(G_OBJECT(area), DT_UI_RESIZABLE_AREA_KEY, state, _resizable_area_free);
  gtk_widget_set_size_request(area, -1, height);

  // Drag grip floating on the area's bottom edge (an overlay, not a packed sibling), so it takes
  // no layout space -- the area stays flush with neighbouring widgets, no margin-like gap. It sits
  // over the graph's bottom inset/axis margin (graphs reserve one), invisible until hovered.
  GtkWidget *handle = dt_bauhaus_resize_handle_new(GTK_ORIENTATION_VERTICAL, FALSE,
                                                   _("Drag to resize"),
                                                   _resizable_area_get_size, _resizable_area_resize, area);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), area);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  return over;
}

gboolean dt_gui_container_has_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  gboolean has_children = !IS_NULL_PTR(children);
  g_list_free(children);
  children = NULL;
  return has_children;
}

int dt_gui_container_num_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  int num_children = g_list_length(children);
  g_list_free(children);
  children = NULL;
  return num_children;
}

GtkWidget *dt_gui_container_first_child(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = children ? (GtkWidget*)children->data : NULL;
  g_list_free(children);
  children = NULL;
  return child;
}

GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = (GtkWidget*)g_list_nth_data(children, which);
  g_list_free(children);
  children = NULL;
  return child;
}

static void _remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove((GtkContainer*)data, widget);
}

void dt_gui_container_remove_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _remove_child, container);
}

static void _delete_child(GtkWidget *widget, gpointer data)
{
  (void)data;  // avoid unreferenced-parameter warning
  gtk_widget_destroy(widget);
}

void dt_gui_container_destroy_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _delete_child, NULL);
}

GtkWidget *dt_gui_get_popup_relative_widget(GtkWidget *widget, GdkRectangle *rect)
{
  if(IS_NULL_PTR(widget)) return NULL;

  GtkWidget *relative = widget;

  // Wayland only accepts the top-most enclosing popup as transient parent.
  for(GtkWidget *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent))
    if(GTK_IS_POPOVER(parent)) relative = parent;

  if(rect)
  {
    rect->x = 0;
    rect->y = 0;
    rect->width = MAX(gtk_widget_get_allocated_width(widget), 1);
    rect->height = MAX(gtk_widget_get_allocated_height(widget), 1);

    if(relative != widget
       && !gtk_widget_translate_coordinates(widget, relative, 0, 0, &rect->x, &rect->y))
    {
      rect->x = 0;
      rect->y = 0;
    }
  }

  return relative;
}

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor)
{
  gtk_widget_show_all(GTK_WIDGET(menu));

  GdkEvent *event = gtk_get_current_event();
  if(button)
  {
    GdkRectangle rect = { 0 };
    GtkWidget *relative = dt_gui_get_popup_relative_widget(button, &rect);

    if(relative && relative != button && gtk_widget_get_window(relative))
      gtk_menu_popup_at_rect(menu, gtk_widget_get_window(relative), &rect, widget_anchor, menu_anchor, event);
    else
      gtk_menu_popup_at_widget(menu, button, widget_anchor, menu_anchor, event);
  }
  else
  {
    if(IS_NULL_PTR(event))
    {
      event = gdk_event_new(GDK_BUTTON_PRESS);
      event->button.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
      event->button.window = gtk_widget_get_window(GTK_WIDGET(darktable.gui->ui->main_window));
      g_object_ref(event->button.window);
    }

    gtk_menu_popup_at_pointer(menu, event);
  }
  gdk_event_free(event);
}

static void _popover_set_relative_to_topmost_parent(GtkPopover *popover, GtkWidget *button)
{
  GdkRectangle rect = { 0 };
  GtkWidget *relative = dt_gui_get_popup_relative_widget(button, &rect);
  gtk_popover_set_relative_to(popover, relative ? relative : button);
  gtk_popover_set_pointing_to(popover, &rect);
}

// draw rounded rectangle
void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y)
{
  const float radius = height / 5.0f;
  const float degrees = M_PI / 180.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path(cr);
  cairo_fill(cr);
}

gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry)
{
  if(gtk_search_entry_handle_event(entry, (GdkEvent *)event))
  {
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(entry));
    return TRUE;
  }

  return FALSE;
}

void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget)
{
  gtk_widget_grab_focus(widget);

  gtk_entry_set_text(GTK_ENTRY(entry), "");

  if(GTK_IS_TREE_VIEW(widget))
  {
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, NULL);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
    gtk_tree_path_free(path);
  }
}

static void _collapsible_set_states(dt_gui_collapsible_section_t *cs, gboolean active)
{
  if(active)
  {
    // We don't apply the GTK_STATE_SELECTED flag to the container here because it would
    // be inherited by all children, which would mess up the state of checkboxes and togglebuttons.
    dt_gui_add_class(GTK_WIDGET(cs->expander), "active");
  }
  else
  {
    gtk_widget_set_state_flags(GTK_WIDGET(cs->expander), GTK_STATE_FLAG_NORMAL, TRUE);
    dt_gui_remove_class(GTK_WIDGET(cs->expander), "active");
  }
}

static void _collapsible_container_show(GtkWidget *widget, gpointer user_data)
{
  /* Called whenever the container receives a "show" event, including from gtk_widget_show_all().
   * If the toggle is not active the section should remain collapsed, so we re-hide the container
   * immediately. By the time this fires, show_all has already recursed into the children and
   * set their visible flags, so a later expand will find all children ready to display. */
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle)))
    gtk_widget_hide(widget);
}

static void _coeffs_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dt_conf_set_bool(cs->confname, active);
  _collapsible_set_states(cs, active);
}

static void _coeffs_expander_click(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return;

  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), !active);
  _collapsible_set_states(cs, !active);
}

void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);

  if(active)
    gtk_widget_show(GTK_WIDGET(cs->container));
  else
    gtk_widget_hide(GTK_WIDGET(cs->container));

  _collapsible_set_states(cs, active);
}

void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), FALSE);
  gtk_widget_hide(GTK_WIDGET(cs->container));
  _collapsible_set_states(cs, FALSE);
}

void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent, GtkPackType pack)
{
  const gboolean expanded = dt_conf_get_bool(confname);

  cs->confname = confname;
  cs->parent = parent;

  // collapsible section header
  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *header_evb = gtk_event_box_new();
  cs->label = dt_ui_section_label_new(label);
  dt_gui_add_class(destdisp_head, "dt_section_expander");
  gtk_container_add(GTK_CONTAINER(header_evb), cs->label);

  cs->toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow,
                                      (expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), expanded);
  dt_gui_add_class(cs->toggle, "dt_ignore_fg_state");

  cs->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING));
  gtk_widget_set_name(GTK_WIDGET(cs->container), "collapsible");
  gtk_box_pack_start(GTK_BOX(destdisp_head), header_evb, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), cs->toggle, FALSE, FALSE, 0);

  cs->expander = dtgtk_expander_new(destdisp_head, GTK_WIDGET(cs->container));
  /* gtk_widget_show_all() on the parent (called by lib modules after gui_init) recurses into
   * the container and would override the collapsed state set by dtgtk_expander_set_expanded.
   * Connect to "show" so we can re-apply the correct visibility right after show_all touches it. */
  g_signal_connect(G_OBJECT(cs->container), "show",
                   G_CALLBACK(_collapsible_container_show), (gpointer)cs);
  // Pack at the requested side so callers control ordering at insertion time.
  if(pack == GTK_PACK_START)
    gtk_box_pack_start(GTK_BOX(cs->parent), cs->expander, FALSE, FALSE, 0);
  else
    gtk_box_pack_end(GTK_BOX(cs->parent), cs->expander, FALSE, FALSE, 0);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), expanded);
  gtk_widget_set_name(cs->expander, "collapse-block");

  g_signal_connect(G_OBJECT(cs->toggle), "toggled",
                   G_CALLBACK(_coeffs_button_changed),  (gpointer)cs);

  g_signal_connect(G_OBJECT(header_evb), "button-release-event",
                   G_CALLBACK(_coeffs_expander_click),
                   (gpointer)cs);
}

void dt_capitalize_label(gchar *text)
{
  if(text)
  {
    const char *underscore = "_";

    // Deal with strings beginning with Mnemonics
    if(text[0] == underscore[0])
      text[1] = g_unichar_toupper(text[1]);
    else
      text[0] = g_unichar_toupper(text[0]);
  }
}

GtkBox * attach_popover(GtkWidget *widget, const char *icon, GtkWidget *content)
{
  // Create the wrapping box and add the original widget to it
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);

  // Create the info icon button that will trigger the popover
  GtkWidget *button = gtk_menu_button_new();
  GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image(GTK_BUTTON(button), image);
  gtk_widget_set_hexpand(button, FALSE);
  gtk_widget_set_vexpand(button, FALSE);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);

  // Create the content of the popover
  GtkWidget *popover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(popover_box), content, FALSE, FALSE, 0);

  // Wrap the content into a popover and attach it to the button
  GtkWidget *popover = gtk_popover_new(button);
  gtk_container_add(GTK_CONTAINER(popover), popover_box);
  gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
  g_signal_connect(G_OBJECT(popover), "show", G_CALLBACK(_popover_set_relative_to_topmost_parent), button);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  gtk_widget_show_all(popover_box);

  return GTK_BOX(box);
}

GtkBox * attach_help_popover(GtkWidget *widget, const char *label)
{
  // Create the content of the popover
  GtkWidget *popover_label = gtk_label_new(label);
  gtk_label_set_line_wrap(GTK_LABEL(popover_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(popover_label), 60);
  return attach_popover(widget, "help-about", popover_label);
}

static gboolean _text_entry_focus_in_event(GtkWidget *self, GdkEventFocus event, gpointer user_data)
{
  dt_accels_disable(darktable.gui->accels, TRUE);
  return FALSE;
}

static gboolean _text_entry_focus_out_event(GtkWidget *self, GdkEventFocus event, gpointer user_data)
{
  dt_accels_disable(darktable.gui->accels, FALSE);
  return FALSE;
}

static gboolean _text_entry_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  if(event->keyval == GDK_KEY_Escape)
  {
    dt_gui_refocus_center();
    return TRUE;
  }
  return FALSE;
}

void dt_accels_disconnect_on_text_input(GtkWidget *widget)
{
  gtk_widget_add_events(widget, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect(G_OBJECT(widget), "focus-in-event", G_CALLBACK(_text_entry_focus_in_event), NULL);
  g_signal_connect(G_OBJECT(widget), "focus-out-event", G_CALLBACK(_text_entry_focus_out_event), NULL);
  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(_text_entry_key_pressed), NULL);
}


void dt_gui_refocus_center()
{
  // Refocus window, useful if we just closed a popup/modal/transient
  gtk_window_present_with_time(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GDK_CURRENT_TIME);

  // Desperate measure to refocus the window
  gtk_grab_add(dt_ui_main_window(darktable.gui->ui));
  gtk_grab_remove(dt_ui_main_window(darktable.gui->ui));
  gtk_widget_grab_focus(dt_ui_main_window(darktable.gui->ui));

  const char *current_view = dt_view_manager_name(darktable.view_manager);
  if(g_strcmp0(current_view, "lighttable"))
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
