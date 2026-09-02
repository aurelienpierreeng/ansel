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

#ifndef DT_WIDGETS_LABEL_H
#define DT_WIDGETS_LABEL_H

/* Text presentation: the two standard label shapes, and the small fixes GTK needs to render
 * text the way ansel.css asks for.
 *
 * Section labels and plain labels differ only in alignment and capitalisation, but they were
 * hand-rolled at ~55 call sites before this pair existed, which is why some panels centred
 * their headings and others did not. */

/* Only what the declarations below need: GtkWidget, gchar. A header that includes anything
 * else becomes a supply line its consumers do not know they are using, and the breakage
 * surfaces somewhere else entirely the day someone tidies it up. */
#include <gtk/gtk.h>

G_BEGIN_DECLS

/** Remove the underscores GTK reads as mnemonic markers from a label.
 *  Newly-allocated; the caller frees it. */
gchar *delete_underscore(const char *s);

/**
 * @brief Remove Pango/Gtk markup and accel mnemonics from a text label.
 * If markup parsing fails, fall back to a copy of the original string.
 *
 * @param s Original string to clean
 * @return gchar* Newly-allocated string. The caller is responsible for freeing it.
 */
gchar *strip_markup(const char *s);

/** Turn an existing label into a section heading: full width, centred, ellipsized. */
void dt_ui_section_label_set(GtkWidget *label);

/** A section heading. Capitalised: grammar says sentences start with a capital, and typography
 * says it makes the structure of the text easier to pick out. */
GtkWidget *dt_ui_section_label_new(const gchar *str);

/** A plain label: start-aligned, capitalised, ellipsized. */
GtkWidget *dt_ui_label_new(const gchar *str);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

/**
 * @brief Set a symbolic icon on an image widget, optionally forcing a specific color.
 *
 * gtk_image_set_from_icon_name() colors symbolic icons from the current CSS "color", but
 * ansel.css's main theme provider is loaded at GTK_STYLE_PROVIDER_PRIORITY_USER + 1, which
 * outranks any per-widget provider added at the more common
 * GTK_STYLE_PROVIDER_PRIORITY_APPLICATION and silently wins the cascade. Loading the icon as a
 * pre-tinted pixbuf via GtkIconInfo sidesteps CSS entirely, so the requested color always wins.
 * Pass color = NULL for the normal (untinted, theme-foreground) rendering.
 */
void dt_gui_set_symbolic_icon(GtkWidget *image, const char *icon_name, GtkIconSize size, const GdkRGBA *color);

/**
 * @brief Load a symbolic icon as a pre-tinted pixbuf, at the pixel size `size` names.
 *
 * The pixbuf half of dt_gui_set_symbolic_icon(), for the callers that need the pixbuf itself
 * rather than an image widget -- a GtkCellRendererPixbuf, say, which has no colour of its own
 * and takes a "pixbuf" property instead. Tinted with `color` when one is given, and with
 * `context`'s own foreground otherwise (`context` is unused when `color` is given).
 *
 * @return GdkPixbuf* owned by the caller, or NULL if the theme has no such icon -- in which case
 * the caller should name the icon and let GTK draw it.
 */
GdkPixbuf *dt_gui_symbolic_icon_pixbuf(const char *icon_name, GtkIconSize size, const GdkRGBA *color,
                                       GtkStyleContext *context);

/**
 * @brief Apply the standard recessed-input text padding to a GtkTextView.
 *
 * CSS padding on the textview "text" node is parsed but ignored for layout in GTK3, so the
 * 2px/4px inset matching `entry`/`treeview` (see data/themes/.css) has to be set on the
 * widget itself.
 *
 * @param textview The GtkTextView to update.
 */
void dt_gui_textview_set_padding(GtkTextView *textview);

G_END_DECLS

#endif // DT_WIDGETS_LABEL_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
