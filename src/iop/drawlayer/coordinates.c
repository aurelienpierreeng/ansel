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

#include "develop/geometry/geometry.h"   // dt_geometry_chain_find(), dt_geometry_record_t

#include <string.h>

/* --- where the raw-anchored canvas sits in this module's frame ------------------------------
 *
 * One derivation, three callers -- the GUI placing a dab, the GUI drawing its overlays, and the
 * pipe compositing. They MUST agree: a dab is placed by the first and rendered by the last, and a
 * disagreement puts the paint somewhere other than where the cursor was.
 */

/**
 * @brief Does the orientation flip.c settled on swap the axes?
 *
 * @details Asked of the SIZE FOLD, not of the EXIF tag: a raw's recorded orientation is not always
 * right, and iop/flip.c exists precisely so the user can override it. Its _flip_resolve() merges
 * the two, and what this reads -- the rect flip's own fold maps its input to -- is the consequence
 * of that merge. So a corrected orientation is honoured with no access to flip's private data and
 * no second copy of the merge rule.
 *
 * Read from flip's record on the GUI side and from flip's piece on a pipe, which are the same fold
 * run by the two sides; nothing else in the pipe transposes. The focused-module exception cannot
 * disturb it either: flip is tagged IOP_TAG_DISTORT and no module's operation_tags_filter()
 * suppresses that tag -- crop filters DECORATION, ashift DECORATION|CLIPPING.
 *
 * A disabled flip folds its input to itself, so the answer is FALSE, which is what an orientation
 * of NONE means. A square frame cannot express a swap, and does not need to: the canvas has the
 * same dimensions either way.
 */
static gboolean _flip_swaps_axes(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_roi_t in = { 0 };
  dt_iop_roi_t out = { 0 };
  gboolean found = FALSE;

  if(IS_NULL_PTR(pipe))
  {
    const dt_geometry_record_t *const record = dt_geometry_chain_find(self->dev->geometry_chain, "flip", 0);
    if(!IS_NULL_PTR(record))
    {
      in = record->in;
      out = record->out;
      found = TRUE;
    }
  }
  else
  {
    for(const GList *node = g_list_first(pipe->nodes); node; node = g_list_next(node))
    {
      const dt_dev_pixelpipe_iop_t *const piece = (const dt_dev_pixelpipe_iop_t *)node->data;
      if(IS_NULL_PTR(piece) || IS_NULL_PTR(piece->module) || strcmp(piece->module->op, "flip")) continue;
      in = piece->buf_in;
      out = piece->buf_out;
      found = TRUE;
      break;
    }
  }

  if(!found || in.width <= 0 || in.height <= 0) return FALSE;
  return (out.width == in.height) && (out.height == in.width) && (in.width != in.height);
}

gboolean dt_drawlayer_layer_canvas_for_pipe(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, int *width,
                                            int *height)
{
  if(!IS_NULL_PTR(width)) *width = 0;
  if(!IS_NULL_PTR(height)) *height = 0;
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev)) return FALSE;

  int32_t raw_width = 0;
  int32_t raw_height = 0;
  if(!dt_dev_geometry_get_raw_size(self->dev, &raw_width, &raw_height)) return FALSE;
  if(raw_width <= 0 || raw_height <= 0) return FALSE;

  /* A raw buffer is landscape; a portrait image is that buffer seen rotated a quarter turn, and
   * the paint is rotated with it. Carried by transposing the canvas rather than by rotating
   * pixels: the layer the user paints, and the page the sidecar holds, are both in the
   * orientation they see. */
  const gboolean swap = _flip_swaps_axes(self, pipe);

  if(!IS_NULL_PTR(width)) *width = swap ? raw_height : raw_width;
  if(!IS_NULL_PTR(height)) *height = swap ? raw_width : raw_height;
  return TRUE;
}

gboolean dt_drawlayer_layer_canvas(dt_iop_module_t *self, int *width, int *height)
{
  return dt_drawlayer_layer_canvas_for_pipe(self, NULL, width, height);
}

gboolean dt_drawlayer_raw_placement_for_frame(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                              const dt_iop_roi_t *const frame,
                                              dt_drawlayer_raw_placement_t *placement)
{
  if(IS_NULL_PTR(placement)) return FALSE;
  *placement = (dt_drawlayer_raw_placement_t){ 0 };
  if(IS_NULL_PTR(frame) || frame->width <= 0 || frame->height <= 0) return FALSE;

  int canvas_width = 0;
  int canvas_height = 0;
  if(!dt_drawlayer_layer_canvas_for_pipe(self, pipe, &canvas_width, &canvas_height)) return FALSE;

  /* The frame's ORIGIN, not the difference of the sizes.
   *
   * The size fold that produces this rect (dt_dev_pixelpipe_get_roi_out, and the chain's mirror of
   * it) runs at scale 1 from the raw frame, and a crop records itself as an OFFSET in that frame:
   * measured on a cropped NEF, crop maps in=(0,0,4016x6016) to out=(0,2802,2464x3213). So the
   * frame's position inside the raw image is exactly frame->x/y, and translating the canvas by it
   * is what makes the paint stay on the content -- for a crop that moves without resizing as much
   * as for one that resizes.
   *
   * Centring on the sizes alone, which this did first, gets the uncropped case right and every
   * moved crop wrong: two crops of equal size at different positions have the same size difference
   * and therefore the same offset, so the layer followed the frame instead of the image. The
   * uncropped case is still centre-on-centre, because there the origin is 0 and the extents match.
   *
   * A module that GROWS the frame (a perspective correction) leaves the origin at 0 while the
   * extent exceeds the canvas, so the canvas anchors to the frame's corner rather than its centre.
   * That is a consequence of being immune to that module -- there is no offset in this frame that
   * both ignores the correction and re-centres against it. */
  placement->offset_x = frame->x;
  placement->offset_y = frame->y;
  placement->valid = TRUE;
  return TRUE;
}

gboolean dt_drawlayer_raw_placement_gui(dt_iop_module_t *self, dt_drawlayer_raw_placement_t *placement,
                                        int *frame_width, int *frame_height)
{
  if(!IS_NULL_PTR(frame_width)) *frame_width = 0;
  if(!IS_NULL_PTR(frame_height)) *frame_height = 0;
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(placement)) return FALSE;

  dt_iop_roi_t frame;
  if(!dt_dev_module_geometry_gui(self->dev, self, &frame, NULL)) return FALSE;
  if(frame.width <= 0 || frame.height <= 0) return FALSE;

  if(!IS_NULL_PTR(frame_width)) *frame_width = frame.width;
  if(!IS_NULL_PTR(frame_height)) *frame_height = frame.height;
  return dt_drawlayer_raw_placement_for_frame(self, NULL, &frame, placement);
}

static gboolean _virtual_piece_layer_geometry(dt_iop_module_t *self, int *layer_width, int *layer_height)
{
  return dt_drawlayer_layer_canvas(self, layer_width, layer_height);
}

gboolean dt_drawlayer_widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(pts) || count <= 0) return FALSE;

  dt_dev_coordinates_widget_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_preview_abs(self->dev, pts, count);

  if(!dt_dev_distort_backtransform_gui(self->dev, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;
  dt_dev_coordinates_preview_abs_to_image_norm(self->dev, pts, count);

  /* ...which lands in this module's own frame, normalised. Finish through the SAME placement the
   * renderer uses: a dab has to be written where the composite will read it from. */
  dt_drawlayer_raw_placement_t placement;
  int frame_width = 0;
  int frame_height = 0;
  if(!dt_drawlayer_raw_placement_gui(self, &placement, &frame_width, &frame_height)) return FALSE;

  for(int i = 0; i < count; i++)
  {
    pts[2 * i]     = (float)placement.offset_x + pts[2 * i]     * (float)frame_width;
    pts[2 * i + 1] = (float)placement.offset_y + pts[2 * i + 1] * (float)frame_height;
  }
  return TRUE;
}

gboolean dt_drawlayer_layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(pts) || count <= 0) return FALSE;

  // Canvas pixels back to this module's own frame, normalised. Inverse of the above.
  dt_drawlayer_raw_placement_t placement;
  int frame_width = 0;
  int frame_height = 0;
  if(!dt_drawlayer_raw_placement_gui(self, &placement, &frame_width, &frame_height)) return FALSE;

  for(int i = 0; i < count; i++)
  {
    pts[2 * i]     = (pts[2 * i]     - (float)placement.offset_x) / (float)frame_width;
    pts[2 * i + 1] = (pts[2 * i + 1] - (float)placement.offset_y) / (float)frame_height;
  }

  dt_dev_coordinates_image_norm_to_preview_abs(self->dev, pts, count);

  if(!dt_dev_distort_transform_gui(self->dev, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;

  dt_dev_coordinates_preview_abs_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, count);
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
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev) || IS_NULL_PTR(dab)) return fallback;

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

  const float widget_w = (float)dt_dev_viewport_widget_width(self->dev);
  const float widget_h = (float)dt_dev_viewport_widget_height(self->dev);
  const float preview_w = dt_dev_roi_request_preview_width(self->dev);
  const float preview_h = dt_dev_roi_request_preview_height(self->dev);
  if(widget_w <= 0.0f || widget_h <= 0.0f || preview_w <= 0.0f || preview_h <= 0.0f) return FALSE;

  const float zoom_scale = dt_dev_get_overlay_scale(self->dev);
  const float border = (float)dt_dev_viewport_border_size(self->dev);
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
