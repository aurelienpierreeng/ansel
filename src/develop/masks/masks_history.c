/*
    This file is part of Ansel
    Copyright (C) 2026 Guillaume Stutin.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "develop/masks/masks_history.h"
#include "develop/masks.h"
#include "develop/masks_gui.h"

#include "system/atomic.h"
#include "develop/develop.h"

dt_masks_form_t *dt_masks_form_ref(dt_masks_form_t *form)
{
  if(IS_NULL_PTR(form)) return NULL;
  dt_atomic_add_int(&form->refcount, 1);
  return form;
}

void dt_masks_form_unref(dt_masks_form_t *form)
{
  if(IS_NULL_PTR(form)) return;
  // dt_atomic_sub_int returns the value *before* the decrement.
  if(dt_atomic_sub_int(&form->refcount, 1) == 1) dt_masks_free_form(form);
}

dt_masks_form_t *dt_masks_cow_touch(dt_develop_t *dev, dt_masks_form_t *form)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(form)) return form;
  if(dt_atomic_get_int(&form->refcount) <= 1) return form;

  dt_pthread_rwlock_wrlock(&dev->masks_mutex);

  GList *node = g_list_find(dev->forms, form);
  if(IS_NULL_PTR(node))
  {
    // Not (yet) a member of dev->forms: e.g. a transient in-creation shape.
    // Nothing to splice, leave it alone.
    dt_pthread_rwlock_unlock(&dev->masks_mutex);
    return form;
  }

  dt_masks_form_t *clone = dt_masks_dup_masks_form(form);
  node->data = clone;

  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  if(!IS_NULL_PTR(dev->form_gui) && dev->form_gui->form_visible == form)
    dev->form_gui->form_visible = clone;

  dt_masks_form_unref(form);

  return clone;
}

void dt_masks_replace_current_forms(dt_develop_t *dev, GList *forms)
{
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);

  GList *forms_shared = g_list_copy(forms);
  for(GList *node = forms_shared; node; node = g_list_next(node))
    dt_masks_form_ref((dt_masks_form_t *)node->data);

  GList *old_forms = dev->forms;
  dev->forms = forms_shared;

  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  // form_visible is a raw pointer cached outside dev->forms (dt_masks_form_gui_t::form_visible).
  // If it pointed at an object we're about to release below, re-resolve it by formid in the new
  // dev->forms (or drop it if that formid is gone) before old_forms is freed -- otherwise the
  // very next mask interaction (mouse move, scroll...) dereferences freed memory.
  if(!IS_NULL_PTR(dev->form_gui) && !IS_NULL_PTR(dev->form_gui->form_visible)
     && g_list_find(old_forms, dev->form_gui->form_visible))
  {
    const int visible_formid = dev->form_gui->form_visible->formid;
    dev->form_gui->form_visible = dt_masks_get_from_id_ext(dev->forms, visible_formid);
  }

  g_list_free_full(old_forms, (void (*)(void *))dt_masks_form_unref);

  // Don't recompute gravity_center/area for every form here: this runs on every history
  // load, undo/redo and history navigation, for every mask of every module in the image,
  // whether or not the mask editor is even open. Just invalidate -- the one GUI hit-testing
  // read site (masks.c, group member selection) recomputes lazily on first actual use.
  for(GList *form_node = dev->forms; form_node; form_node = g_list_next(form_node))
    dt_masks_form_invalidate_gravity_center((dt_masks_form_t *)form_node->data);
}

GList *dt_masks_snapshot_current_forms(dt_develop_t *dev, gboolean reset_changed)
{
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);

  GList *forms_snapshot = g_list_copy(dev->forms);
  for(GList *node = forms_snapshot; node; node = g_list_next(node))
    dt_masks_form_ref((dt_masks_form_t *)node->data);

  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  if(reset_changed) dev->forms_changed = FALSE;
  return forms_snapshot;
}

void dt_masks_forms_snapshot_release(GList **snapshot)
{
  if(IS_NULL_PTR(*snapshot)) return;
  g_list_free_full(*snapshot, (void (*)(void *))dt_masks_form_unref);
  *snapshot = NULL;
}

gboolean dt_masks_forms_hold(GList *forms, const int formid)
{
  return !IS_NULL_PTR(dt_masks_get_from_id_ext(forms, formid));
}

GList *dt_masks_history_forms_at(GList *history, const dt_dev_history_item_t *item)
{
  GList *forms = NULL;
  for(GList *node = history; node; node = g_list_next(node))
  {
    const dt_dev_history_item_t *hist = (const dt_dev_history_item_t *)node->data;
    if(hist->forms) forms = hist->forms;
    if(hist == item) return forms;
  }
  return NULL;
}

void dt_masks_history_carry_new_forms(dt_develop_t *dev, GList *forms_before, GList *history)
{
  GList *new_forms = NULL;
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);
  for(GList *node = dev->forms; node; node = g_list_next(node))
    if(IS_NULL_PTR(g_list_find(forms_before, node->data))) new_forms = g_list_prepend(new_forms, node->data);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
  new_forms = g_list_reverse(new_forms);

  for(GList *item = history; item && new_forms; item = g_list_next(item))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)item->data;
    if(IS_NULL_PTR(hist->forms)) continue;

    GHashTable *ids = g_hash_table_new(g_direct_hash, g_direct_equal);
    for(GList *node = hist->forms; node; node = g_list_next(node))
      g_hash_table_add(ids, GINT_TO_POINTER(((dt_masks_form_t *)node->data)->formid));

    GList *carried = NULL;
    for(GList *node = new_forms; node; node = g_list_next(node))
    {
      dt_masks_form_t *form = (dt_masks_form_t *)node->data;
      if(!g_hash_table_contains(ids, GINT_TO_POINTER(form->formid)))
        carried = g_list_prepend(carried, dt_masks_form_ref(form));
    }
    hist->forms = g_list_concat(hist->forms, g_list_reverse(carried));
    g_hash_table_destroy(ids);
  }
  g_list_free(new_forms);
}

void dt_masks_release_all_forms(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;

  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  /* Release both claims, never free unconditionally: a form held by dev->forms AND dev->allforms
   * only reaches refcount 0 on the second unref. */
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_form_unref);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_form_unref);
  dev->allforms = NULL;
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
}
