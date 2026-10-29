/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

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

/** @file tests/unittests/test_xmp_tags.c
 *
 * @brief Regression for importing the photograph's keywords from XMP.
 *
 * @details Covers the tag half of dt_exif_decode_xmp_data(): reading tags must not be
 * gated on the sidecar-WRITE preference; the replace-the-user-tags wipe may only run when
 * the document actually carries a keyword bag; comma-separated and padded keywords in one
 * bag item must each land as their own tag; and embedded reads overlay instead of
 * replacing, the same rule #1273 fixed for colour labels.
 */

#include "testdb.h"

#include "common/image.h"
#include "common/undo.h"
#include "common/xmp_sidecar.h"
#include "database/database.h"
#include "darktable.h"
#include "metadata/exif.h"
#include "metadata/notify.h"
#include "metadata/tags.h"
#include "test_xmp_fixture.h"

#include <sqlite3.h>
#include <string.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_test_xmp_fixture_t fixture = { 0 };
static int tag_change_count = 0;

static int setup(void **state)
{
  if(dt_test_xmp_fixture_setup(state, &fixture, "tags")) return -1;
  darktable.undo = dt_undo_init();
  return IS_NULL_PTR(darktable.undo) ? -1 : 0;
}

static int teardown(void **state)
{
  dt_metadata_set_tags_changed_handler(NULL);
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  return dt_test_xmp_fixture_teardown(state, &fixture);
}

static void record_tag_change(void)
{
  tag_change_count++;
}

static guint seed_tag(const char *name, const int32_t imgid)
{
  guint tagid = 0;
  assert_true(dt_tag_new(name, &tagid));
  assert_true(tagid > 0);
  assert_true(dt_tag_attach(tagid, imgid, FALSE, FALSE));
  return tagid;
}

static void assert_tag_attached(const char *name, const int32_t imgid, const gboolean expected)
{
  const guint tagid = dt_tag_repository_find_by_name(name);
  if(expected)
  {
    assert_true(tagid > 0);
    assert_true(dt_tag_repository_is_attached(tagid, imgid));
  }
  else
  {
    if(tagid > 0) assert_false(dt_tag_repository_is_attached(tagid, imgid));
  }
}

static int64_t tag_position(const guint tagid, const int32_t imgid)
{
  sqlite3_stmt *statement = NULL;
  assert_int_equal(sqlite3_prepare_v2(dt_database_get_sqlite3_global(),
                                      "SELECT position FROM tagged_images WHERE tagid = ?1 AND imgid = ?2", -1,
                                      &statement, NULL), SQLITE_OK);
  assert_int_equal(sqlite3_bind_int(statement, 1, tagid), SQLITE_OK);
  assert_int_equal(sqlite3_bind_int(statement, 2, imgid), SQLITE_OK);
  assert_int_equal(sqlite3_step(statement), SQLITE_ROW);
  const int64_t position = sqlite3_column_int64(statement, 0);
  assert_int_equal(sqlite3_finalize(statement), SQLITE_OK);
  return position;
}

static void test_full_sidecar_read_imports_tags_without_sidecar_writing(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "import.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag>"
                                             "<rdf:li>alpha</rdf:li><rdf:li>beta</rdf:li>"
                                             "</rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("alpha", image.id, TRUE);
  assert_tag_attached("beta", image.id, TRUE);
}

static void test_keywordless_xmp_preserves_existing_tags(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "preserve.raw");
  seed_tag("keepme", image.id);
  dt_test_xmp_fixture_write_packet(&fixture, "");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("keepme", image.id, TRUE);
}

static void test_xmp_replaces_user_tags_and_keeps_internal(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "replace.raw");
  const guint internal = seed_tag("darktable|format|replaced", image.id);
  seed_tag("oldtag", image.id);
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag><rdf:li>newtag</rdf:li></rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("newtag", image.id, TRUE);
  assert_tag_attached("oldtag", image.id, FALSE);
  assert_true(dt_tag_repository_is_attached(internal, image.id));
}

static void test_comma_separated_and_padded_keywords(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "commas.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag>"
                                             "<rdf:li>gamma, delta</rdf:li><rdf:li>epsilon,</rdf:li>"
                                             "</rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("gamma", image.id, TRUE);
  assert_tag_attached("delta", image.id, TRUE);
  assert_tag_attached("epsilon", image.id, TRUE);
  assert_tag_attached(" delta", image.id, FALSE);
  assert_int_equal(dt_tag_repository_count_attachments(dt_tag_repository_find_by_name("gamma")), 1);
  assert_int_equal(dt_tag_repository_count_attachments(dt_tag_repository_find_by_name("delta")), 1);
}

static void test_empty_tokens_do_not_corrupt(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "empties.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag>"
                                             "<rdf:li>,zeta</rdf:li><rdf:li>   ,</rdf:li>"
                                             "</rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("zeta", image.id, TRUE);
  assert_int_equal(dt_tag_repository_find_by_name(""), 0);
  assert_false(dt_tag_repository_is_attached((guint)-1, image.id));
}

static void test_repeated_reads_are_stable(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "repeat.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag>"
                                             "<rdf:li>eta</rdf:li><rdf:li>theta</rdf:li>"
                                             "</rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("eta", image.id, TRUE);
  assert_tag_attached("theta", image.id, TRUE);
  assert_int_equal(dt_tag_repository_count_attachments(dt_tag_repository_find_by_name("eta")), 1);
}

static void test_hierarchical_subject_takes_precedence(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "hier.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<lr:hierarchicalSubject><rdf:Bag>"
                                             "<rdf:li>Scapes|Dunes</rdf:li></rdf:Bag></lr:hierarchicalSubject>"
                                             "<dc:subject><rdf:Bag><rdf:li>iignored</rdf:li></rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("Scapes|Dunes", image.id, TRUE);
  assert_tag_attached("iignored", image.id, FALSE);
}

static void test_empty_hierarchical_subject_clears_user_tags(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "empty-hierarchical.raw");
  seed_tag("oldtag", image.id);
  dt_test_xmp_fixture_write_packet(&fixture, "<lr:hierarchicalSubject><rdf:Bag/></lr:hierarchicalSubject>"
                                             "<dc:subject><rdf:Bag><rdf:li>ignored</rdf:li></rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached("oldtag", image.id, FALSE);
  assert_tag_attached("ignored", image.id, FALSE);
}

static void test_embedded_read_overlays_without_wipe(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "embedded.raw");
  seed_tag("keepme2", image.id);
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag><rdf:li>fromembed</rdf:li></rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_read_persisted(&image, fixture.xmp_path), 0);
  assert_tag_attached("fromembed", image.id, TRUE);
  assert_tag_attached("keepme2", image.id, TRUE);
}

static void test_arbitrarily_long_keyword_replaces_existing_tag(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "overlong.raw");
  seed_tag("keepme3", image.id);
  char *keyword = g_strnfill(4096, 'a');
  char *properties = g_strdup_printf("<dc:subject><rdf:Bag><rdf:li>%s</rdf:li></rdf:Bag></dc:subject>", keyword);
  dt_test_xmp_fixture_write_packet(&fixture, properties);

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_tag_attached(keyword, image.id, TRUE);
  assert_tag_attached("keepme3", image.id, FALSE);

  dt_free(properties);
  dt_free(keyword);
}

static void test_tag_replacement_rolls_back_when_later_attachment_fails(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "rollback.raw");
  seed_tag("keep", image.id);
  guint rejected = 0;
  assert_true(dt_tag_new("second", &rejected));
  char *trigger = g_strdup_printf(
      "CREATE TRIGGER reject_second_tag BEFORE INSERT ON tagged_images "
      "WHEN NEW.tagid = %u BEGIN SELECT RAISE(ABORT, 'second tag rejected'); END", rejected);
  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), trigger, NULL, NULL, NULL), SQLITE_OK);
  dt_test_xmp_fixture_write_packet(&fixture, "<dc:subject><rdf:Bag>"
                                             "<rdf:li>first</rdf:li><rdf:li>second</rdf:li>"
                                             "</rdf:Bag></dc:subject>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), DT_XMP_IMPORT_PERSIST_ERROR);
  assert_tag_attached("keep", image.id, TRUE);
  assert_tag_attached("first", image.id, FALSE);
  assert_tag_attached("second", image.id, FALSE);

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), "DROP TRIGGER reject_second_tag", NULL, NULL, NULL), SQLITE_OK);
  dt_free(trigger);
}

static void test_batch_attachment_rolls_back_all_images_when_later_insert_fails(void **state)
{
  (void)state;
  dt_image_t first = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-batch", "first.raw");
  dt_image_t second = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-batch", "second.raw");
  const guint existing = seed_tag("existing", first.id);
  assert_true(dt_tag_attach(existing, second.id, FALSE, FALSE));
  const int64_t first_position = tag_position(existing, first.id);
  const int64_t second_position = tag_position(existing, second.id);
  guint rejected = 0;
  assert_true(dt_tag_new("rejected", &rejected));
  char *trigger = g_strdup_printf(
      "CREATE TRIGGER reject_later_batch_tag BEFORE INSERT ON tagged_images "
      "WHEN NEW.tagid = %u AND NEW.imgid = %d BEGIN SELECT RAISE(ABORT, 'batch tag rejected'); END", rejected,
      second.id);
  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), trigger, NULL, NULL, NULL), SQLITE_OK);
  GList *images = g_list_append(NULL, GINT_TO_POINTER(first.id));
  images = g_list_append(images, GINT_TO_POINTER(second.id));
  tag_change_count = 0;
  const int undo_count = dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS);
  dt_metadata_set_tags_changed_handler(record_tag_change);

  assert_false(dt_tag_attach_images(rejected, images, TRUE));
  assert_true(dt_tag_repository_is_attached(existing, first.id));
  assert_true(dt_tag_repository_is_attached(existing, second.id));
  assert_false(dt_tag_repository_is_attached(rejected, first.id));
  assert_false(dt_tag_repository_is_attached(rejected, second.id));
  assert_true(tag_position(existing, first.id) == first_position);
  assert_true(tag_position(existing, second.id) == second_position);
  assert_int_equal(dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS), undo_count);
  assert_int_equal(tag_change_count, 0);

  g_list_free(images);
  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), "DROP TRIGGER reject_later_batch_tag", NULL, NULL, NULL), SQLITE_OK);
  dt_free(trigger);
}

static void test_successful_batch_attachment_records_group_and_action_undo(void **state)
{
  (void)state;
  dt_image_t first = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-success", "first.raw");
  dt_image_t second = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-success", "second.raw");
  guint tagid = 0;
  assert_true(dt_tag_new("batch", &tagid));
  GList *images = g_list_append(NULL, GINT_TO_POINTER(first.id));
  images = g_list_append(images, GINT_TO_POINTER(second.id));
  tag_change_count = 0;
  const int undo_count = dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS);
  dt_metadata_set_tags_changed_handler(record_tag_change);

  assert_true(dt_tag_attach_images(tagid, images, TRUE));
  assert_true(dt_tag_repository_is_attached(tagid, first.id));
  assert_true(dt_tag_repository_is_attached(tagid, second.id));
  assert_int_equal(dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS), undo_count + 3);
  assert_int_equal(tag_change_count, 0);

  g_list_free(images);
}

static void test_noop_batch_attachment_records_no_undo_or_notification(void **state)
{
  (void)state;
  dt_image_t first = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-noop", "first.raw");
  dt_image_t second = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-noop", "second.raw");
  const guint tagid = seed_tag("already-attached", first.id);
  assert_true(dt_tag_attach(tagid, second.id, FALSE, FALSE));
  GList *images = g_list_append(NULL, GINT_TO_POINTER(first.id));
  images = g_list_append(images, GINT_TO_POINTER(second.id));
  tag_change_count = 0;
  const int undo_count = dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS);
  dt_metadata_set_tags_changed_handler(record_tag_change);

  assert_false(dt_tag_attach_images(tagid, images, TRUE));
  assert_int_equal(dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS), undo_count);
  assert_int_equal(tag_change_count, 0);

  g_list_free(images);
}

static void test_batch_attachment_rejects_undo_inside_caller_transaction(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags-nested", "nested.raw");
  guint tagid = 0;
  assert_true(dt_tag_new("nested-undo", &tagid));
  GList *images = g_list_append(NULL, GINT_TO_POINTER(image.id));
  tag_change_count = 0;
  const int undo_count = dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS);
  dt_metadata_set_tags_changed_handler(record_tag_change);
  assert_true(dt_database_start_transaction());

  assert_false(dt_tag_attach_images(tagid, images, TRUE));
  assert_false(dt_tag_repository_is_attached(tagid, image.id));
  assert_int_equal(dt_undo_list_length(dt_undo_get_global(), DT_UNDO_TAGS), undo_count);
  assert_int_equal(tag_change_count, 0);
  assert_true(dt_database_transaction_is_owned_by_current_thread());
  assert_true(dt_database_release_transaction());

  g_list_free(images);
}

static void test_sidecar_write_preserves_double_digit_structured_array_order(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "regions.raw");
  dt_test_xmp_fixture_write_packet(
      &fixture,
      "<MP:RegionInfo xmlns:MP=\"http://ns.microsoft.com/photo/1.2/\" "
      "xmlns:MPRI=\"http://ns.microsoft.com/photo/1.2/t/RegionInfo#\" "
      "xmlns:MPReg=\"http://ns.microsoft.com/photo/1.2/t/Region#\" rdf:parseType=\"Resource\">"
      "<MPRI:Regions><rdf:Bag>"
      "<rdf:li MPReg:PersonDisplayName=\"face-01\"/><rdf:li MPReg:PersonDisplayName=\"face-02\"/>"
      "<rdf:li MPReg:PersonDisplayName=\"face-03\"/><rdf:li MPReg:PersonDisplayName=\"face-04\"/>"
      "<rdf:li MPReg:PersonDisplayName=\"face-05\"/><rdf:li MPReg:PersonDisplayName=\"face-06\"/>"
      "<rdf:li MPReg:PersonDisplayName=\"face-07\"/><rdf:li MPReg:PersonDisplayName=\"face-08\"/>"
      "<rdf:li MPReg:PersonDisplayName=\"face-09\"/><rdf:li MPReg:PersonDisplayName=\"face-10\"/>"
      "<rdf:li MPReg:PersonDisplayName=\"face-11\"/><rdf:li MPReg:PersonDisplayName=\"face-12\"/>"
      "</rdf:Bag></MPRI:Regions></MP:RegionInfo>");

  assert_int_equal(dt_exif_xmp_write_with_imgpath(&image, fixture.xmp_path, fixture.xmp_path), 0);

  gchar *rewritten = NULL;
  assert_true(g_file_get_contents(fixture.xmp_path, &rewritten, NULL, NULL));
  const gchar *next_face = rewritten;
  for(int face = 1; face <= 12; face++)
  {
    gchar *name = g_strdup_printf("face-%02d", face);
    next_face = g_strstr_len(next_face, -1, name);
    assert_non_null(next_face);
    next_face += strlen(name);
    dt_free(name);
  }
  dt_free(rewritten);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_full_sidecar_read_imports_tags_without_sidecar_writing),
    cmocka_unit_test(test_keywordless_xmp_preserves_existing_tags),
    cmocka_unit_test(test_xmp_replaces_user_tags_and_keeps_internal),
    cmocka_unit_test(test_comma_separated_and_padded_keywords),
    cmocka_unit_test(test_empty_tokens_do_not_corrupt),
    cmocka_unit_test(test_repeated_reads_are_stable),
    cmocka_unit_test(test_hierarchical_subject_takes_precedence),
    cmocka_unit_test(test_empty_hierarchical_subject_clears_user_tags),
    cmocka_unit_test(test_embedded_read_overlays_without_wipe),
    cmocka_unit_test(test_arbitrarily_long_keyword_replaces_existing_tag),
    cmocka_unit_test(test_tag_replacement_rolls_back_when_later_attachment_fails),
    cmocka_unit_test(test_batch_attachment_rolls_back_all_images_when_later_insert_fails),
    cmocka_unit_test(test_successful_batch_attachment_records_group_and_action_undo),
    cmocka_unit_test(test_noop_batch_attachment_records_no_undo_or_notification),
    cmocka_unit_test(test_batch_attachment_rejects_undo_inside_caller_transaction),
    cmocka_unit_test(test_sidecar_write_preserves_double_digit_structured_array_order),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
