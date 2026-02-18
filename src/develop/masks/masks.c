/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016, 2019-2021 Aldric Renaudin.
    Copyright (C) 2013, 2016-2021 Pascal Obry.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2016, 2019-2020 Ulrich Pegelow.
    Copyright (C) 2016, 2018 Matthieu Moy.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2017 Peter Budai.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2019-2020 Diederik Ter Rahe.
    Copyright (C) 2019 Jacopo Guderzo.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 darkelectron.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Philipp Lutz.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
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
#include "common/debug.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "common/undo.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"

dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *form)
{
  if (!form) return NULL;

  dt_masks_form_t *new_form = malloc(sizeof(struct dt_masks_form_t));
  memcpy(new_form, form, sizeof(struct dt_masks_form_t));

  // then duplicate the GList *points

  GList* newpoints = NULL;

  if (form->points)
  {
    int size_item = (form->functions) ? form->functions->point_struct_size : 0;

    if (size_item != 0)
    {
      for (GList *pt = form->points; pt; pt = g_list_next(pt))
      {
        void *item = malloc(size_item);
        memcpy(item, pt->data, size_item);
        newpoints = g_list_prepend(newpoints, item);
      }
    }
  }
  new_form->points = g_list_reverse(newpoints);  // list was built in reverse order, so un-reverse it

  return new_form;
}

static void *_dup_masks_form_cb(const void *formdata, gpointer user_data)
{
  // duplicate the main form struct
  dt_masks_form_t *form = (dt_masks_form_t *)formdata;
  dt_masks_form_t *uform = (dt_masks_form_t *)user_data;
  const dt_masks_form_t *f = uform == NULL || form->formid != uform->formid ? form : uform;
  return (void *)dt_masks_dup_masks_form(f);
}

// duplicate the list of forms, replace item in the list with form with the same formid
GList *dt_masks_dup_forms_deep(GList *forms, dt_masks_form_t *form)
{
  return (GList *)g_list_copy_deep(forms, _dup_masks_form_cb, (gpointer)form);
}

static int _get_opacity(dt_masks_form_gui_t *gui, const dt_masks_form_t *form)
{
  const dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
  const dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!sel) return 0;
  const int formid = sel->formid;

  // look for opacity
  const dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, fpt->parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return 0;

  int opacity = 0;
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    const dt_masks_form_group_t *form_pt = (dt_masks_form_group_t *)fpts->data;
    if(form_pt->formid == formid)
    {
      opacity = form_pt->opacity * 100;
      break;
    }
  }

  return opacity;
}

static void _set_hinter_message(dt_masks_form_gui_t *gui, const dt_masks_form_t *form)
{
  char msg[256] = "";

  int ftype = form->type;

  int opacity = 100;

  const dt_masks_form_t *sel = form;
  if((ftype & DT_MASKS_GROUP) && (gui->group_selected >= 0))
  {
    // we get the selected form
    const dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
    sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return;

    opacity = _get_opacity(gui, form);
  }
  else
  {
    opacity = (int)(dt_conf_get_float("plugins/darkroom/masks/opacity") * 100);
  }

  if(sel->functions && sel->functions->set_hint_message)
  {
    sel->functions->set_hint_message(gui, form, opacity, msg, sizeof(msg));
  }

  dt_control_hinter_message(darktable.control, msg);
}

void dt_masks_init_form_gui(dt_masks_form_gui_t *gui)
{
  memset(gui, 0, sizeof(dt_masks_form_gui_t));

  gui->pos[0] = gui->pos[1] = -1.0f;
  gui->mouse_leaved_center = TRUE;
  gui->pos_source[0] = gui->pos_source[1] = -1.0f;
  gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  gui->form_selected = FALSE;
}

void dt_masks_soft_reset_form_gui(dt_masks_form_gui_t *gui)
{
  // Note: we have an hard reset function below that frees all buffers and such
  gui->source_selected = FALSE;
  gui->handle_selected = -1;
  gui->node_selected = -1;
  gui->seg_selected = -1;
  gui->handle_border_selected = -1;
  gui->group_selected = -1;
  gui->group_selected = -1;
  gui->delta[0] = gui->delta[1] = 0.0f;
  gui->form_selected = gui->border_selected = gui->form_dragging = gui->form_rotating = FALSE;
  gui->pivot_selected = FALSE;
  gui->handle_border_selected = gui->seg_selected = gui->node_selected = gui->handle_selected = -1;
  gui->handle_border_dragging = gui->seg_dragging = gui->handle_dragging = gui->node_dragging = -1;
}

void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index, dt_iop_module_t *module)
{
  const int npoints = g_list_length(gui->points);
  if(npoints == index)
  {
    dt_masks_form_gui_points_t *gpt2
        = (dt_masks_form_gui_points_t *)calloc(1, sizeof(dt_masks_form_gui_points_t));
    gui->points = g_list_append(gui->points, gpt2);
  }
  else if(npoints < index)
    return;

  dt_masks_gui_form_remove(form, gui, index);

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(dt_masks_get_points_border(darktable.develop, form, &gpt->points, &gpt->points_count, &gpt->border,
                                &gpt->border_count, 0, NULL) == 0)
  {
    if(form->type & DT_MASKS_CLONE)
    {
      if(dt_masks_get_points_border(darktable.develop, form, &gpt->source, &gpt->source_count, NULL, NULL, TRUE, module) != 0)
        return;
    }
    gui->pipe_hash = darktable.develop->preview_pipe->backbuf.hash;
    gui->formid = form->formid;
  }
}

void dt_masks_form_gui_points_free(gpointer data)
{
  if(!data) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)data;

  dt_pixelpipe_cache_free_align(gpt->points);
  dt_pixelpipe_cache_free_align(gpt->border);
  dt_pixelpipe_cache_free_align(gpt->source);
  free(gpt);
}

void dt_masks_remove_node(struct dt_iop_module_t *module, dt_masks_form_t *form, int parentid,
                          dt_masks_form_gui_t *gui, int index, int node_index)
{
  if(!form || !form->points) return;
  dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)g_list_nth_data(form->points, node_index);
  if(!node) return;
  form->points = g_list_remove(form->points, node);
  free(node);
  gui->node_selected = -1;
  gui->node_edited = -1;
  if(form->functions && form->functions->init_ctrl_points)
    form->functions->init_ctrl_points(form);
    
  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);
}


/**
 * @brief Remove a shape from the GUI and free its resources.
 * 
 * @param module The module owning the mask
 * @param form The form to remove
 * @param parentid The parent ID of the form
 * @param gui The GUI state
 * @param index The index of the form in the group
 * 
 * @return gboolean TRUE if the form was removed, FALSE otherwise.
 */
static gboolean _masks_remove_shape(struct dt_iop_module_t *module, dt_masks_form_t *form, int parentid,
                               dt_masks_form_gui_t *gui, int index)
{
  // if the form doesn't below to a group, we don't delete it
  if(parentid <= 0) return 1;

  // we hide the form
  if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
    dt_masks_change_form_gui(NULL);
  else if(g_list_shorter_than(darktable.develop->form_visible->points, 2))
    dt_masks_change_form_gui(NULL);
  else
  {
    const int emode = gui->edit_mode;
    dt_masks_clear_form_gui(darktable.develop);
    for(GList *forms = darktable.develop->form_visible->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *guipt = (dt_masks_form_group_t *)forms->data;
      if(guipt->formid == form->formid)
      {
        darktable.develop->form_visible->points = g_list_remove(darktable.develop->form_visible->points, guipt);
        free(guipt);
        break;
      }
    }
    gui->edit_mode = emode;
  }

  // we delete or remove the shape
  // Called from node removal, if there was not enough nodes to keep the whole shape,
  // that's how this was called:
  // dt_masks_form_remove(module, NULL, form);
  // Called from shape removal, this is how it was called:
  dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
  // Not sure what difference it makes.

  return 1;
}

gboolean dt_masks_form_cancel_creation(dt_iop_module_t *module, dt_masks_form_gui_t *gui)
{
  if(gui->creation)
  {
    if(gui->guipoints)
    {
      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;
    }

    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);

    return TRUE;
  }
  return FALSE;
}

gboolean dt_masks_gui_delete(struct dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui, const int parentid)
{
  // Just clean temp mask if we are in creation mode
  if(dt_masks_form_cancel_creation(module, gui))
    return TRUE;

  // we remove the selected node (and the entire form if there is too few nodes left)
  if(((form->type & DT_MASKS_BRUSH) || (form->type & DT_MASKS_POLYGON)) && gui->node_selected >= 0)
  {
    if(g_list_shorter_than(form->points, 3))
      return _masks_remove_shape(module, form, parentid, gui, gui->group_selected);

    dt_masks_remove_node(module, form, parentid, gui, gui->group_selected, gui->node_selected);

    return TRUE;
  }
  // we remove the entire shape
  else if(parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
    return _masks_remove_shape(module, form, parentid, gui, gui->group_selected);
  
  return FALSE;
}

void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  gui->pipe_hash = gui->formid = 0;

  if(gpt)
  {
    gpt->points_count = gpt->border_count = gpt->source_count = 0;
    dt_pixelpipe_cache_free_align(gpt->points);
    gpt->points = NULL;
    dt_pixelpipe_cache_free_align(gpt->border);
    gpt->border = NULL;
    dt_pixelpipe_cache_free_align(gpt->source);
    gpt->source = NULL;
  }
}

void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, dt_iop_module_t *module)
{
  // we test if the image has changed
  if(gui->pipe_hash > 0)
  {
    if(gui->pipe_hash != darktable.develop->preview_pipe->backbuf.hash)
    {
      gui->pipe_hash = gui->formid = 0;
      g_list_free_full(gui->points, dt_masks_form_gui_points_free);
      gui->points = NULL;
    }
  }

  // we create the form if needed
  if(gui->pipe_hash == 0)
  {
    if(form->type & DT_MASKS_GROUP)
    {
      int pos = 0;
      for(GList *fpts = form->points; fpts;  fpts = g_list_next(fpts))
      {
        dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
        dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
        if (!sel) return;
        dt_masks_gui_form_create(sel, gui, pos, module);
        pos++;
      }
    }
    else
      dt_masks_gui_form_create(form, gui, 0, module);
  }
}

static void _check_id(dt_masks_form_t *form)
{
  int nid = 100;
  for(GList *forms = darktable.develop->forms; forms; )
  {
    dt_masks_form_t *ff = (dt_masks_form_t *)forms->data;
    if(ff->formid == form->formid)
    {
      form->formid = nid++;
      forms = darktable.develop->forms; // jump back to start of list
    }
    else
      forms = g_list_next(forms); // advance to next form
  }
}

static void _set_group_name_from_module(dt_iop_module_t *module, dt_masks_form_t *grp)
{
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(grp->name, sizeof(grp->name), "grp %s", module_label);
  g_free(module_label);
}

static dt_masks_form_t *_group_create(dt_develop_t *dev, dt_iop_module_t *module, dt_masks_type_t type)
{
  dt_masks_form_t* grp = dt_masks_create(type);
  _set_group_name_from_module(module, grp);
  _check_id(grp);
  dt_masks_append_form(dev, grp);
  module->blend_params->mask_id = grp->formid;
  return grp;
}

static dt_masks_form_t *_group_from_module(dt_develop_t *dev, dt_iop_module_t *module)
{
  return dt_masks_get_from_id(dev, module->blend_params->mask_id);
}

void dt_masks_append_form(dt_develop_t *dev, dt_masks_form_t *form)
{
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  dev->forms = g_list_append(dev->forms, form);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
}

void dt_masks_remove_form(dt_develop_t *dev, dt_masks_form_t *form)
{
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  dev->forms = g_list_remove(dev->forms, form);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
}

void dt_masks_gui_form_save_creation(dt_develop_t *dev, dt_iop_module_t *module, dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui)
{
  // we check if the id is already registered
  _check_id(form);

  if(gui) gui->creation = FALSE;

  // mask nb will be at least the length of the list
  guint nb = 0;

  // count only the same forms to have a clean numbering
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)l->data;
    if(f->type == form->type) nb++;
  }
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  gboolean exist = FALSE;

  // check that we do not have duplicate, in case some masks have been
  // removed we can have hole and so nb could already exists.
  do
  {
    exist = FALSE;
    nb++;

    if(form->functions && form->functions->set_form_name)
      form->functions->set_form_name(form, nb);

    dt_pthread_rwlock_rdlock(&dev->masks_mutex);
    for(GList *l = dev->forms; l; l = g_list_next(l))
    {
      dt_masks_form_t *f = (dt_masks_form_t *)l->data;
      if(!strcmp(f->name, form->name))
      {
        exist = TRUE;
        break;
      }
    }
    dt_pthread_rwlock_unlock(&dev->masks_mutex);

  } while(exist);

  dt_masks_append_form(dev, form);

  if(module)
  {
    // is there already a masks group for this module ?
    dt_masks_form_t *grp = _group_from_module(dev, module);
    if(!grp)
    {
      // we create a new group
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        grp = _group_create(dev, module, DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        grp = _group_create(dev, module, DT_MASKS_GROUP);
    }
    // we add the form in this group
    dt_masks_form_group_t *grpt = malloc(sizeof(dt_masks_form_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grp->formid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(grp->points) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    grp->points = g_list_append(grp->points, grpt);
    // we update module gui
    if(gui) dt_masks_iop_update(module);
  }

  // show the form if needed
  if(gui) dev->form_gui->formid = form->formid;
}

int dt_masks_form_duplicate(dt_develop_t *dev, int formid)
{
  // we create a new empty form
  dt_masks_form_t *fbase = dt_masks_get_from_id(dev, formid);
  if(!fbase) return -1;
  dt_masks_form_t *fdest = dt_masks_create(fbase->type);
  _check_id(fdest);

  // we copy the base values
  fdest->source[0] = fbase->source[0];
  fdest->source[1] = fbase->source[1];
  fdest->version = fbase->version;
  snprintf(fdest->name, sizeof(fdest->name), _("copy of %s"), fbase->name);

  dt_masks_append_form(dev, fdest);

  // we copy all the points
  if(fbase->functions)
    fbase->functions->duplicate_points(dev, fbase, fdest);

  // and we return its id
  return fdest->formid;
}

int dt_masks_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                               float **border, int *border_count, int source, dt_iop_module_t *module)
{
  if(form->functions && form->functions->get_points_border)
    return form->functions->get_points_border(dev, form, points, points_count, border, border_count, source, module);
  return 1;
}

int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      int *width, int *height, int *posx, int *posy)
{
  if(form->functions && form->functions->get_area)
    return form->functions->get_area(module, piece, form, width, height, posx, posy);
  return 1;
}

int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy)
{
  *width = *height = *posx = *posy = 0;

  // must be a clone form
  if(form->type & DT_MASKS_CLONE)
  {
    if(form->functions && form->functions->get_source_area)
      return form->functions->get_source_area(module, piece, form, width, height, posx, posy);
  }
  return 1;
}

int dt_masks_version(void)
{
  return DEVELOP_MASKS_VERSION;
}

static int dt_masks_legacy_params_v1_to_v2(dt_develop_t *dev, void *params)
{
  /*
   * difference: before v2 images were originally rotated on load, and then
   * maybe in flip iop
   * after v2: images are only rotated in flip iop.
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  const dt_image_orientation_t ori = dt_image_orientation(&dev->image_storage);

  if(ori == ORIENTATION_NONE)
  {
    // image is not rotated, we're fine!
    m->version = 2;
    return 0;
  }
  else
  {
    if(dev->iop == NULL) return 1;

    const char *opname = "flip";
    dt_iop_module_t *module = NULL;

    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *find_op = (dt_iop_module_t *)modules->data;
      if(!strcmp(find_op->op, opname))
      {
        module = find_op;
        break;
      }
    }

    if(module == NULL) return 1;

    dt_dev_pixelpipe_iop_t piece = { 0 };

    module->init_pipe(module, NULL, &piece);
    module->commit_params(module, module->default_params, NULL, &piece);

    piece.buf_in.width = 1;
    piece.buf_in.height = 1;

    GList *p = m->points;

    if(!p) return 1;

    if(m->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)p->data;
      if(!circle) return 1;
      module->distort_backtransform(module, &piece, circle->center, 1);
    }
    else if(m->type & DT_MASKS_POLYGON)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_node_polygon_t *polygone = (dt_masks_node_polygon_t *)p->data;
        if(!polygone) return 1;
        module->distort_backtransform(module, &piece, polygone->node, 1);
        module->distort_backtransform(module, &piece, polygone->ctrl1, 1);
        module->distort_backtransform(module, &piece, polygone->ctrl2, 1);
      }
    }
    else if(m->type & DT_MASKS_GRADIENT)
    { // TODO: new ones have wrong rotation.
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)p->data;
      if(!gradient) return 1;
      module->distort_backtransform(module, &piece, gradient->center, 1);

      if(ori == ORIENTATION_ROTATE_180_DEG)
        gradient->rotation -= 180.0f;
      else if(ori == ORIENTATION_ROTATE_CCW_90_DEG)
        gradient->rotation -= 90.0f;
      else if(ori == ORIENTATION_ROTATE_CW_90_DEG)
        gradient->rotation -= -90.0f;
    }
    else if(m->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)p->data;
      module->distort_backtransform(module, &piece, ellipse->center, 1);

      if(ori & ORIENTATION_SWAP_XY)
      {
        const float y = ellipse->radius[0];
        ellipse->radius[0] = ellipse->radius[1];
        ellipse->radius[1] = y;
      }
    }
    else if(m->type & DT_MASKS_BRUSH)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_node_brush_t *brush = (dt_masks_node_brush_t *)p->data;
        if(!brush) return 1;
        module->distort_backtransform(module, &piece, brush->node, 1);
        module->distort_backtransform(module, &piece, brush->ctrl1, 1);
        module->distort_backtransform(module, &piece, brush->ctrl2, 1);
      }
    }

    if(m->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      module->distort_backtransform(module, &piece, m->source, 1);
    }

    m->version = 2;

    return 0;
  }
}

static void dt_masks_legacy_params_v2_to_v3_transform(const dt_image_t *img, float *points)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cx = (float)img->crop_x, cy = (float)img->crop_y;

  const float cw = (float)(img->width - img->crop_x - img->crop_width),
              ch = (float)(img->height - img->crop_y - img->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by image original cropped dimensions
   * 2. un-crop them by adding top-left crop coordinates
   * 3. normalize them by the image fully uncropped dimensions
   */
  points[0] = ((points[0] * cw) + cx) / w;
  points[1] = ((points[1] * ch) + cy) / h;
}

static void dt_masks_legacy_params_v2_to_v3_transform_only_rescale(const dt_image_t *img, float *points,
                                                                   size_t points_count)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cw = (float)(img->width - img->crop_x - img->crop_width),
              ch = (float)(img->height - img->crop_y - img->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by minimal of image original cropped dimensions
   * 2. normalize them by the minimal of image fully uncropped dimensions
   */
  for(size_t i = 0; i < points_count; i++) points[i] = ((points[i] * MIN(cw, ch))) / MIN(w, h);
}

static int dt_masks_legacy_params_v2_to_v3(dt_develop_t *dev, void *params)
{
  /*
   * difference: before v3 images were originally cropped on load
   * after v3: images are cropped in rawprepare iop.
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  const dt_image_t *img = &(dev->image_storage);

  if(img->crop_x == 0 && img->crop_y == 0 && img->crop_width == 0 && img->crop_height == 0)
  {
    // image has no "raw cropping", we're fine!
    m->version = 3;
    return 0;
  }
  else
  {
    GList *p = m->points;

    if(!p) return 1;

    if(m->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)p->data;
      if(!circle) return 1;
      dt_masks_legacy_params_v2_to_v3_transform(img, circle->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &circle->radius, 1);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &circle->border, 1);
    }
    else if(m->type & DT_MASKS_POLYGON)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_node_polygon_t *polygone = (dt_masks_node_polygon_t *)p->data;
        if(!polygone) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(img, polygone->node);
        dt_masks_legacy_params_v2_to_v3_transform(img, polygone->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, polygone->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, polygone->border, 2);
      }
    }
    else if(m->type & DT_MASKS_GRADIENT)
    {
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)p->data;
      dt_masks_legacy_params_v2_to_v3_transform(img, gradient->center);
    }
    else if(m->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)p->data;
      dt_masks_legacy_params_v2_to_v3_transform(img, ellipse->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, ellipse->radius, 2);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &ellipse->border, 1);
    }
    else if(m->type & DT_MASKS_BRUSH)
    {
      for(; p;  p = g_list_next(p))
      {
        dt_masks_node_brush_t *brush = (dt_masks_node_brush_t *)p->data;
        if(!brush) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->node);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, brush->border, 2);
      }
    }

    if(m->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      dt_masks_legacy_params_v2_to_v3_transform(img, m->source);
    }

    m->version = 3;

    return 0;
  }
}

static int dt_masks_legacy_params_v3_to_v4(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting ellipse
   * up to v3: only equidistant feathering
   * after v4: choice between equidistant and proportional feathering
   * type of feathering is defined in new flags parameter
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)p->data;
    ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  }

  m->version = 4;

  return 0;
}


static int dt_masks_legacy_params_v4_to_v5(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting gradient
   * up to v4: only linear gradient (relative to input image)
   * after v5: curved gradients
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)p->data;
    gradient->curvature = 0.0f;
  }

  m->version = 5;

  return 0;
}

static int dt_masks_legacy_params_v5_to_v6(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting gradient
   * up to v5: linear transition
   * after v5: linear or sigmoidal transition
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)p->data;
    gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;
  }

  m->version = 6;

  return 0;
}


int dt_masks_legacy_params(dt_develop_t *dev, void *params, const int old_version, const int new_version)
{
  int res = 1;
#if 0 // we should not need this any longer
  if(old_version == 1 && new_version == 2)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
  }
#endif

  if(old_version == 1 && new_version == 6)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
    if(!res) res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 2 && new_version == 6)
  {
    res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 3 && new_version == 6)
  {
    res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 4 && new_version == 6)
  {
    res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 5 && new_version == 6)
  {
    res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }

  return res;
}

static int form_id = 0;

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *form = (dt_masks_form_t *)calloc(1, sizeof(dt_masks_form_t));
  if(!form) return NULL;

  form->type = type;
  form->version = dt_masks_version();
  form->formid = time(NULL) + form_id++;

  if (type & DT_MASKS_CIRCLE)
    form->functions = &dt_masks_functions_circle;
  else if (type & DT_MASKS_ELLIPSE)
    form->functions = &dt_masks_functions_ellipse;
  else if (type & DT_MASKS_BRUSH)
    form->functions = &dt_masks_functions_brush;
  else if (type & DT_MASKS_POLYGON)
    form->functions = &dt_masks_functions_polygon;
  else if (type & DT_MASKS_GRADIENT)
    form->functions = &dt_masks_functions_gradient;
  else if (type & DT_MASKS_GROUP)
    form->functions = &dt_masks_functions_group;

  if (form->functions && form->functions->sanitize_config)
    form->functions->sanitize_config(type);

  return form;
}

dt_masks_form_t *dt_masks_create_ext(dt_masks_type_t type)
{
  dt_pthread_rwlock_wrlock(&darktable.develop->masks_mutex);
  dt_masks_form_t *form = dt_masks_create(type);

  // all forms created here are registered in darktable.develop->allforms for later cleanup
  if(form)
    darktable.develop->allforms = g_list_append(darktable.develop->allforms, form);

  dt_pthread_rwlock_unlock(&darktable.develop->masks_mutex);

  return form;
}

void dt_masks_replace_current_forms(dt_develop_t *dev, GList *forms)
{
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  GList *forms_tmp = dt_masks_dup_forms_deep(forms, NULL);

  while(dev->forms)
  {
    darktable.develop->allforms = g_list_append(darktable.develop->allforms, dev->forms->data);
    dev->forms = g_list_delete_link(dev->forms, dev->forms);
  }

  dev->forms = forms_tmp;
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
}

dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, int id)
{
  for(; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->formid == id) return form;
  }
  return NULL;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id)
{
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);
  dt_masks_form_t *result = dt_masks_get_from_id_ext(dev->forms, id);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);
  return result;
}

void dt_masks_read_masks_history(dt_develop_t *dev, const int32_t imgid)
{
  dt_dev_history_item_t *hist_item = NULL;
  dt_dev_history_item_t *hist_item_last = NULL;
  int num_prev = -1;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid, formid, form, name, version, points, points_count, source, num "
      "FROM main.masks_history WHERE imgid = ?1 ORDER BY num",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-formid, 2-form_type, 3-name, 4-version, 5-points, 6-points_count, 7-source, 8-num

    // we get the values

    const int formid = sqlite3_column_int(stmt, 1);
    const int num = sqlite3_column_int(stmt, 8);
    const dt_masks_type_t type = sqlite3_column_int(stmt, 2);
    dt_masks_form_t *form = dt_masks_create(type);
    form->formid = formid;
    const char *name = (const char *)sqlite3_column_text(stmt, 3);
    g_strlcpy(form->name, name, sizeof(form->name));
    form->version = sqlite3_column_int(stmt, 4);
    form->points = NULL;
    const int nb_points = sqlite3_column_int(stmt, 6);
    memcpy(form->source, sqlite3_column_blob(stmt, 7), sizeof(float) * 2);

    // and now we "read" the blob
    if(form->functions)
    {
      const char *const ptbuf = (char *)sqlite3_column_blob(stmt, 5);
      const size_t point_size = form->functions->point_struct_size;
      for(int i = 0; i < nb_points; i++)
      {
        char *point = (char *)malloc(point_size);
        memcpy(point, ptbuf + i*point_size, point_size);
        form->points = g_list_append(form->points, point);
      }
    }

    if(form->version != dt_masks_version())
    {
      if(dt_masks_legacy_params(dev, form, form->version, dt_masks_version()))
      {
        const char *fname = dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;
        if(fname > dev->image_storage.filename) fname++;

        fprintf(stderr,
                "[_dev_read_masks_history] %s (imgid `%i'): mask version mismatch: history is %d, dt %d.\n",
                fname, imgid, form->version, dt_masks_version());
        dt_control_log(_("%s: mask version mismatch: %d != %d"), fname, dt_masks_version(), form->version);

        continue;
      }
    }

    // if this is a new history entry let's find it
    if(num_prev != num)
    {
      hist_item = NULL;
      for(GList *history = dev->history; history; history = g_list_next(history))
      {
        dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
        if(hitem->num == num)
        {
          hist_item = hitem;
          break;
        }
      }
      num_prev = num;
    }
    // add the form to the history entry
    if(hist_item)
    {
      hist_item->forms = g_list_append(hist_item->forms, form);
    }
    else
      fprintf(stderr,
              "[_dev_read_masks_history] can't find history entry %i while adding mask %s(%i)\n",
              num, form->name, formid);

    if(num < dt_dev_get_history_end(dev)) hist_item_last = hist_item;
  }
  sqlite3_finalize(stmt);

  // and we update the current forms snapshot
  dt_masks_replace_current_forms(dev, (hist_item_last) ? hist_item_last->forms : NULL);
}

void dt_masks_write_masks_history_item(const int32_t imgid, const int num, dt_masks_form_t *form)
{
  sqlite3_stmt *stmt;

  dt_print(DT_DEBUG_HISTORY, "[dt_masks_write_masks_history_item] writing mask %s of type %i for image %i\n",
           form->name, form->type, imgid);

  // write the form into the database
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.masks_history (imgid, num, formid, form, name, "
                              "version, points, points_count,source) VALUES "
                              "(?1, ?9, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form->type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, form->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, form->source, 2 * sizeof(float), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, form->version);
  if(form->functions)
  {
    const size_t point_size = form->functions->point_struct_size;
    const guint nb = g_list_length(form->points);
    char *const restrict ptbuf = (char *)malloc(nb * point_size);
    int pos = 0;
    for (GList *points = form->points; points; points = g_list_next(points))
    {
      memcpy(ptbuf + pos, points->data, point_size);
      pos += point_size;
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb * point_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(ptbuf);
  }
}

void dt_masks_free_form(dt_masks_form_t *form)
{
  if(!form) return;
  g_list_free_full(form->points, free);
  form->points = NULL;
  free(form);
}

int dt_masks_events_mouse_leave(struct dt_iop_module_t *module)
{
  if(darktable.develop->form_gui)
  {
    dt_masks_form_gui_t *gui = darktable.develop->form_gui;
    gui->mouse_leaved_center = TRUE;
  }
  return 0;
}

int dt_masks_events_mouse_enter(struct dt_iop_module_t *module)
{
  if(darktable.develop->form_gui)
  {
    dt_masks_form_gui_t *gui = darktable.develop->form_gui;
    gui->mouse_leaved_center = FALSE;
  }
  return 0;
}

static void _set_cursor_shape(dt_masks_form_gui_t *gui)
{
  if(!gui) return;

  // circular arrows
  if(gui->pivot_selected)
    dt_control_set_cursor(GDK_EXCHANGE);
  // pointing hand
  else if(gui->creation_closing_form)
    dt_control_set_cursor(GDK_HAND2);

  /*else if(gui->handle_dragging >= 0)
    dt_control_set_cursor(GDK_HAND1);*/

  // crosshair
  else if(!gui->creation && (((gui->form_selected || gui->seg_selected >= 0) && gui->node_edited == -1)
                    || gui->handle_selected >= 0 || gui->handle_border_selected >= 0
                    || gui->node_selected >= 0))
    dt_control_set_cursor(GDK_FLEUR);
}

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure, int which)
{
  // record mouse position even if there are no masks visible
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  dt_masks_form_t *form = darktable.develop->form_visible;
  const float scale = darktable.develop->natural_scale;

  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_retrieve_full_pos(darktable.develop, x, y, &pzx, &pzy);
  if(gui)
  {
    // This assume that if this event is generated, the mouse is over the center window
    gui->mouse_leaved_center = FALSE;
    gui->pos[0] = pzx * darktable.develop->preview_width;
    gui->pos[1] = pzy * darktable.develop->preview_height;
    // unscale
    gui->pos[0] /= scale;
    gui->pos[1] /= scale;
  }

  // do not process if no forms visible
  if(!form) return 0;

  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int rep = 0;
  if(form->functions)
    rep = form->functions->mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);

  if(gui)
  {
    _set_hinter_message(gui, form);
    _set_cursor_shape(gui);
  }
  return rep;
}

int dt_masks_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                    uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_retrieve_full_pos(darktable.develop, x, y, &pzx, &pzy);

  int ret = 0;
  if(form->functions)
    ret = form->functions->button_released(module, pzx, pzy, which, state, form, 0, gui, 0);

  if(darktable.develop->mask_form_selected_id)
    dt_dev_masks_selection_change(darktable.develop, module,
                                  darktable.develop->mask_form_selected_id, FALSE);

  // DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_SELECTION_CHANGED, NULL, NULL);
  if(gui)
  {
    _set_hinter_message(gui, form);
    _set_cursor_shape(gui);
  }

  return ret;
}

static void _masks_gui_remove_form_callback(GtkWidget *menu, struct dt_masks_form_gui_t *gui)
{
  if(!gui) return;
  dt_masks_form_t *forms = darktable.develop->form_visible;
  if(!forms) return;


  if(gui->group_selected >= 0)
  {
    // Delete shape from current group
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(forms->points, gui->group_selected);
    if(!fpt) return;
    dt_iop_module_t *module = darktable.develop->gui_module;
    if(!module) return;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel)
      _masks_remove_shape(module, sel, fpt->parentid, gui, gui->group_selected);

    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
  }

}

void _masks_gui_delete_node_callback(GtkWidget *menu, struct dt_masks_form_gui_t *gui)
{
  if(!gui) return;
  dt_masks_form_t *forms = darktable.develop->form_visible;
  if(!forms) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  if(gui->creation)
  {
    // Minimum points to create a polygon
    if(gui->node_dragging < 1)
    {
      dt_masks_form_cancel_creation(module, gui);
      return;
    }
    dt_masks_form_t *sel = darktable.develop->form_visible;
    if(sel)
      dt_masks_remove_node(module, sel, 0, gui, 0, gui->node_dragging);
    gui->node_dragging -= 1; 
  }
  else if(gui->group_selected >= 0)
  {
    // Delete shape from current group

    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(forms->points, gui->group_selected);
    if(!fpt) return;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel)
      dt_masks_remove_node(module, sel, fpt->parentid, gui, gui->group_selected, gui->node_selected);
    
    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
  }
}

static void _masks_gui_cancel_creation_callback(GtkWidget *menu, struct dt_masks_form_gui_t *gui)
{
  dt_iop_module_t *module = darktable.develop->gui_module;
  dt_masks_form_cancel_creation(module, gui);
}


/** Contextual menu */

static gboolean _brush_menu_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_masks_menu_icon_data_t *data = (dt_masks_menu_icon_data_t *)user_data;
  if(!data || data->shape == DT_MASKS_MENU_ICON_NONE) return FALSE;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GdkRGBA color;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &color);
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
  cairo_set_line_width(cr, 1.2);

  GtkAllocation alloc;
  gtk_widget_get_allocation(widget, &alloc);
  const double pad = 1.0;
  const double w = MAX(0.0, (double)alloc.width - 2.0 * pad);
  const double h = MAX(0.0, (double)alloc.height - 2.0 * pad);
  const double size = fmin(w, h);
  const double x = ((double)alloc.width - size) * 0.5;
  const double y = ((double)alloc.height - size) * 0.5;

  if(data->shape == DT_MASKS_MENU_ICON_CIRCLE)
  {
    cairo_arc(cr, x + size * 0.5, y + size * 0.5, MAX(0.0, size * 0.5 - 0.5), 0.0, 2.0 * M_PI);
    cairo_stroke(cr);
  }
  else if(data->shape == DT_MASKS_MENU_ICON_SQUARE)
  {
    cairo_rectangle(cr, x, y, size, size);
    cairo_stroke(cr);
  }

  return FALSE;
}

GtkWidget *masks_gtk_menu_item_new_with_icon(const char *label, GtkWidget *menu,
                                                 void (*activate_callback)(GtkWidget *widget, dt_masks_form_gui_t *gui),
                                                 dt_masks_form_gui_t *gui, dt_masks_menu_icon_t icon)
{
  GtkWidget *menu_item = gtk_menu_item_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *icon_widget = gtk_drawing_area_new();
  GtkWidget *label_widget = gtk_label_new(NULL);

  gtk_widget_set_size_request(icon_widget, 10, 10);
  gtk_label_set_markup(GTK_LABEL(label_widget), label);

  if(icon != DT_MASKS_MENU_ICON_NONE)
  {
    dt_masks_menu_icon_data_t *data = g_malloc0(sizeof(dt_masks_menu_icon_data_t));
    data->shape = icon;
    g_signal_connect_data(icon_widget, "draw", G_CALLBACK(_brush_menu_icon_draw), data, (GClosureNotify)g_free, 0);
  }

  gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0f);
  gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), icon_widget, FALSE, FALSE, 2);
  gtk_container_add(GTK_CONTAINER(menu_item), box);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  if(activate_callback) g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(activate_callback), gui);

  return menu_item;
}

GtkWidget *masks_gtk_menu_item_new_with_markup(const char *label, GtkWidget *menu,
                                                 void (*activate_callback)(GtkWidget *widget, dt_masks_form_gui_t *gui),
                                                 struct dt_masks_form_gui_t *gui)
{
  GtkWidget *menu_item = gtk_menu_item_new_with_label("");
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(menu_item));
  gtk_label_set_markup(GTK_LABEL(child), label);
  gtk_menu_item_set_reserve_indicator(GTK_MENU_ITEM(menu_item), FALSE);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  if(activate_callback) g_signal_connect(G_OBJECT(menu_item), "activate", G_CALLBACK(activate_callback), gui);

  return menu_item;
}

GtkWidget *dt_masks_create_menu(dt_masks_form_gui_t *gui, dt_masks_form_t *form)
{
  assert(gui);
  assert(form);
  // Always re-create the menu when we show it because we don't bother updating info during the lifetime of the mask
  GtkWidget *menu = gtk_menu_new();
  
  // Title
  gchar *form_name = NULL;
  if(form->name[0])
    form_name = g_strdup(form->name);
  else if(gui->creation)
  {
    // if no name, we are probably creating a new form, we create one based on the type
    form_name = g_strdup(_("New "));
    switch (form->type)
    {
      case DT_MASKS_CIRCLE:
        form_name = g_strconcat(form_name, _("circle"), NULL);
        break;
      case DT_MASKS_ELLIPSE:
        form_name = g_strconcat(form_name, _("ellipse"), NULL);
        break;
      case DT_MASKS_POLYGON:
        form_name = g_strconcat(form_name, _("polygon"), NULL);
        break;
      case DT_MASKS_BRUSH:
        form_name = g_strconcat(form_name, _("brush"), NULL);
        break;
      case DT_MASKS_GRADIENT:
        form_name = g_strconcat(form_name, _("gradient"), NULL);
        break;
      case DT_MASKS_GROUP:
        form_name = g_strconcat(form_name, _("group"), NULL);
        break;
      default:
        g_free(form_name); // Erase the "New " prefix
        form_name = g_strdup(_("Unknown shape"));
        break;
    }
  }
  gchar *node_index = gui->node_selected >= 0 ? g_strdup_printf(" - (%s #%d)", _("node"), gui->node_selected) : g_strdup("");
  gchar *title = g_strdup_printf("<b><big>%s%s</big></b>", form_name, node_index);
  GtkWidget *menu_item = masks_gtk_menu_item_new_with_markup(title, menu, NULL, gui);
  gtk_widget_set_sensitive(menu_item, FALSE);
  g_free(node_index);
  g_free(title);
  g_free(form_name);

  // Shape specific menu items
  if(form && form->functions && form->functions->populate_context_menu)
    form->functions->populate_context_menu(menu, form, gui);



  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  // Common menu items
  if(gui->creation)
  {
    menu_item = masks_gtk_menu_item_new_with_markup(_("Cancel"), menu, _masks_gui_cancel_creation_callback, gui);
    menu_item_set_fake_accel(menu_item, GDK_KEY_Escape, 0);
  }
  else
  {
    if(gui->node_selected >= 0)
    {
      menu_item = masks_gtk_menu_item_new_with_markup(_("Delete node"), menu, _masks_gui_delete_node_callback, gui);
      menu_item_set_fake_accel(menu_item, GDK_KEY_Delete, 0);
    }
    else
    {
      menu_item = masks_gtk_menu_item_new_with_markup(_("Remove form"), menu, _masks_gui_remove_form_callback, gui);
      menu_item_set_fake_accel(menu_item, GDK_KEY_Delete, 0);
      gtk_widget_set_sensitive(menu_item, gui->form_selected >= 0);
    }
  }

  gtk_widget_show_all(menu);
  return menu;
}

int dt_masks_events_button_pressed(struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int which, int type, uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_retrieve_full_pos(darktable.develop, x, y, &pzx, &pzy);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_SELECTION_CHANGED, NULL, NULL);

  gboolean return_val = FALSE;
  if(form->functions)
    return_val = form->functions->button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);

  if(which == 3 && !return_val)
  {
    // mouse is over a form
    if(gui && ((gui->group_selected >= 0 && gui->form_selected) || gui->creation))
    {
      GtkWidget *menu = dt_masks_create_menu(gui, form);
      gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
      return_val = TRUE;
    }
  }

  return return_val;
}

int dt_masks_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  if(!form) return 0;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  if(!gui) return 0;

  gboolean return_value = FALSE;

  if(form->functions)
    return_value = form->functions->key_pressed(module, event, form, 0, gui, 0);
  
  if(!return_value)
  {
    switch(event->keyval)
    {
      case GDK_KEY_Escape:
      {
        return_value = dt_masks_form_cancel_creation(module, gui);
        break;
      }
      case GDK_KEY_Delete:
      {
        if(gui->group_selected >= 0)
        {
          // Delete shape from current group
          dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
          if(!fpt) return 0;
          dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
          if(sel)
            return_value = dt_masks_gui_delete(module, sel, gui, fpt->parentid);
          break;
        }
      }
    }
  }

  return return_value;
}

int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state, int scrolling_delta)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_retrieve_full_pos(darktable.develop, x, y, &pzx, &pzy);

  int ret = 0;
  const gboolean incr = dt_mask_scroll_increases(up);

  // we want delta_y to be an absolute scrolling speed
  int flow = (scrolling_delta < 0) ? -scrolling_delta : scrolling_delta;

  if(form->functions)
    ret = form->functions->mouse_scrolled(module, pzx, pzy,
                                          incr ? 1 : 0, flow,
                                          state, form, 0, gui, 0, DT_MASKS_INTERACTION_UNDEF);

  if(ret && gui) _set_hinter_message(gui, form);
  return ret;
}

gboolean dt_masks_node_is_cusp(const dt_masks_form_gui_points_t *gpt, const int index)
{
  const int offset = 2;
  const float *p = &gpt->points[index * 6];
  return (p[0 + offset] == p[2 + offset]
       && p[1 + offset] == p[3 + offset]);
}

/**
 * @brief Find the best attachment point for the arrow's tip or arrow's base along shape outline
 * 
 * @param pos_x resulting x position
 * @param pos_y resulting y position
 * @param offset offset from the shape outline
 * @param radius max radius of the shape
 * @param origin_x x position of the shape's origin point
 * @param origin_y y position of the shape's origin point
 * @param cosc cosine of the angle
 * @param sinc sine of the angle
 * @param points array of points defining the shape outline
 * @param points_count number of points in the shape outline
 */
void _dt_masks_find_best_attachment_point(float *pos_x, float *pos_y, const float offset, const float radius,
                                    const float origin_x, const float origin_y,
                                    const float cosc, const float sinc,
                                    const float *points, const int points_count)
{
    const float step = radius / 259.0f;
    float best_dist = FLT_MAX;

    for(int k = 1; k < points_count; k += 2)
    {
      const float px = points[k * 2];
      const float py = points[k * 2 + 1];

      for(float r = 0.01f; r < radius; r += step)
      {
        const float epx = origin_x + r * cosc;
        const float epy = origin_y + r * sinc;
        const float ed = sqf(epx - px) + sqf(epy - py);
        if(ed < best_dist)
        {
          best_dist = ed;
          *pos_x = origin_x + (r + offset) * cosc;
          *pos_y = origin_y + (r + offset) * sinc;
        }
      }
    }
}

void dt_masks_draw_source(cairo_t *cr, dt_masks_form_gui_t *gui, const int index, const int nb, 
  const float zoom_scale, const shape_draw_function_t *draw_shape_func)
{
  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  gboolean is_path = (nb > 1);// brush/polygon shapes have nb > 1
  float radius = 2.0f;  // default values for brush/polygon shapes
  // index offset starts differently for brush/polygon shapes and other shapes
  size_t idx = is_path ? 2 : 0;
  // For brush/polygon shapes, the source coordinates are at indices [2] and [3],
  // while for other mask shapes they are at indices [0] and [1]
  const float source_x = is_path ? gpt->source[2] : gpt->source[0];
  const float source_y = is_path ? gpt->source[3] : gpt->source[1];
  const float origin_x = gpt->points[idx];
  const float origin_y = gpt->points[idx + 1];

  // direction from center to source (use atan2 to avoid special cases)
  const float center_angle = atan2f(source_y - origin_y, source_x - origin_x);
  const float cosc = cosf(center_angle);
  const float sinc = sinf(center_angle);
  const float offset = DT_PIXEL_APPLY_DPI(8.0f)/ zoom_scale;
  
  float arrow_x = 0.0f, arrow_y = 0.0f;
  float arrow_source_x = 0.0f, arrow_source_y = 0.0f;
  if(is_path)
  {
    // radial attachment
    arrow_x = origin_x + (offset + radius) * cosc;
    arrow_y = origin_y + (offset + radius) * sinc;

    arrow_source_x = source_x - radius * cosc;
    arrow_source_y = source_y - radius * sinc;
  }
  else
  {
    // compute radius a & radius b.
    const float cnt_x = gpt->points[0]; // center x
    const float cnt_y = gpt->points[1]; // center y
    const float bot_x = gpt->points[2]; // first point x
    const float bot_y = gpt->points[3]; // first point y
    const float rgt_x = gpt->points[6];
    const float rgt_y = gpt->points[7];

    const float delta_x = cnt_x - bot_x;
    const float delta_y = cnt_y - bot_y;
    const float radius_a = delta_x * delta_x + delta_y * delta_y;

    const float border_x = cnt_x - rgt_x;
    const float border_y = cnt_y - rgt_y;
    const float radius_b = border_x * border_x + border_y * border_y;

    radius = sqrtf(fmaxf(radius_a, radius_b));
  
    // find best attachment point for the arrow's tip along shape outline
    _dt_masks_find_best_attachment_point(&arrow_x, &arrow_y, offset, radius,
                                    origin_x, origin_y, cosc, sinc,
                                    gpt->points, gpt->points_count);

    // find best attachment point for the arrow's base along source's shape outline
    _dt_masks_find_best_attachment_point(&arrow_source_x, &arrow_source_y, offset, radius,
                                source_x, source_y, -cosc, -sinc, 
                                gpt->source, gpt->source_count);
  }

  const gboolean selected = (gui->group_selected == index) && (gui->source_selected || gui->source_dragging);
  // don't draw the line if the source is inside the shape
  float arrow_len_sq = sqf(source_x - arrow_x) + sqf(source_y - arrow_y);
  const gboolean draw_tail = (arrow_len_sq > 1e-12f && !dt_masks_point_in_form_exact(arrow_source_x, arrow_source_y, gpt->points, 0, gpt->points_count));
  
  const float arrow[2] = {arrow_x, arrow_y};
  const float source[2] = {arrow_source_x, arrow_source_y};
  dt_draw_arrow(cr, zoom_scale, selected, draw_tail, DT_MASKS_DASH_ROUND, arrow, source);

  // draw the source shape
  {
    cairo_save(cr);
    // Trick to only draw the current polygon lines while editing, but the complete shape when not
    const int nodes_nb = nb + !gui->creation;
    
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    if(draw_shape_func)
      (*draw_shape_func)(cr, gpt->source, gpt->source_count, nodes_nb, FALSE, TRUE);

    dt_draw_set_dash_style(cr, DT_MASKS_NO_DASH, zoom_scale);
    //dark line
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale);
    else
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.6);
    cairo_stroke_preserve(cr);

    //bright line
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_SELECTED / zoom_scale);
    else
      cairo_set_line_width(cr, (1.5f * DT_DRAW_SIZE_LINE) / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);

    cairo_restore(cr);
  }
}

void dt_masks_events_post_expose(struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev) return;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(!gui) return;
  if(!form) return;

  int wd, ht;
  dt_dev_get_processed_size(dev, &wd, &ht);

  if(wd < 1.0 || ht < 1.0) return;
  const float zoom_scale = dt_dev_get_zoom_level(dev);

  // Create a surface to draw the mask, so that we can apply
  // operation that does not affect the main context
  cairo_surface_t *overlay = NULL;
  cairo_t *mask_draw = NULL;
  cairo_surface_t *target = cairo_get_target(cr);
  double sx = 1.0, sy = 1.0;
  cairo_surface_get_device_scale(target, &sx, &sy);
  overlay = cairo_surface_create_similar(target, CAIRO_CONTENT_COLOR_ALPHA, (int)ceil(width * sx),
                                         (int)ceil(height * sy));
  cairo_surface_set_device_scale(overlay, sx, sy);
  mask_draw = cairo_create(overlay);

  // Apply the same transformation to the mask drawing context
  /*cairo_matrix_t m;
  cairo_get_matrix(cr, &m);
  cairo_set_matrix(mask_draw, &m);*/
  
  cairo_save(mask_draw);

  // We rescale to input space
  if(dt_dev_rescale_roi_to_input(dev, mask_draw, width, height))
  {
    cairo_restore(mask_draw);
    cairo_destroy(mask_draw);
    cairo_surface_destroy(overlay);
    return;
  }

  // We update the form if needed
  // Add preview when creating a circle, ellipse and gradient
  if(!(((form->type & DT_MASKS_CIRCLE) || (form->type & DT_MASKS_ELLIPSE) || (form->type & DT_MASKS_GRADIENT))
       && gui->creation))
    dt_masks_gui_form_test_create(form, gui, module);

  // Draw form
  if(form->type & DT_MASKS_GROUP)
    dt_group_events_post_expose(mask_draw, zoom_scale, form, gui);
  else if(form->functions && form->functions->post_expose)
    form->functions->post_expose(mask_draw, zoom_scale, gui, 0, g_list_length(form->points));

  cairo_restore(mask_draw);

  // Draw the overlay with the same transformation as the main context
  cairo_save(cr);
  cairo_identity_matrix(cr);
  cairo_set_source_surface(cr, overlay, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_destroy(mask_draw);
  cairo_surface_destroy(overlay);
}

void dt_masks_clear_form_gui(dt_develop_t *dev)
{
  if(!dev->form_gui) return;
  g_list_free_full(dev->form_gui->points, dt_masks_form_gui_points_free);
  dev->form_gui->points = NULL;
  dt_masks_dynbuf_free(dev->form_gui->guipoints);
  dev->form_gui->guipoints = NULL;
  dt_masks_dynbuf_free(dev->form_gui->guipoints_payload);
  dev->form_gui->guipoints_payload = NULL;
  dev->form_gui->guipoints_count = 0;
  dev->form_gui->pipe_hash = dev->form_gui->formid = 0;
  dev->form_gui->delta[0] = dev->form_gui->delta[1] = 0.0f;
  dev->form_gui->scrollx = dev->form_gui->scrolly = 0.0f;
  dev->form_gui->form_selected = dev->form_gui->border_selected = dev->form_gui->form_dragging
      = dev->form_gui->form_rotating = dev->form_gui->border_toggling = dev->form_gui->gradient_toggling = FALSE;
  dev->form_gui->source_selected = dev->form_gui->source_dragging = FALSE;
  dev->form_gui->pivot_selected = FALSE;
  dev->form_gui->handle_border_selected = dev->form_gui->seg_selected = dev->form_gui->node_selected
      = dev->form_gui->handle_selected = -1;
  dev->form_gui->handle_border_dragging = dev->form_gui->seg_dragging = dev->form_gui->handle_dragging
      = dev->form_gui->node_dragging = -1;
  dev->form_gui->creation_closing_form = dev->form_gui->creation = FALSE;
  dev->form_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  dev->form_gui->creation_module = NULL;
  dev->form_gui->node_edited = -1;

  dev->form_gui->group_selected = -1;
  dev->form_gui->group_selected = -1;
  dev->form_gui->edit_mode = DT_MASKS_EDIT_OFF;
  // allow to select a shape inside an iop
  dt_masks_select_form(NULL, NULL);
}

void dt_masks_change_form_gui(dt_masks_form_t *newform)
{
  dt_masks_clear_form_gui(darktable.develop);
  darktable.develop->form_visible = newform;
}

void dt_masks_reset_form_gui(void)
{
  dt_masks_change_form_gui(NULL);
  dt_iop_module_t *m = darktable.develop->gui_module;
  if(m && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS)
    && m->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), 0);
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), 0);
  }
}

void dt_masks_reset_show_masks_icons(void)
{
  for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if(m && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
      if(!bd) break;  // TODO: this doesn't look right. Why do we break the while look as soon as one module has no blend_data?
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
      gtk_widget_queue_draw(bd->masks_edit);
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), 0);
        gtk_widget_queue_draw(bd->masks_shapes[n]);
      }
    }
  }
}

dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module)
{
  return darktable.develop->form_gui
    ? darktable.develop->form_gui->edit_mode
    : DT_MASKS_EDIT_OFF;
}

void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd) return;

  dt_masks_form_t *grp = NULL;
  dt_masks_form_t *form = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
  if(value && form)
  {
    grp = dt_masks_create_ext(DT_MASKS_GROUP);
    grp->formid = 0;
    dt_masks_group_ungroup(grp, form);
  }

  if(bd) bd->masks_shown = value;

  dt_masks_change_form_gui(grp);
  darktable.develop->form_gui->edit_mode = value;
  if(value && form)
    dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, FALSE);
  else
    dt_dev_masks_selection_change(darktable.develop, NULL, 0, FALSE);

  if(bd->masks_support)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit),
                                 value == DT_MASKS_EDIT_OFF ? FALSE : TRUE);

  dt_control_queue_redraw_center();
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  // we drop all the forms in the iop
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(grp) dt_masks_form_remove(module, NULL, grp);
  module->blend_params->mask_id = 0;

  // and we update the iop
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  dt_masks_iop_update(module);
}

static void _menu_add_shape(struct dt_iop_module_t *module, dt_masks_type_t type)
{
  dt_masks_creation_mode(module, type);
}

static void _menu_add_exist(dt_iop_module_t *module, int formid)
{
  if(!module) return;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(!form) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(!grp)
  {
    grp = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we add the form in this group
  dt_masks_group_add_form(grp, form);
  // we save the group
  // and we ensure that we are in edit mode

  dt_masks_iop_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

void dt_masks_group_update_name(dt_iop_module_t *module)
{
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if (!grp)
    return;

  _set_group_name_from_module(module, grp);

  dt_masks_iop_update(module);
}

void dt_masks_iop_use_same_as(dt_iop_module_t *module, dt_iop_module_t *src)
{
  if(!module || !src) return;

  // we get the source group
  int srcid = src->blend_params->mask_id;
  dt_masks_form_t *src_grp = dt_masks_get_from_id(darktable.develop, srcid);
  if(!src_grp || src_grp->type != DT_MASKS_GROUP) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(!grp)
  {
    grp = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we copy the src group in this group
  for(GList *points = src_grp->points; points; points = g_list_next(points))
  {
    dt_masks_form_group_t *pt = (dt_masks_form_group_t *)points->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(form)
    {
      dt_masks_form_group_t *grpt = dt_masks_group_add_form(grp, form);
      if(grpt)
      {
        grpt->state = pt->state;
        grpt->opacity = pt->opacity;
      }
    }
  }

  // we save the group

}

void dt_masks_iop_combo_populate(GtkWidget *w, void *m)
{
  // we ensure that the module has focus
  dt_iop_module_t *module = (dt_iop_module_t *)m;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  // we determine a higher approx of the entry number
  guint nbe = 5 + g_list_length(module->dev->forms) + g_list_length(module->dev->iop);
  free(bd->masks_combo_ids);
  bd->masks_combo_ids = malloc(sizeof(int) * nbe);

  int *cids = bd->masks_combo_ids;
  GtkWidget *combo = bd->masks_combo;

  // we remove all the combo entries except the first one
  while(dt_bauhaus_combobox_length(combo) > 1)
  {
    dt_bauhaus_combobox_remove_at(combo, 1);
  }

  int pos = 0;
  cids[pos] = 0; // nothing to do for the first entry (already here)
  pos++;

  // add existing shapes
  dt_pthread_rwlock_rdlock(&module->dev->masks_mutex);
  for(GList *forms = module->dev->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) || form->formid == module->blend_params->mask_id)
    {
      continue;
    }

    // we search were this form is used in the current module
    int used = 0;
    dt_masks_form_t *grp = _group_from_module(module->dev, module);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      for(GList *pts = grp->points; pts; pts = g_list_next(pts))
      {
        dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
        if(pt->formid == form->formid)
        {
          used = 1;
          break;
        }
      }
    }
    if(!used)
    {
      dt_bauhaus_combobox_add(combo, form->name);
      cids[pos] = form->formid;
      pos++;
    }
  }
  dt_pthread_rwlock_unlock(&module->dev->masks_mutex);

  // masks from other iops
  int pos2 = 1;
  for(GList *modules = module->dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *other_mod = (dt_iop_module_t *)modules->data;
    if((other_mod != module) && (other_mod->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(other_mod->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *grp = _group_from_module(darktable.develop, other_mod);
      if(grp)
      {
        gchar *module_label = dt_history_item_get_name(other_mod);
        dt_bauhaus_combobox_add(combo, g_strdup_printf(_("reuse shapes from %s"), module_label));
        g_free(module_label);
        cids[pos] = -1 * pos2;
        pos++;
      }
    }
    pos2++;
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  // we get the corresponding value
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  int sel = dt_bauhaus_combobox_get(bd->masks_combo);
  if(sel == 0) return;
  if(sel > 0)
  {
    int val = bd->masks_combo_ids[sel];
    // FIXME : these values should use binary enums
    if(val == -1000000)
    {
      // delete all masks
      _menu_no_masks(module);
    }
    else if(val == -2000001)
    {
      // add a circle shape
      _menu_add_shape(module, DT_MASKS_CIRCLE);
    }
    else if(val == -2000002)
    {
      // add a path shape
      _menu_add_shape(module, DT_MASKS_POLYGON);
    }
    else if(val == -2000016)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_GRADIENT);
    }
    else if(val == -2000032)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_ELLIPSE);
    }
    else if(val == -2000064)
    {
      // add a brush shape
      _menu_add_shape(module, DT_MASKS_BRUSH);
    }
    else if(val < 0)
    {
      // use same shapes as another iop
      val = -1 * val - 1;
      if(val < g_list_length(module->dev->iop))
      {
        dt_iop_module_t *m = (dt_iop_module_t *)g_list_nth_data(module->dev->iop, val);
        dt_masks_iop_use_same_as(module, m);
        // and we ensure that we are in edit mode
        //

        dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      }
    }
    else if(val > 0)
    {
      // add an existing shape
      _menu_add_exist(module, val);
    }
    else
      return;
  }
  // we update the combo line
  dt_masks_iop_update(module);
  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
}

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form)
{
  if(!form) return;
  int id = form->formid;
  if(grp && !(grp->type & DT_MASKS_GROUP)) return;

  if(!(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) && grp)
  {
    // we try to remove the form from the masks group
    int ok = 0;
    for(GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
      if(grpt->formid == id)
      {
        ok = 1;
        grp->points = g_list_remove(grp->points, grpt);
        free(grpt);
        break;
      }
    }
    if(ok)
    if(ok && module)
    {
      dt_masks_iop_update(module);

    }
    if(ok && grp->points == NULL) dt_masks_form_remove(module, NULL, grp);
    return;
  }

  if(form->type & DT_MASKS_GROUP && form->type & DT_MASKS_CLONE)
  {
    // when removing a cloning group the children have to be removed, too, as they won't be shown in the mask manager
    // and are thus not accessible afterwards.
    while(form->points)
    {
      dt_masks_form_group_t *group_child = (dt_masks_form_group_t *)form->points->data;
      dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, group_child->formid);
      dt_masks_form_remove(module, form, child);
      // no need to do anything to form->points, the recursive call will have removed child from the list
    }
  }

  // if we are here that mean we have to permanently delete this form
  // we drop the form from all modules
  for(GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
    if(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      // is the form the base group of the iop ?
      if(id == m->blend_params->mask_id)
      {
        m->blend_params->mask_id = 0;
        dt_masks_iop_update(m);
      }
      else
      {
        dt_masks_form_t *iopgrp = _group_from_module(darktable.develop, m);
        if(iopgrp && (iopgrp->type & DT_MASKS_GROUP))
        {
          int ok = 0;
          GList *forms = iopgrp->points;
          while(forms)
          {
            dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
            if(grpt->formid == id)
            {
              ok = 1;
              iopgrp->points = g_list_remove(iopgrp->points, grpt);
              free(grpt);
              forms = iopgrp->points; // jump back to start of list
              continue;
            }
            forms = g_list_next(forms); // advance to next form
          }
          if(ok)
          {
            dt_masks_iop_update(m);

            if(iopgrp->points == NULL) dt_masks_form_remove(m, NULL, iopgrp);
          }
        }
      }
    }
  }
  // we drop the form from the general list
  for(GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)forms->data;
    if(f->formid == id)
    {
      dt_masks_remove_form(darktable.develop, f);
      break;
    }
  }
}

float dt_masks_form_get_opacity(dt_masks_form_t *form, int parentid)
{
  // Return -1.0f if we couldn't find the opacity of the parent group
  // Note that opacity is not defined at the form level.
  if(!form) return -1.f;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return - 1.f;

  // we first need to test if the opacity can be set to the form
  if(form->type & DT_MASKS_GROUP) return -1.f;
  const int id = form->formid;

  // so we change the value inside the group
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    if(fpt->formid == id)
    {
      return fpt->opacity;
    }
  }
  return -1.f;
}

const char * _get_mask_plugin(dt_masks_form_t *form)
{
  // Internal masks are used by spots removal and retouch modules
  if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
    return "spots";
  // Regular all-purpose masks
  else
    return "masks";
}

const char * _get_mask_type(dt_masks_form_t *form)
{
  // warning: mask types or not int enum but bit flags ?!?
  // that's a shitty design that prevents us from doing a clean switch case over the enum.
  // why would we overlap mask types ?!?
  if(form->type & DT_MASKS_CIRCLE)
    return "circle";
  else if(form->type & DT_MASKS_POLYGON)
    return "polygon";
  else if(form->type & DT_MASKS_ELLIPSE)
    return "ellipse";
  else if(form->type & DT_MASKS_GRADIENT)
    return "gradient";
  else if(form->type & DT_MASKS_BRUSH)
    return "brush";
  else
    return "unknown";
}

float dt_masks_get_set_conf_value(dt_masks_form_t *form, char *feature, float new_value, float v_min, float v_max, dt_masks_increment_t increment, int flow)
{
  gchar *key;
  if(!strcmp(feature, "opacity"))
    key = g_strdup_printf("plugins/darkroom/%s_opacity", _get_mask_plugin(form));
  else
    key = g_strdup_printf("plugins/darkroom/%s/%s/%s", _get_mask_plugin(form), _get_mask_type(form), feature);

  if(!g_strcmp0(feature, "rotation")) flow = (flow > 1) ? (flow - 1) * 5 : flow;

  float value = (increment == DT_MASKS_INCREMENT_SCALE)    ? dt_conf_get_float(key) * powf(new_value, (float)flow)
                : (increment == DT_MASKS_INCREMENT_OFFSET) ? dt_conf_get_float(key) + new_value * flow
                                                           : new_value; // DT_MASKS_INCREMENT_ABSOLUTE
  if(!g_strcmp0(feature, "rotation"))
  {
    // Ensure the rotation value stays within the interval [min, max)
    if(value > v_max) value = fmodf(value, v_max);
    else if(value < v_min) value = v_max - fmodf(v_min - value, v_max);
  }
  else value = MAX(v_min, MIN(value, v_max));

  dt_conf_set_float(key, value);

  g_free(key);
  return value;
}

int dt_masks_form_set_opacity(dt_masks_form_t *form, int parentid, float opacity, dt_masks_increment_t offset, const int flow)
{
  // If offset == TRUE, opacity is treated as an offset to add on top of current mask opacity
  // else it is set absolutely and directly
  if(!form) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return 0;

  // we first need to test if the opacity can be set to the form
  if(form->type & DT_MASKS_GROUP) return 0;
  const int id = form->formid;

  // so we change the value inside the group
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    if(fpt->formid == id)
    {
      float new_opacity = (offset == DT_MASKS_INCREMENT_OFFSET)  ? fpt->opacity + opacity * flow
                                : (offset == DT_MASKS_INCREMENT_SCALE) ? fpt->opacity * powf(opacity, (float)flow)
                                                                       : opacity; // DT_MASKS_INCREMENT_ABSOLUTE
      new_opacity = CLAMP(new_opacity, 0.0f, 1.0f);
      fpt->opacity = new_opacity;
      dt_toast_log(_("Opacity: %3.2f%%"), new_opacity * 100.f);
      return 1;
    }
  }
  return 0;
}

int dt_masks_form_change_opacity(dt_masks_form_t *form, int parentid, int up, const int flow)
{
  const float amount = up ? 0.02f : -0.02f;
  return dt_masks_form_set_opacity(form, parentid, amount, DT_MASKS_INCREMENT_OFFSET, flow);
}

void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  // we search the form in the group
  dt_masks_form_group_t *grpt = NULL;
  guint pos = 0;
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    if(fpt->formid == formid)
    {
      grpt = fpt;
      break;
    }
    pos++;
  }

  // we remove the form and read it
  if(grpt)
  {
    if(!up && pos == 0) return;
    if(up && pos == g_list_length(grp->points) - 1) return;

    grp->points = g_list_remove(grp->points, grpt);
    if(!up)
      pos -= 1;
    else
      pos += 1;
    grp->points = g_list_insert(grp->points, grpt, pos);

  }
}

static int _find_in_group(dt_masks_form_t *grp, int formid)
{
  if(!(grp->type & DT_MASKS_GROUP)) return 0;
  if(grp->formid == formid) return 1;
  int nb = 0;
  for(GList *forms = grp->points; forms; forms = g_list_next(forms))
  {
    const dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
    if(form)
    {
      if(form->type & DT_MASKS_GROUP) nb += _find_in_group(form, formid);
    }
  }
  return nb;
}

dt_masks_form_group_t *dt_masks_group_add_form(dt_masks_form_t *grp, dt_masks_form_t *form)
{
  // add a form to group and check for self inclusion

  if(!(grp->type & DT_MASKS_GROUP)) return NULL;
  // either the form to add is not a group, so no risk
  // or we go through all points of form to see if we find a ref to grp->formid
  if(!(form->type & DT_MASKS_GROUP) || _find_in_group(form, grp->formid) == 0)
  {
    dt_masks_form_group_t *grpt = malloc(sizeof(dt_masks_form_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grp->formid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(grp->points) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    grp->points = g_list_append(grp->points, grpt);
    return grpt;
  }

  dt_control_log(_("masks can not contain themselves"));
  return NULL;
}

void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp)
{
  if(!grp || !dest_grp) return;
  if(!(grp->type & DT_MASKS_GROUP) || !(dest_grp->type & DT_MASKS_GROUP)) return;

  for(GList *forms = grp->points; forms; forms = g_list_next(forms))
  {
    dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
    if(form)
    {
      if(form->type & DT_MASKS_GROUP)
      {
        dt_masks_group_ungroup(dest_grp, form);
      }
      else
      {
        dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));
        fpt->formid = grpt->formid;
        fpt->parentid = grpt->parentid;
        fpt->state = grpt->state;
        fpt->opacity = grpt->opacity;
        dest_grp->points = g_list_append(dest_grp->points, fpt);
      }
    }
  }
}

uint64_t dt_masks_group_get_hash(uint64_t hash, dt_masks_form_t *form)
{
  if(!form) return hash;

  // basic infos
  hash = dt_hash(hash, (char *)&form->type, sizeof(dt_masks_type_t));
  hash = dt_hash(hash, (char *)&form->formid, sizeof(int));
  hash = dt_hash(hash, (char *)&form->version, sizeof(int));
  hash = dt_hash(hash, (char *)&form->source, sizeof(float) * 2);

  for(const GList *forms = form->points; forms; forms = g_list_next(forms))
  {
    if(form->type & DT_MASKS_GROUP)
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
      {
        // state & opacity
        hash = dt_hash(hash, (char *)&grpt->state, sizeof(int));
        hash = dt_hash(hash, (char *)&grpt->opacity, sizeof(float));

        // the form itself
        hash = dt_masks_group_get_hash(hash, f);
      }
    }
    else if(form->functions)
    {
      hash = dt_hash(hash, (char *)forms->data, form->functions->point_struct_size);
    }
  }
  return hash;
}

// adds formid to used array
// if formid is a group it adds all the forms that belongs to that group
static void _cleanup_unused_recurs(GList *forms, int formid, int *used, int nb)
{
  // first, we search for the formid in used table
  for(int i = 0; i < nb; i++)
  {
    if(used[i] == 0)
    {
      // we store the formid
      used[i] = formid;
      break;
    }
    if(used[i] == formid) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    for(GList *grpts = form->points; grpts; grpts = g_list_next(grpts))
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)grpts->data;
      _cleanup_unused_recurs(forms, grpt->formid, used, nb);
    }
  }
}

// removes from _forms all forms that are not used in history_list up to history_end
static int _masks_cleanup_unused(GList **_forms, GList *history_list, const int history_end)
{
  int masks_removed = 0;
  GList *forms = *_forms;

  // we create a table to store the ids of used forms
  guint nbf = g_list_length(forms);
  int *used = calloc(nbf, sizeof(int));

  // check in history if the module has drawn masks and add it to used array
  int num = 0;
  for(GList *history = history_list; history && num < history_end; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    dt_develop_blend_params_t *blend_params = hist->blend_params;
    if(blend_params)
    {
      if(blend_params->mask_id > 0) _cleanup_unused_recurs(forms, blend_params->mask_id, used, nbf);
    }
    num++;
  }

  // and we delete all unused forms
  GList *shapes = forms;
  while(shapes)
  {
    dt_masks_form_t *f = (dt_masks_form_t *)shapes->data;
    int u = 0;
    for(int i = 0; i < nbf; i++)
    {
      if(used[i] == f->formid)
      {
        u = 1;
        break;
      }
      if(used[i] == 0) break;
    }

    shapes = g_list_next(shapes); // need to get 'next' now, because we may be removing the current node

    if(u == 0)
    {
      forms = g_list_remove(forms, f);
      // and add it to allforms for cleanup
      darktable.develop->allforms = g_list_append(darktable.develop->allforms, f);
      masks_removed = 1;
    }
  }

  free(used);

  *_forms = forms;

  return masks_removed;
}

// removes all unused form from history
// if there are multiple hist->forms entries in history it may leave some unused forms
// we do it like this so the user can go back in history
// for a more accurate cleanup the user should compress history
void dt_masks_cleanup_unused_from_list(GList *history_list)
{
  // a mask is used in a given hist->forms entry if it is used up to the next hist->forms
  // so we are going to remove for each hist->forms from the top
  int num = g_list_length(history_list);
  int history_end = num;
  for(const GList *history = g_list_last(history_list); history; history = g_list_previous(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    if(hist->forms && strcmp(hist->op_name, "mask_manager") == 0)
    {
      _masks_cleanup_unused(&hist->forms, history_list, history_end);
      history_end = num - 1;
    }
    num--;
  }
}

void dt_masks_cleanup_unused(dt_develop_t *dev)
{
  dt_masks_change_form_gui(NULL);

  // we remove the forms from history
  dt_masks_cleanup_unused_from_list(dev->history);

  // and we save all that
  GList *forms = NULL;
  int num = 0;
  for(const GList *history = g_list_first(dev->history);
      history && num < dt_dev_get_history_end(dev);
      history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;

    if(hist->forms) forms = hist->forms;
    num++;
  }

  dt_masks_replace_current_forms(dev, forms);
}

/**
 * @brief Check whether the 2D point (x, y) lies inside the polygon (mask) described by `points`.
 *
 * we use ray casting algorithm
 * to avoid most problems with horizontal segments, y should be rounded as int
 * so that there's very little chance than y==points...
 * 
 * @param x The x-coordinate of the point to test.
 * @param y The y-coordinate of the point to test.
 * @param points The array of polygon vertices.
 * @param points_start The starting index of the polygon vertices in the array.
 * @param points_count The total number of vertices in the polygon.
 * @return int 1 if the point is inside the polygon, 0 otherwise.
 */
int dt_masks_point_in_form_exact(float x, float y, float *points, int points_start, int points_count)
{
  int nb = 0;

  if(points_count > 2 + points_start)
  {
    int start = isnan(points[points_start * 2]) && !isnan(points[points_start * 2 + 1])
                    ? points[points_start * 2 + 1]
                    : points_start;

    float yf = y;
    for(int i = start, next = start + 1; i < points_count;)
    {
      float y1 = points[i * 2 + 1];
      float y2 = points[next * 2 + 1];
      //if we need to skip points (in case of deleted point, because of self-intersection)
      if(isnan(points[next * 2]))
      {
        next = isnan(y2) ? start : (int)y2;
        continue;
      }
      if(((yf <= y2 && yf > y1) || (yf >= y2 && yf < y1)) && (points[i * 2] > x)) nb++;

      if(next == start) break;
      i = next++;
      if(next >= points_count) next = start;
    }
  }

  return (nb & 1);
}

int dt_masks_point_in_form_near(float x, float y, float *points, int points_start, int points_count, float distance, int *near)
{
  // we use ray casting algorithm
  // to avoid most problems with horizontal segments, y should be rounded as int
  // so that there's very little chance than y==points...

  // TODO : distance is only evaluated in x, not y...

  if(points_count > 2 + points_start)
  {
    const int start = isnan(points[points_start * 2]) && !isnan(points[points_start * 2 + 1])
                      ? points[points_start * 2 + 1]
                      : points_start;

    const float yf = y;
    int nb = 0;
    for(int i = start, next = start + 1; i < points_count;)
    {
      const float y1 = points[i * 2 + 1];
      const float y2 = points[next * 2 + 1];
      //if we need to jump to skip points (in case of deleted point, because of self-intersection)
      if(isnan(points[next * 2]))
      {
        next = isnan(y2) ? start : (int)y2;
        continue;
      }
      if((yf <= y2 && yf > y1) || (yf >= y2 && yf < y1))
      {
        if(points[i * 2] > x) nb++;
        if(points[i * 2] - x < distance && points[i * 2] - x > -distance) *near = 1;
      }

      if(next == start) break;
      i = next++;
      if(next >= points_count) next = start;
    }
    return (nb & 1);
  }
  return 0;
}

// allow to select a shape inside an iop
void dt_masks_select_form(struct dt_iop_module_t *module, dt_masks_form_t *sel)
{
  gboolean selection_changed = FALSE;

  if(sel)
  {
    if(sel->formid != darktable.develop->mask_form_selected_id)
    {
      darktable.develop->mask_form_selected_id = sel->formid;
      selection_changed = TRUE;
    }
  }
  else
  {
    if(darktable.develop->mask_form_selected_id != 0)
    {
      darktable.develop->mask_form_selected_id = 0;
      selection_changed = TRUE;
    }
  }
  if(selection_changed)
  {
    if(!module && darktable.develop->mask_form_selected_id == 0)
      module = darktable.develop->gui_module;
    if(module)
    {
      if(module->masks_selection_changed)
        module->masks_selection_changed(module, darktable.develop->mask_form_selected_id);
    }
  }
}

// sets if the initial source position for a clone mask will be absolute or relative,
// based on mouse position and key state
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui, const uint32_t state, const float pzx,
                                           const float pzy)
{
  if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    gui->source_pos_type = DT_MASKS_SOURCE_POS_ABSOLUTE;
  else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_state] unknown state for setting masks position type\n");

  // both source types record an absolute position,
  // for the relative type, the first time is used the position is recorded,
  // the second time a relative position is calculated based on that one
  const float scale = darktable.develop->natural_scale;
  // normalize backbuf points
  gui->pos_source[0] = pzx * darktable.develop->preview_width / scale;
  gui->pos_source[1] = pzy * darktable.develop->preview_height / scale;
}

// set the initial source position value for a clone mask
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui, dt_masks_form_t *form,
                                                   const float pzx, const float pzy)
{
  const float wd = darktable.develop->preview_width;
  const float ht = darktable.develop->preview_height;
  const float iwd = darktable.develop->preview_pipe->iwidth;
  const float iht = darktable.develop->preview_pipe->iheight;

  // if this is the first time the relative pos is used
  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if it has not been defined by the user, set some default
    if(gui->pos_source[0] == -1.0f && gui->pos_source[1] == -1.0f)
    {
      if(form->functions && form->functions->initial_source_pos)
      {
        form->functions->initial_source_pos(iwd, iht, &gui->pos_source[0], &gui->pos_source[1]);
      }
      else
        fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unsupported masks type when calculating source position initial value\n");

      // set offset to form->source
      float pts[2] = { pzx, pzy };
      dt_dev_roi_delta_to_input_space(darktable.develop, gui->pos_source, pts, form->source);

    }
    else
    {
      // if a position was defined by the user, use the absolute value the first time
      float pts[2] = { gui->pos_source[0], gui->pos_source[1] };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / iwd;
      form->source[1] = pts[1] / iht;

      gui->pos_source[0] = gui->pos_source[0] - pzx * wd / darktable.develop->natural_scale;
      gui->pos_source[1] = gui->pos_source[1] - pzy * ht / darktable.develop->natural_scale;
    }

    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    // original pos was already defined and relative value calculated, just use it
    float pts[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(darktable.develop, gui->pos_source, pts, form->source);
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // an absolute position was defined by the user
    float pts_src[2] = { gui->pos_source[0], gui->pos_source[1] };
    dt_dev_distort_backtransform(darktable.develop, pts_src, 1);

    form->source[0] = pts_src[0] / iwd;
    form->source[1] = pts_src[1] / iht;
  }
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unknown source position type\n");
}

// calculates the source position value for preview drawing, on cairo coordinates
void dt_masks_calculate_source_pos_value(dt_masks_form_gui_t *gui, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos, float *px,
                                         float *py, const int adding)
{
  float x = 0.0f, y = 0.0f;
  const float iwd = darktable.develop->preview_pipe->iwidth;
  const float iht = darktable.develop->preview_pipe->iheight;
  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    x = xpos + gui->pos_source[0];
    y = ypos + gui->pos_source[1];
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    if(gui->pos_source[0] == -1.0f && gui->pos_source[1] == -1.0f)
    {
      const dt_masks_form_t *form = darktable.develop->form_visible;
      if(form && form->functions && form->functions->initial_source_pos)
      {
        form->functions->initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
      else
        fprintf(stderr, "[dt_masks_calculate_source_pos_value] unsupported masks type when calculating source position value\n");
    }
    else
    {
      x = gui->pos_source[0];
      y = gui->pos_source[1];
    }
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // if the user is actually adding, the mask follow the cursor
    if(adding)
    {
      x = xpos + gui->pos_source[0] - initial_xpos;
      y = ypos + gui->pos_source[1] - initial_ypos;
    }
    else
    {
      // if not added yet set the start position
      x = gui->pos_source[0];
      y = gui->pos_source[1];
    }
  }
  else
    fprintf(stderr, "[dt_masks_calculate_source_pos_value] unknown source position type for setting source position value\n");

  *px = x;
  *py = y;
}

float dt_masks_rotate_with_anchor(dt_develop_t *dev, const float anchor[2], const float center[2], dt_masks_form_gui_t *gui)
{
  const float center_x = center[0];
  const float center_y = center[1];

  // get the current angle
  const float anchor_x = anchor[0];
  const float anchor_y = anchor[1];
  const float angle_current = atan2f(anchor_y - center_y, anchor_x - center_x);

  // get the previous angle
  const float delta_x = gui->delta[0];
  const float delta_y = gui->delta[1];
  const float angle_prev = atan2f(delta_y - center_y, delta_x - center_x);

  // calculate the angle difference an normalize to -180 to 180 degrees
  float delta_angle = angle_current - angle_prev;
  float angle = atan2f(sinf(delta_angle), cosf(delta_angle));

  // check if distortion inverts the axes
  float pts2[8] = { center_x, center_y, anchor_x , anchor_y, center_x+10.0f, center_y, center_x, center_y+10.0f };
  dt_dev_distort_backtransform(dev, pts2, 4);
  float check_angle = atan2f(pts2[7] - pts2[1], pts2[6] - pts2[0]) - atan2f(pts2[5] - pts2[1], pts2[4] - pts2[0]);
  // Normalize to the range -180 to 180 degrees
  check_angle = atan2f(sinf(check_angle), cosf(check_angle));

  // Adjust the sign if the axes are inverted by distortion
  if(check_angle < 0.0f) angle = -angle;

  // Update the delta for the next frame (old position becomes the current one)
  gui->delta[0] = anchor_x;
  gui->delta[1] = anchor_y;

  return angle / M_PI * 180.0f;
}

gboolean dt_masks_is_within_radius(const float px, const float py,
                                        const float cx, const float cy,
                                        const float radius)
{
  const float sq_radius = radius * radius;
  const float dx = px - cx;
  const float dy = py - cy;
  const float sq_dist = dx * dx + dy * dy;
  return sq_dist <= sq_radius;
}

// NOTE: this does quite the same as _menu_add_shape
gboolean dt_masks_creation_mode(dt_iop_module_t *module, const dt_masks_type_t type)
{
  if(!module || (type & DT_MASKS_ALL) == 0) return FALSE;
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);

  dt_masks_form_t *form = dt_masks_create(type);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;

  // Give focus to central view to allow using shortcuts for mask creation right after selecting a mask type in the manager
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  return TRUE;
}

#include "detail.c"

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
