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

#include "widgets/label.h"

#include "common/macros.h"   // IS_NULL_PTR
#include "widgets/widget_settings.h"   // DT_PIXEL_APPLY_DPI

void dt_gui_set_symbolic_icon(GtkWidget *image, const char *icon_name, GtkIconSize size, const GdkRGBA *color)
{
  gint width = 16, height = 16;
  gtk_icon_size_lookup(size, &width, &height);
  // Icon themes only look up square icons: request the larger dimension,
  // then scale the result down to the exact width/height below if the two differ.
  GtkIconInfo *info = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_default(), icon_name, MAX(width, height),
                                                 GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
  if(IS_NULL_PTR(info))
  {
    gtk_image_set_from_icon_name(GTK_IMAGE(image), icon_name, size);
    return;
  }

  GdkPixbuf *pixbuf = IS_NULL_PTR(color)
      ? gtk_icon_info_load_symbolic_for_context(info, gtk_widget_get_style_context(image), NULL, NULL)
      : gtk_icon_info_load_symbolic(info, color, color, color, color, NULL, NULL);
  g_object_unref(info);

  if(!IS_NULL_PTR(pixbuf))
  {
    if(gdk_pixbuf_get_width(pixbuf) != width || gdk_pixbuf_get_height(pixbuf) != height)
    {
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, width, height, GDK_INTERP_BILINEAR);
      g_object_unref(pixbuf);
      pixbuf = scaled;
    }
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
    g_object_unref(pixbuf);
  }
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

void dt_gui_textview_set_padding(GtkTextView *textview)
{
  if(!GTK_IS_TEXT_VIEW(textview)) return;

  gtk_text_view_set_left_margin(textview, DT_PIXEL_APPLY_DPI(4));
  gtk_text_view_set_right_margin(textview, DT_PIXEL_APPLY_DPI(4));
  gtk_text_view_set_top_margin(textview, DT_PIXEL_APPLY_DPI(2));
  gtk_text_view_set_bottom_margin(textview, DT_PIXEL_APPLY_DPI(2));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
