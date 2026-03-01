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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/undo.h"
#include "control/conf.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "gui/actions/menu.h"
#include <assert.h>

#define HARDNESS_MIN 0.0005f
#define HARDNESS_MAX 1.0f

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
static void _polygon_border_get_XY(const float p0_x, const float p0_y, const float p1_x, const float p1_y,
                                   const float p2_x, const float p2_y, const float p3_x, const float p3_y,
                                   const float t, const float radius,
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

  const double dx = -p0_x * a + p1_x * b + p2_x * c + p3_x * d;
  const double dy = -p0_y * a + p1_y * b + p2_y * c + p3_y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *border_x = NAN;
    *border_y = NAN;
    return;
  }
  const double l = 1.0 / sqrt(dx * dx + dy * dy);
  *border_x = (*center_x) + radius * dy * l;
  *border_y = (*center_y) - radius * dx * l;
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

  if(!mask_form || !mask_form->points) return;
  
  dt_masks_node_polygon_t **nodes = malloc((size_t)node_count * sizeof(*nodes));
  if(!nodes) return;
  const GList *form_points = mask_form->points;
  for(guint node_index = 0; node_index < node_count; node_index++)
  {
    nodes[node_index] = (dt_masks_node_polygon_t *)form_points->data;
    form_points = g_list_next(form_points);
  }

  for(guint node_index = 0; node_index < node_count; node_index++)
  {
    dt_masks_node_polygon_t *point3 = nodes[node_index];
    if(!point3) { free(nodes); return; }
    // if the point has not been set manually, we redefine it
    if(point3->state == DT_MASKS_POINT_STATE_NORMAL)
    {
      dt_masks_node_polygon_t *point1 = nodes[(node_index + node_count - 2) % node_count];
      dt_masks_node_polygon_t *point2 = nodes[(node_index + node_count - 1) % node_count];
      dt_masks_node_polygon_t *point4 = nodes[(node_index + 1) % node_count];
      dt_masks_node_polygon_t *point5 = nodes[(node_index + 2) % node_count];
      if(!point1 || !point2 || !point4 || !point5) { free(nodes); return; }

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
  free(nodes);
  return;
}

/**
 * @brief Determine polygon winding order.
 *
 * Returns TRUE when points are clockwise in normalized space.
 */
static gboolean _polygon_is_clockwise(dt_masks_form_t *mask_form)
{
  if(!mask_form || !mask_form->points) return 0;
  if(!g_list_shorter_than(mask_form->points, 3)) // if we have at least three points...
  {
    float sum = 0.0f;
    for(const GList *form_points = mask_form->points; form_points; form_points = g_list_next(form_points))
    {
      const GList *next = g_list_next_wraparound(form_points, mask_form->points); // next, wrapping around if on last elt
      dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data; // kth element of mask_form->points
      dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)next->data;
      if(!point1 || !point2) return 0;
      sum += (point2->node[0] - point1->node[0]) * (point2->node[1] + point1->node[1]);
    }
    return (sum < 0);
  }
  // return dummy answer
  return TRUE;
}

/**
 * @brief Fill gaps between two points with an integer Bresenham line.
 *
 * This avoids repeated floating-point division and rounding errors.
 */
static int _polygon_fill_gaps(int last_x, int last_y, int target_x, int target_y,
                              dt_masks_dynbuf_t *points)
{
  dt_masks_dynbuf_reset(points);
  dt_masks_dynbuf_add_2(points, target_x, target_y);

  const int delta_x = target_x - last_x;
  const int delta_y = target_y - last_y;
  const int abs_dx = abs(delta_x);
  const int abs_dy = abs(delta_y);

  // Only fill gaps if distance is > 1 in either axis
  if(abs_dx <= 1 && abs_dy <= 1) return 1;

  // Use Bresenham's line algorithm (integer-based)
  int err = abs_dx > abs_dy ? (abs_dx / 2) : (abs_dy / 2);
  int point_x = last_x;
  int point_y = last_y;
  const int step_x = delta_x > 0 ? 1 : -1;
  const int step_y = delta_y > 0 ? 1 : -1;

  if(abs_dx > abs_dy)
  {
    // Major axis is X
    while(point_x != target_x)
    {
      point_x += step_x;
      err -= abs_dy;
      if(err < 0)
      {
        point_y += step_y;
        err += abs_dx;
      }
      dt_masks_dynbuf_add_2(points, point_x, point_y);
    }
  }
  else
  {
    // Major axis is Y
    while(point_y != target_y)
    {
      point_y += step_y;
      err -= abs_dx;
      if(err < 0)
      {
        point_x += step_x;
        err += abs_dy;
      }
      dt_masks_dynbuf_add_2(points, point_x, point_y);
    }
  }
  return 1;
}

/**
 * @brief Fill gaps between border points with a circular arc.
 *
 * This is used when the border has gaps, especially near sharp nodes.
 */
static void _polygon_points_recurs_border_gaps(float *center_max, float *border_min,
                                               float *border_min2, float *border_max,
                                               dt_masks_dynbuf_t *draw_points,
                                               dt_masks_dynbuf_t *draw_border,
                                               gboolean clockwise)
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

  // and the max length of the circle arc
  int step_count = 0;
  if(angle_end > angle_start)
    step_count = (angle_end - angle_start) * fmaxf(radius_start, radius_end);
  else
    step_count = (angle_start - angle_end) * fmaxf(radius_start, radius_end);
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
  if(points_ptr)
  {
    for(int step_index = 1; step_index < step_count; step_index++)
    {
      *points_ptr++ = center_max[0];
      *points_ptr++ = center_max[1];
      if(border_ptr)
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
                                   int with_border, const int pixel_threshold)
{
  // we calculate points if needed
  if(isnan(polygon_min[0]))
  {
    _polygon_border_get_XY(segment_start[0], segment_start[1], segment_start[2], segment_start[3],
                           segment_end[2], segment_end[3], segment_end[0], segment_end[1], t_min,
                           segment_start[4]
                               + (segment_end[4] - segment_start[4]) * t_min * t_min * (3.0 - 2.0 * t_min),
                           polygon_min, polygon_min + 1, border_min, border_min + 1);
  }
  if(isnan(polygon_max[0]))
  {
    _polygon_border_get_XY(segment_start[0], segment_start[1], segment_start[2], segment_start[3],
                           segment_end[2], segment_end[3], segment_end[0], segment_end[1], t_max,
                           segment_start[4]
                               + (segment_end[4] - segment_start[4]) * t_max * t_max * (3.0 - 2.0 * t_max),
                           polygon_max, polygon_max + 1, border_max, border_max + 1);
  }

  // are the points near ?
  if((t_max - t_min < 0.0001)
       || (_is_within_pxl_threshold(polygon_min, polygon_max, pixel_threshold)
          && (!with_border || (_is_within_pxl_threshold(border_min, border_max, pixel_threshold)))))
  {
    dt_masks_dynbuf_add_2(draw_points, polygon_max[0], polygon_max[1]);
    result_polygon[0] = polygon_max[0];
    result_polygon[1] = polygon_max[1];

    if(with_border)
    {
      dt_masks_dynbuf_add_2(draw_border, border_max[0], border_max[1]);
      result_border[0] = border_max[0];
      result_border[1] = border_max[1];
    }
    return;
  }

  // we split in two part
  double t_mid = (t_min + t_max) / 2.0;
  float polygon_mid[2] = { NAN, NAN };
  float border_mid[2] = { NAN, NAN };
  float polygon_result_left[2] = { 0 };
  float border_result_left[2] = { 0 };
  _polygon_points_recurs(segment_start, segment_end, t_min, t_mid,
                         polygon_min, polygon_mid, border_min, border_mid,
                         polygon_result_left, border_result_left,
                         draw_points, draw_border, with_border, pixel_threshold);
  _polygon_points_recurs(segment_start, segment_end, t_mid, t_max,
                         polygon_result_left, polygon_max, border_result_left, border_max,
                         result_polygon, result_border,
                         draw_points, draw_border, with_border, pixel_threshold);
}

// Maximum number of self-intersection portions to track;
// helps limit detection complexity
#define POLYGON_MAX_SELF_INTERSECTIONS(nb_nodes) ((nb_nodes) * 4)

/**
 * @brief Find all self-intersection segments in a polygon border.
 */
static int _polygon_find_self_intersection(dt_masks_dynbuf_t *intersections,
                                           int node_count, float *border_points,
                                           int border_point_count,
                                           int *intersection_count_out)
{
  if(node_count == 0 || border_point_count == 0)
  {
    *intersection_count_out = 0;
    return 0;
  }

  int intersection_count = 0;

  // we search extreme points in x and y
  float xmin_f = FLT_MAX, xmax_f = -FLT_MAX;
  float ymin_f = FLT_MAX, ymax_f = -FLT_MAX;
  int extrema_index[4] = { -1 };

  for(int i = node_count * 3; i < border_point_count; i++)
  {
    if(isnan(border_points[i * 2]) || isnan(border_points[i * 2 + 1]))
    {
      // find nearest previous valid point; if at start, wrap to last valid point
      int prev = i - 1;
      while(prev >= node_count * 3
            && (isnan(border_points[prev * 2]) || isnan(border_points[prev * 2 + 1]))) prev--;
      if(prev < node_count * 3)
      {
        // wrap to last valid point in buffer
        prev = border_point_count - 1;
        while(prev >= node_count * 3
              && (isnan(border_points[prev * 2]) || isnan(border_points[prev * 2 + 1]))) prev--;
      }
      if(prev >= node_count * 3)
      {
        border_points[i * 2] = border_points[prev * 2];
        border_points[i * 2 + 1] = border_points[prev * 2 + 1];
      }
      else
      {
        continue; // skip if no valid point found
      }
    }
    if(xmin_f > border_points[i * 2])
    {
      xmin_f = border_points[i * 2];
      extrema_index[0] = i;
    }
    if(xmax_f < border_points[i * 2])
    {
      xmax_f = border_points[i * 2];
      extrema_index[1] = i;
    }
    if(ymin_f > border_points[i * 2 + 1])
    {
      ymin_f = border_points[i * 2 + 1];
      extrema_index[2] = i;
    }
    if(ymax_f < border_points[i * 2 + 1])
    {
      ymax_f = border_points[i * 2 + 1];
      extrema_index[3] = i;
    }
  }

  // Cast to int with explicit rounding for stable grid computation
  int xmin = (int)floorf(xmin_f) - 1;
  int xmax = (int)ceilf(xmax_f) + 1;
  int ymin = (int)floorf(ymin_f) - 1;
  int ymax = (int)ceilf(ymax_f) + 1;
  const int grid_height = ymax - ymin;
  const int grid_width = xmax - xmin;

  // we allocate the buffer
  const size_t grid_size = (size_t)grid_height * grid_width;
  if(grid_size < 10 || grid_height < 0 || grid_width < 0)
  {
    *intersection_count_out = 0;
    return 0;
  }

  int *intersection_grid = dt_pixelpipe_cache_alloc_align_cache(sizeof(int) * grid_size, 0);
  if(intersection_grid == NULL) return 1;
  memset(intersection_grid, 0, sizeof(int) * grid_size);

  dt_masks_dynbuf_t *gap_points = dt_masks_dynbuf_init(100000, "polygon extra");
  if(gap_points == NULL)
  {
    dt_pixelpipe_cache_free_align(intersection_grid);
    return 1;
  }

  // we'll iterate through all border points, but we can't start at point[0]
  // because it may be in a self-intersected section
  // so we choose a point where we are sure there's no intersection:
  // one from border shape extrema (here x_max)
  // start from the point immediately before the x_max extremum, with safe wrap-around
  int start_idx = extrema_index[1] - 1;
  if(start_idx < node_count * 3) start_idx = border_point_count - 1;
  int last_x = border_points[start_idx * 2];
  int last_y = border_points[start_idx * 2 + 1];

  for(int ii = node_count * 3; ii < border_point_count; ii++)
  {
    // we want to loop from one border extremity
    int i = ii - node_count * 3 + extrema_index[1];
    if(i >= border_point_count) i = i - border_point_count + node_count * 3;

    if(intersection_count >= POLYGON_MAX_SELF_INTERSECTIONS(node_count)) break;

    // we want to be sure everything is continuous
    _polygon_fill_gaps(last_x, last_y, border_points[i * 2], border_points[i * 2 + 1], gap_points);

    // extra represent all the points between the last one and the current one
    // for all the points in extra, we'll check for self-intersection
    // and "register" them in binter
    for(int j = dt_masks_dynbuf_position(gap_points) / 2 - 1; j >= 0; j--)
    {
      const int grid_x = (dt_masks_dynbuf_buffer(gap_points))[j * 2];
      const int grid_y = (dt_masks_dynbuf_buffer(gap_points))[j * 2 + 1];

      // we check also 2 points around to be sure catching intersection
      int cell_values[3] = { 0 };
      const int idx = (grid_y - ymin) * grid_width + (grid_x - xmin);
      // ensure idx is within [0, ss)
      if(idx < 0 || (size_t)idx >= grid_size)
      {
        dt_masks_dynbuf_free(gap_points);
        dt_pixelpipe_cache_free_align(intersection_grid);
        return 1;
      }
      cell_values[0] = intersection_grid[idx];
      if(grid_x > xmin) cell_values[1] = intersection_grid[idx - 1];
      if(grid_y > ymin) cell_values[2] = intersection_grid[idx - grid_width];

      for(int k = 0; k < 3; k++)
      {
        if(cell_values[k] > 0)
        {
          // there's already a border point "registered" at this coordinate.
          // so we've potentially found a self-intersection portion between v[k] and i
          if((grid_x == last_x && grid_y == last_y) || cell_values[k] == i - 1)
          {
            // we haven't move from last point.
            // this is not a real self-interesection, so we just update binter
            intersection_grid[idx] = i;
          }
          else if((i > cell_values[k]
                   && ((extrema_index[0] < cell_values[k] || extrema_index[0] > i)
                       && (extrema_index[1] < cell_values[k] || extrema_index[1] > i)
                       && (extrema_index[2] < cell_values[k] || extrema_index[2] > i)
                       && (extrema_index[3] < cell_values[k] || extrema_index[3] > i)))
                  || (i < cell_values[k] && extrema_index[0] < cell_values[k] && extrema_index[0] > i
                      && extrema_index[1] < cell_values[k] && extrema_index[1] > i
                      && extrema_index[2] < cell_values[k] && extrema_index[2] > i
                      && extrema_index[3] < cell_values[k] && extrema_index[3] > i))
          {
            // we have found a self-intersection portion, between v[k] and i
            // and we are sure that this portion doesn't include one of the shape extrema
            if(intersection_count > 0)
            {
              const int inter_last0 = (int)dt_masks_dynbuf_get(intersections, -2);
              const int inter_last1 = (int)dt_masks_dynbuf_get(intersections, -1);
              if((cell_values[k] - i) * (inter_last0 - inter_last1) > 0
                 && inter_last0 >= cell_values[k] && inter_last1 <= i)
              {
                // we find an self-intersection portion which include the last one
                // we just update it
                dt_masks_dynbuf_set(intersections, -2, cell_values[k]);
                dt_masks_dynbuf_set(intersections, -1, i);
              }
              else
              {
                // we find a new self-intersection portion
                dt_masks_dynbuf_add_2(intersections, cell_values[k], i);
                intersection_count++;
              }
            }
            else
            {
              // we find a new self-intersection portion
              dt_masks_dynbuf_add_2(intersections, cell_values[k], i);
              intersection_count++;
            }
          }
        }
        else
        {
          // there wasn't anything "registered" at this place in binter
          // we do it now
          intersection_grid[idx] = i;
        }
      }
      last_x = grid_x;
      last_y = grid_y;
    }
  }

  dt_masks_dynbuf_free(gap_points);
  dt_pixelpipe_cache_free_align(intersection_grid);

  // and we return the number of self-intersection found
  *intersection_count_out = intersection_count;
  return 0;
}

/**
 * @brief Build point and border buffers for a polygon mask.
 *
 * This handles gaps, self-intersections, and optional iop distortion transforms.
 */
static int _polygon_get_pts_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                   const double iop_order, const int transform_direction,
                                   dt_dev_pixelpipe_t *pipe, float **point_buffer, int *point_count,
                                   float **border_buffer, int *border_count, gboolean source)
{
  if(!mask_form || !mask_form->points) return 0;

  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float input_width = pipe->iwidth;
  const float input_height = pipe->iheight;
  const int pixel_threshold = (pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                               || pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL) ? 3 : 1;
  const guint node_count = g_list_length(mask_form->points);

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *intersections = NULL;

  *point_buffer = NULL;
  *point_count = 0;
  if(border_buffer) *border_buffer = NULL;
  if(border_buffer) *border_count = 0;

  dpoints = dt_masks_dynbuf_init(1000000, "polygon dpoints");
  if(dpoints == NULL) return 1;

  if(border_buffer)
  {
    dborder = dt_masks_dynbuf_init(1000000, "polygon dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 1;
    }
  }

  intersections = dt_masks_dynbuf_init(10 * MAX(node_count, 1), "polygon intersections");
  if(intersections == NULL)
  {
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 1;
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  //get the first node's position to use for source offset
  if(source && node_count > 0 && transform_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    dt_masks_node_polygon_t *polygon = (dt_masks_node_polygon_t *)mask_form->points->data;
    if(!polygon) return 0;
    dx = (polygon->node[0] - mask_form->source[0]) * input_width;
    dy = (polygon->node[1] - mask_form->source[1]) * input_height;
  }
  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    const dt_masks_node_polygon_t *const node = (dt_masks_node_polygon_t *)point_node->data;
    float *const buf = dt_masks_dynbuf_reserve_n(dpoints, 6);
    if(buf)
    {
      buf[0] = node->ctrl1[0] * input_width - dx;
      buf[1] = node->ctrl1[1] * input_height - dy;
      buf[2] = node->node[0] * input_width - dx;
      buf[3] = node->node[1] * input_height - dy;
      buf[4] = node->ctrl2[0] * input_width - dx;
      buf[5] = node->ctrl2[1] * input_height - dy;
    }
  }
  // for the border, we store value too
  if(dborder)
  {
    dt_masks_dynbuf_add_zeros(dborder, 6 * node_count);  // need six zeros for each border point
  }

  float *border_init = dt_pixelpipe_cache_alloc_align_float_cache((size_t)6 * node_count, 0);
  if(border_init == NULL)
  {
    dt_masks_dynbuf_free(intersections);
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 1;
  }
  int cw = _polygon_is_clockwise(mask_form);
  if(cw == 0) cw = -1;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points init took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we render all segments
  const GList *form_points = mask_form->points;
  for(int node_index = 0; node_index < node_count; node_index++)
  {
    const int pb = dborder ? dt_masks_dynbuf_position(dborder) : 0;
    border_init[node_index * 6 + 2] = -pb;
    const GList *pt2 = g_list_next_wraparound(form_points, mask_form->points); // next, wrapping around if on last element
    const GList *pt3 = g_list_next_wraparound(pt2, mask_form->points);
    dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data; // kth element of mask_form->points
    dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)pt2->data;
    dt_masks_node_polygon_t *point3 = (dt_masks_node_polygon_t *)pt3->data;
    float p1[5] = { point1->node[0] * input_width - dx, point1->node[1] * input_height - dy,
                    point1->ctrl2[0] * input_width - dx, point1->ctrl2[1] * input_height - dy,
                    cw * point1->border[1] * MIN(input_width, input_height) };
    float p2[5] = { point2->node[0] * input_width - dx, point2->node[1] * input_height - dy,
                    point2->ctrl1[0] * input_width - dx, point2->ctrl1[1] * input_height - dy,
                    cw * point2->border[0] * MIN(input_width, input_height) };
    float p3[5] = { point2->node[0] * input_width - dx, point2->node[1] * input_height - dy,
                    point2->ctrl2[0] * input_width - dx, point2->ctrl2[1] * input_height - dy,
                    cw * point2->border[1] * MIN(input_width, input_height) };
    float p4[5] = { point3->node[0] * input_width - dx, point3->node[1] * input_height - dy,
                    point3->ctrl1[0] * input_width - dx, point3->ctrl1[1] * input_height - dy,
                    cw * point3->border[0] * MIN(input_width, input_height) };

    // advance form_points for next iteration so that it tracks the kth element of mask_form->points
    form_points = g_list_next(form_points);

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2] = { 0 }, rb[2] = { 0 };
    float bmin[2] = { NAN, NAN };
    float bmax[2] = { NAN, NAN };
    float cmin[2] = { NAN, NAN };
    float cmax[2] = { NAN, NAN };

    _polygon_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, dpoints, dborder,
                           border_buffer && (node_count >= 3), pixel_threshold);

    // we check gaps in the border (sharp edges)
    if(dborder)
    {
      const float lastb0 = dt_masks_dynbuf_get(dborder, -2);
      const float lastb1 = dt_masks_dynbuf_get(dborder, -1);
      if(fabs(lastb0 - rb[0]) > 1.0f || fabs(lastb1 - rb[1]) > 1.0f)
      {
        bmin[0] = lastb0;
        bmin[1] = lastb1;
      }
    }

    dt_masks_dynbuf_add_2(dpoints, rc[0], rc[1]);

    border_init[node_index * 6 + 4] = dborder ? -dt_masks_dynbuf_position(dborder) : 0;

    if(dborder)
    {
      if(isnan(rb[0]))
      {
        float lastb0 = dt_masks_dynbuf_get(dborder, -2);
        float lastb1 = dt_masks_dynbuf_get(dborder, -1);
        if(isnan(lastb0))
        {
          lastb0 = dt_masks_dynbuf_get(dborder, -4);
          lastb1 = dt_masks_dynbuf_get(dborder, -3);
          dt_masks_dynbuf_set(dborder, -2, lastb0);
          dt_masks_dynbuf_set(dborder, -1, lastb1);
        }
        rb[0] = lastb0;
        rb[1] = lastb1;
      }
      dt_masks_dynbuf_add_2(dborder, rb[0], rb[1]);

      (dt_masks_dynbuf_buffer(dborder))[node_index * 6] = border_init[node_index * 6]
          = (dt_masks_dynbuf_buffer(dborder))[pb];
      (dt_masks_dynbuf_buffer(dborder))[node_index * 6 + 1] = border_init[node_index * 6 + 1]
          = (dt_masks_dynbuf_buffer(dborder))[pb + 1];
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && node_count >= 3)
    {
      // we get the next point (start of the next segment)
      // t=0.00001f to workaround rounding effects with full optimization that result in bmax[0] NOT being set to
      // NAN when t=0 and the two points in p3 are identical (as is the case on a control node set to sharp corner)
      _polygon_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.00001f, p3[4], cmin, cmin + 1,
                          bmax, bmax + 1);
      if(isnan(bmax[0]))
      {
        _polygon_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.00001f, p3[4], cmin,
                            cmin + 1, bmax, bmax + 1);
      }
      if(bmax[0] - rb[0] > 1 || bmax[0] - rb[0] < -1 || bmax[1] - rb[1] > 1 || bmax[1] - rb[1] < -1)
      {
        float bmin2[2] = { dt_masks_dynbuf_get(dborder, -22), dt_masks_dynbuf_get(dborder, -21) };
        _polygon_points_recurs_border_gaps(rc, rb, bmin2, bmax, dpoints, dborder,
                                           _polygon_is_clockwise(mask_form));
      }
    }
  }

  *point_count = dt_masks_dynbuf_position(dpoints) / 2;
  *point_buffer = dt_masks_dynbuf_harvest(dpoints);
  dt_masks_dynbuf_free(dpoints);

  if(dborder)
  {
    *border_count = dt_masks_dynbuf_position(dborder) / 2;
    *border_buffer = dt_masks_dynbuf_harvest(dborder);
    dt_masks_dynbuf_free(dborder);
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points point recurs %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we don't want the border to self-intersect
  int inter_count = 0;
  if(border_buffer)
  {
    if(_polygon_find_self_intersection(intersections, node_count, *border_buffer, *border_count,
                                       &inter_count) != 0)
    {
      dt_masks_dynbuf_free(intersections);
      dt_pixelpipe_cache_free_align(*point_buffer);
      dt_pixelpipe_cache_free_align(*border_buffer);
      return 1;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points self-intersect took %0.04f sec\n", mask_form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }
  }

  // and we transform them with all distorted modules
  if(source && transform_direction == DT_DEV_TRANSFORM_DIR_ALL)
  {
    // we transform with all distortion that happen *before* the module
    // so we have now the TARGET points in module input reference
    if(dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                     *point_buffer, *point_count))
    {
      // now we move all the points by the shift
      // so we have now the SOURCE points in module input reference
      float pts[2] = { mask_form->source[0] * input_width, mask_form->source[1] * input_height };
      if(!dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1))
        goto fail;

      dx = pts[0] - (*point_buffer)[2];
      dy = pts[1] - (*point_buffer)[3];

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(point_count, point_buffer, dx, dy) \
    schedule(static) if(*point_count > 100) aligned(point_buffer:64)
#endif
      for(int i = 0; i < *point_count; i++)
      {
        (*point_buffer)[i * 2]     += dx;
        (*point_buffer)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                        *point_buffer, *point_count))
        goto fail;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points end took %0.04f sec\n",
               mask_form->name, dt_get_wtime() - start2);

    dt_masks_dynbuf_free(intersections);
    dt_pixelpipe_cache_free_align(border_init);
    return 0;
  }
  else if(dt_dev_distort_transform_plus(develop, pipe, iop_order, transform_direction,
                                        *point_buffer, *point_count))
  {
    if(!border_buffer
       || dt_dev_distort_transform_plus(develop, pipe, iop_order, transform_direction,
                                        *border_buffer, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points transform took %0.04f sec\n", mask_form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }

      if(border_buffer)
      {
        // we don't want to copy the falloff points
        for(int node_index = 0; node_index < node_count; node_index++)
          for(int i = 2; i < 6; i++)
            (*border_buffer)[node_index * 6 + i] = border_init[node_index * 6 + i];

        // now we want to write the skipping zones
        for(int i = 0; i < inter_count; i++)
        {
          const int v = (dt_masks_dynbuf_buffer(intersections))[i * 2];
          const int w = (dt_masks_dynbuf_buffer(intersections))[ i * 2 + 1];
          if(v <= w)
          {
            (*border_buffer)[v * 2] = NAN;
            (*border_buffer)[v * 2 + 1] = w;
          }
          else
          {
            if(w > node_count * 3)
            {
              if(isnan((*border_buffer)[node_count * 6]) && isnan((*border_buffer)[node_count * 6 + 1]))
                (*border_buffer)[node_count * 6 + 1] = w;
              else if(isnan((*border_buffer)[node_count * 6]))
                (*border_buffer)[node_count * 6 + 1]
                    = MAX((*border_buffer)[node_count * 6 + 1], w);
              else
                (*border_buffer)[node_count * 6 + 1] = w;
              (*border_buffer)[node_count * 6] = NAN;
            }
            (*border_buffer)[v * 2] = NAN;
            (*border_buffer)[v * 2 + 1] = NAN;
          }
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points end took %0.04f sec\n", mask_form->name,
                 dt_get_wtime() - start2);

      dt_masks_dynbuf_free(intersections);
      dt_pixelpipe_cache_free_align(border_init);
      return 0;
    }
  }

  // if we failed, then free all and return
fail:
  dt_masks_dynbuf_free(intersections);
  dt_pixelpipe_cache_free_align(border_init);
  dt_pixelpipe_cache_free_align(*point_buffer);
  *point_buffer = NULL;
  *point_count = 0;
  if(border_buffer)
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
  if(!mask_form || !mask_form->points) return 0;
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
  if(!mask_form || !mask_form->points || !mask_gui) return;
  const guint node_count = g_list_length(mask_form->points);
  const int selected_segment = dt_masks_gui_selected_segment_index(mask_gui);
  if(selected_segment < 0 || selected_segment >= (int)node_count) return;

  // we add a new node to the polygon
  dt_masks_node_polygon_t *new_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
  if(!new_node) return;

  // set coordinates
  new_node->node[0] = mask_gui->pos[0];
  new_node->node[1] = mask_gui->pos[1];
  // we backtransform the point to get it in input space
  dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, new_node->node, 1);
  new_node->ctrl1[0] = new_node->ctrl1[1] = new_node->ctrl2[0] = new_node->ctrl2[1] = -1.0;
  new_node->state = DT_MASKS_POINT_STATE_NORMAL;

  // set other attributes of the new node. we interpolate the starting and the end node of that
  // segment
  const float t = _polygon_get_position_in_segment(new_node->node[0], new_node->node[1],
                                                   mask_form, selected_segment);
  // start and end node of the segment
  GList *pt = g_list_nth(mask_form->points, selected_segment);
  if(!pt || !pt->data)
  {
    free(new_node);
    return;
  }
  dt_masks_node_polygon_t *point0 = (dt_masks_node_polygon_t *)pt->data;
  const GList *const next_pt = g_list_next_wraparound(pt, mask_form->points);
  if(!next_pt || !next_pt->data)
  {
    free(new_node);
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
  mask_gui->seg_hovered = -1;
  mask_gui->seg_selected = FALSE;
}

// TODO: Should be in masks.c
static void _change_node_type(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                              dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(!mask_form || !mask_form->points) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
  if(!gui_points) return;
  const int selected_node = dt_masks_gui_selected_node_index(mask_gui);
  if(selected_node < 0) return;
  dt_masks_node_polygon_t *node
      = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, selected_node);
  if(!node) return;
  const gboolean is_corner = dt_masks_node_is_cusp(gui_points, selected_node);

  if(is_corner)
  {
    // Switch to round
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _polygon_init_ctrl_points(mask_form);
  }
  else
  {
    // Switch to corner
    node->ctrl1[0] = node->ctrl2[0] = node->node[0];
    node->ctrl1[1] = node->ctrl2[1] = node->node[1];
    node->state = DT_MASKS_POINT_STATE_USER;

  }
  // we recreate the form points
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
}

static int _polygon_get_points_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                      float **point_buffer, int *point_count,
                                      float **border_buffer, int *border_count,
                                      int source, const dt_iop_module_t *module)
{
  if(source && !module) return 1;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  return _polygon_get_pts_border(develop, mask_form, ioporder, DT_DEV_TRANSFORM_DIR_ALL,
                                 develop->virtual_pipe, point_buffer, point_count,
                                 border_buffer, border_count, source);
}

static void _polygon_get_sizes(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                               dt_masks_form_gui_t *mask_gui, int form_index,
                               float *mask_size, float *border_size)
{
  const dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return;

  const int node_count = g_list_length(mask_form->points);
  float p1[2] = { FLT_MAX, FLT_MAX };
  float p2[2] = { FLT_MIN, FLT_MIN };

  float fp1[2] = { FLT_MAX, FLT_MAX };
  float fp2[2] = { FLT_MIN, FLT_MIN };

  for(int i = node_count * 3; i < gui_points->points_count; i++)
  {
    // line
    const float x = gui_points->points[i * 2];
    const float y = gui_points->points[i * 2 + 1];

    p1[0] = fminf(p1[0], x);
    p2[0] = fmaxf(p2[0], x);
    p1[1] = fminf(p1[1], y);
    p2[1] = fmaxf(p2[1], y);

    if(border_size)
    {
      // border
      const float fx = gui_points->border[i * 2];
      const float fy = gui_points->border[i * 2 + 1];

      // ??? looks like when x border is nan then y is a point index
      // see draw border in _polygon_events_post_expose.
      if(!isnan(fx))
      {
        fp1[0] = fminf(fp1[0], fx);
        fp2[0] = fmaxf(fp2[0], fx);
        fp1[1] = fminf(fp1[1], fy);
        fp2[1] = fmaxf(fp2[1], fy);
      }
    }
  }

  float mask_span[2] = { p2[0] - p1[0], p2[1] - p1[1] };
  dt_dev_coordinates_preview_abs_to_image_norm(darktable.develop, mask_span, 1);
  *mask_size = fmaxf(mask_span[0], mask_span[1]);

  if(border_size)
  {
    float border_span[2] = { fp2[0] - fp1[0], fp2[1] - fp1[1] };
    dt_dev_coordinates_preview_abs_to_image_norm(darktable.develop, border_span, 1);
    *border_size = fmaxf(border_span[0], border_span[1]);
  }
}

static gboolean _polygon_form_gravity_center(const dt_masks_form_t *mask_form, float *center_x,
                                             float *center_y, float *surface);

static float _polygon_get_size_from_nodes(const dt_masks_form_t *mask_form)
{
  if(!mask_form || !mask_form->points) return 0.0f;
  float p1[2] = { FLT_MAX, FLT_MAX };
  float p2[2] = { -FLT_MAX, -FLT_MAX };

  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    const dt_masks_node_polygon_t *node = (const dt_masks_node_polygon_t *)point_node->data;
    if(!node) continue;
    p1[0] = fminf(p1[0], node->node[0]);
    p2[0] = fmaxf(p2[0], node->node[0]);
    p1[1] = fminf(p1[1], node->node[1]);
    p2[1] = fmaxf(p2[1], node->node[1]);
  }

  if(p1[0] == FLT_MAX) return 0.0f;
  return fmaxf(p2[0] - p1[0], p2[1] - p1[1]);
}

static float _polygon_get_interaction_value(const dt_masks_form_t *mask_form,
                                            dt_masks_interaction_t interaction)
{
  if(!mask_form || !mask_form->points) return NAN;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
    {
      const float size = _polygon_get_size_from_nodes(mask_form);
      if(size <= 0.0f) return NAN;
      return size;
    }
    case DT_MASKS_INTERACTION_HARDNESS:
    {
      const dt_masks_node_polygon_t *node = (const dt_masks_node_polygon_t *)mask_form->points->data;
      if(!node) return NAN;
      return node->border[0];
    }
    default:
      return NAN;
  }
}

static gboolean _polygon_get_gravity_center(const dt_masks_form_t *mask_form,
                                            float center[2], float *area)
{
  if(!mask_form || !mask_form->points || !center) return FALSE;

  const int points_count = g_list_length(mask_form->points);
  if(points_count <= 0) return FALSE;

  float *point_buffer = dt_alloc_align_float((size_t)points_count * 2);
  if(!point_buffer) return FALSE;

  int i = 0;
  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    const dt_masks_node_polygon_t *node = (const dt_masks_node_polygon_t *)point_node->data;
    if(!node) continue;
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
static int _change_hardness(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                            struct dt_iop_module_t *module, int form_index, const float amount,
                            const dt_masks_increment_t increment, int flow);

static float _polygon_set_interaction_value(dt_masks_form_t *mask_form,
                                            dt_masks_interaction_t interaction, float value,
                                            dt_masks_increment_t increment, int flow,
                                            dt_masks_form_gui_t *mask_gui, struct dt_iop_module_t *module)
{
  if(!mask_form) return NAN;
  const int index = 0;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      if(!_change_size(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
      return _polygon_get_interaction_value(mask_form, interaction);
    case DT_MASKS_INTERACTION_HARDNESS:
      if(!_change_hardness(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
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
                                  int *near, int *inside_source, float *dist)
{
  if(!mask_gui) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return;

  float min_dist_pixel = FLT_MAX;

  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  const float radius2 = radius * radius;
  const float pt[2] = { point_x, point_y };
  
  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(pt, 1, gui_points->source, node_count * 3,
                                  gui_points->source_count) >= 0)
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

  // we check if we are near a segment
  if(gui_points->points_count > 2 + node_count * 3)
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
            *near = node_count - 1;
          else
            *near = current_seg - 1;
        }
      }
    }
  }

  *dist = min_dist_pixel;

  // we check if it's not inside borders, meaning we are not inside at all
  if(dt_masks_point_in_form_exact(pt, 1, gui_points->border, node_count * 3,
                                  gui_points->border_count) < 0)
    return;
  
  // we are at least inside the border
  *inside = 1;

  // and we check if it's not inside form, meaning we are inside border only
  *inside_border = (dt_masks_point_in_form_exact(pt, 1, gui_points->points,
                                                 node_count * 3, gui_points->points_count) < 0);
}

/**
 * @brief Polygon-specific border handle lookup.
 *
 * Polygon borders are stored directly in gui_points->border at node indices.
 */
static gboolean _polygon_border_handle_cb(const dt_masks_form_gui_points_t *gui_points, int node_count,
                                          int node_index, float *handle_x, float *handle_y, void *user_data)
{
  if(!gui_points || node_index < 0 || node_index >= node_count) return FALSE;
  *handle_x = gui_points->border[node_index * 6];
  *handle_y = gui_points->border[node_index * 6 + 1];
  return !(isnan(*handle_x) || isnan(*handle_y));
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
                                 int *inside_border, int *near, int *inside_source, float *dist, void *user_data)
{
  
  _polygon_get_distance(pointer_x, pointer_y, cursor_radius, mask_gui, form_index, node_count,
                        inside, inside_border, near, inside_source, dist);
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
  if(!point_buffer || point_count < 3) return;

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
    if(center_x) *center_x = centroid_x * inv_divisor;
    if(center_y) *center_y = centroid_y * inv_divisor;
  }
  if(area) *area = signed_area;
}

/**
 * @brief Compute polygon centroid from the form nodes (normalized space).
 */
static gboolean _polygon_form_gravity_center(const dt_masks_form_t *mask_form,
                                             float *center_x, float *center_y, float *area)
{
  if(!mask_form || !mask_form->points || g_list_shorter_than(mask_form->points, 3)) return FALSE;

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

  if(area) *area = signed_area;
  if(fabsf(signed_area) <= 1e-8f) return FALSE;

  const float inv_divisor = 1.0f / (3.0f * signed_area);
  if(center_x) *center_x = centroid_x * inv_divisor;
  if(center_y) *center_y = centroid_y * inv_divisor;
  return TRUE;
}

/**
 * @brief Initialize hardness from config and emit the toast with a size-normalized percentage.
 */
static int _init_hardness(dt_masks_form_t *mask_form, const float amount,
                          const dt_masks_increment_t increment, const int flow,
                          const float mask_size, const float border_size)
{
  const float mask_hardness = dt_masks_get_set_conf_value(mask_form, "hardness", amount,
                                                          HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  dt_toast_log(_("Hardness: %3.2f%%"), (border_size * mask_hardness) / mask_size * 100.0f);
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
  if(!mask_form || !mask_form->points) return 0;

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

  for(GList *node_iter = mask_form->points; node_iter; node_iter = g_list_next(node_iter))
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

  float mask_size = 0.0f;
  _polygon_get_sizes(module, mask_form, mask_gui, form_index, &mask_size, NULL);

  dt_toast_log(_("Size: %3.2f%%"), mask_size * 100.0f);

  // Rebuild the cached GUI geometry.
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  return 1;
}

/**
 * @brief Change polygon hardness, optionally restricted to the selected node.
 */
static int _change_hardness(dt_masks_form_t *mask_form, int parent_id, dt_masks_form_gui_t *mask_gui,
                            struct dt_iop_module_t *module, int form_index, const float amount,
                            const dt_masks_increment_t increment, int flow)
{
  const int node_selected = dt_masks_gui_selected_node_index(mask_gui);
  int node_index = 0;
  const float scale_amount = powf(amount, (float)flow);
  const float offset_amount = amount * (float)flow;

  for(GList *node_iter = mask_form->points; node_iter; node_iter = g_list_next(node_iter))
  {
    if(node_selected == -1 || node_selected == node_index)
    {
      dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)node_iter->data;
      if(!node) continue;

      node->border[0] = CLAMPF(dt_masks_apply_increment_precomputed(node->border[0], amount, scale_amount,
                                                                     offset_amount, increment),
                               HARDNESS_MIN, HARDNESS_MAX);
      node->border[1] = CLAMPF(dt_masks_apply_increment_precomputed(node->border[1], amount, scale_amount,
                                                                     offset_amount, increment),
                               HARDNESS_MIN, HARDNESS_MAX);
      if(node_selected >= 0) break;
    }
    node_index++;
  }

  float mask_size = 1.0f;
  float border_size = 0.0f;
  _polygon_get_sizes(module, mask_form, mask_gui, form_index, &mask_size, &border_size);

  _init_hardness(mask_form, amount, increment, flow, mask_size, border_size);

  // Rebuild the cached GUI geometry.
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);

  return 1;
}

/**
 * @brief Handle mouse wheel updates for polygon size/hardness/opacity.
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

  if(mask_gui->edit_mode == DT_MASKS_EDIT_FULL && dt_masks_is_anything_selected(mask_gui))
  {
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_change_opacity(mask_form, parent_id, up, flow);
    if(dt_modifier_is(state, GDK_SHIFT_MASK) || mask_gui->node_selected)
      return _change_hardness(mask_form, parent_id, mask_gui, module, form_index, up ? 1.02f : 0.98f,
                              DT_MASKS_INCREMENT_SCALE, flow);
    else
      return _change_size(mask_form, parent_id, mask_gui, module, form_index, up ? 1.02f : 0.98f,
                          DT_MASKS_INCREMENT_SCALE, flow);
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
  free(last_node);
  last_node = NULL;

  mask_gui->node_dragging = -1;
  _polygon_init_ctrl_points(mask_form);

  // we save the form and quit creation mode
  dt_masks_gui_form_save_creation(darktable.develop, creation_module, mask_form, mask_gui);

  return 1;
}

/**
 * @brief Reset Bezier control points for the hovered/selected node.
 */
static gboolean _reset_ctrl_points(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                                   dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(!mask_form || !mask_form->points) return FALSE;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return FALSE;

  const int selected_handle = dt_masks_gui_selected_handle_index(mask_gui);
  const int node_index = MAX(mask_gui->node_hovered, selected_handle);
  dt_masks_node_polygon_t *node
      = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, node_index);
  if(!node) return FALSE;

  if(node->state != DT_MASKS_POINT_STATE_NORMAL && !dt_masks_node_is_cusp(gui_points, node_index))
  {
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _polygon_init_ctrl_points(mask_form);
    // we recreate the form points
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    gui_points->clockwise = _polygon_is_clockwise(mask_form);
  }
  return TRUE;
}

static int _polygon_events_button_pressed(struct dt_iop_module_t *module, double x, double y,
                                       double pressure, int which, int type, uint32_t state,
                                       dt_masks_form_t *mask_form, int parent_id,
                                       dt_masks_form_gui_t *mask_gui, int form_index)
{
  
  
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!mask_gui || !mask_form) return 0;

  _find_closest_handle(mask_form, mask_gui, form_index);

  if(which == 1)
  {
    if(mask_gui->creation)
    {
      if(mask_gui->creation_closing_form)
        return _polygon_creation_closing_form(mask_form, mask_gui);

      if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK))
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
        float masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/polygon/hardness"), HARDNESS_MAX);

        int node_count = g_list_length(mask_form->points);
        // change the values
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));

        polygon_node->node[0] = mask_gui->pos[0];
        polygon_node->node[1] = mask_gui->pos[1];
        // we backtransform the point to get it in input space
        dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, polygon_node->node, 1);

        polygon_node->ctrl1[0] = polygon_node->ctrl1[1] = polygon_node->ctrl2[0] = polygon_node->ctrl2[1] = -1.0;
        polygon_node->border[0] = polygon_node->border[1] = MAX(HARDNESS_MIN, masks_border);
        polygon_node->state = DT_MASKS_POINT_STATE_NORMAL;
  
        if(node_count == 0)
        {
          // create the first node
          dt_masks_node_polygon_t *polygon_first_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
          polygon_first_node->node[0] = polygon_node->node[0];
          polygon_first_node->node[1] = polygon_node->node[1];
          polygon_first_node->ctrl1[0] = polygon_first_node->ctrl1[1] = polygon_first_node->ctrl2[0] = polygon_first_node->ctrl2[1] = -1.0;
          polygon_first_node->border[0] = polygon_first_node->border[1] = MAX(HARDNESS_MIN, masks_border);
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
        if(dt_modifier_is(state, GDK_CONTROL_MASK))
        {
          dt_masks_node_polygon_t *polygon_last_node = g_list_nth_data(mask_form->points, node_count - 1);
          polygon_last_node->ctrl1[0] = polygon_last_node->ctrl2[0] = polygon_last_node->node[0];
          polygon_last_node->ctrl1[1] = polygon_last_node->ctrl2[1] = polygon_last_node->node[1];
          polygon_last_node->state = DT_MASKS_POINT_STATE_USER;
        }

        mask_gui->node_hovered = node_count;
        mask_gui->node_selected = TRUE;
        mask_gui->node_dragging = node_count;
        _polygon_init_ctrl_points(mask_form);
      }

      // we recreate the form points in all case
      dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    
      return 1;
    }// end of creation mode

    dt_masks_form_gui_points_t *gui_points
        = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
    if(!gui_points) return 0;

    // The shape handler runs before the shared press-state selection update,
    // so concrete hovered targets must win over stale form/source selection.
    else if(mask_gui->node_hovered >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(mask_gui->node_selected && dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        _change_node_type(module, mask_form, mask_gui, form_index);
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

      if(dt_modifier_is(state, GDK_CONTROL_MASK))
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
  if(!mask_gui) return 0;
  if(mask_gui->creation) return 1;

  if(which == 1)
  {
    const gboolean was_dragging = dt_masks_gui_is_dragging(mask_gui);

    if(was_dragging)
    {
      if(mask_gui->rebuild_pending)
      {
        dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
        dt_develop_t *const dev = darktable.develop;
        if(dev)
        {
          mask_gui->last_rebuild_ts = dt_get_wtime();
          mask_gui->last_rebuild_pos[0] = mask_gui->pos[0];
          mask_gui->last_rebuild_pos[1] = mask_gui->pos[1];
        }
        mask_gui->rebuild_pending = FALSE;
      }
      return 1;
    }
  }
  return 0;
}

static int _polygon_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event,
                                       dt_masks_form_t *mask_form, int parent_id,
                                       dt_masks_form_gui_t *mask_gui, int form_index)
{
  if(!mask_gui || !mask_form) return 0;

  if(mask_gui->creation)
  {
    switch(event->keyval)
    {
      case GDK_KEY_BackSpace:
      {
        // Minimum points to create a polygon
        if(mask_gui->node_dragging < 1)
        {
          dt_masks_form_cancel_creation(module, mask_gui);
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

        dt_dev_pixelpipe_refresh_preview(darktable.develop, FALSE);
        return 1;
      }
      case GDK_KEY_KP_Enter:
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
  dt_develop_t *const dev = (dt_develop_t *)darktable.develop;
  if(!mask_gui) return 0;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return 0;
  if(!mask_form) return 0;

  const int iwidth = darktable.develop->roi.raw_width;
  const int iheight = darktable.develop->roi.raw_height;

  if(mask_gui->node_dragging >= 0)
  {
    if(!mask_form->points) return 0;
    if(mask_gui->creation && !g_list_shorter_than(mask_form->points, 4))
    {
      // check if we are near the first point to close the polygon on creation
      const float dist_curs = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;
      const float dx = mask_gui->pos[0] - gui_points->points[2];
      const float dy = mask_gui->pos[1] - gui_points->points[3];
      const float dist2 = dx * dx + dy * dy;
      mask_gui->creation_closing_form = dist2 <= dist_curs * dist_curs;
    }

    dt_masks_node_polygon_t *dragged_node
        = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_dragging);

    // update continuously the current node to mouse position
    float raw_point[2] = { mask_gui->pos[0] + mask_gui->delta[0], mask_gui->pos[1] + mask_gui->delta[1] };
    dt_dev_coordinates_image_abs_to_raw_norm(dev, raw_point, 1);
    // we move all points
    const float dx = raw_point[0] - dragged_node->node[0];
    const float dy = raw_point[1] - dragged_node->node[1];

    dragged_node->ctrl1[0] += dx;
    dragged_node->ctrl2[0] += dx;
    dragged_node->ctrl1[1] += dy;
    dragged_node->ctrl2[1] += dy;
    dragged_node->node[0] += dx;
    dragged_node->node[1] += dy;

    // if first point, adjust the source position accordingly
    if((mask_form->type & DT_MASKS_CLONE) && mask_gui->node_dragging == 0)
    {
      mask_form->source[0] += dx;
      mask_form->source[1] += dy;
    }

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

  if(!mask_form->points) return 0;
  const guint node_count = g_list_length(mask_form->points);

  if(mask_gui->seg_dragging >= 0)
  {
    const GList *const pt = g_list_nth(mask_form->points, mask_gui->seg_dragging);
    const GList *const next_pt = g_list_next_wraparound(pt, mask_form->points);
    dt_masks_node_polygon_t *point = (dt_masks_node_polygon_t *)pt->data;
    dt_masks_node_polygon_t *next_point = (dt_masks_node_polygon_t *)next_pt->data;

    // we get point0 new values
    float raw_point[2] = { mask_gui->pos[0] + mask_gui->delta[0], mask_gui->pos[1] + mask_gui->delta[1] };
    dt_dev_coordinates_image_abs_to_raw_norm(dev, raw_point, 1);
    // we move all points
    const float dx = raw_point[0] - point->node[0];
    const float dy = raw_point[1] - point->node[1];

    // if first or last segment, update the source accordingly
    // (the source point follows the first/last segment when moved)
    if((mask_form->type & DT_MASKS_CLONE)
       && (mask_gui->seg_dragging == 0 || mask_gui->seg_dragging == (int)node_count - 1))
    {
      mask_form->source[0] += dx;
      mask_form->source[1] += dy;
    }

    point->node[0] += dx;
    point->node[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;

    next_point->node[0] += dx;
    next_point->node[1] += dy;
    next_point->ctrl1[0] += dx;
    next_point->ctrl1[1] += dy;
    next_point->ctrl2[0] += dx;
    next_point->ctrl2[1] += dy;

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
    if(!node) return 0;

    float pts[2] = { mask_gui->pos[0] + mask_gui->delta[0], mask_gui->pos[1] + mask_gui->delta[1] };

    // compute ctrl points directly from new handle position
    float p[4];
    _polygon_handle_to_ctrl(gui_points->points[mask_gui->handle_dragging * 6 + 2],
                            gui_points->points[mask_gui->handle_dragging * 6 + 3],
                            pts[0], pts[1], &p[0], &p[1], &p[2], &p[3], gui_points->clockwise);

    dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, p, 2);

    // set new ctrl points
    node->ctrl1[0] = p[0];
    node->ctrl1[1] = p[1];
    node->ctrl2[0] = p[2];
    node->ctrl2[1] = p[3];
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
    if(!node) return 0;

    const int base = node_index * 6;
    const int node_point_index = base + 2;
    const float dx_line = gui_points->border[base] - gui_points->points[node_point_index];

    // Get delta between the node and its border handle
    float pts[2];
    // Get the cursor position
    const float cursor_x = mask_gui->pos[0] + mask_gui->delta[0];
    const float cursor_y = mask_gui->pos[1] + mask_gui->delta[1];

    // Project the cursor position onto the line defined by the node and its border handle
    if(fabsf(dx_line) < 1e-6f)
    {
      // The line is vertical, so we just take the y coordinate of the cursor
      // and the x coordinate of the node
      pts[0] = gui_points->points[node_point_index];
      pts[1] = cursor_y;
    }
    else
    {
      // Calculate the slope (a) and intercept (b) of the line defined by the node and its border handle
      const float a = (gui_points->border[base + 1] - gui_points->points[node_point_index + 1]) / dx_line;
      const float b = gui_points->points[node_point_index + 1] - a * gui_points->points[node_point_index];
      const float denom = a * a + 1.0f;
      // Project the cursor position onto the line
      const float xproj = (a * cursor_y + cursor_x - b * a) / denom;

      pts[0] = xproj;
      pts[1] = a * xproj + b;
    }

    dt_dev_coordinates_image_abs_to_raw_abs(darktable.develop, pts, 1);

    float node_pos[2] = { node->node[0], node->node[1] };
    dt_dev_coordinates_raw_norm_to_raw_abs(dev, node_pos, 1);
    // Calculate distance from node to border handle
    const float dx = pts[0] - node_pos[0];
    const float dy = pts[1] - node_pos[1];
    const float bdr = sqrtf(dx * dx + dy * dy);
    const float border = bdr / fminf(iwidth, iheight);

    node->border[0] = node->border[1] = border;
    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, form_index, module,
                                       mask_gui->pos[0], mask_gui->pos[1]);

    return 1;
  }
  else if(mask_gui->form_dragging || mask_gui->source_dragging)
  {
    // we get point0 new values
    float raw_point[2] = { mask_gui->pos[0] + mask_gui->delta[0], mask_gui->pos[1] + mask_gui->delta[1] };
    dt_dev_coordinates_image_abs_to_raw_norm(dev, raw_point, 1);

    if(mask_gui->form_dragging)
    {
      dt_masks_node_polygon_t *dragging_shape = (dt_masks_node_polygon_t *)(mask_form->points)->data;
      if(!dragging_shape) return 0;
      const float dx = raw_point[0] - dragging_shape->node[0];
      const float dy = raw_point[1] - dragging_shape->node[1];
      // we move all points
      for(GList *nodes = mask_form->points; nodes; nodes = g_list_next(nodes))
      {
        dragging_shape = (dt_masks_node_polygon_t *)nodes->data;
        dragging_shape->node[0] += dx;
        dragging_shape->node[1] += dy;
        dragging_shape->ctrl1[0] += dx;
        dragging_shape->ctrl1[1] += dy;
        dragging_shape->ctrl2[0] += dx;
        dragging_shape->ctrl2[1] += dy;
      }
    }
    else
    {
      mask_form->source[0] = raw_point[0];
      mask_form->source[1] = raw_point[1];
    }

    // we recreate the form points
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    return 1;
  }

  if(dt_masks_gui_should_hit_test(mask_gui))
    _find_closest_handle(mask_form, mask_gui, form_index);

  if(mask_gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

/**
 * @brief Draw a polygon or border polyline, skipping NaN points.
 */
static void _polygon_draw_shape(cairo_t *cr, const float *point_buffer, const int point_count,
                                const int node_count, const gboolean draw_border, const gboolean draw_source)
{
  
  // Find the first valid non-NaN point to start drawing
  // FIXME: Why not just avoid having NaN points in the array?
  int start_idx = -1;
  for(int point_index = node_count * 3 + draw_border; point_index < point_count; point_index++)
  {
    if(!isnan(point_buffer[point_index * 2]) && !isnan(point_buffer[point_index * 2 + 1]))
    {
      start_idx = point_index;
      break;
    }
  }

  // Only draw if we have at least one valid point
  if(start_idx >= 0)
  {
    cairo_move_to(cr, point_buffer[start_idx * 2], point_buffer[start_idx * 2 + 1]);
    for(int point_index = start_idx + 1; point_index < point_count; point_index++)
    {
      if(!isnan(point_buffer[point_index * 2]) && !isnan(point_buffer[point_index * 2 + 1]))
        cairo_line_to(cr, point_buffer[point_index * 2], point_buffer[point_index * 2 + 1]);
    }
  }
}

/**
 * @brief Draw polygon overlays (nodes, handles, borders, source) after exposure.
 */
static void _polygon_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *mask_gui,
                                        int form_index, int node_count)
{
  if(!mask_gui) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return;
  const int selected_node = dt_masks_gui_selected_node_index(mask_gui);
  const int selected_handle = dt_masks_gui_selected_handle_index(mask_gui);
  const int selected_handle_border = dt_masks_gui_selected_handle_border_index(mask_gui);

  if(mask_gui->creation)
  {
    // draw a cross where the source will be created
    dt_masks_form_t *visible_form = dt_masks_get_visible_form(darktable.develop);
    if(visible_form && (visible_form->type & DT_MASKS_CLONE))
    {

      float node_posx = node_count ? gui_points->points[2] : mask_gui->pos[0];
      float node_posy = node_count ? gui_points->points[3] : mask_gui->pos[1];

      float pts[2] = { 0.0, 0.0 };
      dt_masks_calculate_source_pos_value(mask_gui, node_posx, node_posy, node_posx, node_posy, &pts[0], &pts[1], FALSE);
      dt_draw_cross(cr, zoom_scale, pts[0], pts[1]);
    }
  }

  // update clockwise info for the handles
  else if((mask_gui->type & DT_MASKS_IS_RETOUCHE) != 0 || mask_gui->node_selected || mask_gui->node_dragging >= 0
          || mask_gui->handle_selected)
  {
    dt_masks_form_t *group_form = dt_masks_get_visible_form(darktable.develop);
    if(!group_form) return;
    dt_masks_form_group_t *group_entry = g_list_nth_data(group_form->points, form_index);
    if(!group_entry) return;
    dt_masks_form_t *polygon_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
    if(!polygon_form) return;
    gui_points->clockwise = _polygon_is_clockwise(polygon_form);
  }

  // draw polygon
  if(node_count > 0 && gui_points->points_count > node_count * 3 + 6) // there must be something to draw
  {
    dt_masks_draw_path_seg_by_seg(cr, mask_gui, form_index, gui_points->points, gui_points->points_count,
                                  node_count, zoom_scale);
  }

  if(mask_gui->group_selected == form_index)
  {
    // draw borders
    if(gui_points->border_count > node_count * 3 + 2)
    {
      dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, node_count, (mask_gui->border_selected), zoom_scale,
                          gui_points->border, gui_points->border_count, &dt_masks_functions_polygon.draw_shape,
                          CAIRO_LINE_CAP_ROUND);
    }

    // draw the current node's handle if it's a curve node
    if(mask_gui->node_selected && selected_node >= 0
       && !dt_masks_node_is_cusp(gui_points, selected_node))
    {
      const int node_index = selected_node;
      float handle[2];
      _polygon_ctrl2_to_handle(gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3],
                               gui_points->points[node_index * 6 + 4], gui_points->points[node_index * 6 + 5],
                               &handle[0], &handle[1], gui_points->clockwise);
      const float pt[2] = { gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3] };
      const gboolean selected = (mask_gui->node_hovered == node_index
                                 || (selected_handle == node_index));
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
    if(mask_gui->node_selected && selected_node >= 0)
    {
      const int edited = selected_node;
      const gboolean selected = (mask_gui->node_hovered == edited
                                 || (selected_handle_border == edited));
      const int curr_node = edited * 6;  
      const float handle[2] = { gui_points->border[curr_node], gui_points->border[curr_node + 1] };

      dt_draw_handle(cr, NULL, zoom_scale, handle, selected, TRUE);
    }
  }

  // draw the source if needed
  if(gui_points->source_count > node_count * 3 + 2)
  {
    dt_masks_gui_center_point_t center_pt;
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
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int border_index = corner_count * 3; border_index < border_count; border_index++)
  {
    // we look at the borders
    const float xx = border_buffer[border_index * 2];
    const float yy = border_buffer[border_index * 2 + 1];
    if(isnan(xx))
    {
     if(isnan(yy)) break; // that means we have to skip the end of the border polygon
      border_index = yy - 1;
      continue;
    }
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

static int _get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const mask_form, int *width, int *height, int *posx, int *posy,
                     gboolean get_source)
{
  if(!module) return 1;

  // we get buffers for all points
  float *point_buffer = NULL;
  float *border_buffer = NULL;
  int point_count = 0;
  int border_count = 0;

  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,
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

static int _polygon_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *mask_form, int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, mask_form, width, height, posx, posy, TRUE);
}

static int _polygon_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const mask_form,
                             int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, mask_form, width, height, posx, posy, FALSE);
}

/**
 * @brief Write a falloff segment into the mask buffer.
 */
/*static*/ void _polygon_falloff(float *const restrict buffer, int *p0, int *p1,
                                 int posx, int posy, int buffer_width)
{
  // segment length
  int l = sqrtf(sqf(p1[0] - p0[0]) + sqf(p1[1] - p0[1])) + 1;

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

static int _polygon_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                             dt_masks_form_t *const mask_form,
                             float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 1;
  double start = 0.0;
  double start2 = 0.0;

  if(darktable.unmuted & DT_DEBUG_PERF) start = dt_get_wtime();

  // we get buffers for all points
  float *point_buffer = NULL;
  float *border_buffer = NULL;
  int point_count = 0;
  int border_count = 0;
  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order,
                             DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &point_buffer, &point_count,
                             &border_buffer, &border_count, FALSE) != 0)
  {
    dt_pixelpipe_cache_free_align(point_buffer);
    dt_pixelpipe_cache_free_align(border_buffer);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
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
  const gboolean sparse = (piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                           || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL);
  const int sparse_factor = sparse ? 4 : 1;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  const size_t bufsize = (size_t)(*width) * (*height);
  // ensure that the buffer is zeroed, as the following code only actually sets the polygon+falloff pixels
  float *const restrict bufptr = *buffer = dt_pixelpipe_cache_alloc_align_float_cache(bufsize, 0);
  if(bufptr) memset(bufptr, 0, sizeof(float) * bufsize);
  if(*buffer == NULL)
  {
    dt_pixelpipe_cache_free_align(point_buffer);
    dt_pixelpipe_cache_free_align(border_buffer);
    return 1;
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
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill draw polygon took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

#ifdef _OPENMP
#pragma omp parallel for \
  dt_omp_firstprivate(hb, wb, bufptr) \
  schedule(static) if((size_t)hb * wb > 50000)
#endif
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

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill plain took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // now we fill the falloff
  int p0[2] = { 0 }, p1[2] = { 0 };
  float pf1[2] = { 0.0f };
  int prev0[2] = { 0 }, prev1[2] = { 0 };
  gboolean have_prev = FALSE;
  int last0[2] = { -100, -100 }, last1[2] = { -100, -100 };
  int next = 0;
  for(int i = corner_count * 3; i < border_count; i++)
  {
    p0[0] = point_buffer[i * 2];
    p0[1] = point_buffer[i * 2 + 1];
    if(next > 0)
      p1[0] = pf1[0] = border_buffer[next * 2], p1[1] = pf1[1] = border_buffer[next * 2 + 1];
    else
      p1[0] = pf1[0] = border_buffer[i * 2], p1[1] = pf1[1] = border_buffer[i * 2 + 1];

    // now we check p1 value to know if we have to skip a part
    if(next == i) next = 0;
    while(isnan(pf1[0]))
    {
      if(isnan(pf1[1]))
        next = i - 1;
      else
        next = p1[1];
      p1[0] = pf1[0] = border_buffer[next * 2];
      p1[1] = pf1[1] = border_buffer[next * 2 + 1];
    }

    const gboolean used_next = (next > 0);

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

    // and we draw the falloff
    if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
    {
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

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);

  dt_pixelpipe_cache_free_align(point_buffer);
  dt_pixelpipe_cache_free_align(border_buffer);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start);

  return 0;
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

  roi_crop_segment_t *xmax_segs = malloc(sizeof(*xmax_segs) * point_count);
  roi_crop_segment_t *ymax_segs = malloc(sizeof(*ymax_segs) * point_count);
  if(!xmax_segs || !ymax_segs)
  {
    free(xmax_segs);
    free(ymax_segs);
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

  free(xmax_segs);
  xmax_segs = NULL;

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

  free(ymax_segs);
  ymax_segs = NULL;
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
static int _polygon_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                                 dt_masks_form_t *const mask_form,
                                 const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 1;
  double start = 0.0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start = dt_get_wtime();

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;
  const gboolean sparse = (piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                           || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL);
  const int sparse_factor = sparse ? 4 : 1;

  // we need to take care of four different cases:
  // 1) polygon and feather are outside of roi
  // 2) polygon is outside of roi, feather reaches into roi
  // 3) roi lies completely within polygon
  // 4) all other situations :)
  int polygon_in_roi = 0;
  int feather_in_roi = 0;
  int polygon_encircles_roi = 0;

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count = 0, border_count = 0;
  if(_polygon_get_pts_border(module->dev, mask_form, module->iop_order,
                             DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,
                             &points, &points_count, &border, &border_count, FALSE) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 1;
  }
  if(points_count <= 2)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon points took %0.04f sec\n",
             mask_form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  const guint corner_count = g_list_length(mask_form->points);

  // we shift and scale down polygon and border
  for(int i = corner_count * 3; i < border_count; i++)
  {
    const float xx = border[2 * i];
    const float yy = border[2 * i + 1];
    if(isnan(xx))
    {
      if(isnan(yy)) break; // that means we have to skip the end of the border polygon
      i = yy - 1;
      continue;
    }
    border[2 * i] = xx * scale - px;
    border[2 * i + 1] = yy * scale - py;
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

  // now check if feather is at least partially within roi
  for(int i = corner_count * 3; i < border_count; i++)
  {
    const float xx = border[i * 2];
    const float yy = border[i * 2 + 1];
    if(isnan(xx))
    {
      if(isnan(yy)) break; // that means we have to skip the end of the border polygon
      i = yy - 1;
      continue;
    }
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
    return 0;
  }

  // now get min/max values
  float xmin, xmax, ymin, ymax;
  _polygon_bounding_box_raw(points, border, corner_count, points_count, border_count,
                            &xmin, &xmax, &ymin, &ymax);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
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
    if(cpoints == NULL)
    {
      dt_pixelpipe_cache_free_align(points);
      dt_pixelpipe_cache_free_align(border);
      return 1;
    }
    memcpy(cpoints, points, sizeof(float) * 2 * points_count);

    // now we clip cpoints to roi -> catch special case when roi lies completely within polygon.
    // dirty trick: we allow polygon to extend one pixel beyond height-1. this avoids need of special handling
    // of the last roi line in the following edge-flag polygon fill algorithm.
    const int crop_success = _polygon_crop_to_roi(cpoints + 2 * (corner_count * 3),
                                                  points_count - corner_count * 3, 0,
                                                  width - 1, 0, height);
    polygon_encircles_roi = polygon_encircles_roi || !crop_success;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill crop to roi took %0.04f sec\n", mask_form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }

    if(polygon_encircles_roi)
    {
      // roi lies completely within polygon
      for(size_t k = 0; k < (size_t)width * height; k++) buffer[k] = 1.0f;
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

      if(darktable.unmuted & DT_DEBUG_PERF)
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

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(xxmin, xxmax, yymin, yymax, width, buffer) \
  schedule(static) \
  if((size_t)(yymax - yymin + 1) * (size_t)(xxmax - xxmin + 1) > 50000)
#endif
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

      if(darktable.unmuted & DT_DEBUG_PERF)
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
    const int dpoints_capacity = 4 * border_count * (sparse ? sparse_factor : 1);
    int *dpoints = dt_pixelpipe_cache_alloc_align_cache(sizeof(int) * dpoints_capacity, 0);
    if(dpoints == NULL)
    {
      dt_pixelpipe_cache_free_align(points);
      dt_pixelpipe_cache_free_align(border);
      return 1;
    }

    int dindex = 0;
    int p0[2], p1[2];
    float pf1[2];
    int prev0[2] = { 0, 0 };
    int prev1[2] = { 0, 0 };
    gboolean have_prev = FALSE;
    int last0[2] = { -100, -100 };
    int last1[2] = { -100, -100 };
    int next_index = 0;
    for(int i = corner_count * 3; i < border_count; i++)
    {
      p0[0] = floorf(points[i * 2] + 0.5f);
      p0[1] = ceilf(points[i * 2 + 1]);
      if(next_index > 0)
      {
        p1[0] = pf1[0] = border[next_index * 2];
        p1[1] = pf1[1] = border[next_index * 2 + 1];
      }
      else
      {
        p1[0] = pf1[0] = border[i * 2];
        p1[1] = pf1[1] = border[i * 2 + 1];
      }

      // now we check p1 value to know if we have to skip a part
      if(next_index == i) next_index = 0;
      while(isnan(pf1[0]))
      {
        if(isnan(pf1[1]))
          next_index = i - 1;
        else
          next_index = p1[1];
        p1[0] = pf1[0] = border[next_index * 2];
        p1[1] = pf1[1] = border[next_index * 2 + 1];
      }

      const gboolean used_next = (next_index > 0);

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

      // and we draw the falloff
      if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
      {
        dpoints[dindex] = p0[0];
        dpoints[dindex + 1] = p0[1];
        dpoints[dindex + 2] = p1[0];
        dpoints[dindex + 3] = p1[1];
        dindex += 4;

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

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, dindex, buffer, dpoints) \
  if(dindex > 4096)
#endif
    for(int n = 0; n < dindex; n += 4)
      _polygon_falloff_roi(buffer, dpoints + n, dpoints + n + 2, width, height);

    dt_pixelpipe_cache_free_align(dpoints);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n",
               mask_form->name,
               dt_get_wtime() - start2);
    }
  }

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n",
             mask_form->name,
             dt_get_wtime() - start);

  return 0;
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
                                      const int opacity, char *const restrict msgbuf,
                                      const size_t msgbuf_len)
{
  const guint node_count = mask_form->points ? g_list_length(mask_form->points) : 0;
  if(mask_gui->creation && node_count < 4)
    g_strlcat(msgbuf, _("<b>Add node</b>: click, <b>Add sharp node</b>:ctrl+click\n"
                        "<b>Cancel</b>: right-click or Esc"), msgbuf_len);
  else if(mask_gui->creation)
    g_strlcat(msgbuf, _("<b>Add node</b>: click, <b>Add sharp node</b>:ctrl+click\n"
                        "<b>Finish polygon</b>: Enter or click on first node"), msgbuf_len);
  else if(mask_gui->handle_selected)
    g_strlcat(msgbuf, _("<b>Node curvature</b>: drag\n<b>Reset curvature</b>: right-click"), msgbuf_len);
  else if(mask_gui->node_selected)
    g_strlcat(msgbuf, _("<b>NODE:</b> <b>Move</b>: drag, <b>Delete</b>: right-click or Del\n"
                        "<b>Hardness</b>: scroll, <b>Switch smooth/sharp</b>: ctrl+click"), msgbuf_len);
  else if(mask_gui->node_hovered >= 0)
    g_strlcat(msgbuf, _("<b>Move node</b>: drag\n<b>Delete node</b>: right-click\n"
                        "<b>Hardness</b>: scroll, <b>Switch smooth/sharp</b>: ctrl+click"), msgbuf_len);
  else if(mask_gui->seg_selected)
    g_strlcat(msgbuf, _("<b>Move segment</b>: drag\n<b>Add node</b>: ctrl+click"), msgbuf_len);
  else if(mask_gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len, _("<b>Size</b>: scroll, <b>Hardness</b>: shift+scroll\n"
                                     "<b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _polygon_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
   // unused arg, keep compiler from complaining
  dt_masks_duplicate_points(base, dest, sizeof(dt_masks_node_polygon_t));
}

static void _polygon_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  
  
  float offset[2] = { 0.1f, 0.1f };
  dt_dev_coordinates_raw_norm_to_raw_abs(darktable.develop, offset, 1);
  *x = offset[0];
  *y = offset[1];
}

static void _polygon_creation_closing_form_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  // This is a temp form on creation mode
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  if(!mask_form) return;

  _polygon_creation_closing_form(mask_form, mask_gui);
}

static void _polygon_switch_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(!mask_gui) return;
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;
  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, form_id);
  if(!selected_form) return;

  mask_gui->node_selected = TRUE;
  _change_node_type(module, selected_form, mask_gui, mask_gui->group_selected);
}

static void _polygon_reset_round_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(!mask_gui) return;
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;
  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, form_id);
  if(!selected_form) return;

  mask_gui->node_selected = TRUE;
  _reset_ctrl_points(module, selected_form, mask_gui, mask_gui->group_selected);
}

static void _polygon_add_node_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(!mask_gui) return;
  dt_masks_form_t *visible_forms = dt_masks_get_visible_form(darktable.develop);
  if(!visible_forms) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  dt_masks_form_group_t *group_entry = dt_masks_form_get_selected_group(visible_forms, mask_gui);
  if(!group_entry) return;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);

  if(selected_form)
  {
    _add_node_to_segment(module, selected_form, group_entry->parentid, mask_gui, mask_gui->group_selected);
  }

  //dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
}

static int _polygon_populate_context_menu(GtkWidget *menu, struct dt_masks_form_t *mask_form,
                                          struct dt_masks_form_gui_t *mask_gui,
                                          const float pzx, const float pzy)
{
  
  
  GtkWidget *menu_item = NULL;
  gchar *accel = g_strdup_printf(_("%s+Click"), gtk_accelerator_get_label(0, GDK_CONTROL_MASK));

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
    if(!gui_points) goto end;
    dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)g_list_nth_data(mask_form->points, mask_gui->node_hovered);
    if(!node) goto end;
    const gboolean is_corner = dt_masks_node_is_cusp(gui_points, mask_gui->node_hovered);

    {
      gchar *to_change_type = g_strdup_printf(_("Switch to %s node"), (is_corner) ? _("round") : _("cusp"));
      const dt_menu_icon_t icon = is_corner ? DT_MENU_ICON_CIRCLE : DT_MENU_ICON_SQUARE;
      menu_item = ctx_gtk_menu_item_new_with_icon_and_shortcut(to_change_type, accel, menu,
                                                               _polygon_switch_node_callback, mask_gui, icon);

      g_free(to_change_type);
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
  g_free(accel);
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
