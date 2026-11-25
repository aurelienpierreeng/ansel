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

/** A zone painted over a speck of dust finds it, all of it, and nothing where there is none.
 *
 * The images are synthetic: a level with a grain of 0.5 %, a speck that blocks 70 % of the
 * light in its core and fades over a blurred rim, a light one that lets 60 % more through, a wide
 * one that blocks 1 %, a scratch 3 px wide no denser than 1.7 times the grain, straight, turning
 * lighter along it or bending on an arc, and for the source,
 * vertical stripes the heal must stay in phase with. The thresholds asserted are those of
 * pixel/dust.c, at full resolution.
 */

#include "caches/pixelpipe_cache.h"
#include "darktable.h"
#include "pixel/dust.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define W 200
#define H 200
#define SPECK_X 100.0f
#define SPECK_Y 100.0f
#define SPECK_RADIUS 4.0f
#define RIM 2.0f
#define FAINT_RADIUS 12.0f
#define FAINT_RIM 3.0f
#define STRIPE_PERIOD 6

static int _setup(void **state __attribute__((unused)))
{
  // The box filters size their per-thread scratch by the application's thread budget, which
  // dt_init() seeds and nothing does in a unit test.
  darktable.num_openmp_threads = omp_get_max_threads();
  return dt_dev_pixelpipe_cache_init((size_t)64 * 1024 * 1024, FALSE, FALSE) ? 0 : -1;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

/* A repeatable grain: the sum of four uniform draws of a linear congruential generator. */
static float _grain(uint32_t *seed)
{
  float sum = 0.0f;
  for(int k = 0; k < 4; k++)
  {
    *seed = *seed * 1664525u + 1013904223u;
    sum += (float)(*seed >> 8) / (float)(1u << 24);
  }
  return sum - 2.0f;
}

/* How much light a speck centred at (cx, cy) lets through at (x, y): `core` in its core, of
 * `radius`, then back to all of it across a linear rim. */
static float _speck_shape(const float x, const float y, const float cx, const float cy, const float core,
                          const float radius, const float rim)
{
  const float r = hypotf(x - cx, y - cy);
  const float t = fminf(fmaxf((r - radius) / rim, 0.0f), 1.0f);
  return core + (1.0f - core) * t;
}

/* The dark speck: 30 % of the light in its core. */
static float _speck(const float x, const float y, const float cx, const float cy)
{
  return _speck_shape(x, y, cx, cy, 0.3f, SPECK_RADIUS, RIM);
}

/* The light speck: 60 % more light in its core, as a pinhole, a scratch in the emulsion or dust on
 * the film when it was exposed leaves on a negative. */
static float _light_speck(const float x, const float y, const float cx, const float cy)
{
  return _speck_shape(x, y, cx, cy, 1.6f, SPECK_RADIUS, RIM);
}

static void _lighten(float *img, const float cx, const float cy)
{
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
      for(int c = 0; c < 4; c++) img[4 * ((size_t)y * W + x) + c] *= _light_speck(x, y, cx, cy);
}

/* The image: a level of 0.4, optionally striped, with the grain, the dark specks listed, and
 * optionally the wide faint one at the centre. */
static float *_image(const gboolean striped, const float *specks, const int count, const gboolean faint)
{
  float *img = dt_alloc_align_float((size_t)4 * W * H);
  uint32_t seed = 12345u;
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      float v = 0.4f * (1.0f + 0.005f * _grain(&seed));
      // stripes of 0.05 D: a texture, not specks -- each dark stripe is a peak as narrow as a speck
      if(striped) v *= (x % STRIPE_PERIOD) < STRIPE_PERIOD / 2 ? 1.0f : 0.9f;
      for(int k = 0; k < count; k++) v *= _speck(x, y, specks[2 * k], specks[2 * k + 1]);
      if(faint) v *= _speck_shape(x, y, SPECK_X, SPECK_Y, 0.99f, FAINT_RADIUS, FAINT_RIM);
      for(int c = 0; c < 4; c++) img[4 * ((size_t)y * W + x) + c] = v;
    }
  return img;
}

/* A round zone, as a click of the brush paints it; the mask is freed with dt_free_align(). */
static dt_dust_zone_t _disc(const float cx, const float cy, const int radius, uint8_t **mask)
{
  const int side = 2 * radius + 1;
  *mask = dt_calloc_align((size_t)side * side);
  for(int y = 0; y < side; y++)
    for(int x = 0; x < side; x++)
      (*mask)[y * side + x] = (x - radius) * (x - radius) + (y - radius) * (y - radius) <= radius * radius;
  return (dt_dust_zone_t){ .mask = *mask, .box_x = (int)lrintf(cx) - radius, .box_y = (int)lrintf(cy) - radius,
                           .box_width = side, .box_height = side };
}

static gboolean _in_speck(const dt_dust_speck_t *speck, const int x, const int y)
{
  const int bx = x - speck->box_x;
  const int by = y - speck->box_y;
  return bx >= 0 && by >= 0 && bx < speck->box_width && by < speck->box_height
         && speck->mask[by * speck->box_width + bx];
}

/* Painted off its centre, the speck is found with its whole rim: every pixel that lets through
 * less than 90 % of the light is on the mask, and nothing beyond the rim's end plus the smoothing
 * and the margin. */
static void _a_zone_finds_the_whole_speck(void **state __attribute__((unused)))
{
  const float specks[2] = { SPECK_X, SPECK_Y };
  float *img = _image(FALSE, specks, 1, FALSE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X + 3.0f, SPECK_Y - 2.0f, 12, &mask);
  dt_dust_speck_t speck;

  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_true(fabsf(speck.center[0] - SPECK_X) < 1.0f);
  assert_true(fabsf(speck.center[1] - SPECK_Y) < 1.0f);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      if(_speck(x, y, SPECK_X, SPECK_Y) < 0.9f) assert_true(_in_speck(&speck, x, y));
      if(hypotf(x - SPECK_X, y - SPECK_Y) > SPECK_RADIUS + RIM + 4.0f) assert_false(_in_speck(&speck, x, y));
    }
  assert_true(speck.contrast > 5.0f);

  dt_dust_speck_cleanup(&speck);
  assert_null(speck.mask);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A speck lighter than its surroundings is found as a dark one is: every pixel that lets through
 * 10 % more light than the level is on the mask, nothing beyond the rim's end plus the smoothing and
 * the margin. */
static void _a_light_speck_is_found(void **state __attribute__((unused)))
{
  float *img = _image(FALSE, NULL, 0, FALSE);
  _lighten(img, SPECK_X, SPECK_Y);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X + 3.0f, SPECK_Y - 2.0f, 12, &mask);
  dt_dust_speck_t speck;

  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_int_equal(speck.polarity, -1);
  assert_true(fabsf(speck.center[0] - SPECK_X) < 1.0f);
  assert_true(fabsf(speck.center[1] - SPECK_Y) < 1.0f);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      if(_light_speck(x, y, SPECK_X, SPECK_Y) > 1.1f) assert_true(_in_speck(&speck, x, y));
      if(hypotf(x - SPECK_X, y - SPECK_Y) > SPECK_RADIUS + RIM + 4.0f) assert_false(_in_speck(&speck, x, y));
    }

  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* Where there is only grain, a zone finds nothing. */
static void _a_zone_of_grain_finds_nothing(void **state __attribute__((unused)))
{
  const float specks[2] = { SPECK_X, SPECK_Y };
  float *img = _image(FALSE, specks, 1, FALSE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(40.0f, 40.0f, 12, &mask);
  dt_dust_speck_t speck;

  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_NOT_FOUND);
  assert_null(speck.mask);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A speck wider than the grain's reach that blocks 1 % of the light, 3.5 times the grain of a
 * pixel, is found by its smoothing: its core on the mask, the mask no wider than its rim plus the
 * smoothing and the margin. */
static void _a_wide_faint_speck_is_found(void **state __attribute__((unused)))
{
  float *img = _image(FALSE, NULL, 0, TRUE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X, SPECK_Y, 24, &mask);
  dt_dust_speck_t speck;

  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const float r = hypotf(x - SPECK_X, y - SPECK_Y);
      if(r <= FAINT_RADIUS - 2.0f) assert_true(_in_speck(&speck, x, y));
      if(r > FAINT_RADIUS + FAINT_RIM + 5.0f) assert_false(_in_speck(&speck, x, y));
    }

  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* The outline encloses about the area of the mask: the simplification strays by less than a
 * pixel, so on a speck of some fifty pixels the areas agree within a quarter. */
static void _the_outline_encloses_the_speck(void **state __attribute__((unused)))
{
  const float specks[2] = { SPECK_X, SPECK_Y };
  float *img = _image(FALSE, specks, 1, FALSE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X, SPECK_Y, 12, &mask);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);

  float *points = NULL;
  int count = 0;
  assert_int_equal(dt_dust_outline(&speck, 0.75f, &points, &count), DT_DUST_OK);
  assert_true(count >= 3);

  // shoelace area of the polygon against the pixel count of the mask
  double area = 0.0;
  for(int k = 0; k < count; k++)
  {
    const int l = (k + 1) % count;
    area += (double)points[2 * k] * points[2 * l + 1] - (double)points[2 * l] * points[2 * k + 1];
  }
  area = fabs(area) / 2.0;
  int pixels = 0;
  for(int i = 0; i < speck.box_width * speck.box_height; i++) pixels += speck.mask[i];
  assert_true(fabs(area - pixels) < 0.25 * pixels);

  dt_free_align(points);
  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* The distance from (x, y) to the closed polygon's nearest edge. */
static float _distance_to_outline(const float *points, const int count, const float x, const float y)
{
  float nearest = FLT_MAX;
  for(int k = 0; k < count; k++)
  {
    const int l = (k + 1) % count;
    const float ax = points[2 * k], ay = points[2 * k + 1];
    const float ex = points[2 * l] - ax, ey = points[2 * l + 1] - ay;
    const float length2 = fmaxf(ex * ex + ey * ey, 1e-12f);
    const float t = fminf(fmaxf(((x - ax) * ex + (y - ay) * ey) / length2, 0.0f), 1.0f);
    nearest = fminf(nearest, hypotf(x - ax - t * ex, y - ay - t * ey));
  }
  return nearest;
}

/* A fibre, a band winding over 140 pixels, outlined at three tolerances: a larger one takes fewer
 * nodes, the outline stays within it of the band's edge, and none under three quarters of a pixel
 * follows the pixel steps closer. Its half width is what dt_dust_find_in_zone() would give it: the
 * band winds 12 px each side of its axis and is 6 px thick at most, so no tolerance tried is capped. */
static void _a_smoother_outline_takes_fewer_nodes_within_its_tolerance(void **state __attribute__((unused)))
{
  dt_dust_speck_t speck = { .box_x = 0, .box_y = 0, .box_width = 160, .box_height = 80, .half_width = 19.0f };
  speck.mask = dt_calloc_align((size_t)speck.box_width * speck.box_height);
  assert_non_null(speck.mask);
  for(int x = 10; x < 150; x++)
  {
    const float middle = 40.0f + 12.0f * sinf(x / 15.0f);
    const float half = 4.0f + 2.0f * sinf(x / 7.0f);
    for(int y = 0; y < speck.box_height; y++)
      if(fabsf(y + 0.5f - middle) <= half) speck.mask[y * speck.box_width + x] = 1;
  }

  const float tolerances[3] = { 0.0f, 0.75f, 3.0f };
  int counts[3] = { 0 };
  for(int t = 0; t < 3; t++)
  {
    float *points = NULL;
    assert_int_equal(dt_dust_outline(&speck, tolerances[t], &points, &counts[t]), DT_DUST_OK);
    // the corners of the band's edge pixels, on the edge, within the tolerance of the outline
    const float stray = fmaxf(tolerances[t], 0.75f);
    for(int y = 0; y < speck.box_height; y++)
      for(int x = 0; x < speck.box_width; x++)
      {
        if(!speck.mask[y * speck.box_width + x]) continue;
        const gboolean edge = x == 0 || y == 0 || !speck.mask[y * speck.box_width + x - 1]
                              || !speck.mask[(y - 1) * speck.box_width + x]
                              || !speck.mask[y * speck.box_width + x + 1]
                              || !speck.mask[(y + 1) * speck.box_width + x];
        if(!edge) continue;
        // the pixel's corner nearest to the outline lies on the edge
        float nearest = FLT_MAX;
        for(int c = 0; c < 4; c++)
          nearest = fminf(nearest, _distance_to_outline(points, counts[t], x + (c & 1), y + (c >> 1)));
        assert_true(nearest <= stray + 1e-3f);
      }
    dt_free_align(points);
  }
  assert_int_equal(counts[0], counts[1]);
  assert_true(counts[2] < counts[1] / 2);

  dt_dust_speck_cleanup(&speck);
}

/* On stripes, the speck is found without them, and the source stays in phase with them and does
 * not land on a second speck nearby. */
static void _the_source_matches_the_texture(void **state __attribute__((unused)))
{
  const float specks[4] = { SPECK_X, SPECK_Y, SPECK_X + 24.0f, SPECK_Y };
  float *img = _image(TRUE, specks, 2, FALSE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X, SPECK_Y, 12, &mask);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_true(speck.radius < SPECK_RADIUS + RIM + 4.0f);

  float offset[2] = { 0.0f, 0.0f };
  assert_int_equal(dt_dust_find_source(&image, &speck, offset), DT_DUST_OK);
  assert_int_equal((int)lrintf(offset[0]) % STRIPE_PERIOD, 0);
  assert_true(hypotf(offset[0], offset[1]) >= 2.0f * speck.radius);
  // the moved speck must not cover the second one
  assert_true(hypotf(SPECK_X + offset[0] - specks[2], SPECK_Y + offset[1] - specks[3])
              > SPECK_RADIUS + speck.radius);

  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A source does not land on a light speck either. Over grain alone the dark speck's source is
 * (11, -21) when nothing else is near, measured; a light speck put there lies under the moved speck,
 * off the ring it is matched on, and only the check for another speck keeps the source off it. */
static void _the_source_keeps_clear_of_a_light_speck(void **state __attribute__((unused)))
{
  const float specks[2] = { SPECK_X, SPECK_Y };
  const float light[2] = { SPECK_X + 11.0f, SPECK_Y - 21.0f };
  float *img = _image(FALSE, specks, 1, FALSE);
  _lighten(img, light[0], light[1]);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _disc(SPECK_X, SPECK_Y, 12, &mask);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_int_equal(speck.polarity, 1);

  float offset[2] = { 0.0f, 0.0f };
  assert_int_equal(dt_dust_find_source(&image, &speck, offset), DT_DUST_OK);
  assert_true(hypotf(SPECK_X + offset[0] - light[0], SPECK_Y + offset[1] - light[1])
              > SPECK_RADIUS + speck.radius);

  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* The fraction of a speck's mask whose pixel centres the polygon encloses, even-odd, its nodes joined
 * by straight lines. A mask polygon is drawn smooth through its nodes, bulging out of them, so this
 * is only a floor; it is close where the nodes line up, along a scratch. */
static float _covered(const dt_dust_speck_t *speck, const float *points, const int count)
{
  int on = 0, in = 0;
  for(int y = 0; y < speck->box_height; y++)
    for(int x = 0; x < speck->box_width; x++)
    {
      if(!speck->mask[y * speck->box_width + x]) continue;
      on++;
      const float px = speck->box_x + x + 0.5f, py = speck->box_y + y + 0.5f;
      gboolean inside = FALSE;
      for(int i = 0, j = count - 1; i < count; j = i++)
      {
        const float xi = points[2 * i], yi = points[2 * i + 1], xj = points[2 * j], yj = points[2 * j + 1];
        if((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
      }
      in += inside;
    }
  return on ? (float)in / on : 0.0f;
}

/* A faint scratch, as measured on a scan: 3 px wide at half its height, 5 degrees off vertical,
 * 1.7 times the grain of one pixel at its middle. */
#define SCRATCH_X 80.0f
#define SCRATCH_SLOPE 0.0875f
#define SCRATCH_SIGMA 1.3f
#define SCRATCH_DENSITY 0.0021f

static float _scratch_x(const float y)
{
  return SCRATCH_X + SCRATCH_SLOPE * y;
}

/* The scratch, denser than the film from row `turn` down, lighter above it and a third as strong. */
static void _scratch_turning(float *img, const float turn)
{
  const float cosine = 1.0f / hypotf(1.0f, SCRATCH_SLOPE);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const float d = (x - _scratch_x(y)) * cosine;
      const float density = y >= turn ? SCRATCH_DENSITY : -SCRATCH_DENSITY / 3.0f;
      const float transmission = powf(10.0f, -density * expf(-d * d / (2.0f * SCRATCH_SIGMA * SCRATCH_SIGMA)));
      for(int c = 0; c < 4; c++) img[4 * ((size_t)y * W + x) + c] *= transmission;
    }
}

static void _scratch(float *img)
{
  _scratch_turning(img, 0.0f);
}

/* A stroke through `count` points, as the brush paints it: every pixel within `radius` of the
 * segments between them, the points given with the zone. The mask is freed with dt_free_align(). */
static dt_dust_zone_t _stroke_through(const float *points, const int count, const int radius, uint8_t **mask)
{
  float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
  for(int k = 0; k < count; k++)
  {
    x0 = fminf(x0, points[2 * k]);
    x1 = fmaxf(x1, points[2 * k]);
    y0 = fminf(y0, points[2 * k + 1]);
    y1 = fmaxf(y1, points[2 * k + 1]);
  }
  dt_dust_zone_t zone = { .box_x = (int)floorf(x0) - radius, .box_y = (int)floorf(y0) - radius,
                          .stroke = points, .stroke_count = count };
  zone.box_width = (int)ceilf(x1) + radius - zone.box_x + 1;
  zone.box_height = (int)ceilf(y1) + radius - zone.box_y + 1;
  *mask = dt_calloc_align((size_t)zone.box_width * zone.box_height);
  for(int y = 0; y < zone.box_height; y++)
    for(int x = 0; x < zone.box_width; x++)
      for(int k = 0; k + 1 < count; k++)
      {
        const float ax = points[2 * k], ay = points[2 * k + 1];
        const float ex = points[2 * k + 2] - ax, ey = points[2 * k + 3] - ay;
        const float px = zone.box_x + x - ax, py = zone.box_y + y - ay;
        const float t = fminf(fmaxf((px * ex + py * ey) / (ex * ex + ey * ey), 0.0f), 1.0f);
        if(hypotf(px - t * ex, py - t * ey) <= radius) (*mask)[y * zone.box_width + x] = 1;
      }
  zone.mask = *mask;
  return zone;
}

/* A stroke along the scratch, from y0 to y1: between the scratch's points there, written to
 * `points`, four floats. */
static dt_dust_zone_t _stroke(const int y0, const int y1, const int radius, uint8_t **mask, float *points)
{
  points[0] = _scratch_x(y0);
  points[1] = (float)y0;
  points[2] = _scratch_x(y1);
  points[3] = (float)y1;
  return _stroke_through(points, 2, radius, mask);
}

/* A stroke painted along a faint scratch finds the scratch, not the light speck it also crosses,
 * which stands out of the grain far more on one pixel: the stroke says a line was meant. The scratch
 * is on the mask along most of the stroke, and the speck is not; the source moves the scratch off
 * itself, where a shift along it would copy the scratch onto itself, and off the speck; and the
 * outline at the brush's smoothing still covers it. */
static void _a_stroke_finds_a_faint_scratch(void **state __attribute__((unused)))
{
  float *img = _image(FALSE, NULL, 0, FALSE);
  _scratch(img);
  const float light[2] = { _scratch_x(140.0f) + 12.0f, 140.0f };
  _lighten(img, light[0], light[1]);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  float stroke[4];
  const dt_dust_zone_t zone = _stroke(25, 175, 20, &mask, stroke);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_int_equal(speck.polarity, 1);
  int on = 0;
  for(int y = 40; y < 160; y++) on += _in_speck(&speck, (int)lrintf(_scratch_x(y)), y);
  assert_true(on >= 96);
  assert_false(_in_speck(&speck, (int)lrintf(light[0]), (int)lrintf(light[1])));
  assert_true(speck.half_width < 8.0f);

  // moved by the source offset, the scratch lies on none of itself, and off the light speck
  float offset[2] = { 0.0f, 0.0f };
  assert_int_equal(dt_dust_find_source(&image, &speck, offset), DT_DUST_OK);
  const int dx = (int)lrintf(offset[0]), dy = (int)lrintf(offset[1]);
  for(int y = 0; y < speck.box_height; y++)
    for(int x = 0; x < speck.box_width; x++)
      if(speck.mask[y * speck.box_width + x])
      {
        assert_false(_in_speck(&speck, speck.box_x + x + dx, speck.box_y + y + dy));
        assert_true(hypotf(speck.box_x + x + dx - light[0], speck.box_y + y + dy - light[1])
                    > SPECK_RADIUS + RIM);
      }

  float *points = NULL;
  int count = 0;
  assert_int_equal(dt_dust_outline(&speck, 4.0f, &points, &count), DT_DUST_OK);
  assert_true(_covered(&speck, points, count) >= 0.85f);

  dt_free_align(points);
  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A scratch that turns lighter than the film along it, a third as strong, as the one of that scan
 * does, is followed along its faint part too, and not to the dark speck beside that part, which stands
 * out of the grain a hundred times more. */
static void _a_scratch_is_followed_where_it_turns_faint(void **state __attribute__((unused)))
{
  const float dark[2] = { _scratch_x(60.0f) + 9.0f, 60.0f };
  float *img = _image(FALSE, dark, 1, FALSE);
  _scratch_turning(img, 100.0f);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  float stroke[4];
  const dt_dust_zone_t zone = _stroke(25, 175, 20, &mask, stroke);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  assert_int_equal(speck.polarity, 1);
  int faint = 0, dense = 0;
  for(int y = 35; y < 96; y++) faint += _in_speck(&speck, (int)lrintf(_scratch_x(y)), y);
  for(int y = 105; y < 166; y++) dense += _in_speck(&speck, (int)lrintf(_scratch_x(y)), y);
  assert_true(faint >= 55);
  assert_true(dense >= 55);
  assert_false(_in_speck(&speck, (int)lrintf(dark[0]), (int)lrintf(dark[1])));
  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A scratch that bends, on an arc of 160 px: 19 px off the straight line between its ends. */
#define ARC_X -60.0f
#define ARC_Y 100.0f
#define ARC_RADIUS 160.0f

static float _arc_x(const float y)
{
  return ARC_X + sqrtf(ARC_RADIUS * ARC_RADIUS - (y - ARC_Y) * (y - ARC_Y));
}

/* A bending scratch, painted along with a stroke that bends as it does, is followed along the
 * stroke, all of it: off the straight line it was found along by more than a pixel per step. The
 * stroke crosses a speck as well, which stands out of a box far more than the scratch, and not as far
 * along the stroke. */
static void _a_bending_scratch_is_followed_along_the_stroke(void **state __attribute__((unused)))
{
  const float dark[2] = { _arc_x(60.0f) - 12.0f, 60.0f };
  float *img = _image(FALSE, dark, 1, FALSE);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const float d = hypotf(x - ARC_X, y - ARC_Y) - ARC_RADIUS;
      const float transmission
          = powf(10.0f, -SCRATCH_DENSITY * expf(-d * d / (2.0f * SCRATCH_SIGMA * SCRATCH_SIGMA)));
      for(int c = 0; c < 4; c++) img[4 * ((size_t)y * W + x) + c] *= transmission;
    }
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  float stroke[2 * 16];
  for(int k = 0; k < 16; k++)
  {
    stroke[2 * k + 1] = 25.0f + 10.0f * k;
    stroke[2 * k] = _arc_x(stroke[2 * k + 1]);
  }
  uint8_t *mask = NULL;
  const dt_dust_zone_t zone = _stroke_through(stroke, 16, 20, &mask);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_OK);
  int on = 0;
  for(int y = 40; y < 161; y++) on += _in_speck(&speck, (int)lrintf(_arc_x(y)), y);
  assert_true(on >= 109);
  assert_false(_in_speck(&speck, (int)lrintf(dark[0]), (int)lrintf(dark[1])));
  dt_dust_speck_cleanup(&speck);
  dt_free_align(mask);
  dt_free_align(img);
}

/* A stroke over grain alone finds nothing: the segments searched along it bring no line out. */
static void _a_stroke_over_grain_finds_nothing(void **state __attribute__((unused)))
{
  float *img = _image(FALSE, NULL, 0, FALSE);
  const dt_dust_image_t image = { .rgba = img, .width = W, .height = H, .scale = 1.0f };
  uint8_t *mask = NULL;
  float stroke[4];
  const dt_dust_zone_t zone = _stroke(25, 175, 20, &mask, stroke);
  dt_dust_speck_t speck;
  assert_int_equal(dt_dust_find_in_zone(&image, &zone, &speck), DT_DUST_NOT_FOUND);
  dt_free_align(mask);
  dt_free_align(img);
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_a_zone_finds_the_whole_speck, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_light_speck_is_found, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_zone_of_grain_finds_nothing, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_wide_faint_speck_is_found, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_the_outline_encloses_the_speck, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_smoother_outline_takes_fewer_nodes_within_its_tolerance, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_stroke_finds_a_faint_scratch, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_scratch_is_followed_where_it_turns_faint, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_bending_scratch_is_followed_along_the_stroke, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_a_stroke_over_grain_finds_nothing, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_the_source_matches_the_texture, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_the_source_keeps_clear_of_a_light_speck, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
