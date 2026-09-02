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

#include "common/glib_utils.h"     // dt_string_replace
#include "system/macros.h"         // IS_NULL_PTR
#include "system/mem_alloc.h"      // dt_free
#include "widgets/widget_style.h"  // dt_gui_add_class, dt_capitalize_label

#include "system/macros.h"   // IS_NULL_PTR
#include "widgets/widget_settings.h"   // DT_PIXEL_APPLY_DPI

GdkPixbuf *dt_gui_symbolic_icon_pixbuf(const char *icon_name, GtkIconSize size, const GdkRGBA *color,
                                       GtkStyleContext *context)
{
  gint width = 16, height = 16;
  gtk_icon_size_lookup(size, &width, &height);
  // Icon themes only look up square icons: request the larger dimension,
  // then scale the result down to the exact width/height below if the two differ.
  GtkIconInfo *info = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_default(), icon_name, MAX(width, height),
                                                 GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
  if(IS_NULL_PTR(info)) return NULL;

  GdkPixbuf *pixbuf = IS_NULL_PTR(color)
      ? gtk_icon_info_load_symbolic_for_context(info, context, NULL, NULL)
      : gtk_icon_info_load_symbolic(info, color, color, color, color, NULL, NULL);
  g_object_unref(info);
  if(IS_NULL_PTR(pixbuf)) return NULL;

  if(gdk_pixbuf_get_width(pixbuf) != width || gdk_pixbuf_get_height(pixbuf) != height)
  {
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, width, height, GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    pixbuf = scaled;
  }

  return pixbuf;
}

void dt_gui_set_symbolic_icon(GtkWidget *image, const char *icon_name, GtkIconSize size, const GdkRGBA *color)
{
  GdkPixbuf *pixbuf
      = dt_gui_symbolic_icon_pixbuf(icon_name, size, color, gtk_widget_get_style_context(image));

  // Naming the icon and letting GTK draw it is the fallback for a theme that has no symbolic
  // variant of it -- and now also for one whose variant failed to load, which used to leave the
  // image showing nothing at all.
  if(IS_NULL_PTR(pixbuf))
  {
    gtk_image_set_from_icon_name(GTK_IMAGE(image), icon_name, size);
    return;
  }

  gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
  g_object_unref(pixbuf);
}

/** Remove the underscores GTK reads as mnemonic markers from a label. */
gchar *delete_underscore(const char *s)
{
  return dt_string_replace(s, "_");
}

/**
 * @brief Remove Pango/Gtk markup and accel mnemonics from a text label.
 * If markup parsing fails, fall back to a copy of the original string.
 *
 * @param s Original string to clean
 * @return gchar* Newly-allocated string. The caller is responsible for freeing it.
 */
gchar *strip_markup(const char *s)
{
  if(IS_NULL_PTR(s)) return g_strdup("");

  PangoAttrList *attrs = NULL;
  gchar *plain = NULL;

  const gchar *underscore = "_";
  gunichar mnemonic = underscore[0];
  if(!pango_parse_markup(s, -1, mnemonic, &attrs, &plain, NULL, NULL))
    plain = delete_underscore(s);

  pango_attr_list_unref(attrs);
  return plain;
}

/** Turn an existing label into a section heading: full width, centred, ellipsized. */
void dt_ui_section_label_set(GtkWidget *label)
{
  gtk_widget_set_halign(label, GTK_ALIGN_FILL); // make it span the whole available width
  gtk_label_set_xalign (GTK_LABEL(label), 0.5f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); // ellipsize labels
  dt_gui_add_class(label, "dt_section_label"); // make sure that we can style these easily
}

/** A section heading. Capitalised: grammar says sentences start with a capital, and typography
 * says it makes the structure of the text easier to pick out. */
GtkWidget *dt_ui_section_label_new(const gchar *str)
{
  gchar *str_cpy = g_strdup(str);
  dt_capitalize_label(str_cpy);
  GtkWidget *label = gtk_label_new(str_cpy);
  dt_free(str_cpy);
  dt_ui_section_label_set(label);
  return label;
}

/** A plain label: start-aligned, capitalised, ellipsized. */
GtkWidget *dt_ui_label_new(const gchar *str)
{
  gchar *str_cpy = g_strdup(str);
  dt_capitalize_label(str_cpy);
  GtkWidget *label = gtk_label_new(str_cpy);
  dt_free(str_cpy);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  return label;
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
