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

/** Contracts of database/preset_repository.h.
 *
 * The central one is the data-loss trap the migration had to step around:
 * `data.presets` is UNIQUE on (name, operation, op_version), so an `(operation, name)`
 * predicate is legitimately multi-row, and the version-only setter must not touch
 * `op_params` -- a setter that wrote both would stamp one preset's blob onto every other
 * version of the same name. dt_preset_repository_set_module_version() exists precisely so
 * dt_preset_repository_update_module_params() is never used for that.
 */

#include "testdb.h"


static const unsigned char params_a[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
static const unsigned char params_b[] = { 0x0a, 0x0b, 0x0c };

static void test_set_module_version_leaves_params_alone(void **state)
{
  (void)state;
  dt_preset_repository_add_module_preset("Q", "exposure", 1, params_a, sizeof(params_a));

  dt_module_preset_t *p = dt_preset_repository_get_module_preset("exposure", 1, "Q");
  assert_non_null(p);
  assert_int_equal(p->op_params_size, sizeof(params_a));
  assert_memory_equal(p->op_params, params_a, sizeof(params_a));
  dt_module_preset_free(p);

  // stamp a new version onto the row: the blob must survive untouched
  dt_preset_repository_set_module_version("exposure", "Q", 9);
  assert_null(dt_preset_repository_get_module_preset("exposure", 1, "Q"));
  p = dt_preset_repository_get_module_preset("exposure", 9, "Q");
  assert_non_null(p);
  assert_int_equal(p->op_params_size, sizeof(params_a));
  assert_memory_equal(p->op_params, params_a, sizeof(params_a));
  dt_module_preset_free(p);

  // the params setter, by contrast, replaces both blob and version
  dt_preset_repository_update_module_params("exposure", "Q", 10, params_b, sizeof(params_b));
  p = dt_preset_repository_get_module_preset("exposure", 10, "Q");
  assert_non_null(p);
  assert_int_equal(p->op_params_size, sizeof(params_b));
  assert_memory_equal(p->op_params, params_b, sizeof(params_b));
  dt_module_preset_free(p);
}

static void test_list_for_upgrade_returns_all_versions(void **state)
{
  (void)state;
  // the same name at two versions: two distinct rows under the UNIQUE constraint
  dt_preset_repository_add_module_preset("P", "sharpen", 1, params_a, sizeof(params_a));
  dt_preset_repository_add_module_preset("P", "sharpen", 2, params_b, sizeof(params_b));

  GList *rows = dt_preset_repository_list_for_upgrade("sharpen");
  assert_int_equal(g_list_length(rows), 2);

  int seen_v1 = 0, seen_v2 = 0;
  for(GList *l = rows; l; l = g_list_next(l))
  {
    const dt_module_preset_t *p = (const dt_module_preset_t *)l->data;
    assert_string_equal(p->name, "P");
    if(p->op_version == 1)
    {
      seen_v1++;
      assert_int_equal(p->op_params_size, sizeof(params_a));
      assert_memory_equal(p->op_params, params_a, sizeof(params_a));
    }
    if(p->op_version == 2)
    {
      seen_v2++;
      assert_int_equal(p->op_params_size, sizeof(params_b));
      assert_memory_equal(p->op_params, params_b, sizeof(params_b));
    }
  }
  assert_int_equal(seen_v1, 1);
  assert_int_equal(seen_v2, 1);
  g_list_free_full(rows, dt_module_preset_free);
}

static void test_list_editable_excludes_shipped(void **state)
{
  (void)state;
  dt_preset_repository_add_module_preset("mine", "borders", 1, params_a, sizeof(params_a));
  dt_preset_repository_add_shipped_preset("shipped", "borders", 1, params_b, sizeof(params_b), 1);

  GList *rows = dt_preset_repository_list_editable();
  int mine = 0, shipped = 0;
  for(GList *l = rows; l; l = g_list_next(l))
  {
    const dt_preset_identity_t *p = (const dt_preset_identity_t *)l->data;
    if(!g_strcmp0(p->operation, "borders"))
    {
      if(!g_strcmp0(p->name, "mine")) mine++;
      if(!g_strcmp0(p->name, "shipped")) shipped++;
      assert_true(p->rowid > 0);
    }
  }
  assert_int_equal(mine, 1);
  assert_int_equal(shipped, 0); // writeprotect = 1 must stay out of the editable list
  g_list_free_full(rows, dt_preset_identity_free);
}

static void test_list_all_carries_the_tree_row(void **state)
{
  (void)state;
  dt_preset_repository_add_module_preset("treerow", "vignette", 3, params_a, sizeof(params_a));

  GList *rows = dt_preset_repository_list_all();
  const dt_preset_row_t *found = NULL;
  for(GList *l = rows; l; l = g_list_next(l))
  {
    const dt_preset_row_t *p = (const dt_preset_row_t *)l->data;
    if(!g_strcmp0(p->operation, "vignette") && !g_strcmp0(p->name, "treerow")) found = p;
  }
  assert_non_null(found);
  assert_false(found->writeprotect);
  assert_false(found->autoapply); // a menu preset never auto-applies
  g_list_free_full(rows, dt_preset_row_free);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_set_module_version_leaves_params_alone),
    cmocka_unit_test(test_list_for_upgrade_returns_all_versions),
    cmocka_unit_test(test_list_editable_excludes_shipped),
    cmocka_unit_test(test_list_all_carries_the_tree_row),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
