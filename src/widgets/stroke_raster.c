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

#include "widgets/stroke_raster.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------
 * The touched rectangle, kept on the surface it describes.
 *
 * A surface carries its own record of what was painted into it here, as cairo user data, so a
 * caller that owns the surface can composite or clear exactly that much: the rasteriser is the
 * one thing that knows where its pixels went. Half-open pixel ranges, [x0, x1) x [y0, y1). */
typedef struct _touched_t
{
  gboolean any;
  int x0;
  int y0;
  int x1;
  int y1;
} _touched_t;

static const cairo_user_data_key_t _touched_key = { 0 };

static _touched_t *_touched_of(cairo_surface_t *surface, const gboolean create)
{
  _touched_t *touched = (_touched_t *)cairo_surface_get_user_data(surface, &_touched_key);
  if(touched || !create) return touched;
  touched = g_malloc0(sizeof(_touched_t));
  if(cairo_surface_set_user_data(surface, &_touched_key, touched, g_free) != CAIRO_STATUS_SUCCESS)
  {
    g_free(touched);
    return NULL;
  }
  return touched;
}

static void _touched_add(cairo_surface_t *surface, const int x0, const int y0, const int x1, const int y1)
{
  if(x1 <= x0 || y1 <= y0) return;
  _touched_t *touched = _touched_of(surface, TRUE);
  if(!touched) return;
  if(!touched->any)
  {
    touched->x0 = x0;
    touched->y0 = y0;
    touched->x1 = x1;
    touched->y1 = y1;
    touched->any = TRUE;
    return;
  }
  touched->x0 = MIN(touched->x0, x0);
  touched->y0 = MIN(touched->y0, y0);
  touched->x1 = MAX(touched->x1, x1);
  touched->y1 = MAX(touched->y1, y1);
}

gboolean dt_stroke_raster_touched(cairo_surface_t *surface, cairo_rectangle_int_t *touched)
{
  if(!surface || !touched) return FALSE;
  const _touched_t *record = _touched_of(surface, FALSE);
  if(!record || !record->any) return FALSE;
  touched->x = record->x0;
  touched->y = record->y0;
  touched->width = record->x1 - record->x0;
  touched->height = record->y1 - record->y0;
  return TRUE;
}

void dt_stroke_raster_touched_reset(cairo_surface_t *surface)
{
  if(!surface) return;
  _touched_t *record = _touched_of(surface, FALSE);
  if(record) record->any = FALSE;
}

gboolean dt_stroke_raster_can_paint(cairo_surface_t *surface)
{
  return surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS
         && cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE
         && cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32;
}

/* ---------------------------------------------------------------------------------------------
 * The distance plane.
 *
 * One scratch plane over the polyline's bounding box, holding per pixel not the distance but
 * `reach^2 - d^2', where reach is the widest pass's half-width plus the antialiasing pixel:
 * positive inside the stroke's reach, zero outside and for anything never stamped. Zero being
 * the resting state is what makes the plane cheap to keep: it is never cleared as a whole, only
 * the spans a stroke actually wrote are zeroed again once composited, and a plane that only ever
 * grows starts every new byte at zero. Stamping is MAX, which for this quantity is the nearest
 * point of the polyline, and squared distances need no square root until compositing, which
 * touches only the pixels of the stroke's band and not the box around it.
 *
 * Per row, the extent that was written, so compositing and the clearing after it walk the band
 * rather than the box. Overlays are drawn from one thread at a time; the lock says so. */
typedef struct _plane_t
{
  float *value;      /* reach^2 - d^2, zero at rest */
  int *span_min;     /* per row of the box: first column written, or INT_MAX */
  int *span_max;     /* per row of the box: last column written, or -1 */
  int width;         /* the box's, in pixels */
  int height;
  int x0;            /* the box's origin in the surface's pixel grid */
  int y0;
} _plane_t;

static GMutex _scratch_lock;
static float *_scratch_value = NULL;
static size_t _scratch_value_count = 0;
static int *_scratch_span_min = NULL;
static int *_scratch_span_max = NULL;
static int _scratch_span_rows = 0;

/* Take the scratch for a box of @p width x @p height, growing it -- zeroed -- as needed. */
static gboolean _plane_acquire(_plane_t *plane, const int x0, const int y0, const int width, const int height)
{
  const size_t count = (size_t)width * (size_t)height;
  if(count > _scratch_value_count)
  {
    float *grown = g_try_malloc0(count * sizeof(float));
    if(!grown) return FALSE;
    g_free(_scratch_value);
    _scratch_value = grown;
    _scratch_value_count = count;
  }
  if(height > _scratch_span_rows)
  {
    int *min_grown = g_try_malloc(sizeof(int) * (size_t)height);
    int *max_grown = g_try_malloc(sizeof(int) * (size_t)height);
    if(!min_grown || !max_grown)
    {
      g_free(min_grown);
      g_free(max_grown);
      return FALSE;
    }
    g_free(_scratch_span_min);
    g_free(_scratch_span_max);
    _scratch_span_min = min_grown;
    _scratch_span_max = max_grown;
    _scratch_span_rows = height;
  }
  for(int y = 0; y < height; y++)
  {
    _scratch_span_min[y] = INT_MAX;
    _scratch_span_max[y] = -1;
  }
  plane->value = _scratch_value;
  plane->span_min = _scratch_span_min;
  plane->span_max = _scratch_span_max;
  plane->width = width;
  plane->height = height;
  plane->x0 = x0;
  plane->y0 = y0;
  return TRUE;
}

/* Stamp the capsule of half-width @p reach around the segment (ax, ay)-(bx, by), in surface
 * pixels, into the plane. A capped end is round; an uncapped one is cut flat at the segment's
 * end plane, which is what a butt cap is. */
static inline void _plane_stamp_capsule(_plane_t *const plane, const double ax, const double ay, const double bx,
                                        const double by, const double reach, const gboolean cap_a,
                                        const gboolean cap_b)
{
  const double dx = bx - ax;
  const double dy = by - ay;
  const double len2 = dx * dx + dy * dy;
  const double reach2 = reach * reach;

  int x_first = (int)floor(MIN(ax, bx) - reach) - plane->x0;
  int x_last = (int)ceil(MAX(ax, bx) + reach) - plane->x0;
  int y_first = (int)floor(MIN(ay, by) - reach) - plane->y0;
  int y_last = (int)ceil(MAX(ay, by) + reach) - plane->y0;
  x_first = MAX(x_first, 0);
  y_first = MAX(y_first, 0);
  x_last = MIN(x_last, plane->width - 1);
  y_last = MIN(y_last, plane->height - 1);
  if(x_first > x_last || y_first > y_last) return;

  for(int y = y_first; y <= y_last; y++)
  {
    const double py = (double)(y + plane->y0) + 0.5;
    float *const row = plane->value + (size_t)y * plane->width;
    gboolean wrote = FALSE;
    for(int x = x_first; x <= x_last; x++)
    {
      const double px = (double)(x + plane->x0) + 0.5;
      double t = (len2 > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
      if(t < 0.0)
      {
        if(!cap_a) continue;
        t = 0.0;
      }
      else if(t > 1.0)
      {
        if(!cap_b) continue;
        t = 1.0;
      }
      const double ex = ax + t * dx - px;
      const double ey = ay + t * dy - py;
      const double d2 = ex * ex + ey * ey;
      if(d2 >= reach2) continue;
      const float inside = (float)(reach2 - d2);
      if(inside > row[x])
      {
        row[x] = inside;
        wrote = TRUE;
      }
    }
    if(wrote)
    {
      plane->span_min[y] = MIN(plane->span_min[y], x_first);
      plane->span_max[y] = MAX(plane->span_max[y], x_last);
    }
  }
}

/* ---------------------------------------------------------------------------------------------
 * Compositing: the two passes from the distance, premultiplied OVER, into ARGB32.
 *
 * ARGB32 is one native-endian uint32 per pixel, alpha in the top byte, colour premultiplied by
 * alpha. The dark pass goes first and the bright pass over it, each with coverage
 * clamp(R + 1/2 - d, 0, 1) for its own half-width R -- the one-pixel ramp of a fast antialiased
 * cairo stroke. A pass with alpha or width at zero contributes nothing. */
static inline uint32_t _pixel_pack(const double a, const double r, const double g, const double b)
{
  const uint32_t ia = (uint32_t)(a * 255.0 + 0.5);
  const uint32_t ir = (uint32_t)(r * 255.0 + 0.5);
  const uint32_t ig = (uint32_t)(g * 255.0 + 0.5);
  const uint32_t ib = (uint32_t)(b * 255.0 + 0.5);
  return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

static inline void _pixel_over(uint32_t *const pixel, const dt_stroke_pass_t *const pass, const double coverage)
{
  const double src_a = pass->alpha * coverage;
  if(src_a <= 0.0) return;
  const uint32_t dst = *pixel;
  const double keep = 1.0 - src_a;
  const double a = src_a + ((dst >> 24) & 0xff) / 255.0 * keep;
  const double r = pass->red * src_a + ((dst >> 16) & 0xff) / 255.0 * keep;
  const double g = pass->green * src_a + ((dst >> 8) & 0xff) / 255.0 * keep;
  const double b = pass->blue * src_a + (dst & 0xff) / 255.0 * keep;
  *pixel = _pixel_pack(MIN(a, 1.0), MIN(r, 1.0), MIN(g, 1.0), MIN(b, 1.0));
}

static void _plane_composite_and_clear(_plane_t *const plane, cairo_surface_t *surface, const dt_stroke_style_t *style,
                                       const double reach, cairo_rectangle_int_t *touched)
{
  const double reach2 = reach * reach;
  const double half_dark = 0.5 * style->dark.width;
  const double half_bright = 0.5 * style->bright.width;
  const gboolean with_dark = style->dark.width > 0.0 && style->dark.alpha > 0.0;
  const gboolean with_bright = style->bright.width > 0.0 && style->bright.alpha > 0.0;

  cairo_surface_flush(surface);
  uint8_t *const data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int surface_width = cairo_image_surface_get_width(surface);
  const int surface_height = cairo_image_surface_get_height(surface);

  int tx0 = INT_MAX;
  int ty0 = INT_MAX;
  int tx1 = -1;
  int ty1 = -1;
  for(int y = 0; y < plane->height; y++)
  {
    if(plane->span_max[y] < plane->span_min[y]) continue;
    const int sy = y + plane->y0;
    float *const row = plane->value + (size_t)y * plane->width;
    if(sy >= 0 && sy < surface_height)
    {
      uint32_t *const pixels = (uint32_t *)(data + (size_t)sy * stride);
      for(int x = plane->span_min[y]; x <= plane->span_max[y]; x++)
      {
        const float inside = row[x];
        if(inside <= 0.0f) continue;
        const int sx = x + plane->x0;
        if(sx < 0 || sx >= surface_width) continue;
        const double d = sqrt(MAX(reach2 - (double)inside, 0.0));
        if(with_dark) _pixel_over(&pixels[sx], &style->dark, CLAMP(half_dark + 0.5 - d, 0.0, 1.0));
        if(with_bright) _pixel_over(&pixels[sx], &style->bright, CLAMP(half_bright + 0.5 - d, 0.0, 1.0));
        tx0 = MIN(tx0, sx);
        tx1 = MAX(tx1, sx);
        ty0 = MIN(ty0, sy);
        ty1 = MAX(ty1, sy);
      }
    }
    /* back to rest: only what was written */
    memset(row + plane->span_min[y], 0, sizeof(float) * (size_t)(plane->span_max[y] - plane->span_min[y] + 1));
  }

  if(tx1 >= tx0 && ty1 >= ty0)
  {
    cairo_surface_mark_dirty_rectangle(surface, tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1);
    _touched_add(surface, tx0, ty0, tx1 + 1, ty1 + 1);
    if(touched)
    {
      touched->x = tx0;
      touched->y = ty0;
      touched->width = tx1 - tx0 + 1;
      touched->height = ty1 - ty0 + 1;
    }
  }
}

/* ---------------------------------------------------------------------------------------------
 * The walk along a polyline: dashes cut by arc length, capsules stamped. */
typedef struct _dash_state_t
{
  gboolean on;        /* inside a dash rather than a gap */
  double left;        /* what remains of the current dash or gap */
} _dash_state_t;

static void _polyline_stamp(_plane_t *const plane, const double *xy, const int count, const dt_stroke_style_t *style,
                            const double reach, const gboolean closed)
{
  const gboolean dashed = style->dash_on > 0.0 && style->dash_off > 0.0;
  _dash_state_t dash = { .on = TRUE, .left = dashed ? style->dash_on : DBL_MAX };
  /* The ends of the polyline: round when asked, flat otherwise; a closed polyline has none.
   * Every other piece end is a join or a dash cut inside the line, always capped so the joins
   * between consecutive segments are round and seamless. */
  const gboolean caps = style->round_caps;
  const int last = count - 1;
  /* a dash cut is a piece end; whether it is capped follows the cap style */
  for(int i = 0; i < last; i++)
  {
    const double ax = xy[2 * i];
    const double ay = xy[2 * i + 1];
    const double bx = xy[2 * i + 2];
    const double by = xy[2 * i + 3];
    const double length = hypot(bx - ax, by - ay);
    const gboolean starts_line = (i == 0) && !closed;
    const gboolean ends_line = (i == last - 1) && !closed;

    if(!dashed)
    {
      _plane_stamp_capsule(plane, ax, ay, bx, by, reach, caps || !starts_line, caps || !ends_line);
      continue;
    }

    double pos = 0.0;
    gboolean piece_begins_here = starts_line;   /* the current on-piece started at the line's start */
    while(pos < length)
    {
      const double run = MIN(dash.left, length - pos);
      const double end = pos + run;
      if(dash.on)
      {
        const double t0 = pos / length;
        const double t1 = end / length;
        const gboolean piece_starts = (pos == 0.0) ? piece_begins_here : TRUE;   /* a cut: a dash start */
        const gboolean piece_ends_at_cut = (end < length) || (ends_line && end >= length);
        const gboolean cap_a = caps || !(piece_starts);
        const gboolean cap_b = caps || !piece_ends_at_cut;
        _plane_stamp_capsule(plane, ax + t0 * (bx - ax), ay + t0 * (by - ay), ax + t1 * (bx - ax),
                             ay + t1 * (by - ay), reach, cap_a, cap_b);
      }
      dash.left -= run;
      pos = end;
      if(dash.left <= 0.0)
      {
        dash.on = !dash.on;
        dash.left = dash.on ? style->dash_on : style->dash_off;
        piece_begins_here = TRUE;
      }
      else
        piece_begins_here = FALSE;
    }
  }
  if(count == 1 && caps)
    _plane_stamp_capsule(plane, xy[0], xy[1], xy[0], xy[1], reach, TRUE, TRUE);   /* a dot */
}

static gboolean _stroke_polyline(cairo_surface_t *surface, const double *xy, const int count,
                                 const dt_stroke_style_t *style, const gboolean closed)
{
  if(count < 1) return FALSE;
  const double widest = MAX(style->dark.width, style->bright.width);
  if(widest <= 0.0) return FALSE;
  const double reach = 0.5 * widest + 1.0;   /* the antialiasing pixel beyond the widest pass */

  double x_min = DBL_MAX;
  double y_min = DBL_MAX;
  double x_max = -DBL_MAX;
  double y_max = -DBL_MAX;
  for(int i = 0; i < count; i++)
  {
    x_min = MIN(x_min, xy[2 * i]);
    x_max = MAX(x_max, xy[2 * i]);
    y_min = MIN(y_min, xy[2 * i + 1]);
    y_max = MAX(y_max, xy[2 * i + 1]);
  }
  if(!isfinite(x_min) || !isfinite(x_max) || !isfinite(y_min) || !isfinite(y_max)) return FALSE;

  /* the box: the polyline's, grown by the reach, clipped to the surface */
  const int surface_width = cairo_image_surface_get_width(surface);
  const int surface_height = cairo_image_surface_get_height(surface);
  const int x0 = MAX((int)floor(x_min - reach) - 1, 0);
  const int y0 = MAX((int)floor(y_min - reach) - 1, 0);
  const int x1 = MIN((int)ceil(x_max + reach) + 1, surface_width - 1);
  const int y1 = MIN((int)ceil(y_max + reach) + 1, surface_height - 1);
  if(x1 < x0 || y1 < y0) return FALSE;   /* entirely off the surface */

  g_mutex_lock(&_scratch_lock);
  _plane_t plane;
  gboolean ok = _plane_acquire(&plane, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  if(ok)
  {
    _polyline_stamp(&plane, xy, count, style, reach, closed);
    _plane_composite_and_clear(&plane, surface, style, reach, NULL);
  }
  g_mutex_unlock(&_scratch_lock);
  return ok;
}

gboolean dt_stroke_raster_polyline(cairo_surface_t *surface, const double *xy, int count,
                                   const dt_stroke_style_t *style)
{
  if(!dt_stroke_raster_can_paint(surface) || !xy || !style || count < 1) return FALSE;
  return _stroke_polyline(surface, xy, count, style, FALSE);
}

/* ---------------------------------------------------------------------------------------------
 * From a cairo path. */

/* How much cr's matrix scales a length, taken as the geometric mean of the two axes so an
 * anisotropic matrix -- which no overlay has -- degrades gracefully. */
static double _matrix_scale(cairo_t *cr)
{
  double ax = 1.0;
  double ay = 0.0;
  double bx = 0.0;
  double by = 1.0;
  cairo_user_to_device_distance(cr, &ax, &ay);
  cairo_user_to_device_distance(cr, &bx, &by);
  const double scale = sqrt(hypot(ax, ay) * hypot(bx, by));
  return isfinite(scale) ? scale : 1.0;
}

gboolean dt_stroke_raster_path(cairo_t *cr, const dt_stroke_style_t *style)
{
  if(!cr || !style) return FALSE;
  cairo_surface_t *surface = cairo_get_group_target(cr);
  if(!dt_stroke_raster_can_paint(surface)) return FALSE;

  cairo_path_t *path = cairo_copy_path_flat(cr);
  if(!path || path->status != CAIRO_STATUS_SUCCESS)
  {
    if(path) cairo_path_destroy(path);
    return FALSE;
  }

  /* the surface's pixel grid is device space shifted by the surface's device offset: for the
   * group cairo pushed, that is the clip's origin */
  double offset_x = 0.0;
  double offset_y = 0.0;
  cairo_surface_get_device_offset(surface, &offset_x, &offset_y);

  const double scale = _matrix_scale(cr);
  dt_stroke_style_t device_style = *style;
  device_style.dark.width *= scale;
  device_style.bright.width *= scale;
  device_style.dash_on *= scale;
  device_style.dash_off *= scale;

  GArray *vertices = g_array_sized_new(FALSE, FALSE, sizeof(double), 2 * 1024);
  gboolean closed = FALSE;
  double first_x = 0.0;
  double first_y = 0.0;
  for(int i = 0; i < path->num_data; i += path->data[i].header.length)
  {
    const cairo_path_data_t *const element = &path->data[i];
    const cairo_path_data_type_t type = element->header.type;
    if(type == CAIRO_PATH_MOVE_TO)
    {
      /* a move ends the polyline before it and starts the next one at its point */
      if(vertices->len >= 2)
        _stroke_polyline(surface, (const double *)vertices->data, (int)(vertices->len / 2), &device_style, closed);
      g_array_set_size(vertices, 0);
      closed = FALSE;
    }
    switch(type)
    {
      case CAIRO_PATH_MOVE_TO:
      case CAIRO_PATH_LINE_TO:
      {
        double x = element[1].point.x;
        double y = element[1].point.y;
        cairo_user_to_device(cr, &x, &y);
        x += offset_x;
        y += offset_y;
        if(vertices->len == 0)
        {
          first_x = x;
          first_y = y;
        }
        g_array_append_val(vertices, x);
        g_array_append_val(vertices, y);
        break;
      }
      case CAIRO_PATH_CLOSE_PATH:
        if(vertices->len >= 2)
        {
          g_array_append_val(vertices, first_x);
          g_array_append_val(vertices, first_y);
          closed = TRUE;
        }
        break;
      case CAIRO_PATH_CURVE_TO:
      default:
        break;   /* a flattened path has no curves */
    }
  }
  if(vertices->len >= 2)
    _stroke_polyline(surface, (const double *)vertices->data, (int)(vertices->len / 2), &device_style, closed);

  g_array_free(vertices, TRUE);
  cairo_path_destroy(path);
  cairo_new_path(cr);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
