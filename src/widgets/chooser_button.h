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
 * GtkColorButton and GtkFontButton exist, and are not used. Their dialogs cannot be relied on to
 * be modal, so a chooser left open lingers over whatever the button edits and writes its answer
 * to whatever that has become by the time it is closed. GtkFontButton cannot ellipsise its label,
 * so a long family name makes the row it sits in as wide as the name. And GtkColorChooser shows a
 * palette first and hides the precise colour, opacity included, behind "Custom".
 *
 * A colour button opens a colour well (widgets/color_well.h) in a small window of its own, next
 * to the button: the recent colours, a field, a hue and an opacity strip and the hexadecimal
 * number, all at once, and every change on the canvas while it is dragged. The window is modal,
 * and closes on Escape, on its close button, on a click outside it or when the application loses
 * the focus. A font button opens GtkFontChooserDialog, modal too.
 *
 * The button shows what it is told. A font pick is REPORTED, through the callback, and never shown
 * by the button itself: the caller applies it, and then tells the button what the edited thing now
 * holds -- which is the pick, or is not, when the edit was refused or changed nothing. A colour
 * window reports while it is open instead, and the button shows each colour it reports, since the
 * caller shows it too: see dt_chooser_color_phase_t. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * How a colour window reports. One opening of the window is ONE gesture, however many drags it
 * holds, so a caller keeps a single undo step for it, and Escape can take it back whole.
 */
typedef enum dt_chooser_color_phase_t
{
  /** The colour changed while the window is open: a drag, a recent colour, a typed number. */
  DT_CHOOSER_COLOR_LIVE = 0,
  /** The window closed keeping a colour other than the one it opened with. Once, after LIVE ones. */
  DT_CHOOSER_COLOR_COMMIT,
  /**
   * The window closed giving the colour back -- Escape, dt_chooser_button_close(), or a colour
   * dragged back to where it started -- after LIVE changes were reported. Carries the colour it
   * opened with, which the caller puts back without recording anything.
   */
  DT_CHOOSER_COLOR_CANCEL,
} dt_chooser_color_phase_t;

/** A colour window changed the colour, closed keeping it, or closed giving it back. */
typedef void (*dt_chooser_color_changed_t)(GtkWidget *button, const GdkRGBA *color, dt_chooser_color_phase_t phase,
                                           gpointer user_data);

/**
 * A font was chosen and the dialog confirmed. @p font is a Pango font description of the family
 * and the style ONLY: the size is left unset, since a chooser that offers a face says nothing
 * about the size the text is set at, and choosing a face must not change it.
 */
typedef void (*dt_chooser_font_picked_t)(GtkWidget *button, const char *font, gpointer user_data);

/**
 * @brief A button painted with a colour swatch, opening a modal colour well next to it.
 *
 * A colour that is not opaque is painted over a checkerboard, so that its transparency shows.
 *
 * @param title shown at the top of the window.
 * @param use_alpha whether the window offers the opacity; without it every colour reported is opaque.
 * @param history_key the stored string of recent colours the well reads and writes, shared by every
 * button naming the same key; NULL keeps none.
 * @param changed called as dt_chooser_color_phase_t says, never while nothing changed.
 * @param user_data handed back to @p changed.
 */
GtkWidget *dt_chooser_button_color_new(const char *title, gboolean use_alpha, const char *history_key,
                                       dt_chooser_color_changed_t changed, gpointer user_data);

/**
 * @brief Show a colour on the button, and open the window on it next time. Reports nothing. While
 * the window is open and has reported a change, the colour it holds wins and this is ignored.
 */
void dt_chooser_button_set_color(GtkWidget *button, const GdkRGBA *color);

/** @brief The colour the button shows: the colour last set, or the last one its window reported. */
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
 * @brief The window the dialog or the colour window is kept above.
 *
 * Unset, or once that window is gone, it is transient for the button's own toplevel, and failing
 * that for the host's root window.
 */
void dt_chooser_button_set_parent(GtkWidget *button, GtkWindow *parent);

/** @brief Whether the button's dialog or colour window is open. */
gboolean dt_chooser_button_is_open(GtkWidget *button);

/**
 * @brief Close the button's dialog or colour window, if it is open.
 *
 * For a caller whose target changed or went away while it was open. A font dialog reports nothing:
 * its pick belongs to something no longer edited. A colour window that reported LIVE changes
 * reports CANCEL, so a caller that has not ended the gesture itself puts the colour back. Destroying
 * the button closes either the same way, but reports nothing at all.
 */
void dt_chooser_button_close(GtkWidget *button);

G_END_DECLS

#endif // DT_WIDGETS_CHOOSER_BUTTON_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
