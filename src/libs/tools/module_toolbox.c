/*
    This file is part of darktable,
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Jérémy Rosen.
    Copyright (C) 2019, 2023, 2025 Aurélien PIERRE.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    
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

#include "common/darktable.h"
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

/* proxy function, to add a widget to toolbox */
static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget, dt_view_type_flags_t views);


typedef struct child_data_t
{
  GtkWidget * child;
  dt_view_type_flags_t views;

} child_data_t;

typedef struct dt_lib_module_toolbox_t
{
  GtkWidget *container;
  GList * child_views;
} dt_lib_module_toolbox_t;

const char *name(struct dt_lib_module_t *self)
{
  return _("module toolbox");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 100;
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)g_malloc0(sizeof(dt_lib_module_toolbox_t));
  self->data = (void *)d;

  /* the toolbar container: use a flow box so children wrap to new lines when
   * there are more buttons than fit in one row. */
  d->container = self->widget = gtk_flow_box_new();
  /* set a small spacing and a style class so we can target it in CSS */
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(d->container), 0);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(d->container), 0);
  /* allow children to keep their natural widths (don't force uniform cells) */
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(d->container), FALSE);
  gtk_style_context_add_class(gtk_widget_get_style_context(d->container), "dt-module-toolbox");

  /* setup proxy */
  darktable.view_manager->proxy.module_toolbox.module = self;
  darktable.view_manager->proxy.module_toolbox.add = _lib_module_toolbox_add;
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  g_list_free_full(d->child_views, dt_free_gpointer);
  d->child_views = NULL;
  dt_free(self->data);
}

void view_enter(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  dt_view_type_flags_t nv= new_view->view(new_view);
  for(const GList *child_elt = d->child_views; child_elt; child_elt = g_list_next(child_elt))
  {
    child_data_t* child_data = (child_data_t*)child_elt->data;
    if(child_data->views & nv)
    {
      gtk_widget_show_all(GTK_WIDGET(child_data->child));
    }
    else
    {
      gtk_widget_hide(GTK_WIDGET(child_data->child));
    }
  }
}

static void _lib_module_toolbox_add(dt_lib_module_t *self, GtkWidget *widget, dt_view_type_flags_t views)
{
  dt_lib_module_toolbox_t *d = (dt_lib_module_toolbox_t *)self->data;
  /* If caller set a priority flag on the widget, insert it first and add a
   * separator after it. This is used to place the autoset button first. */
  gpointer prio = g_object_get_data(G_OBJECT(widget), "dt-toolbox-priority");
  if(prio && GPOINTER_TO_INT(prio) == 1)
  {
    /* insert widget at position 0 */
    /* mark widget for css styling and insert at the first position */
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(widget)), "dt-module-toolbox-item");
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(widget)), "dt-toolbox-priority");
    gtk_widget_set_hexpand(GTK_WIDGET(widget), FALSE);
    gtk_widget_set_halign(GTK_WIDGET(widget), GTK_ALIGN_CENTER);
    gtk_flow_box_insert(GTK_FLOW_BOX(d->container), GTK_WIDGET(widget), 0);

    /* register both in child_views so view visibility logic applies */
    child_data_t *child_data = malloc(sizeof(child_data_t));
    child_data->child = widget;
    child_data->views = views;
    d->child_views = g_list_prepend(d->child_views, child_data);

    gtk_widget_show_all(d->container);
    return;
  }

  /* Add widget to flow container so it participates in wrapping. */
  gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(widget)), "dt-module-toolbox-item");
  gtk_widget_set_hexpand(GTK_WIDGET(widget), FALSE);
  gtk_widget_set_halign(GTK_WIDGET(widget), GTK_ALIGN_CENTER);
  gtk_container_add(GTK_CONTAINER(d->container), GTK_WIDGET(widget));
  gtk_widget_show_all(d->container);

  child_data_t *child_data = malloc(sizeof(child_data_t));
  child_data->child = widget;
  child_data->views = views;
  d->child_views = g_list_prepend(d->child_views, child_data);

}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
