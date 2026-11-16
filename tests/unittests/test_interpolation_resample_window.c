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

/** dt_interpolation_resample() must stay inside its input, at every scale.
 *
 * It has two implementations and they used to disagree about one case. At a resampling ratio
 * of 1 -- which is every export -- it takes a memcpy path that walked roi_out->height rows at
 * the INPUT's stride with no bounds check, so an output window larger than the source, or at a
 * negative offset, read whatever followed the input buffer. The generic path cannot do that:
 * _prepare_resampling_plan() clips every tap into the source, RESAMPLING_BORDER_MODE being
 * BORDER_REPLICATE. Which path runs is decided by roi_out->scale alone, so the same request
 * was safe at 0.5 and undefined at 1.0.
 *
 * That is not hypothetical: drawlayer resolved a transposed canvas (7380x4928 for an image
 * that was 4928x7380) and the export read unallocated heap from row 4928 down. doc/drawlayer.md
 * section 8b has the measurement.
 *
 * These tests pin the contract rather than either implementation: whatever the window, the
 * resampler reads only its input and clamps to the edge. The source buffers are allocated with
 * exactly their stated size so that a read past the end is a real overflow -- run under ASAN
 * (build-asan) and an escape is a hard failure, not a silent one.
 */

#include "pixel/interpolation.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/** @brief A pixel whose channels encode its coordinates, so a misplaced copy is identifiable. */
static void _fill_identifiable(float *const pixels, const int width, const int height)
{
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      float *const px = pixels + ((size_t)y * width + x) * 4;
      px[0] = (float)x;
      px[1] = (float)y;
      px[2] = 1.0f;
      px[3] = 1.0f;
    }
}

/** @brief What BORDER_REPLICATE says the output pixel at (x, y) must be. */
static void _expected_pixel(const int x, const int y, const int dx, const int dy,
                            const int in_width, const int in_height, float *const out)
{
  int sx = x + dx;
  int sy = y + dy;
  if(sx < 0) sx = 0;
  if(sy < 0) sy = 0;
  if(sx > in_width - 1) sx = in_width - 1;
  if(sy > in_height - 1) sy = in_height - 1;
  out[0] = (float)sx;
  out[1] = (float)sy;
}

/** @brief Resample one window and check every output pixel against the replicate rule. */
static void _check_window(const int in_width, const int in_height,
                          const int out_x, const int out_y,
                          const int out_width, const int out_height)
{
  /* Sized to the pixel, not rounded up: an over-read has to land outside the allocation. */
  float *const in = malloc((size_t)in_width * in_height * 4 * sizeof(float));
  float *const out = malloc((size_t)out_width * out_height * 4 * sizeof(float));
  assert_non_null(in);
  assert_non_null(out);
  _fill_identifiable(in, in_width, in_height);

  const dt_iop_roi_t roi_in = { .x = 0, .y = 0, .width = in_width, .height = in_height, .scale = 1.0f };
  const dt_iop_roi_t roi_out = { .x = out_x, .y = out_y, .width = out_width, .height = out_height,
                                 .scale = 1.0f };

  const struct dt_interpolation *const itor = dt_interpolation_new(DT_INTERPOLATION_BILINEAR);
  dt_interpolation_resample(itor, out, &roi_out, in, &roi_in);

  for(int y = 0; y < out_height; y++)
    for(int x = 0; x < out_width; x++)
    {
      float expected[2] = { 0.0f, 0.0f };
      _expected_pixel(x, y, out_x, out_y, in_width, in_height, expected);
      const float *const px = out + ((size_t)y * out_width + x) * 4;
      assert_float_equal(px[0], expected[0], 0.0f);
      assert_float_equal(px[1], expected[1], 0.0f);
    }

  free(in);
  free(out);
}

/** The case that always worked: the window sits inside the source. */
static void test_window_contained(void **state)
{
  (void)state;
  _check_window(64, 48, 8, 6, 32, 24);
  _check_window(64, 48, 0, 0, 64, 48);
}

/** Taller than the source. This is the drawlayer shape: rows past the end used to be heap. */
static void test_window_taller_than_source(void **state)
{
  (void)state;
  _check_window(64, 48, 0, 0, 64, 96);
}

/** Wider than the source, which runs off the right of every row rather than off the end. */
static void test_window_wider_than_source(void **state)
{
  (void)state;
  _check_window(64, 48, 0, 0, 128, 48);
}

/** The transpose itself: a 64x48 source read as though it were 48x64. */
static void test_window_transposed_source(void **state)
{
  (void)state;
  _check_window(64, 48, 0, 0, 48, 64);
}

/** Negative offsets, which indexed before the buffer. drawlayer's own KNOWN LIMIT. */
static void test_window_negative_offset(void **state)
{
  (void)state;
  _check_window(64, 48, -16, -12, 32, 24);
  _check_window(64, 48, -16, -12, 128, 96);
}

/** Entirely outside, on both sides: every pixel is a corner of the source. */
static void test_window_entirely_outside(void **state)
{
  (void)state;
  _check_window(64, 48, 200, 200, 16, 16);
  _check_window(64, 48, -200, -200, 16, 16);
}

/** A one-pixel source has an edge to replicate and nothing else. */
static void test_window_single_pixel_source(void **state)
{
  (void)state;
  _check_window(1, 1, -2, -2, 8, 8);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_window_contained),
    cmocka_unit_test(test_window_taller_than_source),
    cmocka_unit_test(test_window_wider_than_source),
    cmocka_unit_test(test_window_transposed_source),
    cmocka_unit_test(test_window_negative_offset),
    cmocka_unit_test(test_window_entirely_outside),
    cmocka_unit_test(test_window_single_pixel_source),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
