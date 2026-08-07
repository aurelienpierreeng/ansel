/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_WIDGETS_WIDGET_SETTINGS_H
#define DT_WIDGETS_WIDGET_SETTINGS_H

#include <gtk/gtk.h>
#include <pthread.h>

/* Toolkit-wide state that widgets need and the application merely configures.
 *
 * These three used to be fields of dt_gui_gtk_t, which meant a widget had to reach the
 * application global to read its own event mask. They are not application data -- a scroll
 * mask, a "who owns scroll right now" register and a cairo filter are properties of the
 * widget toolkit. Ownership moves here so widgets/ needs nothing from gui/.
 *
 * The application sets them once during GUI init; everything else reads them.
 */

/** Event mask widgets must add to receive scroll events. Set once at GUI init. */
GdkEventMask dt_widget_scroll_mask(void);
void dt_widget_set_scroll_mask(GdkEventMask mask);

/* Which widget currently owns scroll input.
 *
 * Ansel routes scroll to one widget at a time rather than letting it propagate: a slider
 * under the pointer takes the wheel, and views clear the register when they change. */
GtkWidget *dt_widget_scroll_focus(void);
void dt_widget_set_scroll_focus(GtkWidget *widget);

/** Cairo filter used when scaling images outside the darkroom. Set once at GUI init. */
cairo_filter_t dt_widget_image_filter(void);
void dt_widget_set_image_filter(cairo_filter_t filter);


/* Widget-update suppression.
 *
 * Programmatic widget updates must not be mistaken for user input. Code wraps such updates
 * in dt_gui_freeze_begin()/end() and every widget callback opens with
 * `if(dt_gui_widgets_suppressed()) return;`.
 *
 * The depth counter used to live in dt_gui_gtk_t, which meant a widget had to reach the
 * application global to find out whether it should ignore its own callback. */
gboolean dt_gui_widgets_suppressed(void);

/** Register the thread that owns widget state. Freeze/unfreeze is a deliberate no-op on any
 *  other thread -- worker-thread reload_defaults has no widgets to suppress, and a concurrent
 *  non-atomic ++/-- would drift the depth and break suppression for the GUI thread.
 *  Until this is called, freezing is inert. */
void dt_widget_set_gui_thread(pthread_t thread);
void dt_gui_freeze_begin_(const char *file, int line);
void dt_gui_freeze_end_(const char *file, int line);
void dt_gui_freeze_reset(void); // hard-reset depth to 0 (GUI init only)

/* Scroll deltas in discrete units, accumulating smooth-scroll fractions and discarding
 * pointer-emulated duplicates. Pure GTK event arithmetic. */
gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y);
gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta);

/** Whether each scroll axis is inverted. A user preference, supplied by the application --
 *  a widget does not read configuration. */
void dt_widget_set_scroll_reversed(gboolean reverse_x, gboolean reverse_y);

/* Colour-label slots. These mirror the application's dt_colorlabels_enum, and gui/gtk.c
 * carries a _Static_assert that they cannot drift apart -- that is the one place both
 * headers are visible. Declaring them here keeps widgets/ free of application headers. */
enum
{
  DT_WIDGET_COLORLABEL_RED = 0,
  DT_WIDGET_COLORLABEL_YELLOW,
  DT_WIDGET_COLORLABEL_GREEN,
  DT_WIDGET_COLORLABEL_BLUE,
  DT_WIDGET_COLORLABEL_PURPLE,
  DT_WIDGET_COLORLABEL_COUNT
};

/* The colour-label palette, as RGBA. Widgets paint colour labels; which colours those are is
 * a theme decision the application supplies. Indices match dt_colorlabels_enum. */
const GdkRGBA *dt_widget_colorlabel(int index);
void dt_widget_set_colorlabels(const GdkRGBA *labels, int count);

#endif // DT_WIDGETS_WIDGET_SETTINGS_H
