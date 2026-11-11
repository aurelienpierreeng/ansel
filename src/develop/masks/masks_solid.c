/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

/* The edge of a stroke's solid part.
 *
 * A brush is painted as spokes, each solid from its spine sample over the first `fading' of its
 * length. Where the spokes lie along the spine -- a size that changes steeply, a small fading on a
 * wide node -- the edge of their union is made of the spokes' sides, not of the line through their
 * ends, so no walk over the ends can trace it. The union is filled instead and its edge traced.
 *
 * The bands (c_k, f_k, f_k+1, c_k+1) between neighbouring spokes are grouped into runs of one
 * orientation, each run a polygon -- spine forward, contour back -- wound positively, so the
 * union is where the winding is not zero. It is filled a few sub-scanlines per cell row with exact
 * crossings, and its edge followed loop by loop by marching squares at half coverage. */

#include "develop/masks/masks_solid.h"
#include "develop/masks_gui.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/times.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Sub-scanlines per cell row: the half-coverage level lands within an eighth of a cell.
#define SOLID_SUBLINES 4
// The most cells the grid holds and crossings the fill stores; past either, the cells grow.
#define SOLID_MAX_CELLS ((size_t)8 << 20)
#define SOLID_MAX_CROSSINGS ((size_t)32 << 20)
// How far, in cells, a run's bands sweep back before the run turns over.
#define SOLID_DRIFT 0.1f
// Up to this many crossings, a sub-scanline is sorted by insertion.
#define SOLID_INSERTION 32

enum { SOLID_TOP = 0, SOLID_RIGHT = 1, SOLID_BOTTOM = 2, SOLID_LEFT = 3 };

typedef struct _solid_edge_t
{
  float x;      // upper end, image pixels
  float y;
  float y_end;  // lower end
  float slope;  // dx / dy
  int dir;      // winding, +1 or -1
} _solid_edge_t;

typedef struct _solid_crossing_t
{
  float x;   // cells
  int dir;
} _solid_crossing_t;

typedef struct _solid_bands_t
{
  const float *spine;     // samples, the header skipped
  const float *contour;   // index-aligned with spine
  int n;                  // samples
  int count;              // bands: n when the walk closes, n - 1 otherwise
  float cell;             // image pixels
} _solid_bands_t;

typedef struct _solid_grid_t
{
  float x0;       // image position of the grid's corner
  float y0;
  float cell;     // a cell's side, image pixels
  int w;
  int h;
} _solid_grid_t;

/* Where band k's two spokes cross, if they do, as the fraction @p t of the first: such a band is a
 * bow tie, its two lobes wound opposite ways. */
static inline gboolean _solid_spokes_cross(const _solid_bands_t *const b, const int k, const int l, float *const t)
{
  const float *const c0 = b->spine + 2 * k;
  const float *const c1 = b->spine + 2 * l;
  const float ax = b->contour[2 * k] - c0[0];
  const float ay = b->contour[2 * k + 1] - c0[1];
  const float bx = b->contour[2 * l] - c1[0];
  const float by = b->contour[2 * l + 1] - c1[1];
  const float wx = c1[0] - c0[0];
  const float wy = c1[1] - c0[1];
  const float den = ax * by - ay * bx;
  if(den == 0.0f) return FALSE;
  *t = (wx * by - wy * bx) / den;
  const float u = (wx * ay - wy * ax) / den;
  return *t > 0.0f && *t < 1.0f && u > 0.0f && u < 1.0f;
}

/* Each band's orientation, +1 or -1, or 0 for a band skipped -- one with a vertex that is not
 * finite, or too thin to tell before any run has begun -- which cuts the run. The sign is the area's,
 * taken about the band's own spine sample: about the origin, thousands of pixels out, float
 * cancellation decides it. A run turns over only once its bands have swept back SOLID_DRIFT of a
 * cell, as width, area over perimeter: a long spoke trembling by a hundredth of a pixel would turn
 * it at every band, and every turn adds that spoke to the fill. Extends @p bbox by every band kept
 * and counts the bow ties among them into @p twisted; returns how many runs. */
static int _solid_band_signs(const _solid_bands_t *const b, int8_t *const sign, float bbox[4], int *const twisted)
{
  const float *const spine = b->spine;
  const float *const contour = b->contour;
  const int n = b->n;
  const float drift_max = SOLID_DRIFT * b->cell;
  int runs = 0;
  int8_t current = 0;
  float drift = 0.0f;
  *twisted = 0;
  for(int k = 0; k < b->count; k++)
  {
    const int l = (k + 1 == n) ? 0 : k + 1;
    const float cx = spine[2 * k];
    const float cy = spine[2 * k + 1];
    const float f0x = contour[2 * k] - cx;
    const float f0y = contour[2 * k + 1] - cy;
    const float f1x = contour[2 * l] - cx;
    const float f1y = contour[2 * l + 1] - cy;
    const float c1x = spine[2 * l] - cx;
    const float c1y = spine[2 * l + 1] - cy;
    const float area2 = (f0x * f1y - f1x * f0y) + (f1x * c1y - c1x * f1y);
    const float perimeter = fabsf(f0x) + fabsf(f0y) + fabsf(f1x - f0x) + fabsf(f1y - f0y) + fabsf(f1x - c1x)
                            + fabsf(f1y - c1y) + fabsf(c1x) + fabsf(c1y);
    const float width = area2 / fmaxf(perimeter, FLT_MIN);
    const int8_t was = current;
    drift = (current == 0) ? 0.0f : fmaxf(0.0f, drift - current * width);
    if(!isfinite(width))
      current = 0;
    else if(current == 0 ? fabsf(width) > drift_max : drift > drift_max)
    {
      current = (width > 0.0f) ? 1 : -1;
      drift = 0.0f;
    }
    sign[k] = current;
    if(current == 0) continue;
    if(current != was) runs++;
    float t = 0.0f;
    *twisted += _solid_spokes_cross(b, k, l, &t);
    const int ends[2] = { k, l };
    for(int q = 0; q < 2; q++)
    {
      const int s = ends[q];
      bbox[0] = fminf(bbox[0], fminf(spine[2 * s], contour[2 * s]));
      bbox[1] = fmaxf(bbox[1], fmaxf(spine[2 * s], contour[2 * s]));
      bbox[2] = fminf(bbox[2], fminf(spine[2 * s + 1], contour[2 * s + 1]));
      bbox[3] = fmaxf(bbox[3], fmaxf(spine[2 * s + 1], contour[2 * s + 1]));
    }
  }
  return runs;
}

static int _solid_add_edge(_solid_edge_t *const e, const float xa, const float ya, const float xb,
                           const float yb, const int winding)
{
  if(ya == yb) return 0;
  const gboolean down = ya < yb;
  e->x = down ? xa : xb;
  e->y = down ? ya : yb;
  e->y_end = down ? yb : ya;
  e->slope = (xb - xa) / (yb - ya);
  e->dir = down ? winding : -winding;
  return 1;
}

/* A bow tie's lobe wound against its run counts -1 where the run counts +1, and cancels a band it
 * lies on: a hole in the solid part. That lobe is added again the other way, twice. A run walks a
 * band c0, c1, f1, f0; its lobes are (c0, c1, X) and (X, f1, f0), X where the spokes cross.
 * Returns how many edges. */
static int _solid_untwist(const _solid_bands_t *const b, const int k, const int l, const float t, const int s,
                          _solid_edge_t *const edges)
{
  const float *const c0 = b->spine + 2 * k;
  const float *const c1 = b->spine + 2 * l;
  const float *const f0 = b->contour + 2 * k;
  const float *const f1 = b->contour + 2 * l;
  const float x = c0[0] + t * (f0[0] - c0[0]);
  const float y = c0[1] + t * (f0[1] - c0[1]);
  // The spine's lobe as the band's own area counts it (c0, X, c1): against the run when its sign is
  // not s. Either lobe is then walked v1, v2, X.
  const float spine_lobe = (x - c0[0]) * (c1[1] - c0[1]) - (y - c0[1]) * (c1[0] - c0[0]);
  const gboolean spine_side = (spine_lobe > 0.0f) != (s > 0);
  const float *const v1 = spine_side ? c0 : f1;
  const float *const v2 = spine_side ? c1 : f0;
  int count = _solid_add_edge(edges, v1[0], v1[1], v2[0], v2[1], -2 * s);
  count += _solid_add_edge(edges + count, v2[0], v2[1], x, y, -2 * s);
  count += _solid_add_edge(edges + count, x, y, v1[0], v1[1], -2 * s);
  return count;
}

/* The runs' polygons as one edge list: the order of the edges is irrelevant to a winding count,
 * so each band adds its spine and contour edges as it comes, each run its two end spokes.
 * Returns how many edges; horizontal ones cross no scanline and are not kept. */
static int _solid_edges(const _solid_bands_t *const bands, const int8_t *const sign, _solid_edge_t *const edges)
{
  const float *const spine = bands->spine;
  const float *const contour = bands->contour;
  const int n = bands->n;
  int count = 0;
  int a = 0;
  gboolean joined = FALSE;   // this run's first spoke ended the run before, and is already in
  while(a < bands->count)
  {
    const int s = sign[a];
    int b = a;
    for(; b < bands->count && sign[b] == s; b++)
    {
      if(s == 0) continue;
      const int l = (b + 1 == n) ? 0 : b + 1;
      count += _solid_add_edge(edges + count, spine[2 * b], spine[2 * b + 1], spine[2 * l], spine[2 * l + 1], s);
      count += _solid_add_edge(edges + count, contour[2 * l], contour[2 * l + 1], contour[2 * b],
                               contour[2 * b + 1], s);
      float t = 0.0f;
      if(_solid_spokes_cross(bands, b, l, &t)) count += _solid_untwist(bands, b, l, t, s, edges + count);
    }
    // Two runs that meet share a spoke, which each walks its own way under its own sign: the same
    // edge twice, added once with both windings.
    const gboolean next = (s != 0) && b < bands->count && sign[b] != 0;
    if(s != 0)
    {
      const int e = (b == n) ? 0 : b;
      count += _solid_add_edge(edges + count, spine[2 * e], spine[2 * e + 1], contour[2 * e], contour[2 * e + 1],
                               next ? 2 * s : s);
      if(!joined)
        count += _solid_add_edge(edges + count, contour[2 * a], contour[2 * a + 1], spine[2 * a], spine[2 * a + 1], s);
    }
    joined = next;
    a = b;
  }
  return count;
}

/* Two empty cells round the box, so no sample on the grid's rim is ever inside and the walk never
 * leaves the grid. */
static void _solid_grid_fit(_solid_grid_t *const g, const float bbox[4], float cell)
{
  double w = 0.0;
  double h = 0.0;
  for(;;)
  {
    w = ceil((bbox[1] - bbox[0]) / cell) + 4.0;
    h = ceil((bbox[3] - bbox[2]) / cell) + 4.0;
    if(w * h <= (double)SOLID_MAX_CELLS) break;
    cell *= 1.01f * (float)sqrt(w * h / (double)SOLID_MAX_CELLS);
  }
  g->cell = cell;
  g->x0 = bbox[0] - 2.0f * cell;
  g->y0 = bbox[2] - 2.0f * cell;
  g->w = (int)w;
  g->h = (int)h;
}

/* The sub-scanlines [t0, t1) an edge crosses, sub-scanline t lying at (t + 0.5) / SOLID_SUBLINES
 * cells: half-open, so a vertex two edges share is crossed once. */
static inline void _solid_lines(const _solid_edge_t *const e, const _solid_grid_t *const g, int *t0, int *t1)
{
  const float scale = SOLID_SUBLINES / g->cell;
  *t0 = (int)ceilf((e->y - g->y0) * scale - 0.5f);
  *t1 = (int)ceilf((e->y_end - g->y0) * scale - 0.5f);
}

/* How many crossings each sub-scanline holds, as offsets into one array, @p offset holding
 * lines + 1 entries. Returns the total; past SOLID_MAX_CROSSINGS the offsets are not written. */
static size_t _solid_count(const _solid_edge_t *const edges, const int count, const _solid_grid_t *const g,
                           int *const offset)
{
  const int lines = g->h * SOLID_SUBLINES;
  memset(offset, 0, sizeof(int) * (lines + 1));
  size_t total = 0;
  for(int k = 0; k < count; k++)
  {
    int t0 = 0;
    int t1 = 0;
    _solid_lines(edges + k, g, &t0, &t1);
    if(t1 <= t0) continue;
    offset[t0]++;
    offset[t1]--;
    total += (size_t)(t1 - t0);
  }
  if(total > SOLID_MAX_CROSSINGS) return total;
  int run = 0;
  int at = 0;
  for(int t = 0; t <= lines; t++)
  {
    run += offset[t];
    offset[t] = at;
    at += run;
  }
  return total;
}

static inline uint32_t _solid_key(const _solid_crossing_t *const c)
{
  uint32_t key = 0;
  memcpy(&key, &c->x, sizeof(key));
  return key;
}

/* One sub-scanline's crossings in order of x. Every x is positive -- the grid starts two cells
 * before the box -- and a positive float's bits sort as its value, so a long line is sorted on
 * them a byte at a time, through @p scratch, as long; a byte all the crossings share is skipped. */
static void _solid_sort(_solid_crossing_t *const c, _solid_crossing_t *const scratch, const int m)
{
  if(m <= SOLID_INSERTION)
  {
    for(int q = 1; q < m; q++)
    {
      const _solid_crossing_t v = c[q];
      int r = q;
      for(; r > 0 && c[r - 1].x > v.x; r--) c[r] = c[r - 1];
      c[r] = v;
    }
    return;
  }
  _solid_crossing_t *from = c;
  _solid_crossing_t *to = scratch;
  for(int shift = 0; shift < 32; shift += 8)
  {
    int start[257] = { 0 };
    for(int q = 0; q < m; q++) start[((_solid_key(from + q) >> shift) & 0xff) + 1]++;
    if(start[((_solid_key(from) >> shift) & 0xff) + 1] == m) continue;
    for(int d = 0; d < 256; d++) start[d + 1] += start[d];
    for(int q = 0; q < m; q++) to[start[(_solid_key(from + q) >> shift) & 0xff]++] = from[q];
    _solid_crossing_t *const was = from;
    from = to;
    to = was;
  }
  if(from != c) memcpy(c, from, sizeof(_solid_crossing_t) * m);
}

/* One span of one sub-scanline into the row: the cells it covers whole through a running step,
 * the two it ends in by the fraction it covers. */
static inline void _solid_span(float *const partial, float *const step, const float xa, const float xb,
                               int *const lo, int *const hi)
{
  const float k = 1.0f / SOLID_SUBLINES;
  const int ia = (int)xa;
  const int ib = (int)xb;
  *lo = MIN(*lo, ia);
  *hi = MAX(*hi, ib);
  if(ia == ib)
  {
    partial[ia] += (xb - xa) * k;
    return;
  }
  partial[ia] += ((float)(ia + 1) - xa) * k;
  partial[ib] += (xb - (float)ib) * k;
  step[ia + 1] += k;
  step[ib] -= k;
}

/* Non-zero coverage of the edges' polygons, 0 to 255 per cell, into @p cover (zeroed, w * h), and
 * the cells of each row it may have written, first and last, into @p range: none, w and -1.
 * @p crossing holds every sub-scanline's crossings at @p offset. Returns 1 on allocation failure. */
static int _solid_fill(const _solid_edge_t *const edges, const int count, const _solid_grid_t *const g,
                       const int *const offset, _solid_crossing_t *const crossing, uint8_t *const cover,
                       int *const range)
{
  const int lines = g->h * SOLID_SUBLINES;
  int longest = 0;
  for(int t = 0; t < lines; t++) longest = MAX(longest, offset[t + 1] - offset[t]);
  int *cursor = dt_alloc_align(sizeof(int) * lines);
  float *partial = dt_calloc_align_float((size_t)g->w + 1);
  float *step = dt_calloc_align_float((size_t)g->w + 1);
  _solid_crossing_t *scratch = dt_alloc_align(sizeof(_solid_crossing_t) * MAX(longest, 1));
  if(IS_NULL_PTR(cursor) || IS_NULL_PTR(partial) || IS_NULL_PTR(step) || IS_NULL_PTR(scratch))
  {
    dt_free_align(cursor);
    dt_free_align(partial);
    dt_free_align(step);
    dt_free_align(scratch);
    return 1;
  }
  memcpy(cursor, offset, sizeof(int) * lines);

  const float line_step = g->cell / SOLID_SUBLINES;
  for(int k = 0; k < count; k++)
  {
    const _solid_edge_t *const e = edges + k;
    int t0 = 0;
    int t1 = 0;
    _solid_lines(e, g, &t0, &t1);
    for(int t = t0; t < t1; t++)
    {
      const float y = g->y0 + ((float)t + 0.5f) * line_step;
      _solid_crossing_t *const c = crossing + cursor[t]++;
      c->x = (e->x + (y - e->y) * e->slope - g->x0) / g->cell;
      c->dir = e->dir;
    }
  }

  for(int j = 0; j < g->h; j++)
  {
    int lo = g->w;
    int hi = -1;
    for(int t = j * SOLID_SUBLINES; t < (j + 1) * SOLID_SUBLINES; t++)
    {
      _solid_crossing_t *const c = crossing + offset[t];
      const int m = offset[t + 1] - offset[t];
      if(m < 2) continue;
      _solid_sort(c, scratch, m);
      int winding = 0;
      float from = 0.0f;
      for(int q = 0; q < m; q++)
      {
        const int was = winding;
        winding += c[q].dir;
        if(was == 0) from = c[q].x;
        else if(winding == 0) _solid_span(partial, step, from, c[q].x, &lo, &hi);
      }
    }
    float run = 0.0f;
    uint8_t *const row = cover + (size_t)j * g->w;
    for(int i = lo; i <= hi; i++)
    {
      run += step[i];
      row[i] = (uint8_t)(fminf(fmaxf(partial[i] + run, 0.0f), 1.0f) * 255.0f + 0.5f);
      partial[i] = 0.0f;
      step[i] = 0.0f;
    }
    range[2 * j] = lo;
    range[2 * j + 1] = hi;
  }

  dt_free_align(cursor);
  dt_free_align(partial);
  dt_free_align(step);
  dt_free_align(scratch);
  return 0;
}

/* Square (i, j) has its corners at samples tl (i, j), tr, br and bl, numbered 0 to 3 clockwise,
 * and edge e runs from corner e to corner e + 1. Its visit is recorded on one sample: a horizontal
 * edge's on its left end (bit 1), a vertical edge's on its upper end (bit 2). */
static const int _corner_i[4] = { 0, 1, 1, 0 };
static const int _corner_j[4] = { 0, 0, 1, 1 };
static const int _visit_corner[4] = { 0, 1, 3, 0 };
static const int _step_i[4] = { 0, 1, 0, -1 };
static const int _step_j[4] = { -1, 0, 1, 0 };

static inline uint8_t *_solid_visit(uint8_t *const visited, const int w, const int i, const int j, const int e,
                                    uint8_t *const bit)
{
  *bit = (e & 1) ? 2 : 1;
  return visited + (size_t)(j + _corner_j[_visit_corner[e]]) * w + i + _corner_i[_visit_corner[e]];
}

/* The edge a contour entering square (i, j) through @p entry leaves it by. A saddle is resolved
 * by its centre, the mean of its corners: the two corners on the centre's side are joined. */
static int _solid_exit(const uint8_t *const cover, const int w, const int i, const int j, const int entry)
{
  const uint8_t *const top = cover + (size_t)j * w + i;
  const int v[4] = { top[0], top[1], top[w + 1], top[w] };
  gboolean crossed[4];
  for(int e = 0; e < 4; e++) crossed[e] = (v[e] >= 128) != (v[(e + 1) & 3] >= 128);

  if(crossed[0] && crossed[1] && crossed[2] && crossed[3])
  {
    static const int tl_joined[4] = { SOLID_RIGHT, SOLID_TOP, SOLID_LEFT, SOLID_BOTTOM };
    static const int tl_alone[4] = { SOLID_LEFT, SOLID_BOTTOM, SOLID_RIGHT, SOLID_TOP };
    const gboolean centre = (v[0] + v[1] + v[2] + v[3]) > 510;
    return ((v[0] >= 128) == centre) ? tl_joined[entry] : tl_alone[entry];
  }
  for(int e = 0; e < 4; e++)
    if(e != entry && crossed[e]) return e;
  return entry;   // a square entered through a crossed edge has another
}

/* Where the half-coverage level crosses edge e of square (i, j), in image pixels: a sample sits
 * at its cell's centre. */
static inline void _solid_point(const uint8_t *const cover, const _solid_grid_t *const g, const int i,
                                const int j, const int e, float *const out)
{
  const int a = e;
  const int b = (e + 1) & 3;
  const float va = cover[(size_t)(j + _corner_j[a]) * g->w + i + _corner_i[a]];
  const float vb = cover[(size_t)(j + _corner_j[b]) * g->w + i + _corner_i[b]];
  const float t = (127.5f - va) / (vb - va);
  const float x = i + _corner_i[a] + t * (_corner_i[b] - _corner_i[a]);
  const float y = j + _corner_j[a] + t * (_corner_j[b] - _corner_j[a]);
  out[0] = g->x0 + (x + 0.5f) * g->cell;
  out[1] = g->y0 + (y + 0.5f) * g->cell;
}

/* Follow the loop through the TOP edge of square (i, j), edge by edge, each crossed into the next
 * square, until it reaches an edge already walked: in a consistent marching-squares field, that
 * first one. Writes the loop's points, closed on the first, and returns how many. */
static int _solid_follow(const uint8_t *const cover, uint8_t *const visited, const _solid_grid_t *const g,
                         int i, int j, float *const out)
{
  int count = 0;
  int e = SOLID_TOP;
  for(;;)
  {
    uint8_t bit = 0;
    uint8_t *const visit = _solid_visit(visited, g->w, i, j, e, &bit);
    if(*visit & bit) break;
    *visit |= bit;
    _solid_point(cover, g, i, j, e, out + 2 * count);
    count++;
    i += _step_i[e];
    j += _step_j[e];
    e = _solid_exit(cover, g->w, i, j, (e + 2) & 3);
  }
  out[2 * count] = out[0];
  out[2 * count + 1] = out[1];
  return count + 1;
}

/* Every loop of the half-coverage level. Every loop crosses at least four grid edges, the
 * squares around a sample, so N crossed edges make at most N / 4 loops and N + N / 4 points.
 * Only the cells the fill wrote, @p range, can be inside: a cell is from 128 up, its top bit. */
static int _solid_trace(const uint8_t *const cover, const int *const range, const _solid_grid_t *const g,
                        dt_masks_form_gui_points_t *const gp)
{
  const int w = g->w;
  const int h = g->h;
  size_t crossed = 0;
  for(int j = 0; j + 1 < h; j++)
  {
    const uint8_t *const row = cover + (size_t)j * w;
    for(int i = range[2 * j] - 1; i <= range[2 * j + 1]; i++) crossed += (row[i] ^ row[i + 1]) >> 7;
    const int last = MAX(range[2 * j + 1], range[2 * j + 3]);
    for(int i = MIN(range[2 * j], range[2 * j + 2]); i <= last; i++) crossed += (row[i] ^ row[i + w]) >> 7;
  }
  if(crossed == 0) return 0;

  const size_t max_loops = crossed / 4 + 1;
  float *points = dt_pixelpipe_cache_alloc_align_float_cache(2 * (crossed + max_loops), 0);
  dt_masks_skip_range_t *breaks
      = dt_pixelpipe_cache_alloc_align_cache(sizeof(dt_masks_skip_range_t) * max_loops, 0);
  uint8_t *visited = dt_calloc_align((size_t)w * h);
  if(IS_NULL_PTR(points) || IS_NULL_PTR(breaks) || IS_NULL_PTR(visited))
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(breaks);
    dt_free_align(visited);
    return 1;
  }

  int count = 0;
  int loops = 0;
  for(int j = 0; j + 1 < h; j++)
    for(int i = range[2 * j] - 1; i <= range[2 * j + 1]; i++)
    {
      const uint8_t *const sample = cover + (size_t)j * w + i;
      if(!((sample[0] ^ sample[1]) & 0x80) || (visited[(size_t)j * w + i] & 1)) continue;
      if(loops > 0) breaks[loops - 1] = (dt_masks_skip_range_t){ .jump_from = count, .resume_at = count };
      count += _solid_follow(cover, visited, g, i, j, points + 2 * count);
      loops++;
    }
  dt_free_align(visited);

  if(loops < 2) dt_pixelpipe_cache_free_align(breaks);
  gp->fading = points;
  gp->fading_count = count;
  gp->fading_breaks = breaks;
  gp->fading_break_count = (loops < 2) ? 0 : loops - 1;
  return 0;
}

int dt_masks_solid_edge(dt_masks_form_gui_points_t *gui_points, const float *contour, const int header,
                        const float cell)
{
  const int n = gui_points->border_count - header;
  if(n < 2) return 0;
  const float *const spine = gui_points->points + 2 * header;
  contour += 2 * header;
  const double start = dt_get_wtime();

  // The samples run round the stroke and back to its start: the band that closes the walk is
  // only there when they do.
  const gboolean closed = hypotf(spine[0] - spine[2 * n - 2], spine[1] - spine[2 * n - 1]) < cell;
  const _solid_bands_t b = { .spine = spine, .contour = contour, .n = n, .count = closed ? n : n - 1, .cell = cell };
  const int bands = b.count;

  int8_t *sign = dt_alloc_align((size_t)bands);
  if(IS_NULL_PTR(sign)) return 1;
  float bbox[4] = { FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX };
  int twisted = 0;
  const int runs = _solid_band_signs(&b, sign, bbox, &twisted);
  const size_t capacity = 2 * ((size_t)bands + runs) + 3 * (size_t)twisted;
  _solid_edge_t *edges = (runs > 0) ? dt_alloc_align(sizeof(_solid_edge_t) * capacity) : NULL;
  const int edge_count = IS_NULL_PTR(edges) ? 0 : _solid_edges(&b, sign, edges);
  dt_free_align(sign);
  if(IS_NULL_PTR(edges)) return runs > 0;

  _solid_grid_t g = { 0 };
  int *offset = NULL;
  size_t crossings = 0;
  for(float c = cell;; c = g.cell * 2.0f)
  {
    _solid_grid_fit(&g, bbox, c);
    dt_free_align(offset);
    offset = dt_alloc_align(sizeof(int) * ((size_t)g.h * SOLID_SUBLINES + 1));
    if(IS_NULL_PTR(offset)) break;
    crossings = _solid_count(edges, edge_count, &g, offset);
    if(crossings <= SOLID_MAX_CROSSINGS) break;
  }

  _solid_crossing_t *crossing = dt_alloc_align(sizeof(_solid_crossing_t) * MAX(crossings, 1));
  uint8_t *cover = dt_calloc_align((size_t)g.w * g.h);
  int *range = dt_alloc_align(sizeof(int) * 2 * (size_t)g.h);
  int failed = IS_NULL_PTR(offset) || IS_NULL_PTR(crossing) || IS_NULL_PTR(cover) || IS_NULL_PTR(range);
  if(!failed) failed = _solid_fill(edges, edge_count, &g, offset, crossing, cover, range);
  dt_free_align(edges);
  dt_free_align(offset);
  dt_free_align(crossing);
  const double filled = dt_get_wtime();

  if(!failed) failed = _solid_trace(cover, range, &g, gui_points);
  dt_free_align(cover);
  dt_free_align(range);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS,
             "[masks] solid edge: %d bands in %d runs, %d of them bow ties, %d edges, %dx%d cells of %.1f px, %"
             G_GSIZE_FORMAT " crossings, %d points in %d loops; filled in %.1f ms, traced in %.1f ms\n",
             bands, runs, twisted, edge_count, g.w, g.h, g.cell, crossings, gui_points->fading_count,
             gui_points->fading_break_count + (gui_points->fading_count > 0), 1000.0 * (filled - start),
             1000.0 * (dt_get_wtime() - filled));
  return failed;
}
