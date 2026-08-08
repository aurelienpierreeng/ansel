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

#ifndef DT_WIDGETS_DIALOG_H
#define DT_WIDGETS_DIALOG_H

/* Modal dialogs that can run before the application GUI exists.
 *
 * These only require gtk_init(), not a built main window, which is what makes them usable
 * during startup -- picking a library file, confirming a database upgrade. They anchor to
 * whatever the host registered as its root window, or to nothing at all if it has none yet. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

// show a dialog box with 2 buttons in case some user interaction is required BEFORE the gui is
// initialised. This expects gtk_init() to have been called already, which is the case during
// most of the init phase.
gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text);

// same as above, but with 3 buttons: returns 0 for first_text, 1 for second_text, 2 for third_text.
int dt_gui_show_standalone_three_choice_dialog(const char *title, const char *markup, const char *first_text,
                                               const char *second_text, const char *third_text);

// similar to the one above. this one asks the user for some string. the hint is shown in the
// empty entry box
char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text);

// Explicitly return keyboard focus to a just-closed modal/dialog window's parent, falling back
// to the root window if @p parent is NULL or not a window. Call right after destroying a dialog
// (gtk_widget_destroy / gtk_dialog_run's caller): the transient-for hint is not enough to get
// focus back reliably on every platform (macOS/quartz in particular).
void dt_gui_refocus_parent(GtkWindow *parent);

G_END_DECLS

#endif // DT_WIDGETS_DIALOG_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
