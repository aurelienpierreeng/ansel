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

/** The rasterisation family answers the same question the same way for every shape.
 *
 * It did not use to. The family returned a bare int on a "0 means success" convention that
 * could not express the third outcome that actually occurs -- a shape with nothing to draw --
 * so each shape picked its own answer and they disagreed. A form with no points made a circle
 * report failure, which aborted the whole group fold and blanked the mask, and made an ellipse
 * report success, which folded it as zeros. Three shapes reported success from the outline
 * builder while writing no outline at all, which stamped the outline cache over a NULL and hid
 * the shape until the geometry next moved. Two more reported success from get_area/get_mask
 * without writing a single out-parameter, which callers then read.
 *
 * Every one of those was a disagreement between shapes about a contract nobody had written
 * down, so these tests pin the contract rather than any one shape's behaviour: the degenerate
 * input goes to every implementation, and they must all answer alike.
 *
 * The forms are built on the stack instead of through dt_masks_create(), which reaches for
 * conf and the supervisor. A shape struct plus its function table is the whole input the
 * contract is about.
 */

#include "develop/masks.h"
#include "develop/masks/masks_functions.h"
#include "develop/masks/masks_touched.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

/** Every shape whose rasteriser can be reached with a degenerate form and no pipeline.
 *
 * Brush and polygon are deliberately absent: both check their module argument before they look
 * at the geometry, so reaching their "no geometry" branch needs a live module and pipe. Their
 * module-less branch is covered by the ERROR case below instead. */
typedef struct
{
  const char *name;
  dt_masks_type_t type;
  const dt_masks_functions_t *functions;
  /* Whether this shape's bounding box is a function of its geometry at all. A gradient's is
   * not: it covers the whole frame by construction, so it reads the pipe's dimensions without
   * ever looking at its points, and "no points" is not a degenerate area for it. Every other
   * shape derives its box from its geometry and must report the absence of one. */
  gboolean area_depends_on_geometry;
} _shape_t;

static const _shape_t _shapes[] = {
  { "circle",   DT_MASKS_CIRCLE,   &dt_masks_functions_circle,   TRUE },
  { "ellipse",  DT_MASKS_ELLIPSE,  &dt_masks_functions_ellipse,  TRUE },
  { "gradient", DT_MASKS_GRADIENT, &dt_masks_functions_gradient, FALSE },
  { "group",    DT_MASKS_GROUP,    &dt_masks_functions_group,    TRUE },
};
static const size_t _shape_count = sizeof(_shapes) / sizeof(_shapes[0]);

/** A form of the given kind carrying no geometry at all -- the degenerate case every shape
 * used to answer differently. */
static dt_masks_form_t _empty_form(const _shape_t *shape)
{
  dt_masks_form_t form = { 0 };
  form.type = shape->type;
  form.functions = shape->functions;
  form.points = NULL;
  return form;
}

static void _from_status_maps_zero_to_ok_and_everything_else_to_error(void **state)
{
  (void)state;
  // The adapter for helpers still on the old convention: they cannot report EMPTY, so
  // anything non-zero has to read as the conservative outcome.
  assert_int_equal(dt_masks_raster_from_status(0), DT_MASKS_RASTER_OK);
  assert_int_equal(dt_masks_raster_from_status(1), DT_MASKS_RASTER_ERROR);
  assert_int_equal(dt_masks_raster_from_status(-1), DT_MASKS_RASTER_ERROR);
}

static void _a_shape_with_no_geometry_rasterises_empty(void **state)
{
  (void)state;
  const dt_iop_roi_t roi = { 0, 0, 4, 4, 1.0f };
  float buffer[16] = { 0.0f };

  for(size_t i = 0; i < _shape_count; i++)
  {
    dt_masks_form_t form = _empty_form(&_shapes[i]);
    dt_iop_roi_t touched;
    const dt_masks_raster_result_t result
        = dt_masks_get_mask_roi(NULL, NULL, NULL, &form, &roi, buffer, &touched);

    // Not ERROR (which aborts the group fold and blanks the whole mask) and not OK (which
    // would claim the buffer was written). This is the disagreement that used to exist
    // between the circle and the ellipse, on identical input.
    assert_int_equal(result, DT_MASKS_RASTER_EMPTY);

    // EMPTY promises the caller an empty touched box, which is what lets the group fold clear
    // and combine only what a child actually wrote.
    assert_true(dt_masks_touched_is_empty(&touched));
  }
}

static void _a_shape_with_no_rasteriser_is_an_error(void **state)
{
  (void)state;
  const dt_iop_roi_t roi = { 0, 0, 4, 4, 1.0f };
  float buffer[16] = { 0.0f };
  dt_iop_roi_t touched;

  // No function table at all: a shape type nobody implemented is a programming error, never an
  // empty shape -- the fold must refuse to publish a buffer nobody wrote.
  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_NONE;
  form.functions = NULL;

  assert_int_equal(dt_masks_get_mask_roi(NULL, NULL, NULL, &form, &roi, buffer, &touched),
                   DT_MASKS_RASTER_ERROR);
}

static void _an_unbuildable_outline_is_never_reported_as_built(void **state)
{
  (void)state;

  for(size_t i = 0; i < _shape_count; i++)
  {
    if(IS_NULL_PTR(_shapes[i].functions->get_points_border)) continue; // group has none

    dt_masks_form_t form = _empty_form(&_shapes[i]);
    float *points = NULL, *border = NULL;
    int points_count = -1, border_count = -1;

    // The caller stamps the group-wide outline cache key on success. Reporting success here
    // with *points still NULL is what marked the cache current over an outline that had never
    // been built, hiding the shape until the geometry generation next changed.
    assert_int_not_equal(dt_masks_get_points_border(NULL, &form, &points, &points_count, &border,
                                                    &border_count, 0, NULL),
                         DT_MASKS_RASTER_OK);
    assert_null(points);
  }
}

static void _area_and_mask_always_write_their_out_parameters(void **state)
{
  (void)state;

  for(size_t i = 0; i < _shape_count; i++)
  {
    dt_masks_form_t form = _empty_form(&_shapes[i]);

    /* Poisoned on purpose: these are the callers' uninitialised stack. iop/spots.c reads
     * width/height straight after the call and iop/retouch.c sizes an allocation from them,
     * so "reported not-OK" is not enough -- the values have to be defined too. */
    int width = -12345, height = -12345, posx = -12345, posy = -12345;
    float *buffer = (float *)(intptr_t)-1;

    if(_shapes[i].area_depends_on_geometry && !IS_NULL_PTR(form.functions->get_area))
    {
      const dt_masks_raster_result_t area = dt_masks_get_area(NULL, NULL, NULL, &form, &width, &height,
                                                              &posx, &posy);
      assert_int_not_equal(area, DT_MASKS_RASTER_OK);
      assert_int_equal(width, 0);
      assert_int_equal(height, 0);
      assert_int_equal(posx, 0);
      assert_int_equal(posy, 0);
    }

    width = height = posx = posy = -12345;
    if(!IS_NULL_PTR(form.functions->get_mask))
    {
      const dt_masks_raster_result_t mask
          = dt_masks_get_mask(NULL, NULL, NULL, &form, &buffer, &width, &height, &posx, &posy);
      assert_int_not_equal(mask, DT_MASKS_RASTER_OK);
      assert_null(buffer);
      assert_int_equal(width, 0);
      assert_int_equal(height, 0);
      assert_int_equal(posx, 0);
      assert_int_equal(posy, 0);
    }
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_from_status_maps_zero_to_ok_and_everything_else_to_error),
    cmocka_unit_test(_a_shape_with_no_geometry_rasterises_empty),
    cmocka_unit_test(_a_shape_with_no_rasteriser_is_an_error),
    cmocka_unit_test(_an_unbuildable_outline_is_never_reported_as_built),
    cmocka_unit_test(_area_and_mask_always_write_their_out_parameters),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
