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

#ifndef DT_WIDGETS_CHOOSER_BUTTON_H
#define DT_WIDGETS_CHOOSER_BUTTON_H

/* A flat button showing a colour or a font, which opens a MODAL chooser when clicked.
 *
 * GtkColorButton and GtkFontButton exist, and are not used for two reasons. Their dialogs cannot be
 * relied on to be modal, so a chooser left open lingers over whatever the button edits and writes
 * its answer to whatever that has become by the time it is closed. And GtkFontButton cannot
 * ellipsise its label, so a long family name makes the row it sits in as wide as the name.
 *
 * The button shows what it is told and nothing else. A pick is REPORTED, through the callback, and
 * never shown by the button itself: the caller applies it, and then tells the button what the
 * edited thing now holds -- which is the pick, or is not, when the edit was refused or changed
 * nothing. A button that displayed its own last pick would be a second copy of the value to go
 * stale. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

/** A colour was chosen and the dialog confirmed. */
typedef void (*dt_chooser_color_picked_t)(GtkWidget *button, const GdkRGBA *color, gpointer user_data);

/**
 * A font was chosen and the dialog confirmed. @p font is a Pango font description of the family
 * and the style ONLY: the size is left unset, since a chooser that offers a face says nothing
 * about the size the text is set at, and choosing a face must not change it.
 */
typedef void (*dt_chooser_font_picked_t)(GtkWidget *button, const char *font, gpointer user_data);

/**
 * @brief A button painted with a colour swatch, opening a modal GtkColorChooserDialog.
 *
 * A colour that is not opaque is painted over a checkerboard, so that its transparency shows.
 *
 * @param title the dialog's title.
 * @param use_alpha whether the dialog offers the alpha channel.
 * @param picked called once per confirmed dialog, never on cancel or close.
 * @param user_data handed back to @p picked.
 */
GtkWidget *dt_chooser_button_color_new(const char *title, gboolean use_alpha, dt_chooser_color_picked_t picked,
                                       gpointer user_data);

/** @brief Show a colour on the button, and offer it first the next time the dialog opens. Reports nothing. */
void dt_chooser_button_set_color(GtkWidget *button, const GdkRGBA *color);

/** @brief The colour the button was last told to show. */
void dt_chooser_button_get_color(GtkWidget *button, GdkRGBA *color);

/**
 * @brief A button labelled with a font's family and style, opening a modal GtkFontChooserDialog.
 *
 * The dialog offers the family and the style, not the size. The label is exactly @p label_chars
 * characters wide and ellipsised past that, so the button's width does not depend on the name it
 * shows: the row it sits in keeps its width whichever font it is refilled with.
 *
 * @param title the dialog's title.
 * @param label_chars the label's width, in characters.
 * @param picked called once per confirmed dialog, never on cancel or close.
 * @param user_data handed back to @p picked.
 */
GtkWidget *dt_chooser_button_font_new(const char *title, int label_chars, dt_chooser_font_picked_t picked,
                                      gpointer user_data);

/**
 * @brief Show a font on the button, and offer it first the next time the dialog opens. Reports nothing.
 * @param font a Pango font description; its size, if any, is not shown. NULL or empty shows nothing.
 */
void dt_chooser_button_set_font(GtkWidget *button, const char *font);

/** @brief The font description the button was last told to show, never NULL. Owned by the button. */
const char *dt_chooser_button_get_font(GtkWidget *button);

/** @brief The label a font button shows its font in, for the caller's styling. NULL for a colour button. */
GtkWidget *dt_chooser_button_get_label(GtkWidget *button);

/**
 * @brief The window the dialog is kept above.
 *
 * Unset, or once that window is gone, the dialog is transient for the button's own toplevel, and
 * failing that for the host's root window.
 */
void dt_chooser_button_set_parent(GtkWidget *button, GtkWindow *parent);

/** @brief Whether the button's dialog is open. */
gboolean dt_chooser_button_is_open(GtkWidget *button);

/**
 * @brief Close the button's dialog, if it is open, reporting nothing.
 *
 * For a caller whose target changed or went away while the dialog was open: the pick it would have
 * reported belongs to something that is no longer being edited. Destroying the button closes its
 * dialog the same way.
 */
void dt_chooser_button_close(GtkWidget *button);

G_END_DECLS

#endif // DT_WIDGETS_CHOOSER_BUTTON_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
