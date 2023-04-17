#include "gui/actions/menu.h"


static void quit_callback()
{
  dt_control_quit();
}

static void remove_from_library_callback()
{
  dt_control_remove_images();
}

static void remove_from_disk_callback()
{
  dt_control_delete_images();
}

void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("File"));
  dt_action_t *ac;

  add_sub_menu_entry(menus, lists, _("Remove from library"), index, NULL, remove_from_library_callback, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Remove files from library"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, remove_from_library_callback, GDK_KEY_Delete, 0);

  add_sub_menu_entry(menus, lists, _("Delete on disk"), index, NULL, remove_from_disk_callback, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Delete files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, remove_from_disk_callback, GDK_KEY_Delete, GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Quit"), index, NULL, quit_callback, NULL, NULL, NULL);
  ac = dt_action_define(pnl, NULL, N_("Quit"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, quit_callback, GDK_KEY_q, GDK_CONTROL_MASK);
}
