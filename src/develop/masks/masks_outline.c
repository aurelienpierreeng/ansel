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

/* The outline of a shape, as its consumers read it.
 *
 * A brush and a polygon both publish two index-aligned arrays: a centreline (the brush's spine,
 * the polygon's path) and a border, one border sample per centreline sample, at the shape's
 * local radius along the normal. The rasteriser paints the spokes between the two; what the GUI
 * draws is the BOUNDARY of what the rasteriser paints. This file holds what deciding that
 * boundary needs, and the one geometric choice the two shapes make the same way at a joint.
 * It knows nothing about nodes, handles or payloads. */

#include "develop/masks/masks_functions.h"
#include "develop/masks_types.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "system/mem_alloc.h"
#include "math/math.h"
#include "common/logging.h"
#include "common/times.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* A border sample at @p radius from @p centre in the direction of (dx, dy). For an end that has
 * a radius but no direction of its own: it borrows another's. Never a position copied from
 * somewhere else -- a spoke of the right length in a borrowed direction is part of the disc
 * union, a spoke to a copied position is of no particular length at all. */
void dt_masks_outline_offset_along(const float *const centre, float dx, float dy, const float radius,
                                   float *const border)
{
  const float len = dt_fast_hypotf(dx, dy);
  if(len > 0.0f)
  {
    dx /= len;
    dy /= len;
  }
  else
  {
    dx = 1.0f;
    dy = 0.0f;
  }
  border[0] = centre[0] + radius * dx;
  border[1] = centre[1] + radius * dy;
}

void dt_masks_outline_envelope_offset(const float *centre, float dx, float dy, float radius, float radius_rate,
                                      float *out)
{
  const float length = dt_fast_hypotf(dx, dy);
  if(!(length > 0.0f))
  {
    out[0] = centre[0];
    out[1] = centre[1];
    return;
  }
  const float l = 1.0f / length;
  /* r' = dr/ds: the rate by the parameter over the speed by the parameter */
  float along = -radius_rate * l;
  along = CLAMP(along, -DT_MASKS_OUTLINE_TILT_MAX, DT_MASKS_OUTLINE_TILT_MAX);
  const float across = sqrtf(1.0f - along * along);
  const float tx = dx * l;
  const float ty = dy * l;
  out[0] = centre[0] + radius * (along * tx + across * ty);
  out[1] = centre[1] + radius * (along * ty - across * tx);
}

/* Which way round a joint arc goes: from @p from to @p to about @p centre, the SHORT way.
 *
 * On the convex side of a turn the short way is the exterior wedge the spokes leave open, which
 * is the whole point of the arc. On the concave side the two borders have crossed and the short
 * way runs through the inside of the shape, painting nothing new but costing only the turn's
 * worth of samples. Sweeping a fixed rotation instead covered the same wedge on one side and
 * went the long way round on the other: a near-full circle of interior spokes at every joint.
 *
 * At a cusp the two are the same length and the choice matters: for a brush the two halves of
 * the tip disc are covered by the two passes, one each, and which is which is the pass's own
 * rotation (the rule its caps follow); for a polygon it is the winding. So a tie within a few
 * degrees of pi keeps @p default_clockwise, and only a sweep clearly longer than pi flips. */
gboolean dt_masks_outline_short_way(const float *const centre, const float *const from, const float *const to,
                                    const gboolean default_clockwise)
{
  const float a1 = atan2f(from[1] - centre[1], from[0] - centre[0]);
  const float a2 = atan2f(to[1] - centre[1], to[0] - centre[0]);
  float sweep_cw = a2 - a1;
  if(sweep_cw < 0.0f) sweep_cw += 2.0f * M_PI;   /* the clockwise sweep, in (0, 2 pi) */

  const float tie = 0.05f;
  if(default_clockwise && sweep_cw > M_PI + tie) return FALSE;
  if(!default_clockwise && sweep_cw < M_PI - tie) return TRUE;
  return default_clockwise;
}

/* THE BOUNDARY OF A SHAPE.
 *
 * The rasteriser paints every spoke it is given, and it should: each one is a radius of a disc
 * the stroke is the union of, so a spoke that lies inside another spoke's disc paints nothing
 * new and costs nothing but time. The DRAWN outline is another matter. It is the boundary of
 * that union, and a border sample that lies inside some other disc is not on it -- it is the
 * inner side of a fold where the centreline bends tighter than its own radius, the inside of
 * a joint arc, a cap swallowed by the next segment, or one side of the stroke running through
 * the other. Every one of those used to be found AFTER the fact, by intersecting the outline
 * with itself and cutting the loops out, and every ordering of those cuts moved the artefact
 * somewhere else (issues #1352 and #1360; the three attempts recorded at the previous version
 * of this function). The question the cuts were approximating is answered here directly, per
 * sample: is this border sample strictly inside any other sample's disc? If so it is not on
 * the boundary and the outline does not show it. Nothing is intersected and nothing is
 * chosen between.
 *
 * Two searches, because the discs that can hide a sample come from two places, and what
 * separates them is where they sit ALONG THE WALK, not where they sit in the plane. A disc
 * can contain a border sample only if its centre is within its own radius of it, and the
 * sample is one radius from its own spine point, so the two centres are within two radii of
 * each other in the plane and -- the spine being a continuous line -- within two radii of
 * each other along it: that much walk either side of the sample's own disc is exhaustive for
 * folds, joints and caps. The window is measured in walk LENGTH, never in discs: a disc is
 * made wherever the spine moves half a pixel or the radius steps, so a count of discs is a
 * different length at every sampling density and at every flare, and the count this used to
 * be (four times the largest radius, plus eight) was ten times too wide once the outline was
 * sampled at the density the screen shows. The stroke can also come back on itself -- a
 * hairpin, a crossing, a spiral, and the brush's own second pass down the other side -- and
 * then the hiding disc is any distance away along the walk but within one radius in the
 * plane. A bucket grid of one reach per cell finds those, and each bucket holds its discs as
 * RUNS of consecutive indices: a run inside the window is the near part, already answered,
 * and is dismissed in one comparison; only runs from far along the walk are tested disc by
 * disc.
 *
 * The test itself is the cost, and it is arranged to be counted rather than computed. The
 * discs are flat arrays -- centre x, centre y, and the SQUARED radius less the tolerance,
 * negative for a disc the tolerance leaves nothing of -- so a probe is one squared distance
 * against each, no square root anywhere, in blocks of eight the compiler can vectorise; and
 * each block carries the box its centres span and its largest radius, so a block that cannot
 * reach the probe costs four comparisons. The previous test took a hypot per disc for the
 * copy test below, and dismissed a block on its first disc's distance against a reach padded
 * by the largest step between discs: measured on the corpus, 4.5 ns per disc test and half
 * the blocks tested.
 *
 * The copy test is a different question and has its own structure. The walk stamps a full
 * disc at a node whose radius steps in BOTH passes, and bridges a joint with an arc about
 * the same node at the same radius; every copy after the first traces a boundary the first
 * already traces, and drawn on top of it with its own dash phase it fills the gaps of the
 * dashes -- measured on a flaring brush: 3,171 samples per pass centred on the node to the
 * float, 4,020 kept on a 2,114 px circumference, drawn as a near-solid line. A copy is not a
 * boundary sample. Neither is the stretch where a segment leaves such a node: its envelope
 * runs within the boundary tolerance of the node's circle for tens of pixels. So a sample
 * that repeats an EARLIER one -- a border position within three quarters of a pixel, at
 * least OUTLINE_REPEAT_MIN_WALK of border walked between the two -- is dropped. Position
 * alone decides, whatever discs the two samples belong to: for drawing, two boundary samples
 * within three quarters of a pixel are one line. The other side of the stroke is a diameter
 * away and never matches. A sample's own run is excluded by the length of border walked
 * between the two: an arc filler can sample closer than the tolerance, and the recursion
 * samples a hundredth of a pixel apart around every integer crossing, so neither a count of
 * samples nor a predecessor test can tell a run from its copy -- the walk can, a copy being
 * the other pass or another stamp, thousands of pixels away along it. Keyed on the sample's
 * disc or its spine point instead, this test missed the copies (a disc's centre is its first
 * sample's, half a pixel off and differently per pass) or the junctions; each round was
 * measured on the corpus before the next. The samples are hashed by pixel cell, so the test
 * reads the nine cells around the probe and nothing else; it used to ride inside the disc
 * test, walking the samples of every disc whose circle passed near the probe, and was what
 * put the square root in that loop.
 *
 * A first version used a coarse occupancy map of the union for the far part instead. It was
 * conservative, and so it left every sample within a few pixels of a far boundary undecided;
 * refining those exactly meant refining every sample, because every boundary sample is within
 * a few pixels of its OWN stroke's interior, and the build went from 30 ms to 250 ms on the
 * corpus. Position cannot tell near from far; the walk can.
 *
 * Cost is bounded by decimation, not by the sample count: consecutive samples closer than half
 * a pixel with the same radius are one disc, so the window and the grid both see a few
 * thousand discs on a stroke of a hundred thousand samples. `-d masks -d perf' prints what a
 * pass cost and how many discs it tested. */

/* Discs per block of the containment test; a block is dismissed on its bounds or tested
 * whole. */
#define OUTLINE_BLOCK 8

/* The bucket grid over the discs: one reach per cell, each bucket a linked list of RUNS of
 * consecutive disc indices, so that a run inside the walk's window can be dismissed with two
 * comparisons and only the discs from far along the walk are ever tested. */
typedef struct _outline_disc_grid_t
{
  int *bucket_head;
  int *run_start;
  int *run_end;
  int *run_next;
  int bw;
  int bh;
  float bucket;
  float minx;
  float miny;
} _outline_disc_grid_t;

/* The discs, decimated, as the flat arrays the containment test streams through. */
typedef struct _outline_discs_t
{
  float *cx;
  float *cy;
  float *rin2;          /* (radius - eps)^2, or -1 for a disc the tolerance leaves nothing of */
  float *dwalk;         /* per disc, the length of spine walked to its centre */
  int count;
} _outline_discs_t;

/* Per block of OUTLINE_BLOCK discs: the box its centres span, and how far past it the largest
 * disc reaches -- zero when none of them can contain anything. */
typedef struct _outline_blocks_t
{
  float *minx;
  float *maxx;
  float *miny;
  float *maxy;
  float *reach;
  int count;
} _outline_blocks_t;

/* The samples hashed by pixel cell, for the copy test. */
typedef struct _outline_cells_t
{
  int *head;
  int *next;
  unsigned mask;
} _outline_cells_t;

/* Everything the per-sample test needs. */
typedef struct _outline_boundary_t
{
  _outline_discs_t discs;
  _outline_blocks_t blocks;
  int *disc_of;         /* per sample, the disc it belongs to */
  const float *border_h;   /* the border samples, past the header */
  float *walk;          /* per sample, the length of border walked to reach it */
  float window;         /* walk either side of a sample's own disc that can reach it */
  float eps;            /* boundary tolerance, in pixels */
  _outline_disc_grid_t grid;
  gboolean have_grid;
  _outline_cells_t cells;
  /* what the pass did, for the perf trace */
  long probes;
  long disc_tests;
} _outline_boundary_t;

/* How far back along the walk a sample may match: closer than this it is the sample's own
 * run. Sixteen samples was the first guess, and the recursion samples a border a hundredth of
 * a pixel apart around every integer crossing, where sixteen samples are less than a pixel. */
#define OUTLINE_REPEAT_MIN_WALK 4.0f

static void _outline_boundary_free(_outline_boundary_t *const b)
{
  dt_free_align(b->discs.cx);
  dt_free_align(b->discs.cy);
  dt_free_align(b->discs.rin2);
  dt_free_align(b->discs.dwalk);
  dt_free_align(b->blocks.minx);
  dt_free_align(b->blocks.maxx);
  dt_free_align(b->blocks.miny);
  dt_free_align(b->blocks.maxy);
  dt_free_align(b->blocks.reach);
  dt_free_align(b->disc_of);
  dt_free_align(b->walk);
  dt_free_align(b->grid.bucket_head);
  dt_free_align(b->grid.run_start);
  dt_free_align(b->grid.run_end);
  dt_free_align(b->grid.run_next);
  dt_free_align(b->cells.head);
  dt_free_align(b->cells.next);
  memset(b, 0, sizeof(*b));
}

/* The arrays a pass over @p n samples needs, all of them, or none. */
static gboolean _outline_boundary_alloc(_outline_boundary_t *const b, const int n)
{
  const int nblock = n / OUTLINE_BLOCK + 1;
  b->discs.cx = dt_alloc_align((size_t)n * sizeof(float));
  b->discs.cy = dt_alloc_align((size_t)n * sizeof(float));
  b->discs.rin2 = dt_alloc_align((size_t)n * sizeof(float));
  b->discs.dwalk = dt_alloc_align((size_t)n * sizeof(float));
  b->blocks.minx = dt_alloc_align((size_t)nblock * sizeof(float));
  b->blocks.maxx = dt_alloc_align((size_t)nblock * sizeof(float));
  b->blocks.miny = dt_alloc_align((size_t)nblock * sizeof(float));
  b->blocks.maxy = dt_alloc_align((size_t)nblock * sizeof(float));
  b->blocks.reach = dt_alloc_align((size_t)nblock * sizeof(float));
  b->disc_of = dt_alloc_align((size_t)n * sizeof(int));
  b->walk = dt_alloc_align((size_t)n * sizeof(float));
  const gboolean ok = !IS_NULL_PTR(b->discs.cx) && !IS_NULL_PTR(b->discs.cy) && !IS_NULL_PTR(b->discs.rin2)
                      && !IS_NULL_PTR(b->discs.dwalk) && !IS_NULL_PTR(b->blocks.minx) && !IS_NULL_PTR(b->blocks.maxx)
                      && !IS_NULL_PTR(b->blocks.miny) && !IS_NULL_PTR(b->blocks.maxy) && !IS_NULL_PTR(b->blocks.reach)
                      && !IS_NULL_PTR(b->disc_of) && !IS_NULL_PTR(b->walk);
  if(!ok) _outline_boundary_free(b);
  return ok;
}

static inline unsigned _outline_cell_hash(const int cx, const int cy)
{
  return ((unsigned)cx * 73856093u) ^ ((unsigned)cy * 19349663u);
}

/* Hash every sample by the pixel cell it falls in. Returns FALSE when the table could not be
 * allocated, in which case the copy test answers "no copy" and the stamps are drawn twice --
 * the outline of a build under memory pressure, not a wrong one. */
static gboolean _outline_cells_build(_outline_boundary_t *const b, const int n)
{
  unsigned size = 1;
  while(size < 2u * (unsigned)n) size <<= 1;
  b->cells.head = dt_alloc_align((size_t)size * sizeof(int));
  b->cells.next = dt_alloc_align((size_t)n * sizeof(int));
  if(IS_NULL_PTR(b->cells.head) || IS_NULL_PTR(b->cells.next))
  {
    dt_free_align(b->cells.head);
    dt_free_align(b->cells.next);
    b->cells.head = NULL;
    b->cells.next = NULL;
    return FALSE;
  }
  memset(b->cells.head, 0xff, (size_t)size * sizeof(int));   /* every head -1 */
  b->cells.mask = size - 1;
  for(int i = 0; i < n; i++)
  {
    const int cx = (int)floorf(b->border_h[2 * i]);
    const int cy = (int)floorf(b->border_h[2 * i + 1]);
    const unsigned h = _outline_cell_hash(cx, cy) & b->cells.mask;
    b->cells.next[i] = b->cells.head[h];
    b->cells.head[h] = i;
  }
  return TRUE;
}

/* Is there, in the hashed cell @p h, a sample within three quarters of a pixel of (bx, by) that
 * lies at least OUTLINE_REPEAT_MIN_WALK before the probe along the border (@p walk_limit). */
static inline gboolean _outline_cell_repeats(const _outline_boundary_t *const b, const unsigned h, const float bx,
                                             const float by, const float walk_limit)
{
  for(int k = b->cells.head[h]; k >= 0; k = b->cells.next[k])
  {
    if(b->walk[k] > walk_limit) continue;
    const float ex = b->border_h[2 * k] - bx;
    const float ey = b->border_h[2 * k + 1] - by;
    if(ex * ex + ey * ey <= 0.5625f) return TRUE;
  }
  return FALSE;
}

/* Does the sample at (bx, by), reached after @p walk of border, repeat an earlier one: a
 * border position within three quarters of a pixel, at least OUTLINE_REPEAT_MIN_WALK before
 * it along the walk. The nine pixel cells around it hold every candidate. */
static inline gboolean _outline_sample_repeats(const _outline_boundary_t *const b, const float bx, const float by,
                                               const float walk)
{
  if(IS_NULL_PTR(b->cells.head)) return FALSE;
  const float walk_limit = walk - OUTLINE_REPEAT_MIN_WALK;
  const int cx = (int)floorf(bx);
  const int cy = (int)floorf(by);
  for(int neighbour = 0; neighbour < 9; neighbour++)
  {
    const unsigned h = _outline_cell_hash(cx - 1 + neighbour % 3, cy - 1 + neighbour / 3) & b->cells.mask;
    if(_outline_cell_repeats(b, h, bx, by, walk_limit)) return TRUE;
  }
  return FALSE;
}

/* Is (bx, by) strictly inside one of the discs [lo, hi], by more than the tolerance: block by
 * block, a block dismissed on its bounds or streamed whole. */
static inline gboolean _outline_discs_contain(_outline_boundary_t *const b, const int lo, const int hi,
                                              const float bx, const float by)
{
  if(lo > hi) return FALSE;
  const int blk_lo = lo / OUTLINE_BLOCK;
  const int blk_hi = hi / OUTLINE_BLOCK;
  for(int blk = blk_lo; blk <= blk_hi; blk++)
  {
    const float reach = b->blocks.reach[blk];
    if(reach <= 0.0f) continue;
    if(bx < b->blocks.minx[blk] - reach || bx > b->blocks.maxx[blk] + reach || by < b->blocks.miny[blk] - reach
       || by > b->blocks.maxy[blk] + reach)
      continue;
    const int j0 = MAX(lo, blk * OUTLINE_BLOCK);
    const int j1 = MIN(hi, blk * OUTLINE_BLOCK + OUTLINE_BLOCK - 1);
    b->disc_tests += j1 - j0 + 1;
    int hit = 0;
    for(int j = j0; j <= j1; j++)
    {
      const float ex = b->discs.cx[j] - bx;
      const float ey = b->discs.cy[j] - by;
      hit |= (ex * ex + ey * ey < b->discs.rin2[j]);
    }
    if(hit) return TRUE;
  }
  return FALSE;
}

static inline int _outline_grid_cell(const _outline_disc_grid_t *const g, const float x, const float y)
{
  const int gx = CLAMP((int)((x - g->minx) / g->bucket) + 1, 0, g->bw - 1);
  const int gy = CLAMP((int)((y - g->miny) / g->bucket) + 1, 0, g->bh - 1);
  return gy * g->bw + gx;
}

/* @p bbox is { minx, maxx, miny, maxy } over the samples. Returns FALSE when the grid could not
 * be allocated, in which case only the near test runs -- the behaviour of a stroke that never
 * revisits its own ground. */
static gboolean _outline_grid_build(_outline_boundary_t *const b, const float *const bbox, const float r_max)
{
  _outline_disc_grid_t *const g = &b->grid;
  g->bucket = fmaxf(r_max, 16.0f);
  g->minx = bbox[0];
  g->miny = bbox[2];
  g->bw = (int)((bbox[1] - bbox[0]) / g->bucket) + 3;
  g->bh = (int)((bbox[3] - bbox[2]) / g->bucket) + 3;
  g->bucket_head = dt_alloc_align((size_t)g->bw * g->bh * sizeof(int));
  g->run_start = dt_alloc_align((size_t)b->discs.count * sizeof(int));
  g->run_end = dt_alloc_align((size_t)b->discs.count * sizeof(int));
  g->run_next = dt_alloc_align((size_t)b->discs.count * sizeof(int));
  if(IS_NULL_PTR(g->bucket_head) || IS_NULL_PTR(g->run_start) || IS_NULL_PTR(g->run_end)
     || IS_NULL_PTR(g->run_next))
    return FALSE;

  for(int cell = 0; cell < g->bw * g->bh; cell++) g->bucket_head[cell] = -1;
  int nruns = 0;
  int last_cell = -1;
  for(int d = 0; d < b->discs.count; d++)
  {
    const int cell = _outline_grid_cell(g, b->discs.cx[d], b->discs.cy[d]);
    if(cell == last_cell)
    {
      g->run_end[nruns - 1] = d;   /* the walk is still in this bucket: extend its latest run */
      continue;
    }
    g->run_start[nruns] = d;
    g->run_end[nruns] = d;
    g->run_next[nruns] = g->bucket_head[cell];
    g->bucket_head[cell] = nruns;
    nruns++;
    last_cell = cell;
  }
  return TRUE;
}

/* The far test: every run in reach of the probe, but only the parts of it OUTSIDE the walk's
 * window [lo, hi] -- the part inside is the near test's, already answered. */
static inline gboolean _outline_far_contains(_outline_boundary_t *const b, const int lo, const int hi,
                                             const float bx, const float by)
{
  const _outline_disc_grid_t *const g = &b->grid;
  const int gx = CLAMP((int)((bx - g->minx) / g->bucket) + 1, 0, g->bw - 1);
  const int gy = CLAMP((int)((by - g->miny) / g->bucket) + 1, 0, g->bh - 1);

  for(int neighbour = 0; neighbour < 9; neighbour++)
  {
    const int xx = gx - 1 + neighbour % 3;
    const int yy = gy - 1 + neighbour / 3;
    if(xx < 0 || yy < 0 || xx >= g->bw || yy >= g->bh) continue;

    for(int r = g->bucket_head[yy * g->bw + xx]; r >= 0; r = g->run_next[r])
    {
      const int start = g->run_start[r];
      const int end = g->run_end[r];
      if(start < lo && _outline_discs_contain(b, start, MIN(end, lo - 1), bx, by)) return TRUE;
      if(end > hi && _outline_discs_contain(b, MAX(start, hi + 1), end, bx, by)) return TRUE;
    }
  }
  return FALSE;
}

/* The discs within the window of walk either side of disc @p d0: the walk to a centre is
 * monotone along the discs, so both ends are a bisection. */
static inline void _outline_window(const _outline_boundary_t *const b, const int d0, int *const lo, int *const hi)
{
  const float w0 = b->discs.dwalk[d0];
  const float w_lo = w0 - b->window;
  int l = 0;
  int h = d0;
  while(l < h)
  {
    const int m = (l + h) / 2;
    if(b->discs.dwalk[m] < w_lo) l = m + 1;
    else h = m;
  }
  *lo = l;
  const float w_hi = w0 + b->window;
  l = d0;
  h = b->discs.count - 1;
  while(l < h)
  {
    const int m = (l + h + 1) / 2;
    if(b->discs.dwalk[m] > w_hi) h = m - 1;
    else l = m;
  }
  *hi = l;
}

/* Is border sample @p i, at (bx, by), strictly inside some other sample's disc, or a copy of
 * an earlier sample: the window along the walk first, then whatever a far part of the stroke
 * brings within reach, then the cells around it. */
static inline gboolean _outline_sample_inside(_outline_boundary_t *const b, const int i, const float bx, const float by)
{
  int lo = 0;
  int hi = 0;
  _outline_window(b, b->disc_of[i], &lo, &hi);
  b->probes++;
  if(_outline_discs_contain(b, lo, hi, bx, by)) return TRUE;
  if(b->have_grid && _outline_far_contains(b, lo, hi, bx, by)) return TRUE;
  return _outline_sample_repeats(b, bx, by, b->walk[i]);
}

/* Settle the samples [from, to) between two probes: when both probes agreed, the samples take
 * their answer (@p agreed is 0 or 1); when they disagreed (@p agreed is -1) each sample is
 * asked itself, which is what makes the cut land on the sample it belongs to and not on a
 * neighbour. Returns how many were dropped. */
static inline int _outline_settle_span(_outline_boundary_t *const b, uint8_t *const dropped, const int from,
                                       const int to, const int agreed)
{
  if(agreed == 0) return 0;
  int ndropped = 0;
  for(int j = from; j < to; j++)
  {
    const gboolean inside
        = (agreed == 1) || _outline_sample_inside(b, j, b->border_h[j * 2], b->border_h[j * 2 + 1]);
    if(!inside) continue;
    dropped[j] = 1;
    ndropped++;
  }
  return ndropped;
}

/* The discs, decimated: consecutive samples closer than half a pixel with the same radius are
 * one disc. Fills @p b's disc arrays, disc_of, the block bounds and the sample bbox; returns
 * the largest radius. */
static float _outline_discs_from_outline(const float *const points_h, const float *const border_h, const int n,
                                         _outline_boundary_t *const b, float *const bbox)
{
  int ndisc = 0;
  float r_max = 0.0f;
  float last_x = 0.0f;
  float last_y = 0.0f;
  float last_r = 0.0f;
  float walked = 0.0f;
  bbox[0] = FLT_MAX;
  bbox[1] = -FLT_MAX;
  bbox[2] = FLT_MAX;
  bbox[3] = -FLT_MAX;
  for(int i = 0; i < n; i++)
  {
    const float px = points_h[i * 2];
    const float py = points_h[i * 2 + 1];
    const float bx = border_h[i * 2];
    const float by = border_h[i * 2 + 1];
    const float r = dt_fast_hypotf(bx - px, by - py);
    bbox[0] = fminf(bbox[0], fminf(bx, px));
    bbox[1] = fmaxf(bbox[1], fmaxf(bx, px));
    bbox[2] = fminf(bbox[2], fminf(by, py));
    bbox[3] = fmaxf(bbox[3], fmaxf(by, py));

    float moved = 0.0f;
    gboolean new_disc = (ndisc == 0);
    if(!new_disc)
    {
      moved = dt_fast_hypotf(px - last_x, py - last_y);
      new_disc = (moved > 0.5f || fabsf(r - last_r) > 0.5f);
    }
    if(new_disc)
    {
      walked += moved;
      const float rin = r - b->eps;
      b->discs.cx[ndisc] = px;
      b->discs.cy[ndisc] = py;
      b->discs.rin2[ndisc] = (rin > 0.0f) ? rin * rin : -1.0f;
      b->discs.dwalk[ndisc] = walked;
      const int blk = ndisc / OUTLINE_BLOCK;
      if(ndisc % OUTLINE_BLOCK == 0)
      {
        b->blocks.minx[blk] = px;
        b->blocks.maxx[blk] = px;
        b->blocks.miny[blk] = py;
        b->blocks.maxy[blk] = py;
        b->blocks.reach[blk] = fmaxf(rin, 0.0f);
      }
      else
      {
        b->blocks.minx[blk] = fminf(b->blocks.minx[blk], px);
        b->blocks.maxx[blk] = fmaxf(b->blocks.maxx[blk], px);
        b->blocks.miny[blk] = fminf(b->blocks.miny[blk], py);
        b->blocks.maxy[blk] = fmaxf(b->blocks.maxy[blk], py);
        b->blocks.reach[blk] = fmaxf(b->blocks.reach[blk], rin);
      }
      ndisc++;
      r_max = fmaxf(r_max, r);
      last_x = px;
      last_y = py;
      last_r = r;
    }
    b->disc_of[i] = ndisc - 1;
  }
  b->discs.count = ndisc;
  b->blocks.count = (ndisc + OUTLINE_BLOCK - 1) / OUTLINE_BLOCK;
  b->border_h = border_h;
  /* the sample is one radius from its spine point, the hiding disc's centre one radius from
   * the sample, and each of them half a pixel from its disc's centre */
  b->window = 2.0f * r_max + 1.0f;
  return r_max;
}

/* The next run of dropped samples at or after *cursor, as [from, to). A kept run of one or
 * two samples between two dropped ones is noise at a crossing, not a boundary, and goes with
 * them. Returns FALSE when none is left. */
static inline gboolean _outline_next_dropped_run(const uint8_t *const dropped, const int n, int *const cursor,
                                        int *const from, int *const to)
{
  int i = *cursor;
  while(i < n && !dropped[i]) i++;
  if(i >= n)
  {
    *cursor = n;
    return FALSE;
  }
  int j = i;
  while(j < n && dropped[j]) j++;
  while(j + 2 < n && !dropped[j] && (dropped[j + 1] || dropped[j + 2]))
  {
    while(j < n && !dropped[j]) j++;
    while(j < n && dropped[j]) j++;
  }
  *from = i;
  *to = j;
  *cursor = j;
  return TRUE;
}

/* A dropped run of one or two samples between kept ones is kept again. Such a speck is a
 * sample that coincides with a neighbour -- a gap filler's first point over the previous leaf's
 * border, a sample a hair inside the next disc -- and hiding it changes nothing on screen while
 * it cuts the run in two: every cut is a sub-path of its own to stroke, and a corpus brush went
 * from one run to forty-seven for nothing. The mirror rule, a kept speck between dropped runs,
 * lives in _outline_next_dropped_run(). */
static void _outline_keep_specks(uint8_t *const dropped, const int n)
{
  int i = 0;
  while(i < n)
  {
    if(!dropped[i])
    {
      i++;
      continue;
    }
    int j = i;
    while(j < n && dropped[j]) j++;
    const gboolean kept_before = (i > 0) && !dropped[i - 1];
    const gboolean kept_after = (j < n) && !dropped[j];
    if(j - i <= 2 && kept_before && kept_after)
      for(int k = i; k < j; k++) dropped[k] = 0;
    i = j;
  }
}

/* Runs of dropped samples become skip ranges, indices offset back past the header. */
static int _outline_skips_from_dropped(const uint8_t *const dropped, const int n, const int header,
                                     dt_masks_skip_range_t **const skips_out)
{
  int nskips = 0;
  int cursor = 0;
  int from = 0;
  int to = 0;
  while(_outline_next_dropped_run(dropped, n, &cursor, &from, &to)) nskips++;
  if(nskips == 0) return 0;

  dt_masks_skip_range_t *skips = dt_pixelpipe_cache_alloc_align_cache(sizeof(dt_masks_skip_range_t) * nskips, 0);
  if(IS_NULL_PTR(skips)) return 0;

  int at = 0;
  cursor = 0;
  while(_outline_next_dropped_run(dropped, n, &cursor, &from, &to))
  {
    skips[at].jump_from = header + from;
    skips[at].resume_at = header + to;
    at++;
  }
  *skips_out = skips;
  return nskips;
}

/* The probes.
 *
 * Not every sample: the border is sampled several times per pixel, and two samples a
 * quarter of a pixel apart cannot be on different sides of a boundary that is decided to
 * half a pixel. So a sample is probed when it has moved half a pixel from the last probe,
 * and the samples between two probes that agree take their answer; only where two probes
 * disagree is every sample between them probed. Measured on the corpus: the same skip
 * ranges to the sample, at a third of the probes. Returns how many samples were dropped. */
static int _outline_probe_samples(_outline_boundary_t *const b, uint8_t *const dropped, const int n)
{
  const float *const border_h = b->border_h;
  int ndropped = 0;
  int last_probe = -1;
  gboolean last_inside = FALSE;
  float last_bx = 0.0f;
  float last_by = 0.0f;
  for(int i = 0; i < n; i++)
  {
    const float bx = border_h[i * 2];
    const float by = border_h[i * 2 + 1];
    const gboolean moved = (fabsf(bx - last_bx) > 0.5f || fabsf(by - last_by) > 0.5f);
    if(last_probe >= 0 && i != n - 1 && !moved) continue;

    const gboolean inside = _outline_sample_inside(b, i, bx, by);
    if(last_probe >= 0 && i > last_probe + 1)
    {
      const int agreed = (inside == last_inside) ? (int)inside : -1;
      ndropped += _outline_settle_span(b, dropped, last_probe + 1, i, agreed);
    }
    if(inside)
    {
      dropped[i] = 1;
      ndropped++;
    }
    last_probe = i;
    last_inside = inside;
    last_bx = bx;
    last_by = by;
  }
  return ndropped;
}

int dt_masks_outline_boundary_skips(const float *const points, const float *const border,
                                         const int count, const int header,
                                         dt_masks_skip_range_t **skips_out)
{
  *skips_out = NULL;
  const int n = count - header;
  if(IS_NULL_PTR(points) || IS_NULL_PTR(border) || n < 8) return 0;
  const float *const points_h = points + 2 * header;
  const float *const border_h = border + 2 * header;

  const double start = dt_get_wtime();
  _outline_boundary_t b = { 0 };
  uint8_t *dropped = dt_alloc_align((size_t)n);
  if(IS_NULL_PTR(dropped) || !_outline_boundary_alloc(&b, n))
  {
    dt_free_align(dropped);
    return 0;
  }
  memset(dropped, 0, (size_t)n);
  b.eps = 0.5f;
  b.walk[0] = 0.0f;
  for(int i = 1; i < n; i++)
    b.walk[i] = b.walk[i - 1]
                + dt_fast_hypotf(border_h[2 * i] - border_h[2 * i - 2], border_h[2 * i + 1] - border_h[2 * i - 1]);

  float bbox[4];
  const float r_max = _outline_discs_from_outline(points_h, border_h, n, &b, bbox);
  b.have_grid = _outline_grid_build(&b, bbox, r_max);
  _outline_cells_build(&b, n);
  const double prepared = dt_get_wtime();

  const int ndropped = _outline_probe_samples(&b, dropped, n);
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS,
             "[masks] boundary pass: %d samples, %d discs, radius %.0f, %ld probes, %ld disc tests, %d dropped;"
             " prepared in %.1f ms, probed in %.1f ms\n",
             n, b.discs.count, r_max, b.probes, b.disc_tests, ndropped, 1000.0 * (prepared - start),
             1000.0 * (dt_get_wtime() - prepared));
  _outline_boundary_free(&b);

  if(ndropped > 0) _outline_keep_specks(dropped, n);
  const int nskips = (ndropped > 0) ? _outline_skips_from_dropped(dropped, n, header, skips_out) : 0;
  dt_free_align(dropped);
  return nskips;
}
