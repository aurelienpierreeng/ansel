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

#include "testdb.h"

#include "common/conf.h"
#include "common/image.h"
#include "common/xmp_sidecar.h"
#include "metadata/colorlabels.h"
#include "metadata/exif.h"
#include "test_xmp_fixture.h"

static dt_test_xmp_fixture_t fixture = { 0 };

static int setup(void **state)
{
  if(dt_test_xmp_fixture_setup(state, &fixture, "colorlabels")) return -1;
  dt_conf_set_bool("ui_last/import_last_tags_imported", FALSE);
  return 0;
}

static int teardown(void **state)
{
  return dt_test_xmp_fixture_teardown(state, &fixture);
}

static void test_full_xmp_read_replaces_persisted_color_labels(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-colorlabels", "replace.raw");
  image.color_labels = 1 << DT_COLORLABELS_RED;
  dt_image_repository_store(&image);

  dt_test_xmp_fixture_write_packet(&fixture, "<darktable:colorlabels><rdf:Seq>"
                                             "<rdf:li>1</rdf:li><rdf:li>3</rdf:li><rdf:li>-1</rdf:li><rdf:li>5</rdf:li>"
                                             "</rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_int_equal(image.color_labels, (1 << DT_COLORLABELS_YELLOW) | (1 << DT_COLORLABELS_BLUE));

  dt_image_repository_store(&image);
  assert_int_equal(dt_colorlabel_repository_get(image.id), image.color_labels);

  dt_image_t reloaded;
  dt_image_init(&reloaded);
  assert_true(dt_image_repository_load(image.id, &reloaded));
  assert_int_equal(reloaded.color_labels, image.color_labels);
}

static void test_standard_xmp_label_takes_precedence(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-colorlabels", "precedence.raw");
  dt_test_xmp_fixture_write_packet(&fixture, "<xmp:Label>Green</xmp:Label>"
                                             "<darktable:colorlabels><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_int_equal(image.color_labels, 1 << DT_COLORLABELS_GREEN);
}

static void test_absent_xmp_labels_clear_existing_labels(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-colorlabels", "clear.raw");
  image.color_labels = 1 << DT_COLORLABELS_PURPLE;
  dt_image_repository_store(&image);
  dt_test_xmp_fixture_write_packet(&fixture, "");

  assert_int_equal(dt_exif_xmp_read(&image, fixture.xmp_path, FALSE, NULL), 0);
  assert_int_equal(image.color_labels, 0);
  dt_image_repository_store(&image);
  assert_int_equal(dt_colorlabel_repository_get(image.id), 0);
}

static void test_embedded_xmp_labels_extend_existing_labels(void **state)
{
  (void)state;
  dt_image_t image = dt_test_xmp_fixture_make_image("/testdb/xmp-colorlabels", "embedded.raw");
  image.color_labels = 1 << DT_COLORLABELS_RED;
  dt_test_xmp_fixture_write_packet(&fixture, "<darktable:colorlabels><rdf:Seq><rdf:li>2</rdf:li></rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_read(&image, fixture.xmp_path), 0);
  assert_int_equal(image.color_labels, (1 << DT_COLORLABELS_RED) | (1 << DT_COLORLABELS_GREEN));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_full_xmp_read_replaces_persisted_color_labels),
    cmocka_unit_test(test_standard_xmp_label_takes_precedence),
    cmocka_unit_test(test_absent_xmp_labels_clear_existing_labels),
    cmocka_unit_test(test_embedded_xmp_labels_extend_existing_labels),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
