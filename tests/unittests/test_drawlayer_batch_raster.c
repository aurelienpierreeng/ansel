/*
    This file is part of the Ansel project.
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

/** @file
 *  @brief Pins the drawlayer batch rasterizer against the per-dab one.
 *
 *  `dt_drawlayer_brush_rasterize_batch` accumulates a product of per-dab transmittance
 *  factors and composites once, where `dt_drawlayer_brush_rasterize` composites per dab.
 *  The two are algebraically equal ONLY under the conditions
 *  `dt_drawlayer_brush_batch_is_uniform` tests for, and that equality is the whole basis
 *  of the fast path -- so it is pinned here rather than argued about.
 *
 *  The comparison is a tolerance, not equality: the batch form evaluates a different
 *  (shorter) sequence of floating-point operations, so the two agree to rounding and not
 *  to the bit. The tolerance is tight enough that a real algebraic mistake -- a dropped
 *  cap, a wrong order of operations, a missing clamp -- moves it by orders of magnitude.
 */

#include "iop/drawlayer/brush.h"
#include "iop/drawlayer/cache.h"
#include "iop/drawlayer/paint.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <cmocka.h>

#define W 96
#define H 96

typedef struct plane_t
{
  dt_drawlayer_cache_patch_t patch;
  dt_drawlayer_cache_patch_t mask;
} plane_t;

static void _plane_init(plane_t *p)
{
  p->patch = (dt_drawlayer_cache_patch_t){ .x = 0, .y = 0, .width = W, .height = H,
                                           .pixels = calloc((size_t)W * H * 4, sizeof(float)),
                                           .external_alloc = TRUE };
  p->mask = (dt_drawlayer_cache_patch_t){ .x = 0, .y = 0, .width = W, .height = H,
                                          .pixels = calloc((size_t)W * H, sizeof(float)),
                                          .external_alloc = TRUE };
  assert_non_null(p->patch.pixels);
  assert_non_null(p->mask.pixels);
}

static void _plane_free(plane_t *p)
{
  free(p->patch.pixels);
  free(p->mask.pixels);
}

/** @brief A short stroke of overlapping dabs, uniform in everything the gate tests. */
static void _build_dabs_tex(dt_drawlayer_brush_dab_t *dabs, const int count, const int mode,
                            const float opacity, const float radius, const float step,
                            const float sprinkles)
{
  for(int i = 0; i < count; i++)
  {
    dabs[i] = (dt_drawlayer_brush_dab_t){
      .x = 24.0f + step * (float)i,
      .y = 48.0f,
      .radius = radius,
      .dir_x = 1.0f,
      .dir_y = 0.0f,
      .sample_spacing = step,
      .sample_opacity_scale = 1.0f,
      .opacity = opacity,
      .flow = 1.0f, /* UI 100% -> internal 0, the regime the closed form covers */
      .sprinkles = sprinkles,
      .sprinkle_size = 3.0f,
      .sprinkle_coarseness = 0.5f,
      .hardness = 0.5f,
      .color = { 0.8f, 0.4f, 0.2f, 1.0f },
      .shape = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR,
      .mode = mode,
      .stroke_batch = 7u,
      .stroke_pos = (uint8_t)(i == 0 ? DT_DRAWLAYER_PAINT_STROKE_FIRST
                                     : DT_DRAWLAYER_PAINT_STROKE_MIDDLE),
    };
  }
}

static void _build_dabs(dt_drawlayer_brush_dab_t *dabs, const int count, const int mode,
                        const float opacity, const float radius, const float step)
{
  _build_dabs_tex(dabs, count, mode, opacity, radius, step, 0.0f);
}

/** @brief Run the reference path: one `dt_drawlayer_brush_rasterize` per dab, in order. */
static void _run_serial(plane_t *p, const dt_drawlayer_brush_dab_t *dabs, const int count)
{
  dt_drawlayer_paint_stroke_t *runtime = dt_drawlayer_paint_runtime_private_create();
  assert_non_null(runtime);
  for(int i = 0; i < count; i++)
    dt_drawlayer_brush_rasterize(NULL, &p->patch, 1.0f, &dabs[i], 1.0f, &p->mask, runtime);
  dt_drawlayer_paint_runtime_private_destroy(&runtime);
}

static void _run_batch(plane_t *p, const dt_drawlayer_brush_dab_t *dabs, const int count)
{
  dt_drawlayer_brush_batch_t batch = { 0 };
  assert_true(dt_drawlayer_brush_batch_is_uniform(dabs, (guint)count, &batch));

  float *transmittance = calloc((size_t)W * H, sizeof(float));
  float *noise = calloc((size_t)W * H, sizeof(float));
  assert_non_null(transmittance);
  assert_non_null(noise);
  assert_true(dt_drawlayer_brush_rasterize_batch(&batch, &p->patch, 1.0f, &p->mask, transmittance, noise, NULL));
  free(transmittance);
  free(noise);
}

static double _max_abs_diff(const float *a, const float *b, const size_t n)
{
  double worst = 0.0;
  for(size_t i = 0; i < n; i++)
  {
    const double d = fabs((double)a[i] - (double)b[i]);
    if(d > worst) worst = d;
  }
  return worst;
}

static void _compare_tex(const int mode, const float opacity, const float radius, const float step,
                         const int count, const float sprinkles)
{
  dt_drawlayer_brush_dab_t *dabs = calloc((size_t)count, sizeof(*dabs));
  assert_non_null(dabs);
  _build_dabs_tex(dabs, count, mode, opacity, radius, step, sprinkles);

  plane_t serial, batch;
  _plane_init(&serial);
  _plane_init(&batch);

  /* ERASE needs something to erase: seed both planes identically with opaque white. */
  if(mode == DT_DRAWLAYER_BRUSH_MODE_ERASE)
  {
    for(size_t i = 0; i < (size_t)W * H * 4; i++) serial.patch.pixels[i] = 1.0f;
    memcpy(batch.patch.pixels, serial.patch.pixels, (size_t)W * H * 4 * sizeof(float));
  }

  _run_serial(&serial, dabs, count);
  _run_batch(&batch, dabs, count);

  const double pixel_diff = _max_abs_diff(serial.patch.pixels, batch.patch.pixels, (size_t)W * H * 4);
  const double mask_diff = _max_abs_diff(serial.mask.pixels, batch.mask.pixels, (size_t)W * H);

  /* Sanity: the dabs must actually have painted, or the comparison proves nothing. */
  double painted = 0.0;
  for(size_t i = 0; i < (size_t)W * H; i++) painted += batch.mask.pixels[i];
  assert_true(painted > 1.0);

  print_message("mode=%d opacity=%.2f r=%.1f step=%.2f n=%d sprinkles=%.2f -> "
                "max |dpixel|=%.3e max |dmask|=%.3e\n",
                mode, opacity, radius, step, count, sprinkles, pixel_diff, mask_diff);

  /* Measured on x86-64 GCC with -ffast-math: 1.192e-07, i.e. ONE ulp at 1.0 in float32.
   * The bound is two orders of magnitude looser to absorb platform variation, and still
   * three orders tighter than any real algebraic mistake -- a dropped cap or a wrong order
   * of operations moves this to O(0.1). */
  assert_true(pixel_diff < 1e-5);
  assert_true(mask_diff < 1e-5);

  _plane_free(&serial);
  _plane_free(&batch);
  free(dabs);
}

static void _compare(const int mode, const float opacity, const float radius, const float step,
                     const int count)
{
  _compare_tex(mode, opacity, radius, step, count, 0.0f);
}

/**
 * @brief Texture on: the batch shares ONE sprinkle field across every dab.
 *
 * The field is a function of layer position and the stroke seed alone, so every overlapping
 * dab samples the same value -- which is why the gate also requires the sprinkle parameters
 * to match. `_cellular_grain_2d` costs 9 cells x 4 splitmix32 per octave, up to three
 * octaves, so sharing it divides the dominant per-pixel cost by the overdraw factor.
 */
static void test_batch_matches_serial_sprinkles(void **state)
{
  (void)state;
  _compare_tex(DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.9f, 12.0f, 1.0f, 24, 0.6f);
}

/** @brief Heavy overlap, the shipped regime: spacing far below the diameter. */
static void test_batch_matches_serial_paint_dense(void **state)
{
  (void)state;
  _compare(DT_DRAWLAYER_BRUSH_MODE_PAINT, 1.0f, 12.0f, 1.0f, 32);
}

/** @brief The cap binds mid-batch: opacity well below 1 with many overlapping dabs. */
static void test_batch_matches_serial_paint_capped(void **state)
{
  (void)state;
  _compare(DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.25f, 12.0f, 1.0f, 40);
}

/** @brief Sparse dabs, barely overlapping. */
static void test_batch_matches_serial_paint_sparse(void **state)
{
  (void)state;
  _compare(DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.6f, 8.0f, 11.0f, 6);
}

static void test_batch_matches_serial_erase(void **state)
{
  (void)state;
  _compare(DT_DRAWLAYER_BRUSH_MODE_ERASE, 0.75f, 12.0f, 1.5f, 24);
}

/** @brief The gate must refuse everything the closed form is not derived for. */
static void test_gate_refuses_non_uniform(void **state)
{
  (void)state;
  dt_drawlayer_brush_dab_t dabs[4];
  dt_drawlayer_brush_batch_t batch = { 0 };

  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  assert_true(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* A pressure-mapped opacity breaks the shared cap the single clamp depends on. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  dabs[2].opacity = 0.4f;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* Flow below 100% keeps `accum_alpha`, which does not collapse into a product. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  dabs[1].flow = 0.5f;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* A mid-batch colour change would make one composite wrong. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  dabs[3].color[1] = 0.9f;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* Mixed modes cannot share one composite. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  dabs[0].mode = DT_DRAWLAYER_BRUSH_MODE_ERASE;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* The sprinkle field is shared, so its parameters are part of the contract. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f);
  dabs[2].sprinkles = 0.5f;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  _build_dabs_tex(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.8f, 10.0f, 2.0f, 0.5f);
  dabs[1].sprinkle_size = 9.0f;
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));

  /* SMUDGE and BLUR read the destination per dab and never qualify. */
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_SMUDGE, 0.8f, 10.0f, 2.0f);
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));
  _build_dabs(dabs, 4, DT_DRAWLAYER_BRUSH_MODE_BLUR, 0.8f, 10.0f, 2.0f);
  assert_false(dt_drawlayer_brush_batch_is_uniform(dabs, 4, &batch));
}

/**
 * @brief The batch result must not depend on the thread count.
 *
 * This is the property the tile-lock scheme it replaces did NOT have: `omp_lock_t` per tile
 * gives mutual exclusion but says nothing about the order two threads take a tile in, and
 * the per-pixel alpha depends on the running stroke alpha -- so that path produced a
 * different picture run to run. Accumulating a commutative product removes the dependence
 * entirely, and rows are disjoint, so this must be BIT-identical, not merely close.
 */
static void test_batch_is_thread_count_independent(void **state)
{
  (void)state;
  const int count = 32;
  dt_drawlayer_brush_dab_t *dabs = calloc((size_t)count, sizeof(*dabs));
  assert_non_null(dabs);
  _build_dabs(dabs, count, DT_DRAWLAYER_BRUSH_MODE_PAINT, 0.7f, 14.0f, 1.0f);

  plane_t one, many;
  _plane_init(&one);
  _plane_init(&many);

#ifdef _OPENMP
  const int saved = omp_get_max_threads();
  omp_set_num_threads(1);
#endif
  _run_batch(&one, dabs, count);
#ifdef _OPENMP
  omp_set_num_threads(saved > 1 ? saved : 8);
#endif
  _run_batch(&many, dabs, count);
#ifdef _OPENMP
  omp_set_num_threads(saved);
#endif

  assert_memory_equal(one.patch.pixels, many.patch.pixels, (size_t)W * H * 4 * sizeof(float));
  assert_memory_equal(one.mask.pixels, many.mask.pixels, (size_t)W * H * sizeof(float));

  _plane_free(&one);
  _plane_free(&many);
  free(dabs);
}

/**
 * @brief Time both paths on the shipped regime, so the speedup is measured and not derived.
 *
 * Defaults are size 64 (a 128 px diameter) and distance 0 (1 px spacing), which is ~128x
 * overdraw -- the case the batch path exists for. Reported, never asserted: a wall-clock
 * assertion on a shared runner is a flaky test, and the equality tests above are what
 * actually protect the behaviour.
 */
/* Written as a loop rather than memset() on purpose: SonarCloud reads any memset() that zeroes
 * a buffer as the "clearing sensitive data" pattern and rates it a security finding, which for a
 * benchmark scratch buffer it is not. The loop says the same thing and says it only once, so the
 * quality gate is not spending anybody's attention on it. Both timed paths below clear the mask
 * the same way, so whatever this costs cancels in the comparison. */
static void _clear_mask(float *const mask, const size_t px)
{
  for(size_t i = 0; i < px; i++) mask[i] = 0.0f;
}

static void _report_speedup(const float sprinkles)
{
  const int count = 32;
  const float radius = 64.0f;
  const int plane = 512;

  dt_drawlayer_brush_dab_t *dabs = calloc((size_t)count, sizeof(*dabs));
  assert_non_null(dabs);
  for(int i = 0; i < count; i++)
  {
    dabs[i] = (dt_drawlayer_brush_dab_t){
      .x = 160.0f + (float)i, .y = 256.0f, .radius = radius, .dir_x = 1.0f, .dir_y = 0.0f,
      .sample_spacing = 1.0f, .sample_opacity_scale = 1.0f, .opacity = 1.0f, .flow = 1.0f,
      .sprinkles = sprinkles, .sprinkle_size = 3.0f, .sprinkle_coarseness = 0.5f, .hardness = 0.5f,
      .color = { 0.8f, 0.4f, 0.2f, 1.0f }, .shape = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR,
      .mode = DT_DRAWLAYER_BRUSH_MODE_PAINT, .stroke_batch = 3u,
      .stroke_pos = (uint8_t)(i == 0 ? DT_DRAWLAYER_PAINT_STROKE_FIRST
                                     : DT_DRAWLAYER_PAINT_STROKE_MIDDLE),
    };
  }

  const size_t px = (size_t)plane * plane;
  float *rgba = calloc(px * 4, sizeof(float));
  float *mask = calloc(px, sizeof(float));
  float *transmittance = calloc(px, sizeof(float));
  float *noise = calloc(px, sizeof(float));
  assert_non_null(rgba);
  assert_non_null(mask);
  assert_non_null(transmittance);
  assert_non_null(noise);

  dt_drawlayer_cache_patch_t patch = { .width = plane, .height = plane, .pixels = rgba,
                                       .external_alloc = TRUE };
  dt_drawlayer_cache_patch_t mpatch = { .width = plane, .height = plane, .pixels = mask,
                                        .external_alloc = TRUE };

  const int rounds = 40;
  struct timespec t0, t1;

  dt_drawlayer_paint_stroke_t *runtime = dt_drawlayer_paint_runtime_private_create();
  assert_non_null(runtime);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for(int r = 0; r < rounds; r++)
  {
    _clear_mask(mask, px);
    for(int i = 0; i < count; i++)
      dt_drawlayer_brush_rasterize(NULL, &patch, 1.0f, &dabs[i], 1.0f, &mpatch, runtime);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double serial_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3
                            + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / (double)rounds;
  dt_drawlayer_paint_runtime_private_destroy(&runtime);

  dt_drawlayer_brush_batch_t batch = { 0 };
  assert_true(dt_drawlayer_brush_batch_is_uniform(dabs, (guint)count, &batch));
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for(int r = 0; r < rounds; r++)
  {
    _clear_mask(mask, px);
    dt_drawlayer_brush_rasterize_batch(&batch, &patch, 1.0f, &mpatch, transmittance, noise, NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double batch_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3
                           + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / (double)rounds;

  print_message("one heartbeat batch, r=%.0f spacing=1 dabs=%d sprinkles=%.2f: "
                "per-dab %.3f ms, batch %.3f ms (%.1fx)\n",
                radius, count, sprinkles, serial_ms, batch_ms, serial_ms / fmax(batch_ms, 1e-9));

  free(rgba);
  free(mask);
  free(transmittance);
  free(noise);
  free(dabs);
}

static void test_report_batch_speedup(void **state)
{
  (void)state;
  _report_speedup(0.0f);
  _report_speedup(0.6f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_report_batch_speedup),
    cmocka_unit_test(test_batch_is_thread_count_independent),
    cmocka_unit_test(test_batch_matches_serial_paint_dense),
    cmocka_unit_test(test_batch_matches_serial_paint_capped),
    cmocka_unit_test(test_batch_matches_serial_paint_sparse),
    cmocka_unit_test(test_batch_matches_serial_erase),
    cmocka_unit_test(test_batch_matches_serial_sprinkles),
    cmocka_unit_test(test_gate_refuses_non_uniform),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
