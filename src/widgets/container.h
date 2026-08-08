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

#ifndef DT_WIDGETS_CONTAINER_H
#define DT_WIDGETS_CONTAINER_H

/* Counting and clearing the children a GtkContainer was given.
 *
 * gtk_container_get_children() allocates a list every time, which is three lines of
 * boilerplate and a leak waiting to happen at each of the ~40 places that just wanted to
 * know whether a box has anything in it. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

// check whether the given container has any user-added children
gboolean dt_gui_container_has_children(GtkContainer *container);
// return a count of the user-added children in the given container
int dt_gui_container_num_children(GtkContainer *container);
// return the first child of the given container
GtkWidget *dt_gui_container_first_child(GtkContainer *container);
// return the requested child of the given container, or NULL if it has fewer children
GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which);

// remove all of the children we've added to the container.  Any which no longer have any
// references will be destroyed.
void dt_gui_container_remove_children(GtkContainer *container);

// delete all of the children we've added to the container.  Use this function only if you are
// SURE there are no other references to any of the children (if in doubt, use
// dt_gui_container_remove_children instead; it's a bit slower but safer).
void dt_gui_container_destroy_children(GtkContainer *container);

G_END_DECLS

#endif // DT_WIDGETS_CONTAINER_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
