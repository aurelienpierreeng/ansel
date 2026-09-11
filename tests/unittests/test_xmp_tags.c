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
#include "common/xmp_sidecar.h"
#include "metadata/exif.h"
#include "metadata/tags.h"
#include "test_xmp_fixture.h"

static dt_test_xmp_fixture_t fixture = { 0 };

static int setup(void **state)
{
  return dt_test_xmp_fixture_setup(state, &fixture, "tags");
}

static int teardown(void **state)
{
  return dt_test_xmp_fixture_teardown(state, &fixture);
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

  assert_int_equal(dt_exif_read(&image, fixture.xmp_path), 0);
  assert_tag_attached("fromembed", image.id, TRUE);
  assert_tag_attached("keepme2", image.id, TRUE);
}

static void test_overlong_keyword_rolls_back_tag_replacement(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-tags", "overlong.raw");
  seed_tag("keepme3", image.id);
  char *keyword = g_strnfill(1024, 'a');
  char *properties = g_strdup_printf("<dc:subject><rdf:Bag><rdf:li>%s</rdf:li></rdf:Bag></dc:subject>", keyword);
  dt_test_xmp_fixture_write_packet(&fixture, properties);

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 1);
  assert_tag_attached("keepme3", image.id, TRUE);

  dt_free(properties);
  dt_free(keyword);
}

int main(void)
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
    cmocka_unit_test(test_overlong_keyword_rolls_back_tag_replacement),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
