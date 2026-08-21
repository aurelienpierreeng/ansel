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

#ifndef DT_WIDGETS_NOTEBOOK_H
#define DT_WIDGETS_NOTEBOOK_H

/* GtkNotebook with tabs that size themselves to the available width, and a page-switch
 * notification the host can subscribe to. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkNotebook *dt_ui_notebook_new(void);

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip);

/**
 * @brief Register an opaque owner for a GtkNotebook's page switches, and report every
 * "switch_page" to the host through dt_widget_notebook_page_changed().
 *
 * This layer does not know or care what @p owner is: it is carried through as-is. Any
 * interested listener (e.g. the color picker, which resets a picker left active on a page the
 * user just switched away from) casts the payload back to whatever type it registered here.
 * Works on any GtkNotebook, whether created via dt_ui_notebook_new() or gtk_notebook_new().
 */
void dt_ui_notebook_set_picker_owner(GtkNotebook *notebook, gpointer owner);

G_END_DECLS

#endif // DT_WIDGETS_NOTEBOOK_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
