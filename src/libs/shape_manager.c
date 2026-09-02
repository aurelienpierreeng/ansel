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
#include "develop/masks/masks_history.h"   // dt_masks_form_unref()
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

#include "widgets/label.h"   // dt_gui_symbolic_icon_pixbuf()
#include "widgets/togglebutton.h"
#include "control/signal.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>   // conditional-ok: GDK_IS_WAYLAND_DISPLAY() is used only inside the same #ifdef
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


DT_MODULE(1)

static void _shape_manager_recreate_list(dt_lib_module_t *self);
static void _shape_manager_update_list(dt_lib_module_t *self);
static void _shape_manager_broadcast(dt_lib_module_t *self, const int formid, const int parentid,
                                     const dt_masks_event_t event);

typedef struct dt_shape_manager_t
{
  GtkWidget *treeview;

  /* The rightmost column, the one carrying the per-row trash / minus icon. Kept because a click
   * and a tooltip are both answered by comparing against the column the pointer is over. */
  GtkTreeViewColumn *action_col;

  /* Replacement for shape_manager_expander */
  GtkWidget *popup_window;
  GtkWidget *popup_button;

  GdkPixbuf *ic_used;
  GdkPixbuf *ic_inverse;
  GdkPixbuf *ic_union;
  GdkPixbuf *ic_intersection;
  GdkPixbuf *ic_difference;
  GdkPixbuf *ic_exclusion;
  int gui_reset;
} dt_shape_manager_t;


const char *name(struct dt_lib_module_t *self __attribute__((unused)))
{
  return _("Shape Manager");
}

/* Never shown in a panel: everything this module builds lives in its own window, opened by the
 * button it pushes into the darkroom toolbox. Same arrangement as libs/export.c, which is also a
 * module whose interface is a window rather than a panel section. */
const char **views(dt_lib_module_t *self __attribute__((unused)))
{
  static const char *v[] = {"special", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self __attribute__((unused)))
{
  return DT_UI_CONTAINER_SIZE;
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
  /* Which of the two action icons the row shows -- a top-level row deletes, a row under a group
   * detaches. Exactly one is TRUE on a form row, and neither on the separator. */
  TREE_IC_DELETE_VISIBLE,
  TREE_IC_UNLINK_VISIBLE,
  TREE_IS_SEPARATOR,
  TREE_COUNT
} dt_masks_tree_cols_t;

static void _shape_manager_get_values(GtkTreeModel *model, GtkTreeIter *iter,
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

/* The shape to create rides on the menu item, the way "masks-operation" already does below --
 * one handler for the five entries, which is also what lets them be built from a loop. Arming the
 * tool is dt_masks_creation_mode_enter()'s business alone, toolbars included: it tells every shape
 * toolbar to press the matching button, so the entry and the button agree without either knowing
 * about the other. */
static void _tree_add_shape(GtkWidget *menu_item, dt_iop_module_t *module)
{
  const dt_masks_type_t type
      = (dt_masks_type_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "masks-shape-type"));

  dt_masks_creation_mode_enter(dt_dev_get_global(), module, type);
  dt_dev_get_global()->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _tree_add_shape_menu_item(GtkWidget *menu, const dt_masks_type_t type, dt_iop_module_t *module)
{
  GtkWidget *item = dt_masks_shape_menu_item_new(menu, type, G_CALLBACK(_tree_add_shape), module);
  if(!IS_NULL_PTR(item)) g_object_set_data(G_OBJECT(item), "masks-shape-type", GINT_TO_POINTER(type));
}

static void _shape_manager_shape_button_started(GtkWidget *button __attribute__((unused)), dt_iop_module_t *module __attribute__((unused)),
                                            dt_masks_type_t type __attribute__((unused)), gpointer user_data __attribute__((unused)))
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

  /* Raised rather than broadcast: unlike the handlers above, this one does not rebuild the tree
   * itself, so our own handler is left to do it as well as blend_gui's. */
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, 0, 0,
                                  DT_MASKS_EVENT_CHANGE);
  }
}

static void _tree_group(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  // we create the new group
  // create_ext registers the group in dev->allforms and dt_masks_append_form() below takes
  // dev->forms's own reference, so both lists have a claim and teardown balances. Neither
  // touches dev->forms outside masks_mutex, which a hand-rolled g_list_append does -- and the
  // pipeline thread reads that list under the same lock.
  dt_masks_form_t *mask = dt_masks_create_ext(dt_dev_get_global(), DT_MASKS_GROUP);
  g_snprintf(mask->name, sizeof(mask->name), _("Mask #%d"), g_list_length(dt_dev_get_global()->forms));

  // we add all selected forms to this group
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter(model, &iter, item)) continue;

    int id = -1;
    _shape_manager_get_values(model, &iter, NULL, NULL, &id);
    if(id <= 0) continue;

    dt_masks_form_t *member = dt_masks_get_from_id(dt_dev_get_global(), id);
    if(IS_NULL_PTR(member)) continue;

    dt_masks_group_add_form_with_state(dt_dev_get_global(), mask, member, mask->formid,
                                       DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION, 1.0f);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  // we add this group to the general list
  dt_masks_append_form(dt_dev_get_global(), mask);

  // add we save
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);
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
    const dt_masks_form_t *grp = dt_masks_get_from_id(m->dev, m->blend_params->mask_id);
    if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) continue;

    for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
    {
      const dt_masks_form_group_t *pt = (const dt_masks_form_group_t *)pts->data;
      if(pt->formid != form->formid) continue;

      // The caller's own module is not worth naming to it, and says so by asking for no label.
      if(m == module) return -1;

      if(nbuse == 0) g_strlcat(str, " (", str_size);
      g_strlcat(str, " ", str_size);
      gchar *module_label = dt_history_item_get_name(m);
      g_strlcat(str, module_label, str_size);
      dt_free(module_label);
      nbuse++;
    }
  }

  if(nbuse > 0) g_strlcat(str, " )", str_size);
  return nbuse;
}

static void _set_iter_name(dt_shape_manager_t *lm, dt_masks_form_t *form, int state, float opacity,
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

static void _tree_delete_unused(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
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
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  // The sweep only rewrote the in-memory snapshots. main.history and main.masks_history are
  // rewritten wholesale from dev->history by the write a commit triggers, so without one the
  // deleted shapes stay in the database and come back on the next read -- and, like every other
  // forms mutation here, the deletion is never recorded as its own history step.
  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);

  dt_dev_undo_end_record(dev);
}

/* Tells the rest of the GUI that the shapes changed.
 *
 * The Drawn tab of a module's blending panel keeps its own two lists, and blend_gui.c refreshes
 * them from DT_SIGNAL_MASK_CHANGED -- the same signal it raises when the user edits shapes from
 * there, which is how this manager already hears about those. Only this direction was missing:
 * the manager rebuilt its own tree and told nobody, so a shape grouped, renamed or deleted here
 * stayed as it was in the panel until something else happened to refresh it.
 *
 * gui_reset is held over the raise because the caller rebuilds our own tree itself: it makes our
 * own handler's rebuild a no-op instead of doing the same work twice. blend_gui's handler is a
 * different callback with its own data and is unaffected.
 *
 * The ids only matter for DT_MASKS_EVENT_UPDATE, the one event both handlers answer by
 * refreshing a single row; anything else refreshes the whole list on either side. */
static void _shape_manager_broadcast(dt_lib_module_t *self, const int formid, const int parentid,
                                     const dt_masks_event_t event)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  const int reset = lm->gui_reset;
  lm->gui_reset = 1;
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, formid, parentid, event);
  lm->gui_reset = reset;
}

static void _add_masks_history_item(dt_shape_manager_t *lm)
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

  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
      _shape_manager_get_values(model, &iter, NULL, &grid, &id);

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

  /* Raised rather than broadcast: unlike the handlers above, this one does not rebuild the tree
   * itself, so our own handler is left to do it as well as blend_gui's. */
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, 0, 0,
                                  DT_MASKS_EVENT_CHANGE);

    dt_control_queue_redraw_center();
  }
}

static void _tree_moveup(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
      _shape_manager_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *group_form = dt_masks_get_from_id(dt_dev_get_global(), grid);
      group_form = dt_masks_cow_touch(dt_dev_get_global(), group_form);
      dt_masks_form_move(group_form, id, 0);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  // Without this, the reorder only mutates the live group's points list: it's never recorded
  // as its own history step, so the next undo/redo silently discards the new order.
  _add_masks_history_item(lm);
}

static void _tree_movedown(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
      _shape_manager_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *group_form = dt_masks_get_from_id(dt_dev_get_global(), grid);
      group_form = dt_masks_cow_touch(dt_dev_get_global(), group_form);
      dt_masks_form_move(group_form, id, 1);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  // Without this, the reorder only mutates the live group's points list: it's never recorded
  // as its own history step, so the next undo/redo silently discards the new order.
  _add_masks_history_item(lm);
}

static void _tree_delete_shape(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
      _shape_manager_get_values(model, &iter, &module, &grid, &id);

      dt_masks_form_delete(dt_dev_get_global(), module, dt_masks_get_from_id(dt_dev_get_global(), grid),
                           dt_masks_get_from_id(dt_dev_get_global(), id));
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  lm->gui_reset = 0;
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  // Without this, the deletion only mutates the live dev->forms: it's never recorded as its
  // own history step, so the next history navigation (undo/redo) silently discards it and
  // reverts to whatever forms snapshot was last actually committed.
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);
}

/* The per-row action icon at the right end of every form row, the same two the shape lists of
 * the Drawn tab offer (develop/blend_gui.c): a top-level row carries a trash and is deleted from
 * every mask and from the list of shapes, a row under a group carries a minus and is only
 * detached from that group, staying available for reuse. Which one a row shows is the model's
 * business (TREE_IC_DELETE_VISIBLE / TREE_IC_UNLINK_VISIBLE); which one a click means is decided
 * here, from the same group id, so the icon and the action cannot disagree.
 *
 * dt_masks_form_delete() reads that distinction off its group argument: a group to detach from,
 * or NULL to destroy. A top-level row has group id 0, which no form answers to. */
static void _tree_row_action(dt_lib_module_t *self, GtkTreeModel *model, GtkTreeIter *iter)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  dt_develop_t *const dev = dt_dev_get_global();

  dt_iop_module_t *module = NULL;
  int grid = -1;
  int id = -1;
  _shape_manager_get_values(model, iter, &module, &grid, &id);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(IS_NULL_PTR(form)) return;

  // Only the permanent delete destroys anything, so only it asks.
  if(grid == 0 && !dt_masks_gui_confirm_permanent_delete(form->name)) return;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dev, NULL);

  lm->gui_reset = 1;
  dt_masks_form_delete(dev, module, dt_masks_get_from_id(dev, grid), form);
  lm->gui_reset = 0;

  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  // Without this, the change only mutates the live dev->forms: it's never recorded as its own
  // history step, so the next history navigation (undo/redo) silently discards it and reverts
  // to whatever forms snapshot was last actually committed.
  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);
}

static void _tree_duplicate_shape(GtkButton *button __attribute__((unused)), dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
    _shape_manager_get_values(model, &iter, &module, &grid, &id);

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
      // window -- _shape_manager_recreate_list's gui_reset guard swallows it. Refresh explicitly,
      // now that gui_reset is back to its prior value, so the new row actually appears.
      _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;
}

/* The "edited" signal hands both strings as gchar *, but this only reads them -- and the
 * connection goes through a GCallback cast, so nothing checks the signature against GTK's. */
static void _tree_cell_edited(GtkCellRendererText *cell __attribute__((unused)), const gchar *path_string,
                              const gchar *new_text, dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  int id = -1;
  _shape_manager_get_values(model, &iter, NULL, NULL, &id);
  dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), id);
  if(IS_NULL_PTR(form)) return;

  // we want to make sure that the new name is not an empty string. else this would convert
  // in the xmp file into "<rdf:li/>" which produces problems. we use a single whitespace
  // as the pure minimum text.
  const gchar *text = strlen(new_text) == 0 ? " " : new_text;

  // first, we need to update the mask name

  g_strlcpy(form->name, text, sizeof(form->name));
  dt_dev_add_history_item(dt_dev_get_global(), NULL, FALSE, TRUE);

  /* Raised rather than broadcast: unlike the handlers above, this one does not rebuild the tree
   * itself, so our own handler is left to do it as well as blend_gui's. */
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, 0, 0,
                                DT_MASKS_EVENT_CHANGE);
}

/* A group's own module, when the tree row names one that can show masks. Presses its "show and
 * edit" toggle, so selecting a module's mask group in the manager lights the module's own
 * button too. */
static void _show_masks_on_owning_module(GtkTreeModel *model, GtkTreeIter *iter)
{
  dt_iop_module_t *module = NULL;
  _shape_manager_get_values(model, iter, &module, NULL, NULL);

  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->blend_data)
     || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || (module->flags() & IOP_FLAGS_NO_MASKS))
    return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->gui->blend_data;
  bd->masks_shown = 1;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
  gtk_widget_queue_draw(bd->masks_edit);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_shape_manager_t *self)
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
    if(!gtk_tree_model_get_iter(model, &iter, item)) continue;

    int grid = -1;
    int id = -1;
    _shape_manager_get_values(model, &iter, NULL, &grid, &id);

    dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
    if(IS_NULL_PTR(form)) continue;

    if(nb == 1) selected_form = form;
    dt_masks_group_add_form_with_state(dev, grp, form, grid, DT_MASKS_STATE_USE, 1.0f);

    // we eventually set the "show masks" icon of iops
    if(nb == 1 && (form->type & DT_MASKS_GROUP)) _show_masks_on_owning_module(model, &iter);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  items = NULL;

  dt_masks_form_t *grp_dest = dt_masks_create(DT_MASKS_GROUP);
  grp_dest->formid = 0;
  dt_masks_group_ungroup(dev, grp_dest, grp);
  // grp was a scratch group built to flatten the selection into grp_dest, which only reads it.
  // It never joined dev->forms or dev->allforms, so this is the only reference there is --
  // unlike grp_dest, whose reference passes to form_visible below.
  dt_masks_form_unref(grp);
  dt_masks_change_form_gui(dev, grp_dest);
  dev->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  if(nb == 1 && !IS_NULL_PTR(selected_form))
    dt_masks_center_view_on_form(dev, selected_form);
  else
    dt_dev_pixelpipe_change_zoom_main(dev);
}

/* The five shapes a group can gain, as their own submenu. Offered on an empty selection and on a
 * selected group alike, which is why it is not written out twice. */
static void _menu_append_new_shape_submenu(GtkMenuShell *menu, dt_iop_module_t *module)
{
  GtkWidget *add_menu = gtk_menu_new();
  GtkWidget *add_item = gtk_menu_item_new_with_label(_("Add new shape ..."));
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(add_item), add_menu);
  gtk_menu_shell_append(menu, add_item);

  _tree_add_shape_menu_item(add_menu, DT_MASKS_BRUSH, module);
  _tree_add_shape_menu_item(add_menu, DT_MASKS_CIRCLE, module);
  _tree_add_shape_menu_item(add_menu, DT_MASKS_ELLIPSE, module);
  _tree_add_shape_menu_item(add_menu, DT_MASKS_POLYGON, module);
  _tree_add_shape_menu_item(add_menu, DT_MASKS_GRADIENT, module);
}

/* The shapes already drawn on this image that grp could take, each labelled with the modules
 * already using it. A shape the caller's own module holds is skipped -- that is what the label
 * formatter reports by refusing to write a label. */
static void _menu_append_existing_shapes(GtkMenuShell *menu, dt_masks_form_t *grp, const int grpid,
                                         dt_iop_module_t *module)
{
  gboolean any = FALSE;
  GtkWidget *shapes_menu = gtk_menu_new();

  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_t *form = (const dt_masks_form_t *)forms->data;
    if((form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) || form->formid == grpid) continue;

    char str[10000] = "";
    if(_tree_format_form_usage_label(str, sizeof(str), form, module) == -1) continue;

    GtkWidget *item = gtk_menu_item_new_with_label(str);
    g_object_set_data(G_OBJECT(item), "formid", GUINT_TO_POINTER(form->formid));
    g_object_set_data(G_OBJECT(item), "module", module);
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_tree_add_exist), grp);
    gtk_menu_shell_append(GTK_MENU_SHELL(shapes_menu), item);
    any = TRUE;
  }

  if(!any)
  {
    gtk_widget_destroy(shapes_menu);
    return;
  }

  GtkWidget *item = gtk_menu_item_new_with_label(_("Add shape ..."));
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), shapes_menu);
  gtk_menu_shell_append(menu, item);
}

/* One entry per combine mode, plus the reordering pair. The same Invert/Union/Intersection/
 * Difference/Exclusion grouping the darkroom canvas and the blend module offer; all five differ
 * by a constant, so they ride on the menu item under "masks-operation". */
static void _menu_append_operations(GtkMenuShell *menu, dt_lib_module_t *self, const int nb)
{
  static const struct
  {
    const char *label;
    dt_masks_state_t state;
  } combine[] = {
    { N_("Union"), DT_MASKS_STATE_UNION },
    { N_("Intersection"), DT_MASKS_STATE_INTERSECTION },
    { N_("Difference"), DT_MASKS_STATE_DIFFERENCE },
    { N_("Exclusion"), DT_MASKS_STATE_EXCLUSION },
  };

  gtk_menu_shell_append(menu, gtk_separator_menu_item_new());

  GtkWidget *item = gtk_menu_item_new_with_label(_("Operation"));
  GtkWidget *op_submenu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), op_submenu);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("Invert shape"));
  g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(DT_MASKS_STATE_INVERSE));
  g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
  gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);

  // Combining is a question about one shape against its group; several at once has no answer.
  if(nb == 1)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), gtk_separator_menu_item_new());
    for(size_t i = 0; i < sizeof(combine) / sizeof(combine[0]); i++)
    {
      item = gtk_menu_item_new_with_label(_(combine[i].label));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(combine[i].state));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
    }
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
    // A single selected row is the only case whose group the menu below needs to know about;
    // read it before the list of paths is freed.
    if(nb == 1 && gtk_tree_model_get_iter(model, &iter, it0))
      _shape_manager_get_values(model, &iter, NULL, &parentid, &grpid);
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    selected = NULL;
  }
  if(depth > 1) from_group = 1;

  // The form the single selected row names, when there is one: several sections below ask
  // whether it is a group, and one lookup answers them all.
  dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), grpid);
  const gboolean grp_is_group = !IS_NULL_PTR(grp) && (grp->type & DT_MASKS_GROUP);

  if(nb == 0)
  {
    _menu_append_new_shape_submenu(menu, module);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }

  if(nb == 1 && grp_is_group)
  {
    _menu_append_new_shape_submenu(menu, module);
    _menu_append_existing_shapes(menu, grp, grpid, module);
  }

  if(nb > 1 && !from_group)
  {
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("Group the forms"));
    g_signal_connect(item, "activate", (GCallback)_tree_group, self);
    gtk_menu_shell_append(menu, item);
  }

  // Same shape-parameter sliders (size/fading/rotation/opacity) as the darkroom canvas's and
  // the blend module's own shape context menus. Available for any single selected shape, not
  // just one nested under a group in the tree: _shape_manager_list_recurs also lists every shape
  // at top level regardless of group membership (TREE_GROUPID == 0 there), so when the tree
  // doesn't hand us the parent directly, look up whichever group actually references it.
  if(nb == 1 && !IS_NULL_PTR(grp) && !grp_is_group)
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

  if(from_group && depth < 3) _menu_append_operations(menu, self, nb);

  if(!from_group && !grp_is_group && nb == 1)
  {
    item = gtk_menu_item_new_with_label(_("Duplicate shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_duplicate_shape, self);
    gtk_menu_shell_append(menu, item);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }
  
  if(!from_group && nb > 0)
  {
    // One entry, named for what the row holds -- the whole mask when it is a group.
    item = gtk_menu_item_new_with_label(grp_is_group ? _("Delete mask") : _("Delete shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
    gtk_menu_shell_append(menu, item);
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

/* The selection a left click asks for. Ctrl toggles the row, so a second one takes it back out
 * rather than being a no-op; Shift extends from the cursor, which is the anchor GTK would have
 * used, and rows the range picks up that _tree_restrict_select refuses -- a different parent, a
 * different depth -- are dropped by it as usual. A click on blank space clears the selection.
 *
 * Returns whether the gesture was answered here, which is what the caller reports as handled:
 * an unmodified click on a row is left to GtkTreeView, the one case it does act on. */
static int _tree_apply_click_selection(GtkWidget *treeview, GtkTreeSelection *selection,
                                       const GdkEventButton *event, GtkTreePath *mouse_path)
{
  if(IS_NULL_PTR(mouse_path))
  {
    gtk_tree_selection_unselect_all(selection);
    return 0;
  }

  if(dt_modifier_is(event->state, DT_PRIMARY_MASK))
  {
    if(gtk_tree_selection_path_is_selected(selection, mouse_path))
      gtk_tree_selection_unselect_path(selection, mouse_path);
    else
      gtk_tree_selection_select_path(selection, mouse_path);
    return 1;
  }

  if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
  {
    GtkTreePath *anchor = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(treeview), &anchor, NULL);
    if(anchor)
    {
      gtk_tree_selection_select_range(selection, anchor, mouse_path);
      gtk_tree_path_free(anchor);
    }
    else
      gtk_tree_selection_select_path(selection, mouse_path);
    return 1;
  }

  return 0;
}

/* Ctrl+click toggles a row, Shift+click extends from the cursor. GtkTreeView's own handler does
 * neither on this tree: measured, the intent masks it derives are the expected ones (modify 0x4,
 * extend 0x1), the event carries them, the selection is GTK_SELECTION_MULTIPLE and the select
 * function allows the row -- yet no selection change follows a modified click, while a plain one
 * works. Why it stays inert was not found; driving the two gestures here is not a workaround for
 * that so much as the place this widget already adjusts its own selection. */
static int _tree_button_pressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  // we first need to adjust selection
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));

  GtkTreePath *mouse_path = NULL;
  GtkTreeViewColumn *mouse_col = NULL;
  GtkTreeIter iter;
  gboolean on_row = FALSE;
  dt_iop_module_t *module = NULL;
  int handled = 0;
  // mouse_path is non-NULL exactly when the pointer is over a row, so it answers "on a row?" too.
  // The module is only wanted for the context menu below; a row that resolves to no iter simply
  // leaves it NULL, which is what _tree_context_menu() already expects.
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &mouse_path,
                                   &mouse_col, NULL, NULL)
     && gtk_tree_model_get_iter(model, &iter, mouse_path))
  {
    on_row = TRUE;
    _shape_manager_get_values(model, &iter, &module, NULL, NULL);
  }
  /* single click with the right mouse button? */
  if(event->type == GDK_BUTTON_PRESS && event->button == 1)
  {
    dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
    // The action icons act on the row under the pointer alone, whatever is selected: they are
    // buttons the row carries, not a command applied to the selection.
    if(on_row && mouse_col == lm->action_col)
    {
      gtk_tree_path_free(mouse_path);
      _tree_row_action(self, model, &iter);
      return 1;
    }

    handled = _tree_apply_click_selection(treeview, selection, event, mouse_path);
  }
  else if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    // if we are already inside the selection, no change
    if(!IS_NULL_PTR(mouse_path) && !gtk_tree_selection_path_is_selected(selection, mouse_path))
    {
      if(!dt_modifier_is(event->state, DT_PRIMARY_MASK)) gtk_tree_selection_unselect_all(selection);
      gtk_tree_selection_select_path(selection, mouse_path);
    }

    // and we display the context-menu
    GtkWidget *menu = _tree_context_menu(selection, model, self, module);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    handled = 1;
  }

  // One exit for the path: it was leaked on every button-1 press before, and freed on only one
  // of the two button-3 branches.
  if(mouse_path) gtk_tree_path_free(mouse_path);
  return handled;
}

static gboolean _tree_restrict_select(GtkTreeSelection *selection, GtkTreeModel *model __attribute__((unused)), GtkTreePath *path,
                                      gboolean path_currently_selected, gpointer data)
{
  dt_shape_manager_t *self = (dt_shape_manager_t *)data;
  if(self->gui_reset) return TRUE;

  // if the change is SELECT->UNSELECT no pb
  if(path_currently_selected) return TRUE;

  // if selection is empty, no pb
  if(gtk_tree_selection_count_selected_rows(selection) == 0) return TRUE;

  /* A row joins the selection only among peers: the same depth, and for a child row the same
   * parent. Whatever is already selected and does not qualify is dropped.
   *
   * The rows to drop are gathered before any is dropped. Unselecting re-enters this function --
   * with path_currently_selected TRUE, so those calls return at the top -- and the previous form
   * answered that by re-reading the selection and restarting the walk after every single
   * removal, which is quadratic for no gain: the list gtk_tree_selection_get_selected_rows()
   * returns is our own copy, and unselecting does not touch it. */
  const int *indices = gtk_tree_path_get_indices(path);
  const int depth = gtk_tree_path_get_depth(path);

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  GList *doomed = NULL;
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    const int dd = gtk_tree_path_get_depth(item);
    const int *ii = gtk_tree_path_get_indices(item);
    const gboolean peer = (dd == depth) && (dd == 1 || ii[dd - 2] == indices[dd - 2]);
    if(!peer) doomed = g_list_prepend(doomed, item);
  }

  for(const GList *doomed_iter = doomed; doomed_iter; doomed_iter = g_list_next(doomed_iter))
    gtk_tree_selection_unselect_path(selection, (GtkTreePath *)doomed_iter->data);

  // doomed borrows its paths from items, which owns and frees them
  g_list_free(doomed);
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

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
  dt_shape_manager_t *lm = (dt_shape_manager_t *)data;

  /* The action icon says what it does, and what it does depends on the row's depth, so the
   * pointer's column is asked first: gtk_tree_view_get_tooltip_context() below reports the row
   * but not the column, and it rewrites x/y on the way. Keyboard tooltips carry no position. */
  if(!keyboard_tip && !IS_NULL_PTR(lm))
  {
    gint bx = 0, by = 0;
    gtk_tree_view_convert_widget_to_bin_window_coords(tree_view, x, y, &bx, &by);

    GtkTreePath *action_path = NULL;
    GtkTreeViewColumn *action_column = NULL;
    if(gtk_tree_view_get_path_at_pos(tree_view, bx, by, &action_path, &action_column, NULL, NULL))
    {
      GtkTreeIter action_iter;
      int grid = -1;
      gboolean got = (action_column == lm->action_col)
                     && gtk_tree_model_get_iter(model, &action_iter, action_path);
      if(got) _shape_manager_get_values(model, &action_iter, NULL, &grid, NULL);
      gtk_tree_path_free(action_path);

      if(got)
      {
        gtk_tooltip_set_text(tooltip,
                             (grid == 0)
                                 ? _("Permanently delete this shape. It is detached from every mask "
                                     "and removed from the list of available shapes.")
                                 : _("Detach this shape from the mask. The shape is kept and stays "
                                     "available for reuse."));
        return TRUE;
      }
    }
  }

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

/* Appends the name of every group inside grp that lists formid, one per line, and counts them.
 * Recurses into member groups, so a shape held by a nested group names that group too. */
static void _groups_naming_form(const int formid, const dt_masks_form_t *grp, char *text,
                                const size_t text_length, int *nb)
{
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return;

  for(const GList *points = grp->points; points; points = g_list_next(points))
  {
    const dt_masks_form_group_t *point = (const dt_masks_form_group_t *)points->data;
    const dt_masks_form_t *form = dt_masks_get_from_id(dt_dev_get_global(), point->formid);
    if(IS_NULL_PTR(form)) continue;

    if(point->formid == formid)
    {
      (*nb)++;
      if(*nb > 1) g_strlcat(text, "\n", text_length);
      g_strlcat(text, grp->name, text_length);
    }

    if(form->type & DT_MASKS_GROUP) _groups_naming_form(formid, form, text, text_length, nb);
  }
}

/* Same, over every group in the image. The entry and the walk used to be one function switching
 * on a NULL group, which is why it took one. */
static void _is_form_used(const int formid, char *text, const size_t text_length, int *nb)
{
  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_t *form = (const dt_masks_form_t *)forms->data;
    if(form->type & DT_MASKS_GROUP) _groups_naming_form(formid, form, text, text_length, nb);
  }
}

/* What one row of the tree says about the form it shows. The recursion used to pass these as
 * nine separate arguments; only treestore and lm are the same for every row. */
typedef struct _tree_row_t
{
  dt_masks_form_t *form;
  int grp_id;              // 0 for a row listed at top level, the parent group's id otherwise
  dt_iop_module_t *module; // the module owning the group this row sits under, when there is one
  int gstate;              // the combine/invert bits this form carries inside its parent
  float opacity;
  int index;               // rank inside the parent, which _set_iter_name() shows
} _tree_row_t;

/* The module whose drawn mask is this group, if any. A group listed at top level with no module
 * yet is the only case worth asking about: a nested one inherits its parent's. */
static dt_iop_module_t *_module_owning_group(const dt_masks_form_t *group)
{
  for(const GList *iops = dt_dev_get_global()->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)iops->data;
    if((iop->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(iop->flags() & IOP_FLAGS_NO_MASKS)
       && iop->blend_params->mask_id == group->formid)
      return iop;
  }
  return NULL;
}

/* Appends the row and returns its iter, which a group needs to hang its members from. Shapes and
 * groups are described identically here; only what happens afterwards differs. */
static void _tree_append_row(GtkTreeStore *treestore, GtkTreeIter *toplevel, dt_shape_manager_t *lm,
                             const _tree_row_t *row, GtkTreeIter *child)
{
  GdkPixbuf *icop = NULL;
  if(row->gstate & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(row->gstate & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(row->gstate & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(row->gstate & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;

  GdkPixbuf *icinv = (row->gstate & DT_MASKS_STATE_INVERSE) ? lm->ic_inverse : NULL;

  // Only a top-level row asks who else uses the shape: a row under a group already says so.
  char used_by[1000] = "";
  int nbuse = 0;
  if(row->grp_id == 0) _is_form_used(row->form->formid, used_by, sizeof(used_by), &nbuse);

  gtk_tree_store_append(treestore, child, toplevel);
  gtk_tree_store_set(treestore, child, TREE_TEXT, row->form->name, TREE_MODULE, row->module,
                     TREE_GROUPID, row->grp_id, TREE_FORMID, row->form->formid,
                     TREE_EDITABLE, (row->grp_id == 0), TREE_IC_OP, icop,
                     TREE_IC_OP_VISIBLE, (!IS_NULL_PTR(icop)), TREE_IC_INVERSE, icinv,
                     TREE_IC_INVERSE_VISIBLE, (!IS_NULL_PTR(icinv)),
                     TREE_IC_USED_VISIBLE, (nbuse > 0), TREE_USED_TEXT, used_by,
                     TREE_IC_DELETE_VISIBLE, (row->grp_id == 0),
                     TREE_IC_UNLINK_VISIBLE, (row->grp_id != 0), -1);
  _set_iter_name(lm, row->form, row->gstate, row->opacity, GTK_TREE_MODEL(treestore), child, row->index);
}

static void _shape_manager_list_recurs(GtkTreeStore *treestore, GtkTreeIter *toplevel,
                                       dt_shape_manager_t *lm, const _tree_row_t *row)
{
  // Clone sources belong to retouch's own UI, not to this tree.
  if(row->form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) return;

  _tree_row_t self = *row;
  if((self.form->type & DT_MASKS_GROUP) && self.grp_id == 0 && IS_NULL_PTR(self.module))
    self.module = _module_owning_group(self.form);

  GtkTreeIter child;
  _tree_append_row(treestore, toplevel, lm, &self, &child);

  if(!(self.form->type & DT_MASKS_GROUP)) return;

  int index = 0;
  for(const GList *forms = self.form->points; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_group_t *grpt = (const dt_masks_form_group_t *)forms->data;
    dt_masks_form_t *member = dt_masks_get_from_id(dt_dev_get_global(), grpt->formid);
    if(!IS_NULL_PTR(member))
    {
      const _tree_row_t member_row = { .form = member, .grp_id = self.form->formid,
                                       .module = self.module, .gstate = grpt->state,
                                       .opacity = grpt->opacity, .index = index };
      _shape_manager_list_recurs(treestore, &child, lm, &member_row);
    }
    index++;
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
    _shape_manager_get_values(model, iter, &mod, NULL, &fid);
    found = (fid == formid)
      && ((level == 1)
          || (IS_NULL_PTR(module) || (mod && (!g_strcmp0(module->op, mod->op)))));
    if(found) return found;
    GtkTreeIter child;
    GtkTreeIter parent = *iter;
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

GList *_shape_manager_get_selected(dt_lib_module_t *self)
{
  GList *res = NULL;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

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
      _shape_manager_get_values(model, &iter, &mod, &gid, &fid);
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

/* Expands to the row, scrolls it into view and selects it. */
static void _tree_reveal_row(dt_shape_manager_t *lm, GtkTreeModel *model, GtkTreeIter *iter,
                             const gboolean exclusive)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GtkTreePath *path = gtk_tree_model_get_path(model, iter);

  if(exclusive) gtk_tree_selection_unselect_all(selection);
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
  gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), path, NULL, TRUE, 0.5, 0.5);
  gtk_tree_selection_select_iter(selection, iter);

  gtk_tree_path_free(path);
}

/* Returns whether it added anything, which is what tells the caller a separator is worth having. */
static gboolean _tree_store_add_forms(GtkTreeStore *treestore, dt_shape_manager_t *lm, const gboolean groups)
{
  gboolean any = FALSE;

  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(!!(form->type & DT_MASKS_GROUP) != groups) continue;

    const _tree_row_t row = { .form = form, .opacity = 1.0f };
    _shape_manager_list_recurs(treestore, NULL, lm, &row);
    any = TRUE;
  }

  return any;
}

/* The one row GTK draws as a rule rather than as content. It carries no form, so every walk that
 * looks a row up by id passes over it, and GtkTreeView skips it for selection on its own. */
static gboolean _tree_row_is_separator(GtkTreeModel *model, GtkTreeIter *iter,
                                       gpointer data __attribute__((unused)))
{
  gboolean is_separator = FALSE;
  gtk_tree_model_get(model, iter, TREE_IS_SEPARATOR, &is_separator, -1);
  return is_separator;
}

/* Groups first, then the shapes no group holds: that is the order the tree shows them in. */
static GtkTreeStore *_tree_store_build(dt_shape_manager_t *lm)
{
  // we store : text ; *module ; groupid ; formid
  GtkTreeStore *treestore = gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT,
                                               G_TYPE_INT, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN,
                                               GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                               G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  const gboolean had_groups = _tree_store_add_forms(treestore, lm, TRUE);

  /* A rule between the module groups and the loose shapes, added between the two passes and kept
   * only when both sides of it exist -- one opening or closing the list would be a line against
   * nothing.
   *
   * Its ids are -1, not 0: _shape_manager_selection_change_r() is asked for id 0 whenever no mask
   * is current, and a row carrying 0 would answer. Nothing ever looks for -1. */
  GtkTreeIter separator;
  if(had_groups)
  {
    gtk_tree_store_append(treestore, &separator, NULL);
    gtk_tree_store_set(treestore, &separator, TREE_IS_SEPARATOR, TRUE, TREE_FORMID, -1,
                       TREE_GROUPID, -1, TREE_EDITABLE, FALSE, -1);
  }

  const gboolean had_shapes = _tree_store_add_forms(treestore, lm, FALSE);
  if(had_groups && !had_shapes) gtk_tree_store_remove(treestore, &separator);

  return treestore;
}

/* Puts back what was selected before the store was replaced. selectids holds three entries per
 * row -- module, group id, form id -- as _shape_manager_get_selected() built it. */
static void _tree_restore_selection(dt_shape_manager_t *lm, GtkTreeModel *model, const GList *selectids)
{
  const GList *ids = selectids;
  while(ids)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)ids->data;
    ids = g_list_next(ids);
    // the group id sits between the module and the form id, and this walk has no use for it
    ids = g_list_next(ids);
    const int fid = GPOINTER_TO_INT(ids->data);
    ids = g_list_next(ids);

    GtkTreeIter iter;
    // An empty store leaves iter untouched, and _find_mask_iter_by_values() then walks a
    // stack-garbage iterator: gtk_tree_store_get_value() and gtk_tree_store_iter_next() assert
    // on it, and the walk has no reason to terminate. Nothing later can make the store
    // non-empty, so stop rather than skip.
    if(!gtk_tree_model_get_iter_first(model, &iter)) return;

    if(_find_mask_iter_by_values(model, &iter, mod, fid, 1)) _tree_reveal_row(lm, model, &iter, FALSE);
  }
}

/* Points the tree at the focused module's mask group, and says whether it moved the selection --
 * the caller replays the selection handler when it did, so the canvas follows. */
static gboolean _tree_select_module_group(dt_shape_manager_t *lm, GtkTreeModel *model,
                                          const dt_masks_form_gui_t *gui)
{
  dt_iop_module_t *const module = dt_dev_get_global()->gui_module;
  const int group_id = (!IS_NULL_PTR(module) && module->blend_params
                        && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
                        && !(module->flags() & IOP_FLAGS_NO_MASKS))
                           ? module->blend_params->mask_id
                           : 0;

  if(group_id <= 0) return FALSE;
  // Mid-creation the tree follows the shape being drawn, not the module.
  if(!IS_NULL_PTR(gui) && gui->creation) return FALSE;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
  if(!_find_mask_iter_by_values(model, &iter, module, group_id, 1)) return FALSE;

  _tree_reveal_row(lm, model, &iter, TRUE);
  return TRUE;
}

static void _shape_manager_recreate_list(dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm) || lm->gui_reset) return;

  // Everything below drives the tree itself, so the handlers it would wake must stay quiet.
  const int gui_reset = lm->gui_reset;
  lm->gui_reset = 1;

  // The tree is about to be replaced, so what is selected has to be read before it goes.
  GList *selectids = lm->treeview ? _shape_manager_get_selected(self) : NULL;

  // Rebuilding the list also refreshes shapes created during continuous creation. In that case
  // the active creation button must stay active until the user cancels creation explicitly.
  dt_masks_form_gui_t *gui = dt_dev_get_global()->form_gui;
  if(IS_NULL_PTR(gui) || !gui->creation) dt_masks_shape_buttons_deactivate_all(NULL);

  GtkTreeStore *treestore = _tree_store_build(lm);
  GtkTreeModel *model = GTK_TREE_MODEL(treestore);
  gtk_tree_view_set_model(GTK_TREE_VIEW(lm->treeview), model);

  if(selectids)
  {
    _tree_restore_selection(lm, model, selectids);
    g_list_free(selectids);
  }

  const gboolean sync_center_view = _tree_select_module_group(lm, model, gui);

  g_object_unref(treestore);
  lm->gui_reset = gui_reset;

  if(sync_center_view)
    _tree_selection_change(gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview)), lm);
}

static void _shape_manager_update_item(dt_lib_module_t *self __attribute__((unused)), int formid, int parentid, dt_shape_manager_t *lm, GtkTreeModel *model, GtkTreeIter *iter)
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

static gboolean _update_foreach(GtkTreeModel *model, GtkTreePath *path __attribute__((unused)), GtkTreeIter *iter, gpointer data)
{
  if(IS_NULL_PTR(iter)) return 0;

  // we retrieve the ids
  int grid = -1;
  int id = -1;
  _shape_manager_get_values(model, iter, NULL, &grid, &id);

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
static void _shape_manager_update_list(dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
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
  _shape_manager_get_values(model, iter, NULL, &grid, &id);

  if(grid == refgid && id == refid)
  {
    GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
    *rl = g_list_append(*rl, rowref);
  }
  return 0;
}

static void _shape_manager_remove_item(dt_lib_module_t *self, int formid, int parentid)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
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

static gboolean _shape_manager_selection_change_r(GtkTreeModel *model, GtkTreeSelection *selection,
                                              GtkTreeIter *iter, struct dt_iop_module_t *module,
                                              const int selectid, int throw_event, const int level)
{
  gboolean found = FALSE;

  // The walk stops at the first match, whether this level made it or a child did, so the loop
  // carries that in its own condition rather than breaking out of it twice.
  GtkTreeIter i = *iter;
  do
  {
    int id = -1;
    dt_iop_module_t *mod;
    _shape_manager_get_values(model, &i, &mod, NULL, &id);

    if((id == selectid)
       && ((level == 1)
           || (IS_NULL_PTR(module) || (mod && (!g_strcmp0(module->op, mod->op))))))
    {
      gtk_tree_selection_select_iter(selection, &i);
      found = TRUE;
      continue;
    }

    // check for children if any
    GtkTreeIter child;
    GtkTreeIter parent = i;
    if(gtk_tree_model_iter_children(model, &child, &parent))
      found = _shape_manager_selection_change_r(model, selection, &child, module, selectid, throw_event, level + 1);
  } while(!found && gtk_tree_model_iter_next(model, &i) == TRUE);

  return found;
}

static void _shape_manager_selection_change(dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
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
    const gboolean found = _shape_manager_selection_change_r(model, selection, &iter, module, selectid, throw_event, 1);
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

static void _shape_manager_handler_callback(gpointer instance __attribute__((unused)), const int formid, const int parentid, const dt_masks_event_t event, dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self)) return;

  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
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
        _shape_manager_update_item(self, formid, parentid, lm, model, &iter);
      }
      break;

      case DT_MASKS_EVENT_CHANGE :
      {
        _shape_manager_recreate_list(self);
      }
      break;

      case DT_MASKS_EVENT_DELETE :
      {
        _shape_manager_recreate_list(self);
      }
      break;

      case DT_MASKS_EVENT_REMOVE :
      {
        _shape_manager_recreate_list(self);
      }
      break;

      case DT_MASKS_EVENT_NONE :
      default:
      {
        dt_print(DT_DEBUG_MASKS, "[_shape_manager_handler_callback] Mask event cannot be found.");
      }
      break;
    }
  }
  
  else if(event == DT_MASKS_EVENT_RESET)
  {
    _shape_manager_recreate_list(self);
  }

  else if(event == DT_MASKS_EVENT_DELETE || event == DT_MASKS_EVENT_REMOVE)
  {
    // When a shape is deleted from the model, we may no longer find its previous row in the current tree.
    // In that case, force a full list refresh so stale rows don't remain visible.
    _shape_manager_recreate_list(self);
  }

  else if(event == DT_MASKS_EVENT_ADD)
  {
    _shape_manager_recreate_list(self);
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
static gboolean _shape_manager_popup_position_is_usable(GtkWidget *window)
{
#ifdef GDK_WINDOWING_WAYLAND
  return !GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(window));
#else
  return TRUE;
#endif
}

/** @brief Remember where the user put the panel and how wide they made it. Called on every path
 * that takes the window off screen, since a hidden window no longer has a position to read. */
static void _shape_manager_popup_save_geometry(dt_shape_manager_t *d)
{
  if(!GTK_IS_WINDOW(d->popup_window) || !gtk_widget_get_visible(d->popup_window)) return;

  gint width = 0;
  gint height = 0;
  gtk_window_get_size(GTK_WINDOW(d->popup_window), &width, &height);
  if(width > 0) dt_conf_set_int(DT_MASKS_PANEL_CONF_WIDTH, width);

  if(!_shape_manager_popup_position_is_usable(d->popup_window)) return;

  gint x = 0;
  gint y = 0;
  gtk_window_get_position(GTK_WINDOW(d->popup_window), &x, &y);
  dt_conf_set_int(DT_MASKS_PANEL_CONF_X, x);
  dt_conf_set_int(DT_MASKS_PANEL_CONF_Y, y);
}

/** @brief Put the panel back where it was left, before it is mapped. With nothing stored -- first
 * run, or a session that never moved it -- nothing is imposed and GTK_WIN_POS_CENTER_ON_PARENT
 * still decides, which is what puts the window on the screen the application is on. */
static void _shape_manager_popup_restore_geometry(dt_shape_manager_t *d)
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

  if(!_shape_manager_popup_position_is_usable(d->popup_window)) return;
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

/** @brief The toolbox button is the panel's only state: showing and hiding both go through its
 * active flag, so every way of closing the panel leaves the button un-pressed. Re-entrant by
 * design -- the window-manager close path toggles the button, which comes back here. */
static void _shape_manager_popup_button_toggled_cb(GtkWidget *button, gpointer user_data)
{
  dt_shape_manager_t *d = (dt_shape_manager_t *)user_data;
  if(IS_NULL_PTR(d->popup_window)) return;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  if(active == gtk_widget_get_visible(d->popup_window)) return;

  if(active)
  {
    // before mapping: a move applied to a mapped window makes it jump in view
    _shape_manager_popup_restore_geometry(d);
    gtk_widget_show_all(d->popup_window);
  }
  else
  {
    _shape_manager_popup_save_geometry(d);
    gtk_widget_hide(d->popup_window);
  }
}

/** @brief Closing from the window manager hides the panel, same as the toolbox button, so its
 * widgets and state survive. Un-pressing the button is what actually hides the window (and saves
 * the geometry before it goes), so the two ways of closing cannot disagree. */
static gboolean _shape_manager_popup_delete_cb(GtkWidget *window __attribute__((unused)),
                                           GdkEvent *event __attribute__((unused)), gpointer user_data)
{
  dt_shape_manager_t *d = (dt_shape_manager_t *)user_data;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->popup_button), FALSE);
  return TRUE;
}

/* Idle callback to add the popup button to the module toolbox once the
 * module_toolbox proxy has been initialized. Returns FALSE when done so
 * it is removed from the idle loop. */
/* Published through dev->proxy.masks so the darkroom can draw the mask overlays while the user
 * is looking at the manager -- what dt_lib_gui_get_expanded() answered while this was a panel
 * section, and what no expander can answer now that it is a window. */
static gboolean _shape_manager_is_window_visible(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return FALSE;
  dt_shape_manager_t *d = (dt_shape_manager_t *)self->data;
  return !IS_NULL_PTR(d->popup_window) && gtk_widget_get_visible(d->popup_window);
}

static gboolean _shape_manager_add_popup_button_idle(gpointer user_data)
{
  dt_shape_manager_t *d = (dt_shape_manager_t *)user_data;
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
  dt_shape_manager_t *d = (dt_shape_manager_t *)g_malloc0(sizeof(dt_shape_manager_t));
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
  gtk_window_set_title(GTK_WINDOW(d->popup_window), _("Shape Manager"));
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
  g_signal_connect(G_OBJECT(d->popup_window), "delete-event", G_CALLBACK(_shape_manager_popup_delete_cb), d);

  // 3. Create a clean box container inside the popup window to receive original shape elements
  GtkWidget *shape_manager_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  // The window is its own frame, so nothing would otherwise separate the content from its edges.
  // DT_GUI_BOX_SPACING is the 0.625em the theme gives a module body (.dt_plugin_ui_main), so the
  // panel breathes the same way a side-panel module does, at any font size.
  gtk_container_set_border_width(GTK_CONTAINER(shape_manager_container), DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(d->popup_window), shape_manager_container);

  // No panel body: this module's interface is the window built below, so self->widget stays
  // NULL and the module is never packed -- dt_lib_is_visible_in_view() already excludes a
  // "special" view from every panel.

  // Create and pack the button to control the popup panel.
  // NOTE: it's added to the darkroom module toolbox, aka not here.
  d->popup_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_drawn, 0, NULL);
  gtk_widget_set_tooltip_text(d->popup_button, _("Open shape manager..."));

  /* module_toolbox may not be initialized yet when modules are being created.
   * Schedule adding the popup button via an idle callback so it runs after
   * other modules (including the module_toolbox) have had their gui_init
   * called. The callback will remove itself once it succeeds. */
  g_idle_add((GSourceFunc)_shape_manager_add_popup_button_idle, d);
  g_signal_connect(G_OBJECT(d->popup_button), "toggled", G_CALLBACK(_shape_manager_popup_button_toggled_cb), d);

  // From here, everything goes into the mask manager popup,
  // so there is no child added to self->widget from here.
  const dt_masks_shape_buttons_config_t shape_buttons_config = {
    .dev = dt_dev_get_global(),
    .owner_module = NULL,
    .creation_module = NULL,
    .buttons = NULL,   // nothing here reads the individual buttons back
    .types = NULL,
    .action_section = NULL,
    .flags = DT_MASKS_SHAPE_BUTTONS_ALL,
    .register_flags = DT_MASKS_SHAPE_BUTTONS_NONE,
    .local = FALSE,
    .user_data = NULL,
    .can_start = NULL,
    .form_type = NULL,
    .started = _shape_manager_shape_button_started,
    .exited = NULL,
  };
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *shape_buttons_box = dt_masks_shape_buttons_create(&shape_buttons_config);
  gtk_box_pack_start(GTK_BOX(hbox), shape_buttons_box, FALSE, FALSE, 0);
  // The button row keeps its natural height and stays at the top: expanding it would split the
  // surplus with the shape list below and stretch the buttons vertically.
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
  /* Icon marking a shape shared by several modules. It is a remark about the shape rather than
   * something to click, so it is drawn in the theme's own disabled grey -- a flat grey, next to
   * the action icon it sits beside, rather than the washed-out foreground a GtkCellRenderer's
   * insensitive state produces. A cell renderer has no colour of its own, so the icon is loaded
   * once as a pre-tinted pixbuf; the model still only carries whether this row shows one. */
  GdkRGBA used_color;
  if(!gtk_style_context_lookup_color(gtk_widget_get_style_context(d->treeview), "disabled_fg_color",
                                     &used_color))
    used_color = (GdkRGBA){ 0.62, 0.62, 0.62, 1.0 };

  d->ic_used = dt_gui_symbolic_icon_pixbuf("mail-attachment-symbolic", GTK_ICON_SIZE_MENU, &used_color, NULL);

  renderer = gtk_cell_renderer_pixbuf_new();
  // A theme with no symbolic variant of that icon leaves the pixbuf NULL: name the icon instead
  // and let GTK draw it, untinted, rather than show nothing.
  if(IS_NULL_PTR(d->ic_used))
    g_object_set(renderer, "icon-name", "mail-attachment-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
  else
    g_object_set(renderer, "pixbuf", d->ic_used, NULL);
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

  /* The per-row action icon, to the right of everything the name column carries -- the "used by"
   * icon included, since that one is packed at that column's end. Both renderers live in this one
   * column and exactly one of them is visible on any row, so the icon lands in the same place
   * whichever action the row offers. The name column expands to take up the slack, which is what
   * keeps the action flush right. Clicks are answered in _tree_button_pressed() by comparing the
   * column, the way develop/blend_gui.c does for the same two icons. */
  gtk_tree_view_column_set_expand(col, TRUE);

  d->action_col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_sizing(d->action_col, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(d->action_col, DT_PIXEL_APPLY_DPI(24));

  renderer = gtk_cell_renderer_pixbuf_new();
  g_object_set(renderer, "icon-name", "list-remove-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
  gtk_tree_view_column_pack_start(d->action_col, renderer, FALSE);
  gtk_tree_view_column_add_attribute(d->action_col, renderer, "visible", TREE_IC_UNLINK_VISIBLE);

  renderer = gtk_cell_renderer_pixbuf_new();
  g_object_set(renderer, "icon-name", "user-trash-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
  gtk_tree_view_column_pack_start(d->action_col, renderer, FALSE);
  gtk_tree_view_column_add_attribute(d->action_col, renderer, "visible", TREE_IC_DELETE_VISIBLE);

  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), d->action_col);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
  gtk_tree_view_set_row_separator_func(GTK_TREE_VIEW(d->treeview), _tree_row_is_separator, NULL, NULL);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
  // A query-tooltip handler rather than a tooltip column: only the rows that carry a "used by"
  // text show one, which a column would not let us decide per row.
  g_object_set(d->treeview, "has-tooltip", TRUE, (gchar *)0);
  g_signal_connect(d->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), d);
  g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), d);
  g_signal_connect(d->treeview, "button-press-event", (GCallback)_tree_button_pressed, self);

  // Auto-grows to its content (the side panel scrolls) up to a user-set, persisted height.
  gtk_box_pack_start(GTK_BOX(shape_manager_container),
                     dt_ui_scroll_wrap(d->treeview, 90, "plugins/darkroom/masks/windowheight",
                                       DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, G_CALLBACK(_shape_manager_handler_callback), self);

  // set proxy functions
  dt_dev_get_global()->proxy.masks.module = self;
  dt_dev_get_global()->proxy.masks.list_change = _shape_manager_recreate_list;
  dt_dev_get_global()->proxy.masks.list_update = _shape_manager_update_list;
  dt_dev_get_global()->proxy.masks.list_remove = _shape_manager_remove_item;
  dt_dev_get_global()->proxy.masks.selection_change = _shape_manager_selection_change;
  dt_dev_get_global()->proxy.masks.is_visible = _shape_manager_is_window_visible;
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  if(self && self->data)
  {
    dt_shape_manager_t *d = (dt_shape_manager_t *)self->data;

    // Destroy window allocation to prevent leaks
    if(d->popup_window)
    {
      // leaving with the panel open still counts as where the user left it
      _shape_manager_popup_save_geometry(d);
      gtk_widget_destroy(d->popup_window);
      d->popup_window = NULL;
    }

    if(!IS_NULL_PTR(d->ic_used)) g_object_unref(d->ic_used);
    if(!IS_NULL_PTR(d->ic_inverse)) g_object_unref(d->ic_inverse);
    if(!IS_NULL_PTR(d->ic_union)) g_object_unref(d->ic_union);
    if(!IS_NULL_PTR(d->ic_intersection)) g_object_unref(d->ic_intersection);
    if(!IS_NULL_PTR(d->ic_difference)) g_object_unref(d->ic_difference);
    if(!IS_NULL_PTR(d->ic_exclusion)) g_object_unref(d->ic_exclusion);

    d->ic_used = NULL;
    d->ic_inverse = NULL;
    d->ic_union = NULL;
    d->ic_intersection = NULL;
    d->ic_difference = NULL;
    d->ic_exclusion = NULL;
  }

  dt_free(self->data);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_shape_manager_handler_callback), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
