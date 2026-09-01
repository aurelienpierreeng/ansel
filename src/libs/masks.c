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
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/masks_group.h"   // dt_masks_group_set_member_operation()
#include "develop/masks_gui.h"
#include "common/logging.h"
#include "system/macros.h"
#include "common/module_versioning.h"
#include "control/redraw.h"
#include "develop/blend.h"
#include "develop/blend_gui.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "widgets/draw.h"
#include "widgets/accelerators.h"
#include "gui/application.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#include "widgets/scroll_wrap.h"
#include "common/conf.h"          // dt_conf_get_int(), dt_conf_key_exists()

#include "widgets/togglebutton.h"
#include "control/signal.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>   // conditional-ok: GDK_IS_WAYLAND_DISPLAY() is used only inside the same #ifdef
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


DT_MODULE(1)

#pragma GCC diagnostic ignored "-Wshadow"

static void _lib_masks_recreate_list(dt_lib_module_t *self);
static void _lib_masks_update_list(dt_lib_module_t *self);

typedef struct dt_lib_masks_t
{
  GtkWidget *hbox;
  GtkWidget *bt_circle, *bt_path, *bt_gradient, *bt_ellipse, *bt_brush;
  GtkWidget *treeview;
  dt_iop_module_t *active_module;
  dt_iop_module_t *hosted_module;

  /* Replacement for shape_manager_expander */
  GtkWidget *popup_window;
  GtkWidget *popup_button;

  GdkPixbuf *ic_inverse, *ic_union, *ic_intersection, *ic_difference, *ic_exclusion;
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

int expandable(dt_lib_module_t *self)
{
  return 1;
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

static gboolean _lib_masks_module_is_current(const dt_iop_module_t *module)
{
  return dt_dev_get_global() && module && g_list_find(dt_dev_get_global()->iop, (gpointer)module);
}

static void _lib_masks_clear_blending_box(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || !GTK_IS_WIDGET(self->widget)) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(self->widget));
  for(GList *iter = children; iter; iter = g_list_next(iter))
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  g_list_free(children);
  children = NULL;
}

static gboolean _lib_masks_can_host_blending(const dt_iop_module_t *module)
{
  if(!_lib_masks_module_is_current(module) || !module->flags
     || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !module->gui || !module->gui->blend_data)
    return FALSE;

  return TRUE;
}

static void _lib_masks_release_blending(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  dt_iop_module_t *hosted_module = lm->hosted_module;
  lm->hosted_module = NULL;

  if(_lib_masks_can_host_blending(hosted_module))
    dt_iop_gui_cleanup_blending_body(hosted_module);
  else
    _lib_masks_clear_blending_box(self);
}

static void _lib_masks_show_blending_message(dt_lib_module_t *self, gchar *markup)
{
  if(IS_NULL_PTR(self) || !GTK_IS_WIDGET(self->widget) || IS_NULL_PTR(markup)) return;

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_sensitive(label, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);
  // self->widget is the expander body: its own visibility encodes the
  // expanded/collapsed state persisted in conf, so show only the child.
  gtk_widget_show_all(label);
}

static void _lib_masks_blending_gui_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;

  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  dt_iop_module_t *module = dt_dev_get_global()->gui_module;

  if(IS_NULL_PTR(dt_dev_get_global()) || !dt_dev_get_global()->history || !module)
  {
    _lib_masks_release_blending(self);
    _lib_masks_clear_blending_box(self);

    gchar *markup = g_markup_printf_escaped(_("<i>Select a module to edit its blending settings.</i>"));

    _lib_masks_show_blending_message(self, markup);
    g_free(markup);
  
    return;
  }

  if(!_lib_masks_can_host_blending(module))
  {
    _lib_masks_release_blending(self);
    _lib_masks_clear_blending_box(self);

    gchar *module_label = dt_history_item_get_name(module);
    gchar *markup = g_markup_printf_escaped(_("<i>Blending is not available for the <b>%s</b> module.</i>"), module_label);

    _lib_masks_show_blending_message(self, markup);
    g_free(markup);
    dt_free(module_label);
  
    return;
  }

  const gboolean module_changed = (lm->active_module != module);
  lm->active_module = module;

  if(module_changed) _lib_masks_release_blending(self);

  if(!lm->hosted_module) _lib_masks_clear_blending_box(self);

  // dt_iop_gui_init_blending_body() shows the children it packs; don't show
  // self->widget itself, it is the expander body whose visibility encodes the
  // expanded/collapsed state persisted in conf.
  dt_iop_gui_init_blending_body(self->widget, module);
  lm->hosted_module = module;

  dt_iop_gui_update_blending(module);
}

static void _tree_add_circle(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode_enter(dt_dev_get_global(), module, DT_MASKS_CIRCLE);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _tree_add_ellipse(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode_enter(dt_dev_get_global(), module, DT_MASKS_ELLIPSE);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _tree_add_polygon(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode_enter(dt_dev_get_global(), module, DT_MASKS_POLYGON);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _tree_add_gradient(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode_enter(dt_dev_get_global(), module, DT_MASKS_GRADIENT);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _tree_add_brush(GtkButton *button, dt_iop_module_t *module)
{
  // we create the new form
  dt_masks_creation_mode_enter(dt_dev_get_global(), module, DT_MASKS_BRUSH);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _lib_masks_shape_button_started(GtkWidget *button, dt_iop_module_t *module,
                                            dt_masks_type_t type, gpointer user_data)
{
  dt_dev_get_global()->form_gui->group_selected = 0;
}

static void _tree_add_exist(GtkButton *button, dt_masks_form_t *grp)
{
  dt_develop_t *const dev = dt_dev_get_global();
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return;
  // we get the new formid
  const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(button), "module");

  // we add the form in this group
  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  grp = dt_masks_cow_touch(dev, grp);
  if(form && dt_masks_group_add_form(dev, grp, form))
  {
    // we save the group
    dt_dev_add_history_item(dev, NULL, FALSE, TRUE);

    // and we apply the change

    dt_iop_gui_blend_masks_update(module);
    dt_dev_masks_selection_change(dev, NULL, grp->formid, TRUE);
  }
}

static void _tree_group(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // we create the new group
  dt_masks_form_t *mask = dt_masks_create(DT_MASKS_GROUP);
  g_snprintf(mask->name, sizeof(mask->name), _("Mask #%d"), g_list_length(dt_dev_get_global()->forms));

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
        dt_masks_form_t *member = dt_masks_get_from_id(dt_dev_get_global(), id);
        if(!IS_NULL_PTR(member))
          dt_masks_group_add_form_with_state(dt_dev_get_global(), mask, member, mask->formid,
                                             DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION, 1.0f);
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  // we add this group to the general list
  dt_dev_get_global()->forms = g_list_append(dt_dev_get_global()->forms, mask);

  // add we save
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
  _lib_masks_recreate_list(self);
  // dt_masks_change_form_gui(darktable.develop, grp);
}

static int _tree_format_form_usage_label(char *str, const size_t str_size,
                                         const dt_masks_form_t *form, const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(str) || IS_NULL_PTR(form)) return -1;

  str[0] = '\0';
  g_strlcat(str, form->name, str_size);

  int nbuse = 0;
  // we search were this form is used
  for(const GList *modules = dt_dev_get_global()->iop; modules; modules = g_list_next(modules))
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
  if(IS_NULL_PTR(form)) return;

  char str[256] = "";

  if(opacity != 1.0f)
  {
    g_snprintf(str, sizeof(str), "%s %d%%",
              form->name, (int)(opacity * 100));
  }
  else
  {
    g_strlcpy(str, form->name, sizeof(str));
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
                     (!IS_NULL_PTR(icop)), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE, (!IS_NULL_PTR(icinv)), -1);
}

static void _tree_delete_unused(GtkButton *button, dt_lib_module_t *self)
{
  dt_develop_t *dev = dt_dev_get_global();

  /* The undo record has to be opened HERE, before the sweep. dt_dev_add_history_item() below
   * opens one of its own, but by then every hist->forms has been rewritten in place and the
   * "before" state it captures is the swept one. dt_dev_history_undo_start_record()'s depth
   * counter makes that inner pair a no-op, so the recorded before/after spans the whole
   * operation.
   *
   * What makes the restore work is that dt_history_duplicate() copies each item's forms LIST
   * (g_list_copy plus one reference per form) rather than aliasing it: the snapshot owns its
   * own cells, the sweep's g_list_remove() on the live items cannot reach them, and every
   * swept shape stays alive as long as the record holds it. _pop_undo() rewrites the database
   * from the restored history, so undoing puts the masks_history rows back too. */
  dt_dev_undo_start_record(dev);

  dt_masks_cleanup_unused(dev);
  _lib_masks_recreate_list(self);

  // The sweep only rewrote the in-memory snapshots. main.history and main.masks_history are
  // rewritten wholesale from dev->history by the write a commit triggers, so without one the
  // deleted shapes stay in the database and come back on the next read -- and, like every other
  // forms mutation here, the deletion is never recorded as its own history step.
  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);

  dt_dev_undo_end_record(dev);
}

static void _add_masks_history_item(dt_lib_masks_t *lm)
{
  const int reset = lm->gui_reset;
  lm->gui_reset = 1;
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
  lm->gui_reset = reset;
}


/* One handler for all five operations. They differed by a single constant and were otherwise
 * identical to the line, which is how five copies of a copy-on-write mistake got written; the
 * operation now rides on the menu item, the way develop/blend_gui.c already carries "blend-state". */
static void _tree_apply_operation(GtkWidget *menu_item, dt_lib_module_t *self)
{
  const dt_masks_state_t operation
      = (dt_masks_state_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "masks-operation"));
  if(operation == DT_MASKS_STATE_NONE) return;

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

      /* The module owns the copy-on-write: it touches the group before resolving the row, which
       * this loop did not do -- it mutated a refcounted membership block that a history snapshot
       * could still be observing. UNCHANGED (the row already had this operator) deliberately does
       * not count as a change, so a no-op click writes no undo step. */
      dt_masks_member_t member;
      if(dt_masks_group_set_member_operation(dt_dev_get_global(), grid, id, operation, &member)
         == DT_MASKS_OK)
      {
        _set_iter_name(lm, dt_masks_get_from_id(dt_dev_get_global(), id), member.state, member.opacity,
                       model, &iter, (int)member.index);
        change = 1;
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
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

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

      dt_masks_form_t *group_form = dt_masks_get_from_id(dt_dev_get_global(), grid);
      group_form = dt_masks_cow_touch(dt_dev_get_global(), group_form);
      dt_masks_form_move(group_form, id, 0);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);

  // Without this, the reorder only mutates the live group's points list: it's never recorded
  // as its own history step, so the next undo/redo silently discards the new order.
  _add_masks_history_item(lm);
}

static void _tree_movedown(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

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

      dt_masks_form_t *group_form = dt_masks_get_from_id(dt_dev_get_global(), grid);
      group_form = dt_masks_cow_touch(dt_dev_get_global(), group_form);
      dt_masks_form_move(group_form, id, 1);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);

  // Without this, the reorder only mutates the live group's points list: it's never recorded
  // as its own history step, so the next undo/redo silently discards the new order.
  _add_masks_history_item(lm);
}

static void _tree_delete_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

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

      dt_masks_form_delete(dt_dev_get_global(), module, dt_masks_get_from_id(dt_dev_get_global(), grid),
                           dt_masks_get_from_id(dt_dev_get_global(), id));
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);

  // Without this, the deletion only mutates the live dev->forms: it's never recorded as its
  // own history step, so the next history navigation (undo/redo) silently discards it and
  // reverts to whatever forms snapshot was last actually committed.
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
}

static void _tree_duplicate_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we get the selected node
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  if(IS_NULL_PTR(items)) return;
  GtkTreePath *item = (GtkTreePath *)items->data;
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(model, &iter, item))
  {
    dt_iop_module_t *module = NULL;
    int grid = -1;
    int id = -1;
    _lib_masks_get_values(model, &iter, &module, &grid, &id);

    // dt_masks_form_duplicate_in_group also attaches the duplicate to the source shape's
    // group (grid), inheriting its state/opacity -- without that, the duplicate would be an
    // orphan: invisible on canvas and useless to the module, since nothing outside a group
    // ever gets rendered.
    const int nid = dt_masks_form_duplicate_in_group(dt_dev_get_global(), grid, id);
    if(nid > 0)
    {
      if(module) dt_iop_gui_blend_masks_update(module);

      dt_dev_masks_selection_change(dt_dev_get_global(), NULL, nid, TRUE);

      // Without this, the new form only exists in the live dev->forms: it's never recorded as
      // its own history step, so it silently disappears on the next undo/redo.
      _add_masks_history_item(lm);

      // _add_masks_history_item briefly sets lm->gui_reset while committing, and
      // dt_dev_add_history_item's own list-change signal fires synchronously inside that
      // window -- _lib_masks_recreate_list's gui_reset guard swallows it. Refresh explicitly,
      // now that gui_reset is back to its prior value, so the new row actually appears.
      _lib_masks_recreate_list(self);
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
  dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), id);
  if(IS_NULL_PTR(form)) return;

  // we want to make sure that the new name is not an empty string. else this would convert
  // in the xmp file into "<rdf:li/>" which produces problems. we use a single whitespace
  // as the pure minimum text.
  gchar *text = strlen(new_text) == 0 ? " " : new_text;

  // first, we need to update the mask name

  g_strlcpy(form->name, text, sizeof(form->name));
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_lib_masks_t *self)
{
  dt_develop_t *const dev = dt_dev_get_global();
  if(self->gui_reset) return;
  dt_masks_form_gui_t *creation_gui = dev->form_gui;
  if(!IS_NULL_PTR(creation_gui) && creation_gui->creation) return;

  // we reset all "show mask" icon of iops
  dt_masks_reset_show_masks_icons(dev);

  // if selection empty, we hide all
  const int nb = gtk_tree_selection_count_selected_rows(selection);
  if(nb == 0)
  {
    dt_masks_change_form_gui(dev, NULL);
    dt_control_queue_redraw_center();
    return;
  }

  // else, we create a new form group with the selection and display it
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(self->treeview));
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  dt_masks_form_t *selected_form = NULL;
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

      dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
      if(!IS_NULL_PTR(form))
      {
        if(nb == 1) selected_form = form;
        dt_masks_group_add_form_with_state(dev, grp, form, grid, DT_MASKS_STATE_USE, 1.0f);
        // we eventually set the "show masks" icon of iops
        if(nb == 1 && (form->type & DT_MASKS_GROUP))
        {
          dt_iop_module_t *module = NULL;
          _lib_masks_get_values(model, &iter, &module, NULL, NULL);

          if(module && module->gui && module->gui->blend_data
             && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
             && !(module->flags() & IOP_FLAGS_NO_MASKS))
          {
            dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->gui->blend_data;
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

  dt_masks_form_t *grp_dest = dt_masks_create(DT_MASKS_GROUP);
  grp_dest->formid = 0;
  dt_masks_group_ungroup(dev, grp_dest, grp);
  dt_masks_change_form_gui(dev, grp_dest);
  dev->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  if(nb == 1 && !IS_NULL_PTR(selected_form))
    dt_masks_center_view_on_form(dev, selected_form);
  else
    dt_dev_pixelpipe_change_zoom_main(dev);
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
  int parentid = 0;
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
        _lib_masks_get_values(model, &iter, NULL, &parentid, &grpid);
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
    dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), grpid);
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
      for(GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
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

  dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), grpid);

  // Same shape-parameter sliders (size/fading/rotation/opacity) as the darkroom canvas's and
  // the blend module's own shape context menus. Available for any single selected shape, not
  // just one nested under a group in the tree: _lib_masks_list_recurs also lists every shape
  // at top level regardless of group membership (TREE_GROUPID == 0 there), so when the tree
  // doesn't hand us the parent directly, look up whichever group actually references it.
  if(nb == 1 && !IS_NULL_PTR(grp) && !(grp->type & DT_MASKS_GROUP))
  {
    const int holding_group = from_group ? parentid
                                         : dt_masks_group_find_holder(dt_dev_get_global(), grpid);

    if(holding_group != 0)
    {
      dt_masks_gui_populate_interaction_sliders(GTK_WIDGET(menu), dt_dev_get_global(), grp, holding_group,
                                                dt_dev_get_global()->form_gui, module);
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    }
  }

  if(from_group && depth < 3)
  {
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());

    // Same "Operation" submenu grouping (Invert/Union/Intersection/Difference/Exclusion) as
    // the darkroom canvas's and the blend module's own shape context menus.
    item = gtk_menu_item_new_with_label(_("Operation"));
    GtkWidget *op_submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), op_submenu);
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("Invert shape"));
    g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_INVERSE));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
    gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
    if(nb == 1)
    {
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("Union"));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_UNION));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
      item = gtk_menu_item_new_with_label(_("Intersection"));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_INTERSECTION));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
      item = gtk_menu_item_new_with_label(_("Difference"));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_DIFFERENCE));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
      item = gtk_menu_item_new_with_label(_("Exclusion"));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_EXCLUSION));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
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

  item = gtk_menu_item_new_with_label(_("Delete unused shapes"));
  g_signal_connect(item, "activate", (GCallback)_tree_delete_unused, self);
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
      if(!dt_modifier_is(event->state, DT_PRIMARY_MASK)) gtk_tree_selection_unselect_all(selection);
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
  if(IS_NULL_PTR(grp))
  {
    for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
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
      dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), point->formid);
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
  if(grp_id == 0) _is_form_used(form->formid, NULL, str2, sizeof(str2), &nbuse);

  if(!(form->type & DT_MASKS_GROUP))
  {
    // we just add it to the tree
    GtkTreeIter child;
    gtk_tree_store_append(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0), TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (!IS_NULL_PTR(icop)), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (!IS_NULL_PTR(icinv)), TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child, index);
  }
  else
  {
    // we first check if it's a "module" group or not
    if(grp_id == 0 && !module)
    {
      for(const GList *iops = dt_dev_get_global()->iop; iops; iops = g_list_next(iops))
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
                       TREE_IC_OP_VISIBLE, (!IS_NULL_PTR(icop)), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (!IS_NULL_PTR(icinv)), TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child, index);

    index = 0;
    // we add all nodes to the tree
    for(const GList *forms = form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(dt_dev_get_global(), grpt->formid);
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
          || (IS_NULL_PTR(module) || (mod && (!g_strcmp0(module->op, mod->op)))));
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
  if(IS_NULL_PTR(lm)) return;
  if(lm->gui_reset) return;

  const int gui_reset = lm->gui_reset;
  lm->gui_reset = 1;
  gboolean sync_center_view = FALSE;

  // if a treeview is already present, let's get the currently selected items
  // as we are going to recreate the tree.
  GList *selectids = NULL;

  if(lm->treeview)
  {
    selectids = _lib_masks_get_selected(self);
  }

  // Rebuilding the shape manager list is also used to refresh shapes created
  // during continuous creation. In that case, the active creation button must
  // stay active until the user cancels creation explicitly.
  dt_masks_form_gui_t *gui = dt_dev_get_global()->form_gui;
  if(IS_NULL_PTR(gui) || !gui->creation) dt_masks_shape_buttons_deactivate_all(NULL);

  GtkTreeStore *treestore;
  // we store : text ; *module ; groupid ; formid
  treestore = gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING);

  // we first add all groups
  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->type & DT_MASKS_GROUP) _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm, 0);
  }

  // and we add all forms
  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
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

  // After list refresh, keep the tree selection aligned with the current GUI module mask group.
  dt_iop_module_t *const current_module = dt_dev_get_global()->gui_module;
  const int current_group_id
      = (!IS_NULL_PTR(current_module) && current_module->blend_params
         && (current_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
         && !(current_module->flags() & IOP_FLAGS_NO_MASKS))
            ? current_module->blend_params->mask_id
            : 0;

  if(current_group_id > 0 && (IS_NULL_PTR(gui) || !gui->creation))
  {
    GtkTreeModel *model = GTK_TREE_MODEL(treestore);
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      const gboolean found = _find_mask_iter_by_values(model, &iter, current_module, current_group_id, 1);
      if(found)
      {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_selection_select_iter(selection, &iter);
        gtk_tree_path_free(path);
        sync_center_view = TRUE;
      }
    }
  }

  g_object_unref(treestore);

  lm->gui_reset = gui_reset;

  if(sync_center_view)
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    _tree_selection_change(selection, lm);
  }
}

static void _lib_masks_update_item(dt_lib_module_t *self, int formid, int parentid, dt_lib_masks_t *lm, GtkTreeModel *model, GtkTreeIter *iter)
{
  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), formid);
  if(IS_NULL_PTR(form)) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), parentid);

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
  if(IS_NULL_PTR(iter)) return 0;

  // we retrieve the ids
  int grid = -1;
  int id = -1;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), id);
  if(IS_NULL_PTR(form)) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), grid);

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
  if(IS_NULL_PTR(lm)) return;
  if(IS_NULL_PTR(lm->treeview)) return;

  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;
  gtk_tree_model_foreach(model, _update_foreach, lm);
}

static gboolean _remove_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  if(IS_NULL_PTR(iter)) return 0;
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
           || (IS_NULL_PTR(module) || (mod && (!g_strcmp0(module->op, mod->op))))))
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
  if(IS_NULL_PTR(lm->treeview)) return;

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
  if(IS_NULL_PTR(self)) return;

  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(IS_NULL_PTR(lm)) return;
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

  else if(event == DT_MASKS_EVENT_DELETE || event == DT_MASKS_EVENT_REMOVE)
  {
    // When a shape is deleted from the model, we may no longer find its previous row in the current tree.
    // In that case, force a full list refresh so stale rows don't remain visible.
    _lib_masks_recreate_list(self);
  }

  else if(event == DT_MASKS_EVENT_ADD)
  {
    _lib_masks_recreate_list(self);
    dt_masks_form_gui_t *gui = dt_dev_get_global()->form_gui;
    if(IS_NULL_PTR(gui) || !gui->creation)
      dt_masks_set_visible_form(dt_dev_get_global(),
                                dt_masks_get_from_id(dt_dev_get_global(), parentid ? parentid : formid));
  }

  dt_control_queue_redraw_center();
}

/* Geometry the user gives the shape manager by hand. The height is not ours: the shape list
 * carries its own persisted height (dt_ui_scroll_wrap below) and the window follows it. */
#define DT_MASKS_PANEL_CONF_WIDTH "plugins/darkroom/masks/windowwidth"
#define DT_MASKS_PANEL_CONF_X "plugins/darkroom/masks/window_x"
#define DT_MASKS_PANEL_CONF_Y "plugins/darkroom/masks/window_y"

/** @brief Is this window on a backend where absolute coordinates mean anything? Wayland gives a
 * client neither its own position nor the right to set it, so there we remember the width only. */
static gboolean _lib_masks_popup_position_is_usable(GtkWidget *window)
{
#ifdef GDK_WINDOWING_WAYLAND
  return !GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(window));
#else
  return TRUE;
#endif
}

/** @brief Remember where the user put the panel and how wide they made it. Called on every path
 * that takes the window off screen, since a hidden window no longer has a position to read. */
static void _lib_masks_popup_save_geometry(dt_lib_masks_t *d)
{
  if(!GTK_IS_WINDOW(d->popup_window) || !gtk_widget_get_visible(d->popup_window)) return;

  gint width = 0;
  gint height = 0;
  gtk_window_get_size(GTK_WINDOW(d->popup_window), &width, &height);
  if(width > 0) dt_conf_set_int(DT_MASKS_PANEL_CONF_WIDTH, width);

  if(!_lib_masks_popup_position_is_usable(d->popup_window)) return;

  gint x = 0;
  gint y = 0;
  gtk_window_get_position(GTK_WINDOW(d->popup_window), &x, &y);
  dt_conf_set_int(DT_MASKS_PANEL_CONF_X, x);
  dt_conf_set_int(DT_MASKS_PANEL_CONF_Y, y);
}

/** @brief Put the panel back where it was left, before it is mapped. With nothing stored -- first
 * run, or a session that never moved it -- nothing is imposed and GTK_WIN_POS_CENTER_ON_PARENT
 * still decides, which is what puts the window on the screen the application is on. */
static void _lib_masks_popup_restore_geometry(dt_lib_masks_t *d)
{
  if(!GTK_IS_WINDOW(d->popup_window)) return;

  gint width = 0;
  gint height = 0;
  gtk_window_get_size(GTK_WINDOW(d->popup_window), &width, &height);

  if(dt_conf_key_exists(DT_MASKS_PANEL_CONF_WIDTH))
  {
    const int stored_width = dt_conf_get_int(DT_MASKS_PANEL_CONF_WIDTH);
    if(stored_width > 0)
    {
      width = stored_width;
      gtk_window_resize(GTK_WINDOW(d->popup_window), width, MAX(height, 1));
    }
  }

  if(!_lib_masks_popup_position_is_usable(d->popup_window)) return;
  if(!dt_conf_key_exists(DT_MASKS_PANEL_CONF_X) || !dt_conf_key_exists(DT_MASKS_PANEL_CONF_Y)) return;

  const int x = dt_conf_get_int(DT_MASKS_PANEL_CONF_X);
  const int y = dt_conf_get_int(DT_MASKS_PANEL_CONF_Y);

  // A position saved on a monitor that is no longer attached would strand the panel off screen,
  // so it is clamped into the work area of whichever monitor it now lands on.
  int clamped_x = x;
  int clamped_y = y;
  GdkDisplay *display = gtk_widget_get_display(d->popup_window);
  if(!IS_NULL_PTR(display))
  {
    GdkMonitor *monitor = gdk_display_get_monitor_at_point(display, x + width / 2, y + height / 2);
    if(IS_NULL_PTR(monitor)) monitor = gdk_display_get_primary_monitor(display);
    if(IS_NULL_PTR(monitor) && gdk_display_get_n_monitors(display) > 0)
      monitor = gdk_display_get_monitor(display, 0);

    if(!IS_NULL_PTR(monitor))
    {
      GdkRectangle workarea = { 0 };
      gdk_monitor_get_workarea(monitor, &workarea);
      clamped_x = CLAMP(x, workarea.x, workarea.x + MAX(0, workarea.width - width));
      clamped_y = CLAMP(y, workarea.y, workarea.y + MAX(0, workarea.height - height));
    }
  }

  gtk_window_move(GTK_WINDOW(d->popup_window), clamped_x, clamped_y);
}

/** @brief Closing from the window manager hides the panel, same as the toolbox button, so its
 * widgets and state survive -- but the geometry has to be read before it goes. */
static gboolean _lib_masks_popup_delete_cb(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  _lib_masks_popup_save_geometry((dt_lib_masks_t *)user_data);
  return gtk_widget_hide_on_delete(window);
}

static void _lib_masks_popup_button_clicked_cb(GtkWidget *button, gpointer user_data)
{
  dt_lib_masks_t *d = (dt_lib_masks_t *)user_data;
  if(!d->popup_window) return;

  if(gtk_widget_get_visible(d->popup_window))
  {
    _lib_masks_popup_save_geometry(d);
    gtk_widget_hide(d->popup_window);
  }
  else
  {
    // before mapping: a move applied to a mapped window makes it jump in view
    _lib_masks_popup_restore_geometry(d);
    gtk_widget_show_all(d->popup_window);
  }
}

/* Idle callback to add the popup button to the module toolbox once the
 * module_toolbox proxy has been initialized. Returns FALSE when done so
 * it is removed from the idle loop. */
static gboolean _lib_masks_add_popup_button_idle(gpointer user_data)
{
  dt_lib_masks_t *d = (dt_lib_masks_t *)user_data;
  if(!d || !d->popup_button) return FALSE;

  if(dt_view_manager_get_global()->proxy.module_toolbox.module)
  {
    dt_view_manager_module_toolbox_add(dt_view_manager_get_global(), d->popup_button, DT_VIEW_DARKROOM);
    return FALSE; /* stop calling this idle handler */
  }
  return TRUE; /* try again later */
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
  d->ic_union = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, bs2 * 2, bs2);
  d->ic_intersection = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, bs2 * 2, bs2);
  d->ic_difference = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, bs2 * 2, bs2);
  d->ic_exclusion = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, bs2 * 2, bs2);

  // 2. Setup the non-modal popup window
  d->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(d->popup_window), _("Shape Manager Panel"));
  gtk_window_set_type_hint(GTK_WINDOW(d->popup_window), GDK_WINDOW_TYPE_HINT_UTILITY);
  
  // NON-MODAL & NO FOCUS STEAL: Prevents window manager from stealing active focus when mapped/shown
  // because it contains drawing tools that should draw on main window
  gtk_window_set_modal(GTK_WINDOW(d->popup_window), FALSE);
  gtk_window_set_focus_on_map(GTK_WINDOW(d->popup_window), FALSE);
  gtk_window_set_accept_focus(GTK_WINDOW(d->popup_window), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(d->popup_window), GTK_WINDOW(dt_gui_main_window()));

  // Being transient for the main window does not decide where the window manager puts this
  // one: with no position asked for, it lands at the root origin, i.e. on the leftmost
  // monitor rather than on the one the application sits on. GTK honours the hint on the
  // first mapping only, so a panel the user has dragged elsewhere keeps its place.
  gtk_window_set_position(GTK_WINDOW(d->popup_window), GTK_WIN_POS_CENTER_ON_PARENT);

  // Let the user shrink the panel down to a narrow strip: the shape list carries its own
  // height rule (dt_ui_scroll_wrap below), the width is theirs to set.
  gtk_widget_set_size_request(d->popup_window, DT_PIXEL_APPLY_DPI(300), -1);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->popup_window);
#endif

  // Intercept the window close action to hide the widget instead of completely destroying it
  g_signal_connect(G_OBJECT(d->popup_window), "delete-event", G_CALLBACK(_lib_masks_popup_delete_cb), d);

  // 3. Create a clean box container inside the popup window to receive original shape elements
  GtkWidget *shape_manager_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(d->popup_window), shape_manager_container);

  // The main container for module blending params.
  // It's populated in blend_gui.c when a mask-able module gets focused.
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);


  // Create and pack the button to control the popup panel.
  // NOTE: it's added to the darkroom module toolbox, aka not here.
  d->popup_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_drawn, 0, NULL);
  gtk_widget_set_tooltip_text(d->popup_button, _("Open shape manager..."));

  /* module_toolbox may not be initialized yet when modules are being created.
   * Schedule adding the popup button via an idle callback so it runs after
   * other modules (including the module_toolbox) have had their gui_init
   * called. The callback will remove itself once it succeeds. */
  g_idle_add((GSourceFunc)_lib_masks_add_popup_button_idle, d);
  g_signal_connect(G_OBJECT(d->popup_button), "clicked", G_CALLBACK(_lib_masks_popup_button_clicked_cb), d);

  // From here, everything goes into the mask manager popup,
  // so there is no child added to self->widget from here.
  GtkWidget *shape_buttons[DEVELOP_MASKS_NB_SHAPES] = { 0 };
  const dt_masks_shape_buttons_config_t shape_buttons_config = {
    .dev = dt_dev_get_global(),
    .owner_module = NULL,
    .creation_module = NULL,
    .buttons = shape_buttons,
    .types = NULL,
    .action_section = NULL,
    .flags = DT_MASKS_SHAPE_BUTTONS_ALL,
    .register_flags = DT_MASKS_SHAPE_BUTTONS_NONE,
    .local = FALSE,
    .user_data = NULL,
    .can_start = NULL,
    .form_type = NULL,
    .started = _lib_masks_shape_button_started,
    .exited = NULL,
  };
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *shape_buttons_box = dt_masks_shape_buttons_create(&shape_buttons_config);
  gtk_box_pack_start(GTK_BOX(hbox), shape_buttons_box, FALSE, FALSE, 0);
  d->bt_gradient = shape_buttons[DT_MASKS_SHAPE_INDEX_GRADIENT];
  d->bt_path = shape_buttons[DT_MASKS_SHAPE_INDEX_POLYGON];
  d->bt_ellipse = shape_buttons[DT_MASKS_SHAPE_INDEX_ELLIPSE];
  d->bt_circle = shape_buttons[DT_MASKS_SHAPE_INDEX_CIRCLE];
  d->bt_brush = shape_buttons[DT_MASKS_SHAPE_INDEX_BRUSH];

  // The button row keeps its natural height and stays at the top: expanding it would split the
  // surplus with the shape list below and stretch the buttons vertically. It is the container's
  // first child, so nothing separates it from the window edge -- give it the same gap the
  // container puts between its children.
  gtk_widget_set_margin_top(hbox, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(shape_manager_container), hbox, FALSE, FALSE, 0);

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
  // Themed icon marking a shape shared by several modules, same pattern as the
  // trash icons of the shapes lists in develop/blend_gui.c: the renderer names the icon
  // and the theme draws it, the model only carries whether this row shows one.
  renderer = gtk_cell_renderer_pixbuf_new();
  g_object_set(renderer, "icon-name", "mail-attachment-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
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

  // Auto-grows to its content (the side panel scrolls) up to a user-set, persisted height.
  gtk_box_pack_start(GTK_BOX(shape_manager_container),
                     dt_ui_scroll_wrap(d->treeview, 90, "plugins/darkroom/masks/windowheight",
                                       DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, G_CALLBACK(_lib_masks_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED,
                                  G_CALLBACK(_lib_masks_blending_gui_changed_callback), self);

  // set proxy functions
  dt_dev_get_global()->proxy.masks.module = self;
  dt_dev_get_global()->proxy.masks.list_change = _lib_masks_recreate_list;
  dt_dev_get_global()->proxy.masks.list_update = _lib_masks_update_list;
  dt_dev_get_global()->proxy.masks.list_remove = _lib_masks_remove_item;
  dt_dev_get_global()->proxy.masks.selection_change = _lib_masks_selection_change;

  _lib_masks_blending_gui_changed_callback(NULL, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  if(self && self->data)
  {
    dt_lib_masks_t *d = (dt_lib_masks_t *)self->data;

    // Destroy window allocation to prevent leaks
    if(d->popup_window)
    {
      // leaving with the panel open still counts as where the user left it
      _lib_masks_popup_save_geometry(d);
      gtk_widget_destroy(d->popup_window);
      d->popup_window = NULL;
    }

    if(!IS_NULL_PTR(d->ic_inverse)) g_object_unref(d->ic_inverse);
    if(!IS_NULL_PTR(d->ic_union)) g_object_unref(d->ic_union);
    if(!IS_NULL_PTR(d->ic_intersection)) g_object_unref(d->ic_intersection);
    if(!IS_NULL_PTR(d->ic_difference)) g_object_unref(d->ic_difference);
    if(!IS_NULL_PTR(d->ic_exclusion)) g_object_unref(d->ic_exclusion);

    d->ic_inverse = NULL;
    d->ic_union = NULL;
    d->ic_intersection = NULL;
    d->ic_difference = NULL;
    d->ic_exclusion = NULL;
    _lib_masks_release_blending(self);
  }

  dt_free(self->data);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_lib_masks_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_lib_masks_blending_gui_changed_callback), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
