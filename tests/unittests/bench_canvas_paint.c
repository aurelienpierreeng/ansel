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
 *
 * CANVAS_BENCH_STARS=<n> builds a document instead of reading one: a column of flowing text with
 * `n` stars laid over it, which is what a drawn shape costs both the painter and the text that has
 * to go round it. Zero is the same page with no shape on it -- the reading the star's own cost is
 * measured against.
 *
 *   CANVAS_BENCH_STARS=20 ./tests/unittests/bench_canvas_paint
 */

#include "caches/pixelpipe_cache.h"
#include "canvas/canvas.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_export.h"
#include "canvas/canvas_props.h"
#include "canvas/canvas_render.h"
#include "colorprofiles/colorspaces.h"
#include "common/conf.h"
#include "common/times.h"
#include "darktable.h"
#include "system/openmp.h"

#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

static char *_rcfile = NULL;

static int _group_setup(void **state)
{
  (void)state;
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_bench_canvas.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  // The parallel loops size their scratch from this, as dt_init() sets it.
  darktable.num_openmp_threads = omp_get_max_threads();
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

/**
 * A paint of the same view confined to one box of the plane, in canvas units: what the view is
 * handed when it invalidates a rectangle instead of the whole window, since GTK then clips the
 * context it draws with. NULL paints the whole view, as a full redraw does.
 */
static double _paint_clipped(cairo_surface_t *surface, const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache,
                             const double zoom, const double center_x, const double center_y, const int width,
                             const int height, const double quality, const dt_canvas_rect_t *clip)
{
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, width * 0.5, height * 0.5);
  cairo_scale(cr, zoom, zoom);
  cairo_translate(cr, -center_x, -center_y);
  if(!IS_NULL_PTR(clip))
  {
    /*
     * Rounded OUT to whole pixels of the widget, as GTK rounds what gtk_widget_queue_draw_area() is
     * given: a clip left on fractional pixels is anti-aliased along its edge by cairo, which blends
     * the old frame and the new one there -- an artefact of the test, not of anything the atelier
     * does, and it read as a ring of differences around every repaint.
     */
    double x0 = clip->x;
    double y0 = clip->y;
    double x1 = clip->x + clip->width;
    double y1 = clip->y + clip->height;
    cairo_user_to_device(cr, &x0, &y0);
    cairo_user_to_device(cr, &x1, &y1);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    const double left = floor(fmin(x0, x1));
    const double top = floor(fmin(y0, y1));
    cairo_rectangle(cr, left, top, ceil(fmax(x0, x1)) - left, ceil(fmax(y0, y1)) - top);
    cairo_restore(cr);
    cairo_clip(cr);
  }
  dt_canvas_rect_t visible = { center_x - width * 0.5 / zoom, center_y - height * 0.5 / zoom, width / zoom, height / zoom };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_display(cache, 1.0 / zoom, visible);
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

/**
 * A page of flowing text with `stars` stars laid over it, at the size and spacing a reader would
 * lay ornaments out at. The text is added FIRST so that everything after it is laid OVER it and is
 * an obstacle its lines must clear -- which is where a polygon's coverage raster is paid for.
 */
static dt_canvas_t *_stars_over_text(const int stars)
{
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 1400.0, 1800.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and the "
      "measure a line is set to belongs to the page rather than to the window looking at it. A line is set "
      "across every clear stretch of its band, not the widest one, so a picture in the middle of a column "
      "leaves space either side and the line carries on past it. What has to clear a picture is the glyphs, "
      "and a logical box carries the font's full ascent above the tallest of them.");
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(0.85f, 0.2f, 0.1f, 1.0f);
  style.border_override = TRUE;
  style.border_width = 6.0f;
  style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 0.6f);
  style.sides = 5;
  style.depth = DT_CANVAS_SHAPE_STAR_DEPTH;
  const double width = 180.0;
  const double height = width / dt_canvas_shape_unit_aspect(style.sides, style.depth, style.roundness);
  for(int idx = 0; idx < stars; idx++)
  {
    const dt_canvas_rect_t box = { -600.0 + 260.0 * (idx % 5), -800.0 + 380.0 * (idx / 5), width, height };
    assert_non_null(dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_POLYGON, &box, &style));
  }
  dt_canvas_props_settle_all(canvas);
  return canvas;
}

static void _bench(void **state)
{
  (void)state;
  const char *path = g_getenv("CANVAS_BENCH_FILE");
  const char *stars_env = g_getenv("CANVAS_BENCH_STARS");
  if(IS_NULL_PTR(path) && IS_NULL_PTR(stars_env))
  {
    printf("neither CANVAS_BENCH_FILE nor CANVAS_BENCH_STARS set: nothing to bench\n");
    return;
  }
  GError *error = NULL;
  dt_canvas_t *canvas = NULL;
  if(IS_NULL_PTR(path))
  {
    const int stars = CLAMP(atoi(stars_env), 0, 200);
    canvas = _stars_over_text(stars);
    path = "<stars over flowing text>";
    printf("built %d stars over a flowing column\n", stars);
  }
  else
  {
    canvas = dt_canvas_load(path, &error);
  }
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

  /*
   * A DRAG, the way the atelier runs one: the largest picture moved a few units per motion event,
   * the generation bumped each time -- which is what makes every frame miss the composite cache --
   * and the view repainted at the gesture's half quality. First over the whole window, which is
   * what a full redraw asks for; then confined to what the move damages, the picture's extent
   * before and after, which is all that can change on the page.
   */
  {
    dt_canvas_object_t *largest = NULL;
    double largest_area = 0.0;
    for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
    {
      dt_canvas_object_t *object = (dt_canvas_object_t *)dt_canvas_object_at(canvas, idx);
      if(object->kind != DT_CANVAS_OBJECT_IMAGE) continue;
      if(object->width * object->height > largest_area)
      {
        largest_area = object->width * object->height;
        largest = object;
      }
    }
    if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_DAMAGE")))
    {
      /*
       * THE DAMAGE, HELD TO A FULL REPAINT. For every object that can be moved: paint the view,
       * move the object by an uneven amount, repaint ONLY what dt_canvas_paint_move_damage() says
       * changed -- over the previous frame, as GTK composites a clipped redraw -- and compare that,
       * pixel for pixel, with the same view repainted whole. A pixel that differs is one the damage
       * left out, and would stay on screen stale as a trail behind the drag.
       */
      cairo_surface_t *partial = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                            (int)(height * device_scale));
      cairo_surface_t *whole = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(width * device_scale),
                                                          (int)(height * device_scale));
      cairo_surface_set_device_scale(partial, device_scale, device_scale);
      cairo_surface_set_device_scale(whole, device_scale, device_scale);
      int checked = 0;
      int failed = 0;
      for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
      {
        dt_canvas_object_t *object = (dt_canvas_object_t *)dt_canvas_object_at(canvas, idx);
        const gboolean movable = dt_canvas_object_is_frame(object) || dt_canvas_connector_has_free_end(object);
        if(!movable) continue;
        const uint32_t id = object->id;
        _paint_clipped(partial, canvas, cache, zoom, center_x, center_y, width, height, 1.0, NULL);
        dt_canvas_rect_t before = { 0 };
        dt_canvas_rect_t after = { 0 };
        const gboolean known_before = dt_canvas_paint_move_damage(canvas, &id, 1, &before);
        const double move_x = 7.3;
        const double move_y = -4.1;
        if(dt_canvas_object_is_frame(object))
        {
          object->x += move_x;
          object->y += move_y;
        }
        else
          dt_canvas_connector_translate(object, move_x, move_y);
        dt_canvas_touch(canvas);
        const gboolean known_after = dt_canvas_paint_move_damage(canvas, &id, 1, &after);
        if(known_before && known_after)
        {
          const double x0 = fmin(before.x, after.x);
          const double y0 = fmin(before.y, after.y);
          const double x1 = fmax(before.x + before.width, after.x + after.width);
          const double y1 = fmax(before.y + before.height, after.y + after.height);
          const dt_canvas_rect_t clip = { x0, y0, x1 - x0, y1 - y0 };
          _paint_clipped(partial, canvas, cache, zoom, center_x, center_y, width, height, 1.0, &clip);
          _paint_clipped(whole, canvas, cache, zoom, center_x, center_y, width, height, 1.0, NULL);
          cairo_surface_flush(partial);
          cairo_surface_flush(whole);
          const uint8_t *a = cairo_image_surface_get_data(partial);
          const uint8_t *b = cairo_image_surface_get_data(whole);
          const int stride = cairo_image_surface_get_stride(partial);
          int64_t differing = 0;
          int worst = 0;
          // Where the clip lands in the surface's pixels, to tell a repaint that differs INSIDE it
          // from a damage that left something OUTSIDE it.
          const double to_px = zoom * device_scale;
          const int clip_x0 = (int)floor(((clip.x - center_x) * zoom + width * 0.5) * device_scale);
          const int clip_y0 = (int)floor(((clip.y - center_y) * zoom + height * 0.5) * device_scale);
          const int clip_x1 = (int)ceil(clip_x0 + clip.width * to_px) + 1;
          const int clip_y1 = (int)ceil(clip_y0 + clip.height * to_px) + 1;
          int64_t inside = 0;
          int worst_inside = 0;
          int worst_outside = 0;
          for(int row = 0; row < (int)(height * device_scale); row++)
            for(int col = 0; col < (int)(width * device_scale); col++)
            {
              const uint32_t first = *(const uint32_t *)(a + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
              const uint32_t second = *(const uint32_t *)(b + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
              if(first == second) continue;
              differing++;
              int delta = 0;
              for(int channel = 0; channel < 3; channel++)
                delta = MAX(delta, abs((int)((first >> (8 * channel)) & 0xFF) - (int)((second >> (8 * channel)) & 0xFF)));
              worst = MAX(worst, delta);
              const gboolean in_clip = col >= clip_x0 && col < clip_x1 && row >= clip_y0 && row < clip_y1;
              if(in_clip)
              {
                inside++;
                worst_inside = MAX(worst_inside, delta);
              }
              else
                worst_outside = MAX(worst_outside, delta);
            }
          printf("%-28s   inside the clip %" G_GINT64_FORMAT " px (worst %d), outside %" G_GINT64_FORMAT
                 " px (worst %d)\n", "", inside, worst_inside, differing - inside, worst_outside);
          if(!IS_NULL_PTR(g_getenv("CANVAS_BENCH_DAMAGE_MAP")) && differing > 0)
          {
            // Where the object now sits, in the surface's pixels, and a coarse map of the differences
            // over the clip: '#' where a cell holds a pixel off by more than 20, '+' more than 2, '.' any.
            dt_canvas_rect_t now = { 0 };
            dt_canvas_paint_object_extent(canvas, object, &now);
            printf("    object at px %.0f..%.0f x %.0f..%.0f, clip px %d..%d x %d..%d\n",
                   ((now.x - center_x) * zoom + width * 0.5) * device_scale,
                   ((now.x + now.width - center_x) * zoom + width * 0.5) * device_scale,
                   ((now.y - center_y) * zoom + height * 0.5) * device_scale,
                   ((now.y + now.height - center_y) * zoom + height * 0.5) * device_scale, clip_x0, clip_x1, clip_y0,
                   clip_y1);
            const int cells_x = 64;
            const int cells_y = 32;
            for(int cy = 0; cy < cells_y; cy++)
            {
              printf("    ");
              for(int cx = 0; cx < cells_x; cx++)
              {
                int cell_worst = -1;
                const int r0 = clip_y0 + (clip_y1 - clip_y0) * cy / cells_y;
                const int r1 = clip_y0 + (clip_y1 - clip_y0) * (cy + 1) / cells_y;
                const int c0 = clip_x0 + (clip_x1 - clip_x0) * cx / cells_x;
                const int c1 = clip_x0 + (clip_x1 - clip_x0) * (cx + 1) / cells_x;
                for(int row = MAX(r0, 0); row < MIN(r1, (int)(height * device_scale)); row++)
                  for(int col = MAX(c0, 0); col < MIN(c1, (int)(width * device_scale)); col++)
                  {
                    const uint32_t first = *(const uint32_t *)(a + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
                    const uint32_t second = *(const uint32_t *)(b + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
                    if(first == second) continue;
                    int delta = 0;
                    for(int channel = 0; channel < 3; channel++)
                      delta = MAX(delta, abs((int)((first >> (8 * channel)) & 0xFF) - (int)((second >> (8 * channel)) & 0xFF)));
                    cell_worst = MAX(cell_worst, delta);
                  }
                putchar(cell_worst < 0 ? ' ' : cell_worst > 20 ? '#' : cell_worst > 2 ? '+' : '.');
              }
              putchar('\n');
            }
          }
          checked++;
          /*
           * More than eight codes is a pixel a person could see. At or under it is the PAPER, which
           * cairo samples a hair differently when the box it fills is smaller: measured on three
           * real canvases, never more than 6 codes and only where paper shows, inside the repainted
           * box. What remains above it is reported, not hidden: one pixel on the edge of a text
           * frame crossed by a connector, off by 56, in a partial repaint -- the painter's, and
           * repainted whole the moment the gesture ends.
           */
          if(worst > 8) failed++;
          printf("%-28s object %3u kind %u: %" G_GINT64_FORMAT " px differ, worst %d\n", "damage vs whole", id,
                 object->kind, differing, worst);
        }
        if(dt_canvas_object_is_frame(object))
        {
          object->x -= move_x;
          object->y -= move_y;
        }
        else
          dt_canvas_connector_translate(object, -move_x, -move_y);
        dt_canvas_touch(canvas);
      }
      printf("%-28s %d objects moved, %d with a pixel off by more than 8 codes\n", "damage vs whole", checked,
             failed);
      cairo_surface_destroy(partial);
      cairo_surface_destroy(whole);
    }
    if(!IS_NULL_PTR(largest))
    {
      const char *steps_env = g_getenv("CANVAS_BENCH_DRAG_STEPS");
      const int steps = IS_NULL_PTR(steps_env) ? 12 : MAX(atoi(steps_env), 1);
      const double step_x = 4.0;
      const double step_y = 3.0;
      double full = 0.0;
      for(int step = 0; step < steps; step++)
      {
        largest->x += step_x;
        largest->y += step_y;
        dt_canvas_touch(canvas);
        full += _paint_clipped(surface, canvas, cache, zoom, center_x, center_y, width, height, 0.5, NULL);
      }
      double damaged = 0.0;
      for(int step = 0; step < steps; step++)
      {
        dt_canvas_rect_t before = { 0 };
        dt_canvas_object_extent(canvas, largest, &before);
        largest->x -= step_x;
        largest->y -= step_y;
        dt_canvas_touch(canvas);
        dt_canvas_rect_t after = { 0 };
        dt_canvas_object_extent(canvas, largest, &after);
        const double x0 = fmin(before.x, after.x) - 2.0 / zoom;
        const double y0 = fmin(before.y, after.y) - 2.0 / zoom;
        const double x1 = fmax(before.x + before.width, after.x + after.width) + 2.0 / zoom;
        const double y1 = fmax(before.y + before.height, after.y + after.height) + 2.0 / zoom;
        const dt_canvas_rect_t clip = { x0, y0, x1 - x0, y1 - y0 };
        const char *quality_env = g_getenv("CANVAS_BENCH_DRAG_QUALITY");
        const double drag_quality = IS_NULL_PTR(quality_env) ? 1.0 : g_ascii_strtod(quality_env, NULL);
        damaged += _paint_clipped(surface, canvas, cache, zoom, center_x, center_y, width, height, drag_quality, &clip);
      }
      printf("%-28s %7.1f ms a frame over the whole view, %.1f ms confined to the damage (picture %.0f x %.0f of a "
             "%.0f x %.0f view)\n",
             "drag, largest picture", full * 1000.0 / steps, damaged * 1000.0 / steps, largest->width * zoom,
             largest->height * zoom, (double)width, (double)height);
    }
  }

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
