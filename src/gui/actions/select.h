#include "gui/actions/menu.h"
#include "common/selection.h"
#include "common/action.h"


void select_all_callback()
{
  dt_selection_select_all(darktable.selection);
}

gboolean select_all_sensitive_callback()
{
  return dt_collection_get_count_no_group(darktable.collection) > dt_collection_get_selected_count(darktable.collection);
}

void clear_selection_callback()
{
  dt_selection_clear(darktable.selection);
}

gboolean clear_selection_sensitive_callback()
{
  return dt_collection_get_selected_count(darktable.collection) > 0;
}

void invert_selection_callback()
{
  dt_selection_invert(darktable.selection);
}

void select_unedited_callback()
{
  dt_selection_select_unaltered(darktable.selection);
}

void append_select(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("Selection"));
  dt_action_t *ac;

  add_sub_menu_entry(menus, lists, _("Select all"), index, NULL, select_all_callback, NULL, NULL, select_all_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Select all"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, select_all_callback, GDK_KEY_a, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Clear selection"), index, NULL, clear_selection_callback, NULL, NULL, clear_selection_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Clear selection"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, clear_selection_callback, GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Invert selection"), index, NULL, invert_selection_callback, NULL, NULL, clear_selection_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Invert selection"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, invert_selection_callback, GDK_KEY_i, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Select unedited"), index, NULL, select_unedited_callback, NULL, NULL, NULL);
  ac = dt_action_define(pnl, NULL, N_("Select unedited"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, select_unedited_callback, 0, 0);

  //add_menu_separator(menus[index]);
}
