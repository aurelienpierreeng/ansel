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

/* This file holds no state of its own: src/widgets keeps none outside its two registries, and
 * a rasteriser has no business remembering anything between two strokes. What it needs across
 * a call -- a scratch plane -- and across a frame -- what was touched -- lives on the surface
 * being painted, as cairo user data, and dies with it. */

/* ---------------------------------------------------------------------------------------------
 * The touched rectangle, kept on the surface it describes.
 *
 * A surface carries its own record of what was painted into it here, so a caller that owns the
 * surface can composite or clear exactly that much: the rasteriser is the one thing that knows
 * where its pixels went. Half-open pixel ranges, [x0, x1) x [y0, y1). */
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
 * The distance plane, and the scratch it is cut from.
 *
 * One plane over the polyline's bounding box, holding per pixel not the distance but
 * `reach^2 - d^2', where reach is the widest pass's half-width plus the antialiasing pixel:
 * positive inside the stroke's reach, zero outside and for anything never stamped. Zero being
 * the resting state is what makes the scratch cheap to keep: it is never cleared as a whole,
 * only the spans a stroke actually wrote are zeroed again once composited, and memory that
 * only ever grows starts every new byte at zero. Stamping is MAX, which for this quantity is
 * the nearest point of the polyline, and squared distances need no square root until
 * compositing, which touches only the pixels of the stroke's band and not the box around it.
 *
 * Per row, the extent that was written, so compositing and the clearing after it walk the band
 * rather than the box. The scratch belongs to the surface, grows to the largest box that
 * surface ever asked for, and is freed with it. */
typedef struct _scratch_t
{
  float *value;
  size_t count;
  int *span_min;
  int *span_max;
  int rows;
} _scratch_t;

static const cairo_user_data_key_t _scratch_key = { 0 };

static void _scratch_free(void *data)
{
  _scratch_t *scratch = (_scratch_t *)data;
  if(!scratch) return;
  g_free(scratch->value);
  g_free(scratch->span_min);
  g_free(scratch->span_max);
  g_free(scratch);
}

static _scratch_t *_scratch_of(cairo_surface_t *surface)
{
  _scratch_t *scratch = (_scratch_t *)cairo_surface_get_user_data(surface, &_scratch_key);
  if(scratch) return scratch;
  scratch = g_malloc0(sizeof(_scratch_t));
  if(cairo_surface_set_user_data(surface, &_scratch_key, scratch, _scratch_free) != CAIRO_STATUS_SUCCESS)
  {
    g_free(scratch);
    return NULL;
  }
  return scratch;
}

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

/* Cut a plane for a box of @p width x @p height from the surface's scratch, growing it --
 * zeroed -- as needed. */
static gboolean _plane_acquire(cairo_surface_t *surface, _plane_t *plane, const int x0, const int y0,
                               const int width, const int height)
{
  _scratch_t *scratch = _scratch_of(surface);
  if(!scratch) return FALSE;
  const size_t count = (size_t)width * (size_t)height;
  if(count > scratch->count)
  {
    float *grown = g_try_malloc0(count * sizeof(float));
    if(!grown) return FALSE;
    g_free(scratch->value);
    scratch->value = grown;
    scratch->count = count;
  }
  if(height > scratch->rows)
  {
    int *min_grown = g_try_malloc(sizeof(int) * (size_t)height);
    int *max_grown = g_try_malloc(sizeof(int) * (size_t)height);
    if(!min_grown || !max_grown)
    {
      g_free(min_grown);
      g_free(max_grown);
      return FALSE;
    }
    g_free(scratch->span_min);
    g_free(scratch->span_max);
    scratch->span_min = min_grown;
    scratch->span_max = max_grown;
    scratch->rows = height;
  }
  for(int y = 0; y < height; y++)
  {
    scratch->span_min[y] = INT_MAX;
    scratch->span_max[y] = -1;
  }
  plane->value = scratch->value;
  plane->span_min = scratch->span_min;
  plane->span_max = scratch->span_max;
  plane->width = width;
  plane->height = height;
  plane->x0 = x0;
  plane->y0 = y0;
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------
 * Stamping: the capsule of a segment. */

/* One segment of a polyline, in surface pixels, with what its ends are: a capped end is round,
 * an uncapped one is cut flat at the segment's end plane, which is what a butt cap is. Every
 * end inside a polyline is capped, so consecutive segments join round and seamless. */
typedef struct _segment_t
{
  double ax;
  double ay;
  double bx;
  double by;
  gboolean cap_a;
  gboolean cap_b;
} _segment_t;

/* One row of the capsule: every pixel of [x_first, x_last] within reach of the segment takes
 * the nearest distance. Returns whether any did. */
static inline gboolean _plane_stamp_row(_plane_t *const plane, const int y, const int x_first, const int x_last,
                                        const _segment_t *const seg, const double reach2)
{
  const double dx = seg->bx - seg->ax;
  const double dy = seg->by - seg->ay;
  const double len2 = dx * dx + dy * dy;
  const double py = (double)(y + plane->y0) + 0.5;
  float *const row = plane->value + (size_t)y * plane->width;
  gboolean wrote = FALSE;
  for(int x = x_first; x <= x_last; x++)
  {
    const double px = (double)(x + plane->x0) + 0.5;
    double t = (len2 > 0.0) ? ((px - seg->ax) * dx + (py - seg->ay) * dy) / len2 : 0.0;
    if(t < 0.0 && !seg->cap_a) continue;
    if(t > 1.0 && !seg->cap_b) continue;
    t = CLAMP(t, 0.0, 1.0);
    const double ex = seg->ax + t * dx - px;
    const double ey = seg->ay + t * dy - py;
    const double d2 = ex * ex + ey * ey;
    if(d2 >= reach2) continue;
    const float inside = (float)(reach2 - d2);
    if(inside <= row[x]) continue;
    row[x] = inside;
    wrote = TRUE;
  }
  return wrote;
}

/* Stamp the capsule of half-width @p reach around @p seg into the plane. */
static inline void _plane_stamp_capsule(_plane_t *const plane, const _segment_t *const seg, const double reach)
{
  const double reach2 = reach * reach;
  const int x_first = MAX((int)floor(MIN(seg->ax, seg->bx) - reach) - plane->x0, 0);
  const int x_last = MIN((int)ceil(MAX(seg->ax, seg->bx) + reach) - plane->x0, plane->width - 1);
  const int y_first = MAX((int)floor(MIN(seg->ay, seg->by) - reach) - plane->y0, 0);
  const int y_last = MIN((int)ceil(MAX(seg->ay, seg->by) + reach) - plane->y0, plane->height - 1);
  if(x_first > x_last || y_first > y_last) return;

  for(int y = y_first; y <= y_last; y++)
  {
    if(!_plane_stamp_row(plane, y, x_first, x_last, seg, reach2)) continue;
    plane->span_min[y] = MIN(plane->span_min[y], x_first);
    plane->span_max[y] = MAX(plane->span_max[y], x_last);
  }
}

/* ---------------------------------------------------------------------------------------------
 * The walk along a polyline: dashes cut by arc length, capsules stamped. */

/* Where the walk is in the dash pattern. */
typedef struct _dash_t
{
  gboolean on;          /* inside a dash rather than a gap */
  double left;          /* what remains of the current dash or gap */
  gboolean fresh;       /* the current dash started at a cut, not at the line's start */
} _dash_t;

/* The dashed pieces of one segment. A piece that starts at a cut, or ends at one, gets the
 * cap style's end there; one that continues from or into the neighbouring segment is a join,
 * always capped so the two halves meet round. */
static void _plane_stamp_dashed(_plane_t *const plane, const _segment_t *const seg, const dt_stroke_style_t *style,
                                const double reach, _dash_t *const dash)
{
  const double length = hypot(seg->bx - seg->ax, seg->by - seg->ay);
  const gboolean caps = style->round_caps;
  double pos = 0.0;
  gboolean piece_starts_at_cut = dash->fresh;   /* the line's own start counts as a cut */
  while(pos < length)
  {
    const double run = MIN(dash->left, length - pos);
    const double end = pos + run;
    if(dash->on)
    {
      const double t0 = pos / length;
      const double t1 = end / length;
      const gboolean ends_at_cut = (end < length) || seg->cap_b == FALSE;
      const _segment_t piece = { .ax = seg->ax + t0 * (seg->bx - seg->ax),
                                 .ay = seg->ay + t0 * (seg->by - seg->ay),
                                 .bx = seg->ax + t1 * (seg->bx - seg->ax),
                                 .by = seg->ay + t1 * (seg->by - seg->ay),
                                 .cap_a = caps || !(piece_starts_at_cut || (pos == 0.0 && !seg->cap_a)),
                                 .cap_b = caps || !ends_at_cut };
      _plane_stamp_capsule(plane, &piece, reach);
    }
    dash->left -= run;
    pos = end;
    if(dash->left <= 0.0)
    {
      dash->on = !dash->on;
      dash->left = dash->on ? style->dash_on : style->dash_off;
      piece_starts_at_cut = TRUE;
    }
    else
      piece_starts_at_cut = FALSE;
  }
  /* the next segment continues whatever this one was in, unless a cut fell exactly at its end */
  dash->fresh = piece_starts_at_cut;
}

static void _polyline_stamp(_plane_t *const plane, const double *xy, const int count, const dt_stroke_style_t *style,
                            const double reach, const gboolean closed, _dash_t *const dash)
{
  const gboolean caps = style->round_caps;
  if(count == 1)
  {
    if(!caps) return;
    const _segment_t dot = { xy[0], xy[1], xy[0], xy[1], TRUE, TRUE };
    _plane_stamp_capsule(plane, &dot, reach);
    return;
  }
  const gboolean dashed = style->dash_on > 0.0 && style->dash_off > 0.0;
  const int last = count - 1;
  for(int i = 0; i < last; i++)
  {
    /* the polyline's own two ends take the cap style; a closed one has none */
    const gboolean starts_line = (i == 0) && !closed;
    const gboolean ends_line = (i == last - 1) && !closed;
    const _segment_t seg = { .ax = xy[2 * i],
                             .ay = xy[2 * i + 1],
                             .bx = xy[2 * i + 2],
                             .by = xy[2 * i + 3],
                             .cap_a = caps || !starts_line,
                             .cap_b = caps || !ends_line };
    if(dashed)
      _plane_stamp_dashed(plane, &seg, style, reach, dash);
    else
      _plane_stamp_capsule(plane, &seg, reach);
  }
}

/* The dash pattern at the start of a stroke: a dash, with its whole length ahead. */
static _dash_t _dash_start(const dt_stroke_style_t *const style)
{
  const _dash_t dash = { .on = TRUE, .left = style->dash_on, .fresh = TRUE };
  return dash;
}

/* ---------------------------------------------------------------------------------------------
 * Compositing: the two passes from the distance, premultiplied OVER, into ARGB32.
 *
 * ARGB32 is one native-endian uint32 per pixel, alpha in the top byte, colour premultiplied by
 * alpha. The dark pass goes first and the bright pass over it, each with coverage
 * clamp(R + 1/2 - d, 0, 1) for its own half-width R -- the one-pixel ramp of a fast antialiased
 * cairo stroke. A pass with alpha or width at zero contributes nothing. */
typedef struct _composite_t
{
  const dt_stroke_pass_t *dark;     /* NULL when the pass contributes nothing */
  const dt_stroke_pass_t *bright;
  double reach2;
  double half_dark;
  double half_bright;
  uint8_t *data;
  int stride;
  int surface_width;
  int surface_height;
} _composite_t;

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

/* One row's span: composite what was stamped, and widen the touched range [tx0, tx1) to it.
 * Returns whether any pixel was painted. */
static inline gboolean _composite_row(const _plane_t *const plane, const int y, const _composite_t *const c,
                                      int *const tx0, int *const tx1)
{
  const int sy = y + plane->y0;
  if(sy < 0 || sy >= c->surface_height) return FALSE;
  const float *const row = plane->value + (size_t)y * plane->width;
  uint32_t *const pixels = (uint32_t *)(c->data + (size_t)sy * c->stride);
  gboolean painted = FALSE;
  for(int x = plane->span_min[y]; x <= plane->span_max[y]; x++)
  {
    const float inside = row[x];
    if(inside <= 0.0f) continue;
    const int sx = x + plane->x0;
    if(sx < 0 || sx >= c->surface_width) continue;
    const double d = sqrt(MAX(c->reach2 - (double)inside, 0.0));
    if(c->dark) _pixel_over(&pixels[sx], c->dark, CLAMP(c->half_dark + 0.5 - d, 0.0, 1.0));
    if(c->bright) _pixel_over(&pixels[sx], c->bright, CLAMP(c->half_bright + 0.5 - d, 0.0, 1.0));
    *tx0 = MIN(*tx0, sx);
    *tx1 = MAX(*tx1, sx + 1);
    painted = TRUE;
  }
  return painted;
}

static void _plane_composite_and_clear(_plane_t *const plane, cairo_surface_t *surface, const dt_stroke_style_t *style,
                                       const double reach)
{
  cairo_surface_flush(surface);
  const gboolean with_dark = style->dark.width > 0.0 && style->dark.alpha > 0.0;
  const gboolean with_bright = style->bright.width > 0.0 && style->bright.alpha > 0.0;
  const _composite_t c = { .dark = with_dark ? &style->dark : NULL,
                           .bright = with_bright ? &style->bright : NULL,
                           .reach2 = reach * reach,
                           .half_dark = 0.5 * style->dark.width,
                           .half_bright = 0.5 * style->bright.width,
                           .data = cairo_image_surface_get_data(surface),
                           .stride = cairo_image_surface_get_stride(surface),
                           .surface_width = cairo_image_surface_get_width(surface),
                           .surface_height = cairo_image_surface_get_height(surface) };

  int tx0 = INT_MAX;
  int tx1 = -1;
  int ty0 = INT_MAX;
  int ty1 = -1;
  for(int y = 0; y < plane->height; y++)
  {
    if(plane->span_max[y] < plane->span_min[y]) continue;
    if(_composite_row(plane, y, &c, &tx0, &tx1))
    {
      ty0 = MIN(ty0, y + plane->y0);
      ty1 = MAX(ty1, y + plane->y0 + 1);
    }
    /* back to rest: only what was written */
    memset(plane->value + (size_t)y * plane->width + plane->span_min[y], 0,
           sizeof(float) * (size_t)(plane->span_max[y] - plane->span_min[y] + 1));
  }

  if(tx1 > tx0 && ty1 > ty0)
  {
    cairo_surface_mark_dirty_rectangle(surface, tx0, ty0, tx1 - tx0, ty1 - ty0);
    _touched_add(surface, tx0, ty0, tx1, ty1);
  }
}

/* ---------------------------------------------------------------------------------------------
 * A polyline, and a path's worth of them. */

/* Stroke one polyline; @p dash is the pattern's state, carried across the sub-paths of one
 * stroke so that a dash is a function of the arc length along everything drawn and not of
 * where a sub-path happened to start. An outline is many sub-paths -- one per run between the
 * stretches the boundary pass hides -- and restarting the pattern at each bunched and stretched
 * the dashes at every run boundary. */
static gboolean _stroke_polyline(cairo_surface_t *surface, const double *xy, const int count,
                                 const dt_stroke_style_t *style, const gboolean closed, _dash_t *const dash)
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

  _plane_t plane;
  if(!_plane_acquire(surface, &plane, x0, y0, x1 - x0 + 1, y1 - y0 + 1)) return FALSE;
  _polyline_stamp(&plane, xy, count, style, reach, closed, dash);
  _plane_composite_and_clear(&plane, surface, style, reach);
  return TRUE;
}

gboolean dt_stroke_raster_polyline(cairo_surface_t *surface, const double *xy, int count,
                                   const dt_stroke_style_t *style)
{
  if(!dt_stroke_raster_can_paint(surface) || !xy || !style || count < 1) return FALSE;
  _dash_t dash = _dash_start(style);
  return _stroke_polyline(surface, xy, count, style, FALSE, &dash);
}

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

/* The polyline being gathered from a path: its vertices in surface pixels, and its first one
 * for a close.
 *
 * Cairo's "device space" is not the pixel grid. It is the space the CTM maps user space into,
 * and the surface then applies its own device transform: pixel = device * device_scale +
 * device_offset, the scale being what a HiDPI widget carries (2 on a 2x screen) and the offset
 * what a pushed group carries (minus its clip's origin, in pixels). cairo_user_to_device()
 * stops at device space -- measured: on a surface with device scale 2, (10, 10) maps to
 * (10, 10) -- so both factors are applied here. Leaving the scale out put every overlay at half
 * size in the top-left quadrant of a HiDPI view. */
typedef struct _gather_t
{
  GArray *vertices;
  double first_x;
  double first_y;
  gboolean closed;
  double scale_x;    /* the surface's device scale */
  double scale_y;
  double offset_x;   /* the surface's device offset, in pixels */
  double offset_y;
} _gather_t;

static void _gather_flush(_gather_t *const g, cairo_surface_t *surface, const dt_stroke_style_t *style,
                          _dash_t *const dash)
{
  if(g->vertices->len >= 2)
    _stroke_polyline(surface, (const double *)g->vertices->data, (int)(g->vertices->len / 2), style, g->closed, dash);
  g_array_set_size(g->vertices, 0);
  g->closed = FALSE;
}

static void _gather_point(_gather_t *const g, cairo_t *cr, const cairo_path_data_t *const point)
{
  double x = point->point.x;
  double y = point->point.y;
  cairo_user_to_device(cr, &x, &y);
  x = x * g->scale_x + g->offset_x;
  y = y * g->scale_y + g->offset_y;
  if(g->vertices->len == 0)
  {
    g->first_x = x;
    g->first_y = y;
  }
  g_array_append_val(g->vertices, x);
  g_array_append_val(g->vertices, y);
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

  /* the surface's pixel grid is device space scaled by the surface's device scale and shifted
   * by its device offset: for the group cairo pushed, that is the clip's origin */
  _gather_t gather = { .vertices = g_array_sized_new(FALSE, FALSE, sizeof(double), 2 * 1024),
                       .scale_x = 1.0,
                       .scale_y = 1.0 };
  cairo_surface_get_device_scale(surface, &gather.scale_x, &gather.scale_y);
  cairo_surface_get_device_offset(surface, &gather.offset_x, &gather.offset_y);
  if(gather.scale_x <= 0.0) gather.scale_x = 1.0;
  if(gather.scale_y <= 0.0) gather.scale_y = 1.0;

  const double scale = _matrix_scale(cr) * sqrt(gather.scale_x * gather.scale_y);
  dt_stroke_style_t device_style = *style;
  device_style.dark.width *= scale;
  device_style.bright.width *= scale;
  device_style.dash_on *= scale;
  device_style.dash_off *= scale;
  _dash_t dash = _dash_start(&device_style);   /* one pattern for the whole path */

  for(int i = 0; i < path->num_data; i += path->data[i].header.length)
  {
    const cairo_path_data_t *const element = &path->data[i];
    switch(element->header.type)
    {
      case CAIRO_PATH_MOVE_TO:
        _gather_flush(&gather, surface, &device_style, &dash);
        _gather_point(&gather, cr, &element[1]);
        break;
      case CAIRO_PATH_LINE_TO:
        _gather_point(&gather, cr, &element[1]);
        break;
      case CAIRO_PATH_CLOSE_PATH:
        if(gather.vertices->len >= 2)
        {
          g_array_append_val(gather.vertices, gather.first_x);
          g_array_append_val(gather.vertices, gather.first_y);
          gather.closed = TRUE;
        }
        break;
      default:
        break;   /* a flattened path has no curves */
    }
  }
  _gather_flush(&gather, surface, &device_style, &dash);

  g_array_free(gather.vertices, TRUE);
  cairo_path_destroy(path);
  cairo_new_path(cr);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
