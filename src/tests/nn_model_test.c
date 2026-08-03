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

/* Golden-fixture parity test for the .anselnn loader + CPU U-Net executor.
 * The fixture is produced by scripts/make_fixture.py in the ansel-denoise
 * training repo from the same model file; the C output must match the torch
 * reference within the stated absolute tolerance.
 *
 * Standalone build (no ansel build system needed):
 *   gcc -O2 -fopenmp -Isrc src/common/nn_model.c src/tests/nn_model_test.c \
 *       $(pkg-config --cflags --libs json-glib-1.0) -lm -o nn_model_test
 * Usage: nn_model_test <model.anselnn> <fixture-dir> [N]   (N defaults to 96)
 */

#include "common/nn_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double max_abs_diff(const float *a, const float *b, size_t count)
{
  double max_abs = 0.0;
  for(size_t i = 0; i < count; i++)
  {
    const double d = fabs((double)a[i] - b[i]);
    if(d > max_abs) max_abs = d;
  }
  return max_abs;
}

static float *read_f32(const char *dir, const char *name, size_t count)
{
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if(!f)
  {
    fprintf(stderr, "cannot open %s\n", path);
    return NULL;
  }
  float *buf = malloc(count * sizeof(float));
  const size_t got = buf ? fread(buf, sizeof(float), count, f) : 0;
  fclose(f);
  if(got != count)
  {
    fprintf(stderr, "%s: expected %zu floats, got %zu\n", path, count, got);
    free(buf);
    return NULL;
  }
  return buf;
}

int main(int argc, char *argv[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s <model.anselnn> <fixture-dir> [N]\n", argv[0]);
    return 2;
  }
  const int n = argc > 3 ? atoi(argv[3]) : 96;

  char err[256] = "";
  dt_nn_model_t *model = dt_nn_model_load(argv[1], err, sizeof(err));
  if(!model)
  {
    fprintf(stderr, "model load failed: %s\n", err);
    return 2;
  }
  printf("model loaded: in=%d out=%d alignment=%d, scratch for %dx%d: %.1f MB\n", dt_nn_model_in_channels(model),
         dt_nn_model_out_channels(model), dt_nn_model_alignment(model), n, n,
         dt_nn_unet_scratch_bytes(model, n, n) / 1048576.0);

  const size_t plane = (size_t)n * n;
  float *in = read_f32(argv[2], "fixture-input.f32", plane * dt_nn_model_in_channels(model));
  float *expected = read_f32(argv[2], "fixture-expected.f32", plane);
  float *out = calloc(plane, sizeof(float));
  if(!in || !expected || !out) return 2;

  /* multi-scale model: gate the binning contract and the coarse stage before
   * the fine parity below (which runs on the fixture's torch-built guide). */
  const int bin = dt_nn_model_bin(model, 0);
  if(bin > 1)
  {
    const int cn = n / bin;
    const size_t cplane = (size_t)cn * cn;
    const int c_in = dt_nn_model_coarse_in_channels(model);
    const int c_out = dt_nn_model_coarse_out_channels(model);
    float *base = read_f32(argv[2], "fixture-base-planes.f32", plane * 5);
    float *c_in_exp = read_f32(argv[2], "fixture-coarse-input.f32", cplane * c_in);
    float *c_out_exp = read_f32(argv[2], "fixture-coarse-expected.f32", cplane * c_out);
    float *rgb = malloc(cplane * 3 * sizeof(float));
    float *cnt = malloc(cplane * 3 * sizeof(float));
    float *c_out_got = malloc(cplane * c_out * sizeof(float));
    if(!base || !c_in_exp || !c_out_exp || !rgb || !cnt || !c_out_got) return 2;

    /* 1. binning contract: our RGB means vs torch's binned planes 0..2 */
    dt_nn_bin_planes(base, n, n, bin, rgb, cnt);
    const double bin_err = max_abs_diff(rgb, c_in_exp, cplane * 3);
    printf("binning contract: max abs err %.3g (tolerance 1e-6)\n", bin_err);
    if(bin_err > 1e-6)
    {
      fprintf(stderr, "FAIL: binning contract\n");
      return 1;
    }

    /* 2. coarse stage parity on torch's own input */
    if(dt_nn_unet_apply_stage(model, 1, c_in_exp, c_out_got, cn, cn, 1))
    {
      fprintf(stderr, "coarse stage apply failed\n");
      return 2;
    }
    const double c_err = max_abs_diff(c_out_got, c_out_exp, cplane * c_out);
    printf("coarse stage parity: max abs err %.3g (tolerance 2e-4)\n", c_err);
    if(c_err > 2e-4)
    {
      fprintf(stderr, "FAIL: coarse stage parity\n");
      return 1;
    }

    /* 3. end-to-end: our binning -> our coarse -> our guide injection -> fine,
     * compared against the torch final output (looser: coarse error propagates) */
    const int fine_in_ch = dt_nn_model_in_channels(model);
    float *fine_in = malloc(plane * fine_in_ch * sizeof(float));
    float *e2e_out = malloc(plane * sizeof(float));
    if(!fine_in || !e2e_out) return 2;
    memcpy(fine_in, base, plane * 5 * sizeof(float));
    /* rebuild the coarse input from our own binning + the fixture's sigma
     * planes (positions 3..5 of the coarse input are the binned sigma, which
     * needs the profile constants — reuse torch's, the contract test above
     * already pinned our RGB planes) */
    dt_nn_unet_apply_stage(model, 1, c_in_exp, c_out_got, cn, cn, 1);
    dt_nn_upsample_nearest(c_out_got, c_out, cn, cn, bin, fine_in + plane * 5);
    if(dt_nn_unet_apply_stage(model, 0, fine_in, e2e_out, n, n, 1))
    {
      fprintf(stderr, "fine stage apply failed\n");
      return 2;
    }
    const double e2e_err = max_abs_diff(e2e_out, expected, plane);
    printf("end-to-end parity: max abs err %.3g (tolerance 5e-4)\n", e2e_err);
    if(e2e_err > 5e-4)
    {
      fprintf(stderr, "FAIL: end-to-end parity\n");
      return 1;
    }
    free(base);
    free(c_in_exp);
    free(c_out_exp);
    free(rgb);
    free(cnt);
    free(c_out_got);
    free(fine_in);
    free(e2e_out);
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const int rc = dt_nn_unet_apply(model, in, out, n, n);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if(rc)
  {
    fprintf(stderr, "dt_nn_unet_apply failed (%d)\n", rc);
    return 2;
  }
  const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

  double max_abs = 0.0, sum_sq = 0.0;
  size_t worst = 0;
  for(size_t i = 0; i < plane; i++)
  {
    const double d = fabs((double)out[i] - expected[i]);
    if(d > max_abs)
    {
      max_abs = d;
      worst = i;
    }
    sum_sq += d * d;
  }
  const double rms = sqrt(sum_sq / plane);
  const double tolerance = 2e-4;
  printf("parity vs torch: max abs err %.3g (at %zu: %.6f vs %.6f), rms %.3g | %.1f ms for %dx%d\n", max_abs,
         worst, out[worst], expected[worst], rms, ms, n, n);

  dt_nn_model_free(model);
  free(in);
  free(expected);
  free(out);
  if(max_abs > tolerance)
  {
    fprintf(stderr, "FAIL: max abs err %.3g > %.3g\n", max_abs, tolerance);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
