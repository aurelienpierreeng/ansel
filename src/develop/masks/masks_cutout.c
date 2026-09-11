/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

/* The shape is built as the darkroom would build it, then rasterised the way
 * dt_masks_debug_rasterise() does: against a dev whose only geometry is the raster's size, on
 * a pipe with no nodes, so the composition is the identity and the unit square lands on the
 * raster. The dev is a throwaway on the stack; nothing here outlives the call. */

#include "develop/masks_cutout.h"

#include "common/logging.h"
#include "develop/develop.h"
#include "develop/dev_geometry.h"
#include "develop/masks.h"
#include "develop/masks/masks_functions.h"
#include "develop/pixelpipe_hb.h"
#include "system/dtpthread.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <string.h>

/** A node's own fall-off radius, or the shape's when it has none of its own. */
static float _node_border(const dt_masks_cutout_t *cutout, const float *node, const int which, const float feather)
{
  const int index = which == 0 ? DT_MASKS_CUTOUT_NODE_BORDER1 : DT_MASKS_CUTOUT_NODE_BORDER2;
  if(cutout->node_stride <= (uint32_t)index) return feather;
  const float own = node[index];
  return own > 0.0f ? own : feather;
}

static dt_masks_form_t *_polygon_form(const dt_masks_cutout_t *cutout, const float feather)
{
  // A node record used to stop at the smooth flag; one that still does gets the shape's
  // fall-off for every node, which is what it always had.
  if(cutout->node_count < 3 || IS_NULL_PTR(cutout->nodes)
     || cutout->node_stride < DT_MASKS_CUTOUT_NODE_SMOOTH + 1)
    return NULL;
  dt_masks_form_t *form = dt_masks_form_new_silent(DT_MASKS_POLYGON);
  if(IS_NULL_PTR(form)) return NULL;
  gboolean any_smooth = FALSE;
  for(uint32_t index = 0; index < cutout->node_count; index++)
  {
    const float *source = cutout->nodes + (size_t)index * cutout->node_stride;
    dt_masks_node_polygon_t *node = calloc(1, sizeof(dt_masks_node_polygon_t));
    if(IS_NULL_PTR(node))
    {
      dt_masks_free_form(form);
      return NULL;
    }
    node->node[0] = source[DT_MASKS_CUTOUT_NODE_X];
    node->node[1] = source[DT_MASKS_CUTOUT_NODE_Y];
    // Only "computed" asks the shape for a tangent. A node carrying its own control points is
    // a cusp or a smooth node according to whether they coincide -- the darkroom decides that
    // by geometry (`dt_masks_node_is_cusp()`), not by a flag -- so both land in the same
    // branch here and the difference lives in where the caller puts the points.
    const gboolean computed = source[DT_MASKS_CUTOUT_NODE_SMOOTH] == 1.0f;
    if(computed)
    {
      // -1 asks the polygon's own initialiser for a Catmull-Rom tangent through the node.
      node->ctrl1[0] = node->ctrl1[1] = node->ctrl2[0] = node->ctrl2[1] = -1.0f;
      node->state = DT_MASKS_POINT_STATE_NORMAL;
      any_smooth = TRUE;
    }
    else
    {
      node->ctrl1[0] = source[DT_MASKS_CUTOUT_NODE_CTRL1_X];
      node->ctrl1[1] = source[DT_MASKS_CUTOUT_NODE_CTRL1_Y];
      node->ctrl2[0] = source[DT_MASKS_CUTOUT_NODE_CTRL2_X];
      node->ctrl2[1] = source[DT_MASKS_CUTOUT_NODE_CTRL2_Y];
      node->state = DT_MASKS_POINT_STATE_USER;
    }
    // A node's border is the fall-off's radius on either side of it: its own where it has
    // one, the shape's where it does not.
    node->border[0] = _node_border(cutout, source, 0, feather);
    node->border[1] = _node_border(cutout, source, 1, feather);
    form->points = g_list_append(form->points, node);
  }
  if(any_smooth && !IS_NULL_PTR(form->functions) && !IS_NULL_PTR(form->functions->init_ctrl_points))
    form->functions->init_ctrl_points(form);
  return form;
}

static dt_masks_form_t *_form_from_cutout(const dt_masks_cutout_t *cutout)
{
  const float feather = fmaxf(cutout->feather, 0.0f);
  switch(cutout->shape)
  {
    case DT_MASKS_CUTOUT_CIRCLE:
    {
      dt_masks_form_t *form = dt_masks_form_new_silent(DT_MASKS_CIRCLE);
      dt_masks_node_circle_t *circle = IS_NULL_PTR(form) ? NULL : calloc(1, sizeof(dt_masks_node_circle_t));
      if(IS_NULL_PTR(circle))
      {
        dt_masks_free_form(form);
        return NULL;
      }
      circle->center[0] = cutout->center[0];
      circle->center[1] = cutout->center[1];
      circle->radius = fmaxf(cutout->radius[0], 0.001f);
      circle->border = feather;
      form->points = g_list_append(form->points, circle);
      return form;
    }
    case DT_MASKS_CUTOUT_ELLIPSE:
    {
      dt_masks_form_t *form = dt_masks_form_new_silent(DT_MASKS_ELLIPSE);
      dt_masks_node_ellipse_t *ellipse = IS_NULL_PTR(form) ? NULL : calloc(1, sizeof(dt_masks_node_ellipse_t));
      if(IS_NULL_PTR(ellipse))
      {
        dt_masks_free_form(form);
        return NULL;
      }
      ellipse->center[0] = cutout->center[0];
      ellipse->center[1] = cutout->center[1];
      ellipse->radius[0] = fmaxf(cutout->radius[0], 0.001f);
      ellipse->radius[1] = fmaxf(cutout->radius[1], 0.001f);
      ellipse->rotation = cutout->rotation;
      ellipse->border = feather;
      ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
      form->points = g_list_append(form->points, ellipse);
      return form;
    }
    case DT_MASKS_CUTOUT_GRADIENT:
    {
      dt_masks_form_t *form = dt_masks_form_new_silent(DT_MASKS_GRADIENT);
      dt_masks_anchor_gradient_t *gradient = IS_NULL_PTR(form) ? NULL : calloc(1, sizeof(dt_masks_anchor_gradient_t));
      if(IS_NULL_PTR(gradient))
      {
        dt_masks_free_form(form);
        return NULL;
      }
      gradient->center[0] = cutout->center[0];
      gradient->center[1] = cutout->center[1];
      gradient->rotation = cutout->rotation;
      gradient->extent = CLAMP(cutout->radius[0], 0.0005f, 1.0f);
      gradient->curvature = CLAMP(cutout->radius[1], -2.0f, 2.0f);
      gradient->steepness = 0.0f;
      gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;
      form->points = g_list_append(form->points, gradient);
      return form;
    }
    case DT_MASKS_CUTOUT_POLYGON:
      return _polygon_form(cutout, feather);
    default:
      return NULL;
  }
}

float *dt_masks_cutout_rasterise(const dt_masks_cutout_t *cutout, const int width, const int height)
{
  if(IS_NULL_PTR(cutout) || cutout->shape == DT_MASKS_CUTOUT_NONE || width <= 0 || height <= 0) return NULL;
  dt_masks_form_t *form = _form_from_cutout(cutout);
  if(IS_NULL_PTR(form)) return NULL;

  float *const buffer = dt_calloc_align_float((size_t)width * height);
  if(IS_NULL_PTR(buffer))
  {
    dt_masks_free_form(form);
    return NULL;
  }

  /* A dev with nothing but a raw size: the shapes read it to turn unit-square coordinates into
   * pixels, and with a nodeless pipe the distortion chain is the identity. The forms list is
   * the one shape: a single shape resolves no children. */
  dt_develop_t dev;
  memset(&dev, 0, sizeof(dev));
  dt_pthread_rwlock_init(&dev.masks_mutex, NULL);
  dt_dev_geometry_init(&dev);
  dt_dev_geometry_set_raw_size(&dev, width, height, TRUE);
  dev.forms = g_list_append(NULL, form);

  dt_dev_pixelpipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.iwidth = width;
  pipe.iheight = height;
  pipe.mask_rasterization_step = 1;
  pipe.forms = dev.forms;

  dt_dev_pixelpipe_iop_t piece;
  memset(&piece, 0, sizeof(piece));
  dt_iop_module_t module;
  memset(&module, 0, sizeof(module));
  module.dev = &dev;
  module.iop_order = 0.0;

  const dt_iop_roi_t roi = { .x = 0, .y = 0, .width = width, .height = height, .scale = 1.0f };
  dt_iop_roi_t touched = { 0 };
  const dt_masks_raster_result_t result = dt_masks_get_mask_roi(&module, &pipe, &piece, form, &roi, buffer, &touched);

  g_list_free(dev.forms);
  dev.forms = NULL;
  dt_pthread_rwlock_destroy(&dev.masks_mutex);
  dt_masks_free_form(form);

  if(result == DT_MASKS_RASTER_ERROR)
  {
    dt_print(DT_DEBUG_MASKS, "[masks cutout] rasterising shape %d failed\n", (int)cutout->shape);
    dt_free_align(buffer);
    return NULL;
  }
  if(cutout->invert)
  {
    const size_t count = (size_t)width * height;
    for(size_t idx = 0; idx < count; idx++) buffer[idx] = 1.0f - buffer[idx];
  }
  return buffer;
}

void dt_masks_cutout_free(float *raster)
{
  dt_free_align(raster);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
