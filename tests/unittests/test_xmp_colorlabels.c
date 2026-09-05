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

#include "testdb.h"

#include "common/conf.h"
#include "common/image.h"
#include "common/xmp_sidecar.h"
#include "darktable.h"
#include "metadata/colorlabels.h"
#include "metadata/exif.h"

#include <glib/gstdio.h>
#include <stdlib.h>

static char *config_path = NULL;
static char *xmp_path = NULL;

static int setup(void **state)
{
  if(testdb_setup(state)) return -1;

  const int config_fd = g_file_open_tmp("ansel-test-xmp-colorlabels-XXXXXX.rc", &config_path, NULL);
  if(config_fd < 0) return -1;
  g_close(config_fd, NULL);
  g_remove(config_path);

  darktable.conf = calloc(1, sizeof(dt_conf_t));
  if(IS_NULL_PTR(darktable.conf)) return -1;

  dt_conf_init(darktable.conf, config_path, NULL);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  dt_conf_set_bool("ui_last/import_last_tags_imported", FALSE);
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();

  GError *error = NULL;
  const int fd = g_file_open_tmp("ansel-test-xmp-colorlabels-XXXXXX.xmp", &xmp_path, &error);
  if(fd < 0)
  {
    g_clear_error(&error);
    return -1;
  }
  g_close(fd, NULL);
  return 0;
}

static int teardown(void **state)
{
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(config_path);
  dt_free(config_path);
  config_path = NULL;
  g_remove(xmp_path);
  dt_free(xmp_path);
  xmp_path = NULL;
  return testdb_teardown(state);
}

static void write_xmp(const char *properties)
{
  char *packet = g_strdup_printf(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
      "xmlns:darktable=\"http://darktable.sf.net/\" darktable:xmp_version=\"0\">%s"
      "</rdf:Description></rdf:RDF></x:xmpmeta>",
      properties);
  assert_true(g_file_set_contents(xmp_path, packet, -1, NULL));
  dt_free(packet);
}

static dt_image_t make_image(const char *filename)
{
  const int32_t film = testdb_make_film("/testdb/xmp-colorlabels");
  const int32_t imgid = testdb_make_image(film, filename);
  assert_true(imgid > 0);

  dt_image_t image;
  dt_image_init(&image);
  assert_true(dt_image_repository_load(imgid, &image));
  return image;
}

static void test_full_xmp_read_replaces_persisted_color_labels(void **state)
{
  (void)state;
  dt_image_t image = make_image("replace.raw");
  image.color_labels = 1 << DT_COLORLABELS_RED;
  dt_image_repository_store(&image);

  write_xmp("<darktable:colorlabels><rdf:Seq>"
            "<rdf:li>1</rdf:li><rdf:li>3</rdf:li><rdf:li>-1</rdf:li><rdf:li>5</rdf:li>"
            "</rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE), 0);
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
  dt_image_t image = make_image("precedence.raw");
  write_xmp("<xmp:Label>Green</xmp:Label>"
            "<darktable:colorlabels><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE), 0);
  assert_int_equal(image.color_labels, 1 << DT_COLORLABELS_GREEN);
}

static void test_absent_xmp_labels_clear_existing_labels(void **state)
{
  (void)state;
  dt_image_t image = make_image("clear.raw");
  image.color_labels = 1 << DT_COLORLABELS_PURPLE;
  dt_image_repository_store(&image);
  write_xmp("");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE), 0);
  assert_int_equal(image.color_labels, 0);
  dt_image_repository_store(&image);
  assert_int_equal(dt_colorlabel_repository_get(image.id), 0);
}

static void test_embedded_xmp_labels_extend_existing_labels(void **state)
{
  (void)state;
  dt_image_t image = make_image("embedded.raw");
  image.color_labels = 1 << DT_COLORLABELS_RED;
  write_xmp("<darktable:colorlabels><rdf:Seq><rdf:li>2</rdf:li></rdf:Seq></darktable:colorlabels>");

  assert_int_equal(dt_exif_read(&image, xmp_path), 0);
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
