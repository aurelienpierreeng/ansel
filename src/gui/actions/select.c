#include "gui/actions/menu.h"
#include "common/selection.h"
#include "common/collection.h"

// Select menu is unavailable in anything else than lighttable

gboolean select_all_sensitive_callback()
{
  return dt_collection_get_count(darktable.collection) > dt_selection_get_length(darktable.selection)
    && _is_lighttable();
}


void select_all_callback()
{
  if(!select_all_sensitive_callback()) return;

  dt_thumbtable_select_all(darktable.gui->ui->thumbtable_lighttable);
}


gboolean clear_selection_sensitive_callback()
{
  return dt_selection_get_length(darktable.selection) > 0
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
  dt_thumbtable_invert_selection(darktable.gui->ui->thumbtable_lighttable);
}

void append_select(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Select all"), index, NULL, select_all_callback, NULL, NULL, select_all_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Clear selection"), index, NULL, clear_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Invert selection"), index, NULL, invert_selection_callback, NULL, NULL, clear_selection_sensitive_callback, GDK_KEY_i, GDK_CONTROL_MASK);

  //add_menu_separator(menus[index]);
}
