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

#include "widgets/container.h"

#include "system/macros.h"   // IS_NULL_PTR

gboolean dt_gui_container_has_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  gboolean has_children = !IS_NULL_PTR(children);
  g_list_free(children);
  children = NULL;
  return has_children;
}

int dt_gui_container_num_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  int num_children = g_list_length(children);
  g_list_free(children);
  children = NULL;
  return num_children;
}

GtkWidget *dt_gui_container_first_child(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = children ? (GtkWidget*)children->data : NULL;
  g_list_free(children);
  children = NULL;
  return child;
}

GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = (GtkWidget*)g_list_nth_data(children, which);
  g_list_free(children);
  children = NULL;
  return child;
}

static void _remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove((GtkContainer*)data, widget);
}

void dt_gui_container_remove_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _remove_child, container);
}

static void _delete_child(GtkWidget *widget, gpointer data)
{
  (void)data;  // avoid unreferenced-parameter warning
  gtk_widget_destroy(widget);
}

void dt_gui_container_destroy_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _delete_child, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
