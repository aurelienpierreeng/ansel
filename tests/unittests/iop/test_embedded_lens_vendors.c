/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * cmocka unit tests for the embedded lens coefficient initializer
 * (iop/embedded_lens/embedded_lens.cc::dt_embedded_lens_init_coeffs).
 *
 * The function maps each vendor's correction data (Sony, Fuji, DNG, Olympus)
 * stored in dt_image_t.exif_correction_data into a vendor-agnostic
 * LENS_MAXKNOTS knot table consumed by the lens iop module's per-pixel
 * solver. The tests cover the four vendor branches, the no-op fall-through
 * (CORRECTION_TYPE_NONE, malformed data) and the fine-tune scaling path.
 *
 * Verification is range-based: for the valid-data cases we assert that the
 * return value, the autoscale factor and each output knot table entry are in
 * the expected neighbourhood. Exact snapshot values will be added in a later
 * iteration once the 8 imaging-science-breaking defects catalogued in
 * docs/2026-07-28-embedded-lens-correction-imaging-audit.md are fixed.
 */
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>
#include <glib.h>

#include "../util/assert.h"
#include "../util/tracing.h"

typedef struct dt_embedded_lens_finetune_t dt_embedded_lens_finetune_t;

#include "iop/embedded_lens/embedded_lens.h"

#define E 1e-6f
#define IMAGE_W 6000
#define IMAGE_H 4000
/* Fuji pads knots_vig past the last knot by +1 per step, so with knots[8]=0.9
 * the upper entries reach 1.9..6.9. Every other vendor keeps knots and
 * cor_rgb inside the unit-of-magnification neighbourhood (~1.0..1.05) and
 * vig below ~1.4. KNOT_HI=10.0 is loose enough to cover the Fuji padding
 * while still catching the kind of overflow that would corrupt the lookup
 * table (NaN/Inf/large magnitude). */
#define KNOT_HI 10.0f

/* Assert each entry of knots[0..n-1] is finite, non-negative and below the
 * loose upper bound KNOT_HI. Used to validate the autoscale output of the
 * valid-data cases. */
static void assert_knots_sane(const float *knots, int n)
{
  for(int i = 0; i < n; i++)
  {
    assert_true(!isnan(knots[i]));
    assert_true(!isinf(knots[i]));
    assert_true(knots[i] >= 0.0f);
    assert_true(knots[i] < KNOT_HI);
  }
}

static void assert_knot_tables_sane(const dt_embedded_lens_knots_t *knots)
{
  assert_knots_sane(knots->knots_dist, LENS_MAXKNOTS);
  assert_knots_sane(knots->knots_vig, LENS_MAXKNOTS);
  assert_knots_sane(knots->cor_rgb[0], LENS_MAXKNOTS);
  assert_knots_sane(knots->cor_rgb[1], LENS_MAXKNOTS);
  assert_knots_sane(knots->cor_rgb[2], LENS_MAXKNOTS);
  assert_knots_sane(knots->vig, LENS_MAXKNOTS);
}

static void record_vendor_trace(GChecksum *checksum,
                                const char *label,
                                int nc,
                                float scale,
                                const dt_embedded_lens_knots_t *knots)
{
  const float *tables[] = {
    knots->knots_dist, knots->knots_vig, knots->cor_rgb[0], knots->cor_rgb[1], knots->cor_rgb[2], knots->vig
  };
  char value[G_ASCII_DTOSTR_BUF_SIZE];

  g_checksum_update(checksum, (const guchar *)label, strlen(label));
  g_checksum_update(checksum, (const guchar *)"\n", 1);
  g_snprintf(value, sizeof(value), "%d", nc);
  g_checksum_update(checksum, (const guchar *)value, strlen(value));
  g_checksum_update(checksum, (const guchar *)"\n", 1);
  g_ascii_formatd(value, sizeof(value), "%.9g", scale);
  g_checksum_update(checksum, (const guchar *)value, strlen(value));
  g_checksum_update(checksum, (const guchar *)"\n", 1);

  for(int table = 0; table < 6; table++)
    for(int knot = 0; knot < LENS_MAXKNOTS; knot++)
    {
      g_ascii_formatd(value, sizeof(value), "%.9g", tables[table][knot]);
      g_checksum_update(checksum, (const guchar *)value, strlen(value));
      g_checksum_update(checksum, (const guchar *)"\n", 1);
    }
}

static int initialize_vendor_trace(void **state)
{
  *state = g_checksum_new(G_CHECKSUM_SHA256);
  return *state ? 0 : -1;
}

static int release_vendor_trace(void **state)
{
  g_checksum_free(*state);
  return 0;
}

static void initialize_sony_metadata(dt_image_t *img)
{
  img->exif_correction_data.sony.nc = 16;
  for(int i = 0; i < 16; i++)
  {
    img->exif_correction_data.sony.distortion[i] = (short)(i + 1);
    img->exif_correction_data.sony.ca_r[i] = (short)(i + 1);
    img->exif_correction_data.sony.ca_b[i] = (short)(i + 1);
    img->exif_correction_data.sony.vignetting[i] = (short)(i + 1);
  }
}

static void test_sony_valid(void **state)
{
  GChecksum *trace = *state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_SONY;
  initialize_sony_metadata(&img);

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = 0.0f;

  TR_STEP("Sony full data: nc=16, autoscale > 1, every knot sane");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 16);
  assert_true(scale > 1.0f);
  assert_knot_tables_sane(&knots);
  record_vendor_trace(trace, "Sony full data", nc, scale, &knots);
}

static void test_sony_empty(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_SONY;
  img.exif_correction_data.sony.nc = 0;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = -1.0f;

  TR_STEP("Sony empty data (nc=0): must short-circuit to nc=0, scale=1.0");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 0);
  assert_float_equal(scale, 1.0f, E);
}

static void test_fuji_valid(void **state)
{
  GChecksum *trace = *state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_FUJI;
  img.exif_correction_data.fuji.nc = 9;
  img.exif_correction_data.fuji.cropf = 1.0f;
  for(int i = 0; i < 9; i++)
  {
    img.exif_correction_data.fuji.knots[i] = 0.1f * (float)(i + 1);
    img.exif_correction_data.fuji.distortion[i] = 1.0f;
    img.exif_correction_data.fuji.ca_r[i] = 0.001f;
    img.exif_correction_data.fuji.ca_b[i] = 0.001f;
    img.exif_correction_data.fuji.vignetting[i] = 95.0f;
  }

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = 0.0f;

  TR_STEP("Fuji full data: nc=16, autoscale > 0, every knot sane (Fuji knots_vig pads to ~6.9)");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, LENS_MAXKNOTS);
  assert_true(scale > 0.0f);
  assert_knot_tables_sane(&knots);
  record_vendor_trace(trace, "Fuji full data", nc, scale, &knots);
}

static void test_fuji_empty(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_FUJI;
  img.exif_correction_data.fuji.nc = 0;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = -1.0f;

  TR_STEP("Fuji empty data (nc=0): must short-circuit to nc=0, scale=1.0");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 0);
  assert_float_equal(scale, 1.0f, E);
}

static void test_dng_valid(void **state)
{
  GChecksum *trace = *state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_DNG;
  img.exif_correction_data.dng.has_warp = TRUE;
  img.exif_correction_data.dng.warp_planes = 1;
  img.exif_correction_data.dng.warp_coeffs[0][0] = 1.002;
  img.exif_correction_data.dng.warp_coeffs[0][1] = -0.01;
  img.exif_correction_data.dng.warp_coeffs[0][2] = 0.02;
  img.exif_correction_data.dng.warp_coeffs[0][3] = -0.003;
  img.exif_correction_data.dng.warp_coeffs[0][4] = 0.0;
  img.exif_correction_data.dng.warp_coeffs[0][5] = 0.0;
  img.exif_correction_data.dng.has_vignette = TRUE;
  img.exif_correction_data.dng.vig_coeffs[0] = -0.3;
  img.exif_correction_data.dng.vig_coeffs[1] = 0.1;
  img.exif_correction_data.dng.vig_coeffs[2] = -0.05;
  img.exif_correction_data.dng.vig_coeffs[3] = 0.01;
  img.exif_correction_data.dng.vig_coeffs[4] = -0.001;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = 0.0f;

  TR_STEP("DNG warp+vignette: nc=16, autoscale > 0, every knot sane");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, LENS_MAXKNOTS);
  assert_true(scale > 0.0f);
  assert_knot_tables_sane(&knots);
  record_vendor_trace(trace, "DNG warp+vignette", nc, scale, &knots);
}

static void test_dng_empty(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_DNG;
  img.exif_correction_data.dng.has_warp = FALSE;
  img.exif_correction_data.dng.has_vignette = FALSE;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = -1.0f;

  TR_STEP("DNG with no warp and no vignette: must short-circuit to nc=0, scale=1.0");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 0);
  assert_float_equal(scale, 1.0f, E);
}

static void test_olympus_valid(void **state)
{
  GChecksum *trace = *state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_OLYMPUS;
  img.exif_correction_data.olympus.has_dist = TRUE;
  img.exif_correction_data.olympus.dist[0] = 0.01f;
  img.exif_correction_data.olympus.dist[1] = -0.02f;
  img.exif_correction_data.olympus.dist[2] = 0.005f;
  img.exif_correction_data.olympus.dist[3] = 1.0f;
  img.exif_correction_data.olympus.has_ca = TRUE;
  img.exif_correction_data.olympus.ca[0] = 0.002f;
  img.exif_correction_data.olympus.ca[1] = -0.001f;
  img.exif_correction_data.olympus.ca[2] = 0.0005f;
  img.exif_correction_data.olympus.ca[3] = -0.001f;
  img.exif_correction_data.olympus.ca[4] = 0.0005f;
  img.exif_correction_data.olympus.ca[5] = -0.0001f;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = 0.0f;

  TR_STEP("Olympus dist+ca: nc=16, autoscale > 0, every knot sane");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, LENS_MAXKNOTS);
  assert_true(scale > 0.0f);
  assert_knot_tables_sane(&knots);
  record_vendor_trace(trace, "Olympus dist+ca", nc, scale, &knots);
}

static void test_vendor_trace_matches_golden(void **state)
{
  GChecksum *trace = *state;
  char *contents = NULL;
  char *expected;
  char *source_dir = g_path_get_dirname(__FILE__);
  char *golden_path = g_build_filename(source_dir, "golden", "embedded_lens_vendors.sha256", NULL);

  assert_true(g_file_get_contents(golden_path, &contents, NULL, NULL));
  expected = g_strndup(contents, 64);
  assert_string_equal(g_checksum_get_string(trace), expected);

  g_free(expected);
  g_free(contents);
  g_free(golden_path);
  g_free(source_dir);
}

static void test_olympus_empty(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_OLYMPUS;
  img.exif_correction_data.olympus.has_dist = FALSE;
  img.exif_correction_data.olympus.has_ca = FALSE;

  dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = -1.0f;

  TR_STEP("Olympus with no dist and no ca: must short-circuit to nc=0, scale=1.0");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 0);
  assert_float_equal(scale, 1.0f, E);
}

static void test_none(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_NONE;

  struct dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_embedded_lens_knots_t knots = {};
  float scale = -1.0f;

  TR_STEP("CORRECTION_TYPE_NONE: default branch, must return nc=0, scale=1.0");
  int nc = dt_embedded_lens_init_coeffs(&img, &ft, &knots, &scale);
  assert_int_equal(nc, 0);
  assert_float_equal(scale, 1.0f, E);
}

static void test_sony_half_fine_tune(void **state)
{
  (void)state;
  dt_image_t img;
  memset(&img, 0, sizeof(img));
  img.p_width = IMAGE_W;
  img.p_height = IMAGE_H;
  img.exif_correction_type = CORRECTION_TYPE_SONY;
  initialize_sony_metadata(&img);

  dt_embedded_lens_knots_t knots_full;
  float scale_full = 0.0f;

  dt_embedded_lens_finetune_t ft_full = { 1.0f, 1.0f, 1.0f, 1.0f };
  TR_STEP("Sony full fine-tune (1.0): capture reference scale and knots");
  int nc_full = dt_embedded_lens_init_coeffs(&img, &ft_full, &knots_full, &scale_full);
  assert_int_equal(nc_full, 16);

  dt_embedded_lens_knots_t knots_half;
  float scale_half = 0.0f;

  dt_embedded_lens_finetune_t ft_half = { 0.5f, 0.5f, 0.5f, 0.5f };
  TR_STEP("Sony half fine-tune (0.5): nc stays 16, scale < full, knots differ");
  int nc_half = dt_embedded_lens_init_coeffs(&img, &ft_half, &knots_half, &scale_half);
  assert_int_equal(nc_half, 16);
  assert_true(scale_half > 0.0f);
  assert_true(scale_half < scale_full);

  int found_difference = 0;
  for(int i = 0; i < LENS_MAXKNOTS; i++)
  {
    if(fabsf(knots_full.knots_dist[i] - knots_half.knots_dist[i]) > E)
    {
      found_difference = 1;
      break;
    }
  }
  assert_true(found_difference);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sony_valid),
    cmocka_unit_test(test_sony_empty),
    cmocka_unit_test(test_fuji_valid),
    cmocka_unit_test(test_fuji_empty),
    cmocka_unit_test(test_dng_valid),
    cmocka_unit_test(test_dng_empty),
    cmocka_unit_test(test_olympus_valid),
    cmocka_unit_test(test_olympus_empty),
    cmocka_unit_test(test_none),
    cmocka_unit_test(test_sony_half_fine_tune),
    cmocka_unit_test(test_vendor_trace_matches_golden),
  };

  return cmocka_run_group_tests(tests, initialize_vendor_trace, release_vendor_trace);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
