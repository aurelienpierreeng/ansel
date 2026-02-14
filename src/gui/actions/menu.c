
#include "common/darktable.h"
#include "common/act_on.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/selection.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "views/view.h"

#include "menu.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/** How to use:
 *  1. write callback functions returning a gboolean that will check the context to decide if
 *  the menu item should be insensitive, checked, active. These should only use the content of
 *  globally accessible structures like `darktable.gui` since they take no arguments.
 *
 *  2. re-use the action callback functions already used for global keyboard shortcuts (actions/accels).
 *  Again, all inputs and internal functions should be globally accessible, for example using proxies.
 *
 *  3. wire everything with the `set_menu_entry` function below. GUI states of the children menu items
 *  will be updated automatically everytime a top-level menu is opened.
 **/


// Wrapper to match menuitem "activate" signal to our generic accel callback
static void _activate_callback_to_action_callback(GtkMenuItem* menu_item, gpointer user_data)
{
  dt_menu_entry_t *entry = (dt_menu_entry_t *)user_data;
  GtkWindow *window = GTK_WINDOW(gtk_widget_get_ancestor(GTK_WIDGET(menu_item), GTK_TYPE_WINDOW));
  entry->action_callback(entry->accel_group, G_OBJECT(window), 0, 0, menu_item);
}


dt_menu_entry_t *set_menu_entry(GtkWidget **menus, GList **items_list,
                                const gchar *label, dt_menus_t menu_index,
                                GtkMenu *parent,
                                void *data,
                                gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data),
                                gboolean (*checked_callback)(GtkWidget *widget),
                                gboolean (*active_callback)(GtkWidget *widget),
                                gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                GdkModifierType mods, GtkAccelGroup *accel_group)
{
  // Alloc and set to 0
  dt_menu_entry_t *entry = calloc(1, sizeof(dt_menu_entry_t));

  // Main widget
  if(checked_callback)
  {
    entry->widget = gtk_check_menu_item_new_with_label("");
    entry->style = DT_MENU_ENTRY_CHECKBUTTON;
  }
  else
  {
    entry->widget = gtk_menu_item_new_with_label("");
    entry->style = DT_MENU_ENTRY_DEFAULT;
  }

  // Set the text label allowing markup
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(entry->widget));
  gtk_label_set_markup(GTK_LABEL(child), label);

  // Store arbitrary data in the GtkWidget if needed
  if(data)
    g_object_set_data(G_OBJECT(entry->widget), "custom-data", data);

  gtk_widget_show_all(GTK_WIDGET(entry->widget));

  entry->menu = menu_index;
  entry->accel_group = accel_group;
  entry->action_callback = action_callback;
  entry->checked_callback = checked_callback;
  entry->sensitive_callback = sensitive_callback;
  entry->active_callback = active_callback;

  // Wire the accelerator
  // Publish a new accel to the global map and attach it to the menu entry widget
  if(action_callback != NULL)
  {    
    // Register accel only if requested
    if(accel_group != NULL && key_val != 0 && mods != 0)
    {
      gchar *clean_label = strip_markup(label);

      // slash is not allowed in control names because that makes accel pathes fail
      assert(g_strrstr(clean_label, "/") == NULL);

      const gchar *parent_path = gtk_menu_get_accel_path(parent);

      dt_accels_new_action_shortcut(
          darktable.gui->accels, action_callback, entry->widget, accel_group,
          parent_path, clean_label,
          key_val, mods, FALSE, _("Triggers the action"));

      gchar *path = dt_accels_build_path(parent_path, clean_label);
      gtk_widget_set_accel_path(entry->widget, path, (action_callback != NULL) ? accel_group : NULL);
      g_free(path);
      g_free(clean_label);
    }
    else if(GTK_IS_ACCEL_LABEL(child))
    {
      // Show a fake shortcut
      gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key_val, mods);
    }

    g_signal_connect(G_OBJECT(entry->widget), "activate", G_CALLBACK(_activate_callback_to_action_callback), entry);
  }

  // Add it to the list of menus items for easy sequential access later
  *items_list = g_list_append(*items_list, entry);
  //fprintf(stdout, "menu %s, ref is at %p, list it as %p\n", label, items_list, *items_list);

  return entry;
}


void update_entry(dt_menu_entry_t *entry)
{
  // Use the callbacks functions to update the visual properties of the menu entry
  if(entry->style > DT_MENU_ENTRY_DEFAULT)
  {
    // Set the visible state of the checkbox without actually triggering the callback running on activation
    // Gtk has no concept of "set costmetic active state" for checkboxes.
    g_signal_handlers_block_matched(G_OBJECT(entry->widget), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _activate_callback_to_action_callback, entry);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(entry->widget), entry->checked_callback(entry->widget));
    g_signal_handlers_unblock_matched(G_OBJECT(entry->widget), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _activate_callback_to_action_callback, entry);
  }

  if(entry->sensitive_callback)
    gtk_widget_set_sensitive(entry->widget, entry->sensitive_callback(entry->widget));

  if(entry->active_callback)
  {
    if(entry->active_callback(entry->widget))
      dt_gui_add_class(entry->widget, "menu-active");
    else
      dt_gui_remove_class(entry->widget, "menu-active");
  }
}

void update_menu_entries(GtkWidget *widget, gpointer user_data)
{
  // Update the graphic state of all sub-menu entries from the current top-level menu
  // but only when it is opened.
  GList **lists = (GList **)user_data;
  //fprintf(stdout, "ref is at %p, list it at %p\n", lists, *lists);

  if(lists && *lists)
  {
    GList *entries = *lists;
    for(GList *entry_iter = g_list_first(entries); entry_iter; entry_iter = g_list_next(entry_iter))
    {
      dt_menu_entry_t *entry = (dt_menu_entry_t *)(entry_iter->data);
      if(entry) update_entry(entry);
    }
  }
}

// Use for first-level entries in any menubar
void add_generic_top_menu_entry(GtkWidget *menu_bar, GtkWidget **menus, GList **lists, const dt_menus_t index,
                                gchar *label, GtkAccelGroup *accel_group, const char *accel_path_prefix)
{
  // Top menus belong to menu bar : file, edit, display, etc.
  menus[index] = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(menus[index]), accel_group);

  gchar *clean_label = strip_markup(label);

  // slash is not allowed in control names because that makes accel pathes fail
  assert(g_strrstr(clean_label, "/") == NULL);

  gchar *accel_path = dt_accels_build_path(accel_path_prefix, clean_label);
  gtk_menu_set_accel_path(GTK_MENU(menus[index]), accel_path);
  g_free(clean_label);
  g_free(accel_path);

  GtkWidget *menu_label = gtk_menu_item_new_with_mnemonic(label);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_label), menus[index]);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), menu_label);
  dt_gui_add_class(menu_label, "top-level-item");
  g_signal_connect(G_OBJECT(menu_label), "activate", G_CALLBACK(update_menu_entries), lists);
}


// Use for first-level entries in the global menubar
void add_top_menu_entry(GtkWidget *menu_bar, GtkWidget **menus, GList **lists, const dt_menus_t index, gchar *label)
{
  // Top menus belong to menu bar : file, edit, display, etc.
  add_generic_top_menu_entry(menu_bar, menus, lists, index, label, darktable.gui->accels->global_accels, "Global/Menu");
}

// Special submenus entries that only open a sub-submenu
void add_generic_top_submenu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index, GtkAccelGroup *accel_group)
{
  GtkWidget *submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_group);

  gchar *clean_label = strip_markup(label);

  // slash is not allowed in control names because that makes accel pathes fail
  assert(g_strrstr(clean_label, "/") == NULL);

  gchar *accel_path = dt_accels_build_path(gtk_menu_get_accel_path(GTK_MENU(menus[index])), clean_label);
  gtk_menu_set_accel_path(GTK_MENU(submenu), accel_path);
  g_free(clean_label);
  g_free(accel_path);

  dt_menu_entry_t *entry = set_menu_entry(menus, lists, label, index, GTK_MENU(menus[index]), NULL, NULL, NULL, NULL, NULL, 0, 0, accel_group);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(entry->widget), submenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menus[index]), entry->widget);
  // We don't take callbacks for top submenus, they do nothing more than opening sub-submenues.
}

// Global menu only
void add_top_submenu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index)
{
  add_generic_top_submenu_entry(menus, lists, label, index, darktable.gui->accels->global_accels);
}


void add_generic_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index,
                                void *data,
                                gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable,
                                                            guint keyval, GdkModifierType mods, gpointer user_data),
                                gboolean (*checked_callback)(GtkWidget *widget),
                                gboolean (*active_callback)(GtkWidget *widget),
                                gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                GdkModifierType mods, GtkAccelGroup *accel_group)
{
  // Default submenu entries
  dt_menu_entry_t *entry = set_menu_entry(menus, lists, label, index, GTK_MENU(menus[index]),
                                          data,
                                          action_callback, checked_callback,
                                          active_callback, sensitive_callback,
                                          key_val, mods, accel_group);

  gtk_menu_shell_append(GTK_MENU_SHELL(menus[index]), entry->widget);
  gtk_menu_item_set_reserve_indicator(GTK_MENU_ITEM(entry->widget), TRUE);
}


void add_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index, void *data,
                        gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                                    GdkModifierType mods, gpointer user_data),
                        gboolean (*checked_callback)(GtkWidget *widget),
                        gboolean (*active_callback)(GtkWidget *widget),
                        gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val, GdkModifierType mods)
{
  add_generic_sub_menu_entry(menus, lists, label, index, data, action_callback, checked_callback, active_callback,
                             sensitive_callback, key_val, mods, darktable.gui->accels->global_accels);
}

void add_no_accel_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index,
                                void *data,
                                gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable,
                                                            guint keyval, GdkModifierType mods, gpointer user_data),
                                gboolean (*checked_callback)(GtkWidget *widget),
                                gboolean (*active_callback)(GtkWidget *widget),
                                gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                GdkModifierType mods)
{
  add_generic_sub_menu_entry(menus, lists, label, index, data, action_callback, checked_callback, active_callback,
                             sensitive_callback, key_val, mods, NULL);
}

void add_generic_sub_sub_menu_entry(GtkWidget **menus, GtkWidget *parent, GList **lists, const gchar *label,
                                    const dt_menus_t index, void *data,
                                    gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable,
                                                                guint keyval, GdkModifierType mods,
                                                                gpointer user_data),
                                    gboolean (*checked_callback)(GtkWidget *widget),
                                    gboolean (*active_callback)(GtkWidget *widget),
                                    gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                    GdkModifierType mods, GtkAccelGroup *accel_group)
{
  // Submenu of submenus entries
  dt_menu_entry_t *entry = set_menu_entry(
      menus, lists, label, index, GTK_MENU(gtk_menu_item_get_submenu(GTK_MENU_ITEM(parent))), data,
      action_callback, checked_callback, active_callback, sensitive_callback, key_val, mods, accel_group);

  gtk_menu_shell_append(GTK_MENU_SHELL(gtk_menu_item_get_submenu(GTK_MENU_ITEM(parent))), entry->widget);
}

void add_sub_sub_menu_entry(GtkWidget **menus, GtkWidget *parent, GList **lists, const gchar *label,
                            const dt_menus_t index, void *data,
                            gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                                        GdkModifierType mods, gpointer user_data),
                            gboolean (*checked_callback)(GtkWidget *widget),
                            gboolean (*active_callback)(GtkWidget *widget),
                            gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val, GdkModifierType mods)
{
  add_generic_sub_sub_menu_entry(menus, parent, lists, label, index, data, action_callback, checked_callback,
                                 active_callback, sensitive_callback, key_val, mods, darktable.gui->accels->global_accels);
}

// We don't go further than 3 levels of menus. This is not a Dassault Systems software.

void add_menu_separator(GtkWidget *menu)
{
  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
}

void add_sub_menu_separator(GtkWidget *parent)
{
  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(gtk_menu_item_get_submenu(GTK_MENU_ITEM(parent))), sep);
}

void * get_custom_data(GtkWidget *widget)
{
  // Grab custom data optionnaly passed by pointer to the menuitem widget
  return g_object_get_data(G_OBJECT(widget), "custom-data");
}

GtkWidget * get_last_widget(GList **list)
{
  GList *last_entry = g_list_last(*list);
  GtkWidget *w = NULL;
  if(last_entry)
  {
    dt_menu_entry_t *entry = (dt_menu_entry_t *)(last_entry)->data;
    if(entry && entry->widget) w = entry->widget;
  }
  return w;
}

gboolean has_selection()
{
  // Can be used to set menu items sensitivity when image(s) is/are selected
  return dt_selection_get_length(darktable.selection) > 0;
}

gboolean has_active_images()
{
  return dt_act_on_get_images_nb(FALSE, FALSE) > 0;
}

gboolean _is_lighttable()
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv && !g_strcmp0(cv->module_name, "lighttable");
}

gboolean _is_darkroom()
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv && !g_strcmp0(cv->module_name, "darkroom");
}

gboolean has_active_image_in_lighttable()
{
  const gboolean image = has_active_images();
  const gboolean lighttable = _is_lighttable();
  return image && lighttable;
}