#include "gui/actions/menu.h"
#include "gui/preferences.h"
#include "common/undo.h"
#include "common/selection.h"
#include "common/collection.h"
#include "common/image_cache.h"

static void preferences_callback(GtkWidget *widget)
{
  dt_gui_preferences_show();
}

static void undo_callback()
{
  if(!darktable.view_manager) return;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return;

  if(!strcmp(cv->module_name, "lighttable"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "map"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_MAP);
  // Beware: it needs to block callbacks declared in view, which may not be loaded.
  // Another piece of shitty peculiar design that doesn't comply with the logic of the rest of the soft.
  // That's what you get from ignoring modularity principles.
  // For now we just ignore the peculiar stuff, no idea how annoying it is, seems it's only GUI candy.
}

static void redo_callback()
{
  if(!darktable.view_manager) return;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return;

  if(!strcmp(cv->module_name, "lighttable"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "map"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_MAP);
  //   see undo_callback()
}

static gboolean undo_sensitive_callback(GtkWidget *w)
{
  if(!darktable.view_manager) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  gboolean sensitive = FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    sensitive = dt_is_undo_list_populated(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_undo_list_populated(darktable.undo, DT_UNDO_DEVELOP);

  return sensitive;
}

static gboolean redo_sensitive_callback(GtkWidget *w)
{
  if(!darktable.view_manager) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  gboolean sensitive = FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    sensitive = dt_is_redo_list_populated(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_redo_list_populated(darktable.undo, DT_UNDO_DEVELOP);

  return sensitive;
}

static gboolean compress_history_sensitive_callback()
{
  return dt_collection_get_selected_count(darktable.collection) > 0;
}

static void compress_history_callback()
{
  GList *imgs = dt_selection_get_list(darktable.selection, TRUE, TRUE);
  if(!imgs) return;

  // As dt_history_compress_on_image does *not* use the history stack data at all
  // make sure the current stack is in the database
  dt_dev_write_history(darktable.develop);

  dt_history_compress_on_list(imgs);
  g_list_free(imgs);

  // We should not need to raise signals here because lighttable
  // reloads the history from SQL at each redraw.
  // FIXME: for obvious perf reasons, history should be cached in thumbnails
  // and then raising signals will be needed here to refresh the cache.
  dt_control_queue_redraw_center();
}

static void delete_development_callback()
{
  GList *imgs = dt_selection_get_list(darktable.selection, TRUE, TRUE);
  if(!imgs) return;

  // We do not ask for confirmation because it can be undone by Ctrl + Z
  dt_history_delete_on_list(imgs, TRUE);
  dt_control_queue_redraw_center();

  g_list_free(imgs);
}

static gboolean copy_sensitive_callback()
{
  return dt_collection_get_selected_count(darktable.collection) == 1
         && dt_selection_get_first_id(darktable.selection) != -1;
}

static void copy_callback()
{
  // Allow copy only when exactly one file is selected
  if(copy_sensitive_callback())
    dt_history_copy(dt_selection_get_first_id(darktable.selection));
  else
    dt_control_log(_("Copy is allowed only with exactly one image selected"));
}

static gboolean paste_sensitive_callback()
{
  return darktable.view_manager->copy_paste.copied_imageid > 0;
}

static void paste_all_callback()
{
  if(paste_sensitive_callback())
  {
    dt_dev_undo_start_record(darktable.develop);
    GList *imgs = dt_selection_get_list(darktable.selection, TRUE, TRUE);
    dt_history_paste_on_list(imgs, TRUE);
    g_list_free(imgs);
    dt_dev_undo_end_record(darktable.develop);
    dt_control_queue_redraw_center();
  }
  else
    dt_control_log(_("Paste needs selected images to work"));
}

static void paste_parts_callback()
{
  /* copy history from previously copied image and past onto selection */
  if(paste_sensitive_callback())
  {
    dt_dev_undo_start_record(darktable.develop);
    GList *imgs = dt_selection_get_list(darktable.selection, TRUE, TRUE);
    dt_history_paste_parts_on_list(imgs, TRUE);
    g_list_free(imgs);
    dt_dev_undo_end_record(darktable.develop);
    dt_control_queue_redraw_center();
  }
  else
    dt_control_log(_("Paste needs selected images to work"));
}

static void load_xmp_callback()
{
  GList *imgs = dt_selection_get_list(darktable.selection, TRUE, TRUE);
  if(!imgs) return;
  const int act_on_one = g_list_is_singleton(imgs); // list length == 1?
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("open sidecar file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
          _("_open"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(act_on_one)
  {
    //single image to load xmp to, assume we want to load from same dir
    const int imgid = GPOINTER_TO_INT(imgs->data);
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(img && img->film_id != -1)
    {
      char pathname[PATH_MAX] = { 0 };
      dt_image_film_roll_directory(img, pathname, sizeof(pathname));
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), pathname);
    }
    else
    {
      // handle situation where there's some problem with cache/film_id
      // i guess that's impossible, but better safe than sorry ;)
      dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
    }
    dt_image_cache_read_release(darktable.image_cache, img);
  }
  else
  {
    // multiple images, use "last import" preference
    dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
  }

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_add_pattern(filter, "*.XMP");
  gtk_file_filter_set_name(filter, _("XMP sidecar files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dtfilename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    if(dt_history_load_and_apply_on_list(dtfilename, imgs) != 0)
    {
      GtkWidget *dialog
          = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE, _("error loading file '%s'"), dtfilename);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dialog);
#endif
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    else
    {
      dt_control_queue_redraw_center();
    }
    if(!act_on_one)
    {
      //remember last import path if applying history to multiple images
      dt_conf_set_folder_from_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
    }
    g_free(dtfilename);
  }
  g_object_unref(filechooser);
  g_list_free(imgs);
}

static void duplicate_callback()
{
  if(compress_history_sensitive_callback())
    dt_control_duplicate_images(FALSE);
  else
    dt_control_log(_("Duplication needs selected images to work"));
}

static void new_development_callback()
{
  if(compress_history_sensitive_callback())
    dt_control_duplicate_images(TRUE);
  else
    dt_control_log(_("Creating new developments needs selected images to work"));
}

void append_edit(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("Edit"));
  dt_action_t *ac;

  add_sub_menu_entry(menus, lists, _("Undo"), index, NULL, undo_callback, NULL, NULL, undo_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Undo last action"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, undo_callback, GDK_KEY_z, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Redo"), index, NULL, redo_callback, NULL, NULL, redo_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Redo last action"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, redo_callback, GDK_KEY_y, GDK_CONTROL_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy development"), index, NULL, copy_callback, NULL, NULL, copy_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Copy development"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, copy_callback, GDK_KEY_c, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Paste development (all)"), index, NULL, paste_all_callback, NULL, NULL,
                     paste_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Paste development (all)"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, paste_all_callback, GDK_KEY_v, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Paste development (parts)…"), index, NULL, paste_parts_callback, NULL, NULL,
                     paste_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Paste development (parts)"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, paste_parts_callback, GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Load development from XMP…"), index, NULL,
                     load_xmp_callback, NULL, NULL, compress_history_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Load development from XMP"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, load_xmp_callback, 0, 0);

  add_sub_menu_entry(menus, lists, _("Create new development"), index, NULL,
                    new_development_callback, NULL, NULL, compress_history_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Create new development"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, new_development_callback, GDK_KEY_n, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Duplicate existing development"), index, NULL,
                     duplicate_callback, NULL, NULL, compress_history_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Duplicate existing development"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, duplicate_callback, GDK_KEY_d, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Compress development history"), index, NULL,
                     compress_history_callback, NULL, NULL, compress_history_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Compress development history"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, compress_history_callback, 0, 0);

  add_sub_menu_entry(menus, lists, _("Delete development"), index, NULL,
                     delete_development_callback, NULL, NULL, compress_history_sensitive_callback);
  ac = dt_action_define(pnl, NULL, N_("Delete development"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, delete_development_callback, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Preferences"), index, NULL, preferences_callback, NULL, NULL, NULL);
}
