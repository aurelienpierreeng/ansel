/*
    This file is part of the Ansel project.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2025 Guillaume Stutin.
    
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
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/masks.h"

#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

typedef struct dt_lib_tool_mask_t
{
  GtkWidget *mask_lock;
  GtkWidget *opacity;
} dt_lib_tool_mask_t;

const char *name(struct dt_lib_module_t *self)
{
  return _("mask toolbar");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1000;
}

static void mask_lock_callback(GtkWidget *widget, gpointer data)
{
  if(darktable.gui->reset) return;
  dt_masks_set_lock_mode(darktable.develop, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  darktable.develop->darkroom_skip_mouse_events = dt_masks_get_lock_mode(darktable.develop);
}

static void _opacity_changed_callback(GtkWidget *widget, gpointer data)
{
  const float new_value = dt_bauhaus_slider_get(widget);
  int *parent_id = g_object_get_data(G_OBJECT(widget), "parentid");
  dt_masks_form_t *sel = g_object_get_data(G_OBJECT(widget), "selected");
  if(parent_id && sel)
  {
    dt_masks_form_set_opacity(sel, *parent_id, new_value, FALSE, 1);
    dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);
  }

  dt_conf_set_float("plugins/darkroom/masks/opacity", new_value);
}

static void _reset_opacity_slider(dt_lib_tool_mask_t *d)
{
  dt_bauhaus_slider_set(d->opacity, dt_conf_get_float("plugins/darkroom/masks/opacity"));
  gtk_widget_set_sensitive(d->opacity, FALSE);
  g_object_set_data(G_OBJECT(d->opacity), "parentid", NULL);
  g_object_set_data(G_OBJECT(d->opacity), "selected", NULL);
}

static void _set_opacity_slider(dt_lib_tool_mask_t *d, dt_masks_form_t *sel, dt_masks_form_group_t *fpt)
{
  const float opacity = dt_masks_form_get_opacity(sel, fpt->parentid);
  if(opacity != -1.f)
  {
    dt_bauhaus_slider_set(d->opacity, opacity);
    gtk_widget_set_sensitive(d->opacity, TRUE);
    g_object_set_data(G_OBJECT(d->opacity), "parentid", (void *)&fpt->parentid);
    g_object_set_data(G_OBJECT(d->opacity), "selected", (void *)sel);
  }
  else
  {
    _reset_opacity_slider(d);
  }
}

static void give_control_to_form(gpointer instance, void *_s, void *_t, dt_lib_tool_mask_t *d)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  if(!darktable.develop->form_gui) return;

  // we try to get the selected form among what we can find
  int group = gui->group_selected;
  dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, group);
  if(!fpt) return;

  dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!sel) return;

  //dt_masks_select_form(NULL, sel);

  _set_opacity_slider(d, sel, fpt);
}


void gui_init(dt_lib_module_t *self)
{
  dt_lib_tool_mask_t *d = (dt_lib_tool_mask_t *)g_malloc0(sizeof(dt_lib_tool_mask_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(self->widget, GTK_ALIGN_START);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);
  gtk_widget_set_name(self->widget, "mask-toolbar");

  d->opacity = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), 0., 1., 0.01, 1., 2);
  dt_bauhaus_widget_set_label(d->opacity, N_("Mask opacity"));
  dt_bauhaus_slider_set_factor(d->opacity, 100.);
  dt_bauhaus_slider_set_format(d->opacity, "%");
  dt_bauhaus_slider_set(d->opacity, dt_conf_get_float("plugins/darkroom/masks/opacity"));
  gtk_widget_set_size_request(d->opacity, DT_PIXEL_APPLY_DPI(250), DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->opacity), TRUE, TRUE, 0);
  gtk_widget_set_sensitive(d->opacity, FALSE);
  gtk_widget_set_tooltip_text(d->opacity, _("Control the opacity of the currently-selected mask form.\n"
                                            "This works only after a mask has been selected by click."));
  g_signal_connect(G_OBJECT(d->opacity), "value-changed", G_CALLBACK(_opacity_changed_callback), self);

  d->mask_lock = gtk_check_button_new_with_label(_("Lock masks"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->mask_lock), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->mask_lock, _("Prevent accidental masks displacement when moving the view"));
  g_signal_connect(G_OBJECT(d->mask_lock), "toggled", G_CALLBACK(mask_lock_callback), self);

  /*DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MASK_SELECTION_CHANGED,
                                  G_CALLBACK(give_control_to_form), (gpointer)d);*/

  gtk_widget_show_all(GTK_WIDGET(self->widget));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_mask_t *d = (dt_lib_tool_mask_t *)self->data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(give_control_to_form),
                               (gpointer)d);
  g_free(self->data);
  self->data = NULL;
}
