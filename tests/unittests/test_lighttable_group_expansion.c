/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"
#include "gui/dtgtk/thumbtable.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_group_member_lookup_excludes_requested_member(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/lighttable-expansion");
  const int32_t representative = testdb_make_image(film_id, "representative.raw");
  const int32_t member = testdb_make_image(film_id, "member.raw");
  assert_true(representative > 0);
  assert_true(member > 0);
  assert_true(dt_image_repository_set_group(representative, representative));
  assert_true(dt_image_repository_set_group(member, representative));
  GList *members = dt_image_repository_get_group_members(representative, representative);
  assert_int_equal(g_list_length(members), 1);
  assert_int_equal(GPOINTER_TO_INT(members->data), member);
  g_list_free(members);
}

static void test_clearing_lighttable_expansion_keeps_filmstrip_inert(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t lighttable = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .expanded_group_id = 10 };
  dt_thumbtable_t filmstrip = { .mode = DT_THUMBTABLE_MODE_FILMSTRIP, .expanded_group_id = UNKNOWN_IMAGE };

  dt_thumbtable_clear_expanded_group(&lighttable);
  dt_thumbtable_clear_expanded_group(&filmstrip);

  assert_int_equal(lighttable.expanded_group_id, UNKNOWN_IMAGE);
  assert_int_equal(filmstrip.expanded_group_id, UNKNOWN_IMAGE);
}

static void test_collection_changes_clear_expansion_except_sorting(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .expanded_group_id = 10 };

  assert_false(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, TRUE, TRUE));
  assert_int_equal(table.expanded_group_id, 10);
  assert_true(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_QUERY, TRUE, TRUE));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);

  table.expanded_group_id = 10;
  assert_true(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING, FALSE, TRUE));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);

  table.expanded_group_id = 10;
  assert_false(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_GROUP_REPRESENTATIVE, DT_COLLECTION_PROP_GROUPING, TRUE, TRUE));
  assert_int_equal(table.expanded_group_id, 10);
  assert_true(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_GROUP_REPRESENTATIVE, DT_COLLECTION_PROP_GROUPING, FALSE, FALSE));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);

  table.expanded_group_id = 10;
  assert_true(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, FALSE, FALSE));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);

  table.mode = DT_THUMBTABLE_MODE_FILMSTRIP;
  table.expanded_group_id = 10;
  assert_false(dt_thumbtable_update_expanded_group_for_collection_change(
      &table, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_QUERY, TRUE, TRUE));
  assert_int_equal(table.expanded_group_id, 10);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_group_member_lookup_excludes_requested_member),
    cmocka_unit_test(test_clearing_lighttable_expansion_keeps_filmstrip_inert),
    cmocka_unit_test(test_collection_changes_clear_expansion_except_sorting)
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
