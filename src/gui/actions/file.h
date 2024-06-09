#include "gui/actions/menu.h"
#include "common/collection.h"
#include "libs/collect.h"
#include "common/import.h"
#include "libs/lib.h"


static void pretty_print_collection(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);

    if(n == 3)
    {
      if(k > 0) switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                   item == 0 ? dt_image_film_roll_name(str) : str);
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}


void update_collection_callback(GtkWidget *widget)
{
  // Grab the position of current menuitem in menu list
  const int index = GPOINTER_TO_INT(get_custom_data(widget));

  // Grab the corresponding config line
  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", index);
  const char *collection = dt_conf_get_string_const(confname);

  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", index);
  const int position = dt_conf_get_int(confname);

  // Update the collection to the value defined in config line
  dt_collection_deserialize(collection);

  // Restore position in collection
  dt_thumbtable_t *table = dt_ui_thumbtable(darktable.gui->ui);
  dt_thumbtable_set_offset(table, position, TRUE);
}


void init_collection_line(gpointer instance,
                          dt_collection_change_t query_change,
                          dt_collection_properties_t changed_property, gpointer imgs, int next,
                          gpointer user_data)
{
  GtkWidget *widget = GTK_WIDGET(user_data);

  // Grab the position of current menuitem in menu list
  const int index = GPOINTER_TO_INT(get_custom_data(widget));

  // Grab the corresponding config line
  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", index);

  // Get the human-readable name of the collection
  const char *collection = dt_conf_get_string_const(confname);


  if(collection && collection[0] != '\0')
  {
    char label[2048] = { 0 };
    pretty_print_collection(collection, label, sizeof(label));
    dt_capitalize_label(label);

    // Update the menu entry label for current collection name
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    gtk_label_set_markup(GTK_LABEL(child), label);
  }
}

gboolean _is_lighttable()
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv && !strcmp(cv->module_name, "lighttable");
}

void import_files_callback()
{
    dt_images_import();
}

void _close_export_popup(GtkWidget *dialog, gint response_id, gpointer data)
{
  // We need to increase the reference count of the module,
  // then remove it from the popup before closing it,
  // otherwise it gets destroyed along with it.
  darktable.gui->export_popup.module = (GtkWidget *)g_object_ref(darktable.gui->export_popup.module);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(darktable.gui->export_popup.window));
  gtk_container_remove(GTK_CONTAINER(content), darktable.gui->export_popup.module);
  darktable.gui->export_popup.window = NULL;
}

void export_files_callback()
{
  if(darktable.gui->export_popup.window)
  {
    // if not NULL, we already have a popup open and can't re-instanciate a live GtkWidget
    return;
  }

  dt_lib_module_t *module = dt_lib_get_module("export");
  if(!module) return;

  // get_expander actually builds the expander, it's not a getter despite what the name suggests.
  // On first run we need to build, an the following runs, just fetch it
  GtkWidget *w = darktable.gui->export_popup.module
                  ? darktable.gui->export_popup.module
                  : dt_lib_gui_get_expander(module);
  if(!w) return;

  // Save the module
  darktable.gui->export_popup.module = w;

  // Prepare the popup
  GtkWidget *dialog = gtk_dialog_new();
#ifdef GDK_WINDOWING_QUARTZ
// TODO: On MacOS (at least on version 13) the dialog windows doesn't behave as expected. The dialog
// needs to have a parent window. "set_parent_window" wasn't working, so set_transient_for is
// the way to go. Still the window manager isn't dealing with the dialog properly, when the dialog
// is shifted outside its parent. The dialog isn't visible any longer but still listed as a window
// of the app.
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_title(GTK_WINDOW(dialog), _("Ansel - Export images"));
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_close_export_popup), NULL);

  // Ensure the module is expanded
  dt_lib_gui_set_expanded(module, TRUE);
  dt_gui_add_help_link(w, dt_get_help_url(module->plugin_name));
  gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(360), -1);

  // Populate popup and fire everything
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content), w, FALSE, FALSE, 0);
  gtk_widget_set_visible(w, TRUE);
  gtk_widget_show_all(dialog);

  // Save the ref to the window. We don't reuse its content, we just need to know if it exists.
  darktable.gui->export_popup.window = dialog;
}


void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("File"));
  dt_action_t *ac;

  add_sub_menu_entry(menus, lists, _("Import..."), index, NULL, import_files_callback, NULL, NULL,
                     NULL);
  ac = dt_action_define(pnl, NULL, N_("Import images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, import_files_callback, GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Export..."), index, NULL, export_files_callback, NULL, NULL,
                     NULL);
  ac = dt_action_define(pnl, NULL, N_("Export images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, export_files_callback, GDK_KEY_e, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_top_submenu_entry(menus, lists, _("Recent collections"), index);
  GtkWidget *parent = get_last_widget(lists);

  for(int i = 0; i < NUM_LAST_COLLECTIONS; i++)
  {
    // Pass the position of current menuitem in list as custom-data pointer
    add_sub_sub_menu_entry(parent, lists, "", index, GINT_TO_POINTER(i), update_collection_callback, NULL, NULL, NULL);

    // Call init directly just this once
    GtkWidget *this = get_last_widget(lists);
    init_collection_line(NULL, DT_COLLECTION_CHANGE_NONE, DT_COLLECTION_PROP_UNDEF, NULL, 0, this);

    // Connect init to collection_changed signal for future updates
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                              G_CALLBACK(init_collection_line), (gpointer)this);
  }

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy files on disk..."), index, NULL, dt_control_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Copy files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_copy_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Move files on disk..."), index, NULL, dt_control_move_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Move files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_move_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Create a blended HDR"), index, NULL, dt_control_merge_hdr, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Create a blended HDR"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_merge_hdr, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy distant images locally"), index, NULL, dt_control_set_local_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Copy distant images locally"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_set_local_copy_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Resynchronize distant images"), index, NULL, dt_control_reset_local_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Resynchronize distant images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_reset_local_copy_images, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Remove from library"), index, NULL, (void *)dt_control_remove_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Remove files from library"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, (void *)dt_control_remove_images, GDK_KEY_Delete, 0);

  add_sub_menu_entry(menus, lists, _("Delete on disk"), index, NULL, dt_control_delete_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Delete files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_delete_images, GDK_KEY_Delete, GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Quit"), index, NULL, dt_control_quit, NULL, NULL, NULL);
  ac = dt_action_define(pnl, NULL, N_("Quit"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_quit, GDK_KEY_q, GDK_CONTROL_MASK);
}
