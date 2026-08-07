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
 * cmocka unit tests for the lens correction module's pure-function
 * predicate family, per_axis_modify_flags derivation, and
 * corrections_status_string formatting.
 *
 * The functions live in src/iop/lens_predicates.h as static inline
 * definitions. The test does NOT include that header (it carries C++
 * enum class types that a C compiler cannot parse); instead it links
 * against src/iop/lens_predicates.cc, which exposes a thin
 * `extern "C"` test-export shim under #ifdef BUILD_TESTING. The shim
 * functions take plain int (the C-compatible spelling of the
 * `enum class` underlying values) and forward to the static inline
 * functions inside the header.
 *
 * Coverage:
 *   - distortion_selector_entries, vignetting_selector_entries,
 *     tca_selector_entries — entry-list contracts (FR-13, FR-14,
 *     FR-15, FR-18). Includes NULL-safety (defensive narrowing).
 *   - tca_show_manual_sliders, tca_show_override_sliders — visibility
 *     predicates (FR-04, FR-19, FR-25).
 *   - per_axis_modify_flags — per-axis bit derivation + monochrome
 *     TCA gate across the 9-cell (dist x vig) matrix
 *     (FR-22, FR-24, NFR-09).
 *   - corrections_status_string — per-axis status label format
 *     (FR-08, FR-26). Includes monochrome TCA-forced-off case.
 *
 * The test deliberately includes only glib.h and cmocka.h; it does
 * NOT pull in any GTK header (FR-18, NFR-09). The shim is the single
 * linkage surface; no pixelpipe, no GUI, no lensfun DB at runtime.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lensfun.h>

/* LENSFUN_MODFLAG_MASK is a project-specific bitmask defined in
 * src/iop/lens_predicates.h:20. The public <lensfun.h> does not export
 * it, and the project header cannot be included here (it carries C++
 * `enum class` types a C compiler cannot parse). Mirror the definition
 * locally to keep the bitmask test self-contained; if the production
 * mask ever changes, this line must be updated in lockstep. */
#define LENSFUN_MODFLAG_MASK (LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING)

#include <glib.h>

#include <cmocka.h>

#include "../util/assert.h"
#include "../util/tracing.h"

/* ---------------------------------------------------------------------------
 * Underlying enum values (mirror the C++ enum classes in
 * src/iop/lens_predicates.h). The shim takes plain int, so the C test
 * spells the values as integer constants rather than relying on the
 * C++ enum class syntax.
 * --------------------------------------------------------------------------- */

#define SOURCE_OFF         0
#define SOURCE_EMBEDDED    1
#define SOURCE_LENSFUN_DB  2

#define TCA_OFF            0
#define TCA_MANUAL         1
#define TCA_LENSFUN_DB     2

/* ---------------------------------------------------------------------------
 * Shim forward declarations.
 *
 * The shim is the only externally-callable surface for these predicates
 * from a C test (the inline definitions in the header take C++ enum
 * classes, which a C compiler cannot spell). The signatures mirror
 * the C-friendly int variants in src/iop/lens_predicates.cc; the
 * shim is gated by #ifdef BUILD_TESTING, so the production build
 * does not export these symbols.
 * --------------------------------------------------------------------------- */

int test_correction_source_selector_entries(gboolean has_embedded,
                                           const char *out_labels[3],
                                           int out_values[3]);
int test_tca_selector_entries(gboolean has_embedded,
                               const char *out_labels[3],
                               int out_values[3]);
gboolean test_tca_show_manual_sliders(int tca_method);
int test_per_axis_modify_flags(int dist, int vig, int tca, gboolean monochrome);
const char *test_corrections_status_string(int dist, int vig, int tca,
                                            gboolean monochrome);

/* ---------------------------------------------------------------------------
 * correction_source_selector_entries (FR-13, FR-14, FR-18)
 * --------------------------------------------------------------------------- */

static void test_correction_source_selector_entries_false_returns_2(void **state)
{
  (void)state;
  const char *labels[3] = { NULL, NULL, NULL };
  int values[3] = { -1, -1, -1 };
  TR_STEP("correction_source, has_embedded=FALSE -> 2 entries: [off, lensfun DB]");
  int n = test_correction_source_selector_entries(FALSE, labels, values);
  assert_int_equal(n, 2);
  assert_string_equal(labels[0], "off");
  assert_string_equal(labels[1], "lensfun DB");
  assert_int_equal(values[0], SOURCE_OFF);
  assert_int_equal(values[1], SOURCE_LENSFUN_DB);
}

static void test_correction_source_selector_entries_true_returns_3_with_embedded(void **state)
{
  (void)state;
  const char *labels[3] = { NULL, NULL, NULL };
  int values[3] = { -1, -1, -1 };
  TR_STEP("correction_source, has_embedded=TRUE -> 3 entries: [off, embedded, lensfun DB]");
  int n = test_correction_source_selector_entries(TRUE, labels, values);
  assert_int_equal(n, 3);
  assert_string_equal(labels[0], "off");
  assert_string_equal(labels[1], "embedded");
  assert_string_equal(labels[2], "lensfun DB");
  assert_int_equal(values[0], SOURCE_OFF);
  assert_int_equal(values[1], SOURCE_EMBEDDED);
  assert_int_equal(values[2], SOURCE_LENSFUN_DB);
}

static void test_correction_source_selector_entries_null_safe(void **state)
{
  (void)state;
  TR_STEP("correction_source, out_labels=NULL or out_values=NULL -> 0");
  int n = test_correction_source_selector_entries(TRUE, NULL, NULL);
  assert_int_equal(n, 0);
  n = test_correction_source_selector_entries(FALSE, NULL, NULL);
  assert_int_equal(n, 0);
}

/* ---------------------------------------------------------------------------
 * tca_selector_entries (FR-15: MANUAL is always present; has_embedded
 * is accepted for signature uniformity but does not gate any entry)
 * --------------------------------------------------------------------------- */

static void test_tca_selector_entries_false_returns_3_with_manual(void **state)
{
  (void)state;
  const char *labels[3] = { NULL, NULL, NULL };
  int values[3] = { -1, -1, -1 };
  TR_STEP("tca, has_embedded=FALSE -> 3 entries: [off, manual, lensfun DB]");
  int n = test_tca_selector_entries(FALSE, labels, values);
  assert_int_equal(n, 3);
  assert_string_equal(labels[0], "off");
  assert_string_equal(labels[1], "manual");
  assert_string_equal(labels[2], "lensfun DB");
  assert_int_equal(values[0], TCA_OFF);
  assert_int_equal(values[1], TCA_MANUAL);
  assert_int_equal(values[2], TCA_LENSFUN_DB);
}

static void test_tca_selector_entries_true_returns_3_with_manual(void **state)
{
  (void)state;
  const char *labels[3] = { NULL, NULL, NULL };
  int values[3] = { -1, -1, -1 };
  TR_STEP("tca, has_embedded=TRUE -> 3 entries (FR-15: has_embedded ignored, MANUAL always present)");
  int n = test_tca_selector_entries(TRUE, labels, values);
  assert_int_equal(n, 3);
  assert_string_equal(labels[0], "off");
  assert_string_equal(labels[1], "manual");
  assert_string_equal(labels[2], "lensfun DB");
  assert_int_equal(values[0], TCA_OFF);
  assert_int_equal(values[1], TCA_MANUAL);
  assert_int_equal(values[2], TCA_LENSFUN_DB);
}

static void test_tca_selector_entries_null_safe(void **state)
{
  (void)state;
  TR_STEP("tca, out_labels=NULL or out_values=NULL -> 0 (defensive narrowing)");
  int n = test_tca_selector_entries(FALSE, NULL, NULL);
  assert_int_equal(n, 0);
  n = test_tca_selector_entries(TRUE, NULL, NULL);
  assert_int_equal(n, 0);
}

/* ---------------------------------------------------------------------------
 * tca_show_manual_sliders (FR-04, FR-15, FR-25)
 * --------------------------------------------------------------------------- */

static void test_tca_show_manual_sliders_off_false(void **state)
{
  (void)state;
  TR_STEP("tca_show_manual_sliders(OFF) == FALSE");
  assert_false(test_tca_show_manual_sliders(TCA_OFF));
}

static void test_tca_show_manual_sliders_manual_true(void **state)
{
  (void)state;
  TR_STEP("tca_show_manual_sliders(MANUAL) == TRUE");
  assert_true(test_tca_show_manual_sliders(TCA_MANUAL));
}

static void test_tca_show_manual_sliders_lensfun_DB_false(void **state)
{
  (void)state;
  TR_STEP("tca_show_manual_sliders(LENSFUN_DB) == FALSE");
  assert_false(test_tca_show_manual_sliders(TCA_LENSFUN_DB));
}

/* ---------------------------------------------------------------------------
 * per_axis_modify_flags (FR-22, FR-24, NFR-09)
 *
 * The function returns the bitwise OR of LF_MODIFY_DISTORTION /
 * LF_MODIFY_VIGNETTING / LF_MODIFY_TCA for axes that select the
 * lensfun DB source, masked with LENSFUN_MODFLAG_MASK. The TCA bit
 * is cleared when monochrome is TRUE.
 * --------------------------------------------------------------------------- */

static void test_per_axis_modify_flags_all_lensfun_DB_color(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(LF, LF, LF, color) == DIST|VIG|TCA");
  int flags = test_per_axis_modify_flags(SOURCE_LENSFUN_DB,
                                          SOURCE_LENSFUN_DB,
                                          TCA_LENSFUN_DB,
                                          FALSE);
  assert_int_equal(flags, LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING | LF_MODIFY_TCA);
}

static void test_per_axis_modify_flags_all_lensfun_DB_monochrome(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(LF, LF, LF, mono) == DIST|VIG (TCA bit cleared per FR-24)");
  int flags = test_per_axis_modify_flags(SOURCE_LENSFUN_DB,
                                          SOURCE_LENSFUN_DB,
                                          TCA_LENSFUN_DB,
                                          TRUE);
  assert_int_equal(flags, LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING);
}

static void test_per_axis_modify_flags_embedded_embedded_off(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(EMB, EMB, OFF, color) == 0 (no lensfun axis)");
  int flags = test_per_axis_modify_flags(SOURCE_EMBEDDED,
                                          SOURCE_EMBEDDED,
                                          TCA_OFF,
                                          FALSE);
  assert_int_equal(flags, 0);
}

static void test_per_axis_modify_flags_lensfun_DB_embedded_manual(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(LF, EMB, MANUAL, color) == DIST "
          "(TCA bit clear because TCA == MANUAL, not LENSFUN_DB)");
  int flags = test_per_axis_modify_flags(SOURCE_LENSFUN_DB,
                                          SOURCE_EMBEDDED,
                                          TCA_MANUAL,
                                          FALSE);
  assert_int_equal(flags, LF_MODIFY_DISTORTION);
}

static void test_per_axis_modify_flags_embedded_lensfun_DB_lensfun_DB(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(EMB, LF, LF, color) == VIG|TCA "
          "(distortion off because EMBEDDED, not LENSFUN_DB)");
  int flags = test_per_axis_modify_flags(SOURCE_EMBEDDED,
                                          SOURCE_LENSFUN_DB,
                                          TCA_LENSFUN_DB,
                                          FALSE);
  assert_int_equal(flags, LF_MODIFY_VIGNETTING | LF_MODIFY_TCA);
}

static void test_per_axis_modify_flags_off_off_off(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(OFF, OFF, OFF, color) == 0 (identity for all axes)");
  int flags = test_per_axis_modify_flags(SOURCE_OFF, SOURCE_OFF, TCA_OFF, FALSE);
  assert_int_equal(flags, 0);
}

static void test_per_axis_modify_flags_dist_only_LENSFUN_DB(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(LF, OFF, OFF, color) == DIST (only distortion lensfun)");
  int flags = test_per_axis_modify_flags(SOURCE_LENSFUN_DB, SOURCE_OFF, TCA_OFF, FALSE);
  assert_int_equal(flags, LF_MODIFY_DISTORTION);
}

static void test_per_axis_modify_flags_vig_only_LENSFUN_DB(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(OFF, LF, OFF, color) == VIG (only vignetting lensfun)");
  int flags = test_per_axis_modify_flags(SOURCE_OFF, SOURCE_LENSFUN_DB, TCA_OFF, FALSE);
  assert_int_equal(flags, LF_MODIFY_VIGNETTING);
}

static void test_per_axis_modify_flags_tca_only_LENSFUN_DB_color(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(OFF, OFF, LF, color) == TCA (only TCA lensfun, color)");
  int flags = test_per_axis_modify_flags(SOURCE_OFF, SOURCE_OFF, TCA_LENSFUN_DB, FALSE);
  assert_int_equal(flags, LF_MODIFY_TCA);
}

static void test_per_axis_modify_flags_tca_only_LENSFUN_DB_monochrome(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(OFF, OFF, LF, mono) == 0 (TCA bit cleared for monochrome)");
  int flags = test_per_axis_modify_flags(SOURCE_OFF, SOURCE_OFF, TCA_LENSFUN_DB, TRUE);
  assert_int_equal(flags, 0);
}

static void test_per_axis_modify_flags_MANUAL_tca_not_lensfun(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(OFF, OFF, MANUAL, color) == 0 (MANUAL is not LENSFUN_DB, no TCA bit)");
  int flags = test_per_axis_modify_flags(SOURCE_OFF, SOURCE_OFF, TCA_MANUAL, FALSE);
  assert_int_equal(flags, 0);
}

static void test_per_axis_modify_flags_EMBEDDED_does_not_set_bits(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(EMB, EMB, OFF, color) == 0 (EMBEDDED is not LENSFUN_DB)");
  int flags = test_per_axis_modify_flags(SOURCE_EMBEDDED,
                                          SOURCE_EMBEDDED,
                                          TCA_OFF,
                                          FALSE);
  assert_int_equal(flags, 0);
}

/* ---------------------------------------------------------------------------
 * per_axis_modify_flags: monochrome TCA gate across the full 9-cell
 * (dist x vig) matrix. Every cell with tca == LENSFUN_DB and
 * monochrome == TRUE must clear the TCA bit. Color cells with the
 * same enums must set the TCA bit (proves the bit is gated on
 * monochrome, not on dist/vig).
 * --------------------------------------------------------------------------- */

static void test_per_axis_modify_flags_monochrome_clears_tca_all_nine_combos(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(<any>, <any>, LF, mono) -- TCA bit never set for monochrome "
          "across all 9 (dist x vig) cells");
  const int dists[] = { SOURCE_OFF, SOURCE_EMBEDDED, SOURCE_LENSFUN_DB };
  const int vigs[]  = { SOURCE_OFF, SOURCE_EMBEDDED, SOURCE_LENSFUN_DB };
  for(size_t i = 0; i < 3; i++)
  {
    for(size_t j = 0; j < 3; j++)
    {
      int flags = test_per_axis_modify_flags(dists[i], vigs[j],
                                              TCA_LENSFUN_DB, TRUE);
      assert_int_equal(flags & LF_MODIFY_TCA, 0);
    }
  }
}

static void test_per_axis_modify_flags_color_sets_tca_for_lensfun_DB_combos(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags(<any>, <any>, LF, color) -- TCA bit always set in color mode "
          "across all 9 (dist x vig) cells");
  const int dists[] = { SOURCE_OFF, SOURCE_EMBEDDED, SOURCE_LENSFUN_DB };
  const int vigs[]  = { SOURCE_OFF, SOURCE_EMBEDDED, SOURCE_LENSFUN_DB };
  for(size_t i = 0; i < 3; i++)
  {
    for(size_t j = 0; j < 3; j++)
    {
      int flags = test_per_axis_modify_flags(dists[i], vigs[j],
                                              TCA_LENSFUN_DB, FALSE);
      assert_int_equal(flags & LF_MODIFY_TCA, LF_MODIFY_TCA);
    }
  }
}

static void test_per_axis_modify_flags_masked_to_LENSFUN_MODFLAG_MASK(void **state)
{
  (void)state;
  TR_STEP("per_axis_modify_flags output is masked with LENSFUN_MODFLAG_MASK "
          "(only the 3 named LF_MODIFY_* bits survive)");
  int flags = test_per_axis_modify_flags(SOURCE_LENSFUN_DB,
                                          SOURCE_LENSFUN_DB,
                                          TCA_LENSFUN_DB,
                                          FALSE);
  assert_int_equal(flags & ~LENSFUN_MODFLAG_MASK, 0);
}

/* ---------------------------------------------------------------------------
 * corrections_status_string (FR-08, FR-26, OQ-08)
 *
 * Format (per OQ-08): "distortion: <s>, vignetting: <s>, TCA: <s>" where
 * <s> is one of {off, embedded, lensfun DB, manual}. Monochrome forces
 * TCA's <s> to "off" regardless of the user's selection (FR-24).
 *
 * The function returns a pointer to a static buffer; each test reads
 * the result before the next call to avoid aliasing.
 * --------------------------------------------------------------------------- */

static void test_corrections_status_string_all_lensfun_DB_color(void **state)
{
  (void)state;
  TR_STEP("corrections_status_string(LF, LF, LF, color) == "
          "\"distortion: lensfun DB, vignetting: lensfun DB, TCA: lensfun DB\"");
  const char *s = test_corrections_status_string(SOURCE_LENSFUN_DB,
                                                  SOURCE_LENSFUN_DB,
                                                  TCA_LENSFUN_DB,
                                                  FALSE);
  assert_non_null(s);
  assert_string_equal(s, "distortion: lensfun DB, vignetting: lensfun DB, TCA: lensfun DB");
}

static void test_corrections_status_string_mixed(void **state)
{
  (void)state;
  TR_STEP("corrections_status_string(LF, EMB, MANUAL, color) == "
          "\"distortion: lensfun DB, vignetting: embedded, TCA: manual\"");
  const char *s = test_corrections_status_string(SOURCE_LENSFUN_DB,
                                                  SOURCE_EMBEDDED,
                                                  TCA_MANUAL,
                                                  FALSE);
  assert_non_null(s);
  assert_string_equal(s, "distortion: lensfun DB, vignetting: embedded, TCA: manual");
}

static void test_corrections_status_string_all_off(void **state)
{
  (void)state;
  TR_STEP("corrections_status_string(OFF, OFF, OFF, color) == "
          "\"distortion: off, vignetting: off, TCA: off\"");
  const char *s = test_corrections_status_string(SOURCE_OFF, SOURCE_OFF, TCA_OFF, FALSE);
  assert_non_null(s);
  assert_string_equal(s, "distortion: off, vignetting: off, TCA: off");
}

static void test_corrections_status_string_monochrome_tca_forced_off(void **state)
{
  (void)state;
  TR_STEP("corrections_status_string(LF, LF, LF, mono) ends with \"TCA: off\" "
          "even though the combobox selection is LENSFUN_DB (FR-24, EC-06)");
  const char *s = test_corrections_status_string(SOURCE_LENSFUN_DB,
                                                  SOURCE_LENSFUN_DB,
                                                  TCA_LENSFUN_DB,
                                                  TRUE);
  assert_non_null(s);
  assert_string_equal(s, "distortion: lensfun DB, vignetting: lensfun DB, TCA: off");
}

static void test_corrections_status_string_all_embedded(void **state)
{
  (void)state;
  TR_STEP("corrections_status_string(EMB, EMB, OFF, color) == "
          "\"distortion: embedded, vignetting: embedded, TCA: off\"");
  const char *s = test_corrections_status_string(SOURCE_EMBEDDED,
                                                  SOURCE_EMBEDDED,
                                                  TCA_OFF,
                                                  FALSE);
  assert_non_null(s);
  assert_string_equal(s, "distortion: embedded, vignetting: embedded, TCA: off");
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    /* correction_source_selector_entries (3) — merged distortion + vignetting */
    cmocka_unit_test(test_correction_source_selector_entries_false_returns_2),
    cmocka_unit_test(test_correction_source_selector_entries_true_returns_3_with_embedded),
    cmocka_unit_test(test_correction_source_selector_entries_null_safe),
    /* tca_selector_entries (3) */
    cmocka_unit_test(test_tca_selector_entries_false_returns_3_with_manual),
    cmocka_unit_test(test_tca_selector_entries_true_returns_3_with_manual),
    cmocka_unit_test(test_tca_selector_entries_null_safe),
    /* tca_show_manual_sliders (3) */
    cmocka_unit_test(test_tca_show_manual_sliders_off_false),
    cmocka_unit_test(test_tca_show_manual_sliders_manual_true),
    cmocka_unit_test(test_tca_show_manual_sliders_lensfun_DB_false),
    /* per_axis_modify_flags (12) */
    cmocka_unit_test(test_per_axis_modify_flags_all_lensfun_DB_color),
    cmocka_unit_test(test_per_axis_modify_flags_all_lensfun_DB_monochrome),
    cmocka_unit_test(test_per_axis_modify_flags_embedded_embedded_off),
    cmocka_unit_test(test_per_axis_modify_flags_lensfun_DB_embedded_manual),
    cmocka_unit_test(test_per_axis_modify_flags_embedded_lensfun_DB_lensfun_DB),
    cmocka_unit_test(test_per_axis_modify_flags_off_off_off),
    cmocka_unit_test(test_per_axis_modify_flags_dist_only_LENSFUN_DB),
    cmocka_unit_test(test_per_axis_modify_flags_vig_only_LENSFUN_DB),
    cmocka_unit_test(test_per_axis_modify_flags_tca_only_LENSFUN_DB_color),
    cmocka_unit_test(test_per_axis_modify_flags_tca_only_LENSFUN_DB_monochrome),
    cmocka_unit_test(test_per_axis_modify_flags_MANUAL_tca_not_lensfun),
    cmocka_unit_test(test_per_axis_modify_flags_EMBEDDED_does_not_set_bits),
    cmocka_unit_test(test_per_axis_modify_flags_monochrome_clears_tca_all_nine_combos),
    cmocka_unit_test(test_per_axis_modify_flags_color_sets_tca_for_lensfun_DB_combos),
    cmocka_unit_test(test_per_axis_modify_flags_masked_to_LENSFUN_MODFLAG_MASK),
    /* corrections_status_string (5) */
    cmocka_unit_test(test_corrections_status_string_all_lensfun_DB_color),
    cmocka_unit_test(test_corrections_status_string_mixed),
    cmocka_unit_test(test_corrections_status_string_all_off),
    cmocka_unit_test(test_corrections_status_string_monochrome_tca_forced_off),
    cmocka_unit_test(test_corrections_status_string_all_embedded),
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
