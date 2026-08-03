/*
    This file is part of the Ansel project.
    Copyright (C) 2023-2025 Aurélien PIERRE.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2024, 2026 Guillaume Stutin.
    
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
#include "common/darktable.h"
#include "gui/actions/menu.h"
#include "common/collection.h"
#include "common/image.h"
#include "common/selection.h"
#include "libs/collect.h"
#include "common/import.h"
#include "libs/lib.h"
#include "control/control.h"


static void pretty_print_collection(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(IS_NULL_PTR(buf) || buf[0] == '\0') return;

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


static gboolean update_collection_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  // Grab the position of current menuitem in menu list
  const int index = GPOINTER_TO_INT(get_custom_data(GTK_WIDGET(user_data)));

  // Grab the corresponding config line
  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", index);
  const char *collection = dt_conf_get_string_const(confname);

  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", index);

  // Update the collection to the value defined in config line
  dt_collection_deserialize(collection);

  return TRUE;
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

    // Update the menu entry label for current collection name. Escape it: a collection value
    // can contain markup-significant characters (e.g. the < > operators in date/numeric rules).
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    gchar *escaped = g_markup_escape_text(label, -1);
    gtk_label_set_markup(GTK_LABEL(child), escaped);
    g_free(escaped);
  }
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

static gboolean export_files_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(darktable.gui->export_popup.window)
  {
    // if not NULL, we already have a popup open and can't re-instanciate a live GtkWidget
    gtk_window_present_with_time(GTK_WINDOW(darktable.gui->export_popup.window), GDK_CURRENT_TIME);
    return TRUE;
  }

  dt_lib_module_t *module = dt_lib_get_module("export");
  if(IS_NULL_PTR(module)) return TRUE;

  // get_expander actually builds the expander, it's not a getter despite what the name suggests.
  // On first run we need to build, an the following runs, just fetch it
  GtkWidget *w = darktable.gui->export_popup.module
                  ? darktable.gui->export_popup.module
                  : dt_lib_gui_get_expander(module);
  if(IS_NULL_PTR(w)) return TRUE;

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
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_title(GTK_WINDOW(dialog), _("Ansel - Export images"));
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_close_export_popup), NULL);

  // Ensure the module is expanded
  dt_lib_gui_set_expanded(module, TRUE);
  dt_gui_add_help_link(w, dt_get_help_url(module->plugin_name));
  gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(450), -1);

  // Populate popup and fire everything
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content), w, TRUE, TRUE, 0);
  gtk_widget_set_visible(w, TRUE);
  gtk_widget_show_all(dialog);

  // Save the ref to the window. We don't reuse its content, we just need to know if it exists.
  darktable.gui->export_popup.window = dialog;

  return TRUE;
}


typedef enum dt_export_list_mode_t
{
  DT_EXPORT_LIST_IDS = 0,
  DT_EXPORT_LIST_FILENAMES = 1
} dt_export_list_mode_t;

// On screen and in the clipboard, both modes emit a single line ready to paste
// as script arguments: IDs comma-separated (--ids 1,2,3), filenames
// space-separated and shell-quoted (paths can contain spaces and
// metacharacters; POSIX single-quote escaping also suits PowerShell — only
// cmd.exe differs). Saved files instead get one raw item per line, unquoted:
// files are read verbatim, not parsed by a shell.
static gchar *_export_list_build(GList *imgids, const int mode, const gboolean one_per_line)
{
  GString *str = g_string_new(NULL);
  for(GList *l = imgids; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    if(mode == DT_EXPORT_LIST_FILENAMES)
    {
      char path[PATH_MAX] = { 0 };
      gboolean from_cache = FALSE;
      dt_image_full_path(imgid, path, sizeof(path), &from_cache, __FUNCTION__);
      if(one_per_line)
        g_string_append(str, path);
      else
      {
        gchar *quoted = g_shell_quote(path);
        g_string_append(str, quoted);
        g_free(quoted);
      }
    }
    else
      g_string_append_printf(str, "%i", imgid);

    if(one_per_line)
      g_string_append_c(str, '\n');
    else if(l->next)
      g_string_append_c(str, mode == DT_EXPORT_LIST_FILENAMES ? ' ' : ',');
  }
  return g_string_free(str, FALSE);
}

static void _export_list_fill(GtkComboBox *combo, gpointer user_data)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
  GList *imgids = (GList *)g_object_get_data(G_OBJECT(buffer), "imgids");
  gchar *text = _export_list_build(imgids, gtk_combo_box_get_active(combo), FALSE);
  gtk_text_buffer_set_text(buffer, text, -1);
  g_free(text);
}

static void _export_list_save(GtkWidget *dialog, GtkTextBuffer *buffer)
{
  GList *imgids = (GList *)g_object_get_data(G_OBJECT(buffer), "imgids");
  GtkComboBox *combo = GTK_COMBO_BOX(g_object_get_data(G_OBJECT(buffer), "combo"));
  const int mode = gtk_combo_box_get_active(combo);

  // GtkFileChooserNative uses the platform-native save dialog on Windows and macOS
  GtkFileChooserNative *chooser
      = gtk_file_chooser_native_new(_("Ansel - Save image list"), GTK_WINDOW(dialog),
                                    GTK_FILE_CHOOSER_ACTION_SAVE, _("_Save"), _("_Cancel"));
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                    mode == DT_EXPORT_LIST_FILENAMES ? "ansel-image-files.txt"
                                                                     : "ansel-image-ids.txt");
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gchar *text = _export_list_build(imgids, mode, TRUE);
    GError *error = NULL;
    if(!g_file_set_contents(filename, text, -1, &error))
    {
      dt_control_log(_("could not save the image list to '%s': %s"), filename, error->message);
      g_error_free(error);
    }
    g_free(text);
    g_free(filename);
  }
  g_object_unref(chooser);
}

static void _export_list_response(GtkWidget *dialog, gint response_id, gpointer user_data)
{
  if(response_id == GTK_RESPONSE_OK)
    _export_list_save(dialog, GTK_TEXT_BUFFER(user_data));
  else if(response_id == GTK_RESPONSE_APPLY)
  {
    // GDK_SELECTION_CLIPBOARD is the explicit-copy clipboard on all backends
    // (X11, Wayland, Windows, macOS) — as opposed to the X11-only PRIMARY selection.
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(user_data);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, text, -1);
    gtk_clipboard_store(clipboard);
    g_free(text);
  }
  else
    gtk_widget_destroy(dialog);
}

static gboolean export_image_list_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  static GtkWidget *dialog = NULL;

  // Rebuild from scratch on each call so the content always reflects the current selection
  if(dialog) gtk_widget_destroy(dialog);

  GList *imgids = dt_selection_get_list(darktable.selection);
  if(IS_NULL_PTR(imgids)) return TRUE;

  dialog = gtk_dialog_new_with_buttons(_("Ansel - Export image list"),
                                       GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("Copy to clipboard"), GTK_RESPONSE_APPLY,
                                       _("Save as file..."), GTK_RESPONSE_OK,
                                       _("Close"), GTK_RESPONSE_CLOSE, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(gtk_widget_destroyed), &dialog);

  GtkWidget *combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Image ID"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), _("Image filename"));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Export:")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), combo, TRUE, TRUE, 0);

  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  g_object_set_data_full(G_OBJECT(buffer), "imgids", imgids, (GDestroyNotify)g_list_free);
  g_object_set_data(G_OBJECT(buffer), "combo", combo);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(450));
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(300));
  gtk_container_add(GTK_CONTAINER(scroll), view);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(_export_list_fill), buffer);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_export_list_response), buffer);

  // Setting the active entry fires "changed", which fills the buffer
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), DT_EXPORT_LIST_IDS);

  gtk_widget_show_all(dialog);
  return TRUE;
}


MAKE_ACCEL_WRAPPER(dt_images_import)
MAKE_ACCEL_WRAPPER(dt_control_copy_images)
MAKE_ACCEL_WRAPPER(dt_control_move_images)
MAKE_ACCEL_WRAPPER(dt_control_merge_hdr)
MAKE_ACCEL_WRAPPER(dt_control_set_local_copy_images)
MAKE_ACCEL_WRAPPER(dt_control_reset_local_copy_images)
MAKE_ACCEL_WRAPPER(dt_control_remove_images)
MAKE_ACCEL_WRAPPER(dt_control_delete_images)
MAKE_ACCEL_WRAPPER(dt_control_quit)

void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Import..."), index, NULL, GET_ACCEL_WRAPPER(dt_images_import), NULL, NULL,
                     NULL, GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Export..."), index, NULL, export_files_callback, NULL, NULL,
                     NULL, GDK_KEY_e, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Export image list..."), index, NULL, export_image_list_callback, NULL, NULL,
                     has_selection, 0, 0);

  add_menu_separator(menus[index]);

  add_top_submenu_entry(menus, lists, _("Recent collections"), index);
  GtkWidget *parent = get_last_widget(lists);

  for(int i = 0; i < NUM_LAST_COLLECTIONS; i++)
  {
    // Pass the position of current menuitem in list as custom-data pointer
    gchar *item_label = g_strdup_printf(_("Most recent collection #%i"), i);
    add_sub_sub_menu_entry(menus, parent, lists, item_label, index, GINT_TO_POINTER(i), update_collection_callback, NULL, NULL, NULL, 0, 0);
    dt_free(item_label);

    // Call init directly just this once
    GtkWidget *this = get_last_widget(lists);
    init_collection_line(NULL, DT_COLLECTION_CHANGE_NONE, DT_COLLECTION_PROP_UNDEF, NULL, 0, this);

    // Connect init to collection_changed signal for future updates
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                              G_CALLBACK(init_collection_line), (gpointer)this);
  }

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy files on disk..."), index, NULL, GET_ACCEL_WRAPPER(dt_control_copy_images), NULL, NULL,
                     has_active_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Move files on disk..."), index, NULL, GET_ACCEL_WRAPPER(dt_control_move_images), NULL, NULL,
                     has_active_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Create a blended HDR"), index, NULL, GET_ACCEL_WRAPPER(dt_control_merge_hdr), NULL, NULL,
                     has_active_images, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy distant images locally"), index, NULL, GET_ACCEL_WRAPPER(dt_control_set_local_copy_images), NULL, NULL,
                     has_active_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Resynchronize distant images"), index, NULL, GET_ACCEL_WRAPPER(dt_control_reset_local_copy_images), NULL, NULL,
                     has_active_images, 0, 0);

  add_menu_separator(menus[index]);

  add_no_accel_sub_menu_entry(menus, lists, _("Remove from library"), index, NULL, GET_ACCEL_WRAPPER(dt_control_remove_images), NULL, NULL,
                             has_active_image_in_lighttable, GDK_KEY_Delete, 0);

  add_sub_menu_entry(menus, lists, _("Delete from disk"), index, NULL, GET_ACCEL_WRAPPER(dt_control_delete_images), NULL, NULL,
                     has_active_image_in_lighttable, GDK_KEY_Delete, GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Quit"), index, NULL, GET_ACCEL_WRAPPER(dt_control_quit), NULL, NULL, NULL,  GDK_KEY_q, GDK_CONTROL_MASK);
}
