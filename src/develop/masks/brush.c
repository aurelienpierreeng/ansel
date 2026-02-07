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

#define HARDNESS_MIN 0.0005f
#define HARDNESS_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

/** get squared distance of indexed point to line segment, taking weighted payload data into account */
static float _brush_point_line_distance2(int index, int pointscount, const float *points, const float *payload)
{
  const float x = points[2 * index];
  const float y = points[2 * index + 1];
  const float b = payload[4 * index];
  const float h = payload[4 * index + 1];
  const float d = payload[4 * index + 2];
  const float xstart = points[0];
  const float ystart = points[1];
  const float bstart = payload[0];
  const float hstart = payload[1];
  const float dstart = payload[2];
  const float xend = points[2 * (pointscount - 1)];
  const float yend = points[2 * (pointscount - 1) + 1];
  const float bend = payload[4 * (pointscount - 1)];
  const float hend = payload[4 * (pointscount - 1) + 1];
  const float dend = payload[4 * (pointscount - 1) + 2];
  const float bweight = 1.0f;
  const float hweight = 0.01f;
  const float dweight = 0.01f;

  const float r1 = x - xstart;
  const float r2 = y - ystart;
  const float r3 = xend - xstart;
  const float r4 = yend - ystart;
  const float r5 = bend - bstart;
  const float r6 = hend - hstart;
  const float r7 = dend - dstart;

  const float r = r1 * r3 + r2 * r4;
  const float l = sqf(r3) + sqf(r4);
  const float p = r / l;

  float dx = 0.0f, dy = 0.0f, db = 0.0f, dh = 0.0f, dd = 0.0f;

  if(l == 0.0f)
  {
    dx = x - xstart;
    dy = y - ystart;
    db = b - bstart;
    dh = h - hstart;
    dd = d - dstart;
  }
  else if(p < 0.0f)
  {
    dx = x - xstart;
    dy = y - ystart;
    db = b - bstart;
    dh = h - hstart;
    dd = d - dstart;
  }
  else if(p > 1.0f)
  {
    dx = x - xend;
    dy = y - yend;
    db = b - bend;
    dh = h - hend;
    dd = d - dend;
  }
  else
  {
    dx = x - (xstart + p * r3);
    dy = y - (ystart + p * r4);
    db = b - (bstart + p * r5);
    dh = h - (hstart + p * r6);
    dd = d - (dstart + p * r7);
  }

  return sqf(dx) + sqf(dy) + bweight * sqf(db) + hweight * dh * dh + dweight * sqf(dd);
}

/** remove unneeded points (Ramer-Douglas-Peucker algorithm) and return resulting path as linked list */
static GList *_brush_ramer_douglas_peucker(const float *points, int points_count, const float *payload,
                                           float epsilon2)
{
  GList *ResultList = NULL;

  float dmax2 = 0.0f;
  int index = 0;

  for(int i = 1; i < points_count - 1; i++)
  {
    float d2 = _brush_point_line_distance2(i, points_count, points, payload);
    if(d2 > dmax2)
    {
      index = i;
      dmax2 = d2;
    }
  }

  if(dmax2 >= epsilon2)
  {
    GList *ResultList1 = _brush_ramer_douglas_peucker(points, index + 1, payload, epsilon2);
    GList *ResultList2 = _brush_ramer_douglas_peucker(points + index * 2, points_count - index,
                                                      payload + index * 4, epsilon2);

    // remove last element from ResultList1
    GList *end1 = g_list_last(ResultList1);
    free(end1->data);
    ResultList1 = g_list_delete_link(ResultList1, end1);

    ResultList = g_list_concat(ResultList1, ResultList2);
  }
  else
  {
    dt_masks_node_brush_t *point1 = malloc(sizeof(dt_masks_node_brush_t));
    point1->node[0] = points[0];
    point1->node[1] = points[1];
    point1->ctrl1[0] = point1->ctrl1[1] = point1->ctrl2[0] = point1->ctrl2[1] = -1.0f;
    point1->border[0] = point1->border[1] = payload[0];
    point1->hardness = payload[1];
    point1->density = payload[2];
    point1->state = DT_MASKS_POINT_STATE_NORMAL;
    ResultList = g_list_append(ResultList, (gpointer)point1);

    dt_masks_node_brush_t *pointn = malloc(sizeof(dt_masks_node_brush_t));
    pointn->node[0] = points[(points_count - 1) * 2];
    pointn->node[1] = points[(points_count - 1) * 2 + 1];
    pointn->ctrl1[0] = pointn->ctrl1[1] = pointn->ctrl2[0] = pointn->ctrl2[1] = -1.0f;
    pointn->border[0] = pointn->border[1] = payload[(points_count - 1) * 4];
    pointn->hardness = payload[(points_count - 1) * 4 + 1];
    pointn->density = payload[(points_count - 1) * 4 + 2];
    pointn->state = DT_MASKS_POINT_STATE_NORMAL;
    ResultList = g_list_append(ResultList, (gpointer)pointn);
  }

  return ResultList;
}

/** get the point of the brush at pos t [0,1]  */
static void _brush_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                          float p3y, float t, float *x, float *y)
{
  const float ti = 1.0f - t;
  const float a = ti * ti * ti;
  const float b = 3.0f * t * ti * ti;
  const float c = 3.0f * sqf(t) * ti;
  const float d = t * t * t;
  *x = p0x * a + p1x * b + p2x * c + p3x * d;
  *y = p0y * a + p1y * b + p2y * c + p3y * d;
}

/** get the point of the brush at pos t [0,1]  AND the corresponding border point */
static void _brush_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                                 float p3y, float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  // we get the point
  _brush_get_XY(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, xc, yc);

  // now we get derivative points
  const float ti = 1.0f - t;
  const float a = 3.0f * ti * ti;
  const float b = 3.0f * (ti * ti - 2.0f * t * ti);
  const float c = 3.0f * (2.0f * t * ti - t * t);
  const float d = 3.0f * sqf(t);

  const float dx = -p0x * a + p1x * b + p2x * c + p3x * d;
  const float dy = -p0y * a + p1y * b + p2y * c + p3y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *xb = NAN;
    *yb = NAN;
    return;
  }
  const float l = 1.0f / sqrtf(dx * dx + dy * dy);
  *xb = (*xc) + rad * dy * l;
  *yb = (*yc) - rad * dx * l;
}

/** get handle extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _brush_ctrl2_to_handle(float ptx, float pty, float ctrlx, float ctrly, float *fx, float *fy,
                                    gboolean clockwise)
{
  if(clockwise)
  {
    *fx = ptx + ctrly - pty;
    *fy = pty + ptx - ctrlx;
  }
  else
  {
    *fx = ptx - ctrly + pty;
    *fy = pty - ptx + ctrlx;
  }
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

/** Get the control points of a segment to match exactly a catmull-rom spline */
static void _brush_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                     float y4, float *bx1, float *by1, float *bx2, float *by2)
{
  *bx1 = (-x1 + 6 * x2 + x3) / 6;
  *by1 = (-y1 + 6 * y2 + y3) / 6;
  *bx2 = (x2 + 6 * x3 - x4) / 6;
  *by2 = (y2 + 6 * y3 - y4) / 6;
}

/** initialise all control points to eventually match a catmull-rom like spline */
static void _brush_init_ctrl_points(dt_masks_form_t *form)
{
  // if we have less than 2 points, what to do ??
  if(g_list_shorter_than(form->points, 2)) return;

  // we need extra points to deal with curve ends
  dt_masks_node_brush_t start_point[2], end_point[2];

  for (GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
  {
    dt_masks_node_brush_t *point3 = (dt_masks_node_brush_t *)form_points->data;
    // if the point has not been set manually, we redefine it
    if(point3->state & DT_MASKS_POINT_STATE_NORMAL)
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

/** recursive function to get all points of the brush AND all point of the border */
/** the function takes care to avoid big gaps between points */
static void _brush_points_recurs(float *p1, float *p2, double tmin, double tmax, float *points_min,
                                 float *points_max, float *border_min, float *border_max, float *rpoints,
                                 float *rborder, float *rpayload, dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                 dt_masks_dynbuf_t *dpayload)
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

  const int pixel_threshold = 2 * darktable.gui->ppd;

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
      while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
      {
        rpayload[0] = p1[5] + tmax * (p2[5] - p1[5]);
        rpayload[1] = p1[6] + tmax * (p2[6] - p1[6]);
        dt_masks_dynbuf_add_2(dpayload, rpayload[0], rpayload[1]);
      }
    }

    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { NAN, NAN }, b[2] = { NAN, NAN };
  float rc[2], rb[2], rp[2];
  _brush_points_recurs(p1, p2, tmin, tx, points_min, c, border_min, b, rc, rb, rp, dpoints, dborder, dpayload);
  _brush_points_recurs(p1, p2, tx, tmax, rc, points_max, rb, border_max, rpoints, rborder, rpayload, dpoints,
                       dborder, dpayload);
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
static int _brush_get_pts_border(dt_develop_t *dev, dt_masks_form_t *form, const double iop_order, const int transf_direction,
                                    dt_dev_pixelpipe_t *pipe, float **points, int *points_count,
                                    float **border, int *border_count, float **payload, int *payload_count,
                                    int source)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float iwd = pipe->iwidth;
  const float iht = pipe->iheight;

  *points = NULL;
  *points_count = 0;
  if(border) *border = NULL;
  if(border) *border_count = 0;
  if(payload) *payload = NULL;
  if(payload) *payload_count = 0;

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *dpayload = NULL;

  dpoints = dt_masks_dynbuf_init(1000000, "brush dpoints");
  if(dpoints == NULL) return 0;

  if(border)
  {
    dborder = dt_masks_dynbuf_init(1000000, "brush dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 0;
    }
  }

  if(payload)
  {
    dpayload = dt_masks_dynbuf_init(1000000, "brush dpayload");
    if(dpayload == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      dt_masks_dynbuf_free(dborder);
      return 0;
    }
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  if(source && form->points && transf_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    dt_masks_node_brush_t *pt = (dt_masks_node_brush_t *)form->points->data;
    dx = (pt->node[0] - form->source[0]) * iwd;
    dy = (pt->node[1] - form->source[1]) * iht;
  }

  for(GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
  {
    const dt_masks_node_brush_t *const pt = (dt_masks_node_brush_t *)form_points->data;
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

  const guint nb = g_list_length(form->points);

  // for the border, we store value too
  if(dborder)
  {
    dt_masks_dynbuf_add_zeros(dborder, 6 * nb);  // we need six zeros for each border point
  }

  // for the payload, we reserve an equivalent number of cells to keep it in sync
  if(dpayload)
  {
    dt_masks_dynbuf_add_zeros(dpayload, 6 * nb); // we need six zeros for each border point
  }

  int cw = 1;
  int start_stamp = 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points init took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we render all segments first upwards, then downwards
  for(int n = 0; n < 2 * nb; n++)
  {
    float p1[7], p2[7], p3[7], p4[7];
    const int k = _brush_cyclic_cursor(n, nb);
    const int k1 = _brush_cyclic_cursor(n + 1, nb);
    const int k2 = _brush_cyclic_cursor(n + 2, nb);

    dt_masks_node_brush_t *point1 = (dt_masks_node_brush_t *)g_list_nth_data(form->points, k);
    dt_masks_node_brush_t *point2 = (dt_masks_node_brush_t *)g_list_nth_data(form->points, k1);
    dt_masks_node_brush_t *point3 = (dt_masks_node_brush_t *)g_list_nth_data(form->points, k2);
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
    if((fabsf(p1[5] - p2[5]) > 0.05f || fabsf(p1[6] - p2[6]) > 0.05f) || (start_stamp && n == 2 * nb - 1))
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
          while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
          {
            dt_masks_dynbuf_add_2(dpayload, p1[5], p1[6]);
          }
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
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, dpoints, dborder, TRUE);
      }

      if(dpayload)
      {
        while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
        {
          dt_masks_dynbuf_add_2(dpayload, p1[5], p1[6]);
        }
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
        while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
        {
          dt_masks_dynbuf_add_2(dpayload, p1[5], p1[6]);
        }
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

    _brush_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, rp, dpoints, dborder, dpayload);

    dt_masks_dynbuf_add_2(dpoints, rc[0], rc[1]);

    if(dpayload)
    {
      dt_masks_dynbuf_add_2(dpayload, rp[0], rp[1]);
    }

    if(dborder)
    {
      if(isnan(rb[0]))
      {
        if(isnan(dt_masks_dynbuf_get(dborder, -2)))
        {
          dt_masks_dynbuf_set(dborder, -2, dt_masks_dynbuf_get(dborder, -4));
          dt_masks_dynbuf_set(dborder, -1, dt_masks_dynbuf_get(dborder, -3));
        }
        rb[0] = dt_masks_dynbuf_get(dborder, -2);
        rb[1] = dt_masks_dynbuf_get(dborder, -1);
      }
      dt_masks_dynbuf_add_2(dborder, rb[0], rb[1]);
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && nb >= 3)
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
        _brush_points_recurs_border_gaps(rc, rb, NULL, bmax, dpoints, dborder, cw);
      }
    }

    if(dpayload)
    {
      while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
      {
        dt_masks_dynbuf_add_2(dpayload, rp[0], rp[1]);
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

  if(dpayload)
  {
    *payload_count = dt_masks_dynbuf_position(dpayload) / 2;
    *payload = dt_masks_dynbuf_harvest(dpayload);
    dt_masks_dynbuf_free(dpayload);
  }
  // printf("points %d, border %d, playload %d\n", *points_count, border ? *border_count : -1, payload ?
  // *payload_count : -1);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points point recurs %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
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
    dt_omp_firstprivate(points_count, points, dx, dy)              \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 0; i < *points_count; i++)
      {
        (*points)[i * 2] += dx;
        (*points)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL, *points,
                                        *points_count))
        goto fail;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_points end took %0.04f sec\n", form->name, dt_get_wtime() - start2);

    return 1;
  }
  else if(dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *points, *points_count))
  {
    if(!border || dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *border, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points transform took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
      return 1;
    }
  }

  // if we failed, then free all and return
fail:
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  if(border)
  {
    dt_free_align(*border);
    *border = NULL;
    *border_count = 0;
  }
  if(payload)
  {
    dt_free_align(*payload);
    *payload = NULL;
    *payload_count = 0;
  }
  return 0;
}

/** get the distance between point (x,y) and the brush */
static void _brush_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                int corner_count, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  if(!gui) return;

  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  const float as2 = as * as;

  // we first check if we are inside the source form

  // add support for clone masks
  if(gpt->points_count > 2 + corner_count * 3 && gpt->source_count > 2 + corner_count * 3)
  {
    // distance from form origin and source origin
    const float dx = -gpt->points[2] + gpt->source[2];
    const float dy = -gpt->points[3] + gpt->source[3];

    int current_seg = 1;
    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      // distance from tested point to current form point
      const float yy = gpt->points[i * 2 + 1] + dy;
      const float xx = gpt->points[i * 2] + dx;

      const float sdx = x - xx;
      const float sdy = y - yy;
      const float dd = (sdx * sdx) + (sdy * sdy);
      *dist = fminf(*dist, dd);

      if(*dist == dd && dd < as2)
      {
        if(*inside == 0)
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
  if(gpt->border_count > 2 + corner_count * 3)
  {
    int nearest = -1;

    const int start = corner_count * 3;
    const float *const border = gpt->border;
    float last_y = border[gpt->border_count * 2 - 1];
    int crossings = 0;

    for(int i = start; i < gpt->border_count; i++)
    {
      const int idx = i * 2;
      const float xx = border[idx];
      const float yy = border[idx + 1];

      const float dx = x - xx;
      const float dy = y - yy;
      if(dx * dx + dy * dy < as2) nearest = idx;

      if(((y <= yy && y > last_y) || (y >= yy && y < last_y)) && (xx > x))
        crossings++;

      last_y = yy;
    }

    *inside = *inside_border = (nearest != -1 || (crossings & 1));
  }

  // and we check if we are near a segment
  if(gpt->points_count > 2 + corner_count * 3)
  {
    int current_seg = 1;
    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      //distance from tested point to current form point
      const float yy = gpt->points[i * 2 + 1];
      const float xx = gpt->points[i * 2];

      const float dx = x - xx;
      const float dy = y - yy;
      const float dd = (dx * dx) + (dy * dy);
      *dist = fminf(*dist, dd);
      if(*dist == dd && current_seg > 0 && dd < as2)
      {
        *near = current_seg - 1;
      }
    }
  }
}

static int _brush_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                                    float **border, int *border_count, int source, const dt_iop_module_t *module)
{
  if(source && !module) return 0;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  return _brush_get_pts_border(dev, form, ioporder, DT_DEV_TRANSFORM_DIR_ALL, dev->preview_pipe, points,
                               points_count, border, border_count, NULL, NULL, source);
}

/** find relative position within a brush segment that is closest to the point given by coordinates x and y;
    we only need to find the minimum with a resolution of 1%, so we just do an exhaustive search without any
   frills */
static float _brush_get_position_in_segment(float x, float y, dt_masks_form_t *form, int segment)
{
  GList *firstpt = g_list_nth(form->points, segment);
  dt_masks_node_brush_t *point0 = (dt_masks_node_brush_t *)firstpt->data;
  // advance to next node in list, if not already on the last
  GList *nextpt = g_list_next_bounded(firstpt);
  dt_masks_node_brush_t *point1 = (dt_masks_node_brush_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_brush_t *point2 = (dt_masks_node_brush_t *)nextpt->data;
  nextpt = g_list_next_bounded(nextpt);
  dt_masks_node_brush_t *point3 = (dt_masks_node_brush_t *)nextpt->data;

  float tmin = 0;
  float dmin = FLT_MAX;

  for(int i = 0; i <= 100; i++)
  {
    const float t = i / 100.0f;
    float sx, sy;
    _brush_get_XY(point0->node[0], point0->node[1], point1->node[0], point1->node[1],
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

static int _find_closest_handle(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form, int parentid,
                                 dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  // get the zoom scale
  const dt_develop_t *dev = (const dt_develop_t *)darktable.develop;

  // we define a distance to the cursor for handle detection (in backbuf dimensions)
  const float dist_curs = darktable.gui->mouse.effect_radius_screen; // transformed to backbuf dimensions

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->handle_selected = -1;
  gui->node_selected = -1;
  gui->seg_selected = -1;
  gui->handle_border_selected = -1;
  const guint nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_pipe->backbuf.width / dev->natural_scale;
  pzy *= darktable.develop->preview_pipe->backbuf.height / dev->natural_scale;


  if((gui->group_selected == index) && gui->node_edited >= 0)
  {
    const int k = gui->node_edited;
    // we can select the handle only if the node is a curve
    if(!dt_masks_is_corner_node(gpt, k, 6, 2))
    {
      float ffx, ffy;
      _brush_ctrl2_to_handle(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], gpt->points[k * 6 + 4],
                              gpt->points[k * 6 + 5], &ffx, &ffy, TRUE);
    if(dt_masks_is_within_radius(pzx, pzy, ffx, ffy, dist_curs))
      {
        gui->handle_selected = k;

        return 1;
      }
    }

    // are we also close to the node ?
    if(dt_masks_is_within_radius(pzx, pzy, gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], dist_curs))
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
  int in, inside_border, near, inside_source;
  float dist;
  _brush_get_distance(pzx, pzy, dist_curs, gui, index, nb, &in, &inside_border, &near, &inside_source, &dist);
  // the maximum segment number is nb-1 (open brush)
  if(near < (g_list_length(form->points) - 1))
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
    else if(in)
    {
      gui->form_selected = TRUE;
      return 1;
    }
  }
  return 0;
}


static int _init_hardness(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float masks_hardness = dt_masks_get_set_conf_value(form, "hardness", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  if(gui->guipoints_count > 0) dt_masks_dynbuf_set(gui->guipoints_payload, -3, masks_hardness);
  dt_toast_log(_("hardness: %3.2f%%"), masks_hardness*100.0f);
  return 1;
}

static int _init_size(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float masks_border = dt_masks_get_set_conf_value(form, "border", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  if(gui->guipoints_count > 0) dt_masks_dynbuf_set(gui->guipoints_payload, -4, masks_border);
  dt_toast_log(_("size: %3.2f%%"), masks_border*2.f*100.f);
  return 1;
}

static int _init_opacity(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float masks_opacity = dt_masks_get_set_conf_value(form, "opacity", amount, 0.f, 1.f, increment, flow);
  dt_toast_log(_("opacity: %3.2f%%"), masks_opacity*100.f);
  return 1;
}

static int _change_hardness(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  int node_index = 0;
  const float flowed_amount = powf(amount, (float)flow);
  float res_amount = 0;
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    if(gui->node_selected == -1 || gui->node_selected == node_index)
    {
      dt_masks_node_brush_t *point = (dt_masks_node_brush_t *)l->data;
      const float masks_hardness = point->hardness;
      res_amount = increment ? masks_hardness * flowed_amount : amount; 

      point->hardness = CLAMPF(res_amount, HARDNESS_MIN, HARDNESS_MAX);
    }
    node_index++;
  }

  dt_masks_get_set_conf_value(form, "hardness", res_amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _change_size(dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  // Sanitize loop
  // do not exceed upper limit of 1.0 and lower limit of 0.004
  int pts_number = 0;
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    if(gui->node_selected == -1 || gui->node_selected == pts_number)
    {
      dt_masks_node_brush_t *point = (dt_masks_node_brush_t *)l->data;
      if(amount > 1.0f && (point->border[0] > 1.0f || point->border[1] > 1.0f))
        return 1;
    }
    pts_number++;
  }

  // Growing/shrinking loop
  pts_number = 0;
  for(GList *l = form->points; l; l = g_list_next(l))
  {
    if(gui->node_selected == -1 || gui->node_selected == pts_number)
    {
      dt_masks_node_brush_t *point = (dt_masks_node_brush_t *)l->data;

      switch(increment)
      {
        case(DT_MASKS_INCREMENT_SCALE):
        {
          point->border[0] *= amount;
          point->border[1] *= amount;
          break;
        }
        case(DT_MASKS_INCREMENT_OFFSET):
        {
          point->border[0] += amount;
          point->border[1] += amount;
          break;
        }
        case(DT_MASKS_INCREMENT_ABSOLUTE):
        {
          point->border[0] = amount;
          point->border[1] = amount;
        }
      }
    }
    pts_number++;
  }

  dt_masks_get_set_conf_value(form, "border", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _brush_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, const int flow,
                                        uint32_t state, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index,
                                        dt_masks_interaction_t interaction)
{
  if(gui->creation)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _init_hardness(form, parentid, gui, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return _init_opacity(form, parentid, gui, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
    else
      return _init_size(form, parentid, gui, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  else if(gui->form_selected || gui->node_selected >= 0 || gui->handle_selected >= 0
          || gui->seg_selected >= 0)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }

    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_change_opacity(form, parentid, up, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _change_hardness(form, parentid, gui, module, index, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else // resize don't care where the mouse is inside a shape
      return _change_size(form, parentid, gui, module, index, up ? 1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  return 0;
}


static void _get_pressure_sensitivity(dt_masks_form_gui_t *gui)
{
  gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  const char *psens = dt_conf_get_string_const("pressure_sensitivity");
  if(psens)
  {
    if(!strcmp(psens, "hardness (absolute)"))
      gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_ABS;
    else if(!strcmp(psens, "hardness (relative)"))
      gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_REL;
    else if(!strcmp(psens, "opacity (absolute)"))
      gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_ABS;
    else if(!strcmp(psens, "opacity (relative)"))
      gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_REL;
    else if(!strcmp(psens, "brush size (relative)"))
      gui->pressure_sensitivity = DT_MASKS_PRESSURE_BRUSHSIZE_REL;
  }
}

static void _change_node_type(struct dt_iop_module_t *module, dt_masks_form_t *form, int parentid,
                               dt_masks_form_gui_t *gui, int index)
{
  dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)g_list_nth_data(form->points, gui->node_edited);
  if(node->state != DT_MASKS_POINT_STATE_NORMAL)
  {
    node->state = DT_MASKS_POINT_STATE_NORMAL;
    _brush_init_ctrl_points(form);
  }
  else
  {
    node->ctrl1[0] = node->ctrl2[0] = node->node[0];
    node->ctrl1[1] = node->ctrl2[1] = node->node[1];
    node->state = DT_MASKS_POINT_STATE_USER;
  }
  // we recreate the form points
  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);
}

static void _add_node_to_segment(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form,
                                  int parentid, dt_masks_form_gui_t *gui, int index)
{
  // we add a new node to the brush
  dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)(malloc(sizeof(dt_masks_node_brush_t)));

  dt_dev_roi_to_input_space(darktable.develop, TRUE, pzx, pzy, &node->node[0], &node->node[1]);

  node->ctrl1[0] = node->ctrl1[1] = node->ctrl2[0] = node->ctrl2[1] = -1.0;
  node->state = DT_MASKS_POINT_STATE_NORMAL;

  // set other attributes of the new node. we interpolate the starting and the end node of that
  // segment
  const float t = _brush_get_position_in_segment(node->node[0], node->node[1], form, gui->seg_selected);
  // start and end node of the segment
  GList *pt = g_list_nth(form->points, gui->seg_selected);
  dt_masks_node_brush_t *point0 = (dt_masks_node_brush_t *)pt->data;
  dt_masks_node_brush_t *point1 = (dt_masks_node_brush_t *)g_list_next_wraparound(pt, form->points)->data;
  node->border[0] = point0->border[0] * (1.0f - t) + point1->border[0] * t;
  node->border[1] = point0->border[1] * (1.0f - t) + point1->border[1] * t;
  node->hardness = point0->hardness * (1.0f - t) + point1->hardness * t;
  node->density = point0->density * (1.0f - t) + point1->density * t;

  form->points = g_list_insert(form->points, node, gui->seg_selected + 1);
  _brush_init_ctrl_points(form);

  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);

  gui->node_edited = gui->node_dragging = gui->node_selected = gui->seg_selected + 1;
  gui->seg_selected = -1;
}

static int _brush_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                        double pressure, int which, int type, uint32_t state,
                                        dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  // double click or triple click: ignore here
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  dt_develop_t *dev = (dt_develop_t *)darktable.develop;

  // Do we need to refresh currently active node ?
  // Its requested to give back the focus when clicking outside current shape.
  _find_closest_handle(module, pzx, pzy, form, parentid, gui, index);

  // always start with a mask density of 100%, it will be adjusted with pen pressure if used.
  const float masks_density = 1.0f;

  if(gui->creation)
  {
    if(which == 1)
    {
      // The trick is to use the incremental setting, set to 1.0 to re-use the generic getter/setter without changing value
      float masks_border = dt_masks_get_set_conf_value(form, "border", 1.0f, HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);
      float masks_hardness = dt_masks_get_set_conf_value(form, "hardness", 1.0f, HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);
    
      if(dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

        return 1;
      }

      const float wd = darktable.develop->preview_pipe->backbuf.width / dev->natural_scale;
      const float ht = darktable.develop->preview_pipe->backbuf.height / dev->natural_scale;

      if(!gui->guipoints) gui->guipoints = dt_masks_dynbuf_init(200000, "brush guipoints");
      if(!gui->guipoints) return 1;
      if(!gui->guipoints_payload) gui->guipoints_payload = dt_masks_dynbuf_init(400000, "brush guipoints_payload");
      if(!gui->guipoints_payload) return 1;
      dt_masks_dynbuf_add_2(gui->guipoints, pzx * wd, pzy * ht);
      dt_masks_dynbuf_add_2(gui->guipoints_payload, masks_border, masks_hardness);
      dt_masks_dynbuf_add_2(gui->guipoints_payload, masks_density, pressure);

      gui->guipoints_count = 1;

      // add support for clone masks
      if(form->type & DT_MASKS_CLONE)
        dt_masks_set_source_pos_initial_value(gui, form, pzx, pzy);
      // not used by regular masks
      else
        form->source[0] = form->source[1] = 0.0f;

      _get_pressure_sensitivity(gui);

      return 1;
    }
    else if(which == 3)
    {
      // Delete shape from current group
      // TODO: map that to context menu
      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);

      return 1;
    }
  }

  else if(which == 1)
  {
    if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the source dragging
      gui->source_dragging = TRUE;
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
        // FIXME: handle that in context menu
        _change_node_type(module, form, parentid, gui, index);
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
      if(!dt_masks_is_corner_node(gpt, gui->handle_selected, 6, 2))
      {
        gui->handle_dragging = gui->handle_selected;
      
        // we need to find the handle position
        float handle_x, handle_y;
        const int k = gui->handle_dragging;
        _brush_ctrl2_to_handle(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3],
                                gpt->points[k * 6 + 4], gpt->points[k * 6 + 5],
                                &handle_x, &handle_y, TRUE);
        // compute offsets
        gui->delta[0] = handle_x - gui->pos[0];
        gui->delta[1] = handle_y - gui->pos[1];

        return 1;
      }
    }
    /* unused
    else if(gui->handle_border_selected >= 0)
    {
      gui->node_edited = -1;
      gui->handle_border_dragging = gui->handle_border_selected;
      gui->handle_border_selected = -1; // reset
      return 1;
    }*/
    else if(gui->seg_selected >= 0)
    {
      gui->node_edited = -1;

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

  else if(which == 3)
  {
    if(gui->handle_selected >= 0)
    {
      // reset handle to default position
      dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)g_list_nth_data(form->points, gui->handle_selected);
      if(node && node->state != DT_MASKS_POINT_STATE_NORMAL && !dt_masks_is_corner_node(gpt, gui->handle_selected, 6, 2))
      {
        node->state = DT_MASKS_POINT_STATE_NORMAL;
        _brush_init_ctrl_points(form);

        // we recreate the form points
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index, module);
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

static void _apply_pen_pressure(dt_masks_form_gui_t *gui, float *guipoints_payload)
{
  for(int i = 0; i < gui->guipoints_count; i++)
  {
    float *payload = guipoints_payload + 4 * i;
    float pressure = payload[3];
    payload[3] = 1.0f;

    switch(gui->pressure_sensitivity)
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


static int _brush_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                         uint32_t state, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  // The trick is to use the incremental setting, set to 1.0 to re-use the generic getter/setter without changing value
  float masks_border = dt_masks_get_set_conf_value(form, "border", 1.0f, HARDNESS_MIN, HARDNESS_MAX, TRUE, 1);

  if(gui->creation && which == 1)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK) || dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    {
      // user just set the source position, so just return
      return 1;
    }

    dt_iop_module_t *crea_module = gui->creation_module;

    if(gui->guipoints && gui->guipoints_count > 0)
    {
      // if the path consists only of one x/y pair we add a second one close so we don't need to deal with
      // this special case later
      if(gui->guipoints_count == 1)
      {
        // add a helper node very close to the single spot
        const float x = dt_masks_dynbuf_get(gui->guipoints, -2) + 0.01f;
        const float y = dt_masks_dynbuf_get(gui->guipoints, -1) - 0.01f;
        dt_masks_dynbuf_add_2(gui->guipoints, x, y);
        const float border = dt_masks_dynbuf_get(gui->guipoints_payload, -4);
        const float hardness = dt_masks_dynbuf_get(gui->guipoints_payload, -3);
        const float density = dt_masks_dynbuf_get(gui->guipoints_payload, -2);
        const float pressure = dt_masks_dynbuf_get(gui->guipoints_payload, -1);
        dt_masks_dynbuf_add_2(gui->guipoints_payload, border, hardness);
        dt_masks_dynbuf_add_2(gui->guipoints_payload, density, pressure);
        gui->guipoints_count++;
      }

      float *guipoints = dt_masks_dynbuf_buffer(gui->guipoints);
      float *guipoints_payload = dt_masks_dynbuf_buffer(gui->guipoints_payload);

      // we transform the points
      dt_dev_distort_backtransform(darktable.develop, guipoints, gui->guipoints_count);

      for(int i = 0; i < gui->guipoints_count; i++)
      {
        guipoints[i * 2] /= darktable.develop->preview_pipe->iwidth;
        guipoints[i * 2 + 1] /= darktable.develop->preview_pipe->iheight;
      }

      // we consolidate pen pressure readings into payload
      _apply_pen_pressure(gui, guipoints_payload);

      // accuracy level for node elimination, dependent on brush size
      const float epsilon2 = _get_brush_smoothing() * sqf(MAX(HARDNESS_MIN, masks_border));

      // we simplify the path and generate the nodes
      form->points = _brush_ramer_douglas_peucker(guipoints, gui->guipoints_count, guipoints_payload, epsilon2);

      // printf("guipoints_count %d, points %d\n", gui->guipoints_count, g_list_length(form->points));

      _brush_init_ctrl_points(form);

      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      // we save the form and quit creation mode
      dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

      if(crea_module)
      {
        // TODO: handle this at the mask.c level
        dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
        dt_masks_iop_update(crea_module);
        dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid, TRUE);
        gui->creation_module = NULL;
      }
      else
      {
        dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, TRUE);
      }

      if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        dt_masks_form_t *grp = darktable.develop->form_visible;
        if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
        int pos3 = 0, pos2 = -1;
        for(GList *fs = grp->points; fs; fs = g_list_next(fs))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)fs->data;
          if(pt->formid == form->formid)
          {
            pos2 = pos3;
            break;
          }
          pos3++;
        }
        if(pos2 < 0) return 1;
        dt_masks_form_gui_t *gui2 = darktable.develop->form_gui;
        if(!gui2) return 1;
        gui2->group_selected = pos2;

        dt_masks_select_form(crea_module, dt_masks_get_from_id(darktable.develop, form->formid));
      }
    }
    else
    {
      // unlikely case of button released but no points gathered -> no form
      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);

      dt_masks_change_form_gui(NULL);
    }
    return 1;
  }

  else if(which == 1)
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

static int _brush_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                     int which, dt_masks_form_t *form, int parentid,
                                     dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  dt_develop_t *dev = (dt_develop_t *)darktable.develop;
  const float wd = dev->preview_pipe->backbuf.width / dev->natural_scale;
  const float ht = dev->preview_pipe->backbuf.height / dev->natural_scale;

  if(gui->creation)
  {
    if(gui->guipoints)
    {
      dt_masks_dynbuf_add_2(gui->guipoints, pzx * wd, pzy * ht);
      const float border = dt_masks_dynbuf_get(gui->guipoints_payload, -4);
      const float hardness = dt_masks_dynbuf_get(gui->guipoints_payload, -3);
      const float density = dt_masks_dynbuf_get(gui->guipoints_payload, -2);
      dt_masks_dynbuf_add_2(gui->guipoints_payload, border, hardness);
      dt_masks_dynbuf_add_2(gui->guipoints_payload, density, pressure);
      gui->guipoints_count++;
      return 1;
    }
    else
    {
      // Let the cursor motion be redrawn as it moves in GUI
      return 1;
    }
  }

  if(gui->node_dragging >= 0)
  {
    dt_masks_node_brush_t *dragged_node
        = (dt_masks_node_brush_t *)g_list_nth_data(form->points, gui->node_dragging);

    // apply delta to the current mouse position
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

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  else if(gui->seg_dragging >= 0)
  {
    // we get point0 new values
    const GList *pt1 = g_list_nth(form->points, gui->seg_dragging);
    const GList *pt2 = g_list_next_wraparound(pt1, form->points);
    dt_masks_node_brush_t *point = (dt_masks_node_brush_t *)pt1->data;
    dt_masks_node_brush_t *point2 = (dt_masks_node_brush_t *)pt2->data;
    float pts[2] = { pzx * wd + gui->delta[0], pzy * ht + gui->delta[1] };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->node[0];
    const float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->node[1];

    // we move all points
    point->node[0] += dx;
    point->node[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;
    point2->node[0] += dx;
    point2->node[1] += dy;
    point2->ctrl1[0] += dx;
    point2->ctrl1[1] += dy;
    point2->ctrl2[0] += dx;
    point2->ctrl2[1] += dy;

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  else if(gui->handle_dragging >= 0)
  {
    dt_masks_node_brush_t *node
        = (dt_masks_node_brush_t *)g_list_nth_data(form->points, gui->handle_dragging);

    float pts[2] = { pzx * wd + gui->delta[0], pzy * ht + gui->delta[1] };

    // compute ctrl points directly from new handle position
    float p[4]; 
    _brush_handle_to_ctrl(gpt->points[gui->handle_dragging * 6 + 2], gpt->points[gui->handle_dragging * 6 + 3],
                           pts[0], pts[1], &p[0], &p[1], &p[2], &p[3], TRUE);

    dt_dev_distort_backtransform(darktable.develop, p, 2);
    
    // set new ctrl points
    const float iw = darktable.develop->preview_pipe->iwidth;
    const float ih = darktable.develop->preview_pipe->iheight;
    node->ctrl1[0] = p[0] / iw;
    node->ctrl1[1] = p[1] / ih;
    node->ctrl2[0] = p[2] / iw;
    node->ctrl2[1] = p[3] / ih;
    node->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  /* unused
  else if(gui->handle_border_dragging >= 0)
  {
    const int k = gui->handle_border_dragging;

    // now we want to know the position reflected on actual corner/border segment
    const float a = (gpt->border[k * 6 + 1] - gpt->points[k * 6 + 3])
                    / (float)(gpt->border[k * 6] - gpt->points[k * 6 + 2]);
    const float b = gpt->points[k * 6 + 3] - a * gpt->points[k * 6 + 2];

    float pts[2];
    pts[0] = (a * pzy * ht + pzx * wd - b * a) / (a * a + 1.0);
    pts[1] = a * pts[0] + b;

    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    dt_masks_node_brush_t *point = (dt_masks_node_brush_t *)g_list_nth_data(form->points, k);
    const float nx = point->node[0] * darktable.develop->preview_pipe->iwidth;
    const float ny = point->node[1] * darktable.develop->preview_pipe->iheight;
    const float nr = sqrtf((pts[0] - nx) * (pts[0] - nx) + (pts[1] - ny) * (pts[1] - ny));
    const float bdr = nr / fminf(darktable.develop->preview_pipe->iwidth, darktable.develop->preview_pipe->iheight);

    point->border[0] = point->border[1] = bdr;

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }*/
  else if(gui->form_dragging || gui->source_dragging)
  {
    float pts[2] = { pzx * wd + gui->delta[0], pzy * ht + gui->delta[1] };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    dt_masks_node_brush_t *dragging_shape = (dt_masks_node_brush_t *)(form->points)->data;

    // we move all points
    if(gui->form_dragging)
    {
      const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - dragging_shape->node[0];
      const float dy = pts[1] / darktable.develop->preview_pipe->iheight - dragging_shape->node[1];
      for(GList *nodes = form->points; nodes; nodes = g_list_next(nodes))
      {
        dt_masks_node_brush_t *node = (dt_masks_node_brush_t *)nodes->data;
        node->node[0] += dx;
        node->node[1] += dy;
        node->ctrl1[0] += dx;
        node->ctrl1[1] += dy;
        node->ctrl2[0] += dx;
        node->ctrl2[1] += dy;
      }
    }
    else //source dragging
    {
      form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }

  if(_find_closest_handle(module, pzx, pzy, form, parentid, gui, index)) return 1;
  if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void _brush_draw_shape(cairo_t *cr, const float *points, const int points_count, const int node_nb, const gboolean border, const gboolean source)
{
 // Find the first valid non-NaN point to start drawing
 // FIXME: Why not just avoid having NaN points in the array?
  int start_idx = -1;
  for (int i = node_nb * 3 + border; i < points_count; i++)
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
    
    for (int i = start_idx + 1; i < end_idx; i++)
    {
      if(!isnan(points[i * 2]) && !isnan(points[i * 2 + 1]))
        cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
    }
  }
}

static void _brush_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int node_count)
{
  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // in creation mode
  if(gui->creation)
  {
    const float iwd = darktable.develop->preview_pipe->iwidth;
    const float iht = darktable.develop->preview_pipe->iheight;
    const float min_iwd_iht= MIN(iwd,iht);

    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      const float masks_border = dt_masks_get_set_conf_value(form, "border", 1.0f, HARDNESS_MIN, HARDNESS_MAX, DT_MASKS_INCREMENT_SCALE, 1);
      const float masks_hardness = dt_masks_get_set_conf_value(form, "hardness", 1.0f, HARDNESS_MIN, HARDNESS_MAX, DT_MASKS_INCREMENT_SCALE, 1);
      const float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");

      const float radius1 = masks_border * masks_hardness * min_iwd_iht;
      const float radius2 = masks_border * min_iwd_iht;

      float xpos = gui->pos[0];
      float ypos = gui->pos[1];
      if((xpos == -1.0f && ypos == -1.0f) || gui->mouse_leaved_center)
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
      dt_draw_stroke_line(DT_MASKS_DASH_STICK, FALSE, cr, FALSE, zoom_scale);

      if(form->type & DT_MASKS_CLONE)
      {
        float x = 0.0f, y = 0.0f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_BRUSH, xpos, ypos,
                                            xpos, ypos, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    else
    {
      float masks_border = 0.0f, masks_hardness = 0.0f, masks_density = 0.0f;
      float radius = 0.0f, oldradius = 0.0f, opacity = 0.0f, oldopacity = 0.0f, pressure = 0.0f;
      int stroked = 1;

      const float *guipoints = dt_masks_dynbuf_buffer(gui->guipoints);
      const float *guipoints_payload = dt_masks_dynbuf_buffer(gui->guipoints_payload);

      cairo_save(cr);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      masks_border = guipoints_payload[0];
      masks_hardness = guipoints_payload[1];
      masks_density = guipoints_payload[2];
      pressure = guipoints_payload[3];

      switch(gui->pressure_sensitivity)
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
      for(int i = 1; i < gui->guipoints_count; i++)
      {
        cairo_line_to(cr, guipoints[i * 2], guipoints[i * 2 + 1]);
        stroked = 0;
        masks_border = guipoints_payload[i * 4];
        masks_hardness = guipoints_payload[i * 4 + 1];
        masks_density = guipoints_payload[i * 4 + 2];
        pressure = guipoints_payload[i * 4 + 3];

        switch(gui->pressure_sensitivity)
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
      cairo_arc(cr, guipoints[2 * (gui->guipoints_count - 1)],
                guipoints[2 * (gui->guipoints_count - 1) + 1],
                radius, 0, 2.0 * M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      dt_draw_set_dash_style(cr, DT_MASKS_DASH_STICK, zoom_scale);
      cairo_arc(cr, guipoints[2 * (gui->guipoints_count - 1)],
                guipoints[2 * (gui->guipoints_count - 1) + 1], masks_border * min_iwd_iht, 0,
                2.0 * M_PI);
      cairo_stroke(cr);

      if(darktable.develop->form_visible
         && (darktable.develop->form_visible->type & DT_MASKS_CLONE))
      {
        const int i = gui->guipoints_count - 1;
        float x = 0.0f, y = 0.0f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_BRUSH, guipoints[0], guipoints[1], guipoints[i * 2],
                                            guipoints[i * 2 + 1], &x, &y, TRUE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    return;
  } // creation

  // minimum points
  if(gpt->points_count <= node_count * 3 + 2) return;

  // draw path
  {
    const gboolean all_selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
    const int total_points = gpt->points_count * 0.5;
    
    // Step 1: Draw the entire curve and track selected segment boundaries
    int seg = 1, current_seg = 0;
    int seg_start_idx = node_count * 3;
    // Track current segment start and end index for later
    int sel_start = -1, sel_end = -1;
    
    cairo_move_to(cr, gpt->points[node_count * 6], gpt->points[node_count * 6 + 1]);
    
    for(int i = node_count * 3; i < total_points; i++)
    {
      double x = gpt->points[i * 2];
      double y = gpt->points[i * 2 + 1];
      cairo_line_to(cr, x, y);

      int seg_idx = seg * 6;
      double segment_x = gpt->points[seg_idx + 2];
      double segment_y = gpt->points[seg_idx + 3];

      // End of current segment reached
      if(x == segment_x && y == segment_y)
      {
        // Is this segment the user selected segment?
        if((gui->group_selected == index) && (gui->seg_selected == current_seg))
        {
          sel_start = seg_start_idx;
          sel_end = i;
        }
        seg = (seg + 1) % node_count;
        current_seg++;
        seg_start_idx = i;  // Next segment starts here
      }
    }
    dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, all_selected, zoom_scale);
    
    // Step 2: Draw selected segment on top if needed
    if(sel_start >= 0 && sel_end >= 0)
    {      
      cairo_move_to(cr, gpt->points[sel_start * 2], gpt->points[sel_start * 2 + 1]);
      for(int i = sel_start; i <= sel_end; i++)
      {
        cairo_line_to(cr, gpt->points[i * 2], gpt->points[i * 2 + 1]);
      }
      dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, TRUE, zoom_scale);
    }
  }

  // draw borders
  if((gui->group_selected == index) && gpt->border_count > node_count * 3 + 2)
  {
    dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, node_count, (gui->border_selected), zoom_scale, gpt->border,
                       gpt->border_count, &dt_masks_functions_brush.draw_shape);
  }

  // draw nodes and attached stuff
  if(gui->group_selected == index)
  {
    cairo_save(cr);

    // draw the current node's handle if it's a curve node
    if(gui->node_edited >= 0 && !dt_masks_is_corner_node(gpt, gui->node_edited, 6, 2))
    {
      const int n = gui->node_edited;
      float handle_x, handle_y;
      _brush_ctrl2_to_handle(gpt->points[n * 6 + 2], gpt->points[n * 6 + 3], gpt->points[n * 6 + 4],
                                gpt->points[n * 6 + 5], &handle_x, &handle_y, TRUE);
      const float pt_x = gpt->points[n * 6 + 2];
      const float pt_y = gpt->points[n * 6 + 3];
      const gboolean selected = (gui->node_edited == gui->handle_selected) && gui->handle_selected >= 0;
      dt_draw_handle(cr, pt_x, pt_y, zoom_scale, handle_x, handle_y, selected);
    }

    // draw all nodes
    for(int k = 0; k < node_count; k++)
    {
      const gboolean corner = dt_masks_is_corner_node(gpt, k, 6, 2);
      const float x = gpt->points[k * 6 + 2];
      const float y = gpt->points[k * 6 + 3];
      const gboolean selected = (k == gui->node_selected || k == gui->node_dragging);
      const gboolean action = (k == gui->node_edited);

      dt_draw_node(cr, corner, action, selected, zoom_scale, x, y);
    }
    cairo_restore(cr);
  }

  // draw the source if needed
  if(gpt->source_count > node_count * 3 + 2)
  {
    dt_masks_draw_source(cr, gui, index, node_count, zoom_scale, &dt_masks_functions_brush.draw_shape);
  }
}

static void _brush_bounding_box_raw(const float *const points, const float *const border, const int nb_corner,
                                    const int num_points, float *x_min, float *x_max, float *y_min, float *y_max)
{
  // now we want to find the area, so we search min/max points
  float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
#ifdef _OPENMP
#pragma omp parallel for reduction(min : xmin, ymin) reduction(max : xmax, ymax) \
  schedule(static) if(num_points > 1000)
#endif
  for(int i = nb_corner * 3; i < num_points; i++)
  {
    // we look at the borders
    const float x = border[i * 2];
    const float y = border[i * 2 + 1];
    xmin = MIN(x, xmin);
    xmax = MAX(x, xmax);
    ymin = MIN(y, ymin);
    ymax = MAX(y, ymax);
    // we look at the brush too
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

static void _brush_bounding_box(const float *const points, const float *const border, const int nb_corner,
                                const int num_points, int *width, int *height, int *posx, int *posy)
{
  float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
  _brush_bounding_box_raw(points, border, nb_corner, num_points, &xmin, &xmax, &ymin, &ymax);
  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
}

static int _get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const form, int *width, int *height, int *posx, int *posy, int get_source)
{
  if(!module) return 0;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_brush_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                            &border, &border_count, NULL, NULL, get_source))
  {
    dt_free_align(points);
    dt_free_align(border);
    return 0;
  }

  const guint nb_corner = g_list_length(form->points);
  _brush_bounding_box(points, border, nb_corner, points_count, width, height, posx, posy);

  dt_free_align(points);
  dt_free_align(border);
  return 1;
}

static int _brush_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                  dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, 1);
}

static int _brush_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form, int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, 0);
}

/** we write a falloff segment */
static void _brush_falloff(float *const restrict buffer, int p0[2], int p1[2], int posx, int posy, int bw,
                           float hardness, float density)
{
  // segment length
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;
  const int solid = (int)l * hardness;
  const int soft = l - solid;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0] - posx;
    const int y = (int)((float)i * ly / (float)l) + p0[1] - posy;
    const float op = density * ((i <= solid) ? 1.0f : 1.0 - (float)(i - solid) / (float)soft);
    buffer[y * bw + x] = MAX(buffer[y * bw + x], op);
    if(x > 0)
      buffer[y * bw + x - 1]
          = MAX(buffer[y * bw + x - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      buffer[(y - 1) * bw + x]
          = MAX(buffer[(y - 1) * bw + x], op); // this one is to avoid gap due to int rounding
  }
}

static int _brush_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form,
                           float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  double start = 0.0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start = start2 = dt_get_wtime();

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;
  int points_count, border_count, payload_count;
  if(!_brush_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,&points, &points_count,
                               &border, &border_count, &payload, &payload_count, 0))
  {
    dt_free_align(points);
    dt_free_align(border);
    dt_free_align(payload);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  const guint nb_corner = g_list_length(form->points);
  _brush_bounding_box(points, border, nb_corner, points_count, width, height, posx, posy);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  // we allocate the buffer
  const size_t bufsize = (size_t)(*width) * (*height);
  // ensure that the buffer is zeroed, as the below code only fills in pixels in the falloff region
  *buffer = dt_calloc_align_float(bufsize);
  if(*buffer == NULL)
  {
    dt_free_align(points);
    dt_free_align(border);
    dt_free_align(payload);
    return 0;
  }

  // now we fill the falloff
  int p0[2], p1[2];

  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2];
    p0[1] = points[i * 2 + 1];
    p1[0] = border[i * 2];
    p1[1] = border[i * 2 + 1];

    _brush_falloff(*buffer, p0, p1, *posx, *posy, *width, payload[i * 2], payload[i * 2 + 1]);
  }

  dt_free_align(points);
  dt_free_align(border);
  dt_free_align(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}

/** we write a falloff segment respecting limits of buffer */
static inline void _brush_falloff_roi(float *buffer, const int *p0, const int *p1, int bw, int bh, float hardness,
                                      float density)
{
  // segment length (increase by 1 to avoid division-by-zero special case handling)
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;
  const int solid = hardness * l;

  const float lx = (float)(p1[0] - p0[0]) / (float)l;
  const float ly = (float)(p1[1] - p0[1]) / (float)l;

  const int dx = lx <= 0 ? -1 : 1;
  const int dy = ly <= 0 ? -1 : 1;
  const int dpx = dx;
  const int dpy = dy * bw;

  float fx = p0[0];
  float fy = p0[1];

  float op = density;
  const float dop = density / (float)(l - solid);

  for(int i = 0; i < l; i++)
  {
    const int x = fx;
    const int y = fy;

    fx += lx;
    fy += ly;
    if(i > solid) op -= dop;

    if(x < 0 || x >= bw || y < 0 || y >= bh) continue;

    float *buf = buffer + (size_t)y * bw + x;

    *buf = MAX(*buf, op);
    if(x + dx >= 0 && x + dx < bw)
      buf[dpx] = MAX(buf[dpx], op); // this one is to avoid gaps due to int rounding
    if(y + dy >= 0 && y + dy < bh)
      buf[dpy] = MAX(buf[dpy], op); // this one is to avoid gaps due to int rounding
  }
}

// build a stamp which can be combined with other shapes in the same group
// prerequisite: 'buffer' is all zeros
static int _brush_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                               dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 0;
  double start = 0.0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start = start2 = dt_get_wtime();

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;

  int points_count, border_count, payload_count;

  if(!_brush_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe,&points, &points_count,
                               &border, &border_count, &payload, &payload_count, 0))
  {
    dt_free_align(points);
    dt_free_align(border);
    dt_free_align(payload);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  const guint nb_corner = g_list_length(form->points);

  // we shift and scale down brush and border
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    const float xx = border[2 * i];
    const float yy = border[2 * i + 1];
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


  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
  _brush_bounding_box_raw(points, border, nb_corner, points_count, &xmin, &xmax, &ymin, &ymax);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // check if the path completely lies outside of roi -> we're done/mask remains empty
  if(xmax < 0 || ymax < 0 || xmin >= width || ymin >= height)
  {
    dt_free_align(points);
    dt_free_align(border);
    dt_free_align(payload);
    return 1;
  }

  // now we fill the falloff
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(nb_corner, border_count, width, height) \
  shared(buffer, points, border, payload) schedule(static)
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    const int p0[] = { points[i * 2], points[i * 2 + 1] };
    const int p1[] = { border[i * 2], border[i * 2 + 1] };

    if(MAX(p0[0], p1[0]) < 0 || MIN(p0[0], p1[0]) >= width || MAX(p0[1], p1[1]) < 0
       || MIN(p0[1], p1[1]) >= height)
      continue;

    _brush_falloff_roi(buffer, p0, p1, width, height, payload[i * 2], payload[i * 2 + 1]);
  }

  dt_free_align(points);
  dt_free_align(border);
  dt_free_align(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush set falloff took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);
  }

  return 1;
}

static void _brush_sanitize_config(dt_masks_type_t type)
{
  // nothing to do (yet?)
}

static void _brush_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("brush #%d"), (int)nb);
}

static void _brush_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation || gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len,
               _("<b>Size</b>: scroll, <b>Hardness</b>: shift+scroll\n"
                 "<b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
  else if(gui->border_selected)
    g_strlcat(msgbuf, _("<b>Size</b>: scroll"), msgbuf_len);
}

static void _brush_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_node_brush_t *pt = (dt_masks_node_brush_t *)pts->data;
    dt_masks_node_brush_t *npt = (dt_masks_node_brush_t *)malloc(sizeof(dt_masks_node_brush_t));
    memcpy(npt, pt, sizeof(dt_masks_node_brush_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _brush_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  *x = 0.01f * iwd;
  *y = 0.01f * iht;
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
  .mouse_moved = _brush_events_mouse_moved,
  .mouse_scrolled = _brush_events_mouse_scrolled,
  .button_pressed = _brush_events_button_pressed,
  .button_released = _brush_events_button_released,
  .post_expose = _brush_events_post_expose,
  .draw_shape = _brush_draw_shape,
  .init_ctrl_points = _brush_init_ctrl_points
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
