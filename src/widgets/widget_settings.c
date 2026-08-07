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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
