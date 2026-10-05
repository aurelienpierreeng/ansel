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
#include "widgets/widget_settings.h"
#include "system/macros.h"   // IS_NULL_PTR -- a pure macro header, no state

#include <pango/pangocairo.h>

#include <glib.h>
#include <string.h>

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
  if(!fo)
  {
    GtkWidget *root = dt_widget_root_window();
    if(root)
    {
      PangoContext *pc = gtk_widget_get_pango_context(root);
      if(pc) fo = pango_cairo_context_get_font_options(pc);
    }
  }
  if(!fo)
  {
    GdkScreen *screen = gdk_screen_get_default();
    if(screen) fo = gdk_screen_get_font_options(screen);
  }

  // cairo_set_font_options() copies internally, so the const pointer's lifetime is not a concern.
  if(fo) cairo_set_font_options(cr, fo);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
