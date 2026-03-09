/*
    This file is part of darktable,
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011-2012, 2014 johannes hanika.
    Copyright (C) 2012, 2014 Jérémy Rosen.
    Copyright (C) 2012 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013 Simon Spannagel.
    Copyright (C) 2012, 2014-2019 Tobias Ellinghaus.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018-2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2022 Aldric Renaudin.
    Copyright (C) 2019-2021, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019, 2021 Hanno Schwalm.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2022 Nicolas Auffray.
    Copyright (C) 2020 parafin.
    Copyright (C) 2020 Sergey Salnikov.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Marco.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Luca Zulberti.
    
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


#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

#include "modulegroups.h"

typedef struct dt_lib_modulegroups_t
{
  uint32_t current;
  GtkWidget *notebook;
} dt_lib_modulegroups_t;

static dt_lib_module_t *g_modulegroups_module = NULL;
static dt_lib_modulegroups_t *g_modulegroups_data = NULL;

/* toggle button callback */
static void _lib_modulegroups_toggle(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
/* helper function to update iop module view depending on group */
static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self);

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data);

static gboolean _focus_next_module();
static gboolean _focus_previous_module();
static gboolean _focus_next_control();
static gboolean _focus_previous_control();
static gboolean _is_module_in_history(const dt_iop_module_t *module);

const char *name(struct dt_lib_module_t *self)
{
  return _("modulegroups");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "darkroom", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}


/* this module should always be shown without expander */
int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 999;
}

int dt_iop_get_group(const dt_iop_module_t *module)
{
  return 1 << (module->default_group());
}

int _modulegroups_cycle_tabs(int user_set_group)
{
  int group;
  if(user_set_group < 0)
  {
    // cycle to the end
    group = DT_MODULEGROUP_SIZE - 1;
  }
  else if(user_set_group >= DT_MODULEGROUP_SIZE)
  {
    // cycle to the beginning
    group = 0;
  }
  else
  {
    group = user_set_group;
  }
  return group;
}

static uint32_t _modulegroups_get_current_group()
{
  if(g_modulegroups_data && g_modulegroups_data->current < DT_MODULEGROUP_SIZE)
    return g_modulegroups_data->current;

  return DT_MODULEGROUP_ACTIVE_PIPE;
}

static void _modulegroups_set_current_group(uint32_t group)
{
  if(!g_modulegroups_module || !g_modulegroups_data) return;
  if(group >= DT_MODULEGROUP_SIZE) return;
  if(g_modulegroups_data->current == group) return;

  g_modulegroups_data->current = group;
  if(GTK_IS_NOTEBOOK(g_modulegroups_data->notebook))
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_modulegroups_data->notebook), group);

  _lib_modulegroups_update_iop_visibility(g_modulegroups_module);
}

static gboolean _modulegroups_switch_tab_next(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                              GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(!dev) return FALSE;

  dt_iop_module_t *focused = dev->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  uint32_t current = _modulegroups_get_current_group();
  _modulegroups_set_current_group(_modulegroups_cycle_tabs(current + 1));
  dt_iop_request_focus(NULL);
  return TRUE;
}

static gboolean _modulegroups_switch_tab_previous(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                                  GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(!dev) return FALSE;

  dt_iop_module_t *focused = dev->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  uint32_t current = _modulegroups_get_current_group();
  _modulegroups_set_current_group(_modulegroups_cycle_tabs(current - 1));
  dt_iop_request_focus(NULL);

  return TRUE;
}

static gboolean _lib_modulegroups_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_x, delta_y;

  // We will accumulate scrolls here
  static int scrolls = 0;

  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    int current = _modulegroups_get_current_group();
    int future = 0;
    if(delta_x > 0. || delta_y > 0.)
      future = current + 1;
    else if(delta_x < 0. || delta_y < 0.)
      future = current - 1;

    if(future < 0 || future > DT_MODULEGROUP_SIZE - 1)
    {
      // We reached the end of tabs. Allow cycling through, but add a little inertia to fight.
      // This is to ensure user really wants to cycle through.
      if(scrolls > 4)
      {
        scrolls = 0;
      }
      else
      {
        // Do nothing but increment
        scrolls++;
        return FALSE;
      }
    }

    _modulegroups_set_current_group(_modulegroups_cycle_tabs(future));
    dt_iop_request_focus(NULL);
  }

  return TRUE;
}


static void _focus_module(dt_iop_module_t *module)
{
  if(module && dt_iop_gui_module_is_visible(module))
  {
    dt_iop_request_focus(module);
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
    darktable.gui->scroll_to[1] = module->expander;
  }
  else
  {
    // we reached the extremity of the list.
    dt_iop_request_focus(NULL);
  }
}

static dt_iop_module_t *_module_from_active_group(dt_iop_module_t *mod, uint32_t current_group)
{
  if(!mod) return NULL; // that should never happen

  if(dt_iop_gui_module_is_visible(mod) &&
    (dt_is_module_in_group(mod, current_group) || _is_module_in_history(mod)))
    return mod;
  else
    return NULL;
}

/* WARNING: first/last refer to pipeline nodes order, which is reversed compared to GUI order. */
static dt_iop_module_t *_find_first_visible_module()
{
  uint32_t current_group = _modulegroups_get_current_group();

  for(GList *module = g_list_first(darktable.develop->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_module_from_active_group(mod, current_group)) return mod;
  }
  return NULL;
}

static dt_iop_module_t *_find_last_visible_module()
{
  uint32_t current_group = _modulegroups_get_current_group();

  for(GList *module = g_list_last(darktable.develop->iop); module; module = g_list_previous(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_module_from_active_group(mod, current_group)) return mod;
  }
  return NULL;
}

/* WARNING: next/previous refer to GUI order, which is reversed pipeline order
* in a "layer over" logic: first pipeline node is at the GUI bottom.
*/
static gboolean _focus_previous_module()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(focused == NULL)
  {
    _focus_module(_find_first_visible_module());
  }
  else
  {
    dt_iop_gui_set_expanded(focused, FALSE, TRUE);
    _focus_module(dt_iop_gui_get_next_visible_module(focused));
  }

  return TRUE;
}

static gboolean _focus_next_module()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(focused == NULL)
  {
    _focus_module(_find_last_visible_module());
  }
  else
  {
    dt_iop_gui_set_expanded(focused, FALSE, TRUE);
    _focus_module(dt_iop_gui_get_previous_visible_module(focused));
  }

  return TRUE;
}

static gboolean _is_valid_widget(GtkWidget *widget)
{
  // The parent will always be a GtkBox
  GtkWidget *parent = gtk_widget_get_parent(widget);
  GtkWidget *grandparent = gtk_widget_get_parent(parent);
  GType type = G_OBJECT_TYPE(grandparent);

  gboolean visible_parent = TRUE;

  if(type == GTK_TYPE_NOTEBOOK)
  {
    // Find the page in which the current widget is and try to show it
    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(grandparent), parent);
    if(page_num > -1)
      gtk_notebook_set_current_page(GTK_NOTEBOOK(grandparent), page_num);
    else
      visible_parent = FALSE;
  }
  else if(type == GTK_TYPE_STACK)
  {
    // Stack pages are enabled based on user parameteters,
    // so if not visible, then do nothing.
    GtkWidget *visible_child = gtk_stack_get_visible_child(GTK_STACK(grandparent));
    if(visible_child != parent) visible_parent = FALSE;
  }

  return gtk_widget_is_visible(widget) && gtk_widget_is_sensitive(widget)
         && visible_parent;
}


// Because Gtk can't focus on invisible widgets without errors
// (and weird behaviour on user's end), getting the first widget in the list is not enough.
static GList *_find_next_visible_widget(GList *widgets)
{
  for(GList *first = widgets; first; first = g_list_next(first))
  {
    GtkWidget *widget = (GtkWidget *)first->data;
    if(_is_valid_widget(widget)) return first;
  }
  return NULL;
}


static GList *_find_previous_visible_widget(GList *widgets)
{
  for(GList *last = widgets; last; last = g_list_previous(last))
  {
    GtkWidget *widget = (GtkWidget *)last->data;
    if(_is_valid_widget(widget)) return last;
  }
  return NULL;
}

static void _focus_widget(GtkWidget *widget)
{
  gtk_widget_grab_focus(widget);
  darktable.gui->has_scroll_focus = widget;
}


static gboolean _focus_next_control()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  dt_gui_module_t *m = DT_GUI_MODULE(focused);
  if(!focused || !m->widget_list) return FALSE;

  GtkWidget *current_widget = darktable.gui->has_scroll_focus;
  GList *first_item = _find_next_visible_widget(g_list_first(m->widget_list));

  if(!current_widget && first_item)
  {
    // No active widget, start by the first
    _focus_widget(GTK_WIDGET(first_item->data));
  }
  else
  {
    GList *current_item = g_list_find(m->widget_list, current_widget);
    GList *next_item = _find_next_visible_widget(g_list_next(current_item));

    // Select the next visible item, if any
    if(next_item)
      _focus_widget(GTK_WIDGET(next_item->data));
    // Cycle back to the beginning
    else if(first_item)
      _focus_widget(GTK_WIDGET(first_item->data));
  }

  return TRUE;
}

static gboolean _focus_previous_control()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  dt_gui_module_t *m = DT_GUI_MODULE(focused);
  if(!focused || !m->widget_list) return FALSE;

  GtkWidget *current_widget = darktable.gui->has_scroll_focus;
  GList *last_item = _find_previous_visible_widget(g_list_last(m->widget_list));

  if(!current_widget && last_item)
  {
    // No active widget, start by the last
    _focus_widget(GTK_WIDGET(last_item->data));
  }
  else
  {
    GList *current_item = g_list_find(m->widget_list, current_widget);
    GList *previous_item = _find_previous_visible_widget(g_list_previous(current_item));

    // Select the previous item, if any
    if(previous_item)
      _focus_widget(GTK_WIDGET(previous_item->data));
    // Cycle back to the end
    else if(last_item)
      _focus_widget(GTK_WIDGET(last_item->data));
  }

  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;
  const int conf_group = dt_conf_get_int("plugins/darkroom/groups");
  d->current = (conf_group >= 0 && conf_group < DT_MODULEGROUP_SIZE)
                   ? (uint32_t)conf_group
                   : DT_MODULEGROUP_ACTIVE_PIPE;
  g_modulegroups_module = self;
  g_modulegroups_data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_widget_set_name(self->widget, "modules-tabs");

  /* Tabs */
  d->notebook = GTK_WIDGET(gtk_notebook_new());
  char *labels[DT_MODULEGROUP_SIZE] = { _("Pipeline"),  _("Tones"),   _("Film"),     _("Color"), _("Repair"),
                                        _("Sharpness"), _("Effects"), _("Technics"), _("All") };
  char *tooltips[DT_MODULEGROUP_SIZE]
      = { _("List all modules currently enabled in the reverse order of application in the pipeline."),
          _("Modules destined to adjust brightness, contrast and dynamic range."),
          _("Modules used when working with film scans."),
          _("Modules destined to adjust white balance and perform color-grading."),
          _("Modules destined to repair and reconstruct noisy or missing pixels."),
          _("Modules destined to manipulate local contrast, sharpness and blur."),
          _("Modules applying special effects."),
          _("Technical modules that can be ignored in most situations."),
          _("All modules available in the software.") };

  for(int i = 0; i < DT_MODULEGROUP_SIZE; i++)
  {
    GtkWidget *label = gtk_label_new(labels[i]);
    gtk_widget_set_tooltip_text(label, tooltips[i]);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(d->notebook), page, label);
  }
  gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), d->current);
  gtk_notebook_popup_enable(GTK_NOTEBOOK(d->notebook));
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(d->notebook), TRUE);
  g_signal_connect(G_OBJECT(d->notebook), "switch_page", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_signal_connect(G_OBJECT(d->notebook), "scroll-event", G_CALLBACK(_lib_modulegroups_scroll), self);
  gtk_widget_add_events(GTK_WIDGET(d->notebook), darktable.gui->scroll_mask);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->notebook), TRUE, TRUE, 0);

  _lib_modulegroups_update_iop_visibility(self);
  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULEGROUPS_SET,
                                  G_CALLBACK(_lib_modulegroups_signal_set), self);

  dt_accels_new_darkroom_action(_modulegroups_switch_tab_next, darktable.develop, N_("Darkroom/Actions"),
                                N_("move to the next modules tab"), GDK_KEY_Tab, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_action(_modulegroups_switch_tab_previous, darktable.develop, N_("Darkroom/Actions"),
                                N_("move to the previous modules tab"), GDK_KEY_Tab,
                                GDK_CONTROL_MASK | GDK_SHIFT_MASK, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_module, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the next module"), GDK_KEY_Page_Down, 0, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_module, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module"), GDK_KEY_Page_Up, 0, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_control, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the next module control"), GDK_KEY_Down, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_control, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module control"), GDK_KEY_Up, GDK_CONTROL_MASK, _("Triggers the action"));
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_signal_set), self);

  if(self->data)
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    dt_conf_set_int("plugins/darkroom/groups", (int)d->current);
  }

  if(g_modulegroups_module == self)
  {
    g_modulegroups_module = NULL;
    g_modulegroups_data = NULL;
  }

  dt_free(self->data);
}

static gboolean _is_module_in_history(const dt_iop_module_t *module)
{
  for(GList *history = g_list_last(darktable.develop->history); history; history = g_list_previous(history))
  {
    const dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
    if(hitem->module == module) return TRUE;
  }

  return FALSE;
}

static gboolean _modulegroups_module_visible_in_current(const dt_lib_modulegroups_t *d, const dt_iop_module_t *module)
{
  if(!d || !module) return FALSE;

  switch(d->current)
  {
    case DT_MODULEGROUP_ACTIVE_PIPE:
      return _is_module_in_history(module) || module->enabled;

    case DT_MODULEGROUP_NONE:
      return !(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled;

    default:
      return d->current == module->default_group();
  }
}


static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  for(GList *modules = g_list_first(darktable.develop->iop); modules; modules = g_list_next(modules))
  {

    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    GtkWidget *w = module->expander;

    /* skip modules without a gui */
    if(dt_iop_is_hidden(module)) continue;

    /* lets show/hide modules dependent on current group*/
    switch(d->current)
    {
      case DT_MODULEGROUP_ACTIVE_PIPE:
      {
        if(_is_module_in_history(module) || module->enabled)
        {
          if(w) gtk_widget_show(w);
        }
        else
        {
          if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
          if(w) gtk_widget_hide(w);
        }
        break;
      }

      case DT_MODULEGROUP_NONE:
      {
        /* show all except deprecated ones - in case of deprecated, still show it if enabled*/
        if(!(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled)
        {
          if(w) gtk_widget_show(w);
        }
        else
        {
          if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
          if(w) gtk_widget_hide(w);
        }
        break;
      }

      default:
      {
        if(d->current == module->default_group() 
           && (!(module->flags() & IOP_FLAGS_DEPRECATED) 
               || module->enabled
               || _is_module_in_history(module)))
        {
          if(w) gtk_widget_show(w);
        }
        else
        {
          if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
          if(w) gtk_widget_hide(w);
        }
      }
    }
  }
  // now that visibility has been updated set multi-show
  dt_dev_modules_update_multishow(darktable.develop);
}

static void _lib_modulegroups_toggle(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  if(d->current == page_num)
    return; // nothing to do
  else
    d->current = page_num;

  /* update visibility */
  _lib_modulegroups_update_iop_visibility(self);
}

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_iop_module_t *iop_module = (dt_iop_module_t *)module;

  if(iop_module && !_modulegroups_module_visible_in_current(d, iop_module) && GTK_IS_NOTEBOOK(d->notebook))
  {
    const uint32_t group = iop_module->default_group();
    if(group < DT_MODULEGROUP_SIZE)
    {
      d->current = group;
      gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), group);
    }
  }

  _lib_modulegroups_update_iop_visibility(self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
