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
 * white noise shaped by a radial amplitude filter, transformed back. The discrete transform
 * is periodic by construction, so a sprite wraps without a seam, and the shaping is what
 * gives each paper its character rather than a lattice of interpolated corners, which
 * reads as a mosaic.
 *
 * One sprite repeated shows its period, and sprites sharing a border repeat that border.
 * So several sprites are laid on a half-overlapping grid, each a random sprite in a random
 * orientation at a random phase, blended by Hann windows that sum to one, into a field
 * four sprites wide that is itself periodic: no seam, no border band, and a period four
 * times the sprite's.
 *
 * Every coefficient is drawn from a hash of its frequency, so a sprite synthesised at a
 * higher resolution keeps the same broad features and only adds finer ones: the grain
 * sharpens as the zoom grows instead of the same texture being enlarged.
 */

#define PAPER_TILE 512          ///< a sprite's extent in canvas units, whatever resolution it is synthesised at
#define PAPER_SPRITES 6
#define PAPER_FIELD_MIN_LOG2 8  ///< 256 pixels: the base resolution
#define PAPER_FIELD_MAX_LOG2 9  ///< 512 pixels per sprite: the finest grain the composed field is kept at

typedef struct dt_paper_complex_t
{
  double real;
  double imag;
} dt_paper_complex_t;

/** In-place radix-2 FFT of `count` samples (a power of two), stride `stride`, inverse when `inverse`. */
static void _fft_1d(dt_paper_complex_t *data, const int count, const int stride, const gboolean inverse)
{
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

/** Two uniforms from a frequency's own hash: the same coefficient at every resolution. */
static void _paper_hash_uniforms(const guint32 seed, const int frequency_x, const int frequency_y, double *uniform_a,
                                 double *uniform_b)
{
  guint32 hash = seed ^ ((guint32)frequency_x * 374761393u) ^ ((guint32)frequency_y * 668265263u);
  hash = (hash ^ (hash >> 13)) * 1274126177u;
  hash ^= hash >> 16;
  guint32 second = hash * 2246822519u + 3266489917u;
  second = (second ^ (second >> 15)) * 2654435761u;
  second ^= second >> 13;
  *uniform_a = fmax((hash & 0xFFFFFF) / 16777215.0, 1e-12);
  *uniform_b = (second & 0xFFFFFF) / 16777215.0;
}

/**
 * A periodic random field, `size` square, of one sprite: white noise shaped by
 * 1 / (1 + (k / knee)^slope) in cycles per sprite, a plateau below the knee and a power-law
 * fall-off above it. The knee sets the size of the features, the slope how soft they are.
 * The amplitude is normalised against the power a 256-pixel field holds, so a finer field
 * adds detail without changing the broad features' contrast.
 */
/** The radial gain: a plateau below `knee`, a power-law fall-off above; with `low_cut` > 0, a band above it. */
static double _paper_gain(const double frequency, const double knee, const double slope, const double low_cut)
{
  double gain = 1.0 / (1.0 + pow(frequency / knee, slope));
  if(low_cut > 0.0) gain *= 1.0 - 1.0 / (1.0 + pow(frequency / low_cut, slope));
  return gain;
}

static double *_paper_field_band(const int size, const double knee, const double slope, const double low_cut,
                                 const guint32 seed)
{
  double base_power = 0.0;
  for(int row = -128; row < 128; row++)
  {
    for(int col = -128; col < 128; col++)
    {
      if(row == 0 && col == 0) continue;
      const double gain = _paper_gain(hypot(col, row), knee, slope, low_cut);
      base_power += gain * gain;
    }
  }
  const double normalisation = (double)size * size / sqrt(base_power);

  dt_paper_complex_t *spectrum = g_new0(dt_paper_complex_t, (size_t)size * size);
  for(int row = 0; row < size; row++)
  {
    const int frequency_y = row <= size / 2 ? row : row - size;
    for(int col = 0; col < size; col++)
    {
      const int frequency_x = col <= size / 2 ? col : col - size;
      if(frequency_x == 0 && frequency_y == 0) continue; // no mean: the base colour carries it
      double uniform_a = 0.0;
      double uniform_b = 0.0;
      _paper_hash_uniforms(seed, frequency_x, frequency_y, &uniform_a, &uniform_b);
      const double magnitude = sqrt(-2.0 * log(uniform_a));
      const double gain = normalisation * _paper_gain(hypot(frequency_x, frequency_y), knee, slope, low_cut);
      spectrum[(size_t)row * size + col].real = magnitude * cos(2.0 * M_PI * uniform_b) * gain;
      spectrum[(size_t)row * size + col].imag = magnitude * sin(2.0 * M_PI * uniform_b) * gain;
    }
  }
  _fft_2d(spectrum, size, TRUE);
  // The real part of the transform of a non-symmetric spectrum is the transform of its
  // symmetrised half: real, and still keyed by frequency.
  double *field = g_new(double, (size_t)size * size);
  for(size_t idx = 0; idx < (size_t)size * size; idx++) field[idx] = spectrum[idx].real * M_SQRT2;
  dt_free(spectrum);
  return field;
}

static double *_paper_field(const int size, const double knee, const double slope, const guint32 seed)
{
  return _paper_field_band(size, knee, slope, 0.0, seed);
}

#define PAPER_FIBRES_PER_SPRITE 5000

/**
 * Short fibres in random directions, as a periodic field: segments stamped at random
 * positions, angles and lengths, half darker and half lighter than the sheet, antialiased
 * across their width. Defined in the sprite's own units, so a fibre is the same fibre at
 * every resolution and only sharper at a higher one.
 */
static double *_paper_fibres(const int size, const guint32 seed)
{
  double *field = g_new0(double, (size_t)size * size);
  const double pixels_per_unit = (double)size / PAPER_TILE;
  for(int fibre = 0; fibre < PAPER_FIBRES_PER_SPRITE; fibre++)
  {
    double uniform_a = 0.0;
    double uniform_b = 0.0;
    double uniform_c = 0.0;
    double uniform_d = 0.0;
    _paper_hash_uniforms(seed, fibre, 1, &uniform_a, &uniform_b);
    _paper_hash_uniforms(seed, fibre, 2, &uniform_c, &uniform_d);
    const double center_x = uniform_a * size;
    const double center_y = uniform_b * size;
    const double angle = uniform_c * M_PI;
    // 6 to 22 units long, about a unit wide, as dark as light on average.
    const double half_length = (6.0 + 16.0 * uniform_d) * 0.5 * pixels_per_unit;
    const double half_width = fmax(0.55 * pixels_per_unit, 0.6);
    const double sign = (fibre & 1) ? 1.0 : -1.0;
    const double direction_x = cos(angle);
    const double direction_y = sin(angle);
    const int reach = (int)ceil(half_length + half_width + 1.0);
    for(int y = (int)floor(center_y) - reach; y <= (int)ceil(center_y) + reach; y++)
    {
      for(int x = (int)floor(center_x) - reach; x <= (int)ceil(center_x) + reach; x++)
      {
        // Distance from the pixel centre to the segment.
        const double offset_x = x + 0.5 - center_x;
        const double offset_y = y + 0.5 - center_y;
        const double along = CLAMP(offset_x * direction_x + offset_y * direction_y, -half_length, half_length);
        const double distance = hypot(offset_x - along * direction_x, offset_y - along * direction_y);
        const double coverage = CLAMP(half_width + 0.5 - distance, 0.0, 1.0);
        if(coverage <= 0.0) continue;
        const int wrapped_x = ((x % size) + size) % size;
        const int wrapped_y = ((y % size) + size) % size;
        field[(size_t)wrapped_y * size + wrapped_x] += sign * coverage;
      }
    }
  }
  return field;
}

static gboolean _is_paper(const uint32_t style)
{
  return style >= DT_CANVAS_BACKGROUND_MOLESKINE && style < DT_CANVAS_BACKGROUND_LAST;
}

/** One sprite's relief, `size` square, before the colour: the paper's random components combined. */
static double *_paper_relief(const dt_canvas_background_t style, const int size, const int variant)
{
  const guint32 seed = 1000u * (guint32)(variant + 1);
  double *relief = g_new0(double, (size_t)size * size);
  if(style == DT_CANVAS_BACKGROUND_MOLESKINE)
  {
    // Fine, soft clouds; short fibres in every direction, that show as the zoom lets them; a whisper of grain.
    double *mottle = _paper_field(size, 40.0, 2.0, seed + 101u);
    double *fibres = _paper_fibres(size, seed + 105u);
    double *grain = _paper_field(size, 160.0, 1.1, seed + 103u);
    for(size_t idx = 0; idx < (size_t)size * size; idx++)
      relief[idx] = mottle[idx] * 0.011 + fibres[idx] * 0.012 + grain[idx] * 0.0025;
    dt_free(mottle);
    dt_free(fibres);
    dt_free(grain);
  }
  else if(style == DT_CANVAS_BACKGROUND_WATERCOLOUR)
  {
    // A tooth of shallow hollows between peaks -- paper is white at its peaks, so the tooth
    // only carves, and no deeper than the saturation allows -- a band of rounded pores, and
    // a fine, quiet grain.
    double *tooth = _paper_field(size, 45.0, 1.8, seed + 201u);
    double *pores = _paper_field_band(size, 320.0, 2.5, 110.0, seed + 205u);
    double *grain = _paper_field(size, 200.0, 1.0, seed + 203u);
    for(size_t idx = 0; idx < (size_t)size * size; idx++)
    {
      const double hollow = fmin(tooth[idx], 0.0);
      relief[idx] = -0.06 * (1.0 - exp(-hollow * hollow * 0.5)) + pores[idx] * 0.008 + grain[idx] * 0.003;
    }
    dt_free(tooth);
    dt_free(pores);
    dt_free(grain);
  }
  else if(style == DT_CANVAS_BACKGROUND_EMBOSSED)
  {
    // The random part of a wove sheet: a mottle and fibres; the mesh comes after the blend,
    // and takes its wobble from this very relief.
    double *mottle = _paper_field(size, 36.0, 2.0, seed + 301u);
    double *fibres = _paper_fibres(size, seed + 305u);
    for(size_t idx = 0; idx < (size_t)size * size; idx++) relief[idx] = mottle[idx] * 0.01 + fibres[idx] * 0.009;
    dt_free(mottle);
    dt_free(fibres);
  }
  else if(style == DT_CANVAS_BACKGROUND_JAPANESE)
  {
    // Large soft clouds, a little more contrast than watercolour, and long wrinkles: the
    // zero crossings of a low-frequency field are long curved lines, lit as ridges, and
    // the same lines at every resolution.
    double *clouds = _paper_field(size, 9.0, 2.2, seed + 401u);
    double *wrinkle_field = _paper_field(size, 14.0, 2.5, seed + 405u);
    double *grain = _paper_field(size, 180.0, 1.0, seed + 403u);
    for(size_t idx = 0; idx < (size_t)size * size; idx++)
    {
      const double ridge = exp(-wrinkle_field[idx] * wrinkle_field[idx] * 80.0);
      relief[idx] = clouds[idx] * 0.018 + ridge * 0.1 + grain[idx] * 0.002;
    }
    dt_free(clouds);
    dt_free(wrinkle_field);
    dt_free(grain);
  }
  return relief;
}

#define PAPER_WEFT_PITCH 6.0  ///< the mesh's weft threads, across the sheet: close and pressed in deep
#define PAPER_WARP_PITCH 12.0 ///< its warp threads, along the sheet: sparser and fainter

/**
 * The mesh's imprint at a point of the composed field, in canvas units: a groove along each
 * thread, the weft closer and deeper than the warp. `wobble` -- the sheet's own relief at
 * that point, fibres included -- bends the threads and varies their pressure, so the
 * imprint is that of fibres pressed on a mesh rather than a print of the mesh itself.
 */
static double _paper_mesh(const double unit_x, const double unit_y, const double wobble)
{
  const double bend = wobble * 15.0;
  const double pressure = CLAMP(1.0 + wobble * 8.0, 0.7, 1.3);
  // A low power makes a wide groove: the weft is thick and deep, the warp thin and faint.
  const double weft = fmax(cos(2.0 * M_PI * (unit_y + bend) / PAPER_WEFT_PITCH), 0.0);
  const double warp = fmax(cos(2.0 * M_PI * (unit_x - bend) / PAPER_WARP_PITCH), 0.0);
  return -pressure * (0.07 * pow(weft, 2.0) + 0.01 * pow(warp, 4.0));
}

#define PAPER_CELLS 6       ///< sprites per side of the composed field: its period is PAPER_CELLS sprites
#define PAPER_JITTER 0.35   ///< how far, in cells, a placement may stray from its cell centre

/** A placement's random choices, from a hash of its cell: stable across resolutions. */
static void _paper_placement(const int col, const int row, int *variant, int *orientation, double *phase_x,
                             double *phase_y, double *jitter_x, double *jitter_y)
{
  guint32 hash = (guint32)col * 2654435761u ^ (guint32)row * 2246822519u ^ 0x9E3779B9u;
  hash = (hash ^ (hash >> 15)) * 1274126177u;
  hash ^= hash >> 13;
  guint32 second = hash * 2246822519u + 3266489917u;
  second = (second ^ (second >> 15)) * 2654435761u;
  second ^= second >> 13;
  *variant = (int)(hash % PAPER_SPRITES);
  *orientation = (int)((hash >> 4) % 8);
  *phase_x = ((hash >> 8) & 0xFFF) / 4096.0;
  *phase_y = ((hash >> 20) & 0xFFF) / 4096.0;
  *jitter_x = (((second) & 0xFFF) / 4096.0 - 0.5) * 2.0 * PAPER_JITTER;
  *jitter_y = (((second >> 12) & 0xFFF) / 4096.0 - 0.5) * 2.0 * PAPER_JITTER;
}

/** A sprite sample at (x, y) after the placement's orientation and phase, wrapping. */
static double _paper_sample(const double *sprite, const int size, const int orientation, const int phase_x,
                            const int phase_y, int x, int y)
{
  // Eight orientations: four quarter turns, each with or without a mirror.
  if(orientation & 1)
  {
    const int swap = x;
    x = y;
    y = swap;
  }
  if(orientation & 2) x = size - 1 - x;
  if(orientation & 4) y = size - 1 - y;
  x = ((x + phase_x) % size + size) % size;
  y = ((y + phase_y) % size + size) % size;
  return sprite[(size_t)y * size + x];
}

/**
 * The composed field, PAPER_CELLS sprites square and periodic: sprites twice a cell wide,
 * one per cell, each a random sprite in a random orientation at a random phase, its centre
 * jittered off the cell's, weighted by a two-dimensional Hann window. The weights are summed
 * and divided out, so however the placements stray the blend is exact: neither a seam, nor a
 * border band, nor the lattice of window centres a regular grid leaves for a trained eye.
 */
static double *_paper_compose(const dt_canvas_background_t style, const int sprite_size)
{
  double *sprites[PAPER_SPRITES];
  for(int variant = 0; variant < PAPER_SPRITES; variant++) sprites[variant] = _paper_relief(style, sprite_size, variant);
  const int total = PAPER_CELLS * sprite_size;
  double *field = g_new0(double, (size_t)total * total);
  double *weights = g_new0(double, (size_t)total * total);
  double *window = g_new(double, 2 * (size_t)sprite_size);
  for(int idx = 0; idx < 2 * sprite_size; idx++)
    window[idx] = 0.5 * (1.0 - cos(2.0 * M_PI * (idx + 0.5) / (2.0 * sprite_size)));

  for(int row = 0; row < PAPER_CELLS; row++)
  {
    for(int col = 0; col < PAPER_CELLS; col++)
    {
      int variant = 0;
      int orientation = 0;
      double phase_x = 0.0;
      double phase_y = 0.0;
      double jitter_x = 0.0;
      double jitter_y = 0.0;
      _paper_placement(col, row, &variant, &orientation, &phase_x, &phase_y, &jitter_x, &jitter_y);
      const int shift_x = (int)(phase_x * sprite_size);
      const int shift_y = (int)(phase_y * sprite_size);
      const int origin_x = col * sprite_size - sprite_size / 2 + (int)lround(jitter_x * sprite_size);
      const int origin_y = row * sprite_size - sprite_size / 2 + (int)lround(jitter_y * sprite_size);
      for(int y = 0; y < 2 * sprite_size; y++)
      {
        const int field_y = ((origin_y + y) % total + total) % total;
        for(int x = 0; x < 2 * sprite_size; x++)
        {
          const int field_x = ((origin_x + x) % total + total) % total;
          const double weight = window[x] * window[y];
          field[(size_t)field_y * total + field_x]
              += weight * _paper_sample(sprites[variant], sprite_size, orientation, shift_x, shift_y, x, y);
          weights[(size_t)field_y * total + field_x] += weight;
        }
      }
    }
  }
  for(size_t idx = 0; idx < (size_t)total * total; idx++)
  {
    if(weights[idx] > 1e-6) field[idx] /= weights[idx];
  }
  dt_free(weights);
  dt_free(window);
  for(int variant = 0; variant < PAPER_SPRITES; variant++) dt_free(sprites[variant]);
  return field;
}

/** The composed paper's sRGB pixels, PAPER_CELLS * sprite_size square. */
static uint8_t *_paper_pixels(const dt_canvas_background_t style, const int sprite_size)
{
  double base_r = 1.0;
  double base_g = 1.0;
  double base_b = 1.0;
  if(style == DT_CANVAS_BACKGROUND_MOLESKINE)
  {
    // A pale cream, sRGB; the display transform takes it from there, so a wide-gamut screen
    // does not show the raw numbers, which read far yellower.
    base_r = 0.961;
    base_g = 0.941;
    base_b = 0.886;
  }
  else if(style == DT_CANVAS_BACKGROUND_EMBOSSED)
  {
    base_r = 0.965;
    base_g = 0.962;
    base_b = 0.950;
  }
  else if(style == DT_CANVAS_BACKGROUND_JAPANESE)
  {
    base_r = 0.972;
    base_g = 0.962;
    base_b = 0.935;
  }
  const int total = PAPER_CELLS * sprite_size;
  const double pixels_per_unit = (double)sprite_size / PAPER_TILE;
  double *field = _paper_compose(style, sprite_size);
  uint8_t *pixels = g_malloc((size_t)total * total * 4);
  for(size_t idx = 0; idx < (size_t)total * total; idx++)
  {
    double relief = field[idx];
    // The mesh is stamped over the blended field in absolute coordinates, so it stays one
    // mesh across placements whatever their phase and jitter; its pitch divides the period.
    if(style == DT_CANVAS_BACKGROUND_EMBOSSED)
      relief += _paper_mesh((idx % total) / pixels_per_unit, (idx / total) / pixels_per_unit, field[idx]);
    pixels[4 * idx + 0] = (uint8_t)lround(CLAMP(base_r + relief, 0.0, 1.0) * 255.0);
    pixels[4 * idx + 1] = (uint8_t)lround(CLAMP(base_g + relief, 0.0, 1.0) * 255.0);
    pixels[4 * idx + 2] = (uint8_t)lround(CLAMP(base_b + relief, 0.0, 1.0) * 255.0);
    pixels[4 * idx + 3] = 255;
  }
  dt_free(field);
  return pixels;
}

typedef struct dt_paper_cache_t
{
  uint8_t *fields[PAPER_FIELD_MAX_LOG2 + 1]; ///< the composed sRGB paper per sprite resolution
  cairo_surface_t *tile[2];                  ///< scaled, colour-managed, per target
  uint64_t generation[2];
  int size[2];
} dt_paper_cache_t;

/**
 * The composed paper as one seamless tile, PAPER_CELLS sprites wide, in display or sRGB
 * colours, at the size it shows on screen. The sprites are synthesised at the smallest
 * power of two holding the sprite's on-screen size, so zooming in reveals finer grain;
 * kept per resolution, and the scaled, colour-managed tile per style, target and display
 * profile generation.
 */
static cairo_surface_t *_paper_tile(const uint32_t style, const gboolean for_display, const int sprite_scaled)
{
  if(!_is_paper(style)) return NULL;
  static dt_paper_cache_t caches[DT_CANVAS_BACKGROUND_LAST];
  static GMutex lock;
  dt_paper_cache_t *cache = &caches[style];
  dt_colorprofiles_settings_t settings;
  dt_colorprofiles_get_settings(&settings);
  const uint64_t generation = for_display ? settings.generation + 1 : 1;
  const int target = for_display ? 1 : 0;
  // Never above the field's own resolution: past it, the painter scales each cell up instead
  // of a tile that would grow with the square of the zoom.
  const int scaled_size = MIN(PAPER_CELLS * sprite_scaled, PAPER_CELLS * (1 << PAPER_FIELD_MAX_LOG2));

  g_mutex_lock(&lock);
  if(!IS_NULL_PTR(cache->tile[target]) && cache->generation[target] == generation && cache->size[target] == scaled_size)
  {
    g_mutex_unlock(&lock);
    return cache->tile[target];
  }
  int field_log2 = PAPER_FIELD_MIN_LOG2;
  while(field_log2 < PAPER_FIELD_MAX_LOG2 && (1 << field_log2) < sprite_scaled) field_log2++;
  const int sprite_size = 1 << field_log2;
  const int field_size = PAPER_CELLS * sprite_size;
  if(IS_NULL_PTR(cache->fields[field_log2]))
    cache->fields[field_log2] = _paper_pixels((dt_canvas_background_t)style, sprite_size);
  const uint8_t *base = cache->fields[field_log2];

  // Colour-manage the field once per target, then scale it to what the zoom shows.
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, field_size);
  uint8_t *bgra = g_malloc((size_t)stride * field_size);
  if(!for_display || !dt_colorprofiles_rgba8_to_display_bgra8(base, bgra, field_size, field_size, DT_COLORSPACE_SRGB))
  {
    for(size_t idx = 0; idx < (size_t)field_size * field_size; idx++)
    {
      bgra[4 * idx + 0] = base[4 * idx + 2];
      bgra[4 * idx + 1] = base[4 * idx + 1];
      bgra[4 * idx + 2] = base[4 * idx + 0];
      bgra[4 * idx + 3] = 255;
    }
  }
  cairo_surface_t *unscaled = cairo_image_surface_create_for_data(bgra, CAIRO_FORMAT_RGB24, field_size, field_size, stride);
  cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, scaled_size, scaled_size);
  cairo_t *cr = cairo_create(tile);
  const double scale = (double)scaled_size / field_size;
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, unscaled, 0.0, 0.0);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter(cairo_get_source(cr), scale < 1.0 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(unscaled);
  dt_free(bgra);
  if(!IS_NULL_PTR(cache->tile[target])) cairo_surface_destroy(cache->tile[target]);
  cache->tile[target] = tile;
  cache->generation[target] = generation;
  cache->size[target] = scaled_size;
  g_mutex_unlock(&lock);
  return tile;
}

#define PAPER_DITHER_TILE 256
#define PAPER_DITHER_SIGMA 0.008 ///< at zoom 1, the standard deviation of the multiplicative noise

/**
 * A tile of achromatic Gaussian noise around one, for a multiply blend: pixel values of
 * 1 - sigma - sigma * g, so the mean factor is 1 - sigma and a multiply cannot exceed one.
 * Cached per sigma, which is quantised so a smooth zoom does not regenerate it every frame.
 */
static cairo_surface_t *_paper_dither_tile(const double sigma)
{
  static cairo_surface_t *cached = NULL;
  static int cached_key = -1;
  static GMutex lock;
  const int key = (int)lround(sigma * 4096.0);
  g_mutex_lock(&lock);
  if(!IS_NULL_PTR(cached) && cached_key == key)
  {
    cairo_surface_t *tile = cached;
    g_mutex_unlock(&lock);
    return tile;
  }
  cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, PAPER_DITHER_TILE, PAPER_DITHER_TILE);
  uint8_t *data = cairo_image_surface_get_data(tile);
  const int stride = cairo_image_surface_get_stride(tile);
  for(int y = 0; y < PAPER_DITHER_TILE; y++)
  {
    for(int x = 0; x < PAPER_DITHER_TILE; x++)
    {
      double uniform_a = 0.0;
      double uniform_b = 0.0;
      _paper_hash_uniforms(0xD17Eu, x, y, &uniform_a, &uniform_b);
      const double gaussian = sqrt(-2.0 * log(uniform_a)) * cos(2.0 * M_PI * uniform_b);
      const uint8_t value = (uint8_t)lround(CLAMP(1.0 - sigma - sigma * gaussian, 0.0, 1.0) * 255.0);
      uint8_t *pixel = data + (size_t)y * stride + (size_t)x * 4;
      pixel[0] = value;
      pixel[1] = value;
      pixel[2] = value;
      pixel[3] = 255;
    }
  }
  cairo_surface_mark_dirty(tile);
  if(!IS_NULL_PTR(cached)) cairo_surface_destroy(cached);
  cached = tile;
  cached_key = key;
  g_mutex_unlock(&lock);
  return tile;
}

/**
 * Finish the paper with a gentle multiplicative dither, one device pixel wide at every zoom:
 * its deviation grows with the square root of the zoom, so a magnified paper, whose own
 * grain is interpolated, gets a little more of it. Anchored to the canvas origin, so it does
 * not shimmer under a pan.
 */
static void _paint_dither(cairo_t *cr, const dt_canvas_paint_options_t *options, const double strength)
{
  const double zoom = 1.0 / options->units_per_pixel;
  const double sigma = PAPER_DITHER_SIGMA * strength * CLAMP(sqrt(zoom), 0.5, 2.0);
  cairo_surface_t *tile = _paper_dither_tile(sigma);
  double origin_x = 0.0;
  double origin_y = 0.0;
  cairo_user_to_device(cr, &origin_x, &origin_y);
  cairo_save(cr);
  cairo_rectangle(cr, options->clip.x, options->clip.y, options->clip.width, options->clip.height);
  cairo_clip(cr);
  cairo_identity_matrix(cr);
  cairo_pattern_t *pattern = cairo_pattern_create_for_surface(tile);
  cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
  cairo_matrix_t matrix;
  cairo_matrix_init_translate(&matrix, -floor(origin_x), -floor(origin_y));
  cairo_pattern_set_matrix(pattern, &matrix);
  cairo_set_source(cr, pattern);
  cairo_pattern_destroy(pattern);
  cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
  cairo_paint(cr);
  cairo_restore(cr);
}

/** Fill the clip with the paper, cell by cell in device space at integer offsets: cairo's fastest blit. */
static void _paint_paper(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  const double pixels_per_unit = 1.0 / options->units_per_pixel;
  const int sprite_scaled = CLAMP((int)lround(PAPER_TILE * pixels_per_unit), 8, 2048);
  cairo_surface_t *tile = _paper_tile(canvas->background_style, options->for_display, sprite_scaled);
  if(IS_NULL_PTR(tile)) return;
  if(options->clip.width <= 0.0 || options->clip.height <= 0.0) return;
  const double cell = (double)PAPER_CELLS * PAPER_TILE;
  const int first_col = (int)floor(options->clip.x / cell);
  const int last_col = (int)floor((options->clip.x + options->clip.width) / cell);
  const int first_row = (int)floor(options->clip.y / cell);
  const int last_row = (int)floor((options->clip.y + options->clip.height) / cell);
  if((double)(last_col - first_col + 1) * (double)(last_row - first_row + 1) > 65536.0) return;

  cairo_save(cr);
  cairo_rectangle(cr, options->clip.x, options->clip.y, options->clip.width, options->clip.height);
  cairo_clip(cr);
  cairo_matrix_t user_to_device;
  cairo_get_matrix(cr, &user_to_device);
  cairo_identity_matrix(cr);
  for(int row = first_row; row <= last_row; row++)
  {
    for(int col = first_col; col <= last_col; col++)
    {
      // The cell's device box, from its canvas corners, so neighbours share their edges exactly.
      double left = col * cell;
      double top = row * cell;
      double right = (col + 1) * cell;
      double bottom = (row + 1) * cell;
      cairo_matrix_transform_point(&user_to_device, &left, &top);
      cairo_matrix_transform_point(&user_to_device, &right, &bottom);
      const double cell_x = floor(left);
      const double cell_y = floor(top);
      const double cell_width = floor(right) - cell_x;
      const double cell_height = floor(bottom) - cell_y;
      const double tile_size = cairo_image_surface_get_width(tile);
      cairo_save(cr);
      cairo_rectangle(cr, cell_x, cell_y, cell_width, cell_height);
      cairo_clip(cr);
      cairo_translate(cr, cell_x, cell_y);
      // At one pixel per tile pixel this is a plain blit; zoomed past the field, a bilinear scale.
      const gboolean upscaled = fabs(cell_width - tile_size) > 1.0;
      if(upscaled) cairo_scale(cr, cell_width / tile_size, cell_height / tile_size);
      cairo_set_source_surface(cr, tile, 0.0, 0.0);
      cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
      cairo_pattern_set_filter(cairo_get_source(cr), upscaled ? CAIRO_FILTER_BILINEAR : CAIRO_FILTER_NEAREST);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }
  cairo_restore(cr);
  // Washi is grainier to the eye than the western sheets: twice the dither.
  _paint_dither(cr, options, canvas->background_style == DT_CANVAS_BACKGROUND_JAPANESE ? 2.0 : 1.0);
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
  if(options->draw_background && _is_paper(canvas->background_style))
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
