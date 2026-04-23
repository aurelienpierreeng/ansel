/*
    This file is part of the Ansel project.
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

#include "iop/drawlayer/coordinates.h"

static gboolean _virtual_piece_layer_geometry(dt_iop_module_t *self, int *layer_width, int *layer_height)
{
  if(!IS_NULL_PTR(layer_width)) *layer_width = 0;
  if(!IS_NULL_PTR(layer_height)) *layer_height = 0;
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev)) return FALSE;

  /* GUI coordinate mapping should prefer the virtual pipe geometry because it
   * tracks the currently committed distortion stack even before the global
   * darkroom ROI bookkeeping is fully refreshed. Falling back to `dev->roi`
   * too early makes the first displayed layer appear stretched until the next
   * display-pipe refresh catches up. */
  int resolved_width = 0;
  int resolved_height = 0;
  if(self->dev->virtual_pipe && self->dev->virtual_pipe->processed_width > 0
     && self->dev->virtual_pipe->processed_height > 0)
  {
    resolved_width = self->dev->virtual_pipe->processed_width;
    resolved_height = self->dev->virtual_pipe->processed_height;
  }
  else
  {
    resolved_width = self->dev->roi.processed_width;
    resolved_height = self->dev->roi.processed_height;
  }
  if(!IS_NULL_PTR(layer_width)) *layer_width = resolved_width;
  if(!IS_NULL_PTR(layer_height)) *layer_height = resolved_height;
  return resolved_width > 0 && resolved_height > 0;
}

gboolean dt_drawlayer_widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(self->dev->virtual_pipe) || IS_NULL_PTR(pts) || count <= 0) return FALSE;

  dt_dev_coordinates_widget_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_preview_abs(self->dev, pts, count);

  if(!dt_dev_distort_backtransform_plus(self->dev->virtual_pipe, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;
  dt_dev_coordinates_preview_abs_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pts, count);

  return TRUE;
}

gboolean dt_drawlayer_layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(IS_NULL_PTR(pts) || count <= 0) return FALSE;
  dt_dev_coordinates_image_abs_to_image_norm(darktable.develop, pts, count);
  dt_dev_coordinates_image_norm_to_preview_abs(darktable.develop, pts, count);

  if(!dt_dev_distort_transform_plus(darktable.develop->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;

  dt_dev_coordinates_preview_abs_to_image_norm(darktable.develop, pts, count);
  dt_dev_coordinates_image_norm_to_widget(darktable.develop, pts, count);
  return TRUE;
}

gboolean dt_drawlayer_widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy,
                                             float *lx, float *ly)
{
  if(IS_NULL_PTR(lx) || IS_NULL_PTR(ly)) return FALSE;

  float pt[2] = { (float)wx, (float)wy };
  if(!dt_drawlayer_widget_points_to_layer_coords(self, pt, 1)) return FALSE;

  *lx = pt[0];
  *ly = pt[1];
  return TRUE;
}

gboolean dt_drawlayer_layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y,
                                             float *wx, float *wy)
{
  if(IS_NULL_PTR(wx) || IS_NULL_PTR(wy)) return FALSE;

  float pt[2] = { x, y };
  if(!dt_drawlayer_layer_points_to_widget_coords(self, pt, 1)) return FALSE;
  *wx = pt[0];
  *wy = pt[1];
  return TRUE;
}

gboolean dt_drawlayer_layer_bounds_to_widget_bounds(dt_iop_module_t *self, const float x0, const float y0,
                                                    const float x1, const float y1,
                                                    float *left, float *top,
                                                    float *right, float *bottom)
{
  float pts[8] = {
    x0, y0, x1, y0, x0, y1, x1, y1,
  };

  if(!dt_drawlayer_layer_points_to_widget_coords(self, pts, 4)) return FALSE;

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  if(!IS_NULL_PTR(left)) *left = min_x;
  if(!IS_NULL_PTR(top)) *top = min_y;
  if(!IS_NULL_PTR(right)) *right = max_x;
  if(!IS_NULL_PTR(bottom)) *bottom = max_y;
  return TRUE;
}

float dt_drawlayer_widget_brush_radius(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                       const float fallback)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(self->dev->virtual_pipe) || IS_NULL_PTR(dab)) return fallback;

  float pts[6] = {
    dab->x, dab->y, dab->x + dab->radius, dab->y, dab->x, dab->y + dab->radius,
  };

  if(!dt_drawlayer_layer_points_to_widget_coords(self, pts, 3)) return fallback;

  const float rx = hypotf(pts[2] - pts[0], pts[3] - pts[1]);
  const float ry = hypotf(pts[4] - pts[0], pts[5] - pts[1]);
  const float radius = 0.5f * (rx + ry);
  return fmaxf(0.5f, isfinite(radius) ? radius : fallback);
}

float dt_drawlayer_current_live_padding(dt_iop_module_t *self)
{
  dt_drawlayer_brush_dab_t dab = {
    .radius = fmaxf(_conf_size(), 0.5f),
    .hardness = _conf_hardness(),
    .shape = _conf_brush_shape(),
  };
  return ceilf(dab.radius + 1.0f);
}

gboolean dt_drawlayer_compute_view_patch(dt_iop_module_t *self, const float padding, drawlayer_view_patch_info_t *view)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(view)) return FALSE;

  int layer_width = 0;
  int layer_height = 0;
  if(!_virtual_piece_layer_geometry(self, &layer_width, &layer_height)) return FALSE;

  const float widget_w = (float)self->dev->roi.orig_width;
  const float widget_h = (float)self->dev->roi.orig_height;
  const float preview_w = self->dev->roi.preview_width;
  const float preview_h = self->dev->roi.preview_height;
  if(widget_w <= 0.0f || widget_h <= 0.0f || preview_w <= 0.0f || preview_h <= 0.0f) return FALSE;

  const float zoom_scale = dt_dev_get_overlay_scale(self->dev);
  const float border = (float)self->dev->roi.border_size;
  const float roi_w = fminf(widget_w, preview_w * zoom_scale);
  const float roi_h = fminf(widget_h, preview_h * zoom_scale);
  const float rec_x = fmaxf(border, 0.5f * (widget_w - roi_w));
  const float rec_y = fmaxf(border, 0.5f * (widget_h - roi_h));
  const float rec_w = fminf(widget_w - 2.0f * border, roi_w);
  const float rec_h = fminf(widget_h - 2.0f * border, roi_h);
  if(rec_w <= 0.0f || rec_h <= 0.0f) return FALSE;

  float pts[8] = {
    rec_x, rec_y, rec_x + rec_w, rec_y, rec_x, rec_y + rec_h, rec_x + rec_w, rec_y + rec_h,
  };
  if(!dt_drawlayer_widget_points_to_layer_coords(self, pts, 4)) return FALSE;

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  view->layer_x0 = min_x;
  view->layer_y0 = min_y;
  view->layer_x1 = max_x;
  view->layer_y1 = max_y;

  view->patch.x = MAX(0, (int)floorf(min_x - padding));
  view->patch.y = MAX(0, (int)floorf(min_y - padding));
  const int right = MIN(layer_width, (int)ceilf(max_x + padding));
  const int bottom = MIN(layer_height, (int)ceilf(max_y + padding));
  view->patch.width = MAX(0, right - view->patch.x);
  view->patch.height = MAX(0, bottom - view->patch.y);
  return view->patch.width > 0 && view->patch.height > 0;
}
