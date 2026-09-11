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
#include "develop/masks_group.h"   // dt_masks_group_set_member_operation(), dt_masks_group_get_member()
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
#include "widgets/widget_settings.h"  // dt_widget_root_window(), dt_widget_store_int()
#include "common/conf.h"          // dt_conf_get_int(), dt_conf_key_exists()

#include "widgets/label.h"   // dt_gui_symbolic_icon_pixbuf()
#include "widgets/dialog.h"        // dt_gui_refocus_parent()
#include "widgets/togglebutton.h"
#include "control/signal.h"
#include "control/user_message.h"   // dt_control_log()

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>   // conditional-ok: GDK_IS_WAYLAND_DISPLAY() is used only inside the same #ifdef
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


DT_MODULE(1)

static void _shape_manager_recreate_list(dt_lib_module_t *self);
static void _shape_manager_update_list(dt_lib_module_t *self);
static GList *_modules_owning_group(const dt_masks_form_t *group);
static void _shape_manager_broadcast(dt_lib_module_t *self, const int formid, const int parentid,
                                     const dt_masks_event_t event);

/* The panel splits the forms by the one question that separates them: is this group some
 * module's drawn mask, or nobody's yet? The left list is the inventory -- every shape and every
 * group, module masks included, so any of them can be picked up and reused -- and the right one
 * is the assignment: only the groups modules actually render, members underneath. A module mask
 * therefore appears in both, once in the inventory as an ordinary reusable group and once in the
 * assignment under the module rendering it, the same way a shape a module uses appears in both
 * its own row and as a member -- the arrangement a module's own Drawn tab already uses. */
typedef enum dt_shape_list_t
{
  DT_SHAPE_LIST_SHAPES = 0,   // every shape and every group, module masks included
  DT_SHAPE_LIST_MODULES,      // only the groups that are some module's drawn mask
  DT_SHAPE_LIST_COUNT
} dt_shape_list_t;

/* One of the two lists. Every tree handler is handed this rather than the module, because the
 * first thing each of them needs to know is which of the two trees the user acted on. */
typedef struct dt_shape_manager_list_t
{
  GtkWidget *treeview;

  /* The rightmost column, the one carrying the per-row trash / minus icon. Kept because a click
   * and a tooltip are both answered by comparing against the column the pointer is over. */
  GtkTreeViewColumn *action_col;

  /* The "add to the selected module group" column, on the inventory list only -- NULL on the
   * module list. Kept for the same reason as action_col: a click and a tooltip are both answered
   * by comparing against the column the pointer is over. */
  GtkTreeViewColumn *add_col;

  /* The "pick the modules that should render this" column, inventory only. */
  GtkTreeViewColumn *assign_col;

  /* The name column and the renderer inside it, kept so a row can be opened straight into
   * editing -- gtk_tree_view_set_cursor_on_cell() needs both. */
  GtkTreeViewColumn *name_col;
  GtkCellRenderer *name_renderer;

  dt_shape_list_t which;
  dt_lib_module_t *self;   // the module both lists belong to

  /* The conf key dt_ui_scroll_wrap() persists this list's dragged height under. Kept so the
   * popup can raise the ceiling on it at show time, independently of whatever gui_init built the
   * wrapper with -- see _shape_manager_relax_height_caps(). */
  const char *height_key;
} dt_shape_manager_list_t;

typedef struct dt_shape_manager_t
{
  dt_shape_manager_list_t lists[DT_SHAPE_LIST_COUNT];

  /* Replacement for shape_manager_expander */
  GtkWidget *popup_window;
  GtkWidget *popup_button;

  GdkPixbuf *ic_used;
  /* The "+" in its two states. A cell renderer has no colour of its own and its insensitive
   * rendering is far too faint to read as disabled (measured: at most 49 of 255 on a channel),
   * so availability is shown by swapping the icon for a differently tinted one. */
  GdkPixbuf *ic_add;
  GdkPixbuf *ic_add_off;
  GdkPixbuf *ic_assign;
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
  /* What the row has to say about itself beyond its name -- currently only that the same shape
   * is reached twice within one module's mask. Empty on every row that has nothing to add. */
  TREE_NOTE,
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

/* Refresh the masks header of every module whose drawn mask IS @p grp, after its members changed:
 * those are the panels whose "N shapes used" moved. Not the module a tree row stores: a nested
 * group's row carries the module of the mask it sits in, whose own count does not change, and a
 * group no module renders yet carries none at all. Every module's member list is rebuilt by the
 * DT_SIGNAL_MASK_CHANGED the caller raises. */
static void _refresh_owning_modules(const dt_masks_form_t *grp)
{
  GList *owners = _modules_owning_group(grp);
  for(const GList *m = owners; m; m = g_list_next(m))
    dt_iop_gui_blend_masks_update((dt_iop_module_t *)m->data);
  g_list_free(owners);
}

static void _tree_add_exist(GtkButton *button, dt_masks_form_t *grp)
{
  dt_develop_t *const dev = dt_dev_get_global();
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return;
  // we get the new formid
  const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));

  // we add the form in this group
  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  grp = dt_masks_cow_touch(dev, grp);
  if(form && dt_masks_group_add_form(dev, grp, form))
  {
    // we save the group
    dt_dev_add_history_item(dev, NULL, FALSE, TRUE);

    _refresh_owning_modules(grp);
    dt_dev_masks_selection_change(dev, NULL, grp->formid, TRUE);

  /* Raised rather than broadcast: unlike the handlers above, this one does not rebuild the tree
   * itself, so our own handler is left to do it as well as blend_gui's. */
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, 0, 0,
                                  DT_MASKS_EVENT_CHANGE);
  }
}

/* The form ids the selection of @p list names, each once, in selection order. The same form can
 * sit on several rows -- a shape at top level and under each group holding it -- and an action on
 * the selection must act on it once.
 *
 * Returns a GList of GINT_TO_POINTER(formid) the caller frees with g_list_free(). */
static GList *_selected_form_ids(dt_shape_manager_list_t *list)
{
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));

  GList *ids = NULL;
  GList *rows = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *row = rows; row; row = g_list_next(row))
  {
    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)row->data)) continue;

    int id = -1;
    _shape_manager_get_values(model, &iter, NULL, NULL, &id);
    if(id > 0 && IS_NULL_PTR(g_list_find(ids, GINT_TO_POINTER(id)))) ids = g_list_prepend(ids, GINT_TO_POINTER(id));
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

  return g_list_reverse(ids);
}

/* "New group from selection": a new group holding every selected form, one or several. */
static void _tree_group(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;

  // Nothing to put in it, no group: an empty one would only be a row left to delete.
  GList *ids = _selected_form_ids(list);
  if(IS_NULL_PTR(ids)) return;

  // we create the new group
  // create_ext registers the group in dev->allforms and dt_masks_append_form() below takes
  // dev->forms's own reference, so both lists have a claim and teardown balances. Neither
  // touches dev->forms outside masks_mutex, which a hand-rolled g_list_append does -- and the
  // pipeline thread reads that list under the same lock.
  dt_masks_form_t *mask = dt_masks_create_ext(dt_dev_get_global(), DT_MASKS_GROUP);
  g_snprintf(mask->name, sizeof(mask->name), _("Mask #%d"), g_list_length(dt_dev_get_global()->forms));

  // we add all selected forms to this group
  for(const GList *id = ids; id; id = g_list_next(id))
  {
    dt_masks_form_t *member = dt_masks_get_from_id(dt_dev_get_global(), GPOINTER_TO_INT(id->data));
    if(IS_NULL_PTR(member)) continue;

    dt_masks_group_add_form_with_state(dt_dev_get_global(), mask, member, mask->formid,
                                       DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION, 1.0f);
  }
  g_list_free(ids);

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
      // The caller's own module is not worth naming to it: it is the one asking.
      if(pt->formid != form->formid || m == module) continue;

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

static void _tree_delete_unused(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
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
static void _tree_apply_operation(GtkWidget *menu_item, dt_shape_manager_list_t *list)
{
  const dt_masks_state_t operation
      = (dt_masks_state_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "masks-operation"));
  if(operation == DT_MASKS_STATE_NONE) return;

  dt_lib_module_t *self = list->self;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
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

static void _tree_moveup(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
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

static void _tree_movedown(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
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

static void _tree_delete_shape(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(dt_dev_get_global(), NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
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

/* The group the module list currently points at, which is what the inventory's "+" adds to.
 *
 * A row that is itself a group answers for itself; any other row hands the question up to its
 * parent, so clicking "+" after selecting a shape inside a module's mask adds to that mask
 * rather than doing nothing. Returns 0 when nothing usable is selected, which is also what
 * greys the button out. */
/* The top-level ancestor of a row -- itself, if it has none. The module list holds one row per
 * module and that row's whole subtree, so nothing below the top level is its own destination:
 * selecting a sub-group or a shape nested inside a module's mask still means "this mask". */
static int _tree_root_formid(GtkTreeModel *model, GtkTreeIter iter)
{
  GtkTreeIter parent;
  while(gtk_tree_model_iter_parent(model, &parent, &iter))
    iter = parent;

  int formid = 0;
  gtk_tree_model_get(model, &iter, TREE_FORMID, &formid, -1);
  return formid;
}

static int _selected_group_in_module_list(const dt_shape_manager_t *lm)
{
  const dt_shape_manager_list_t *list = &lm->lists[DT_SHAPE_LIST_MODULES];
  if(IS_NULL_PTR(list->treeview)) return 0;

  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
  GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
  if(IS_NULL_PTR(rows)) return 0;

  int group_id = 0;
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)rows->data))
    group_id = _tree_root_formid(model, iter);

  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  return group_id;
}

/* Whether the form @p fid can join the group @p group_id. Three ways it cannot: the group already
 * reaches this form, directly or through one of its own sub-groups, so adding it again would write
 * an undo step for a no-op; the form is a group that already contains the target, and adding it
 * would close a cycle in the membership graph -- every walk over it, the tree build first of all,
 * would then stop terminating; or the form is a group every one of whose shapes the target already
 * renders, adding nothing new.
 *
 * A row's "+", the "Attach to the group" menu and its action all ask this one function, so an offer
 * that looks available always is. */
static gboolean _form_can_join_group(dt_develop_t *dev, const int group_id, const int fid)
{
  if(group_id <= 0 || fid <= 0) return FALSE;

  const dt_masks_form_t *grp = dt_masks_get_from_id(dev, group_id);
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return FALSE;

  /* Subsumes "it is the same group" (trivial at depth 0) and "it is already a direct member": a
   * mask that already reaches this form ANYWHERE in its own subtree gains nothing from reaching
   * it again at the top. */
  if(dt_masks_group_contains(dev, group_id, fid) == DT_MASKS_OK) return FALSE;
  if(dt_masks_group_contains(dev, fid, group_id) == DT_MASKS_OK) return FALSE;

  /* A group every one of whose shapes the target already renders would add nothing: nesting it
   * duplicates coverage the mask already has. */
  const dt_masks_form_t *form = dt_masks_get_from_id(dev, fid);
  if(!IS_NULL_PTR(form) && (form->type & DT_MASKS_GROUP))
  {
    gboolean has_shapes = FALSE;
    if(dt_masks_group_covers_shapes(dev, fid, group_id, &has_shapes) == DT_MASKS_OK && has_shapes)
      return FALSE;
  }

  return TRUE;
}

/* Whether this row's "+" can do anything: not when nothing is selected in the module list, so
 * there is nowhere to add to, and otherwise whatever _form_can_join_group() answers for the target
 * mask -- always resolved to its top level, never to whatever sub-group or shape happens to be
 * selected inside it.
 *
 * The icon and the click both ask this one function, so a button that looks available always is. */
static gboolean _row_can_be_added(const dt_shape_manager_t *lm, GtkTreeModel *model, GtkTreeIter *iter)
{
  int fid = -1;
  _shape_manager_get_values(model, iter, NULL, NULL, &fid);
  return _form_can_join_group(dt_dev_get_global(), _selected_group_in_module_list(lm), fid);
}

/* "Attach to the group": every selected form that can join the group named on the menu item joins
 * it, the others are left alone. Asked again form by form rather than once when the menu was
 * built: a group joining first may bring later selected shapes along with it. */
static void _tree_add_to_group(GtkMenuItem *item, dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
  dt_develop_t *const dev = dt_dev_get_global();

  const int group_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "group-formid"));
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, group_id);
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return;

  GList *ids = _selected_form_ids(list);
  gboolean added = FALSE;
  for(const GList *id = ids; id; id = g_list_next(id))
  {
    const int fid = GPOINTER_TO_INT(id->data);
    dt_masks_form_t *form = dt_masks_get_from_id(dev, fid);
    if(IS_NULL_PTR(form) || !_form_can_join_group(dev, group_id, fid)) continue;

    // Touched before the first change only: a group nothing joins must not be cloned for nothing.
    if(!added) grp = dt_masks_cow_touch(dev, grp);
    if(!IS_NULL_PTR(dt_masks_group_add_form(dev, grp, form))) added = TRUE;
  }
  g_list_free(ids);
  if(!added) return;

  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);
  _refresh_owning_modules(grp);
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);
}

/* Availability is per row and depends on the OTHER list's selection, so it is recomputed at draw
 * time rather than stored: nothing can go stale, and there is no model column to keep in step.
 *
 * The renderer's sensitivity carries the meaning but not the look -- measured on this exact
 * renderer, offscreen, an insensitive icon-name pixbuf cell differs from a sensitive one by at
 * most 49 of 255 on a channel, a faint dim rather than a grey-out -- so the icon is swapped for
 * a differently tinted one, the same answer the "used by" icon got.
 *
 * A cell data func replaces the column's attribute mapping rather than adding to it, so this
 * sets "visible" too instead of leaving it to gtk_tree_view_column_add_attribute(). */
static void _add_cell_data_func(GtkTreeViewColumn *col __attribute__((unused)), GtkCellRenderer *renderer,
                                GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  const dt_shape_manager_t *lm = (const dt_shape_manager_t *)data;

  gboolean on_row = FALSE;
  gtk_tree_model_get(model, iter, TREE_IC_DELETE_VISIBLE, &on_row, -1);
  g_object_set(renderer, "visible", on_row, NULL);
  if(!on_row) return;

  const gboolean available = _row_can_be_added(lm, model, iter);
  gtk_cell_renderer_set_sensitive(renderer, available);

  GdkPixbuf *icon = available ? lm->ic_add : lm->ic_add_off;
  if(!IS_NULL_PTR(icon)) g_object_set(renderer, "pixbuf", icon, NULL);
}

/* The module list's selection decides every inventory row's "+", so a change there redraws the
 * inventory and the data func above answers again. */
static void _shape_manager_sync_add_sensitivity(const dt_shape_manager_t *lm)
{
  const dt_shape_manager_list_t *list = &lm->lists[DT_SHAPE_LIST_SHAPES];
  if(IS_NULL_PTR(list->add_col) || IS_NULL_PTR(list->treeview)) return;
  gtk_widget_queue_draw(list->treeview);
}

/* The panel never takes the keyboard focus on its own (see gui_init: its drawing tools act on
 * the main window), so while a name is being edited every keystroke would still go to the main
 * window, where dt_accels_dispatch() and the view read it as a shortcut -- Escape leaves the
 * darkroom for the lighttable, a letter fires whatever it is bound to -- and the editor would get
 * none of it. For the life of the editor the panel accepts the focus and asks for it: the entry
 * then receives every key and consumes Escape itself (it cancels the edit and does nothing else),
 * while both main-window handlers stand down, since they act only when that window is active. */
static void _tree_name_editing_done(GtkCellEditable *editable __attribute__((unused)), dt_shape_manager_t *lm)
{
  if(IS_NULL_PTR(lm->popup_window)) return;

  const gboolean had_focus = gtk_window_is_active(GTK_WINDOW(lm->popup_window));
  gtk_window_set_accept_focus(GTK_WINDOW(lm->popup_window), FALSE);
  // Ended by a key (Enter, Escape), the panel still holds the focus and hands it back. Ended by a
  // click in another window, the focus is already where that click put it.
  if(had_focus) dt_gui_refocus_parent(GTK_WINDOW(dt_gui_main_window()));
}

static void _tree_name_editing_started(GtkCellRenderer *renderer __attribute__((unused)),
                                       GtkCellEditable *editable, const gchar *path __attribute__((unused)),
                                       dt_shape_manager_list_t *list)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)list->self->data;
  if(IS_NULL_PTR(lm->popup_window)) return;

  // "editing-done" is emitted however the edit ends -- Enter, Escape, or the entry losing the
  // focus -- and the connection goes with the entry, which the tree destroys right after.
  g_signal_connect(editable, "editing-done", G_CALLBACK(_tree_name_editing_done), lm);
  gtk_window_set_accept_focus(GTK_WINDOW(lm->popup_window), TRUE);
  gtk_window_present(GTK_WINDOW(lm->popup_window));
}

/* Opens a row's name for editing, when the row may be renamed at all (TREE_EDITABLE).
 *
 * The renderer's "editable" property is deliberately not bound to TREE_EDITABLE. Whenever a cell
 * is editable, GtkTreeView opens the editor on a single click on a row that is already selected --
 * and that same click is how a drag, a Ctrl+click or a right-click on a selected row starts, so
 * each of them would open the editor by accident. The property therefore stays FALSE and is set
 * TRUE only while this function opens the editor. Once open, the editor does not read it again:
 * "edited" is still emitted when the name is validated. */
static void _tree_start_name_editing(const dt_shape_manager_list_t *list, GtkTreeModel *model,
                                     GtkTreeIter *iter, GtkTreePath *path)
{
  if(IS_NULL_PTR(list->treeview) || IS_NULL_PTR(list->name_col) || IS_NULL_PTR(list->name_renderer))
    return;

  gboolean editable = FALSE;
  gtk_tree_model_get(model, iter, TREE_EDITABLE, &editable, -1);
  if(!editable) return;

  g_object_set(list->name_renderer, "editable", TRUE, NULL);
  gtk_tree_view_set_cursor_on_cell(GTK_TREE_VIEW(list->treeview), path, list->name_col,
                                   list->name_renderer, TRUE);
  g_object_set(list->name_renderer, "editable", FALSE, NULL);
}

/* Opens the module list's row for that group straight into name editing.
 *
 * The tree has just been rebuilt, so the row is found by id rather than kept across the rebuild:
 * every iter from before it is stale. Only a top-level row is editable (TREE_EDITABLE), which a
 * module group in that list always is. */
static void _tree_edit_group_name(dt_lib_module_t *self, const int formid)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  const dt_shape_manager_list_t *list = &lm->lists[DT_SHAPE_LIST_MODULES];
  if(IS_NULL_PTR(list->treeview)) return;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_first(model, &iter)) return;

  do
  {
    int fid = -1;
    _shape_manager_get_values(model, &iter, NULL, NULL, &fid);
    if(fid != formid) continue;

    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    if(!IS_NULL_PTR(path))
    {
      _tree_start_name_editing(list, model, &iter, path);
      gtk_tree_path_free(path);
    }
    return;
  } while(gtk_tree_model_iter_next(model, &iter));
}

/* ---------------------------------------------------------------------------------------------
 * The module chooser: which modules should render a shape.
 *
 * Same set of modules as the "Pipeline" tab of the module groups panel, in the same order --
 * dev->iop walked backwards, so the list reads bottom-up the way the pipeline is applied --
 * narrowed to the ones a drawn mask means anything to. Disabled modules are in: attaching a mask
 * to a module that is off is legitimate, it applies the day the module is switched on.
 * ------------------------------------------------------------------------------------------- */

typedef enum dt_modchooser_col_t
{
  MODCHOOSER_CHECKED = 0,
  MODCHOOSER_WAS_CHECKED,   // as the dialog opened, so validation can act on the difference
  MODCHOOSER_SENSITIVE,     // FALSE for a row ticking could not act on -- the module's own mask
  MODCHOOSER_NAME,
  MODCHOOSER_NOTE,          // why an insensitive row is insensitive; empty otherwise
  MODCHOOSER_MODULE,
  MODCHOOSER_COUNT
} dt_modchooser_col_t;

static void _modchooser_toggled(GtkCellRendererToggle *cell __attribute__((unused)),
                                const gchar *path_string, GtkListStore *store)
{
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path_string)) return;

  gboolean checked = FALSE;
  gboolean sensitive = TRUE;
  gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, MODCHOOSER_CHECKED, &checked,
                     MODCHOOSER_SENSITIVE, &sensitive, -1);

  // A row ticking could not change stays where it is, whichever way it was reached.
  if(!sensitive) return;

  gtk_list_store_set(store, &iter, MODCHOOSER_CHECKED, !checked, -1);
}

/* A left click anywhere on the row toggles it, not just on the 12 pixels of the checkbox. */
static gboolean _modchooser_button_pressed(GtkWidget *treeview, const GdkEventButton *event,
                                           GtkListStore *store)
{
  if(event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY) return FALSE;

  GtkTreePath *path = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path,
                                    NULL, NULL, NULL))
    return FALSE;

  gchar *path_string = gtk_tree_path_to_string(path);
  gtk_tree_path_free(path);
  _modchooser_toggled(NULL, path_string, store);
  dt_free(path_string);

  return TRUE;
}

/* Clicking past this window dismisses it, applying nothing -- the same as Cancel, which is what
 * dismissing a window by clicking past it means everywhere else.
 *
 * Tested against the press itself rather than against the keyboard focus. Focus is too coarse a
 * signal for this: it also moves when the window manager hands the pointer over a frame edge, so
 * a focus-out handler closed the dialog on a click on its OWN border.
 *
 * The window is modal, so GTK routes every button press in the application to it; a press landing
 * on one of its own GdkWindows is inside it and is left to the widget under the pointer. */
static gboolean _modchooser_button_press(GtkWidget *dialog, const GdkEventButton *event,
                                         gpointer user_data __attribute__((unused)))
{
  if(event->type != GDK_BUTTON_PRESS) return FALSE;

  const GdkWindow *const toplevel = gtk_widget_get_window(dialog);
  for(GdkWindow *w = event->window; !IS_NULL_PTR(w); w = gdk_window_get_parent(w))
    if(w == toplevel) return FALSE;

  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  return TRUE;
}

/* Reads the validated dialog back: the rows whose box the user actually moved, in the order they
 * were listed. An untouched module goes in neither list -- ticking a box and unticking it again
 * is not a change, and reporting it as one would rewrite a mask the user left alone. */
static void _modchooser_collect(GtkListStore *store, GList **to_attach, GList **to_detach)
{
  GtkTreeIter iter;
  for(gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter); valid;
      valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter))
  {
    gboolean checked = FALSE;
    gboolean was_checked = FALSE;
    dt_iop_module_t *module = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, MODCHOOSER_CHECKED, &checked,
                       MODCHOOSER_WAS_CHECKED, &was_checked, MODCHOOSER_MODULE, &module, -1);

    if(IS_NULL_PTR(module) || checked == was_checked) continue;

    GList **target = checked ? to_attach : to_detach;
    *target = g_list_prepend(*target, module);
  }

  *to_attach = g_list_reverse(*to_attach);
  *to_detach = g_list_reverse(*to_detach);
}

/* Runs the attachment manager modally and answers with what the user changed: the modules to
 * attach the shape to, and the ones to detach it from. Both lists are in the order the modules
 * were listed, borrow their modules, and are freed by the caller with g_list_free().
 *
 * A box is ticked when the shape is already a DIRECT member of that module's mask. Direct, not
 * at any depth: the box is the attachment this dialog manages, and unticking it has to be able
 * to undo exactly what ticking it did. A shape reaching a module through a nested group is that
 * group's business, not this dialog's.
 *
 * @return whether the user validated; both lists are empty when nothing changed. */
static gboolean _modchooser_run(const dt_masks_form_t *form, GList **to_attach, GList **to_detach)
{
  *to_attach = NULL;
  *to_detach = NULL;

  // One by-value description of the form: every field below is taken from it, not from a raw
  // reach into the struct.
  dt_masks_form_info_t info = { 0 };
  if(!dt_masks_form_get_info(form, &info)) return FALSE;

  dt_develop_t *const dev = dt_dev_get_global();
  GtkListStore *store = gtk_list_store_new(MODCHOOSER_COUNT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                                           G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);

  // What the dialog's title, intro and tooltip call the thing being managed.
  const char *noun = info.is_group ? _("group") : _("shape");

  gboolean any = FALSE;
  for(const GList *iops = g_list_last(dt_dev_get_global()->iop); iops; iops = g_list_previous(iops))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)iops->data;
    if(!dt_iop_module_is_in_pipeline(module) || !dt_iop_module_supports_drawn_mask(module)) continue;

    /* A module cannot render its own mask as a member of itself -- ticking this would ask
     * dt_masks_group_add_form() to nest a group inside itself, which _row_can_be_added()'s
     * equivalent, dt_masks_group_contains(), already refuses trivially at depth 0. Surfacing it
     * here instead of letting the tick silently do nothing on validation. */
    const gboolean is_self_mask = (module->blend_params->mask_id == info.formid);
    const gboolean attached
        = !is_self_mask
          && (dt_masks_group_get_member(dev, module->blend_params->mask_id, info.formid, NULL)
              == DT_MASKS_OK);

    gchar *label = dt_history_item_get_name(module);
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, MODCHOOSER_CHECKED, attached, MODCHOOSER_WAS_CHECKED, attached,
                       MODCHOOSER_SENSITIVE, !is_self_mask, MODCHOOSER_NAME, label,
                       MODCHOOSER_NOTE, is_self_mask ? _("this is the module's own mask") : "",
                       MODCHOOSER_MODULE, module, -1);
    dt_free(label);
    any = TRUE;
  }

  if(!any)
  {
    g_object_unref(store);
    dt_control_log(_("No module in the pipeline can carry a drawn mask."));
    return FALSE;
  }

  /* Parented to the main window rather than to the shape manager's own panel: that panel is a
   * UTILITY window that declines focus, which is not something to hang a modal dialog off. */
  GtkWindow *parent = GTK_WINDOW(dt_gui_main_window());
  gchar *title = g_strdup_printf(_("Modules using this %s"), noun);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(title, parent,
                                                  GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                                  _("Cancel"), GTK_RESPONSE_CANCEL,
                                                  _("Apply"), GTK_RESPONSE_ACCEPT, NULL);
  dt_free(title);   // gtk_window_set_title() (called internally) copies it
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  /* Above everything, because the window it is about -- the shape manager's own panel -- is a
   * toplevel of its own and would otherwise be free to cover it. */
  gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);

  gtk_widget_add_events(dialog, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(dialog, "button-press-event", G_CALLBACK(_modchooser_button_press), NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), DT_GUI_BOX_SPACING);
  gtk_box_set_spacing(GTK_BOX(content), DT_GUI_BOX_SPACING);

  gchar *intro = g_strdup_printf(_("Tick the modules that should use the %s '%s', untick the ones "
                                   "that should stop."), noun, info.name);
  GtkWidget *label = dt_ui_label_new(intro);
  dt_free(intro);
  gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

  GtkWidget *treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeview), FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview)), GTK_SELECTION_NONE);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_expand(col, TRUE);

  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
  g_object_set(renderer, "activatable", TRUE, NULL);
  g_signal_connect(renderer, "toggled", G_CALLBACK(_modchooser_toggled), store);
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_add_attribute(col, renderer, "active", MODCHOOSER_CHECKED);
  gtk_tree_view_column_add_attribute(col, renderer, "sensitive", MODCHOOSER_SENSITIVE);

  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", MODCHOOSER_NAME);
  gtk_tree_view_column_add_attribute(col, renderer, "sensitive", MODCHOOSER_SENSITIVE);

  /* The reason a row is dead, said on the row itself: italic and against the right edge, so it
   * reads as an annotation rather than part of the module's name. Empty on every live row, which
   * costs it no space. */
  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "style", PANGO_STYLE_ITALIC, "xalign", 1.0f, NULL);
  gtk_cell_renderer_set_sensitive(renderer, FALSE);
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", MODCHOOSER_NOTE);

  gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), col);

  g_signal_connect(treeview, "button-press-event", G_CALLBACK(_modchooser_button_pressed), store);

  gtk_box_pack_start(GTK_BOX(content),
                     dt_ui_scroll_wrap(treeview, 200, "plugins/darkroom/masks/modulechooserheight",
                                       DT_UI_RESIZE_DYNAMIC),
                     TRUE, TRUE, 0);

  gtk_widget_show_all(dialog);
  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  if(response == GTK_RESPONSE_ACCEPT) _modchooser_collect(store, to_attach, to_detach);

  gtk_widget_destroy(dialog);
  g_object_unref(store);
  dt_gui_refocus_parent(parent);

  return response == GTK_RESPONSE_ACCEPT;
}

/* Gives a module a drawn mask of its own -- an empty group named after it, with drawn blending
 * switched on -- and answers it, writing its id to own_id. Answers NULL if the form could not be
 * made; the abandoned one is registered in dev->allforms and released with the image. */
static dt_masks_form_t *_module_create_own_mask(dt_develop_t *dev, dt_iop_module_t *module, int *own_id)
{
  dt_masks_form_t *own = dt_masks_create_ext(dev, DT_MASKS_GROUP);
  if(IS_NULL_PTR(own)) return NULL;

  gchar *name = dt_dev_get_masks_group_name(module);
  g_strlcpy(own->name, name, sizeof(own->name));
  dt_free(name);

  dt_masks_form_info_t own_info = { 0 };
  if(!dt_masks_form_get_info(own, &own_info)) return NULL;
  *own_id = own_info.formid;

  dt_masks_append_form(dev, own);

  // A module's blend_params are its own history entry; the forms get one of their own later.
  if(dt_iop_gui_blend_set_drawn_mask_group(module, *own_id))
    dt_dev_add_history_item(dev, module, TRUE, TRUE);

  return own;
}

/* Puts the row's form to work in the modules the user picks.
 *
 * Every module renders its OWN mask group -- created here, named "Mask <module>", if it has none
 * yet -- and the row's form is nested as a member of each. What is shared between the modules is
 * that form, not the mask holding it: one shape or shape group, referenced by as many module
 * masks as tick it.
 *
 * That is what keeps each module independent. A module's own mask carries its own combine
 * operators, opacities and order, so attaching the same shape group to a second module cannot
 * disturb the first -- and unticking one removes the form from THAT module's mask alone. The
 * alternative, pointing several modules at one mask group, would make every one of those
 * settings, and every detach, common to all of them.
 */
static void _tree_row_assign_to_modules(dt_shape_manager_list_t *list, GtkTreeModel *model,
                                        GtkTreeIter *iter)
{
  dt_lib_module_t *self = list->self;
  dt_develop_t *const dev = dt_dev_get_global();

  int fid = -1;
  _shape_manager_get_values(model, iter, NULL, NULL, &fid);
  dt_masks_form_t *form = dt_masks_get_from_id(dev, fid);
  if(IS_NULL_PTR(form)) return;

  GList *to_attach = NULL;
  GList *to_detach = NULL;
  if(!_modchooser_run(form, &to_attach, &to_detach)) return;

  int created_id = 0;
  int created_count = 0;
  gboolean changed = FALSE;

  /* Detaching first, so a module the user unticked and one they ticked cannot fight over the
   * same mask within one validation. */
  for(const GList *m = to_detach; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)m->data;
    dt_masks_form_t *own = dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(IS_NULL_PTR(own) || !(own->type & DT_MASKS_GROUP)) continue;

    // Empties the mask and deletes it if nothing is left, which dt_masks_form_delete() handles.
    dt_masks_form_delete(dev, module, own, form);
    dt_iop_gui_blend_masks_update(module);
    changed = TRUE;
  }

  for(const GList *m = to_attach; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)m->data;
    int own_id = module->blend_params->mask_id;
    dt_masks_form_t *own = dt_masks_get_from_id(dev, own_id);

    if(IS_NULL_PTR(own) || !(own->type & DT_MASKS_GROUP))
    {
      own = _module_create_own_mask(dev, module, &own_id);
      if(IS_NULL_PTR(own)) break;

      created_id = own_id;
      created_count++;
      changed = TRUE;
    }

    // Already there, or it would close a cycle: leave the mask alone.
    if(dt_masks_group_get_member(dev, own_id, fid, NULL) == DT_MASKS_OK) continue;
    if(dt_masks_group_contains(dev, fid, own_id) == DT_MASKS_OK) continue;

    own = dt_masks_cow_touch(dev, own);
    if(IS_NULL_PTR(dt_masks_group_add_form(dev, own, form))) continue;

    dt_iop_gui_blend_masks_update(module);
    changed = TRUE;
  }

  g_list_free(to_attach);
  g_list_free(to_detach);
  if(!changed) return;

  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);

  /* A mask the user has just conjured wants a name, so its row opens straight into editing --
   * but only when exactly one was made. With several there is no "the" one to open, and each
   * already carries its module's name, which is the answer most of the time anyway. */
  if(created_count == 1) _tree_edit_group_name(self, created_id);
}

/* The inventory's "+": add this row's form to the group the module list points at. */
static void _tree_row_add_to_group(dt_shape_manager_list_t *list, GtkTreeModel *model, GtkTreeIter *iter)
{
  dt_lib_module_t *self = list->self;
  const dt_shape_manager_t *const lm = (const dt_shape_manager_t *)self->data;
  dt_develop_t *const dev = dt_dev_get_global();

  // The same question the icon was drawn from, so a greyed "+" cannot act.
  if(!_row_can_be_added(lm, model, iter)) return;

  int fid = -1;
  _shape_manager_get_values(model, iter, NULL, NULL, &fid);

  dt_masks_form_t *form = dt_masks_get_from_id(dev, fid);
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, _selected_group_in_module_list(lm));
  if(IS_NULL_PTR(form) || IS_NULL_PTR(grp)) return;

  grp = dt_masks_cow_touch(dev, grp);
  if(IS_NULL_PTR(dt_masks_group_add_form(dev, grp, form))) return;

  dt_dev_add_history_item(dev, NULL, FALSE, TRUE);
  _shape_manager_recreate_list(self);
  _shape_manager_broadcast(self, 0, 0, DT_MASKS_EVENT_CHANGE);
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
static void _tree_row_action(dt_shape_manager_list_t *list, GtkTreeModel *model, GtkTreeIter *iter)
{
  dt_lib_module_t *self = list->self;
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

static void _tree_duplicate_shape(GtkButton *button __attribute__((unused)), dt_shape_manager_list_t *list)
{
  dt_lib_module_t *self = list->self;
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;

  // we get the selected node
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
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
                              const gchar *new_text, dt_shape_manager_list_t *list)
{
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
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
     || !dt_iop_module_supports_drawn_mask(module))
    return;

  /* Blend data exists as soon as the module has a GUI, but its masks widgets only once the
   * masks body has been built, which masks_inited says -- dt_iop_gui_blend_masks_update() asks
   * the same question. A module whose mask the manager lists need never have been expanded. */
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->gui->blend_data;
  if(!bd->masks_inited) return;

  bd->masks_shown = 1;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
  gtk_widget_queue_draw(bd->masks_edit);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_shape_manager_list_t *list)
{
  const dt_shape_manager_t *lm = (const dt_shape_manager_t *)list->self->data;
  dt_develop_t *const dev = dt_dev_get_global();

  /* Ahead of the gui_reset gate: what the module list points at decides whether the inventory's
   * "+" is available, and that stays true while the panel is driving itself -- a rebuild
   * reselects rows with gui_reset raised, and the button must follow. */
  if(list->which == DT_SHAPE_LIST_MODULES) _shape_manager_sync_add_sensitivity(lm);

  if(lm->gui_reset) return;
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
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
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
  /* dt_masks_change_form_gui() is NULL-safe on dev->form_gui throughout -- it is only ever
   * allocated by dt_masks_gui_init(), on entering darkroom, and freed back to NULL on leaving it
   * (views/darkroom.c, views/studio_capture.c's own dev teardown). This panel's window is a
   * standalone toplevel that can outlive that: switching away from darkroom while it stays open,
   * then selecting a row here, reached this point with dev->form_gui NULL and no guard (SIGSEGV,
   * observed live). edit_mode has nothing to record it into then, and there is no "current form"
   * for the pipeline to preview either -- the whole point of a view with no darkroom -- so this
   * simply has nothing to do. */
  if(IS_NULL_PTR(dev->form_gui)) return;
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

/* The shapes already drawn on this image, each labelled with the other modules already using it.
 * One grp cannot take -- it already holds it, directly or through a sub-group, or it is a group
 * that would close a cycle -- is listed greyed rather than left out, so the user sees it is there;
 * the same rule, _form_can_join_group(), as "Attach to the group". */
static void _menu_append_existing_shapes(GtkMenuShell *menu, dt_masks_form_t *grp, const int grpid,
                                         dt_iop_module_t *module)
{
  gboolean any = FALSE;
  GtkWidget *shapes_menu = gtk_menu_new();

  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_t *form = (const dt_masks_form_t *)forms->data;
    dt_masks_form_info_t info;
    if(!dt_masks_form_get_info(form, &info) || (info.type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
       || info.formid == grpid)
      continue;

    char str[10000] = "";
    if(_tree_format_form_usage_label(str, sizeof(str), form, module) < 0) continue;

    GtkWidget *item = gtk_menu_item_new_with_label(str);
    g_object_set_data(G_OBJECT(item), "formid", GUINT_TO_POINTER(info.formid));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_tree_add_exist), grp);
    gtk_widget_set_sensitive(item, _form_can_join_group(dt_dev_get_global(), grpid, info.formid));
    gtk_menu_shell_append(GTK_MENU_SHELL(shapes_menu), item);
    any = TRUE;
  }

  if(!any)
  {
    gtk_widget_destroy(shapes_menu);
    return;
  }

  GtkWidget *item = gtk_menu_item_new_with_label(_("Attach shape ..."));
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), shapes_menu);
  gtk_menu_shell_append(menu, item);
}

/* "Attach to the group": every group of the image, as its own submenu. The whole selection joins the
 * group picked, less whatever cannot -- _tree_add_to_group() asks _form_can_join_group() again,
 * form by form. A group none of the selection can join (it already holds it, or taking it would
 * close a cycle) is listed greyed rather than left out, so the user sees where the shapes already
 * are instead of wondering where a group went. Left out are the selected groups themselves -- a
 * group joining itself is not a choice at all -- and the retouch and spot groups, which belong to
 * their module, as in "Attach shape ...". The entry itself is greyed only when there is no group to
 * list. */
static void _menu_append_join_group(GtkMenuShell *menu, dt_shape_manager_list_t *list)
{
  dt_develop_t *const dev = dt_dev_get_global();

  // Counted, then copied: a list that grew in between is only cut short, never overrun.
  const guint capacity = dt_masks_group_list(dev, NULL, 0);
  dt_masks_form_info_t *groups = g_new0(dt_masks_form_info_t, MAX(capacity, 1));
  const guint count = MIN(capacity, dt_masks_group_list(dev, groups, capacity));

  GList *ids = _selected_form_ids(list);
  GtkWidget *groups_menu = gtk_menu_new();
  gboolean any = FALSE;
  for(guint g = 0; g < count; g++)
  {
    if(groups[g].is_retouch || !IS_NULL_PTR(g_list_find(ids, GINT_TO_POINTER(groups[g].formid)))) continue;

    gboolean joinable = FALSE;
    for(const GList *id = ids; id && !joinable; id = g_list_next(id))
      joinable = _form_can_join_group(dev, groups[g].formid, GPOINTER_TO_INT(id->data));

    GtkWidget *item = gtk_menu_item_new_with_label(groups[g].name);
    g_object_set_data(G_OBJECT(item), "group-formid", GINT_TO_POINTER(groups[g].formid));
    g_signal_connect(item, "activate", G_CALLBACK(_tree_add_to_group), list);
    gtk_widget_set_sensitive(item, joinable);
    gtk_menu_shell_append(GTK_MENU_SHELL(groups_menu), item);
    any = TRUE;
  }
  g_list_free(ids);
  dt_free(groups);

  GtkWidget *item = gtk_menu_item_new_with_label(_("Attach to the group"));
  gtk_menu_shell_append(menu, item);
  if(any)
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), groups_menu);
  else
  {
    gtk_widget_destroy(groups_menu);
    gtk_widget_set_sensitive(item, FALSE);
  }
}

/* One entry per combine mode, plus the reordering pair. The same Invert/Union/Intersection/
 * Difference/Exclusion grouping the darkroom canvas and the blend module offer; all five differ
 * by a constant, so they ride on the menu item under "masks-operation". */
static void _menu_append_operations(GtkMenuShell *menu, dt_shape_manager_list_t *list, const int nb)
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
  g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, list);
  gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);

  // Combining is a question about one shape against its group; several at once has no answer.
  if(nb == 1)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), gtk_separator_menu_item_new());
    for(size_t i = 0; i < sizeof(combine) / sizeof(combine[0]); i++)
    {
      item = gtk_menu_item_new_with_label(_(combine[i].label));
      g_object_set_data(G_OBJECT(item), "masks-operation", GINT_TO_POINTER(combine[i].state));
      g_signal_connect(item, "activate", (GCallback)_tree_apply_operation, list);
      gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), item);
    }
  }

  gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  item = gtk_menu_item_new_with_label(_("Move up"));
  g_signal_connect(item, "activate", (GCallback)_tree_moveup, list);
  gtk_menu_shell_append(menu, item);
  item = gtk_menu_item_new_with_label(_("Move down"));
  g_signal_connect(item, "activate", (GCallback)_tree_movedown, list);
  gtk_menu_shell_append(menu, item);
  gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
}

static GtkWidget *_tree_context_menu(GtkTreeSelection *selection, GtkTreeModel *model,
                                     dt_shape_manager_list_t *list, dt_iop_module_t *module)
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

  // Both act on the whole selection, one row or several, wherever the rows sit.
  if(nb > 0)
  {
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("New group from selection"));
    g_signal_connect(item, "activate", (GCallback)_tree_group, list);
    gtk_menu_shell_append(menu, item);
    _menu_append_join_group(menu, list);
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

  if(from_group && depth < 3) _menu_append_operations(menu, list, nb);

  if(!from_group && !grp_is_group && nb == 1)
  {
    item = gtk_menu_item_new_with_label(_("Duplicate shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_duplicate_shape, list);
    gtk_menu_shell_append(menu, item);
    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
  }
  
  if(!from_group && nb > 0)
  {
    // One entry, named for what the row holds -- the whole mask when it is a group.
    item = gtk_menu_item_new_with_label(grp_is_group ? _("Delete mask") : _("Delete shape"));
    g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, list);
    gtk_menu_shell_append(menu, item);
  }
  else if(nb > 0 && depth < 3)
  {
    item = gtk_menu_item_new_with_label(_("Remove shape from mask"));
    g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, list);
    gtk_menu_shell_append(menu, item);
  }

  item = gtk_menu_item_new_with_label(_("Delete unused shapes"));
  g_signal_connect(item, "activate", (GCallback)_tree_delete_unused, list);
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
static int _tree_button_pressed(GtkWidget *treeview, GdkEventButton *event, dt_shape_manager_list_t *list)
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
    // The action icons act on the row under the pointer alone, whatever is selected in this
    // list: they are buttons the row carries, not a command applied to the selection.
    if(on_row && mouse_col == list->action_col)
    {
      gtk_tree_path_free(mouse_path);
      _tree_row_action(list, model, &iter);
      return 1;
    }

    /* The "+" is the one exception: it reads the OTHER list's selection, which is what it adds
     * to. Greyed out when there is none, and then a click on it does nothing rather than
     * falling through to selecting the row -- the row under a disabled button is not what the
     * user was aiming at. */
    if(on_row && !IS_NULL_PTR(list->add_col) && mouse_col == list->add_col)
    {
      gtk_tree_path_free(mouse_path);
      _tree_row_add_to_group(list, model, &iter);
      return 1;
    }

    if(on_row && !IS_NULL_PTR(list->assign_col) && mouse_col == list->assign_col)
    {
      gtk_tree_path_free(mouse_path);
      _tree_row_assign_to_modules(list, model, &iter);
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
    GtkWidget *menu = _tree_context_menu(selection, model, list, module);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    handled = 1;
  }

  // One exit for the path: it was leaked on every button-1 press before, and freed on only one
  // of the two button-3 branches.
  if(mouse_path) gtk_tree_path_free(mouse_path);
  return handled;
}

/* A double-click on a name renames it. GtkTreeView reports it as an activation of that row and
 * column, which nothing else on these trees answers. The icon columns never get here from the
 * mouse: _tree_button_pressed() consumes both presses of a double-click on them. */
static void _tree_row_activated(GtkTreeView *treeview, GtkTreePath *path, GtkTreeViewColumn *column,
                                dt_shape_manager_list_t *list)
{
  if(column != list->name_col) return;

  GtkTreeModel *model = gtk_tree_view_get_model(treeview);
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, path)) return;

  _tree_start_name_editing(list, model, &iter, path);
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

/* What the action column's icon does depends on the row's depth: a top-level row owns its shape
 * and deleting it is permanent, a nested one only holds a membership. */
static const char *_tooltip_action_text(const int grid)
{
  return (grid == 0) ? _("Permanently delete this shape. It is detached from every mask "
                         "and removed from the list of available shapes.")
                     : _("Detach this shape from the mask. The shape is kept and stays "
                         "available for reuse.");
}

/* The chooser manages attachments for whatever the row holds, so the text names it. */
static gchar *_tooltip_assign_text(GtkTreeModel *model, GtkTreeIter *iter)
{
  int fid = -1;
  _shape_manager_get_values(model, iter, NULL, NULL, &fid);

  dt_masks_form_info_t row_info = { 0 };
  dt_masks_form_get_info(dt_masks_get_from_id(dt_dev_get_global(), fid), &row_info);

  return g_strdup_printf(_("Manage which modules use this %s. A mask is created for the "
                           "ticked ones that have none, and shared between them."),
                         row_info.is_group ? _("group") : _("shape"));
}

/* The "+" says what it will do, so it has to name what is selected right now -- and when it is
 * dead, which of the reasons it is dead for. The refusals are read off the same questions
 * _row_can_be_added() asks, so the wording can never disagree with the icon. */
static gchar *_tooltip_add_text(const dt_shape_manager_t *lm, GtkTreeModel *model, GtkTreeIter *iter)
{
  const int group_id = _selected_group_in_module_list(lm);
  const dt_masks_form_t *grp = dt_masks_get_from_id(dt_dev_get_global(), group_id);

  if(IS_NULL_PTR(grp))
    return g_strdup(_("Select a mask in the module groups list to attach this shape to it."));

  if(_row_can_be_added(lm, model, iter))
    return g_strdup_printf(_("Attach this to the mask '%s'."), grp->name);

  int fid = -1;
  _shape_manager_get_values(model, iter, NULL, NULL, &fid);
  dt_develop_t *const dev = dt_dev_get_global();

  if(dt_masks_group_contains(dev, group_id, fid) == DT_MASKS_OK)
    return g_strdup_printf(_("This is already part of the mask '%s'."), grp->name);

  if(dt_masks_group_contains(dev, fid, group_id) == DT_MASKS_OK)
    return g_strdup_printf(_("This group cannot be added to the mask '%s': it already contains it."),
                           grp->name);

  // The remaining refusal: every shape it holds is already in the target.
  return g_strdup_printf(_("The mask '%s' already uses every shape of this group."), grp->name);
}

/* The tooltip of the row's buttons, answered from the column the pointer is over.
 *
 * The pointer's column has to be asked here rather than left to gtk_tree_view_get_tooltip_context(),
 * which reports the row but not the column, and rewrites x/y on the way.
 *
 * @return whether the pointer was over a button column, i.e. whether the tooltip was set. */
static gboolean _tree_button_tooltip(const dt_shape_manager_list_t *list, GtkTreeView *tree_view,
                                     const gint x, const gint y, GtkTooltip *tooltip)
{
  gint bx = 0, by = 0;
  gtk_tree_view_convert_widget_to_bin_window_coords(tree_view, x, y, &bx, &by);

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(tree_view, bx, by, &path, &column, NULL, NULL)) return FALSE;

  const gboolean on_action = (column == list->action_col);
  const gboolean on_add = !IS_NULL_PTR(list->add_col) && (column == list->add_col);
  const gboolean on_assign = !IS_NULL_PTR(list->assign_col) && (column == list->assign_col);

  GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
  GtkTreeIter iter;
  const gboolean got = (on_action || on_add || on_assign) && gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_path_free(path);
  if(!got) return FALSE;

  if(on_action)
  {
    int grid = -1;
    _shape_manager_get_values(model, &iter, NULL, &grid, NULL);
    gtk_tooltip_set_text(tooltip, _tooltip_action_text(grid));
    return TRUE;
  }

  gchar *text = on_assign ? _tooltip_assign_text(model, &iter)
                          : _tooltip_add_text((const dt_shape_manager_t *)list->self->data, model, &iter);
  gtk_tooltip_set_text(tooltip, text);
  dt_free(text);

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
  const dt_shape_manager_list_t *list = (const dt_shape_manager_list_t *)data;

  // Keyboard tooltips carry no position, so they can only be about the row.
  if(!keyboard_tip && !IS_NULL_PTR(list) && _tree_button_tooltip(list, tree_view, x, y, tooltip))
    return TRUE;

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
  int root_id;             /* the module mask this row sits somewhere inside, 0 outside the
                            * module list -- the scope the note below is searched in */
  gboolean flat;           /* TRUE to append this row without descending into its own members --
                            * a module mask shown in the inventory: a single, non-expandable row
                            * there, its full subtree still shown, expandable, in the module list */
} _tree_row_t;

/* Every module whose drawn mask is this group, in pipeline order.
 *
 * The link between a group and a module is blend_params->mask_id, which lives in each module's
 * own params blob, so nothing stops several modules from naming the same group -- and that is
 * what a group shared between modules is. There is no stored back-reference to keep in step:
 * the answer is derived here, from the one place the truth is, on a walk over ~80 modules.
 *
 * Returns a GList of dt_iop_module_t* the caller frees with g_list_free(), borrowing the
 * modules themselves; NULL when no module uses the group. */
static GList *_modules_owning_group(const dt_masks_form_t *group)
{
  if(IS_NULL_PTR(group)) return NULL;

  GList *owners = NULL;
  for(const GList *iops = dt_dev_get_global()->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)iops->data;
    if(dt_iop_module_supports_drawn_mask(iop) && iop->blend_params->mask_id == group->formid)
      owners = g_list_prepend(owners, iop);
  }

  return g_list_reverse(owners);
}

/* The first module using this group, for the callers that need one module to speak for the row --
 * the tree stores a single module per row. A group listed at top level with no module yet is the
 * only case worth asking about: a nested one inherits its parent's. */
static dt_iop_module_t *_module_owning_group(const dt_masks_form_t *group)
{
  GList *owners = _modules_owning_group(group);
  dt_iop_module_t *first = IS_NULL_PTR(owners) ? NULL : (dt_iop_module_t *)owners->data;
  g_list_free(owners);
  return first;
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

  /* One by-value description of the form, which every field below is taken from. The name it
   * carries is copied rather than borrowed, so nothing here holds a pointer into a refcounted
   * form across a call that could clone it. */
  dt_masks_form_info_t info = { 0 };
  if(!dt_masks_form_get_info(row->form, &info)) return;

  // Only a top-level row asks who else uses the shape: a row under a group already says so.
  char used_by[1000] = "";
  int nbuse = 0;
  if(row->grp_id == 0) _is_form_used(info.formid, used_by, sizeof(used_by), &nbuse);

  /* Only inside a module's mask, and only for a shape sitting under a group: a top-level row is
   * not reached through anything, and a group's own duplication is a different question.
   *
   * Only the instances AFTER the first are marked. Both used to be, symmetrically, which said
   * that the shape appears twice but not which application the other is measured against. */
  gchar *note = NULL;
  if(row->root_id > 0 && row->grp_id > 0 && !info.is_group)
  {
    int holder_id = 0;
    guint holder_index = 0;
    char holder_name[DT_MASKS_FORM_NAME_LEN] = "";
    if(dt_masks_group_first_use(dt_dev_get_global(), row->root_id, info.formid, &holder_id,
                                &holder_index, holder_name, sizeof(holder_name)) == DT_MASKS_OK
       && (holder_id != row->grp_id || (int)holder_index != row->index))
      // Same wording the Drawn tab's shape list already uses for the same kind of remark.
      note = g_strdup_printf(_("Already in '%s'"), holder_name);
  }

  gtk_tree_store_append(treestore, child, toplevel);
  gtk_tree_store_set(treestore, child, TREE_TEXT, info.name, TREE_MODULE, row->module,
                     TREE_GROUPID, row->grp_id, TREE_FORMID, info.formid,
                     TREE_EDITABLE, (row->grp_id == 0), TREE_IC_OP, icop,
                     TREE_IC_OP_VISIBLE, (!IS_NULL_PTR(icop)), TREE_IC_INVERSE, icinv,
                     TREE_IC_INVERSE_VISIBLE, (!IS_NULL_PTR(icinv)),
                     TREE_IC_USED_VISIBLE, (nbuse > 0), TREE_USED_TEXT, used_by,
                     TREE_IC_DELETE_VISIBLE, (row->grp_id == 0),
                     TREE_IC_UNLINK_VISIBLE, (row->grp_id != 0),
                     TREE_NOTE, IS_NULL_PTR(note) ? "" : note, -1);
  dt_free(note);
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

  // A flat row is a single line by request: no expander, no children appended under it.
  if(self.flat) return;

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
                                       .opacity = grpt->opacity, .index = index,
                                       .root_id = self.root_id };
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

GList *_shape_manager_get_selected(dt_shape_manager_list_t *list)
{
  GList *res = NULL;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));

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
static void _tree_reveal_row(dt_shape_manager_list_t *list, GtkTreeModel *model, GtkTreeIter *iter,
                             const gboolean exclusive)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
  GtkTreePath *path = gtk_tree_model_get_path(model, iter);

  if(exclusive) gtk_tree_selection_unselect_all(selection);
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(list->treeview), path);
  gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(list->treeview), path, NULL, TRUE, 0.5, 0.5);
  gtk_tree_selection_select_iter(selection, iter);

  gtk_tree_path_free(path);
}

/* Whether this group is some module's drawn mask, which is what the module list holds. */
static gboolean _group_is_module_mask(const dt_masks_form_t *form)
{
  if(IS_NULL_PTR(form) || !(form->type & DT_MASKS_GROUP)) return FALSE;

  GList *owners = _modules_owning_group(form);
  const gboolean assigned = !IS_NULL_PTR(owners);
  g_list_free(owners);

  return assigned;
}

/* Appends one group's row (and, recursively, everything under it, unless @p flat) and answers
 * whether it did -- FALSE only when a module-mask row's own id could not be read, which leaves
 * the row unlisted rather than mislabelled. root_id is a property of the row's SCOPE (see
 * _tree_row_t), not of the group itself: it is only ever set for a module mask, so the "applied
 * twice in this mask" note has something to search.
 *
 * @param flat the inventory's own copy of a module mask: a single row, no expander, no members
 *             appended under it -- that subtree is the module list's to show, not shown twice. */
static gboolean _tree_store_append_group_row(GtkTreeStore *treestore, dt_shape_manager_t *lm,
                                             dt_masks_form_t *form, const gboolean is_module_mask,
                                             const gboolean flat)
{
  int root_id = 0;
  if(is_module_mask)
  {
    dt_masks_form_info_t info = { 0 };
    if(!dt_masks_form_get_info(form, &info)) return FALSE;
    root_id = info.formid;
  }

  const _tree_row_t row = { .form = form, .opacity = 1.0f, .root_id = root_id, .flat = flat };
  _shape_manager_list_recurs(treestore, NULL, lm, &row);
  return TRUE;
}

/* Returns whether it added anything, which is what tells the caller a separator is worth having. */
static gboolean _tree_store_add_forms(GtkTreeStore *treestore, dt_shape_manager_t *lm,
                                      const dt_shape_list_t which, const gboolean groups)
{
  gboolean any = FALSE;

  /* Module masks are walked in pipeline order rather than in creation order, in both lists --
   * the inventory holds them too (see _group_is_module_mask()'s own comment).
   * Reverse iop_order, the "bottom of the stack first" convention _modchooser_run() and the
   * module groups panel's Pipeline tab already use for the same kind of module list, so a
   * module's mask lands at the same relative position here as its own row does everywhere else
   * in the darkroom.
   *
   * Scoped exactly like _modules_owning_group() -- every module in dev->iop, not just the ones
   * dt_iop_module_is_in_pipeline() currently shows -- so a mask belonging to a hidden or
   * not-yet-reached instance is still found here rather than silently dropped from both this
   * loop and the "unclaimed groups" one below, which skips it on the assumption it was already
   * handled. */
  if(groups)
  {
    dt_develop_t *const dev = dt_dev_get_global();
    for(const GList *iops = g_list_last(dev->iop); iops; iops = g_list_previous(iops))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)iops->data;
      if(!dt_iop_module_supports_drawn_mask(module)) continue;

      dt_masks_form_t *mask = dt_masks_get_from_id(dev, module->blend_params->mask_id);
      if(IS_NULL_PTR(mask) || !(mask->type & DT_MASKS_GROUP)) continue;

      // Expandable in the module list (its own home), a single flat row in the inventory.
      if(_tree_store_append_group_row(treestore, lm, mask, TRUE, which == DT_SHAPE_LIST_SHAPES))
        any = TRUE;
    }

    // The module list holds nothing else.
    if(which == DT_SHAPE_LIST_MODULES) return any;
  }

  for(const GList *forms = dt_dev_get_global()->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(!!(form->type & DT_MASKS_GROUP) != groups) continue;

    // Already appended above, in pipeline order: this pass is unclaimed groups (and shapes) only.
    if(groups && _group_is_module_mask(form)) continue;

    if(_tree_store_append_group_row(treestore, lm, form, FALSE, FALSE)) any = TRUE;
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

/* An empty store with the columns both lists read (see dt_masks_tree_cols_t). */
static GtkTreeStore *_tree_store_new(void)
{
  // we store : text ; *module ; groupid ; formid
  return gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN,
                            GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                            G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_BOOLEAN);
}

/* The inventory list shows the unclaimed groups first, then every shape, with a rule between
 * them. The module list has only groups, so it gets neither the second pass nor the rule. */
static GtkTreeStore *_tree_store_build(dt_shape_manager_t *lm, const dt_shape_list_t which)
{
  GtkTreeStore *treestore = _tree_store_new();
  const gboolean had_groups = _tree_store_add_forms(treestore, lm, which, TRUE);
  if(which == DT_SHAPE_LIST_MODULES) return treestore;


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

  const gboolean had_shapes = _tree_store_add_forms(treestore, lm, which, FALSE);
  if(had_groups && !had_shapes) gtk_tree_store_remove(treestore, &separator);

  return treestore;
}

/* Puts back what was selected before the store was replaced. selectids holds three entries per
 * row -- module, group id, form id -- as _shape_manager_get_selected() built it. */
static void _tree_restore_selection(dt_shape_manager_list_t *list, GtkTreeModel *model, const GList *selectids)
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

    if(_find_mask_iter_by_values(model, &iter, mod, fid, 1)) _tree_reveal_row(list, model, &iter, FALSE);
  }
}

/* Points the tree at the focused module's mask group, and says whether it moved the selection --
 * the caller replays the selection handler when it did, so the canvas follows. */
static gboolean _tree_select_module_group(dt_shape_manager_list_t *list, GtkTreeModel *model,
                                          const dt_masks_form_gui_t *gui)
{
  dt_iop_module_t *const module = dt_dev_get_global()->gui_module;
  const int group_id = dt_iop_module_supports_drawn_mask(module) ? module->blend_params->mask_id : 0;

  if(group_id <= 0) return FALSE;
  // Mid-creation the tree follows the shape being drawn, not the module.
  if(!IS_NULL_PTR(gui) && gui->creation) return FALSE;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
  if(!_find_mask_iter_by_values(model, &iter, module, group_id, 1)) return FALSE;

  _tree_reveal_row(list, model, &iter, TRUE);
  return TRUE;
}

/* Whether the panel has anything to show: only the darkroom has a develop to read shapes from.
 * The window is a toplevel of its own and outlives the view -- the user can leave the darkroom
 * with it open -- while what its rows point at does not: the darkroom's leave() frees dev->iop,
 * and a row stores its module by address (TREE_MODULE). */
static gboolean _shape_manager_is_active(void)
{
  dt_view_manager_t *vm = dt_view_manager_get_global();
  const dt_view_t *cv = IS_NULL_PTR(vm) ? NULL : dt_view_manager_get_current_view(vm);
  return !IS_NULL_PTR(cv) && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM;
}

static void _shape_manager_recreate_list(dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm) || lm->gui_reset) return;

  // Everything below drives the trees itself, so the handlers they would wake must stay quiet.
  const int gui_reset = lm->gui_reset;
  lm->gui_reset = 1;

  /* Outside the darkroom both lists are emptied and the whole window greyed out: no row would
   * still be valid, and no button has a develop to act on. Decided here, where every rebuild
   * passes, so no caller -- a mask signal, the develop proxy, a view switch -- can refill it. */
  const gboolean active = _shape_manager_is_active();
  GtkWidget *content = IS_NULL_PTR(lm->popup_window) ? NULL : gtk_bin_get_child(GTK_BIN(lm->popup_window));
  if(!IS_NULL_PTR(content)) gtk_widget_set_sensitive(content, active);

  if(!active)
  {
    for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
    {
      dt_shape_manager_list_t *list = &lm->lists[i];
      if(IS_NULL_PTR(list->treeview)) continue;

      // An empty store rather than none: every handler reads the model without a NULL check.
      GtkTreeStore *empty = _tree_store_new();
      gtk_tree_view_set_model(GTK_TREE_VIEW(list->treeview), GTK_TREE_MODEL(empty));
      g_object_unref(empty);
    }
    lm->gui_reset = gui_reset;
    return;
  }

  // Rebuilding the list also refreshes shapes created during continuous creation. In that case
  // the active creation button must stay active until the user cancels creation explicitly.
  dt_masks_form_gui_t *gui = dt_dev_get_global()->form_gui;
  if(IS_NULL_PTR(gui) || !gui->creation) dt_masks_shape_buttons_deactivate_all(NULL);

  /* Both stores are rebuilt from scratch: a form crosses from one list to the other the moment
   * a module claims or releases its group, so neither can be refreshed on its own. */
  dt_shape_manager_list_t *follow = NULL;
  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    dt_shape_manager_list_t *list = &lm->lists[i];
    if(IS_NULL_PTR(list->treeview)) continue;

    // The store is about to be replaced, so what is selected has to be read before it goes.
    GList *selectids = _shape_manager_get_selected(list);

    GtkTreeStore *treestore = _tree_store_build(lm, list->which);
    GtkTreeModel *model = GTK_TREE_MODEL(treestore);
    gtk_tree_view_set_model(GTK_TREE_VIEW(list->treeview), model);

    if(selectids)
    {
      _tree_restore_selection(list, model, selectids);
      g_list_free(selectids);
    }

    // Only the module list can follow the focused module's own mask group; the inventory does
    // not hold it.
    if(list->which == DT_SHAPE_LIST_MODULES && _tree_select_module_group(list, model, gui))
      follow = list;

    g_object_unref(treestore);
  }

  lm->gui_reset = gui_reset;

  // Both models were replaced, so whatever the module list held may or may not have come back.
  _shape_manager_sync_add_sensitivity(lm);

  if(!IS_NULL_PTR(follow))
    _tree_selection_change(gtk_tree_view_get_selection(GTK_TREE_VIEW(follow->treeview)), follow);
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

// Update each item of both lists. The same form can have a row in each, so neither is skipped.
static void _shape_manager_update_list(dt_lib_module_t *self)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm)) return;

  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    const dt_shape_manager_list_t *list = &lm->lists[i];
    if(IS_NULL_PTR(list->treeview)) continue;

    // for each node , we refresh the string
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
    if(!GTK_IS_TREE_MODEL(model)) continue;
    gtk_tree_model_foreach(model, _update_foreach, lm);
  }
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

/* Drops the (formid, parentid) row from one list's store. */
static void _shape_manager_remove_item_from(const dt_shape_manager_list_t *list, int formid, int parentid)
{
  if(IS_NULL_PTR(list->treeview)) return;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;
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

static void _shape_manager_remove_item(dt_lib_module_t *self, int formid, int parentid)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm)) return;

  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
    _shape_manager_remove_item_from(&lm->lists[i], formid, parentid);
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

/* Points one list at the form, and says whether it holds it at all. */
static gboolean _shape_manager_selection_change_in(dt_shape_manager_t *lm, const dt_shape_manager_list_t *list,
                                                  struct dt_iop_module_t *module, const int selectid,
                                                  const int throw_event)
{
  if(IS_NULL_PTR(list->treeview)) return FALSE;

  // we first unselect all
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
  lm->gui_reset = 1;
  gtk_tree_selection_unselect_all(selection);
  lm->gui_reset = 0;

  // we go through all nodes
  lm->gui_reset = 1 - throw_event;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
  if(!GTK_IS_TREE_MODEL(model))
  {
    lm->gui_reset = 0;
    return FALSE;
  }

  /* The recursive search below walks the MODEL (gtk_tree_model_iter_children()), which holds
   * every row regardless of the view's own expand/collapse display state -- it needs nothing
   * expanded to find its target. Expanding the WHOLE tree first, as this used to, was only ever
   * about making the match visible afterward, and it did that by exploding every OTHER group
   * open too, module masks and unclaimed groups alike, for a search that had nothing to do with
   * them. Revealing just the path to the row actually found -- the same targeted
   * expand-to-path/scroll _tree_reveal_row() already uses elsewhere -- gets the same visibility
   * without the side effect. */
  gboolean found = FALSE;
  if(gtk_tree_model_get_iter_first(model, &iter))
    found = _shape_manager_selection_change_r(model, selection, &iter, module, selectid, throw_event, 1);

  if(found)
  {
    GList *rows = gtk_tree_selection_get_selected_rows(selection, NULL);
    if(!IS_NULL_PTR(rows))
    {
      GtkTreePath *path = (GtkTreePath *)rows->data;
      gtk_tree_view_expand_to_path(GTK_TREE_VIEW(list->treeview), path);
      gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(list->treeview), path, NULL, TRUE, 0.5, 0.5);
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  }

  lm->gui_reset = 0;
  return found;
}

/* A form can have a row in either list, or in both -- a shape is listed in the inventory and
 * again under whichever module group holds it -- so both are pointed at it. */
static void _shape_manager_selection_change(dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event)
{
  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm)) return;

  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
    _shape_manager_selection_change_in(lm, &lm->lists[i], module, selectid, throw_event);
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

/* Answers whether the event names a row this panel is currently showing, refreshing every row
 * that does when the event is an UPDATE.
 *
 * A single-row event can name a row in either list -- or in both, when a module group holds a
 * shape the inventory also lists -- so both are asked. */
static gboolean _shape_manager_refresh_row(dt_lib_module_t *self, dt_shape_manager_t *lm,
                                           const int formid, const int parentid,
                                           const dt_masks_event_t event)
{
  gboolean found = FALSE;
  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    const dt_shape_manager_list_t *list = &lm->lists[i];
    if(IS_NULL_PTR(list->treeview)) continue;

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list->treeview));
    if(!GTK_IS_TREE_MODEL(model)) continue;

    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter_first(model, &iter)) continue;
    if(!_find_iter_by_parentid_and_formid(model, parentid, formid, &iter)) continue;

    found = TRUE;
    if(event == DT_MASKS_EVENT_UPDATE)
      _shape_manager_update_item(self, formid, parentid, lm, model, &iter);
  }

  return found;
}

static void _shape_manager_handler_callback(gpointer instance __attribute__((unused)), const int formid, const int parentid, const dt_masks_event_t event, dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self)) return;

  dt_shape_manager_t *lm = (dt_shape_manager_t *)self->data;
  if(IS_NULL_PTR(lm)) return;

  const gboolean found = _shape_manager_refresh_row(self, lm, formid, parentid, event);

  if(found)
  {
    switch(event)
    {
      case DT_MASKS_EVENT_UPDATE :
        // already done, once per list holding the row
        break;

      case DT_MASKS_EVENT_CHANGE :
      case DT_MASKS_EVENT_DELETE :
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

  /* A change or a deletion whose row cannot be found still rebuilds the whole list. A deleted
   * shape may no longer have its row; and a CHANGE raised with the ids (0, 0) names no row at
   * all -- it is how "the structure changed" is spelled by the handlers here that raise rather
   * than broadcast (adding an existing shape to a group, renaming, changing a combine
   * operation), which count on this handler to rebuild the tree. The handlers that rebuild it
   * themselves broadcast under gui_reset, which makes the rebuild a no-op for them. */
  else if(event == DT_MASKS_EVENT_CHANGE || event == DT_MASKS_EVENT_DELETE || event == DT_MASKS_EVENT_REMOVE)
  {
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

/* Empties or refills the lists on a view switch (see _shape_manager_is_active()). A "special"
 * module is never handed view_enter()/view_leave(), hence the signal; it is raised once the new
 * view has entered, so the darkroom's modules are loaded by the time the rows are built. */
static void _shape_manager_view_changed(gpointer instance __attribute__((unused)),
                                        dt_view_t *old_view __attribute__((unused)),
                                        dt_view_t *new_view __attribute__((unused)), dt_lib_module_t *self)
{
  _shape_manager_recreate_list(self);
}

/* Both lists order module masks by the pipe order (reverse iop_order), which is read at each
 * rebuild, so a reorder has to trigger one. Every reorder path raises this signal: drag and drop,
 * an order preset or reset (dt_iop_gui_commit_iop_order_change()), history navigation. */
static void _shape_manager_modules_moved(gpointer instance __attribute__((unused)), dt_lib_module_t *self)
{
  _shape_manager_recreate_list(self);
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
/** @brief The room a fresh, un-dragged dt_ui_scroll_wrap area is allowed to grow to -- the same
 * ceiling scroll_wrap.c's own ungated case uses, so raising a list's cap to this number is
 * indistinguishable from that list never having been dragged at all. */
static gint _shape_manager_height_ceiling(void)
{
  GtkWidget *win = dt_widget_root_window();
  return win ? gtk_widget_get_allocated_height(win) : DT_PIXEL_APPLY_DPI(1000);
}

/** @brief Forget how far the user last dragged each list, so the auto-sizing pass that follows
 * measures true, full content instead of stopping at a small stale cap from a previous session.
 *
 * There is no "erase this conf key" call, so the ceiling itself is stored in its place: as far as
 * dt_ui_scroll_wrap()'s own sizing rule can tell, that is exactly what an un-dragged list looks
 * like. A later manual drag overwrites it with a real choice again, same as always -- this only
 * resets what STARTUP sees. */
static void _shape_manager_relax_height_caps(const dt_shape_manager_t *d)
{
  const gint ceiling = _shape_manager_height_ceiling();
  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    const dt_shape_manager_list_t *list = &d->lists[i];
    if(!IS_NULL_PTR(list->height_key)) dt_widget_store_int(list->height_key, ceiling);
  }
}

/** @brief Grows the just-shown window by one row past whatever height the (now uncapped) lists
 * settled on, so the longer one reads as complete rather than filled edge-to-edge -- and so a
 * list exactly as tall as the window doesn't look like it might have one more row hidden below.
 *
 * Must run AFTER gtk_widget_show_all(): the row-height query works on an empty model, but the
 * window's OWN height only reflects the lists' true content once they have been realized and
 * dt_ui_scroll_wrap's sizing rule has run against the raised ceiling. */
static void _shape_manager_grow_by_one_row(const dt_shape_manager_t *d)
{
  if(!GTK_IS_WINDOW(d->popup_window)) return;

  gint row = 0;
  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    const gint h = dt_ui_scroll_wrap_row_height(d->lists[i].treeview);
    if(h > row) row = h;
  }
  if(row <= 0) return;

  gint width = 0;
  gint height = 0;
  gtk_window_get_size(GTK_WINDOW(d->popup_window), &width, &height);
  gtk_window_resize(GTK_WINDOW(d->popup_window), width, height + row);
}

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

    // Also before mapping: dt_ui_scroll_wrap's sizing rule reads this the moment each treeview
    // realizes, which show_all() triggers below.
    _shape_manager_relax_height_caps(d);

    gtk_widget_show_all(d->popup_window);

    // Only after: needs the lists' post-realize, freshly-uncapped height to add one row to it.
    _shape_manager_grow_by_one_row(d);
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

  /* No width request of its own: the window's own minimum is whatever the two lists below need
   * -- their own content (names are never ellipsized, so a long one raises that floor) or their
   * min-content-width default when there is none -- added together, and it grows further with
   * whatever the user drags the paned or the frame to. Heights come from each list's
   * dt_ui_scroll_wrap() rule. */

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

  // From here, everything goes into the shape manager popup,
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

  /* The "used by" icon, tinted once for both trees. It is a remark about the shape rather than
   * something to click, so it is drawn in the theme's own disabled grey -- a flat grey, next to
   * the action icon it sits beside, rather than the washed-out foreground a GtkCellRenderer's
   * insensitive state produces. A cell renderer has no colour of its own, hence a pre-tinted
   * pixbuf; the model still only carries whether a row shows one. */
  GdkRGBA used_color;
  if(!gtk_style_context_lookup_color(gtk_widget_get_style_context(shape_manager_container),
                                     "disabled_fg_color", &used_color))
    used_color = (GdkRGBA){ 0.62, 0.62, 0.62, 1.0 };

  d->ic_used = dt_gui_symbolic_icon_pixbuf("mail-attachment-symbolic", GTK_ICON_SIZE_MENU, &used_color, NULL);

  // The "+" in its available and unavailable tints: the plain foreground against the same
  // disabled grey the "used by" icon uses, which is the contrast that reads as a dead button.
  GdkRGBA fg_color;
  if(!gtk_style_context_lookup_color(gtk_widget_get_style_context(shape_manager_container),
                                     "fg_color", &fg_color))
    fg_color = (GdkRGBA){ 1.0, 1.0, 1.0, 1.0 };

  d->ic_add = dt_gui_symbolic_icon_pixbuf("list-add-symbolic", GTK_ICON_SIZE_MENU, &fg_color, NULL);
  d->ic_add_off = dt_gui_symbolic_icon_pixbuf("list-add-symbolic", GTK_ICON_SIZE_MENU, &used_color, NULL);
  d->ic_assign = dt_gui_symbolic_icon_pixbuf("view-list-symbolic", GTK_ICON_SIZE_MENU, &fg_color, NULL);

  /* The two lists sit side by side in a paned, so the split is the user's and persists. Each
   * half is built identically -- the only thing that differs between them is which forms their
   * store holds, which _tree_store_build() decides from list->which. */
  GtkWidget *lists_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  // Named so the theme can draw the divider between the two lists: a paned's handle is invisible
  // by default, and these are two separate inventories rather than two views of one thing.
  gtk_widget_set_name(lists_paned, "shape-manager-lists");

  static const struct
  {
    const char *title;
    const char *tooltip;
    const char *height_key;
  } list_defs[DT_SHAPE_LIST_COUNT] = {
    [DT_SHAPE_LIST_SHAPES] = { N_("All shapes"),
                               N_("Every shape and group drawn on this image, including the masks "
                                  "modules already use -- pick any of them up to reuse."),
                               "plugins/darkroom/masks/windowheight" },
    [DT_SHAPE_LIST_MODULES] = { N_("Module groups"),
                                N_("The masks modules actually render, and the shapes each one is made of."),
                                "plugins/darkroom/masks/moduleslistheight" },
  };

  for(int i = 0; i < DT_SHAPE_LIST_COUNT; i++)
  {
    dt_shape_manager_list_t *list = &d->lists[i];
    list->which = (dt_shape_list_t)i;
    list->self = self;
    list->height_key = list_defs[i].height_key;
    list->treeview = gtk_tree_view_new();

    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(list->treeview), col);

    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_OP, NULL);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_OP_VISIBLE);

    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_INVERSE, NULL);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_INVERSE_VISIBLE);

    /* No ellipsize: a name that does not fit is not something to shorten, it is something the
     * column has to make room for. Measured offscreen -- with no ellipsize set, a GtkTreeView
     * reports its true, full-content preferred width, and that width propagates all the way up
     * through a GTK_POLICY_NEVER scrolled window to the window itself; gtk_window_resize() (used
     * to restore a persisted width below) cannot force the window narrower than that reported
     * minimum, GTK clamps it back up. So the name is simply never compressed, in either list, by
     * a narrow paned split or a narrow restored window -- both floors hold at the content's own
     * minimum instead. */
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(col, renderer, "text", TREE_TEXT);
    // No "editable" attribute: see _tree_start_name_editing(), the only place that opens the editor.
    g_signal_connect(renderer, "edited", (GCallback)_tree_cell_edited, list);
    g_signal_connect(renderer, "editing-started", (GCallback)_tree_name_editing_started, list);
    // Kept so a double-clicked row, or a freshly created group's, can be opened into editing.
    list->name_col = col;
    list->name_renderer = renderer;

    /* Measured offscreen: of two renderers packed at a column's end, the FIRST one packed lands
     * at the true right edge, and each renderer packed after it sits closer to the main content
     * instead -- so the icon has to be packed before the note text, not after, to end up to the
     * note's right. */
    renderer = gtk_cell_renderer_pixbuf_new();
    // A theme with no symbolic variant of that icon leaves the pixbuf NULL: name the icon instead
    // and let GTK draw it, untinted, rather than show nothing.
    if(IS_NULL_PTR(d->ic_used))
      g_object_set(renderer, "icon-name", "mail-attachment-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
    else
      g_object_set(renderer, "pixbuf", d->ic_used, NULL);
    gtk_tree_view_column_pack_end(col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

    /* What the row has to say about itself, in italics, sitting to the LEFT of the "used by" icon
     * above (packed after it, per the same measurement) so it reads as an annotation on the name
     * rather than as part of it. Empty on every row that has nothing to add, which costs those no
     * space. */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "style", PANGO_STYLE_ITALIC, "xalign", 1.0f, NULL);
    gtk_cell_renderer_set_sensitive(renderer, FALSE);
    gtk_tree_view_column_pack_end(col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(col, renderer, "text", TREE_NOTE);

    /* The per-row action icon, to the right of everything the name column carries -- the "used
     * by" icon included, since that one is packed at that column's end. Both renderers live in
     * this one column and exactly one of them is visible on any row, so the icon lands in the
     * same place whichever action the row offers. The name column expands to take up the slack,
     * which is what keeps the action flush right. Clicks are answered in _tree_button_pressed()
     * by comparing the column, the way develop/blend_gui.c does for the same two icons. */
    gtk_tree_view_column_set_expand(col, TRUE);

    /* The inventory's "+", between the "used by" icon and the trash. Shown on the same rows the
     * trash is -- the top-level ones -- and greyed out until the module list points at a mask. */
    if(list->which == DT_SHAPE_LIST_SHAPES)
    {
      list->add_col = gtk_tree_view_column_new();
      gtk_tree_view_column_set_sizing(list->add_col, GTK_TREE_VIEW_COLUMN_FIXED);
      gtk_tree_view_column_set_fixed_width(list->add_col, DT_PIXEL_APPLY_DPI(24));

      renderer = gtk_cell_renderer_pixbuf_new();
      // Same fallback as the "used by" icon: a theme with no symbolic variant gets the plain
      // named icon, untinted, rather than nothing. The data func swaps the tinted pixbufs in
      // when they exist, and sets visibility, so no attribute is bound on this column.
      if(IS_NULL_PTR(d->ic_add_off))
        g_object_set(renderer, "icon-name", "list-add-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
      gtk_cell_renderer_set_sensitive(renderer, FALSE);
      gtk_tree_view_column_pack_start(list->add_col, renderer, FALSE);
      gtk_tree_view_column_set_cell_data_func(list->add_col, renderer, _add_cell_data_func, d, NULL);

      gtk_tree_view_append_column(GTK_TREE_VIEW(list->treeview), list->add_col);

      /* And next to it, the button that picks the modules instead of reusing a selected mask.
       * Always available: it needs nothing selected anywhere. */
      list->assign_col = gtk_tree_view_column_new();
      gtk_tree_view_column_set_sizing(list->assign_col, GTK_TREE_VIEW_COLUMN_FIXED);
      gtk_tree_view_column_set_fixed_width(list->assign_col, DT_PIXEL_APPLY_DPI(24));

      renderer = gtk_cell_renderer_pixbuf_new();
      if(IS_NULL_PTR(d->ic_assign))
        g_object_set(renderer, "icon-name", "view-list-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
      else
        g_object_set(renderer, "pixbuf", d->ic_assign, NULL);
      gtk_tree_view_column_pack_start(list->assign_col, renderer, FALSE);
      gtk_tree_view_column_add_attribute(list->assign_col, renderer, "visible", TREE_IC_DELETE_VISIBLE);

      gtk_tree_view_append_column(GTK_TREE_VIEW(list->treeview), list->assign_col);
    }

    list->action_col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_sizing(list->action_col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(list->action_col, DT_PIXEL_APPLY_DPI(24));

    renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer, "icon-name", "list-remove-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
    gtk_tree_view_column_pack_start(list->action_col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(list->action_col, renderer, "visible", TREE_IC_UNLINK_VISIBLE);

    renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(renderer, "icon-name", "user-trash-symbolic", "stock-size", GTK_ICON_SIZE_MENU, NULL);
    gtk_tree_view_column_pack_start(list->action_col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(list->action_col, renderer, "visible", TREE_IC_DELETE_VISIBLE);

    gtk_tree_view_append_column(GTK_TREE_VIEW(list->treeview), list->action_col);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list->treeview));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
    // Only the inventory list carries a rule; the module list holds groups alone.
    if(list->which == DT_SHAPE_LIST_SHAPES)
      gtk_tree_view_set_row_separator_func(GTK_TREE_VIEW(list->treeview), _tree_row_is_separator, NULL, NULL);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(list->treeview), FALSE);
    // A query-tooltip handler rather than a tooltip column: only the rows that carry a "used by"
    // text show one, which a column would not let us decide per row.
    g_object_set(list->treeview, "has-tooltip", TRUE, (gchar *)0);
    g_signal_connect(list->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), list);
    g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), list);
    g_signal_connect(list->treeview, "button-press-event", (GCallback)_tree_button_pressed, list);
    g_signal_connect(list->treeview, "row-activated", (GCallback)_tree_row_activated, list);

    /* Each half is a titled section of its own: with two trees side by side and no column
     * headers, the heading is what says which is which, and the rule the theme draws under a
     * section label is what closes each list off from the other. */
    GtkWidget *half = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
    gtk_widget_set_name(half, "shape-manager-list");
    GtkWidget *title = dt_ui_section_label_new(_(list_defs[i].title));
    gtk_widget_set_tooltip_text(title, _(list_defs[i].tooltip));
    gtk_box_pack_start(GTK_BOX(half), title, FALSE, FALSE, 0);

    // Auto-grows to its content (the window scrolls) up to a user-set, persisted height.
    GtkWidget *wrapper = dt_ui_scroll_wrap(list->treeview, 90, list_defs[i].height_key,
                                           DT_UI_RESIZE_DYNAMIC);

    /* A default floor for an EMPTY list, where the treeview's own content-derived minimum is
     * near zero: without this, an empty paned half could be dragged down to nothing. It never
     * shrinks a list below its actual content, though -- gtk_scrolled_window_set_min_content_width()
     * only raises the reported minimum when it is the larger of the two; a longer name simply
     * wins on its own, per the name renderer's own comment above. */
    GtkWidget *scrolled = dt_ui_scroll_wrap_get_scrolled_window(wrapper);
    if(GTK_IS_SCROLLED_WINDOW(scrolled))
      gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), DT_PIXEL_APPLY_DPI(190));

    gtk_box_pack_start(GTK_BOX(half), wrapper, TRUE, TRUE, 0);

    /* Measured offscreen: with both sides resize=TRUE, GtkPaned splits any width the window
     * gains between the two -- the divider drifts to keep an even split, rather than staying
     * where it was left. The inventory (pack1) is pinned instead (resize=FALSE) so growing the
     * window hands all of the new width to the module list (pack2, resize=TRUE) and the divider
     * itself does not move; a manual drag still repositions it normally either way. */
    if(i == 0)
      gtk_paned_pack1(GTK_PANED(lists_paned), half, FALSE, FALSE);
    else
      gtk_paned_pack2(GTK_PANED(lists_paned), half, TRUE, FALSE);
  }

  gtk_box_pack_start(GTK_BOX(shape_manager_container), lists_paned, TRUE, TRUE, 0);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, G_CALLBACK(_shape_manager_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                                  G_CALLBACK(_shape_manager_view_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_MODULE_MOVED,
                                  G_CALLBACK(_shape_manager_modules_moved), self);

  // Modules are loaded before any view is entered: start out empty and greyed out.
  _shape_manager_recreate_list(self);

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
    if(!IS_NULL_PTR(d->ic_add)) g_object_unref(d->ic_add);
    if(!IS_NULL_PTR(d->ic_add_off)) g_object_unref(d->ic_add_off);
    if(!IS_NULL_PTR(d->ic_assign)) g_object_unref(d->ic_assign);
    if(!IS_NULL_PTR(d->ic_inverse)) g_object_unref(d->ic_inverse);
    if(!IS_NULL_PTR(d->ic_union)) g_object_unref(d->ic_union);
    if(!IS_NULL_PTR(d->ic_intersection)) g_object_unref(d->ic_intersection);
    if(!IS_NULL_PTR(d->ic_difference)) g_object_unref(d->ic_difference);
    if(!IS_NULL_PTR(d->ic_exclusion)) g_object_unref(d->ic_exclusion);

    d->ic_used = NULL;
    d->ic_add = NULL;
    d->ic_add_off = NULL;
    d->ic_assign = NULL;
    d->ic_inverse = NULL;
    d->ic_union = NULL;
    d->ic_intersection = NULL;
    d->ic_difference = NULL;
    d->ic_exclusion = NULL;
  }

  dt_free(self->data);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_shape_manager_handler_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_shape_manager_view_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_shape_manager_modules_moved), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
