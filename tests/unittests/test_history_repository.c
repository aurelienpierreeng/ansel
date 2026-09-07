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

/** Contracts of database/history_repository.h: the main.history item cycle, the
 * main.module_order row (whose ABSENCE is a meaningful state -- "nobody chose"), and the
 * blob-identity version probe the startup preset upgrade relies on. */

#include "testdb.h"


static const unsigned char params_a[] = { 0xde, 0xad, 0xbe, 0xef, 0x01 };
static const unsigned char params_b[] = { 0xca, 0xfe, 0xba, 0xbe };
static const unsigned char blend_a[] = { 0x42, 0x42 };

static void test_module_order_absent_vs_zero(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/order");
  const int32_t img = testdb_make_image(film, "a.raw");
  assert_true(img > 0);

  // no row at all: FALSE, and the out-parameter is left alone -- "no row" must stay
  // distinguishable from "row with version 0"
  int version = 12345;
  assert_false(dt_history_repository_get_module_order_version(img, &version));
  assert_int_equal(version, 12345);
  assert_false(dt_history_repository_has_module_order(img));
  assert_false(dt_history_repository_has_custom_module_order(img));

  // a built-in order: version only, no serialized list
  assert_true(dt_history_repository_set_module_order(img, 5, NULL));
  assert_true(dt_history_repository_get_module_order_version(img, &version));
  assert_int_equal(version, 5);
  assert_true(dt_history_repository_has_module_order(img));
  assert_false(dt_history_repository_has_custom_module_order(img));

  // a custom order carries its list, copied out of the statement
  assert_true(dt_history_repository_set_module_order(img, 7, "rawprepare,0,exposure,0"));
  dt_module_order_row_t row = { 0 };
  assert_true(dt_history_repository_get_module_order(img, &row));
  assert_int_equal(row.version, 7);
  assert_non_null(row.iop_list);
  assert_string_equal(row.iop_list, "rawprepare,0,exposure,0");
  dt_module_order_row_cleanup(&row);
  assert_true(dt_history_repository_has_custom_module_order(img));

  // and back to a built-in: the list column returns to NULL
  assert_true(dt_history_repository_set_module_order(img, 5, NULL));
  assert_false(dt_history_repository_has_custom_module_order(img));
}

static void test_history_item_cycle(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/items");
  const int32_t img = testdb_make_image(film, "a.raw");
  assert_true(img > 0);

  assert_int_equal(dt_history_repository_count_items(img), 0);
  assert_int_equal(dt_history_repository_get_next_num(img), 0);
  assert_false(dt_history_repository_module_exists(img, "exposure"));

  assert_true(dt_history_repository_write_item(img, 0, "exposure", params_a, sizeof(params_a), 3,
                                               TRUE, blend_a, sizeof(blend_a), 11, 0, ""));
  assert_int_equal(dt_history_repository_count_items(img), 1);
  assert_int_equal(dt_history_repository_get_next_num(img), 1);
  assert_true(dt_history_repository_module_exists(img, "exposure"));
  assert_false(dt_history_repository_module_exists(img, "filmic"));

  assert_true(dt_history_repository_set_end(img, 1));
  assert_int_equal(dt_history_repository_get_end(img), 1);
}

static void test_find_version_for_params(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/probe");
  const int32_t img = testdb_make_image(film, "a.raw");
  assert_true(img > 0);
  assert_true(dt_history_repository_write_item(img, 0, "sharpen", params_a, sizeof(params_a), 4,
                                               TRUE, blend_a, sizeof(blend_a), 11, 0, ""));

  // byte-identical params date the preset; anything else finds nothing
  assert_int_equal(dt_history_repository_find_version_for_params("sharpen", params_a, sizeof(params_a)), 4);
  assert_int_equal(dt_history_repository_find_version_for_params("sharpen", params_b, sizeof(params_b)), 0);
  // the group fixture shares one database, so pick an operation NO other test writes --
  // "exposure" exists here, courtesy of test_history_item_cycle
  assert_int_equal(dt_history_repository_find_version_for_params("filmic", params_a, sizeof(params_a)), 0);
}

typedef struct _active_collect_t
{
  int count;
  int first_num;
  char op[32];
} _active_collect_t;

static void _collect_active(void *user_data, const int num, const char *operation, const char *multi_name)
{
  (void)multi_name;
  _active_collect_t *c = (_active_collect_t *)user_data;
  if(c->count == 0)
  {
    c->first_num = num;
    g_strlcpy(c->op, operation, sizeof(c->op));
  }
  c->count++;
}

static void test_foreach_active_module(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/active");
  const int32_t img = testdb_make_image(film, "a.raw");
  assert_true(img > 0);

  // the same module twice (grouped to one entry at its LOWEST num), plus a disabled one
  // (not listed at all)
  assert_true(dt_history_repository_write_item(img, 0, "exposure", params_a, sizeof(params_a), 3,
                                               TRUE, blend_a, sizeof(blend_a), 11, 0, ""));
  assert_true(dt_history_repository_write_item(img, 1, "exposure", params_b, sizeof(params_b), 3,
                                               TRUE, blend_a, sizeof(blend_a), 11, 0, ""));
  assert_true(dt_history_repository_write_item(img, 2, "borders", params_a, sizeof(params_a), 1,
                                               FALSE, blend_a, sizeof(blend_a), 11, 0, ""));

  _active_collect_t c = { 0 };
  dt_history_repository_foreach_active_module(img, _collect_active, &c);
  assert_int_equal(c.count, 1);
  assert_int_equal(c.first_num, 0);
  assert_string_equal(c.op, "exposure");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_module_order_absent_vs_zero),
    cmocka_unit_test(test_history_item_cycle),
    cmocka_unit_test(test_find_version_for_params),
    cmocka_unit_test(test_foreach_active_module),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
