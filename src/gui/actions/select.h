#include "gui/actions/menu.h"
#include "common/selection.h"

// Select menu is unavailable in anything else than lighttable

gboolean select_all_sensitive_callback()
{
  return dt_collection_get_count_no_group(darktable.collection) > dt_collection_get_selected_count(darktable.collection)
    && _is_lighttable();
}


void select_all_callback()
{
  if(!select_all_sensitive_callback()) return;

  dt_selection_select_all(darktable.selection);
}


gboolean clear_selection_sensitive_callback()
{
  return dt_collection_get_selected_count(darktable.collection) > 0
    && _is_lighttable();
}


void clear_selection_callback()
{
  if(!clear_selection_sensitive_callback()) return;

  dt_selection_clear(darktable.selection);
}


void invert_selection_callback()
{
  if(!clear_selection_sensitive_callback()) return;

  dt_selection_invert(darktable.selection);
}

void select_unedited_callback()
{
  if(!_is_lighttable()) return;

  dt_selection_select_unaltered(darktable.selection);
}

void append_select(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Select all"), index, NULL, select_all_callback, NULL, NULL, select_all_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Clear selection"), index, NULL, clear_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Invert selection"), index, NULL, invert_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_i, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Select unedited"), index, NULL, select_unedited_callback, NULL, NULL, _is_lighttable, 0, 0);

  //add_menu_separator(menus[index]);
}
