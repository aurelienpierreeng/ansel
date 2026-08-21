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
#include "system/macros.h"
#include "database/history_repository.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "common/hash.h"
#include "common/logging.h"
#include "common/times.h"
#include "common/glib_utils.h"
#include "system/dtpthread.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "develop/masks.h"
#include "history/notify.h"
#include "develop/masks/masks_functions.h"
#include "develop/develop.h"
#include "develop/supervisor.h"
#include "math/math.h"
#include "common/conf.h"
#include "develop/blend.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include <stdint.h>


/**
 * @brief Deep-copy a mask form, including its points list.
 *
 * Caveats:
 * - The caller owns the returned form and must free it.
 * - Point payloads are copied byte-for-byte using the type's point struct size.
 */
dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *mask_form)
{
  if (IS_NULL_PTR(mask_form)) return NULL;

  dt_masks_form_t *duplicate_form = malloc(sizeof(struct dt_masks_form_t));
  memcpy(duplicate_form, mask_form, sizeof(struct dt_masks_form_t));

  // The memcpy above copied the source's live refcount: a fresh clone must
  // start with exactly one owner (whoever holds the returned pointer).
  dt_atomic_set_int(&duplicate_form->refcount, 1);

  // Duplicate the GList *points payloads into a new list.
  GList *duplicated_points = NULL;

  if(mask_form->points)
  {
    const int point_struct_size = (mask_form->functions) ? mask_form->functions->point_struct_size : 0;

    if(point_struct_size != 0)
    {
      for(GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
      {
        void *point_copy = malloc(point_struct_size);
        memcpy(point_copy, point_node->data, point_struct_size);
        duplicated_points = g_list_prepend(duplicated_points, point_copy);
      }
    }
  }

  // The list was built in reverse order, so un-reverse it.
  duplicate_form->points = g_list_reverse(duplicated_points);

  return duplicate_form;
}

void dt_masks_form_gui_points_free(gpointer data)
{
  if(IS_NULL_PTR(data)) return;

  dt_masks_form_gui_points_t *gui_points = (dt_masks_form_gui_points_t *)data;

  dt_pixelpipe_cache_free_align(gui_points->points);
  dt_pixelpipe_cache_free_align(gui_points->border);
  dt_pixelpipe_cache_free_align(gui_points->source);
  dt_free(gui_points);
}

void _check_id(dt_develop_t *dev, dt_masks_form_t *mask_form)
{
  int new_form_id = 100;
  for(GList *form_node = dev->forms; form_node; )
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->formid == mask_form->formid)
    {
      mask_form->formid = new_form_id++;
      form_node = dev->forms; // jump back to start of list
    }
    else
    {
      form_node = g_list_next(form_node); // advance to next form
    }
  }
}

void _set_group_name_from_module(dt_iop_module_t *module, dt_masks_form_t *group_form)
{
  gchar *group_name = dt_dev_get_masks_group_name(module);
  g_strlcpy(group_form->name, group_name, sizeof(group_form->name));
  dt_free(group_name);
}

dt_masks_form_t *_group_create(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_type_t group_type)
{
  dt_masks_form_t *group_form = dt_masks_create(group_type);
  _set_group_name_from_module(module, group_form);
  _check_id(develop, group_form);
  dt_masks_append_form(develop, group_form);
  module->blend_params->mask_id = group_form->formid;
  return group_form;
}

// get the group form associated to the module, if any
dt_masks_form_t *_group_from_module(dt_develop_t *develop, dt_iop_module_t *module)
{
  return dt_masks_get_from_id(develop, module->blend_params->mask_id);
}

int dt_masks_form_duplicate(dt_develop_t *develop, int form_id)
{
  // we create a new empty form
  dt_masks_form_t *base_form = dt_masks_get_from_id(develop, form_id);
  if(IS_NULL_PTR(base_form)) return -1;
  dt_masks_form_t *dest_form = dt_masks_create(base_form->type);
  _check_id(develop, dest_form);

  // we copy the base values
  dest_form->source[0] = base_form->source[0];
  dest_form->source[1] = base_form->source[1];
  dest_form->version = base_form->version;
  snprintf(dest_form->name, sizeof(dest_form->name), _("copy of %s"), base_form->name);

  dt_masks_append_form(develop, dest_form);

  // we copy all the points
  if(base_form->functions)
    base_form->functions->duplicate_points(develop, base_form, dest_form);

  // and we return its id
  return dest_form->formid;
}

int dt_masks_form_duplicate_in_group(dt_develop_t *develop, int group_id, int form_id)
{
  const int nid = dt_masks_form_duplicate(develop, form_id);
  if(nid <= 0) return nid;

  dt_masks_form_t *grp = (group_id > 0) ? dt_masks_get_from_id(develop, group_id) : NULL;
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return nid;

  grp = dt_masks_cow_touch(develop, grp);

  dt_masks_form_group_t *orig_entry = NULL;
  for(GList *pts = grp->points; pts; pts = g_list_next(pts))
  {
    dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
    if(pt->formid == form_id) { orig_entry = pt; break; }
  }

  dt_masks_form_t *dup_form = dt_masks_get_from_id(develop, nid);
  dt_masks_form_group_t *new_entry = dup_form ? dt_masks_group_add_form(develop, grp, dup_form) : NULL;
  if(new_entry && orig_entry)
  {
    new_entry->state = orig_entry->state;
    new_entry->opacity = orig_entry->opacity;
  }

  return nid;
}

int dt_masks_get_points_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                               float **point_buffer, int *point_count,
                               float **border_buffer, int *border_count,
                               int source, dt_iop_module_t *module)
{
  if(mask_form->functions && mask_form->functions->get_points_border)
    return mask_form->functions->get_points_border(develop, mask_form, point_buffer, point_count,
                                                   border_buffer, border_count, source, module);
  return 1;
}

int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *mask_form,
                      int *area_width, int *area_height, int *area_pos_x, int *area_pos_y)
{
  if(mask_form->functions && mask_form->functions->get_area)
    return mask_form->functions->get_area(module, pipe, piece, mask_form, area_width, area_height,
                                          area_pos_x, area_pos_y);
  return 1;
}

int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                             dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *mask_form,
                             int *area_width, int *area_height,
                             int *area_pos_x, int *area_pos_y)
{
  *area_width = *area_height = *area_pos_x = *area_pos_y = 0;

  // must be a clone form
  if(mask_form->type & DT_MASKS_CLONE)
  {
    if(mask_form->functions && mask_form->functions->get_source_area)
      return mask_form->functions->get_source_area(module, pipe, piece, mask_form, area_width, area_height,
                                                   area_pos_x, area_pos_y);
  }
  return 1;
}

int dt_masks_version(void)
{
  return DEVELOP_MASKS_VERSION;
}

static int dt_masks_legacy_params_v1_to_v2(dt_develop_t *develop, void *params)
{
  /*
   * difference: before v2 images were originally rotated on load, and then
   * maybe in flip iop
   * after v2: images are only rotated in flip iop.
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  const dt_image_orientation_t orientation = dt_image_orientation(&develop->image_storage);

  if(orientation == ORIENTATION_NONE)
  {
    // image is not rotated, we're fine!
    mask_form->version = 2;
    return 0;
  }
  else
  {
    if(IS_NULL_PTR(develop->iop)) return 1;

    const char *opname = "flip";
    dt_iop_module_t *module = NULL;

    for(GList *module_node = develop->iop; module_node; module_node = g_list_next(module_node))
    {
      dt_iop_module_t *iop_module = (dt_iop_module_t *)module_node->data;
      if(!strcmp(iop_module->op, opname))
      {
        module = iop_module;
        break;
      }
    }

    if(IS_NULL_PTR(module)) return 1;

    dt_dev_pixelpipe_iop_t piece = { 0 };

    module->init_pipe(module, NULL, &piece);
    module->commit_params(module, module->default_params, NULL, &piece);

    piece.buf_in.width = 1;
    piece.buf_in.height = 1;

    GList *point_node = mask_form->points;

    if(IS_NULL_PTR(point_node)) return 1;

    if(mask_form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)point_node->data;
      if(IS_NULL_PTR(circle)) return 1;
      module->distort_backtransform(module, NULL, &piece, circle->center, 1);
    }
    else if(mask_form->type & DT_MASKS_POLYGON)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)point_node->data;
        if(IS_NULL_PTR(polygon_node)) return 1;
        module->distort_backtransform(module, NULL, &piece, polygon_node->node, 1);
        module->distort_backtransform(module, NULL, &piece, polygon_node->ctrl1, 1);
        module->distort_backtransform(module, NULL, &piece, polygon_node->ctrl2, 1);
      }
    }
    else if(mask_form->type & DT_MASKS_GRADIENT)
    { // TODO: new ones have wrong rotation.
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
      if(IS_NULL_PTR(gradient)) return 1;
      module->distort_backtransform(module, NULL, &piece, gradient->center, 1);

      if(orientation == ORIENTATION_ROTATE_180_DEG)
        gradient->rotation -= 180.0f;
      else if(orientation == ORIENTATION_ROTATE_CCW_90_DEG)
        gradient->rotation -= 90.0f;
      else if(orientation == ORIENTATION_ROTATE_CW_90_DEG)
        gradient->rotation -= -90.0f;
    }
    else if(mask_form->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
      module->distort_backtransform(module, NULL, &piece, ellipse->center, 1);

      if(orientation & ORIENTATION_SWAP_XY)
      {
        const float y = ellipse->radius[0];
        ellipse->radius[0] = ellipse->radius[1];
        ellipse->radius[1] = y;
      }
    }
    else if(mask_form->type & DT_MASKS_BRUSH)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)point_node->data;
        if(IS_NULL_PTR(brush_node)) return 1;
        module->distort_backtransform(module, NULL, &piece, brush_node->node, 1);
        module->distort_backtransform(module, NULL, &piece, brush_node->ctrl1, 1);
        module->distort_backtransform(module, NULL, &piece, brush_node->ctrl2, 1);
      }
    }

    if(mask_form->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      module->distort_backtransform(module, NULL, &piece, mask_form->source, 1);
    }

    mask_form->version = 2;

    return 0;
  }
}

static void dt_masks_legacy_params_v2_to_v3_transform(const dt_image_t *image, float *coords)
{
  const float image_width = (float)image->width;
  const float image_height = (float)image->height;

  const float crop_x = (float)image->crop_x;
  const float crop_y = (float)image->crop_y;

  const float crop_width = (float)(image->width - image->crop_x - image->crop_width);
  const float crop_height = (float)(image->height - image->crop_y - image->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by image original cropped dimensions
   * 2. un-crop them by adding top-left crop coordinates
   * 3. normalize them by the image fully uncropped dimensions
   */
  coords[0] = ((coords[0] * crop_width) + crop_x) / image_width;
  coords[1] = ((coords[1] * crop_height) + crop_y) / image_height;
}

static void dt_masks_legacy_params_v2_to_v3_transform_only_rescale(const dt_image_t *image, float *coords,
                                                                   size_t coords_count)
{
  const float image_width = (float)image->width;
  const float image_height = (float)image->height;

  const float crop_width = (float)(image->width - image->crop_x - image->crop_width);
  const float crop_height = (float)(image->height - image->crop_y - image->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by minimal of image original cropped dimensions
   * 2. normalize them by the minimal of image fully uncropped dimensions
   */
  const float crop_min = MIN(crop_width, crop_height);
  const float image_min = MIN(image_width, image_height);
  for(size_t coord_index = 0; coord_index < coords_count; coord_index++)
    coords[coord_index] = ((coords[coord_index] * crop_min)) / image_min;
}

static int dt_masks_legacy_params_v2_to_v3(dt_develop_t *develop, void *params)
{
  /*
   * difference: before v3 images were originally cropped on load
   * after v3: images are cropped in rawprepare iop.
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  const dt_image_t *image = &(develop->image_storage);

  if(image->crop_x == 0 && image->crop_y == 0 && image->crop_width == 0 && image->crop_height == 0)
  {
    // image has no "raw cropping", we're fine!
    mask_form->version = 3;
    return 0;
  }
  else
  {
    GList *point_node = mask_form->points;

    if(IS_NULL_PTR(point_node)) return 1;

    if(mask_form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)point_node->data;
      if(IS_NULL_PTR(circle)) return 1;
      dt_masks_legacy_params_v2_to_v3_transform(image, circle->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &circle->radius, 1);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &circle->border, 1);
    }
    else if(mask_form->type & DT_MASKS_POLYGON)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)point_node->data;
        if(IS_NULL_PTR(polygon_node)) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->node);
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, polygon_node->border, 2);
      }
    }
    else if(mask_form->type & DT_MASKS_GRADIENT)
    {
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
      dt_masks_legacy_params_v2_to_v3_transform(image, gradient->center);
    }
    else if(mask_form->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
      dt_masks_legacy_params_v2_to_v3_transform(image, ellipse->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, ellipse->radius, 2);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &ellipse->border, 1);
    }
    else if(mask_form->type & DT_MASKS_BRUSH)
    {
      for(; point_node;  point_node = g_list_next(point_node))
      {
        dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)point_node->data;
        if(IS_NULL_PTR(brush_node)) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->node);
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, brush_node->border, 2);
      }
    }

    if(mask_form->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      dt_masks_legacy_params_v2_to_v3_transform(image, mask_form->source);
    }

    mask_form->version = 3;

    return 0;
  }
}

static int dt_masks_legacy_params_v3_to_v4(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting ellipse
   * up to v3: only equidistant feathering
   * after v4: choice between equidistant and proportional feathering
   * type of feathering is defined in new flags parameter
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(IS_NULL_PTR(point_node)) return 1;

  if(mask_form->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
    ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  }

  mask_form->version = 4;

  return 0;
}


static int dt_masks_legacy_params_v4_to_v5(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting gradient
   * up to v4: only linear gradient (relative to input image)
   * after v5: curved gradients
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(IS_NULL_PTR(point_node)) return 1;

  if(mask_form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
    gradient->curvature = 0.0f;
  }

  mask_form->version = 5;

  return 0;
}

static int dt_masks_legacy_params_v5_to_v6(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting gradient
   * up to v5: linear transition
   * after v5: linear or sigmoidal transition
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(IS_NULL_PTR(point_node)) return 1;

  if(mask_form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
    gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;
  }

  mask_form->version = 6;

  return 0;
}


int dt_masks_legacy_params(dt_develop_t *develop, void *params, const int old_version, const int new_version)
{
  int result = 1;
#if 0 // we should not need this any longer
  if(old_version == 1 && new_version == 2)
  {
    result = dt_masks_legacy_params_v1_to_v2(develop, params);
  }
#endif

  if(old_version == 1 && new_version == 6)
  {
    result = dt_masks_legacy_params_v1_to_v2(develop, params);
    if(!result) result = dt_masks_legacy_params_v2_to_v3(develop, params);
    if(!result) result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 2 && new_version == 6)
  {
    result = dt_masks_legacy_params_v2_to_v3(develop, params);
    if(!result) result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 3 && new_version == 6)
  {
    result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 4 && new_version == 6)
  {
    result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 5 && new_version == 6)
  {
    result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }

  return result;
}

static int form_id_seed = 0;

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *mask_form = (dt_masks_form_t *)calloc(1, sizeof(dt_masks_form_t));
  if(IS_NULL_PTR(mask_form)) return NULL;

  // Freshly created: exactly one owner (whoever holds the returned pointer).
  dt_atomic_set_int(&mask_form->refcount, 1);

  mask_form->type = type;
  mask_form->version = dt_masks_version();
  mask_form->formid = time(NULL) + form_id_seed++;
  mask_form->uses_bezier_points_layout = (type & (DT_MASKS_BRUSH | DT_MASKS_POLYGON)) ? TRUE : FALSE;

  if (type & DT_MASKS_CIRCLE)
    mask_form->functions = &dt_masks_functions_circle;
  else if (type & DT_MASKS_ELLIPSE)
    mask_form->functions = &dt_masks_functions_ellipse;
  else if (type & DT_MASKS_BRUSH)
    mask_form->functions = &dt_masks_functions_brush;
  else if (type & DT_MASKS_POLYGON)
    mask_form->functions = &dt_masks_functions_polygon;
  else if (type & DT_MASKS_GRADIENT)
    mask_form->functions = &dt_masks_functions_gradient;
  else if (type & DT_MASKS_GROUP)
    mask_form->functions = &dt_masks_functions_group;

  if (mask_form->functions && mask_form->functions->sanitize_config)
    mask_form->functions->sanitize_config(type);

  if(dt_supervisor_active()) dt_supervisor_form(DT_SV_CREATE, mask_form);

  return mask_form;
}

dt_masks_form_t *dt_masks_create_ext(dt_develop_t *dev, dt_masks_type_t type)
{
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  dt_masks_form_t *mask_form = dt_masks_create(type);

  // all forms created here are registered in dev->allforms for later cleanup
  if(mask_form)
    dev->allforms = g_list_append(dev->allforms, mask_form);

  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  return mask_form;
}

dt_masks_form_t *dt_masks_get_from_id_ext(GList *form_list, int form_id)
{
  for(; form_list; form_list = g_list_next(form_list))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_list->data;
    if(mask_form->formid == form_id) return mask_form;
  }
  return NULL;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *develop, int form_id)
{
  dt_pthread_rwlock_rdlock(&develop->masks_mutex);
  dt_masks_form_t *result = dt_masks_get_from_id_ext(develop->forms, form_id);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
  return result;
}

dt_iop_module_t *dt_masks_get_mask_manager(dt_develop_t *develop)
{
  for(GList *module_node = g_list_first(develop->iop); module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(module_node->data);
    if(strcmp(module->op, "mask_manager") == 0)
      return module;
  }
  return NULL;
}

static void _masks_fill_used_forms(GList *forms_list, const int form_id, int *used_form_ids,
                                   const int used_count)
{
  for(int used_index = 0; used_index < used_count; used_index++)
  {
    if(used_form_ids[used_index] == 0)
    {
      used_form_ids[used_index] = form_id;
      break;
    }
    if(used_form_ids[used_index] == form_id) break;
  }

  dt_masks_form_t *mask_form = dt_masks_get_from_id_ext(forms_list, form_id);
  if(!IS_NULL_PTR(mask_form) && (mask_form->type & DT_MASKS_GROUP))
  {
    for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      _masks_fill_used_forms(forms_list, group_entry->formid, used_form_ids, used_count);
    }
  }
}

int dt_masks_copy_used_forms_for_module(dt_develop_t *develop_dest, dt_develop_t *develop_src,
                                        const dt_iop_module_t *source_module)
{
  if(!(source_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return 0;
  if(source_module->blend_params->mask_id <= 0) return 0;

  const guint form_count = g_list_length(develop_src->forms);
  if(form_count == 0) return 0;

  int *used_form_ids = calloc(form_count, sizeof(int));
  if(IS_NULL_PTR(used_form_ids)) return 1;

  _masks_fill_used_forms(develop_src->forms, source_module->blend_params->mask_id,
                         used_form_ids, form_count);

  for(int form_index = 0; form_index < (int)form_count && used_form_ids[form_index] > 0; form_index++)
  {
    dt_masks_form_t *mask_form = dt_masks_get_from_id(develop_src, used_form_ids[form_index]);
    if(!IS_NULL_PTR(mask_form))
    {
      dt_masks_form_t *existing_form = dt_masks_get_from_id_ext(develop_dest->forms,
                                                                used_form_ids[form_index]);
      if(existing_form)
      {
        develop_dest->forms = g_list_remove(develop_dest->forms, existing_form);
        develop_dest->allforms = g_list_append(develop_dest->allforms, existing_form);
      }

      dt_masks_form_t *new_form = dt_masks_dup_masks_form(mask_form);
      if(IS_NULL_PTR(new_form))
      {
        dt_free(used_form_ids);
        return 1;
      }
      develop_dest->forms = g_list_append(develop_dest->forms, new_form);
    }
    else
    {
      fprintf(stderr, "[dt_masks_copy_used_forms_for_module] form %i not found in source image\n",
              used_form_ids[form_index]);
    }
  }

  dt_free(used_form_ids);
  return 0;
}

typedef struct _masks_read_ctx_t
{
  dt_develop_t *develop;
  int32_t image_id;
  dt_dev_history_item_t *history_item;
  dt_dev_history_item_t *last_history_item;
  int previous_num;
} _masks_read_ctx_t;

static void _read_mask_row(void *user_data, const int history_num, const int form_id,
                           const int form, const char *name, const int version,
                           const void *points, const int points_len, const int point_count,
                           const void *source, const int source_len)
{
  _masks_read_ctx_t *ctx = (_masks_read_ctx_t *)user_data;
  dt_develop_t *develop = ctx->develop;

  const dt_masks_type_t mask_type = form;
  dt_masks_form_t *mask_form = dt_masks_create(mask_type);
  mask_form->formid = form_id;
  g_strlcpy(mask_form->name, name, sizeof(mask_form->name));
  mask_form->version = version;
  mask_form->points = NULL;
  memcpy(mask_form->source, source, sizeof(float) * 2);

  // and now we "read" the blob
  if(mask_form->functions)
  {
    const char *const point_buffer = (const char *)points;
    const size_t point_struct_size = mask_form->functions->point_struct_size;
    for(int point_index = 0; point_index < point_count; point_index++)
    {
      char *point_data = (char *)malloc(point_struct_size);
      memcpy(point_data, point_buffer + point_index * point_struct_size, point_struct_size);
      mask_form->points = g_list_append(mask_form->points, point_data);
    }
  }

  if(mask_form->version != dt_masks_version())
  {
    if(dt_masks_legacy_params(develop, mask_form, mask_form->version, dt_masks_version()))
    {
      const char *fname = develop->image_storage.filename + strlen(develop->image_storage.filename);
      while(fname > develop->image_storage.filename && *fname != '/') fname--;
      if(fname > develop->image_storage.filename) fname++;

      fprintf(stderr,
              "[_dev_read_masks_history] %s (imgid `%i\'): mask version mismatch: history is %d, dt %d.\n",
              fname, ctx->image_id, mask_form->version, dt_masks_version());
      dt_history_message(_("%s: mask version mismatch: %d != %d"),
                     fname, dt_masks_version(), mask_form->version);

      // was a `continue` over the rest of the loop body
      return;
    }
  }

    // Not computed here: dt_masks_replace_current_forms() below (via
    // dt_masks_form_update_gravity_center() in masks_history.c) already computes it for
    // every form that actually ends up live in dev->forms. Computing it here too would
    // redundantly do it for every history step's row read from masks_history -- including
    // every duplicate of a form shared unchanged across many steps (see the un-deduped
    // masks_history table, doc/masks_history_dedup.md) -- for forms that are either
    // superseded (never read again) or about to be recomputed anyway.

    // if this is a new history entry let's find it
  if(ctx->previous_num != history_num)
  {
    ctx->history_item = NULL;
    for(GList *history_node = g_list_first(develop->history); history_node; history_node = g_list_next(history_node))
    {
      dt_dev_history_item_t *history_entry = (dt_dev_history_item_t *)(history_node->data);
      if(history_entry->num == history_num)
      {
        ctx->history_item = history_entry;
        break;
      }
    }
    ctx->previous_num = history_num;
  }
  // add the form to the history entry
    // FIXME: there is no reason to hack history_item to add a forms snapshot that doesn't
    // belong to it because dt_dev_write_history_item() doesn't save history_item->forms to the DB.
    // So this forms snapshot should be attached to its own object, and that object should be
    // linked by ID to the history_item object. That would allow to share one forms snapshot
    // between several history items without duplication.
  if(ctx->history_item)
  {
    ctx->history_item->forms = g_list_append(ctx->history_item->forms, mask_form);
  }
  else
    fprintf(stderr,
            "[_dev_read_masks_history] can't find history entry %i while adding mask %s(%i)\n",
            history_num, mask_form->name, form_id);

  if(history_num < dt_dev_get_history_end_ext(develop)) ctx->last_history_item = ctx->history_item;
}

void dt_masks_read_masks_history(dt_develop_t *develop, const int32_t image_id)
{
  // The per-row state the loop used to keep in locals lives in the context: which history entry
  // the previous row belonged to, and the last one at or below history_end.
  _masks_read_ctx_t ctx = { .develop = develop, .image_id = image_id, .history_item = NULL,
                            .last_history_item = NULL, .previous_num = -1 };

  dt_history_repository_foreach_mask_item(image_id, _read_mask_row, &ctx);

  // and we update the current forms snapshot
  dt_masks_replace_current_forms(develop, (ctx.last_history_item) ? ctx.last_history_item->forms : NULL);
}

void dt_masks_write_masks_history_item(const int32_t image_id, const int history_num,
                                       dt_masks_form_t *mask_form)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_masks_write_masks_history_item] writing mask %s of type %i for image %i\n",
           mask_form->name, mask_form->type, image_id);

  // The points are a flat array of the shape's own point struct; only this layer knows its size.
  const size_t point_struct_size = mask_form->functions ? mask_form->functions->point_struct_size : 0;
  const guint point_count = mask_form->functions ? g_list_length(mask_form->points) : 0;

  // Preserved: with no functions there is no point struct to serialise, and the original wrote
  // nothing at all in that case -- the whole INSERT sat inside this test.
  if(!mask_form->functions) return;

  char *const restrict point_buffer = (char *)malloc(point_count * point_struct_size);
  int buffer_offset = 0;
  for(GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    memcpy(point_buffer + buffer_offset, point_node->data, point_struct_size);
    buffer_offset += point_struct_size;
  }

  dt_history_repository_write_mask_item(image_id, history_num, mask_form->formid, mask_form->type,
                                        mask_form->name, mask_form->version, point_buffer,
                                        point_count * point_struct_size, point_count,
                                        mask_form->source, 2 * sizeof(float));

  dt_free(point_buffer);
}

void dt_masks_free_form(dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return;
  g_list_free_full(mask_form->points, dt_free_gpointer);
  mask_form->points = NULL;
  dt_free(mask_form);
}

dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->dev)) return DT_MASKS_EDIT_OFF;
  return module->dev->form_gui
    ? module->dev->form_gui->edit_mode
    : DT_MASKS_EDIT_OFF;
}


void dt_masks_iop_use_same_as(dt_iop_module_t *module, dt_iop_module_t *source_module)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(source_module)) return;

  // we get the source group
  int source_id = source_module->blend_params->mask_id;
  dt_masks_form_t *source_group = dt_masks_get_from_id(module->dev, source_id);
  if(IS_NULL_PTR(source_group) || source_group->type != DT_MASKS_GROUP) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *group_form = _group_from_module(module->dev, module);
  if(IS_NULL_PTR(group_form))
  {
    group_form = _group_create(module->dev, module, DT_MASKS_GROUP);
  }
  // Touch once, before the loop: dt_masks_group_add_form mutates group_form->points on every
  // iteration, so it must already be private by the first call, or later iterations would
  // append to an orphaned clone instead of the object actually in dev->forms.
  group_form = dt_masks_cow_touch(module->dev, group_form);
  // we copy the src group in this group
  for(GList *group_node = source_group->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(module->dev, group_entry->formid);
    if(mask_form)
    {
      dt_masks_form_group_t *new_entry = dt_masks_group_add_form(module->dev, group_form, mask_form);
      if(new_entry)
      {
        new_entry->state = group_entry->state;
        new_entry->opacity = group_entry->opacity;
      }
    }
  }

  // we save the group

}

void dt_masks_form_delete(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *group_form,
                          dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return;
  int form_id = mask_form->formid;
  if(!IS_NULL_PTR(group_form) && !(group_form->type & DT_MASKS_GROUP)) return;

  if(!(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) && !IS_NULL_PTR(group_form))
  {
    group_form = dt_masks_cow_touch(dev, group_form);
    // we try to remove the form from the masks group
    int removed = 0;
    for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      if(group_entry->formid == form_id)
      {
        removed = 1;
        group_form->points = g_list_remove(group_form->points, group_entry);
        dt_free(group_entry);
        break;
      }
    }
    if(removed)
    if(removed && !IS_NULL_PTR(module))
    {
      dt_masks_iop_update(module);

    }
    if(removed) dt_masks_form_update_gravity_center(dev, group_form);
    if(removed && IS_NULL_PTR(group_form->points)) dt_masks_form_delete(dev, module, NULL, group_form);
    return;
  }

  if(mask_form->type & DT_MASKS_GROUP && mask_form->type & DT_MASKS_CLONE)
  {
    // when removing a cloning group the children have to be removed, too, as they won't be shown in the mask manager
    // and are thus not accessible afterwards.
    while(mask_form->points)
    {
      dt_masks_form_group_t *group_child = (dt_masks_form_group_t *)mask_form->points->data;
      dt_masks_form_t *child = dt_masks_get_from_id(dev, group_child->formid);
      dt_masks_form_delete(dev, module, mask_form, child);
      // The recursive call passes mask_form as its own group_form parameter and may have
      // COW-cloned it (see the touch above): re-fetch the live object so this loop keeps
      // observing the mutation instead of spinning on a stale, now-orphaned copy.
      mask_form = dt_masks_get_from_id(dev, form_id);
      if(IS_NULL_PTR(mask_form)) break;
    }
  }

  // if we are here that mean we have to permanently delete this form
  // we drop the form from all modules
  for(GList *iop_node = dev->iop; iop_node; iop_node = g_list_next(iop_node))
  {
    dt_iop_module_t *iop_module = (dt_iop_module_t *)iop_node->data;
    if(iop_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      // is the form the base group of the iop ?
      if(form_id == iop_module->blend_params->mask_id)
      {
        iop_module->blend_params->mask_id = 0;
        dt_masks_iop_update(iop_module);
      }
      else
      {
        dt_masks_form_t *iop_group = _group_from_module(dev, iop_module);
        if(iop_group && (iop_group->type & DT_MASKS_GROUP))
        {
          iop_group = dt_masks_cow_touch(dev, iop_group);
          int removed = 0;
          GList *shapes = iop_group->points;
          while(shapes)
          {
            dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)shapes->data;
            if(group_entry->formid == form_id)
            {
              removed = 1;
              // Remove the shape from the list
              iop_group->points = g_list_remove(iop_group->points, group_entry);
              dt_free(group_entry);
              shapes = iop_group->points; // jump back to start of list
              continue;
            }
            shapes = g_list_next(shapes); // advance to next form
          }
          if(removed)
          {
            dt_masks_iop_update(iop_module);

            if(IS_NULL_PTR(iop_group->points)) dt_masks_form_delete(dev, iop_module, NULL, iop_group);
          }
        }
      }
    }
  }
  // we drop the form from the general list
  for(GList *form_node = dev->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->formid == form_id)
    {
      dt_masks_remove_form(dev, existing_form);
      break;
    }
  }
}

void dt_masks_form_update_gravity_center(dt_develop_t *dev, dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return;

  float center_point[2];
  float area = 0.0f;
  const gboolean ok = dt_masks_form_get_gravity_center(dev, mask_form, center_point, &area);
  mask_form->gravity_center[0] = center_point[0];
  mask_form->gravity_center[1] = center_point[1];
  mask_form->area = area;
  mask_form->gravity_center_valid = TRUE;

  dt_print(DT_DEBUG_MASKS,
           "[masks] gravity center updated: form=%p id=%d type=0x%x ok=%d center=(%f,%f), area=%f\n",
           (void *)mask_form, mask_form->formid, mask_form->type, ok,
           mask_form->gravity_center[0], mask_form->gravity_center[1], mask_form->area);
}

void dt_masks_form_invalidate_gravity_center(dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return;
  mask_form->gravity_center_valid = FALSE;
}


/* dt_masks_set_edit_mode, _change_opacity and dt_masks_form_change_opacity moved to
 * masks_gui.c: edit-mode pokes the blending toggle, and opacity is scroll-interaction
 * machinery whose toast belongs beside its events. Declarations unchanged. */
void dt_masks_duplicate_points(const dt_masks_form_t *base_form, dt_masks_form_t *dest_form,
                               size_t node_size)
{
  if(IS_NULL_PTR(base_form) || IS_NULL_PTR(dest_form) || IS_NULL_PTR(base_form->points) || node_size == 0) return;

  for(const GList *point_node = base_form->points; point_node; point_node = g_list_next(point_node))
  {
    const void *point_data = point_node->data;
    if(IS_NULL_PTR(point_data)) continue;
    void *point_copy = malloc(node_size);
    if(IS_NULL_PTR(point_copy)) continue;
    memcpy(point_copy, point_data, node_size);
    dest_form->points = g_list_append(dest_form->points, point_copy);
  }
}


void dt_masks_form_move(dt_masks_form_t *group_form, int form_id, int move_up)
{
  if(IS_NULL_PTR(group_form) || !(group_form->type & DT_MASKS_GROUP)) return;

  // we search the form in the group
  dt_masks_form_group_t *group_entry = NULL;
  guint group_index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *entry = (dt_masks_form_group_t *)group_node->data;
    if(entry->formid == form_id)
    {
      group_entry = entry;
      break;
    }
    group_index++;
  }

  // we remove the form and read it
  if(!IS_NULL_PTR(group_entry))
  {
    const guint group_length = g_list_length(group_form->points);
    if(!move_up && group_index == 0) return;
    if(move_up && group_index == group_length - 1) return;

    group_form->points = g_list_remove(group_form->points, group_entry);
    if(!move_up)
      group_index -= 1;
    else
      group_index += 1;
    group_form->points = g_list_insert(group_form->points, group_entry, group_index);

  }
}

int _find_in_group(dt_develop_t *dev, dt_masks_form_t *group_form, int form_id)
{
  if(!(group_form->type & DT_MASKS_GROUP)) return 0;
  if(group_form->formid == form_id) return 1;
  int nested_count = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(dev, group_entry->formid);
    if(mask_form)
    {
      if(mask_form->type & DT_MASKS_GROUP) nested_count += _find_in_group(dev, mask_form, form_id);
    }
  }
  return nested_count;
}

uint64_t dt_masks_group_get_hash_ext(uint64_t hash, GList *masks, dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return hash;

  // basic infos
  hash = dt_hash(hash, (char *)&mask_form->type, sizeof(dt_masks_type_t));
  hash = dt_hash(hash, (char *)&mask_form->formid, sizeof(int));
  hash = dt_hash(hash, (char *)&mask_form->version, sizeof(int));
  hash = dt_hash(hash, (char *)&mask_form->source, sizeof(float) * 2);

  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    if(mask_form->type & DT_MASKS_GROUP)
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)point_node->data;
      // Children must come from the SAME list the top-level group was resolved from. Resolving
      // them from live dev->forms while hashing a history/pipe snapshot mixes two states into
      // one hash: two genuinely different states can then hash identically (and vice versa),
      // which defeats the cache invalidation this hash exists for.
      dt_masks_form_t *child_form = dt_masks_get_from_id_ext(masks, group_entry->formid);
      if(child_form)
      {
        // state & opacity
        hash = dt_hash(hash, (char *)&group_entry->state, sizeof(int));
        hash = dt_hash(hash, (char *)&group_entry->opacity, sizeof(float));

        // the form itself
        hash = dt_masks_group_get_hash_ext(hash, masks, child_form);
      }
    }
    else if(mask_form->functions)
    {
      hash = dt_hash(hash, (char *)point_node->data, mask_form->functions->point_struct_size);
    }
  }
  return hash;
}

uint64_t dt_masks_form_get_own_hash(uint64_t hash, GList *masks, const dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return hash;

  // basic infos
  hash = dt_hash(hash, (const char *)&mask_form->type, sizeof(dt_masks_type_t));
  hash = dt_hash(hash, (const char *)&mask_form->formid, sizeof(int));
  hash = dt_hash(hash, (const char *)&mask_form->version, sizeof(int));
  hash = dt_hash(hash, (const char *)&mask_form->source, sizeof(float) * 2);

  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    if(mask_form->type & DT_MASKS_GROUP)
    {
      const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)point_node->data;
      // Membership only (id + state/opacity): the referenced form's own content is hashed
      // once, separately, when dev->forms reaches that form's own top-level entry. Recursing
      // into it here would re-hash every grouped shape's full point list a second time.
      if(dt_masks_get_from_id_ext(masks, group_entry->formid))
      {
        hash = dt_hash(hash, (const char *)&group_entry->formid, sizeof(int));
        hash = dt_hash(hash, (const char *)&group_entry->state, sizeof(int));
        hash = dt_hash(hash, (const char *)&group_entry->opacity, sizeof(float));
      }
    }
    else if(mask_form->functions)
    {
      hash = dt_hash(hash, (const char *)point_node->data, mask_form->functions->point_struct_size);
    }
  }
  return hash;
}

// adds formid to used array
// if formid is a group it adds all the forms that belongs to that group
static void _cleanup_unused_recurs(GList *form_list, int form_id, int *used_form_ids, int used_count)
{
  // first, we search for the formid in used table
  for(int used_index = 0; used_index < used_count; used_index++)
  {
    if(used_form_ids[used_index] == 0)
    {
      // we store the formid
      used_form_ids[used_index] = form_id;
      break;
    }
    if(used_form_ids[used_index] == form_id) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *mask_form = dt_masks_get_from_id_ext(form_list, form_id);
  if(!IS_NULL_PTR(mask_form) && (mask_form->type & DT_MASKS_GROUP))
  {
    for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      _cleanup_unused_recurs(form_list, group_entry->formid, used_form_ids, used_count);
    }
  }
}

// removes from _forms all forms that are not used in history_list up to history_end
static int _masks_cleanup_unused(dt_develop_t *dev, GList **forms_list, GList *history_list, const int history_end)
{
  int masks_removed = 0;
  GList *forms = *forms_list;

  // we create a table to store the ids of used forms
  guint form_count = g_list_length(forms);
  int *used_form_ids = calloc(form_count, sizeof(int));

  // check in history if the module has drawn masks and add it to used array
  int history_index = 0;
  for(GList *history_node = history_list; history_node && history_index < history_end;
      history_node = g_list_next(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;
    dt_develop_blend_params_t *blend_params
        = history_item && history_item->blendop_params_size == sizeof(dt_develop_blend_params_t)
              ? history_item->blend_params
              : NULL;
    if(blend_params)
    {
      if(blend_params->mask_id > 0)
        _cleanup_unused_recurs(forms, blend_params->mask_id, used_form_ids, form_count);
    }
    history_index++;
  }

  // and we delete all unused forms
  GList *shape_node = forms;
  while(shape_node)
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)shape_node->data;
    int is_used = 0;
    for(int used_index = 0; used_index < form_count; used_index++)
    {
      if(used_form_ids[used_index] == mask_form->formid)
      {
        is_used = 1;
        break;
      }
      if(used_form_ids[used_index] == 0) break;
    }

    shape_node = g_list_next(shape_node); // need to get 'next' now, because we may be removing the current node

    if(is_used == 0)
    {
      forms = g_list_remove(forms, mask_form);
      // and add it to allforms for cleanup
      dev->allforms = g_list_append(dev->allforms, mask_form);
      masks_removed = 1;
    }
  }

  dt_free(used_form_ids);

  *forms_list = forms;

  return masks_removed;
}

// removes all unused form from history
/**
 * @brief Remove unused mask forms from a history list, preserving undo safety.
 *
 * Caveat: if multiple history entries reference masks, some unused masks may remain.
 *         This is intentional so users can still jump back in history.
 */
void dt_masks_cleanup_unused_from_list(dt_develop_t *dev, GList *history_list)
{
  // a mask is used in a given hist->forms entry if it is used up to the next hist->forms
  // so we are going to remove for each hist->forms from the top
  int history_count = g_list_length(history_list);
  int history_end = history_count;
  for(const GList *history_node = g_list_last(history_list); history_node;
      history_node = g_list_previous(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;
    if(!IS_NULL_PTR(history_item->forms)) //&& strcmp(history_item->op_name, "mask_manager") == 0)
    {
      _masks_cleanup_unused(dev, &history_item->forms, history_list, history_end);
      history_end = history_count - 1;
    }
    history_count--;
  }
}

/**
 * @brief Cleanup unused masks and refresh the current forms snapshot.
 *
 * Assumption: caller already decided to drop unused forms (non-reversible).
 */
void dt_masks_cleanup_unused(dt_develop_t *develop)
{
  dt_masks_change_form_gui(develop, NULL);

  // we remove the forms from history
  dt_masks_cleanup_unused_from_list(develop, develop->history);

  // and we save all that
  GList *forms = NULL;
  int history_index = 0;
  for(const GList *history_node = g_list_first(develop->history);
      history_node && history_index < dt_dev_get_history_end_ext(develop);
      history_node = g_list_next(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;

    if(history_item->forms) forms = history_item->forms;
    history_index++;
  }

  dt_masks_replace_current_forms(develop, forms);
}

#include "detail.c"

/* The two rasterisation dispatchers. They were inline in masks.h, which forced the
 * per-shape function table to be public; a per-buffer call is not a per-pixel cost,
 * so the inline bought nothing and the table is private now. */
int dt_masks_get_mask(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *const piece,
                      dt_masks_form_t *const form,
                      float **buffer, int *width, int *height, int *posx, int *posy)
{
  return (form->functions && form->functions->get_mask)
    ? form->functions->get_mask(module, pipe, piece, form, buffer, width, height, posx, posy)
    : 1;
}

int dt_masks_get_mask_roi(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                          const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  return (form->functions && form->functions->get_mask_roi)
    ? form->functions->get_mask_roi(module, pipe, piece, form, roi, buffer)
    : 1;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
