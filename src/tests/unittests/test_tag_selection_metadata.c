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

/** Contracts of the selection, tag and metadata repositories that the metadata panels
 * lean on: which image is "first", whether a set of images agrees on its tags, and the
 * one-pass distinct-value walk over the selection. */

#include "testdb.h"

#include "common/image.h" // UNKNOWN_IMAGE
#include "common/tags.h"  // DT_TF_CATEGORY


static void test_selection_lowest_id(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/sel");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  const int32_t c = testdb_make_image(film, "c.raw");
  assert_true(a > 0 && b > a && c > b);

  // nothing selected: the sentinel, not a garbage id
  dt_selection_repository_clear();
  assert_int_equal(dt_selection_repository_get_lowest_id(), UNKNOWN_IMAGE);

  // selection order must not matter: the answer is the LOWEST id, which the unordered
  // scan this replaced produced only because imgid is the table's INTEGER PRIMARY KEY
  dt_selection_repository_select(c);
  dt_selection_repository_select(a);
  dt_selection_repository_select(b);
  assert_int_equal(dt_selection_repository_get_lowest_id(), a);

  // get_all comes back ascending, whatever order the ids went in
  GList *all = dt_selection_repository_get_all();
  assert_int_equal(g_list_length(all), 3);
  assert_int_equal(GPOINTER_TO_INT(all->data), a);
  assert_int_equal(GPOINTER_TO_INT(all->next->data), b);
  assert_int_equal(GPOINTER_TO_INT(all->next->next->data), c);
  g_list_free(all);

  dt_selection_repository_deselect(a);
  assert_int_equal(dt_selection_repository_get_lowest_id(), b);
  dt_selection_repository_clear();
}

static void test_tag_agreement(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/tags");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(a > 0 && b > 0);
  GList *both = g_list_append(g_list_append(NULL, GINT_TO_POINTER(a)), GINT_TO_POINTER(b));

  const guint shared = dt_tag_repository_insert("alpha");
  const guint only_a = dt_tag_repository_insert("beta");
  const guint category = dt_tag_repository_insert("group|c");
  assert_true(shared > 0 && only_a > 0 && category > 0);
  dt_tag_repository_set_flags(category, DT_TF_CATEGORY);

  // a tag on only one image breaks tag agreement; the shared category keeps its own
  assert_true(dt_tag_repository_attach(shared, a));
  assert_true(dt_tag_repository_attach(shared, b));
  assert_true(dt_tag_repository_attach(only_a, a));
  assert_true(dt_tag_repository_attach(category, a));
  assert_true(dt_tag_repository_attach(category, b));

  gboolean same_tags = FALSE, same_categories = FALSE;
  dt_tag_repository_get_agreement(both, &same_tags, &same_categories);
  assert_false(same_tags);
  assert_true(same_categories);

  // complete the pair and both agree
  assert_true(dt_tag_repository_attach(only_a, b));
  dt_tag_repository_get_agreement(both, &same_tags, &same_categories);
  assert_true(same_tags);
  assert_true(same_categories);

  // the darktable| namespace is bookkeeping, not a keyword: it must not break agreement
  const guint internal = dt_tag_repository_insert("darktable|style|x");
  assert_true(dt_tag_repository_attach(internal, a));
  dt_tag_repository_get_agreement(both, &same_tags, &same_categories);
  assert_true(same_tags);
  assert_true(same_categories);

  // the empty set agrees vacuously
  dt_tag_repository_get_agreement(NULL, &same_tags, &same_categories);
  assert_true(same_tags);
  assert_true(same_categories);

  g_list_free(both);
}

typedef struct _meta_collect_t
{
  int rows;
  int shared_key_count;   /**< the count reported for the value both images carry */
  int split_key_rows;     /**< how many distinct values the disagreeing key produced */
} _meta_collect_t;

static void _collect_meta(void *user_data, const int keyid, const char *value, const uint32_t count)
{
  _meta_collect_t *c = (_meta_collect_t *)user_data;
  c->rows++;
  if(keyid == 0 && !g_strcmp0(value, "same-creator")) c->shared_key_count = (int)count;
  if(keyid == 2) c->split_key_rows++;
}

static void test_metadata_foreach_selected(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/meta");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(a > 0 && b > 0);

  const dt_metadata_row_t rows[] = {
    { .imgid = a, .keyid = 0, .value = "same-creator" },
    { .imgid = b, .keyid = 0, .value = "same-creator" },
    { .imgid = a, .keyid = 2, .value = "title-a" },
    { .imgid = b, .keyid = 2, .value = "title-b" },
  };
  dt_metadata_repository_add(rows, 4);

  dt_selection_repository_clear();
  dt_selection_repository_select(a);
  dt_selection_repository_select(b);

  _meta_collect_t c = { 0 };
  dt_metadata_repository_foreach_selected(_collect_meta, &c);
  assert_int_equal(c.rows, 3);             // one shared value + two split values
  assert_int_equal(c.shared_key_count, 2); // both selected images carry it -> they agree
  assert_int_equal(c.split_key_rows, 2);   // two values at count 1 -> they disagree

  dt_selection_repository_clear();
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_selection_lowest_id),
    cmocka_unit_test(test_tag_agreement),
    cmocka_unit_test(test_metadata_foreach_selected),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
