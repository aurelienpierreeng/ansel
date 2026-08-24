/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "widgets/widget_settings.h"



#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"   // dt_osx_get_ppd(), the only reliable ppd source on quartz
#endif

// Toolkit state, set by the application during GUI init and read everywhere after.
// Single-threaded by construction: all of it is touched from the GUI thread only.
static GdkEventMask _scroll_mask = GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK;
static GtkWidget *_scroll_focus = NULL;
static cairo_filter_t _image_filter = CAIRO_FILTER_GOOD;

GdkEventMask dt_widget_scroll_mask(void)
{
  return _scroll_mask;
}

void dt_widget_set_scroll_mask(GdkEventMask mask)
{
  _scroll_mask = mask;
}

GtkWidget *dt_widget_scroll_focus(void)
{
  return _scroll_focus;
}

void dt_widget_set_scroll_focus(GtkWidget *widget)
{
  _scroll_focus = widget;
}

cairo_filter_t dt_widget_image_filter(void)
{
  return _image_filter;
}

void dt_widget_set_image_filter(cairo_filter_t filter)
{
  _image_filter = filter;
}

// Widget-update suppression depth. Was a field of dt_gui_gtk_t; it is toolkit state, and a
// widget should not need the application global to know whether to ignore its own callback.
static int32_t _widget_suppress_depth = 0;

// Which thread owns widget state, and whether it has been registered at all.
static pthread_t _gui_thread;
static gboolean _gui_thread_set = FALSE;

// Scroll axis inversion, a user preference the application supplies.
static gboolean _reverse_x = FALSE, _reverse_y = FALSE;

// Toolkit metrics. Defaults are the 96-DPI, 16px-font reference: correct for standalone
// dialogs that run after gtk_init() but before the application has resolved the real values.
//
// The store is here rather than in gui/, even though the GUI is what resolves the values,
// because every widget reads them on every draw: putting it in gui/ would make widgets/
// depend on gui/, and that dependency direction is what makes this module portable at all.
// gui/screen_metrics.c forwards to these under the dt_screen_* names for the handful of
// readers below layer 4 (the crash reporter, telemetry) and for the cairo helpers.
static double _dpi = 96.0;
static double _dpi_factor = 1.0;
static double _ppd = 1.0;
static double _em = 16.0;
static gboolean _metrics_probed = FALSE;

double dt_widget_dpi(void) { return _dpi; }
void dt_widget_set_dpi(double dpi) { if(dpi > 0.0) { _dpi = dpi; _metrics_probed = TRUE; } }

gboolean dt_widget_metrics_probed(void) { return _metrics_probed; }

static dt_widget_cursor_handler_t _cursor_handler = NULL;
static dt_widget_message_handler_t _message_handler = NULL;
static dt_widget_root_window_handler_t _root_window_handler = NULL;
static dt_widget_natural_width_handler_t _natural_width_handler = NULL;

void dt_widget_set_root_window_handler(dt_widget_root_window_handler_t handler)
{
  _root_window_handler = handler;
}

GtkWidget *dt_widget_root_window(void)
{
  return _root_window_handler ? _root_window_handler() : NULL;
}

void dt_widget_set_natural_width_handler(dt_widget_natural_width_handler_t handler)
{
  _natural_width_handler = handler;
}

gint dt_widget_natural_width(GtkWidget *widget)
{
  return _natural_width_handler ? _natural_width_handler(widget) : -1;
}

void dt_widget_set_message_handler(dt_widget_message_handler_t handler)
{
  _message_handler = handler;
}

void dt_widget_message(const char *message)
{
  if(_message_handler && message) _message_handler(message);
}

void dt_widget_set_cursor_handler(dt_widget_cursor_handler_t handler)
{
  _cursor_handler = handler;
}

void dt_widget_set_cursor(GdkCursorType cursor)
{
  if(_cursor_handler) _cursor_handler(cursor);
}

double dt_widget_dpi_factor(void) { return _dpi_factor; }
void dt_widget_set_dpi_factor(double factor) { if(factor > 0.0) { _dpi_factor = factor; _metrics_probed = TRUE; } }

double dt_widget_ppd(void) { return _ppd; }
void dt_widget_set_ppd(double ppd) { if(ppd > 0.0) { _ppd = ppd; _metrics_probed = TRUE; } }

double dt_widget_em_size(void) { return _em; }
void dt_widget_set_em_size(double em) { if(em > 0.0) _em = em; }

void dt_widget_set_gui_thread(pthread_t thread)
{
  _gui_thread = thread;
  _gui_thread_set = TRUE;
}

void dt_widget_set_scroll_reversed(gboolean reverse_x, gboolean reverse_y)
{
  _reverse_x = reverse_x;
  _reverse_y = reverse_y;
}

gboolean dt_widget_on_gui_thread(void)
{
  return _gui_thread_set && pthread_equal(_gui_thread, pthread_self());
}

// Colour-label palette, supplied by the application's theme.
static GdkRGBA _colorlabels[16];
static int _colorlabels_count = 0;

const GdkRGBA *dt_widget_colorlabel(int index)
{
  if(index < 0 || index >= _colorlabels_count) return NULL;
  return &_colorlabels[index];
}

void dt_widget_set_colorlabels(const GdkRGBA *labels, int count)
{
  if(labels == NULL || count <= 0) { _colorlabels_count = 0; return; }
  if(count > (int)(sizeof(_colorlabels) / sizeof(_colorlabels[0])))
    count = (int)(sizeof(_colorlabels) / sizeof(_colorlabels[0]));
  for(int i = 0; i < count; i++) _colorlabels[i] = labels[i];
  _colorlabels_count = count;
}

gboolean dt_gui_widgets_suppressed(void)
{
  return _widget_suppress_depth > 0;
}

void dt_gui_freeze_begin_(const char *file, int line)
{
  // Only the GUI thread owns widget state. Off-thread callers (notably worker-thread
  // reload_defaults during thumbnail/export, which has no widgets to suppress) must not touch
  // the shared depth, or concurrent non-atomic ++/-- drift it and break suppression for the
  // GUI thread. For them this is a deliberate no-op.
  if(!dt_widget_on_gui_thread()) return;
  // MAX(.,0) heals any pre-existing negative drift so the depth is always genuinely suppressing.
  _widget_suppress_depth = MAX(_widget_suppress_depth, 0) + 1;
  (void)file;
  (void)line;
}

void dt_gui_freeze_end_(const char *file, int line)
{
  if(!dt_widget_on_gui_thread()) return;
  if(_widget_suppress_depth <= 0)
  {
    // A bare end with nothing to match: an unbalanced freeze bracket exists. Surface it (with
    // the offending site) instead of letting the counter go negative and silently disable
    // suppression for the rest of the session.
    fprintf(stderr, "[dt_gui_freeze] unbalanced end at %s:%d (depth was %d); "
                    "look for a freeze begin without a matching end.\n",
            file, line, _widget_suppress_depth);
    _widget_suppress_depth = 0;
    return;
  }
  _widget_suppress_depth--;
}

void dt_gui_freeze_reset(void)
{
  _widget_suppress_depth = 0;
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
        *delta_x = _reverse_x ? 1 : -1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = _reverse_x ? -1 : 1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = _reverse_y ? 1 : -1;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = _reverse_y ? -1 : 1;
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
      acc_x += _reverse_x ? -event->delta_x / 50. : event->delta_x / 50.;
      acc_y += _reverse_y ? -event->delta_y / 50. : event->delta_y / 50.;
#else
      acc_x += _reverse_x ? -event->delta_x : event->delta_x;
      acc_y += _reverse_y ? -event->delta_y : event->delta_y;
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
#ifdef _DEBUG
void dt_widget_log(const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  vfprintf(stdout, format, ap);
  va_end(ap);
  // Unflushed diagnostics are lost when the process does not exit cleanly, which is exactly
  // the run you were debugging.
  fflush(stdout);
}
#endif

static gboolean _debug_overlays = FALSE;

gboolean dt_widget_debug_overlays(void)
{
  return _debug_overlays;
}

void dt_widget_set_debug_overlays(gboolean enabled)
{
  _debug_overlays = enabled;
}

static dt_widget_stored_int_getter_t _stored_int_getter = NULL;
static dt_widget_stored_int_setter_t _stored_int_setter = NULL;
static dt_widget_stored_bool_getter_t _stored_bool_getter = NULL;
static dt_widget_stored_bool_setter_t _stored_bool_setter = NULL;

void dt_widget_set_storage_handlers(dt_widget_stored_int_getter_t get_int,
                                    dt_widget_stored_int_setter_t set_int,
                                    dt_widget_stored_bool_getter_t get_bool,
                                    dt_widget_stored_bool_setter_t set_bool)
{
  _stored_int_getter = get_int;
  _stored_int_setter = set_int;
  _stored_bool_getter = get_bool;
  _stored_bool_setter = set_bool;
}

gboolean dt_widget_stored_int(const char *key, int *value)
{
  if(!key || !_stored_int_getter) return FALSE;
  return _stored_int_getter(key, value);
}

void dt_widget_store_int(const char *key, int value)
{
  if(key && _stored_int_setter) _stored_int_setter(key, value);
}

gboolean dt_widget_stored_bool(const char *key)
{
  if(!key || !_stored_bool_getter) return FALSE;
  return _stored_bool_getter(key);
}

void dt_widget_store_bool(const char *key, gboolean value)
{
  if(key && _stored_bool_setter) _stored_bool_setter(key, value);
}

static gint _min_panel_width = 350;

gint dt_widget_min_panel_width(void)
{
  return _min_panel_width;
}

void dt_widget_set_min_panel_width(gint width)
{
  _min_panel_width = width;
}

static gboolean _theme_loaded = FALSE;

gboolean dt_widget_theme_loaded(void)
{
  return _theme_loaded;
}

void dt_widget_set_theme_loaded(gboolean loaded)
{
  _theme_loaded = loaded;
}

static dt_widget_refocus_handler_t _refocus_handler = NULL;
static dt_widget_notebook_page_handler_t _notebook_page_handler = NULL;

void dt_widget_set_refocus_handler(dt_widget_refocus_handler_t handler)
{
  _refocus_handler = handler;
}

void dt_widget_refocus(void)
{
  if(_refocus_handler) _refocus_handler();
}

void dt_widget_set_notebook_page_handler(dt_widget_notebook_page_handler_t handler)
{
  _notebook_page_handler = handler;
}

void dt_widget_notebook_page_changed(gpointer owner)
{
  if(_notebook_page_handler) _notebook_page_handler(owner);
}


/* ------------------------------------------------------------------------------------------
 * Theme palette, overlay tint and mouse radius.
 *
 * All three used to be fields of dt_gui_gtk_t, which meant a widget had to reach the
 * application global to find out what colour to paint with or how close a click counts as a
 * hit. They are toolkit state: the application resolves them (from the CSS theme, from user
 * preferences, from the darkroom zoom) and pushes them here, and widgets read them.
 * ------------------------------------------------------------------------------------------ */
static GdkRGBA _colors[DT_GUI_COLOR_LAST] = { { 0.0, 0.0, 0.0, 1.0 } };
static dt_widget_overlay_color_t _overlay = { 1.0, 1.0, 1.0, 0.5 };
static float _mouse_radius = 15.f;
static float _mouse_radius_clamped = 15.f;

GdkRGBA *dt_widget_colors(void)
{
  return _colors;
}

void dt_widget_set_source_rgb(cairo_t *cr, dt_gui_color_t color)
{
  const GdkRGBA bc = _colors[color];
  cairo_set_source_rgb(cr, bc.red, bc.green, bc.blue);
}

void dt_widget_set_source_rgba(cairo_t *cr, dt_gui_color_t color, float opacity_coef)
{
  const GdkRGBA bc = _colors[color];
  cairo_set_source_rgba(cr, bc.red, bc.green, bc.blue, bc.alpha * opacity_coef);
}

const dt_widget_overlay_color_t *dt_widget_overlay_color(void)
{
  return &_overlay;
}

void dt_widget_set_overlay_color(double red, double green, double blue, double contrast)
{
  _overlay.red = red;
  _overlay.green = green;
  _overlay.blue = blue;
  _overlay.contrast = contrast;
}

float dt_widget_mouse_radius(void)
{
  return _mouse_radius;
}

float dt_widget_mouse_radius_clamped(void)
{
  return _mouse_radius_clamped;
}

void dt_widget_set_mouse_radius(float radius, float clamped)
{
  _mouse_radius = radius;
  _mouse_radius_clamped = clamped;
}


/* Scroll deltas as fractions. Same event arithmetic as the discrete-unit pair below, minus
 * the accumulation: callers of this form want the raw smooth-scroll amount. */
gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent *)event)) return FALSE;

  gboolean handled = FALSE;
  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = _reverse_x ? 1.0 : -1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = _reverse_x ? -1.0 : 1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = _reverse_y ? 1.0 : -1.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = _reverse_y ? -1.0 : 1.0;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      if((delta_x && event->delta_x != 0) || (delta_y && event->delta_y != 0))
      {
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
        if(delta_x) *delta_x = _reverse_x ? -event->delta_x / 50. : event->delta_x / 50.;
        if(delta_y) *delta_y = _reverse_y ? -event->delta_y / 50. : event->delta_y / 50.;
#else
        if(delta_x) *delta_x = _reverse_x ? -event->delta_x : event->delta_x;
        if(delta_y) *delta_y = _reverse_y ? -event->delta_y : event->delta_y;
#endif
        handled = TRUE;
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


#ifdef _DEBUG
void dt_gtk_widget_queue_draw_ext(GtkWidget *widget, const char *name, const char *file, const int line)
{
  if(!GTK_IS_WIDGET(widget))
  {
    dt_widget_log("gtk_widget_queue_draw(%s) called with a non-WIDGET or NULL widget at %s:%d (widget=%p)\n",
             name, file, line, widget);
    return;
  }
  else
    dt_widget_log("queueing redraw for `%s` (`%s`) at %s:%d\n",
             name, gtk_widget_get_name(widget), file, line);

  (gtk_widget_queue_draw)(widget);
}

void dt_gtk_toggle_button_set_active_ext(GtkToggleButton *toggle_button, const char *name, const gboolean active,
                                         const char *file, const int line)
{
  if(!GTK_IS_TOGGLE_BUTTON(toggle_button))
  {
    dt_widget_log("gtk_toggle_button_set_active(%s) called with a non-TOGGLE_BUTTON or NULL widget at %s:%d (toggle_button=%p)\n",
             name, file, line, toggle_button);
    return;
  }
  else
    dt_widget_log("setting toggle button `%s` (`%s`) to %s at %s:%d\n",
             name, gtk_widget_get_name(GTK_WIDGET(toggle_button)), active ? "active" : "inactive", file, line);

  (gtk_toggle_button_set_active)(toggle_button, active);
}
#endif

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
    dt_widget_log("[dt_get_system_gui_ppd] can't detect system ppd\n");
    return 1.0f;
  }
  dt_widget_log("[dt_get_system_gui_ppd] system ppd is %f\n", res);
  return res;
}

GdkModifierType dt_key_modifier_state()
{
  guint state = 0;
  GdkWindow *window = gtk_widget_get_window(dt_widget_root_window());
  gdk_device_get_state(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window))), window, NULL, &state);
  return state;

/* FIXME double check correct way of doing this (merge conflict with Input System NG 20210319)
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  return gdk_keymap_get_modifier_state(keymap) & gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_DEFAULT_MOD_MASK);
*/
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
