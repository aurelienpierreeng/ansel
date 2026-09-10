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

/* A headless paint of a real canvas, the way the atelier paints it, with the compositor's
 * phase timings: the profile the tuning starts from. Needs CANVAS_BENCH_FILE to point at an
 * .anselcanvas; passes trivially otherwise, so the suite stays green on every machine. Run:
 *
 *   CANVAS_BENCH_FILE=/path/to/some.anselcanvas ./tests/unittests/bench_canvas_paint
 */

#include "caches/pixelpipe_cache.h"
#include "canvas/canvas.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_export.h"
#include "canvas/canvas_render.h"
#include "colorprofiles/colorspaces.h"
#include "common/conf.h"
#include "common/times.h"
#include "darktable.h"

#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static char *_rcfile = NULL;

static int _group_setup(void **state)
{
  (void)state;
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_bench_canvas.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  // The parallel loops size their scratch from this, as dt_init() sets it.
#ifdef _OPENMP
  darktable.num_openmp_threads = omp_get_max_threads();
#else
  darktable.num_openmp_threads = 1;
#endif
  dt_colorprofiles_init();
  return dt_dev_pixelpipe_cache_init(512u * 1024u * 1024u, FALSE, FALSE) ? 0 : 1;
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
  return 0;
}

static void _print(const char *label, const double seconds)
{
  const dt_canvas_paint_stats_t stats = dt_canvas_paint_last_stats();
  printf("%-28s %7.1f ms %s| %" G_GINT64_FORMAT " px, %d objects, %d layers, %d shadows | background %.1f, objects %.1f "
         "(cairo %.1f, shadows %.1f), encode %.1f\n",
         label, seconds * 1000.0, stats.cached ? "(cached) " : "", stats.pixels, stats.objects, stats.layers, stats.shadows,
         stats.background_seconds * 1000.0, stats.objects_seconds * 1000.0, stats.paint_seconds * 1000.0,
         stats.shadow_seconds * 1000.0, stats.encode_seconds * 1000.0);
}

static double _paint_once_quality(cairo_surface_t *surface, const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache,
                                  const double zoom, const double center_x, const double center_y, const int width,
                                  const int height, const gboolean for_display, const double quality)
{
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, width * 0.5, height * 0.5);
  cairo_scale(cr, zoom, zoom);
  cairo_translate(cr, -center_x, -center_y);
  dt_canvas_rect_t visible = { center_x - width * 0.5 / zoom, center_y - height * 0.5 / zoom, width / zoom, height / zoom };
  dt_canvas_paint_options_t options = for_display ? dt_canvas_paint_options_display(cache, 1.0 / zoom, visible)
                                                  : dt_canvas_paint_options_export(cache, 1.0 / zoom, visible);
  options.quality = quality;
  const double start = dt_get_wtime();
  dt_canvas_paint(cr, canvas, &options);
  const double seconds = dt_get_wtime() - start;
  cairo_destroy(cr);
  return seconds;
}

static double _paint_once(cairo_surface_t *surface, const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache,
                          const double zoom, const double center_x, const double center_y, const int width,
                          const int height, const gboolean for_display)
{
  return _paint_once_quality(surface, canvas, cache, zoom, center_x, center_y, width, height, for_display, 1.0);
}

static void _bench(void **state)
{
  (void)state;
  const char *path = g_getenv("CANVAS_BENCH_FILE");
  if(IS_NULL_PTR(path))
  {
    printf("CANVAS_BENCH_FILE not set: nothing to bench\n");
    return;
  }
  GError *error = NULL;
  dt_canvas_t *canvas = dt_canvas_load(path, &error);
  assert_non_null(canvas);
  // A 2560x1440 viewport on a 2x screen, fitted to the canvas.
  const int width = 2560;
  const int height = 1440;
  const double device_scale = 2.0;
  double min_x = INFINITY;
  double min_y = INFINITY;
  double max_x = -INFINITY;
  double max_y = -INFINITY;
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(canvas, idx);
    if(!dt_canvas_object_is_frame(object)) continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
    min_x = fmin(min_x, bounds.x);
    min_y = fmin(min_y, bounds.y);
    max_x = fmax(max_x, bounds.x + bounds.width);
    max_y = fmax(max_y, bounds.y + bounds.height);
  }
  const double zoom = 0.9 * fmin(width / fmax(max_x - min_x, 1.0), height / fmax(max_y - min_y, 1.0));
  const double center_x = 0.5 * (min_x + max_x);
  const double center_y = 0.5 * (min_y + max_y);
  printf("canvas %s: %u objects, style %u, fit zoom %.3f\n", path, dt_canvas_object_count(canvas),
         canvas->background_style, zoom);

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                        (int)(height * device_scale));
  cairo_surface_set_device_scale(surface, device_scale, device_scale);
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(TRUE, 512u * 1024u * 1024u);

  _print("cold, display", _paint_once(surface, canvas, cache, zoom, center_x, center_y, width, height, TRUE));
  _print("same frame again (cache)", _paint_once(surface, canvas, cache, zoom, center_x, center_y, width, height, TRUE));
  // CANVAS_BENCH_WARM_LOOPS repeats the warm frame for a profiler: the cold frame's paper and
  // cutout synthesis otherwise dominates any sample.
  const char *loops_env = g_getenv("CANVAS_BENCH_WARM_LOOPS");
  const int loops = IS_NULL_PTR(loops_env) ? 0 : atoi(loops_env);
  for(int loop = 0; loop < loops; loop++)
    _print("warm loop", _paint_once(surface, canvas, cache, zoom, center_x + (loop % 2), center_y, width, height, TRUE));
  _print("warm, display, panned 1", _paint_once(surface, canvas, cache, zoom, center_x + 5.0, center_y, width, height, TRUE));
  _print("warm, display", _paint_once(surface, canvas, cache, zoom, center_x, center_y, width, height, TRUE));
  _print("half quality, panned", _paint_once_quality(surface, canvas, cache, zoom, center_x + 7.0, center_y, width, height, TRUE, 0.5));
  _print("half quality, panned", _paint_once_quality(surface, canvas, cache, zoom, center_x + 9.0, center_y, width, height, TRUE, 0.5));
  _print("half quality, zoomed", _paint_once_quality(surface, canvas, cache, zoom * 1.2, center_x, center_y, width, height, TRUE, 0.5));
  _print("warm, display, panned", _paint_once(surface, canvas, cache, zoom, center_x + 10.0, center_y, width, height, TRUE));
  _print("warm, display, zoomed x1.5", _paint_once(surface, canvas, cache, zoom * 1.5, center_x, center_y, width, height, TRUE));
  _print("warm, display, zoomed x1.5", _paint_once(surface, canvas, cache, zoom * 1.5, center_x, center_y, width, height, TRUE));
  _print("warm, export encoding", _paint_once(surface, canvas, cache, zoom, center_x, center_y, width, height, FALSE));
  cairo_surface_write_to_png(surface, "/tmp/canvas_bench.png");

  // CANVAS_BENCH_VERIFY=1 paints every view twice, once through the caches and once with
  // none, and compares the pixels. The caches are the only difference between the two, so a
  // pixel that differs is one of them answering with something it should not have kept.
  if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_VERIFY")))
  {
    const double checks[] = { 1.0, 0.5, 1.0, 2.0, 1.0, 0.5, 3.0, 1.0 };
    cairo_surface_t *cached_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                                 (int)(height * device_scale));
    cairo_surface_t *plain_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                                (int)(height * device_scale));
    cairo_surface_set_device_scale(cached_surface, device_scale, device_scale);
    cairo_surface_set_device_scale(plain_surface, device_scale, device_scale);
    dt_canvas_surface_cache_t *verify_cache = dt_canvas_surface_cache_new(TRUE, 512u * 1024u * 1024u);
    for(size_t idx = 0; idx < sizeof(checks) / sizeof(checks[0]); idx++)
    {
      const double view_zoom = zoom * checks[idx];
      const double view_x = center_x + idx * 3.0;
      // The warmed cache against a cache that has seen nothing else: both take every fast
      // path, so the only thing that can differ between them is what the warmed one kept.
      dt_canvas_surface_cache_t *fresh_cache = dt_canvas_surface_cache_new(TRUE, 512u * 1024u * 1024u);
      _paint_once(cached_surface, canvas, verify_cache, view_zoom, view_x, center_y, width, height, TRUE);
      _paint_once(plain_surface, canvas, fresh_cache, view_zoom, view_x, center_y, width, height, TRUE);
      dt_canvas_surface_cache_free(fresh_cache);
      cairo_surface_flush(cached_surface);
      cairo_surface_flush(plain_surface);
      const uint8_t *a = cairo_image_surface_get_data(cached_surface);
      const uint8_t *b = cairo_image_surface_get_data(plain_surface);
      const int stride = cairo_image_surface_get_stride(cached_surface);
      int64_t differing = 0;
      int worst = 0;
      int worst_x = -1;
      int worst_y = -1;
      for(int row = 0; row < (int)(height * device_scale); row++)
      {
        for(int col = 0; col < (int)(width * device_scale); col++)
        {
          const uint32_t first = *(const uint32_t *)(a + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
          const uint32_t second = *(const uint32_t *)(b + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
          if(first == second) continue;
          differing++;
          for(int channel = 0; channel < 3; channel++)
          {
            const int delta = abs((int)((first >> (8 * channel)) & 0xFF) - (int)((second >> (8 * channel)) & 0xFF));
            if(delta > worst)
            {
              worst = delta;
              worst_x = col;
              worst_y = row;
            }
          }
        }
      }
      printf("%-28s zoom x%.2f: %" G_GINT64_FORMAT " px differ, worst %d at (%d, %d)\n", "verify warm vs fresh",
             checks[idx], differing, worst, worst_x, worst_y);
    }
    dt_canvas_surface_cache_free(verify_cache);
    cairo_surface_destroy(cached_surface);
    cairo_surface_destroy(plain_surface);
  }

  // CANVAS_BENCH_CUTOUTS=1 reports every cut frame's raster on its own, at every size the
  // quantisation can pick, as saved and turned inside out: the alpha at the shape's centre
  // against the alpha at the frame's corner. Whichever side a shape keeps, it keeps at every
  // size, so those two must not trade places down the column.
  if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_CUTOUTS")))
  {
    dt_canvas_surface_cache_t *probe_cache = dt_canvas_surface_cache_new(TRUE, 512u * 1024u * 1024u);
    for(guint object_idx = 0; object_idx < dt_canvas_object_count(canvas); object_idx++)
    {
      const dt_canvas_object_t *object = dt_canvas_object_at(canvas, object_idx);
      if(IS_NULL_PTR(object) || !dt_canvas_object_is_frame(object)) continue;
      if(object->mask.shape == DT_CANVAS_MASK_NONE) continue;
      printf("cutout on object %u: shape %u, invert %d, feather %.3f, frame %.0f x %.0f\n", object->id,
             object->mask.shape, (object->mask.flags & DT_CANVAS_MASK_INVERT) != 0, object->mask.feather,
             object->width, object->height);
      // The raster on its own, at every size the quantisation can pick: alpha at the shape's
      // centre against alpha at the frame's corner. Whichever side the shape keeps, it keeps
      // it at every size -- the two must not trade places down this column.
      for(int pass = 0; pass < 2; pass++)
      {
      // The second pass asks for the other side of the same shape: a shape and its inverse
      // must answer with the same raster read the other way round, at every size.
      dt_canvas_object_t probe_object = *object;
      if(pass) probe_object.mask.flags |= DT_CANVAS_MASK_INVERT;
      printf("  %s\n", pass ? "inverted:" : "as saved:");
      for(int longer = 64; longer <= 2048; longer *= 2)
      {
        object = &probe_object;
        const double raster_scale = (double)longer / fmax(object->width, object->height);
        const int mask_width = MAX((int)lround(object->width * raster_scale), 2);
        const int mask_height = MAX((int)lround(object->height * raster_scale), 2);
        cairo_surface_t *raster = dt_canvas_render_mask(object, mask_width, mask_height, 0, 0);
        if(IS_NULL_PTR(raster)) continue;
        cairo_surface_flush(raster);
        const uint8_t *alpha = cairo_image_surface_get_data(raster);
        const int mask_stride = cairo_image_surface_get_stride(raster);
        const int centre_col = (int)lround(object->mask.center_x * mask_width);
        const int centre_row = (int)lround(object->mask.center_y * mask_height);
        const uint8_t at_centre = alpha[(size_t)CLAMP(centre_row, 0, mask_height - 1) * mask_stride
                                        + CLAMP(centre_col, 0, mask_width - 1)];
        const uint8_t at_corner = alpha[(size_t)(mask_height / 40) * mask_stride + mask_width / 40];
        printf("    raster %4d x %4d: centre alpha %3u, corner alpha %3u\n", mask_width, mask_height, at_centre,
               at_corner);
        cairo_surface_destroy(raster);
      }
      }
      object = dt_canvas_object_at(canvas, object_idx);
    }
    dt_canvas_surface_cache_free(probe_cache);
  }

  // CANVAS_BENCH_INVERT=<object id> turns that frame's cutout inside out and paints the whole
  // document at a sweep of resolutions, reporting what lands at the shape's centre and at the
  // frame's corner. Which side a cutout keeps cannot depend on how far the view is zoomed in.
  if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_INVERT")))
  {
    const uint32_t wanted = (uint32_t)atoi(g_getenv("CANVAS_BENCH_INVERT"));
    dt_canvas_object_t *target = NULL;
    for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
    {
      dt_canvas_object_t *candidate = dt_canvas_object_at(canvas, idx);
      if(!IS_NULL_PTR(candidate) && candidate->id == wanted) target = candidate;
    }
    if(!IS_NULL_PTR(target))
    {
      target->mask.flags |= DT_CANVAS_MASK_INVERT;
      dt_canvas_touch(canvas);
      dt_canvas_surface_cache_t *invert_cache = dt_canvas_surface_cache_new(TRUE, 512u * 1024u * 1024u);
      printf("object %u inverted, frame %.0f x %.0f, shape centre (%.3f, %.3f) radius %.3f\n", target->id,
             target->width, target->height, target->mask.center_x, target->mask.center_y, target->mask.radius_x);
      const double sweep[] = { 0.2, 0.4, 0.665, 1.0, 1.5, 2.0, 3.0, 4.167 };
      for(size_t idx = 0; idx < sizeof(sweep) / sizeof(sweep[0]); idx++)
      {
        const double view_zoom = sweep[idx];
        const double centre_x = target->x + (target->mask.center_x - 0.5) * target->width;
        const double centre_y = target->y + (target->mask.center_y - 0.5) * target->height;
        cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                            (int)(height * device_scale));
        cairo_surface_set_device_scale(probe, device_scale, device_scale);
        const double probe_quality = IS_NULL_PTR(g_getenv("CANVAS_BENCH_QUALITY"))
                                         ? 1.0
                                         : g_ascii_strtod(g_getenv("CANVAS_BENCH_QUALITY"), NULL);
        _paint_once_quality(probe, canvas, invert_cache, view_zoom, centre_x, centre_y, width, height, TRUE,
                            probe_quality);
        cairo_surface_flush(probe);
        const uint8_t *pixels = cairo_image_surface_get_data(probe);
        const int stride = cairo_image_surface_get_stride(probe);
        const int centre_col = (int)lround(width * 0.5 * device_scale);
        const int centre_row = (int)lround(height * 0.5 * device_scale);
        // A point inside the frame but well outside the circle, along its longer side.
        const int outer_col = centre_col + (int)lround(target->width * 0.45 * view_zoom * device_scale);
        uint32_t at_centre = 0;
        uint32_t at_outer = 0;
        if(centre_col >= 0 && centre_col < (int)(width * device_scale))
          at_centre = *(const uint32_t *)(pixels + (size_t)centre_row * stride + (size_t)centre_col * 4) & 0xFFFFFFu;
        if(outer_col >= 0 && outer_col < (int)(width * device_scale))
          at_outer = *(const uint32_t *)(pixels + (size_t)centre_row * stride + (size_t)outer_col * 4) & 0xFFFFFFu;
        printf("    %.3f px/unit (frame %.0f px): hole %06x, ring %06x\n", view_zoom, target->width * view_zoom,
               at_centre, at_outer);
        if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_LOOK")))
        {
          gchar *shot = g_strdup_printf("%s_%03d.png", g_getenv("CANVAS_BENCH_LOOK"), (int)lround(view_zoom * 100.0));
          cairo_surface_write_to_png(probe, shot);
          dt_free(shot);
        }
        cairo_surface_destroy(probe);
      }
      dt_canvas_surface_cache_free(invert_cache);
    }
  }

  // CANVAS_BENCH_PDF=<path> exports the document too, and prints what the file weighs per
  // page and per megapixel: the raster's size is a fact of the geometry, its weight is not.
  const char *pdf_path = g_getenv("CANVAS_BENCH_PDF");
  if(!IS_NULL_PTR(pdf_path))
  {
    dt_canvas_export_options_t pdf = dt_canvas_export_options_default();
    pdf.dpi = (float)(IS_NULL_PTR(g_getenv("CANVAS_BENCH_DPI")) ? 300 : atoi(g_getenv("CANVAS_BENCH_DPI")));
    if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_FORMAT")))
      pdf.format = (dt_canvas_export_format_t)CLAMP(atoi(g_getenv("CANVAS_BENCH_FORMAT")), 0, DT_CANVAS_EXPORT_LAST - 1);
    GError *pdf_error = NULL;
    const double pdf_start = dt_get_wtime();
    const gboolean pdf_ok = dt_canvas_export(canvas, pdf_path, &pdf, &pdf_error);
    const double pdf_seconds = dt_get_wtime() - pdf_start;
    GStatBuf info;
    memset(&info, 0, sizeof(info));
    if(pdf_ok && g_stat(pdf_path, &info) == 0)
      printf("%-28s %7.1f ms  | %.1f MB at %.0f dpi\n", "pdf export", pdf_seconds * 1000.0,
             (double)info.st_size / (1024.0 * 1024.0), pdf.dpi);
    else
      printf("pdf export FAILED: %s\n", IS_NULL_PTR(pdf_error) ? "?" : pdf_error->message);
    g_clear_error(&pdf_error);
  }

  dt_canvas_surface_cache_free(cache);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
}

int main(void)
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_bench) };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
