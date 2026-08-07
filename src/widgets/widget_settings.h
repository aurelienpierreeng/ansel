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

#endif // DT_WIDGETS_WIDGET_SETTINGS_H
