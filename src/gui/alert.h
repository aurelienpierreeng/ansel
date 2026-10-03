/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

#ifndef DT_GUI_ALERT_H
#define DT_GUI_ALERT_H

#include <gtk/gtk.h>

/** An alert window: Ansel's small window that says something which must not go unseen, with an
 * icon on the left and, on its right, a bold heading over the lines and buttons the caller adds.
 *
 * Everything about the window itself -- its frame, its icon, which window it stays above, whether
 * it is modal, what Escape and its title bar do, where the focus goes -- is decided here, by the
 * kind it is created with. A caller says what the window tells and what its buttons do, never how
 * the window behaves.
 *
 * Built and used from the GUI thread only, except dt_gui_alert(), which any thread may call. */
typedef struct dt_gui_alert_t dt_gui_alert_t;

typedef enum dt_gui_alert_kind_t
{
  /** Tells something, and lets the user go on working: not modal, kept above the main window.
   * OK, Escape or its title bar close it, and the main window gets the focus back. */
  DT_GUI_ALERT_NOTICE,
  /** Asks something, and nothing else may change until it is answered: modal over the main
   * window. Escape and its title bar answer with the callback of dt_gui_alert_set_cancel(). */
  DT_GUI_ALERT_QUESTION,
  /** Says that something is under way, with a spinner, and goes away when it is done: only its
   * owner closes it. Not attached to the main window, which may be hidden, but centred on the
   * screen; in a window group of its own, so that a grab held over the others leaves it usable. */
  DT_GUI_ALERT_PROGRESS,
} dt_gui_alert_kind_t;

/** What a button, or the cancellation of a question, does. @p data is what was given with it. */
typedef void (*dt_gui_alert_callback_t)(dt_gui_alert_t *alert, void *data);

/** Create an alert, not shown yet: add its lines and buttons, then dt_gui_alert_show() it.
 *
 * @param kind    how the window behaves, see ::dt_gui_alert_kind_t
 * @param title   the window's title, already translated
 * @param heading the bold line at the top, already translated
 * @return the alert, owned by its window: a NOTICE is freed when the user closes it, the other
 *         kinds by dt_gui_alert_destroy() */
dt_gui_alert_t *dt_gui_alert_new(dt_gui_alert_kind_t kind, const char *title, const char *heading);

/** Add a line of text under what is already there, wrapped past 60 characters.
 * @param text plain text, already translated; NULL for a line the caller fills later
 * @return the label, for the caller to update: gtk_label_set_text() or _set_markup() */
GtkWidget *dt_gui_alert_add_text(dt_gui_alert_t *alert, const char *text);

/** Add a widget of the caller's under what is already there -- a list, an expander. The window
 * takes it. */
void dt_gui_alert_add_widget(dt_gui_alert_t *alert, GtkWidget *widget);

/** Add a button at the bottom right, after those already there. The first one added has the
 * focus when the window shows: put first what Enter should do. */
void dt_gui_alert_add_button(dt_gui_alert_t *alert, const char *label, dt_gui_alert_callback_t callback,
                             void *data);

/** What Escape and the title bar of a QUESTION answer: a question cannot be closed without an
 * answer. Ignored by the other kinds. */
void dt_gui_alert_set_cancel(dt_gui_alert_t *alert, dt_gui_alert_callback_t callback, void *data);

/** Show the window, in front, with the focus on its first button. */
void dt_gui_alert_show(dt_gui_alert_t *alert);

/** Close the window and free the alert. The main window does not get the focus back: what comes
 * after a question or a progress -- going back to work, or quitting -- is the caller's to say. */
void dt_gui_alert_destroy(dt_gui_alert_t *alert);

/** Tell the user something that must not go unseen: a NOTICE with a single OK button.
 *
 * A toast goes away by itself; this is for what stays true after it would have -- a module that
 * failed, an image that was not updated. Nothing waits for the answer: the call returns at once,
 * and the rest of the application stays usable while the window is up.
 *
 * One window per title: an alert whose title is already on screen replaces its message, so a
 * failure that repeats does not stack windows.
 *
 * Any thread: the window is built later, from the GUI thread's main loop, out of copies of @p title
 * and @p message -- never inside the call, so a caller holding a lock may alert.
 *
 * @param title   the window's title and the bold line at the top, already translated
 * @param message the text under it, already translated; plain text, not markup
 */
void dt_gui_alert(const char *title, const char *message);

#endif // DT_GUI_ALERT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
