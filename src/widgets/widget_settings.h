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

#include "system/screen_metrics.h"

G_BEGIN_DECLS

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

/* The host's root window, used for things that exist before any widget does: resolving theme
 * colours at toolkit init, and parenting a popup so Wayland compositors place it correctly.
 * Unregistered: NULL, and callers fall back to screen defaults. */
typedef GtkWidget *(*dt_widget_root_window_handler_t)(void);
void dt_widget_set_root_window_handler(dt_widget_root_window_handler_t handler);
GtkWidget *dt_widget_root_window(void);

/* How wide the host would like `widget` to be naturally -- it knows which panel the widget
 * sits in and how wide that panel is. Returns -1 when the host has no opinion, which is also
 * what an unregistered handler reports. */
typedef gint (*dt_widget_natural_width_handler_t)(GtkWidget *widget);
void dt_widget_set_natural_width_handler(dt_widget_natural_width_handler_t handler);
gint dt_widget_natural_width(GtkWidget *widget);

/* Transient user-facing message ("that widget no longer exists"). How and where it is shown
 * is the host's decision -- a toast, a status line, or nothing at all. Unregistered, the
 * message is dropped. */
typedef void (*dt_widget_message_handler_t)(const char *message);
void dt_widget_set_message_handler(dt_widget_message_handler_t handler);
void dt_widget_message(const char *message);

/* Pointer shape during widget interaction (a panel-handle drag wants a resize cursor).
 * The host owns the window whose cursor changes, so it supplies the setter. Unregistered,
 * the cursor is left alone. */
typedef void (*dt_widget_cursor_handler_t)(GdkCursorType cursor);
void dt_widget_set_cursor_handler(dt_widget_cursor_handler_t handler);
void dt_widget_set_cursor(GdkCursorType cursor);

/* Toolkit metrics: the UI zoom factor, the integer device scale, and the resolved root font
 * size in pixels. Widgets scale themselves by these; the application computes them from the
 * screen and the theme and pushes them here. They lived in dt_gui_gtk_t, which meant a widget
 * had to reach the application global to size itself. */
double dt_widget_dpi_factor(void);
void dt_widget_set_dpi_factor(double factor);

double dt_widget_ppd(void);
void dt_widget_set_ppd(double ppd);

/** Resolved root font size in px; 16.0 until the application resolves it. */
double dt_widget_em_size(void);
void dt_widget_set_em_size(double em);

/* Scale a 96-DPI-baseline value. UI: logical pixels GTK will scale further. DEVICE: raw
 * device pixels for cairo surfaces and hit-tests, which GTK does not scale for us. */
#define DT_UI_SCALE_UI(value) ((value) * dt_widget_dpi_factor())
#define DT_UI_SCALE_DEVICE(value) ((value) * dt_widget_dpi_factor() * dt_widget_ppd())
#define DT_PIXEL_APPLY_DPI(value) DT_UI_SCALE_UI(value)
#define DT_PIXEL_APPLY_DPI_DPP(value) DT_UI_SCALE_DEVICE(value)

/* Gutter between children of boxes/grids/flowboxes -- settable only from code, so it is
 * centralised here for the whole app. Expressed as a fraction of 1em so it tracks the user's
 * font size like the em-based margins in ansel.css. 0.625em == 10px at the 16px reference.
 * The font's pt->px conversion already folds in DPI, so this needs no DPI scaling on top. */
#define DT_GUI_EM_SIZE ((gint)dt_widget_em_size())
#define DT_GUI_BOX_SPACING_EM 0.625
#define DT_GUI_BOX_SPACING ((gint)(DT_GUI_EM_SIZE * DT_GUI_BOX_SPACING_EM + 0.5))

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

/* Does `state` carry exactly `desired_modifier_mask`, ignoring lock/scroll bits? */
static inline gboolean dt_modifier_is(const GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in desired_modifier_mask to be the bit for the Cmd key
  return (state & modifiers) == desired_modifier_mask;
}

/* dt_cairo_image_surface_create{,_for_data}() moved to system/screen_metrics.h with the ppd
 * they scale by, so code below this layer can build a device-scaled surface too. This header
 * includes it, so the ~45 files using them by these names are unaffected. */

G_END_DECLS

#endif // DT_WIDGETS_WIDGET_SETTINGS_H
