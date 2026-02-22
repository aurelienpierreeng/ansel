/*
    This file is part of the Ansel project.
    Copyright (C) 2023-2025 Aurélien PIERRE.
    Copyright (C) 2023 Luca Zulberti.
    
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
#include "gui/preferences.h"
#include "common/undo.h"
#include "common/selection.h"
#include "common/collection.h"
#include "common/image_cache.h"
#include "develop/dev_history.h"
#include "control/control.h"


MAKE_ACCEL_WRAPPER(dt_gui_preferences_show)

static gboolean undo_sensitive_callback()
{
  if(!darktable.view_manager) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  gboolean sensitive = FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    sensitive = dt_is_undo_list_populated(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_undo_list_populated(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_undo_list_populated(darktable.undo, DT_UNDO_MAP);

  return sensitive;
}

static gboolean undo_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!darktable.view_manager || !undo_sensitive_callback()) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "map"))
    dt_undo_do_undo(darktable.undo, DT_UNDO_MAP);
  // Beware: it needs to block callbacks declared in view, which may not be loaded.
  // Another piece of shitty peculiar design that doesn't comply with the logic of the rest of the soft.
  // That's what you get from ignoring modularity principles.
  // For now we just ignore the peculiar stuff, no idea how annoying it is, seems it's only GUI candy.

  return TRUE;
}


static gboolean redo_sensitive_callback()
{
  if(!darktable.view_manager) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  gboolean sensitive = FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    sensitive = dt_is_redo_list_populated(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_redo_list_populated(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "darkroom"))
    sensitive = dt_is_redo_list_populated(darktable.undo, DT_UNDO_MAP);

  return sensitive;
}


static gboolean redo_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!darktable.view_manager || !redo_sensitive_callback()) return FALSE;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return FALSE;

  if(!strcmp(cv->module_name, "lighttable"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);
  else if(!strcmp(cv->module_name, "darkroom"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_DEVELOP);
  else if(!strcmp(cv->module_name, "map"))
    dt_undo_do_redo(darktable.undo, DT_UNDO_MAP);
  //   see undo_callback()

  return TRUE;
}


static gboolean is_image_in_dev(GList *imgs)
{
  return darktable.develop != NULL
    && g_list_find(imgs, GINT_TO_POINTER(darktable.develop->image_storage.id));
}

static gboolean compress_history_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GList *imgs = dt_act_on_get_images();
  if(!imgs) return FALSE;

  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_develop_t *dev = darktable.develop;
    dt_dev_undo_start_record(dev);
    dt_dev_history_compress(dev);
    dt_dev_undo_end_record(dev);
    int pipe_remove = dt_dev_history_refresh_nodes(dev, dev->iop, dev->history);
    dt_dev_history_gui_update(dev);
    dt_dev_history_pixelpipe_update(dev, pipe_remove);
    dt_dev_history_notify_change(dev, dev->image_storage.id);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

    // Avoid running a headless compression for the current darkroom image: the history module
    // (src/libs/history.c) compresses directly from the loaded pipeline.
    imgs = g_list_remove(imgs, GINT_TO_POINTER(dev->image_storage.id));
  }

  if(imgs) dt_history_compress_on_list(imgs);

  g_list_free(imgs);
  return TRUE;
}

static gboolean delete_history_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!has_active_images()) return FALSE;

  GList *imgs = dt_act_on_get_images();
  if(!imgs) return FALSE;

  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_start_record(darktable.develop);
    dt_dev_write_history(darktable.develop);
  }

  // We do not ask for confirmation because it can be undone by Ctrl + Z
  dt_history_delete_on_list(imgs, TRUE);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_end_record(darktable.develop);
    dt_dev_reload_history_items(darktable.develop, darktable.develop->image_storage.id);
    dt_dev_history_gui_update(darktable.develop);
    dt_dev_history_pixelpipe_update(darktable.develop, TRUE);
    dt_dev_history_notify_change(darktable.develop, darktable.develop->image_storage.id);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  dt_control_queue_redraw_center();
  g_list_free(imgs);
  return TRUE;
}

static gboolean copy_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  // Allow copy only when exactly one file is selected
  if(dt_selection_get_length(darktable.selection) != 1)
  {
    dt_control_log(_("Copy is allowed only with exactly one image selected"));
    return FALSE;
  }

  GList *imgs = dt_selection_get_list(darktable.selection);
  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);
  g_list_free(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_dev_write_history(darktable.develop);
  }

  dt_history_copy(dt_selection_get_first_id(darktable.selection));
  return TRUE;
}


static gboolean copy_parts_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  // Allow copy only when exactly one file is selected
  if(dt_selection_get_length(darktable.selection) != 1)
  {
    dt_control_log(_("Copy is allowed only with exactly one image selected"));
    return FALSE;
  }

  GList *imgs = dt_selection_get_list(darktable.selection);
  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);
  g_list_free(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_dev_write_history(darktable.develop);
  }

  dt_history_copy_parts(dt_selection_get_first_id(darktable.selection));
  return TRUE;
}


static gboolean paste_sensitive_callback()
{
  return darktable.view_manager->copy_paste.copied_imageid > 0;
}

static gboolean paste_all_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!paste_sensitive_callback())
  {
    dt_control_log(_("Paste needs selected images to work"));
    return FALSE;
  }

  GList *imgs = dt_selection_get_list(darktable.selection);
  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_start_record(darktable.develop);
    dt_dev_write_history(darktable.develop);
  }

  dt_history_paste_on_list(imgs, TRUE);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_end_record(darktable.develop);
    dt_dev_reload_history_items(darktable.develop, darktable.develop->image_storage.id);
    dt_dev_history_gui_update(darktable.develop);
    dt_dev_history_pixelpipe_update(darktable.develop, TRUE);
    dt_dev_history_notify_change(darktable.develop, darktable.develop->image_storage.id);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  dt_control_queue_redraw_center();
  g_list_free(imgs);
  return TRUE;
}

static gboolean paste_parts_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!paste_sensitive_callback())
  {
    dt_control_log(_("Paste needs selected images to work"));
    return FALSE;
  }

  GList *imgs = dt_selection_get_list(darktable.selection);
  gboolean is_darkroom_image_in_list = is_image_in_dev(imgs);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_start_record(darktable.develop);
    dt_dev_write_history(darktable.develop);
  }

  dt_history_paste_parts_on_list(imgs, TRUE);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_end_record(darktable.develop);
    dt_dev_reload_history_items(darktable.develop, darktable.develop->image_storage.id);
    dt_dev_history_gui_update(darktable.develop);
    dt_dev_history_pixelpipe_update(darktable.develop, TRUE);
    dt_dev_history_notify_change(darktable.develop, darktable.develop->image_storage.id);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  dt_control_queue_redraw_center();
  g_list_free(imgs);
  return TRUE;
}

static gboolean load_xmp_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GList *imgs = dt_selection_get_list(darktable.selection);
  if(!imgs) return FALSE;

  const int act_on_one = g_list_is_singleton(imgs); // list length == 1?
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("open sidecar file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
          _("_open"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(act_on_one)
  {
    //single image to load xmp to, assume we want to load from same dir
    const int32_t imgid = GPOINTER_TO_INT(imgs->data);
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

      // TODO: only when needed, check imgid
      dt_dev_reload_history_items(darktable.develop, darktable.develop->image_storage.id);
      dt_dev_history_gui_update(darktable.develop);
      dt_dev_history_pixelpipe_update(darktable.develop, TRUE);
      dt_dev_history_notify_change(darktable.develop, darktable.develop->image_storage.id);
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
  return TRUE;
}

static gboolean duplicate_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(has_active_images())
  {
    dt_control_duplicate_images(FALSE);
    return TRUE;
  }

  dt_control_log(_("Duplication needs selected images to work"));
  return FALSE;
}

static gboolean new_history_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(has_active_images())
  {
    dt_control_duplicate_images(TRUE);
    return TRUE;
  }

  dt_control_log(_("Creating new historys needs selected images to work"));
  return TRUE;
}


static gboolean shortcuts_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_accels_window(darktable.gui->accels, GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  return TRUE;
}


void append_edit(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Undo"), index, NULL, undo_callback, NULL, NULL, undo_sensitive_callback, GDK_KEY_z, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Redo"), index, NULL, redo_callback, NULL, NULL, redo_sensitive_callback, GDK_KEY_y, GDK_CONTROL_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy history (all)"), index, NULL, copy_callback, NULL, NULL, has_selection, GDK_KEY_c, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Copy history (parts)..."), index, NULL, copy_parts_callback, NULL, NULL, has_selection, GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_menu_entry(menus, lists, _("Paste history (all)"), index, NULL, paste_all_callback, NULL, NULL,
                     paste_sensitive_callback, GDK_KEY_v, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Paste history (parts)..."), index, NULL, paste_parts_callback, NULL, NULL,
                     paste_sensitive_callback, GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Load history from XMP..."), index, NULL,
                     load_xmp_callback, NULL, NULL, has_active_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Create new history"), index, NULL,
                    new_history_callback, NULL, NULL, has_active_images, GDK_KEY_n, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Duplicate existing history"), index, NULL,
                     duplicate_callback, NULL, NULL, has_active_images, GDK_KEY_d, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Compress history"), index, NULL,
                     compress_history_callback, NULL, NULL, has_active_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Delete history"), index, NULL,
                     delete_history_callback, NULL, NULL, has_active_images, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Preferences..."), index, NULL, GET_ACCEL_WRAPPER(dt_gui_preferences_show), NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Keyboard shortcuts..."), index, NULL, shortcuts_callback, NULL, NULL, NULL, 0, 0);
}
