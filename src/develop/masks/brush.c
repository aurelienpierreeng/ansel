/*
    This file is part of darktable,
    Copyright (C) 2013-2016, 2021 Aldric Renaudin.
    Copyright (C) 2013 Jean-Sébastien Pédron.
    Copyright (C) 2013, 2017, 2019-2022 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2013-2016, 2019 Ulrich Pegelow.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017 Matthieu Moy.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Diederik Ter Rahe.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
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

#define HARDNESS_MIN 0.0005f
#define HARDNESS_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

/**
 * @brief Get squared distance of a point to the segment, including payload deltas.
 *
 * Uses the first and last points as segment endpoints and adds weighted distance
 * in border/hardness/density space to preserve brush dynamics.
 */
static float _brush_point_line_distance2(int point_index, int point_count,
                                         const float *point_buffer, const float *payload_buffer)
{
  const float point_x = point_buffer[2 * point_index];
  const float point_y = point_buffer[2 * point_index + 1];
  const float point_border = payload_buffer[4 * point_index];
  const float point_hardness = payload_buffer[4 * point_index + 1];
  const float point_density = payload_buffer[4 * point_index + 2];
  const float start_x = point_buffer[0];
  const float start_y = point_buffer[1];
  const float start_border = payload_buffer[0];
  const float start_hardness = payload_buffer[1];
  const float start_density = payload_buffer[2];
  const float end_x = point_buffer[2 * (point_count - 1)];
  const float end_y = point_buffer[2 * (point_count - 1) + 1];
  const float end_border = payload_buffer[4 * (point_count - 1)];
  const float end_hardness = payload_buffer[4 * (point_count - 1) + 1];
  const float end_density = payload_buffer[4 * (point_count - 1) + 2];
  const float bweight = 1.0f;
  const float hweight = 0.01f;
  const float dweight = 0.01f;

  const float segment_dx = end_x - start_x;
  const float segment_dy = end_y - start_y;
  const float segment_db = end_border - start_border;
  const float segment_dh = end_hardness - start_hardness;
  const float segment_dd = end_density - start_density;

  const float dot = (point_x - start_x) * segment_dx + (point_y - start_y) * segment_dy;
  const float segment_len2 = sqf(segment_dx) + sqf(segment_dy);
  const float t = dot / segment_len2;

  float dx = 0.0f, dy = 0.0f, db = 0.0f, dh = 0.0f, dd = 0.0f;

  if(segment_len2 == 0.0f)
  {
    dx = point_x - start_x;
    dy = point_y - start_y;
    db = point_border - start_border;
    dh = point_hardness - start_hardness;
    dd = point_density - start_density;
  }
  else if(t < 0.0f)
  {
    dx = point_x - start_x;
    dy = point_y - start_y;
    db = point_border - start_border;
    dh = point_hardness - start_hardness;
    dd = point_density - start_density;
  }
  else if(t > 1.0f)
  {
    dx = point_x - end_x;
    dy = point_y - end_y;
    db = point_border - end_border;
    dh = point_hardness - end_hardness;
    dd = point_density - end_density;
  }
  else
  {
    dx = point_x - (start_x + t * segment_dx);
    dy = point_y - (start_y + t * segment_dy);
    db = point_border - (start_border + t * segment_db);
    dh = point_hardness - (start_hardness + t * segment_dh);
    dd = point_density - (start_density + t * segment_dd);
  }

  return sqf(dx) + sqf(dy) + bweight * sqf(db) + hweight * dh * dh + dweight * sqf(dd);
}

/**
 * @brief Remove unneeded points (Ramer-Douglas-Peucker) and return the reduced path.
 */
static GList *_brush_ramer_douglas_peucker(const float *point_buffer, int point_count,
                                           const float *payload_buffer, float epsilon2)
{
  GList *result_list = NULL;

  float dmax2 = 0.0f;
  int split_index = 0;

  for(int point_index = 1; point_index < point_count - 1; point_index++)
  {
    const float d2 = _brush_point_line_distance2(point_index, point_count,
                                                 point_buffer, payload_buffer);
    if(d2 > dmax2)
    {
      split_index = point_index;
      dmax2 = d2;
    }
  }

  if(dmax2 >= epsilon2)
  {
    GList *left_list = _brush_ramer_douglas_peucker(point_buffer, split_index + 1,
                                                    payload_buffer, epsilon2);
    GList *right_list = _brush_ramer_douglas_peucker(point_buffer + split_index * 2,
                                                     point_count - split_index,
                                                     payload_buffer + split_index * 4, epsilon2);

    // remove last element from left_list to avoid duplication at the split
    GList *end1 = g_list_last(left_list);
    free(end1->data);
    left_list = g_list_delete_link(left_list, end1);

    result_list = g_list_concat(left_list, right_list);
  }
  else
  {
    dt_masks_node_brush_t *first_node = malloc(sizeof(dt_masks_node_brush_t));
    first_node->node[0] = point_buffer[0];
    first_node->node[1] = point_buffer[1];
    first_node->ctrl1[0] = first_node->ctrl1[1] = first_node->ctrl2[0] = first_node->ctrl2[1] = -1.0f;
    first_node->border[0] = first_node->border[1] = payload_buffer[0];
    first_node->hardness = payload_buffer[1];
    first_node->density = payload_buffer[2];
    first_node->state = DT_MASKS_POINT_STATE_NORMAL;
    result_list = g_list_append(result_list, (gpointer)first_node);

    dt_masks_node_brush_t *last_node = malloc(sizeof(dt_masks_node_brush_t));
    last_node->node[0] = point_buffer[(point_count - 1) * 2];
    last_node->node[1] = point_buffer[(point_count - 1) * 2 + 1];
    last_node->ctrl1[0] = last_node->ctrl1[1] = last_node->ctrl2[0] = last_node->ctrl2[1] = -1.0f;
    last_node->border[0] = last_node->border[1] = payload_buffer[(point_count - 1) * 4];
    last_node->hardness = payload_buffer[(point_count - 1) * 4 + 1];
    last_node->density = payload_buffer[(point_count - 1) * 4 + 2];
    last_node->state = DT_MASKS_POINT_STATE_NORMAL;
    result_list = g_list_append(result_list, (gpointer)last_node);
  }

  return result_list;
}

/**
 * @brief Evaluate a cubic Bezier at t in [0, 1].
 */
static void _brush_get_XY(float p0_x, float p0_y, float p1_x, float p1_y, float p2_x, float p2_y, float p3_x,
                          float p3_y, float t, float *out_x, float *out_y)
{
  const float ti = 1.0f - t;
  const float a = ti * ti * ti;
  const float b = 3.0f * t * ti * ti;
  const float c = 3.0f * sqf(t) * ti;
  const float d = t * t * t;
  *out_x = p0_x * a + p1_x * b + p2_x * c + p3_x * d;
  *out_y = p0_y * a + p1_y * b + p2_y * c + p3_y * d;
}

/**
 * @brief Evaluate a cubic Bezier and compute its offset border point.
 */
static void _brush_border_get_XY(float p0_x, float p0_y, float p1_x, float p1_y, float p2_x, float p2_y,
                                 float p3_x, float p3_y, float t, float radius,
                                 float *point_x, float *point_y, float *border_x, float *border_y)
{
  // we get the point
  _brush_get_XY(p0_x, p0_y, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y, t, point_x, point_y);

  // now we get derivative points
  const float ti = 1.0f - t;
  const float a = 3.0f * ti * ti;
  const float b = 3.0f * (ti * ti - 2.0f * t * ti);
  const float c = 3.0f * (2.0f * t * ti - t * t);
  const float d = 3.0f * sqf(t);

  const float dx = -p0_x * a + p1_x * b + p2_x * c + p3_x * d;
  const float dy = -p0_y * a + p1_y * b + p2_y * c + p3_y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *border_x = NAN;
    *border_y = NAN;
    return;
  }
  const float l = 1.0f / sqrtf(dx * dx + dy * dy);
  *border_x = (*point_x) + radius * dy * l;
  *border_y = (*point_y) - radius * dx * l;
}

/**
 * @brief Convert control point 2 into the handle position (orthonormal space).
 */
static void _brush_ctrl2_to_handle(float point_x, float point_y, float ctrl_x, float ctrl_y,
                                   float *handle_x, float *handle_y, gboolean clockwise)
{
  if(clockwise)
  {
    *handle_x = point_x + ctrl_y - point_y;
    *handle_y = point_y + point_x - ctrl_x;
  }
  else
  {
    *handle_x = point_x - ctrl_y + point_y;
    *handle_y = point_y - point_x + ctrl_x;
  }
}

/**
 * @brief get the border handle position corresponding to a node, by looking for the closest border point in the resampled points of the form GUI.
 * 
 * This is used when we have to initialize control points for a node that has been set manually by the user,
 * so we want to get the corresponding handle position to eventually match a catmull-rom like spline.
 * The values should be in orthonormal space.
 * 
 * @param gui_points the form GUI points
 * @param node_count the number of nodes in the form
 * @param node_index the index of the node for which we want to get the border handle position
 * @param x the x coordinate of the border handle position (output)
 * @param y the y coordinate of the border handle position (output)
 * @return gboolean TRUE if the border handle position has been successfully retrieved, FALSE otherwise
 */
static gboolean _brush_get_border_handle_resampled(const dt_masks_form_gui_points_t *gui_points, int node_count,
                                                   int node_index, float *handle_x, float *handle_y)
{
  if(!gui_points || node_count <= 0 || node_index < 0 || node_index >= node_count) return FALSE;

  const int start = node_count * 3;
  const int max_points = MIN(gui_points->points_count, gui_points->border_count);
  if(max_points <= start) return FALSE;

  const float node_x = gui_points->points[node_index * 6 + 2];
  const float node_y = gui_points->points[node_index * 6 + 3];

  float best_dist2 = FLT_MAX;
  int best_idx = -1;

  for(int i = start; i < max_points; i++)
  {
    const float px = gui_points->points[i * 2];
    const float py = gui_points->points[i * 2 + 1];
    if(isnan(px) || isnan(py)) continue;

    const float dx = node_x - px;
    const float dy = node_y - py;
    const float dist2 = dx * dx + dy * dy;
    if(dist2 < best_dist2)
    {
      best_dist2 = dist2;
      best_idx = i;
    }
  }

  if(best_idx < 0) return FALSE;

  *handle_x = gui_points->border[best_idx * 2];
  *handle_y = gui_points->border[best_idx * 2 + 1];
  return !(isnan(*handle_x) || isnan(*handle_y));
}

static gboolean _brush_get_border_handle_mirrored(const dt_masks_form_gui_points_t *gui_points, int node_count,
                                                  int node_index, float *handle_x, float *handle_y)
{
  float resampled_x = NAN;
  float resampled_y = NAN;
  if(!_brush_get_border_handle_resampled(gui_points, node_count, node_index, &resampled_x, &resampled_y)) return FALSE;

  const float node_x = gui_points->points[node_index * 6 + 2];
  const float node_y = gui_points->points[node_index * 6 + 3];

  *handle_x = node_x - (resampled_x - node_x);
  *handle_y = node_y - (resampled_y - node_y);
  return !(isnan(*handle_x) || isnan(*handle_y));
}

/** get bezier control points from feather extremity */
/** the values should be in orthonormal space */
static void _brush_handle_to_ctrl(float ptx, float pty, float fx, float fy,
                                   float *ctrl1x, float *ctrl1y,
                                   float *ctrl2x, float *ctrl2y, gboolean clockwise)
{
  if(clockwise)
  {
    *ctrl2x = ptx + pty - fy;
    *ctrl2y = pty + fx - ptx;
    *ctrl1x = ptx - pty + fy;
    *ctrl1y = pty - fx + ptx;
  }
  else
  {
    *ctrl1x = ptx + pty - fy;
    *ctrl1y = pty + fx - ptx;
    *ctrl2x = ptx - pty + fy;
    *ctrl2y = pty - fx + ptx;
  }
}

/**
 * @brief Get Bezier control points that match a Catmull-Rom segment.
 */
static void _brush_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                     float y4, float *bezier1_x, float *bezier1_y,
                                     float *bezier2_x, float *bezier2_y)
{
  *bezier1_x = (-x1 + 6 * x2 + x3) / 6;
  *bezier1_y = (-y1 + 6 * y2 + y3) / 6;
  *bezier2_x = (x2 + 6 * x3 - x4) / 6;
  *bezier2_y = (y2 + 6 * y3 - y4) / 6;
}

/**
 * @brief Initialize all control points to match a Catmull-Rom-like spline.
 *
 * Only nodes in DT_MASKS_POINT_STATE_NORMAL are regenerated.
 */
static void _brush_init_ctrl_points(dt_masks_form_t *mask_form)
{
  if(!mask_form || !mask_form->points) return;
  // if we have less than 2 points, what to do ??
  if(g_list_shorter_than(mask_form->points, 2)) return;

  // we need extra points to deal with curve ends
  dt_masks_node_brush_t start_point[2], end_point[2];

  for(GList *form_points = mask_form->points; form_points; form_points = g_list_next(form_points))
  {
    dt_masks_node_brush_t *point3 = (dt_masks_node_brush_t *)form_points->data;
    if(!point3) continue;
    // if the point has not been set manually, we redefine it
    if(point3->state == DT_MASKS_POINT_STATE_NORMAL)
    {
      // we want to get point-2, point-1, point+1, point+2
      GList *const prev = g_list_previous(form_points);             // point-1
      GList *const prevprev = prev ? g_list_previous(prev) : NULL;  // point-2
      GList *const next = g_list_next(form_points);                 // point+1
      GList *const nextnext = next ? g_list_next(next) : NULL;      // point+2
      dt_masks_node_brush_t *point1 = prevprev ? prevprev->data : NULL;
      dt_masks_node_brush_t *point2 = prev ? prev->data : NULL;
      dt_masks_node_brush_t *point4 = next ? next->data : NULL;
      dt_masks_node_brush_t *point5 = nextnext ? nextnext->data : NULL;

      // deal with end points: make both extending points mirror their neighborhood
      if(point1 == NULL && point2 == NULL)
      {
        start_point[0].node[0] = start_point[1].node[0] = 2 * point3->node[0] - point4->node[0];
        start_point[0].node[1] = start_point[1].node[1] = 2 * point3->node[1] - point4->node[1];
        point1 = &(start_point[0]);
        point2 = &(start_point[1]);
      }
      else if(point1 == NULL)
      {
        start_point[0].node[0] = 2 * point2->node[0] - point3->node[0];
        start_point[0].node[1] = 2 * point2->node[1] - point3->node[1];
        point1 = &(start_point[0]);
      }

      if(point4 == NULL && point5 == NULL)
      {
        end_point[0].node[0] = end_point[1].node[0] = 2 * point3->node[0] - point2->node[0];
        end_point[0].node[1] = end_point[1].node[1] = 2 * point3->node[1] - point2->node[1];
        point4 = &(end_point[0]);
        point5 = &(end_point[1]);
      }
      else if(point5 == NULL)
      {
        end_point[0].node[0] = 2 * point4->node[0] - point3->node[0];
        end_point[0].node[1] = 2 * point4->node[1] - point3->node[1];
        point5 = &(end_point[0]);
      }


      float bx1 = 0.0f, by1 = 0.0f, bx2 = 0.0f, by2 = 0.0f;
      _brush_catmull_to_bezier(point1->node[0], point1->node[1], point2->node[0], point2->node[1],
                               point3->node[0], point3->node[1], point4->node[0], point4->node[1],
                               &bx1, &by1, &bx2, &by2);
      if(point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bx1;
      if(point2->ctrl2[1] == -1.0) point2->ctrl2[1] = by1;
      point3->ctrl1[0] = bx2;
      point3->ctrl1[1] = by2;
      _brush_catmull_to_bezier(point2->node[0], point2->node[1], point3->node[0], point3->node[1],
                               point4->node[0], point4->node[1], point5->node[0], point5->node[1],
                               &bx1, &by1, &bx2, &by2);
      if(point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bx2;
      if(point4->ctrl1[1] == -1.0) point4->ctrl1[1] = by2;
      point3->ctrl2[0] = bx1;
      point3->ctrl2[1] = by1;
    }
  }
}


/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the corner is very sharp */
static void _brush_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax,
                                             dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                             gboolean clockwise)
{
  // we want to find the start and end angles
  float a1 = atan2f(bmin[1] - cmax[1], bmin[0] - cmax[0]);
  float a2 = atan2f(bmax[1] - cmax[1], bmax[0] - cmax[0]);

  if(a1 == a2) return;

  // we have to be sure that we turn in the correct direction
  if(a2 < a1 && clockwise)
  {
    a2 += 2.0f * M_PI;
  }
  if(a2 > a1 && !clockwise)
  {
    a1 += 2.0f * M_PI;
  }

  // we determine start and end radius too
  float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // and the max length of the circle arc
  const int l = fabsf(a2 - a1) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  const float incra = (a2 - a1) / l;
  const float incrr = (r2 - r1) / l;
  
  // Use incremental rotation to avoid repeated cosf/sinf calls
  const float cos_incra = cosf(incra);
  const float sin_incra = sinf(incra);
  float rr = r1 + incrr;
  float cos_aa = cosf(a1 + incra);
  float sin_aa = sinf(a1 + incra);
  
  // allocate entries in the dynbufs
  float *dpoints_ptr = dt_masks_dynbuf_reserve_n(dpoints, 2*(l-1));
  float *dborder_ptr = dt_masks_dynbuf_reserve_n(dborder, 2*(l-1));
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if (dpoints_ptr && dborder_ptr)
  {
    for(int i = 1; i < l; i++)
    {
      *dpoints_ptr++ = cmax[0];
      *dpoints_ptr++ = cmax[1];
      *dborder_ptr++ = cmax[0] + rr * cos_aa;
      *dborder_ptr++ = cmax[1] + rr * sin_aa;
      
      // Incremental rotation: rotate by incra using addition formulas
      const float new_cos = cos_aa * cos_incra - sin_aa * sin_incra;
      const float new_sin = sin_aa * cos_incra + cos_aa * sin_incra;
      cos_aa = new_cos;
      sin_aa = new_sin;
      rr += incrr;
    }
  }
}

/** fill small gap between 2 points with an arc of circle */
/** in contrast to the previous function it will always run the shortest path (max. PI) and does not consider
 * clock or anti-clockwise action */
static void _brush_points_recurs_border_small_gaps(float *cmax, float *bmin, float *bmin2, float *bmax,
                                                   dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder)
{
  // we want to find the start and end angles
  const float a1 = fmodf(atan2f(bmin[1] - cmax[1], bmin[0] - cmax[0]) + 2.0f * M_PI, 2.0f * M_PI);
  const float a2 = fmodf(atan2f(bmax[1] - cmax[1], bmax[0] - cmax[0]) + 2.0f * M_PI, 2.0f * M_PI);

  if(a1 == a2) return;

  // we determine start and end radius too
  const float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  const float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // we close the gap in the shortest direction
  float delta = a2 - a1;
  if(fabsf(delta) > M_PI) delta = delta - copysignf(2.0f * M_PI, delta);

  // get the max length of the circle arc
  const int l = fabsf(delta) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  const float incra = delta / l;
  const float incrr = (r2 - r1) / l;
  
  // Use incremental rotation to avoid repeated cosf/sinf calls
  const float cos_incra = cosf(incra);
  const float sin_incra = sinf(incra);
  float rr = r1 + incrr;
  float cos_aa = cosf(a1 + incra);
  float sin_aa = sinf(a1 + incra);
  
  // allocate entries in the dynbufs
  float *dpoints_ptr = dt_masks_dynbuf_reserve_n(dpoints, 2*(l-1));
  float *dborder_ptr = dt_masks_dynbuf_reserve_n(dborder, 2*(l-1));
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if (dpoints_ptr && dborder_ptr)
  {
    for(int i = 1; i < l; i++)
    {
      *dpoints_ptr++ = cmax[0];
      *dpoints_ptr++ = cmax[1];
      *dborder_ptr++ = cmax[0] + rr * cos_aa;
      *dborder_ptr++ = cmax[1] + rr * sin_aa;
      
      // Incremental rotation: rotate by incra using addition formulas
      const float new_cos = cos_aa * cos_incra - sin_aa * sin_incra;
      const float new_sin = sin_aa * cos_incra + cos_aa * sin_incra;
      cos_aa = new_cos;
      sin_aa = new_sin;
      rr += incrr;
    }
  }
}


/** draw a circle with given radius. can be used to terminate a stroke and to draw junctions where attributes
 * (opacity) change */
static void _brush_points_stamp(float *cmax, float *bmin, dt_masks_dynbuf_t *dpoints,  dt_masks_dynbuf_t *dborder,
                                gboolean clockwise)
{
  // we want to find the start angle
  const float a1 = atan2f(bmin[1] - cmax[1], bmin[0] - cmax[0]);

  // we determine the radius too
  const float rad = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));

  // determine the max length of the circle arc
  const int l = 2.0f * M_PI * rad;
  if(l < 2) return;

  // and now we add the points
  const float incra = 2.0f * M_PI / l;
  float aa = a1 + incra;
  // allocate entries in the dynbuf
  float *dpoints_ptr = dt_masks_dynbuf_reserve_n(dpoints, 2*(l-1));
  float *dborder_ptr = dt_masks_dynbuf_reserve_n(dborder, 2*(l-1));
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if (dpoints_ptr && dborder_ptr)
  {
    for(int i = 0; i < l; i++)
    {
      *dpoints_ptr++ = cmax[0];
      *dpoints_ptr++ = cmax[1];
      *dborder_ptr++ = cmax[0] + rad * cosf(aa);
      *dborder_ptr++ = cmax[1] + rad * sinf(aa);
      aa += incra;
    }
  }
}

static inline gboolean _is_within_pxl_threshold(float *min, float *max, int pixel_threshold)
{
  return abs((int)min[0] - (int)max[0]) < pixel_threshold && 
         abs((int)min[1] - (int)max[1]) < pixel_threshold;
}

static inline void _brush_payload_sync(dt_masks_dynbuf_t *dpayload, dt_masks_dynbuf_t *dpoints,
                                       const float v0, const float v1)
{
  size_t payload_pos = dt_masks_dynbuf_position(dpayload);
  const size_t target_pos = dt_masks_dynbuf_position(dpoints);
  while(payload_pos < target_pos)
  {
    dt_masks_dynbuf_add_2(dpayload, v0, v1);
    payload_pos += 2;
  }
}

/** recursive function to get all points of the brush AND all point of the border */
/** the function takes care to avoid big gaps between points */
static void _brush_points_recurs(float *p1, float *p2, double tmin, double tmax, float *points_min,
                                 float *points_max, float *border_min, float *border_max, float *rpoints,
                                 float *rborder, float *rpayload, dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                 dt_masks_dynbuf_t *dpayload, const int pixel_threshold)
{
  const gboolean withborder = (dborder != NULL);
  const gboolean withpayload = (dpayload != NULL);

  // we calculate points if needed
  if(isnan(points_min[0]))
  {
    _brush_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmin,
                         p1[4] + (p2[4] - p1[4]) * tmin * tmin * (3.0 - 2.0 * tmin), points_min,
                         points_min + 1, border_min, border_min + 1);
  }
  if(isnan(points_max[0]))
  {
    _brush_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmax,
                         p1[4] + (p2[4] - p1[4]) * tmax * tmax * (3.0 - 2.0 * tmax), points_max,
                         points_max + 1, border_max, border_max + 1);
  }

  // are the points near ?
  if((tmax - tmin < 0.0001f)
     || (_is_within_pxl_threshold(points_min, points_max, pixel_threshold)
         && (!withborder || (_is_within_pxl_threshold(border_min, border_max, pixel_threshold)))))
  {
    rpoints[0] = points_max[0];
    rpoints[1] = points_max[1];
    dt_masks_dynbuf_add_2(dpoints, rpoints[0], rpoints[1]);

    if(withborder)
    {
      if(isnan(border_max[0]))
      {
        border_max[0] = border_min[0];
        border_max[1] = border_min[1];
      }
      else if(isnan(border_min[0]))
      {
        border_min[0] = border_max[0];
        border_min[1] = border_max[1];
      }

      // we check gaps in the border (sharp edges)
      if(abs((int)border_max[0] - (int)border_min[0]) > 2 || abs((int)border_max[1] - (int)border_min[1]) > 2)
      {
        _brush_points_recurs_border_small_gaps(points_max, border_min, NULL, border_max, dpoints, dborder);
      }

      rborder[0] = border_max[0];
      rborder[1] = border_max[1];
      dt_masks_dynbuf_add_2(dborder, rborder[0], rborder[1]);
    }

    if(withpayload)
    {
      rpayload[0] = p1[5] + tmax * (p2[5] - p1[5]);
      rpayload[1] = p1[6] + tmax * (p2[6] - p1[6]);
      _brush_payload_sync(dpayload, dpoints, rpayload[0], rpayload[1]);
    }

    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { NAN, NAN }, b[2] = { NAN, NAN };
  float rc[2], rb[2], rp[2];
  _brush_points_recurs(p1, p2, tmin, tx, points_min, c, border_min, b, rc, rb, rp, dpoints, dborder, dpayload,
                       pixel_threshold);
  _brush_points_recurs(p1, p2, tx, tmax, rc, points_max, rb, border_max, rpoints, rborder, rpayload, dpoints,
                       dborder, dpayload, pixel_threshold);
}


/** converts n into a cyclical sequence counting upwards from 0 to nb-1 and back down again, counting
 * endpoints twice */
static inline int _brush_cyclic_cursor(int n, int nb)
{
  const int o = n % (2 * nb);
  const int p = o % nb;

  return (o <= p) ? o : o - 2 * p - 1;
}


/** get all points of the brush and the border */
/** this takes care of gaps and iop distortions */
// Brush points are stored in a cyclic way because the border goes around the main line.
// This means that it record the main line twice (up and down) while the border only once (around).
static int _brush_get_pts_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                 const double iop_order, const int transform_direction,
                                 dt_dev_pixelpipe_t *pipe, float **point_buffer, int *point_count,
                                 float **border_buffer, int *border_count, float **payload_buffer,
                                 int *payload_count, int use_source)
{
  if(!mask_form || !mask_form->points) return 0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float iwd = pipe->iwidth;
  const float iht = pipe->iheight;
  const int pixel_threshold = (pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                               || pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL) ? 3 : 1;

  *point_buffer = NULL;
  *point_count = 0;
  if(border_buffer) *border_buffer = NULL;
  if(border_buffer) *border_count = 0;
  if(payload_buffer) *payload_buffer = NULL;
  if(payload_buffer) *payload_count = 0;

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *dpayload = NULL;

  dpoints = dt_masks_dynbuf_init(1000000, "brush dpoints");
  if(dpoints == NULL) return 1;

  if(border_buffer)
  {
    dborder = dt_masks_dynbuf_init(1000000, "brush dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 1;
    }
  }

  if(payload_buffer)
  {
    dpayload = dt_masks_dynbuf_init(1000000, "brush dpayload");
    if(dpayload == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      dt_masks_dynbuf_free(dborder);
      return 1;
    }
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  if(use_source && mask_form->points && transform_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    dt_masks_node_brush_t *pt = (dt_masks_node_brush_t *)mask_form->points->data;
    dx = (pt->node[0] - mask_form->source[0]) * iwd;
    dy = (pt->node[1] - mask_form->source[1]) * iht;
  }

  const guint node_count = g_list_length(mask_form->points);

  dt_masks_node_brush_t **nodes = malloc((size_t)node_count * sizeof(*nodes));
  if(!nodes)
  {
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    dt_masks_dynbuf_free(dpayload);
    return 1;
  }

  guint node_index = 0;
  for(GList *form_points = mask_form->points; form_points; form_points = g_list_next(form_points))
  {
    const dt_masks_node_brush_t *const pt = (dt_masks_node_brush_t *)form_points->data;
    nodes[node_index++] = (dt_masks_node_brush_t *)form_points->data;
    float *const buf = dt_masks_dynbuf_reserve_n(dpoints, 6);
    if (buf)
    {
      buf[0] = pt->ctrl1[0] * iwd - dx;
      buf[1] = pt->ctrl1[1] * iht - dy;
      buf[2] = pt->node[0] * iwd - dx;
      buf[3] = pt->node[1] * iht - dy;
      buf[4] = pt->ctrl2[0] * iwd - dx;
      buf[5] = pt->ctrl2[1] * iht - dy;
    }
  }

  // for the border, we store value too
  if(dborder)
  {
    dt_masks_dynbuf_add_zeros(dborder, 6 * node_count);  // we need six zeros for each border point
  }

  // for the payload, we reserve an equivalent number of cells to keep it in sync
  if(dpayload)
  {
    dt_masks_dynbuf_add_zeros(dpayload, 6 * node_count); // we need six zeros for each border point
  }

  int cw = 1;
  int start_stamp = 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points init took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we render all segments first upwards, then downwards
  for(int n = 0; n < 2 * node_count; n++)
  {
    float p1[7], p2[7], p3[7], p4[7];
    const int k = _brush_cyclic_cursor(n, node_count);
    const int k1 = _brush_cyclic_cursor(n + 1, node_count);
    const int k2 = _brush_cyclic_cursor(n + 2, node_count);
    const gboolean allow_border_gap_rounding = FALSE;

    dt_masks_node_brush_t *point1 = nodes[k];
    dt_masks_node_brush_t *point2 = nodes[k1];
    dt_masks_node_brush_t *point3 = nodes[k2];
    if(cw > 0)
    {
      const float pa[7] = { point1->node[0] * iwd - dx, point1->node[1] * iht - dy, point1->ctrl2[0] * iwd - dx,
                            point1->ctrl2[1] * iht - dy, point1->border[1] * MIN(iwd, iht), point1->hardness,
                            point1->density };
      const float pb[7] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl1[0] * iwd - dx,
                            point2->ctrl1[1] * iht - dy, point2->border[0] * MIN(iwd, iht), point2->hardness,
                            point2->density };
      const float pc[7] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl2[0] * iwd - dx,
                            point2->ctrl2[1] * iht - dy, point2->border[1] * MIN(iwd, iht), point2->hardness,
                            point2->density };
      const float pd[7] = { point3->node[0] * iwd - dx, point3->node[1] * iht - dy, point3->ctrl1[0] * iwd - dx,
                            point3->ctrl1[1] * iht - dy, point3->border[0] * MIN(iwd, iht), point3->hardness,
                            point3->density };
      memcpy(p1, pa, sizeof(float) * 7);
      memcpy(p2, pb, sizeof(float) * 7);
      memcpy(p3, pc, sizeof(float) * 7);
      memcpy(p4, pd, sizeof(float) * 7);
    }
    else
    {
      const float pa[7] = { point1->node[0] * iwd - dx, point1->node[1] * iht - dy, point1->ctrl1[0] * iwd - dx,
                            point1->ctrl1[1] * iht - dy, point1->border[1] * MIN(iwd, iht), point1->hardness,
                            point1->density };
      const float pb[7] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl2[0] * iwd - dx,
                            point2->ctrl2[1] * iht - dy, point2->border[0] * MIN(iwd, iht), point2->hardness,
                            point2->density };
      const float pc[7] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl1[0] * iwd - dx,
                            point2->ctrl1[1] * iht - dy, point2->border[1] * MIN(iwd, iht), point2->hardness,
                            point2->density };
      const float pd[7] = { point3->node[0] * iwd - dx, point3->node[1] * iht - dy, point3->ctrl2[0] * iwd - dx,
                            point3->ctrl2[1] * iht - dy, point3->border[0] * MIN(iwd, iht), point3->hardness,
                            point3->density };
      memcpy(p1, pa, sizeof(float) * 7);
      memcpy(p2, pb, sizeof(float) * 7);
      memcpy(p3, pc, sizeof(float) * 7);
      memcpy(p4, pd, sizeof(float) * 7);
    }

    // 1st. special case: render abrupt transitions between different opacity and/or hardness values
    if((fabsf(p1[5] - p2[5]) > 0.05f || fabsf(p1[6] - p2[6]) > 0.05f)
       || (start_stamp && n == 2 * node_count - 1))
    {
      if(n == 0)
      {
        start_stamp = 1; // remember to deal with the first node as a final step
      }
      else
      {
        if(dborder)
        {
          float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
          float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
          _brush_points_stamp(cmax, bmin, dpoints, dborder, TRUE);
        }

        if(dpayload)
        {
          _brush_payload_sync(dpayload, dpoints, p1[5], p1[6]);
        }
      }
    }

    // 2nd. special case: render transition point between different brush sizes
    if(fabsf(p1[4] - p2[4]) > 0.0001f && n > 0)
    {
      if(dborder)
      {
        float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
        float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
        float bmax[2] = { 2 * cmax[0] - bmin[0], 2 * cmax[1] - bmin[1] };
        if(allow_border_gap_rounding)
          _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, dpoints, dborder, TRUE);
      }

      if(dpayload)
      {
        _brush_payload_sync(dpayload, dpoints, p1[5], p1[6]);
      }
    }

    // 3rd. special case: render endpoints
    if(k == k1)
    {
      if(dborder)
      {
        float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
        float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
        float bmax[2] = { 2 * cmax[0] - bmin[0], 2 * cmax[1] - bmin[1] };
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, dpoints, dborder, TRUE);
      }

      if(dpayload)
      {
        _brush_payload_sync(dpayload, dpoints, p1[5], p1[6]);
      }

      cw *= -1;
      continue;
    }

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2], rb[2], rp[2];
    float bmin[2] = { NAN, NAN };
    float bmax[2] = { NAN, NAN };
    float cmin[2] = { NAN, NAN };
    float cmax[2] = { NAN, NAN };

    _brush_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, rp, dpoints, dborder, dpayload,
                         pixel_threshold);

    dt_masks_dynbuf_add_2(dpoints, rc[0], rc[1]);

    if(dpayload)
    {
      dt_masks_dynbuf_add_2(dpayload, rp[0], rp[1]);
    }

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
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && node_count >= 3)
    {
      // we get the next point (start of the next segment)
      _brush_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0, p3[4], cmin, cmin + 1,
                           bmax, bmax + 1);
      if(isnan(bmax[0]))
      {
        _brush_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.0001, p3[4], cmin,
                             cmin + 1, bmax, bmax + 1);
      }
      if(bmax[0] - rb[0] > 1 || bmax[0] - rb[0] < -1 || bmax[1] - rb[1] > 1 || bmax[1] - rb[1] < -1)
      {
        // float bmin2[2] = {(*border)[posb-22],(*border)[posb-21]};
        if(allow_border_gap_rounding)
          _brush_points_recurs_border_gaps(rc, rb, NULL, bmax, dpoints, dborder, cw);
      }
    }

    if(dpayload)
    {
      _brush_payload_sync(dpayload, dpoints, rp[0], rp[1]);
    }
  }

  free(nodes);
  nodes = NULL;

  *point_count = dt_masks_dynbuf_position(dpoints) / 2;
  *point_buffer = dt_masks_dynbuf_harvest(dpoints);
  dt_masks_dynbuf_free(dpoints);

  if(dborder)
  {
    *border_count = dt_masks_dynbuf_position(dborder) / 2;
    *border_buffer = dt_masks_dynbuf_harvest(dborder);
    dt_masks_dynbuf_free(dborder);
  }

  if(dpayload)
  {
    *payload_count = dt_masks_dynbuf_position(dpayload) / 2;
    *payload_buffer = dt_masks_dynbuf_harvest(dpayload);
    dt_masks_dynbuf_free(dpayload);
  }
  // printf("points %d, border %d, playload %d\n", *points_count, border ? *border_count : -1, payload ?
  // *payload_count : -1);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points point recurs %0.04f sec\n", mask_form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // and we transform them with all distorted modules
  if(use_source && transform_direction == DT_DEV_TRANSFORM_DIR_ALL)
  {
    // we transform with all distortion that happen *before* the module
    // so we have now the TARGET points in module input reference
    if(dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                     *point_buffer, *point_count))
    {
      // now we move all the points by the shift
      // so we have now the SOURCE points in module input reference
      float pts[2] = { mask_form->source[0] * iwd, mask_form->source[1] * iht };
      if(!dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1))
        goto fail;

      dx = pts[0] - (*point_buffer)[2];
      dy = pts[1] - (*point_buffer)[3];
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(point_count, point_buffer, dx, dy)              \
    schedule(static) if(*point_count > 100) aligned(point_buffer:64)
#endif
      for(int i = 0; i < *point_count; i++)
      {
        (*point_buffer)[i * 2] += dx;
        (*point_buffer)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_dev_distort_transform_plus(develop, pipe, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                        *point_buffer, *point_count))
        goto fail;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_points end took %0.04f sec\n",
               mask_form->name, dt_get_wtime() - start2);

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
        dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points transform took %0.04f sec\n",
                 mask_form->name,
                 dt_get_wtime() - start2);
      return 0;
    }
  }

  // if we failed, then free all and return
fail:
  dt_pixelpipe_cache_free_align(*point_buffer);
  *point_buffer = NULL;
  *point_count = 0;
  if(border_buffer)
  {
    dt_pixelpipe_cache_free_align(*border_buffer);
    *border_buffer = NULL;
    *border_count = 0;
  }
  if(payload_buffer)
  {
    dt_pixelpipe_cache_free_align(*payload_buffer);
    *payload_buffer = NULL;
    *payload_count = 0;
  }
  return 1;
}

/**
 * @brief Get the distance between a point and the brush path/border.
 */
static void _brush_get_distance(float point_x, float point_y, float radius,
                                dt_masks_form_gui_t *mask_gui, int form_index,
                                int corner_count, int *inside, int *inside_border,
                                int *near, int *inside_source, float *distance)
{
  if(!mask_gui) return;

  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *distance = FLT_MAX;

  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return;

  float inv_width = 1.0f;
  float inv_height = 1.0f;
  dt_masks_get_distance_normalization(&inv_width, &inv_height);

  float min_dist_pixel = FLT_MAX;
  float min_dist_norm = FLT_MAX;

  const float radius2 = radius * radius;

  // we first check if we are inside the source form

  // add support for clone masks
  if(gui_points->points_count > 2 + corner_count * 3
     && gui_points->source_count > 2 + corner_count * 3)
  {
    // distance from form origin and source origin
    const float dx = -gui_points->points[2] + gui_points->source[2];
    const float dy = -gui_points->points[3] + gui_points->source[3];

    int current_seg = 1;
    for(int i = corner_count * 3; i < gui_points->points_count; i++)
    {
      // do we change of path segment ?
      if(gui_points->points[i * 2 + 1] == gui_points->points[current_seg * 6 + 3]
         && gui_points->points[i * 2] == gui_points->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      // distance from tested point to current form point
      const float yy = gui_points->points[i * 2 + 1] + dy;
      const float xx = gui_points->points[i * 2] + dx;

      const float sdx = point_x - xx;
      const float sdy = point_y - yy;
      const float dd = (sdx * sdx) + (sdy * sdy);
      const float dd_norm = dt_masks_distance_sq_normalized(sdx, sdy, inv_width, inv_height);
      min_dist_norm = fminf(min_dist_norm, dd_norm);
      if(dd < min_dist_pixel)
      {
        min_dist_pixel = dd;

        if(dd < radius2 && *inside == 0)
        {
          if(current_seg == 0)
            *inside_source = corner_count - 1;
          else
            *inside_source = current_seg - 1;

          if(*inside_source)
          {
            *inside = 1;
          }
        }
      }
    }
  }

 // we check if it's inside borders
  if(gui_points->border_count > 2 + corner_count * 3)
  {
    int nearest = -1;

    const int start = corner_count * 3;
    const float *const border = gui_points->border;
    float last_y = border[gui_points->border_count * 2 - 1];
    int crossings = 0;

    for(int i = start; i < gui_points->border_count; i++)
    {
      const int idx = i * 2;
      const float xx = border[idx];
      const float yy = border[idx + 1];

      const float dx = point_x - xx;
      const float dy = point_y - yy;
      if(dx * dx + dy * dy < radius2) nearest = idx;

      if(((point_y <= yy && point_y > last_y) || (point_y >= yy && point_y < last_y)) && (xx > point_x))
        crossings++;

      last_y = yy;
    }

    *inside = *inside_border = (nearest != -1 || (crossings & 1));
  }

  // and we check if we are near a segment
  if(gui_points->points_count > 2 + corner_count * 3)
  {
    int current_seg = 1;
    for(int i = corner_count * 3; i < gui_points->points_count; i++)
    {
      // do we change of path segment ?
      if(gui_points->points[i * 2 + 1] == gui_points->points[current_seg * 6 + 3]
         && gui_points->points[i * 2] == gui_points->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      //distance from tested point to current form point
      const float yy = gui_points->points[i * 2 + 1];
      const float xx = gui_points->points[i * 2];

      const float dx = point_x - xx;
      const float dy = point_y - yy;
      const float dd = (dx * dx) + (dy * dy);
      const float dd_norm = dt_masks_distance_sq_normalized(dx, dy, inv_width, inv_height);
      min_dist_norm = fminf(min_dist_norm, dd_norm);
      if(dd < min_dist_pixel)
      {
        min_dist_pixel = dd;

        if(current_seg > 0 && dd < radius2)
        {
          *near = current_seg - 1;
        }
      }
    }
  }

  *distance = min_dist_norm;
}

static int _brush_get_points_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                                    float **point_buffer, int *point_count,
                                    float **border_buffer, int *border_count,
                                    int use_source, const dt_iop_module_t *module)
{
  if(use_source && !module) return 1;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  return _brush_get_pts_border(develop, mask_form, ioporder, DT_DEV_TRANSFORM_DIR_ALL,
                               develop->virtual_pipe, point_buffer, point_count, border_buffer,
                               border_count, NULL, NULL, use_source);
}

/** find relative position within a brush segment that is closest to the point given by coordinates x and y;
    we only need to find the minimum with a resolution of 1%, so we just do an exhaustive search without any
   frills */
static float _brush_get_position_in_segment(float point_x, float point_y,
                                            dt_masks_form_t *mask_form, int segment_index)
{
  if(!mask_form || !mask_form->points) return 0;
  GList *first_node_entry = g_list_nth(mask_form->points, segment_index);
  dt_masks_node_brush_t *point0 = (dt_masks_node_brush_t *)first_node_entry->data;
  // advance to next node in list, if not already on the last
  GList *next_node_entry = g_list_next_bounded(first_node_entry);
  dt_masks_node_brush_t *point1 = (dt_masks_node_brush_t *)next_node_entry->data;
  next_node_entry = g_list_next_bounded(next_node_entry);
  dt_masks_node_brush_t *point2 = (dt_masks_node_brush_t *)next_node_entry->data;
  next_node_entry = g_list_next_bounded(next_node_entry);
  dt_masks_node_brush_t *point3 = (dt_masks_node_brush_t *)next_node_entry->data;

  float best_t = 0.0f;
  float best_dist2 = FLT_MAX;

  for(int sample_index = 0; sample_index <= 100; sample_index++)
  {
    const float t = sample_index / 100.0f;
    float sample_x = 0.0f;
    float sample_y = 0.0f;
    _brush_get_XY(point0->node[0], point0->node[1], point1->node[0], point1->node[1],
                  point2->node[0], point2->node[1], point3->node[0], point3->node[1], t, &sample_x, &sample_y);

    const float dist2 = (point_x - sample_x) * (point_x - sample_x)
                        + (point_y - sample_y) * (point_y - sample_y);
    if(dist2 < best_dist2)
    {
      best_dist2 = dist2;
      best_t = t;
    }
  }

  return best_t;
}

/**
 * @brief Brush-specific border handle lookup.
 *
 * Uses mirrored border handles computed from resampled border points.
 */
static gboolean _brush_border_handle_cb(const dt_masks_form_gui_points_t *gui_points, int node_count, int node_index,
                                        float *handle_x, float *handle_y, void *user_data)
{
  return _brush_get_border_handle_mirrored(gui_points, node_count, node_index, handle_x, handle_y);
}

/**
 * @brief Brush-specific curve handle lookup.
 *
 * Converts the node's second control point into the GUI handle position.
 */
static void _brush_curve_handle_cb(const dt_masks_form_gui_points_t *gui_points, int node_index,
                                   float *handle_x, float *handle_y, void *user_data)
{
  _brush_ctrl2_to_handle(gui_points->points[node_index * 6 + 2], gui_points->points[node_index * 6 + 3],
                         gui_points->points[node_index * 6 + 4], gui_points->points[node_index * 6 + 5],
                         handle_x, handle_y, TRUE);
}

/**
 * @brief Brush-specific inside/border/segment hit testing.
 */
static void _brush_distance_cb(float pointer_x, float pointer_y, float cursor_radius,
                               dt_masks_form_gui_t *mask_gui, int form_index, int node_count, int *inside,
                               int *inside_border, int *near, int *inside_source, float *dist, void *user_data)
{
  _brush_get_distance(pointer_x, pointer_y, cursor_radius, mask_gui, form_index, node_count,
                      inside, inside_border, near, inside_source, dist);
}

static int _find_closest_handle(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                dt_masks_form_t *mask_form, int parentid,
                                dt_masks_form_gui_t *mask_gui, int form_index)
{
  return dt_masks_find_closest_handle_common(pointer_x, pointer_y, mask_form, parentid, mask_gui, form_index, -1,
                                             _brush_border_handle_cb, _brush_curve_handle_cb, NULL,
                                             _brush_distance_cb, NULL, NULL);
}


static int _init_hardness(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                          const float amount, const dt_masks_increment_t increment, const int flow)
{
  const float masks_hardness = dt_masks_get_set_conf_value_with_toast(mask_form, "hardness", amount,
                                                                      HARDNESS_MIN, HARDNESS_MAX, increment, flow,
                                                                      _("hardness: %3.2f%%"), 100.0f);
  if(mask_gui->guipoints_count > 0)
    dt_masks_dynbuf_set(mask_gui->guipoints_payload, -3, masks_hardness);
  return 1;
}

static int _init_size(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                      const float amount, const dt_masks_increment_t increment, const int flow)
{
  const float masks_border = dt_masks_get_set_conf_value_with_toast(mask_form, "border", amount,
                                                                    HARDNESS_MIN, HARDNESS_MAX, increment, flow,
                                                                    _("size: %3.2f%%"), 2.f * 100.f);
  if(mask_gui->guipoints_count > 0)
    dt_masks_dynbuf_set(mask_gui->guipoints_payload, -4, masks_border);
  return 1;
}

static int _init_opacity(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                         const float amount, const dt_masks_increment_t increment, const int flow)
{
  dt_masks_get_set_conf_value_with_toast(mask_form, "opacity", amount, 0.f, 1.f,
                                         increment, flow, _("opacity: %3.2f%%"), 100.f);
  return 1;
}

static float _brush_get_interaction_value(const dt_masks_form_t *mask_form, dt_masks_interaction_t interaction)
{
  if(!mask_form || !mask_form->points) return NAN;
  const dt_masks_node_brush_t *point = (const dt_masks_node_brush_t *)(mask_form->points)->data;
  if(!point) return NAN;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      return fmaxf(point->border[0], point->border[1]);
    case DT_MASKS_INTERACTION_HARDNESS:
      return point->hardness;
    default:
      return NAN;
  }
}

static gboolean _brush_get_gravity_center(const dt_masks_form_t *mask_form, float center[2], float *area)
{
  if(!mask_form || !mask_form->points) return FALSE;

  const int points_count = g_list_length(mask_form->points);
  if(points_count <= 0) return FALSE;

  float *point_buffer = dt_alloc_align_float((size_t)points_count * 2);
  if(!point_buffer) return FALSE;

  int i = 0;
  for(const GList *l = mask_form->points; l; l = g_list_next(l))
  {
    const dt_masks_node_brush_t *point = (const dt_masks_node_brush_t *)l->data;
    if(!point) continue;
    point_buffer[2 * i] = point->node[0];
    point_buffer[2 * i + 1] = point->node[1];
    i++;
  }

  const gboolean ok = dt_masks_center_of_gravity_from_points(point_buffer, i, center, area);
  dt_free_align(point_buffer);
  return ok;
}

static int _change_hardness(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                            struct dt_iop_module_t *module, int index, const float amount,
                            const dt_masks_increment_t increment, const int flow);
static int _change_size(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                        struct dt_iop_module_t *module, int index, const float amount,
                        const dt_masks_increment_t increment, const int flow);

static float _brush_set_interaction_value(dt_masks_form_t *mask_form, dt_masks_interaction_t interaction, float value,
                                          dt_masks_increment_t increment, int flow,
                                          dt_masks_form_gui_t *mask_gui, struct dt_iop_module_t *module)
{
  if(!mask_form) return NAN;
  const int index = 0;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      if(!_change_size(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
      return _brush_get_interaction_value(mask_form, interaction);
    case DT_MASKS_INTERACTION_HARDNESS:
      if(!_change_hardness(mask_form, 0, mask_gui, module, index, value, increment, flow)) return NAN;
      return _brush_get_interaction_value(mask_form, interaction);
    default:
      return NAN;
  }
}

static int _change_hardness(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                            struct dt_iop_module_t *module, int index, const float amount,
                            const dt_masks_increment_t increment, const int flow)
{
  if(!mask_form || !mask_form->points) return 0;
  const int node_selected = mask_gui->node_selected;
  const int selected = (mask_gui->form_selected && node_selected == -1);
  int node_index = 0;
  const float scale_amount = powf(amount, (float)flow);
  const float offset_amount = amount * (float)flow;
  float result_amount = 0.0f;
  for(GList *node_entry = mask_form->points; node_entry; node_entry = g_list_next(node_entry))
  {
    if(node_selected == node_index || selected)
    {
      dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)node_entry->data;
      const float current_hardness = node->hardness;
      result_amount = dt_masks_apply_increment_precomputed(current_hardness, amount, scale_amount, offset_amount, increment);

      node->hardness = CLAMPF(result_amount, HARDNESS_MIN, HARDNESS_MAX);
    }
    node_index++;
  }

  dt_masks_get_set_conf_value(mask_form, "hardness", result_amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_create(mask_form, mask_gui, index, module);

  return 1;
}

static int _change_size(dt_masks_form_t *mask_form, int parentid, dt_masks_form_gui_t *mask_gui,
                        struct dt_iop_module_t *module, int index, const float amount,
                        const dt_masks_increment_t increment, const int flow)
{
  if(!mask_form || !mask_form->points) return 0;
  // Sanitize loop
  // do not exceed upper limit of 1.0 and lower limit of 0.004
  const int node_hovered = mask_gui->node_hovered;
  int node_index = 0;
  for(GList *node_entry = mask_form->points; node_entry; node_entry = g_list_next(node_entry))
  {
    if(node_hovered == -1 || node_hovered == node_index)
    {
      dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)node_entry->data;
      if(!node) continue;
      if(amount > 1.0f && (node->border[0] > 1.0f || node->border[1] > 1.0f))
        return 1;
    }
    node_index++;
  }

  // Growing/shrinking loop
  const float scale_amount = amount;
  const float offset_amount = amount;
  node_index = 0;
  for(GList *node_entry = mask_form->points; node_entry; node_entry = g_list_next(node_entry))
  {
    if(node_hovered == -1 || node_hovered == node_index)
    {
      dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)node_entry->data;
      if(!node) continue;
      node->border[0] = dt_masks_apply_increment_precomputed(node->border[0], amount,
                                                              scale_amount, offset_amount, increment);
      node->border[1] = dt_masks_apply_increment_precomputed(node->border[1], amount,
                                                              scale_amount, offset_amount, increment);
    }
    node_index++;
  }

  dt_masks_get_set_conf_value(mask_form, "border", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);

  // we recreate the form points
  if(mask_gui && module) dt_masks_gui_form_create(mask_form, mask_gui, index, module);

  return 1;
}

static int _brush_events_mouse_scrolled(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                        int scroll_up, const int flow, uint32_t state, dt_masks_form_t *mask_form,
                                        int parentid, dt_masks_form_gui_t *mask_gui, int index,
                                        dt_masks_interaction_t interaction)
{
  if(mask_gui->creation)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _init_hardness(mask_form, parentid, mask_gui, scroll_up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return _init_opacity(mask_form, parentid, mask_gui, scroll_up ? +0.02f : -0.02f,
                           DT_MASKS_INCREMENT_OFFSET, flow);
    else
      return _init_size(mask_form, parentid, mask_gui, scroll_up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  else if(mask_gui->form_selected || mask_gui->source_selected
          || mask_gui->node_hovered >= 0 || mask_gui->node_selected >= 0
          || mask_gui->handle_selected >= 0 || mask_gui->handle_border_selected >= 0
          || mask_gui->seg_selected >= 0)
  {
    // we register the current position
    if(mask_gui->scrollx == 0.0f && mask_gui->scrolly == 0.0f)
    {
      mask_gui->scrollx = pointer_x;
      mask_gui->scrolly = pointer_y;
    }

    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_change_opacity(mask_form, parentid, scroll_up, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _change_hardness(mask_form, parentid, mask_gui, module, index, scroll_up ? 1.02f : 0.98f,
                              DT_MASKS_INCREMENT_SCALE, flow);
    else // resize don't care where the mouse is inside a shape
      return _change_size(mask_form, parentid, mask_gui, module, index, scroll_up ? 1.02f : 0.98f,
                          DT_MASKS_INCREMENT_SCALE, flow);
  }
  return 0;
}


static void _get_pressure_sensitivity(dt_masks_form_gui_t *mask_gui)
{
  mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  const char *psens = dt_conf_get_string_const("pressure_sensitivity");
  if(psens)
  {
    if(!strcmp(psens, "hardness (absolute)"))
      mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_ABS;
    else if(!strcmp(psens, "hardness (relative)"))
      mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_REL;
    else if(!strcmp(psens, "opacity (absolute)"))
      mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_ABS;
    else if(!strcmp(psens, "opacity (relative)"))
      mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_REL;
    else if(!strcmp(psens, "brush size (relative)"))
      mask_gui->pressure_sensitivity = DT_MASKS_PRESSURE_BRUSHSIZE_REL;
  }
}

// TODO: Should be in masks.c
static void _change_node_type(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                              dt_masks_form_gui_t *mask_gui, int index)
{
  if(!mask_form || !mask_form->points) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
  if(!gui_points) return;
  dt_masks_node_brush_t *node
      = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, mask_gui->node_selected);
  if(!node) return;
  const gboolean is_corner = dt_masks_node_is_cusp(gui_points, mask_gui->node_hovered);

  if(is_corner)
  {
    // Switch to round
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _brush_init_ctrl_points(mask_form);
  }
  else
  {
    // Switch to corner
    node->ctrl1[0] = node->ctrl2[0] = node->node[0];
    node->ctrl1[1] = node->ctrl2[1] = node->node[1];
    node->state = DT_MASKS_POINT_STATE_USER;
  }
  // We recreate the form points

  dt_masks_gui_form_create(mask_form, mask_gui, index, module);
}

static gboolean _reset_ctrl_points(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                                   dt_masks_form_gui_t *mask_gui, int index)
{
  if(!mask_form || !mask_form->points) return FALSE;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, index);
  if(!gui_points) return FALSE;
  const int node_index = MAX(mask_gui->node_hovered, mask_gui->handle_selected);
  dt_masks_node_brush_t *node
      = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, node_index);
  if(!node) return FALSE;

  if(node->state != DT_MASKS_POINT_STATE_NORMAL && !dt_masks_node_is_cusp(gui_points, node_index))
  {
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _brush_init_ctrl_points(mask_form);
    // We recreate the form points
    dt_masks_gui_form_create(mask_form, mask_gui, index, module);
  }
  return TRUE;
}


static void _add_node_to_segment(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                 dt_masks_form_t *mask_form, int parentid,
                                 dt_masks_form_gui_t *mask_gui, int index)
{
  if(!mask_form || !mask_form->points || !mask_gui) return;
  const guint node_count = g_list_length(mask_form->points);
  if(mask_gui->seg_selected < 0 || mask_gui->seg_selected >= (int)node_count) return;

  // we add a new node to the brush
  dt_masks_node_brush_t *new_node = (dt_masks_node_brush_t *)(malloc(sizeof(dt_masks_node_brush_t)));
  if(!new_node) return;

  // set coordinates
  dt_dev_roi_to_input_space(darktable.develop, TRUE, pointer_x, pointer_y, &new_node->node[0], &new_node->node[1]);
  new_node->ctrl1[0] = new_node->ctrl1[1] = new_node->ctrl2[0] = new_node->ctrl2[1] = -1.0;
  new_node->state = DT_MASKS_POINT_STATE_NORMAL;

  // set other attributes of the new node. we interpolate the starting and the end node of that
  // segment
  const float t = _brush_get_position_in_segment(new_node->node[0], new_node->node[1],
                                                 mask_form, mask_gui->seg_selected);
  // start and end node of the segment
  GList *pt = g_list_nth(mask_form->points, mask_gui->seg_selected);
  if(!pt || !pt->data)
  {
    free(new_node);
    return;
  }
  dt_masks_node_brush_t *point0 = (dt_masks_node_brush_t *)pt->data;
  const GList *const next_pt = g_list_next_wraparound(pt, mask_form->points);
  if(!next_pt || !next_pt->data)
  {
    free(new_node);
    return;
  }
  dt_masks_node_brush_t *point1 = (dt_masks_node_brush_t *)next_pt->data;
  new_node->border[0] = point0->border[0] * (1.0f - t) + point1->border[0] * t;
  new_node->border[1] = point0->border[1] * (1.0f - t) + point1->border[1] * t;
  new_node->hardness = point0->hardness * (1.0f - t) + point1->hardness * t;
  new_node->density = point0->density * (1.0f - t) + point1->density * t;

  mask_form->points = g_list_insert(mask_form->points, new_node, mask_gui->seg_selected + 1);
  _brush_init_ctrl_points(mask_form);
  
  dt_masks_gui_form_create(mask_form, mask_gui, index, module);

  mask_gui->node_selected = mask_gui->node_hovered = mask_gui->seg_selected + 1;
  mask_gui->seg_selected = -1;
}

static int _brush_events_button_pressed(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                        double pressure, int which, int type, uint32_t state,
                                        dt_masks_form_t *mask_form, int parentid,
                                        dt_masks_form_gui_t *mask_gui, int index)
{
  // double click or triple click: ignore here
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!mask_gui) return 0;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, index);
  if(!gui_points) return 0;
  if(!mask_form) return 0;
  const guint node_count = g_list_length(mask_form->points);
  dt_develop_t *dev = (dt_develop_t *)darktable.develop;

  // Do we need to refresh currently active node ?
  // Its requested to give back the focus when clicking outside current shape.
  _find_closest_handle(module, pointer_x, pointer_y, mask_form, parentid, mask_gui, index);

  // always start with a mask density of 100%, it will be adjusted with pen pressure if used.
  const float masks_density = 1.0f;

  if(mask_gui->creation)
  {
    if(which == 1)
    {
      // The trick is to use the incremental setting, set to 1.0 to re-use the generic getter/setter without changing value
      float masks_border = dt_masks_get_set_conf_value(mask_form, "border", 1.0f,
                                                       HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);
      float masks_hardness = dt_masks_get_set_conf_value(mask_form, "hardness", 1.0f,
                                                         HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);
    
      if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(mask_form->type & DT_MASKS_CLONE)
          dt_masks_set_source_pos_initial_state(mask_gui, state, pointer_x, pointer_y);

        return 1;
      }

      const float preview_width = darktable.develop->roi.preview_width / dev->roi.natural_scale;
      const float preview_height = darktable.develop->roi.preview_height / dev->roi.natural_scale;

      if(!mask_gui->guipoints) mask_gui->guipoints = dt_masks_dynbuf_init(200000, "brush guipoints");
      if(!mask_gui->guipoints) return 1;
      if(!mask_gui->guipoints_payload)
        mask_gui->guipoints_payload = dt_masks_dynbuf_init(400000, "brush guipoints_payload");
      if(!mask_gui->guipoints_payload) return 1;
      dt_masks_dynbuf_add_2(mask_gui->guipoints, pointer_x * preview_width, pointer_y * preview_height);
      dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, masks_border, masks_hardness);
      dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, masks_density, pressure);

      mask_gui->guipoints_count = 1;

      // add support for clone masks
      if(mask_form->type & DT_MASKS_CLONE)
        dt_masks_set_source_pos_initial_value(mask_gui, mask_form, pointer_x, pointer_y);
      // not used by regular masks
      else
        mask_form->source[0] = mask_form->source[1] = 0.0f;

      _get_pressure_sensitivity(mask_gui);

      return 1;
    }
  }

  else if(which == 1)
  {
    if(mask_gui->source_selected && mask_gui->edit_mode == DT_MASKS_EDIT_FULL)
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
    else if(mask_gui->node_hovered >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(mask_gui->node_selected == mask_gui->node_hovered && dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        _change_node_type(module, mask_form, mask_gui, index);
        return 1;
      }
      /*// we register the current position to avoid accidental move
      if(mask_gui->node_edited < 0 && mask_gui->scrollx == 0.0f && mask_gui->scrolly == 0.0f)
      {
        mask_gui->scrollx = pointer_x;
        mask_gui->scrolly = pointer_y;
      }*/
      mask_gui->delta[0] = gui_points->points[mask_gui->node_hovered * 6 + 2] - mask_gui->pos[0];
      mask_gui->delta[1] = gui_points->points[mask_gui->node_hovered * 6 + 3] - mask_gui->pos[1];

      return 1;
    }
    else if(mask_gui->handle_selected >= 0)
    {
      if(!dt_masks_node_is_cusp(gui_points, mask_gui->handle_selected))
      {
        // we need to find the handle position
        float handle_x, handle_y;
        const int k = mask_gui->handle_selected;
        _brush_ctrl2_to_handle(gui_points->points[k * 6 + 2], gui_points->points[k * 6 + 3],
                               gui_points->points[k * 6 + 4], gui_points->points[k * 6 + 5],
                               &handle_x, &handle_y, TRUE);
        // compute offsets
        mask_gui->delta[0] = handle_x - mask_gui->pos[0];
        mask_gui->delta[1] = handle_y - mask_gui->pos[1];

        return 1;
      }
    }
    else if(mask_gui->handle_border_selected >= 0)
    {
      float handle_x = NAN, handle_y = NAN;
      if(_brush_get_border_handle_mirrored(gui_points, node_count, mask_gui->handle_border_selected,
                                           &handle_x, &handle_y))
      {
        mask_gui->delta[0] = handle_x - mask_gui->pos[0];
        mask_gui->delta[1] = handle_y - mask_gui->pos[1];
      }

      return 1;
    }
    else if(mask_gui->seg_selected >= 0)
    {
      mask_gui->node_hovered = -1;

      if(dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        _add_node_to_segment(module, pointer_x, pointer_y, mask_form, parentid, mask_gui, index);
      }
      else
      {
        // we move the entire segment
        mask_gui->delta[0] = gui_points->points[mask_gui->seg_selected * 6 + 2] - mask_gui->pos[0];
        mask_gui->delta[1] = gui_points->points[mask_gui->seg_selected * 6 + 3] - mask_gui->pos[1];
      }
      return 1;
    }
  }

  return 0;
}

static float _get_brush_smoothing()
{
  float factor = 0.01f;
  const char *smoothing = dt_conf_get_string_const("brush_smoothing");
  if(!strcmp(smoothing, "low"))
    factor = 0.0025f;
  else if(!strcmp(smoothing, "medium"))
    factor = 0.01f;
  else if(!strcmp(smoothing, "high"))
    factor = 0.04f;
  return factor;
}

static void _apply_pen_pressure(dt_masks_form_gui_t *mask_gui, float *payload_buffer)
{
  for(int i = 0; i < mask_gui->guipoints_count; i++)
  {
    float *payload = payload_buffer + 4 * i;
    float pressure = payload[3];
    payload[3] = 1.0f;

    switch(mask_gui->pressure_sensitivity)
    {
      case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
        payload[0] = MAX(HARDNESS_MIN, payload[0] * pressure);
        break;
      case DT_MASKS_PRESSURE_HARDNESS_ABS:
        payload[1] = MAX(HARDNESS_MIN, pressure);
        break;
      case DT_MASKS_PRESSURE_HARDNESS_REL:
        payload[1] = MAX(HARDNESS_MIN, payload[1] * pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_ABS:
        payload[2] = MAX(0.05f, pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_REL:
        payload[2] = MAX(0.05f, payload[2] * pressure);
        break;
      default:
      case DT_MASKS_PRESSURE_OFF:
        // ignore pressure value
        break;
    }
  }
}


static int _brush_events_button_released(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                         int which, uint32_t state, dt_masks_form_t *mask_form, int parentid,
                                         dt_masks_form_gui_t *mask_gui, int index)
{
  if(!mask_gui) return 0;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, index);
  if(!gui_points) return 0;

  // The trick is to use the incremental setting, set to 1.0 to re-use the generic getter/setter without changing value
  float masks_border = dt_masks_get_set_conf_value(mask_form, "border", 1.0f,
                                                   HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);

  if(mask_gui->creation && which == 1)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    {
      // user just set the source position, so just return
      return 1;
    }

    dt_iop_module_t *creation_module = mask_gui->creation_module;

    if(mask_gui->guipoints && mask_gui->guipoints_count > 0)
    {
      // if the path consists only of one x/y pair we add a second one close so we don't need to deal with
      // this special case later
      if(mask_gui->guipoints_count == 1)
      {
        // add a helper node very close to the single spot
        const float x = dt_masks_dynbuf_get(mask_gui->guipoints, -2) + 0.01f;
        const float y = dt_masks_dynbuf_get(mask_gui->guipoints, -1) - 0.01f;
        dt_masks_dynbuf_add_2(mask_gui->guipoints, x, y);
        const float border = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -4);
        const float hardness = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -3);
        const float density = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -2);
        const float pressure = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -1);
        dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, border, hardness);
        dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, density, pressure);
        mask_gui->guipoints_count++;
      }

      float *guipoints = dt_masks_dynbuf_buffer(mask_gui->guipoints);
      float *guipoints_payload = dt_masks_dynbuf_buffer(mask_gui->guipoints_payload);

      // we transform the points
      dt_dev_distort_backtransform(darktable.develop, guipoints, mask_gui->guipoints_count);

      for(int i = 0; i < mask_gui->guipoints_count; i++)
      {
        guipoints[i * 2] /= darktable.develop->roi.raw_width;
        guipoints[i * 2 + 1] /= darktable.develop->roi.raw_height;
      }

      // we consolidate pen pressure readings into payload
      _apply_pen_pressure(mask_gui, guipoints_payload);

      // accuracy level for node elimination, dependent on brush size
      const float epsilon2 = _get_brush_smoothing() * sqf(MAX(HARDNESS_MIN, masks_border));

      // we simplify the path and generate the nodes
      mask_form->points = _brush_ramer_douglas_peucker(guipoints, mask_gui->guipoints_count,
                                                       guipoints_payload, epsilon2);

      // printf("guipoints_count %d, points %d\n", mask_gui->guipoints_count, g_list_length(mask_form->points));

      _brush_init_ctrl_points(mask_form);

      dt_masks_dynbuf_free(mask_gui->guipoints);
      dt_masks_dynbuf_free(mask_gui->guipoints_payload);
      mask_gui->guipoints = NULL;
      mask_gui->guipoints_payload = NULL;
      mask_gui->guipoints_count = 0;

      // we save the form and quit creation mode
      dt_masks_gui_form_save_creation(darktable.develop, creation_module, mask_form, mask_gui);

      if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        dt_masks_form_t *grp = dt_masks_get_visible_form(darktable.develop);
        if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
        int group_index = 0;
        int selected_index = -1;
        for(GList *group_entry = grp->points; group_entry; group_entry = g_list_next(group_entry))
        {
          dt_masks_form_group_t *group_form = (dt_masks_form_group_t *)group_entry->data;
          if(group_form->formid == mask_form->formid)
          {
            selected_index = group_index;
            break;
          }
          group_index++;
        }
        if(selected_index < 0) return 1;
        dt_masks_form_gui_t *visible_gui = darktable.develop->form_gui;
        if(!visible_gui) return 1;
        visible_gui->group_selected = selected_index;

        dt_masks_select_form(creation_module, dt_masks_get_from_id(darktable.develop, mask_form->formid));
      }
    }
    else
    {
      // unlikely case of button released but no points gathered -> no form
      dt_masks_dynbuf_free(mask_gui->guipoints);
      dt_masks_dynbuf_free(mask_gui->guipoints_payload);
      mask_gui->guipoints = NULL;
      mask_gui->guipoints_payload = NULL;
      mask_gui->guipoints_count = 0;

      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);

      dt_masks_change_form_gui(NULL);
    }
    return 1;
  }

  else if(which == 1)
  {
    const gboolean was_dragging = dt_masks_gui_is_dragging(mask_gui);

    if(was_dragging)
    {
      if(mask_gui->rebuild_pending)
      {
        dt_masks_gui_form_create(mask_form, mask_gui, index, module);
        dt_develop_t *const dev = darktable.develop;
        if(dev)
        {
          const float wd = dev->roi.preview_width / dev->roi.natural_scale;
          const float ht = dev->roi.preview_height / dev->roi.natural_scale;
          mask_gui->last_rebuild_ts = dt_get_wtime();
          mask_gui->last_rebuild_pos[0] = pointer_x * wd;
          mask_gui->last_rebuild_pos[1] = pointer_y * ht;
        }
        mask_gui->rebuild_pending = FALSE;
      }
      return 1;
    }
  }
  return 0;
}

static int _brush_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event,
                                     dt_masks_form_t *mask_form, int parentid,
                                     dt_masks_form_gui_t *mask_gui, int index)
{
  return 0;
}

static int _brush_events_mouse_moved(struct dt_iop_module_t *module, float pointer_x, float pointer_y,
                                     double pressure, int which, dt_masks_form_t *mask_form, int parentid,
                                     dt_masks_form_gui_t *mask_gui, int index)
{
  if(!mask_gui) return 0;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, index);
  if(!gui_points) return 0;
  if(!mask_form) return 0;

  dt_develop_t *dev = (dt_develop_t *)darktable.develop;
  const float preview_width = dev->roi.preview_width / dev->roi.natural_scale;
  const float preview_height = dev->roi.preview_height / dev->roi.natural_scale;
  const int iwidth = darktable.develop->roi.raw_width;
  const int iheight = darktable.develop->roi.raw_height;

  if(mask_gui->creation)
  {
    if(mask_gui->guipoints)
    {
      dt_masks_dynbuf_add_2(mask_gui->guipoints, pointer_x * preview_width, pointer_y * preview_height);
      const float border = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -4);
      const float hardness = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -3);
      const float density = dt_masks_dynbuf_get(mask_gui->guipoints_payload, -2);
      dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, border, hardness);
      dt_masks_dynbuf_add_2(mask_gui->guipoints_payload, density, pressure);
      mask_gui->guipoints_count++;
      return 1;
    }
    else
    {
      // Let the cursor motion be redrawn as it moves in GUI
      return 1;
    }
  }

  if(!mask_form->points) return 0;
  const guint node_count = g_list_length(mask_form->points);
  
  if(mask_gui->node_dragging >= 0)
  {
    dt_masks_node_brush_t *dragged_node
        = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, mask_gui->node_dragging);
    if(!dragged_node) return 0;
    // apply delta to the current mouse position
    float cursor_pos[2] = { -1.0f, -1.0f };
    const float pointer_pos[2] = { pointer_x, pointer_y };
    dt_dev_roi_delta_to_input_space(dev, mask_gui->delta, pointer_pos, cursor_pos);
    
    const float delta_x = cursor_pos[0] - dragged_node->node[0];
    const float delta_y = cursor_pos[1] - dragged_node->node[1];

    // we move all points
    dragged_node->ctrl1[0] += delta_x;
    dragged_node->ctrl2[0] += delta_x;
    dragged_node->ctrl1[1] += delta_y;
    dragged_node->ctrl2[1] += delta_y;
    dragged_node->node[0] += delta_x;
    dragged_node->node[1] += delta_y;

    // if first point, adjust the source position accordingly
    if((mask_form->type & DT_MASKS_CLONE) && mask_gui->node_dragging == 0)
    {
      mask_form->source[0] += delta_x;
      mask_form->source[1] += delta_y;
    }

    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, index, module, pointer_x, pointer_y);

    return 1;
  }
  else if(mask_gui->seg_dragging >= 0)
  {
    // we get point0 new values
    const GList *segment_start = g_list_nth(mask_form->points, mask_gui->seg_dragging);
    const GList *segment_end = g_list_next_wraparound(segment_start, mask_form->points);
    dt_masks_node_brush_t *segment_node = (dt_masks_node_brush_t *)segment_start->data;
    dt_masks_node_brush_t *segment_node_next = (dt_masks_node_brush_t *)segment_end->data;
    if(!segment_node || !segment_node_next) return 0;
    float cursor_pos[2]
        = { pointer_x * preview_width + mask_gui->delta[0], pointer_y * preview_height + mask_gui->delta[1] };
    dt_dev_distort_backtransform(darktable.develop, cursor_pos, 1);
    const float delta_x = cursor_pos[0] / iwidth - segment_node->node[0];
    const float delta_y = cursor_pos[1] / iheight - segment_node->node[1];

    // we move all points
    segment_node->node[0] += delta_x;
    segment_node->node[1] += delta_y;
    segment_node->ctrl1[0] += delta_x;
    segment_node->ctrl1[1] += delta_y;
    segment_node->ctrl2[0] += delta_x;
    segment_node->ctrl2[1] += delta_y;
    segment_node_next->node[0] += delta_x;
    segment_node_next->node[1] += delta_y;
    segment_node_next->ctrl1[0] += delta_x;
    segment_node_next->ctrl1[1] += delta_y;
    segment_node_next->ctrl2[0] += delta_x;
    segment_node_next->ctrl2[1] += delta_y;
    
    // if first point's segment, adjust the source position accordingly
    if((mask_form->type & DT_MASKS_CLONE) && mask_gui->seg_dragging == 0)
    {
      mask_form->source[0] += delta_x;
      mask_form->source[1] += delta_y;
    }
    
    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, index, module, pointer_x, pointer_y);

    return 1;
  }
  else if(mask_gui->handle_dragging >= 0)
  {
    dt_masks_node_brush_t *node
        = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, mask_gui->handle_dragging);
    if(!node) return 0;
    float cursor_pos[2]
        = { pointer_x * preview_width + mask_gui->delta[0], pointer_y * preview_height + mask_gui->delta[1] };

    // compute ctrl points directly from new handle position
    float control_points[4];
    _brush_handle_to_ctrl(gui_points->points[mask_gui->handle_dragging * 6 + 2],
                          gui_points->points[mask_gui->handle_dragging * 6 + 3], cursor_pos[0],
                          cursor_pos[1], &control_points[0], &control_points[1], &control_points[2],
                          &control_points[3], TRUE);

    dt_dev_distort_backtransform(darktable.develop, control_points, 2);
    
    // set new ctrl points
    node->ctrl1[0] = control_points[0] / iwidth;
    node->ctrl1[1] = control_points[1] / iheight;
    node->ctrl2[0] = control_points[2] / iwidth;
    node->ctrl2[1] = control_points[3] / iheight;
    node->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(mask_form);
    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, index, module, pointer_x, pointer_y);

    return 1;
  }
 else if(mask_gui->handle_border_dragging >= 0)
  {
    const int node_index = mask_gui->handle_border_dragging;
    dt_masks_node_brush_t *node
        = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, node_index);
    if(!node) return 0;

    float handle_x = NAN, handle_y = NAN;
    if(!_brush_get_border_handle_mirrored(gui_points, node_count, node_index,
                        &handle_x, &handle_y))
      return 0;

    const float node_px = gui_points->points[node_index * 6 + 2];
    const float node_py = gui_points->points[node_index * 6 + 3];
    const float dx_line = handle_x - node_px;
    const float dy_line = handle_y - node_py;

    // Get the cursor position
    float pts[2];
    const float cursor_x = pointer_x * preview_width + mask_gui->delta[0];
    const float cursor_y = pointer_y * preview_height + mask_gui->delta[1];

    // Project the cursor position onto the line defined by the node and its border handle
    if(fabsf(dx_line) < 1e-6f)
    {
      // The line is vertical, so we just take the y coordinate of the cursor
      // and the x coordinate of the node
      pts[0] = node_px;
      pts[1] = cursor_y;
    }
    else
    {
      // Calculate the slope (a) and intercept (b) of the line defined by the node and its border handle
      const float a = dy_line / dx_line;
      const float b = node_py - a * node_px;

      // Project the cursor position onto the line
      const float denom = a * a + 1.0f;
      const float xproj = (a * cursor_y + cursor_x - b * a) / denom;

      pts[0] = xproj;
      pts[1] = a * xproj + b;
    }

    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    // Calculate distance from node to border handle
    const float node_x = node->node[0] * iwidth;
    const float node_y = node->node[1] * iheight;
    const float delta_x = pts[0] - node_x;
    const float delta_y = pts[1] - node_y;
    const float distance = sqrtf(delta_x * delta_x + delta_y * delta_y);
    const float border = distance / fminf(iwidth, iheight);
    
    node->border[0] = node->border[1] = border;

    // we recreate the form points
    dt_masks_gui_form_create_throttled(mask_form, mask_gui, index, module, pointer_x, pointer_y);

    return 1;
  }
  else if(mask_gui->form_dragging || mask_gui->source_dragging)
  {
    float cursor_pos[2]
        = { pointer_x * preview_width + mask_gui->delta[0], pointer_y * preview_height + mask_gui->delta[1] };
    dt_dev_distort_backtransform(darktable.develop, cursor_pos, 1);
    dt_masks_node_brush_t *dragging_shape = (dt_masks_node_brush_t *)(mask_form->points)->data;
    if(!dragging_shape) return 0;

    // we move all points
    if(mask_gui->form_dragging)
    {
      const float delta_x = cursor_pos[0] / darktable.develop->roi.raw_width - dragging_shape->node[0];
      const float delta_y = cursor_pos[1] / darktable.develop->roi.raw_height - dragging_shape->node[1];
      for(GList *node_entry = mask_form->points; node_entry; node_entry = g_list_next(node_entry))
      {
        dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)node_entry->data;
        if(!node) continue;
        node->node[0] += delta_x;
        node->node[1] += delta_y;
        node->ctrl1[0] += delta_x;
        node->ctrl1[1] += delta_y;
        node->ctrl2[0] += delta_x;
        node->ctrl2[1] += delta_y;
      }
    }
    else //source dragging
    {
      mask_form->source[0] = cursor_pos[0] / darktable.develop->roi.raw_width;
      mask_form->source[1] = cursor_pos[1] / darktable.develop->roi.raw_height;
    }
    // we recreate the form points
    dt_masks_gui_form_create(mask_form, mask_gui, index, module);

    return 1;
  }

  if(dt_masks_gui_should_hit_test(mask_gui, dev, pointer_x, pointer_y))
  {
    if(_find_closest_handle(module, pointer_x, pointer_y, mask_form, parentid, mask_gui, index)) return 1;
  }
  if(mask_gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void _brush_draw_shape(cairo_t *cr, const float *points, const int points_count, const int node_nb, const gboolean border, const gboolean source)
{
 // Find the first valid non-NaN point to start drawing
 // FIXME: Why not just avoid having NaN points in the array?
  int start_idx = -1;
  for(int i = node_nb * 3 + border; i < points_count; i++)
  {
    if(!isnan(points[i * 2]) && !isnan(points[i * 2 + 1]))
    {
      start_idx = i;
      break;
    }
  }

  // Only draw if we have at least one valid point
  if(start_idx >= 0)
  {
    cairo_move_to(cr, points[start_idx * 2], points[start_idx * 2 + 1]);

    // We don't want to draw the plain line twice, adapt the end index accordingly
    const int end_idx = border ? points_count : 0.5 * points_count;
    
    for(int i = start_idx + 1; i < end_idx; i++)
    {
      if(!isnan(points[i * 2]) && !isnan(points[i * 2 + 1]))
        cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
    }
  }
}

static float _brush_line_length(const float *line, const int first_pt, const int last_pt)
{
  float total_len = 0.0f;
  for(int i = first_pt; i < last_pt; i++)
  {
    const int i0 = i * 2;
    const int i1 = (i + 1) * 2;
    const float x0 = line[i0];
    const float y0 = line[i0 + 1];
    const float x1 = line[i1];
    const float y1 = line[i1 + 1];
    if(isnan(x0) || isnan(y0) || isnan(x1) || isnan(y1)) continue;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = dx * dx + dy * dy;
    if(len > 1e-12f) total_len += sqrtf(len);
  }
  return total_len;
}

static gboolean _brush_line_point_at_length(const float *line, const int first_pt, const int last_pt,
                                            const float target_len, float *x, float *y)
{
  if(!line || !x || !y) return FALSE;
  if(last_pt <= first_pt) return FALSE;

  float acc = 0.0f;
  gboolean has_fallback = FALSE;
  float fallback_x = NAN, fallback_y = NAN;

  for(int i = first_pt; i < last_pt; i++)
  {
    const int i0 = i * 2;
    const int i1 = (i + 1) * 2;
    const float x0 = line[i0];
    const float y0 = line[i0 + 1];
    const float x1 = line[i1];
    const float y1 = line[i1 + 1];
    if(isnan(x0) || isnan(y0) || isnan(x1) || isnan(y1)) continue;

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    if(len <= 1e-6f) continue;

    has_fallback = TRUE;
    fallback_x = x1;
    fallback_y = y1;

    if(acc + len >= target_len)
    {
      const float t = (target_len - acc) / len;
      *x = x0 + t * dx;
      *y = y0 + t * dy;
      return TRUE;
    }
    acc += len;
  }

  if(!has_fallback || isnan(fallback_x) || isnan(fallback_y)) return FALSE;
  *x = fallback_x;
  *y = fallback_y;
  return TRUE;
}

static gboolean _brush_get_line_midpoint(const float *line, const int first_pt, const int last_pt,
                                         float *mx, float *my)
{
  if(!line || !mx || !my) return FALSE;
  const float total_len = _brush_line_length(line, first_pt, last_pt);
  if(total_len <= 1e-6f) return FALSE;

  const float half_len = 0.5f * total_len;
  return _brush_line_point_at_length(line, first_pt, last_pt, half_len, mx, my);
}

static gboolean _brush_get_source_center(const dt_masks_form_gui_points_t *gui_points, const int node_count,
                                         dt_masks_gui_center_point_t *center_pt)
{
  if(!gui_points || !center_pt) return FALSE;

  // Work on the exact centerline span that is actually drawn (non-border path):
  // [node_count * 3, 0.5 * points_count)
  const int line_offset_pt = node_count * 3;
  const int points_line_end = gui_points->points_count / 2; // exclusive
  const int source_line_end = gui_points->source_count / 2; // exclusive
  const int line_end = MIN(points_line_end, source_line_end);
  const int line_count = line_end - line_offset_pt;
  if(line_count < 2) return FALSE;

  const float *const points_line = gui_points->points + 2 * line_offset_pt;
  const float *const source_line = gui_points->source + 2 * line_offset_pt;
  const int first_pt = 0;
  const int last_pt = line_count - 1;

  if(!_brush_get_line_midpoint(points_line, first_pt, last_pt,
                               &center_pt->main.x, &center_pt->main.y))
    return FALSE;
  if(!_brush_get_line_midpoint(source_line, first_pt, last_pt,
                               &center_pt->source.x, &center_pt->source.y))
    return FALSE;
  return TRUE;
}

static void _brush_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *mask_gui, int index,
                                      int node_count)
{
  if(!mask_gui) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, index);
  if(!gui_points) return;

  // in creation mode
  if(mask_gui->creation)
  {
    const float iwd = darktable.develop->roi.raw_width;
    const float iht = darktable.develop->roi.raw_height;
    const float min_iwd_iht = MIN(iwd, iht);

    if(mask_gui->guipoints_count == 0)
    {
      dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
      if(!mask_form) return;

      const float masks_border = dt_masks_get_set_conf_value(mask_form, "border", 1.0f, BORDER_MIN, BORDER_MAX,
                                                             DT_MASKS_INCREMENT_SCALE, 1);
      const float masks_hardness = dt_masks_get_set_conf_value(mask_form, "hardness", 1.0f, HARDNESS_MIN,
                                                               HARDNESS_MAX, DT_MASKS_INCREMENT_SCALE, 1);
      const float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");

      const float radius1 = masks_border * masks_hardness * min_iwd_iht;
      const float radius2 = masks_border * min_iwd_iht;

      float xpos = mask_gui->pos[0];
      float ypos = mask_gui->pos[1];
      if((xpos == -1.0f && ypos == -1.0f) || mask_gui->mouse_leaved_center)
      {
        xpos = 0.f;
        ypos = 0.f;
      }

      // draw brush circle at current mouse position
      cairo_save(cr);
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, opacity);
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE / zoom_scale);
      cairo_arc(cr, xpos, ypos, radius1, 0, 2.0 * M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      cairo_arc(cr, xpos, ypos, radius2, 0, 2.0 * M_PI);
      dt_draw_stroke_line(DT_MASKS_DASH_STICK, FALSE, cr, FALSE, zoom_scale, CAIRO_LINE_CAP_ROUND);

      if(mask_form->type & DT_MASKS_CLONE)
      {
        float x = 0.0f, y = 0.0f;
        dt_masks_calculate_source_pos_value(mask_gui, xpos, ypos,
                                            xpos, ypos, &x, &y, FALSE);
        dt_draw_cross(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    else
    {
      float masks_border = 0.0f, masks_hardness = 0.0f, masks_density = 0.0f;
      float radius = 0.0f, oldradius = 0.0f, opacity = 0.0f, oldopacity = 0.0f, pressure = 0.0f;
      int stroked = 1;

      const float *guipoints = dt_masks_dynbuf_buffer(mask_gui->guipoints);
      const float *guipoints_payload = dt_masks_dynbuf_buffer(mask_gui->guipoints_payload);

      cairo_save(cr);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      masks_border = guipoints_payload[0];
      masks_hardness = guipoints_payload[1];
      masks_density = guipoints_payload[2];
      pressure = guipoints_payload[3];

      switch(mask_gui->pressure_sensitivity)
      {
        case DT_MASKS_PRESSURE_HARDNESS_ABS:
          masks_hardness = MAX(HARDNESS_MIN, pressure);
          break;
        case DT_MASKS_PRESSURE_HARDNESS_REL:
          masks_hardness = MAX(HARDNESS_MIN, masks_hardness * pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_ABS:
          masks_density = MAX(0.05f, pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_REL:
          masks_density = MAX(0.05f, masks_density * pressure);
          break;
        case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
          masks_border = MAX(HARDNESS_MIN, masks_border * pressure);
          break;
        default:
        case DT_MASKS_PRESSURE_OFF:
          // ignore pressure value
          break;
      }

      radius = oldradius = masks_border * masks_hardness * min_iwd_iht;
      opacity = oldopacity = masks_density;

      cairo_set_line_width(cr,  DT_PIXEL_APPLY_DPI(2 * radius));
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_TRACE, opacity);

      cairo_move_to(cr, guipoints[0], guipoints[1]);
      for(int i = 1; i < mask_gui->guipoints_count; i++)
      {
        cairo_line_to(cr, guipoints[i * 2], guipoints[i * 2 + 1]);
        stroked = 0;
        masks_border = guipoints_payload[i * 4];
        masks_hardness = guipoints_payload[i * 4 + 1];
        masks_density = guipoints_payload[i * 4 + 2];
        pressure = guipoints_payload[i * 4 + 3];

        switch(mask_gui->pressure_sensitivity)
        {
          case DT_MASKS_PRESSURE_HARDNESS_ABS:
            masks_hardness = MAX(HARDNESS_MIN, pressure);
            break;
          case DT_MASKS_PRESSURE_HARDNESS_REL:
            masks_hardness = MAX(HARDNESS_MIN, masks_hardness * pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_ABS:
            masks_density = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_REL:
            masks_density = MAX(0.05f, masks_density * pressure);
            break;
          case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
            masks_border = MAX(HARDNESS_MIN, masks_border * pressure);
            break;
          default:
          case DT_MASKS_PRESSURE_OFF:
            // ignore pressure value
            break;
        }

        radius = masks_border * masks_hardness * min_iwd_iht;
        opacity = masks_density;

        if(radius != oldradius || opacity != oldopacity)
        {
          cairo_stroke(cr);
          stroked = 1;
          cairo_set_line_width(cr,  DT_PIXEL_APPLY_DPI(2 * radius));
          dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_TRACE, opacity);
          oldradius = radius;
          oldopacity = opacity;
          cairo_move_to(cr, guipoints[i * 2], guipoints[i * 2 + 1]);
        }
      }
      if(!stroked) cairo_stroke(cr);

      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE / zoom_scale);
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, opacity);
      cairo_arc(cr, guipoints[2 * (mask_gui->guipoints_count - 1)],
                guipoints[2 * (mask_gui->guipoints_count - 1) + 1],
                radius, 0, 2.0 * M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      dt_draw_set_dash_style(cr, DT_MASKS_DASH_STICK, zoom_scale);
      cairo_arc(cr, guipoints[2 * (mask_gui->guipoints_count - 1)],
                guipoints[2 * (mask_gui->guipoints_count - 1) + 1], masks_border * min_iwd_iht, 0,
                2.0 * M_PI);
      cairo_stroke(cr);

      dt_masks_form_t *visible_form = dt_masks_get_visible_form(darktable.develop);
      if(visible_form && (visible_form->type & DT_MASKS_CLONE))
      {
        const int i = mask_gui->guipoints_count - 1;
        float x = 0.0f, y = 0.0f;
        dt_masks_calculate_source_pos_value(mask_gui, guipoints[0], guipoints[1], guipoints[i * 2],
                                            guipoints[i * 2 + 1], &x, &y, TRUE);
        dt_draw_cross(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    return;
  } // creation

  // minimum points
  if(gui_points->points_count <= node_count * 3 + 2) return;

  // draw path
  if(node_count > 0 && gui_points->points_count > node_count * 3 + 6) // there must be something to draw
  {
    dt_masks_draw_path_seg_by_seg(cr, mask_gui, index, gui_points->points, 0.5f * gui_points->points_count,
                                  node_count, zoom_scale);
  }

  if(0)
  {
    const int total_points = gui_points->points_count * 2;
    int seg1 = 1;
    int current_seg = 0;

    /* Draw the line point-by-point up to the next node, then stroke it; repeat in a loop. */
    cairo_move_to(cr, gui_points->points[node_count * 6], gui_points->points[node_count * 6 + 1]);

    const int end_idx = 0.5 * gui_points->points_count;

    for(int i = node_count * 3; i < end_idx; i++)
    {
      const double x = gui_points->points[i * 2];
      const double y = gui_points->points[i * 2 + 1];
      cairo_line_to(cr, x, y);

      int seg_idx = seg1 * 6;
      if((seg_idx + 3) < total_points)
      {
        const double segment_x = gui_points->points[seg_idx + 2];
        const double segment_y = gui_points->points[seg_idx + 3];

        /* Is this point the next node? */
        if(x == segment_x && y == segment_y)
        {
          const gboolean seg_selected = (mask_gui->group_selected == index) && (mask_gui->seg_selected == current_seg);
          const gboolean all_selected = (mask_gui->group_selected == index) && mask_gui->node_selected == -1
                                        && (mask_gui->form_selected || mask_gui->form_dragging);
          // creation mode: draw the current segment as round dotted line
          if(mask_gui->creation && current_seg == node_count - 2)
            dt_draw_stroke_line(DT_MASKS_DASH_ROUND, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);
          else
            dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, (seg_selected || all_selected), zoom_scale, CAIRO_LINE_CAP_BUTT);
          seg1 = (seg1 + 1) % node_count;
          current_seg++;
        }
      }
    }
  }

  // draw nodes and attached stuff
  if(mask_gui->group_selected == index)
  {
    // draw borders
    if(gui_points->border_count > node_count * 3 + 2)
    {
      dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, node_count, (mask_gui->border_selected), zoom_scale,
                          gui_points->border, gui_points->border_count, &dt_masks_functions_brush.draw_shape,
                          CAIRO_LINE_CAP_ROUND);
    }

    // draw the current node's handle if it's a curve node
    if(mask_gui->node_selected >= 0 && !dt_masks_node_is_cusp(gui_points, mask_gui->node_selected))
    {
      const int n = mask_gui->node_selected;
      float handle[2];
      _brush_ctrl2_to_handle(gui_points->points[n * 6 + 2], gui_points->points[n * 6 + 3],
                             gui_points->points[n * 6 + 4], gui_points->points[n * 6 + 5], &handle[0], &handle[1],
                             TRUE);
      const float pt[2] = { gui_points->points[n * 6 + 2], gui_points->points[n * 6 + 3] };
      const gboolean selected = (mask_gui->node_hovered == n || mask_gui->handle_selected == n);
      dt_draw_handle(cr, pt, zoom_scale, handle, selected, FALSE);
    }

    // draw all nodes
    for(int k = 0; k < node_count; k++)
    {
      const gboolean corner = dt_masks_node_is_cusp(gui_points, k);
      const float x = gui_points->points[k * 6 + 2];
      const float y = gui_points->points[k * 6 + 3];
      const gboolean selected = (k == mask_gui->node_hovered || k == mask_gui->node_dragging);
      const gboolean action = (k == mask_gui->node_selected);

      dt_draw_node(cr, corner, action, selected, zoom_scale, x, y);
    }

    // Draw the current node's border handle, if needed
    if(mask_gui->node_selected >= 0)
    {
      const int edited = mask_gui->node_selected;
      const gboolean selected = (mask_gui->node_hovered == edited
                              || mask_gui->handle_border_selected == edited);
      float handle[2] = {NAN, NAN};
      // Show the border handle on the opposite side from the curve handle
      if(_brush_get_border_handle_mirrored(gui_points, node_count, edited, &handle[0], &handle[1]))
      {
        dt_draw_handle(cr, NULL, zoom_scale, handle, selected, TRUE);
      }
    }
  }

  // Draw the source if needed
  if(gui_points->source_count > node_count * 3 + 2)
  {
    dt_masks_gui_center_point_t center_pt;
    if(_brush_get_source_center(gui_points, node_count, &center_pt))
      dt_masks_draw_source(cr, mask_gui, index, node_count, zoom_scale, &center_pt,
                           &dt_masks_functions_brush.draw_shape);

    //draw the current node projection
    for(int k = 0; k < node_count; k++)
    {
      if(mask_gui->group_selected == index
         && (k == mask_gui->node_hovered || k == mask_gui->node_selected
             || (mask_gui->creation && k == node_count - 1)))
      {
        const int node_index = k * 6 + 2;
        const float proj[2] = { gui_points->source[node_index], gui_points->source[node_index + 1] };
        const gboolean selected = mask_gui->node_hovered == k;
        const gboolean squared = dt_masks_node_is_cusp(gui_points, k);

        dt_draw_handle(cr, NULL, zoom_scale, proj, selected, squared);
      }
    }
  }
}

/**
 * @brief Compute the bounding box (min/max) for both brush centerline and border points.
 *
 * Assumes point arrays contain interleaved x/y coordinates and that indices before
 * `node_count * 3` are control points which should be ignored for bounds.
 */
static void _brush_bounding_box_raw(const float *const restrict points, const float *const restrict border,
                                    const int node_count, const int total_points, float *x_min, float *x_max,
                                    float *y_min, float *y_max)
{
  // now we want to find the area, so we search min/max points
  float min_x = FLT_MAX, max_x = FLT_MIN, min_y = FLT_MAX, max_y = FLT_MIN;
#ifdef _OPENMP
#pragma omp parallel for reduction(min : min_x, min_y) reduction(max : max_x, max_y) \
  schedule(static) if(total_points > 1000)
#endif
  // Skip control points and only consider actual polyline samples.
  for(int point_index = node_count * 3; point_index < total_points; point_index++)
  {
    // we look at the borders
    const float border_x = border[point_index * 2];
    const float border_y = border[point_index * 2 + 1];
    min_x = MIN(border_x, min_x);
    max_x = MAX(border_x, max_x);
    min_y = MIN(border_y, min_y);
    max_y = MAX(border_y, max_y);
    // we look at the brush too
    const float brush_x = points[point_index * 2];
    const float brush_y = points[point_index * 2 + 1];
    min_x = MIN(brush_x, min_x);
    max_x = MAX(brush_x, max_x);
    min_y = MIN(brush_y, min_y);
    max_y = MAX(brush_y, max_y);
  }
  *x_min = min_x;
  *x_max = max_x;
  *y_min = min_y;
  *y_max = max_y;
}

/**
 * @brief Compute integer bounds used for buffer allocation.
 *
 * Adds a 2-pixel padding on each side to avoid rounding gaps.
 */
static void _brush_bounding_box(const float *const points, const float *const border, const int node_count,
                                const int total_points, int *width, int *height, int *offset_x, int *offset_y)
{
  float min_x = FLT_MAX, max_x = FLT_MIN, min_y = FLT_MAX, max_y = FLT_MIN;
  _brush_bounding_box_raw(points, border, node_count, total_points, &min_x, &max_x, &min_y, &max_y);
  *height = max_y - min_y + 4;
  *width = max_x - min_x + 4;
  *offset_x = min_x - 2;
  *offset_y = min_y - 2;
}

/**
 * @brief Compute the minimal bounding box for a brush (optionally including the source path).
 *
 * Used by both ROI and full-frame paths to size temporary buffers.
 */
static int _get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const mask_form, int *width, int *height, int *offset_x, int *offset_y,
                     int include_source)
{
  if(!module) return 1;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(_brush_get_pts_border(module->dev, mask_form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,
                           &points, &points_count, &border, &border_count, NULL, NULL, include_source) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 1;
  }

  const guint node_count = g_list_length(mask_form->points);
  _brush_bounding_box(points, border, node_count, points_count, width, height, offset_x, offset_y);

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  return 0;
}

static int _brush_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                  dt_masks_form_t *mask_form, int *width, int *height, int *offset_x, int *offset_y)
{
  return _get_area(module, piece, mask_form, width, height, offset_x, offset_y, 1);
}

static int _brush_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const mask_form, int *width, int *height, int *offset_x,
                           int *offset_y)
{
  return _get_area(module, piece, mask_form, width, height, offset_x, offset_y, 0);
}

/** we write a falloff segment */
static void _brush_falloff(float *const restrict buffer, int segment_start[2], int segment_end[2],
                           int offset_x, int offset_y, int buffer_width, float hardness, float density)
{
  // segment length
  const int segment_length = sqrt((segment_end[0] - segment_start[0]) * (segment_end[0] - segment_start[0])
                                  + (segment_end[1] - segment_start[1]) * (segment_end[1] - segment_start[1]))
                             + 1;
  const int solid_length = (int)segment_length * hardness;
  const int soft_length = segment_length - solid_length;

  const float segment_dx = segment_end[0] - segment_start[0];
  const float segment_dy = segment_end[1] - segment_start[1];
  const float inv_length = 1.0f / (float)segment_length;
  const float inv_soft = (soft_length > 0) ? 1.0f / (float)soft_length : 0.0f;

  for(int step = 0; step < segment_length; step++)
  {
    // position
    const int x = (int)((float)step * segment_dx * inv_length) + segment_start[0] - offset_x;
    const int y = (int)((float)step * segment_dy * inv_length) + segment_start[1] - offset_y;
    const float opacity = density * ((step <= solid_length) ? 1.0f : 1.0f - (float)(step - solid_length) * inv_soft);
    const int buffer_index = y * buffer_width + x;
    buffer[buffer_index] = MAX(buffer[buffer_index], opacity);
    if(x > 0)
      buffer[buffer_index - 1] = MAX(buffer[buffer_index - 1], opacity); // avoid gaps from rounding
    if(y > 0)
      buffer[buffer_index - buffer_width] = MAX(buffer[buffer_index - buffer_width], opacity); // avoid gaps from rounding
  }
}

/**
 * @brief Build a full-resolution brush mask into a newly allocated buffer.
 *
 * The buffer is returned zero-initialized and filled only in the falloff region.
 */
static int _brush_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const mask_form,
                           float **buffer, int *width, int *height, int *offset_x, int *offset_y)
{
  if(!module) return 1;
  double timer_start = 0.0;
  double timer_step_start = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) timer_start = timer_step_start = dt_get_wtime();

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;
  int points_count, border_count, payload_count;
  if(_brush_get_pts_border(module->dev, mask_form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,
                           &points, &points_count,
                               &border, &border_count, &payload, &payload_count, 0) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    dt_pixelpipe_cache_free_align(payload);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_step_start);
    timer_step_start = dt_get_wtime();
  }

  const guint node_count = g_list_length(mask_form->points);
  const gboolean use_sparse = (piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                               || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL);
  const int sparse_step = use_sparse ? 4 : 1;
  _brush_bounding_box(points, border, node_count, points_count, width, height, offset_x, offset_y);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_step_start);

  // we allocate the buffer
  const size_t buffer_size = (size_t)(*width) * (*height);
  // ensure that the buffer is zeroed, as the below code only fills in pixels in the falloff region
  *buffer = dt_pixelpipe_cache_alloc_align_float_cache(buffer_size, 0);
  if(*buffer == NULL)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    dt_pixelpipe_cache_free_align(payload);
    return 1;
  }
  memset(*buffer, 0, sizeof(float) * buffer_size);

  // now we fill the falloff
  int segment_start[2], segment_end[2];
  int prev_start[2] = { 0, 0 };
  int prev_end[2] = { 0, 0 };
  float prev_payload[2] = { 0.0f, 0.0f };
  gboolean have_prev = FALSE;

  // Walk all border samples and stamp a falloff segment for each.
  for(int border_index = node_count * 3; border_index < border_count; border_index++)
  {
    segment_start[0] = points[border_index * 2];
    segment_start[1] = points[border_index * 2 + 1];
    segment_end[0] = border[border_index * 2];
    segment_end[1] = border[border_index * 2 + 1];

    if(use_sparse && have_prev
       && (prev_start[0] != segment_start[0] || prev_start[1] != segment_start[1]
           || prev_end[0] != segment_end[0] || prev_end[1] != segment_end[1]))
    {
      // In sparse mode (preview/thumbnail), interpolate missing segments to reduce holes.
      for(int step = 1; step < sparse_step; step++)
      {
        const float t = (float)step / (float)sparse_step;
        int interp_start[2] = { (int)floorf(prev_start[0] + t * (segment_start[0] - prev_start[0]) + 0.5f),
                                (int)floorf(prev_start[1] + t * (segment_start[1] - prev_start[1]) + 0.5f) };
        int interp_end[2] = { (int)floorf(prev_end[0] + t * (segment_end[0] - prev_end[0]) + 0.5f),
                              (int)floorf(prev_end[1] + t * (segment_end[1] - prev_end[1]) + 0.5f) };
        const float hard = prev_payload[0] + t * (payload[border_index * 2] - prev_payload[0]);
        const float dens = prev_payload[1] + t * (payload[border_index * 2 + 1] - prev_payload[1]);
        _brush_falloff(*buffer, interp_start, interp_end, *offset_x, *offset_y, *width, hard, dens);
      }
    }

    _brush_falloff(*buffer, segment_start, segment_end, *offset_x, *offset_y, *width,
                   payload[border_index * 2], payload[border_index * 2 + 1]);

    prev_start[0] = segment_start[0];
    prev_start[1] = segment_start[1];
    prev_end[0] = segment_end[0];
    prev_end[1] = segment_end[1];
    prev_payload[0] = payload[border_index * 2];
    prev_payload[1] = payload[border_index * 2 + 1];
    have_prev = TRUE;
  }

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  dt_pixelpipe_cache_free_align(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_start);

  return 0;
}

/** we write a falloff segment respecting limits of buffer */
static inline void _brush_falloff_roi(float *buffer, const int *segment_start, const int *segment_end,
                                      int buffer_width, int buffer_height, float hardness, float density)
{
  // segment length (increase by 1 to avoid division-by-zero special case handling)
  const int segment_length = sqrt((segment_end[0] - segment_start[0]) * (segment_end[0] - segment_start[0])
                                  + (segment_end[1] - segment_start[1]) * (segment_end[1] - segment_start[1]))
                             + 1;
  const int solid_length = hardness * segment_length;

  const float step_x = (float)(segment_end[0] - segment_start[0]) / (float)segment_length;
  const float step_y = (float)(segment_end[1] - segment_start[1]) / (float)segment_length;

  const int direction_x = step_x <= 0 ? -1 : 1;
  const int direction_y = step_y <= 0 ? -1 : 1;
  const int neighbor_offset_x = direction_x;
  const int neighbor_offset_y = direction_y * buffer_width;

  float cursor_x = segment_start[0];
  float cursor_y = segment_start[1];

  float opacity = density;
  const float opacity_step = density / (float)(segment_length - solid_length);

  const int start_x = segment_start[0], start_y = segment_start[1];
  const int end_x = segment_end[0], end_y = segment_end[1];
  if((start_x < 0 && end_x < 0) || (start_x >= buffer_width && end_x >= buffer_width)
     || (start_y < 0 && end_y < 0) || (start_y >= buffer_height && end_y >= buffer_height))
    return;
  const int fully_inside = (start_x >= 0 && start_x < buffer_width && end_x >= 0 && end_x < buffer_width
                            && start_y >= 0 && start_y < buffer_height && end_y >= 0
                            && end_y < buffer_height);

  for(int step = 0; step < segment_length; step++)
  {
    const int x = cursor_x;
    const int y = cursor_y;

    cursor_x += step_x;
    cursor_y += step_y;
    if(step > solid_length) opacity -= opacity_step;

    if(!fully_inside && (x < 0 || x >= buffer_width || y < 0 || y >= buffer_height)) continue;

    float *buf = buffer + (size_t)y * buffer_width + x;

    *buf = MAX(*buf, opacity);
    if(x + direction_x >= 0 && x + direction_x < buffer_width)
      buf[neighbor_offset_x] = MAX(buf[neighbor_offset_x], opacity); // avoid gaps from rounding
    if(y + direction_y >= 0 && y + direction_y < buffer_height)
      buf[neighbor_offset_y] = MAX(buf[neighbor_offset_y], opacity); // avoid gaps from rounding
  }
}

// build a stamp which can be combined with other shapes in the same group
// prerequisite: 'buffer' is all zeros
/**
 * @brief Build a brush mask directly into an ROI-sized buffer.
 *
 * The buffer is assumed pre-zeroed. Points are scaled/shifted into ROI space before stamping.
 */
static int _brush_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                               dt_masks_form_t *const mask_form, const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 1;
  double timer_start = 0.0;
  double timer_step_start = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) timer_start = timer_step_start = dt_get_wtime();

  const int roi_offset_x = roi->x;
  const int roi_offset_y = roi->y;
  const int roi_width = roi->width;
  const int roi_height = roi->height;
  const float roi_scale = roi->scale;
  const gboolean use_sparse = (piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                               || piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL);
  const int sparse_step = use_sparse ? 4 : 1;

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;

  int points_count, border_count, payload_count;

  if(_brush_get_pts_border(module->dev, mask_form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,
                           &points, &points_count, &border, &border_count, &payload, &payload_count, 0) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    dt_pixelpipe_cache_free_align(payload);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_step_start);
    timer_step_start = dt_get_wtime();
  }

  const guint node_count = g_list_length(mask_form->points);

  // we shift and scale down brush and border
  // Shift/scale border samples into ROI space.
  for(int border_index = node_count * 3; border_index < border_count; border_index++)
  {
    const float border_x = border[2 * border_index];
    const float border_y = border[2 * border_index + 1];
    border[2 * border_index] = border_x * roi_scale - roi_offset_x;
    border[2 * border_index + 1] = border_y * roi_scale - roi_offset_y;
  }

  // Shift/scale centerline samples into ROI space.
  for(int point_index = node_count * 3; point_index < points_count; point_index++)
  {
    const float point_x = points[2 * point_index];
    const float point_y = points[2 * point_index + 1];
    points[2 * point_index] = point_x * roi_scale - roi_offset_x;
    points[2 * point_index + 1] = point_y * roi_scale - roi_offset_y;
  }


  float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
  _brush_bounding_box_raw(points, border, node_count, points_count, &min_x, &max_x, &min_y, &max_y);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_step_start);
    timer_step_start = dt_get_wtime();
  }

  // check if the path completely lies outside of roi -> we're done/mask remains empty
  if(max_x < 0 || max_y < 0 || min_x >= roi_width || min_y >= roi_height)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    dt_pixelpipe_cache_free_align(payload);
    return 0;
  }

  // now we fill the falloff
  if(!use_sparse)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(node_count, border_count, roi_width, roi_height, buffer, points, border, payload) \
  if(border_count - node_count * 3 > 1000)
#endif
    // Stamp each border segment directly (full sampling).
    for(int border_index = node_count * 3; border_index < border_count; border_index++)
    {
      const int segment_start[] = { points[border_index * 2], points[border_index * 2 + 1] };
      const int segment_end[] = { border[border_index * 2], border[border_index * 2 + 1] };

      if(MAX(segment_start[0], segment_end[0]) < 0 || MIN(segment_start[0], segment_end[0]) >= roi_width
         || MAX(segment_start[1], segment_end[1]) < 0
         || MIN(segment_start[1], segment_end[1]) >= roi_height)
        continue;

      _brush_falloff_roi(buffer, segment_start, segment_end, roi_width, roi_height, payload[border_index * 2],
                         payload[border_index * 2 + 1]);
    }
  }
  else
  {
    int prev_start[2] = { 0, 0 };
    int prev_end[2] = { 0, 0 };
    float prev_payload[2] = { 0.0f, 0.0f };
    gboolean have_prev = FALSE;

    // Sparse mode: interpolate between consecutive segments to reduce artifacts.
    for(int border_index = node_count * 3; border_index < border_count; border_index++)
    {
      int segment_start[2] = { points[border_index * 2], points[border_index * 2 + 1] };
      int segment_end[2] = { border[border_index * 2], border[border_index * 2 + 1] };

      if(use_sparse && have_prev
         && (prev_start[0] != segment_start[0] || prev_start[1] != segment_start[1]
             || prev_end[0] != segment_end[0] || prev_end[1] != segment_end[1]))
      {
        for(int step = 1; step < sparse_step; step++)
        {
          const float t = (float)step / (float)sparse_step;
          int interp_start[2] = { (int)floorf(prev_start[0] + t * (segment_start[0] - prev_start[0]) + 0.5f),
                                  (int)floorf(prev_start[1] + t * (segment_start[1] - prev_start[1]) + 0.5f) };
          int interp_end[2] = { (int)floorf(prev_end[0] + t * (segment_end[0] - prev_end[0]) + 0.5f),
                                (int)floorf(prev_end[1] + t * (segment_end[1] - prev_end[1]) + 0.5f) };
          if(!(MAX(interp_start[0], interp_end[0]) < 0 || MIN(interp_start[0], interp_end[0]) >= roi_width
               || MAX(interp_start[1], interp_end[1]) < 0 || MIN(interp_start[1], interp_end[1]) >= roi_height))
          {
            const float hard = prev_payload[0] + t * (payload[border_index * 2] - prev_payload[0]);
            const float dens = prev_payload[1] + t * (payload[border_index * 2 + 1] - prev_payload[1]);
            _brush_falloff_roi(buffer, interp_start, interp_end, roi_width, roi_height, hard, dens);
          }
        }
      }

      if(!(MAX(segment_start[0], segment_end[0]) < 0 || MIN(segment_start[0], segment_end[0]) >= roi_width
           || MAX(segment_start[1], segment_end[1]) < 0
           || MIN(segment_start[1], segment_end[1]) >= roi_height))
        _brush_falloff_roi(buffer, segment_start, segment_end, roi_width, roi_height,
                           payload[border_index * 2], payload[border_index * 2 + 1]);

      prev_start[0] = segment_start[0];
      prev_start[1] = segment_start[1];
      prev_end[0] = segment_end[0];
      prev_end[1] = segment_end[1];
      prev_payload[0] = payload[border_index * 2];
      prev_payload[1] = payload[border_index * 2 + 1];
      have_prev = TRUE;
    }
  }

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  dt_pixelpipe_cache_free_align(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush set falloff took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_step_start);
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", mask_form->name,
             dt_get_wtime() - timer_start);
  }

  return 0;
}

static void _brush_sanitize_config(dt_masks_type_t type)
{
  // nothing to do (yet?)
}

static void _brush_set_form_name(struct dt_masks_form_t *const mask_form, const size_t form_number)
{
  snprintf(mask_form->name, sizeof(mask_form->name), _("brush #%d"), (int)form_number);
}

static void _brush_set_hint_message(const dt_masks_form_gui_t *const mask_gui,
                                    const dt_masks_form_t *const mask_form,
                                    const int opacity, char *const restrict msgbuf,
                                    const size_t msgbuf_len)
{
  if(mask_gui->creation || mask_gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len,
               _("<b>Size</b>: scroll, <b>Hardness</b>: shift+scroll\n"
                 "<b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
  else if(mask_gui->border_selected)
    g_strlcat(msgbuf, _("<b>Size</b>: scroll"), msgbuf_len);
}

static void _brush_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  dt_masks_duplicate_points(base, dest, sizeof(dt_masks_node_brush_t));
}

static void _brush_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  *x = 0.01f * iwd;
  *y = 0.01f * iht;
}

static void _brush_switch_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(!mask_gui) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  mask_gui->node_selected = mask_gui->node_hovered;

  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, form_id);
  if(!selected_form) return;
  _change_node_type(module, selected_form, mask_gui, mask_gui->group_selected);
}

static void _brush_reset_round_node_callback(GtkWidget *widget, gpointer user_data)
{
  dt_masks_form_gui_t *mask_gui = (dt_masks_form_gui_t *)user_data;
  if(!mask_gui) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  mask_gui->node_selected = mask_gui->node_hovered;

  const int form_id = mask_gui->formid;
  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, form_id);
  if(!selected_form) return;

  _reset_ctrl_points(module, selected_form, mask_gui, mask_gui->group_selected);
}

static void _brush_add_node_callback(GtkWidget *menu, gpointer user_data)
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
    _add_node_to_segment(module, mask_gui->pos[0], mask_gui->pos[1], selected_form,
                         group_entry->parentid, mask_gui, mask_gui->group_selected);
    
  dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
}

static int _brush_populate_context_menu(GtkWidget *menu, struct dt_masks_form_t *mask_form,
                                        struct dt_masks_form_gui_t *mask_gui,
                                        const float pointer_x, const float pointer_y)
{
  GtkWidget *menu_item = NULL;
  gchar *accel = g_strdup_printf(_("%s+Click"), gtk_accelerator_get_label(0, GDK_CONTROL_MASK));

  gboolean ret = FALSE;

  if(mask_gui->node_hovered >= 0)
  {
    dt_masks_form_gui_points_t *gui_points
        = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, mask_gui->group_selected);
    if(!gui_points) goto end;
    dt_masks_node_brush_t *node
        = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, mask_gui->node_hovered);
    if(!node) goto end;
    const gboolean is_corner = dt_masks_node_is_cusp(gui_points, mask_gui->node_hovered);

    {
      gchar *to_change_type = g_strdup_printf(_("Switch to %s node"), (is_corner) ? _("round") : _("cusp"));
      const dt_menu_icon_t icon = is_corner ? DT_MENU_ICON_CIRCLE : DT_MENU_ICON_SQUARE;
      menu_item = ctx_gtk_menu_item_new_with_icon_and_shortcut(to_change_type, accel, menu,
                                                               _brush_switch_node_callback, mask_gui, icon);
      g_free(to_change_type);
    }

    {
      menu_item = ctx_gtk_menu_item_new_with_markup(_("Reset round node"), menu,
                                                    _brush_reset_round_node_callback, mask_gui);
      gtk_widget_set_sensitive(menu_item, !is_corner && node->state != DT_MASKS_POINT_STATE_NORMAL);
    }
    ret = TRUE;
  }

  if(mask_gui->seg_selected >= 0)
  {
    mask_gui->pos[0] = pointer_x;
    mask_gui->pos[1] = pointer_y;
    menu_item = ctx_gtk_menu_item_new_with_markup_and_shortcut(_("Add a node here"), accel,
                                                               menu, _brush_add_node_callback, mask_gui);
    ret = TRUE;
  }

  end:
  g_free(accel);
  return ret;
}

// The function table for brushes.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_brush = {
  .point_struct_size = sizeof(struct dt_masks_node_brush_t),
  .sanitize_config = _brush_sanitize_config,
  .set_form_name = _brush_set_form_name,
  .set_hint_message = _brush_set_hint_message,
  .duplicate_points = _brush_duplicate_points,
  .initial_source_pos = _brush_initial_source_pos,
  .get_distance = _brush_get_distance,
  .get_points_border = _brush_get_points_border,
  .get_mask = _brush_get_mask,
  .get_mask_roi = _brush_get_mask_roi,
  .get_area = _brush_get_area,
  .get_source_area = _brush_get_source_area,
  .get_gravity_center = _brush_get_gravity_center,
  .get_interaction_value = _brush_get_interaction_value,
  .set_interaction_value = _brush_set_interaction_value,
  .mouse_moved = _brush_events_mouse_moved,
  .mouse_scrolled = _brush_events_mouse_scrolled,
  .button_pressed = _brush_events_button_pressed,
  .button_released = _brush_events_button_released,
  .key_pressed = _brush_events_key_pressed,
  .post_expose = _brush_events_post_expose,
  .draw_shape = _brush_draw_shape,
  .init_ctrl_points = _brush_init_ctrl_points,
  .populate_context_menu = _brush_populate_context_menu

};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
