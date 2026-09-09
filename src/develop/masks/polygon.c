/*
    This file is part of darktable,
    Copyright (C) 2013-2016, 2021 Aldric Renaudin.
    Copyright (C) 2013 Moritz Lipp.
    Copyright (C) 2013-2014, 2020-2022 Pascal Obry.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2013-2017, 2019 Ulrich Pegelow.
    Copyright (C) 2014 Jérémy Rosen.
    Copyright (C) 2016 Fabio Valentini.
    Copyright (C) 2016, 2018 johannes hanika.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Victor Forsiuk.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2024 Alynx Zhou.
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
#include "control/user_message.h"
#include "math/math.h"
#include "system/macros.h"
#include "system/openmp.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/times.h"
#include "common/glib_utils.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "widgets/gdkkeys.h"
#include "widgets/accelerators.h"
#include "widgets/widget_settings.h"
#include "common/conf.h"
#include "develop/imageop.h"
#include "develop/masks/masks_distort.h"
#include "develop/masks.h"
#include "develop/masks_gui.h"
#include "develop/masks/masks_functions.h"
#include "develop/masks/masks_touched.h"
#include "math/openmp_maths.h"
#include "gui/actions/menu.h"
#include <assert.h>

#define FADING_MIN 0.0005f
#define FADING_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

static void _polygon_bounding_box_raw(const float *const point_buffer, const float *border_buffer,
                                      const int corner_count, const int point_count, int border_count,
                                      float *x_min, float *x_max, float *y_min, float *y_max);

/**
 * @brief Evaluate a cubic Bezier at t in [0, 1].
 *
 * Uses the four control points (p0..p3) and returns the interpolated point.
 */
static void _polygon_get_XY(const float p0_x, const float p0_y, const float p1_x, const float p1_y,
                            const float p2_x, const float p2_y, const float p3_x, const float p3_y,
                            const float t, float *out_x, float *out_y)
{
  const float one_minus_t = 1.0f - t;
  const float a = one_minus_t * one_minus_t * one_minus_t;
  const float b = 3.0f * t * one_minus_t * one_minus_t;
  const float c = 3.0f * t * t * one_minus_t;
  const float d = t * t * t;
  *out_x = p0_x * a + p1_x * b + p2_x * c + p3_x * d;
  *out_y = p0_y * a + p1_y * b + p2_y * c + p3_y * d;
}

/**
 * @brief Evaluate a cubic Bezier and its border offset at t in [0, 1].
 *
 * The border point is offset along the normal, scaled by rad.
 */
static gboolean _polygon_border_get_XY(const float p0_x, const float p0_y, const float p1_x, const float p1_y,
                                   const float p2_x, const float p2_y, const float p3_x, const float p3_y,
                                   const float t, const float radius, float radius_rate,
                                   float *center_x, float *center_y, float *border_x, float *border_y)
{
  // we get the point
  _polygon_get_XY(p0_x, p0_y, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y, t, center_x, center_y);

  // now we get derivative points
  const double ti = 1.0 - (double)t;

  const double t_t = (double)t * t;
  const double ti_ti = ti * ti;
  const double t_ti = t * ti;

  const double a = 3.0 * ti_ti;
  const double b = 3.0 * (ti_ti - 2.0 * t_ti);
  const double c = 3.0 * (2.0 * t_ti - t_t);
  const double d = 3.0 * t_t;

  /* not const: a degenerate first-order term is replaced by the limit direction below */
  double dx = -p0_x * a + p1_x * b + p2_x * c + p3_x * d;
  double dy = -p0_y * a + p1_y * b + p2_y * c + p3_y * d;

  /* A vanishing derivative does not mean the tangent is undefined -- it means the first-order
   * term is degenerate and the direction is the limit, given by the first non-zero term. That
   * is exactly how a CUSP is stored: both of the node's handles sit on the node, so the cubic's
   * endpoint derivative 3*(p3 - p2) is zero there.
   *
   * Giving up instead left the offset with no direction at that one sample, and the caller then
   * held the previous border point. A polygon's feather is painted as radial spokes from each
   * outline sample to its border sample, so a held border point means a spoke that goes nowhere:
   * reported on polygon #2 of issue #1313's sidecar as "a radial spoke is missing in the
   * feathering area" at node 12, and visible in the rasterised mask as a thin dark line cutting
   * down through the feather band.
   *
   * This is the same defect the brush had at its own cusp, fixed the same way, and the
   * comparison has to be RELATIVE for the same reason: these are image pixels, so the products
   * either side of the cancellation are rounded before they are subtracted and what should be
   * zero arrives as a small residue. The brush measured 1.22e-4 in float. Doubles here make it
   * smaller, not absent, and normalising a residue yields a direction made of rounding noise --
   * which is worse than giving up, because it looks like an answer. */
  const double span = fmax(fmax(fabs((double)p3_x - p0_x), fabs((double)p3_y - p0_y)),
                           fmax(fabs((double)p2_x - p1_x), fabs((double)p2_y - p1_y)));
  const double degenerate = fmax(span, 1.0) * 1e-9;

  if(fabs(dx) < degenerate && fabs(dy) < degenerate)
  {
    /* for a cubic, if the first-order term vanishes the second-order one gives the limit */
    if(t < 0.5f) { dx = (double)p2_x - p0_x; dy = (double)p2_y - p0_y; }
    else         { dx = (double)p3_x - p1_x; dy = (double)p3_y - p1_y; }

    if(fabs(dx) < degenerate && fabs(dy) < degenerate)
    {
      dx = (double)p3_x - p0_x;
      dy = (double)p3_y - p0_y;
    }
    /* only a curve collapsed to a single point has no direction at all */
    if(dx == 0.0 && dy == 0.0) return FALSE;

    /* a limit direction has a direction and no speed, so no rate can be taken against it */
    radius_rate = 0.0f;
  }

  /* on the envelope of the feather's discs, not on the normal: see
   * dt_masks_outline_envelope_offset() */
  const float centre[2] = { *center_x, *center_y };
  float border[2];
  dt_masks_outline_envelope_offset(centre, (float)dx, (float)dy, radius, radius_rate, border);
  *border_x = border[0];
  *border_y = border[1];
  return TRUE;
}

/* The feather along a segment eases from one node's to the other's, r1 + (r2 - r1) * t^2 (3 - 2t);
 * its rate by t, (r2 - r1) * 6 t (1 - t), is zero at both ends, so the end samples stay on the
 * normal and the joints built from them are unchanged. */
static inline float _polygon_radius_at(const float r1, const float r2, const double t)
{
  return r1 + (r2 - r1) * t * t * (3.0 - 2.0 * t);
}

static inline float _polygon_radius_rate_at(const float r1, const float r2, const double t)
{
  return (r2 - r1) * 6.0 * t * (1.0 - t);
}

/**
 * @brief Convert control point #2 into a handle extremity.
 *
 * The values are expected in orthonormal space.
 */
static void _polygon_ctrl2_to_handle(const float point_x, const float point_y,
                                     const float ctrl_x, const float ctrl_y,
                                     float *handle_x, float *handle_y, const gboolean clockwise)
{
  const float delta_y = ctrl_y - point_y;
  const float delta_x = point_x - ctrl_x;
  if(clockwise)
  {
    *handle_x = point_x - delta_y;
    *handle_y = point_y - delta_x;
  }
  else
  {
    *handle_x = point_x + delta_y;
    *handle_y = point_y + delta_x;
  }
}

/**
 * @brief Convert a handle extremity into symmetric Bezier control points.
 *
 * The values are expected in orthonormal space.
 */
static void _polygon_handle_to_ctrl(const float point_x, const float point_y,
                                    const float handle_x, const float handle_y,
                                    float *ctrl1_x, float *ctrl1_y, float *ctrl2_x, float *ctrl2_y,
                                    const gboolean clockwise)
{
  const float delta_y = handle_y - point_y;
  const float delta_x = point_x - handle_x;

  if(clockwise)
  {
    *ctrl1_x = point_x - delta_y;
    *ctrl1_y = point_y - delta_x;
    *ctrl2_x = point_x + delta_y;
    *ctrl2_y = point_y + delta_x;
  }
  else
  {
    *ctrl1_x = point_x + delta_y;
    *ctrl1_y = point_y + delta_x;
    *ctrl2_x = point_x - delta_y;
    *ctrl2_y = point_y - delta_x;
  }
}

/**
 * @brief Convert a Catmull-Rom segment to Bezier control points.
 */
static void _polygon_catmull_to_bezier(const float x1, const float y1, const float x2, const float y2,
                                       const float x3, const float y3, const float x4, const float y4,
                                       float *bezier_x1, float *bezier_y1,
                                       float *bezier_x2, float *bezier_y2)
{
  *bezier_x1 = (-x1 + 6 * x2 + x3) / 6;
  *bezier_y1 = (-y1 + 6 * y2 + y3) / 6;
  *bezier_x2 = (x2 + 6 * x3 - x4) / 6;
  *bezier_y2 = (y2 + 6 * y3 - y4) / 6;
}

/**
 * @brief Initialize control points to match a Catmull-Rom-like spline.
 *
 * Only points in DT_MASKS_POINT_STATE_NORMAL are regenerated.
 */
static void _polygon_init_ctrl_points(dt_masks_form_t *mask_form)
{
  // if we have less that 3 points, what to do ??
  const guint node_count = g_list_length(mask_form->points);
  if(node_count < 2) return;

  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return;
  
  dt_masks_node_polygon_t **nodes = dt_alloc_align((size_t)node_count * sizeof(*nodes));
  if(IS_NULL_PTR(nodes)) return;
  const GList *form_points = mask_form->points;
  for(guint node_index = 0; node_index < node_count; node_index++)
  {
    nodes[node_index] = (dt_masks_node_polygon_t *)form_points->data;
    form_points = g_list_next(form_points);
  }

  for(guint node_index = 0; node_index < node_count; node_index++)
  {
    dt_masks_node_polygon_t *point3 = nodes[node_index];
    if(IS_NULL_PTR(point3)) { dt_free_align(nodes); return; }
    // if the point has not been set manually, we redefine it
    if(point3->state == DT_MASKS_POINT_STATE_NORMAL)
    {
      dt_masks_node_polygon_t *point1 = nodes[(node_index + node_count - 2) % node_count];
      dt_masks_node_polygon_t *point2 = nodes[(node_index + node_count - 1) % node_count];
      dt_masks_node_polygon_t *point4 = nodes[(node_index + 1) % node_count];
      dt_masks_node_polygon_t *point5 = nodes[(node_index + 2) % node_count];
      if(IS_NULL_PTR(point1) || IS_NULL_PTR(point2) || IS_NULL_PTR(point4) || IS_NULL_PTR(point5)) { dt_free_align(nodes); return; }

      float bezier1_x = 0.0f;
      float bezier1_y = 0.0f;
      float bezier2_x = 0.0f;
      float bezier2_y = 0.0f;
      _polygon_catmull_to_bezier(point1->node[0], point1->node[1], point2->node[0], point2->node[1],
                              point3->node[0], point3->node[1], point4->node[0], point4->node[1],
                              &bezier1_x, &bezier1_y, &bezier2_x, &bezier2_y);
      if(point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bezier1_x;
      if(point2->ctrl2[1] == -1.0) point2->ctrl2[1] = bezier1_y;
      point3->ctrl1[0] = bezier2_x;
      point3->ctrl1[1] = bezier2_y;
      _polygon_catmull_to_bezier(point2->node[0], point2->node[1], point3->node[0], point3->node[1],
                              point4->node[0], point4->node[1], point5->node[0], point5->node[1],
                              &bezier1_x, &bezier1_y, &bezier2_x, &bezier2_y);
      if(point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bezier2_x;
      if(point4->ctrl1[1] == -1.0) point4->ctrl1[1] = bezier2_y;
      point3->ctrl2[0] = bezier1_x;
      point3->ctrl2[1] = bezier1_y;
    }
  }
  dt_free_align(nodes);
  return;
}

/**
 * @brief Determine polygon winding order.
 *
 * Returns TRUE when points are clockwise in normalized space.
 */
static gboolean _polygon_is_clockwise(dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return 0;
  if(!g_list_shorter_than(mask_form->points, 3)) // if we have at least three points...
  {
    float sum = 0.0f;
    for(const GList *form_points = mask_form->points; form_points; form_points = g_list_next(form_points))
    {
      const GList *next = g_list_next_wraparound(form_points, mask_form->points); // next, wrapping around if on last elt
      dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data; // kth element of mask_form->points
      dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)next->data;
      if(IS_NULL_PTR(point1) || IS_NULL_PTR(point2)) return 0;
      sum += (point2->node[0] - point1->node[0]) * (point2->node[1] + point1->node[1]);
    }
    return (sum < 0);
  }
  // return dummy answer
  return TRUE;
}

/**
 * @brief Fill gaps between border points with a circular arc.
 *
 * This is used when the border has gaps, especially near_handle sharp nodes.
 */
static void _polygon_points_recurs_border_gaps(const float *const center_max, const float *const border_min,
                                               const float *const border_max, dt_masks_dynbuf_t *draw_points,
                                               dt_masks_dynbuf_t *draw_border,
                                               gboolean clockwise, const int step)
{
  // we want to find the start and end angles
  double angle_start = atan2f(border_min[1] - center_max[1], border_min[0] - center_max[0]);
  double angle_end = atan2f(border_max[1] - center_max[1], border_max[0] - center_max[0]);
  if(angle_start == angle_end) return;

  // we have to be sure that we turn in the correct direction
  if(angle_end < angle_start && clockwise)
  {
    angle_end += 2 * M_PI;
  }
  if(angle_end > angle_start && !clockwise)
  {
    angle_start += 2 * M_PI;
  }

  // we determine start and end radius too
  const float radius_start = sqrtf((border_min[1] - center_max[1]) * (border_min[1] - center_max[1])
                                   + (border_min[0] - center_max[0]) * (border_min[0] - center_max[0]));
  const float radius_end = sqrtf((border_max[1] - center_max[1]) * (border_max[1] - center_max[1])
                                 + (border_max[0] - center_max[0]) * (border_max[0] - center_max[0]));

  // and the max length of the circle arc, in samples
  int step_count = 0;
  if(angle_end > angle_start)
    step_count = (angle_end - angle_start) * fmaxf(radius_start, radius_end) / (float)step;
  else
    step_count = (angle_start - angle_end) * fmaxf(radius_start, radius_end) / (float)step;
  if(step_count < 2) return;

  // and now we add the points
  const float angle_step = (angle_end - angle_start) / step_count;
  const float radius_step = (radius_end - radius_start) / step_count;
  float current_radius = radius_start + radius_step;
  float current_angle = angle_start + angle_step;
  // allocate entries in the dynbufs
  float *points_ptr = dt_masks_dynbuf_reserve_n(draw_points, 2 * (step_count - 1));
  float *border_ptr = draw_border ? dt_masks_dynbuf_reserve_n(draw_border, 2 * (step_count - 1)) : NULL;
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if(!IS_NULL_PTR(points_ptr))
  {
    for(int step_index = 1; step_index < step_count; step_index++)
    {
      *points_ptr++ = center_max[0];
      *points_ptr++ = center_max[1];
      if(!IS_NULL_PTR(border_ptr))
      {
        *border_ptr++ = center_max[0] + current_radius * cosf(current_angle);
        *border_ptr++ = center_max[1] + current_radius * sinf(current_angle);
      }
      current_radius += radius_step;
      current_angle += angle_step;
    }
  }
}

static inline gboolean _is_within_pxl_threshold(float *min, float *max, int pixel_threshold)
{
  return abs((int)min[0] - (int)max[0]) < pixel_threshold && 
         abs((int)min[1] - (int)max[1]) < pixel_threshold;
}


/**
 * @brief Recursive subdivision to sample polygon and border points.
 *
 * This avoids large gaps by subdividing until points are within a pixel threshold.
 */
static void _polygon_points_recurs(float *segment_start, float *segment_end,
                                   double t_min, double t_max,
                                   float *polygon_min, float *polygon_max,
                                   float *border_min, float *border_max,
                                   float *result_polygon, float *result_border,
                                   dt_masks_dynbuf_t *draw_points, dt_masks_dynbuf_t *draw_border,
                                   int with_border, const int pixel_threshold,
                                   const gboolean have_min, const gboolean have_max,
                                   gboolean have_border_min, gboolean have_border_max,
                                   gboolean *out_have_border)
{
  /* have_* say whether the caller already evaluated that endpoint and whether a border point
   * came with it. They were read out of the arrays as NaN, so "not computed", "no border" and
   * real geometry all shared one representation in the buffers the outline is built from. */
  if(!have_min)
  {
    have_border_min =
    _polygon_border_get_XY(segment_start[0], segment_start[1], segment_start[2], segment_start[3],
                           segment_end[2], segment_end[3], segment_end[0], segment_end[1], t_min,
                           _polygon_radius_at(segment_start[4], segment_end[4], t_min),
                           _polygon_radius_rate_at(segment_start[4], segment_end[4], t_min),
                           polygon_min, polygon_min + 1, border_min, border_min + 1);
  }
  if(!have_max)
  {
    have_border_max =
    _polygon_border_get_XY(segment_start[0], segment_start[1], segment_start[2], segment_start[3],
                           segment_end[2], segment_end[3], segment_end[0], segment_end[1], t_max,
                           _polygon_radius_at(segment_start[4], segment_end[4], t_max),
                           _polygon_radius_rate_at(segment_start[4], segment_end[4], t_max),
                           polygon_max, polygon_max + 1, border_max, border_max + 1);
  }

  // are the points near_handle ?
  if((t_max - t_min < 0.0001)
       || (_is_within_pxl_threshold(polygon_min, polygon_max, pixel_threshold)
          && (!with_border || (_is_within_pxl_threshold(border_min, border_max, pixel_threshold)))))
  {
    dt_masks_dynbuf_add_2(draw_points, polygon_max[0], polygon_max[1]);
    result_polygon[0] = polygon_max[0];
    result_polygon[1] = polygon_max[1];

    if(with_border)
    {
      /* one end of the span may have had no direction to offset along; borrow the other's */
      if(!have_border_max && have_border_min)
      {
        border_max[0] = border_min[0];
        border_max[1] = border_min[1];
        have_border_max = TRUE;
      }
      else if(!have_border_max)
      {
        /* Neither end has a direction. The walk only hands the recursion segments that have
         * one at an end, so this is unreachable in practice -- but what used to happen here
         * is the brush's issue #1360 on a polygon: border_max was the caller's scratch, NaN
         * at the top level and (0, 0) -- the image origin -- below it, written to the buffer
         * as geometry. A spoke of the right LENGTH along the chord's normal is always a valid
         * piece of the disc union. */
        const float radius = _polygon_radius_at(segment_start[4], segment_end[4], t_max);
        dt_masks_outline_offset_along(polygon_max, segment_end[1] - segment_start[1],
                                      -(segment_end[0] - segment_start[0]), radius, border_max);
        have_border_max = TRUE;
      }
      dt_masks_dynbuf_add_2(draw_border, border_max[0], border_max[1]);
      result_border[0] = border_max[0];
      result_border[1] = border_max[1];
      if(!IS_NULL_PTR(out_have_border)) *out_have_border = have_border_max;
    }
    return;
  }

  // we split in two part
  double t_mid = (t_min + t_max) / 2.0;
  float polygon_mid[2] = { 0.0f, 0.0f };
  float border_mid[2] = { 0.0f, 0.0f };
  float polygon_result_left[2] = { 0 };
  float border_result_left[2] = { 0 };
  _polygon_points_recurs(segment_start, segment_end, t_min, t_mid,
                         polygon_min, polygon_mid, border_min, border_mid,
                         polygon_result_left, border_result_left,
                         draw_points, draw_border, with_border, pixel_threshold,
                         have_min, FALSE, have_border_min, FALSE, NULL);
  _polygon_points_recurs(segment_start, segment_end, t_mid, t_max,
                         polygon_result_left, polygon_max, border_result_left, border_max,
                         result_polygon, result_border,
                         draw_points, draw_border, with_border, pixel_threshold,
                         TRUE, have_max, TRUE, have_border_max, out_have_border);
}

// Maximum number of self-intersection portions to track;
// helps limit detection complexity

// Self-intersection cuts are dt_masks_skip_range_t (masks_types.h), built by
// dt_masks_skip_ranges_build() and handed to every consumer OUT-OF-BAND -- never encoded into
// the border buffer. The in-band NaN-jump encoding this replaced hosted both bugs the
// mechanism ever had (a reader cycle, then issue #1313's seam fold) while the geometry was
// right both times.

/* THE WALK.
 *
 * A polygon's feather is the union of a disc of the local radius over every point of its
 * path, outside the path; the rasteriser paints it as spokes from every path sample out to
 * its border sample, and fills the path's interior separately. The path is closed, so it is
 * walked once, the border on the outside -- the winding is folded into the sign of the
 * radius -- and every convex joint gets an arc centred on its node.
 *
 * Everything a joint needs is taken from the two segment END SAMPLES that meet there and from
 * the node data. Nothing is read back out of the buffers. The previous walk took "the border
 * sample last written", and the one ten samples before it, as the inputs of every joint arc,
 * and wrote its zero-initialised scratch -- the image origin -- into the border wherever a
 * segment had no direction. That is the brush's issue #1360, on a polygon: a pen resting
 * under rising pressure, or a node dropped twice on the same spot, produces exactly such a
 * segment. A DEGENERATE segment -- its four control points one point -- contributes nothing;
 * the joint that closes over it is between its two live neighbours, which for the disc union
 * is exactly right. */
typedef struct _polygon_frame_t
{
  float iwd;
  float iht;
  float dx;
  float dy;
  float radius_sign;   /* +1 or -1: the winding, so the offset falls outside the path */
} _polygon_frame_t;

/* The two ends of one segment, in image pixels: node, the control point that faces the other
 * end, signed radius. The start of a segment carries its node's border[1] and the end its
 * node's border[0]; every writer sets the two together. */
static void _polygon_segment_load(const dt_masks_node_polygon_t *const from,
                                  const dt_masks_node_polygon_t *const to, const _polygon_frame_t *const f,
                                  float p1[5], float p2[5])
{
  const float scale = f->radius_sign * MIN(f->iwd, f->iht);
  p1[0] = from->node[0] * f->iwd - f->dx;
  p1[1] = from->node[1] * f->iht - f->dy;
  p1[2] = from->ctrl2[0] * f->iwd - f->dx;
  p1[3] = from->ctrl2[1] * f->iht - f->dy;
  p1[4] = from->border[1] * scale;
  p2[0] = to->node[0] * f->iwd - f->dx;
  p2[1] = to->node[1] * f->iht - f->dy;
  p2[2] = to->ctrl1[0] * f->iwd - f->dx;
  p2[3] = to->ctrl1[1] * f->iht - f->dy;
  p2[4] = to->border[0] * scale;
}

typedef struct _polygon_walk_t
{
  dt_masks_node_polygon_t **nodes;
  int node_count;
  _polygon_frame_t frame;
  int pixel_threshold;
  gboolean with_border;     /* a border is built: it was asked for, and there are enough nodes */
  gboolean clockwise;
  dt_masks_dynbuf_t *dpoints;
  dt_masks_dynbuf_t *dborder;
  float *node_border;       /* per node, the border sample at the node: the header's handle */
  uint8_t *node_has_border;
} _polygon_walk_t;

/* The walk at the node the next segment starts from, and the first live segment's start, for
 * the joint that closes the path. */
typedef struct _polygon_walk_state_t
{
  gboolean have_prev;
  float c[2];
  float b[2];
  gboolean have_first;
  float first_c[2];
  float first_b[2];
} _polygon_walk_state_t;

/* The arc that bridges a joint, the short way round; on a tie the winding decides. The
 * filler is the polygon's own. */
static void _polygon_joint_arc(const _polygon_walk_t *const w, const float *const centre, const float *const from,
                               const float *const to)
{
  if(fabsf(to[0] - from[0]) <= 1.0f && fabsf(to[1] - from[1]) <= 1.0f) return;
  const gboolean clockwise = dt_masks_outline_short_way(centre, from, to, w->clockwise);
  _polygon_points_recurs_border_gaps(centre, from, to, w->dpoints, w->dborder, clockwise, w->pixel_threshold);
}

/* One segment of the walk: the joint at its start node, then every sample within a pixel of
 * the last and its border. Returns FALSE for a degenerate segment, which contributes nothing. */
static gboolean _polygon_walk_segment(const _polygon_walk_t *const w, _polygon_walk_state_t *const s, const int k)
{
  const int k1 = (k + 1) % w->node_count;
  float p1[5];
  float p2[5];
  _polygon_segment_load(w->nodes[k], w->nodes[k1], &w->frame, p1, p2);

  /* the segment's own end samples; a segment with a direction at neither end is a point */
  float c0[2];
  float b0[2];
  float c1[2];
  float b1[2];
  const gboolean have_b0 = _polygon_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], 0.0f,
                                                  p1[4], 0.0f, c0, c0 + 1, b0, b0 + 1);
  const gboolean have_b1 = _polygon_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], 1.0f,
                                                  p2[4], 0.0f, c1, c1 + 1, b1, b1 + 1);
  if(!have_b0 && !have_b1) return FALSE;
  if(!have_b0) dt_masks_outline_offset_along(c0, b1[0] - c1[0], b1[1] - c1[1], p1[4], b0);
  if(!have_b1) dt_masks_outline_offset_along(c1, b0[0] - c0[0], b0[1] - c0[1], p2[4], b1);

  if(w->with_border)
  {
    if(s->have_prev) _polygon_joint_arc(w, c0, s->b, b0);
    w->node_border[k * 2] = b0[0];
    w->node_border[k * 2 + 1] = b0[1];
    w->node_has_border[k] = 1;
    if(!s->have_first)
    {
      s->first_c[0] = c0[0];
      s->first_c[1] = c0[1];
      s->first_b[0] = b0[0];
      s->first_b[1] = b0[1];
      s->have_first = TRUE;
    }
  }

  float rc[2];
  float rb[2];
  float bmin[2] = { b0[0], b0[1] };
  float bmax[2] = { b1[0], b1[1] };
  float cmin[2] = { c0[0], c0[1] };
  float cmax[2] = { c1[0], c1[1] };
  gboolean have_rb = FALSE;
  _polygon_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, w->dpoints, w->dborder,
                         w->with_border, w->pixel_threshold, TRUE, TRUE, TRUE, TRUE, &have_rb);

  dt_masks_dynbuf_add_2(w->dpoints, rc[0], rc[1]);
  if(w->with_border)
  {
    if(!have_rb) dt_masks_outline_offset_along(rc, b1[0] - c1[0], b1[1] - c1[1], p2[4], rb);
    dt_masks_dynbuf_add_2(w->dborder, rb[0], rb[1]);
  }

  s->c[0] = rc[0];
  s->c[1] = rc[1];
  s->b[0] = rb[0];
  s->b[1] = rb[1];
  s->have_prev = TRUE;
  return TRUE;
}

/* The header's per-node border sample: a node whose own segment was a point takes its
 * successor's, so a handle is drawn for every node and drawn where the border is. */
static void _polygon_walk_write_header(const _polygon_walk_t *const w)
{
  if(!w->with_border) return;
  float *const border = dt_masks_dynbuf_buffer(w->dborder);
  for(int k = 0; k < w->node_count; k++)
  {
    int from = k;
    for(int step = 0; step < w->node_count && !w->node_has_border[from]; step++) from = (from + 1) % w->node_count;
    if(!w->node_has_border[from]) return;   /* no segment had a direction: the header stays zero */
    border[k * 6] = w->node_border[from * 2];
    border[k * 6 + 1] = w->node_border[from * 2 + 1];
  }
}

/** get all points of the polygon and the border */
/** this takes care of gaps and iop distortions */
static int _polygon_get_pts_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                   const double iop_order, const int transform_direction,
                                   const dt_masks_distort_t *const dist, float **point_buffer, int *point_count,
                                   float **border_buffer, int *border_count, gboolean source)
{
  *point_buffer = NULL;
  *point_count = 0;
  if(!IS_NULL_PTR(border_buffer)) *border_buffer = NULL;
  if(!IS_NULL_PTR(border_buffer)) *border_count = 0;

  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return 0;

  double start2 = 0.0;
  if(dt_get_debug_flags() & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float input_width = dist->iwidth;
  const float input_height = dist->iheight;

  /* The pipe's is fixed at one pixel: its interior fill is a scanline over these samples and
   * needs every row crossed, and its mask_rasterization_step is a spoke-spacing budget, not a
   * scanline one. The GUI fills nothing and samples at the density it shows. */
  const int pixel_threshold = IS_NULL_PTR(dist->pipe) ? dist->rasterization_step : 1;
  const int node_count = (int)g_list_length(mask_form->points);

  dt_masks_dynbuf_t *dpoints = dt_masks_dynbuf_init(1000000, "polygon dpoints");
  if(IS_NULL_PTR(dpoints)) return 1;
  dt_masks_dynbuf_t *dborder = NULL;
  if(!IS_NULL_PTR(border_buffer))
  {
    dborder = dt_masks_dynbuf_init(1000000, "polygon dborder");
    if(IS_NULL_PTR(dborder))
    {
      dt_masks_dynbuf_free(dpoints);
      return 1;
    }
  }

  dt_masks_node_polygon_t **nodes = dt_alloc_align((size_t)node_count * sizeof(*nodes));
  float *node_border = dt_alloc_align((size_t)node_count * 2 * sizeof(float));
  uint8_t *node_has_border = dt_alloc_align((size_t)node_count);
  if(IS_NULL_PTR(nodes) || IS_NULL_PTR(node_border) || IS_NULL_PTR(node_has_border))
  {
    dt_free_align(nodes);
    dt_free_align(node_border);
    dt_free_align(node_has_border);
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 1;
  }
  memset(node_has_border, 0, (size_t)node_count);

  // the source shape of a clone is walked in place, shifted from its target
  float dx = 0.0f;
  float dy = 0.0f;
  if(source && transform_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    const dt_masks_node_polygon_t *const first = (dt_masks_node_polygon_t *)mask_form->points->data;
    dx = (first->node[0] - mask_form->source[0]) * input_width;
    dy = (first->node[1] - mask_form->source[1]) * input_height;
  }

  // the header: three entries per node, ctrl1 / node / ctrl2
  int node_index = 0;
  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    dt_masks_node_polygon_t *const node = (dt_masks_node_polygon_t *)point_node->data;
    nodes[node_index++] = node;
    float *const buf = dt_masks_dynbuf_reserve_n(dpoints, 6);
    if(!IS_NULL_PTR(buf))
    {
      buf[0] = node->ctrl1[0] * input_width - dx;
      buf[1] = node->ctrl1[1] * input_height - dy;
      buf[2] = node->node[0] * input_width - dx;
      buf[3] = node->node[1] * input_height - dy;
      buf[4] = node->ctrl2[0] * input_width - dx;
      buf[5] = node->ctrl2[1] * input_height - dy;
    }
  }
  if(!IS_NULL_PTR(dborder)) dt_masks_dynbuf_add_zeros(dborder, 6 * node_count);

  const gboolean clockwise = _polygon_is_clockwise(mask_form);
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points init took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  const _polygon_walk_t walk = { .nodes = nodes,
                                 .node_count = node_count,
                                 .frame = { input_width, input_height, dx, dy, clockwise ? 1.0f : -1.0f },
                                 .pixel_threshold = pixel_threshold,
                                 .with_border = (!IS_NULL_PTR(dborder) && node_count >= 3),
                                 .clockwise = clockwise,
                                 .dpoints = dpoints,
                                 .dborder = dborder,
                                 .node_border = node_border,
                                 .node_has_border = node_has_border };
  _polygon_walk_state_t state = { 0 };
  for(int k = 0; k < node_count; k++) _polygon_walk_segment(&walk, &state, k);
  /* the joint that closes the path, between the last live segment and the first */
  if(walk.with_border && state.have_prev && state.have_first)
    _polygon_joint_arc(&walk, state.first_c, state.b, state.first_b);
  _polygon_walk_write_header(&walk);

  dt_free_align(nodes);
  dt_free_align(node_border);
  dt_free_align(node_has_border);

  *point_count = dt_masks_dynbuf_position(dpoints) / 2;
  *point_buffer = dt_masks_dynbuf_harvest(dpoints);
  dt_masks_dynbuf_free(dpoints);
  if(!IS_NULL_PTR(dborder))
  {
    *border_count = dt_masks_dynbuf_position(dborder) / 2;
    *border_buffer = dt_masks_dynbuf_harvest(dborder);
    dt_masks_dynbuf_free(dborder);
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points point recurs %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // and we transform them with all distorted modules
  if(source && transform_direction == DT_DEV_TRANSFORM_DIR_ALL)
  {
    // we transform with all distortion that happen *before* the module
    // so we have now the TARGET points in module input reference
    if(dt_masks_distort_transform(dist, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                  *point_buffer, *point_count))
    {
      // now we move all the points by the shift
      // so we have now the SOURCE points in module input reference
      float pts[2] = { mask_form->source[0] * input_width, mask_form->source[1] * input_height };
      if(!dt_masks_distort_transform(dist, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1))
        goto fail;

      dx = pts[0] - (*point_buffer)[2];
      dy = pts[1] - (*point_buffer)[3];
      __OMP_PARALLEL_FOR_SIMD__(if(*point_count > 100) aligned(point_buffer:64))
      for(int i = 0; i < *point_count; i++)
      {
        (*point_buffer)[i * 2]     += dx;
        (*point_buffer)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_masks_distort_transform(dist, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                     *point_buffer, *point_count))
        goto fail;
    }

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points end took %0.04f sec\n",
               mask_form->name, dt_get_wtime() - start2);
    return 0;
  }
  else if(dt_masks_distort_transform(dist, iop_order, transform_direction, *point_buffer, *point_count)
          && (IS_NULL_PTR(border_buffer)
              || dt_masks_distort_transform(dist, iop_order, transform_direction, *border_buffer, *border_count)))
  {
    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points transform took %0.04f sec\n", mask_form->name,
               dt_get_wtime() - start2);
    return 0;
  }

fail:
  // if we failed, then free all and return
  dt_pixelpipe_cache_free_align(*point_buffer);
  *point_buffer = NULL;
  *point_count = 0;
  if(!IS_NULL_PTR(border_buffer))
  {
    dt_pixelpipe_cache_free_align(*border_buffer);
    *border_buffer = NULL;
    *border_count = 0;
  }
  return 1;
}

/**
 * @brief Find the parametric position along a segment closest to a point.
 *
 * We only need 1% precision, so we use exhaustive sampling.
 */
static float _polygon_get_position_in_segment(float point_x, float point_y,
                                              dt_masks_form_t *mask_form, int segment_index)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return 0;
  GList *firstpt = g_list_nth(mask_form->points, segment_index);
  dt_masks_node_polygon_t *point0 = (dt_masks_node_polygon_t *)firstpt->data;
  // advance to next node in list, if not already on the last
  GList *nextpt = g_list_next_bounded(firstpt);
  dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_polygon_t *point3 = (dt_masks_node_polygon_t *)nextpt->data;

  float min_t = 0.0f;
  float min_dist = FLT_MAX;

  for(int i = 0; i <= 100; i++)
  {
    const float t = i / 100.0f;
    float sample_x = 0.0f;
    float sample_y = 0.0f;
    _polygon_get_XY(point0->node[0], point0->node[1], point1->node[0], point1->node[1],
                    point2->node[0], point2->node[1], point3->node[0], point3->node[1], t,
                    &sample_x, &sample_y);

    const float dist = (point_x - sample_x) * (point_x - sample_x)
                       + (point_y - sample_y) * (point_y - sample_y);
    if(dist < min_dist)
    {
      min_dist = dist;
      min_t = t;
    }
  }

  return min_t;
}

static void _add_node_to_segment(struct dt_iop_module_t *module,
                                 dt_masks_form_t *mask_form, int parent_id,
                                 dt_masks_form_gui_t *mask_gui, int form_index)
{  
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points) || IS_NULL_PTR(mask_gui)) return;
  const guint node_count = g_list_length(mask_form->points);
  const int selected_segment = dt_masks_gui_selected_segment_index(mask_gui);
  if(selected_segment < 0 || selected_segment >= (int)node_count) return;

  // we add a new node to the polygon
  dt_masks_node_polygon_t *new_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
  if(IS_NULL_PTR(new_node)) return;

  // set coordinates
  dt_masks_gui_cursor_to_raw_norm(mask_gui->dev, mask_gui, new_node->node);
  new_node->ctrl1[0] = new_node->ctrl1[1] = new_node->ctrl2[0] = new_node->ctrl2[1] = -1.0;
  new_node->state = DT_MASKS_POINT_STATE_NORMAL;

  // set other attributes of the new node. we interpolate the starting and the end node of that
  // segment
  const float t = _polygon_get_position_in_segment(new_node->node[0], new_node->node[1],
                                                   mask_form, selected_segment);
  // start and end node of the segment
  GList *pt = g_list_nth(mask_form->points, selected_segment);
  if(IS_NULL_PTR(pt) || IS_NULL_PTR(pt->data))
  {
    dt_free(new_node);
    return;
  }
  dt_masks_node_polygon_t *point0 = (dt_masks_node_polygon_t *)pt->data;
  const GList *const next_pt = g_list_next_wraparound(pt, mask_form->points);
  if(IS_NULL_PTR(next_pt) || IS_NULL_PTR(next_pt->data))
  {
    dt_free(new_node);
    return;
  }
  dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)next_pt->data;
  new_node->border[0] = point0->border[0] * (1.0f - t) + point1->border[0] * t;
  new_node->border[1] = point0->border[1] * (1.0f - t) + point1->border[1] * t;

  mask_form->points = g_list_insert(mask_form->points, new_node, selected_segment + 1);
  _polygon_init_ctrl_points(mask_form);

  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);

  mask_gui->node_hovered = selected_segment + 1;
  mask_gui->node_selected = TRUE;
  mask_gui->node_selected_idx = selected_segment + 1;
  mask_gui->seg_hovered = -1;
  mask_gui->seg_selected = FALSE;
}

static inline void _polygon_translate_node(dt_masks_node_polygon_t *node, const float delta_x, const float delta_y)
{
  dt_masks_translate_ctrl_node(node->node, node->ctrl1, node->ctrl2, delta_x, delta_y);
}

static void _polygon_translate_all_nodes(dt_masks_form_t *mask_form, const float delta_x, const float delta_y)
{
  for(GList *node_entry = mask_form->points; node_entry; node_entry = g_list_next(node_entry))
    _polygon_translate_node((dt_masks_node_polygon_t *)node_entry->data, delta_x, delta_y);
}

static dt_masks_raster_result_t _polygon_get_points_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                      float **point_buffer, int *point_count,
                                      float **border_buffer, int *border_count,
                                      dt_masks_skip_range_t **border_skips, int *border_skip_count,
                                      int source, const dt_iop_module_t *module)
{
  // Asking for the source outline without a module is a programming error, not an empty shape.
  if(source && IS_NULL_PTR(module)) return DT_MASKS_RASTER_ERROR;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  const dt_masks_distort_t gui_dist = dt_masks_distort_for_gui(develop);
  if(!IS_NULL_PTR(border_skips)) *border_skips = NULL;
  if(!IS_NULL_PTR(border_skip_count)) *border_skip_count = 0;
  const int status = _polygon_get_pts_border(develop, mask_form, ioporder, DT_DEV_TRANSFORM_DIR_ALL, &gui_dist,
                                             point_buffer, point_count, border_buffer, border_count, source);

  /* This outline feeds the GUI only -- the rasterisers build their own from the pixel path and
   * paint every spoke. What the outline shows is the BOUNDARY of what they paint, decided per
   * sample by dt_masks_outline_boundary_skips(): a border sample is on it iff it is not strictly
   * inside any other sample's disc. The folds of a concave run, the inside of a joint arc and
   * one side of the path running through the other all fail that test and travel out-of-band
   * as skip ranges, which is what every consumer of this outline already reads. The polygon's
   * own detector, which intersected the outline with itself and chose cuts, is gone with the
   * brush's. */
  if(status == 0 && !IS_NULL_PTR(border_buffer) && !IS_NULL_PTR(*border_buffer)
     && !IS_NULL_PTR(border_skips) && !IS_NULL_PTR(border_skip_count))
  {
    const int header = (int)g_list_length(mask_form->points) * 3;
    *border_skip_count = dt_masks_outline_boundary_skips(*point_buffer, *border_buffer, *border_count, header,
                                                         border_skips);
  }
  return dt_masks_raster_from_status(status);
}

static void _polygon_get_sizes(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                               dt_masks_form_gui_t *mask_gui, int form_index,
                               float *mask_size, float *border_size)
{
  const dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return;

  const int node_count = g_list_length(mask_form->points);
  float p1[2] = { FLT_MAX, FLT_MAX };
  float p2[2] = { -FLT_MAX, -FLT_MAX };

  float fp1[2] = { FLT_MAX, FLT_MAX };
  float fp2[2] = { -FLT_MAX, -FLT_MAX };

  for(int i = node_count * 3; i < gui_points->points_count; i++)
  {
    // line
    const float x = gui_points->points[i * 2];
    const float y = gui_points->points[i * 2 + 1];

    p1[0] = fminf(p1[0], x);
    p2[0] = fmaxf(p2[0], x);
    p1[1] = fminf(p1[1], y);
    p2[1] = fmaxf(p2[1], y);

    // Bounded by border_count, not by the loop's points_count. points/points_count and
    // border/border_count are separate arrays with separate lengths -- the other border loops
    // in this file spell it `i < border_count` -- so indexing border[] with an index only
    // checked against points_count reads past its end whenever the border is the shorter of
    // the two. That is Sentry 142390966: EXCEPTION_ACCESS_VIOLATION here, reached from the
    // interaction slider (dt_masks_form_set_interaction_value -> _change_size). border can
    // also be NULL outright when none was rasterised.
    if(!IS_NULL_PTR(border_size)
       && !IS_NULL_PTR(gui_points->border)
       && i < gui_points->border_count)
    {
      // border
      const float fx = gui_points->border[i * 2];
      const float fy = gui_points->border[i * 2 + 1];

      /* The "??? looks like when x border is nan then y is a point index" this replaces was a
       * true reading of the in-band jump encoding, and the question mark was the problem: a
       * coordinate buffer that sometimes holds an index, recognisable only by a NaN beside it.
       * It is gone; the border is coordinates. */
      fp1[0] = fminf(fp1[0], fx);
      fp2[0] = fmaxf(fp2[0], fx);
      fp1[1] = fminf(fp1[1], fy);
      fp2[1] = fmaxf(fp2[1], fy);
    }
  }

  float mask_span[2] = { p2[0] - p1[0], p2[1] - p1[1] };
  dt_dev_coordinates_preview_abs_to_image_norm(mask_gui->dev, mask_span, 1);
  *mask_size = fmaxf(mask_span[0], mask_span[1]);

  if(!IS_NULL_PTR(border_size))
  {
    float border_span[2] = { fp2[0] - fp1[0], fp2[1] - fp1[1] };
    dt_dev_coordinates_preview_abs_to_image_norm(mask_gui->dev, border_span, 1);
    *border_size = fmaxf(border_span[0], border_span[1]);
  }
}

static gboolean _polygon_form_gravity_center(const dt_masks_form_t *mask_form, float *center_x,
                                             float *center_y, float *surface);

static float _polygon_get_interaction_value(const dt_masks_form_t *mask_form,
                                            dt_masks_interaction_t interaction)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return NAN;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
    {
      const float size = dt_masks_get_form_size_from_nodes(mask_form->points);
      if(size <= 0.0f) return NAN;
      return size;
    }
    case DT_MASKS_INTERACTION_FADING:
    {
      float fading_sum = 0.0f;
      int fading_count = 0;

      for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
      {
        const dt_masks_node_polygon_t *node = (const dt_masks_node_polygon_t *)point_node->data;
        if(IS_NULL_PTR(node)) continue;
        fading_sum += node->border[0] + node->border[1];
        fading_count += 2;
      }

      return fading_count > 0 ? fading_sum / (float)fading_count : NAN;
    }
    default:
      return NAN;
  }
}

static gboolean _polygon_get_gravity_center(dt_develop_t *dev, const dt_masks_form_t *mask_form,
                                            float center[2], float *area)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points) || IS_NULL_PTR(center)) return FALSE;

  const int points_count = g_list_length(mask_form->points);
  if(points_count <= 0) return FALSE;

  float *point_buffer = dt_alloc_align_float((size_t)points_count * 2);
  if(IS_NULL_PTR(point_buffer)) return FALSE;

  int i = 0;
  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    const dt_masks_node_polygon_t *node = (const dt_masks_node_polygon_t *)point_node->data;
    if(IS_NULL_PTR(node)) continue;
    point_buffer[2 * i] = node->node[0];
    point_buffer[2 * i + 1] = node->node[1];
    i++;
  }

  const gboolean ok = dt_masks_center_of_gravity_from_points(point_buffer, i, center, area);
  dt_free_align(point_buffer);
  return ok;
}

static int _change_size(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                        struct dt_iop_module_t *module, int form_index, const float amount,
                        const dt_masks_increment_t increment, const int flow);
static int _change_fading(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                          struct dt_iop_module_t *module, int form_index, const float amount,
                          const dt_masks_increment_t increment, int flow);

static float _polygon_set_interaction_value(dt_masks_form_t *mask_form,
                                            dt_masks_interaction_t interaction, float value,
                                            dt_masks_increment_t increment, int flow,
                                            dt_masks_form_gui_t *mask_gui, struct dt_iop_module_t *module)
{
  if(IS_NULL_PTR(mask_form)) return NAN;
  // Mirrors _dt_masks_events_get_dispatch_form()'s form_index: this shape's position in the
  // currently displayed group, so dt_masks_gui_form_create() below refreshes the right
  // mask_gui->points slot instead of clobbering whatever shape sits at index 0.
  const int index = (!IS_NULL_PTR(mask_gui) && mask_gui->group_selected >= 0) ? mask_gui->group_selected : 0;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      if(!_change_size(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
      return _polygon_get_interaction_value(mask_form, interaction);
    case DT_MASKS_INTERACTION_FADING:
      if(!_change_fading(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
      return _polygon_get_interaction_value(mask_form, interaction);
    default:
      return NAN;
  }
}

/**
 * @brief Compute proximity between a point and the polygon GUI shape.
 */
static void _polygon_get_distance(float point_x, float point_y, float radius,
                                  dt_masks_form_gui_t *mask_gui, int form_index,
                                  int node_count, int *inside, int *inside_border,
                                  int *near_handle, int *inside_source, float *dist)
{
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near_handle = -1;
  *dist = FLT_MAX;

  if(IS_NULL_PTR(mask_gui)) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return;
  /* Nothing to walk when the cursor cannot reach a sample: the box every sample spans, grown by
   * the cursor's reach, answers for the whole shape in four comparisons, and every answer
   * initialised above is what the walk would have given -- nothing inside, nothing near. */
  if(!dt_masks_gui_points_reach(gui_points, point_x, point_y, 2.0f * radius)) return;

  float min_dist_pixel = FLT_MAX;

  const float radius2 = radius * radius;
  const float pt[2] = { point_x, point_y };
  
  // we first check if we are inside the source form
  if(gui_points->source && gui_points->points
     && gui_points->source_count > node_count * 3 && gui_points->points_count > node_count * 3
     && dt_masks_point_in_form_exact(pt, 1, gui_points->source, node_count * 3,
                                     gui_points->source_count, NULL, 0) >= 0)
  {
    *inside_source = 1;
    *inside = 1;

    // offset between form origin and source origin
    const float offset_x = -gui_points->points[2] + gui_points->source[2];
    const float offset_y = -gui_points->points[3] + gui_points->source[3];
    int current_seg = 1;

    // distance from source border
    for(int i = node_count * 3; i < gui_points->points_count; i++)
    {
      // check if we advance to next polygon segment
      if(gui_points->points[i * 2] == gui_points->points[current_seg * 6 + 2]
         && gui_points->points[i * 2 + 1] == gui_points->points[current_seg * 6 + 3])
      {
        current_seg = (current_seg + 1) % node_count;
      }
      
      // calculate source position for current point
      const float source_x = gui_points->points[i * 2] + offset_x;
      const float source_y = gui_points->points[i * 2 + 1] + offset_y;

      // distance from tested point to current source point
      const float sdx = point_x - source_x;
      const float sdy = point_y - source_y;
      const float sdd = sqf(sdx) + sqf(sdy);
      if(sdd < min_dist_pixel)
        min_dist_pixel = sdd;
    }
    *dist = min_dist_pixel;
    return;
  }

  // we check if we are near_handle a segment
  if(gui_points->points && gui_points->points_count > 2 + node_count * 3)
  {
    int current_seg = 1;
    for(int i = node_count * 3; i < gui_points->points_count; i++)
    {
      // do we change of polygon segment ?
      if(gui_points->points[i * 2 + 1] == gui_points->points[current_seg * 6 + 3]
         && gui_points->points[i * 2] == gui_points->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % node_count;
      }
      //distance from tested point to current form point
      const float yy = gui_points->points[i * 2 + 1];
      const float xx = gui_points->points[i * 2];

      const float dx = point_x - xx;
      const float dy = point_y - yy;
      const float dd = sqf(dx) + sqf(dy);
      if(dd < min_dist_pixel)
      {
        min_dist_pixel = dd;

        if(current_seg >= 0 && dd < radius2)
        {
          if(current_seg == 0)
            *near_handle = node_count - 1;
          else
            *near_handle = current_seg - 1;
        }
      }
    }
  }

  *dist = min_dist_pixel;

  if(!gui_points->border || gui_points->border_count <= node_count * 3) return;

  // Proximity to the feather's OUTER line, the same measure the form line was tested with above.
  int near_border = 0;
  for(int i = node_count * 3; i < gui_points->border_count && !near_border; i++)
  {
    const float bdx = point_x - gui_points->border[i * 2];
    const float bdy = point_y - gui_points->border[i * 2 + 1];
    near_border = (sqf(bdx) + sqf(bdy)) < radius2;
  }

  const int enclosed = dt_masks_point_in_form_exact(pt, 1, gui_points->border, node_count * 3,
                                                    gui_points->border_count,
                                                    gui_points->border_skips,
                                                    gui_points->border_skip_count) >= 0;

  /* The feathering is part of the shape and answers `inside'; being anywhere in that band is not
   * a hit on the border. Only proximity to its outer line is, and only where no form-line segment
   * is closer. The shared hit test answers on the border before the segment, so reporting the
   * whole band left the segment reachable from the inside of the form line alone -- the outer
   * half of the cursor's reach, the half that falls inside the feathering, went to the border
   * instead. Same definition as the brush, whose band covers the whole stroke and hit this first. */
  *inside = enclosed || near_border;
  *inside_border = near_border && (*near_handle < 0);
}

/**
 * @brief Polygon-specific border handle lookup.
 *
 * Polygon borders are stored directly in gui_points->border at node indices.
 */
static gboolean _polygon_border_handle_cb(const dt_masks_form_gui_points_t *gui_points, int node_count,
                                          int node_index, float *handle_x, float *handle_y, void *user_data)
{
  if(IS_NULL_PTR(gui_points) || node_index < 0 || node_index >= node_count) return FALSE;
  *handle_x = gui_points->border[node_index * 6];
  *handle_y = gui_points->border[node_index * 6 + 1];
  return TRUE;
}

/**
 * @brief Polygon-specific curve handle lookup (depends on winding direction).
 */
static void _polygon_curve_handle_cb(const dt_masks_form_gui_points_t *gui_points, int node_index,
                                     float *handle_x, float *handle_y, void *user_data)
{
  
  _polygon_ctrl2_to_handle(gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3],
                           gui_points->points[node_index * 6 + 4], gui_points->points[node_index * 6 + 5],
                           handle_x, handle_y, gui_points->clockwise);
}

/**
 * @brief Polygon-specific inside/border/segment hit testing.
 */
static void _polygon_distance_cb(float pointer_x, float pointer_y, float cursor_radius,
                                 dt_masks_form_gui_t *mask_gui, int form_index, int node_count, int *inside,
                                 int *inside_border, int *near_handle, int *inside_source, float *dist, void *user_data)
{
  
  _polygon_get_distance(pointer_x, pointer_y, cursor_radius, mask_gui, form_index, node_count,
                        inside, inside_border, near_handle, inside_source, dist);
}

static int _find_closest_handle(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui, int form_index)
{
  return dt_masks_find_closest_handle_common(mask_form, mask_gui, form_index, -1,
                                             _polygon_border_handle_cb, _polygon_curve_handle_cb, NULL,
                                             _polygon_distance_cb, NULL, NULL);
}

/**
 * @brief Compute polygon centroid from GUI points using the shoelace formula.
 *
 * This treats the polygon as simple and closed, using the node positions only
 * (no border points). The area is returned with sign (clockwise vs counter).
 */
static void _polygon_gui_gravity_center(const float *point_buffer, int point_count,
                                        float *center_x, float *center_y, float *area)
{
  if(IS_NULL_PTR(point_buffer) || point_count < 3) return;

  float centroid_x = 0.0f;
  float centroid_y = 0.0f;
  float signed_area = 0.0f;

  for(int node_index = 0; node_index < point_count; node_index++)
  {
    const int next_index = (node_index + 1) % point_count;
    const float x0 = point_buffer[node_index * 2];
    const float y0 = point_buffer[node_index * 2 + 1];
    const float x1 = point_buffer[next_index * 2];
    const float y1 = point_buffer[next_index * 2 + 1];
    const float cross = x0 * y1 - x1 * y0;

    signed_area += cross;
    centroid_x += (x0 + x1) * cross;
    centroid_y += (y0 + y1) * cross;
  }

  if(fabsf(signed_area) > 1e-8f)
  {
    const float inv_divisor = 1.0f / (3.0f * signed_area);
    if(!IS_NULL_PTR(center_x)) *center_x = centroid_x * inv_divisor;
    if(!IS_NULL_PTR(center_y)) *center_y = centroid_y * inv_divisor;
  }
  if(!IS_NULL_PTR(area)) *area = signed_area;
}

/**
 * @brief Compute polygon centroid from the form nodes (normalized space).
 */
static gboolean _polygon_form_gravity_center(const dt_masks_form_t *mask_form,
                                             float *center_x, float *center_y, float *area)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points) || g_list_shorter_than(mask_form->points, 3)) return FALSE;

  float centroid_x = 0.0f;
  float centroid_y = 0.0f;
  float signed_area = 0.0f;

  for(const GList *node_iter = mask_form->points; node_iter; node_iter = g_list_next(node_iter))
  {
    const GList *next_iter = g_list_next_wraparound(node_iter, mask_form->points);
    const dt_masks_node_polygon_t *node0 = (const dt_masks_node_polygon_t *)node_iter->data;
    const dt_masks_node_polygon_t *node1 = (const dt_masks_node_polygon_t *)next_iter->data;
    if(!node0 || !node1) return FALSE;

    const float cross = node0->node[0] * node1->node[1] - node1->node[0] * node0->node[1];
    signed_area += cross;
    centroid_x += (node0->node[0] + node1->node[0]) * cross;
    centroid_y += (node0->node[1] + node1->node[1]) * cross;
  }

  if(!IS_NULL_PTR(area)) *area = signed_area;
  if(fabsf(signed_area) <= 1e-8f) return FALSE;

  const float inv_divisor = 1.0f / (3.0f * signed_area);
  if(!IS_NULL_PTR(center_x)) *center_x = centroid_x * inv_divisor;
  if(!IS_NULL_PTR(center_y)) *center_y = centroid_y * inv_divisor;
  return TRUE;
}

/**
 * @brief Initialize fading from config and emit the toast with a size-normalized percentage.
 */
static int _init_fading(dt_masks_form_t *mask_form, const float amount,
                        const dt_masks_increment_t increment, const int flow,
                        const float mask_size, const float border_size)
{
  const float mask_fading = dt_masks_get_set_conf_value(mask_form, "fading", amount,
                                                        FADING_MIN, FADING_MAX, increment, flow);
  dt_toast_log(_("Fading: %3.2f%%"), (border_size * mask_fading) / mask_size * 100.0f);
  return 1;
}

/**
 * @brief Scale the polygon around its centroid.
 *
 * This preserves each node's local control-point offsets so the curve shape
 * scales uniformly with the polygon size.
 */
static int _change_size(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                        struct dt_iop_module_t *module, int form_index, const float amount,
                        const dt_masks_increment_t increment, const int flow)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return 0;

  float center_x = 0.0f;
  float center_y = 0.0f;
  float signed_area = 0.0f;
  if(!_polygon_form_gravity_center(mask_form, &center_x, &center_y, &signed_area)) return 1;

  // Avoid expanding degenerate polygons or overly large shapes.
  if(amount < 1.0f && signed_area < 0.00001f && signed_area > -0.00001f) return 1;
  if(amount > 1.0f && signed_area > 4.0f) return 1;

  float scale_delta = amount;
  switch(increment)
  {
    case DT_MASKS_INCREMENT_SCALE:
      scale_delta = powf(amount, (float)flow);
      break;
    case DT_MASKS_INCREMENT_OFFSET:
      // For polygon global scaling, interpret offset as multiplicative offset around 1.0.
      scale_delta = 1.0f + amount * (float)flow;
      break;
    case DT_MASKS_INCREMENT_ABSOLUTE:
    default:
      scale_delta = amount;
      break;
  }

  int node_index = 0;
  for(GList *node_iter = mask_form->points; node_iter; node_iter = g_list_next(node_iter), node_index++)
  {
    if(dt_masks_gui_change_affects_selected_node_or_all(mask_gui, node_index))
    {
      dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)node_iter->data;
      if(!node) continue;

      const float new_node_x = center_x + (node->node[0] - center_x) * scale_delta;
      const float new_node_y = center_y + (node->node[1] - center_y) * scale_delta;
      const float ctrl1_offset_x = (node->ctrl1[0] - node->node[0]) * scale_delta;
      const float ctrl1_offset_y = (node->ctrl1[1] - node->node[1]) * scale_delta;
      const float ctrl2_offset_x = (node->ctrl2[0] - node->node[0]) * scale_delta;
      const float ctrl2_offset_y = (node->ctrl2[1] - node->node[1]) * scale_delta;

      // Update all coordinates while keeping local offsets consistent.
      node->node[0] = new_node_x;
      node->node[1] = new_node_y;
      node->ctrl1[0] = new_node_x + ctrl1_offset_x;
      node->ctrl1[1] = new_node_y + ctrl1_offset_y;
      node->ctrl2[0] = new_node_x + ctrl2_offset_x;
      node->ctrl2[1] = new_node_y + ctrl2_offset_y;
    }
  }

  float mask_size = 0.0f;
  _polygon_get_sizes(module, mask_form, mask_gui, form_index, &mask_size, NULL);

  dt_toast_log(_("Size: %3.2f%%"), mask_size * 100.0f);

  // Rebuild the cached GUI geometry.
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  return 1;
}

/**
 * @brief Change polygon fading for the active node scope or the full shape.
 */
static int _change_fading(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                          struct dt_iop_module_t *module, int form_index, const float amount,
                          const dt_masks_increment_t increment, int flow)
{
  int node_index = 0;
  const float scale_amount = powf(amount, (float)flow);
  const float offset_amount = amount * (float)flow;

  for(GList *node_iter = mask_form->points; node_iter; node_iter = g_list_next(node_iter), node_index++)
  {
    if(dt_masks_gui_change_affects_selected_node_or_all(mask_gui, node_index))
    {
      dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)node_iter->data;
      if(!node) continue;

      node->border[0] = CLAMPF(dt_masks_apply_increment_precomputed(node->border[0], amount, scale_amount,
                                                                     offset_amount, increment),
                               FADING_MIN, FADING_MAX);
      node->border[1] = CLAMPF(dt_masks_apply_increment_precomputed(node->border[1], amount, scale_amount,
                                                                     offset_amount, increment),
                               FADING_MIN, FADING_MAX);
    }
  }

  float mask_size = 1.0f;
  float border_size = 0.0f;
  _polygon_get_sizes(module, mask_form, mask_gui, form_index, &mask_size, &border_size);

  _init_fading(mask_form, amount, increment, flow, mask_size, border_size);

  // Rebuild the cached GUI geometry.
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);

  return 1;
}

/**
 * @brief Handle mouse wheel updates for polygon size/fading/opacity.
 */
/* Shape handlers receive widget-space coordinates, while normalized output-image
 * coordinates come from `mask_gui->rel_pos` and absolute output-image
 * coordinates come from `mask_gui->pos`. */
static int _polygon_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, int flow,
                                          uint32_t state, dt_masks_form_t *mask_form, int parent_id,
                                          dt_masks_form_gui_t *mask_gui, int form_index,
                                          dt_masks_interaction_t interaction)
{
  
  
  
  if(mask_gui->creation)
  {
    // no change during creation
    return 0;
  }

  /* `state` is the caller's raw key state, kept for the callback signature: the property to
   * act on was already resolved from it by dt_masks_scroll_get_interaction(). A polygon owns
   * no rotation, so that mapping falls through and the wheel does nothing here. Size and
   * fading apply to the selected node when there is one, to every node otherwise -- that
   * scoping is the selection's business (dt_masks_gui_change_affects_selected_node_or_all),
   * not the wheel's, which is why a selected node no longer forces fading. */
  if(mask_gui->edit_mode == DT_MASKS_EDIT_FULL && dt_masks_is_anything_selected(mask_gui))
  {
    switch(interaction)
    {
      case DT_MASKS_INTERACTION_OPACITY:
        return dt_masks_form_change_opacity(mask_gui->dev, mask_form, parent_id, up, flow);
      case DT_MASKS_INTERACTION_FADING:
        return _change_fading(mask_form, parent_id, mask_gui, module, form_index, up ? +0.01f : -0.01f,
                              DT_MASKS_INCREMENT_OFFSET, flow);
      case DT_MASKS_INTERACTION_SIZE:
        return _change_size(mask_form, parent_id, mask_gui, module, form_index, up ? 1.02f : 0.98f,
                            DT_MASKS_INCREMENT_SCALE, flow);
      default:
        return 0;
    }
  }
  return 0;
}

/**
 * @brief Close the polygon creation by removing the temporary last node.
 */
static int _polygon_creation_closing_form(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui)
{
  // we don't want a form with less than 3 points
  if(g_list_shorter_than(mask_form->points, 4))
  {
    dt_toast_log(_("Polygon mask requires at least 3 nodes."));
    return 1;
  }

  dt_iop_module_t *creation_module = mask_gui->creation_module;
  // we delete last point (the one we are currently dragging)
  dt_masks_node_polygon_t *last_node = (dt_masks_node_polygon_t *)g_list_last(mask_form->points)->data;
  mask_form->points = g_list_remove(mask_form->points, last_node);
  dt_free(last_node);

  mask_gui->node_dragging = -1;
  _polygon_init_ctrl_points(mask_form);

  dt_masks_gui_form_save_creation(mask_gui->dev, creation_module, mask_form, mask_gui);

  return 1;
}

static int _polygon_events_button_pressed(struct dt_iop_module_t *module, double x, double y,
                                       double pressure, int which, int type, uint32_t state,
                                       dt_masks_form_t *mask_form, int parent_id,
                                       dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;

  if(which == 1)
  {
    if(mask_gui->creation)
    {
      if(mask_gui->creation_closing_form)
        return _polygon_creation_closing_form(mask_form, mask_gui);

      if(dt_modifier_is(state, DT_PRIMARY_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(mask_form->type & DT_MASKS_CLONE)
        {
          dt_masks_set_source_pos_initial_state(mask_gui, state);
          return 1;
        }
      }

      else // we create a node
      {
        float masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/polygon/fading"), FADING_MAX);

        int node_count = g_list_length(mask_form->points);
        // change the values
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
        if(IS_NULL_PTR(polygon_node)) return 0;

        dt_masks_gui_cursor_to_raw_norm(mask_gui->dev, mask_gui, polygon_node->node);

        polygon_node->ctrl1[0] = polygon_node->ctrl1[1] = polygon_node->ctrl2[0] = polygon_node->ctrl2[1] = -1.0;
        polygon_node->border[0] = polygon_node->border[1] = MAX(FADING_MIN, masks_border);
        polygon_node->state = DT_MASKS_POINT_STATE_NORMAL;
  
        if(node_count == 0)
        {
          // create the first node
          dt_masks_node_polygon_t *polygon_first_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
          polygon_first_node->node[0] = polygon_node->node[0];
          polygon_first_node->node[1] = polygon_node->node[1];
          polygon_first_node->ctrl1[0] = polygon_first_node->ctrl1[1] = polygon_first_node->ctrl2[0] = polygon_first_node->ctrl2[1] = -1.0;
          polygon_first_node->border[0] = polygon_first_node->border[1] = MAX(FADING_MIN, masks_border);
          polygon_first_node->state = DT_MASKS_POINT_STATE_NORMAL;
          mask_form->points = g_list_append(mask_form->points, polygon_first_node);

          if(mask_form->type & DT_MASKS_CLONE)
          {
            dt_masks_set_source_pos_initial_value(mask_gui, mask_form);
          }
          else
          {
            // not used by regular masks
            mask_form->source[0] = mask_form->source[1] = 0.0f;
          }
          node_count++;
        }
        mask_form->points = g_list_append(mask_form->points, polygon_node);

        // if this is a ctrl click, the last created point is a sharp one
        if(dt_modifier_is(state, DT_PRIMARY_MASK))
        {
          dt_masks_node_polygon_t *polygon_last_node = g_list_nth_data(mask_form->points, node_count - 1);
          polygon_last_node->ctrl1[0] = polygon_last_node->ctrl2[0] = polygon_last_node->node[0];
          polygon_last_node->ctrl1[1] = polygon_last_node->ctrl2[1] = polygon_last_node->node[1];
          polygon_last_node->state = DT_MASKS_POINT_STATE_USER;
        }

        mask_gui->node_hovered = node_count;
        mask_gui->node_selected = TRUE;
        mask_gui->node_selected_idx = node_count;
        mask_gui->node_dragging = node_count;
        _polygon_init_ctrl_points(mask_form);
      }

      // we recreate the form points in all case
      dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    
      return 1;
    }// end of creation mode

    dt_masks_form_gui_points_t *gui_points
        = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
    if(IS_NULL_PTR(gui_points)) return 0;

    // The shape handler runs before the shared press-state selection update,
    // so concrete hovered targets must win over stale form/source selection.
    else if(mask_gui->node_hovered >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(mask_gui->node_selected && dt_modifier_is(state, DT_PRIMARY_MASK))
      {
        dt_masks_node_polygon_t *node
            = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_hovered);
        if(IS_NULL_PTR(node)) return 0;
        dt_masks_toggle_bezier_node_type(module, mask_form, mask_gui, form_index, gui_points,
                                         mask_gui->node_hovered, node->node, node->ctrl1, node->ctrl2,
                                         &node->state);
        return 1;
      }
      /*// we register the current position to avoid accidental move
      if(mask_gui->node_selected < 0 && mask_gui->scrollx == 0.0f && mask_gui->scrolly == 0.0f)
      {
        mask_gui->scrollx = pzx;
        mask_gui->scrolly = pzy;
      }*/
      mask_gui->delta[0] = gui_points->points[mask_gui->node_hovered * 6 + 2] - mask_gui->pos[0];
      mask_gui->delta[1] = gui_points->points[mask_gui->node_hovered * 6 + 3] - mask_gui->pos[1];

      return 1;
    }
    else if(mask_gui->handle_hovered >= 0)
    {  
      if(!dt_masks_node_is_cusp(gui_points, mask_gui->handle_hovered))
      {
        // we need to find the handle position
        float handle_x, handle_y;
        const int handle_index = mask_gui->handle_hovered;
        _polygon_ctrl2_to_handle(gui_points->points[handle_index * 6 + 2],
                                 gui_points->points[handle_index * 6 + 3],
                                 gui_points->points[handle_index * 6 + 4],
                                 gui_points->points[handle_index * 6 + 5],
                                 &handle_x, &handle_y, gui_points->clockwise);
        // compute offsets
        mask_gui->delta[0] = handle_x - mask_gui->pos[0];
        mask_gui->delta[1] = handle_y - mask_gui->pos[1];

        return 1;
      }
    }
    else if(mask_gui->handle_border_hovered >= 0)
    {
      const float handle_x = gui_points->border[mask_gui->handle_border_hovered * 6];
      const float handle_y = gui_points->border[mask_gui->handle_border_hovered * 6 + 1];
      mask_gui->delta[0] = handle_x - mask_gui->pos[0];
      mask_gui->delta[1] = handle_y - mask_gui->pos[1];

      return 1;
    }
    else if(mask_gui->seg_hovered >= 0)
    {
      mask_gui->node_hovered = -1;

      if(dt_modifier_is(state, DT_PRIMARY_MASK))
      {
        _add_node_to_segment(module, mask_form, parent_id, mask_gui, form_index);
      }
      else
      {
        // we move the entire segment
        mask_gui->delta[0] = gui_points->points[mask_gui->seg_hovered * 6 + 2] - mask_gui->pos[0];
        mask_gui->delta[1] = gui_points->points[mask_gui->seg_hovered * 6 + 3] - mask_gui->pos[1];
      }
      return 1;
    }
    else if(mask_gui->source_selected && mask_gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the source dragging
      mask_gui->delta[0] = gui_points->source[2] - mask_gui->pos[0];
      mask_gui->delta[1] = gui_points->source[3] - mask_gui->pos[1];
      return 1;
    }
    else if(mask_gui->form_selected && mask_gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the form dragging
      mask_gui->delta[0] = gui_points->points[2] - mask_gui->pos[0];
      mask_gui->delta[1] = gui_points->points[3] - mask_gui->pos[1];
      return 1;
    }
  }

  return 0;
}

static int _polygon_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                           uint32_t state, dt_masks_form_t *mask_form, int parent_id,
                                           dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(IS_NULL_PTR(mask_gui)) return 0;
  if(mask_gui->creation) return 1;

  if(which == 1)
  {
    if(dt_masks_gui_is_dragging(mask_gui))
      return 1;
  }
  return 0;
}

static int _polygon_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event,
                                       dt_masks_form_t *mask_form, int parent_id,
                                       dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(IS_NULL_PTR(mask_gui) || IS_NULL_PTR(mask_form)) return 0;

  guint key = dt_keys_mainpad_alternatives(event->keyval);


  if(mask_gui->creation)
  {
    switch(key)
    {
      case GDK_KEY_BackSpace:
      {
        // Minimum points to create a polygon
        if(mask_gui->node_dragging < 1)
        {
          dt_masks_form_exit_creation(module, mask_gui);
          return 1;
        }
        // switch previous node coords to the current one
        dt_masks_node_polygon_t *previous_node
            = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_dragging - 1);
        dt_masks_node_polygon_t *current_node
            = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_dragging);
        if(!previous_node || !current_node) return 0;
        previous_node->node[0] = current_node->node[0];
        previous_node->node[1] = current_node->node[1];

        dt_masks_remove_node(module, mask_form, 0, mask_gui, 0, mask_gui->node_dragging);
        // Decrease the current dragging node index
        mask_gui->node_dragging -= 1;

        dt_dev_pixelpipe_update_history_preview(mask_gui->dev);
        return 1;
      }
      case GDK_KEY_Return:
        return _polygon_creation_closing_form(mask_form, mask_gui);
    }
  }
  return 0;
}

/**
 * @brief Polygon mouse-move handler.
 *
 * Widget-space coordinates are only used by the top-level dispatcher.
 * Absolute output-image coordinates come from `mask_gui->pos`, normalized
 * output-image coordinates come from `mask_gui->rel_pos`, and raw-space edits are derived
 * locally through the appropriate backtransform helper.
 */
static int _polygon_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure,
                                       int which, dt_masks_form_t *mask_form, int parent_id,
                                       dt_masks_form_gui_t *mask_gui, int form_index)
{
  // centre view will have zoom_scale * backbuf_width pixels, we want the handle offset to scale with DPI:
  dt_develop_t *const dev = mask_gui->dev;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return 0;

  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);
  const int iwidth = geometry.raw_width;
  const int iheight = geometry.raw_height;

  if(mask_gui->node_dragging >= 0)
  {
    if(IS_NULL_PTR(mask_form->points)) return 0;
    if(mask_gui->creation && !g_list_shorter_than(mask_form->points, 4))
    {
      // check if we are near_handle the first point to close the polygon on creation
      const float dist_curs = DT_GUI_MOUSE_EFFECT_RADIUS;
      const float dx = mask_gui->pos[0] - gui_points->points[2];
      const float dy = mask_gui->pos[1] - gui_points->points[3];
      const float dist2 = dx * dx + dy * dy;
      mask_gui->creation_closing_form = dist2 <= dist_curs * dist_curs;
    }

    dt_masks_node_polygon_t *dragged_node
        = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_dragging);
    if(IS_NULL_PTR(dragged_node)) return 0;

    float dx = 0.0f;
    float dy = 0.0f;
    dt_masks_gui_delta_from_raw_anchor(dev, mask_gui, dragged_node->node, &dx, &dy);
    _polygon_translate_node(dragged_node, dx, dy);

    // if first point, adjust the source position accordingly
    if((mask_form->type & DT_MASKS_CLONE) && mask_gui->node_dragging == 0)
      dt_masks_translate_source(mask_form, dx, dy);

    if(mask_gui->creation)
      _polygon_init_ctrl_points(mask_form);

    // we recreate the form points
    if(dt_masks_gui_form_create_throttled(mask_form, mask_gui, form_index, module,
                                          mask_gui->pos[0], mask_gui->pos[1]))
      gui_points->clockwise = _polygon_is_clockwise(mask_form);

    return 1;
  }
  else if(mask_gui->creation)
  {
    // Let the cursor motion be redrawn as it moves in GUI
    return 1;
  }

  if(IS_NULL_PTR(mask_form->points)) return 0;
  const guint node_count = g_list_length(mask_form->points);

  if(mask_gui->seg_dragging >= 0)
  {
    const GList *const pt = g_list_nth(mask_form->points, mask_gui->seg_dragging);
    const GList *const next_pt = g_list_next_wraparound(pt, mask_form->points);
    dt_masks_node_polygon_t *point = (dt_masks_node_polygon_t *)pt->data;
    dt_masks_node_polygon_t *next_point = (dt_masks_node_polygon_t *)next_pt->data;
    if(IS_NULL_PTR(point) || IS_NULL_PTR(next_point)) return 0;

    float dx = 0.0f;
    float dy = 0.0f;
    dt_masks_gui_delta_from_raw_anchor(dev, mask_gui, point->node, &dx, &dy);

    // if first or last segment, update the source accordingly
    // (the source point follows the first/last segment when moved)
    if((mask_form->type & DT_MASKS_CLONE)
       && (mask_gui->seg_dragging == 0 || mask_gui->seg_dragging == (int)node_count - 1))
      dt_masks_translate_source(mask_form, dx, dy);

    _polygon_translate_node(point, dx, dy);
    _polygon_translate_node(next_point, dx, dy);

    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, form_index, module,
                                       mask_gui->pos[0], mask_gui->pos[1]);
    gui_points->clockwise = _polygon_is_clockwise(mask_form);

    return 1;
  }
  else if(mask_gui->handle_dragging >= 0)
  {
    dt_masks_node_polygon_t *node
        = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->handle_dragging);
    if(IS_NULL_PTR(node)) return 0;

    float pts[2];
    dt_masks_gui_delta_to_image_abs(mask_gui, pts);

    // compute ctrl points directly from new handle position
    float p[4];
    _polygon_handle_to_ctrl(gui_points->points[mask_gui->handle_dragging * 6 + 2],
                            gui_points->points[mask_gui->handle_dragging * 6 + 3],
                            pts[0], pts[1], &p[0], &p[1], &p[2], &p[3], gui_points->clockwise);

    dt_dev_coordinates_image_abs_to_raw_norm(dev, p, 2);

    // set new ctrl points
    dt_masks_set_ctrl_points(node->ctrl1, node->ctrl2, p);
    node->state = DT_MASKS_POINT_STATE_USER;

    _polygon_init_ctrl_points(mask_form);
    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, form_index, module,
                                       mask_gui->pos[0], mask_gui->pos[1]);

    return 1;
  }
  else if(mask_gui->handle_border_dragging >= 0)
  {
    const int node_index = mask_gui->handle_border_dragging;
    dt_masks_node_polygon_t *node
        = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, node_index);
    if(IS_NULL_PTR(node)) return 0;

    const int base = node_index * 6;
    const int node_point_index = base + 2;

    // Get delta between the node and its border handle
    float pts[2];
    float cursor_pos[2];
    const float node_pos_gui[2] = { gui_points->points[node_point_index],
                                    gui_points->points[node_point_index + 1] };
    const float handle_pos[2] = { gui_points->border[base], gui_points->border[base + 1] };
    dt_masks_gui_delta_to_image_abs(mask_gui, cursor_pos);
    dt_masks_project_on_line(cursor_pos, node_pos_gui, handle_pos, pts);

    const float border = dt_masks_border_from_projected_handle(dev, node->node, pts, fminf(iwidth, iheight));

    node->border[0] = node->border[1] = border;
    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, form_index, module,
                                       mask_gui->pos[0], mask_gui->pos[1]);

    return 1;
  }
  else if(mask_gui->form_dragging || mask_gui->source_dragging)
  {
    if(mask_gui->form_dragging)
    {
      dt_masks_node_polygon_t *dragging_shape = (dt_masks_node_polygon_t *)(mask_form->points)->data;
      if(IS_NULL_PTR(dragging_shape)) return 0;
      float dx = 0.0f;
      float dy = 0.0f;
      dt_masks_gui_delta_from_raw_anchor(dev, mask_gui, dragging_shape->node, &dx, &dy);
      _polygon_translate_all_nodes(mask_form, dx, dy);
    }
    else
    {
      float raw_point[2];
      dt_masks_gui_delta_to_raw_norm(dev, mask_gui, raw_point);
      mask_form->source[0] = raw_point[0];
      mask_form->source[1] = raw_point[1];
    }

    // we recreate the form points
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    return 1;
  }
  return 0;
}

/**
 * @brief Draw a polygon or border polyline, skipping NaN points.
 */
static void _polygon_draw_shape(struct dt_develop_t *dev, cairo_t *cr, const float *point_buffer, const int point_count,
                                const int node_count, const gboolean draw_border, const gboolean draw_source,
                                const dt_masks_skip_range_t *skips, const int skip_count)
{
  /* dev and draw_source are the shared shape_draw_function_t signature; the source outline is
   * drawn by its own caller, which passes no exclusion list. */
  (void)dev; (void)draw_source;

  /* This used to ignore the exclusion list and test the buffer for NaN instead -- which polygon
   * never writes, since its cuts have travelled out-of-band since the #1313 refactor. So the
   * test matched nothing and the folds were drawn. Honouring the list is the fix and costs one
   * call: it is the same walk the brush does, because it is the same question. */
  dt_masks_draw_outline_runs(cr, point_buffer, node_count * 3 + draw_border, point_count,
                             skips, skip_count);
}

/**
 * @brief Draw polygon overlays (nodes, handles, borders, source) after exposure.
 */
static void _polygon_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *mask_gui,
                                        int form_index, int node_count)
{
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return;
  const int selected_node = dt_masks_gui_selected_node_index(mask_gui);
  const int selected_handle = dt_masks_gui_selected_handle_index(mask_gui);
  const int selected_handle_border = dt_masks_gui_selected_handle_border_index(mask_gui);

  if(mask_gui->creation)
  {
    // draw a cross where the source will be created
    dt_masks_form_t *visible_form = dt_masks_get_visible_form(mask_gui->dev);
    if(visible_form && (visible_form->type & DT_MASKS_CLONE))
    {
      const gboolean have_first_node = node_count && gui_points->points && gui_points->points_count > 1;
      float node_posx = have_first_node ? gui_points->points[2] : mask_gui->pos[0];
      float node_posy = have_first_node ? gui_points->points[3] : mask_gui->pos[1];

      dt_masks_draw_source_preview(cr, zoom_scale, mask_gui, node_posx, node_posy, node_posx, node_posy, FALSE);
    }
  }

  // update clockwise info for the handles
  else if((mask_gui->type & DT_MASKS_IS_RETOUCHE) != 0 || mask_gui->node_selected || mask_gui->node_dragging >= 0
          || mask_gui->handle_selected)
  {
    dt_masks_form_t *group_form = dt_masks_get_visible_form(mask_gui->dev);
    if(!IS_NULL_PTR(group_form) && (group_form->type & DT_MASKS_GROUP))
    {
      dt_masks_form_group_t *group_entry = g_list_nth_data(group_form->points, form_index);
      dt_masks_form_t *polygon_form = group_entry
                                          ? dt_masks_get_from_id(mask_gui->dev, group_entry->formid)
                                          : NULL;
      if(!IS_NULL_PTR(polygon_form)) gui_points->clockwise = _polygon_is_clockwise(polygon_form);
    }
  }

  // draw polygon
  if(gui_points->points && node_count > 0 && gui_points->points_count > node_count * 3 + 6) // there must be something to draw
  {
    dt_masks_draw_path_seg_by_seg(cr, mask_gui, form_index, gui_points->points, gui_points->points_count,
                                  node_count, zoom_scale, FALSE);
  }

  if(mask_gui->group_selected == form_index)
  {
    // draw borders
    if(gui_points->border_count > node_count * 3 + 2)
    {
      dt_draw_shape_lines(mask_gui->dev, DT_MASKS_DASH_STICK, FALSE, cr, node_count, (mask_gui->border_selected), zoom_scale,
                          gui_points->border, gui_points->border_count, &dt_masks_functions_polygon.draw_shape,
                          CAIRO_LINE_CAP_ROUND, gui_points->border_skips, gui_points->border_skip_count);
    }

    // draw the current node's handle if it's a curve node
    if(mask_gui->node_selected && selected_node >= 0 && selected_node < node_count
       && !dt_masks_node_is_cusp(gui_points, selected_node))
    {
      const int node_index = selected_node;
      float handle[2];
      _polygon_ctrl2_to_handle(gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3],
                               gui_points->points[node_index * 6 + 4], gui_points->points[node_index * 6 + 5],
                               &handle[0], &handle[1], gui_points->clockwise);
      const float pt[2] = { gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3] };
      const gboolean selected = (mask_gui->node_hovered == node_index
                                 || (selected_handle == node_index)
                                 || (mask_gui->handle_hovered == node_index));
      dt_draw_handle(cr, pt, zoom_scale, handle, selected, FALSE);
    }
  }

  // draw nodes
  if(mask_gui->group_selected == form_index || mask_gui->creation)
  {
    for(int node_index = 0; node_index < node_count; node_index++)
    {
      // don't draw the last node while creating
      if(mask_gui->creation && node_index == node_count - 1) break;
      if(IS_NULL_PTR(gui_points->points) || gui_points->points_count <= node_index * 3 + 1) break;

      const gboolean squared = dt_masks_node_is_cusp(gui_points, node_index);
      const gboolean selected = (node_index == mask_gui->node_hovered || node_index == mask_gui->node_dragging);
      const gboolean action = (node_index == selected_node);
      const float x = gui_points->points[node_index * 6 + 2];
      const float y = gui_points->points[node_index * 6 + 3];
     
      // draw the first node as big circle while creating the polygon
      if(mask_gui->creation && node_index == 0)
        dt_draw_node(cr, FALSE, TRUE, TRUE, zoom_scale, x, y);
      else
        dt_draw_node(cr, squared, action, selected, zoom_scale, x, y);
    }

    // Draw the current node's border handle, if needed
    if(mask_gui->node_selected && selected_node >= 0 && selected_node < node_count
       && gui_points->border && gui_points->border_count > selected_node * 3 && !mask_gui->creation)
    {
      const int edited = selected_node;
      const gboolean selected = (mask_gui->node_hovered == edited
                                 || (selected_handle_border == edited)
                                 || mask_gui->handle_border_hovered == edited);
      const int curr_node = edited * 6;  
      const float handle[2] = { gui_points->border[curr_node], gui_points->border[curr_node + 1] };

      dt_draw_handle(cr, NULL, zoom_scale, handle, selected, TRUE);
    }
  }

  // draw the source if needed
  if(gui_points->source && gui_points->source_count > node_count * 3 + 2
     && gui_points->points && gui_points->points_count > 0)
  {
    dt_masks_gui_center_point_t center_pt = { .main = { gui_points->points[0], gui_points->points[1] },
                                              .source = { gui_points->source[0], gui_points->source[1] } };
    _polygon_gui_gravity_center(gui_points->points, gui_points->points_count,
                                &center_pt.main.x, &center_pt.main.y, NULL);
    // project the source's center point from the center of gravity
    float offset_x = gui_points->source[0] - gui_points->points[0];
    float offset_y = gui_points->source[1] - gui_points->points[1];
    center_pt.source.x = center_pt.main.x + offset_x;
    center_pt.source.y = center_pt.main.y + offset_y;
    dt_masks_draw_source(cr, mask_gui, form_index, node_count, zoom_scale,
                         &center_pt, &dt_masks_functions_polygon.draw_shape);
    
    //draw the current node projection
    for(int node_index = 0; node_index < node_count; node_index++)
    {
      if(mask_gui->group_selected == form_index
         && (node_index == mask_gui->node_hovered || node_index == selected_node
             || (mask_gui->creation && node_index == node_count - 1)))
      {
        const int proj_index = node_index * 6 + 2;
        if(gui_points->source_count <= node_index * 3 + 1) break;
        const float proj[2] = { gui_points->source[proj_index], gui_points->source[proj_index + 1] };
        const gboolean selected = mask_gui->node_hovered == node_index;
        const gboolean squared = dt_masks_node_is_cusp(gui_points, node_index);

        dt_draw_handle(cr, NULL, zoom_scale, proj, selected, squared);
      }
    }
  }
}

/**
 * @brief Compute raw bounding box for polygon points and border samples.
 */
static void _polygon_bounding_box_raw(const float *const point_buffer, const float *border_buffer,
                                      const int corner_count, const int point_count, int border_count,
                                      float *x_min, float *x_max, float *y_min, float *y_max)
{
  /* -FLT_MAX, not FLT_MIN: FLT_MIN is the smallest POSITIVE float, and a running maximum seeded
   * with it clamps the box at 0 for a shape entirely off the left or top edge */
  float xmin = FLT_MAX;
  float ymin = FLT_MAX;
  float xmax = -FLT_MAX;
  float ymax = -FLT_MAX;
  for(int border_index = corner_count * 3; border_index < border_count; border_index++)
  {
    // A cut span is not shape: its points are the offset curve folded over itself, and the
    // walks that render the mask never visit them, so the box must not grow to include them.
    // we look at the borders
    const float xx = border_buffer[border_index * 2];
    const float yy = border_buffer[border_index * 2 + 1];
    xmin = MIN(xx, xmin);
    xmax = MAX(xx, xmax);
    ymin = MIN(yy, ymin);
    ymax = MAX(yy, ymax);
  }
  for(int point_index = corner_count * 3; point_index < point_count; point_index++)
  {
    // we look at the polygon too
    const float xx = point_buffer[point_index * 2];
    const float yy = point_buffer[point_index * 2 + 1];
    xmin = MIN(xx, xmin);
    xmax = MAX(xx, xmax);
    ymin = MIN(yy, ymin);
    ymax = MAX(yy, ymax);
  }

  *x_min = xmin;
  *x_max = xmax;
  *y_min = ymin;
  *y_max = ymax;
}

/**
 * @brief Compute bounding box and add a small padding for rasterization safety.
 */
static void _polygon_bounding_box(const float *const point_buffer, const float *border_buffer,
                                  const int corner_count, const int point_count, int border_count,
                                  int *width, int *height, int *posx, int *posy)
{
  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  _polygon_bounding_box_raw(point_buffer, border_buffer, corner_count, point_count, border_count,
                            &xmin, &xmax, &ymin, &ymax);
  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
}

static int _get_area(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                     const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const mask_form, int *width, int *height, int *posx, int *posy,
                     gboolean get_source)
{
  if(IS_NULL_PTR(module)) return 1;

  // we get buffers for all points
  float *point_buffer = NULL;
  float *border_buffer = NULL;
  int point_count = 0;
  int border_count = 0;

  const dt_masks_distort_t pipe_dist = dt_masks_distort_for_pipe(pipe, module->dev);
  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &pipe_dist,
                             &point_buffer, &point_count, &border_buffer, &border_count, get_source) != 0)
  {
    dt_pixelpipe_cache_free_align(point_buffer);
    dt_pixelpipe_cache_free_align(border_buffer);
    return 1;
  }

  const guint corner_count = g_list_length(mask_form->points);
  _polygon_bounding_box(point_buffer, border_buffer, corner_count, point_count, border_count,
                        width, height, posx, posy);

  dt_pixelpipe_cache_free_align(point_buffer);
  dt_pixelpipe_cache_free_align(border_buffer);
  return 0;
}

static dt_masks_raster_result_t _polygon_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                                   dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *mask_form, int *width, int *height, int *posx, int *posy)
{
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  return dt_masks_raster_from_status(
      _get_area(module, pipe, piece, mask_form, width, height, posx, posy, TRUE));
}

static dt_masks_raster_result_t _polygon_get_area(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                             const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const mask_form,
                             int *width, int *height, int *posx, int *posy)
{
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  return dt_masks_raster_from_status(
      _get_area(module, pipe, piece, mask_form, width, height, posx, posy, FALSE));
}

/**
 * @brief Write a falloff segment into the mask buffer.
 */
/*static*/ void _polygon_falloff(float *const restrict buffer, int *p0, int *p1,
                                 int posx, int posy, int buffer_width)
{
  // segment length
  int l = dt_fast_hypotf(p1[0] - p0[0], p1[1] - p0[1]) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];
  const float inv_l = 1.0f / (float)l;

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx * inv_l) + p0[0] - posx;
    const int y = (int)((float)i * ly * inv_l) + p0[1] - posy;
    const float op = 1.0f - (float)i * inv_l;
    const size_t idx = y * buffer_width + x;
    buffer[idx] = fmaxf(buffer[idx], op);
    if(x > 0)
      buffer[idx - 1] = fmaxf(buffer[idx - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      buffer[idx - buffer_width] = fmaxf(buffer[idx - buffer_width], op); // this one is to avoid gap due to int rounding
  }
}

/** How many sub-steps bridge the jump between two consecutive feather spokes.
 *
 * The feather is the union of segments from each outline sample to its border sample, so it stays
 * continuous only while consecutive segments are within a pixel of each other at BOTH ends. At a
 * cut boundary they are not: the border side collapses to the cut's resume point and the fan opens
 * in a single step, leaving a wedge nothing stamps -- the thin dark radial line reported as "a
 * radial spoke is missing" at node 12 of polygon #2 in issue #1313's sidecar. One sub-step per
 * pixel of the larger jump closes it.
 *
 * The two rasterisers differ only in where the bridging spokes GO, so the geometry lives here once
 * and each sink gets its own three-line wrapper below. */
static inline int _falloff_bridge_steps(const int *const last0, const int *const last1,
                                        const int *const p0, const int *const p1)
{
  const int jump = MAX(MAX(abs(p0[0] - last0[0]), abs(p0[1] - last0[1])),
                       MAX(abs(p1[0] - last1[0]), abs(p1[1] - last1[1])));
  /* a bound: past this the two samples are not a fan, they are unrelated geometry */
  return MIN(jump, 64);
}

static inline void _falloff_bridge_lerp(const int *const from, const int *const to, const float t,
                                        int *const out)
{
  out[0] = (int)floorf(from[0] + t * (to[0] - from[0]) + 0.5f);
  out[1] = (int)floorf(from[1] + t * (to[1] - from[1]) + 0.5f);
}

/** Stamp the bridging spokes straight into @p buffer. */
static inline void _falloff_bridge_stamp(float *const restrict buffer, const int *const last0,
                                         const int *const last1, const int *const p0,
                                         const int *const p1, const int posx, const int posy,
                                         const int width)
{
  const int steps = _falloff_bridge_steps(last0, last1, p0, p1);
  for(int k = 1; k < steps; k++)
  {
    const float t = (float)k / (float)steps;
    int b0[2];
    int b1[2];
    _falloff_bridge_lerp(last0, p0, t, b0);
    _falloff_bridge_lerp(last1, p1, t, b1);
    _polygon_falloff(buffer, b0, b1, posx, posy, width);
  }
}

/** Queue the bridging spokes for the ROI path, which stamps them in parallel afterwards. Returns
 * the new write index. Same geometry as _falloff_bridge_stamp(), different sink. */
static inline int _falloff_bridge_queue(int *const dpoints, int dindex, const int capacity,
                                        const int *const last0, const int *const last1,
                                        const int *const p0, const int *const p1)
{
  const int steps = _falloff_bridge_steps(last0, last1, p0, p1);
  for(int k = 1; k < steps && dindex + 4 <= capacity; k++)
  {
    const float t = (float)k / (float)steps;
    int b0[2];
    int b1[2];
    _falloff_bridge_lerp(last0, p0, t, b0);
    _falloff_bridge_lerp(last1, p1, t, b1);
    dpoints[dindex] = b0[0];
    dpoints[dindex + 1] = b0[1];
    dpoints[dindex + 2] = b1[0];
    dpoints[dindex + 3] = b1[1];
    dindex += 4;
  }
  return dindex;
}

static dt_masks_raster_result_t _polygon_get_mask(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                             const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const mask_form,
                             float **buffer, int *width, int *height, int *posx, int *posy)
{
  *buffer = NULL;
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  if(IS_NULL_PTR(module)) return DT_MASKS_RASTER_ERROR;
  double start = 0.0;
  double start2 = 0.0;

  if(dt_get_debug_flags() & DT_DEBUG_PERF) start = dt_get_wtime();

  // we get buffers for all points
  float *point_buffer = NULL;
  float *border_buffer = NULL;
  int point_count = 0;
  int border_count = 0;
  const dt_masks_distort_t pipe_dist = dt_masks_distort_for_pipe(pipe, module->dev);
  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order,
                             DT_DEV_TRANSFORM_DIR_BACK_INCL, &pipe_dist, &point_buffer, &point_count,
                             &border_buffer, &border_count, FALSE) != 0)
  {
    dt_pixelpipe_cache_free_align(point_buffer);
    dt_pixelpipe_cache_free_align(border_buffer);
    return DT_MASKS_RASTER_ERROR;
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon points took %0.04f sec\n",
             mask_form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  // now we want to find the area, so we search min/max points
  const guint corner_count = g_list_length(mask_form->points);
  _polygon_bounding_box(point_buffer, border_buffer, corner_count, point_count, border_count,
                        width, height, posx, posy);

  const int hb = *height;
  const int wb = *width;
  /* Nothing left to interpolate: the outline above is sampled at one pixel whatever the pipe's
   * step, because the self-intersection detector needs it that way, so consecutive falloff
   * segments are already adjacent. Interpolating between them would only stamp the same pixels
   * again. Polygon therefore buys no speed from a coarse step today -- making it do so means
   * decimating the PAINTING while keeping the geometry exact, which is a separate change. */
  const gboolean sparse = FALSE;
  const int sparse_factor = 1;

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  const size_t bufsize = (size_t)(*width) * (*height);
  // ensure that the buffer is zeroed, as the following code only actually sets the polygon+falloff pixels
  float *const restrict bufptr = *buffer = dt_pixelpipe_cache_alloc_align_float_cache(bufsize, 0);
  if(!IS_NULL_PTR(bufptr)) memset(bufptr, 0, sizeof(float) * bufsize);
  if(IS_NULL_PTR(*buffer))
  {
    dt_pixelpipe_cache_free_align(point_buffer);
    dt_pixelpipe_cache_free_align(border_buffer);
    return DT_MASKS_RASTER_ERROR;
  }

  // we write all the point around the polygon into the buffer
  const int border_point_count = border_count;
  if(border_point_count > 2)
  {
    int lastx = (int)point_buffer[(border_point_count - 1) * 2];
    int lasty = (int)point_buffer[(border_point_count - 1) * 2 + 1];
    int lasty2 = (int)point_buffer[(border_point_count - 2) * 2 + 1];

    int just_change_dir = 0;
    for(int ii = corner_count * 3; ii < 2 * border_point_count - corner_count * 3; ii++)
    {
      // we are writing more than 1 loop in the case the dir in y change
      // exactly at start/end point
      int i = ii;
      if(ii >= border_point_count)
        i = (ii - corner_count * 3) % (border_point_count - corner_count * 3) + corner_count * 3;
      const int xx = (int)point_buffer[i * 2];
      const int yy = (int)point_buffer[i * 2 + 1];

      // we don't store the point if it has the same y value as the last one
      if(yy == lasty) continue;

      // we want to be sure that there is no y jump
      if(yy - lasty > 1 || yy - lasty < -1)
      {
        if(yy < lasty)
        {
          for(int j = yy + 1; j < lasty; j++)
          {
            const int nx = (j - yy) * (lastx - xx) / (float)(lasty - yy) + xx;
            const size_t idx = (size_t)(j - (*posy)) * (*width) + nx - (*posx);
            assert(idx < bufsize);
            bufptr[idx] = 1.0f;
          }
          lasty2 = yy + 2;
          lasty = yy + 1;
        }
        else
        {
          for(int j = lasty + 1; j < yy; j++)
          {
            const int nx = (j - lasty) * (xx - lastx) / (float)(yy - lasty) + lastx;
            const size_t idx = (size_t)(j - (*posy)) * (*width) + nx - (*posx);
            assert(idx < bufsize);
            bufptr[idx] = 1.0f;
          }
          lasty2 = yy - 2;
          lasty = yy - 1;
        }
      }
      // if we change the direction of the polygon (in y), then we add a extra point
      if((lasty - lasty2) * (lasty - yy) > 0)
      {
        const size_t idx = (size_t)(lasty - (*posy)) * (*width) + lastx + 1 - (*posx);
        assert(idx < bufsize);
        bufptr[idx] = 1.0f;
        just_change_dir = 1;
      }
      // we add the point
      if(just_change_dir && ii == i)
      {
        // if we have changed the direction, we have to be careful that point can be at the same place
        // as the previous one, especially on sharp edges
        const size_t idx = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
        assert(idx < bufsize);
        float v = bufptr[idx];
        if(v > 0.0)
        {
          if(xx - (*posx) > 0)
          {
            const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - 1 - (*posx);
            assert(idx_ < bufsize);
            bufptr[idx_] = 1.0f;
          }
          else if(xx - (*posx) < (*width) - 1)
          {
            const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx + 1 - (*posx);
            assert(idx_ < bufsize);
            bufptr[idx_] = 1.0f;
          }
        }
        else
        {
          const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
          assert(idx_ < bufsize);
          bufptr[idx_] = 1.0f;
          just_change_dir = 0;
        }
      }
      else
      {
        const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
        assert(idx_ < bufsize);
        bufptr[idx_] = 1.0f;
      }
      // we change last values
      lasty2 = lasty;
      lasty = yy;
      lastx = xx;
      if(ii != i) break;
    }
  }
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill draw polygon took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }
  __OMP_PARALLEL_FOR__(if((size_t)hb * wb > 50000))
  for(int yy = 0; yy < hb; yy++)
  {
    float *const restrict row = bufptr + (size_t)yy * wb;
    int state = 0;
    for(int xx = 0; xx < wb; xx++)
    {
      const float v = row[xx];
      if(v == 1.0f) state = !state;
      if(state) row[xx] = 1.0f;
    }
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill plain took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // now we fill the falloff
  int p0[2] = { 0 }, p1[2] = { 0 };
  int prev0[2] = { 0 }, prev1[2] = { 0 };
  gboolean have_prev = FALSE;
  int last0[2] = { -100, -100 }, last1[2] = { -100, -100 };
  for(int i = corner_count * 3; i < border_count; i++)
  {
    p0[0] = point_buffer[i * 2];
    p0[1] = point_buffer[i * 2 + 1];

    /* Inside a cut, the border side of every falloff segment collapses to the cut's resume
     * point: the same segments the in-band jump used to produce, read from a range instead of
     * decoded out of a NaN slot. */
    const int border_index = i;
    p1[0] = border_buffer[border_index * 2];
    p1[1] = border_buffer[border_index * 2 + 1];

    const gboolean used_next = FALSE;

    if(sparse && have_prev && !used_next
       && (prev0[0] != p0[0] || prev0[1] != p0[1] || prev1[0] != p1[0] || prev1[1] != p1[1]))
    {
      for(int k = 1; k < sparse_factor; k++)
      {
        const float t = (float)k / (float)sparse_factor;
        int mp0[2] = { (int)floorf(prev0[0] + t * (p0[0] - prev0[0]) + 0.5f),
                       (int)floorf(prev0[1] + t * (p0[1] - prev0[1]) + 0.5f) };
        int mp1[2] = { (int)floorf(prev1[0] + t * (p1[0] - prev1[0]) + 0.5f),
                       (int)floorf(prev1[1] + t * (p1[1] - prev1[1]) + 0.5f) };
        _polygon_falloff(bufptr, mp0, mp1, *posx, *posy, *width);
      }
    }

    /* BRIDGE A JUMP BETWEEN CONSECUTIVE SPOKES.
     *
     * The feather is the union of segments from each outline sample to its border sample, so it
     * is only continuous while consecutive segments stay within a pixel of each other at BOTH
     * ends. At a cut boundary they do not: the border side collapses to the cut's resume point,
     * which is somewhere else entirely, and the fan opens in one step. Measured on polygon #2 of
     * issue #1313's sidecar, at the sample before the cut at 23528: the outline end moves a
     * fraction of a pixel while the border end jumps 4.57 px, and the wedge between the two
     * segments is never stamped -- a thin dark radial line through the feather, reported as "a
     * radial spoke is missing" at node 12. A second one, 2.80 px, sits before the cut at 6532.
     *
     * Subdividing until each step is at most a pixel closes it. This is not the same thing as
     * the `sparse' interpolation above, which fills in for samples deliberately not visited
     * when the outline is walked at reduced density; this fires on adjacent samples, where the
     * geometry itself is discontinuous. */
    if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
    {
      if(last0[0] > -100 || last0[1] > -100)
        _falloff_bridge_stamp(bufptr, last0, last1, p0, p1, *posx, *posy, *width);

      _polygon_falloff(bufptr, p0, p1, *posx, *posy, *width);
      last0[0] = p0[0];
      last0[1] = p0[1];
      last1[0] = p1[0];
      last1[1] = p1[1];
    }

    if(!used_next)
    {
      prev0[0] = p0[0];
      prev0[1] = p0[1];
      prev1[0] = p1[0];
      prev1[1] = p1[1];
      have_prev = TRUE;
    }
    else
    {
      have_prev = FALSE;
    }
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);

  dt_pixelpipe_cache_free_align(point_buffer);
  dt_pixelpipe_cache_free_align(border_buffer);
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start);

  return DT_MASKS_RASTER_OK;
}


/** crop polygon to roi given by xmin, xmax, ymin, ymax. polygon segments outside of roi are replaced by
    nodes lying on roi borders. */
static int _polygon_crop_to_roi(float *polygon, const int point_count, float xmin, float xmax, float ymin,
                             float ymax)
{
  int point_start = -1;
  int l = -1, r = -1;


  // first try to find a node clearly inside roi
  for(int k = 0; k < point_count; k++)
  {
    float x = polygon[2 * k];
    float y = polygon[2 * k + 1];

    if(x >= xmin + 1 && y >= ymin + 1
       && x <= xmax - 1 && y <= ymax - 1)
    {
      point_start = k;
      break;
    }
  }

  // printf("crop to xmin %f, xmax %f, ymin %f, ymax %f - start %d (%f, %f)\n", xmin, xmax, ymin, ymax,
  // point_start, polygon[2*point_start], polygon[2*point_start+1]);

  if(point_start < 0) return 0; // no point means roi lies completely within polygon

  typedef struct
  {
    int l;
    int r;
    float start;
    float delta;
  } roi_crop_segment_t;

  roi_crop_segment_t *xmax_segs = dt_alloc_align(sizeof(*xmax_segs) * point_count);
  roi_crop_segment_t *ymax_segs = dt_alloc_align(sizeof(*ymax_segs) * point_count);
  if(IS_NULL_PTR(xmax_segs) || IS_NULL_PTR(ymax_segs))
  {
    dt_free_align(xmax_segs);
    dt_free_align(ymax_segs);
    goto fallback_passes;
  }

  int xmin_l = -1, xmin_r = -1;
  int xmax_l = -1, xmax_r = -1;
  int xmax_count = 0;

  // find the crossing points with xmin/xmax in a single pass
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;
    const float x = polygon[2 * kk];

    if(xmin_l < 0 && x < xmin) xmin_l = k;       // where we leave roi (xmin)
    if(xmin_l >= 0 && x >= xmin) xmin_r = k - 1; // where we re-enter roi (xmin)

    if(xmin_l >= 0 && xmin_r >= 0)
    {
      const int count = xmin_r - xmin_l + 1;
      const int ll = (xmin_l - 1 + point_start) % point_count;
      const int rr = (xmin_r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (polygon[2 * rr + 1] - polygon[2 * ll + 1]) / (count - 1);
      const float start_y = polygon[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + xmin_l + point_start) % point_count;
        polygon[2 * nn] = xmin;
        polygon[2 * nn + 1] = start_y + n * delta_y;
      }

      xmin_l = xmin_r = -1;
    }

    if(xmax_l < 0 && x > xmax) xmax_l = k;       // where we leave roi (xmax)
    if(xmax_l >= 0 && x <= xmax) xmax_r = k - 1; // where we re-enter roi (xmax)

    if(xmax_l >= 0 && xmax_r >= 0)
    {
      const int count = xmax_r - xmax_l + 1;
      const int ll = (xmax_l - 1 + point_start) % point_count;
      const int rr = (xmax_r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (polygon[2 * rr + 1] - polygon[2 * ll + 1]) / (count - 1);
      const float start_y = polygon[2 * ll + 1];

      xmax_segs[xmax_count].l = xmax_l;
      xmax_segs[xmax_count].r = xmax_r;
      xmax_segs[xmax_count].start = start_y;
      xmax_segs[xmax_count].delta = delta_y;
      xmax_count++;

      xmax_l = xmax_r = -1;
    }
  }

  for(int s = 0; s < xmax_count; s++)
  {
    const int count = xmax_segs[s].r - xmax_segs[s].l + 1;
    const float start_y = xmax_segs[s].start;
    const float delta_y = xmax_segs[s].delta;
    for(int n = 0; n < count; n++)
    {
      const int nn = (n + xmax_segs[s].l + point_start) % point_count;
      polygon[2 * nn] = xmax;
      polygon[2 * nn + 1] = start_y + n * delta_y;
    }
  }

  dt_free_align(xmax_segs);

  int ymin_l = -1, ymin_r = -1;
  int ymax_l = -1, ymax_r = -1;
  int ymax_count = 0;

  // find the crossing points with ymin/ymax in a single pass
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;
    const float y = polygon[2 * kk + 1];

    if(ymin_l < 0 && y < ymin) ymin_l = k;       // where we leave roi (ymin)
    if(ymin_l >= 0 && y >= ymin) ymin_r = k - 1; // where we re-enter roi (ymin)

    if(ymin_l >= 0 && ymin_r >= 0)
    {
      const int count = ymin_r - ymin_l + 1;
      const int ll = (ymin_l - 1 + point_start) % point_count;
      const int rr = (ymin_r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (polygon[2 * rr] - polygon[2 * ll]) / (count - 1);
      const float start_x = polygon[2 * ll];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + ymin_l + point_start) % point_count;
        polygon[2 * nn] = start_x + n * delta_x;
        polygon[2 * nn + 1] = ymin;
      }

      ymin_l = ymin_r = -1;
    }

    if(ymax_l < 0 && y > ymax) ymax_l = k;       // where we leave roi (ymax)
    if(ymax_l >= 0 && y <= ymax) ymax_r = k - 1; // where we re-enter roi (ymax)

    if(ymax_l >= 0 && ymax_r >= 0)
    {
      const int count = ymax_r - ymax_l + 1;
      const int ll = (ymax_l - 1 + point_start) % point_count;
      const int rr = (ymax_r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (polygon[2 * rr] - polygon[2 * ll]) / (count - 1);
      const float start_x = polygon[2 * ll];

      ymax_segs[ymax_count].l = ymax_l;
      ymax_segs[ymax_count].r = ymax_r;
      ymax_segs[ymax_count].start = start_x;
      ymax_segs[ymax_count].delta = delta_x;
      ymax_count++;

      ymax_l = ymax_r = -1;
    }
  }

  for(int s = 0; s < ymax_count; s++)
  {
    const int count = ymax_segs[s].r - ymax_segs[s].l + 1;
    const float start_x = ymax_segs[s].start;
    const float delta_x = ymax_segs[s].delta;
    for(int n = 0; n < count; n++)
    {
      const int nn = (n + ymax_segs[s].l + point_start) % point_count;
      polygon[2 * nn] = start_x + n * delta_x;
      polygon[2 * nn + 1] = ymax;
    }
  }

  dt_free_align(ymax_segs);
  return 1;

fallback_passes:
  l = r = -1;
  // find the crossing points with xmin and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && polygon[2 * kk] < xmin) l = k;       // where we leave roi
    if(l >= 0 && polygon[2 * kk] >= xmin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (polygon[2 * rr + 1] - polygon[2 * ll + 1]) / (count - 1);
      const float start_y = polygon[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        polygon[2 * nn] = xmin;
        polygon[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with xmax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && polygon[2 * kk] > xmax) l = k;       // where we leave roi
    if(l >= 0 && polygon[2 * kk] <= xmax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (polygon[2 * rr + 1] - polygon[2 * ll + 1]) / (count - 1);
      const float start_y = polygon[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        polygon[2 * nn] = xmax;
        polygon[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymin and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && polygon[2 * kk + 1] < ymin) l = k;       // where we leave roi
    if(l >= 0 && polygon[2 * kk + 1] >= ymin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (polygon[2 * rr] - polygon[2 * ll]) / (count - 1);
      const float start_x = polygon[2 * ll];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        polygon[2 * nn] = start_x + n * delta_x;
        polygon[2 * nn + 1] = ymin;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && polygon[2 * kk + 1] > ymax) l = k;       // where we leave roi
    if(l >= 0 && polygon[2 * kk + 1] <= ymax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (polygon[2 * rr] - polygon[2 * ll]) / (count - 1);
      const float start_x = polygon[2 * ll];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        polygon[2 * nn] = start_x + n * delta_x;
        polygon[2 * nn + 1] = ymax;
      }

      l = r = -1;
    }
  }
  return 1;
}

/** we write a falloff segment respecting limits of buffer */
static inline void _polygon_falloff_roi(float *buffer, int *p0, int *p1, int bw, int bh)
{
  // segment length
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];
  const float inv_l = 1.0f / (float)l;

  const int dx = lx < 0 ? -1 : 1;
  const int dy = ly < 0 ? -1 : 1;
  const int dpy = dy * bw;

  const int x0 = p0[0], y0 = p0[1];
  const int x1 = p1[0], y1 = p1[1];
  if((x0 < 0 && x1 < 0) || (x0 >= bw && x1 >= bw) || (y0 < 0 && y1 < 0) || (y0 >= bh && y1 >= bh)) return;
  const int inside = (x0 >= 0 && x0 < bw && x1 >= 0 && x1 < bw && y0 >= 0 && y0 < bh && y1 >= 0 && y1 < bh);

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx * inv_l) + p0[0];
    const int y = (int)((float)i * ly * inv_l) + p0[1];
    const float op = 1.0f - (float)i * inv_l;
    if(!inside && (x < 0 || x >= bw || y < 0 || y >= bh)) continue;
    float *buf = buffer + (size_t)y * bw + x;
    if(inside)
      buf[0] = MAX(buf[0], op);
    else if(x >= 0 && x < bw && y >= 0 && y < bh)
      buf[0] = MAX(buf[0], op);
    if(x + dx >= 0 && x + dx < bw && y >= 0 && y < bh)
      buf[dx] = MAX(buf[dx], op); // this one is to avoid gap due to int rounding
    if(x >= 0 && x < bw && y + dy >= 0 && y + dy < bh)
      buf[dpy] = MAX(buf[dpy], op); // this one is to avoid gap due to int rounding
  }
}

// build a stamp which can be combined with other shapes in the same group
// prerequisite: 'buffer' is all zeros
static dt_masks_raster_result_t _polygon_get_mask_roi(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                                 const dt_dev_pixelpipe_iop_t *const piece,
                                 dt_masks_form_t *const mask_form,
                                 const dt_iop_roi_t *roi, float *buffer,
                                 dt_iop_roi_t *touched)
{
  dt_masks_touched_none(touched);
  if(IS_NULL_PTR(module)) return DT_MASKS_RASTER_ERROR;
  double start = 0.0;
  double start2 = 0.0;
  if(dt_get_debug_flags() & DT_DEBUG_PERF) start = dt_get_wtime();

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;
  /* Nothing left to interpolate: the outline above is sampled at one pixel whatever the pipe's
   * step, because the self-intersection detector needs it that way, so consecutive falloff
   * segments are already adjacent. Interpolating between them would only stamp the same pixels
   * again. Polygon therefore buys no speed from a coarse step today -- making it do so means
   * decimating the PAINTING while keeping the geometry exact, which is a separate change. */
  const gboolean sparse = FALSE;
  const int sparse_factor = 1;

  // we need to take care of four different cases:
  // 1) polygon and feather are outside of roi
  // 2) polygon is outside of roi, feather reaches into roi
  // 3) roi lies completely within polygon
  // 4) all other situations :)
  int polygon_in_roi = 0;
  int feather_in_roi = 0;
  int polygon_encircles_roi = 0;

  // we get buffers for all points
  float *points = NULL;
  float *border = NULL;
  int points_count = 0;
  int border_count = 0;
  const dt_masks_distort_t pipe_dist = dt_masks_distort_for_pipe(pipe, module->dev);
  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order,
                             DT_DEV_TRANSFORM_DIR_BACK_INCL, &pipe_dist,
                             &points, &points_count, &border, &border_count, FALSE) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return DT_MASKS_RASTER_ERROR;
  }
  /* nothing past the header: every segment was a point */
  if(points_count <= 2 || points_count <= (int)g_list_length(mask_form->points) * 3)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return DT_MASKS_RASTER_EMPTY;
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon points took %0.04f sec\n",
             mask_form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  const guint corner_count = g_list_length(mask_form->points);

  // we shift and scale down polygon and border. Cut spans are scaled too: their points are
  // valid coordinates (the cuts travel out-of-band now), and every reader below skips the
  // spans by range, so nothing observes them either way -- while the old skip-during-scaling
  // left them in IMAGE coordinates, which is what painted the feather from out-of-buffer
  // positions when a spurious cut appeared (issue #1116).
  for(int i = corner_count * 3; i < border_count; i++)
  {
    border[2 * i] = border[2 * i] * scale - px;
    border[2 * i + 1] = border[2 * i + 1] * scale - py;
  }
  for(int i = corner_count * 3; i < points_count; i++)
  {
    const float xx = points[2 * i];
    const float yy = points[2 * i + 1];
    points[2 * i] = xx * scale - px;
    points[2 * i + 1] = yy * scale - py;
  }

  // now check if polygon is at least partially within roi
  for(int i = corner_count * 3; i < points_count; i++)
  {
    const int xx = points[i * 2];
    const int yy = points[i * 2 + 1];

    if(xx > 1 && yy > 1 && xx < width - 2 && yy < height - 2)
    {
      polygon_in_roi = 1;
      break;
    }
  }

  // if not this still might mean that polygon fully encircles roi -> we need to check that
  if(!polygon_in_roi)
  {
    int crossing_count = 0;
    int last_y = -9999;
    const int x = width / 2;
    const int y = height / 2;

    for(int i = corner_count * 3; i < points_count; i++)
    {
      const int yy = (int)points[2 * i + 1];
      if(yy != last_y && yy == y)
      {
        if(points[2 * i] > x) crossing_count++;
      }
      last_y = yy;
    }
    // if there is an uneven number of intersection points roi lies within polygon
    if(crossing_count & 1)
    {
      polygon_in_roi = 1;
      polygon_encircles_roi = 1;
    }
  }

  // now check if feather is at least partially within roi. Cut spans are not feather.
  for(int i = corner_count * 3; i < border_count; i++)
  {
    const float xx = border[i * 2];
    const float yy = border[i * 2 + 1];
    if(xx > 1 && yy > 1 && xx < width - 2 && yy < height - 2)
    {
      feather_in_roi = 1;
      break;
    }
  }

  // if polygon and feather completely lie outside of roi -> we're done/mask remains empty
  if(!polygon_in_roi && !feather_in_roi)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return DT_MASKS_RASTER_EMPTY;
  }

  // now get min/max values
  float xmin, xmax, ymin, ymax;
  _polygon_bounding_box_raw(points, border, corner_count, points_count, border_count,
                            &xmin, &xmax, &ymin, &ymax);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill clear mask took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // deal with polygon if it does not lie outside of roi
  if(polygon_in_roi)
  {
    // second copy of polygon which we can modify when cropping to roi
    float *cpoints = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * points_count, 0);
    if(IS_NULL_PTR(cpoints))
    {
      dt_pixelpipe_cache_free_align(points);
      dt_pixelpipe_cache_free_align(border);
      return DT_MASKS_RASTER_ERROR;
    }
    memcpy(cpoints, points, sizeof(float) * 2 * points_count);

    // now we clip cpoints to roi -> catch special case when roi lies completely within polygon.
    // dirty trick: we allow polygon to extend one pixel beyond height-1. this avoids need of special handling
    // of the last roi line in the following edge-flag polygon fill algorithm.
    const int crop_success = _polygon_crop_to_roi(cpoints + 2 * (corner_count * 3),
                                                  points_count - corner_count * 3, 0,
                                                  width - 1, 0, height);
    polygon_encircles_roi = polygon_encircles_roi || !crop_success;

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill crop to roi took %0.04f sec\n", mask_form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }

    if(polygon_encircles_roi)
    {
      // roi lies completely within polygon
      for(size_t k = 0; k < (size_t)width * height; k++) buffer[k] = 1.0f;
      dt_masks_touched_full(touched, width, height);
    }
    else
    {
      // all other cases

      // edge-flag polygon fill: we write all the point around the polygon into the buffer
      float xlast = cpoints[(points_count - 1) * 2];
      float ylast = cpoints[(points_count - 1) * 2 + 1];

      for(int i = corner_count * 3; i < points_count; i++)
      {
        float xstart = xlast;
        float ystart = ylast;

        float xend = xlast = cpoints[i * 2];
        float yend = ylast = cpoints[i * 2 + 1];

        if(ystart > yend)
        {
          float tmp;
          tmp = ystart, ystart = yend, yend = tmp;
          tmp = xstart, xstart = xend, xend = tmp;
        }

        const float m = (xstart - xend) / (ystart - yend); // we don't need special handling of ystart==yend
                                                           // as following loop will take care

        for(int yy = (int)ceilf(ystart); (float)yy < yend;
            yy++) // this would normally never touch the last roi line => see comment further above
        {
          const float xcross = xstart + m * (yy - ystart);

          int xx = floorf(xcross);
          if((float)xx + 0.5f <= xcross) xx++;

          if(xx < 0 || xx >= width || yy < 0 || yy >= height)
            continue; // sanity check just to be on the safe side

          const size_t index = (size_t)yy * width + xx;

          buffer[index] = 1.0f - buffer[index];
        }
      }

      if(dt_get_debug_flags() & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill draw polygon took %0.04f sec\n", mask_form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }

      // we fill the inside plain
      // we don't need to deal with parts of shape outside of roi
      const int xxmin = MAX(xmin, 0);
      const int xxmax = MIN(xmax, width - 1);
      const int yymin = MAX(ymin, 0);
      const int yymax = MIN(ymax, height - 1);
      __OMP_PARALLEL_FOR__(if((size_t)(yymax - yymin + 1) * (size_t)(xxmax - xxmin + 1) > 50000))
      for(int yy = yymin; yy <= yymax; yy++)
      {
        float *const restrict row = buffer + (size_t)yy * width;
        int state = 0;
        for(int xx = xxmin; xx <= xxmax; xx++)
        {
          const float v = row[xx];
          if(v > 0.5f) state = !state;
          if(state) row[xx] = 1.0f;
        }
      }

      if(dt_get_debug_flags() & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill plain took %0.04f sec\n", mask_form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }
    }
    dt_pixelpipe_cache_free_align(cpoints);
  }

  // deal with feather if it does not lie outside of roi
  if(!polygon_encircles_roi)
  {
    /* the jump bridge below can add segments between two adjacent samples, so the buffer needs
     * headroom beyond one segment per sample; it is bounded and the writes are capacity-checked */
    const int dpoints_capacity = 4 * (border_count + 1024) * (sparse ? sparse_factor : 1);
    int *dpoints = dt_pixelpipe_cache_alloc_align_cache(sizeof(int) * dpoints_capacity, 0);
    if(IS_NULL_PTR(dpoints))
    {
      dt_pixelpipe_cache_free_align(points);
      dt_pixelpipe_cache_free_align(border);
      return DT_MASKS_RASTER_ERROR;
    }

    int dindex = 0;
    int p0[2], p1[2];
    int prev0[2] = { 0, 0 };
    int prev1[2] = { 0, 0 };
    gboolean have_prev = FALSE;
    int last0[2] = { -100, -100 };
    int last1[2] = { -100, -100 };
    gboolean have_last = FALSE;
    for(int i = corner_count * 3; i < border_count; i++)
    {
      p0[0] = floorf(points[i * 2] + 0.5f);
      p0[1] = ceilf(points[i * 2 + 1]);

      /* Inside a cut, the border side of every falloff segment collapses to the cut's resume
       * point: the same segments the in-band jump used to produce, read from a range instead
       * of decoded out of a NaN slot. */
      const int border_index = i;
      p1[0] = border[border_index * 2];
      p1[1] = border[border_index * 2 + 1];

      const gboolean used_next = FALSE;

      if(sparse && have_prev && !used_next
         && (prev0[0] != p0[0] || prev0[1] != p0[1] || prev1[0] != p1[0] || prev1[1] != p1[1]))
      {
        for(int k = 1; k < sparse_factor; k++)
        {
          const float t = (float)k / (float)sparse_factor;
          const int mp0[2] = { (int)floorf(prev0[0] + t * (p0[0] - prev0[0]) + 0.5f),
                               (int)floorf(prev0[1] + t * (p0[1] - prev0[1]) + 0.5f) };
          const int mp1[2] = { (int)floorf(prev1[0] + t * (p1[0] - prev1[0]) + 0.5f),
                               (int)floorf(prev1[1] + t * (p1[1] - prev1[1]) + 0.5f) };
          if(dindex + 4 <= dpoints_capacity)
          {
            dpoints[dindex] = mp0[0];
            dpoints[dindex + 1] = mp0[1];
            dpoints[dindex + 2] = mp1[0];
            dpoints[dindex + 3] = mp1[1];
            dindex += 4;
          }
        }
      }

      /* BRIDGE A JUMP BETWEEN CONSECUTIVE SPOKES.
       *
       * The feather is the union of segments from each outline sample to its border sample, so
       * it stays continuous only while consecutive segments are within a pixel of each other at
       * BOTH ends. At a cut boundary they are not: the border side collapses to the cut's
       * resume point, which is somewhere else, and the fan opens in a single step. Measured on
       * polygon #2 of issue #1313's sidecar, at the sample before the cut at 23528 -- the
       * outline end does not move at all while the border end jumps 5 px -- and the wedge
       * between the two segments is never stamped. That is the thin dark radial line through
       * the feather reported as "a radial spoke is missing" at node 12; a second, 3 px, sits
       * before the cut at 6532.
       *
       * Subdividing until each step is at most a pixel closes it. This is NOT the `sparse'
       * interpolation above: that one fills in for samples deliberately not visited when the
       * outline is walked at reduced density, and skips itself precisely when the spoke is
       * inside a cut -- which is the one case that needs bridging. This fires between adjacent
       * samples, where the geometry itself is discontinuous. */
      if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
      {
        if(have_last)
          dindex = _falloff_bridge_queue(dpoints, dindex, dpoints_capacity,
                                         last0, last1, p0, p1);

        if(dindex + 4 > dpoints_capacity) break;
        dpoints[dindex] = p0[0];
        dpoints[dindex + 1] = p0[1];
        dpoints[dindex + 2] = p1[0];
        dpoints[dindex + 3] = p1[1];
        dindex += 4;
        have_last = TRUE;

        last0[0] = p0[0];
        last0[1] = p0[1];
        last1[0] = p1[0];
        last1[1] = p1[1];
      }

      if(!used_next)
      {
        prev0[0] = p0[0];
        prev0[1] = p0[1];
        prev1[0] = p1[0];
        prev1[1] = p1[1];
        have_prev = TRUE;
      }
      else
      {
        have_prev = FALSE;
      }
    }
    __OMP_PARALLEL_FOR__(if(dindex > 4096))
    for(int n = 0; n < dindex; n += 4)
      _polygon_falloff_roi(buffer, dpoints + n, dpoints + n + 2, width, height);

    dt_pixelpipe_cache_free_align(dpoints);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n",
               mask_form->name,
               dt_get_wtime() - start2);
    }
  }

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);

  /* The raw bounding box already spans the border samples, so the feather falloff lies inside
   * it too; the margin covers the one-pixel neighbour writes of the falloff stamps. The
   * encircling case reported the full buffer above. */
  if(!polygon_encircles_roi)
    dt_masks_touched_set(touched, (int)floorf(xmin) - 2, (int)floorf(ymin) - 2, (int)ceilf(xmax) + 2,
                         (int)ceilf(ymax) + 2, width, height);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n",
             mask_form->name,
             dt_get_wtime() - start);

  return DT_MASKS_RASTER_OK;
}

static void _polygon_sanitize_config(dt_masks_type_t type)
{
  // nothing to do (yet?)
}

/**
 * @brief Assign a default name for a polygon form.
 */
static void _polygon_set_form_name(struct dt_masks_form_t *const mask_form, const size_t form_number)
{
  snprintf(mask_form->name, sizeof(mask_form->name), _("polygon #%d"), (int)form_number);
}

static void _polygon_set_hint_message(const dt_masks_form_gui_t *const mask_gui,
                                      const dt_masks_form_t *const mask_form,
                                      char *const restrict msgbuf, const size_t msgbuf_len)
{
  // Ordered like the hit test in dt_masks_find_closest_handle_common(): the innermost target
  // under the cursor is the one the next click will act on, so it is the one we describe.
  // Only gestures that cannot be discovered any other way -- what the wheel does is the user's
  // own mapping now (masks_gui.h), shown in the Drawn tab, so it is not repeated here.
  const guint node_count = mask_form->points ? g_list_length(mask_form->points) : 0;
  if(mask_gui->creation && node_count < 4)
    g_strlcat(msgbuf, _("<b>Add sharp node</b>: Ctrl+Click\n"
                        "<b>Remove last node</b>: Backspace, <b>Cancel</b>: Esc"), msgbuf_len);
  else if(mask_gui->creation)
    g_strlcat(msgbuf, _("<b>Add sharp node</b>: Ctrl+Click, <b>Close path</b>: Enter, or Click on the first node\n"
                        "<b>Remove last node</b>: Backspace, <b>Cancel</b>: Esc"), msgbuf_len);
  else if(mask_gui->source_selected)
    g_strlcat(msgbuf, _("<b>Move source</b>: Drag"), msgbuf_len);
  else if(mask_gui->handle_border_hovered >= 0)
    g_strlcat(msgbuf, _("<b>Node fading</b>: Drag"), msgbuf_len);
  else if(mask_gui->handle_hovered >= 0)
    g_strlcat(msgbuf, _("<b>Node curvature</b>: Drag"), msgbuf_len);
  // The node operations below need the node to be selected, which the first click does.
  else if(mask_gui->node_hovered >= 0 && mask_gui->node_selected)
    g_strlcat(msgbuf, _("<b>Move node</b>: Drag, <b>Switch smooth/sharp</b>: Ctrl+Click\n"
                        "<b>Delete node</b>: Right-click or Del"), msgbuf_len);
  else if(mask_gui->node_hovered >= 0)
    g_strlcat(msgbuf, _("<b>Move node</b>: Drag, <b>Delete node</b>: Right-click"), msgbuf_len);
  else if(mask_gui->seg_hovered >= 0 || mask_gui->seg_selected)
    g_strlcat(msgbuf, _("<b>Move segment</b>: Drag, <b>Add node</b>: Ctrl+Click"), msgbuf_len);
  else if(mask_gui->form_selected || mask_gui->border_selected)
    g_strlcat(msgbuf, _("<b>Move</b>: Drag"), msgbuf_len);
}

static void _polygon_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
   // unused arg, keep compiler from complaining
  dt_masks_duplicate_points(base, dest, sizeof(dt_masks_node_polygon_t));
}

static void _polygon_initial_source_pos(struct dt_develop_t *dev, const float iwd, const float iht, float *x, float *y)
{


  float offset[2] = { 0.1f, 0.1f };
  dt_dev_coordinates_raw_norm_to_raw_abs(dev, offset, 1);
  *x = offset[0];
  *y = offset[1];
}

static void _polygon_creation_closing_form_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  // This is a temp form on creation mode
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(mask_gui->dev);
  if(IS_NULL_PTR(mask_form)) return;

  _polygon_creation_closing_form(mask_form, mask_gui);
}

static void _polygon_switch_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(mask_gui)) return;
  dt_iop_module_t *module = mask_gui->dev->gui_module;
  if(IS_NULL_PTR(module)) return;
  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(mask_gui->dev, form_id);
  if(IS_NULL_PTR(selected_form)) return;

  mask_gui->node_selected = TRUE;
  mask_gui->node_selected_idx = mask_gui->node_hovered;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
  const int node_index = dt_masks_gui_selected_node_index(mask_gui);
  dt_masks_node_polygon_t *node
      = (dt_masks_node_polygon_t *)g_list_nth_data(selected_form->points, node_index);
  if(IS_NULL_PTR(gui_points) || IS_NULL_PTR(node)) return;
  dt_masks_toggle_bezier_node_type(module, selected_form, mask_gui, mask_gui->group_selected, gui_points,
                                   node_index, node->node, node->ctrl1, node->ctrl2, &node->state);
}

static void _polygon_reset_round_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(mask_gui)) return;
  dt_iop_module_t *module = mask_gui->dev->gui_module;
  if(IS_NULL_PTR(module)) return;
  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(mask_gui->dev, form_id);
  if(IS_NULL_PTR(selected_form)) return;

  mask_gui->node_selected = TRUE;
  mask_gui->node_selected_idx = mask_gui->node_hovered;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
  const int selected_handle = dt_masks_gui_selected_handle_index(mask_gui);
  const int node_index = MAX(mask_gui->node_hovered, selected_handle);
  dt_masks_node_polygon_t *node
      = (dt_masks_node_polygon_t *)g_list_nth_data(selected_form->points, node_index);
  if(IS_NULL_PTR(gui_points) || IS_NULL_PTR(node)) return;
  if(dt_masks_reset_bezier_ctrl_points(module, selected_form, mask_gui, mask_gui->group_selected, gui_points,
                                       node_index, &node->state))
    gui_points->clockwise = _polygon_is_clockwise(selected_form);
}

static void _polygon_add_node_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(mask_gui)) return;
  dt_masks_form_t *visible_forms = dt_masks_get_visible_form(mask_gui->dev);
  if(IS_NULL_PTR(visible_forms)) return;

  dt_iop_module_t *module = mask_gui->dev->gui_module;
  if(IS_NULL_PTR(module)) return;

  dt_masks_form_group_t *group_entry = dt_masks_form_get_selected_group(visible_forms, mask_gui);
  if(IS_NULL_PTR(group_entry)) return;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(mask_gui->dev, group_entry->formid);

  if(selected_form)
  {
    _add_node_to_segment(module, selected_form, group_entry->parentid, mask_gui, mask_gui->group_selected);
  }

  //dt_dev_add_history_item(mask_gui->dev, module, TRUE, TRUE);
}

static int _polygon_populate_context_menu(GtkWidget *menu, struct dt_masks_form_t *mask_form,
                                          struct dt_masks_form_gui_t *mask_gui,
                                          const float pzx, const float pzy)
{
  
  
  GtkWidget *menu_item = NULL;
  gchar *accel = g_strdup_printf(_("%s+Click"), gtk_accelerator_get_label(0, dt_accels_display_mods(DT_PRIMARY_MASK)));

  gboolean ret = FALSE;

  if(mask_gui->creation)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Close path"), menu,
                                                  _polygon_creation_closing_form_callback, mask_gui);
    gtk_widget_set_sensitive(menu_item, mask_form->points && !g_list_shorter_than(mask_form->points, 4));
    menu_item_set_fake_accel(menu_item, GDK_KEY_Return, 0);

    menu_item = ctx_gtk_menu_item_new_with_markup(_("Remove last point"), menu,
                                                  _masks_gui_delete_node_callback, mask_gui);
    menu_item_set_fake_accel(menu_item, GDK_KEY_BackSpace, 0);

    ret = TRUE;
  }

  else if(mask_gui->node_hovered >= 0)
  {
    dt_masks_form_gui_points_t *gui_points
        = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
    if(IS_NULL_PTR(gui_points)) goto end;
    dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_hovered);
    if(IS_NULL_PTR(node)) goto end;
    const gboolean is_corner = dt_masks_node_is_cusp(gui_points, mask_gui->node_hovered);

    {
      gchar *to_change_type = g_strdup_printf(_("Switch to %s node"), (is_corner) ? _("round") : _("cusp"));
      const dt_menu_icon_t icon = is_corner ? DT_MENU_ICON_CIRCLE : DT_MENU_ICON_SQUARE;
      menu_item = ctx_gtk_menu_item_new_with_icon_and_shortcut(to_change_type, accel, menu,
                                                               _polygon_switch_node_callback, mask_gui, icon);

      dt_free(to_change_type);
    }

    {
      menu_item = ctx_gtk_menu_item_new_with_markup(_("Reset round node"), menu,
                                                    _polygon_reset_round_node_callback, mask_gui);
      gtk_widget_set_sensitive(menu_item, !is_corner);
    }

    ret = TRUE;
  }

  if(mask_gui->seg_selected)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup_and_shortcut(_("Add a node here"), accel,
                                                               menu, _polygon_add_node_callback, mask_gui);
    ret = TRUE;
  }

  end:
  dt_free(accel);
  return ret;
}

// The function table for polygons.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_polygon = {
  .point_struct_size = sizeof(struct dt_masks_node_polygon_t),
  .sanitize_config = _polygon_sanitize_config,
  .set_form_name = _polygon_set_form_name,
  .set_hint_message = _polygon_set_hint_message,
  .duplicate_points = _polygon_duplicate_points,
  .initial_source_pos = _polygon_initial_source_pos,
  .get_distance = _polygon_get_distance,
  .get_points_border = _polygon_get_points_border,
  .get_mask = _polygon_get_mask,
  .get_mask_roi = _polygon_get_mask_roi,
  .get_area = _polygon_get_area,
  .get_source_area = _polygon_get_source_area,
  .get_gravity_center = _polygon_get_gravity_center,
  .get_interaction_value = _polygon_get_interaction_value,
  .set_interaction_value = _polygon_set_interaction_value,
  .update_hover = _find_closest_handle,
  .mouse_moved = _polygon_events_mouse_moved,
  .mouse_scrolled = _polygon_events_mouse_scrolled,
  .button_pressed = _polygon_events_button_pressed,
  .button_released = _polygon_events_button_released,
  .key_pressed = _polygon_events_key_pressed,
  .post_expose = _polygon_events_post_expose,
  .draw_shape = _polygon_draw_shape,
  .init_ctrl_points = _polygon_init_ctrl_points,
  .populate_context_menu = _polygon_populate_context_menu
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
