/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_VIEWS_CANVAS_PROPS_GTK_H
#define DT_VIEWS_CANVAS_PROPS_GTK_H

/**
 * @file canvas_props_gtk.h
 * @brief The GTK face of the canvas property table: one strip that can grow a card.
 *
 * @details Every row is built ONCE, from `dt_canvas_props()`, for every kind at the same time, and
 * a refill only shows, hides and fills what is there. The same nature of property is the same
 * control wherever it sits -- a spin button for an exact number, a slider for a perceptual one --
 * so a property never changes shape between the strip and the card, or between two kinds.
 *
 * The widget holds no state of the document's. Every change goes to the host through
 * `dt_canvas_props_host_t.edit`, tagged with the phase of the gesture it belongs to, and the host
 * refills afterwards; a refill never reaches the host back, because every handler it could wake
 * is blocked while it writes. Nothing here reads or writes the configuration either: which
 * section a kind opens, and whether the card was open, are the host's to remember.
 *
 * Placement is not decided here. The host measures the widget (`dt_canvas_props_gtk_measure()`),
 * solves where it goes, and tells it which side the card grows on and how tall it may get
 * (`dt_canvas_props_gtk_set_card()`); the card never shows or hides on its own, and its button
 * shows what the host last said.
 *
 * The host's side of the bargain, `dt_canvas_props_host_t`, is declared with the property table
 * in canvas/canvas_props.h: nothing in it is GTK's.
 */

#include "canvas/canvas_props.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct dt_canvas_props_gtk_t dt_canvas_props_gtk_t;

/**
 * @brief Build the widget, every kind's rows included, hidden.
 * @param host copied; its `data` must outlive the widget.
 */
dt_canvas_props_gtk_t *dt_canvas_props_gtk_new(const dt_canvas_props_host_t *host);

/**
 * @brief The widget to put on screen: an event box with a window of its own, so crossings between
 * its controls read as inferior ones, that keeps plain keys for its controls while one has the focus.
 * Show it with gtk_widget_show(). A gtk_widget_show_all() from outside -- a window showing
 * everything in it -- is harmless: every row, section and the card are kept out of it, and only a
 * refill and dt_canvas_props_gtk_set_card() show them.
 */
GtkWidget *dt_canvas_props_gtk_root(dt_canvas_props_gtk_t *props);

/**
 * @brief Show one object's properties, or none.
 * @details The rows that apply are shown and those that do not are hidden when the object, its
 * kind or anything a row depends on changed; the values are then written into every control but
 * the one being edited, with its handlers blocked, so nothing reaches the host back. A new object
 * closes any colour or font dialog still open for the previous one, and forgets any gesture in
 * flight without reporting it: the host commits what it has pending BEFORE it shows another object.
 * A drag still held on a control when that happens reports nothing more, and the control goes back
 * to the new object's value when the button comes up.
 * @param object NULL forgets the object the same way and writes nothing; the rows keep what they
 * showed, so the host hides the widget rather than showing it empty.
 */
void dt_canvas_props_gtk_refill(dt_canvas_props_gtk_t *props, const dt_canvas_t *canvas,
                                const dt_canvas_object_t *object);

/**
 * @brief Measure the widget at a width, CSS boxes included.
 * @param width the width it will be given, dt_canvas_props_gtk_strip_width() or less
 * @param strip_height the whole widget's height with no card shown, frame included
 * @param card_content_height the height the card adds when shown whole, scrolling nothing
 *
 * The card's height is to be taken from here and handed to dt_canvas_props_gtk_set_card() whenever
 * its content may have changed -- a section opened or folded, a refill -- and never read back from
 * the widget's own natural height: a scrolled window's natural height is not height-for-width, so
 * a card holding wrapped text or a flow box asks for its content's height at its NARROWEST, which
 * is taller than it is at the width it is given. Only the cap set_card() leaves keeps that out of
 * the allocation.
 */
void dt_canvas_props_gtk_measure(dt_canvas_props_gtk_t *props, int width, int *strip_height,
                                 int *card_content_height);

/** @brief The width to give the widget, strip and card alike: the strip's natural width, never below a floor. */
int dt_canvas_props_gtk_strip_width(dt_canvas_props_gtk_t *props);

/**
 * @brief Attach the card, or take it away.
 * @param shown whether the card is on screen
 * @param grow_up TRUE puts the card above the strip, FALSE below it
 * @param max_height the most the card is given, frame and borders included; it scrolls past that.
 * A card is never shorter than its own scrollbar: given less than that, it is not shown, and its
 * button reads as clipped.
 * @param clipped the user asked for the card and there was no room for it: the card button says so
 *
 * The card button is set to whether the card is shown, reporting nothing, so a click on it always
 * asks for the opposite of what is on screen: a clipped card's button is up, and a click asks for
 * the card anyway.
 */
void dt_canvas_props_gtk_set_card(dt_canvas_props_gtk_t *props, gboolean shown, gboolean grow_up, int max_height,
                                  gboolean clipped);

/** @brief Open one section of the card, folding the others, as the host restores it; -1 folds all. Tells nobody. */
void dt_canvas_props_gtk_set_section(dt_canvas_props_gtk_t *props, int section);

/**
 * @brief Whether the card button is pressed: the card is shown, or the user has just asked for it
 * and the host has not answered with dt_canvas_props_gtk_set_card() yet.
 */
gboolean dt_canvas_props_gtk_card_open(dt_canvas_props_gtk_t *props);

/** @brief Whether the keyboard focus is inside the widget. */
gboolean dt_canvas_props_gtk_focus_inside(dt_canvas_props_gtk_t *props);

/** @brief Give the keyboard focus to the first control of the strip. */
void dt_canvas_props_gtk_focus_first(dt_canvas_props_gtk_t *props);

/** @brief Close any colour or font dialog that is open, reporting nothing. */
void dt_canvas_props_gtk_close_dialogs(dt_canvas_props_gtk_t *props);

/**
 * @brief Destroy the widget and free it. Reports nothing to the host, not even a pending edit:
 * the host commits what it has pending before it lets the properties go.
 */
void dt_canvas_props_gtk_free(dt_canvas_props_gtk_t *props);

G_END_DECLS

#endif // DT_VIEWS_CANVAS_PROPS_GTK_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
