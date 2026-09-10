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
#include "colorprofiles/colorspaces.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define PAINT_GRID_MIN_PIXEL_SPACING 6.0
#define PAINT_GRID_DOT_FRACTION 0.03   ///< dot radius as a fraction of the grid step: it scales with the zoom
#define PAINT_GRID_DOT_MIN_PIXELS 0.75 ///< but never vanishes
#define PAINT_ARROW_LENGTH 14.0
#define PAINT_ARROW_HALF_WIDTH 5.0

static void _paint_pages(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options);
static void _paint_paper(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options);

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
  const double radius = fmax(canvas->grid_size * PAINT_GRID_DOT_FRACTION,
                             PAINT_GRID_DOT_MIN_PIXELS * options->units_per_pixel);
  const double first_x = floor(options->clip.x / step) * step;
  const double first_y = floor(options->clip.y / step) * step;
  const double last_x = options->clip.x + options->clip.width;
  const double last_y = options->clip.y + options->clip.height;
  // Bound the dot count in case the clip is huge relative to the step.
  const double columns = (last_x - first_x) / step;
  const double rows = (last_y - first_y) / step;
  if(columns * rows > 250000.0) return;

  cairo_save(cr);
  _set_color(cr, &canvas->grid_color, options->for_display);
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

/** The pages of the paper tiling that cross the clip, as dashed outlines. */
static void _paint_pages(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  double page_width = 0.0;
  double page_height = 0.0;
  if(!dt_canvas_paper_dimensions(canvas, &page_width, &page_height)) return;
  if(options->clip.width <= 0.0 || options->clip.height <= 0.0) return;
  const int first_col = (int)floor(options->clip.x / page_width);
  const int last_col = (int)floor((options->clip.x + options->clip.width) / page_width);
  const int first_row = (int)floor(options->clip.y / page_height);
  const int last_row = (int)floor((options->clip.y + options->clip.height) / page_height);
  if((double)(last_col - first_col + 1) * (double)(last_row - first_row + 1) > 4096.0) return;
  cairo_save(cr);
  _set_color(cr, &canvas->grid_color, options->for_display);
  cairo_set_line_width(cr, 1.0 * options->units_per_pixel);
  const double dashes[2] = { 8.0 * options->units_per_pixel, 6.0 * options->units_per_pixel };
  cairo_set_dash(cr, dashes, 2, 0.0);
  for(int row = first_row; row <= last_row; row++)
  {
    for(int col = first_col; col <= last_col; col++)
    {
      const dt_canvas_rect_t page = dt_canvas_page_rect(canvas, col, row);
      cairo_rectangle(cr, page.x, page.y, page.width, page.height);
    }
  }
  cairo_stroke(cr);
  cairo_restore(cr);
}

/* --- paper textures ------------------------------------------------------------ */

/*
 * A paper is a random field with a chosen spectrum, synthesised in the frequency domain:
 * white Gaussian noise, transformed, shaped by a radial amplitude filter, transformed back.
 * The discrete Fourier transform is periodic by construction, so the tile wraps without a
 * seam, and the shaping is what gives each paper its character rather than a lattice of
 * interpolated corners, which is what reads as a mosaic.
 */

#define PAPER_TILE_LOG2 9
#define PAPER_TILE (1 << PAPER_TILE_LOG2)

typedef struct dt_paper_complex_t
{
  double real;
  double imag;
} dt_paper_complex_t;

/** In-place radix-2 FFT of `count` samples (a power of two), stride `stride`, inverse when `inverse`. */
static void _fft_1d(dt_paper_complex_t *data, const int count, const int stride, const gboolean inverse)
{
  // Bit reversal.
  for(int idx = 1, reversed = 0; idx < count; idx++)
  {
    int bit = count >> 1;
    for(; reversed & bit; bit >>= 1) reversed ^= bit;
    reversed ^= bit;
    if(idx < reversed)
    {
      const dt_paper_complex_t swap = data[idx * stride];
      data[idx * stride] = data[reversed * stride];
      data[reversed * stride] = swap;
    }
  }
  for(int length = 2; length <= count; length <<= 1)
  {
    const double angle = 2.0 * M_PI / length * (inverse ? 1.0 : -1.0);
    const double root_real = cos(angle);
    const double root_imag = sin(angle);
    for(int start = 0; start < count; start += length)
    {
      double twiddle_real = 1.0;
      double twiddle_imag = 0.0;
      for(int idx = 0; idx < length / 2; idx++)
      {
        dt_paper_complex_t *even = &data[(start + idx) * stride];
        dt_paper_complex_t *odd = &data[(start + idx + length / 2) * stride];
        const double product_real = odd->real * twiddle_real - odd->imag * twiddle_imag;
        const double product_imag = odd->real * twiddle_imag + odd->imag * twiddle_real;
        odd->real = even->real - product_real;
        odd->imag = even->imag - product_imag;
        even->real += product_real;
        even->imag += product_imag;
        const double next_real = twiddle_real * root_real - twiddle_imag * root_imag;
        twiddle_imag = twiddle_real * root_imag + twiddle_imag * root_real;
        twiddle_real = next_real;
      }
    }
  }
  if(inverse)
  {
    for(int idx = 0; idx < count; idx++)
    {
      data[idx * stride].real /= count;
      data[idx * stride].imag /= count;
    }
  }
}

static void _fft_2d(dt_paper_complex_t *data, const int size, const gboolean inverse)
{
  for(int row = 0; row < size; row++) _fft_1d(data + (size_t)row * size, size, 1, inverse);
  for(int col = 0; col < size; col++) _fft_1d(data + col, size, size, inverse);
}

/** A Gaussian deviate from a seeded generator, so a paper is the same paper every time. */
static double _paper_gaussian(GRand *generator)
{
  const double uniform_a = fmax(g_rand_double(generator), 1e-12);
  const double uniform_b = g_rand_double(generator);
  return sqrt(-2.0 * log(uniform_a)) * cos(2.0 * M_PI * uniform_b);
}

/**
 * A periodic random field of unit variance, `size` square: white noise shaped by
 * 1 / (1 + (k / knee)^slope), a plateau below the knee and a power-law fall-off above it.
 * The knee sets the size of the features, the slope how soft they are.
 */
static double *_paper_field(const int size, const double knee, const double slope, const guint32 seed)
{
  dt_paper_complex_t *spectrum = g_new0(dt_paper_complex_t, (size_t)size * size);
  GRand *generator = g_rand_new_with_seed(seed);
  for(size_t idx = 0; idx < (size_t)size * size; idx++) spectrum[idx].real = _paper_gaussian(generator);
  g_rand_free(generator);
  _fft_2d(spectrum, size, FALSE);
  for(int row = 0; row < size; row++)
  {
    const double frequency_y = row <= size / 2 ? row : row - size;
    for(int col = 0; col < size; col++)
    {
      const double frequency_x = col <= size / 2 ? col : col - size;
      const double frequency = hypot(frequency_x, frequency_y);
      const double gain = 1.0 / (1.0 + pow(frequency / knee, slope));
      spectrum[(size_t)row * size + col].real *= gain;
      spectrum[(size_t)row * size + col].imag *= gain;
    }
  }
  spectrum[0].real = 0.0; // no mean: the base colour carries it
  spectrum[0].imag = 0.0;
  _fft_2d(spectrum, size, TRUE);
  double *field = g_new(double, (size_t)size * size);
  double variance = 0.0;
  for(size_t idx = 0; idx < (size_t)size * size; idx++)
  {
    field[idx] = spectrum[idx].real;
    variance += field[idx] * field[idx];
  }
  dt_free(spectrum);
  const double deviation = sqrt(variance / ((double)size * size));
  if(deviation > 0.0)
  {
    for(size_t idx = 0; idx < (size_t)size * size; idx++) field[idx] /= deviation;
  }
  return field;
}

/** The paper's sRGB pixels, `size` square. */
static uint8_t *_paper_pixels(const dt_canvas_background_t style, const int size)
{
  double base_r = 1.0;
  double base_g = 1.0;
  double base_b = 1.0;
  double *mottle = NULL;
  double *tooth = NULL;
  double *grain = NULL;
  double mottle_amplitude = 0.0;
  double tooth_amplitude = 0.0;
  double grain_amplitude = 0.0;
  if(style == DT_CANVAS_BACKGROUND_MOLESKINE)
  {
    // Ivory; soft, broad clouds and a whisper of fibre.
    base_r = 0.957;
    base_g = 0.925;
    base_b = 0.847;
    mottle = _paper_field(size, size / 40.0, 2.2, 1101u);
    grain = _paper_field(size, size / 3.0, 1.2, 1103u);
    mottle_amplitude = 0.018;
    grain_amplitude = 0.006;
  }
  else
  {
    // Pure white; a thick tooth of hollows between peaks, and a fine grain.
    tooth = _paper_field(size, size / 18.0, 1.7, 2201u);
    grain = _paper_field(size, size / 2.5, 1.0, 2203u);
    tooth_amplitude = 0.075;
    grain_amplitude = 0.012;
  }
  uint8_t *rgba = g_malloc((size_t)size * size * 4);
  for(size_t idx = 0; idx < (size_t)size * size; idx++)
  {
    double relief = 0.0;
    if(!IS_NULL_PTR(mottle)) relief += mottle[idx] * mottle_amplitude;
    if(!IS_NULL_PTR(tooth))
    {
      // Paper is white at its peaks: the tooth only carves hollows, the deeper the rarer.
      const double hollow = fmin(tooth[idx], 0.0);
      relief += -(hollow * hollow) * tooth_amplitude;
    }
    if(!IS_NULL_PTR(grain)) relief += grain[idx] * grain_amplitude;
    rgba[4 * idx + 0] = (uint8_t)lround(CLAMP(base_r + relief, 0.0, 1.0) * 255.0);
    rgba[4 * idx + 1] = (uint8_t)lround(CLAMP(base_g + relief, 0.0, 1.0) * 255.0);
    rgba[4 * idx + 2] = (uint8_t)lround(CLAMP(base_b + relief, 0.0, 1.0) * 255.0);
    rgba[4 * idx + 3] = 255;
  }
  dt_free(mottle);
  dt_free(tooth);
  dt_free(grain);
  return rgba;
}

/**
 * The paper as a seamless tile, in display or sRGB colours, at the size it shows on screen.
 * The base tile is synthesised once per style; a version scaled to the current zoom is cached
 * per style, target and display profile generation, so the plane is filled with an unscaled
 * repeat -- cairo's fast path -- rather than a transformed one on every frame.
 */
static cairo_surface_t *_paper_tile(const uint32_t style, const gboolean for_display, const int scaled_size)
{
  if(style != DT_CANVAS_BACKGROUND_MOLESKINE && style != DT_CANVAS_BACKGROUND_WATERCOLOUR) return NULL;
  static uint8_t *base[3] = { NULL, NULL, NULL };
  static cairo_surface_t *cached[3][2] = { { NULL, NULL }, { NULL, NULL }, { NULL, NULL } };
  static uint64_t cached_generation[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
  static int cached_size[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
  static GMutex lock;
  dt_colorprofiles_settings_t settings;
  dt_colorprofiles_get_settings(&settings);
  const uint64_t generation = for_display ? settings.generation + 1 : 1;
  const int target = for_display ? 1 : 0;

  g_mutex_lock(&lock);
  if(!IS_NULL_PTR(cached[style][target]) && cached_generation[style][target] == generation
     && cached_size[style][target] == scaled_size)
  {
    cairo_surface_t *tile = cached[style][target];
    g_mutex_unlock(&lock);
    return tile;
  }
  if(IS_NULL_PTR(base[style])) base[style] = _paper_pixels((dt_canvas_background_t)style, PAPER_TILE);

  // Colour-manage the base once per target, then scale it to what the zoom shows.
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, PAPER_TILE);
  uint8_t *bgra = g_malloc((size_t)stride * PAPER_TILE);
  if(!for_display || !dt_colorprofiles_rgba8_to_display_bgra8(base[style], bgra, PAPER_TILE, PAPER_TILE, DT_COLORSPACE_SRGB))
  {
    for(size_t idx = 0; idx < (size_t)PAPER_TILE * PAPER_TILE; idx++)
    {
      bgra[4 * idx + 0] = base[style][4 * idx + 2];
      bgra[4 * idx + 1] = base[style][4 * idx + 1];
      bgra[4 * idx + 2] = base[style][4 * idx + 0];
      bgra[4 * idx + 3] = 255;
    }
  }
  cairo_surface_t *unscaled = cairo_image_surface_create_for_data(bgra, CAIRO_FORMAT_RGB24, PAPER_TILE, PAPER_TILE, stride);
  cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, scaled_size, scaled_size);
  cairo_t *cr = cairo_create(tile);
  const double scale = (double)scaled_size / PAPER_TILE;
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, unscaled, 0.0, 0.0);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter(cairo_get_source(cr), scale < 1.0 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(unscaled);
  dt_free(bgra);

  if(!IS_NULL_PTR(cached[style][target])) cairo_surface_destroy(cached[style][target]);
  cached[style][target] = tile;
  cached_generation[style][target] = generation;
  cached_size[style][target] = scaled_size;
  g_mutex_unlock(&lock);
  return tile;
}

/** Fill the clip with the paper: in device space, at an integer offset, the repeat cairo does fastest. */
static void _paint_paper(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  const double pixels_per_unit = 1.0 / options->units_per_pixel;
  const int scaled_size = CLAMP((int)lround(PAPER_TILE * pixels_per_unit), 32, 8192);
  cairo_surface_t *tile = _paper_tile(canvas->background_style, options->for_display, scaled_size);
  if(IS_NULL_PTR(tile)) return;
  // Where the canvas origin lands in device pixels, so the paper stays put under a pan.
  double origin_x = 0.0;
  double origin_y = 0.0;
  cairo_user_to_device(cr, &origin_x, &origin_y);
  cairo_save(cr);
  if(options->clip.width > 0.0 && options->clip.height > 0.0)
  {
    cairo_rectangle(cr, options->clip.x, options->clip.y, options->clip.width, options->clip.height);
    cairo_clip(cr);
  }
  cairo_identity_matrix(cr);
  cairo_pattern_t *pattern = cairo_pattern_create_for_surface(tile);
  cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
  cairo_matrix_t matrix;
  cairo_matrix_init_translate(&matrix, -floor(origin_x), -floor(origin_y));
  cairo_pattern_set_matrix(pattern, &matrix);
  cairo_set_source(cr, pattern);
  cairo_pattern_destroy(pattern);
  cairo_paint(cr);
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
  // The border is part of the frame: it sits inside its edge, and the content is inset by it.
  cairo_save(cr);
  _set_color(cr, &color, options->for_display);
  cairo_set_line_width(cr, width);
  cairo_rectangle(cr, -object->width * 0.5 + width * 0.5, -object->height * 0.5 + width * 0.5,
                  fmax(object->width - width, 0.0), fmax(object->height - width, 0.0));
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
  dt_canvas_color_t border_color;
  float border_width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &border_color, &border_width);
  const double inner_width = fmax(object->width - 2.0 * border_width, 1.0);
  const double inner_height = fmax(object->height - 2.0 * border_width, 1.0);
  if(surface_width > 0.0 && surface_height > 0.0)
  {
    cairo_save(cr);
    cairo_rectangle(cr, -inner_width * 0.5, -inner_height * 0.5, inner_width, inner_height);
    cairo_clip(cr);
    cairo_translate(cr, -inner_width * 0.5, -inner_height * 0.5);
    cairo_scale(cr, inner_width / surface_width, inner_height / surface_height);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    // Set AFTER cairo_set_source_surface(): the filter belongs to the pattern that scales.
    const double downscale = (inner_width / surface_width) / options->units_per_pixel;
    cairo_pattern_set_filter(cairo_get_source(cr), downscale < 0.5 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  if(!IS_NULL_PTR(owned)) cairo_surface_destroy(owned);
}

/** The text sits inside the border and the padding. */
static double _text_inset(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  dt_canvas_color_t border_color;
  float border_width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &border_color, &border_width);
  const double padding = object->text.padding > 0.0f ? object->text.padding : 0.0;
  return padding + border_width;
}

static PangoLayout *_text_layout(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font = pango_font_description_from_string(dt_canvas_text_effective_font(canvas, object));
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  const double padding = _text_inset(canvas, object);
  const double text_width = fmax(object->width - 2.0 * padding, 1.0);
  pango_layout_set_width(layout, (int)(text_width * PANGO_SCALE));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  switch(object->text.align_h)
  {
    case DT_CANVAS_ALIGN_CENTER:
      pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
      break;
    case DT_CANVAS_ALIGN_END:
      pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
      break;
    case DT_CANVAS_ALIGN_JUSTIFY:
      pango_layout_set_justify(layout, TRUE);
      break;
    default:
      break;
  }
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
  const double padding = _text_inset(canvas, object);
  PangoLayout *layout = _text_layout(cr, canvas, object);
  // Vertical alignment: the layout's height against the inner height.
  int layout_width = 0;
  int layout_height = 0;
  pango_layout_get_pixel_size(layout, &layout_width, &layout_height);
  const double inner_height = fmax(object->height - 2.0 * padding, 0.0);
  double offset_y = 0.0;
  if(object->text.align_v == DT_CANVAS_ALIGN_CENTER) offset_y = (inner_height - layout_height) * 0.5;
  else if(object->text.align_v == DT_CANVAS_ALIGN_END) offset_y = inner_height - layout_height;
  cairo_translate(cr, -half_width + padding, -half_height + padding + fmax(offset_y, 0.0));
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
  return layout_height + 2.0 * _text_inset(canvas, object);
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
  if(route.routing == DT_CANVAS_ROUTING_CUBIC && route.segment_count == 2)
  {
    cairo_curve_to(cr, route.control1_x, route.control1_y, route.control2_x, route.control2_y, route.via_x, route.via_y);
    cairo_curve_to(cr, route.control3_x, route.control3_y, route.control4_x, route.control4_y, route.to_x, route.to_y);
  }
  else if(route.routing == DT_CANVAS_ROUTING_CUBIC)
  {
    cairo_curve_to(cr, route.control1_x, route.control1_y, route.control2_x, route.control2_y, route.to_x, route.to_y);
  }
  else
  {
    for(int idx = 1; idx < route.point_count; idx++) cairo_line_to(cr, route.points[2 * idx], route.points[2 * idx + 1]);
  }
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0.0);

  // A head points along the line it ends: the last leg of the route, which for a straight
  // connector is the chord itself and for the others the stub or tangent at that anchor.
  const int last = route.point_count - 1;
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_END)
    _paint_arrow_head(cr, route.to_x, route.to_y, route.points[2 * last - 2], route.points[2 * last - 1], head_scale);
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_START)
    _paint_arrow_head(cr, route.from_x, route.from_y, route.points[2], route.points[3], head_scale);
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
  if(!_rect_intersects(&options->clip, &bounds)) return;

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
  if(options->draw_background && (canvas->background_style == DT_CANVAS_BACKGROUND_MOLESKINE
                                  || canvas->background_style == DT_CANVAS_BACKGROUND_WATERCOLOUR))
  {
    _paint_paper(cr, canvas, options);
  }
  else if(options->draw_background)
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
  if(options->draw_grid) _paint_pages(cr, canvas, options);
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
