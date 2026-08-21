/*
    This file is part of Ansel.
    Copyright (C) 2026 Ansel developers.

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

/* Three-way parity check for the .anselnn executor: torch (the golden fixture) against the CPU
 * path and against the OpenCL path, on the same input.
 *
 * src/tests/nn_model_test.c already does torch-vs-CPU and builds standalone, without Ansel.
 * The GPU side cannot: dt_nn_cl_create() wants a compiled program number, which only exists
 * once dt_opencl_init() has run, so this half has to live in an application that links
 * lib_ansel. It follows ansel-cltest's pattern of appending its own arguments to force the
 * subsystem up.
 *
 * Usage:
 *   ansel-nn-parity <model.anselnn> <fixture-dir> [N] [--core <ansel options>]
 *
 * Fixtures come from scripts/make_fixture.py in the ansel-denoise training repo, and must be
 * regenerated whenever the model files change -- they pin a model_sha256, and a stale fixture
 * reports a large error for reasons that have nothing to do with the code under test.
 */

#include "darktable.h"
#include "common/nn_model.h"
#include "common/opencl.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "develop/pixelpipe.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NN_PROGRAM 39 // rawdenoiseai.cl, from data/kernels/programs.conf

static float *read_f32(const char *dir, const char *name, const size_t count)
{
  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = g_fopen(path, "rb");
  if(!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
  float *buf = (float *)malloc(sizeof(float) * count);
  if(!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, sizeof(float), count, f);
  fclose(f);
  if(got != count)
  {
    fprintf(stderr, "%s: expected %zu floats, got %zu -- stale or mismatched fixture\n", path, count, got);
    dt_free(buf);
    return NULL;
  }
  return buf;
}

/* Refuse to run a fixture against a model it was not generated from. Without this the
 * comparison still runs and reports a large error that reads like a broken executor -- the
 * failure mode that once made six good models look like six regressions. */
static int check_model_hash(const char *dir, const char *model_path)
{
  char path[4096];
  snprintf(path, sizeof(path), "%s/fixture-meta.json", dir);
  JsonParser *parser = json_parser_new();
  int rc = 0;
  if(json_parser_load_from_file(parser, path, NULL))
  {
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if(json_object_has_member(root, "model_sha256"))
    {
      const char *want = json_object_get_string_member(root, "model_sha256");
      gchar *blob = NULL;
      gsize len = 0;
      if(g_file_get_contents(model_path, &blob, &len, NULL))
      {
        gchar *got = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)blob, len);
        if(g_strcmp0(got, want))
        {
          fprintf(stderr, "FAIL: fixture was generated from a different model\n  pins %s\n  got  %s\n",
                  want, got);
          rc = 1;
        }
        g_free(got);
        g_free(blob);
      }
    }
  }
  g_object_unref(parser);
  return rc;
}

static double max_abs_diff(const float *a, const float *b, const size_t n, size_t *where)
{
  double worst = 0.0;
  for(size_t i = 0; i < n; i++)
  {
    const double d = fabs((double)a[i] - (double)b[i]);
    if(d > worst) { worst = d; if(where) *where = i; }
  }
  return worst;
}

int main(int argc, char *arg[])
{
  if(argc < 3)
  {
    fprintf(stderr, "usage: %s <model.anselnn> <fixture-dir> [N]\n", arg[0]);
    return 1;
  }
  const char *model_path = arg[1];
  const char *fixture_dir = arg[2];
  const int n = (argc > 3 && arg[3][0] != '-') ? atoi(arg[3]) : 96;

  int result = 1;
  float *in = NULL, *expected = NULL, *cpu = NULL, *gpu = NULL, *head = NULL;
  dt_nn_model_t *model = NULL;
  dt_nn_cl_t *nncl = NULL;
  void *dev_in = NULL, *dev_out = NULL;

  /* dt_init() treats every positional argument as an image to import and prints its usage if it
   * does not recognise one, so it must never see ours. Everything after `--core` is Ansel's,
   * everything before it is this tool's; we hand dt_init argv[0], Ansel's share, and the
   * options that force the subsystem up. */
  int core_at = argc;
  for(int i = 1; i < argc; i++)
    if(!strcmp(arg[i], "--core")) { core_at = i; break; }

  char *m_arg[] = { "--library", ":memory:" };
  const int m_argc = (int)(sizeof(m_arg) / sizeof(m_arg[0]));
  const int passthrough = (core_at < argc) ? (argc - core_at - 1) : 0;
  char **argv = (char **)malloc(sizeof(char *) * (size_t)(1 + passthrough + m_argc));
  if(IS_NULL_PTR(argv)) return 1;
  int dt_argc = 0;
  argv[dt_argc++] = arg[0];
  for(int i = 0; i < passthrough; i++) argv[dt_argc++] = arg[core_at + 1 + i];
  for(int i = 0; i < m_argc; i++) argv[dt_argc++] = m_arg[i];
  if(dt_init(dt_argc, argv, FALSE, FALSE)) { dt_free(argv); return 1; }

  if(check_model_hash(fixture_dir, model_path)) goto done;

  char err[256] = { 0 };
  model = dt_nn_model_load(model_path, err, sizeof(err));
  if(!model) { fprintf(stderr, "cannot load %s: %s\n", model_path, err); goto done; }

  const int in_ch = dt_nn_model_in_channels(model);
  const int out_ch = dt_nn_model_out_channels(model);
  const size_t plane = (size_t)n * n;

  in = read_f32(fixture_dir, "fixture-input.f32", plane * in_ch);
  expected = read_f32(fixture_dir, "fixture-expected.f32", plane * out_ch);
  cpu = (float *)malloc(sizeof(float) * plane * out_ch);
  gpu = (float *)malloc(sizeof(float) * plane * out_ch);
  head = (float *)malloc(sizeof(float) * plane * out_ch);
  if(!in || !expected || !cpu || !gpu || !head) goto done;

  printf("model %s: in=%d out=%d, fixture %dx%d\n", model_path, in_ch, out_ch, n, n);

  // ---- CPU: stage 0 with the residual applied, matching what the module renders ----------
  if(dt_nn_unet_apply_stage(model, 0, in, cpu, n, n, 1))
  {
    fprintf(stderr, "CPU stage failed\n");
    goto done;
  }
  size_t w1 = 0;
  const double cpu_err = max_abs_diff(cpu, expected, plane * out_ch, &w1);
  printf("  torch vs CPU     : max abs err %.3g (at %zu: %.6f vs %.6f)\n",
         cpu_err, w1, cpu[w1], expected[w1]);

  // ---- OpenCL: same stage, then apply the residual host-side ------------------------------
  const int devid = dt_opencl_reserve_device_for_pipe(DT_DEV_PIXELPIPE_EXPORT);
  if(devid < 0)
  {
    printf("  torch vs OpenCL  : SKIPPED (no OpenCL device available)\n");
    result = (cpu_err > 2e-4) ? 1 : 0;
    goto done;
  }
  printf("  using OpenCL device %d\n", devid);

  nncl = dt_nn_cl_create(NN_PROGRAM);
  if(!nncl) { fprintf(stderr, "dt_nn_cl_create failed\n"); dt_opencl_release_device(devid); goto done; }

  dev_in = dt_opencl_alloc_device_buffer(devid, sizeof(float) * plane * in_ch);
  dev_out = dt_opencl_alloc_device_buffer(devid, sizeof(float) * plane * out_ch);
  if(!dev_in || !dev_out) { fprintf(stderr, "device allocation failed\n"); dt_opencl_release_device(devid); goto done; }

  if(dt_opencl_write_buffer_to_device(devid, in, dev_in, 0, sizeof(float) * plane * in_ch, CL_TRUE) != CL_SUCCESS)
  {
    fprintf(stderr, "upload failed\n"); dt_opencl_release_device(devid); goto done;
  }

  const int rc = dt_nn_unet_apply_stage_cl(model, 0, nncl, devid, dev_in, dev_out, n, n);
  if(rc != CL_SUCCESS)
  {
    fprintf(stderr, "dt_nn_unet_apply_stage_cl failed (%d)\n", rc);
    dt_opencl_release_device(devid);
    goto done;
  }

  if(dt_opencl_read_buffer_from_device(devid, head, dev_out, 0, sizeof(float) * plane * out_ch, CL_TRUE) != CL_SUCCESS)
  {
    fprintf(stderr, "readback failed\n"); dt_opencl_release_device(devid); goto done;
  }
  dt_opencl_release_device(devid);

  /* The CL entry point writes the RAW head -- the predicted noise -- by contract, while the CPU
   * twin above was asked to apply the residual. Close the gap here rather than by asking for a
   * different CPU call, so both sides are compared against the same torch output. */
  for(size_t i = 0; i < plane * out_ch; i++) gpu[i] = in[i] - head[i];

  size_t w2 = 0, w3 = 0;
  const double gpu_err = max_abs_diff(gpu, expected, plane * out_ch, &w2);
  const double dev_err = max_abs_diff(gpu, cpu, plane * out_ch, &w3);
  printf("  torch vs OpenCL  : max abs err %.3g (at %zu: %.6f vs %.6f)\n",
         gpu_err, w2, gpu[w2], expected[w2]);
  printf("  CPU   vs OpenCL  : max abs err %.3g\n", dev_err);

  {
    const double tol = 2e-4;
    const int pass = (cpu_err <= tol) && (gpu_err <= tol);
    printf("  %s (tolerance %.0e)\n", pass ? "PASS" : "FAIL", tol);
    result = pass ? 0 : 1;
  }

done:
  if(dev_in) dt_opencl_release_mem_object(dev_in);
  if(dev_out) dt_opencl_release_mem_object(dev_out);
  if(nncl) dt_nn_cl_destroy(nncl);
  if(model) dt_nn_model_free(model);
  dt_free(in); dt_free(expected); dt_free(cpu); dt_free(gpu); dt_free(head);
  dt_cleanup();
  dt_free(argv);
  return result;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
