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

#include "system/screen_metrics.h"
#include "widgets/widget_settings.h"


#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>

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
// dpi/ppd/em are screen hardware facts and live in system/screen_metrics.c, which sits
// below this layer so common/ can read them too. These stay as the toolkit's names for
// them -- 45-odd files use them -- but hold no copy of their own.

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

double dt_widget_dpi_factor(void) { return dt_screen_dpi_factor(); }
void dt_widget_set_dpi_factor(double factor) { dt_screen_set_dpi_factor(factor); }

double dt_widget_ppd(void) { return dt_screen_ppd(); }
void dt_widget_set_ppd(double ppd) { dt_screen_set_ppd(ppd); }

double dt_widget_em_size(void) { return dt_screen_em_size(); }
void dt_widget_set_em_size(double em) { dt_screen_set_em_size(em); }

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

static inline gboolean _on_gui_thread(void)
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
  if(!_on_gui_thread()) return;
  // MAX(.,0) heals any pre-existing negative drift so the depth is always genuinely suppressing.
  _widget_suppress_depth = MAX(_widget_suppress_depth, 0) + 1;
  (void)file;
  (void)line;
}

void dt_gui_freeze_end_(const char *file, int line)
{
  if(!_on_gui_thread()) return;
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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
