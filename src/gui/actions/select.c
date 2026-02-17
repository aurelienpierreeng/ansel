/*
    This file is part of the Ansel project.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023, 2025 Aurélien PIERRE.
    
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
#include "gui/actions/menu.h"
#include "common/selection.h"
#include "common/collection.h"

// Select menu is unavailable in anything else than lighttable

gboolean select_all_sensitive_callback()
{
  return dt_collection_get_count(darktable.collection) > dt_selection_get_length(darktable.selection)
    && _is_lighttable();
}


static gboolean select_all_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!select_all_sensitive_callback()) return FALSE;
  dt_thumbtable_select_all(darktable.gui->ui->thumbtable_lighttable);
  return TRUE;
}


gboolean clear_selection_sensitive_callback()
{
  return dt_selection_get_length(darktable.selection) > 0
    && _is_lighttable();
}


static gboolean clear_selection_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!clear_selection_sensitive_callback()) return FALSE;
  dt_selection_clear(darktable.selection);
  return TRUE;
}


static gboolean invert_selection_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!clear_selection_sensitive_callback()) return FALSE;
  dt_thumbtable_invert_selection(darktable.gui->ui->thumbtable_lighttable);
  return TRUE;
}

static gboolean scroll_to_selection_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_scroll_to_selection(darktable.gui->ui->thumbtable_filmstrip);
  dt_thumbtable_scroll_to_selection(darktable.gui->ui->thumbtable_lighttable);
  return TRUE;
}

void append_select(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Select all"), index, NULL, select_all_callback, NULL, NULL, select_all_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Clear selection"), index, NULL, clear_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Invert selection"), index, NULL, invert_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_i, GDK_CONTROL_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Scroll back to selection"), index, NULL, scroll_to_selection_callback, NULL, NULL, NULL, 0, 0);
}
