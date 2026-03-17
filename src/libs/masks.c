/*
    This file is part of darktable,
    Copyright (C) 2013, 2016, 2022 Aldric Renaudin.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2018 Tobias Ellinghaus.
    Copyright (C) 2013, 2015-2016 Ulrich Pegelow.
    Copyright (C) 2014 parafin.
    Copyright (C) 2017-2018 Edgardo Hoszowski.
    Copyright (C) 2018 luzpaz.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Ari.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2021 Philipp Lutz.
    Copyright (C) 2021 Philippe Weyland.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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
#include "develop/masks.h"
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#pragma GCC diagnostic ignored "-Wshadow"

static void _lib_masks_recreate_list(dt_lib_module_t *self);
static void _lib_masks_update_list(dt_lib_module_t *self);

typedef struct dt_lib_masks_t
{
  /* vbox with managed history items */
  GtkWidget *blending_box;
  GtkWidget *hbox;
  GtkWidget *bt_circle, *bt_path, *bt_gradient, *bt_ellipse, *bt_brush;
  GtkWidget *treeview;
  dt_iop_module_t *active_module;
  dt_iop_module_t *hosted_module;

  GdkPixbuf *ic_inverse, *ic_union, *ic_intersection, *ic_difference, *ic_exclusion, *ic_wired;
  int gui_reset;
} dt_lib_masks_t;


const char *name(struct dt_lib_module_t *self)
{
  return _("Masking & Blending");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 850;
}

typedef enum dt_masks_tree_cols_t
{
  TREE_TEXT = 0,
  TREE_MODULE,
  TREE_GROUPID,
  TREE_FORMID,
  TREE_EDITABLE,
  TREE_IC_OP,
  TREE_IC_OP_VISIBLE,
  TREE_IC_INVERSE,
  TREE_IC_INVERSE_VISIBLE,
  TREE_IC_USED,
  TREE_IC_USED_VISIBLE,
  TREE_USED_TEXT,
  TREE_COUNT
} dt_masks_tree_cols_t;

static void _lib_masks_get_values(GtkTreeModel *model, GtkTreeIter *iter,
                                  dt_iop_module_t **module, int *groupid, int *formid)
{
  // returns module & groupid & formid if requested

  if(module)
  {
    GValue gv = { 0, };
    gtk_tree_model_get_value(model, iter, TREE_MODULE, &gv);
    *module = NULL;
    if(G_VALUE_TYPE(&gv) == G_TYPE_POINTER)
      *module = (dt_iop_module_t *)g_value_get_pointer(&gv);
    g_value_unset(&gv);
  }

  if(groupid)
  {
    GValue gv = { 0, };
    gtk_tree_model_get_value(model, iter, TREE_GROUPID, &gv);
    *groupid = g_value_get_int(&gv);
    g_value_unset(&gv);
  }

  if(formid)
  {
    GValue gv = { 0,};
    gtk_tree_model_get_value(model, iter, TREE_FORMID, &gv);
    *formid = g_value_get_int(&gv);
    g_value_unset(&gv);
  }
}

static void _lib_masks_inactivate_icons(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we set the add shape icons inactive
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_circle), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_ellipse), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_path), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_gradient), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_brush), FALSE);
}

static gboolean _lib_masks_module_is_current(const dt_iop_module_t *module)
{
  return darktable.develop && module && g_list_find(darktable.develop->iop, (gpointer)module);
}

static void _lib_masks_clear_blending_box(dt_lib_masks_t *lm)
{
  if(!lm || !lm->blending_box) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(lm->blending_box));
  for(GList *iter = children; iter; iter = g_list_next(iter))
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  g_list_free(children);
  children = NULL;
}

static gboolean _lib_masks_can_host_blending(const dt_iop_module_t *module)
{
  if(!_lib_masks_module_is_current(module) || !module->flags
     || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !module->blend_data)
    return FALSE;

  return TRUE;
}

static void _lib_masks_release_blending(dt_lib_masks_t *lm)
{
  if(!lm) return;

  dt_iop_module_t *hosted_module = lm->hosted_module;
  lm->hosted_module = NULL;

  if(_lib_masks_can_host_blending(hosted_module))
    dt_iop_gui_cleanup_blending_body(hosted_module);
  else
    _lib_masks_clear_blending_box(lm);
}

static void _lib_masks_show_blending_message(dt_lib_masks_t *lm, gchar *markup)
{
  if(!lm || !lm->blending_box || !markup) return;

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_sensitive(label, FALSE);
  gtk_box_pack_start(GTK_BOX(lm->blending_box), label, FALSE, FALSE, 0);
  gtk_widget_show_all(lm->blending_box);
}

static void _lib_masks_reveal(dt_lib_module_t *self)
{
  if(!dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT))
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, TRUE, TRUE);

  if(self->expander && !dt_lib_gui_get_expanded(self))
    dt_lib_gui_set_expanded(self, TRUE);

  if(self->expander)
  {
    darktable.gui->scroll_to[0] = self->expander;
    gtk_widget_grab_focus(self->expander);
  }
}

static void _lib_masks_blending_gui_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  if(!self || !self->data) return;

  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  dt_iop_module_t *module = darktable.develop ? darktable.develop->gui_module : NULL;

  if(!darktable.develop || !darktable.develop->history || !module)
  {
    _lib_masks_release_blending(lm);
    _lib_masks_clear_blending_box(lm);

    gchar *markup = g_markup_printf_escaped(_("<i>Select a module to edit its blending settings.</i>"));

    _lib_masks_show_blending_message(lm, markup);
    g_free(markup);
  
    return;
  }

  if(!_lib_masks_can_host_blending(module))
  {
    _lib_masks_release_blending(lm);
    _lib_masks_clear_blending_box(lm);

    gchar *module_label = dt_history_item_get_name(module);
    gchar *markup = g_markup_printf_escaped(_("<i>Blending is not available for the <b>%s</b> module.</i>"), module_label);

    _lib_masks_show_blending_message(lm, markup);
    g_free(markup);
    dt_free(module_label);
  
    return;
  }

  const gboolean module_changed = (lm->active_module != module);
  lm->active_module = module;

  if(module_changed) _lib_masks_release_blending(lm);

  if(!lm->hosted_module) _lib_masks_clear_blending_box(lm);

  dt_iop_gui_init_blending_body(GTK_BOX(lm->blending_box), module);
  lm->hosted_module = module;
  gtk_widget_show(lm->blending_box);

  dt_iop_gui_update_blending(module);

  if(module_changed) _lib_masks_reveal(self);
}

static void _tree_add_circle(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode(module, DT_MASKS_CIRCLE);
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _bt_add_circle(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    _lib_masks_inactivate_icons(self);
    _tree_add_circle(NULL, NULL);
  }
}

static void _tree_add_ellipse(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode(module, DT_MASKS_ELLIPSE);
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _bt_add_ellipse(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    _lib_masks_inactivate_icons(self);
    _tree_add_ellipse(NULL, NULL);
  }
}

static void _tree_add_polygon(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode(module, DT_MASKS_POLYGON);
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _bt_add_path(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    _lib_masks_inactivate_icons(self);
    _tree_add_polygon(NULL, NULL);
  }
}

static void _tree_add_gradient(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode(module, DT_MASKS_GRADIENT);
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _bt_add_gradient(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    _lib_masks_inactivate_icons(self);
    _tree_add_gradient(NULL, NULL);
  }
}
static void _tree_add_brush(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode(module, DT_MASKS_BRUSH);
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}
static void _bt_add_brush(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    // we unset the creation mode
    dt_masks_change_form_gui(NULL);
    _lib_masks_inactivate_icons(self);
    _tree_add_brush(NULL, NULL);
  }
}

static void _tree_add_exist(GtkButton *button, dt_masks_form_t *grp)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;
  // we get the new formid
  const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(button), "module");

  // we add the form in this group
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(form && dt_masks_group_add_form(grp, form))
  {
    // we save the group
    dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);

    // and we apply the change

    dt_masks_iop_update(module);
    dt_dev_masks_selection_change(darktable.develop, NULL, grp->formid, TRUE);
  }
}

static void _tree_group(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // we create the new group
  dt_masks_form_t *mask = dt_masks_create(DT_MASKS_GROUP);
  snprintf(mask->name, sizeof(mask->name), _("Mask #%d"), g_list_length(darktable.develop->forms));

  // we add all selected forms to this group
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, NULL, &id);

      if(id > 0)
      {
        dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));
        fpt->formid = id;
        fpt->parentid = mask->formid;
        fpt->opacity = 1.0f;
        fpt->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
        mask->points = g_list_append(mask->points, fpt);
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  // we add this group to the general list
  darktable.develop->forms = g_list_append(darktable.develop->forms, mask);

  // add we save
  dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);
  _lib_masks_recreate_list(self);
  // dt_masks_change_form_gui(grp);
}

static int _tree_format_form_usage_label(char *str, const size_t str_size,
                                         const dt_masks_form_t *form, const dt_iop_module_t *module)
{
  if(!str || !form) return -1;

  str[0] = '\0';
  g_strlcat(str, form->name, str_size);

  int nbuse = 0;
  // we search were this form is used
  for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    dt_masks_form_t *grp = dt_masks_get_from_id(m->dev, m->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
      {
        dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
        if(pt->formid == form->formid)
        {
          if(m == module) return -1;
          if(nbuse == 0) g_strlcat(str, " (", str_size);
          g_strlcat(str, " ", str_size);
          gchar *module_label = dt_history_item_get_name(m);
          g_strlcat(str, module_label, str_size);
          dt_free(module_label);
          nbuse++;
        }
      }
    }
  }

  if(nbuse > 0) g_strlcat(str, " )", str_size);
  return nbuse;
}

static void _set_iter_name(dt_lib_masks_t *lm, dt_masks_form_t *form, int state, float opacity,
                           GtkTreeModel *model, GtkTreeIter *iter, int index)
{
  if(!form) return;

  char str[256] = "";
  g_strlcat(str, form->name, sizeof(str));

  if(opacity != 1.0f)
  {
    char str2[256] = "";
    g_strlcpy(str2, str, sizeof(str2));
    snprintf(str, sizeof(str), "%s %d%%", str2, (int)(opacity * 100));
  }

  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;
  if(index != 0)
  {
    if(state & DT_MASKS_STATE_UNION)
      icop = lm->ic_union;
    else if(state & DT_MASKS_STATE_INTERSECTION)
      icop = lm->ic_intersection;
    else if(state & DT_MASKS_STATE_DIFFERENCE)
      icop = lm->ic_difference;
    else if(state & DT_MASKS_STATE_EXCLUSION)
      icop = lm->ic_exclusion;
  }
  if(state & DT_MASKS_STATE_INVERSE) icinv = lm->ic_inverse;

  gtk_tree_store_set(GTK_TREE_STORE(model), iter, TREE_TEXT, str, TREE_IC_OP, icop, TREE_IC_OP_VISIBLE,
                     (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE, (icinv != NULL), -1);
}

static void _tree_cleanup(GtkButton *button, dt_lib_module_t *self)
{
  dt_masks_cleanup_unused(darktable.develop);
  _lib_masks_recreate_list(self);
}

static void _add_masks_history_item(dt_lib_masks_t *lm)
{
  const int reset = lm->gui_reset;
  lm->gui_reset = 1;
  dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);
  lm->gui_reset = reset;
}


static void _tree_inverse(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        int i = 0;
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
          if(pt->formid == id)
          {
            const int old_state = pt->state;
            apply_operation(pt, DT_MASKS_STATE_INVERSE);
            if(pt->state != old_state)
            {
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter, i);
              change = 1;
            }
            break;
          }
          i++;
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  if(change)
  {
    _add_masks_history_item(lm);

    dt_control_queue_redraw_center();
  }
}

static void _tree_intersection(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        int i = 0;
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
          if(pt->formid == id)
          {
            const int old_state = pt->state;
            apply_operation(pt, DT_MASKS_STATE_INTERSECTION);
            if(pt->state != old_state)
            {
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter, i);
              change = 1;
            }
            break;
          }
          i++;
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  if(change)
  {
    _add_masks_history_item(lm);

    dt_control_queue_redraw_center();
  }
}

static void _tree_difference(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        int i = 0;
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
          if(pt->formid == id)
          {
            const int old_state = pt->state;
            apply_operation(pt, DT_MASKS_STATE_DIFFERENCE);
            if(pt->state != old_state)
            {
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter, i);
              change = 1;
            }
            break;
          }
          i++;
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  if(change)
  {
    _add_masks_history_item(lm);

    dt_control_queue_redraw_center();
  }
}

static void _tree_exclusion(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        int i = 0;
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
          if(pt->formid == id)
          {
            const int old_state = pt->state;
            apply_operation(pt, DT_MASKS_STATE_EXCLUSION);
            if(pt->state != old_state)
            {
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter, i);
              change = 1;
            }
            break;
          }
          i++;
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  if(change)
  {
    _add_masks_history_item(lm);

    dt_control_queue_redraw_center();
  }
}

static void _tree_union(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        int i = 0;
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
          if(pt->formid == id)
          {
            const int old_state = pt->state;
            apply_operation(pt, DT_MASKS_STATE_UNION);
            if(pt->state != old_state)
            {
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter, i);
              change = 1;
            }
            break;
          }
          i++;
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  if(change)
  {
    _add_masks_history_item(lm);

    dt_control_queue_redraw_center();
  }
}

static void _tree_moveup(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 0);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);

}

static void _tree_movedown(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 1);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);

}

static void _tree_delete_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  dt_iop_module_t *module = NULL;
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, &module, &grid, &id);

      dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, grid),
                           dt_masks_get_from_id(darktable.develop, id));
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);
}

static void _tree_duplicate_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we get the selected node
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  if(!items) return;
  GtkTreePath *item = (GtkTreePath *)items->data;
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(model, &iter, item))
  {
    int id = -1;
    _lib_masks_get_values(model, &iter, NULL, NULL, &id);

    const int nid = dt_masks_form_duplicate(darktable.develop, id);
    if(nid > 0)
    {
      dt_dev_masks_selection_change(darktable.develop, NULL, nid, TRUE);
      //_lib_masks_recreate_list(self);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;
}

static void _tree_cell_edited(GtkCellRendererText *cell, gchar *path_string, gchar *new_text,
                              dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  int id = -1;
  _lib_masks_get_values(model, &iter, NULL, NULL, &id);
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return;

  // we want to make sure that the new name is not an empty string. else this would convert
  // in the xmp file into "<rdf:li/>" which produces problems. we use a single whitespace
  // as the pure minimum text.
  gchar *text = strlen(new_text) == 0 ? " " : new_text;

  // first, we need to update the mask name

  g_strlcpy(form->name, text, sizeof(form->name));
  dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_lib_masks_t *self)
{
  if(self->gui_reset) return;
  // we reset all "show mask" icon of iops
  dt_masks_reset_show_masks_icons();

  // if selection empty, we hide all
  const int nb = gtk_tree_selection_count_selected_rows(selection);
  if(nb == 0)
  {
    dt_masks_change_form_gui(NULL);
    dt_control_queue_redraw_center();
    return;
  }

  // else, we create a new form group with the selection and display it
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(self->treeview));
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
      if(form)
      {
        dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));
        fpt->formid = id;
        fpt->parentid = grid;
        fpt->state = DT_MASKS_STATE_USE;
        fpt->opacity = 1.0f;
        grp->points = g_list_append(grp->points, fpt);
        // we eventually set the "show masks" icon of iops
        if(nb == 1 && (form->type & DT_MASKS_GROUP))
        {
          dt_iop_module_t *module = NULL;
          _lib_masks_get_values(model, &iter, &module, NULL, NULL);

          if(module && module->blend_data
             && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
             && !(module->flags() & IOP_FLAGS_NO_MASKS))
          {
            dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
            bd->masks_shown = 1;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
            gtk_widget_queue_draw(bd->masks_edit);
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  dt_masks_form_t *grp2 = dt_masks_create(DT_MASKS_GROUP);
  grp2->formid = 0;
  dt_masks_group_ungroup(grp2, grp);
  dt_masks_change_form_gui(grp2);
  darktable.develop->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  dt_control_queue_redraw_center();
}

static GtkWidget *_tree_context_menu(GtkTreeSelection *selection, GtkTreeModel *model,
                                     dt_lib_module_t *self, dt_iop_module_t *module)
{
  GtkTreeIter iter;
  GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
  GtkWidget *item;

  // we get all infos from selection
  const int nb = gtk_tree_selection_count_selected_rows(selection);
  int from_group = 0;

  int grpid = 0;
  int depth = 0;

  if(nb > 0)
  {
    GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
    GtkTreePath *it0 = (GtkTreePath *)selected->data;
    depth = gtk_tree_path_get_depth(it0);
    if(nb == 1)
    {
      // before freeing the list of selected rows, we check if the form is a group or not
      if(gtk_tree_model_get_iter(model, &iter, it0))
      {
        _lib_masks_get_values(model, &iter, NULL, NULL, &grpid);
      }
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    selected = NULL;
  }
  if(depth > 1) from_group = 1;

  if(nb == 0)
  {
    GtkWidget *add_menu = gtk_menu_new();
    GtkWidget *add_item = gtk_menu_item_new_with_label(_("Add new shape ..."));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(add_item), add_menu);
    gtk_menu_shell_append(menu, add_item);

    item = gtk_menu_item_new_with_label(_("add circle"));
    g_signal_connect(item, "activate", (GCallback)_tree_add_circle, module);
    gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

    item = gtk_menu_item_new_with_label(_("add ellipse"));
    g_signal_connect(item, "activate", (GCallback)_tree_add_ellipse, module);
    gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

    item = gtk_menu_item_new_with_label(_("add path"));
    g_signal_connect(item, "activate", (GCallback)_tree_add_polygon, module);
    gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

    item = gtk_menu_item_new_with_label(_("add gradient"));
    g_signal_connect(item, "activate", (GCallback)_tree_add_gradient, module);
    gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }

  if(nb == 1)
  {
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      GtkWidget *add_menu = gtk_menu_new();
      GtkWidget *add_item = gtk_menu_item_new_with_label(_("Add new shape ..."));
      gtk_menu_item_set_submenu(GTK_MENU_ITEM(add_item), add_menu);
      gtk_menu_shell_append(menu, add_item);

      item = gtk_menu_item_new_with_label(_("Add brush"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_brush, module);
      gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

      item = gtk_menu_item_new_with_label(_("Add circle"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_circle, module);
      gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

      item = gtk_menu_item_new_with_label(_("Add ellipse"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_ellipse, module);
      gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

      item = gtk_menu_item_new_with_label(_("Add polygon"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_polygon, module);
      gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

      item = gtk_menu_item_new_with_label(_("Add gradient"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_gradient, module);
      gtk_menu_shell_append(GTK_MENU_SHELL(add_menu), item);

      // existing forms
      gboolean has_unused_shapes = FALSE;
      GtkWidget *menu0 = gtk_menu_new();
      for(GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
      {
        dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
        if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) || form->formid == grpid)
        {
          continue;
        }
        char str[10000] = "";
        const int nbuse = _tree_format_form_usage_label(str, sizeof(str), form, module);
        if(nbuse == -1) continue;

        // we add the menu entry
        item = gtk_menu_item_new_with_label(str);
        g_object_set_data(G_OBJECT(item), "formid", GUINT_TO_POINTER(form->formid));
        g_object_set_data(G_OBJECT(item), "module", module);
        g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_tree_add_exist), grp);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu0), item);
        has_unused_shapes = TRUE;
      }

      if(has_unused_shapes)
      {
        item = gtk_menu_item_new_with_label(_("Add shape ..."));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu0);
        gtk_menu_shell_append(menu, item);
      }
    }
  }

  if(nb > 1 && !from_group)
  {
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("Group the forms"));
    g_signal_connect(item, "activate", (GCallback)_tree_group, self);
    gtk_menu_shell_append(menu, item);
  }

  if(from_group && depth < 3)
  {
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("Invert shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_inverse, self);
    gtk_menu_shell_append(menu, item);
    if(nb == 1)
    {
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("Union"));
      g_signal_connect(item, "activate", (GCallback)_tree_union, self);
      gtk_menu_shell_append(menu, item);
      item = gtk_menu_item_new_with_label(_("Intersection"));
      g_signal_connect(item, "activate", (GCallback)_tree_intersection, self);
      gtk_menu_shell_append(menu, item);
      item = gtk_menu_item_new_with_label(_("Difference"));
      g_signal_connect(item, "activate", (GCallback)_tree_difference, self);
      gtk_menu_shell_append(menu, item);
      item = gtk_menu_item_new_with_label(_("Exclusion"));
      g_signal_connect(item, "activate", (GCallback)_tree_exclusion, self);
      gtk_menu_shell_append(menu, item);
    }
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("Move up"));
    g_signal_connect(item, "activate", (GCallback)_tree_moveup, self);
    gtk_menu_shell_append(menu, item);
    item = gtk_menu_item_new_with_label(_("Move down"));
    g_signal_connect(item, "activate", (GCallback)_tree_movedown, self);
    gtk_menu_shell_append(menu, item);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }

  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
  if(!from_group && !(grp && (grp->type & DT_MASKS_GROUP)) && nb == 1)
  {
    item = gtk_menu_item_new_with_label(_("Duplicate shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_duplicate_shape, self);
    gtk_menu_shell_append(menu, item);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }
  
  if(!from_group && nb > 0)
  {
    if(!(grp && (grp->type & DT_MASKS_GROUP)))
    {
      item = gtk_menu_item_new_with_label(_("Delete shape"));
      g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
      gtk_menu_shell_append(menu, item);
    }
    else
    {
      item = gtk_menu_item_new_with_label(_("Delete mask"));
      g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
      gtk_menu_shell_append(menu, item);
    }
  }
  else if(nb > 0 && depth < 3)
  {
    item = gtk_menu_item_new_with_label(_("Remove shape from mask"));
    g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
    gtk_menu_shell_append(menu, item);
  }

  item = gtk_menu_item_new_with_label(_("Cleanup unused shapes"));
  g_signal_connect(item, "activate", (GCallback)_tree_cleanup, self);
  gtk_menu_shell_append(menu, item);
  
  return GTK_WIDGET(menu);
}

static int _tree_button_pressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  // we first need to adjust selection
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));

  GtkTreePath *mouse_path = NULL;
  GtkTreeIter iter;
  dt_iop_module_t *module = NULL;
  int on_row = 0;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &mouse_path, NULL,
                                   NULL, NULL))
  {
    on_row = 1;
    // we retrieve the iter and module from path
    if(gtk_tree_model_get_iter(model, &iter, mouse_path))
    {
      _lib_masks_get_values(model, &iter, &module, NULL, NULL);
    }
  }
  /* single click with the right mouse button? */
  if(event->type == GDK_BUTTON_PRESS && event->button == 1)
  {
    // if click on a blank space, then deselect all
    if(!on_row)
    {
      gtk_tree_selection_unselect_all(selection);
    }
  }
  else if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    // if we are already inside the selection, no change
    if(on_row && !gtk_tree_selection_path_is_selected(selection, mouse_path))
    {
      if(!dt_modifier_is(event->state, GDK_CONTROL_MASK)) gtk_tree_selection_unselect_all(selection);
      gtk_tree_selection_select_path(selection, mouse_path);
      gtk_tree_path_free(mouse_path);
    }

    // and we display the context-menu
    GtkWidget *menu = _tree_context_menu(selection, model, self, module);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    return 1;
  }

  return 0;
}

static gboolean _tree_restrict_select(GtkTreeSelection *selection, GtkTreeModel *model, GtkTreePath *path,
                                      gboolean path_currently_selected, gpointer data)
{
  dt_lib_masks_t *self = (dt_lib_masks_t *)data;
  if(self->gui_reset) return TRUE;

  // if the change is SELECT->UNSELECT no pb
  if(path_currently_selected) return TRUE;

  // if selection is empty, no pb
  if(gtk_tree_selection_count_selected_rows(selection) == 0) return TRUE;

  // now we unselect all members of selection with not the same parent node
  // idem for all those with a different depth
  int *indices = gtk_tree_path_get_indices(path);
  int depth = gtk_tree_path_get_depth(path);

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  GList *items_iter = items;
  while(items_iter)
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    int dd = gtk_tree_path_get_depth(item);
    int *ii = gtk_tree_path_get_indices(item);
    int ok = 1;
    if(dd != depth)
      ok = 0;
    else if(dd == 1)
      ok = 1;
    else if(ii[dd - 2] != indices[dd - 2])
      ok = 0;
    if(!ok)
    {
      gtk_tree_selection_unselect_path(selection, item);
      g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
      items = NULL;
      items_iter = items = gtk_tree_selection_get_selected_rows(selection, NULL);
      continue;
    }
    items_iter = g_list_next(items_iter);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;
  return TRUE;
}

static gboolean _tree_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_tip,
                                    GtkTooltip *tooltip, gpointer data)
{
  GtkTreeIter iter;
  GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
  GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
  GtkTreePath *path = NULL;
  gchar *tmp = NULL;
  gboolean show = FALSE;

  if(!gtk_tree_view_get_tooltip_context(tree_view, &x, &y, keyboard_tip, &model, &path, &iter)) return FALSE;

  gtk_tree_model_get(model, &iter, TREE_IC_USED_VISIBLE, &show, TREE_USED_TEXT, &tmp, -1);
  if(show)
  {
    gtk_tooltip_set_markup(tooltip, tmp);
    gtk_tree_view_set_tooltip_row(tree_view, tooltip, path);
  }

  gtk_tree_path_free(path);
  dt_free(tmp);

  return show;
}

static void _is_form_used(int formid, dt_masks_form_t *grp, char *text, size_t text_length, int *nb)
{
  if(!grp)
  {
    for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
    {
      dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
      if(form->type & DT_MASKS_GROUP) _is_form_used(formid, form, text, text_length, nb);
    }
  }
  else if(grp->type & DT_MASKS_GROUP)
  {
    for(const GList *points = grp->points; points; points = g_list_next(points))
    {
      dt_masks_form_group_t *point = (dt_masks_form_group_t *)points->data;
      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, point->formid);
      if(form)
      {
        if(point->formid == formid)
        {
          (*nb)++;
          if(*nb > 1) g_strlcat(text, "\n", text_length);
          g_strlcat(text, grp->name, text_length);
        }
        if(form->type & DT_MASKS_GROUP) _is_form_used(formid, form, text, text_length, nb);
      }
    }
  }
}

static void _lib_masks_list_recurs(GtkTreeStore *treestore, GtkTreeIter *toplevel, dt_masks_form_t *form,
                                   int grp_id, dt_iop_module_t *module, int gstate, float opacity,
                                   dt_lib_masks_t *lm, int index)
{
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) return;
  // we create the text entry
  char str[256] = "";
  g_strlcat(str, form->name, sizeof(str));
  // we get the right pixbufs
  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;
  GdkPixbuf *icuse = NULL;
  if(gstate & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(gstate & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(gstate & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(gstate & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;
  if(gstate & DT_MASKS_STATE_INVERSE) icinv = lm->ic_inverse;
  char str2[1000] = "";
  int nbuse = 0;
  if(grp_id == 0)
  {
    _is_form_used(form->formid, NULL, str2, sizeof(str2), &nbuse);
    if(nbuse > 0) icuse = lm->ic_wired;
  }

  if(!(form->type & DT_MASKS_GROUP))
  {
    // we just add it to the tree
    GtkTreeIter child;
    gtk_tree_store_append(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0), TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (icinv != NULL), TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child, index);
  }
  else
  {
    // we first check if it's a "module" group or not
    if(grp_id == 0 && !module)
    {
      for(const GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
      {
        dt_iop_module_t *iop = (dt_iop_module_t *)iops->data;
        if((iop->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(iop->flags() & IOP_FLAGS_NO_MASKS)
           && iop->blend_params->mask_id == form->formid)
        {
          module = iop;
          break;
        }
      }
    }

    // we add the group node to the tree
    GtkTreeIter child;
    gtk_tree_store_append(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0), TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (icinv != NULL), TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child, index);

    index = 0;
    // we add all nodes to the tree
    for(const GList *forms = form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
        _lib_masks_list_recurs(treestore, &child, f, form->formid, module, grpt->state, grpt->opacity, lm, index);
      index++;
    }
  }
}

gboolean _find_mask_iter_by_values(GtkTreeModel *model, GtkTreeIter *iter,
                                   const dt_iop_module_t *module, const int formid, const int level)
{
  gboolean found = FALSE;
  do
  {
    int fid = -1;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, iter, &mod, NULL, &fid);
    found = (fid == formid)
      && ((level == 1)
          || (module == NULL || (mod && (!g_strcmp0(module->op, mod->op)))));
    if(found) return found;
    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_mask_iter_by_values(model, &child, module, formid, level + 1);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

GList *_lib_masks_get_selected(dt_lib_module_t *self)
{
  GList *res = NULL;
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  GList *items = gtk_tree_selection_get_selected_rows(selection, &model);

  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int fid = -1;
      int gid = -1;
      dt_iop_module_t *mod;
      _lib_masks_get_values(model, &iter, &mod, &gid, &fid);
      res = g_list_prepend(res, GINT_TO_POINTER(fid));
      res = g_list_prepend(res, GINT_TO_POINTER(gid));
      res = g_list_prepend(res, (void *)(mod));
    }
  }

  g_list_foreach(items, (GFunc)gtk_tree_path_free, NULL);
  g_list_free(items);
  items = NULL;

  return res;
}

static void _lib_masks_recreate_list(dt_lib_module_t *self)
{
  /* first destroy all buttons in list */
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(!lm) return;
  if(lm->gui_reset) return;

  const int gui_reset = lm->gui_reset;
  lm->gui_reset = 1;
  // if (lm->treeview) gtk_widget_destroy(lm->treeview);

  // if a treeview is already present, let's get the currently selected items
  // as we are going to recreate the tree.
  GList *selectids = NULL;

  if(lm->treeview)
  {
    selectids = _lib_masks_get_selected(self);
  }

  _lib_masks_inactivate_icons(self);

  GtkTreeStore *treestore;
  // we store : text ; *module ; groupid ; formid
  treestore = gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_STRING);

  // we first add all groups
  for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->type & DT_MASKS_GROUP) _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm, 0);
  }

  // and we add all forms
  for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(!(form->type & DT_MASKS_GROUP)) _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm, 0);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(lm->treeview), GTK_TREE_MODEL(treestore));
  
  // select the images as selected in the previous tree
  if(selectids)
  {
    GList *ids = selectids;
    while(ids)
    {
      GtkTreeModel *model = GTK_TREE_MODEL(treestore);
      dt_iop_module_t *mod = (dt_iop_module_t *)ids->data;
      ids = g_list_next(ids);
      // const int gid = GPOINTER_TO_INT(ids->data); // not needed, skip it
      ids = g_list_next(ids);
      const int fid = GPOINTER_TO_INT(ids->data);
      ids = g_list_next(ids);

      GtkTreeIter iter;
      gtk_tree_model_get_iter_first(model, &iter);
      // get formid in group for the given module
      const gboolean found = _find_mask_iter_by_values(model, &iter, mod, fid, 1);

      if(found)
      {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_path_free(path);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
        gtk_tree_selection_select_iter(selection, &iter);
      }
    }
    g_list_free(selectids);
    selectids = NULL;
  }

  g_object_unref(treestore);

  lm->gui_reset = gui_reset;
}

static void _lib_masks_update_item(dt_lib_module_t *self, int formid, int parentid, dt_lib_masks_t *lm, GtkTreeModel *model, GtkTreeIter *iter)
{
  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(!form) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, parentid);

  // and the values
  int state = 0;
  float opacity = 1.0f;

  int index = 0;
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
    {
      dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
      if(pt->formid == formid)
      {
        state = pt->state;
        opacity = pt->opacity;
        break;
      }
      index++;
    }
  }

  _set_iter_name(lm, form, state, opacity, model, iter, index);
  return;
}

static gboolean _update_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  if(!iter) return 0;

  // we retrieve the ids
  int grid = -1;
  int id = -1;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

  // and the values
  int state = 0;
  float opacity = 1.0f;

  int index = 0;
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
    {
      dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
      if(pt->formid == id)
      {
        state = pt->state;
        opacity = pt->opacity;
        break;
      }
      index++;
    }
  }

  _set_iter_name(data, form, state, opacity, model, iter, index);
  return 0;
}

// Update each item of the list
static void _lib_masks_update_list(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  gtk_tree_model_foreach(model, _update_foreach, lm);
}

static gboolean _remove_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  if(!iter) return 0;
  GList **rl = (GList **)data;
  const int refid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "formid"));
  const int refgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "groupid"));

  int grid = -1;
  int id = -1;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  if(grid == refgid && id == refid)
  {
    GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
    *rl = g_list_append(*rl, rowref);
  }
  return 0;
}

static void _lib_masks_remove_item(dt_lib_module_t *self, int formid, int parentid)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GList *rl = NULL;
  g_object_set_data(G_OBJECT(model), "formid", GUINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(model), "groupid", GUINT_TO_POINTER(parentid));
  gtk_tree_model_foreach(model, _remove_foreach, &rl);

  for(const GList *rlt = rl; rlt; rlt = g_list_next(rlt))
  {
    GtkTreeRowReference *rowref = (GtkTreeRowReference *)rlt->data;
    GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
    gtk_tree_row_reference_free(rowref);
    if(path)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
      }
      gtk_tree_path_free(path);
    }
  }
  g_list_free(rl);
  rl = NULL;
}

static gboolean _lib_masks_selection_change_r(GtkTreeModel *model, GtkTreeSelection *selection,
                                              GtkTreeIter *iter, struct dt_iop_module_t *module,
                                              const int selectid, int throw_event, const int level)
{
  gboolean found = FALSE;

  GtkTreeIter i = *iter;
  do
  {
    int id = -1;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, &i, &mod, NULL, &id);

    if((id == selectid)
       && ((level == 1)
           || (module == NULL || (mod && (!g_strcmp0(module->op, mod->op))))))
    {
      gtk_tree_selection_select_iter(selection, &i);
      found = TRUE;
      break;
    }

    // check for children if any
    GtkTreeIter child, parent = i;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _lib_masks_selection_change_r(model, selection, &child, module, selectid, throw_event, level + 1);
      if(found)
      {
        break;
      }
    }
  } while(gtk_tree_model_iter_next(model, &i) == TRUE);

  return found;
}

static void _lib_masks_selection_change(dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(!lm->treeview) return;

  // we first unselect all
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  gtk_tree_selection_unselect_all(selection);
  lm->gui_reset = 0;

  // we go through all nodes
  lm->gui_reset = 1 - throw_event;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  if(!GTK_IS_TREE_MODEL(model))
  {
    lm->gui_reset = 0;
    return;
  }
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

  if(valid)
  {
    gtk_tree_view_expand_all(GTK_TREE_VIEW(lm->treeview));
    const gboolean found = _lib_masks_selection_change_r(model, selection, &iter, module, selectid, throw_event, 1);
    if(!found) gtk_tree_view_collapse_all(GTK_TREE_VIEW(lm->treeview));
  }

  lm->gui_reset = 0;
}

static gboolean _find_child_iter_by_formid(GtkTreeModel *model, GtkTreeIter *parent_iter, int formid, GtkTreeIter *child_iter)
{
  GtkTreeIter iter;
  gboolean found = FALSE;

  // Obtenir le premier enfant du parent
  if(gtk_tree_model_iter_children(model, &iter, parent_iter))
  {
    do
    {
      int current_formid = -1;
      gtk_tree_model_get(model, &iter, TREE_FORMID, &current_formid, -1);

      if(current_formid == formid)
      {
        *child_iter = iter;
        found = TRUE;
        break;
      }
    } while(gtk_tree_model_iter_next(model, &iter));
  }

  return found;
}

static gboolean _find_iter_by_parentid_and_formid(GtkTreeModel *model, int parentid, int formid, GtkTreeIter *iter)
{
  gboolean found = FALSE;

  // Obtenir le premier itérateur du modèle
  do
  {
    int current_parentid = -1;
    gtk_tree_model_get(model, iter, TREE_FORMID, &current_parentid, -1);

    if(current_parentid == parentid)
    {
      // Rechercher le formid dans les enfants du parent
      found = _find_child_iter_by_formid(model, iter, formid, iter);
      if(found)
      {
        break;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));

  return found;
}

static void _lib_masks_handler_callback(gpointer instance, const int formid, const int parentid, const dt_masks_event_t event, dt_lib_module_t *self)
{
  if(!self) return;

  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(!lm) return;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;
  GtkTreeIter iter;
  gboolean found_iter = gtk_tree_model_get_iter_first(model, &iter);

  if(found_iter && _find_iter_by_parentid_and_formid(model, parentid, formid, &iter))
  {
    switch(event)
    {
      case DT_MASKS_EVENT_UPDATE :
      {
        _lib_masks_update_item(self, formid, parentid, lm, model, &iter);
      }
      break;

      case DT_MASKS_EVENT_CHANGE :
      {
        _lib_masks_recreate_list(self);
      }
      break;

      case DT_MASKS_EVENT_DELETE :
      {
        _lib_masks_recreate_list(self);
      }
      break;

      case DT_MASKS_EVENT_REMOVE :
      {
        _lib_masks_recreate_list(self);
        //_lib_masks_remove_item(self, formid, parentid);
      }
      break;

      case DT_MASKS_EVENT_NONE :
      default:
      {
        dt_print(DT_DEBUG_MASKS, "[_lib_masks_handler_callback] Mask event cannot be found.");
      }
      break;
    }
  }
  
  else if(event == DT_MASKS_EVENT_RESET)
  {
    _lib_masks_recreate_list(self);
  }

  else if(event == DT_MASKS_EVENT_ADD)
  {
    _lib_masks_recreate_list(self);
    dt_masks_set_visible_form(darktable.develop, dt_masks_get_from_id(darktable.develop, parentid));
  }

  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_masks_t *d = (dt_lib_masks_t *)g_malloc0(sizeof(dt_lib_masks_t));
  self->data = (void *)d;
  d->gui_reset = 0;

  // initialise all masks pixbuf. This is needed for the "automatic" cell renderer of the treeview
  const int bs2 = DT_PIXEL_APPLY_DPI(13);
  d->ic_inverse = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, bs2, bs2);
  d->ic_wired = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_link_chain, bs2, bs2);
  d->ic_union = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, bs2 * 2, bs2);
  d->ic_intersection = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, bs2 * 2, bs2);
  d->ic_difference = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, bs2 * 2, bs2);
  d->ic_exclusion = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, bs2 * 2, bs2);

  // initialise widgets
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  dt_gui_collapsible_section_t *shape_manager_expander = malloc(sizeof(dt_gui_collapsible_section_t));

  dt_gui_new_collapsible_section(shape_manager_expander, "plugins/darkroom/blending/shape_manager/expanded",
                                 _("Shape manager"), GTK_BOX(self->widget), GTK_PACK_START);
  GtkWidget *shape_manager_box = GTK_WIDGET(shape_manager_expander->container); // gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  //gtk_box_pack_start(GTK_BOX(shape_manager_expander->container), shape_manager_box, TRUE, TRUE, 0);

  GtkWidget *label = gtk_label_new(_("created shapes"));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);

  d->bt_gradient = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_gradient, 0, NULL);
  g_signal_connect(G_OBJECT(d->bt_gradient), "button-press-event", G_CALLBACK(_bt_add_gradient), self);
  gtk_widget_set_tooltip_text(d->bt_gradient, _("add gradient"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_gradient), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_gradient, FALSE, FALSE, 0);

  d->bt_path = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, 0, NULL);
  g_signal_connect(G_OBJECT(d->bt_path), "button-press-event", G_CALLBACK(_bt_add_path), self);
  gtk_widget_set_tooltip_text(d->bt_path, _("add path"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_path), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_path, FALSE, FALSE, 0);

  d->bt_ellipse = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, 0, NULL);
  g_signal_connect(G_OBJECT(d->bt_ellipse), "button-press-event", G_CALLBACK(_bt_add_ellipse), self);
  gtk_widget_set_tooltip_text(d->bt_ellipse, _("add ellipse"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_ellipse), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_ellipse, FALSE, FALSE, 0);

  d->bt_circle = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, 0, NULL);
  g_signal_connect(G_OBJECT(d->bt_circle), "button-press-event", G_CALLBACK(_bt_add_circle), self);
  gtk_widget_set_tooltip_text(d->bt_circle, _("add circle"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_circle), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_circle, FALSE, FALSE, 0);

  d->bt_brush = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, 0, NULL);
  g_signal_connect(G_OBJECT(d->bt_brush), "button-press-event", G_CALLBACK(_bt_add_brush), self);
  gtk_widget_set_tooltip_text(d->bt_brush, _("add brush"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_brush), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_brush, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(shape_manager_box), hbox, TRUE, TRUE, 0);

  d->treeview = gtk_tree_view_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(col, "shapes");
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), col);

  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_OP, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_OP_VISIBLE);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_INVERSE, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_INVERSE_VISIBLE);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", TREE_TEXT);
  gtk_tree_view_column_add_attribute(col, renderer, "editable", TREE_EDITABLE);
  g_signal_connect(renderer, "edited", (GCallback)_tree_cell_edited, self);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_USED, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
  // gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->treeview),TREE_USED_TEXT);
  g_object_set(d->treeview, "has-tooltip", TRUE, (gchar *)0);
  g_signal_connect(d->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), NULL);
  g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), d);
  g_signal_connect(d->treeview, "button-press-event", (GCallback)_tree_button_pressed, self);

  gtk_box_pack_start(GTK_BOX(shape_manager_box), d->treeview, TRUE, TRUE, 0);
  dt_gui_widget_init_auto_height(d->treeview, TREE_LIST_MIN_ROWS, TREE_LIST_MAX_ROWS);
  // Ensure the expander body has its subtree visible even when global show_all is blocked.
  gtk_widget_show_all(shape_manager_box);

  
  GtkWidget *blending_label = dt_ui_section_label_new(_("Blending"));
  gtk_widget_set_margin_top(blending_label, DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(self->widget), blending_label, TRUE, TRUE, 0);

  d->blending_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->blending_box, FALSE, FALSE, 0);


  gtk_widget_show_all(self->widget);
  gtk_widget_hide(d->blending_box);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MASK_CHANGED, G_CALLBACK(_lib_masks_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED,
                                  G_CALLBACK(_lib_masks_blending_gui_changed_callback), self);

  // set proxy functions
  darktable.develop->proxy.masks.module = self;
  darktable.develop->proxy.masks.list_change = _lib_masks_recreate_list;
  darktable.develop->proxy.masks.list_update = _lib_masks_update_list;
  darktable.develop->proxy.masks.list_remove = _lib_masks_remove_item;
  darktable.develop->proxy.masks.selection_change = _lib_masks_selection_change;

  _lib_masks_blending_gui_changed_callback(NULL, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(self && self->data)
  {
    dt_lib_masks_t *d = (dt_lib_masks_t *)self->data;
    _lib_masks_release_blending(d);
  }

  dt_free(self->data);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_masks_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_masks_blending_gui_changed_callback), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
