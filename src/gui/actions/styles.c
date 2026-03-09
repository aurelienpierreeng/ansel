/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.
    
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
#include "gui/gtk.h"
#include "gui/styles.h"
#include "common/act_on.h"
#include "common/history.h"
#include "common/styles.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "libs/lib.h"

#include <glib.h>

static GtkWidget **_styles_menus = NULL;
static GList **_styles_lists = NULL;
static dt_menus_t _styles_index = DT_MENU_STYLES;
static gboolean _styles_signal_connected = FALSE;
static guint _styles_menu_rebuild_source = 0;

static gboolean _styles_menu_disabled(GtkWidget *widget)
{
  return FALSE;
}

static gboolean _styles_apply_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                       GdkModifierType mods, gpointer user_data)
{
  const char *style_name = get_custom_data(GTK_WIDGET(user_data));
  if(!style_name || !*style_name) return FALSE;

  GList *imgs = dt_act_on_get_images();
  const gboolean duplicate = dt_conf_get_bool("ui_last/styles_create_duplicate");
  gboolean is_darkroom_image_in_list = dt_menu_is_image_in_dev(imgs);

  if(is_darkroom_image_in_list)
  {
    imgs = g_list_remove(imgs, GINT_TO_POINTER(darktable.develop->image_storage.id));
    dt_dev_undo_start_record(darktable.develop);
    const gboolean applied = dt_history_style_on_image(darktable.develop->image_storage.id, style_name, duplicate);
    dt_dev_undo_end_record(darktable.develop);
    if(applied)
      dt_menu_apply_dev_history_update(darktable.develop, TRUE);
  }

  if(imgs) dt_history_style_on_list(imgs, style_name, duplicate);

  g_list_free(imgs);
  imgs = NULL;

  return TRUE;
}

static gboolean _styles_create_sensitive_callback(GtkWidget *widget)
{
  return dt_act_on_get_images_nb(FALSE, FALSE) == 1;
}

static gboolean _styles_create_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                        GdkModifierType mods, gpointer user_data)
{
  const int32_t imgid = dt_act_on_get_first_image();
  if(imgid <= 0) return FALSE;

  dt_gui_styles_dialog_new(imgid);
  return TRUE;
}

static void _close_styles_popup(GtkWidget *dialog, gint response_id, gpointer data)
{
  darktable.gui->styles_popup.module = (GtkWidget *)g_object_ref(darktable.gui->styles_popup.module);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(darktable.gui->styles_popup.window));
  gtk_container_remove(GTK_CONTAINER(content), darktable.gui->styles_popup.module);
  darktable.gui->styles_popup.window = NULL;
}

static gboolean _styles_open_popup_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                            GdkModifierType mods, gpointer user_data)
{
  if(darktable.gui->styles_popup.window)
  {
    gtk_window_present_with_time(GTK_WINDOW(darktable.gui->styles_popup.window), GDK_CURRENT_TIME);
    return TRUE;
  }

  dt_lib_module_t *module = dt_lib_get_module("styles");
  if(!module) return TRUE;

  GtkWidget *w = darktable.gui->styles_popup.module
                  ? darktable.gui->styles_popup.module
                  : dt_lib_gui_get_expander(module);
  if(!w) return TRUE;

  darktable.gui->styles_popup.module = w;

  GtkWidget *dialog = gtk_dialog_new();
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_title(GTK_WINDOW(dialog), _("Ansel - Styles"));
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_close_styles_popup), NULL);

  dt_lib_gui_set_expanded(module, TRUE);
  dt_gui_add_help_link(w, dt_get_help_url(module->plugin_name));
  gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(450), -1);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(content), w, TRUE, TRUE, 0);
  gtk_widget_set_visible(w, TRUE);
  gtk_widget_show_all(dialog);

  darktable.gui->styles_popup.window = dialog;
  return TRUE;
}

static gchar *_styles_build_label(gchar **split)
{
  if(!split || !split[0]) return g_strdup("");

  if(split[1])
  {
    GString *label = g_string_new(split[1]);
    for(int i = 2; split[i]; i++)
    {
      g_string_append(label, " | ");
      g_string_append(label, split[i]);
    }
    gchar *markup = g_markup_escape_text(label->str, -1);
    g_string_free(label, TRUE);
    return markup ? markup : g_strdup("");
  }

  gchar *markup = g_markup_escape_text(split[0], -1);
  return markup ? markup : g_strdup("");
}

static gchar *_styles_build_tooltip(const dt_style_t *style)
{
  char *items_string = dt_styles_get_item_list_as_string(style->name);
  gchar *tooltip = NULL;

  if(items_string && *items_string)
  {
    if(style->description && *style->description)
    {
      gchar *desc = g_markup_escape_text(style->description, -1);
      tooltip = g_strconcat("<b>", desc, "</b>\n", items_string, NULL);
      dt_free(desc);
    }
    else
    {
      tooltip = g_strdup(items_string);
    }
  }
  else if(style->description && *style->description)
  {
    tooltip = g_markup_escape_text(style->description, -1);
  }

  dt_free(items_string);
  return tooltip;
}

static GtkWidget *_styles_get_submenu(GtkWidget **menus, GList **lists, GHashTable *submenus,
                                      const gchar *group, const dt_menus_t index)
{
  if(!group || !*group) return menus[index];

  GtkWidget *submenu = g_hash_table_lookup(submenus, group);
  if(submenu) return submenu;

  gchar *group_label = g_markup_escape_text(group, -1);
  if(!group_label) group_label = g_strdup("");
  add_top_submenu_entry(menus, lists, group_label, index);
  dt_free(group_label);

  GtkWidget *menu_item = get_last_widget(lists);
  submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(menu_item));

  g_hash_table_insert(submenus, g_strdup(group), submenu);
  return submenu;
}

static void _styles_add_menu_entry(GtkWidget **menus, GList **lists, GtkWidget *parent, const dt_menus_t index,
                                   const gchar *label, const gchar *tooltip, const gchar *style_name)
{
  dt_menu_entry_t *entry = set_menu_entry(menus, lists, label, index, GTK_MENU(parent), NULL,
                                          _styles_apply_callback, NULL, NULL, has_active_images,
                                          0, 0, darktable.gui->accels->global_accels);

  gtk_menu_shell_append(GTK_MENU_SHELL(parent), entry->widget);
  gtk_menu_item_set_reserve_indicator(GTK_MENU_ITEM(entry->widget), TRUE);
  g_object_set_data_full(G_OBJECT(entry->widget), "custom-data", g_strdup(style_name), g_free);

  if(tooltip)
    gtk_widget_set_tooltip_markup(entry->widget, tooltip);
}

static void _styles_menu_clear(void)
{
  if(!_styles_menus || !_styles_lists) return;

  GtkWidget *menu = _styles_menus[_styles_index];
  GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
  for(GList *child = children; child; child = g_list_next(child))
    gtk_widget_destroy(GTK_WIDGET(child->data));
  g_list_free(children);
  children = NULL;

  if(*_styles_lists)
  {
    g_list_free_full(*_styles_lists, dt_free_gpointer);
    *_styles_lists = NULL;
  }
}

static gboolean _styles_menu_rebuild_idle(gpointer user_data)
{
  if(!_styles_menus || !_styles_lists)
  {
    _styles_menu_rebuild_source = 0;
    return G_SOURCE_REMOVE;
  }

  _styles_menu_clear();
  append_styles(_styles_menus, _styles_lists, _styles_index);
  gtk_widget_show_all(_styles_menus[_styles_index]);
  gtk_widget_queue_resize(_styles_menus[_styles_index]);

  _styles_menu_rebuild_source = 0;
  return G_SOURCE_REMOVE;
}

static void _styles_menu_rebuild_callback(gpointer instance, gpointer user_data)
{
  if(_styles_menu_rebuild_source == 0)
    _styles_menu_rebuild_source = g_idle_add(_styles_menu_rebuild_idle, NULL);
}


void append_styles(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  _styles_menus = menus;
  _styles_lists = lists;
  _styles_index = index;
  if(!_styles_signal_connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_STYLE_CHANGED,
                                    G_CALLBACK(_styles_menu_rebuild_callback), NULL);
    _styles_signal_connected = TRUE;
  }

  GList *styles = dt_styles_get_list("");

  if(!styles)
  {
    add_sub_menu_entry(menus, lists, _("No styles available"), index, NULL, NULL, NULL, NULL,
                       _styles_menu_disabled, 0, 0);
  }
  else
  {
    GHashTable *submenus = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);

    for(GList *iter = styles; iter; iter = g_list_next(iter))
    {
      dt_style_t *style = (dt_style_t *)iter->data;
      if(!style || !style->name) continue;

      gchar **split = g_strsplit(style->name, "|", 0);
      gchar *label = _styles_build_label(split);
      gchar *tooltip = _styles_build_tooltip(style);

      GtkWidget *parent_menu = menus[index];
      if(split[1])
        parent_menu = _styles_get_submenu(menus, lists, submenus, split[0], index);

      _styles_add_menu_entry(menus, lists, parent_menu, index, label, tooltip, style->name);

      dt_free(label);
      dt_free(tooltip);
      g_strfreev(split);
    }

    g_hash_table_destroy(submenus);
    g_list_free_full(styles, dt_style_free);
    styles = NULL;
  }

  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Create new style..."), index, NULL,
                     _styles_create_callback, NULL, NULL, _styles_create_sensitive_callback, 0, 0);

  add_sub_menu_entry(menus, lists, _("Manage styles..."), index, NULL,
                     _styles_open_popup_callback, NULL, NULL, NULL, 0, 0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
