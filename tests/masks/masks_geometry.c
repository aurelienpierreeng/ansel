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

static dt_masks_node_polygon_t *_polygon_node(const float x, const float y, const float c1x, const float c1y,
                                              const float c2x, const float c2y, const float radius)
{
  dt_masks_node_polygon_t *n = (dt_masks_node_polygon_t *)calloc(1, sizeof(dt_masks_node_polygon_t));
  n->node[0] = x;     n->node[1] = y;
  n->ctrl1[0] = c1x;  n->ctrl1[1] = c1y;
  n->ctrl2[0] = c2x;  n->ctrl2[1] = c2y;
  n->border[0] = n->border[1] = radius;
  n->state = DT_MASKS_POINT_STATE_NORMAL;
  return n;
}

/** Straight-segment control points: a node whose handles sit on the chords is a corner. */
static void _brush_straight(GList **points, const float pts[][2], const int n, const float radius)
{
  for(int i = 0; i < n; i++)
  {
    const float px = pts[i][0], py = pts[i][1];
    const int prev = (i == 0) ? 0 : i - 1;
    const int next = (i == n - 1) ? n - 1 : i + 1;
    const float c1x = px + (pts[prev][0] - px) * 0.25f, c1y = py + (pts[prev][1] - py) * 0.25f;
    const float c2x = px + (pts[next][0] - px) * 0.25f, c2y = py + (pts[next][1] - py) * 0.25f;
    *points = g_list_append(*points, _brush_node(px, py, c1x, c1y, c2x, c2y, radius));
  }
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
      /* the radius interpolates between the two nodes exactly as _brush_points_recurs does */
      const float sm = t * t * (3.0f - 2.0f * t);
      const float r = (table[seg][6] + (table[seg + 1][6] - table[seg][6]) * sm) * radius_scale;
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
  uint8_t *miss = (uint8_t *)calloc((size_t)w * h, 1);
  int *stack = (int *)malloc(sizeof(int) * (size_t)w * h);
  if(IS_NULL_PTR(miss) || IS_NULL_PTR(stack)) { free(miss); free(stack); return; }

  /* ANY coverage counts, not a thresholded core. `border' is the OUTER radius: the stroke is
   * solid in the middle and fades to zero at that edge, so most of the disc legitimately holds
   * values below a half. What cannot be legitimate is a pixel the disc covers with no coverage
   * at all -- that is the stroke missing, which is the defect this corpus is about. */
  for(size_t i = 0; i < (size_t)w * h; i++)
    if(reference[i] && mask[i] <= 0.0f) { miss[i] = 1; (*total)++; }

  uint8_t *seen = (uint8_t *)calloc((size_t)w * h, 1);
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
          seen[ni] = 1; stack[top++] = (int)ni;
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

  int *label = (int *)calloc((size_t)w * h, sizeof(int));
  int *stack = (int *)malloc(sizeof(int) * (size_t)w * h);
  if(IS_NULL_PTR(label) || IS_NULL_PTR(stack)) { free(label); free(stack); return; }

  size_t painted = 0;
  for(size_t i = 0; i < (size_t)w * h; i++) if(mask[i] > 0.5f) painted++;
  *coverage = (double)painted / ((double)w * h);

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
          stack[top++] = (int)ni;
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

static void _run_brush_case(dt_develop_t *dev, const float table[][9], const int count,
                            const char *name, const char *dir, const int budget_px)
{
  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_BRUSH;
  form.functions = &dt_masks_functions_brush;
  form.version = 6;
  form.formid = 900;
  g_strlcpy(form.name, name, sizeof(form.name));
  form.points = _brush_from_table(table, count);

  float *mask = dt_masks_debug_rasterise(dev, &form, IMG_W, IMG_H);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", name);
    failures++;
    g_list_free_full(form.points, free);
    return;
  }

  uint8_t *reference = (uint8_t *)malloc((size_t)IMG_W * IMG_H);
  int missing = 0, largest = 0, cx = -1, cy = -1;
  if(!IS_NULL_PTR(reference))
  {
    _brush_reference(table, count, IMG_W, IMG_H, reference);
    _missing_coverage(mask, reference, IMG_W, IMG_H, &missing, &largest, &cx, &cy);
  }

  char *alpha_path = g_strdup_printf("%s/%s-alpha.png", dir, name);
  char *over_path = g_strdup_printf("%s/%s-overlay.png", dir, name);
  const dt_masks_debug_request_t alpha_req
      = { .width = IMG_W, .height = IMG_H, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = FALSE };
  const dt_masks_debug_request_t over_req
      = { .width = IMG_W, .height = IMG_H, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = TRUE };
  dt_masks_debug_write_png(dev, &form, &alpha_req, alpha_path);
  dt_masks_debug_write_png(dev, &form, &over_req, over_path);

  /* A picture of exactly what is owed and missing: red where the disc union covers a pixel the
   * rasteriser left empty, over the mask itself. This is the artefact to look at first when a
   * case fails -- it says WHERE the stroke went missing, which no scalar can. */
  if(!IS_NULL_PTR(reference) && missing > 0)
  {
    char *miss_path = g_strdup_printf("%s/%s-missing.png", dir, name);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, IMG_W, IMG_H);
    if(cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_flush(surf);
      uint8_t *const px = cairo_image_surface_get_data(surf);
      const int stride = cairo_image_surface_get_stride(surf);
      for(int y = 0; y < IMG_H; y++)
      {
        uint32_t *const row = (uint32_t *)(px + (size_t)y * stride);
        for(int x = 0; x < IMG_W; x++)
        {
          const size_t i = (size_t)y * IMG_W + x;
          const uint32_t g = (uint32_t)(CLAMPF(mask[i], 0.0f, 1.0f) * 200.0f + 0.5f);
          row[x] = (reference[i] && mask[i] <= 0.0f) ? 0x00FF2020u : ((g << 16) | (g << 8) | g);
        }
      }
      cairo_surface_mark_dirty(surf);
      cairo_surface_write_to_png(surf, miss_path);
    }
    cairo_surface_destroy(surf);
    g_free(miss_path);
  }

  if(missing > 0)
  {
    char *csv = g_strdup_printf("%s/%s-outline.csv", dir, name);
    dt_masks_debug_write_outline_csv(dev, &form, csv);
    g_free(csv);
  }

  const gboolean ok = (largest <= budget_px);
  printf("[%s] %-22s missing coverage %6d px, largest run %5d px", ok ? "PASS" : "FAIL", name,
         missing, largest);
  if(largest > 0) printf(" around (%d,%d)", cx, cy);
  printf("  budget %d  -> %s\n", budget_px, alpha_path);
  if(!ok) failures++;

  free(reference);
  dt_free_align(mask);
  g_free(alpha_path);
  g_free(over_path);
  g_list_free_full(form.points, free);
}

static void _run_case(dt_develop_t *dev, dt_masks_form_t *form, const char *name, const char *dir,
                      const int max_holes, const int max_hole_px)
{
  float *mask = dt_masks_debug_rasterise(dev, form, IMG_W, IMG_H);
  if(IS_NULL_PTR(mask))
  {
    printf("[FAIL] %-22s rasterisation returned nothing\n", name);
    failures++;
    return;
  }

  double coverage = 0.0;
  int holes = 0, largest = 0;
  _measure(mask, IMG_W, IMG_H, &coverage, &holes, &largest);
  dt_free_align(mask);

  char *alpha_path = g_strdup_printf("%s/%s-alpha.png", dir, name);
  char *over_path = g_strdup_printf("%s/%s-overlay.png", dir, name);
  const dt_masks_debug_request_t alpha_req
      = { .width = IMG_W, .height = IMG_H, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = FALSE };
  const dt_masks_debug_request_t over_req
      = { .width = IMG_W, .height = IMG_H, .backdrop = DT_MASKS_DEBUG_BACKDROP_RASTER, .draw_overlay = TRUE };
  dt_masks_debug_write_png(dev, form, &alpha_req, alpha_path);
  dt_masks_debug_write_png(dev, form, &over_req, over_path);

  const gboolean ok = (holes <= max_holes) && (largest <= max_hole_px);
  printf("[%s] %-22s coverage %.4f  enclosed holes %d (largest %d px)  budget %d/%d  -> %s\n",
         ok ? "PASS" : "FAIL", name, coverage, holes, largest, max_holes, max_hole_px, alpha_path);
  if(!ok) failures++;

  g_free(alpha_path);
  g_free(over_path);
}

/* ------------------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
  const char *dir = (argc > 1) ? argv[1] : "/tmp";
  g_mkdir_with_parents(dir, 0755);

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
  _run_brush_case(&dev, _brush_1313, 11, "brush-1313-cusp", dir, 0);

  _run_brush_case(&dev, _brush_cusp_tbl,      3, "brush-cusp",      dir, 0);
  _run_brush_case(&dev, _brush_hairpin_tbl,   3, "brush-hairpin",   dir, 0);
  _run_brush_case(&dev, _brush_zigzag_tbl,    6, "brush-zigzag",    dir, 0);
  _run_brush_case(&dev, _brush_selfcross_tbl, 5, "brush-selfcross", dir, 0);
  _run_brush_case(&dev, _brush_concave_tbl,   5, "brush-concave",   dir, 0);

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

  /* 5. A circle, as the control: no joints, no folds. If this ever grows a hole the fault is
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

  printf("%s: %d failing case(s)\n", failures ? "FAIL" : "PASS", failures);

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
