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

#include "test_xmp_fixture.h"

#include "testdb.h"

#include "common/conf.h"
#include "common/xmp_sidecar.h"
#include "darktable.h"
#include "metadata/exif.h"

#include <glib/gstdio.h>
#include <stdlib.h>

int dt_test_xmp_fixture_setup(void **state, dt_test_xmp_fixture_t *fixture, const char *name)
{
  if(testdb_setup(state)) return -1;

  char *config_template = g_strdup_printf("ansel-test-xmp-%s-XXXXXX.rc", name);
  if(IS_NULL_PTR(config_template)) goto cleanup_database;

  const int config_fd = g_file_open_tmp(config_template, &fixture->config_path, NULL);
  dt_free(config_template);
  if(config_fd < 0) goto cleanup_database;
  g_close(config_fd, NULL);
  g_remove(fixture->config_path);

  darktable.conf = calloc(1, sizeof(dt_conf_t));
  if(IS_NULL_PTR(darktable.conf)) goto cleanup_config;

  dt_conf_init(darktable.conf, fixture->config_path, NULL);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();

  char *xmp_template = g_strdup_printf("ansel-test-xmp-%s-XXXXXX.xmp", name);
  if(IS_NULL_PTR(xmp_template)) goto cleanup_exif;

  const int xmp_fd = g_file_open_tmp(xmp_template, &fixture->xmp_path, NULL);
  dt_free(xmp_template);
  if(xmp_fd < 0) goto cleanup_xmp;
  g_close(xmp_fd, NULL);
  return 0;

cleanup_xmp:
  dt_free(fixture->xmp_path);
  fixture->xmp_path = NULL;
cleanup_exif:
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = NULL;
cleanup_config:
  g_remove(fixture->config_path);
  dt_free(fixture->config_path);
  fixture->config_path = NULL;
cleanup_database:
  testdb_teardown(state);
  return -1;
}

int dt_test_xmp_fixture_teardown(void **state, dt_test_xmp_fixture_t *fixture)
{
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = NULL;
  g_remove(fixture->config_path);
  dt_free(fixture->config_path);
  fixture->config_path = NULL;
  g_remove(fixture->xmp_path);
  dt_free(fixture->xmp_path);
  fixture->xmp_path = NULL;
  return testdb_teardown(state);
}

void dt_test_xmp_fixture_write_packet(const dt_test_xmp_fixture_t *fixture, const char *properties)
{
  char *packet = g_strdup_printf(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
      "xmlns:darktable=\"http://darktable.sf.net/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:lr=\"http://ns.adobe.com/lightroom/1.0/\" "
      "darktable:xmp_version=\"0\">%s"
      "</rdf:Description></rdf:RDF></x:xmpmeta>",
      properties);
  assert_true(g_file_set_contents(fixture->xmp_path, packet, -1, NULL));
  dt_free(packet);
}

dt_image_t dt_test_xmp_fixture_make_image(const char *film_folder, const char *filename)
{
  const int32_t film = testdb_make_film(film_folder);
  const int32_t imgid = testdb_make_image(film, filename);
  assert_true(imgid > 0);

  dt_image_t image;
  dt_image_init(&image);
  assert_true(dt_image_repository_load(imgid, &image));
  return image;
}
