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
#include "develop/masks_group.h"
#include "develop/develop.h"       // dt_develop_t, for the write-API fixture

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
    float *points = NULL;
    float *border = NULL;
    int points_count = -1;
    int border_count = -1;

    // The caller stamps the group-wide outline cache key on success. Reporting success here
    // with *points still NULL is what marked the cache current over an outline that had never
    // been built, hiding the shape until the geometry generation next changed.
    assert_int_not_equal(dt_masks_get_points_border(NULL, &form, &points, &points_count, &border,
                                                    &border_count, NULL, NULL, 0, NULL),
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
    int width = -12345;
    int height = -12345;
    int posx = -12345;
    int posy = -12345;
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

    width = -12345;
    height = -12345;
    posx = -12345;
    posy = -12345;
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


/* ---------------------------------------------------------------------------------------
 * The read side of the group API (develop/masks_group.h).
 *
 * These pin the contract rather than any implementation: what a caller is promised when it asks
 * the module a question instead of reading its structs. The group fixture is built on the stack
 * with its membership rows in a plain array, so the ORDER under test is unambiguous.
 * ------------------------------------------------------------------------------------- */

static void _form_info_describes_a_shape(void **state)
{
  (void)state;
  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_CIRCLE;
  form.formid = 4242;
  form.version = 6;
  g_strlcpy(form.name, "circle #2", sizeof(form.name));

  dt_masks_form_info_t info;
  assert_true(dt_masks_form_get_info(&form, &info));
  assert_int_equal(info.formid, 4242);
  assert_int_equal(info.version, 6);
  assert_false(info.is_group);
  assert_false(info.is_retouch);
  assert_int_equal(info.member_count, 0);   // not a group
  assert_string_equal(info.name, "circle #2");

  // A clone shape belongs to retouch, and the predicate for that was hand-written in eight files.
  form.type = DT_MASKS_CIRCLE | DT_MASKS_CLONE;
  assert_true(dt_masks_form_get_info(&form, &info));
  assert_true(info.is_retouch);
}

static void _form_info_leaves_out_untouched_on_failure(void **state)
{
  (void)state;
  dt_masks_form_info_t info;
  info.formid = -999;   // a default the caller wants to survive a failed call
  assert_false(dt_masks_form_get_info(NULL, &info));
  assert_int_equal(info.formid, -999);
}

static void _copy_members_preserves_order_and_reports_the_total(void **state)
{
  (void)state;
  dt_masks_form_group_t rows[3] = {
    { .formid = 11, .parentid = 7, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION,        .opacity = 0.25f },
    { .formid = 22, .parentid = 7, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_INTERSECTION, .opacity = 0.50f },
    { .formid = 33, .parentid = 7, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_DIFFERENCE,   .opacity = 1.00f },
  };
  dt_masks_form_t group = { 0 };
  group.type = DT_MASKS_GROUP;
  for(int i = 0; i < 3; i++) group.points = g_list_append(group.points, &rows[i]);

  dt_masks_form_info_t info;
  assert_true(dt_masks_form_get_info(&group, &info));
  assert_true(info.is_group);
  assert_int_equal(info.member_count, 3);

  // NULL storage asks for the count only.
  assert_int_equal(dt_masks_group_copy_members(&group, NULL, 0), 3);

  dt_masks_member_t members[3];
  assert_int_equal(dt_masks_group_copy_members(&group, members, 3), 3);
  for(guint i = 0; i < 3; i++)
  {
    // Order is the contract: it is the compositing order, the GTK row order, and the index into
    // retouch's rt_forms[] and spots' clone_algo[] -- the last two persisted in the user's
    // database. A filtering or reordering implementation passes every other test in this file.
    assert_int_equal(members[i].index, i);
    assert_int_equal(members[i].formid, rows[i].formid);
    assert_int_equal(members[i].parentid, rows[i].parentid);
    assert_int_equal((int)members[i].state, rows[i].state);
  }
  assert_true(members[0].opacity == rows[0].opacity);

  // A short buffer still reports the total, and writes exactly what fits.
  dt_masks_member_t two[2];
  two[0].formid = two[1].formid = -1;
  assert_int_equal(dt_masks_group_copy_members(&group, two, 2), 3);
  assert_int_equal(two[0].formid, 11);
  assert_int_equal(two[1].formid, 22);

  g_list_free(group.points);
}

static void _copy_members_refuses_anything_that_is_not_a_group(void **state)
{
  (void)state;
  // A shape's ->points holds geometry nodes, not membership rows -- the same field, a different
  // element type, told apart only by this bit. Refusing here is what keeps that polymorphism
  // unreachable from outside the module.
  dt_masks_node_circle_t node = { 0 };
  dt_masks_form_t shape = { 0 };
  shape.type = DT_MASKS_CIRCLE;
  shape.points = g_list_append(NULL, &node);

  dt_masks_member_t members[4];
  assert_int_equal(dt_masks_group_copy_members(&shape, members, 4), 0);
  assert_int_equal(dt_masks_group_copy_members(NULL, members, 4), 0);

  g_list_free(shape.points);
}

static void _type_tokens_are_the_persisted_conf_key_spellings(void **state)
{
  (void)state;
  /* These strings build plugins/darkroom/<plugin>/<type>/<feature>, declared in
   * data/anselconfig.xml.in. A shape reading a key that is not in confgen gets 0, so renaming a
   * token here silently resets that setting for every existing user. In particular the polygon
   * token is "polygon" and must never become "path" -- another part of the tree spells it that
   * way in machine-readable JSON, and unifying on that spelling is the trap. */
  assert_string_equal(dt_masks_type_name(DT_MASKS_CIRCLE), "circle");
  assert_string_equal(dt_masks_type_name(DT_MASKS_ELLIPSE), "ellipse");
  assert_string_equal(dt_masks_type_name(DT_MASKS_POLYGON), "polygon");
  assert_string_equal(dt_masks_type_name(DT_MASKS_BRUSH), "brush");
  assert_string_equal(dt_masks_type_name(DT_MASKS_GRADIENT), "gradient");
  assert_string_equal(dt_masks_type_name(DT_MASKS_GROUP), "group");
  assert_string_equal(dt_masks_type_name(DT_MASKS_NONE), "unknown");

  // The type is a bit field, so a clone circle is still a circle for the conf key.
  assert_string_equal(dt_masks_type_name(DT_MASKS_CIRCLE | DT_MASKS_CLONE), "circle");
}


/* ---------------------------------------------------------------------------------------
 * The write side: dt_masks_group_set_member_operation().
 *
 * The fixture is a one-group dev on the stack. dt_masks_cow_touch() returns immediately when a
 * form's refcount is 1 -- no lock, no list walk -- so a freshly built form exercises the setter
 * end to end without needing a live darkroom. That is also the case every caller hits when
 * nothing else references the group.
 * ------------------------------------------------------------------------------------- */

typedef struct
{
  dt_develop_t dev;
  dt_masks_form_t group;
  dt_masks_form_group_t rows[2];
} _write_fixture_t;

static void _write_fixture_init(_write_fixture_t *f)
{
  memset(f, 0, sizeof(*f));
  dt_pthread_rwlock_init(&f->dev.masks_mutex, NULL);

  f->rows[0] = (dt_masks_form_group_t){ .formid = 11, .parentid = 7,
                                        .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION,
                                        .opacity = 0.5f };
  f->rows[1] = (dt_masks_form_group_t){ .formid = 22, .parentid = 7,
                                        .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_INTERSECTION,
                                        .opacity = 1.0f };
  f->group.type = DT_MASKS_GROUP;
  f->group.formid = 7;
  /* The vtable is what makes a group's membership rows copyable: dt_masks_dup_masks_form() sizes
   * each ->points payload from functions->point_struct_size, and a form whose functions are NULL
   * clones with an EMPTY membership list. Needed by the copy-on-write case below. */
  f->group.functions = &dt_masks_functions_group;
  /* Sole owner: dt_masks_cow_touch() returns the form unchanged at refcount 1, without taking a
   * lock or walking dev->forms, so the setter runs end to end on a stack fixture. */
  dt_atomic_set_int(&f->group.refcount, 1);
  for(int i = 0; i < 2; i++) f->group.points = g_list_append(f->group.points, &f->rows[i]);
  f->dev.forms = g_list_append(NULL, &f->group);
}

static void _write_fixture_cleanup(_write_fixture_t *f)
{
  g_list_free(f->group.points);
  g_list_free(f->dev.forms);
  dt_pthread_rwlock_destroy(&f->dev.masks_mutex);
}

static void _set_operation_replaces_the_combine_op(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  dt_masks_member_t m;
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 22, DT_MASKS_STATE_UNION, &m),
                   DT_MASKS_OK);
  // the OLD operator is gone, not merely joined by the new one
  assert_int_equal(f.rows[1].state & DT_MASKS_STATE_INTERSECTION, 0);
  assert_true((f.rows[1].state & DT_MASKS_STATE_UNION) != 0);
  // and everything that is not a combine operator survives
  assert_true((f.rows[1].state & DT_MASKS_STATE_USE) != 0);
  // the caller gets the row back, index included, without ever holding a pointer into the group
  assert_int_equal(m.formid, 22);
  assert_int_equal(m.index, 1);
  assert_true(m.opacity == 1.0f);

  _write_fixture_cleanup(&f);
}

static void _setting_the_state_it_already_has_is_unchanged(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  // row 0 is already UNION. A caller must be able to tell "nothing happened" from "done", or a
  // no-op click writes an undo step and a database row.
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 11, DT_MASKS_STATE_UNION, NULL),
                   DT_MASKS_UNCHANGED);

  _write_fixture_cleanup(&f);
}

static void _inverse_toggles_and_leaves_the_operator_alone(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  const int before = f.rows[0].state;
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 11, DT_MASKS_STATE_INVERSE, NULL),
                   DT_MASKS_OK);
  assert_true((f.rows[0].state & DT_MASKS_STATE_INVERSE) != 0);
  assert_true((f.rows[0].state & DT_MASKS_STATE_UNION) != 0);   // operator untouched

  // toggling back restores exactly the previous state -- it is a toggle, not a set
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 11, DT_MASKS_STATE_INVERSE, NULL),
                   DT_MASKS_OK);
  assert_int_equal(f.rows[0].state, before);

  _write_fixture_cleanup(&f);
}

static void _set_operation_rejects_what_it_cannot_do(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 999, 11, DT_MASKS_STATE_UNION, NULL),
                   DT_MASKS_NOT_FOUND);                       // no such group
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 999, DT_MASKS_STATE_UNION, NULL),
                   DT_MASKS_NOT_FOUND);                       // no such member
  assert_int_equal(dt_masks_group_set_member_operation(&f.dev, 7, 11, DT_MASKS_STATE_USE, NULL),
                   DT_MASKS_INVALID);                         // not an operator at all
  assert_int_equal(dt_masks_group_set_member_operation(NULL, 7, 11, DT_MASKS_STATE_UNION, NULL),
                   DT_MASKS_INVALID);

  // a refused call changes nothing
  assert_int_equal(f.rows[0].state, DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION);

  _write_fixture_cleanup(&f);
}


/* ---------------------------------------------------------------------------------------
 * dt_masks_group_get_member() / dt_masks_group_set_member_opacity().
 * ------------------------------------------------------------------------------------- */

static void _get_member_reads_a_row_by_identity(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  dt_masks_member_t m;
  assert_int_equal(dt_masks_group_get_member(&f.dev, 7, 22, &m), DT_MASKS_OK);
  assert_int_equal(m.formid, 22);
  assert_int_equal(m.parentid, 7);
  assert_int_equal(m.index, 1);
  assert_true(m.opacity == 1.0f);

  // NULL out is a legitimate existence probe -- the menu builders use it exactly that way
  assert_int_equal(dt_masks_group_get_member(&f.dev, 7, 11, NULL), DT_MASKS_OK);

  assert_int_equal(dt_masks_group_get_member(&f.dev, 999, 11, &m), DT_MASKS_NOT_FOUND);
  assert_int_equal(dt_masks_group_get_member(&f.dev, 7, 999, &m), DT_MASKS_NOT_FOUND);
  assert_int_equal(dt_masks_group_get_member(NULL, 7, 11, &m), DT_MASKS_INVALID);

  _write_fixture_cleanup(&f);
}

static void _set_opacity_clamps_and_reports_what_it_stored(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  dt_masks_member_t m;
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, 0.25f, &m), DT_MASKS_OK);
  assert_true(f.rows[0].opacity == 0.25f);
  assert_true(m.opacity == 0.25f);

  // out-of-range is clamped, and the caller is told the CLAMPED value -- a slider showing what it
  // asked for rather than what was stored is how a control drifts away from its own model
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, 4.0f, &m), DT_MASKS_OK);
  assert_true(f.rows[0].opacity == 1.0f);
  assert_true(m.opacity == 1.0f);

  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, -2.0f, &m), DT_MASKS_OK);
  assert_true(f.rows[0].opacity == 0.0f);

  // and re-setting the value it already holds is not a change: every shape's scrolled handler
  // treats this as "handled" but must not push an undo step for it
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, 0.0f, &m), DT_MASKS_UNCHANGED);

  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 999, 11, 0.5f, NULL), DT_MASKS_NOT_FOUND);
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 999, 0.5f, NULL), DT_MASKS_NOT_FOUND);
  assert_int_equal(dt_masks_group_set_member_opacity(NULL, 7, 11, 0.5f, NULL), DT_MASKS_INVALID);

  _write_fixture_cleanup(&f);
}

static void _set_opacity_refuses_a_nan_rather_than_clamping_it(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  /* CLAMPF() is a pair of ordered comparisons and every comparison against NaN is false, so
   * clamping a NaN yields the LOW bound: a NaN reaching this from a caller's own arithmetic would
   * silently blank the shape instead of being reported. */
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, NAN, NULL), DT_MASKS_INVALID);
  assert_true(f.rows[0].opacity == 0.5f);

  _write_fixture_cleanup(&f);
}

static void _a_shared_group_is_cloned_before_its_opacity_changes(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  /* Refcount 2 is what a history commit leaves behind: the dev's live form list holds one
   * reference and the frozen snapshot holds the other. It is the state an opacity slider is in from its SECOND step
   * onward, because each step commits history -- which is exactly why writing through a row
   * pointer resolved once, when the menu was built, could not stay correct. */
  dt_atomic_set_int(&f.group.refcount, 2);

  dt_masks_member_t m;
  assert_int_equal(dt_masks_group_set_member_opacity(&f.dev, 7, 11, 0.25f, &m), DT_MASKS_OK);

  // The snapshot still reads what it froze. This assertion IS the bug that motivated the API.
  assert_true(f.rows[0].opacity == 0.5f);

  // dev->forms carries the clone, and the clone carries the edit
  dt_masks_form_t *const live = (dt_masks_form_t *)f.dev.forms->data;
  assert_ptr_not_equal(live, &f.group);
  assert_true(m.opacity == 0.25f);

  dt_masks_member_t read_back;
  assert_int_equal(dt_masks_group_get_member(&f.dev, 7, 11, &read_back), DT_MASKS_OK);
  assert_true(read_back.opacity == 0.25f);

  dt_masks_form_unref(live);
  _write_fixture_cleanup(&f);
}

static void _find_holder_names_the_group_that_references_a_shape(void **state)
{
  (void)state;
  _write_fixture_t f;
  _write_fixture_init(&f);

  assert_int_equal(dt_masks_group_find_holder(&f.dev, 22), 7);
  assert_int_equal(dt_masks_group_find_holder(&f.dev, 999), 0);
  assert_int_equal(dt_masks_group_find_holder(&f.dev, 0), 0);
  assert_int_equal(dt_masks_group_find_holder(NULL, 22), 0);

  _write_fixture_cleanup(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_from_status_maps_zero_to_ok_and_everything_else_to_error),
    cmocka_unit_test(_a_shape_with_no_geometry_rasterises_empty),
    cmocka_unit_test(_a_shape_with_no_rasteriser_is_an_error),
    cmocka_unit_test(_an_unbuildable_outline_is_never_reported_as_built),
    cmocka_unit_test(_area_and_mask_always_write_their_out_parameters),
    cmocka_unit_test(_form_info_describes_a_shape),
    cmocka_unit_test(_form_info_leaves_out_untouched_on_failure),
    cmocka_unit_test(_copy_members_preserves_order_and_reports_the_total),
    cmocka_unit_test(_copy_members_refuses_anything_that_is_not_a_group),
    cmocka_unit_test(_type_tokens_are_the_persisted_conf_key_spellings),
    cmocka_unit_test(_set_operation_replaces_the_combine_op),
    cmocka_unit_test(_setting_the_state_it_already_has_is_unchanged),
    cmocka_unit_test(_inverse_toggles_and_leaves_the_operator_alone),
    cmocka_unit_test(_set_operation_rejects_what_it_cannot_do),
    cmocka_unit_test(_get_member_reads_a_row_by_identity),
    cmocka_unit_test(_set_opacity_clamps_and_reports_what_it_stored),
    cmocka_unit_test(_set_opacity_refuses_a_nan_rather_than_clamping_it),
    cmocka_unit_test(_a_shared_group_is_cloned_before_its_opacity_changes),
    cmocka_unit_test(_find_holder_names_the_group_that_references_a_shape),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
