/*
    This file is part of darktable,
    Copyright (C) 2009-2015 johannes hanika.
    Copyright (C) 2010-2012, 2014 Henrik Andersson.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010-2020 Tobias Ellinghaus.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Kanstantsin Shautsou.
    Copyright (C) 2011 Moritz Lipp.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011, 2013 Simon Spannagel.
    Copyright (C) 2012-2014 José Carlos García Sogo.
    Copyright (C) 2012, 2014-2015 Jérémy Rosen.
    Copyright (C) 2012 marcel.
    Copyright (C) 2012 Michal Babej.
    Copyright (C) 2012 Mika Boström.
    Copyright (C) 2012 Pascal de Bruijn.
    Copyright (C) 2012-2021 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013, 2016, 2019-2022 Aldric Renaudin.
    Copyright (C) 2013 Benjamin Cahill.
    Copyright (C) 2013 Gaspard Jankowiak.
    Copyright (C) 2013 Pierre Le Magourou.
    Copyright (C) 2013-2016, 2020 Roman Lebedev.
    Copyright (C) 2013-2014 Ronny Kahl.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2014 Pedro Côrte-Real.
    Copyright (C) 2016 Asma.
    Copyright (C) 2017, 2019, 2021 luzpaz.
    Copyright (C) 2017, 2019 Marcello Mamino.
    Copyright (C) 2017-2018 Matthieu Moy.
    Copyright (C) 2017, 2019-2020 parafin.
    Copyright (C) 2018 grand-piano.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019-2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 Felipe Contreras.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 Jacopo Guderzo.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2020 Miloš Komarčević.
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2021 Bill Ferguson.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2022 Sakari Kapanen.
    Copyright (C) 2023 Maurizio Paglia.
    Copyright (C) 2025 Guillaume Stutin.
    
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

#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/module.h"
#include "common/selection.h"
#include "common/undo.h"
#include "common/usermanual_url.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/thumbtable.h"

#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DECORATION_SIZE_LIMIT 40

static void dt_view_manager_load_modules(dt_view_manager_t *vm);
static int dt_view_load_module(void *v, const char *libname, const char *module_name);
static void dt_view_unload_module(dt_view_t *view);
static int32_t _view_surface_fetch_job_run(dt_job_t *job);

void dt_view_manager_init(dt_view_manager_t *vm)
{
  dt_view_manager_load_modules(vm);
  vm->current_view = NULL;
  vm->audio.audio_player_id = -1;
  vm->active_images = NULL;
}

void dt_view_manager_gui_init(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(view->gui_init) view->gui_init(view);
  }
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  g_list_free(vm->active_images);
  vm->active_images = NULL;
  for(GList *iter = vm->views; iter; iter = g_list_next(iter)) dt_view_unload_module((dt_view_t *)iter->data);
  g_list_free_full(vm->views, dt_free_gpointer);
  vm->views = NULL;
}

const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm)
{
  return vm->current_view;
}

// we want a stable order of views, for example for viewswitcher.
// anything not hardcoded will be put alphabetically wrt. localised names.
static gint sort_views(gconstpointer a, gconstpointer b)
{
  static const char *view_order[] = {"lighttable", "darkroom"};
  static const int n_view_order = G_N_ELEMENTS(view_order);

  dt_view_t *av = (dt_view_t *)a;
  dt_view_t *bv = (dt_view_t *)b;
  const char *aname = av->name(av);
  const char *bname = bv->name(bv);
  int apos = n_view_order;
  int bpos = n_view_order;

  for(int i = 0; i < n_view_order; i++)
  {
    if(!strcmp(av->module_name, view_order[i])) apos = i;
    if(!strcmp(bv->module_name, view_order[i])) bpos = i;
  }

  // order will be zero iff apos == bpos which can only happen when both views are not in view_order
  const int order = apos - bpos;
  return order ? order : strcmp(aname, bname);
}

static void dt_view_manager_load_modules(dt_view_manager_t *vm)
{
  vm->views = dt_module_load_modules("/views", sizeof(dt_view_t), dt_view_load_module, NULL, sort_views);
}

/* default flags for view which does not implement the flags() function */
static uint32_t default_flags()
{
  return 0;
}

/** load a view module */
static int dt_view_load_module(void *v, const char *libname, const char *module_name)
{
  dt_view_t *module = (dt_view_t *)v;
  g_strlcpy(module->module_name, module_name, sizeof(module->module_name));

#define INCLUDE_API_FROM_MODULE_LOAD "view_load_module"
#include "views/view_api.h"

  module->data = NULL;
  module->vscroll_size = module->vscroll_viewport_size = 1.0;
  module->hscroll_size = module->hscroll_viewport_size = 1.0;
  module->vscroll_pos = module->hscroll_pos = 0.0;
  module->height = module->width = 100; // set to non-insane defaults before first expose/configure.

#ifdef USE_LUA
  dt_lua_register_view(darktable.lua_state.state, module);
#endif

  if(module->init) module->init(module);

  return 0;
}

/** unload, cleanup */
static void dt_view_unload_module(dt_view_t *view)
{
  if(view->cleanup) view->cleanup(view);

  if(view->module) g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  if(GTK_IS_CONTAINER(data))
    gtk_container_remove(GTK_CONTAINER(data), widget);
}

/*
   When expanders get destroyed, they destroy the child
   so remove the child before that
   */
static void _remove_child(GtkWidget *child,GtkContainer *container)
{
  // some libs module can be used inside popups and not attached to panels, they have no container.
  if(DTGTK_IS_EXPANDER(child))
  {
    GtkWidget *evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(child));
    if(GTK_IS_CONTAINER(evb))
      gtk_container_remove(GTK_CONTAINER(evb), dtgtk_expander_get_body(DTGTK_EXPANDER(child)));
    gtk_widget_destroy(child);
  }
  else if(GTK_IS_CONTAINER(container))
  {
    gtk_container_remove(container, child);
  }
}

int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name)
{
  gboolean switching_to_none = *view_name == '\0';
  dt_view_t *new_view = NULL;

  if(!switching_to_none)
  {
    for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    {
      dt_view_t *v = (dt_view_t *)iter->data;
      if(!strcmp(v->module_name, view_name))
      {
        new_view = v;
        break;
      }
    }
    if(IS_NULL_PTR(new_view)) return 1; // the requested view doesn't exist
  }

  return dt_view_manager_switch_by_view(vm, new_view);
}

int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *nv)
{
  dt_view_t *old_view = vm->current_view;
  dt_view_t *new_view = (dt_view_t *)nv; // views belong to us, we can de-const them :-)

  // reset the cursor to the default one
  dt_control_change_cursor(GDK_LEFT_PTR);

  /* Reset Gtk focus */
  gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
  darktable.gui->has_scroll_focus = NULL;

  // also ignore what scrolling there was previously happening
  memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));

  // destroy old module list

  /*  clear the undo list, for now we do this unconditionally. At some point we will probably want to clear
     only part
      of the undo list. This should probably done with a view proxy routine returning the type of undo to
     remove. */
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);

  /* Special case when entering nothing (just before leaving dt) */
  if(IS_NULL_PTR(new_view))
  {
    if(old_view)
    {
      /* leave the current view*/
      if(old_view->leave) old_view->leave(old_view);

      /* iterator plugins and cleanup plugins in current view */
      for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
      {
        dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

        /* does this module belong to current view ?*/
        if(dt_lib_is_visible_in_view(plugin, old_view))
        {
          if(plugin->view_leave) plugin->view_leave(plugin, old_view, NULL);
          plugin->gui_cleanup(plugin);
          plugin->data = NULL;
          plugin->widget = NULL;
        }
      }
    }

    /* remove all widgets in all containers */
    dt_ui_cleanup_main_table(darktable.gui->ui);
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_destroy_children(darktable.gui->ui, l);
    vm->current_view = NULL;

    return 0;
  }

  // invariant: !IS_NULL_PTR(new_view) after this point
  assert(!IS_NULL_PTR(new_view));

  if(new_view->try_enter)
  {
    const int error = new_view->try_enter(new_view);
    if(error)
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE, old_view, new_view);
      return error;
    }
  }

  /* cleanup current view before initialization of new  */
  if(old_view)
  {
    /* leave current view */
    if(old_view->leave) old_view->leave(old_view);

    /* iterator plugins and cleanup plugins in current view */
    for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

      /* does this module belong to current view ?*/
      if(dt_lib_is_visible_in_view(plugin, old_view))
      {
        if(plugin->view_leave) plugin->view_leave(plugin, old_view, new_view);
      }
    }

    /* remove all widets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_foreach(darktable.gui->ui, l,(GtkCallback)_remove_child);
  }

  /* change current view to the new view */
  vm->current_view = new_view;

  /* restore visible stat of panels for the new view */
  dt_ui_restore_panels(darktable.gui->ui);

  /* lets add plugins related to new view into panels.
   * this has to be done in reverse order to have the lowest position at the bottom! */
  for(GList *iter = g_list_last(darktable.lib->plugins); iter; iter = g_list_previous(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {

      /* try get the module expander  */
      GtkWidget *w = dt_lib_gui_get_expander(plugin);

      /* if we didn't get an expander let's add the widget */
      if(IS_NULL_PTR(w)) w = plugin->widget;

      dt_gui_add_help_link(w, dt_get_help_url(plugin->plugin_name));
      // some plugins help links depend on the view
      if(!strcmp(plugin->plugin_name,"module_toolbox")
        || !strcmp(plugin->plugin_name,"view_toolbox"))
      {
        dt_view_type_flags_t view_type = new_view->view(new_view);
        if(view_type == DT_VIEW_LIGHTTABLE)
          dt_gui_add_help_link(w, dt_get_help_url("lighttable_mode"));
        if(view_type == DT_VIEW_DARKROOM)
          dt_gui_add_help_link(w, dt_get_help_url("darkroom_bottom_panel"));
      }


      /* add module to its container */
      dt_ui_container_add_widget(darktable.gui->ui, plugin->container(plugin), w);
    }
  }

  /* hide/show modules as last config */
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {
      /* set expanded if last mode was that */
      char var[1024];
      gboolean expanded = FALSE;
      gboolean visible = dt_lib_is_visible(plugin);
      if(plugin->expandable(plugin))
      {
        snprintf(var, sizeof(var), "plugins/%s/%s/expanded", new_view->module_name, plugin->plugin_name);
        expanded = dt_conf_get_bool(var);
        dt_lib_gui_set_expanded(plugin, expanded);
        dt_lib_set_visible(plugin, visible);
      }
      else
      {
        /* show/hide plugin widget depending on expanded flag or if plugin
            not is expandeable() */
        if(visible)
          gtk_widget_show_all(plugin->widget);
        else
          gtk_widget_hide(plugin->widget);
      }
      if(plugin->view_enter) plugin->view_enter(plugin, old_view, new_view);
    }
  }

  /* enter view. crucially, do this before initing the plugins below,
      as e.g. modulegroups requires the dr stuff to be inited. */
  if(new_view->enter) new_view->enter(new_view);

  /* raise view changed signal */
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, old_view, new_view);

  // update log visibility
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW);

  // update toast visibility
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW);
  return 0;
}

const char *dt_view_manager_name(dt_view_manager_t *vm)
{
  if(IS_NULL_PTR(vm->current_view)) return "";
  if(vm->current_view->name)
    return vm->current_view->name(vm->current_view);
  else
    return vm->current_view->module_name;
}

void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery)
{
  if(IS_NULL_PTR(vm->current_view))
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_BG);
    cairo_paint(cr);
    return;
  }
  vm->current_view->width = width;
  vm->current_view->height = height;

  if(vm->current_view->expose)
  {
    /* expose the view */
    cairo_rectangle(cr, 0, 0, vm->current_view->width, vm->current_view->height);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_save(cr);
    float px = pointerx, py = pointery;
    if(pointery > vm->current_view->height)
    {
      px = 10000.0;
      py = -1.0;
    }
    vm->current_view->expose(vm->current_view, cr, vm->current_view->width, vm->current_view->height, px, py);

    cairo_restore(cr);
    /* expose plugins */
    for(const GList *plugins = g_list_last(darktable.lib->plugins); plugins; plugins = g_list_previous(plugins))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      /* does this module belong to current view ?*/
      if(plugin->gui_post_expose
         && dt_lib_is_visible_in_view(plugin, vm->current_view))
        plugin->gui_post_expose(plugin, cr, vm->current_view->width, vm->current_view->height, px, py);
    }
  }
}

void dt_view_manager_reset(dt_view_manager_t *vm)
{
  if(IS_NULL_PTR(vm->current_view)) return;
  if(vm->current_view->reset) vm->current_view->reset(vm->current_view);
}

void dt_view_manager_mouse_leave(dt_view_manager_t *vm)
{
  if(IS_NULL_PTR(vm->current_view)) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_leave && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_leave(plugin)) handled = TRUE;
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_leave) v->mouse_leave(v);
}

void dt_view_manager_mouse_enter(dt_view_manager_t *vm)
{
  if(IS_NULL_PTR(vm->current_view)) return;
  if(vm->current_view->mouse_enter) vm->current_view->mouse_enter(vm->current_view);
}

void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which)
{
  if(IS_NULL_PTR(vm->current_view)) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_moved && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_moved(plugin, x, y, pressure, which)) handled = TRUE;
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_moved) v->mouse_moved(v, x, y, pressure, which);
}

int dt_view_manager_key_pressed(dt_view_manager_t *vm, GdkEventKey *event)
{
  if(IS_NULL_PTR(vm->current_view)) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->key_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->key_pressed(plugin, event)) handled = TRUE;
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->key_pressed)
    v->key_pressed(v, event);

  return 0;
}

int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(IS_NULL_PTR(vm->current_view)) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_released && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_released(plugin, x, y, which, state)) handled = TRUE;
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_released)
    v->button_released(v, x, y, which, state);

  return 0;
}

int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state)
{
  if(IS_NULL_PTR(vm->current_view)) return 0;
  dt_view_t *v = vm->current_view;

  /* Reset Gtk focus */
  gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
  darktable.gui->has_scroll_focus = NULL;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;

  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins && !handled;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_pressed(plugin, x, y, pressure, which, type, state)) handled = TRUE;
  }

  if(handled) return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_pressed)
    return v->button_pressed(v, x, y, pressure, which, type, state);

  return 0;
}

void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    // this is necessary for all
    dt_view_t *v = (dt_view_t *)iter->data;
    v->width = width;
    v->height = height;
    if(v->configure) v->configure(v, width, height);
  }
}

int dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state, int delta_y)
{
  if(IS_NULL_PTR(vm->current_view)) return FALSE;
  if(vm->current_view->scrolled)
    return vm->current_view->scrolled(vm->current_view, x, y, up, state, delta_y);
  return 0;
}

typedef struct dt_view_surface_fetch_job_t
{
  dt_view_image_surface_fetcher_t *fetcher;
  guint request_id;
  int32_t imgid;
  int width;
  int height;
  int zoom;
} dt_view_surface_fetch_job_t;

typedef struct dt_view_surface_fetch_commit_t
{
  dt_view_image_surface_fetcher_t *fetcher;
  guint request_id;
  int32_t imgid;
  int width;
  int height;
  int zoom;
  cairo_surface_t *surface;
  dt_view_surface_value_t result;
} dt_view_surface_fetch_commit_t;

static dt_view_surface_value_t _view_image_get_surface_internal(int32_t imgid, int width, int height,
                                                                cairo_surface_t **surface, int zoom,
                                                                dt_atomic_int *shutdown);

static void _destroy_surface(cairo_surface_t **surface)
{
  if(surface && *surface && cairo_surface_get_reference_count(*surface) > 0)
    cairo_surface_destroy(*surface);
  if(surface) *surface = NULL;
}

static gboolean _view_surface_matches(const dt_view_image_surface_fetcher_t *fetcher, cairo_surface_t **target,
                                      const int32_t imgid, const int width, const int height, const int zoom)
{
  return target && *target && fetcher->cached_imgid == imgid && fetcher->cached_width == width
         && fetcher->cached_height == height && fetcher->cached_zoom == zoom;
}

static void _enqueue_surface_fetch(dt_view_image_surface_fetcher_t *fetcher)
{
  dt_view_surface_fetch_job_t *params = g_malloc0(sizeof(dt_view_surface_fetch_job_t));
  params->fetcher = fetcher;
  params->request_id = fetcher->request_id;
  params->imgid = fetcher->imgid;
  params->width = fetcher->width;
  params->height = fetcher->height;
  params->zoom = fetcher->zoom;

  dt_job_t *job = dt_control_job_create(&_view_surface_fetch_job_run, "fetch image surface %i", params->imgid);
  if(IS_NULL_PTR(job))
  {
    g_free(params);
    return;
  }

  dt_atomic_set_int(&fetcher->shutdown, FALSE);
  dt_control_job_set_params_with_size(job, params, sizeof(dt_view_surface_fetch_job_t), g_free);
  fetcher->job_queued = TRUE;
  fetcher->queued_request_id = fetcher->request_id;
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, job);
}

static gboolean _view_surface_commit_main(gpointer user_data)
{
  dt_view_surface_fetch_commit_t *commit = (dt_view_surface_fetch_commit_t *)user_data;
  dt_view_image_surface_fetcher_t *fetcher = commit->fetcher;

  gboolean queue_redraw = FALSE;
  gboolean enqueue_next = FALSE;

  dt_pthread_mutex_lock(&fetcher->lock);
  fetcher->commit_pending = FALSE;
  pthread_cond_broadcast(&fetcher->cond);

  if(!fetcher->destroying && commit->request_id == fetcher->request_id && commit->result == DT_VIEW_SURFACE_OK)
  {
    _destroy_surface(fetcher->target);
    if(fetcher->target) *fetcher->target = commit->surface;
    commit->surface = NULL;
    fetcher->cached_imgid = commit->imgid;
    fetcher->cached_width = commit->width;
    fetcher->cached_height = commit->height;
    fetcher->cached_zoom = commit->zoom;
    queue_redraw = TRUE;
  }

  if(!fetcher->destroying && !fetcher->job_queued && fetcher->request_id != commit->request_id)
    enqueue_next = TRUE;
  dt_pthread_mutex_unlock(&fetcher->lock);

  if(commit->surface) cairo_surface_destroy(commit->surface);

  if(queue_redraw)
  {
    GtkWidget *widget = g_weak_ref_get(&fetcher->widget_ref);
    if(widget)
    {
      if(widget == dt_ui_center(darktable.gui->ui))
      {
        dt_control_queue_redraw_center();
      }
      else
      {
        gtk_widget_queue_draw(widget);
        GdkWindow *window = gtk_widget_get_window(widget);
        if(window)
        {
          G_GNUC_BEGIN_IGNORE_DEPRECATIONS
          gdk_window_process_updates(window, TRUE);
          G_GNUC_END_IGNORE_DEPRECATIONS
        }
      }
      g_object_unref(widget);
    }
  }

  if(enqueue_next)
  {
    dt_pthread_mutex_lock(&fetcher->lock);
    if(!fetcher->destroying && !fetcher->job_queued && !fetcher->commit_pending)
      _enqueue_surface_fetch(fetcher);
    dt_pthread_mutex_unlock(&fetcher->lock);
  }

  g_free(commit);
  return G_SOURCE_REMOVE;
}

static int32_t _view_surface_fetch_job_run(dt_job_t *job)
{
  dt_view_surface_fetch_job_t *params = dt_control_job_get_params(job);
  dt_view_image_surface_fetcher_t *fetcher = params->fetcher;

  dt_pthread_mutex_lock(&fetcher->lock);
  const gboolean stale = fetcher->destroying || params->request_id != fetcher->request_id;
  const gboolean cancelled = dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED;
  if(stale || cancelled)
  {
    if(fetcher->job_queued && fetcher->queued_request_id == params->request_id) fetcher->job_queued = FALSE;

    // A stale request means the widget parameters changed while the current
    // job was queued or running. If no newer request is already tracked
    // locally, start the latest one immediately from the current fetcher
    // state instead of trying to reason about queue-owned job objects.
    if(!fetcher->destroying && params->request_id != fetcher->request_id && !fetcher->job_queued
       && !fetcher->commit_pending)
      _enqueue_surface_fetch(fetcher);

    pthread_cond_broadcast(&fetcher->cond);
    dt_pthread_mutex_unlock(&fetcher->lock);
    return 0;
  }
  dt_pthread_mutex_unlock(&fetcher->lock);

  cairo_surface_t *surface = NULL;
  const dt_view_surface_value_t result =
      _view_image_get_surface_internal(params->imgid, params->width, params->height, &surface, params->zoom,
                                       &fetcher->shutdown);

  dt_view_surface_fetch_commit_t *commit = g_malloc0(sizeof(dt_view_surface_fetch_commit_t));
  commit->fetcher = fetcher;
  commit->request_id = params->request_id;
  commit->imgid = params->imgid;
  commit->width = params->width;
  commit->height = params->height;
  commit->zoom = params->zoom;
  commit->surface = surface;
  commit->result = result;

  dt_pthread_mutex_lock(&fetcher->lock);
  if(fetcher->job_queued && fetcher->queued_request_id == params->request_id) fetcher->job_queued = FALSE;
  fetcher->commit_pending = TRUE;
  pthread_cond_broadcast(&fetcher->cond);
  dt_pthread_mutex_unlock(&fetcher->lock);

  g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT, _view_surface_commit_main,
                             commit, NULL);
  g_main_context_wakeup(g_main_context_default());
  return 0;
}

void dt_view_image_surface_fetcher_init(dt_view_image_surface_fetcher_t *fetcher)
{
  memset(fetcher, 0, sizeof(dt_view_image_surface_fetcher_t));
  dt_pthread_mutex_init(&fetcher->lock, NULL);
  pthread_cond_init(&fetcher->cond, NULL);
  g_weak_ref_init(&fetcher->widget_ref, NULL);
  fetcher->cached_imgid = UNKNOWN_IMAGE;
  fetcher->imgid = UNKNOWN_IMAGE;
  dt_atomic_set_int(&fetcher->shutdown, FALSE);
}

void dt_view_image_surface_fetcher_cleanup(dt_view_image_surface_fetcher_t *fetcher)
{
  dt_pthread_mutex_lock(&fetcher->lock);
  fetcher->destroying = TRUE;
  dt_atomic_set_int(&fetcher->shutdown, TRUE);
  while(fetcher->job_queued)
    dt_pthread_cond_wait(&fetcher->cond, &fetcher->lock);
  dt_pthread_mutex_unlock(&fetcher->lock);

  for(;;)
  {
    dt_pthread_mutex_lock(&fetcher->lock);
    const gboolean pending = fetcher->commit_pending;
    dt_pthread_mutex_unlock(&fetcher->lock);
    if(!pending) break;
    g_main_context_iteration(g_main_context_default(), TRUE);
  }

  _destroy_surface(fetcher->target);
  g_weak_ref_clear(&fetcher->widget_ref);
  pthread_cond_destroy(&fetcher->cond);
  dt_pthread_mutex_destroy(&fetcher->lock);
}

void dt_view_image_surface_fetcher_invalidate(dt_view_image_surface_fetcher_t *fetcher, cairo_surface_t **target)
{
  dt_pthread_mutex_lock(&fetcher->lock);
  fetcher->target = target;
  fetcher->request_id++;
  dt_atomic_set_int(&fetcher->shutdown, TRUE);
  fetcher->cached_imgid = UNKNOWN_IMAGE;
  fetcher->cached_width = 0;
  fetcher->cached_height = 0;
  fetcher->cached_zoom = 0;
  dt_pthread_mutex_unlock(&fetcher->lock);

  _destroy_surface(target);
}

dt_view_surface_value_t dt_view_image_get_surface_async(dt_view_image_surface_fetcher_t *fetcher, int32_t imgid,
                                                        int width, int height, cairo_surface_t **target,
                                                        GtkWidget *widget, int zoom)
{
  if(IS_NULL_PTR(fetcher) || !target || !widget || width < 2 || height < 2 || imgid <= UNKNOWN_IMAGE)
    return DT_VIEW_SURFACE_KO;

  dt_view_surface_value_t ret = DT_VIEW_SURFACE_KO;

  dt_pthread_mutex_lock(&fetcher->lock);
  const gboolean changed = fetcher->target != target || fetcher->imgid != imgid || fetcher->width != width
                           || fetcher->height != height || fetcher->zoom != zoom;
  const gboolean exact_match = _view_surface_matches(fetcher, target, imgid, width, height, zoom);
  const gboolean fallback_match = target && *target && fetcher->cached_imgid == imgid
                                  && fetcher->cached_zoom == zoom;
  fetcher->target = target;
  fetcher->imgid = imgid;
  fetcher->width = width;
  fetcher->height = height;
  fetcher->zoom = zoom;
  g_weak_ref_set(&fetcher->widget_ref, widget);

  if(exact_match || fallback_match)
    ret = DT_VIEW_SURFACE_OK;

  if(changed && !exact_match && !fetcher->destroying && !fetcher->job_queued
     && !fetcher->commit_pending)
  {
    fetcher->request_id++;
    _enqueue_surface_fetch(fetcher);
  }
  else if(changed && !exact_match && !fetcher->destroying)
  {
    fetcher->request_id++;
    dt_atomic_set_int(&fetcher->shutdown, TRUE);
  }
  dt_pthread_mutex_unlock(&fetcher->lock);

  return ret;
}

cairo_surface_t *dt_cairo_rescale_surface(cairo_surface_t *src, int dst_w, int dst_h)
{
  if(!src || dst_w <= 0 || dst_h <= 0)
      return NULL;

  const int src_w = cairo_image_surface_get_width(src);
  const int src_h = cairo_image_surface_get_height(src);

  cairo_surface_t *dst =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, dst_w, dst_h);

  cairo_t *cr = cairo_create(dst);

  // clear (important if aspect ratio leaves borders)
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);

  double scale_x = (double)dst_w / src_w;
  double scale_y = (double)dst_h / src_h;

  double sx = scale_x;
  double sy = scale_y;
  double tx = 0.0;
  double ty = 0.0;


  double s = MIN(scale_x, scale_y);
  sx = sy = s;

  tx = (dst_w - src_w * s) * 0.5;
  ty = (dst_h - src_h * s) * 0.5;
  

  cairo_translate(cr, tx, ty);
  cairo_scale(cr, sx, sy);

  cairo_set_source_surface(cr, src, 0, 0);

  cairo_pattern_t *pat = cairo_get_source(cr);
  cairo_pattern_set_filter(pat, CAIRO_FILTER_BEST);
  cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);

  cairo_paint(cr);

  cairo_destroy(cr);

  return dst;
}

void dt_cairo_sharpen_surface_rgb24(cairo_surface_t *surface)
{
  if(!surface) return;

  cairo_surface_flush(surface);

  unsigned char *data = cairo_image_surface_get_data(surface);
  int width  = cairo_image_surface_get_width(surface);
  int height = cairo_image_surface_get_height(surface);
  int stride = cairo_image_surface_get_stride(surface);

  // Copy original buffer
  unsigned char *copy = dt_alloc_align(stride * height);
  memcpy(copy, data, stride * height);

  // Kernel weights
  const float k_center =  4.0f;
  const float k_edge   = -0.5f;
  const float k_corner = -0.25f;

  // Unsharp mask coeffs
  const float amount = 0.05f;
  const float amount_inv = 1.f - amount;

  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = 1; y < height - 1; y++)
    for(int x = 1; x < width - 1; x++)
    {
      int idx = y * stride + x * 4;

      #pragma unroll
      for(int c = 0; c < 3; c++) // B, G, R
      {
        int i = idx + c;

        float v =
          k_center * copy[i]

          + k_edge * (
              copy[i - 4] +                 // left
              copy[i + 4] +                 // right
              copy[i - stride] +            // top
              copy[i + stride]              // bottom
          )

          + k_corner * (
              copy[i - stride - 4] +        // top-left
              copy[i - stride + 4] +        // top-right
              copy[i + stride - 4] +        // bottom-left
              copy[i + stride + 4]          // bottom-right
          );

        // Unsharp-style recombination
        const float out = amount_inv * copy[i] + amount * v;
        data[i] = (unsigned char)(CLAMP(roundf(out), 0.f, 255.f));
      }

      data[idx + 3] = copy[idx + 3];
    }

  dt_free(copy);
  cairo_surface_mark_dirty(surface);
}

static dt_view_surface_value_t _view_image_get_surface_internal(int32_t imgid, int width, int height,
                                                                cairo_surface_t **surface, int zoom,
                                                                dt_atomic_int *shutdown)
{
  double tt = 0;
  if((darktable.unmuted & (DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF)) == (DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF))
    tt = dt_get_wtime();

  dt_view_surface_value_t ret = DT_VIEW_SURFACE_KO;

  // if surface not null, clean it up
  if(*surface && cairo_surface_get_reference_count(*surface) > 0)
    cairo_surface_destroy(*surface);
  *surface = NULL;

  // get mipmap cache image
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  dt_mipmap_size_t mip = DT_MIPMAP_NONE;

  if(zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    mip = dt_mipmap_cache_get_matching_size(cache, ceilf(width * darktable.gui->ppd), ceilf(height * darktable.gui->ppd), imgid);
  }
  else
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    const int full_width = image->width;
    const int full_height = image->height;
    dt_image_cache_read_release(darktable.image_cache, image);

    if(zoom == DT_THUMBTABLE_ZOOM_HALF)
      mip = dt_mipmap_cache_get_matching_size(cache, ceilf(full_width / 2.f ), ceilf(full_height / 2.f), imgid);
    else if(zoom >= DT_THUMBTABLE_ZOOM_FULL)
      mip = dt_mipmap_cache_get_matching_size(cache, full_width, full_height, imgid);
  }

  // Can't have float32 types here
  if(mip >= DT_MIPMAP_F) return DT_VIEW_SURFACE_KO;

  // if needed, we load the mimap buffer
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get_with_shutdown(cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r', shutdown);
  const int buf_wd = buf.width;
  const int buf_ht = buf.height;

  // if we don't get buffer, no image is available at the moment
  if(IS_NULL_PTR(buf.buf))
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return DT_VIEW_SURFACE_KO;
  }

  // so we create a new image surface to return
  float scale = 1.f;
  int img_width = buf_wd;
  int img_height = buf_ht;

  if(zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    scale = fminf((float)width / (float)buf_wd, (float)height / (float)buf_ht) * darktable.gui->ppd;
    img_width = roundf(buf_wd * scale);
    img_height = roundf(buf_ht * scale);

    // due to the forced rounding above, we need to recompute scaling
    scale = fmaxf((float)img_width / (float)buf_wd, (float)img_height / (float)buf_ht);
  }
  else if(zoom == DT_THUMBTABLE_ZOOM_TWICE)
  {
    // NOTE: we upscale the image surface, which means we will oversample
    // the full-res input buffer
    scale = 2.f;
    img_width = roundf(buf_wd * scale);
    img_height = roundf(buf_ht * scale);
  }

  *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, img_width, img_height);

  // we transfer cached image on a cairo_surface (with colorspace transform if needed)
  uint8_t *rgbbuf = (uint8_t *)calloc((size_t)buf_wd * buf_ht * 4, sizeof(uint8_t));
  if(IS_NULL_PTR(rgbbuf))
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return ret;
  }

  cmsHTRANSFORM transform = NULL;
  pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
  gboolean alloc = FALSE;

  // we only color manage when a thumbnail is sRGB or AdobeRGB. everything else just gets dumped to the
  // screen
  if(buf.color_space == DT_COLORSPACE_SRGB)
  {
    transform = darktable.color_profiles->transform_srgb_to_display;
  }
  else if(buf.color_space == DT_COLORSPACE_ADOBERGB)
  {
    transform = darktable.color_profiles->transform_adobe_rgb_to_display;
  }
  else if(buf.color_space == DT_COLORSPACE_DISPLAY)
  {
    // no-op, buffer is already in display space, pass pixels through
    // and simply swap R <-> B, which happens because transform = NULL
  }
  else
  {
    alloc = TRUE;
    transform = cmsCreateTransform(
        dt_colorspaces_get_profile(buf.color_space, "", DT_PROFILE_DIRECTION_DISPLAY)->profile, TYPE_RGBA_8, 
        dt_colorspaces_get_profile(DT_COLORSPACE_DISPLAY, "", DT_PROFILE_DIRECTION_DISPLAY)->profile, TYPE_BGRA_8,
        INTENT_PERCEPTUAL, 0);
  }

  dt_colorspaces_transform_rgba8_to_bgra8(transform, buf.buf, rgbbuf, buf.width, buf.height);
  if(alloc) cmsDeleteTransform(transform);
  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_wd);
  cairo_surface_t *tmp_surface = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf_wd, buf_ht, stride);
  if(IS_NULL_PTR(tmp_surface))
  {
    dt_free(rgbbuf);
    return ret;
  }

  // draw the image scaled:
  cairo_t *cr = cairo_create(*surface);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, tmp_surface, 0, 0);

  // set filter no nearest:
  // in skull mode, we want to see big pixels.
  // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
  // in between, filtering just makes stuff go unsharp.
  if((buf_wd <= 8 && buf_ht <= 8)
      || fabsf(scale - 1.0f) < 0.01f
      || zoom == DT_THUMBTABLE_ZOOM_TWICE)
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  else
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);

  cairo_paint(cr);
  cairo_surface_destroy(tmp_surface);
  cairo_destroy(cr);

  /* The async/shared surface path returns pixel-sized Cairo image surfaces.
   * Publish the widget PPD on the finished surface so GUI callers can place it
   * in logical coordinates without re-deriving HiDPI scaling on every draw. */
  cairo_surface_set_device_scale(*surface, darktable.gui->ppd, darktable.gui->ppd);

  // we consider skull as ok as the image hasn't to be reloaded
  if(buf_wd <= 8 && buf_ht <= 8)
  {
    ret = DT_VIEW_SURFACE_OK;
  }
  else
  {
    ret = DT_VIEW_SURFACE_OK;
    //dt_cairo_sharpen_surface_rgb24(*surface);
  }

  dt_free(rgbbuf);

  // logs
  if((darktable.unmuted & (DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF)) == (DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF))
  {
    dt_print(DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF,
             "[dt_view_image_get_surface]  id %i, mip code %i, dots %ix%i, mip %ix%i, surf %ix%i created in %0.04f sec\n",
             imgid, mip, width, height, buf_wd, buf_ht, img_width, img_height, dt_get_wtime() - tt);
  }
  else if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dt_view_image_get_surface]  id %i, mip code %i, dots %ix%i, mip %ix%i, surf %ix%i\n", imgid, mip,
             width, height, buf_wd, buf_ht, img_width, img_height);
  }

  // we consider skull as ok as the image hasn't to be reload
  return ret;
}

dt_view_surface_value_t dt_view_image_get_surface(int32_t imgid, int width, int height, cairo_surface_t **surface,
                                                  int zoom)
{
  return _view_image_get_surface_internal(imgid, width, height, surface, zoom, NULL);
}

char* dt_view_extend_modes_str(const char * name, const gboolean is_hdr, const gboolean is_bw, const gboolean is_bw_flow)
{
  char* upcase = g_ascii_strup(name, -1);  // extension in capital letters to avoid character descenders
  // convert to canonical format extension
  if(0 == g_ascii_strcasecmp(upcase, "JPG"))
  {
      gchar* canonical = g_strdup("JPEG");
      dt_free(upcase);
      upcase = canonical;
  }
  else if(0 == g_ascii_strcasecmp(upcase, "HDR"))
  {
      gchar* canonical = g_strdup("RGBE");
      dt_free(upcase);
      upcase = canonical;
  }
  else if(0 == g_ascii_strcasecmp(upcase, "TIF"))
  {
      gchar* canonical = g_strdup("TIFF");
      dt_free(upcase);
      upcase = canonical;
  }

  if(is_hdr)
  {
    gchar* fullname = g_strdup_printf("%s HDR", upcase);
    dt_free(upcase);
    upcase = fullname;
  }
  if(is_bw)
  {
    gchar* fullname = g_strdup_printf("%s B&W", upcase);
    dt_free(upcase);
    upcase = fullname;
    if(!is_bw_flow)
    {
      fullname = g_strdup_printf("%s-", upcase);
      dt_free(upcase);
      upcase = fullname;
    }
  }

  return upcase;
}


void dt_view_active_images_reset(gboolean raise)
{
  if(IS_NULL_PTR(darktable.view_manager->active_images)) return;
  g_list_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  if(raise) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

void dt_view_active_images_add(int32_t imgid, gboolean raise)
{
  darktable.view_manager->active_images
      = g_list_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  if(raise)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

void dt_view_active_images_remove(int32_t imgid, gboolean raise)
{
  GList *link = g_list_find(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  if(link)
  {
    darktable.view_manager->active_images = g_list_delete_link(darktable.view_manager->active_images, link);

    if(raise)
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
  }
}

gboolean dt_view_active_images_has_imgid(int32_t imgid)
{
  return g_list_find(dt_view_active_images_get_all(), GINT_TO_POINTER(imgid)) != NULL;
}

GList *dt_view_active_images_get_all()
{
  return darktable.view_manager->active_images;
}

int32_t dt_view_active_images_get_first()
{
  if(IS_NULL_PTR(darktable.view_manager->active_images)) return -1;
  return GPOINTER_TO_INT(darktable.view_manager->active_images->data);
}

void dt_view_active_images_set(GList *images, gboolean raise)
{
  darktable.view_manager->active_images = images;

  if(raise)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.module_toolbox.module)
    vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module, tool, views);
}

dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.darkroom.view)
    return vm->proxy.darkroom.get_layout(vm->proxy.darkroom.view);
  else
    return DT_DARKROOM_LAYOUT_EDITING;
}

#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom)
{
  if(vm->proxy.map.view)
    vm->proxy.map.center_on_location(vm->proxy.map.view, lon, lat, zoom);
}

void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2)
{
  if(vm->proxy.map.view)
    vm->proxy.map.center_on_bbox(vm->proxy.map.view, lon1, lat1, lon2, lat2);
}

void dt_view_map_show_osd(const dt_view_manager_t *vm)
{
  if(vm->proxy.map.view)
    vm->proxy.map.show_osd(vm->proxy.map.view);
}

void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source)
{
  if(vm->proxy.map.view)
    vm->proxy.map.set_map_source(vm->proxy.map.view, map_source);
}

GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points)
{
  if(vm->proxy.map.view)
    return vm->proxy.map.add_marker(vm->proxy.map.view, type, points);
  return NULL;
}

gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker)
{
  if(vm->proxy.map.view)
    return vm->proxy.map.remove_marker(vm->proxy.map.view, type, marker);
  return FALSE;
}
void dt_view_map_add_location(const dt_view_manager_t *vm, dt_map_location_data_t *p, const guint posid)
{
  if(vm->proxy.map.view)
    vm->proxy.map.add_location(vm->proxy.map.view, p, posid);
}

void dt_view_map_location_action(const dt_view_manager_t *vm, const int action)
{
  if(vm->proxy.map.view)
    vm->proxy.map.location_action(vm->proxy.map.view, action);
}

void dt_view_map_redraw(const dt_view_manager_t *vm)
{
  if(vm->proxy.map.view)
    vm->proxy.map.redraw(vm->proxy.map.view);
}

#endif

#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo, dt_images_box *imgs)
{
  if (vm->proxy.print.view)
    vm->proxy.print.print_settings(vm->proxy.print.view, pinfo, imgs);
}
#endif


static void _audio_child_watch(GPid pid, gint status, gpointer data)
{
  dt_view_manager_t *vm = (dt_view_manager_t *)data;
  vm->audio.audio_player_id = -1;
  g_spawn_close_pid(pid);
}

void dt_view_audio_start(dt_view_manager_t *vm, int32_t imgid)
{
  char *player = dt_conf_get_string("plugins/lighttable/audio_player");
  if(player && *player)
  {
    char *filename = dt_image_get_audio_path(imgid);
    if(filename)
    {
      char *argv[] = { player, filename, NULL };
      gboolean ret = g_spawn_async(NULL, argv, NULL,
                                   G_SPAWN_DO_NOT_REAP_CHILD
                                   | G_SPAWN_SEARCH_PATH
                                   | G_SPAWN_STDOUT_TO_DEV_NULL
                                   | G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL,
                                   &vm->audio.audio_player_pid, NULL);

      if(ret)
      {
        vm->audio.audio_player_id = imgid;
        vm->audio.audio_player_event_source
            = g_child_watch_add(vm->audio.audio_player_pid, (GChildWatchFunc)_audio_child_watch, vm);
      }
      else
        vm->audio.audio_player_id = -1;

      dt_free(filename);
    }
  }
  dt_free(player);
}

void dt_view_audio_stop(dt_view_manager_t *vm)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(vm->audio.audio_player_id == -1)
    return;

  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(vm->audio.audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(vm->audio.audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(vm->audio.audio_player_pid))
      kill(-vm->audio.audio_player_pid, SIGKILL);
    else
      kill(vm->audio.audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(vm->audio.audio_player_pid);
  vm->audio.audio_player_id = -1;
}


void dt_view_image_info_update(int32_t imgid)
{
  darktable.view_manager->image_info_id = imgid;

  if(imgid == UNKNOWN_IMAGE)
  {
    dt_ui_set_image_info_label(darktable.gui->ui, "");
    return;
  }

  char input_dir[512] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid,  input_dir,  sizeof(input_dir),  &from_cache, __FUNCTION__);

  dt_variables_params_t *vp;
  dt_variables_params_init(&vp);

  vp->filename = input_dir;
  vp->jobcode = "infos";
  vp->imgid = imgid;
  vp->sequence = 0;
  vp->escape_markup = TRUE;

  gchar *pattern = dt_conf_get_string("plugins/darkroom/image_infos_pattern");
  gchar *msg = dt_variables_expand(vp, pattern, TRUE);
  dt_free(pattern);
  dt_variables_params_destroy(vp);

  dt_ui_set_image_info_label(darktable.gui->ui, msg);

  dt_free(msg);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
