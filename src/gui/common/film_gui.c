/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "gui/common/film_gui.h"

#include "common/film.h"
#include "system/mem_alloc.h"
#include "gui/gtk.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/* Runs on the GUI thread via g_idle_add(): dt_film_remove_empty() is reached from worker
 * threads, and gtk_dialog_run() must not be. Takes ownership of the list. */
static gboolean _ask_and_delete(gpointer user_data)
{
  GList *empty_dirs = (GList *)user_data;
  const int n_empty_dirs = g_list_length(empty_dirs);

  GtkWidget *win = dt_gui_main_window();
  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                             GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                             ngettext("do you want to remove this empty directory?",
                                                      "do you want to remove these empty directories?",
                                                      n_empty_dirs));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_title(GTK_WINDOW(dialog),
                       ngettext("remove empty directory?", "remove empty directories?", n_empty_dirs));

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(scroll, TRUE);
  dt_gui_add_class(scroll, "dt_recessed_scroll");

  GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);

  for(GList *list_iter = empty_dirs; list_iter; list_iter = g_list_next(list_iter))
  {
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0, list_iter->data, -1);
  }

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
  gtk_widget_set_name(GTK_WIDGET(tree), "delete-dialog");
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("name"), gtk_cell_renderer_text_new(),
                                                                       "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(25));

  gtk_container_add(GTK_CONTAINER(content_area), scroll);

  gtk_widget_show_all(dialog); // needed for the content area!

  const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  // The deleting itself is the backend's job; this module only obtained consent.
  if(res == GTK_RESPONSE_YES) dt_film_remove_directories(empty_dirs);

  g_list_free_full(empty_dirs, dt_free_gpointer);
  g_object_unref(store);

  return FALSE;
}

static void _confirm_rmdir(GList *empty_dirs)
{
  // defer to the GUI thread; ownership travels with the list
  g_idle_add(_ask_and_delete, empty_dirs);
}

void dt_film_gui_register_handlers(void)
{
  dt_film_set_confirm_rmdir_handler(_confirm_rmdir);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
