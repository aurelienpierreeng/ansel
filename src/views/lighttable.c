/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.
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
/** this is the view for the lighttable module.  */

#include "common/extra_optimizations.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/focus_peaking.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"

#ifdef USE_LUA
#include "lua/image.h"
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
} dt_library_t;

const char *name(const dt_view_t *self)
{
  return _("Lighttable");
}


uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

#ifdef USE_LUA

static int set_image_visible_cb(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  dt_view_t *self = lua_touserdata(L, lua_upvalueindex(1));  //check were in lighttable view
  if(view(self) == DT_VIEW_LIGHTTABLE)
  {
    if(luaL_testudata(L, 1, "dt_lua_image_t"))
    {
      luaA_to(L, dt_lua_image_t, &imgid, 1);
      dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);
      return 0;
    }
    else
      return luaL_error(L, "no image specified");

  }
  else
    return luaL_error(L, "must be in lighttable view");
}

static gboolean is_image_visible_cb(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  dt_view_t *self = lua_touserdata(L, lua_upvalueindex(1));  //check were in lighttable view
  //check we are in file manager or zoomable
  if(view(self) == DT_VIEW_LIGHTTABLE)
  {
    //check we are in file manager or zoomable
    if(luaL_testudata(L, 1, "dt_lua_image_t"))
    {
      luaA_to(L, dt_lua_image_t, &imgid, 1);
      lua_pushboolean(L, dt_thumbtable_check_imgid_visibility(dt_ui_thumbtable(darktable.gui->ui), imgid));
      return 1;
    }
    else
      return luaL_error(L, "no image specified");
  }
  else
    return luaL_error(L, "must be in lighttable view");
}

#endif

void cleanup(dt_view_t *self)
{
  free(self->data);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const double start = dt_get_wtime();
  if(!darktable.collection || darktable.collection->count <= 0)
  {
    // thumbtable displays an help message
  }
  else // we do pass on expose to manager
  {
    if(!gtk_widget_get_visible(dt_ui_thumbtable(darktable.gui->ui)->widget))
      gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);

    // No filmstrip in file manager
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, FALSE);
  }

  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);
}

void enter(dt_view_t *self)
{
  dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), dt_ui_center_base(darktable.gui->ui),
                            DT_THUMBTABLE_MODE_FILEMANAGER);
  gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  dt_collection_hint_message(darktable.collection);
  dt_lib_set_visible(darktable.view_manager->proxy.filmstrip.module, FALSE); // not available in this layouts
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
  dt_ui_restore_panels(darktable.gui->ui);
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
  darktable.view_manager->proxy.lighttable.view = self;

  // ensure the memory table is up to date
  dt_collection_memory_update();

#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  const int my_type = dt_lua_module_entry_get_type(L, "view", self->module_name);

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, set_image_visible_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "set_image_visible");

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, is_image_visible_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "is_image_visible");
#endif
}

void leave(dt_view_t *self)
{
  // ensure we have no active image remaining
  if(darktable.view_manager->active_images)
  {
    g_slist_free(darktable.view_manager->active_images);
    darktable.view_manager->active_images = NULL;
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
  }

  // we remove the thumbtable from main view
  dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), NULL, DT_THUMBTABLE_MODE_NONE);
  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
  dt_control_set_mouse_over_id(-1);
}


void scrollbar_changed(dt_view_t *self, double x, double y)
{
  dt_thumbtable_scrollbar_changed(dt_ui_thumbtable(darktable.gui->ui), x, y);
}

enum
{
  DT_ACTION_ELEMENT_FOCUS_DETECT = 1,
};

const dt_action_element_def_t _action_elements_preview[]
  = { { "normal", dt_action_effect_hold },
      { "focus detection", dt_action_effect_hold },
      { NULL } };

enum
{
  DT_ACTION_ELEMENT_MOVE = 0,
  DT_ACTION_ELEMENT_SELECT = 1,
};

enum
{
  _ACTION_TABLE_MOVE_STARTEND = 0,
  _ACTION_TABLE_MOVE_LEFTRIGHT = 1,
  _ACTION_TABLE_MOVE_UPDOWN = 2,
  _ACTION_TABLE_MOVE_PAGE = 3,
  _ACTION_TABLE_MOVE_LEAVE = 4,
};

static float _action_process_move(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  if(isnan(move_size)) return 0; // FIXME return should be relative position

  int action = GPOINTER_TO_INT(target);

  // navigation accels for thumbtable layouts
  // this can't be "normal" key accels because it's usually arrow keys and lot of other widgets
  // will capture them before the usual accel is triggered
  dt_thumbtable_move_t move = DT_THUMBTABLE_MOVE_NONE;
  gboolean select = element == DT_ACTION_ELEMENT_SELECT;
  if(action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_PREVIOUS)
    move = DT_THUMBTABLE_MOVE_LEFT;
  else if(action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_NEXT)
    move = DT_THUMBTABLE_MOVE_UP;
  else if(action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_NEXT)
    move = DT_THUMBTABLE_MOVE_RIGHT;
  else if(action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_PREVIOUS)
    move = DT_THUMBTABLE_MOVE_DOWN;
  else if(action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_NEXT)
    move = DT_THUMBTABLE_MOVE_PAGEUP;
  else if(action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_PREVIOUS)
    move = DT_THUMBTABLE_MOVE_PAGEDOWN;
  else if(action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_PREVIOUS)
    move = DT_THUMBTABLE_MOVE_START;
  else if(action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_NEXT)
    move = DT_THUMBTABLE_MOVE_END;
  else if(action == _ACTION_TABLE_MOVE_LEAVE && effect == DT_ACTION_EFFECT_NEXT)
    move = DT_THUMBTABLE_MOVE_LEAVE;
  else
  {
    // MIDDLE
  }

  if(move != DT_THUMBTABLE_MOVE_NONE)
  {
    // for this layout navigation keys are managed directly by thumbtable
    dt_thumbtable_key_move(dt_ui_thumbtable(darktable.gui->ui), move, select);
    gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  }

  return 0; // FIXME return should be relative position
}

const gchar *_action_effect_move[]
  = { N_("middle"),
      N_("next"),
      N_("previous"),
      NULL };

const dt_action_element_def_t _action_elements_move[]
  = { { N_("move"  ), _action_effect_move },
      { N_("select"), _action_effect_move },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks_move[]
  = { { .mods = GDK_SHIFT_MASK, .element = DT_ACTION_ELEMENT_SELECT },
      { } };

const dt_action_def_t _action_def_move
  = { N_("move"),
      _action_process_move,
      _action_elements_move,
      _action_fallbacks_move,
      TRUE };

static void zoom_in_callback(dt_action_t *action)
{
  int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);

  zoom--;
  if(zoom < 1) zoom = 1;

  dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void zoom_out_callback(dt_action_t *action)
{
  int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);

  zoom++;
  if(zoom > 2 * DT_LIGHTTABLE_MAX_ZOOM) zoom = 2 * DT_LIGHTTABLE_MAX_ZOOM;

  dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void zoom_max_callback(dt_action_t *action)
{
  dt_view_lighttable_set_zoom(darktable.view_manager, 1);
}

static void zoom_min_callback(dt_action_t *action)
{
  dt_view_lighttable_set_zoom(darktable.view_manager, DT_LIGHTTABLE_MAX_ZOOM);
}

static void _accel_reset_first_offset(dt_action_t *action)
{
  dt_thumbtable_reset_first_offset(dt_ui_thumbtable(darktable.gui->ui));
}

static void _accel_select_toggle(dt_action_t *action)
{
  const int32_t id = dt_control_get_mouse_over_id();
  dt_selection_toggle(darktable.selection, id);
}

static void _accel_open_single(dt_action_t *action)
{
  const int32_t id = dt_control_get_mouse_over_id();
  dt_selection_select_single(darktable.selection, id);
  if(id > 0) dt_view_manager_switch(darktable.view_manager, "darkroom");
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;

  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0, _("open image in darkroom"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0, _("scroll the collection"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
                                      _("change number of images per row"));

  if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER)
    lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DRAG_DROP, GDK_BUTTON1_MASK, _("change image order"));

  return lm;
}

void gui_init(dt_view_t *self)
{
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_log_msg(darktable.gui->ui)), -1);
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_toast_msg(darktable.gui->ui)), -1);

  dt_action_t *sa = &self->actions, *ac = NULL;

  ac = dt_action_define(sa, N_("move"), N_("whole"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_STARTEND), &_action_def_move);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Home, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT    , GDK_KEY_End, 0);

  ac = dt_action_define(sa, N_("move"), N_("horizontal"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_LEFTRIGHT), &_action_def_move);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Left, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT    , GDK_KEY_Right, 0);

  ac = dt_action_define(sa, N_("move"), N_("vertical"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_UPDOWN), &_action_def_move);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Down, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT    , GDK_KEY_Up, 0);

  ac = dt_action_define(sa, N_("move"), N_("page"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_PAGE), &_action_def_move);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Page_Down, 0);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT    , GDK_KEY_Page_Up, 0);

  ac = dt_action_define(sa, N_("move"), N_("leave"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_LEAVE), &_action_def_move);
  dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT    , GDK_KEY_Escape, GDK_MOD1_MASK);

  dt_action_register(DT_ACTION(self), N_("reset first image offset"), _accel_reset_first_offset, 0, 0);
  dt_action_register(DT_ACTION(self), N_("select toggle image"), _accel_select_toggle, GDK_KEY_space, 0);
  dt_action_register(DT_ACTION(self), N_("open single image in darkroom"), _accel_open_single, GDK_KEY_Return, 0);

  // zoom in/out/min/max
  dt_action_register(DT_ACTION(self), N_("zoom in"), zoom_in_callback, GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom max"), zoom_max_callback, GDK_KEY_plus, GDK_MOD1_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom out"), zoom_out_callback, GDK_KEY_minus, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("zoom min"), zoom_min_callback, GDK_KEY_minus, GDK_MOD1_MASK);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
