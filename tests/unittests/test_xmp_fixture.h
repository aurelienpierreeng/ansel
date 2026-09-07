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

#ifndef DT_TESTS_UNITTESTS_TEST_XMP_FIXTURE_H
#define DT_TESTS_UNITTESTS_TEST_XMP_FIXTURE_H

#include "common/image.h"

typedef struct dt_test_xmp_fixture_t
{
  char *config_path;
  char *xmp_path;
} dt_test_xmp_fixture_t;

/** Initialise an isolated database, configuration, EXIF decoder, and XMP file. */
int dt_test_xmp_fixture_setup(void **state, dt_test_xmp_fixture_t *fixture, const char *name);

/** Destroy the resources initialised by dt_test_xmp_fixture_setup(). */
int dt_test_xmp_fixture_teardown(void **state, dt_test_xmp_fixture_t *fixture);

/** Write an XMP version 0 packet containing properties to the fixture's sidecar. */
void dt_test_xmp_fixture_write_packet(const dt_test_xmp_fixture_t *fixture, const char *properties);

/** Create and load a default image belonging to film_folder. */
dt_image_t dt_test_xmp_fixture_make_image(const char *film_folder, const char *filename);

#endif
