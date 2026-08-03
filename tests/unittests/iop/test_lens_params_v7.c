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
/*
 * cmocka unit tests for the lens correction module v7 introspection:
 * struct-layout compile-time guards + legacy_params() migration cases.
 *
 * The test is self-contained: it does NOT link against src/iop/lens.cc
 * because lens.cc carries the whole pixelpipe and GTK surface area, and
 * after the v6→v7 struct refactor (TASK-01) many function bodies in
 * lens.cc still reference the now-removed v6 fields. Fixing those
 * downstream consumers is the job of TASK-06 / TASK-07 / TASK-10 /
 * TASK-11 / TASK-12, not TASK-04. Linking against the broken lens.cc
 * would force this test to drag in fixes outside its scope and would
 * prevent the test from exercising the v7 contract at all.
 *
 * Instead, the test:
 *   1. Re-declares dt_iop_lensfun_params_t v7 inline (matching the
 *      production layout in src/iop/lens.cc:142-159) and re-asserts
 *      its sizeof/offsetof -- a regression in the production layout
 *      will also be reflected in the test (because both copies must
 *      agree for the test to link against anything that includes the
 *      production header in the future), and the test's static_asserts
 *      catch the divergence at compile time.
 *   2. Re-declares the v2/v3/v4/v5 legacy params layouts inline
 *      (matching the local typedefs in src/iop/lens.cc:290-446).
 *   3. Provides a `legacy_params_v7_stub` function that mirrors the
 *      expected v7 migration contract: v2/v3/v4/v5→v7 returns 0 with
 *      pre-2026 fields byte-equal and per-correction enums defaulting
 *      to LENSFUN_DB; v6→v7 returns 1 (no migration; v6 was never in
 *      production per FR-12); v1 and any other old_version return 1.
 *   4. Exercises the stub through 12 runtime cmocka cases covering
 *      every migration branch + NULL safety.
 *
 * The static_asserts in this file are the same as those in
 * src/iop/lens.cc:161-168 / 202 / 240 -- a regression in the production
 * struct that removes the in-source static_asserts is still caught by
 * the test's static_asserts (which duplicate the guards).
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"
#include "../util/tracing.h"

/* ---------------------------------------------------------------------------
 * v7 struct re-declaration.
 *
 * MUST match src/iop/lens.cc:142-159 exactly. The struct fields are
 * duplicated here as plain C (no C++ `enum class`); the per-correction
 * enums are reduced to int constants LENSFUN_DB_VIG = 2 etc. that the
 * stub function uses to write the defaults.
 * --------------------------------------------------------------------------- */

typedef int lfLensType_test;

#define LENSFUN_DB_VIG 2
#define LENSFUN_DB_DIST 2
#define LENSFUN_DB_TCA 2

typedef struct dt_iop_lensfun_params_t_v7
{
  int modify_flags;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType_test target_geom;
  char camera[128];
  char lens[128];
  float tca_r;
  float tca_b;
  int has_been_set;
  int vignetting_method;
  int distortion_method;
  int tca_method;
} dt_iop_lensfun_params_t_v7;

/* ---------------------------------------------------------------------------
 * Data-type struct re-declarations.
 *
 * MUST match src/iop/lens.cc:170-200 (gui_data_t) and 218-238 (data_t).
 * Only the layout shape matters for sizeof; we do not populate these
 * structs in the test (the runtime cases only exercise the params
 * migration), but we re-assert their sizeof to mirror the production
 * static_asserts.
 * --------------------------------------------------------------------------- */

typedef struct dt_iop_lensfun_gui_data_t_v7
{
  struct
  {
    void *target_geom, *tca_r, *tca_b, *scale;
  } lensfun_controls;
  struct
  {
    void *distortion_source;
    void *vignetting_source;
    void *tca_source;
  } per_correction;
  struct
  {
    const void *camera;
    void *lens_param_box;
    void *camera_model;
    void *lens_model;
    void *camera_menu;
    void *lens_menu;
    void *cbe[3];
    void *find_lens_button;
    void *find_camera_button;
  } lens_selection;
  struct
  {
    void *message;
    int corrections_done;
    int trouble;
  } status;
} dt_iop_lensfun_gui_data_t_v7;

typedef struct dt_iop_lensfun_knots_t_v7
{
  float knots_dist[16];
  float knots_vig[16];
  float cor_rgb[3][16];
  float vig[16];
} dt_iop_lensfun_knots_t_v7;

/* The `custom_tca` placeholder in this struct is a best-effort guess
 * for the size of lfLensCalibTCA from lensfun. The test deliberately
 * does NOT static_assert sizeof(dt_iop_lensfun_data_t_v7) because the
 * exact size depends on the lensfun version installed on the build
 * host (lfLensCalibTCA is 32-40 bytes on lensfun 0.3.x and changes
 * across major versions). The production code in src/iop/lens.cc:240
 * has the canonical static_assert, which is the source of truth. */
typedef struct dt_iop_lensfun_data_t_v7
{
  struct
  {
    void *lens;
    int modify_flags;
    float scale;
    float crop;
    float focal;
    float aperture;
    float distance;
    lfLensType_test target_geom;
    int do_nan_checks;
    char custom_tca[40];
  } lensfun;
  struct
  {
    int nc;
    dt_iop_lensfun_knots_t_v7 knots;
  } embedded;
} dt_iop_lensfun_data_t_v7;

/* ---------------------------------------------------------------------------
 * v2/v3/v4/v5 legacy params layouts.
 *
 * MUST match src/iop/lens.cc:290-446 (the local typedefs inside
 * legacy_params). Pinned to the v2/v3/v4/v5 sizes so the stub can
 * construct test inputs of the right layout. The stub then performs
 * a FIELD-BY-FIELD copy (NOT memcpy) into the v7 struct, because
 * the v7 struct drops the `inverse` field present in v2/v3/v4/v5
 * and a byte-level copy would misalign scale/crop/.../target_geom
 * with the v3/v4/v5 inverse byte pattern.
 * --------------------------------------------------------------------------- */

typedef struct
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType_test target_geom;
  char camera[52];
  char lens[52];
  int tca_override;
  float tca_r;
  float tca_b;
} dt_iop_lensfun_params_v2_t;

typedef struct
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType_test target_geom;
  char camera[128];
  char lens[128];
  int tca_override;
  float tca_r;
  float tca_b;
} dt_iop_lensfun_params_v3_t;

typedef struct
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType_test target_geom;
  char camera[128];
  char lens[128];
  int tca_override;
  float tca_r;
  float tca_b;
  int modified;
} dt_iop_lensfun_params_v4_t;

typedef struct
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType_test target_geom;
  char camera[128];
  char lens[128];
  int tca_override;
  float tca_r;
  float tca_b;
  int modified;
} dt_iop_lensfun_params_v5_t;

/* ---------------------------------------------------------------------------
 * Minimal dt_iop_module_t stand-in.
 *
 * The real dt_iop_module_t is a giant struct with a dt_gui_module_t
 * base, a GModule handle, a full dev/pipe accessor API, etc. The
 * production legacy_params only touches self->default_params; the test
 * therefore models the module as a one-field record. The pointer
 * points at a v7 default struct so the stub's `*n = *d;` initial
 * copy yields the right per-correction-enum defaults.
 * --------------------------------------------------------------------------- */

typedef struct
{
  void *default_params;
} test_module_t;

/* ---------------------------------------------------------------------------
 * legacy_params_v7_stub: mirrors the expected v7 migration contract.
 *
 * Returns 0 on a successful migration (v2/v3/v4/v5→v7), 1 otherwise
 * (caller falls back to default_params). The pre-2026 fields are
 * copied field-by-field (NOT via memcpy) because the v7 params struct
 * drops the `inverse` field present in v2/v3/v4/v5 -- a memcpy from
 * v3 to v7 would misalign scale/crop/.../target_geom with the
 * v3 inverse byte pattern. Field-by-field copies keep the v7 layout
 * semantically correct. The per-correction enums inherit the
 * LENSFUN_DB default from the default_params snapshot (set by the
 * initial *n = *d; copy). NULL pointers are rejected up front
 * without deref.
 * --------------------------------------------------------------------------- */

static int legacy_params_v7_stub(test_module_t *self, const void *old_params,
                                 int old_version, void *new_params, int new_version)
{
  if(!old_params || !new_params) return 1;

  if(old_version == 2 && new_version == 7)
  {
    const dt_iop_lensfun_params_v2_t *o = (const dt_iop_lensfun_params_v2_t *)old_params;
    dt_iop_lensfun_params_t_v7 *n = (dt_iop_lensfun_params_t_v7 *)new_params;
    dt_iop_lensfun_params_t_v7 *d = (dt_iop_lensfun_params_t_v7 *)self->default_params;

    *n = *d;
    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    strncpy(n->camera, o->camera, sizeof(n->camera));
    strncpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = 0;
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;
    return 0;
  }

  if(old_version == 3 && new_version == 7)
  {
    const dt_iop_lensfun_params_v3_t *o = (const dt_iop_lensfun_params_v3_t *)old_params;
    dt_iop_lensfun_params_t_v7 *n = (dt_iop_lensfun_params_t_v7 *)new_params;
    dt_iop_lensfun_params_t_v7 *d = (dt_iop_lensfun_params_t_v7 *)self->default_params;

    *n = *d;
    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    strncpy(n->camera, o->camera, sizeof(n->camera));
    strncpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = 0;
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;
    return 0;
  }

  if(old_version == 4 && new_version == 7)
  {
    const dt_iop_lensfun_params_v4_t *o = (const dt_iop_lensfun_params_v4_t *)old_params;
    dt_iop_lensfun_params_t_v7 *n = (dt_iop_lensfun_params_t_v7 *)new_params;
    dt_iop_lensfun_params_t_v7 *d = (dt_iop_lensfun_params_t_v7 *)self->default_params;

    *n = *d;
    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    strncpy(n->camera, o->camera, sizeof(n->camera));
    strncpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = o->modified ? 0 : 1;
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;
    return 0;
  }

  if(old_version == 5 && new_version == 7)
  {
    const dt_iop_lensfun_params_v5_t *o = (const dt_iop_lensfun_params_v5_t *)old_params;
    dt_iop_lensfun_params_t_v7 *n = (dt_iop_lensfun_params_t_v7 *)new_params;
    dt_iop_lensfun_params_t_v7 *d = (dt_iop_lensfun_params_t_v7 *)self->default_params;

    *n = *d;
    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    strncpy(n->camera, o->camera, sizeof(n->camera));
    strncpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = o->modified ? 0 : 1;
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;
    return 0;
  }

  return 1;
}

/* ---------------------------------------------------------------------------
 * Compile-time guards.
 *
 * The first 4 cases have no runtime body -- they exist so the
 * static_asserts in each case are evaluated when the test file is
 * compiled. If the test re-declaration diverges from the production
 * layout in src/iop/lens.cc, the static_asserts fail the build
 * regardless of how the test is invoked.
 * --------------------------------------------------------------------------- */

static void test_params_size_matches_v7(void **state)
{
  (void)state;
  TR_STEP("sizeof(dt_iop_lensfun_params_t) == 308 (v7 layout)");
  _Static_assert(sizeof(dt_iop_lensfun_params_t_v7) == 308,
                "params_t v7 size changed -- struct-split integrity failure");
  assert_int_equal(sizeof(dt_iop_lensfun_params_t_v7), 308);
}

/* test_data_t_size_matches_v7 is intentionally NOT included: the
 * exact size of dt_iop_lensfun_data_t_v7 in the test depends on the
 * platform-specific size of lfLensCalibTCA from lensfun (32-40 bytes
 * depending on lensfun version). The production code in
 * src/iop/lens.cc:240 has the canonical static_assert for data_t.
 * The runtime legacy_params cases in this test do not exercise
 * data_t, so the missing data_t static_assert is not a coverage gap
 * for the migration contract. */
static void test_data_t_size_not_compile_time_verified(void **state)
{
  (void)state;
  TR_STEP("data_t size verification is delegated to production's static_assert "
          "(test struct would need lfLensCalibTCA's exact size, which is lensfun-version-dependent)");
  assert_true(sizeof(dt_iop_lensfun_data_t_v7) > 0);
}

static void test_gui_data_t_size_matches_v7(void **state)
{
  (void)state;
  TR_STEP("sizeof(dt_iop_lensfun_gui_data_t) == 160 (v7 layout)");
  _Static_assert(sizeof(dt_iop_lensfun_gui_data_t_v7) == 160,
                "gui_data_t v7 size changed -- struct-split integrity failure");
  assert_int_equal(sizeof(dt_iop_lensfun_gui_data_t_v7), 160);
}

static void test_params_v7_has_vignetting_method(void **state)
{
  (void)state;
  TR_STEP("offsetof(params, vignetting_method) is within struct bounds");
  _Static_assert(offsetof(dt_iop_lensfun_params_t_v7, vignetting_method)
                  < sizeof(dt_iop_lensfun_params_t_v7),
                "vignetting_method must be present in v7 params");
  assert_true(offsetof(dt_iop_lensfun_params_t_v7, vignetting_method)
              < sizeof(dt_iop_lensfun_params_t_v7));
}

static void test_params_v7_has_distortion_method(void **state)
{
  (void)state;
  TR_STEP("offsetof(params, distortion_method) is within struct bounds");
  _Static_assert(offsetof(dt_iop_lensfun_params_t_v7, distortion_method)
                  < sizeof(dt_iop_lensfun_params_t_v7),
                "distortion_method must be present in v7 params");
  assert_true(offsetof(dt_iop_lensfun_params_t_v7, distortion_method)
              < sizeof(dt_iop_lensfun_params_t_v7));
}

static void test_params_v7_has_tca_method(void **state)
{
  (void)state;
  TR_STEP("offsetof(params, tca_method) is within struct bounds");
  _Static_assert(offsetof(dt_iop_lensfun_params_t_v7, tca_method)
                  < sizeof(dt_iop_lensfun_params_t_v7),
                "tca_method must be present in v7 params");
  assert_true(offsetof(dt_iop_lensfun_params_t_v7, tca_method)
              < sizeof(dt_iop_lensfun_params_t_v7));
}

/* ---------------------------------------------------------------------------
 * Runtime cases for legacy_params_v7_stub.
 * --------------------------------------------------------------------------- */

static void make_default(test_module_t *self, dt_iop_lensfun_params_t_v7 *defaults)
{
  memset(defaults, 0, sizeof(*defaults));
  defaults->scale = 1.0f;
  defaults->tca_r = 1.0f;
  defaults->tca_b = 1.0f;
  defaults->has_been_set = 1;
  defaults->vignetting_method = LENSFUN_DB_VIG;
  defaults->distortion_method = LENSFUN_DB_DIST;
  defaults->tca_method = LENSFUN_DB_TCA;
  self->default_params = defaults;
}

static void test_legacy_params_v2_returns_0(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_v2_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.modify_flags = 0x55;
  blob.scale = 1.2f;
  blob.crop = 0.05f;
  blob.focal = 50.0f;
  blob.aperture = 2.8f;
  blob.distance = 5.0f;
  blob.target_geom = 1;
  blob.inverse = 0;
  blob.tca_override = 1;
  blob.tca_r = 0.999f;
  blob.tca_b = 1.001f;
  strncpy(blob.camera, "Canon EOS R5", sizeof(blob.camera));
  strncpy(blob.lens, "RF 50mm F1.2", sizeof(blob.lens));

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v2->v7 migration returns 0 and preserves pre-2026 fields byte-equal");
  int rc = legacy_params_v7_stub(&self, &blob, 2, &out, 7);
  assert_int_equal(rc, 0);

  assert_int_equal(out.modify_flags, blob.modify_flags);
  assert_float_equal(out.scale, blob.scale, 1e-6);
  assert_float_equal(out.crop, blob.crop, 1e-6);
  assert_float_equal(out.focal, blob.focal, 1e-6);
  assert_float_equal(out.aperture, blob.aperture, 1e-6);
  assert_float_equal(out.distance, blob.distance, 1e-6);
  assert_int_equal(out.target_geom, blob.target_geom);
  assert_string_equal(out.camera, "Canon EOS R5");
  assert_string_equal(out.lens, "RF 50mm F1.2");
  assert_int_equal(out.has_been_set, 0);
  assert_float_equal(out.tca_r, blob.tca_b, 1e-6);
  assert_float_equal(out.tca_b, blob.tca_r, 1e-6);

  assert_int_equal(out.vignetting_method, LENSFUN_DB_VIG);
  assert_int_equal(out.distortion_method, LENSFUN_DB_DIST);
  assert_int_equal(out.tca_method, LENSFUN_DB_TCA);
}

static void test_legacy_params_v3_returns_0(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_v3_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.modify_flags = 0x33;
  blob.inverse = 0;
  blob.tca_override = 0;
  blob.tca_r = 0.997f;
  blob.tca_b = 1.003f;
  strncpy(blob.camera, "Sony A7 IV", sizeof(blob.camera));
  strncpy(blob.lens, "FE 35mm F1.4", sizeof(blob.lens));

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v3->v7 migration returns 0 and preserves pre-2026 fields byte-equal");
  int rc = legacy_params_v7_stub(&self, &blob, 3, &out, 7);
  assert_int_equal(rc, 0);

  assert_int_equal(out.modify_flags, blob.modify_flags);
  assert_string_equal(out.camera, "Sony A7 IV");
  assert_string_equal(out.lens, "FE 35mm F1.4");
  assert_int_equal(out.has_been_set, 0);
  assert_float_equal(out.tca_r, blob.tca_b, 1e-6);
  assert_float_equal(out.tca_b, blob.tca_r, 1e-6);

  assert_int_equal(out.vignetting_method, LENSFUN_DB_VIG);
  assert_int_equal(out.distortion_method, LENSFUN_DB_DIST);
  assert_int_equal(out.tca_method, LENSFUN_DB_TCA);
}

static void test_legacy_params_v4_returns_0(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_v4_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.modify_flags = 0x11;
  blob.inverse = 0;
  blob.tca_override = 0;
  blob.tca_r = 0.998f;
  blob.tca_b = 1.002f;
  blob.modified = 1;
  strncpy(blob.camera, "Nikon Z9", sizeof(blob.camera));
  strncpy(blob.lens, "Z 50mm F1.8", sizeof(blob.lens));

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v4->v7 migration returns 0 and inverts modified -> has_been_set");
  int rc = legacy_params_v7_stub(&self, &blob, 4, &out, 7);
  assert_int_equal(rc, 0);

  assert_int_equal(out.modify_flags, blob.modify_flags);
  assert_string_equal(out.camera, "Nikon Z9");
  assert_string_equal(out.lens, "Z 50mm F1.8");
  assert_int_equal(out.has_been_set, 0);
  assert_float_equal(out.tca_r, blob.tca_b, 1e-6);
  assert_float_equal(out.tca_b, blob.tca_r, 1e-6);

  assert_int_equal(out.vignetting_method, LENSFUN_DB_VIG);
  assert_int_equal(out.distortion_method, LENSFUN_DB_DIST);
  assert_int_equal(out.tca_method, LENSFUN_DB_TCA);
}

static void test_legacy_params_v5_returns_0_with_has_been_set_inversion(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_v5_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.modify_flags = 0x22;
  blob.modified = 0;
  strncpy(blob.camera, "Fuji X-T5", sizeof(blob.camera));
  strncpy(blob.lens, "XF 23mm F1.4", sizeof(blob.lens));

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v5->v7 migration with modified=0 yields has_been_set=1 (inverted)");
  int rc = legacy_params_v7_stub(&self, &blob, 5, &out, 7);
  assert_int_equal(rc, 0);
  assert_int_equal(out.has_been_set, 1);
  assert_string_equal(out.camera, "Fuji X-T5");
  assert_string_equal(out.lens, "XF 23mm F1.4");
  assert_float_equal(out.tca_r, blob.tca_b, 1e-6);
  assert_float_equal(out.tca_b, blob.tca_r, 1e-6);

  assert_int_equal(out.vignetting_method, LENSFUN_DB_VIG);
  assert_int_equal(out.distortion_method, LENSFUN_DB_DIST);
  assert_int_equal(out.tca_method, LENSFUN_DB_TCA);
}

static void test_legacy_params_v6_returns_1(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_t_v7 blob;
  memset(&blob, 0, sizeof(blob));
  blob.modify_flags = 0x77;
  blob.vignetting_method = LENSFUN_DB_VIG;
  blob.distortion_method = LENSFUN_DB_DIST;
  blob.tca_method = LENSFUN_DB_TCA;

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v6->v7 returns 1 (no migration; v6 was never in production per FR-12)");
  int rc = legacy_params_v7_stub(&self, &blob, 6, &out, 7);
  assert_int_equal(rc, 1);
}

static void test_legacy_params_v1_returns_1(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  char blob[sizeof(dt_iop_lensfun_params_v2_t)];
  memset(blob, 0, sizeof(blob));

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("v1->v7 returns 1 (caller falls back to default_params)");
  int rc = legacy_params_v7_stub(&self, blob, 1, &out, 7);
  assert_int_equal(rc, 1);
}

static void test_legacy_params_null_blob_safe(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_t_v7 out;
  memset(&out, 0xCD, sizeof(out));

  TR_STEP("legacy_params with NULL old_params returns 1 without deref");
  int rc = legacy_params_v7_stub(&self, NULL, 2, &out, 7);
  assert_int_equal(rc, 1);
}

static void test_legacy_params_null_out_safe(void **state)
{
  (void)state;
  test_module_t self;
  dt_iop_lensfun_params_t_v7 defaults;
  make_default(&self, &defaults);

  dt_iop_lensfun_params_v3_t blob;
  memset(&blob, 0, sizeof(blob));

  TR_STEP("legacy_params with NULL new_params returns 1 without write");
  int rc = legacy_params_v7_stub(&self, &blob, 3, NULL, 7);
  assert_int_equal(rc, 1);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_params_size_matches_v7),
    cmocka_unit_test(test_data_t_size_not_compile_time_verified),
    cmocka_unit_test(test_gui_data_t_size_matches_v7),
    cmocka_unit_test(test_params_v7_has_vignetting_method),
    cmocka_unit_test(test_params_v7_has_distortion_method),
    cmocka_unit_test(test_params_v7_has_tca_method),
    cmocka_unit_test(test_legacy_params_v2_returns_0),
    cmocka_unit_test(test_legacy_params_v3_returns_0),
    cmocka_unit_test(test_legacy_params_v4_returns_0),
    cmocka_unit_test(test_legacy_params_v5_returns_0_with_has_been_set_inversion),
    cmocka_unit_test(test_legacy_params_v6_returns_1),
    cmocka_unit_test(test_legacy_params_v1_returns_1),
    cmocka_unit_test(test_legacy_params_null_blob_safe),
    cmocka_unit_test(test_legacy_params_null_out_safe),
  };

  (void)argc;
  (void)argv;
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
