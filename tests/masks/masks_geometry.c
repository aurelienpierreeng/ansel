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
 * what the user said: the brush lost its radius near the cusp. */
static void _brush_reference(const float table[][9], const int count, const int w, const int h,
                             uint8_t *reference)
{
  const float radius_scale = (float)MIN(w, h);
  memset(reference, 0, (size_t)w * h);

  for(int seg = 0; seg + 1 < count; seg++)
  {
    const float p0x = table[seg][0] * w,     p0y = table[seg][1] * h;
    const float p1x = table[seg][4] * w,     p1y = table[seg][5] * h;   /* ctrl2 of this node */
    const float p2x = table[seg + 1][2] * w, p2y = table[seg + 1][3] * h; /* ctrl1 of the next */
    const float p3x = table[seg + 1][0] * w, p3y = table[seg + 1][1] * h;

    for(int k = 0; k <= 2000; k++)
    {
      const float t = (float)k / 2000.0f, u = 1.0f - t;
      const float bx = u*u*u*p0x + 3*u*u*t*p1x + 3*u*t*t*p2x + t*t*t*p3x;
      const float by = u*u*u*p0y + 3*u*u*t*p1y + 3*u*t*t*p2y + t*t*t*p3y;
      /* The SMALLER of the two node radii, not the interpolated one.
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
       * master, which still fails this case). */
      const float r = MIN(table[seg][6], table[seg + 1][6]) * radius_scale;
      /* Shrink by two pixels. A disc and a rasterised stroke never agree exactly along the
       * perimeter -- the stroke is stamped from discrete spokes and anti-aliased -- so the
       * outermost ring would report a one-pixel sliver on every well-behaved case and drown
       * the signal. Two pixels in, any disagreement is interior, which is the only kind that
       * means the stroke is missing. */
      const int r_i = MAX((int)floorf(r) - 2, 0);
      const int cx = (int)lrintf(bx), cy = (int)lrintf(by);
      for(int dy = -r_i; dy <= r_i; dy++)
        for(int dx = -r_i; dx <= r_i; dx++)
        {
          if(dx * dx + dy * dy > r_i * r_i) continue;
          const int x = cx + dx, y = cy + dy;
          if(x < 0 || y < 0 || x >= w || y >= h) continue;
          reference[(size_t)y * w + x] = 1;
        }
    }
  }
}

/** Largest connected run of owed-but-missing coverage, and its total. */
static void _missing_coverage(const float *mask, const uint8_t *reference, const int w, const int h,
                              int *total, int *largest, int *cx, int *cy)
{
  *total = 0; *largest = 0; *cx = -1; *cy = -1;

  /* A frame with no pixels has nothing to measure, and saying so here is not only defensive: it
   * is what lets a reader (and a static analyser) bound every index below. */
  if(w <= 0 || h <= 0) return;
  const size_t npix = (size_t)w * h;

  uint8_t *miss = (uint8_t *)calloc(npix, 1);
  int *stack = (int *)malloc(sizeof(int) * npix);
  if(IS_NULL_PTR(miss) || IS_NULL_PTR(stack)) { free(miss); free(stack); return; }

  /* ANY coverage counts, not a thresholded core. `border' is the OUTER radius: the stroke is
   * solid in the middle and fades to zero at that edge, so most of the disc legitimately holds
   * values below a half. What cannot be legitimate is a pixel the disc covers with no coverage
   * at all -- that is the stroke missing, which is the defect this corpus is about. */
  for(size_t i = 0; i < npix; i++)
    if(reference[i] && mask[i] <= 0.0f) { miss[i] = 1; (*total)++; }

  uint8_t *seen = (uint8_t *)calloc(npix, 1);
  if(IS_NULL_PTR(seen)) { free(miss); free(stack); return; }
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t seed = (size_t)y * w + x;
      if(!miss[seed] || seen[seed]) continue;
      int top = 0, size = 0;
      long sx = 0, sy = 0;
      stack[top++] = (int)seed; seen[seed] = 1;
      while(top > 0)
      {
        const int cur = stack[--top];
        const int px = cur % w, py = cur / w;
        size++; sx += px; sy += py;
        const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for(int k = 0; k < 4; k++)
        {
          const int nx = px + dx[k], ny = py + dy[k];
          if(nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t ni = (size_t)ny * w + nx;
          if(!miss[ni] || seen[ni]) continue;
          seen[ni] = 1;
          /* The push is bounded because a cell is marked BEFORE it is pushed, so none can enter
           * the stack twice and `top' cannot pass npix. Stating that in code rather than only in
           * a comment costs one compare per neighbour, and removes the reader's -- and the
           * analyser's -- obligation to reconstruct the argument from two places. */
          if((size_t)top < npix) stack[top++] = (int)ni;
        }
      }
      if(size > *largest) { *largest = size; *cx = (int)(sx / size); *cy = (int)(sy / size); }
    }

  free(miss); free(seen); free(stack);
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
    if(worst > MASKS_BASELINE_MAX_DELTA || share > MASKS_BASELINE_MAX_SHARE)
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

static void _run_brush_case_at(dt_develop_t *dev, const float table[][9], const int count,
                               const char *name, const char *dir, const int budget_px,
                               const int img_w, const int img_h)
{
  _set_frame(dev, img_w, img_h);
  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_BRUSH;
  form.functions = &dt_masks_functions_brush;
  form.version = 6;
  form.formid = 900;
  g_strlcpy(form.name, name, sizeof(form.name));
  form.points = _brush_from_table(table, count);

  float *mask = dt_masks_debug_rasterise(dev, &form, img_w, img_h);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", name);
    failures++;
    g_list_free_full(form.points, free);
    return;
  }

  uint8_t *reference = (uint8_t *)malloc((size_t)img_w * img_h);
  int missing = 0, largest = 0, cx = -1, cy = -1;
  if(!IS_NULL_PTR(reference))
  {
    _brush_reference(table, count, img_w, img_h, reference);
    _missing_coverage(mask, reference, img_w, img_h, &missing, &largest, &cx, &cy);
  }

  char *alpha_path = g_strdup_printf("%s/%s-alpha.png", dir, name);
  char *over_path = g_strdup_printf("%s/%s-overlay.png", dir, name);
  const dt_masks_debug_request_t alpha_req
      = { .width = img_w, .height = img_h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = FALSE };
  const dt_masks_debug_request_t over_req
      = { .width = img_w, .height = img_h, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = TRUE };
  dt_masks_debug_write_png(dev, &form, &alpha_req, alpha_path);
  dt_masks_debug_write_png(dev, &form, &over_req, over_path);

  char *alpha_name = g_strdup_printf("%s-alpha", name);
  char *over_name = g_strdup_printf("%s-overlay", name);
  const gboolean baseline_ok = _baseline_check(alpha_path, alpha_name)
                               & _baseline_check(over_path, over_name);
  g_free(alpha_name);
  g_free(over_name);

  /* A picture of exactly what is owed and missing: red where the disc union covers a pixel the
   * rasteriser left empty, over the mask itself. This is the artefact to look at first when a
   * case fails -- it says WHERE the stroke went missing, which no scalar can. */
  if(!IS_NULL_PTR(reference) && missing > 0)
    _write_missing_map(dir, name, mask, reference, img_w, img_h);

  if(missing > 0)
  {
    char *csv = g_strdup_printf("%s/%s-outline.csv", dir, name);
    dt_masks_debug_write_outline_csv(dev, &form, csv);
    g_free(csv);
  }

  const gboolean ok = (largest <= budget_px) && baseline_ok;
  printf("[%s] %-22s %5dx%-5d missing coverage %6d px, largest run %5d px", ok ? "PASS" : "FAIL",
         name, img_w, img_h, missing, largest);
  if(largest > 0) printf(" around (%d,%d)", cx, cy);
  printf("  budget %d  -> %s\n", budget_px, alpha_path);
  if(!ok) failures++;

  free(reference);
  dt_free_align(mask);
  g_free(alpha_path);
  g_free(over_path);
  g_list_free_full(form.points, free);
}

static void _run_brush_case(dt_develop_t *dev, const float table[][9], const int count,
                            const char *name, const char *dir, const int budget_px)
{
  _run_brush_case_at(dev, table, count, name, dir, budget_px, IMG_W, IMG_H);
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
    _run_brush_case_at(&dev, _brush_1313, 11, nm, dir, 0, frames[f][0], frames[f][1]);
    g_free(nm);
  }

  _run_brush_case(&dev, _brush_cusp_tbl,      3, "brush-cusp",      dir, 0);
  _run_brush_case(&dev, _brush_hairpin_tbl,   3, "brush-hairpin",   dir, 0);
  _run_brush_case(&dev, _brush_zigzag_tbl,    6, "brush-zigzag",    dir, 0);
  _run_brush_case(&dev, _brush_selfcross_tbl, 5, "brush-selfcross", dir, 0);
  _run_brush_case(&dev, _brush_concave_tbl,   5, "brush-concave",   dir, 0);

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
      _run_case_at(&dev, &form, nm, dir, 0, 0, poly_frames[f][0], poly_frames[f][1]);
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
    _run_case(&dev, &form, "polygon-comb", dir, 0, 0);
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
