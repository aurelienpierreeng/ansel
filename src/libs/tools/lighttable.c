/*
    This file is part of darktable,
    Copyright (C) 2011, 2013 Henrik Andersson.
    Copyright (C) 2012 Petr Styblo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2013 johannes hanika.
    Copyright (C) 2013 Pascal de Bruijn.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019, 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2019, 2021 Bill Ferguson.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <gdk/gdkkeysyms.h>

#include "common/collection.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/togglebutton.h"
#include "gui/actions/menu.h"

#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *columns;
  GList *menu_items;
} dt_lib_tool_lighttable_t;

/* set columns proxy function */
static void _lib_lighttable_set_columns(dt_lib_module_t *self, gint columns);

/* columns slider change callback */
static void _lib_lighttable_columns_slider_changed(GtkWidget *widget, gpointer user_data);

static void _set_columns(dt_lib_module_t *self, int columns);

const char *name(struct dt_lib_module_t *self)
{
  return _("lighttable");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

gboolean _columns_in_action(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                         GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  int current_level = dt_conf_get_int("plugins/lighttable/images_in_row");
  int new_level = CLAMP(current_level - 1, 1, 12);
  _lib_lighttable_set_columns(self, new_level);
  dt_conf_set_int("plugins/lighttable/images_in_row_backup", new_level);
  return TRUE;
}

gboolean _columns_out_action(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                          GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  int current_level = dt_conf_get_int("plugins/lighttable/images_in_row");
  int new_level = CLAMP(current_level + 1, 1, 12);
  _lib_lighttable_set_columns(self, new_level);
  dt_conf_set_int("plugins/lighttable/images_in_row_backup", new_level);
  return TRUE;
}

static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change,
                                            dt_collection_properties_t changed_property, gpointer imgs,
                                            const int next, gpointer user_data)
{
  if(!user_data) return;
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  if(darktable.gui->culling_mode)
  {
    int current_level = dt_conf_get_int("plugins/lighttable/images_in_row");
    int num_images = dt_collection_get_count(darktable.collection);

    switch(num_images)
    {
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        _lib_lighttable_set_columns(self, num_images);
        dt_conf_set_int("plugins/lighttable/images_in_row_backup", current_level);
        break;
      case 6:
        _lib_lighttable_set_columns(self, 3);
        dt_conf_set_int("plugins/lighttable/images_in_row_backup", current_level);
        break;
      case 7:
      case 8:
        _lib_lighttable_set_columns(self, 4);
        dt_conf_set_int("plugins/lighttable/images_in_row_backup", current_level);
        break;
      case 9:
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
      case 15:
        _lib_lighttable_set_columns(self, 5);
        dt_conf_set_int("plugins/lighttable/images_in_row_backup", current_level);
        break;
      default:
        if(dt_conf_key_exists("plugins/lighttable/images_in_row_backup"))
          _lib_lighttable_set_columns(self, dt_conf_get_int("plugins/lighttable/images_in_row_backup"));
    }
  }
  else if(dt_conf_key_exists("plugins/lighttable/images_in_row_backup"))
  {
    _lib_lighttable_set_columns(self, dt_conf_get_int("plugins/lighttable/images_in_row_backup"));
  }

  // Reset zoom
  dt_thumbtable_set_zoom(darktable.gui->ui->thumbtable_lighttable, 0);
}

static gboolean _zoom_combobox_changed(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  const int level = GPOINTER_TO_INT(get_custom_data(GTK_WIDGET(user_data)));
  dt_thumbtable_set_zoom(darktable.gui->ui->thumbtable_lighttable, level);
  return TRUE;
}

static gboolean _zoom_checked(GtkWidget *widget)
{
  const int level = GPOINTER_TO_INT(get_custom_data(widget));
  return dt_thumbtable_get_zoom(darktable.gui->ui->thumbtable_lighttable) == level;
}


// Ctrl + Scroll changes the number of columns
static gboolean _thumbtable_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;

  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    int scroll_y;
    dt_gui_get_scroll_unit_deltas(event, NULL, &scroll_y);

    int current_level = dt_conf_get_int("plugins/lighttable/images_in_row");
    int new_level = CLAMP(current_level + CLAMP(scroll_y, -1, 1), 1, 12);

    _lib_lighttable_set_columns(self, new_level);
    dt_conf_set_int("plugins/lighttable/images_in_row_backup", new_level);
    return TRUE;
  }
  return FALSE;
}

static gboolean _focus_toggle_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  gboolean state = dt_thumbtable_get_focus_regions(table);
  dt_thumbtable_set_focus_regions(table, !state);
  return TRUE;
}

gboolean _focus_checked(GtkWidget *widget)
{
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  return dt_thumbtable_get_focus_regions(table);
}

static gboolean focus_peaking_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  gboolean focus_peaking = dt_thumbtable_get_focus_peaking(table);
  dt_thumbtable_set_focus_peaking(table, !focus_peaking);
  return TRUE;
}

static gboolean focus_peaking_checked_callback()
{
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  return dt_thumbtable_get_focus_peaking(table);
}

void append_thumbnails(GtkWidget **menus, GList **lists, const dt_menus_t index, GtkAccelGroup *accel_group)
{
  // Focusing options
  add_generic_sub_menu_entry(menus, lists, _("Overlay focus zones"), index, NULL, _focus_toggle_callback, _focus_checked, NULL,
                             NULL, 0, 0, accel_group);

  add_generic_sub_menu_entry(menus, lists, _("Overlay focus peaking"), index, NULL, focus_peaking_callback,
                             focus_peaking_checked_callback, NULL, NULL, GDK_KEY_p,
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK, accel_group);

  // Zoom
  add_generic_top_submenu_entry(menus, lists, _("Zoom"), index, accel_group);
  GtkWidget *parent = get_last_widget(lists);
  add_generic_sub_sub_menu_entry(menus, parent, lists, _("Fit"), index, GINT_TO_POINTER(0), _zoom_combobox_changed,
                                 _zoom_checked, NULL, NULL, 0, 0, accel_group);
  add_generic_sub_sub_menu_entry(menus, parent, lists, _("50 %"), index, GINT_TO_POINTER(1), _zoom_combobox_changed,
                                 _zoom_checked, NULL, NULL, 0, 0, accel_group);
  add_generic_sub_sub_menu_entry(menus, parent, lists, _("100 %"), index, GINT_TO_POINTER(2), _zoom_combobox_changed,
                                 _zoom_checked, NULL, NULL, 0, 0, accel_group);
  add_generic_sub_sub_menu_entry(menus, parent, lists, _("200 %"), index, GINT_TO_POINTER(3), _zoom_combobox_changed,
                                 _zoom_checked, NULL, NULL, 0, 0, accel_group);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)g_malloc0(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(self->widget, "lighttable_box");
  gtk_widget_set_halign(self->widget, GTK_ALIGN_END);
  gtk_widget_set_hexpand(self->widget, FALSE);

  // Thumbnail menu
  GtkAccelGroup *accel_group = darktable.gui->accels->lighttable_accels;
  GtkWidget *menu_bar = gtk_menu_bar_new();
  GtkWidget *menus[1];
  const int index = 0;
  d->menu_items = NULL;
  add_generic_top_menu_entry(menu_bar, menus, &d->menu_items, index, _("_Thumbnails"), accel_group, "Lighttable");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(menu_bar), FALSE, FALSE, 0);
  append_thumbnails(menus, &d->menu_items, index, accel_group);

  // dumb empty flexible spacer at the end
  GtkWidget *spacer = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(self->widget), spacer, TRUE, TRUE, 0);

  GtkWidget *label = gtk_label_new(C_("quickfilter", "Columns"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(label), FALSE, FALSE, 0);

  d->columns = gtk_spin_button_new_with_range(1., 12., 1.);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->columns), FALSE, FALSE, 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->columns), dt_conf_get_int("plugins/lighttable/images_in_row"));
  dt_accels_disconnect_on_text_input(d->columns);

  g_signal_connect(G_OBJECT(d->columns), "value-changed", G_CALLBACK(_lib_lighttable_columns_slider_changed), self);

  dt_accels_new_lighttable_action(_columns_in_action, self, N_("Lighttable/Actions"), N_("Zoom in the thumbtable grid"),
                                  GDK_KEY_plus, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_lighttable_action(_columns_out_action, self, N_("Lighttable/Actions"), N_("Zoom out the thumbtable grid"),
                                  GDK_KEY_minus, GDK_CONTROL_MASK, _("Triggers the action"));

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_dt_collection_changed_callback), self);

  _lib_lighttable_columns_slider_changed(d->columns, self); // the slider defaults to 1 and GTK doesn't
                                                      // fire a value-changed signal when setting
                                                      // it to 1 => empty text box

  // Wire a scroll event handler on thumbtable here. This avoids us a proxy
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  g_signal_connect(G_OBJECT(table->scroll_window), "scroll-event", G_CALLBACK(_thumbtable_scroll), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_changed_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static void _set_columns(dt_lib_module_t *self, int columns)
{
  dt_conf_set_int("plugins/lighttable/images_in_row", columns);
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  dt_thumbtable_set_active_rowid(table);
  dt_thumbtable_redraw(table);
  g_idle_add((GSourceFunc) dt_thumbtable_scroll_to_active_rowid, table);
}

static void _lib_lighttable_columns_slider_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  const int cols = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->columns));
  _set_columns(self, cols);
  dt_conf_set_int("plugins/lighttable/images_in_row_backup", cols);
}

static void _lib_lighttable_set_columns(dt_lib_module_t *self, gint columns)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->columns), columns);
  _set_columns(self, columns);
}

#ifdef USE_LUA

static int columns_level_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const gint tmp = dt_conf_get_int("plugins/lighttable/images_in_row");
  if(lua_gettop(L) > 0){
    int value;
    luaA_to(L, int, &value, 1);
    _lib_lighttable_set_columns(self, value);
  }
  luaA_push(L, int, &tmp);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, columns_level_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "zoom_level");
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
