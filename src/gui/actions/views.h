
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "gui/actions/menu.h"


gboolean views_active_callback(GtkWidget *menu_item)
{
  // The active view is the one whose name matches the menu item label
  const char *current_view = dt_view_manager_name(darktable.view_manager);
  const char *current_label = get_label_text(menu_item);
  return !g_strcmp0(current_view, current_label);
}

gboolean views_sensitive_callback(GtkWidget *menu_item)
{
  // The insensitive view is the one whose name matches the menu item label
  const char *current_view = dt_view_manager_name(darktable.view_manager);
  const char *current_label = get_label_text(menu_item);
  return g_strcmp0(current_view, current_label);
}

void view_switch_callback(GtkWidget *menu_item)
{
  const char *view_name = get_label_text(menu_item);

  for(GList *view_iter = darktable.view_manager->views; view_iter; view_iter = g_list_next(view_iter))
  {
    dt_view_t *view = (dt_view_t *)view_iter->data;
    if(view->flags() & VIEW_FLAGS_HIDDEN) continue;

    // If we found the dt_view_t object whose name matches the Gtk label, switch view
    if(!g_strcmp0(view->name(view), view_name))
      dt_ctl_switch_mode_to_by_view(view);
  }
}

void append_views(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  for(GList *view_iter = darktable.view_manager->views; view_iter; view_iter = g_list_next(view_iter))
  {
    dt_view_t *view = (dt_view_t *)view_iter->data;
    if(view->flags() & VIEW_FLAGS_HIDDEN) continue;
    add_sub_menu_entry(menus, lists, view->name(view), index,
                       NULL, view_switch_callback, NULL, views_active_callback, views_sensitive_callback);
    dt_action_define(&darktable.control->actions_global, _("Switch views"), view->module_name, get_last_widget(lists), NULL);
  }
}

/* TODO ?
* The current logic is to execute state callbacks (active, sensisitive, check) on each menu activation,
* in the menu.h:update_menu_entries() function.
* This is inexpensive as long as there are not too many items.
* The other approach is to connect menu.h:update_entry() to signals, e.g.

    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                              G_CALLBACK(update_entry), self);
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,
                                    G_CALLBACK(_lib_viewswitcher_view_cannot_change_callback), self);

    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(update_entry), self);
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_viewswitcher_view_cannot_change_callback), self);
*
* So the update happens as soon as the signal is emited, only for the relevant menuitems.
*
* To re-evaluate in the future…
*/
