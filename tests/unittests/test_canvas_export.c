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

/* Writing a canvas out as pages: the page is the document's, the raster is exactly the
 * resolution asked for times the sheet's own size, and the bleed grows the sheet. */

#include "caches/pixelpipe_cache.h"
#include "canvas/canvas.h"
#include "canvas/canvas_export.h"
#include "colorprofiles/colorspaces.h"
#include "common/conf.h"
#include "darktable.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

static char *_rcfile = NULL;
static char *_directory = NULL;

static int _group_setup(void **state)
{
  (void)state;
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_test_canvas_export.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  darktable.num_openmp_threads = 1;
  dt_colorprofiles_init();
  _directory = g_dir_make_tmp("ansel_canvas_export_XXXXXX", NULL);
  return IS_NULL_PTR(_directory) || !dt_dev_pixelpipe_cache_init(64u * 1024u * 1024u, FALSE, FALSE) ? 1 : 0;
}

static int _group_teardown(void **state)
{
  (void)state;
  dt_dev_pixelpipe_cache_cleanup();
  dt_colorprofiles_cleanup();
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  g_free(_rcfile);
  if(!IS_NULL_PTR(_directory))
  {
    g_rmdir(_directory);
    g_free(_directory);
  }
  return 0;
}

/** A canvas of `pages` A6 pages side by side, one frame on each. */
static dt_canvas_t *_canvas_of_pages(const int pages)
{
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->background_style = DT_CANVAS_BACKGROUND_PLAIN;
  canvas->paper_size = DT_CANVAS_PAPER_A6;
  canvas->paper_landscape = 0;
  double width = 0.0;
  double height = 0.0;
  assert_true(dt_canvas_paper_dimensions(canvas, &width, &height));
  for(int page = 0; page < pages; page++)
  {
    dt_canvas_object_t *frame
        = dt_canvas_add_text(canvas, (page + 0.5) * width, height * 0.5, width * 0.5, height * 0.5, "");
    frame->text.background = dt_canvas_color(0.2f, 0.4f, 0.8f, 1.0f);
  }
  return canvas;
}

static gchar *_output(const char *name)
{
  return g_build_filename(_directory, name, NULL);
}

/** A PNG's pixel size, read back from the file. */
static void _png_size(const char *path, int *width, int *height)
{
  *width = 0;
  *height = 0;
  FILE *file = g_fopen(path, "rb");
  assert_non_null(file);
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  assert_int_equal(setjmp(png_jmpbuf(png)), 0);
  png_init_io(png, file);
  png_read_info(png, info);
  *width = (int)png_get_image_width(png, info);
  *height = (int)png_get_image_height(png, info);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(file);
}

/**
 * The raster is the sheet's own size times the resolution, and not one pixel more: an A6 page
 * is 298 x 420 points, which at 150 dpi is 621 x 875 pixels.
 */
static void _a_page_is_rasterised_at_exactly_its_size_times_the_resolution(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _canvas_of_pages(1);
  dt_canvas_export_options_t options = dt_canvas_export_options_default();
  options.format = DT_CANVAS_EXPORT_PNG;
  options.dpi = 150.0f;
  gchar *path = _output("one.png");
  GError *error = NULL;
  assert_true(dt_canvas_export(canvas, path, &options, &error));
  assert_null(error);
  int width = 0;
  int height = 0;
  _png_size(path, &width, &height);
  assert_int_equal(width, (int)lround(298.0 / 72.0 * 150.0));
  assert_int_equal(height, (int)lround(420.0 / 72.0 * 150.0));
  g_remove(path);
  dt_free(path);
  dt_canvas_free(canvas);
}

/**
 * The bleed grows the sheet on all four sides, and nothing else moves. It belongs to the
 * document, beside the page size it grows, not to the export that writes it.
 */
static void _a_bleed_grows_the_sheet_on_every_side(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _canvas_of_pages(1);
  canvas->page_bleed = 72.0f; // one inch of canvas units: one page's worth of pixels per side at 72 dpi
  dt_canvas_export_options_t options = dt_canvas_export_options_default();
  options.format = DT_CANVAS_EXPORT_PNG;
  options.dpi = 72.0f;
  gchar *path = _output("bleed.png");
  GError *error = NULL;
  assert_true(dt_canvas_export(canvas, path, &options, &error));
  int width = 0;
  int height = 0;
  _png_size(path, &width, &height);
  assert_int_equal(width, 298 + 144);
  assert_int_equal(height, 420 + 144);
  g_remove(path);
  dt_free(path);
  dt_canvas_free(canvas);
}

/** One file per page for PNG and JPEG, numbered; one file holding them all for PDF and TIFF. */
static void _every_format_writes_every_page(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _canvas_of_pages(3);
  const struct
  {
    dt_canvas_export_format_t format;
    const char *name;
    gboolean one_file;
  } cases[] = {
    { DT_CANVAS_EXPORT_PDF, "book.pdf", TRUE },
    { DT_CANVAS_EXPORT_PNG, "book.png", FALSE },
    { DT_CANVAS_EXPORT_JPEG, "book.jpg", FALSE },
    { DT_CANVAS_EXPORT_TIFF, "book.tif", TRUE },
  };
  for(size_t idx = 0; idx < sizeof(cases) / sizeof(cases[0]); idx++)
  {
    dt_canvas_export_options_t options = dt_canvas_export_options_default();
    options.format = cases[idx].format;
    options.dpi = 72.0f;
    gchar *path = _output(cases[idx].name);
    GError *error = NULL;
    assert_true(dt_canvas_export(canvas, path, &options, &error));
    assert_null(error);
    if(cases[idx].one_file)
    {
      assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
      g_remove(path);
    }
    else
    {
      // The lone-page name is not used when there are several: they are numbered from one.
      assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
      gchar *stem = g_strndup(path, strlen(path) - 4);
      for(int page = 1; page <= 3; page++)
      {
        gchar *numbered = g_strdup_printf("%s_%02d%s", stem, page, path + strlen(path) - 4);
        assert_true(g_file_test(numbered, G_FILE_TEST_EXISTS));
        g_remove(numbered);
        dt_free(numbered);
      }
      dt_free(stem);
    }
    dt_free(path);
  }
  dt_canvas_free(canvas);
}

/** A page of photographs is carried as one: the lossless stream is many times the size. */
static void _a_pdf_page_is_a_photograph_not_a_bitmap(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _canvas_of_pages(1);
  canvas->background_style = DT_CANVAS_BACKGROUND_EMBOSSED;
  dt_canvas_touch(canvas);
  GStatBuf lossy;
  GStatBuf lossless;
  memset(&lossy, 0, sizeof(lossy));
  memset(&lossless, 0, sizeof(lossless));
  gchar *path = _output("weight.pdf");
  GError *error = NULL;

  dt_canvas_export_options_t options = dt_canvas_export_options_default();
  options.dpi = 200.0f;
  options.quality = 92;
  assert_true(dt_canvas_export(canvas, path, &options, &error));
  assert_int_equal(g_stat(path, &lossy), 0);

  options.quality = 100;
  assert_true(dt_canvas_export(canvas, path, &options, &error));
  assert_int_equal(g_stat(path, &lossless), 0);
  // A flat frame on a textured paper compresses about 2.8 times better as a photograph here,
  // and about six times on a page of real pictures. Two is the floor a regression would break.
  assert_true(lossless.st_size > 2 * lossy.st_size);
  g_remove(path);
  dt_free(path);
  dt_canvas_free(canvas);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_page_is_rasterised_at_exactly_its_size_times_the_resolution),
    cmocka_unit_test(_a_bleed_grows_the_sheet_on_every_side),
    cmocka_unit_test(_every_format_writes_every_page),
    cmocka_unit_test(_a_pdf_page_is_a_photograph_not_a_bitmap),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
