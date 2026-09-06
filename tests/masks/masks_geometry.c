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

/** Rasterised geometry for mask shapes that are known to break.
 *
 * Every shape here is a defect that shipped, reduced to the geometry that causes it. The
 * runner rasterises each one, writes the alpha as a PNG, and reports simple, stable measures
 * of it -- coverage, and the count and size of enclosed holes. Those numbers are the
 * regression signal; the PNGs are how a human sees what the numbers mean.
 *
 * Why measures and not a golden-image diff: the rasteriser's exact anti-aliasing is allowed to
 * change, and a byte comparison would fail on every legitimate improvement while still missing
 * a hole that moved. A hole is what these bugs ARE, so a hole is what is counted.
 *
 * The overlay is rendered too, over the alpha, because the two layers are confused so easily:
 * a brush cusp losing coverage and a dashed outline drawing self-intersecting circles are
 * different bugs, and an always-FALSE flag once deleted geometry the rasteriser needed in
 * order to tidy a line the GUI drew. Seeing them superimposed is what tells them apart.
 *
 * Run: ansel-test-masks-geometry [output-dir]
 */

#include "darktable.h"
#include "develop/develop.h"
#include "develop/dev_geometry.h"
#include "develop/geometry/geometry.h"
#include "develop/masks.h"
#include "develop/masks_debug.h"
#include "develop/masks/masks_functions.h"
#include "math/math.h"
#include "system/mem_alloc.h"

#include <cairo/cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The reported raw's own size. Not an arbitrary canvas: several thresholds in the outline
 * builder are in ABSOLUTE pixels -- the recursion splits until samples are within a pixel, the
 * arc fillers bail when the arc is under two pixels long -- so which defects appear at all is
 * scale-dependent. Reproducing what a user sees means rendering at the size they render at. */
#define IMG_W 5184
#define IMG_H 3888

static int failures = 0;

/* ------------------------------------------------------------------------------------- */

static dt_masks_node_brush_t *_brush_node(const float x, const float y, const float c1x, const float c1y,
                                          const float c2x, const float c2y, const float radius)
{
  dt_masks_node_brush_t *n = (dt_masks_node_brush_t *)calloc(1, sizeof(dt_masks_node_brush_t));
  n->node[0] = x;      n->node[1] = y;
  n->ctrl1[0] = c1x;   n->ctrl1[1] = c1y;
  n->ctrl2[0] = c2x;   n->ctrl2[1] = c2y;
  n->border[0] = n->border[1] = radius;
  n->density = 1.0f;
  n->fading = 0.66f;
  n->state = DT_MASKS_POINT_STATE_NORMAL;
  return n;
}

static dt_masks_node_polygon_t *_polygon_node_state(const float x, const float y, const float c1x, const float c1y,
                                                    const float c2x, const float c2y, const float radius,
                                                    const dt_masks_points_states_t state)
{
  dt_masks_node_polygon_t *n = (dt_masks_node_polygon_t *)calloc(1, sizeof(dt_masks_node_polygon_t));
  n->node[0] = x;     n->node[1] = y;
  n->ctrl1[0] = c1x;  n->ctrl1[1] = c1y;
  n->ctrl2[0] = c2x;  n->ctrl2[1] = c2y;
  n->border[0] = n->border[1] = radius;
  /* the state is not decoration: a CUSP node has both handles on the node, which is where the
   * curve loses its direction and where both reported defects live */
  n->state = state;
  return n;
}

static dt_masks_node_polygon_t *_polygon_node(const float x, const float y, const float c1x, const float c1y,
                                              const float c2x, const float c2y, const float radius)
{
  return _polygon_node_state(x, y, c1x, c1y, c2x, c2y, radius, DT_MASKS_POINT_STATE_NORMAL);
}

/* ------------------------------------------------------------------------------------- */
/* Geometry decoded verbatim from the XMP attached to issue #1313's follow-up (mask_id
 * 1788089411, "brush #1"). Node 8 is the cusp: both handles collapsed onto the node, arms
 * meeting at a sharp angle. Kept as data rather than parsed from the sidecar at run time --
 * the test then needs no file, no database and no XMP reader to reproduce the exact shape a
 * user reported, and a diff shows when the geometry itself is edited. */
static const float _brush_1313[11][9] = {
  /* node.x, node.y, ctrl1.x, ctrl1.y, ctrl2.x, ctrl2.y, border, density, fading */
  { 0.471628428f, 0.0363773443f, 0.480041265f, 0.0386592895f, 0.46321559f,  0.0340954065f, 0.00624799589f, 1.0f, 0.66f },
  { 0.446389854f, 0.0295315273f, 0.454724789f, 0.0302576013f, 0.438054889f, 0.0288054571f, 0.00704672467f, 1.0f, 0.66f },
  { 0.421618611f, 0.0320209153f, 0.425721943f, 0.0304650553f, 0.417515308f, 0.0335767828f, 0.00707301404f, 1.0f, 0.66f },
  { 0.395679384f, 0.0483706258f, 0.404481769f, 0.0413173735f, 0.38687706f,  0.0554238781f, 0.0267287921f,  1.0f, 0.66f },
  { 0.368804485f, 0.0743404329f, 0.380839676f, 0.0592248067f, 0.356769323f, 0.0894560665f, 0.00639372552f, 1.0f, 0.66f },
  { 0.323468447f, 0.139064401f,  0.336321443f, 0.124024376f,  0.31061548f,  0.154104441f,  0.0175853837f,  1.0f, 0.66f },
  { 0.291686505f, 0.164580554f,  0.29908672f,  0.159394339f,  0.28428629f,  0.169766784f,  0.0175853837f,  1.0f, 0.66f },
  { 0.279067189f, 0.170181692f,  0.285639763f, 0.161496982f,  0.272494644f, 0.178866416f,  0.0175853837f,  1.0f, 0.66f },
  { 0.252251089f, 0.216688871f,  0.252251089f, 0.216688871f,  0.252251089f, 0.216688871f,  0.0175853837f,  1.0f, 0.66f }, /* CUSP */
  { 0.229524732f, 0.17267108f,   0.237051532f, 0.181563243f,  0.221997947f, 0.163778931f,  0.0175853837f,  1.0f, 0.66f },
  { 0.207090423f, 0.16333589f,   0.214568526f, 0.16644761f,   0.199612319f, 0.160224169f,  0.0175853837f,  1.0f, 0.66f },
};

static GList *_brush_from_table(const float table[][9], const int count)
{
  GList *points = NULL;
  for(int i = 0; i < count; i++)
    points = g_list_append(points, _brush_node(table[i][0], table[i][1], table[i][2], table[i][3],
                                               table[i][4], table[i][5], table[i][6]));
  return points;
}

/* ------------------------------------------------------------------------------------- */
/* The oracle for a brush.
 *
 * A brush stroke IS the Minkowski sum of its centreline with a disc of the node's radius, so
 * the reference can be constructed instead of remembered: sample the Bezier densely, stamp a
 * disc at every sample, and any pixel the reference covers but the mask does not is coverage
 * the rasteriser owed and did not deliver.
 *
 * This is what "enclosed holes" could not see. The defect reported against #1313 is a V that
 * bites INTO the stroke from outside -- it is open, connected to the background, and no
 * hole-counting metric will ever flag it. Measuring against the disc union does, and it says
 * what the user said: the brush lost its radius near the cusp.
 *
 * The same construction, run the other way, answers the question issue #1360 asks: coverage
 * the rasteriser delivered that NO disc owes. A border sample that lands far from its
 * centreline paints a spoke out to wherever it landed, and the mask grows a region the stroke
 * never covered -- in that report, a circle spanning the whole frame. The owed map cannot see
 * it: every owed pixel is painted, and then some. So two maps are built from the same discs:
 * OWED, the smaller radius shrunk by two pixels, which the mask must cover entirely; and
 * PERMITTED, the larger radius grown by a margin, outside which the mask must be empty. */
typedef enum _disc_pick_t { DISC_OWED, DISC_PERMITTED } _disc_pick_t;

/* One disc of integer radius @p r_i at (cx, cy) into the row difference array: a row of it
 * reaches |dx| <= floor(sqrt(r_i^2 - dy^2)), the discretisation the per-pixel form used. */
static inline void _stamp_disc_rows(int32_t *const diff, const size_t stride, const int w, const int h,
                             const int cx, const int cy, const int r_i)
{
  for(int dy = -r_i; dy <= r_i; dy++)
  {
    const int y = cy + dy;
    if(y < 0 || y >= h) continue;
    const int half = (int)floorf(sqrtf((float)(r_i * r_i - dy * dy)));
    const int x0 = MAX(cx - half, 0);
    const int x1 = MIN(cx + half, w - 1);
    if(x0 > x1) continue;
    diff[(size_t)y * stride + x0] += 1;
    diff[(size_t)y * stride + x1 + 1] -= 1;
  }
}

/* Stamp the disc union of the stroke into @p map. Row spans through a difference array: a
 * disc costs O(r) rather than O(r^2), which is what makes a 43-node, 176 px stroke measurable
 * -- the per-pixel form of this loop is ten thousand million writes for that one case. The
 * discretisation is the one the per-pixel form used: a row of a disc of integer radius r_i
 * reaches |dx| <= floor(sqrt(r_i^2 - dy^2)), so the owed map is identical to before. */
static void _brush_disc_union(const GList *const nodes, const int w, const int h, const _disc_pick_t pick,
                              uint8_t *const map)
{
  const float radius_scale = (float)MIN(w, h);
  memset(map, 0, (size_t)w * h);

  /* one spare slot per row: a span ending at the last pixel closes at x = w, in its own row */
  const size_t stride = (size_t)w + 1;
  int32_t *diff = (int32_t *)calloc(stride * h, sizeof(int32_t));
  if(IS_NULL_PTR(diff)) return;

  for(const GList *l = nodes; l && l->next; l = l->next)
  {
    const dt_masks_node_brush_t *const n0 = (const dt_masks_node_brush_t *)l->data;
    const dt_masks_node_brush_t *const n1 = (const dt_masks_node_brush_t *)l->next->data;
    /* the cubic: this node, its ctrl2, the next node's ctrl1, the next node */
    const float p0x = n0->node[0] * w;
    const float p0y = n0->node[1] * h;
    const float p1x = n0->ctrl2[0] * w;
    const float p1y = n0->ctrl2[1] * h;
    const float p2x = n1->ctrl1[0] * w;
    const float p2y = n1->ctrl1[1] * h;
    const float p3x = n1->node[0] * w;
    const float p3y = n1->node[1] * h;

    /* The segment runs from this node's OUTGOING radius to the next node's INCOMING one --
     * border[1] then border[0] -- which is how the builder reads them (see the pa/pb rows in
     * _brush_get_pts_border()). Nodes with one radius store it in both. */
    const float r_out = n0->border[1] * radius_scale;
    const float r_in = n1->border[0] * radius_scale;

    /* OWED: the SMALLER of the two node radii, not the interpolated one.
     *
     * A disc union and a normal-offset stroke are not the same shape wherever the radius is
     * changing. The implementation offsets the centreline along its normal by the local
     * radius; the envelope of a growing disc family leans outward from that normal by
     * asin(dr/ds), so across a fast radius transition the disc union genuinely covers more
     * than the rasteriser owes. Asserting the interpolated radius there flagged 3552 px of
     * crescents hugging the OUTSIDE of the widest bulge -- a real difference between two
     * definitions, and not a defect under either of them.
     *
     * The smaller radius is what both definitions agree on, so that is the strongest claim
     * this oracle can honestly make. It costs nothing where it matters: a cusp is a direction
     * reversal, not a radius change, so the two nodes bracketing one have near-equal radii and
     * the V hole is still caught at full strength (verified by re-running the corpus against
     * master, which still fails this case).
     *
     * Shrink by two pixels. A disc and a rasterised stroke never agree exactly along the
     * perimeter -- the stroke is stamped from discrete spokes and anti-aliased -- so the
     * outermost ring would report a one-pixel sliver on every well-behaved case and drown
     * the signal. Two pixels in, any disagreement is interior, which is the only kind that
     * means the stroke is missing.
     *
     * PERMITTED: the LARGER radius, grown by three pixels: one for the spoke's extra neighbour
     * write, the rest for the same perimeter disagreement in the other direction. Nothing a
     * correct stroke paints can lie outside it. */
    const float r = (pick == DISC_OWED) ? MIN(r_out, r_in) : MAX(r_out, r_in);
    const int r_i = (pick == DISC_OWED) ? MAX((int)floorf(r) - 2, 0) : (int)ceilf(r) + 3;

    for(int k = 0; k <= 2000; k++)
    {
      const float t = (float)k / 2000.0f;
      const float u = 1.0f - t;
      const float bx = u*u*u*p0x + 3*u*u*t*p1x + 3*u*t*t*p2x + t*t*t*p3x;
      const float by = u*u*u*p0y + 3*u*u*t*p1y + 3*u*t*t*p2y + t*t*t*p3y;
      _stamp_disc_rows(diff, stride, w, h, (int)lrintf(bx), (int)lrintf(by), r_i);
    }
  }

  for(int y = 0; y < h; y++)
  {
    int32_t running = 0;
    for(int x = 0; x < w; x++)
    {
      running += diff[(size_t)y * stride + x];
      map[(size_t)y * w + x] = (running > 0) ? 1 : 0;
    }
  }
  free(diff);
}

typedef enum _runs_mode_t { RUNS_MISSING, RUNS_EXCESS } _runs_mode_t;

/** How much coverage disagrees with the reference, and where the largest connected run is. */
typedef struct _runs_t
{
  int total;
  int largest;
  int cx;
  int cy;
} _runs_t;

/* One connected component of @p flag from @p seed, 4-connected: its size and the sum of its
 * coordinates, so the caller can place it. @p seen is marked as it goes. */
static inline void _flood_component(const uint8_t *const flag, uint8_t *const seen, int *const stack, const int w,
                             const int h, const size_t seed, long *const sum)
{
  const size_t npix = (size_t)w * h;
  int top = 0;
  stack[top++] = (int)seed;
  seen[seed] = 1;
  sum[0] = 0;
  sum[1] = 0;
  sum[2] = 0;
  while(top > 0)
  {
    const int cur = stack[--top];
    const int px = cur % w;
    const int py = cur / w;
    sum[0]++;
    sum[1] += px;
    sum[2] += py;
    const int dx[4] = { 1, -1, 0, 0 };
    const int dy[4] = { 0, 0, 1, -1 };
    for(int k = 0; k < 4; k++)
    {
      const int nx = px + dx[k];
      const int ny = py + dy[k];
      if(nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      const size_t ni = (size_t)ny * w + nx;
      if(!flag[ni] || seen[ni]) continue;
      seen[ni] = 1;
      /* The push is bounded because a cell is marked BEFORE it is pushed, so none can enter
       * the stack twice and `top' cannot pass npix. Stating that in code rather than only in
       * a comment costs one compare per neighbour, and removes the reader's -- and the
       * analyser's -- obligation to reconstruct the argument from two places. */
      if((size_t)top < npix) stack[top++] = (int)ni;
    }
  }
}

/** Largest connected run of coverage that disagrees with the reference, and its total:
 * MISSING is owed and unpainted, EXCESS is painted and not permitted. */
static void _coverage_runs(const float *mask, const uint8_t *reference, const int w, const int h,
                           const _runs_mode_t mode, _runs_t *const out)
{
  out->total = 0;
  out->largest = 0;
  out->cx = -1;
  out->cy = -1;

  /* A frame with no pixels has nothing to measure, and saying so here is not only defensive: it
   * is what lets a reader (and a static analyser) bound every index below. */
  if(w <= 0 || h <= 0) return;
  const size_t npix = (size_t)w * h;

  uint8_t *flag = (uint8_t *)calloc(npix, 1);
  uint8_t *seen = (uint8_t *)calloc(npix, 1);
  int *stack = (int *)malloc(sizeof(int) * npix);
  if(IS_NULL_PTR(flag) || IS_NULL_PTR(seen) || IS_NULL_PTR(stack))
  {
    free(flag);
    free(seen);
    free(stack);
    return;
  }

  /* ANY coverage counts, not a thresholded core. `border' is the OUTER radius: the stroke is
   * solid in the middle and fades to zero at that edge, so most of the disc legitimately holds
   * values below a half. What cannot be legitimate is a pixel the disc covers with no coverage
   * at all -- that is the stroke missing, which is the defect this corpus is about. */
  for(size_t i = 0; i < npix; i++)
  {
    const gboolean flagged = (mode == RUNS_MISSING) ? (reference[i] && mask[i] <= 0.0f)
                                                     : (!reference[i] && mask[i] > 0.0f);
    if(!flagged) continue;
    flag[i] = 1;
    out->total++;
  }

  for(size_t seed = 0; seed < npix; seed++)
  {
    if(!flag[seed] || seen[seed]) continue;
    long sum[3];
    _flood_component(flag, seen, stack, w, h, seed, sum);
    if(sum[0] <= out->largest) continue;
    out->largest = (int)sum[0];
    out->cx = (int)(sum[1] / sum[0]);
    out->cy = (int)(sum[2] / sum[0]);
  }

  free(flag);
  free(seen);
  free(stack);
}

/** Coverage plus enclosed holes: an unpainted component that does not touch the border. */
static void _measure(const float *mask, const int w, const int h, double *coverage, int *hole_count,
                     int *largest_hole)
{
  *coverage = 0.0; *hole_count = 0; *largest_hole = 0;

  /* A frame with no pixels has nothing to measure, and saying so here is not only defensive: it
   * is what lets a reader (and a static analyser) bound every index below. */
  if(w <= 0 || h <= 0) return;
  const size_t npix = (size_t)w * h;

  int *label = (int *)calloc(npix, sizeof(int));
  int *stack = (int *)malloc(sizeof(int) * npix);
  if(IS_NULL_PTR(label) || IS_NULL_PTR(stack)) { free(label); free(stack); return; }

  size_t painted = 0;
  for(size_t i = 0; i < npix; i++) if(mask[i] > 0.5f) painted++;
  *coverage = (double)painted / (double)npix;

  int next_label = 0;
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t seed = (size_t)y * w + x;
      if(mask[seed] > 0.5f || label[seed]) continue;

      next_label++;
      int top = 0, size = 0;
      gboolean touches_border = FALSE;
      stack[top++] = (int)seed;
      label[seed] = next_label;
      while(top > 0)
      {
        const int cur = stack[--top];
        const int cx = cur % w, cy = cur / w;
        size++;
        if(cx == 0 || cy == 0 || cx == w - 1 || cy == h - 1) touches_border = TRUE;
        const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for(int k = 0; k < 4; k++)
        {
          const int nx = cx + dx[k], ny = cy + dy[k];
          if(nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t ni = (size_t)ny * w + nx;
          if(mask[ni] > 0.5f || label[ni]) continue;
          label[ni] = next_label;
          /* The push is bounded because a cell is marked BEFORE it is pushed, so none can enter
           * the stack twice and `top' cannot pass npix. Stating that in code rather than only in
           * a comment costs one compare per neighbour, and removes the reader's -- and the
           * analyser's -- obligation to reconstruct the argument from two places. */
          if((size_t)top < npix) stack[top++] = (int)ni;
        }
      }
      if(!touches_border)
      {
        (*hole_count)++;
        if(size > *largest_hole) *largest_hole = size;
      }
    }

  free(label);
  free(stack);
}



/* Synthetic classes, each isolating one way a stroke's outline can fail. Radii are large
 * relative to the node spacing on purpose: that is the regime where a brush's offset curve
 * folds over itself, which is where every defect in this family has come from. */

/* A cusp: handles collapsed onto the middle node, arms at a sharp angle. */
static const float _brush_cusp_tbl[3][9] = {
  { 0.30f, 0.30f, 0.275f, 0.30f, 0.35f, 0.405f, 0.030f, 1.0f, 0.66f },
  { 0.50f, 0.72f, 0.50f,  0.72f, 0.50f, 0.72f,  0.030f, 1.0f, 0.66f },
  { 0.70f, 0.30f, 0.65f,  0.405f, 0.725f, 0.30f, 0.030f, 1.0f, 0.66f },
};

/* The same, tighter: the arms nearly parallel, so the two offset sides overlap along their
 * whole length and the wedge at the tip is at its narrowest. */
static const float _brush_hairpin_tbl[3][9] = {
  { 0.42f, 0.20f, 0.40f, 0.20f, 0.44f, 0.345f, 0.035f, 1.0f, 0.66f },
  { 0.50f, 0.78f, 0.50f, 0.78f, 0.50f, 0.78f,  0.035f, 1.0f, 0.66f },
  { 0.58f, 0.20f, 0.56f, 0.345f, 0.60f, 0.20f, 0.035f, 1.0f, 0.66f },
};

/* Several sharp joints in a row: each one is another chance to drop a wedge, and a fix that
 * only handles the first would pass the cusp case and fail here. */
static const float _brush_zigzag_tbl[6][9] = {
  { 0.15f, 0.35f, 0.113f, 0.35f,  0.187f, 0.425f, 0.022f, 1.0f, 0.66f },
  { 0.30f, 0.65f, 0.30f,  0.65f,  0.30f,  0.65f,  0.022f, 1.0f, 0.66f },
  { 0.45f, 0.35f, 0.45f,  0.35f,  0.45f,  0.35f,  0.022f, 1.0f, 0.66f },
  { 0.60f, 0.65f, 0.60f,  0.65f,  0.60f,  0.65f,  0.022f, 1.0f, 0.66f },
  { 0.75f, 0.35f, 0.75f,  0.35f,  0.75f,  0.35f,  0.022f, 1.0f, 0.66f },
  { 0.88f, 0.60f, 0.847f, 0.545f, 0.913f, 0.60f, 0.022f, 1.0f, 0.66f },
};

/* SELF-INTERSECTING: the stroke crosses itself, so the offset curve does too and the crossing
 * is real geometry rather than a fold to be cut away. The union must still be solid. */
static const float _brush_selfcross_tbl[5][9] = {
  { 0.25f, 0.30f, 0.20f, 0.30f, 0.35f, 0.34f, 0.030f, 1.0f, 0.66f },
  { 0.62f, 0.42f, 0.52f, 0.39f, 0.70f, 0.44f, 0.030f, 1.0f, 0.66f },
  { 0.62f, 0.62f, 0.72f, 0.58f, 0.52f, 0.66f, 0.030f, 1.0f, 0.66f },
  { 0.30f, 0.44f, 0.40f, 0.50f, 0.24f, 0.41f, 0.030f, 1.0f, 0.66f },
  { 0.30f, 0.24f, 0.27f, 0.30f, 0.33f, 0.20f, 0.030f, 1.0f, 0.66f },
};

/* CONCAVE, tighter than the radius: the classic fold. The inner offset curve loops, and the
 * loop has to be removed without removing the stroke around it. */
static const float _brush_concave_tbl[5][9] = {
  { 0.20f, 0.30f, 0.16f, 0.30f, 0.28f, 0.32f, 0.045f, 1.0f, 0.66f },
  { 0.44f, 0.36f, 0.36f, 0.34f, 0.50f, 0.38f, 0.045f, 1.0f, 0.66f },
  { 0.50f, 0.50f, 0.50f, 0.44f, 0.50f, 0.56f, 0.045f, 1.0f, 0.66f },
  { 0.44f, 0.64f, 0.50f, 0.62f, 0.36f, 0.66f, 0.045f, 1.0f, 0.66f },
  { 0.20f, 0.70f, 0.28f, 0.68f, 0.16f, 0.70f, 0.045f, 1.0f, 0.66f },
};

/* THE THIRD REPORTED SHAPE. Issue #1360, "brush #1" of the attached sidecar, decoded verbatim.
 * Drawn with a pen: the tablet delivered the first seventeen nodes within half a pixel of
 * each other while the pressure -- mapped to opacity -- ramped from 0.05 to 0.91, and the
 * last three the same way as the pen lifted. Every one of those density steps takes the
 * stroke through the opacity-transition stamp, and every one of those coincident segments is
 * degenerate: no direction to offset along. The reporter's mask grew a circle spanning the
 * frame. Frame 5184x3888 is the reporter's own (Panasonic GX9). */
static const float _brush_1360[43][11] = {
  /* columns: node x y | ctrl1 x y | ctrl2 x y | border in out | density | fading | state */
  { 0.22329402f, 0.437683344f, 0.223308727f, 0.437683344f, 0.223279327f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.0500000007f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223257273f, 0.437683344f, 0.223242566f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.163728267f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.263469368f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.330415487f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.436690927f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.493527323f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.559117258f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.612624824f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.651337683f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.68659842f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.720872879f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.770558476f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.223249912f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.795832813f, 0.790861964f, 1 },
  { 0.223249912f, 0.437683344f, 0.223242566f, 0.437683344f, 0.223257273f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.82949084f, 0.790861964f, 1 },
  { 0.22329402f, 0.437683344f, 0.223271966f, 0.437683344f, 0.223316073f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.851436317f, 0.790861964f, 1 },
  { 0.223382235f, 0.437683344f, 0.223367542f, 0.437683344f, 0.223396942f, 0.437683344f, 0.0340311043f, 0.0340311043f, 0.857847393f, 0.790861964f, 1 },
  { 0.223382235f, 0.437683344f, 0.223374888f, 0.4377231f, 0.223389596f, 0.437643617f, 0.0340311043f, 0.0340311043f, 0.909628928f, 0.790861964f, 1 },
  { 0.223426342f, 0.437444836f, 0.224293813f, 0.44460988f, 0.222558886f, 0.430279791f, 0.0340311043f, 0.0340311043f, 0.932067573f, 0.790861964f, 1 },
  { 0.218177453f, 0.394693047f, 0.217354104f, 0.403786033f, 0.219000816f, 0.38560009f, 0.0340311043f, 0.0340311043f, 0.892984807f, 0.790861964f, 1 },
  { 0.228366479f, 0.382887095f, 0.218559727f, 0.388342857f, 0.238173231f, 0.377431333f, 0.0340311043f, 0.0340311043f, 0.883491576f, 0.790861964f, 1 },
  { 0.277017951f, 0.361958414f, 0.259396702f, 0.372373074f, 0.29463923f, 0.351543754f, 0.0340311043f, 0.0340311043f, 0.858956993f, 0.790861964f, 1 },
  { 0.334094107f, 0.320399076f, 0.315803885f, 0.332115591f, 0.352384329f, 0.308682591f, 0.0340311043f, 0.0340311043f, 0.856861055f, 0.790861964f, 1 },
  { 0.3867594f, 0.291659355f, 0.376151383f, 0.28612408f, 0.397367477f, 0.29719466f, 0.0340311043f, 0.0340311043f, 0.859450102f, 0.790861964f, 1 },
  { 0.39774242f, 0.353610754f, 0.392405331f, 0.330813766f, 0.40307951f, 0.376407743f, 0.0340311043f, 0.0340311043f, 0.892984807f, 0.790861964f, 1 },
  { 0.418782055f, 0.428441316f, 0.419451058f, 0.415542245f, 0.418113083f, 0.441340417f, 0.0340311043f, 0.0340311043f, 0.94464308f, 0.790861964f, 1 },
  { 0.393728524f, 0.431005239f, 0.407460898f, 0.427636355f, 0.379996151f, 0.434374154f, 0.0340311043f, 0.0340311043f, 0.966835141f, 0.790861964f, 1 },
  { 0.336387753f, 0.448654532f, 0.364014268f, 0.445325434f, 0.308761239f, 0.45198366f, 0.0340311043f, 0.0340311043f, 0.981383324f, 0.790861964f, 1 },
  { 0.227969483f, 0.450979918f, 0.243914649f, 0.456723928f, 0.212024316f, 0.445235968f, 0.0340311043f, 0.0340311043f, 0.999876738f, 0.790861964f, 1 },
  { 0.240716785f, 0.41419071f, 0.214722306f, 0.423989266f, 0.266711295f, 0.404392183f, 0.0340311043f, 0.0340311043f, 0.981383324f, 0.790861964f, 1 },
  { 0.383936495f, 0.392188728f, 0.362793893f, 0.401897848f, 0.405079097f, 0.382479638f, 0.0340311043f, 0.0340311043f, 0.968807817f, 0.790861964f, 1 },
  { 0.367572308f, 0.35593611f, 0.371056885f, 0.366261333f, 0.36408776f, 0.345610917f, 0.0340311043f, 0.0340311043f, 0.971273601f, 0.790861964f, 1 },
  { 0.363029122f, 0.330237389f, 0.364991963f, 0.330992669f, 0.361066312f, 0.329482168f, 0.0340311043f, 0.0340311043f, 0.981383324f, 0.790861964f, 1 },
  { 0.355795383f, 0.351404577f, 0.359191746f, 0.345690429f, 0.352399051f, 0.357118726f, 0.0340311043f, 0.0340311043f, 0.948095202f, 0.790861964f, 1 },
  { 0.342651129f, 0.364522308f, 0.351039052f, 0.359195709f, 0.334263206f, 0.369848907f, 0.0340311043f, 0.0340311043f, 0.953026772f, 0.790861964f, 1 },
  { 0.305467844f, 0.383364111f, 0.314164549f, 0.380064815f, 0.296771169f, 0.386663437f, 0.0340311043f, 0.0340311043f, 0.987671077f, 0.790861964f, 1 },
  { 0.290470988f, 0.384318113f, 0.28567791f, 0.388144135f, 0.295264095f, 0.380492091f, 0.0340311043f, 0.0340311043f, 0.999630153f, 0.790861964f, 1 },
  { 0.334226459f, 0.360408098f, 0.325316578f, 0.363538474f, 0.343136311f, 0.357277751f, 0.0340311043f, 0.0340311043f, 0.990999877f, 0.790861964f, 1 },
  { 0.343930244f, 0.365535945f, 0.340533912f, 0.361550927f, 0.347326607f, 0.369520962f, 0.0340311043f, 0.0340311043f, 0.970780432f, 0.790861964f, 1 },
  { 0.354604453f, 0.384318113f, 0.35239169f, 0.379160494f, 0.356817216f, 0.389475763f, 0.0340311043f, 0.0340311043f, 0.968807817f, 0.790861964f, 1 },
  { 0.357206851f, 0.396481782f, 0.358831495f, 0.393172562f, 0.355582207f, 0.399791002f, 0.0340311043f, 0.0340311043f, 0.969794095f, 0.790861964f, 1 },
  { 0.34485653f, 0.404173523f, 0.346914947f, 0.4029015f, 0.342798173f, 0.405445546f, 0.0340311043f, 0.0340311043f, 0.988164246f, 0.790861964f, 1 },
  { 0.34485653f, 0.404113948f, 0.344812453f, 0.404282898f, 0.344900668f, 0.403945029f, 0.0340311043f, 0.0340311043f, 0.647515714f, 0.790861964f, 1 },
  { 0.345121175f, 0.403159916f, 0.34503299f, 0.403477967f, 0.34520942f, 0.402841896f, 0.0340311043f, 0.0340311043f, 0.0500000007f, 0.790861964f, 1 },
};

/* THE FOURTH. Issue #1352, "brush #2": seven nodes, the sixth with a smaller radius than its
 * neighbours and its handles pulled far apart -- the "fading handle near the end" the report
 * describes. The radius step takes the builder through the size-transition arc, and the
 * drawn border came out with straight chords across the stroke. Reported at an unknown
 * frame size; the JPEG it was drawn on is not attached, so it runs at the corpus default. */
static const float _brush_1352[7][11] = {
  /* columns: node x y | ctrl1 x y | ctrl2 x y | border in out | density | fading | state */
  { 0.465645701f, 0.0646421909f, 0.474728316f, 0.0589693412f, 0.456563115f, 0.0703150481f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
  { 0.438397855f, 0.0816607475f, 0.445377052f, 0.0763829798f, 0.431418657f, 0.0869385153f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
  { 0.423770666f, 0.0963087901f, 0.428022474f, 0.0899883211f, 0.419518888f, 0.102629259f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
  { 0.412887126f, 0.119583569f, 0.415665925f, 0.10959287f, 0.410108328f, 0.129574269f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
  { 0.407097936f, 0.156252995f, 0.410558552f, 0.147158578f, 0.40363735f, 0.165347442f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
  { 0.392123342f, 0.174150184f, 0.395232916f, 0.141441733f, 0.389013737f, 0.206858635f, 0.018198235f, 0.018198235f, 1.0f, 0.660000026f, 2 },
  { 0.36773169f, 0.193801045f, 0.375862241f, 0.187250778f, 0.35960114f, 0.200351343f, 0.0239629708f, 0.0239629708f, 1.0f, 0.660000026f, 1 },
};

/* Point the dev's geometry at a given frame size. The chain must be rebuilt afterwards or it
 * stops being authoritative and every outline comes back empty, silently. */
static void _set_frame(dt_develop_t *dev, const int w, const int h)
{
  dt_dev_geometry_set_raw_size(dev, w, h, TRUE);
  dt_dev_geometry_set_processed_size(dev, w, h);
  dt_geometry_chain_rebuild(dev);
}


/* THE SECOND REPORTED SHAPE. Polygon #2 of the same sidecar, decoded verbatim like the brush
 * above. Fifteen nodes; node 12 is a CUSP (both handles collapsed onto it, state 2) and nodes 0
 * and 14 bracket the concavity where the outer border was reported self-intersecting. Kept as
 * data for the same reason: the test needs no file, no database and no XMP reader to reproduce
 * the exact shape a user reported, and a diff shows when the geometry itself is edited. */
static const float _polygon_1788045925[15][8] = {
  /* node.x, node.y, ctrl1.x, ctrl1.y, ctrl2.x, ctrl2.y, border, state */
  { 0.109192476f, 0.048409186f, 0.102207638f, 0.045089375f, 0.116177313f, 0.051728997f, 0.021000000f, 1 },
  { 0.141800687f, 0.053578191f, 0.133969516f, 0.053021252f, 0.149631873f, 0.054135136f, 0.021000000f, 1 },
  { 0.156179547f, 0.051750816f, 0.152295023f, 0.049384728f, 0.160064086f, 0.054116912f, 0.021000000f, 1 },
  { 0.165107980f, 0.067774758f, 0.159996808f, 0.066224061f, 0.170219168f, 0.069325462f, 0.021000000f, 1 },
  { 0.186846718f, 0.061055038f, 0.181864932f, 0.064845644f, 0.191828534f, 0.057264429f, 0.021000000f, 1 },
  { 0.194998786f, 0.045031108f, 0.191052154f, 0.047960225f, 0.198945418f, 0.042101998f, 0.021000000f, 1 },
  { 0.210526481f, 0.043480407f, 0.206644550f, 0.040637448f, 0.214408413f, 0.046323366f, 0.021000000f, 1 },
  { 0.218290374f, 0.062088847f, 0.215120122f, 0.055455282f, 0.221460640f, 0.068722412f, 0.021000000f, 1 },
  { 0.229547918f, 0.083281785f, 0.225083694f, 0.080955729f, 0.234012157f, 0.085607842f, 0.021000000f, 1 },
  { 0.245075703f, 0.076045178f, 0.241193786f, 0.074408330f, 0.248957604f, 0.077682026f, 0.021000000f, 1 },
  { 0.252839506f, 0.093102910f, 0.253421843f, 0.087416999f, 0.252257198f, 0.098788828f, 0.021000000f, 1 },
  { 0.241581917f, 0.110160641f, 0.254290909f, 0.109167710f, 0.228872970f, 0.111153573f, 0.021000000f, 1 },
  { 0.176586017f, 0.099060528f, 0.176586017f, 0.099060528f, 0.176586017f, 0.099060528f, 0.021000000f, 2 },
  { 0.099036753f, 0.105304882f, 0.111819178f, 0.116205104f, 0.086254336f, 0.094404668f, 0.021000000f, 1 },
  { 0.099891551f, 0.033659276f, 0.098198950f, 0.043141894f, 0.101584166f, 0.024176663f, 0.021000000f, 1 },
};


/* ------------------------------------------------------------------------------------- */
/* Baseline comparison.
 *
 * The oracle above answers "did the rasteriser deliver the coverage it owed", which is the
 * question the reported defects were about. It cannot answer "did anything change" -- a shifted
 * edge, a different feather ramp, an overlay that stopped drawing a handle all satisfy it. Most
 * of what was actually found while fixing this series was found by rendering before and after and
 * diffing, so that comparison belongs in the test rather than in somebody's scratch directory.
 *
 * The baselines live in the shared sample bank (tests/image_test/samples/baseline/masks-geometry),
 * alongside the raw-export baselines and reviewed the same way -- regenerated deliberately,
 * looked at, and committed. They are full resolution on purpose: the defects in this series are
 * 1 to 5 pixels wide and a downscaled baseline would not see any of them.
 *
 * A tolerance rather than exact equality, because the overlay is antialiased by cairo and its
 * output is not promised to be identical across cairo versions. Anything that moves geometry
 * moves far more than this. */
#define MASKS_BASELINE_MAX_DELTA 8       /* per channel, of 255 */
#define MASKS_BASELINE_MAX_SHARE 0.0002  /* share of pixels allowed to differ at all */
/* The per-pixel bound only means something once enough pixels carry it. A change that moves
 * geometry moves hundreds of pixels; an outline sample moved by a float ulp moves a handful,
 * because a dash edge on the antialiased overlay lands one pixel over. Measured: restructuring
 * brush.c into named steps moved every outline sample by at most 0.0003 px -- FMA contraction
 * landing differently across the new function boundaries, identical sample counts and skip
 * ranges -- and one frame size out of sixteen came out with 4 pixels at a delta of 16. Below
 * this many differing pixels, the worst delta is noise; at or above it, it is a change. */
#define MASKS_BASELINE_NOISE_PX 64

/** Compare two same-sized ARGB surfaces: how many pixels differ at all, and the worst per-channel
 * delta with where it is. Lifted out of the baseline check because "are these the same picture"
 * is a separate question from "what do I do about it", and nesting a triple loop inside the
 * decision made both harder to read. */
static void _surface_diff(cairo_surface_t *const a, cairo_surface_t *const b,
                          size_t *const differing, int *const worst, int *const wx, int *const wy)
{
  cairo_surface_flush(a);
  cairo_surface_flush(b);

  const int w = cairo_image_surface_get_width(a);
  const int h = cairo_image_surface_get_height(a);
  const int sa = cairo_image_surface_get_stride(a);
  const int sb = cairo_image_surface_get_stride(b);
  const uint8_t *const pa = cairo_image_surface_get_data(a);
  const uint8_t *const pb = cairo_image_surface_get_data(b);

  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      int pixel_worst = 0;
      for(int c = 0; c < 3; c++)
      {
        const int d = abs((int)pa[y * sa + x * 4 + c] - (int)pb[y * sb + x * 4 + c]);
        if(d > pixel_worst) pixel_worst = d;
      }
      if(pixel_worst == 0) continue;
      (*differing)++;
      if(pixel_worst > *worst)
      {
        *worst = pixel_worst;
        *wx = x;
        *wy = y;
      }
    }
}

static const char *baseline_dir = NULL;
static gboolean baseline_update = FALSE;
static int baseline_missing = 0;

/** Compare @p path against its baseline, or create the baseline when updating. Returns TRUE when
 * the render is acceptable (or there is nothing to compare against). */
static gboolean _baseline_check(const char *path, const char *name)
{
  if(IS_NULL_PTR(baseline_dir)) return TRUE;

  char *base = g_strdup_printf("%s/%s.png", baseline_dir, name);

  if(baseline_update)
  {
    /* never overwrite: an existing entry is reviewed, and silently replacing it is how a
     * regression becomes the new reference. Delete it deliberately to refresh one. */
    if(!g_file_test(base, G_FILE_TEST_EXISTS))
    {
      char *dirname = g_path_get_dirname(base);
      g_mkdir_with_parents(dirname, 0755);
      g_free(dirname);
      GError *e = NULL;
      char *content = NULL;
      gsize len = 0;
      if(g_file_get_contents(path, &content, &len, &e) && g_file_set_contents(base, content, len, &e))
        printf("      baseline: added %s.png\n", name);
      else
      {
        printf("      baseline: could NOT add %s.png (%s)\n", name, e ? e->message : "?");
        if(e) g_error_free(e);
      }
      g_free(content);
    }
    g_free(base);
    return TRUE;
  }

  if(!g_file_test(base, G_FILE_TEST_EXISTS))
  {
    baseline_missing++;
    g_free(base);
    return TRUE;   /* no baseline yet is not a failure; `update-baseline' adds it */
  }

  cairo_surface_t *a = cairo_image_surface_create_from_png(path);
  cairo_surface_t *b = cairo_image_surface_create_from_png(base);
  gboolean ok = TRUE;

  if(cairo_surface_status(a) != CAIRO_STATUS_SUCCESS || cairo_surface_status(b) != CAIRO_STATUS_SUCCESS)
  {
    printf("      baseline: unreadable (%s)\n", name);
    ok = FALSE;
  }
  else if(cairo_image_surface_get_width(a) != cairo_image_surface_get_width(b)
          || cairo_image_surface_get_height(a) != cairo_image_surface_get_height(b))
  {
    printf("      baseline: %s is %dx%d, baseline is %dx%d\n", name,
           cairo_image_surface_get_width(a), cairo_image_surface_get_height(a),
           cairo_image_surface_get_width(b), cairo_image_surface_get_height(b));
    ok = FALSE;
  }
  else
  {
    size_t differing = 0;
    int worst = 0;
    int wx = -1;
    int wy = -1;
    _surface_diff(a, b, &differing, &worst, &wx, &wy);

    const double share = (double)differing / ((double)cairo_image_surface_get_width(a)
                                              * cairo_image_surface_get_height(a));
    if(share > MASKS_BASELINE_MAX_SHARE
       || (worst > MASKS_BASELINE_MAX_DELTA && differing >= MASKS_BASELINE_NOISE_PX))
    {
      printf("      baseline: %s differs -- %zu px (%.4f%%), worst %d at (%d,%d)\n",
             name, differing, 100.0 * share, worst, wx, wy);
      ok = FALSE;
    }
  }

  cairo_surface_destroy(a);
  cairo_surface_destroy(b);
  g_free(base);
  return ok;
}

/** A picture of exactly what is owed and missing: red where the disc union covers a pixel the
 * rasteriser left empty, over the mask itself. This is the artefact to look at first when a case
 * fails -- it says WHERE the stroke went missing, which no scalar can. */
static void _write_missing_map(const char *dir, const char *name, const float *const mask,
                               const uint8_t *const reference, const int w, const int h)
{
  char *path = g_strdup_printf("%s/%s-missing.png", dir, name);
  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);

  if(cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_flush(surf);
    uint8_t *const pixels = cairo_image_surface_get_data(surf);
    const int stride = cairo_image_surface_get_stride(surf);
    for(int y = 0; y < h; y++)
    {
      uint32_t *const row = (uint32_t *)(pixels + (size_t)y * stride);
      for(int x = 0; x < w; x++)
      {
        const size_t i = (size_t)y * w + x;
        const uint32_t g = (uint32_t)(CLAMPF(mask[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        row[x] = (reference[i] && mask[i] <= 0.0f) ? 0x00FF2020u : ((g << 16) | (g << 8) | g);
      }
    }
    cairo_surface_mark_dirty(surf);
    cairo_surface_write_to_png(surf, path);
  }

  cairo_surface_destroy(surf);
  g_free(path);
}

/* An 11-column node: both radii, the density and the fading are data here, because the
 * defects this corpus grew for depend on them -- issue #1360's stroke is a run of coincident
 * nodes whose only difference is a pen-pressure density ramp, and the density step is what
 * triggers the code path that misbehaves. A 9-column case cannot express it. */
static GList *_brush_from_table11(const float table[][11], const int count)
{
  GList *points = NULL;
  for(int i = 0; i < count; i++)
  {
    dt_masks_node_brush_t *n = _brush_node(table[i][0], table[i][1], table[i][2], table[i][3],
                                           table[i][4], table[i][5], table[i][6]);
    n->border[1] = table[i][7];
    n->density = table[i][8];
    n->fading = table[i][9];
    n->state = (dt_masks_points_states_t)(int)table[i][10];
    points = g_list_append(points, n);
  }
  return points;
}

/** The drawn outline against the disc union.
 *
 * What the GUI draws is meant to be the boundary of what the pipe paints. The two maps the
 * raster is judged against bound that boundary from both sides -- a boundary point is outside
 * the owed map (the union shrunk by two pixels) and inside the permitted one (grown by three)
 * -- so every border sample the outline keeps must land in that band. One that lands deep
 * inside the union is a fold, a joint arc or a crossing the outline failed to hide (issue
 * #1352's chords); one outside the permitted map is a spoke to nowhere (issue #1360). Counts
 * both, and how long the GUI-side build took, which is the cost a drag pays. */
typedef struct _band_t
{
  int kept;
  int inside;
  int outside;
  double seconds;
} _band_t;

static _band_t _outline_band_check(dt_develop_t *dev, dt_masks_form_t *form, const uint8_t *owed,
                                   const uint8_t *permitted, const int w, const int h)
{
  _band_t band = { 0 };
  float *points = NULL;
  float *border = NULL;
  int points_count = 0;
  int border_count = 0;
  int skip_count = 0;
  dt_masks_skip_range_t *skips = NULL;

  const double t0 = dt_get_wtime();
  const dt_masks_raster_result_t st = dt_masks_get_points_border(dev, form, &points, &points_count, &border,
                                                                 &border_count, &skips, &skip_count, 0, NULL);
  band.seconds = dt_get_wtime() - t0;
  if(st != DT_MASKS_RASTER_OK || IS_NULL_PTR(border)) goto done;

  const int header = (int)g_list_length(form->points) * 3;
  for(int i = header; i < border_count; i++)
  {
    if(dt_masks_skip_contains(skips, skip_count, i)) continue;
    band.kept++;
    const int x = (int)lrintf(border[i * 2]);
    const int y = (int)lrintf(border[i * 2 + 1]);
    if(x < 0 || y < 0 || x >= w || y >= h)
    {
      band.outside++;
      continue;
    }
    const size_t at = (size_t)y * w + x;
    if(owed[at]) band.inside++;
    else if(!permitted[at]) band.outside++;
  }

done:
  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  dt_pixelpipe_cache_free_align(skips);
  return band;
}

/** One brush case: what to call it, where to write, what to tolerate, at what frame size. */
typedef struct _brush_case_t
{
  const char *name;
  const char *dir;
  int budget_px;
  int w;
  int h;
} _brush_case_t;

/* Everything that judges a rendered mask against its two maps and reports it: the raster in
 * both directions, the drawn outline against the same maps, the renders, the baselines, and
 * the artefacts a failure needs. The brush and the polygon differ only in how the maps are
 * built, so this is where both meet. */
static void _judge_raster(dt_develop_t *dev, dt_masks_form_t *form, const _brush_case_t *const c,
                          const float *const mask, const uint8_t *const owed, const uint8_t *const permitted)
{
  _runs_t missing = { 0, 0, -1, -1 };
  _runs_t excess = { 0, 0, -1, -1 };
  _band_t band = { 0 };
  if(!IS_NULL_PTR(owed) && !IS_NULL_PTR(permitted))
  {
    _coverage_runs(mask, owed, c->w, c->h, RUNS_MISSING, &missing);
    _coverage_runs(mask, permitted, c->w, c->h, RUNS_EXCESS, &excess);
    band = _outline_band_check(dev, form, owed, permitted, c->w, c->h);
  }

  char *alpha_path = g_strdup_printf("%s/%s-alpha.png", c->dir, c->name);
  char *over_path = g_strdup_printf("%s/%s-overlay.png", c->dir, c->name);
  const dt_masks_debug_request_t alpha_req
      = { .width = c->w, .height = c->h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = FALSE };
  const dt_masks_debug_request_t over_req
      = { .width = c->w, .height = c->h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = TRUE };
  dt_masks_debug_write_png(dev, form, &alpha_req, alpha_path);
  dt_masks_debug_write_png(dev, form, &over_req, over_path);

  char *alpha_name = g_strdup_printf("%s-alpha", c->name);
  char *over_name = g_strdup_printf("%s-overlay", c->name);
  const gboolean baseline_ok = _baseline_check(alpha_path, alpha_name)
                               & _baseline_check(over_path, over_name);
  g_free(alpha_name);
  g_free(over_name);

  /* A picture of exactly what is owed and missing: red where the disc union covers a pixel the
   * rasteriser left empty, over the mask itself. This is the artefact to look at first when a
   * case fails -- it says WHERE the stroke went missing, which no scalar can. */
  if(!IS_NULL_PTR(owed) && missing.total > 0)
    _write_missing_map(c->dir, c->name, mask, owed, c->w, c->h);

  /* Either kind of disagreement is explained by the outline buffers and nothing else. */
  const gboolean disagreement = (missing.total > 0 || excess.total > 0 || band.inside > 0 || band.outside > 0);
  if(disagreement || !IS_NULL_PTR(g_getenv("MASKS_DUMP_OUTLINE")))
  {
    char *csv = g_strdup_printf("%s/%s-outline.csv", c->dir, c->name);
    dt_masks_debug_write_outline_csv(dev, form, csv);
    g_free(csv);
  }

  const gboolean ok = (missing.largest <= c->budget_px) && (excess.largest <= c->budget_px) && baseline_ok
                      && (band.inside <= c->budget_px) && (band.outside <= c->budget_px);
  printf("[%s] %-22s %5dx%-5d missing %6d px (largest run %5d px", ok ? "PASS" : "FAIL",
         c->name, c->w, c->h, missing.total, missing.largest);
  if(missing.largest > 0) printf(" around (%d,%d)", missing.cx, missing.cy);
  printf(")  excess %7d px (largest run %7d px", excess.total, excess.largest);
  if(excess.largest > 0) printf(" around (%d,%d)", excess.cx, excess.cy);
  printf(")  outline: %d kept, %d inside, %d outside, built in %.1f ms  budget %d  -> %s\n",
         band.kept, band.inside, band.outside, 1000.0 * band.seconds, c->budget_px, alpha_path);
  if(!ok) failures++;

  g_free(alpha_path);
  g_free(over_path);
}

/* The oracle for a polygon.
 *
 * A polygon's mask is its path's interior, filled, plus the union of a disc of the local radius
 * over every point of the path -- the feather. Both maps take the interior whole (the fill is
 * exact to the pixel) and the discs at the segment's smaller radius shrunk two pixels (OWED)
 * or its larger one grown three (PERMITTED), for the reasons the brush's oracle gives. The
 * interior is an even-odd scanline over the same dense samples the discs are stamped from.
 *
 * Judging a polygon in both directions is what the previous, holes-only measure could not do:
 * a fold of the outer border filled as shape is coverage nobody owes, and a feather spoke sent
 * to the wrong sample is coverage that goes missing -- neither is a hole. */
#define PATH_SAMPLES_PER_SEGMENT 2001

/* The dense closed path into (px, py), stamping the feather discs as it goes. Returns the
 * sample count. */
static int _path_dense_samples(const GList *const nodes, const int w, const int h, const _disc_pick_t pick,
                               float *const px, float *const py, int32_t *const diff)
{
  const size_t stride = (size_t)w + 1;
  const float radius_scale = (float)MIN(w, h);
  int count = 0;
  for(const GList *l = nodes; l; l = l->next)
  {
    const dt_masks_node_polygon_t *const n0 = (const dt_masks_node_polygon_t *)l->data;
    const dt_masks_node_polygon_t *const n1 = (const dt_masks_node_polygon_t *)(l->next ? l->next : nodes)->data;
    const float p0x = n0->node[0] * w;
    const float p0y = n0->node[1] * h;
    const float p1x = n0->ctrl2[0] * w;
    const float p1y = n0->ctrl2[1] * h;
    const float p2x = n1->ctrl1[0] * w;
    const float p2y = n1->ctrl1[1] * h;
    const float p3x = n1->node[0] * w;
    const float p3y = n1->node[1] * h;
    const float r_out = n0->border[1] * radius_scale;
    const float r_in = n1->border[0] * radius_scale;
    const float r = (pick == DISC_OWED) ? MIN(r_out, r_in) : MAX(r_out, r_in);
    const int r_i = (pick == DISC_OWED) ? MAX((int)floorf(r) - 2, 0) : (int)ceilf(r) + 3;
    for(int k = 0; k < PATH_SAMPLES_PER_SEGMENT; k++)
    {
      const float t = (float)k / (float)(PATH_SAMPLES_PER_SEGMENT - 1);
      const float u = 1.0f - t;
      const float bx = u*u*u*p0x + 3*u*u*t*p1x + 3*u*t*t*p2x + t*t*t*p3x;
      const float by = u*u*u*p0y + 3*u*u*t*p1y + 3*u*t*t*p2y + t*t*t*p3y;
      px[count] = bx;
      py[count] = by;
      count++;
      _stamp_disc_rows(diff, stride, w, h, (int)lrintf(bx), (int)lrintf(by), r_i);
    }
  }
  return count;
}

/* The per-row crossing table: how many crossings each row has, where each row's run starts in
 * @p xs, and how many entries @p xs can hold at all. */
typedef struct _row_table_t
{
  int *count;
  const int *at;
  float *xs;   /* NULL while only counting */
  int capacity;
} _row_table_t;

/* Where the closed path crosses each row, sampled at the row's centre. Two passes: with
 * @p t->xs NULL only the per-row counts are taken; with it, the crossings are written at the
 * row offsets, never outside [0, capacity), and the counts rebuilt from what was written. The
 * two passes walk the same edges and agree, but the bound is what makes that a property of the
 * code rather than of the reader. */
static void _path_row_crossings(const float *const px, const float *const py, const int count, const int h,
                                const _row_table_t *const t)
{
  memset(t->count, 0, sizeof(int) * (size_t)h);
  for(int i = 0; i < count; i++)
  {
    const int j = (i + 1) % count;
    float x0 = px[i];
    float y0 = py[i];
    float x1 = px[j];
    float y1 = py[j];
    if(y0 == y1) continue;
    if(y0 > y1)
    {
      const float sx = x0;
      const float sy = y0;
      x0 = x1;
      y0 = y1;
      x1 = sx;
      y1 = sy;
    }
    /* a row y is crossed if y0 <= y + 0.5 < y1 */
    const int ya = MAX((int)ceilf(y0 - 0.5f), 0);
    const int yb = MIN((int)ceilf(y1 - 0.5f), h);
    for(int y = ya; y < yb; y++)
    {
      if(!IS_NULL_PTR(t->xs))
      {
        const int at = t->at[y] + t->count[y];
        if(at < 0 || at >= t->capacity) continue;
        t->xs[at] = x0 + (x1 - x0) * (((float)y + 0.5f) - y0) / (y1 - y0);
      }
      t->count[y]++;
    }
  }
}

/* The interior of the closed dense path, even-odd, into the row difference array. */
static void _path_interior(const float *const px, const float *const py, const int count, const int w, const int h,
                           int32_t *const diff, const size_t stride)
{
  int *row_count = (int *)calloc((size_t)h + 1, sizeof(int));
  int *row_at = (int *)calloc((size_t)h + 1, sizeof(int));
  if(IS_NULL_PTR(row_count) || IS_NULL_PTR(row_at))
  {
    free(row_count);
    free(row_at);
    return;
  }
  const _row_table_t counting = { .count = row_count, .at = row_at, .xs = NULL, .capacity = 0 };
  _path_row_crossings(px, py, count, h, &counting);
  int total = 0;
  for(int y = 0; y < h; y++)
  {
    row_at[y] = total;
    total += row_count[y];
  }
  /* zeroed, so that nothing the bound refused to write is ever read as a crossing */
  float *xs = (float *)calloc((size_t)total + 1, sizeof(float));
  if(IS_NULL_PTR(xs))
  {
    free(row_count);
    free(row_at);
    return;
  }
  const _row_table_t filling = { .count = row_count, .at = row_at, .xs = xs, .capacity = total };
  _path_row_crossings(px, py, count, h, &filling);

  for(int y = 0; y < h; y++)
  {
    float *const row = xs + row_at[y];
    const int m = MIN(row_count[y], total - row_at[y]);
    /* insertion sort: a row rarely has more than a handful of crossings */
    for(int a = 1; a < m; a++)
    {
      const float v = row[a];
      int b = a - 1;
      while(b >= 0 && row[b] > v)
      {
        row[b + 1] = row[b];
        b--;
      }
      row[b + 1] = v;
    }
    for(int a = 0; a + 1 < m; a += 2)
    {
      const int xa = MAX((int)ceilf(row[a] - 0.5f), 0);
      const int xb = MIN((int)ceilf(row[a + 1] - 0.5f), w);   /* half-open */
      if(xa >= xb) continue;
      diff[(size_t)y * stride + xa] += 1;
      diff[(size_t)y * stride + xb] -= 1;
    }
  }
  free(xs);
  free(row_count);
  free(row_at);
}

static void _polygon_disc_union(const GList *const nodes, const int w, const int h, const _disc_pick_t pick,
                                uint8_t *const map)
{
  memset(map, 0, (size_t)w * h);
  int n = 0;
  for(const GList *l = nodes; l; l = l->next) n++;
  if(n < 3) return;

  const size_t stride = (size_t)w + 1;
  int32_t *diff = (int32_t *)calloc(stride * h, sizeof(int32_t));
  float *px = (float *)malloc(sizeof(float) * (size_t)n * PATH_SAMPLES_PER_SEGMENT);
  float *py = (float *)malloc(sizeof(float) * (size_t)n * PATH_SAMPLES_PER_SEGMENT);
  if(!IS_NULL_PTR(diff) && !IS_NULL_PTR(px) && !IS_NULL_PTR(py))
  {
    const int count = _path_dense_samples(nodes, w, h, pick, px, py, diff);
    _path_interior(px, py, count, w, h, diff, stride);
    for(int y = 0; y < h; y++)
    {
      int32_t running = 0;
      for(int x = 0; x < w; x++)
      {
        running += diff[(size_t)y * stride + x];
        map[(size_t)y * w + x] = (running > 0) ? 1 : 0;
      }
    }
  }
  free(diff);
  free(px);
  free(py);
}

/** Run one polygon through both consumers and judge it. The form is the caller's. */
static void _run_polygon_form_at(dt_develop_t *dev, dt_masks_form_t *form, const _brush_case_t *const c)
{
  _set_frame(dev, c->w, c->h);
  float *mask = dt_masks_debug_rasterise(dev, form, c->w, c->h);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", c->name);
    failures++;
    return;
  }
  uint8_t *owed = (uint8_t *)malloc((size_t)c->w * c->h);
  uint8_t *permitted = (uint8_t *)malloc((size_t)c->w * c->h);
  if(!IS_NULL_PTR(owed) && !IS_NULL_PTR(permitted))
  {
    _polygon_disc_union(form->points, c->w, c->h, DISC_OWED, owed);
    _polygon_disc_union(form->points, c->w, c->h, DISC_PERMITTED, permitted);
  }
  _judge_raster(dev, form, c, mask, owed, permitted);
  free(owed);
  free(permitted);
  dt_free_align(mask);
}

/** Run one brush through both consumers and judge it. Takes ownership of @p nodes. */
static void _run_brush_nodes_at(dt_develop_t *dev, GList *nodes, const _brush_case_t *const c)
{
  _set_frame(dev, c->w, c->h);
  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_BRUSH;
  form.functions = &dt_masks_functions_brush;
  form.version = 6;
  form.formid = 900;
  g_strlcpy(form.name, c->name, sizeof(form.name));
  form.points = nodes;

  float *mask = dt_masks_debug_rasterise(dev, &form, c->w, c->h);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", c->name);
    failures++;
    g_list_free_full(form.points, free);
    return;
  }
  uint8_t *owed = (uint8_t *)malloc((size_t)c->w * c->h);
  uint8_t *permitted = (uint8_t *)malloc((size_t)c->w * c->h);
  if(!IS_NULL_PTR(owed) && !IS_NULL_PTR(permitted))
  {
    _brush_disc_union(form.points, c->w, c->h, DISC_OWED, owed);
    _brush_disc_union(form.points, c->w, c->h, DISC_PERMITTED, permitted);
  }
  _judge_raster(dev, &form, c, mask, owed, permitted);
  free(owed);
  free(permitted);
  dt_free_align(mask);
  g_list_free_full(form.points, free);
}

static void _run_brush_case_at(dt_develop_t *dev, const float table[][9], const int count,
                               const _brush_case_t *const c)
{
  _run_brush_nodes_at(dev, _brush_from_table(table, count), c);
}

static void _run_brush_case11_at(dt_develop_t *dev, const float table[][11], const int count,
                                 const _brush_case_t *const c)
{
  _run_brush_nodes_at(dev, _brush_from_table11(table, count), c);
}

static void _run_brush_case(dt_develop_t *dev, const float table[][9], const int count,
                            const char *name, const char *dir, const int budget_px)
{
  const _brush_case_t c = { name, dir, budget_px, IMG_W, IMG_H };
  _run_brush_case_at(dev, table, count, &c);
}

static void _run_case_at(dt_develop_t *dev, dt_masks_form_t *form, const char *name, const char *dir,
                         const int max_holes, const int max_hole_px, const int img_w, const int img_h)
{
  _set_frame(dev, img_w, img_h);
  float *mask = dt_masks_debug_rasterise(dev, form, img_w, img_h);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", name);
    failures++;
    return;
  }

  double coverage = 0.0;
  int holes = 0, largest = 0;
  _measure(mask, img_w, img_h, &coverage, &holes, &largest);
  dt_free_align(mask);

  char *alpha_path = g_strdup_printf("%s/%s-alpha.png", dir, name);
  char *over_path = g_strdup_printf("%s/%s-overlay.png", dir, name);
  const dt_masks_debug_request_t alpha_req
      = { .width = img_w, .height = img_h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = FALSE };
  const dt_masks_debug_request_t over_req
      = { .width = img_w, .height = img_h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = TRUE };
  dt_masks_debug_write_png(dev, form, &alpha_req, alpha_path);
  dt_masks_debug_write_png(dev, form, &over_req, over_path);

  char *alpha_name = g_strdup_printf("%s-alpha", name);
  char *over_name = g_strdup_printf("%s-overlay", name);
  const gboolean baseline_ok = _baseline_check(alpha_path, alpha_name)
                               & _baseline_check(over_path, over_name);
  g_free(alpha_name);
  g_free(over_name);

  if(holes > max_holes || largest > max_hole_px)
  {
    char *csv = g_strdup_printf("%s/%s-outline.csv", dir, name);
    dt_masks_debug_write_outline_csv(dev, form, csv);
    g_free(csv);
  }

  const gboolean ok = (holes <= max_holes) && (largest <= max_hole_px) && baseline_ok;
  printf("[%s] %-22s %5dx%-5d coverage %.4f  enclosed holes %d (largest %d px)  budget %d/%d  -> %s\n",
         ok ? "PASS" : "FAIL", name, img_w, img_h, coverage, holes, largest, max_holes, max_hole_px, alpha_path);
  if(!ok) failures++;

  g_free(alpha_path);
  g_free(over_path);
}
static void _run_case(dt_develop_t *dev, dt_masks_form_t *form, const char *name, const char *dir,
                      const int max_holes, const int max_hole_px)
{
  _run_case_at(dev, form, name, dir, max_holes, max_hole_px, IMG_W, IMG_H);
}

/* ------------------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
  /* Defaulting to "/tmp" wrote three dozen renders and CSVs to PREDICTABLE names in a
   * world-writable directory: anyone can pre-create those names, or leave symlinks under them
   * pointing elsewhere, and this follows them. ctest always passes an explicit directory, so it
   * only ever affected a manual run -- which is exactly when someone is poking at this as root.
   * g_mkdtemp() creates one atomically, mode 0700, under a name nobody can guess, and two
   * concurrent manual runs stop overwriting each other's output as a side effect. */
  char *scratch_out = NULL;
  const char *dir = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
  if(IS_NULL_PTR(dir))
  {
    scratch_out = g_strdup_printf("%s/ansel-test-masks-out-XXXXXX", g_get_tmp_dir());
    dir = g_mkdtemp(scratch_out);
    if(IS_NULL_PTR(dir))
    {
      fprintf(stderr, "[FAIL] could not create an output directory\n");
      g_free(scratch_out);
      return 1;
    }
    printf("output directory: %s\n", dir);
  }
  else
    g_mkdir_with_parents(dir, 0755);

  /* Baselines live in the shared sample bank, beside the raw-export ones and reviewed the same
   * way. The bank is a plain clone, not a submodule (the superproject is public), so presence is
   * decided by what is on disk -- exactly as tests/image_test.sh decides it. Nothing to compare
   * against is not a failure: a fresh checkout without the bank runs the oracle and says so. */
  char *default_baseline
      = g_strdup(ANSEL_TEST_SOURCE_DIR "/tests/image_test/samples/baseline/masks-geometry");
  for(int i = 1; i < argc; i++)
  {
    if(!strcmp(argv[i], "--update-baseline")) baseline_update = TRUE;
    else if(!strcmp(argv[i], "--baseline") && i + 1 < argc)
    {
      g_free(default_baseline);
      default_baseline = g_strdup(argv[++i]);
    }
    else if(!strcmp(argv[i], "--no-baseline"))
    {
      g_free(default_baseline);
      default_baseline = NULL;
    }
  }
  if(!IS_NULL_PTR(default_baseline)
     && (baseline_update || g_file_test(default_baseline, G_FILE_TEST_IS_DIR)))
    baseline_dir = default_baseline;

  /* The masks code allocates through the pixelpipe cache, whose lock dt_init() creates, and
   * reads conf for per-shape defaults -- so a geometry test still needs a booted instance,
   * just not a GUI one. Everything below is scratch: an in-memory library and temp dirs. */
  char *config_dir = g_strdup_printf("%s/ansel-test-masks-config-XXXXXX", g_get_tmp_dir());
  char *cache_dir = g_strdup_printf("%s/ansel-test-masks-cache-XXXXXX", g_get_tmp_dir());
  char *tmp_dir = g_strdup_printf("%s/ansel-test-masks-tmp-XXXXXX", g_get_tmp_dir());
  if(IS_NULL_PTR(g_mkdtemp(config_dir)) || IS_NULL_PTR(g_mkdtemp(cache_dir))
     || IS_NULL_PTR(g_mkdtemp(tmp_dir)))
  {
    fprintf(stderr, "[FAIL] could not create scratch directories\n");
    return 1;
  }

  char *argv_override[] = {
    "ansel-test-masks-geometry",
    "--library", ":memory:",
    "--datadir", ANSEL_TEST_SOURCE_DIR "/data",
    // the build tree keeps its modules under src/, laid out as the installed tree expects
    "--moduledir", ANSEL_TEST_BINARY_DIR "/src",
    "--configdir", config_dir,
    "--cachedir", cache_dir,
    "--tmpdir", tmp_dir,
    "--disable-opencl",
    "--conf", "write_sidecar_files=FALSE",
    "-t", "1",
    NULL
  };
  const int argc_override = sizeof(argv_override) / sizeof(*argv_override) - 1;
  if(dt_init(argc_override, argv_override, FALSE, FALSE))
  {
    fprintf(stderr, "[FAIL] dt_init\n");
    return 1;
  }

  dt_develop_t dev = { 0 };
  dt_pthread_rwlock_init(&dev.masks_mutex, NULL);
  dt_dev_geometry_init(&dev);
  dt_dev_geometry_set_raw_size(&dev, IMG_W, IMG_H, TRUE);
  /* The GUI outline builder composes through the geometry chain, so a dev that never went
   * through dt_dev_init() needs one: without it the outlines never build and the overlay draws
   * nothing at all -- silently, because an empty outline is a legitimate result. */
  dev.geometry_chain = dt_geometry_chain_new();
  /* The outline builder composes through the geometry service, which refuses to answer until
   * the chain is AUTHORITATIVE -- a guard against transforming against a half-published chain.
   * Rebuilding it here over an empty module list publishes the only honest answer for a dev
   * with no pipeline: the identity. That is also what a geometry regression wants, so a
   * difference means the mask code changed and not some module's distortion. Without this the
   * builder returns ERROR and both the outline and the overlay come back empty -- silently,
   * because an empty outline is a legitimate result. */
  dt_pthread_rwlock_init(&dev.history_mutex, NULL);
  dt_dev_geometry_set_processed_size(&dev, IMG_W, IMG_H);
  dt_geometry_chain_rebuild(&dev);
  printf("geometry chain authoritative: %s\n",
         dt_geometry_chain_authoritative(dev.geometry_chain) ? "yes" : "NO -- outlines will be empty");

  printf("mask geometry corpus -> %s\n", dir);

  /* 0. THE REPORTED SHAPE. Issue #1313's follow-up: brush #1, cusp at node 8. The stroke loses
   *    its radius toward the point of the cusp and leaves a V that is OPEN to the background --
   *    which is why it must be measured against the disc union and not by counting holes.
   *    Budget 0: any owed pixel the rasteriser does not deliver is the bug. */
  /*    SWEEP THE FRAME SIZE, and that is not thoroughness for its own sake.
   *
   *    The defect this case exists for is a floating-point cancellation, so whether it appears
   *    at all depends on the pixel COORDINATES the normalised nodes land on -- that is, on the
   *    frame size. At the cusp the two products 3*p2 and 3*p3 are mathematically equal and
   *    cancel; what survives is the rounding of the -p0*a + p1*b terms above them, which are
   *    tiny but not zero because the recursion never samples t at exactly 1. When that residue
   *    is zero the old code took its degenerate branch and came out round; when it is not, the
   *    code normalised the residue and the border direction became noise, leaving the reported
   *    V hole.
   *
   *    MEASURED, with the fix reverted: 14 of these 16 frames come out clean and two do not --
   *    5000x3750 (3936 px missing at the cusp) and 2999x2251 (1358 px, the same place scaled).
   *    The reporter's own 5198x3904 is among the clean ones. A corpus pinned to a single frame
   *    size would therefore have passed this shape while the reported defect was live, which is
   *    exactly what it did for a whole round of this investigation. Do not reduce this list to
   *    one size; if it ever needs trimming, keep 5000x3750 and 2999x2251, which are the two
   *    that actually detect. */
  static const int frames[][2] = {
    { 5198, 3904 },   // the reporter's own frame -- clean, which is the trap
    { 5184, 3888 },
    { 5000, 3750 },   // DETECTS
    { 4321, 3241 },
    { 4000, 3000 },
    { 2999, 2251 },   // DETECTS
    { 2137, 1603 },
    { 1234,  987 },
  };
  for(int f = 0; f < (int)(sizeof(frames) / sizeof(*frames)); f++)
  {
    char *nm = g_strdup_printf("brush-1313-cusp-%dx%d", frames[f][0], frames[f][1]);
    const _brush_case_t c = { nm, dir, 0, frames[f][0], frames[f][1] };
    _run_brush_case_at(&dev, _brush_1313, 11, &c);
    g_free(nm);
  }

  _run_brush_case(&dev, _brush_cusp_tbl,      3, "brush-cusp",      dir, 0);
  _run_brush_case(&dev, _brush_hairpin_tbl,   3, "brush-hairpin",   dir, 0);
  _run_brush_case(&dev, _brush_zigzag_tbl,    6, "brush-zigzag",    dir, 0);
  _run_brush_case(&dev, _brush_selfcross_tbl, 5, "brush-selfcross", dir, 0);
  _run_brush_case(&dev, _brush_concave_tbl,   5, "brush-concave",   dir, 0);

  /* 3a. THE THIRD AND FOURTH REPORTED SHAPES. Both are judged in BOTH directions: #1360's
   *     defect is coverage nobody owes (a circle the size of the frame), which the owed map
   *     cannot see, and #1352's is the drawn border, which the overlay baseline sees. */
  {
    const _brush_case_t c1360 = { "brush-1360-pressure-ramp", dir, 0, 5184, 3888 };
    const _brush_case_t c1352 = { "brush-1352-radius-step", dir, 0, IMG_W, IMG_H };
    const _brush_case_t c1352_small = { "brush-1352-radius-step-2999x2251", dir, 0, 2999, 2251 };
    _run_brush_case11_at(&dev, _brush_1360, 43, &c1360);
    _run_brush_case11_at(&dev, _brush_1352, 7, &c1352);
    _run_brush_case11_at(&dev, _brush_1352, 7, &c1352_small);
  }

  /* 3b. THE SECOND REPORTED SHAPE, polygon #2. Two defects were reported against it: the outer
   *     border self-intersecting between nodes 0 and 14, where the outline runs into a
   *     concavity, and a missing radial spoke in the feather at node 12, which is the shape's
   *     one cusp. The second is a RASTER defect, so it is measured on the mask and not just
   *     looked at. Frame-swept for the same reason the brush is: the geometry that decides both
   *     is evaluated in pixels. */
  {
    static const int poly_frames[][2] = { { 5198, 3904 }, { 4000, 3000 }, { 2137, 1603 } };
    for(int f = 0; f < (int)(sizeof(poly_frames) / sizeof(*poly_frames)); f++)
    {
      dt_masks_form_t form = { 0 };
      form.type = DT_MASKS_POLYGON;
      form.functions = &dt_masks_functions_polygon;
      form.version = 6;
      form.formid = 106;
      g_strlcpy(form.name, "polygon #2", sizeof(form.name));
      for(int i = 0; i < 15; i++)
      {
        const float *r = _polygon_1788045925[i];
        form.points = g_list_append(form.points,
                                    _polygon_node_state(r[0], r[1], r[2], r[3], r[4], r[5], r[6],
                                                        (dt_masks_points_states_t)(int)r[7]));
      }
      char *nm = g_strdup_printf("polygon-1788045925-%dx%d", poly_frames[f][0], poly_frames[f][1]);
      const _brush_case_t c = { nm, dir, 0, poly_frames[f][0], poly_frames[f][1] };
      _run_polygon_form_at(&dev, &form, &c);
      g_free(nm);
      g_list_free_full(form.points, free);
    }
  }

  /* 4. A polygon whose concave runs are tighter than its feather: its offset curve
   *    self-intersects at every one of them, which is the geometry issue #1313 turned on --
   *    the cuts that remove those folds must not remove anything else. */
  {
    dt_masks_form_t form = { 0 };
    form.type = DT_MASKS_POLYGON;
    form.functions = &dt_masks_functions_polygon;
    form.version = 6;
    form.formid = 104;
    g_strlcpy(form.name, "comb polygon", sizeof(form.name));
    const float radius = 0.028f;
    const int teeth = 5;
    for(int i = 0; i < teeth; i++)
    {
      const float x = 0.20f + 0.13f * i;
      form.points = g_list_append(form.points, _polygon_node(x, 0.35f, x, 0.35f, x, 0.35f, radius));
      form.points = g_list_append(form.points, _polygon_node(x + 0.05f, 0.62f, x + 0.05f, 0.62f,
                                                             x + 0.05f, 0.62f, radius));
    }
    form.points = g_list_append(form.points, _polygon_node(0.80f, 0.78f, 0.80f, 0.78f, 0.80f, 0.78f, radius));
    form.points = g_list_append(form.points, _polygon_node(0.20f, 0.78f, 0.20f, 0.78f, 0.20f, 0.78f, radius));
    const _brush_case_t c = { "polygon-comb", dir, 0, IMG_W, IMG_H };
    _run_polygon_form_at(&dev, &form, &c);
    g_list_free_full(form.points, free);
  }

  /* 5. The two shapes the corpus had no case for at all. A circle is a degenerate ellipse and
   *    the two files share most of their rasteriser by copy-paste, so anything factored out of
   *    one has to be answerable for in the other -- and until now only the circle was covered.
   *    Both are rotated and non-axis-aligned on purpose: an axis-aligned ellipse hides a whole
   *    class of transform error, and a gradient at 0 or 90 degrees hides another. */
  {
    dt_masks_form_t form = { 0 };
    form.type = DT_MASKS_ELLIPSE;
    form.functions = &dt_masks_functions_ellipse;
    form.version = 6;
    form.formid = 107;
    g_strlcpy(form.name, "ellipse", sizeof(form.name));
    dt_masks_node_ellipse_t *e = (dt_masks_node_ellipse_t *)calloc(1, sizeof(dt_masks_node_ellipse_t));
    e->center[0] = 0.42f;
    e->center[1] = 0.55f;
    e->radius[0] = 0.20f;
    e->radius[1] = 0.09f;
    e->rotation = 27.0f;
    e->border = 0.04f;
    e->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
    form.points = g_list_append(form.points, e);
    _run_case(&dev, &form, "ellipse-rotated", dir, 0, 0);
    g_list_free_full(form.points, free);
  }

  {
    dt_masks_form_t form = { 0 };
    form.type = DT_MASKS_GRADIENT;
    form.functions = &dt_masks_functions_gradient;
    form.version = 6;
    form.formid = 108;
    g_strlcpy(form.name, "gradient", sizeof(form.name));
    dt_masks_anchor_gradient_t *g
        = (dt_masks_anchor_gradient_t *)calloc(1, sizeof(dt_masks_anchor_gradient_t));
    g->center[0] = 0.5f;
    g->center[1] = 0.5f;
    g->rotation = 34.0f;
    g->extent = 0.12f;
    g->steepness = 0.0f;
    g->curvature = 0.3f;
    g->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    form.points = g_list_append(form.points, g);
    /* A gradient covers the frame edge to edge, so "enclosed holes" is the only thing to assert
     * and coverage is whatever the ramp gives; the baseline is what actually pins its shape. */
    _run_case(&dev, &form, "gradient-curved", dir, 0, 0);
    g_list_free_full(form.points, free);
  }

  /* 6. A circle, as the control: no joints, no folds. If this ever grows a hole the fault is
   *    in the fill, not in any of the geometry above. */
  {
    dt_masks_form_t form = { 0 };
    form.type = DT_MASKS_CIRCLE;
    form.functions = &dt_masks_functions_circle;
    form.version = 6;
    form.formid = 105;
    g_strlcpy(form.name, "circle", sizeof(form.name));
    dt_masks_node_circle_t *c = (dt_masks_node_circle_t *)calloc(1, sizeof(dt_masks_node_circle_t));
    c->center[0] = 0.5f; c->center[1] = 0.5f;
    c->radius = 0.15f; c->border = 0.03f;
    form.points = g_list_append(form.points, c);
    _run_case(&dev, &form, "circle-control", dir, 0, 0);
    g_list_free_full(form.points, free);
  }

  dt_pthread_rwlock_destroy(&dev.masks_mutex);
  dt_pthread_rwlock_destroy(&dev.history_mutex);

  if(IS_NULL_PTR(baseline_dir))
    printf("baseline: not compared (no %s)\n",
           "tests/image_test/samples/baseline/masks-geometry -- clone the bank to enable it");
  else if(baseline_missing > 0)
    printf("baseline: %d render(s) have no entry yet -- run with --update-baseline to add them\n",
           baseline_missing);

  printf("%s: %d failing case(s)\n", failures ? "FAIL" : "PASS", failures);
  g_free(scratch_out);

  dt_cleanup();
  g_free(config_dir);
  g_free(cache_dir);
  g_free(tmp_dir);
  return failures ? 1 : 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
