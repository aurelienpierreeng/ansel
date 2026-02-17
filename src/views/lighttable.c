/*
    This file is part of darktable,
    Copyright (C) 2009-2015 johannes hanika.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2018 Tobias Ellinghaus.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Jérémy Rosen.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011 Simon Spannagel.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012-2014 José Carlos García Sogo.
    Copyright (C) 2012 Petr Styblo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Ulrich Pegelow.
    Copyright (C) 2013 Eckhart Pedersen.
    Copyright (C) 2013 Jochem Kossen.
    Copyright (C) 2013-2016, 2018-2021 Pascal Obry.
    Copyright (C) 2013 Pierre Le Magourou.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2014-2015, 2019-2022 Aldric Renaudin.
    Copyright (C) 2014 Matthias Gehre.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2014 moopmonster.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2015 Jan Kundrát.
    Copyright (C) 2015 JohnnyRun.
    Copyright (C) 2016 Asma.
    Copyright (C) 2017 Dan Torop.
    Copyright (C) 2017 itinerarium.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2017, 2019 Marcello Mamino.
    Copyright (C) 2017 Matthieu Moy.
    Copyright (C) 2017 parafin.
    Copyright (C) 2017-2018 Peter Budai.
    Copyright (C) 2018 Frederic Chanal.
    Copyright (C) 2018-2019 Heiko Bauke.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018-2019 Rikard Öxler.
    Copyright (C) 2019-2020, 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019, 2022 Philippe Weyland.
    Copyright (C) 2019 Sam Smith.
    Copyright (C) 2019 vacaboja.
    Copyright (C) 2020 Bill Ferguson.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 EdgarLux.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2021 domosbg.
    Copyright (C) 2021 Fabio Heer.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Sakari Kapanen.
    Copyright (C) 2022 solarer.
    
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
  dt_lua_image_t imgid = UNKNOWN_IMAGE;
  dt_view_t *self = lua_touserdata(L, lua_upvalueindex(1));  //check were in lighttable view
  if(view(self) == DT_VIEW_LIGHTTABLE)
  {
    if(luaL_testudata(L, 1, "dt_lua_image_t"))
    {
      luaA_to(L, dt_lua_image_t, &imgid, 1);
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
  dt_lua_image_t imgid = UNKNOWN_IMAGE;
  dt_view_t *self = lua_touserdata(L, lua_upvalueindex(1));  //check were in lighttable view
  //check we are in file manager or zoomable
  if(view(self) == DT_VIEW_LIGHTTABLE)
  {
    //check we are in file manager or zoomable
    if(luaL_testudata(L, 1, "dt_lua_image_t"))
    {
      luaA_to(L, dt_lua_image_t, &imgid, 1);
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


static void _view_lighttable_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid > UNKNOWN_IMAGE)
  {
    dt_view_manager_switch(darktable.view_manager, "darkroom");
  }
}

void configure(dt_view_t *self, int width, int height)
{
  dt_thumbtable_t *table = darktable.gui->ui->thumbtable_lighttable;
  dt_thumbtable_set_active_rowid(table);
  dt_thumbtable_redraw(table);
  g_idle_add((GSourceFunc)dt_thumbtable_scroll_to_active_rowid, table);
}


void enter(dt_view_t *self)
{
  dt_view_active_images_reset(FALSE);

  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);
  dt_gui_refocus_center();
  dt_collection_hint_message(darktable.collection);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);

  // Attach shortcuts
  dt_accels_connect_accels(darktable.gui->accels);
  dt_accels_connect_active_group(darktable.gui->accels, "lighttable");

  gtk_widget_hide(dt_ui_center(darktable.gui->ui));
  dt_thumbtable_show(darktable.gui->ui->thumbtable_lighttable);
  dt_thumbtable_update_parent(darktable.gui->ui->thumbtable_lighttable);

  /* connect signal for thumbnail image activate */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_lighttable_activate_callback), self);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
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
  // Detach shortcuts
  dt_accels_disconnect_active_group(darktable.gui->accels);

  // ensure we have no active image remaining
  dt_view_active_images_reset(FALSE);

  dt_thumbtable_hide(darktable.gui->ui->thumbtable_lighttable);
  gtk_widget_show(dt_ui_center(darktable.gui->ui));

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_lighttable_activate_callback),
                                     (gpointer)self);
}

void reset(dt_view_t *self)
{
  dt_control_set_mouse_over_id(-1);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
