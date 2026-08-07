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

#include "widgets/widget_style.h"

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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
