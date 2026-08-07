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


#ifndef DT_WIDGETS_WIDGET_STYLE_H
#define DT_WIDGETS_WIDGET_STYLE_H

#include <gtk/gtk.h>

/* CSS class helpers. Pure GTK style-context manipulation with a redraw request -- no
 * application involved, which is why they belong here rather than in gui/gtk.c where they
 * used to live. gui/gtk.h includes this, so existing consumers are unaffected. */

/** Add `class_name` to `widget`'s style context if absent, and queue a redraw. */
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name);

/** Remove `class_name` from `widget`'s style context if present, and queue a redraw. */
void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name);

#endif // DT_WIDGETS_WIDGET_STYLE_H
