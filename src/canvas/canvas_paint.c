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

#include "canvas/canvas_paint.h"

#include "canvas/canvas_markdown.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <pango/pangocairo.h>

#define PAINT_GRID_MIN_PIXEL_SPACING 6.0
#define PAINT_GRID_DOT_PIXELS 1.5
#define PAINT_ARROW_LENGTH 14.0
#define PAINT_ARROW_HALF_WIDTH 5.0

dt_canvas_paint_options_t dt_canvas_paint_options_display(dt_canvas_surface_cache_t *cache, double units_per_pixel,
                                                          dt_canvas_rect_t clip)
{
  dt_canvas_paint_options_t options;
  options.for_display = TRUE;
  options.cache = cache;
  options.draw_background = TRUE;
  options.draw_grid = TRUE;
  options.draw_placeholders = TRUE;
  options.units_per_pixel = units_per_pixel > 0.0 ? units_per_pixel : 1.0;
  options.clip = clip;
  return options;
}

dt_canvas_paint_options_t dt_canvas_paint_options_export(dt_canvas_surface_cache_t *cache, double units_per_pixel,
                                                         dt_canvas_rect_t clip)
{
  dt_canvas_paint_options_t options = dt_canvas_paint_options_display(cache, units_per_pixel, clip);
  options.for_display = FALSE;
  options.draw_grid = FALSE;
  options.draw_placeholders = FALSE;
  return options;
}

static void _set_color(cairo_t *cr, const dt_canvas_color_t *color, const gboolean for_display)
{
  double rgb[3] = { 0.0, 0.0, 0.0 };
  dt_canvas_render_color(color, for_display, rgb);
  cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], CLAMP(color->alpha, 0.0f, 1.0f));
}

static gboolean _rect_intersects(const dt_canvas_rect_t *clip, const dt_canvas_rect_t *rect)
{
  if(clip->width <= 0.0 || clip->height <= 0.0) return TRUE;
  return rect->x < clip->x + clip->width && rect->x + rect->width > clip->x && rect->y < clip->y + clip->height
         && rect->y + rect->height > clip->y;
}

/* --- grid ------------------------------------------------------------------- */

static void _paint_grid(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  if(!(canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) || canvas->grid_size <= 0.0f) return;
  if(options->clip.width <= 0.0 || options->clip.height <= 0.0) return;
  double step = canvas->grid_size;
  // Too dense on screen: show every nth crossing instead of a grey wash.
  while(step / options->units_per_pixel < PAINT_GRID_MIN_PIXEL_SPACING) step *= 2.0;
  const double radius = PAINT_GRID_DOT_PIXELS * options->units_per_pixel;
  const double first_x = floor(options->clip.x / step) * step;
  const double first_y = floor(options->clip.y / step) * step;
  const double last_x = options->clip.x + options->clip.width;
  const double last_y = options->clip.y + options->clip.height;
  // Bound the dot count in case the clip is huge relative to the step.
  const double columns = (last_x - first_x) / step;
  const double rows = (last_y - first_y) / step;
  if(columns * rows > 250000.0) return;

  cairo_save(cr);
  const dt_canvas_color_t background = canvas->background;
  const float luminance = 0.2126f * background.red + 0.7152f * background.green + 0.0722f * background.blue;
  const float dot = luminance > 0.5f ? luminance - 0.25f : luminance + 0.25f;
  const dt_canvas_color_t dot_color = dt_canvas_color(dot, dot, dot, 1.0f);
  _set_color(cr, &dot_color, options->for_display);
  for(double y = first_y; y <= last_y; y += step)
  {
    for(double x = first_x; x <= last_x; x += step)
    {
      cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    }
  }
  cairo_restore(cr);
}

/* --- frames ----------------------------------------------------------------- */

static void _paint_border(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                          const dt_canvas_paint_options_t *options)
{
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &color, &width);
  if(width <= 0.0f || color.alpha <= 0.0f) return;
  // The border sits outside the frame, so it never covers the picture.
  cairo_save(cr);
  _set_color(cr, &color, options->for_display);
  cairo_set_line_width(cr, width);
  cairo_rectangle(cr, -object->width * 0.5 - width * 0.5, -object->height * 0.5 - width * 0.5,
                  object->width + width, object->height + width);
  cairo_stroke(cr);
  cairo_restore(cr);
}

static void _paint_placeholder(cairo_t *cr, const dt_canvas_object_t *object, const dt_canvas_paint_options_t *options)
{
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.35);
  cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.6);
  cairo_set_line_width(cr, 1.5 * options->units_per_pixel);
  cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
  cairo_stroke(cr);
  cairo_move_to(cr, -half_width, -half_height);
  cairo_line_to(cr, half_width, half_height);
  cairo_move_to(cr, half_width, -half_height);
  cairo_line_to(cr, -half_width, half_height);
  cairo_stroke(cr);
  cairo_restore(cr);
}

static void _paint_image(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                         const dt_canvas_paint_options_t *options)
{
  cairo_surface_t *surface = NULL;
  cairo_surface_t *owned = NULL;
  if(!IS_NULL_PTR(options->cache))
  {
    surface = dt_canvas_surface_cache_get(options->cache, object);
  }
  else
  {
    owned = dt_canvas_render_decode(object->image.jpeg, options->for_display);
    surface = owned;
  }
  _paint_border(cr, canvas, object, options);
  if(IS_NULL_PTR(surface))
  {
    if(options->draw_placeholders) _paint_placeholder(cr, object, options);
    return;
  }
  const double surface_width = cairo_image_surface_get_width(surface);
  const double surface_height = cairo_image_surface_get_height(surface);
  if(surface_width > 0.0 && surface_height > 0.0)
  {
    cairo_save(cr);
    cairo_rectangle(cr, -object->width * 0.5, -object->height * 0.5, object->width, object->height);
    cairo_clip(cr);
    cairo_translate(cr, -object->width * 0.5, -object->height * 0.5);
    cairo_scale(cr, object->width / surface_width, object->height / surface_height);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    // Set AFTER cairo_set_source_surface(): the filter belongs to the pattern that scales.
    const double downscale = (object->width / surface_width) / options->units_per_pixel;
    cairo_pattern_set_filter(cairo_get_source(cr), downscale < 0.5 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  if(!IS_NULL_PTR(owned)) cairo_surface_destroy(owned);
}

static PangoLayout *_text_layout(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font = pango_font_description_from_string(dt_canvas_text_effective_font(canvas, object));
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  const double padding = object->text.padding > 0.0f ? object->text.padding : 0.0;
  const double text_width = fmax(object->width - 2.0 * padding, 1.0);
  pango_layout_set_width(layout, (int)(text_width * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  gchar *markup = dt_canvas_markdown_to_pango(dt_canvas_text_get_markdown(object));
  pango_layout_set_markup(layout, markup, -1);
  dt_free(markup);
  return layout;
}

static void _paint_text(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                        const dt_canvas_paint_options_t *options)
{
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  _paint_border(cr, canvas, object, options);
  if(object->text.background.alpha > 0.0f)
  {
    cairo_save(cr);
    _set_color(cr, &object->text.background, options->for_display);
    cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  cairo_save(cr);
  cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
  cairo_clip(cr);
  const double padding = object->text.padding > 0.0f ? object->text.padding : 0.0;
  cairo_translate(cr, -half_width + padding, -half_height + padding);
  PangoLayout *layout = _text_layout(cr, canvas, object);
  _set_color(cr, &object->text.text_color, options->for_display);
  pango_cairo_update_layout(cr, layout);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
  cairo_restore(cr);
}

double dt_canvas_paint_text_natural_height(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return 0.0;
  PangoLayout *layout = _text_layout(cr, canvas, object);
  int layout_width = 0;
  int layout_height = 0;
  pango_layout_get_pixel_size(layout, &layout_width, &layout_height);
  g_object_unref(layout);
  const double padding = object->text.padding > 0.0f ? object->text.padding : 0.0;
  return layout_height + 2.0 * padding;
}

/* --- connectors --------------------------------------------------------------- */

static void _paint_arrow_head(cairo_t *cr, const double tip_x, const double tip_y, const double from_x,
                              const double from_y, const double scale)
{
  const double angle = atan2(tip_y - from_y, tip_x - from_x);
  const double length = PAINT_ARROW_LENGTH * scale;
  const double half_width = PAINT_ARROW_HALF_WIDTH * scale;
  const double base_x = tip_x - cos(angle) * length;
  const double base_y = tip_y - sin(angle) * length;
  const double normal_x = -sin(angle) * half_width;
  const double normal_y = cos(angle) * half_width;
  cairo_move_to(cr, tip_x, tip_y);
  cairo_line_to(cr, base_x + normal_x, base_y + normal_y);
  cairo_line_to(cr, base_x - normal_x, base_y - normal_y);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void _paint_connector(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                             const dt_canvas_paint_options_t *options)
{
  dt_canvas_route_t route;
  if(!dt_canvas_connector_route(canvas, object, &route)) return;
  const double line_width = object->connector.line_width > 0.0f ? object->connector.line_width : 2.0;
  // Arrow heads are sized to the line, so a thick connector gets a proportionate head.
  const double head_scale = fmax(line_width / 2.0, 1.0);

  cairo_save(cr);
  _set_color(cr, &object->connector.color, options->for_display);
  cairo_set_line_width(cr, line_width);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  if(object->connector.style & DT_CANVAS_CONNECTOR_DASHED)
  {
    const double dashes[2] = { 4.0 * line_width, 3.0 * line_width };
    cairo_set_dash(cr, dashes, 2, 0.0);
  }
  cairo_move_to(cr, route.from_x, route.from_y);
  if(route.routing == DT_CANVAS_ROUTING_CUBIC)
  {
    cairo_curve_to(cr, route.control1_x, route.control1_y, route.control2_x, route.control2_y, route.to_x, route.to_y);
  }
  else
  {
    for(int idx = 1; idx < route.point_count; idx++) cairo_line_to(cr, route.points[2 * idx], route.points[2 * idx + 1]);
  }
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0.0);

  // A head points along the route's tangent at its end: the normal it left the frame by.
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_END)
    _paint_arrow_head(cr, route.to_x, route.to_y, route.to_x + route.to_normal_x, route.to_y + route.to_normal_y,
                      head_scale);
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_START)
    _paint_arrow_head(cr, route.from_x, route.from_y, route.from_x + route.from_normal_x,
                      route.from_y + route.from_normal_y, head_scale);
  cairo_restore(cr);
}

/* --- entry points ------------------------------------------------------------- */

void dt_canvas_paint_object(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                            const dt_canvas_paint_options_t *options)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || IS_NULL_PTR(options)) return;
  if(object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN) return;
  if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    _paint_connector(cr, canvas, object, options);
    return;
  }
  const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
  dt_canvas_color_t border_color;
  float border_width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &border_color, &border_width);
  dt_canvas_rect_t reach = bounds;
  reach.x -= border_width;
  reach.y -= border_width;
  reach.width += 2.0 * border_width;
  reach.height += 2.0 * border_width;
  if(!_rect_intersects(&options->clip, &reach)) return;

  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  if(object->kind == DT_CANVAS_OBJECT_IMAGE)
    _paint_image(cr, canvas, object, options);
  else if(object->kind == DT_CANVAS_OBJECT_TEXT)
    _paint_text(cr, canvas, object, options);
  cairo_restore(cr);
}

void dt_canvas_paint(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(canvas) || IS_NULL_PTR(options)) return;
  if(options->draw_background)
  {
    cairo_save(cr);
    _set_color(cr, &canvas->background, options->for_display);
    if(options->clip.width > 0.0 && options->clip.height > 0.0)
    {
      cairo_rectangle(cr, options->clip.x, options->clip.y, options->clip.width, options->clip.height);
      cairo_fill(cr);
    }
    else
    {
      cairo_paint(cr);
    }
    cairo_restore(cr);
  }
  if(options->draw_grid) _paint_grid(cr, canvas, options);
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    dt_canvas_paint_object(cr, canvas, dt_canvas_object_at(canvas, idx), options);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
