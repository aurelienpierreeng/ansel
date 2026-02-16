/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include <assert.h>

#define HARDNESS_MIN 0.0005f
#define HARDNESS_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

static void _polygon_bounding_box_raw(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                                   float *x_min, float *x_max, float *y_min, float *y_max);

/** get the point of the polygon at pos t [0,1]  */
static void _polygon_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                         float p3y, float t, float *x, float *y)
{
  const float ti = 1.0f - t;
  const float a = ti*ti*ti;
  const float b = 3.0f * t * ti*ti;
  const float c = 3.0f * t*t * ti;
  const float d = t*t*t;
  *x = p0x * a + p1x * b + p2x * c + p3x * d;
  *y = p0y * a + p1y * b + p2y * c + p3y * d;
}

/** get the point of the polygon at pos t [0,1]  AND the corresponding border point */
static void _polygon_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                                float p3y, float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  // we get the point
  _polygon_get_XY(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, xc, yc);

  // now we get derivative points
  const double ti = 1.0 - (double)t;

  const double t_t = (double)t * t;
  const double ti_ti = ti * ti;
  const double t_ti = t * ti;

  const double a = 3.0 * ti_ti;
  const double b = 3.0 * (ti_ti - 2.0 * t_ti);
  const double c = 3.0 * (2.0 * t_ti - t_t);
  const double d = 3.0 * t_t;

  const double dx = -p0x * a + p1x * b + p2x * c + p3x * d;
  const double dy = -p0y * a + p1y * b + p2y * c + p3y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *xb = NAN;
    *yb = NAN;
    return;
  }
  const double l = 1.0 / sqrt(dx * dx + dy * dy);
  *xb = (*xc) + rad * dy * l;
  *yb = (*yc) - rad * dx * l;
}

/** get handle extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _polygon_ctrl2_to_handle(const float ptx, const float pty, const float ctrlx, const float ctrly,
                              float *fx, float *fy, const gboolean clockwise)
{
  const float dy = ctrly - pty;
  const float dx = ptx - ctrlx;
  if(clockwise)
  {
    *fx = ptx - dy;
    *fy = pty - dx;
  }
  else
  {
    *fx = ptx + dy;
    *fy = pty + dx;
  }
}

/** get bezier control points from handle extremity */
/** the values should be in orthonormal space */
static void _polygon_handle_to_ctrl(float ptx, float pty, float fx, float fy,
                                  float *ctrl1x, float *ctrl1y, float *ctrl2x, float *ctrl2y,
                                  gboolean clockwise)
{
  const float dy = fy - pty;
  const float dx = ptx - fx;

  if(clockwise)
  {
    *ctrl1x = ptx - dy;
    *ctrl1y = pty - dx;
    *ctrl2x = ptx + dy;
    *ctrl2y = pty + dx;
  }
  else
  {
    *ctrl1x = ptx + dy;
    *ctrl1y = pty + dx;
    *ctrl2x = ptx - dy;
    *ctrl2y = pty - dx;
  }
}

/** Get the control points of a segment to match exactly a catmull-rom spline */
static void _polygon_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                    float y4, float *bx1, float *by1, float *bx2, float *by2)
{
  *bx1 = (-x1 + 6 * x2 + x3) / 6;
  *by1 = (-y1 + 6 * y2 + y3) / 6;
  *bx2 = (x2 + 6 * x3 - x4) / 6;
  *by2 = (y2 + 6 * y3 - y4) / 6;
}

/** initialise all control points to eventually match a catmull-rom like spline */
static void _polygon_init_ctrl_points(dt_masks_form_t *form)
{
  // if we have less that 3 points, what to do ??
  const guint nb = g_list_length(form->points);
  if(nb < 2) return;

  if(!form || !form->points) return;
  
  const GList *form_points = form->points;
  for(int k = 0; k < nb; k++)
  {
    dt_masks_node_polygon_t *point3 = (dt_masks_node_polygon_t *)form_points->data;
    if(!point3) return;
    // if the point has not been set manually, we redefine it
    if(point3->state == DT_MASKS_POINT_STATE_NORMAL)
    {
      // we want to get point-2 (into pt1), point-1 (into pt2), point+1 (into pt4), point+2 (into pt5), wrapping
      // around to the other end of the list
      const GList *pt2 = g_list_prev_wraparound(form_points); // prev, wrapping around if already on first element
      const GList *pt1 = g_list_prev_wraparound(pt2);
      const GList *pt4 = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last element
      const GList *pt5 = g_list_next_wraparound(pt4, form->points);
      dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)pt1->data;
      dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)pt2->data;
      dt_masks_node_polygon_t *point4 = (dt_masks_node_polygon_t *)pt4->data;
      dt_masks_node_polygon_t *point5 = (dt_masks_node_polygon_t *)pt5->data;

      float bx1 = 0.0f, by1 = 0.0f, bx2 = 0.0f, by2 = 0.0f;
      _polygon_catmull_to_bezier(point1->node[0], point1->node[1], point2->node[0], point2->node[1],
                              point3->node[0], point3->node[1], point4->node[0], point4->node[1],
                              &bx1, &by1, &bx2, &by2);
      if(point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bx1;
      if(point2->ctrl2[1] == -1.0) point2->ctrl2[1] = by1;
      point3->ctrl1[0] = bx2;
      point3->ctrl1[1] = by2;
      _polygon_catmull_to_bezier(point2->node[0], point2->node[1], point3->node[0], point3->node[1],
                              point4->node[0], point4->node[1], point5->node[0], point5->node[1],
                              &bx1, &by1, &bx2, &by2);
      if(point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bx2;
      if(point4->ctrl1[1] == -1.0) point4->ctrl1[1] = by2;
      point3->ctrl2[0] = bx1;
      point3->ctrl2[1] = by1;
    }
    // keep form_points tracking the kth element of form->points
    form_points = g_list_next(form_points);
  }
}

static gboolean _polygon_is_clockwise(dt_masks_form_t *form)
{
  if(!form || !form->points) return 0;
  if(!g_list_shorter_than(form->points, 3)) // if we have at least three points...
  {
    float sum = 0.0f;
    for(const GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
    {
      const GList *next = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last elt
      dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data; // kth element of form->points
      dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)next->data;
      if(!point1 || !point2) return 0;
      sum += (point2->node[0] - point1->node[0]) * (point2->node[1] + point1->node[1]);
    }
    return (sum < 0);
  }
  // return dummy answer
  return TRUE;
}

/** fill eventual gaps between 2 points with a line using Bresenham algorithm
    This avoids repeated floating-point division and rounding errors.
 */
static int _polygon_fill_gaps(int lastx, int lasty, int x, int y, dt_masks_dynbuf_t *points)
{
  dt_masks_dynbuf_reset(points);
  dt_masks_dynbuf_add_2(points, x, y);

  const int dx = x - lastx;
  const int dy = y - lasty;
  const int abs_dx = abs(dx);
  const int abs_dy = abs(dy);

  // Only fill gaps if distance is > 1 in either axis
  if(abs_dx <= 1 && abs_dy <= 1) return 1;

  // Use Bresenham's line algorithm (integer-based)
  int err = abs_dx > abs_dy ? (abs_dx / 2) : (abs_dy / 2);
  int px = lastx;
  int py = lasty;
  const int sx = dx > 0 ? 1 : -1;
  const int sy = dy > 0 ? 1 : -1;

  if(abs_dx > abs_dy)
  {
    // Major axis is X
    while(px != x)
    {
      px += sx;
      err -= abs_dy;
      if(err < 0)
      {
        py += sy;
        err += abs_dx;
      }
      dt_masks_dynbuf_add_2(points, px, py);
    }
  }
  else
  {
    // Major axis is Y
    while(py != y)
    {
      py += sy;
      err -= abs_dx;
      if(err < 0)
      {
        px += sx;
        err += abs_dy;
      }
      dt_masks_dynbuf_add_2(points, px, py);
    }
  }
  return 1;
}

/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the node is very sharp */
static void _polygon_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax, dt_masks_dynbuf_t *dpoints,
                                            dt_masks_dynbuf_t *dborder, gboolean clockwise)
{
  // we want to find the start and end angles
  double a1 = atan2f(bmin[1] - cmax[1], bmin[0] - cmax[0]);
  double a2 = atan2f(bmax[1] - cmax[1], bmax[0] - cmax[0]);
  if(a1 == a2) return;

  // we have to be sure that we turn in the correct direction
  if(a2 < a1 && clockwise)
  {
    a2 += 2 * M_PI;
  }
  if(a2 > a1 && !clockwise)
  {
    a1 += 2 * M_PI;
  }

  // we determine start and end radius too
  const float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  const float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // and the max length of the circle arc
  int l = 0;
  if(a2 > a1)
    l = (a2 - a1) * fmaxf(r1, r2);
  else
    l = (a1 - a2) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  const float incra = (a2 - a1) / l;
  const float incrr = (r2 - r1) / l;
  float rr = r1 + incrr;
  float aa = a1 + incra;
  // allocate entries in the dynbufs
  float *dpoints_ptr = dt_masks_dynbuf_reserve_n(dpoints, 2*(l-1));
  float *dborder_ptr = dborder ? dt_masks_dynbuf_reserve_n(dborder, 2*(l-1)) : NULL;
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if(dpoints_ptr)
  {
    for(int i = 1; i < l; i++)
    {
      *dpoints_ptr++ = cmax[0];
      *dpoints_ptr++ = cmax[1];
      if(dborder_ptr)
      {
        *dborder_ptr++ = cmax[0] + rr * cosf(aa);
        *dborder_ptr++ = cmax[1] + rr * sinf(aa);
      }
      rr += incrr;
      aa += incra;
    }
  }
}

static inline gboolean _is_within_pxl_threshold(float *min, float *max, int pixel_threshold)
{
  return abs((int)min[0] - (int)max[0]) < pixel_threshold && 
         abs((int)min[1] - (int)max[1]) < pixel_threshold;
}


/** recursive function to get all points of the polygon AND all point of the border */
/** the function take care to avoid big gaps between points */
static void _polygon_points_recurs(float *p1, float *p2, double tmin, double tmax, float *polygon_min,
                                float *polygon_max, float *border_min, float *border_max, float *rpolygon,
                                float *rborder, dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                int withborder)
{
  // we calculate points if needed
  if(isnan(polygon_min[0]))
  {
    _polygon_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmin,
                        p1[4] + (p2[4] - p1[4]) * tmin * tmin * (3.0 - 2.0 * tmin), polygon_min, polygon_min + 1,
                        border_min, border_min + 1);
  }
  if(isnan(polygon_max[0]))
  {
    _polygon_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmax,
                        p1[4] + (p2[4] - p1[4]) * tmax * tmax * (3.0 - 2.0 * tmax), polygon_max, polygon_max + 1,
                        border_max, border_max + 1);
  }

  const int pixel_threshold = 2 * darktable.gui->ppd;

  // are the points near ?
  if((tmax - tmin < 0.0001)
       || (_is_within_pxl_threshold(polygon_min, polygon_max, pixel_threshold)
          && (!withborder || (_is_within_pxl_threshold(border_min, border_max, pixel_threshold)))))
  {
    dt_masks_dynbuf_add_2(dpoints, polygon_max[0], polygon_max[1]);
    rpolygon[0] = polygon_max[0];
    rpolygon[1] = polygon_max[1];

    if(withborder)
    {
      dt_masks_dynbuf_add_2(dborder, border_max[0], border_max[1]);
      rborder[0] = border_max[0];
      rborder[1] = border_max[1];
    }
    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { NAN, NAN }, b[2] = { NAN, NAN };
  float rc[2] = { 0 }, rb[2] = { 0 };
  _polygon_points_recurs(p1, p2, tmin, tx, polygon_min, c, border_min, b, rc, rb, dpoints, dborder, withborder);
  _polygon_points_recurs(p1, p2, tx, tmax, rc, polygon_max, rb, border_max, rpolygon, rborder, dpoints, dborder, withborder);
}

// Maximum number of self-intersection portions to track;
// helps limit detection complexity
#define POLYGON_MAX_SELF_INTERSECTIONS(nb_nodes) ((nb_nodes) * 4)

/** find all self intersections in a polygon */
static int _polygon_find_self_intersection(dt_masks_dynbuf_t *inter, int nb_nodes, float *border, int border_count,
                                           int *inter_count_out)
{
  if(nb_nodes == 0 || border_count == 0)
  {
    *inter_count_out = 0;
    return 0;
  }

  int inter_count = 0;

  // we search extreme points in x and y
  float xmin_f = FLT_MAX, xmax_f = -FLT_MAX;
  float ymin_f = FLT_MAX, ymax_f = -FLT_MAX;
  int posextr[4] = { -1 };

  for(int i = nb_nodes * 3; i < border_count; i++)
  {
    if(isnan(border[i * 2]) || isnan(border[i * 2 + 1]))
    {
      // find nearest previous valid point; if at start, wrap to last valid point
      int prev = i - 1;
      while(prev >= nb_nodes * 3 && (isnan(border[prev * 2]) || isnan(border[prev * 2 + 1]))) prev--;
      if(prev < nb_nodes * 3)
      {
        // wrap to last valid point in buffer
        prev = border_count - 1;
        while(prev >= nb_nodes * 3 && (isnan(border[prev * 2]) || isnan(border[prev * 2 + 1]))) prev--;
      }
      if(prev >= nb_nodes * 3)
      {
        border[i * 2] = border[prev * 2];
        border[i * 2 + 1] = border[prev * 2 + 1];
      }
      else
      {
        continue; // skip if no valid point found
      }
    }
    if(xmin_f > border[i * 2])
    {
      xmin_f = border[i * 2];
      posextr[0] = i;
    }
    if(xmax_f < border[i * 2])
    {
      xmax_f = border[i * 2];
      posextr[1] = i;
    }
    if(ymin_f > border[i * 2 + 1])
    {
      ymin_f = border[i * 2 + 1];
      posextr[2] = i;
    }
    if(ymax_f < border[i * 2 + 1])
    {
      ymax_f = border[i * 2 + 1];
      posextr[3] = i;
    }
  }

  // Cast to int with explicit rounding for stable grid computation
  int xmin = (int)floorf(xmin_f) - 1;
  int xmax = (int)ceilf(xmax_f) + 1;
  int ymin = (int)floorf(ymin_f) - 1;
  int ymax = (int)ceilf(ymax_f) + 1;
  const int hb = ymax - ymin;
  const int wb = xmax - xmin;

  // we allocate the buffer
  const size_t ss = (size_t)hb * wb;
  if(ss < 10 || hb < 0 || wb < 0)
  {
    *inter_count_out = 0;
    return 0;
  }

  int *binter = dt_pixelpipe_cache_alloc_align_cache(sizeof(int) * ss, 0);
  if(binter == NULL) return 1;
  memset(binter, 0, sizeof(int) * ss);

  dt_masks_dynbuf_t *extra = dt_masks_dynbuf_init(100000, "polygon extra");
  if(extra == NULL)
  {
    dt_pixelpipe_cache_free_align(binter);
    return 1;
  }

  // we'll iterate through all border points, but we can't start at point[0]
  // because it may be in a self-intersected section
  // so we choose a point where we are sure there's no intersection:
  // one from border shape extrema (here x_max)
  // start from the point immediately before the x_max extremum, with safe wrap-around
  int start_idx = posextr[1] - 1;
  if(start_idx < nb_nodes * 3) start_idx = border_count - 1;
  int lastx = border[start_idx * 2];
  int lasty = border[start_idx * 2 + 1];

  for(int ii = nb_nodes * 3; ii < border_count; ii++)
  {
    // we want to loop from one border extremity
    int i = ii - nb_nodes * 3 + posextr[1];
    if(i >= border_count) i = i - border_count + nb_nodes * 3;

    if(inter_count >= POLYGON_MAX_SELF_INTERSECTIONS(nb_nodes)) break;

    // we want to be sure everything is continuous
    _polygon_fill_gaps(lastx, lasty, border[i * 2], border[i * 2 + 1], extra);

    // extra represent all the points between the last one and the current one
    // for all the points in extra, we'll check for self-intersection
    // and "register" them in binter
    for(int j = dt_masks_dynbuf_position(extra) / 2 - 1; j >= 0; j--)
    {
      const int xx = (dt_masks_dynbuf_buffer(extra))[j * 2];
      const int yy = (dt_masks_dynbuf_buffer(extra))[j * 2 + 1];

      // we check also 2 points around to be sure catching intersection
      int v[3] = { 0 };
      const int idx = (yy - ymin) * wb + (xx - xmin);
      // ensure idx is within [0, ss)
      if(idx < 0 || (size_t)idx >= ss)
      {
        dt_masks_dynbuf_free(extra);
        dt_pixelpipe_cache_free_align(binter);
        return 1;
      }
      v[0] = binter[idx];
      if(xx > xmin) v[1] = binter[idx - 1];
      if(yy > ymin) v[2] = binter[idx - wb];

      for(int k = 0; k < 3; k++)
      {
        if(v[k] > 0)
        {
          // there's already a border point "registered" at this coordinate.
          // so we've potentially found a self-intersection portion between v[k] and i
          if((xx == lastx && yy == lasty) || v[k] == i - 1)
          {
            // we haven't move from last point.
            // this is not a real self-interesection, so we just update binter
            binter[idx] = i;
          }
          else if((i > v[k]
                   && ((posextr[0] < v[k] || posextr[0] > i) && (posextr[1] < v[k] || posextr[1] > i)
                       && (posextr[2] < v[k] || posextr[2] > i) && (posextr[3] < v[k] || posextr[3] > i)))
                  || (i < v[k] && posextr[0] < v[k] && posextr[0] > i && posextr[1] < v[k] && posextr[1] > i
                      && posextr[2] < v[k] && posextr[2] > i && posextr[3] < v[k] && posextr[3] > i))
          {
            // we have found a self-intersection portion, between v[k] and i
            // and we are sure that this portion doesn't include one of the shape extrema
            if(inter_count > 0)
            {
              if((v[k] - i) * ((int)dt_masks_dynbuf_get(inter, -2) - (int)dt_masks_dynbuf_get(inter, -1)) > 0
                 && (int)dt_masks_dynbuf_get(inter, -2) >= v[k] && (int)dt_masks_dynbuf_get(inter, -1) <= i)
              {
                // we find an self-intersection portion which include the last one
                // we just update it
                dt_masks_dynbuf_set(inter, -2, v[k]);
                dt_masks_dynbuf_set(inter, -1, i);
              }
              else
              {
                // we find a new self-intersection portion
                dt_masks_dynbuf_add_2(inter, v[k], i);
                inter_count++;
              }
            }
            else
            {
              // we find a new self-intersection portion
              dt_masks_dynbuf_add_2(inter, v[k], i);
              inter_count++;
            }
          }
        }
        else
        {
          // there wasn't anything "registered" at this place in binter
          // we do it now
          binter[idx] = i;
        }
      }
      lastx = xx;
      lasty = yy;
    }
  }

  dt_masks_dynbuf_free(extra);
  dt_pixelpipe_cache_free_align(binter);

  // and we return the number of self-intersection found
  *inter_count_out = inter_count;
  return 0;
}

/** get all points of the polygon and the border */
/** this take care of gaps and self-intersection and iop distortions */
static int _polygon_get_pts_border(dt_develop_t *dev, dt_masks_form_t *form, const double iop_order, const int transf_direction,
                                   dt_dev_pixelpipe_t *pipe, float **points, int *points_count,
                                   float **border, int *border_count, gboolean source)
{
  if(!form || !form->points) return 0;

  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float iwd = pipe->iwidth;
  const float iht = pipe->iheight;
  const guint nb = g_list_length(form->points);

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *intersections = NULL;

  *points = NULL;
  *points_count = 0;
  if(border) *border = NULL;
  if(border) *border_count = 0;

  dpoints = dt_masks_dynbuf_init(1000000, "polygon dpoints");
  if(dpoints == NULL) return 1;

  if(border)
  {
    dborder = dt_masks_dynbuf_init(1000000, "polygon dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 1;
    }
  }

  intersections = dt_masks_dynbuf_init(10 * MAX(nb, 1), "polygon intersections");
  if(intersections == NULL)
  {
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 1;
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  //get the first node's position to use for source offset
  if(source && nb > 0 && transf_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    dt_masks_node_polygon_t *polygon = (dt_masks_node_polygon_t *)form->points->data;
    if(!polygon) return 0;
    dx = (polygon->node[0] - form->source[0]) * iwd;
    dy = (polygon->node[1] - form->source[1]) * iht;
  }
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_node_polygon_t *const pt = (dt_masks_node_polygon_t *)l->data;
    float *const buf = dt_masks_dynbuf_reserve_n(dpoints, 6);
    if(buf)
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
    dt_masks_dynbuf_add_zeros(dborder, 6 * nb);  // need six zeros for each border point
  }

  float *border_init = dt_pixelpipe_cache_alloc_align_float_cache((size_t)6 * nb, 0);
  if(border_init == NULL)
  {
    dt_masks_dynbuf_free(intersections);
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 1;
  }
  int cw = _polygon_is_clockwise(form);
  if(cw == 0) cw = -1;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points init took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we render all segments
  const GList *form_points = form->points;
  for(int k = 0; k < nb; k++)
  {
    const int pb = dborder ? dt_masks_dynbuf_position(dborder) : 0;
    border_init[k * 6 + 2] = -pb;
    const GList *pt2 = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last element
    const GList *pt3 = g_list_next_wraparound(pt2, form->points);
    dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data; // kth element of form->points
    dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)pt2->data;
    dt_masks_node_polygon_t *point3 = (dt_masks_node_polygon_t *)pt3->data;
    float p1[5] = { point1->node[0] * iwd - dx, point1->node[1] * iht - dy, point1->ctrl2[0] * iwd - dx,
                    point1->ctrl2[1] * iht - dy, cw * point1->border[1] * MIN(iwd, iht) };
    float p2[5] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl1[0] * iwd - dx,
                    point2->ctrl1[1] * iht - dy, cw * point2->border[0] * MIN(iwd, iht) };
    float p3[5] = { point2->node[0] * iwd - dx, point2->node[1] * iht - dy, point2->ctrl2[0] * iwd - dx,
                    point2->ctrl2[1] * iht - dy, cw * point2->border[1] * MIN(iwd, iht) };
    float p4[5] = { point3->node[0] * iwd - dx, point3->node[1] * iht - dy, point3->ctrl1[0] * iwd - dx,
                    point3->ctrl1[1] * iht - dy, cw * point3->border[0] * MIN(iwd, iht) };

    // advance form_points for next iteration so that it tracks the kth element of form->points
    form_points = g_list_next(form_points);

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2] = { 0 }, rb[2] = { 0 };
    float bmin[2] = { NAN, NAN };
    float bmax[2] = { NAN, NAN };
    float cmin[2] = { NAN, NAN };
    float cmax[2] = { NAN, NAN };

    _polygon_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, dpoints, dborder, border && (nb >= 3));

    // we check gaps in the border (sharp edges)
    if(dborder && (fabs(dt_masks_dynbuf_get(dborder, -2) - rb[0]) > 1.0f
                   || fabs(dt_masks_dynbuf_get(dborder, -1) - rb[1]) > 1.0f))
    {
      bmin[0] = dt_masks_dynbuf_get(dborder, -2);
      bmin[1] = dt_masks_dynbuf_get(dborder, -1);
    }

    dt_masks_dynbuf_add_2(dpoints, rc[0], rc[1]);

    border_init[k * 6 + 4] = dborder ? -dt_masks_dynbuf_position(dborder) : 0;

    if(dborder)
    {
      if(isnan(rb[0]))
      {
        if(isnan(dt_masks_dynbuf_get(dborder, - 2)))
        {
          dt_masks_dynbuf_set(dborder, -2, dt_masks_dynbuf_get(dborder, -4));
          dt_masks_dynbuf_set(dborder, -1, dt_masks_dynbuf_get(dborder, -3));
        }
        rb[0] = dt_masks_dynbuf_get(dborder, -2);
        rb[1] = dt_masks_dynbuf_get(dborder, -1);
      }
      dt_masks_dynbuf_add_2(dborder, rb[0], rb[1]);

      (dt_masks_dynbuf_buffer(dborder))[k * 6] = border_init[k * 6] = (dt_masks_dynbuf_buffer(dborder))[pb];
      (dt_masks_dynbuf_buffer(dborder))[k * 6 + 1] = border_init[k * 6 + 1] = (dt_masks_dynbuf_buffer(dborder))[pb + 1];
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && nb >= 3)
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
        _polygon_points_recurs_border_gaps(rc, rb, bmin2, bmax, dpoints, dborder, _polygon_is_clockwise(form));
      }
    }
  }

  *points_count = dt_masks_dynbuf_position(dpoints) / 2;
  *points = dt_masks_dynbuf_harvest(dpoints);
  dt_masks_dynbuf_free(dpoints);

  if(dborder)
  {
    *border_count = dt_masks_dynbuf_position(dborder) / 2;
    *border = dt_masks_dynbuf_harvest(dborder);
    dt_masks_dynbuf_free(dborder);
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points point recurs %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we don't want the border to self-intersect
  int inter_count = 0;
  if(border)
  {
    if(_polygon_find_self_intersection(intersections, nb, *border, *border_count, &inter_count) != 0)
    {
      dt_masks_dynbuf_free(intersections);
      dt_pixelpipe_cache_free_align(*points);
      dt_pixelpipe_cache_free_align(*border);
      return 1;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points self-intersect took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }
  }

  // and we transform them with all distorted modules
  if(source && transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
  {
    // we transform with all distortion that happen *before* the module
    // so we have now the TARGET points in module input reference
    if(dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, *points, *points_count))
    {
      // now we move all the points by the shift
      // so we have now the SOURCE points in module input reference
      float pts[2] = { form->source[0] * iwd, form->source[1] * iht };
      if(!dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1)) goto fail;

      dx = pts[0] - (*points)[2];
      dy = pts[1] - (*points)[3];

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, dx, dy) \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 0; i < *points_count; i++)
      {
        (*points)[i * 2]     += dx;
        (*points)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL, *points,
                                        *points_count))
        goto fail;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points end took %0.04f sec\n", form->name, dt_get_wtime() - start2);

    dt_masks_dynbuf_free(intersections);
    dt_pixelpipe_cache_free_align(border_init);
    return 0;
  }
  else if(dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *points, *points_count))
  {
    if(!border || dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *border, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points transform took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }

      if(border)
      {
        // we don't want to copy the falloff points
        for(int k = 0; k < nb; k++)
          for(int i = 2; i < 6; i++) (*border)[k * 6 + i] = border_init[k * 6 + i];

        // now we want to write the skipping zones
        for(int i = 0; i < inter_count; i++)
        {
          const int v = (dt_masks_dynbuf_buffer(intersections))[i * 2];
          const int w = (dt_masks_dynbuf_buffer(intersections))[ i * 2 + 1];
          if(v <= w)
          {
            (*border)[v * 2] = NAN;
            (*border)[v * 2 + 1] = w;
          }
          else
          {
            if(w > nb * 3)
            {
              if(isnan((*border)[nb * 6]) && isnan((*border)[nb * 6 + 1]))
                (*border)[nb * 6 + 1] = w;
              else if(isnan((*border)[nb * 6]))
                (*border)[nb * 6 + 1] = MAX((*border)[nb * 6 + 1], w);
              else
                (*border)[nb * 6 + 1] = w;
              (*border)[nb * 6] = NAN;
            }
            (*border)[v * 2] = NAN;
            (*border)[v * 2 + 1] = NAN;
          }
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_points end took %0.04f sec\n", form->name,
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
  dt_pixelpipe_cache_free_align(*points);
  *points = NULL;
  *points_count = 0;
  if(border)
  {
    dt_pixelpipe_cache_free_align(*border);
    *border = NULL;
    *border_count = 0;
  }
  return 1;
}

/** find relative position within a brush segment that is closest to the point given by coordinates x and y;
    we only need to find the minimum with a resolution of 1%, so we just do an exhaustive search without any
   frills */
static float _polygon_get_position_in_segment(float x, float y, dt_masks_form_t *form, int segment)
{
  if(!form || !form->points) return 0;
  GList *firstpt = g_list_nth(form->points, segment);
  dt_masks_node_polygon_t *point0 = (dt_masks_node_polygon_t *)firstpt->data;
  // advance to next node in list, if not already on the last
  GList *nextpt = g_list_next_bounded(firstpt);
  dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_polygon_t *point3 = (dt_masks_node_polygon_t *)nextpt->data;

  float tmin = 0;
  float dmin = FLT_MAX;

  for(int i = 0; i <= 100; i++)
  {
    const float t = i / 100.0f;
    float sx, sy;
    _polygon_get_XY(point0->node[0], point0->node[1], point1->node[0], point1->node[1],
                  point2->node[0], point2->node[1], point3->node[0], point3->node[1], t, &sx, &sy);

    const float d = (x - sx) * (x - sx) + (y - sy) * (y - sy);
    if(d < dmin)
    {
      dmin = d;
      tmin = t;
    }
  }

  return tmin;
}

static void _add_node_to_segment(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form,
                                  int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!form || !form->points) return;
  // we add a new node to the brush
  dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));

  const float wd = darktable.develop->preview_width;
  const float ht = darktable.develop->preview_height;
  float pts[2] = { pzx * wd, pzy * ht };
  dt_dev_distort_backtransform(darktable.develop, pts, 1);

  // set coordinates
  dt_dev_roi_to_input_space(darktable.develop, TRUE, pzx, pzy, &node->node[0], &node->node[1]);
  node->ctrl1[0] = node->ctrl1[1] = node->ctrl2[0] = node->ctrl2[1] = -1.0;
  node->state = DT_MASKS_POINT_STATE_NORMAL;

  // set other attributes of the new node. we interpolate the starting and the end node of that
  // segment
  const float t = _polygon_get_position_in_segment(node->node[0], node->node[1], form, gui->seg_selected);
  // start and end node of the segment
  GList *pt = g_list_nth(form->points, gui->seg_selected);
  dt_masks_node_polygon_t *point0 = (dt_masks_node_polygon_t *)pt->data;
  dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)g_list_next_wraparound(pt, form->points)->data;
  node->border[0] = point0->border[0] * (1.0f - t) + point1->border[0] * t;
  node->border[1] = point0->border[1] * (1.0f - t) + point1->border[1] * t;

  form->points = g_list_insert(form->points, node, gui->seg_selected + 1);
  _polygon_init_ctrl_points(form);

  dt_masks_gui_form_create(form, gui, index, module);

  gui->node_edited = gui->node_dragging = gui->node_selected = gui->seg_selected + 1;
  gui->seg_selected = -1;
}

// TODO: Should be in masks.c
static void _change_node_type(struct dt_iop_module_t *module, dt_masks_form_t *form,
                               dt_masks_form_gui_t *gui, int index)
{
  if(!form || !form->points) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, gui->group_selected);
  if(!gpt) return;
  dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->node_edited);
  if(!node) return;
  const gboolean is_corner = dt_masks_node_is_cusp(gpt ,gui->node_selected);

  if(is_corner)
  {
    // Switch to round
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _polygon_init_ctrl_points(form);
  }
  else
  {
    // Switch to corner
    node->ctrl1[0] = node->ctrl2[0] = node->node[0];
    node->ctrl1[1] = node->ctrl2[1] = node->node[1];
    node->state = DT_MASKS_POINT_STATE_USER;

  }
  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);
}

static int _polygon_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                                   float **border, int *border_count, int source, const dt_iop_module_t *module)
{
  if(source && !module) return 1;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  return _polygon_get_pts_border(dev, form, ioporder, DT_DEV_TRANSFORM_DIR_ALL, dev->preview_pipe, points,
                              points_count, border, border_count, source);
}

static void _polygon_get_sizes(struct dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index, float *masks_size, float *border_size)
{
  const dt_masks_form_gui_points_t *gpt =
    (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  const int nb = g_list_length(form->points);
  const float wd = darktable.develop->preview_width;
  const float ht = darktable.develop->preview_height;

  float p1[2] = { FLT_MAX, FLT_MAX };
  float p2[2] = { FLT_MIN, FLT_MIN };

  float fp1[2] = { FLT_MAX, FLT_MAX };
  float fp2[2] = { FLT_MIN, FLT_MIN };

  for(int i = nb * 3; i < gpt->points_count; i++)
  {
    // line
    const float x = gpt->points[i * 2];
    const float y = gpt->points[i * 2 + 1];

    p1[0] = fminf(p1[0], x);
    p2[0] = fmaxf(p2[0], x);
    p1[1] = fminf(p1[1], y);
    p2[1] = fmaxf(p2[1], y);

    if(border_size)
    {
      // border
      const float fx = gpt->border[i * 2];
      const float fy = gpt->border[i * 2 + 1];

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

  *masks_size = fmaxf((p2[0] - p1[0]) / wd, (p2[1] - p1[1]) / ht);
  if(border_size) *border_size = fmaxf((fp2[0] - fp1[0]) / wd, (fp2[1] - fp1[1]) / ht);
}

/** get the distance between point (x,y) and the brush */
static void _polygon_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                int node_count, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  const float as2 = as * as;
  
  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(x, y, gpt->source, node_count * 3, gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;

    // offset between form origin and source origin
    const float offset_x = -gpt->points[2] + gpt->source[2];
    const float offset_y = -gpt->points[3] + gpt->source[3];
    int current_seg = 1;

    // distance from source border
    for(int i = node_count * 3; i < gpt->points_count; i++)
    {
      // check if we advance to next polygon segment
      if(gpt->points[i * 2] == gpt->points[current_seg * 6 + 2]
         && gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3])
      {
        current_seg = (current_seg + 1) % node_count;
      }
      
      // calculate source position for current point
      const float source_x = gpt->points[i * 2] + offset_x;
      const float source_y = gpt->points[i * 2 + 1] + offset_y;

      // distance from tested point to current source point
      const float sdx = x - source_x;
      const float sdy = y - source_y;
      const float sdd = (sdx * sdx) + (sdy * sdy);
      
      if(sdd < *dist)
      {
        *dist = sdd;
      }
    }
    return;
  }

  // we check if we are near a segment
  if(gpt->points_count > 2 + node_count * 3)
  {
    int current_seg = 1;
    for(int i = node_count * 3; i < gpt->points_count; i++)
    {
      // do we change of polygon segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % node_count;
      }
      //distance from tested point to current form point
      const float yy = gpt->points[i * 2 + 1];
      const float xx = gpt->points[i * 2];

      const float dx = x - xx;
      const float dy = y - yy;
      const float dd = (dx * dx) + (dy * dy);
      *dist = fminf(*dist, dd);
      if(*dist == dd && current_seg >= 0 && dd < as2)
      {
        if(current_seg == 0)
          *near = node_count - 1;
        else
          *near = current_seg - 1;
      }
    }
  }

  // we check if it's not inside borders, meaning we are not inside at all
  if(!dt_masks_point_in_form_exact(x, y, gpt->border, node_count * 3, gpt->border_count))
    return;
  
  // we are at least inside the border
  *inside = 1;

  // and we check if it's not inside form, meaning we are inside border only
  *inside_border = !(dt_masks_point_in_form_exact(x, y, gpt->points, node_count * 3, gpt->points_count));
}

static int _find_closest_handle(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form, int parentid,
                                 dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;
  const dt_develop_t *const dev = (const dt_develop_t *)darktable.develop;

  // we define a distance to the cursor for handle detection (in backbuf dimensions)
  const float dist_curs = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED; // transformed to backbuf dimensions

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->handle_selected = -1;
  gui->node_selected = -1;
  gui->seg_selected = -1;
  gui->handle_border_selected = -1;
  const guint nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_width / dev->natural_scale;
  pzy *= darktable.develop->preview_height / dev->natural_scale;


  if((gui->group_selected == index) && gui->node_edited >= 0)
  {
    const int k = gui->node_edited;

    // Current node's border handle
    const float bh_x = gpt->border[k * 6];
    const float bh_y = gpt->border[k * 6 + 1];
    if(dt_masks_is_within_radius(pzx, pzy, bh_x, bh_y, dist_curs))
    {
      gui->handle_border_selected = k;

      return 1;
    }

    // Current node's curve handle
    // We can select the handle only if the node is a curve
    if(!dt_masks_node_is_cusp(gpt ,k))
    {
      float ffx, ffy;
      _polygon_ctrl2_to_handle(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], gpt->points[k * 6 + 4],
                              gpt->points[k * 6 + 5], &ffx, &ffy, gpt->clockwise);
      if(dt_masks_is_within_radius(pzx, pzy, ffx, ffy, dist_curs))
      {
        gui->handle_selected = k;

        return 1;
      }
    }
    
    // are we close to the node ?
    if(dt_masks_is_within_radius(pzx, pzy, gpt->points[k * 6 + 2], gpt->points[k * 6 + 3],
                                              dist_curs))
    {
      gui->node_selected = k;

      return 1;
    }
  }

  // iterate all nodes and look for one that is close enough
  for(int k = 0; k < nb; k++)
  {
    if(dt_masks_is_within_radius(pzx, pzy, gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], dist_curs))
    {
      gui->node_selected = k;

      return 1;
    }
  }

  // are we inside the form or the borders or near a segment ???
  int inside, inside_border, near, inside_source;
  float dist;
  _polygon_get_distance(pzx, pzy, dist_curs, gui, index, nb, &inside, &inside_border, &near, &inside_source, &dist);
  if(near < (g_list_length(form->points)) && gui->node_edited == -1)
    gui->seg_selected = near;

  if(near < 0)
  {
    if(inside_source)
    {
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
      return 1;
    }
    else if(inside_border)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
      return 1;
    }
    else if(inside)
    {
      gui->form_selected = TRUE;
      return 1;
    }
  }
  return 0;
}

// get the center of gravity of the form (like if it was a simple polygon)
static void _polygon_gravity_center(dt_masks_form_t *form, float *gx, float *gy, float *surf)
{
  float bx = 0.0f, by = 0.0f, surface = 0.0f;
  
  for(const GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
  {
    const GList *next = g_list_next_wraparound(form_points, form->points);
    const dt_masks_node_polygon_t *point1 = (dt_masks_node_polygon_t *)form_points->data;
    const dt_masks_node_polygon_t *point2 = (dt_masks_node_polygon_t *)next->data;
    
    const float cross_product = point1->node[0] * point2->node[1] - point2->node[0] * point1->node[1];
    surface += cross_product;
    
    bx += (point1->node[0] + point2->node[0]) * cross_product;
    by += (point1->node[1] + point2->node[1]) * cross_product;
  }
  
  const float divisor = 3.0f * surface;
  *gx = bx / divisor;
  *gy = by / divisor;
  *surf = surface;
}

static int _init_hardness(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow, const float masks_size, const float border_size)
{
  float masks_hardness = dt_masks_get_set_conf_value(form, "hardness", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  dt_toast_log(_("Hardness: %3.2f%%"), (border_size * masks_hardness) / masks_size * 100.0f);
  return 1;
}

static int _change_size(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(!form || !form->points) return 0;
  float gx = 0.0f;
  float gy = 0.0f;
  float surf = 0.0f; 
  _polygon_gravity_center(form, &gx, &gy, &surf);

  // Sanitize
  // do not exceed upper limit of 1.0 and lower limit of 0.004
  if(amount < 1.0f && surf < 0.00001f && surf > -0.00001f) return 1;
  if(amount > 1.0f && surf > 4.0f) return 1;
  
  float delta = 0.0f;
  if(increment)
    delta = powf(amount, (float)flow);
  else
    delta = amount;

  for(GList *l = form->points; l; l = g_list_next(l))
  {
    dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)l->data;
    if(!node) continue;

    float new_node_x = 0.0f, new_node_y = 0.0f;
    float ctrl1_offset_x = 0.0f, ctrl1_offset_y = 0.0f, ctrl2_offset_x = 0.0f, ctrl2_offset_y = 0.0f;

    if(increment)
    {
      // Calculate new node position
      new_node_x = gx + (node->node[0] - gx) * delta;
      new_node_y = gy + (node->node[1] - gy) * delta;

      // Calculate control point offsets once
      ctrl1_offset_x = (node->ctrl1[0] - node->node[0]) * delta;
      ctrl1_offset_y = (node->ctrl1[1] - node->node[1]) * delta;
      ctrl2_offset_x = (node->ctrl2[0] - node->node[0]) * delta;
      ctrl2_offset_y = (node->ctrl2[1] - node->node[1]) * delta;
    }
    else
    {
      // Calculate new node position
      new_node_x = gx + (node->node[0] - gx) * delta;
      new_node_y = gy + (node->node[1] - gy) * delta;

      // Calculate control point offsets once
      ctrl1_offset_x = (node->ctrl1[0] - node->node[0]) * delta;
      ctrl1_offset_y = (node->ctrl1[1] - node->node[1]) * delta;
      ctrl2_offset_x = (node->ctrl2[0] - node->node[0]) * delta;
      ctrl2_offset_y = (node->ctrl2[1] - node->node[1]) * delta;
    }


    // Update all coordinates
    node->node[0] = new_node_x;
    node->node[1] = new_node_y;
    node->ctrl1[0] = new_node_x + ctrl1_offset_x;
    node->ctrl1[1] = new_node_y + ctrl1_offset_y;
    node->ctrl2[0] = new_node_x + ctrl2_offset_x;
    node->ctrl2[1] = new_node_y + ctrl2_offset_y;
  }  

  float masks_size = 0.0f;
  _polygon_get_sizes(module, form, gui, index, &masks_size, NULL);

  dt_toast_log(_("Size: %3.2f%%"), masks_size * 100.0f);

    // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);
  return 1;
}

static int _change_hardness(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module,
   int index, const float amount, const dt_masks_increment_t increment, int flow)
{
  // Growing/shrinking loop
  int node_index = 0;
  const float flowed_amount = powf(amount, (float)flow);
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    if(gui->node_edited == -1 || gui->node_edited == node_index)
    {
      dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)l->data;
      if(!node) continue;
      if(increment)
      {
        node->border[0] = CLAMPF(node->border[0] * flowed_amount, HARDNESS_MIN, HARDNESS_MAX);
        node->border[1] = CLAMPF(node->border[1] * flowed_amount, HARDNESS_MIN, HARDNESS_MAX);
      }
      else
      {
        node->border[0] = CLAMPF(amount, HARDNESS_MIN, HARDNESS_MAX);
        node->border[1] = CLAMPF(amount, HARDNESS_MIN, HARDNESS_MAX);
      }
    }
    node_index++;
  }

  // grab sizes for the toast log
  float masks_size = 1.0f, border_size = 0.0f;
  _polygon_get_sizes(module, form, gui, index, &masks_size, &border_size);

  _init_hardness(form, amount, increment, flow, masks_size, border_size);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _polygon_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, int flow,
                                       uint32_t state, dt_masks_form_t *form, int parentid,
                                       dt_masks_form_gui_t *gui, int index,
                                       dt_masks_interaction_t interaction)
{
  if(gui->creation)
  {
    // no change during creation
    return 0;
  }

  else if(gui->edit_mode == DT_MASKS_EDIT_FULL && (gui->form_selected || gui->node_selected >= 0 || gui->handle_selected >= 0 || gui->seg_selected >= 0))
  {
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_change_opacity(form, parentid, up, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK) || gui->node_edited >= 0)
      return _change_hardness(form, parentid, gui, module, index, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else
      return _change_size(form, parentid, gui, module, index, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  return 0;
}

static int _polygon_creation_closing_form(dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  // we don't want a form with less than 3 points
  if(g_list_shorter_than(form->points, 4))
  {
    dt_toast_log(_("Polygon mask requires at least 3 nodes."));
    return 1;
  }

  dt_iop_module_t *crea_module = gui->creation_module;
  // we delete last point (the one we are currently dragging)
  dt_masks_node_polygon_t *point = (dt_masks_node_polygon_t *)g_list_last(form->points)->data;
  form->points = g_list_remove(form->points, point);
  free(point);
  point = NULL;

  gui->node_dragging = -1;
  _polygon_init_ctrl_points(form);

  // we save the form and quit creation mode
  dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);
  if(crea_module)
  {

    dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(crea_module);
    dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid, TRUE);
    gui->creation_module = NULL;
  }
  else
  {
    dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, TRUE);
  }

  return 1;
}

static gboolean _reset_ctrl_points(struct dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  if(!form || !form->points) return FALSE;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return FALSE;
  int node_index = MAX(gui->node_selected, gui->handle_selected);
  dt_masks_node_polygon_t *node
      = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, node_index);
  if(!node) return FALSE;

  if(node->state != DT_MASKS_POINT_STATE_NORMAL && !dt_masks_node_is_cusp(gpt ,node_index))
  {
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _polygon_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _polygon_is_clockwise(form);
  }
  return TRUE;
}

static int _polygon_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                       double pressure, int which, int type, uint32_t state,
                                       dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!gui || !form) return 0;

  _find_closest_handle(module, pzx, pzy, form, parentid, gui, index);

  if(which == 1)
  {
    if(gui->creation)
    {
      if(gui->creation_closing_form)
        return _polygon_creation_closing_form(form, gui);

      if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(form->type & DT_MASKS_CLONE)
        {
          dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);
          return 1;
        }
      }

      else // we create a node
      {
        float masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/polygon/hardness"), HARDNESS_MAX);

        int nb = g_list_length(form->points);
        // change the values
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));

        dt_dev_roi_to_input_space(darktable.develop, TRUE, pzx, pzy, &polygon_node->node[0], &polygon_node->node[1]);

        polygon_node->ctrl1[0] = polygon_node->ctrl1[1] = polygon_node->ctrl2[0] = polygon_node->ctrl2[1] = -1.0;
        polygon_node->border[0] = polygon_node->border[1] = MAX(HARDNESS_MIN, masks_border);
        polygon_node->state = DT_MASKS_POINT_STATE_NORMAL;
  
        if(nb == 0)
        {
          // create the first node
          dt_masks_node_polygon_t *polygon_first_node = (dt_masks_node_polygon_t *)(malloc(sizeof(dt_masks_node_polygon_t)));
          polygon_first_node->node[0] = polygon_node->node[0];
          polygon_first_node->node[1] = polygon_node->node[1];
          polygon_first_node->ctrl1[0] = polygon_first_node->ctrl1[1] = polygon_first_node->ctrl2[0] = polygon_first_node->ctrl2[1] = -1.0;
          polygon_first_node->border[0] = polygon_first_node->border[1] = MAX(HARDNESS_MIN, masks_border);
          polygon_first_node->state = DT_MASKS_POINT_STATE_NORMAL;
          form->points = g_list_append(form->points, polygon_first_node);

          if(form->type & DT_MASKS_CLONE)
          {
            dt_masks_set_source_pos_initial_value(gui, form, pzx, pzy);
          }
          else
          {
            // not used by regular masks
            form->source[0] = form->source[1] = 0.0f;
          }
          nb++;
        }
        form->points = g_list_append(form->points, polygon_node);

        // if this is a ctrl click, the last created point is a sharp one
        if(dt_modifier_is(state, GDK_CONTROL_MASK))
        {
          dt_masks_node_polygon_t *polygon_last_node = g_list_nth_data(form->points, nb - 1);
          polygon_last_node->ctrl1[0] = polygon_last_node->ctrl2[0] = polygon_last_node->node[0];
          polygon_last_node->ctrl1[1] = polygon_last_node->ctrl2[1] = polygon_last_node->node[1];
          polygon_last_node->state = DT_MASKS_POINT_STATE_USER;
        }

        gui->node_dragging = nb;
        _polygon_init_ctrl_points(form);
      }

      // we recreate the form points in all case
      dt_masks_gui_form_create(form, gui, index, module);
    
      return 1;
    }// end of creation mode

    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    else if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the source dragging
      gui->source_dragging = TRUE;
      gui->node_edited = -1;
      gui->delta[0] = gpt->source[2] - gui->pos[0];
      gui->delta[1] = gpt->source[3] - gui->pos[1];
      return 1;
    }
    else if(gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the form dragging
      gui->form_dragging = TRUE;
      gui->node_edited = -1;
      gui->delta[0] = gpt->points[2] - gui->pos[0];
      gui->delta[1] = gpt->points[3] - gui->pos[1];
      return 1;
    }
    else if(gui->node_selected >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(gui->node_edited == gui->node_selected && dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        _change_node_type(module, form, gui, index);
        return 1;
      }
      /*// we register the current position to avoid accidental move
      if(gui->node_edited < 0 && gui->scrollx == 0.0f && gui->scrolly == 0.0f)
      {
        gui->scrollx = pzx;
        gui->scrolly = pzy;
      }*/
      gui->node_edited = gui->node_dragging = gui->node_selected;

      gui->delta[0] = gpt->points[gui->node_selected * 6 + 2] - gui->pos[0];
      gui->delta[1] = gpt->points[gui->node_selected * 6 + 3] - gui->pos[1];

      return 1;
    }
    else if(gui->handle_selected >= 0)
    {  
      if(!dt_masks_node_is_cusp(gpt ,gui->handle_selected))
      {
        gui->handle_dragging = gui->handle_selected;
      
        // we need to find the handle position
        float handle_x, handle_y;
        const int k = gui->handle_dragging;
        _polygon_ctrl2_to_handle(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3],
                                gpt->points[k * 6 + 4], gpt->points[k * 6 + 5],
                                &handle_x, &handle_y, gpt->clockwise);
        // compute offsets
        gui->delta[0] = handle_x - gui->pos[0];
        gui->delta[1] = handle_y - gui->pos[1];

        return 1;
      }
    }
    else if(gui->handle_border_selected >= 0)
    {
      gui->handle_border_dragging = gui->handle_border_selected;

      const float handle_x = gpt->border[gui->handle_border_dragging * 6];
      const float handle_y = gpt->border[gui->handle_border_dragging * 6 + 1];
      gui->delta[0] = handle_x - gui->pos[0];
      gui->delta[1] = handle_y - gui->pos[1];

      return 1;
    }
    else if(gui->seg_selected >= 0)
    {
      gui->node_selected = -1;

      if(dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        _add_node_to_segment(module, pzx, pzy, form, parentid, gui, index);
      }
      else
      {
        // we move the entire segment
        gui->seg_dragging = gui->seg_selected;
        gui->delta[0] = gpt->points[gui->seg_selected * 6 + 2] - gui->pos[0];
        gui->delta[1] = gpt->points[gui->seg_selected * 6 + 3] - gui->pos[1];
      }
      return 1;
    }
    gui->node_edited = -1;
  }

  return 0;
}

static int _polygon_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                        uint32_t state, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  if(gui->creation) return 1;

  if(which == 1)
  {
    if(gui->form_dragging)
    {
      // we end the form dragging
      gui->form_dragging = FALSE;
      return 1;
    }
    else if(gui->source_dragging)
    {
      // we end the form dragging
      gui->source_dragging = FALSE;
      return 1;
    }
    else if(gui->seg_dragging >= 0)
    {
      gui->seg_dragging = -1;
      return 1;
    }
    else if(gui->node_dragging >= 0)
    {
      gui->node_dragging = -1;
      return 1;
    }
    else if(gui->handle_dragging >= 0)
    {
      gui->handle_dragging = -1;
      return 1;
    }
    else if(gui->handle_border_dragging >= 0)
    {
      gui->handle_border_dragging = -1;
      return 1;
    }
  }
  return 0;
}

static int _polygon_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event, dt_masks_form_t *form,
                                              int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!gui || !form) return 0;
  
  if(gui->creation)
  {
    switch(event->keyval)
    {
      case GDK_KEY_BackSpace:
      {
        // Minimum points to create a polygon
        if(gui->node_dragging < 1)
        {
          dt_masks_form_cancel_creation(module, gui);
          return 1;
        }
        // switch previous node coords to the current one
        dt_masks_node_polygon_t *previous_node = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->node_dragging - 1);
        dt_masks_node_polygon_t *current_node = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->node_dragging);
        if(!previous_node || !current_node) return 0;
        previous_node->node[0] = current_node->node[0];
        previous_node->node[1] = current_node->node[1];
      
        dt_masks_remove_node(module, form, 0, gui, 0, gui->node_dragging);
        // Decrease the current dragging node index
        gui->node_dragging -= 1;
        
        dt_dev_pixelpipe_refresh_preview(darktable.develop, FALSE);
        return 1;
      }
      case GDK_KEY_KP_Enter:
      case GDK_KEY_Return:
        return _polygon_creation_closing_form(form, gui);
    }
  }
  return 0;
}

static int _polygon_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                    int which, dt_masks_form_t *form, int parentid,
                                    dt_masks_form_gui_t *gui, int index)
{
  // centre view will have zoom_scale * backbuf_width pixels, we want the handle offset to scale with DPI:
  dt_develop_t *const dev = (dt_develop_t *)darktable.develop;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;
  if(!form) return 0;

  const float wd = dev->preview_width / dev->natural_scale;
  const float ht = dev->preview_height / dev->natural_scale;
  const int iwidth = darktable.develop->preview_pipe->iwidth;
  const int iheight = darktable.develop->preview_pipe->iheight;

  if(gui->node_dragging >= 0)
  {
    if(!form->points) return 0;
    // check if we are near the first point to close the polygon on creation
    if(gui->creation && !g_list_shorter_than(form->points, 4)) // at least 3 points + the one being created 
    {
      const float dist_curs = darktable.gui->mouse.effect_radius;
      
      float pt[2] = { pzx * wd, pzy * ht }; // no backtransform here
      const float dx = pt[0] - gpt->points[2];
      const float dy = pt[1] - gpt->points[3];
      const float dist2 = dx * dx + dy * dy;
      gui->creation_closing_form = dist2 <= dist_curs * dist_curs;
    }

    // update continuously the current node to mouse position
    dt_masks_node_polygon_t *dragged_node = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->node_dragging);

    float pts[2] = { -1 , -1 };
    const float pointer[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(dev, gui->delta, pointer, pts);
    const float dx = pts[0] - dragged_node->node[0];
    const float dy = pts[1] - dragged_node->node[1];

    // we move all points
    dragged_node->ctrl1[0] += dx;
    dragged_node->ctrl2[0] += dx;
    dragged_node->ctrl1[1] += dy;
    dragged_node->ctrl2[1] += dy;
    dragged_node->node[0] += dx;
    dragged_node->node[1] += dy;

    // if first point, adjust the source position accordingly
    if((form->type & DT_MASKS_CLONE) && gui->node_dragging == 0)
    {
      form->source[0] += dx;
      form->source[1] += dy;
    }

    if(gui->creation) _polygon_init_ctrl_points(form);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _polygon_is_clockwise(form);

    return 1;
  }
  else if(gui->creation)
  {
    // Let the cursor motion be redrawn as it moves in GUI
    return 1;
  }

  if(!form->points) return 0;

  else if(gui->seg_dragging >= 0)
  {
    // we get point0 new values
    const GList *const pt = g_list_nth(form->points, gui->seg_dragging);
    const GList *const next_pt = g_list_next_wraparound(pt, form->points);
    dt_masks_node_polygon_t *point = (dt_masks_node_polygon_t *)pt->data;
    dt_masks_node_polygon_t *next_point = (dt_masks_node_polygon_t *)next_pt->data;

    float pts[2] = { -1 , -1 };
    const float pointer[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(dev, gui->delta, pointer, pts);
    const float dx = pts[0] - point->node[0];
    const float dy = pts[1] - point->node[1];

    // if first or last segment, update the source accordingly
    // (the source point follows the first/last segment when moved)
    if((form->type & DT_MASKS_CLONE) && (gui->seg_dragging == 0 || gui->seg_dragging == (g_list_length(form->points) - 1)))
    {
      form->source[0] += dx;
      form->source[1] += dy;
    }

    // we move all points
    point->node[0] += dx;
    point->node[1] += dy;
    point->ctrl1[0]  += dx;
    point->ctrl1[1]  += dy;
    point->ctrl2[0]  += dx;
    point->ctrl2[1]  += dy;

    next_point->node[0] += dx;
    next_point->node[1] += dy;
    next_point->ctrl1[0]  += dx;
    next_point->ctrl1[1]  += dy;
    next_point->ctrl2[0]  += dx;
    next_point->ctrl2[1]  += dy;

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _polygon_is_clockwise(form);

    return 1;
  }

  else if(gui->handle_dragging >= 0)
  {
    dt_masks_node_polygon_t *node
        = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->handle_dragging);
    if(!node) return 0;
    float pts[2] = { pzx * wd + gui->delta[0], pzy * ht + gui->delta[1] };

    // compute ctrl points directly from new handle position
    float p[4]; 
    _polygon_handle_to_ctrl(gpt->points[gui->handle_dragging * 6 + 2], gpt->points[gui->handle_dragging * 6 + 3],
                           pts[0], pts[1], &p[0], &p[1], &p[2], &p[3], gpt->clockwise);

    dt_dev_distort_backtransform(darktable.develop, p, 2);
    
    // set new ctrl points
    for(size_t i = 0; i < 4; i += 2)
    {
      p[i] /= iwidth;
      p[i + 1] /= iheight;
    }

    node->ctrl1[0] = p[0];
    node->ctrl1[1] = p[1];
    node->ctrl2[0] = p[2];
    node->ctrl2[1] = p[3];
    node->state = DT_MASKS_POINT_STATE_USER;

    _polygon_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  
  else if(gui->handle_border_dragging >= 0)
  {
    const int node_index = gui->handle_border_dragging;
    dt_masks_node_polygon_t *node
        = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, node_index);
    if(!node) return 0;

    const int base = node_index * 6;
    const int node_point_index = base + 2;

    // Get delta between the node and its border handle
    const float dx_line = gpt->border[base] - gpt->points[node_point_index];

    // Get the cursor position
    float pts[2];
    const float cursor_x = pzx * wd + gui->delta[0];
    const float cursor_y = pzy * ht + gui->delta[1];

    // Project the cursor position onto the line defined by the node and its border handle
    if(fabsf(dx_line) < 1e-6f)
    {
      // The line is vertical, so we just take the y coordinate of the cursor
      // and the x coordinate of the node
      pts[0] = gpt->points[node_point_index];
      pts[1] = cursor_y;
    }
    else
    {
      // Calculate the slope (a) and intercept (b) of the line defined by the node and its border handle
      const float a = (gpt->border[base + 1] - gpt->points[node_point_index + 1]) / dx_line;
      const float b = gpt->points[node_point_index + 1] - a * gpt->points[node_point_index];

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
    const float dx = pts[0] - node_x;
    const float dy = pts[1] - node_y;
    const float bdr = sqrtf(dx * dx + dy * dy);
    const float border = bdr / fminf(iwidth, iheight);
    
    node->border[0] = node->border[1] = border;

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }


  else if(gui->form_dragging || gui->source_dragging)
  {

    float pts[2] = { -1 , -1 };
    const float pointer[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(dev, gui->delta, pointer, pts);

    // we move all points
    if(gui->form_dragging)
    {
      dt_masks_node_polygon_t *dragging_shape = (dt_masks_node_polygon_t *)(form->points)->data;
      if(!dragging_shape) return 0;
      const float dx = pts[0] - dragging_shape->node[0];
      const float dy = pts[1] - dragging_shape->node[1];
      for(GList *nodes = form->points; nodes; nodes = g_list_next(nodes))
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
      form->source[0] = pts[0];
      form->source[1] = pts[1];
    }

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }

  if(_find_closest_handle(module, pzx, pzy, form, parentid, gui, index)) return 1;
  if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void _polygon_draw_shape(cairo_t *cr, const float *points, const int points_count, const int node_nb, const gboolean border, const gboolean source)
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
    for (int i = start_idx + 1; i < points_count; i++)
    {
      if(!isnan(points[i * 2]) && !isnan(points[i * 2 + 1]))
        cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
    }
  }
}

static void _polygon_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int node_count)
{
  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  if(gui->creation)
  {
    // draw a cross where the source will be created
    if(darktable.develop->form_visible
     && (darktable.develop->form_visible->type & DT_MASKS_CLONE))
    {

      float node_posx = node_count ? gpt->points[2] : gui->pos[0];
      float node_posy = node_count ? gpt->points[3] : gui->pos[1];

      float pts[2] = { 0.0, 0.0 };
      dt_masks_calculate_source_pos_value(gui, DT_MASKS_POLYGON, node_posx, node_posy, node_posx, node_posy, &pts[0], &pts[1], FALSE);
      dt_draw_cross(cr, zoom_scale, pts[0], pts[1]);
    }
  }
  
  // update clockwise info for the handles
  else if(gui->node_edited >= 0 || gui->node_dragging >= 0 || gui->handle_selected >= 0)
  {
    dt_masks_form_t *group_form = darktable.develop->form_visible ? darktable.develop->form_visible : NULL;
    if(!group_form) return;
    dt_masks_form_group_t *fpt = g_list_nth_data(group_form->points, index);
    if(!fpt) return;
    dt_masks_form_t *polygone = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!polygone) return;
    gpt->clockwise = _polygon_is_clockwise(polygone);
  }

  // draw polygon
  if(gpt->points_count > node_count * 3 + 6) // there must be something to draw
  {
    const int total_points = gpt->points_count * 2;
    int seg1 = 1;
    int current_seg = 0;
    /* Draw the line point-by-point up to the next node, then stroke it; repeat in a loop. */
    cairo_move_to(cr, gpt->points[node_count * 6], gpt->points[node_count * 6 + 1]);
    for(int i = node_count * 3; i < gpt->points_count; i++)
    {
      const double x = gpt->points[i * 2];
      const double y = gpt->points[i * 2 + 1];
      cairo_line_to(cr, x, y);

      int seg_idx = seg1 * 6;
      if((seg_idx + 3) < total_points)
      {
        const double segment_x = gpt->points[seg_idx + 2];
        const double segment_y = gpt->points[seg_idx + 3];

        /* Is this point the next node? */
        if(x == segment_x && y == segment_y)
        {
          const gboolean seg_selected = (gui->group_selected == index) && (gui->seg_selected == current_seg);
          const gboolean all_selected = (gui->group_selected == index) && gui->node_edited == -1 && (gui->form_selected || gui->form_dragging);
          // creation mode: draw the current segment as round dotted line
          if(gui->creation && current_seg == node_count -2)
            dt_draw_stroke_line(DT_MASKS_DASH_ROUND, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);
          else
            dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, (seg_selected || all_selected), zoom_scale, CAIRO_LINE_CAP_BUTT);
          seg1 = (seg1 + 1) % node_count;
          current_seg++;

          // stop drawing on the last segment if we are creating
          if(gui->creation && current_seg >= node_count -1 ) break;
        }
      }
    }
  }

  if(gui->group_selected == index)
  {
    // draw borders
    if(gpt->border_count > node_count * 3 + 2)
    {
      dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, node_count, (gui->border_selected), zoom_scale, gpt->border,
                        gpt->border_count, &dt_masks_functions_polygon.draw_shape, CAIRO_LINE_CAP_ROUND);
    }

    // draw the current node's handle if it's a curve node
    if(gui->node_edited >= 0 && !dt_masks_node_is_cusp(gpt ,gui->node_edited))
    {
      const int n = gui->node_edited;
      float handle_x, handle_y;
      _polygon_ctrl2_to_handle(gpt->points[n * 6 + 2], gpt->points[n * 6 + 3], gpt->points[n * 6 + 4],
                                gpt->points[n * 6 + 5], &handle_x, &handle_y, gpt->clockwise);
      const float pt_x = gpt->points[n * 6 + 2];
      const float pt_y = gpt->points[n * 6 + 3];
      const gboolean selected = (gui->node_selected == n
                              || gui->handle_selected == n);
      dt_draw_handle(cr, pt_x, pt_y, zoom_scale, handle_x, handle_y, selected, FALSE);
    }
  }

  // draw nodes
  if(gui->group_selected == index || gui->creation)
  {
    for(int k = 0; k < node_count; k++)
    {
      // don't draw the last node while creating
      if(gui->creation && k == node_count - 1) break;

      const gboolean squared = dt_masks_node_is_cusp(gpt ,k);
      const gboolean selected = (k == gui->node_selected || k == gui->node_dragging);
      const gboolean action = (k == gui->node_edited);
      const float x = gpt->points[k * 6 + 2];
      const float y = gpt->points[k * 6 + 3];
     
      // draw the first node as big circle while creating the polygon
      if(gui->creation && k == 0)
        dt_draw_node(cr, FALSE, TRUE, TRUE, zoom_scale, x, y);
      else
        dt_draw_node(cr, squared, action, selected, zoom_scale, x, y);
    }

    // Draw the current node's border handle, if needed
    if(gui->node_edited >= 0)
    {
      const int edited = gui->node_edited;
      const gboolean selected = (gui->node_selected == edited
                              || gui->handle_border_selected == edited);
      const int curr_node = edited * 6;  
      const float x = gpt->border[curr_node];
      const float y = gpt->border[curr_node + 1];

      dt_draw_handle(cr, -1, -1, zoom_scale, x, y, selected, TRUE);
    }
  }

  // draw the source if needed
  if(gpt->source_count > node_count * 3 + 2)
  {
    dt_masks_draw_source(cr, gui, index, node_count, zoom_scale, &dt_masks_functions_polygon.draw_shape);
    
    //draw the current node projection
    for(int k = 0; k < node_count; k++)
    {
      if(k == gui->node_selected || k == gui->node_edited || k == node_count - 1)
      {
        const int node_index = k * 6 + 2;
        const float proj_x = gpt->source[node_index];
        const float proj_y = gpt->source[node_index + 1];
        const gboolean selected = gui->node_selected == k;
        const gboolean squared = dt_masks_node_is_cusp(gpt ,k);

        dt_draw_handle(cr, -1, -1, zoom_scale, proj_x, proj_y, selected, squared);
      }
    }
  }
}

static void _polygon_bounding_box_raw(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                                   float *x_min, float *x_max, float *y_min, float *y_max)
{
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = nb_corner * 3; i < num_borders; i++)
  {
    // we look at the borders
    const float xx = border[i * 2];
    const float yy = border[i * 2 + 1];
    if(isnan(xx))
    {
     if(isnan(yy)) break; // that means we have to skip the end of the border polygon
      i = yy - 1;
      continue;
    }
    xmin = MIN(xx, xmin);
    xmax = MAX(xx, xmax);
    ymin = MIN(yy, ymin);
    ymax = MAX(yy, ymax);
  }
  for(int i = nb_corner * 3; i < num_points; i++)
  {
    // we look at the polygon too
    const float xx = points[i * 2];
    const float yy = points[i * 2 + 1];
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

static void _polygon_bounding_box(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                               int *width, int *height, int *posx, int *posy)
{
  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  _polygon_bounding_box_raw(points, border, nb_corner, num_points, num_borders, &xmin, &xmax, &ymin, &ymax);
  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
}

static int _get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const form, int *width, int *height, int *posx, int *posy, gboolean get_source)
{
  if(!module) return 1;

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count = 0, border_count = 0;

  if(_polygon_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, get_source) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 1;
  }

  const guint nb_corner = g_list_length(form->points);
  _polygon_bounding_box(points, border, nb_corner, points_count, border_count, width, height, posx, posy);

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  return 0;
}

static int _polygon_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                 dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, TRUE);
}

static int _polygon_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, FALSE);
}

/** we write a falloff segment */
/*static*/ void _polygon_falloff(float *const restrict buffer, int *p0, int *p1, int posx, int posy, int bw)
{
  // segment length
  int l = sqrtf(sqf(p1[0] - p0[0]) + sqf(p1[1] - p0[1])) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0] - posx;
    const int y = (int)((float)i * ly / (float)l) + p0[1] - posy;
    const float op = 1.0 - (float)i / (float)l;
    size_t idx = y * bw + x;
    buffer[idx] = fmaxf(buffer[idx], op);
    if(x > 0)
      buffer[idx - 1] = fmaxf(buffer[idx - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      buffer[idx - bw] = fmaxf(buffer[idx - bw], op); // this one is to avoid gap due to int rounding
  }
}

static int _polygon_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 1;
  double start = 0.0;
  double start2 = 0.0;

  if(darktable.unmuted & DT_DEBUG_PERF) start = dt_get_wtime();

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(_polygon_get_pts_border(module->dev, form, module->iop_order,
                           DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, FALSE) != 0)
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon points took %0.04f sec\n", form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  // now we want to find the area, so we search min/max points
  const guint nb_corner = g_list_length(form->points);
  _polygon_bounding_box(points, border, nb_corner, points_count, border_count, width, height, posx, posy);

  const int hb = *height;
  const int wb = *width;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", form->name,
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
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    return 1;
  }

  // we write all the point around the polygon into the buffer
  const int nbp = border_count;
  if(nbp > 2)
  {
    int lastx = (int)points[(nbp - 1) * 2];
    int lasty = (int)points[(nbp - 1) * 2 + 1];
    int lasty2 = (int)points[(nbp - 2) * 2 + 1];

    int just_change_dir = 0;
    for(int ii = nb_corner * 3; ii < 2 * nbp - nb_corner * 3; ii++)
    {
      // we are writing more than 1 loop in the case the dir in y change
      // exactly at start/end point
      int i = ii;
      if(ii >= nbp) i = (ii - nb_corner * 3) % (nbp - nb_corner * 3) + nb_corner * 3;
      const int xx = (int)points[i * 2];
      const int yy = (int)points[i * 2 + 1];

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
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill draw polygon took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

#ifdef _OPENMP
#pragma omp parallel for \
  dt_omp_firstprivate(hb, wb, bufptr) \
  schedule(static)
#endif
  for(int yy = 0; yy < hb; yy++)
  {
    int state = 0;
    for(int xx = 0; xx < wb; xx++)
    {
      const float v = bufptr[yy * wb + xx];
      if(v == 1.0f) state = !state;
      if(state) bufptr[yy * wb + xx] = 1.0f;
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill plain took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // now we fill the falloff
  int p0[2] = { 0 }, p1[2] = { 0 };
  float pf1[2] = { 0.0f };
  int last0[2] = { -100, -100 }, last1[2] = { -100, -100 };
  int next = 0;
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2];
    p0[1] = points[i * 2 + 1];
    if(next > 0)
      p1[0] = pf1[0] = border[next * 2], p1[1] = pf1[1] = border[next * 2 + 1];
    else
      p1[0] = pf1[0] = border[i * 2], p1[1] = pf1[1] = border[i * 2 + 1];

    // now we check p1 value to know if we have to skip a part
    if(next == i) next = 0;
    while(isnan(pf1[0]))
    {
      if(isnan(pf1[1]))
        next = i - 1;
      else
        next = p1[1];
      p1[0] = pf1[0] = border[next * 2];
      p1[1] = pf1[1] = border[next * 2 + 1];
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
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n", form->name,
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
static void _polygon_falloff_roi(float *buffer, int *p0, int *p1, int bw, int bh)
{
  // segment length
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  const int dx = lx < 0 ? -1 : 1;
  const int dy = ly < 0 ? -1 : 1;
  const int dpy = dy * bw;

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0];
    const int y = (int)((float)i * ly / (float)l) + p0[1];
    const float op = 1.0f - (float)i / (float)l;
    float *buf = buffer + (size_t)y * bw + x;
    if(x >= 0 && x < bw && y >= 0 && y < bh)
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
                              dt_masks_form_t *const form,
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
  if(_polygon_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, FALSE) != 0)
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
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon points took %0.04f sec\n", form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  const guint nb_corner = g_list_length(form->points);

  // we shift and scale down polygon and border
  for(int i = nb_corner * 3; i < border_count; i++)
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
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    const float xx = points[2 * i];
    const float yy = points[2 * i + 1];
    points[2 * i] = xx * scale - px;
    points[2 * i + 1] = yy * scale - py;
  }

  // now check if polygon is at least partially within roi
  for(int i = nb_corner * 3; i < points_count; i++)
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
    int nb = 0;
    int last = -9999;
    const int x = width / 2;
    const int y = height / 2;

    for(int i = nb_corner * 3; i < points_count; i++)
    {
      const int yy = (int)points[2 * i + 1];
      if(yy != last && yy == y)
      {
        if(points[2 * i] > x) nb++;
      }
      last = yy;
    }
    // if there is an uneven number of intersection points roi lies within polygon
    if(nb & 1)
    {
      polygon_in_roi = 1;
      polygon_encircles_roi = 1;
    }
  }

  // now check if feather is at least partially within roi
  for(int i = nb_corner * 3; i < border_count; i++)
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
  _polygon_bounding_box_raw(points, border, nb_corner, points_count, border_count, &xmin, &xmax, &ymin, &ymax);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill clear mask took %0.04f sec\n", form->name,
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
    const int crop_success = _polygon_crop_to_roi(cpoints + 2 * (nb_corner * 3),
                                               points_count - nb_corner * 3, 0,
                                               width - 1, 0, height);
    polygon_encircles_roi = polygon_encircles_roi || !crop_success;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill crop to roi took %0.04f sec\n", form->name,
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

      for(int i = nb_corner * 3; i < points_count; i++)
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
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill draw polygon took %0.04f sec\n", form->name,
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(xxmin, xxmax, yymin, yymax, width) \
  shared(buffer) schedule(static) num_threads(MIN(8,darktable.num_openmp_threads))
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
      for(int yy = yymin; yy <= yymax; yy++)
      {
        int state = 0;
        for(int xx = xxmin; xx <= xxmax; xx++)
        {
          const size_t index = (size_t)yy * width + xx;
          const float v = buffer[index];
          if(v > 0.5f) state = !state;
          if(state) buffer[index] = 1.0f;
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill plain took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }
    }
    dt_pixelpipe_cache_free_align(cpoints);
  }

  // deal with feather if it does not lie outside of roi
  if(!polygon_encircles_roi)
  {
    int *dpoints = dt_pixelpipe_cache_alloc_align_cache(sizeof(int) * 4 * border_count, 0);
    if(dpoints == NULL)
    {
      dt_pixelpipe_cache_free_align(points);
      dt_pixelpipe_cache_free_align(border);
      return 1;
    }

    int dindex = 0;
    int p0[2], p1[2];
    float pf1[2];
    int last0[2] = { -100, -100 };
    int last1[2] = { -100, -100 };
    int next = 0;
    for(int i = nb_corner * 3; i < border_count; i++)
    {
      p0[0] = floorf(points[i * 2] + 0.5f);
      p0[1] = ceilf(points[i * 2 + 1]);
      if(next > 0)
      {
        p1[0] = pf1[0] = border[next * 2];
        p1[1] = pf1[1] = border[next * 2 + 1];
      }
      else
      {
        p1[0] = pf1[0] = border[i * 2];
        p1[1] = pf1[1] = border[i * 2 + 1];
      }

      // now we check p1 value to know if we have to skip a part
      if(next == i) next = 0;
      while(isnan(pf1[0]))
      {
        if(isnan(pf1[1]))
          next = i - 1;
        else
          next = p1[1];
        p1[0] = pf1[0] = border[next * 2];
        p1[1] = pf1[1] = border[next * 2 + 1];
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
    }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, dindex) \
  shared(buffer, dpoints)
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
    for(int n = 0; n < dindex; n += 4)
      _polygon_falloff_roi(buffer, dpoints + n, dpoints + n + 2, width, height);

    dt_pixelpipe_cache_free_align(dpoints);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] polygon_fill fill falloff took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
    }
  }

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] polygon fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 0;
}

static void _polygon_sanitize_config(dt_masks_type_t type)
{
  // nothing to do (yet?)
}

static void _polygon_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("polygon #%d"), (int)nb);
}

static void _polygon_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation && g_list_length(form->points) < 4)
    g_strlcat(msgbuf, _("<b>Add node</b>: click, <b>Add sharp node</b>:ctrl+click\n"
                        "<b>Cancel</b>: right-click or Esc"), msgbuf_len);
  else if(gui->creation)
    g_strlcat(msgbuf, _("<b>Add node</b>: click, <b>Add sharp node</b>:ctrl+click\n"
                        "<b>Finish polygon</b>: Enter or click on first node"), msgbuf_len);
  else if(gui->handle_selected >= 0)
    g_strlcat(msgbuf, _("<b>Node curvature</b>: drag\n<b>Reset curvature</b>: right-click"), msgbuf_len);
  else if(gui->node_edited >= 0)
    g_strlcat(msgbuf, _("<b>NODE:</b> <b>Move</b>: drag, <b>Delete</b>: right-click or Del\n"
                        "<b>Hardness</b>: scroll, <b>Switch smooth/sharp</b>: ctrl+click"), msgbuf_len);
  else if(gui->node_selected >= 0)
    g_strlcat(msgbuf, _("<b>Move node</b>: drag\n<b>Delete node</b>: right-click\n"
                        "<b>Hardness</b>: scroll, <b>Switch smooth/sharp</b>: ctrl+click"), msgbuf_len);
  else if(gui->seg_selected >= 0)
    g_strlcat(msgbuf, _("<b>Move segment</b>: drag\n<b>Add node</b>: ctrl+click"), msgbuf_len);
  else if(gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len, _("<b>Size</b>: scroll, <b>Hardness</b>: shift+scroll\n"
                                     "<b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _polygon_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(const GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_node_polygon_t *pt = (dt_masks_node_polygon_t *)pts->data;
    dt_masks_node_polygon_t *npt = (dt_masks_node_polygon_t *)malloc(sizeof(dt_masks_node_polygon_t));
    memcpy(npt, pt, sizeof(dt_masks_node_polygon_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _polygon_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  *x = (0.1f * iwd);
  *y = (0.1f * iht);
}

static void _polygon_creation_closing_form_callback(GtkWidget *widget, struct dt_masks_form_gui_t *gui)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  if(!form) return;

  _polygon_creation_closing_form(form, gui);
}

static void _polygon_switch_node_callback(GtkWidget *widget, struct dt_masks_form_gui_t *gui)
{
  gui->node_edited = gui->node_selected;

  if(!gui) return;
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  dt_masks_form_t *forms = darktable.develop->form_visible;
  if(!forms) return;
  dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(forms->points, gui->group_selected);
  if(!fpt) return;
  dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!sel) return;
  _change_node_type(module, sel, gui, gui->group_selected);
}

static void _polygon_reset_round_node_callback(GtkWidget *widget, struct dt_masks_form_gui_t *gui)
{
  if(!gui) return;
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  dt_masks_form_t *forms = darktable.develop->form_visible;
  if(!forms) return;
  dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(forms->points, gui->group_selected);
  if(!fpt) return;
  dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!sel) return;

  _reset_ctrl_points(module, sel, gui, gui->group_selected);
}

static int _polygon_populate_context_menu(GtkWidget *menu, struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui)
{
  // Only add separator if there will be menu items
  if(gui->creation || gui->node_selected >= 0)
  {
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
  }

  GtkWidget *menu_item = NULL;

  if(gui->creation)
  {
    menu_item = masks_gtk_menu_item_new_with_markup(_("Close path"), menu, _polygon_creation_closing_form_callback, gui);
    gtk_widget_set_sensitive(menu_item, form->points && !g_list_shorter_than(form->points, 4));
    menu_item_set_fake_accel(menu_item, GDK_KEY_Return, 0);

    menu_item = masks_gtk_menu_item_new_with_markup(_("Remove last point"), menu, _masks_gui_delete_node_callback, gui);
    menu_item_set_fake_accel(menu_item, GDK_KEY_BackSpace, 0);

  }

  else if(gui->node_selected >= 0)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, gui->group_selected);
    if(!gpt) return 0;
    dt_masks_node_polygon_t *node = (dt_masks_node_polygon_t *)g_list_nth_data(form->points, gui->node_selected);
    if(!node) return 0;
    const gboolean is_corner = dt_masks_node_is_cusp(gpt ,gui->node_selected);

    {
      gchar *to_change_type = g_strdup_printf(_("Switch to %s node"), (is_corner) ? _("round") : _("cusp"));
      const dt_masks_menu_icon_t icon = is_corner ? DT_MASKS_MENU_ICON_CIRCLE : DT_MASKS_MENU_ICON_SQUARE;
      menu_item = masks_gtk_menu_item_new_with_icon(to_change_type, menu, _polygon_switch_node_callback, gui, icon);
      g_free(to_change_type);
    }

    {
      menu_item = masks_gtk_menu_item_new_with_markup(_("Reset round node"), menu, _polygon_reset_round_node_callback, gui);
      gtk_widget_set_sensitive(menu_item, !is_corner);
    }
  }

  return 1;
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
