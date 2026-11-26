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

/** The repair of a speck of dust or a scratch (dt_heal_dust()).
 *
 * Its result is the harmonic interpolation of the destination's border plus the source's detail
 * finer than the box. On a ramp, which is harmonic, the interpolation is the ramp itself; a
 * checkerboard is all detail. So: on a flat source the ramp comes back, on a checkerboard the
 * checkerboard comes back on the ramp, and outside the mask nothing moves.
 */

#include "caches/pixelpipe_cache.h"
#include "darktable.h"
#include "pixel/heal_dust.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"

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

#define W 64
#define H 64
#define HOLE 8.0f
#define CHECKER 0.01f

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

static float _ramp(const int x, const int y)
{
  return 0.2f + 0.002f * x + 0.001f * y;
}

static gboolean _in_hole(const int x, const int y)
{
  return hypotf(x - W / 2.0f, y - H / 2.0f) < HOLE;
}

/* Heal a ramp from a source that is flat, or flat with a checkerboard; return the destination. */
static float *_heal_ramp(const gboolean checkered, const dt_heal_domain_t domain)
{
  float *src = dt_alloc_align_float((size_t)4 * W * H);
  float *dest = dt_alloc_align_float((size_t)4 * W * H);
  float *mask = dt_alloc_align_float((size_t)W * H);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const size_t i = (size_t)y * W + x;
      const float grain = checkered ? (((x + y) & 1) ? CHECKER : -CHECKER) : 0.0f;
      for(int c = 0; c < 4; c++)
      {
        src[4 * i + c] = 0.5f + grain;
        // the hole holds something the heal must not keep
        dest[4 * i + c] = _in_hole(x, y) ? 0.9f : _ramp(x, y);
      }
      mask[i] = _in_hole(x, y) ? 1.0f : 0.0f;
    }
  const dt_heal_dust_t repair = { .solver = { .max_iter = 2000, .domain = domain }, .texture_radius = 2 };
  dt_heal_dust(src, dest, mask, W, H, &repair);
  dt_free_align(src);
  dt_free_align(mask);
  return dest;
}

static void _a_flat_source_gives_the_border_interpolation(void **state __attribute__((unused)))
{
  float *dest = _heal_ramp(FALSE, DT_HEAL_DOMAIN_LINEAR);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const float v = dest[4 * ((size_t)y * W + x)];
      if(_in_hole(x, y)) assert_true(fabsf(v - _ramp(x, y)) < 2e-3f);
      else assert_true(v == _ramp(x, y)); // outside the mask, untouched to the bit
    }
  dt_free_align(dest);
}

static void _the_source_detail_comes_through(void **state __attribute__((unused)))
{
  // in the square-root domain too: there the checkerboard's amplitude follows the level
  for(int domain = DT_HEAL_DOMAIN_LINEAR; domain <= DT_HEAL_DOMAIN_SQRT; domain++)
  {
    float *dest = _heal_ramp(TRUE, (dt_heal_domain_t)domain);
    double sum2 = 0.0;
    int count = 0;
    for(int y = 0; y < H; y++)
      for(int x = 0; x < W; x++)
      {
        if(!_in_hole(x, y)) continue;
        const double d = dest[4 * ((size_t)y * W + x)] - _ramp(x, y);
        sum2 += d * d;
        count++;
      }
    const double rms = sqrt(sum2 / count);
    // linear: the checkerboard as it is; square root: scaled by sqrt(level / source level), the
    // ramp being 0.296 at the centre of the hole
    const double expected = domain == DT_HEAL_DOMAIN_LINEAR ? CHECKER : CHECKER * sqrt(0.296 / 0.5);
    assert_true(fabs(rms - expected) < 0.25 * expected);
    dt_free_align(dest);
  }
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_a_flat_source_gives_the_border_interpolation, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_the_source_detail_comes_through, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
