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
 * separates them is their INDEX along the walk, not their position. A disc within twice the
 * largest radius of the sample's own spine position can reach it and no other can, so a
 * window of discs either side of the sample's own is exhaustive for folds, joints and caps;
 * consecutive discs move at most a step, so the window is scanned in blocks and a block whose
 * first disc is out of reach is skipped whole. The stroke can also come back on itself -- a
 * hairpin, a crossing, a spiral -- and then the hiding disc is any distance away along the
 * walk but within one radius in the plane. A bucket grid of one reach per cell finds those,
 * and each bucket holds its discs as RUNS of consecutive indices: a run inside the window is
 * the near part, already answered, and is dismissed in one comparison; only runs from far
 * along the walk are tested disc by disc. On a stroke that never revisits its own ground that
 * is nothing at all, and at a crossing it is the discs of the other pass and no more.
 *
 * A first version used a coarse occupancy map of the union for the far part instead. It was
 * conservative, and so it left every sample within a few pixels of a far boundary undecided;
 * refining those exactly meant refining every sample, because every boundary sample is within
 * a few pixels of its OWN stroke's interior, and the build went from 30 ms to 250 ms on the
 * corpus. Position cannot tell near from far; the index can.
 *
 * Cost is bounded by decimation, not by the sample count: consecutive samples closer than half
 * a pixel with the same radius are one disc, so the window and the grid both see a few
 * thousand discs on a stroke of a hundred thousand samples. */
typedef struct _outline_disc_t
{
  float x, y, r;
} _outline_disc_t;

/* is @p b strictly inside disc @p d, by more than the boundary tolerance */
static inline gboolean _outline_disc_contains(const _outline_disc_t *const d, const float bx, const float by,
                                            const float eps)
{
  const float jx = d->x - bx;
  const float jy = d->y - by;
  const float rin = d->r - eps;
  return (rin > 0.0f && jx * jx + jy * jy < rin * rin);
}

/* test the discs [lo, hi] against the probe, in blocks: consecutive discs move at most
 * @p step_max, so a block whose first disc is further than reach + block * step_max away
 * holds nothing that can contain the probe */
static inline gboolean _outline_discs_contain(const _outline_disc_t *const discs, const int lo, const int hi,
                                     const float bx, const float by, const float reach, const float eps)
{
  const int block = 8;
  for(int d = lo; d <= hi; d += block)
  {
    const float ddx = discs[d].x - bx;
    const float ddy = discs[d].y - by;
    if(ddx * ddx + ddy * ddy > reach * reach) continue;
    const int e = MIN(d + block - 1, hi);
    for(int j = d; j <= e; j++)
      if(_outline_disc_contains(&discs[j], bx, by, eps)) return TRUE;
  }
  return FALSE;
}

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

/* Everything the per-sample test needs. */
typedef struct _outline_boundary_t
{
  const _outline_disc_t *discs;
  int ndisc;
  const int *disc_of;   /* per sample, the disc it belongs to */
  int window;           /* discs either side of a sample's own that can reach it along the walk */
  float reach;          /* how far a block's first disc may be for the block to matter */
  float eps;            /* boundary tolerance, in pixels */
  _outline_disc_grid_t grid;
  gboolean have_grid;
} _outline_boundary_t;

static void _outline_grid_free(_outline_disc_grid_t *const g)
{
  dt_free_align(g->bucket_head);
  dt_free_align(g->run_start);
  dt_free_align(g->run_end);
  dt_free_align(g->run_next);
  g->bucket_head = NULL;
  g->run_start = NULL;
  g->run_end = NULL;
  g->run_next = NULL;
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
  g->run_start = dt_alloc_align((size_t)b->ndisc * sizeof(int));
  g->run_end = dt_alloc_align((size_t)b->ndisc * sizeof(int));
  g->run_next = dt_alloc_align((size_t)b->ndisc * sizeof(int));
  if(IS_NULL_PTR(g->bucket_head) || IS_NULL_PTR(g->run_start) || IS_NULL_PTR(g->run_end)
     || IS_NULL_PTR(g->run_next))
  {
    _outline_grid_free(g);
    return FALSE;
  }

  for(int cell = 0; cell < g->bw * g->bh; cell++) g->bucket_head[cell] = -1;
  int nruns = 0;
  int last_cell = -1;
  for(int d = 0; d < b->ndisc; d++)
  {
    const int cell = _outline_grid_cell(g, b->discs[d].x, b->discs[d].y);
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
static inline gboolean _outline_far_contains(const _outline_boundary_t *const b, const int lo, const int hi,
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
      if(start < lo && _outline_discs_contain(b->discs, start, MIN(end, lo - 1), bx, by, b->reach, b->eps))
        return TRUE;
      if(end > hi && _outline_discs_contain(b->discs, MAX(start, hi + 1), end, bx, by, b->reach, b->eps))
        return TRUE;
    }
  }
  return FALSE;
}

/* Is border sample @p i, at (bx, by), strictly inside some other sample's disc: the window along
 * the walk first, then whatever a far part of the stroke brings within reach. */
static inline gboolean _outline_sample_inside(const _outline_boundary_t *const b, const int i, const float bx, const float by)
{
  const int d0 = b->disc_of[i];
  const int lo = MAX(d0 - b->window, 0);
  const int hi = MIN(d0 + b->window, b->ndisc - 1);
  if(_outline_discs_contain(b->discs, lo, hi, bx, by, b->reach, b->eps)) return TRUE;
  return b->have_grid && _outline_far_contains(b, lo, hi, bx, by);
}

/* Settle the samples [from, to) between two probes: when both probes agreed, the samples take
 * their answer (@p agreed is 0 or 1); when they disagreed (@p agreed is -1) each sample is
 * asked itself, which is what makes the cut land on the sample it belongs to and not on a
 * neighbour. @p border_h is the border array already offset past the header. Returns how many
 * were dropped. */
static inline int _outline_settle_span(const _outline_boundary_t *const b, const float *const border_h,
                              uint8_t *const dropped, const int from, const int to, const int agreed)
{
  if(agreed == 0) return 0;
  int ndropped = 0;
  for(int j = from; j < to; j++)
  {
    const gboolean inside = (agreed == 1) || _outline_sample_inside(b, j, border_h[j * 2], border_h[j * 2 + 1]);
    if(!inside) continue;
    dropped[j] = 1;
    ndropped++;
  }
  return ndropped;
}

/* The discs, decimated: consecutive samples closer than half a pixel with the same radius are
 * one disc. Fills @p b's discs/disc_of/ndisc and the sample bbox; returns the largest step
 * between consecutive discs, which bounds how far a block of them can travel. */
static float _outline_discs_from_outline(const float *const points_h, const float *const border_h, const int n,
                                       _outline_disc_t *const discs, int *const disc_of, _outline_boundary_t *const b,
                                       float *const bbox)
{
  int ndisc = 0;
  float r_max = 0.0f;
  float step_max = 0.0f;
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

    gboolean new_disc = (ndisc == 0);
    if(!new_disc)
    {
      const _outline_disc_t *const last = &discs[ndisc - 1];
      const float moved = dt_fast_hypotf(px - last->x, py - last->y);
      new_disc = (moved > 0.5f || fabsf(r - last->r) > 0.5f);
      if(new_disc) step_max = fmaxf(step_max, moved);
    }
    if(new_disc)
    {
      discs[ndisc].x = px;
      discs[ndisc].y = py;
      discs[ndisc].r = r;
      ndisc++;
      r_max = fmaxf(r_max, r);
    }
    disc_of[i] = ndisc - 1;
  }
  b->discs = discs;
  b->ndisc = ndisc;
  b->disc_of = disc_of;
  b->window = (int)(4.0f * r_max) + 8;
  b->reach = r_max + 8.0f * fmaxf(step_max, 1.0f);
  b->eps = 0.5f;
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

int dt_masks_outline_boundary_skips(const float *const points, const float *const border,
                                         const int count, const int header,
                                         dt_masks_skip_range_t **skips_out)
{
  *skips_out = NULL;
  const int n = count - header;
  if(IS_NULL_PTR(points) || IS_NULL_PTR(border) || n < 8) return 0;
  const float *const points_h = points + 2 * header;
  const float *const border_h = border + 2 * header;

  _outline_disc_t *discs = dt_alloc_align((size_t)n * sizeof(_outline_disc_t));
  int *disc_of = dt_alloc_align((size_t)n * sizeof(int));
  uint8_t *dropped = dt_alloc_align((size_t)n);
  if(IS_NULL_PTR(discs) || IS_NULL_PTR(disc_of) || IS_NULL_PTR(dropped))
  {
    dt_free_align(discs);
    dt_free_align(disc_of);
    dt_free_align(dropped);
    return 0;
  }
  memset(dropped, 0, (size_t)n);

  _outline_boundary_t b = { 0 };
  float bbox[4];
  const float r_max = _outline_discs_from_outline(points_h, border_h, n, discs, disc_of, &b, bbox);
  b.have_grid = _outline_grid_build(&b, bbox, r_max);

  /* The probes.
   *
   * Not every sample: the border is sampled several times per pixel, and two samples a
   * quarter of a pixel apart cannot be on different sides of a boundary that is decided to
   * half a pixel. So a sample is probed when it has moved half a pixel from the last probe,
   * and the samples between two probes that agree take their answer; only where two probes
   * disagree is every sample between them probed. Measured on the corpus: the same skip
   * ranges to the sample, at a third of the probes. */
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

    const gboolean inside = _outline_sample_inside(&b, i, bx, by);
    if(last_probe >= 0 && i > last_probe + 1)
    {
      const int agreed = (inside == last_inside) ? (int)inside : -1;
      ndropped += _outline_settle_span(&b, border_h, dropped, last_probe + 1, i, agreed);
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

  _outline_grid_free(&b.grid);
  dt_free_align(discs);
  dt_free_align(disc_of);

  const int nskips = (ndropped > 0) ? _outline_skips_from_dropped(dropped, n, header, skips_out) : 0;
  dt_free_align(dropped);
  return nskips;
}
