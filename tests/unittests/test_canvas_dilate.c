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

/* The grow a shadow's extent applies before its blur.
 *
 * Two different questions, held apart:
 *
 * - Is the RUNNING MAXIMUM right? Judged against a brute force over the very same segments, on
 *   random planes of awkward sizes, every window near every edge included -- so the block logic,
 *   the bands of lines and the "0 outside" edges are pinned to the value, not to a tolerance.
 * - Is the SHAPE round? Judged against the disc: how far the segments reach in every direction
 *   (the support function, which is where a grown straight edge lands), and where a single lit
 *   pixel spreads to.
 */

#include "canvas/canvas_dilate.h"

#include <glib.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

static const int _steps[8][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { -1, 1 }, { 2, 1 }, { -2, 1 }, { 1, 2 }, { -1, 2 } };

static int _length_of(const dt_canvas_dilate_lengths_t lengths, const int step)
{
  if(step < 2) return lengths.axis;
  if(step < 4) return lengths.diagonal;
  return lengths.knight;
}

/*
 * The reference: the largest value within the segments' reach of every pixel, nothing outside the
 * plane -- each segment's maximum taken sample by sample, on a copy padded with nothing as far as
 * the segments reach. The padding is not a detail: a sum of segments reaches some of its points
 * only through points outside the plane, and running the passes on the plane itself cut those
 * paths and read short along the edges, which the tiles, whose buffers carry that room, did not.
 */
static void _brute_force(float *plane, const int width, const int height, const dt_canvas_dilate_lengths_t lengths)
{
  const int margin = lengths.axis + 2 * lengths.diagonal + 6 * lengths.knight;
  const int padded_width = width + 2 * margin;
  const int padded_height = height + 2 * margin;
  const size_t padded_count = (size_t)padded_width * padded_height;
  float *padded = g_new0(float, padded_count);
  float *copy = g_new(float, padded_count);
  for(int row = 0; row < height; row++)
    memcpy(padded + (size_t)(row + margin) * padded_width + margin, plane + (size_t)row * width,
           sizeof(float) * width);
  for(int step = 0; step < 8; step++)
  {
    const int half = _length_of(lengths, step);
    if(half <= 0) continue;
    memcpy(copy, padded, sizeof(float) * padded_count);
    for(int row = 0; row < padded_height; row++)
      for(int col = 0; col < padded_width; col++)
      {
        float best = 0.0f;
        for(int t = -half; t <= half; t++)
        {
          const int x = col + t * _steps[step][0];
          const int y = row + t * _steps[step][1];
          if(x < 0 || x >= padded_width || y < 0 || y >= padded_height) continue;
          best = fmaxf(best, copy[(size_t)y * padded_width + x]);
        }
        padded[(size_t)row * padded_width + col] = best;
      }
  }
  for(int row = 0; row < height; row++)
    memcpy(plane + (size_t)row * width, padded + (size_t)(row + margin) * padded_width + margin,
           sizeof(float) * width);
  g_free(padded);
  g_free(copy);
}

static float _worst_support_error(const float radius)
{
  const dt_canvas_dilate_lengths_t lengths = dt_canvas_dilate_lengths(radius);
  float worst = 0.0f;
  for(int sample = 0; sample <= 720; sample++)
  {
    const float angle = (float)(2.0 * M_PI) * (float)sample / 720.0f;
    worst = fmaxf(worst, fabsf(dt_canvas_dilate_reach(lengths, angle) - radius));
  }
  return worst;
}

/*
 * Inside the margin, the running maximum IS the segments' maximum, to the bit, over the whole plane
 * with 0 outside it -- judged on planes of awkward sizes, so the blocks, the bands of lines and the
 * restarts at the sides all get exercised. Nearer the edges every pixel lies between its own value
 * and that maximum, never above it.
 */
static void _the_running_maximum_is_the_segments_maximum(void **state)
{
  (void)state;
  const int sizes[][2] = { { 1, 1 }, { 1, 17 }, { 23, 1 }, { 7, 5 }, { 40, 31 }, { 64, 9 }, { 13, 57 } };
  const float radii[] = { 0.6f, 1.0f, 2.0f, 3.4f, 5.0f, 9.0f, 12.5f, 20.0f, 33.0f, 70.0f };
  GRand *random = g_rand_new_with_seed(1234);
  for(size_t s = 0; s < G_N_ELEMENTS(sizes); s++)
    for(size_t r = 0; r < G_N_ELEMENTS(radii); r++)
    {
      const int margin = dt_canvas_dilate_margin(radii[r]);
      const int width = sizes[s][0] + 2 * margin;
      const int height = sizes[s][1] + 2 * margin;
      const size_t count = (size_t)width * height;
      float *plane = g_new(float, count);
      float *source = g_new(float, count);
      float *expected = g_new(float, count);
      float *prefix = g_new(float, count);
      float *suffix = g_new(float, count);
      // Sparse bright dots over a faint field, the padding included: the margin's bargain is about
      // WHERE the answer is exact, not about what the padding holds.
      for(size_t idx = 0; idx < count; idx++)
        plane[idx] = g_rand_int_range(random, 0, 12) == 0 ? (float)g_rand_double(random)
                                                          : 0.05f * (float)g_rand_double(random);
      memcpy(source, plane, sizeof(float) * count);
      memcpy(expected, plane, sizeof(float) * count);
      _brute_force(expected, width, height, dt_canvas_dilate_lengths(radii[r]));
      assert_true(dt_canvas_dilate(plane, prefix, suffix, width, height, radii[r]));
      for(int row = 0; row < height; row++)
        for(int col = 0; col < width; col++)
        {
          const size_t idx = (size_t)row * width + col;
          const gboolean inside = row >= margin && row < height - margin && col >= margin && col < width - margin;
          if(inside && plane[idx] != expected[idx])
            fail_msg("%dx%d radius %.1f: (%d, %d) is %.6f, the segments give %.6f", width, height, radii[r], col, row,
                     plane[idx], expected[idx]);
          if(plane[idx] < source[idx] || plane[idx] > expected[idx])
            fail_msg("%dx%d radius %.1f: (%d, %d) is %.6f, outside [%.6f, %.6f]", width, height, radii[r], col, row,
                     plane[idx], source[idx], expected[idx]);
        }
      g_free(plane);
      g_free(source);
      g_free(expected);
      g_free(prefix);
      g_free(suffix);
    }
  g_rand_free(random);
}

/*
 * A plane large enough to be grown in tiles, laid out the way a shadow's plane is -- a solid frame
 * with a hole and a feathered edge, empty room around it, a faint line, stray dots and something
 * touching the plane's own edge -- so the tiles left alone because their neighbourhood is uniform
 * and the tiles grown in their own buffers are both held to the segments' maximum, to the bit.
 */
static void _a_plane_grown_in_tiles_is_grown_exactly(void **state)
{
  (void)state;
  const int width = 420;
  const int height = 380;
  const size_t count = (size_t)width * height;
  float *plane = g_new0(float, count);
  float *source = g_new(float, count);
  float *expected = g_new(float, count);
  float *prefix = g_new(float, count);
  float *suffix = g_new(float, count);
  const float radii[] = { 6.0f, 20.0f, 40.0f };
  GRand *random = g_rand_new_with_seed(99);
  for(size_t r = 0; r < G_N_ELEMENTS(radii); r++)
  {
    memset(plane, 0, sizeof(float) * count);
    for(int row = 50; row < 250; row++)
      for(int col = 60; col < 330; col++)
        plane[(size_t)row * width + col] = col < 300 ? 1.0f : (float)(330 - col) / 30.0f;
    for(int row = 100; row < 140; row++)
      for(int col = 150; col < 200; col++) plane[(size_t)row * width + col] = 0.0f;
    for(int row = 20; row < 360; row++) plane[(size_t)row * width + 380] = 0.4f;
    for(int row = 0; row < 10; row++)
      for(int col = 0; col < 100; col++) plane[(size_t)row * width + col] = 0.7f;
    for(int dot = 0; dot < 12; dot++)
      plane[(size_t)g_rand_int_range(random, 270, height) * width + g_rand_int_range(random, 0, 360)]
          = (float)g_rand_double_range(random, 0.3, 1.0);
    memcpy(source, plane, sizeof(float) * count);
    memcpy(expected, plane, sizeof(float) * count);
    _brute_force(expected, width, height, dt_canvas_dilate_lengths(radii[r]));
    assert_true(dt_canvas_dilate(plane, prefix, suffix, width, height, radii[r]));
    const int margin = dt_canvas_dilate_margin(radii[r]);
    for(int row = 0; row < height; row++)
      for(int col = 0; col < width; col++)
      {
        const size_t idx = (size_t)row * width + col;
        const gboolean inside = row >= margin && row < height - margin && col >= margin && col < width - margin;
        if(inside && plane[idx] != expected[idx])
          fail_msg("radius %.0f: (%d, %d) is %.6f, the segments give %.6f", radii[r], col, row, plane[idx],
                   expected[idx]);
        if(plane[idx] < source[idx] || plane[idx] > expected[idx])
          fail_msg("radius %.0f: (%d, %d) is %.6f, outside [%.6f, %.6f]", radii[r], col, row, plane[idx], source[idx],
                   expected[idx]);
      }
  }
  g_rand_free(random);
  g_free(plane);
  g_free(source);
  g_free(expected);
  g_free(prefix);
  g_free(suffix);
}

/* How far the sum reaches, in every direction, against the radius asked for. */
static void _the_segments_reach_as_far_as_a_disc_does(void **state)
{
  (void)state;
  float up_to_20 = 0.0f;   // pixels
  float up_to_40 = 0.0f;   // pixels
  float up_to_160 = 0.0f;  // fraction of the radius
  float beyond_160 = 0.0f; // fraction of the radius
  for(int step = 2; step <= 1280; step++)
  {
    const float radius = 0.5f * (float)step;
    const float error = _worst_support_error(radius);
    if(radius <= 20.0f)
      up_to_20 = fmaxf(up_to_20, error);
    else if(radius <= 40.0f)
      up_to_40 = fmaxf(up_to_40, error);
    else if(radius <= 160.0f)
      up_to_160 = fmaxf(up_to_160, error / radius);
    else
      beyond_160 = fmaxf(beyond_160, error / radius);
  }
  printf("worst support error: %.3f px to 20 px, %.3f px to 40, %.2f %% to 160, %.2f %% to 640\n", up_to_20, up_to_40,
         100.0f * up_to_160, 100.0f * beyond_160);
  assert_true(up_to_20 <= 0.76f);
  assert_true(up_to_40 <= 1.0f);
  assert_true(up_to_160 <= 0.024f);
  assert_true(beyond_160 <= 0.016f);
  // The square the separable filter would be, for scale: 41 % short or long on the diagonal.
  const dt_canvas_dilate_lengths_t square = { 10, 0, 0 };
  assert_true(fabsf(dt_canvas_dilate_reach(square, (float)M_PI_4) - 10.0f * (float)M_SQRT2) < 1e-3f);
}

/* A single lit pixel spreads to the shape itself; a ray from it ends near the radius every way. */
static void _a_lit_pixel_spreads_to_a_round_patch(void **state)
{
  (void)state;
  const float radius = 50.0f;
  const int margin = dt_canvas_dilate_margin(radius);
  const int size = 2 * (margin + (int)radius) + 21;
  const int centre = size / 2;
  const size_t count = (size_t)size * size;
  float *plane = g_new0(float, count);
  float *prefix = g_new(float, count);
  float *suffix = g_new(float, count);
  plane[(size_t)centre * size + centre] = 1.0f;
  assert_true(dt_canvas_dilate(plane, prefix, suffix, size, size, radius));
  // Walk out along 64 rays and note where the patch stops.
  float shortest = INFINITY;
  float longest = 0.0f;
  for(int ray = 0; ray < 64; ray++)
  {
    const double angle = 2.0 * M_PI * ray / 64.0;
    double reached = 0.0;
    for(double distance = 0.0; distance < centre - margin; distance += 0.25)
    {
      const int x = centre + (int)lround(distance * cos(angle));
      const int y = centre + (int)lround(distance * sin(angle));
      if(plane[(size_t)y * size + x] < 1.0f) break;
      reached = distance;
    }
    shortest = fminf(shortest, (float)reached);
    longest = fmaxf(longest, (float)reached);
  }
  printf("a lit pixel grown by %.0f px reaches between %.2f and %.2f px\n", radius, shortest, longest);
  // A polygon of sixteen sides, sized to the disc: within a pixel or two of it every way, where a
  // square would reach 70.7 px on its diagonals.
  assert_true(shortest >= radius - 2.5f);
  assert_true(longest <= radius + 2.5f);
  g_free(plane);
  g_free(prefix);
  g_free(suffix);
}

/* A soft edge keeps its softness: every level moves out by the same reach, so a ramp shifts. */
static void _a_feather_moves_out_whole(void **state)
{
  (void)state;
  const float radius = 10.0f;
  const int margin = dt_canvas_dilate_margin(radius);
  const int width = 120 + 2 * margin;
  const int height = 9 + 2 * margin;
  const size_t count = (size_t)width * height;
  float *plane = g_new(float, count);
  float *original = g_new(float, count);
  float *prefix = g_new(float, count);
  float *suffix = g_new(float, count);
  // Solid on the left, a 30-pixel ramp down to nothing, then nothing.
  const int ramp = margin + 30;
  for(int row = 0; row < height; row++)
    for(int col = 0; col < width; col++)
      plane[(size_t)row * width + col] = col < ramp ? 1.0f : col < ramp + 30 ? (float)(ramp + 30 - col) / 30.0f : 0.0f;
  memcpy(original, plane, sizeof(float) * count);
  const int shift = (int)lroundf(dt_canvas_dilate_reach(dt_canvas_dilate_lengths(radius), 0.0f));
  assert_int_equal(shift, margin);
  assert_true(dt_canvas_dilate(plane, prefix, suffix, width, height, radius));
  // Along the middle row the ramp is the same ramp, further out.
  const int row = height / 2;
  for(int col = margin + shift; col < width - margin; col++)
    assert_float_equal(plane[(size_t)row * width + col], original[(size_t)row * width + col - shift], 1e-6f);
  // And the ramp's first step outward is exactly one step of it, not the solid core.
  assert_float_equal(plane[(size_t)row * width + ramp + 29 + shift], 1.0f / 30.0f, 1e-6f);
  g_free(plane);
  g_free(original);
  g_free(prefix);
  g_free(suffix);
}

/* A partial stroke spreads at its own density: the grow never invents coverage the plane lacks. */
static void _a_faint_stroke_spreads_at_its_own_density(void **state)
{
  (void)state;
  const int size = 41;
  const size_t count = (size_t)size * size;
  float *plane = g_new0(float, count);
  float *prefix = g_new(float, count);
  float *suffix = g_new(float, count);
  for(int row = 0; row < size; row++) plane[(size_t)row * size + size / 2] = 0.4f;
  assert_true(dt_canvas_dilate(plane, prefix, suffix, size, size, 6.0f));
  float highest = 0.0f;
  for(size_t idx = 0; idx < count; idx++) highest = fmaxf(highest, plane[idx]);
  assert_float_equal(highest, 0.4f, 1e-6f);
  // Six pixels either side of the stroke are lit at that density, and the corners are not.
  assert_float_equal(plane[(size_t)(size / 2) * size + size / 2 + 6], 0.4f, 1e-6f);
  assert_float_equal(plane[(size_t)(size / 2) * size + size / 2 - 6], 0.4f, 1e-6f);
  assert_float_equal(plane[0], 0.0f, 1e-6f);
  g_free(plane);
  g_free(prefix);
  g_free(suffix);
}

/* Under half a pixel is no grow at all, and a plane that is not there is refused. */
static void _no_reach_changes_nothing(void **state)
{
  (void)state;
  const dt_canvas_dilate_lengths_t none = dt_canvas_dilate_lengths(0.4f);
  assert_int_equal(none.axis + none.diagonal + none.knight, 0);
  const dt_canvas_dilate_lengths_t nan = dt_canvas_dilate_lengths(NAN);
  assert_int_equal(nan.axis + nan.diagonal + nan.knight, 0);
  float plane[6] = { 0.1f, 0.9f, 0.2f, 0.0f, 0.3f, 0.0f };
  const float before[6] = { 0.1f, 0.9f, 0.2f, 0.0f, 0.3f, 0.0f };
  float prefix[6];
  float suffix[6];
  assert_true(dt_canvas_dilate(plane, prefix, suffix, 3, 2, 0.3f));
  assert_memory_equal(plane, before, sizeof(plane));
  assert_false(dt_canvas_dilate(NULL, prefix, suffix, 3, 2, 4.0f));
  assert_false(dt_canvas_dilate(plane, prefix, suffix, 0, 2, 4.0f));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_the_running_maximum_is_the_segments_maximum),
    cmocka_unit_test(_a_plane_grown_in_tiles_is_grown_exactly),
    cmocka_unit_test(_the_segments_reach_as_far_as_a_disc_does),
    cmocka_unit_test(_a_lit_pixel_spreads_to_a_round_patch),
    cmocka_unit_test(_a_feather_moves_out_whole),
    cmocka_unit_test(_a_faint_stroke_spreads_at_its_own_density),
    cmocka_unit_test(_no_reach_changes_nothing),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
