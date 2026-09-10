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
#include "common/logging.h"
#include "common/times.h"
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
  if(!(canvas->grid_flags & DT_CANVAS_PAGE_VISIBLE)) return;
  cairo_save(cr);
  _set_color(cr, &canvas->page_color, options->for_display);
  cairo_set_line_width(cr, 1.0 * options->units_per_pixel);
  // One line per border, not one rectangle per page: a shared edge stroked twice with two
  // dash phases fills its own gaps and reads as solid. Each line starts on a multiple of
  // the dash period from the origin, so the dashes neither crawl under a pan nor differ
  // between the horizontal and the vertical.
  const double dashes[2] = { 8.0 * options->units_per_pixel, 6.0 * options->units_per_pixel };
  const double period = dashes[0] + dashes[1];
  cairo_set_dash(cr, dashes, 2, 0.0);
  const double start_x = floor(options->clip.x / period) * period;
  const double end_x = options->clip.x + options->clip.width;
  const double start_y = floor(options->clip.y / period) * period;
  const double end_y = options->clip.y + options->clip.height;
  for(int col = first_col; col <= last_col + 1; col++)
  {
    cairo_move_to(cr, col * page_width, start_y);
    cairo_line_to(cr, col * page_width, end_y);
  }
  for(int row = first_row; row <= last_row + 1; row++)
  {
    cairo_move_to(cr, start_x, row * page_height);
    cairo_line_to(cr, end_x, row * page_height);
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

/**
 * One sprite's relief, `size` square, before the colour, in two parts the user weighs
 * separately: `low`, the paper's body -- its mottle, tooth or clouds, what "contrast"
 * scales -- and `high`, its fine structure -- fibres, pores, grain, wrinkles, what "detail"
 * scales. `scale` sizes the features: every knee is divided by it, so 2 makes them twice as
 * large. Both are zero-mean modulations of the paper's colour.
 */
static void _paper_relief(const dt_canvas_background_t style, const int size, const int variant, const double scale,
                          double **low, double **high)
{
  const guint32 seed = 1000u * (guint32)(variant + 1);
  const size_t count = (size_t)size * size;
  *low = g_new0(double, count);
  *high = g_new0(double, count);
  if(style == DT_CANVAS_BACKGROUND_MOLESKINE)
  {
    // Fine, soft clouds; short fibres in every direction, that show as the zoom lets them; a whisper of grain.
    double *mottle = _paper_field(size, 40.0 / scale, 2.0, seed + 101u);
    double *fibres = _paper_fibres(size, seed + 105u);
    double *grain = _paper_field(size, 160.0 / scale, 1.1, seed + 103u);
    for(size_t idx = 0; idx < count; idx++)
    {
      (*low)[idx] = mottle[idx] * 0.011;
      (*high)[idx] = fibres[idx] * 0.012 + grain[idx] * 0.0025;
    }
    dt_free(mottle);
    dt_free(fibres);
    dt_free(grain);
  }
  else if(style == DT_CANVAS_BACKGROUND_WATERCOLOUR)
  {
    // A tooth of shallow hollows between peaks -- paper is white at its peaks, so the tooth
    // only carves, and no deeper than the saturation allows -- a band of rounded pores, and
    // a fine, quiet grain.
    double *tooth = _paper_field(size, 45.0 / scale, 1.8, seed + 201u);
    double *pores = _paper_field_band(size, 320.0 / scale, 2.5, 110.0 / scale, seed + 205u);
    double *grain = _paper_field(size, 200.0 / scale, 1.0, seed + 203u);
    for(size_t idx = 0; idx < count; idx++)
    {
      const double hollow = fmin(tooth[idx], 0.0);
      (*low)[idx] = -0.06 * (1.0 - exp(-hollow * hollow * 0.5));
      (*high)[idx] = pores[idx] * 0.008 + grain[idx] * 0.003;
    }
    dt_free(tooth);
    dt_free(pores);
    dt_free(grain);
  }
  else if(style == DT_CANVAS_BACKGROUND_EMBOSSED)
  {
    // The random part of a wove sheet: a mottle and fibres; the mesh comes after the blend,
    // and takes its wobble from this very relief.
    double *mottle = _paper_field(size, 36.0 / scale, 2.0, seed + 301u);
    double *fibres = _paper_fibres(size, seed + 305u);
    for(size_t idx = 0; idx < count; idx++)
    {
      (*low)[idx] = mottle[idx] * 0.01;
      (*high)[idx] = fibres[idx] * 0.009;
    }
    dt_free(mottle);
    dt_free(fibres);
  }
  else if(style == DT_CANVAS_BACKGROUND_JAPANESE)
  {
    // Large soft clouds, a little more contrast than watercolour, and long wrinkles: the
    // zero crossings of a low-frequency field are long curved lines, lit as ridges, and
    // the same lines at every resolution.
    double *clouds = _paper_field(size, 9.0 / scale, 2.2, seed + 401u);
    double *wrinkle_field = _paper_field(size, 14.0 / scale, 2.5, seed + 405u);
    double *grain = _paper_field(size, 180.0 / scale, 1.0, seed + 403u);
    for(size_t idx = 0; idx < count; idx++)
    {
      const double ridge = exp(-wrinkle_field[idx] * wrinkle_field[idx] * 80.0);
      (*low)[idx] = clouds[idx] * 0.018;
      (*high)[idx] = ridge * 0.1 + grain[idx] * 0.002;
    }
    dt_free(clouds);
    dt_free(wrinkle_field);
    dt_free(grain);
  }
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
static double *_paper_compose(double **sprites, const int sprite_size)
{
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
  return field;
}

/** Both composed fields of a paper, PAPER_CELLS * sprite_size square, at a feature scale. */
static void _paper_fields(const dt_canvas_background_t style, const int sprite_size, const double scale, double **low,
                          double **high)
{
  double *low_sprites[PAPER_SPRITES];
  double *high_sprites[PAPER_SPRITES];
  for(int variant = 0; variant < PAPER_SPRITES; variant++)
    _paper_relief(style, sprite_size, variant, scale, &low_sprites[variant], &high_sprites[variant]);
  *low = _paper_compose(low_sprites, sprite_size);
  *high = _paper_compose(high_sprites, sprite_size);
  for(int variant = 0; variant < PAPER_SPRITES; variant++)
  {
    dt_free(low_sprites[variant]);
    dt_free(high_sprites[variant]);
  }
}

/**
 * The composed paper's sRGB pixels, PAPER_CELLS * sprite_size square: the canvas's background
 * colour is the paper's colour -- the fundamental the relief modulates around -- and the
 * relief, its two parts weighed by `contrast` and `detail`, moves every channel by the same
 * fraction of it, so a coloured paper stays that colour in its hollows and on its peaks.
 */
static uint8_t *_paper_pixels(const dt_canvas_background_t style, const int sprite_size, const dt_canvas_color_t *color,
                              const double contrast, const double detail, const double *low, const double *high)
{
  const double base[3] = { CLAMP(color->red, 0.0f, 1.0f), CLAMP(color->green, 0.0f, 1.0f), CLAMP(color->blue, 0.0f, 1.0f) };
  const int total = PAPER_CELLS * sprite_size;
  const double pixels_per_unit = (double)sprite_size / PAPER_TILE;
  uint8_t *pixels = g_malloc((size_t)total * total * 4);
  for(size_t idx = 0; idx < (size_t)total * total; idx++)
  {
    double relief = contrast * low[idx] + detail * high[idx];
    // The mesh is stamped over the blended field in absolute coordinates, so it stays one
    // mesh across placements whatever their phase and jitter; its pitch divides the period.
    if(style == DT_CANVAS_BACKGROUND_EMBOSSED)
      relief += detail * _paper_mesh((idx % total) / pixels_per_unit, (idx / total) / pixels_per_unit, low[idx] + high[idx]);
    for(int channel = 0; channel < 3; channel++)
      pixels[4 * idx + channel] = (uint8_t)lround(CLAMP(base[channel] * (1.0 + relief), 0.0, 1.0) * 255.0);
    pixels[4 * idx + 3] = 255;
  }
  return pixels;
}

typedef struct dt_paper_cache_t
{
  double *low[PAPER_FIELD_MAX_LOG2 + 1];  ///< the composed relief per sprite resolution, its two parts
  double *high[PAPER_FIELD_MAX_LOG2 + 1];
  double scale[PAPER_FIELD_MAX_LOG2 + 1]; ///< the feature scale the fields were built at
  cairo_surface_t *tile;                  ///< coloured, weighed, scaled to what the zoom shows
  uint64_t key;                           ///< the colour, the weights and the size the tile was built for
} dt_paper_cache_t;

/** A key over what the tile depends on: the colour, the two weights and the size on screen. */
static uint64_t _paper_key(const dt_canvas_color_t *color, const double contrast, const double detail,
                           const double scale, const int scaled_size)
{
  uint64_t hash = 1469598103934665603ULL;
  const float values[7] = { color->red, color->green, color->blue, (float)contrast, (float)detail, (float)scale,
                            (float)scaled_size };
  const uint8_t *bytes = (const uint8_t *)values;
  for(size_t idx = 0; idx < sizeof(values); idx++)
  {
    hash ^= bytes[idx];
    hash *= 1099511628211ULL;
  }
  return hash;
}

/**
 * The composed paper as one seamless tile, PAPER_CELLS sprites wide, in the layer encoding,
 * at the size it shows on screen. The sprites are synthesised at the smallest power of two
 * holding the sprite's on-screen size, so zooming in reveals finer grain; the two relief
 * fields are kept per resolution and feature scale, the coloured and weighed tile per key,
 * so the colour and the weights are cheap to change and only the scale rebuilds the fields.
 */
static cairo_surface_t *_paper_tile(const dt_canvas_t *canvas, const int sprite_scaled)
{
  const uint32_t style = canvas->background_style;
  if(!_is_paper(style)) return NULL;
  static dt_paper_cache_t caches[DT_CANVAS_BACKGROUND_LAST];
  static GMutex lock;
  dt_paper_cache_t *cache = &caches[style];
  float contrast = 1.0f;
  float detail = 1.0f;
  float scale = 1.0f;
  dt_canvas_texture_get(canvas, &contrast, &detail, &scale, NULL);
  // Never above the field's own resolution: past it, the painter scales each cell up instead
  // of a tile that would grow with the square of the zoom.
  const int scaled_size = MIN(PAPER_CELLS * sprite_scaled, PAPER_CELLS * (1 << PAPER_FIELD_MAX_LOG2));
  const uint64_t key = _paper_key(&canvas->background, contrast, detail, scale, scaled_size);

  g_mutex_lock(&lock);
  if(!IS_NULL_PTR(cache->tile) && cache->key == key)
  {
    g_mutex_unlock(&lock);
    return cache->tile;
  }
  int field_log2 = PAPER_FIELD_MIN_LOG2;
  while(field_log2 < PAPER_FIELD_MAX_LOG2 && (1 << field_log2) < sprite_scaled) field_log2++;
  const int sprite_size = 1 << field_log2;
  const int field_size = PAPER_CELLS * sprite_size;
  if(IS_NULL_PTR(cache->low[field_log2]) || cache->scale[field_log2] != scale)
  {
    dt_free(cache->low[field_log2]);
    dt_free(cache->high[field_log2]);
    _paper_fields((dt_canvas_background_t)style, sprite_size, scale, &cache->low[field_log2], &cache->high[field_log2]);
    cache->scale[field_log2] = scale;
  }
  uint8_t *base = _paper_pixels((dt_canvas_background_t)style, sprite_size, &canvas->background, contrast, detail,
                                cache->low[field_log2], cache->high[field_log2]);

  // The field is sRGB: into the layer encoding, then scaled to what the zoom shows.
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, field_size);
  uint8_t *bgra = g_malloc((size_t)stride * field_size);
  dt_canvas_render_srgb8_to_layer8(base, (size_t)field_size * field_size, 4);
  for(size_t idx = 0; idx < (size_t)field_size * field_size; idx++)
  {
    bgra[4 * idx + 0] = base[4 * idx + 2];
    bgra[4 * idx + 1] = base[4 * idx + 1];
    bgra[4 * idx + 2] = base[4 * idx + 0];
    bgra[4 * idx + 3] = 255;
  }
  dt_free(base);
  cairo_surface_t *unscaled = cairo_image_surface_create_for_data(bgra, CAIRO_FORMAT_RGB24, field_size, field_size, stride);
  cairo_surface_t *tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, scaled_size, scaled_size);
  cairo_t *cr = cairo_create(tile);
  cairo_scale(cr, (double)scaled_size / field_size, (double)scaled_size / field_size);
  cairo_set_source_surface(cr, unscaled, 0.0, 0.0);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
  cairo_pattern_set_filter(cairo_get_source(cr), scaled_size == field_size ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(unscaled);
  dt_free(bgra);
  if(!IS_NULL_PTR(cache->tile)) cairo_surface_destroy(cache->tile);
  cache->tile = tile;
  cache->key = key;
  g_mutex_unlock(&lock);
  return tile;
}

#define PAPER_DITHER_TILE 256   ///< the dither tile, repeated in device space
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
  cairo_surface_t *tile = _paper_tile(canvas, sprite_scaled);
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
  // Washi is grainier to the eye than the western sheets: twice the dither; the user's grain on top.
  float grain = 1.0f;
  dt_canvas_texture_get(canvas, NULL, NULL, NULL, &grain);
  _paint_dither(cr, options, (canvas->background_style == DT_CANVAS_BACKGROUND_JAPANESE ? 2.0 : 1.0) * grain);
}

/* --- frames ----------------------------------------------------------------- */

/** The frame's outline, inset by `inset` on every side, with the frame's corners rounded less the inset. */
static void _frame_path(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object, const double inset)
{
  const double width = fmax(object->width - 2.0 * inset, 0.0);
  const double height = fmax(object->height - 2.0 * inset, 0.0);
  const double left = -object->width * 0.5 + inset;
  const double top = -object->height * 0.5 + inset;
  const double radius = CLAMP(dt_canvas_object_effective_corner_radius(canvas, object) - inset, 0.0, 0.5 * fmin(width, height));
  if(radius <= 0.0)
  {
    cairo_rectangle(cr, left, top, width, height);
    return;
  }
  cairo_new_sub_path(cr);
  cairo_arc(cr, left + width - radius, top + radius, radius, -M_PI / 2.0, 0.0);
  cairo_arc(cr, left + width - radius, top + height - radius, radius, 0.0, M_PI / 2.0);
  cairo_arc(cr, left + radius, top + height - radius, radius, M_PI / 2.0, M_PI);
  cairo_arc(cr, left + radius, top + radius, radius, M_PI, 3.0 * M_PI / 2.0);
  cairo_close_path(cr);
}

/** A cut-out frame's border follows the cutout, dilated outward, and is composited, not stroked. */
static gboolean _object_cut(const dt_canvas_object_t *object)
{
  return dt_canvas_object_is_frame(object) && object->mask.shape != DT_CANVAS_MASK_NONE;
}

/** How far the content sits inside the frame: the border, for a rectangular frame; nothing for a cut one. */
static double _border_inset(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  if(_object_cut(object)) return 0.0;
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &color, &width);
  return width;
}

static void _paint_border(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                          const dt_canvas_paint_options_t *options)
{
  if(_object_cut(object)) return;
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &color, &width);
  if(width <= 0.0f || color.alpha <= 0.0f) return;
  // The border is part of the frame: it sits inside its edge, and the content is inset by it.
  cairo_save(cr);
  _set_color(cr, &color, options->for_display);
  cairo_set_line_width(cr, width);
  _frame_path(cr, canvas, object, width * 0.5);
  cairo_stroke(cr);
  cairo_restore(cr);
}

static void _paint_placeholder(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                               const dt_canvas_paint_options_t *options)
{
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.35);
  _frame_path(cr, canvas, object, 0.0);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.6);
  cairo_set_line_width(cr, 1.5 * options->units_per_pixel);
  _frame_path(cr, canvas, object, 0.0);
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
    owned = dt_canvas_render_decode(dt_canvas_object_raster(object),
                                    object->kind == DT_CANVAS_OBJECT_IMAGE ? object->image.colorspace
                                                                            : DT_CANVAS_COLORSPACE_SRGB);
    surface = owned;
  }
  _paint_border(cr, canvas, object, options);
  const dt_canvas_color_t background = dt_canvas_object_background(object);
  if(background.alpha > 0.0f && !_object_cut(object))
  {
    // Under the picture, inside the border: what shows through a translucent or missing render.
    cairo_save(cr);
    _set_color(cr, &background, options->for_display);
    _frame_path(cr, canvas, object, _border_inset(canvas, object));
    cairo_fill(cr);
    cairo_restore(cr);
  }
  if(IS_NULL_PTR(surface))
  {
    if(options->draw_placeholders) _paint_placeholder(cr, canvas, object, options);
    return;
  }
  const double surface_width = cairo_image_surface_get_width(surface);
  const double surface_height = cairo_image_surface_get_height(surface);
  const double border_width = _border_inset(canvas, object);
  const double inner_width = fmax(object->width - 2.0 * border_width, 1.0);
  const double inner_height = fmax(object->height - 2.0 * border_width, 1.0);
  if(surface_width > 0.0 && surface_height > 0.0)
  {
    cairo_save(cr);
    _frame_path(cr, canvas, object, border_width);
    cairo_clip(cr);
    double scale_x = inner_width / surface_width;
    double scale_y = inner_height / surface_height;
    if(object->kind == DT_CANVAS_OBJECT_MAP)
    {
      // A map keeps its own ratio: it covers the frame, centred, and is cropped by it.
      scale_x = fmax(scale_x, scale_y);
      scale_y = scale_x;
    }
    cairo_translate(cr, -surface_width * scale_x * 0.5, -surface_height * scale_y * 0.5);
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    // Set AFTER cairo_set_source_surface(): the filter belongs to the pattern that scales.
    const double downscale = scale_x / options->units_per_pixel;
    cairo_pattern_set_filter(cairo_get_source(cr), downscale < 0.5 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  if(!IS_NULL_PTR(owned)) cairo_surface_destroy(owned);
  if(object->kind == DT_CANVAS_OBJECT_MAP)
  {
    // The provider's attribution, in a strip along the bottom edge, as its terms ask.
    const char *attribution = dt_canvas_map_source_attribution(object->map.source);
    if(!IS_NULL_PTR(attribution) && attribution[0] != '\0')
    {
      cairo_save(cr);
      cairo_rectangle(cr, -inner_width * 0.5, -inner_height * 0.5, inner_width, inner_height);
      cairo_clip(cr);
      const double font_size = CLAMP(inner_height * 0.035, 6.0, 14.0);
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, font_size);
      cairo_text_extents_t extents;
      cairo_text_extents(cr, attribution, &extents);
      const double strip_height = font_size * 1.6;
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
      cairo_rectangle(cr, inner_width * 0.5 - extents.x_advance - font_size, inner_height * 0.5 - strip_height,
                      extents.x_advance + font_size, strip_height);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.9);
      cairo_move_to(cr, inner_width * 0.5 - extents.x_advance - font_size * 0.5, inner_height * 0.5 - font_size * 0.45);
      cairo_show_text(cr, attribution);
      cairo_restore(cr);
    }
  }
}

/** The text sits inside the border and the padding. */
static double _text_inset(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  const double padding = object->text.padding > 0.0f ? object->text.padding : 0.0;
  return padding + _border_inset(canvas, object);
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
  if(object->text.background.alpha > 0.0f && !_object_cut(object))
  {
    // The background fills the frame inside its border, so the border is not painted over.
    cairo_save(cr);
    _set_color(cr, &object->text.background, options->for_display);
    _frame_path(cr, canvas, object, _border_inset(canvas, object));
    cairo_fill(cr);
    cairo_restore(cr);
  }
  cairo_save(cr);
  _frame_path(cr, canvas, object, 0.0);
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
  // The line stops short of an arrow's tip: a disc of the head's length around each
  // arrowed end is cut out of the stroke, so the tip is the triangle's alone and stays sharp.
  // The cut-out is bounded to the route's own box: cairo's coordinates are 24.8 fixed point,
  // and a "whole plane" rectangle overflows them under the zoom and clips everything away.
  const double head_length = PAINT_ARROW_LENGTH * head_scale;
  cairo_save(cr);
  if(object->connector.style & (DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START))
  {
    double box_left = route.points[0];
    double box_top = route.points[1];
    double box_right = route.points[0];
    double box_bottom = route.points[1];
    for(int idx = 1; idx < route.point_count; idx++)
    {
      box_left = fmin(box_left, route.points[2 * idx]);
      box_right = fmax(box_right, route.points[2 * idx]);
      box_top = fmin(box_top, route.points[2 * idx + 1]);
      box_bottom = fmax(box_bottom, route.points[2 * idx + 1]);
    }
    const double margin = head_length * 2.0 + line_width * 4.0;
    cairo_new_path(cr);
    cairo_rectangle(cr, box_left - margin, box_top - margin, box_right - box_left + 2.0 * margin,
                    box_bottom - box_top + 2.0 * margin);
    if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_END)
    {
      cairo_new_sub_path(cr);
      cairo_arc_negative(cr, route.to_x, route.to_y, head_length * 0.9, 2.0 * M_PI, 0.0);
    }
    if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_START)
    {
      cairo_new_sub_path(cr);
      cairo_arc_negative(cr, route.from_x, route.from_y, head_length * 0.9, 2.0 * M_PI, 0.0);
    }
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
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
  cairo_restore(cr);

  // A head points along the line it ends: the last leg of the route, which for a straight
  // connector is the chord itself and for the others the stub or tangent at that anchor.
  const int last = route.point_count - 1;
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_END)
    _paint_arrow_head(cr, route.to_x, route.to_y, route.points[2 * last - 2], route.points[2 * last - 1], head_scale);
  if(object->connector.style & DT_CANVAS_CONNECTOR_ARROW_START)
    _paint_arrow_head(cr, route.from_x, route.from_y, route.points[2], route.points[3], head_scale);
  cairo_restore(cr);
}

/* --- the compositor ----------------------------------------------------------------- */

/* Everything the canvas shows is composited here, in linear light with premultiplied alpha, in
 * 32-bit floats: cairo paints each object into its own 8-bit layer, in the encoding it
 * arrived in; the layer is decoded through the sRGB curve into linear premultiplied RGBA,
 * scaled by the object's opacity, its shadow is derived from its alpha and laid under it, and
 * both go "over" the float canvas. The finished canvas is encoded back to 8 bits with the
 * inverse curve and handed to cairo as one image. Feathered cutouts, semi-transparent frames
 * and shadows therefore blend in linear light, where a 50% mix is half the light and not half
 * the code value.
 *
 * On screen the layers hold display-encoded colours (each render is colour-managed when it
 * is decoded), so the curve stands in for the display's own transfer function; that is exact
 * for an sRGB display, and for any other it only shapes the blending at feathered and
 * translucent pixels, since an opaque pixel decodes and re-encodes to the very code it held.
 * The export keeps sRGB throughout and is converted to the output profile afterwards. */

/* The working space is linear Adobe RGB (1998): every layer cairo paints is already in Adobe
 * RGB's encoding (the renders leave the pipeline in it, everything sRGB is converted on the
 * way in), so a layer is decoded through the 563/256 gamma alone, the blends happen in the
 * linear space, and the finished canvas leaves it for the display profile (through XYZ, D50 as
 * the colour module's XYZ profile is: the matrix is the specification's Bradford-adapted one)
 * or re-encoded, still Adobe RGB, for the export. Wider than sRGB and what a print can use;
 * wider still would buy nothing in eight bits. */
#define WORKING_GAMMA (563.0f / 256.0f)
static const float _working_to_xyz_d50[9] = { 0.6097559f, 0.2052401f, 0.1492240f,
                                              0.3111242f, 0.6256560f, 0.0632197f,
                                              0.0194811f, 0.0608902f, 0.7448387f };
static const float _xyz_d50_to_working[9] = { 1.9624274f, -0.6105343f, -0.3413404f,
                                              -0.9787684f, 1.9161415f, 0.0334540f,
                                              0.0286869f, -0.1406752f, 1.3487655f };

static inline void _matrix_apply(const float *matrix, const float in[3], float out[3])
{
  out[0] = matrix[0] * in[0] + matrix[1] * in[1] + matrix[2] * in[2];
  out[1] = matrix[3] * in[0] + matrix[4] * in[1] + matrix[5] * in[2];
  out[2] = matrix[6] * in[0] + matrix[7] * in[1] + matrix[8] * in[2];
}

#define COMPOSE_OETF_STEPS 16383
#define COMPOSE_BAND_MAX_PIXELS (24 * 1024 * 1024) ///< a band of the float canvas: 384 MB of RGBA floats
#define COMPOSE_MASK_MAX_PIXELS 3072              ///< a cutout raster's longer side
#define COMPOSE_SHADOW_SIGMAS 3.0                 ///< how far a shadow reaches past its blur

static float _eotf_lut[256];
static uint8_t _oetf_lut[COMPOSE_OETF_STEPS + 1];
static gsize _luts_ready = 0;

static float _working_eotf(const float value)
{
  return powf(CLAMP(value, 0.0f, 1.0f), WORKING_GAMMA);
}

/* The decode table is exact per code. The encode table is indexed by the square root of the
 * value, which packs its steps at the dark end where a gamma curve is steepest -- a uniform
 * table would miss the first codes by whole steps -- so every code round-trips to itself. */
static void _luts_init(void)
{
  if(g_once_init_enter(&_luts_ready))
  {
    for(int idx = 0; idx < 256; idx++) _eotf_lut[idx] = _working_eotf((float)idx / 255.0f);
    for(int idx = 0; idx <= COMPOSE_OETF_STEPS; idx++)
    {
      const float root = (float)idx / (float)COMPOSE_OETF_STEPS;
      _oetf_lut[idx] = (uint8_t)lrintf(powf(root * root, 1.0f / WORKING_GAMMA) * 255.0f);
    }
    g_once_init_leave(&_luts_ready, 1);
  }
}

static inline uint8_t _encode(const float linear)
{
  return _oetf_lut[(int)lrintf(sqrtf(CLAMP(linear, 0.0f, 1.0f)) * (float)COMPOSE_OETF_STEPS)];
}

/** A canvas colour in the working space: the layer encoding, decoded. */
static void _color_to_working(const dt_canvas_color_t *color, float working[3])
{
  double rgb[3] = { 0.0, 0.0, 0.0 };
  dt_canvas_render_color(color, FALSE, rgb);
  working[0] = _working_eotf((float)rgb[0]);
  working[1] = _working_eotf((float)rgb[1]);
  working[2] = _working_eotf((float)rgb[2]);
}

/* Timings of the last paint, for `-d perf` and for tuning: what each phase of the compositor
 * cost, accumulated over the bands of one dt_canvas_paint() call. */
static dt_canvas_paint_stats_t _stats;

dt_canvas_paint_stats_t dt_canvas_paint_last_stats(void)
{
  return _stats;
}

/** An integer box in device pixels. */
typedef struct dt_canvas_box_t
{
  int x;
  int y;
  int width;
  int height;
} dt_canvas_box_t;

static gboolean _box_empty(const dt_canvas_box_t *box)
{
  return box->width <= 0 || box->height <= 0;
}

static dt_canvas_box_t _box_intersect(const dt_canvas_box_t *first, const dt_canvas_box_t *second)
{
  dt_canvas_box_t box;
  box.x = MAX(first->x, second->x);
  box.y = MAX(first->y, second->y);
  box.width = MIN(first->x + first->width, second->x + second->width) - box.x;
  box.height = MIN(first->y + first->height, second->y + second->height) - box.y;
  return box;
}

static dt_canvas_box_t _box_grow(const dt_canvas_box_t *box, const int margin)
{
  dt_canvas_box_t grown = { box->x - margin, box->y - margin, box->width + 2 * margin, box->height + 2 * margin };
  return grown;
}

/** Device pixels per canvas unit under a transform. */
static double _matrix_scale(const cairo_matrix_t *matrix)
{
  return sqrt(fabs(matrix->xx * matrix->yy - matrix->xy * matrix->yx));
}

/** The device box holding user-space points, grown by a margin in device pixels. */
static dt_canvas_box_t _box_of_points(const cairo_matrix_t *matrix, const double *points, const int count,
                                      const double margin)
{
  double min_x = INFINITY;
  double min_y = INFINITY;
  double max_x = -INFINITY;
  double max_y = -INFINITY;
  for(int idx = 0; idx < count; idx++)
  {
    double x = points[2 * idx];
    double y = points[2 * idx + 1];
    cairo_matrix_transform_point(matrix, &x, &y);
    min_x = fmin(min_x, x);
    max_x = fmax(max_x, x);
    min_y = fmin(min_y, y);
    max_y = fmax(max_y, y);
  }
  dt_canvas_box_t box = { 0, 0, 0, 0 };
  if(count <= 0 || !isfinite(min_x) || !isfinite(max_x)) return box;
  box.x = (int)floor(min_x - margin - 1.0);
  box.y = (int)floor(min_y - margin - 1.0);
  box.width = (int)ceil(max_x + margin + 1.0) - box.x;
  box.height = (int)ceil(max_y + margin + 1.0) - box.y;
  return box;
}

/** The user-space rectangle a device box covers, for the painters that clip in canvas units. */
static dt_canvas_rect_t _box_to_user(const cairo_matrix_t *matrix, const dt_canvas_box_t *box)
{
  cairo_matrix_t inverse = *matrix;
  dt_canvas_rect_t rect = { 0.0, 0.0, 0.0, 0.0 };
  if(cairo_matrix_invert(&inverse) != CAIRO_STATUS_SUCCESS) return rect;
  const double corners[8] = { (double)box->x, (double)box->y, (double)(box->x + box->width), (double)box->y,
                              (double)(box->x + box->width), (double)(box->y + box->height), (double)box->x,
                              (double)(box->y + box->height) };
  double min_x = INFINITY;
  double min_y = INFINITY;
  double max_x = -INFINITY;
  double max_y = -INFINITY;
  for(int idx = 0; idx < 4; idx++)
  {
    double x = corners[2 * idx];
    double y = corners[2 * idx + 1];
    cairo_matrix_transform_point(&inverse, &x, &y);
    min_x = fmin(min_x, x);
    max_x = fmax(max_x, x);
    min_y = fmin(min_y, y);
    max_y = fmax(max_y, y);
  }
  rect.x = min_x;
  rect.y = min_y;
  rect.width = max_x - min_x;
  rect.height = max_y - min_y;
  return rect;
}

/**
 * A cairo context painting user space, under `matrix`, into a surface whose origin is `box`,
 * with the target's font options: text hinted and antialiased the way the screen asks.
 */
static cairo_t *_layer_context(cairo_surface_t *surface, const cairo_matrix_t *matrix, const dt_canvas_box_t *box,
                               const cairo_font_options_t *font_options)
{
  cairo_t *cr = cairo_create(surface);
  if(!IS_NULL_PTR(font_options)) cairo_set_font_options(cr, font_options);
  cairo_translate(cr, -box->x, -box->y);
  cairo_transform(cr, matrix);
  return cr;
}

/**
 * Decode an 8-bit premultiplied cairo layer into linear premultiplied RGBA floats, scaled by
 * an opacity. An opaque pixel goes through the table; a translucent one is unpremultiplied
 * first, so the curve is applied to the colour and not to the coverage.
 */
static void _layer_linearise(cairo_surface_t *surface, const float opacity, float *rgba)
{
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) shared(_eotf_lut) schedule(static)
#endif
  for(int row = 0; row < height; row++)
  {
    const uint32_t *source = (const uint32_t *)(pixels + (size_t)row * stride);
    float *target = rgba + (size_t)row * width * 4;
    for(int col = 0; col < width; col++)
    {
      const uint32_t pixel = source[col];
      const uint32_t alpha = pixel >> 24;
      if(alpha == 0)
      {
        target[4 * col + 0] = 0.0f;
        target[4 * col + 1] = 0.0f;
        target[4 * col + 2] = 0.0f;
        target[4 * col + 3] = 0.0f;
        continue;
      }
      const uint32_t red = (pixel >> 16) & 0xFF;
      const uint32_t green = (pixel >> 8) & 0xFF;
      const uint32_t blue = pixel & 0xFF;
      const float coverage = (float)alpha / 255.0f;
      float linear_red = 0.0f;
      float linear_green = 0.0f;
      float linear_blue = 0.0f;
      if(alpha == 255)
      {
        linear_red = _eotf_lut[red];
        linear_green = _eotf_lut[green];
        linear_blue = _eotf_lut[blue];
      }
      else
      {
        linear_red = _working_eotf((float)red / (float)alpha);
        linear_green = _working_eotf((float)green / (float)alpha);
        linear_blue = _working_eotf((float)blue / (float)alpha);
      }
      const float weight = coverage * opacity;
      target[4 * col + 0] = linear_red * weight;
      target[4 * col + 1] = linear_green * weight;
      target[4 * col + 2] = linear_blue * weight;
      target[4 * col + 3] = weight;
    }
  }
}

/**
 * Leave the working space: the finished canvas, opaque, into an RGB24 cairo surface. For the
 * display it goes through XYZ to the display profile, in floats, by the colour module; for
 * an export it is re-encoded, Adobe RGB still, through the table. The float canvas is
 * consumed either way.
 */
static void _canvas_encode(float *rgba, cairo_surface_t *surface, const gboolean for_display)
{
  cairo_surface_flush(surface);
  uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  if(for_display)
  {
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) shared(_working_to_xyz_d50) schedule(static)
#endif
    for(int row = 0; row < height; row++)
    {
      float *pixel = rgba + (size_t)row * width * 4;
      for(int col = 0; col < width; col++)
      {
        float xyz[3];
        _matrix_apply(_working_to_xyz_d50, pixel + 4 * col, xyz);
        pixel[4 * col + 0] = xyz[0];
        pixel[4 * col + 1] = xyz[1];
        pixel[4 * col + 2] = xyz[2];
        pixel[4 * col + 3] = 1.0f;
      }
    }
    if(dt_colorprofiles_xyza_to_display_bgra8(rgba, pixels, width, height, stride))
    {
      cairo_surface_mark_dirty(surface);
      return;
    }
    // No colour module behind us: XYZ back to the working space and out re-encoded, below.
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) shared(_xyz_d50_to_working) schedule(static)
#endif
    for(int row = 0; row < height; row++)
    {
      float *pixel = rgba + (size_t)row * width * 4;
      for(int col = 0; col < width; col++)
      {
        float working[3];
        _matrix_apply(_xyz_d50_to_working, pixel + 4 * col, working);
        pixel[4 * col + 0] = working[0];
        pixel[4 * col + 1] = working[1];
        pixel[4 * col + 2] = working[2];
      }
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) shared(_oetf_lut) schedule(static)
#endif
  for(int row = 0; row < height; row++)
  {
    const float *source = rgba + (size_t)row * width * 4;
    uint32_t *target = (uint32_t *)(pixels + (size_t)row * stride);
    for(int col = 0; col < width; col++)
    {
      // What is left transparent shows black: the background under everything is opaque anyway.
      const uint32_t red = _encode(source[4 * col + 0]);
      const uint32_t green = _encode(source[4 * col + 1]);
      const uint32_t blue = _encode(source[4 * col + 2]);
      target[col] = 0xFF000000u | (red << 16) | (green << 8) | blue;
    }
  }
  cairo_surface_mark_dirty(surface);
}

/** Source-over of a layer onto the canvas, both premultiplied, over the pixels they share. */
static void _canvas_over(float *canvas_rgba, const dt_canvas_box_t *canvas_box, const float *layer_rgba,
                         const dt_canvas_box_t *layer_box, const dt_canvas_box_t *area)
{
  const int rows = area->height;
  const int cols = area->width;
  if(rows <= 0 || cols <= 0) return;
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = 0; row < rows; row++)
  {
    const int y = area->y + row;
    float *target = canvas_rgba + ((size_t)(y - canvas_box->y) * canvas_box->width + (area->x - canvas_box->x)) * 4;
    const float *source = layer_rgba + ((size_t)(y - layer_box->y) * layer_box->width + (area->x - layer_box->x)) * 4;
    for(int col = 0; col < cols; col++)
    {
      const float keep = 1.0f - source[4 * col + 3];
      target[4 * col + 0] = source[4 * col + 0] + target[4 * col + 0] * keep;
      target[4 * col + 1] = source[4 * col + 1] + target[4 * col + 1] * keep;
      target[4 * col + 2] = source[4 * col + 2] + target[4 * col + 2] * keep;
      target[4 * col + 3] = source[4 * col + 3] + target[4 * col + 3] * keep;
    }
  }
}

/** One box-blur pass along rows then columns, zero outside the plane; three make a Gaussian. */
static void _box_blur(float *plane, float *scratch, const int width, const int height, const int radius)
{
  const float norm = 1.0f / (float)(2 * radius + 1);
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = 0; row < height; row++)
  {
    const float *source = plane + (size_t)row * width;
    float *target = scratch + (size_t)row * width;
    float sum = 0.0f;
    for(int col = 0; col <= radius && col < width; col++) sum += source[col];
    for(int col = 0; col < width; col++)
    {
      target[col] = sum * norm;
      const int leaving = col - radius;
      const int entering = col + radius + 1;
      if(leaving >= 0) sum -= source[leaving];
      if(entering < width) sum += source[entering];
    }
  }
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int col = 0; col < width; col++)
  {
    float sum = 0.0f;
    for(int row = 0; row <= radius && row < height; row++) sum += scratch[(size_t)row * width + col];
    for(int row = 0; row < height; row++)
    {
      plane[(size_t)row * width + col] = sum * norm;
      const int leaving = row - radius;
      const int entering = row + radius + 1;
      if(leaving >= 0) sum -= scratch[(size_t)leaving * width + col];
      if(entering < height) sum += scratch[(size_t)entering * width + col];
    }
  }
}

/**
 * The blurred silhouette a shadow is made of: the layer's alpha (an outset shadow) or what
 * the layer leaves uncovered (an inset one), through three box blurs of the radius, which
 * approximate a Gaussian of that sigma closely enough for a shadow. The caller frees it.
 */
static float *_shadow_plane(const float *layer_rgba, const dt_canvas_box_t *layer_box, const dt_canvas_shadow_t *shadow,
                            const double pixels_per_unit, int *pad)
{
  const gboolean inset = shadow->blur < 0.0f;
  const int radius = (int)lround(fabs(shadow->blur) * pixels_per_unit);
  // An inset plane is padded with ones: past the layer's box the world is uncovered, and the
  // blur's zero padding would read it as covered and thin the shadow wherever the shape comes
  // near its own box. Three passes of radius r reach 3r, so that much padding keeps the blur
  // honest all the way to the box's edge. An outset plane pads with zeros: nothing casts there.
  *pad = inset ? 3 * radius : 0;
  const int width = layer_box->width + 2 * *pad;
  const int height = layer_box->height + 2 * *pad;
  const size_t count = (size_t)width * height;
  float *alpha = dt_alloc_align_float(count);
  float *scratch = dt_alloc_align_float(count);
  if(IS_NULL_PTR(alpha) || IS_NULL_PTR(scratch))
  {
    dt_free_align(alpha);
    dt_free_align(scratch);
    return NULL;
  }
  for(size_t idx = 0; idx < count; idx++) alpha[idx] = inset ? 1.0f : 0.0f;
  for(int row = 0; row < layer_box->height; row++)
  {
    const float *source = layer_rgba + (size_t)row * layer_box->width * 4;
    float *target = alpha + (size_t)(row + *pad) * width + *pad;
    for(int col = 0; col < layer_box->width; col++)
      target[col] = inset ? 1.0f - source[4 * col + 3] : source[4 * col + 3];
  }
  if(radius >= 1)
  {
    for(int pass = 0; pass < 3; pass++) _box_blur(alpha, scratch, width, height, radius);
  }
  dt_free_align(scratch);
  return alpha;
}

/**
 * An outset shadow: the layer's silhouette, blurred and offset, tinted, laid "over" the
 * canvas before the layer itself. The layer box was grown by the shadow's reach, so the blur
 * has room on every side.
 */
static void _canvas_shadow(float *canvas_rgba, const dt_canvas_box_t *canvas_box, const float *layer_rgba,
                           const dt_canvas_box_t *layer_box, const dt_canvas_box_t *area,
                           const dt_canvas_shadow_t *shadow, const double pixels_per_unit)
{
  int pad = 0;
  float *alpha = _shadow_plane(layer_rgba, layer_box, shadow, pixels_per_unit, &pad);
  if(IS_NULL_PTR(alpha)) return;
  float tint[3];
  _color_to_working(&shadow->color, tint);
  const float strength = CLAMP(shadow->color.alpha, 0.0f, 1.0f);
  const int offset_x = (int)lround(shadow->offset_x * pixels_per_unit);
  const int offset_y = (int)lround(shadow->offset_y * pixels_per_unit);
  const int rows = area->height;
  const int cols = area->width;
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = 0; row < rows; row++)
  {
    const int y = area->y + row;
    const int source_y = y - offset_y - layer_box->y;
    float *target = canvas_rgba + ((size_t)(y - canvas_box->y) * canvas_box->width + (area->x - canvas_box->x)) * 4;
    for(int col = 0; col < cols; col++)
    {
      const int source_x = area->x + col - offset_x - layer_box->x;
      if(source_y < 0 || source_y >= layer_box->height || source_x < 0 || source_x >= layer_box->width) continue;
      const float coverage = alpha[(size_t)source_y * layer_box->width + source_x] * strength;
      if(coverage <= 0.0f) continue;
      const float keep = 1.0f - coverage;
      target[4 * col + 0] = tint[0] * coverage + target[4 * col + 0] * keep;
      target[4 * col + 1] = tint[1] * coverage + target[4 * col + 1] * keep;
      target[4 * col + 2] = tint[2] * coverage + target[4 * col + 2] * keep;
      target[4 * col + 3] = coverage + target[4 * col + 3] * keep;
    }
  }
  dt_free_align(alpha);
}

/**
 * An inset shadow: what the layer leaves uncovered, blurred and offset, falls onto the layer
 * inside its own edges -- the shadow the object would cast on itself were it a hole. Laid
 * over the layer, within the layer's own coverage, before the layer goes over the canvas.
 */
static void _layer_inset_shadow(float *layer_rgba, const dt_canvas_box_t *layer_box, const dt_canvas_shadow_t *shadow,
                                const double pixels_per_unit)
{
  int pad = 0;
  float *alpha = _shadow_plane(layer_rgba, layer_box, shadow, pixels_per_unit, &pad);
  if(IS_NULL_PTR(alpha)) return;
  float tint[3];
  _color_to_working(&shadow->color, tint);
  const float strength = CLAMP(shadow->color.alpha, 0.0f, 1.0f);
  const int offset_x = (int)lround(shadow->offset_x * pixels_per_unit);
  const int offset_y = (int)lround(shadow->offset_y * pixels_per_unit);
  const int rows = layer_box->height;
  const int cols = layer_box->width;
  const int plane_width = cols + 2 * pad;
  const int plane_height = rows + 2 * pad;
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = 0; row < rows; row++)
  {
    float *target = layer_rgba + (size_t)row * cols * 4;
    const int source_y = row - offset_y;
    for(int col = 0; col < cols; col++)
    {
      const float own = target[4 * col + 3];
      if(own <= 0.0f) continue;
      const int source_x = col - offset_x;
      // Past the padded plane the uncovered world is whole: the shadow falls at full strength.
      const int plane_x = source_x + pad;
      const int plane_y = source_y + pad;
      const float outside = (plane_y < 0 || plane_y >= plane_height || plane_x < 0 || plane_x >= plane_width)
                                ? 1.0f
                                : alpha[(size_t)plane_y * plane_width + plane_x];
      const float coverage = outside * strength * own;
      if(coverage <= 0.0f) continue;
      const float keep = 1.0f - coverage;
      target[4 * col + 0] = tint[0] * coverage + target[4 * col + 0] * keep;
      target[4 * col + 1] = tint[1] * coverage + target[4 * col + 1] * keep;
      target[4 * col + 2] = tint[2] * coverage + target[4 * col + 2] * keep;
      target[4 * col + 3] = coverage + target[4 * col + 3] * keep;
    }
  }
  dt_free_align(alpha);
}

/** Paint one object's own pixels, in user space: what it looked like before this compositor existed. */
static void _paint_object_pixels(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                 const dt_canvas_paint_options_t *options)
{
  if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    _paint_connector(cr, canvas, object, options);
    return;
  }
  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  if(object->kind == DT_CANVAS_OBJECT_IMAGE || object->kind == DT_CANVAS_OBJECT_MAP)
    _paint_image(cr, canvas, object, options);
  else if(object->kind == DT_CANVAS_OBJECT_TEXT)
    _paint_text(cr, canvas, object, options);
  cairo_restore(cr);
}

/**
 * How a cutout is rasterised: the frame's size on screen, capped, the shape confined to the
 * frame less the border's width on every side. One rule for every frame: the frame is the
 * object's outer size, border included; a rectangular frame's border sits inside its edge
 * with the content inset, a cut frame's content stops the border's width short of the edge
 * and the border, dilated from it, ends at the edge. Only a shadow may reach past the frame.
 */
typedef struct dt_canvas_mask_geometry_t
{
  int width;  ///< the frame's raster, pixels
  int height;
  int inset;  ///< pixels of border the shape is kept clear of, on every side
  int corner; ///< the frame's corner radius, pixels
} dt_canvas_mask_geometry_t;

static dt_canvas_mask_geometry_t _mask_geometry(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                                const double pixels_per_unit)
{
  dt_canvas_mask_geometry_t geometry;
  int mask_width = (int)lround(object->width * pixels_per_unit);
  int mask_height = (int)lround(object->height * pixels_per_unit);
  if(mask_width > COMPOSE_MASK_MAX_PIXELS || mask_height > COMPOSE_MASK_MAX_PIXELS)
  {
    const double shrink = (double)COMPOSE_MASK_MAX_PIXELS / (double)MAX(mask_width, mask_height);
    mask_width = (int)lround(mask_width * shrink);
    mask_height = (int)lround(mask_height * shrink);
  }
  geometry.width = MAX(mask_width, 2);
  geometry.height = MAX(mask_height, 2);
  dt_canvas_color_t color;
  float border = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &color, &border);
  const double raster_per_unit = geometry.width / object->width;
  geometry.inset = border > 0.0f && color.alpha > 0.0f ? (int)lround(border * raster_per_unit) : 0;
  geometry.corner = (int)lround(dt_canvas_object_effective_corner_radius(canvas, object) * raster_per_unit);
  return geometry;
}

/** Paint an alpha surface in the cutout's raster space over the frame, with the given operator. */
static void _paint_frame_alpha(cairo_t *cr, const dt_canvas_object_t *object, cairo_surface_t *alpha,
                               const dt_canvas_mask_geometry_t *geometry, const cairo_operator_t operator)
{
  cairo_save(cr);
  cairo_set_operator(cr, operator);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  cairo_scale(cr, object->width / geometry->width, object->height / geometry->height);
  cairo_translate(cr, -geometry->width * 0.5, -geometry->height * 0.5);
  if(operator == CAIRO_OPERATOR_DEST_IN)
  {
    cairo_set_source_surface(cr, alpha, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
  }
  else
  {
    // The source is already set: the alpha is the mask it paints through.
    cairo_mask_surface(cr, alpha, 0.0, 0.0);
  }
  cairo_restore(cr);
}

/**
 * Multiply the layer's alpha by the object's cutout, rasterised at the frame's size on screen:
 * the feathered shape for the content, or its hard-edged support for what fills the shape.
 */
static void _apply_cutout(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                          const dt_canvas_paint_options_t *options, const double pixels_per_unit,
                          const gboolean support)
{
  if(!_object_cut(object)) return;
  const dt_canvas_mask_geometry_t geometry = _mask_geometry(canvas, object, pixels_per_unit);
  cairo_surface_t *mask = NULL;
  cairo_surface_t *owned = NULL;
  if(!IS_NULL_PTR(options->cache))
    mask = support ? dt_canvas_surface_cache_get_mask_support(options->cache, object, geometry.width, geometry.height,
                                                              geometry.inset, geometry.corner)
                   : dt_canvas_surface_cache_get_mask(options->cache, object, geometry.width, geometry.height,
                                                      geometry.inset, geometry.corner);
  else
  {
    owned = support ? dt_canvas_render_mask_support(object, geometry.width, geometry.height, geometry.inset, geometry.corner)
                    : dt_canvas_render_mask(object, geometry.width, geometry.height, geometry.inset, geometry.corner);
    mask = owned;
  }
  if(IS_NULL_PTR(mask)) return;
  _paint_frame_alpha(cr, object, mask, &geometry, CAIRO_OPERATOR_DEST_IN);
  if(!IS_NULL_PTR(owned)) cairo_surface_destroy(owned);
}

/** The object's background over its whole frame: a cut-out frame's fill, clipped to the shape's support afterwards. */
static void _paint_fill(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                        const dt_canvas_paint_options_t *options)
{
  const dt_canvas_color_t background = dt_canvas_object_background(object);
  if(background.alpha <= 0.0f) return;
  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  _set_color(cr, &background, options->for_display);
  _frame_path(cr, canvas, object, 0.0);
  cairo_fill(cr);
  cairo_restore(cr);
}

/**
 * A cut-out frame's border: the cutout's edge dilated outward by the border width, painted in
 * the border colour into a layer of its own, to go over the content in linear light.
 * @return whether anything was painted.
 */
static gboolean _paint_cut_border(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                  const dt_canvas_paint_options_t *options, const double pixels_per_unit)
{
  if(!_object_cut(object)) return FALSE;
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(canvas, object, &color, &width);
  if(width <= 0.0f || color.alpha <= 0.0f) return FALSE;
  const dt_canvas_mask_geometry_t geometry = _mask_geometry(canvas, object, pixels_per_unit);
  // The band's radius in the cutout's raster pixels, which may be coarser than the screen's.
  const int radius = MAX(1, (int)lround(width * geometry.width / object->width));
  cairo_surface_t *band = NULL;
  cairo_surface_t *owned = NULL;
  if(!IS_NULL_PTR(options->cache))
    band = dt_canvas_surface_cache_get_mask_band(options->cache, object, geometry.width, geometry.height,
                                                 geometry.inset, geometry.corner, radius);
  else
  {
    owned = dt_canvas_render_mask_band(object, geometry.width, geometry.height, geometry.inset, geometry.corner, radius);
    band = owned;
  }
  if(IS_NULL_PTR(band)) return FALSE;
  _set_color(cr, &color, options->for_display);
  _paint_frame_alpha(cr, object, band, &geometry, CAIRO_OPERATOR_OVER);
  if(!IS_NULL_PTR(owned)) cairo_surface_destroy(owned);
  return TRUE;
}

/** Source-over of one premultiplied float layer onto another of the same box. */
static void _layer_over(float *below, const float *above, const size_t pixels)
{
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(size_t idx = 0; idx < pixels; idx++)
  {
    const float keep = 1.0f - above[4 * idx + 3];
    below[4 * idx + 0] = above[4 * idx + 0] + below[4 * idx + 0] * keep;
    below[4 * idx + 1] = above[4 * idx + 1] + below[4 * idx + 1] * keep;
    below[4 * idx + 2] = above[4 * idx + 2] + below[4 * idx + 2] * keep;
    below[4 * idx + 3] = above[4 * idx + 3] + below[4 * idx + 3] * keep;
  }
}

/** The device box an object touches, its shadow included. */
static dt_canvas_box_t _object_box(const cairo_matrix_t *matrix, const dt_canvas_t *canvas,
                                   const dt_canvas_object_t *object, const dt_canvas_shadow_t *shadow,
                                   const double pixels_per_unit)
{
  dt_canvas_box_t box = { 0, 0, 0, 0 };
  if(dt_canvas_object_is_frame(object))
  {
    double corners[8];
    dt_canvas_object_corners(object, corners);
    box = _box_of_points(matrix, corners, 4, 1.0);
  }
  else
  {
    dt_canvas_route_t route;
    if(!dt_canvas_connector_route(canvas, object, &route) || route.point_count <= 0) return box;
    const double line_width = object->connector.line_width > 0.0f ? object->connector.line_width : 2.0;
    const double reach = (line_width + PAINT_ARROW_LENGTH * fmax(line_width / 2.0, 1.0)) * pixels_per_unit;
    box = _box_of_points(matrix, route.points, route.point_count, reach);
  }
  if(dt_canvas_shadow_visible(shadow) && shadow->blur > 0.0f)
  {
    const double reach = (fabs(shadow->offset_x) + fabs(shadow->offset_y) + COMPOSE_SHADOW_SIGMAS * shadow->blur)
                         * pixels_per_unit;
    box = _box_grow(&box, (int)ceil(reach) + 1);
  }
  return box;
}

/** Composite one band of the device plane and hand it to the context. */
static void _paint_band(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options,
                        const cairo_matrix_t *matrix, const dt_canvas_box_t *band, const double scale_x,
                        const double scale_y)
{
  const double pixels_per_unit = _matrix_scale(matrix);
  dt_canvas_paint_options_t local = *options;
  local.clip = _box_to_user(matrix, band);
  cairo_font_options_t *font_options = cairo_font_options_create();
  cairo_get_font_options(cr, font_options);
  double clock = dt_get_wtime();
  _stats.pixels += (int64_t)band->width * band->height;

  // 1. The background, the grid and the pages: cairo, into the base layer.
  cairo_surface_t *base = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, band->width, band->height);
  if(cairo_surface_status(base) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(base);
    cairo_font_options_destroy(font_options);
    return;
  }
  cairo_t *base_cr = _layer_context(base, matrix, band, font_options);
  if(local.draw_background && _is_paper(canvas->background_style))
  {
    _paint_paper(base_cr, canvas, &local);
  }
  else if(local.draw_background)
  {
    _set_color(base_cr, &canvas->background, local.for_display);
    cairo_paint(base_cr);
  }
  if(local.draw_grid) _paint_grid(base_cr, canvas, &local);
  if(local.draw_grid) _paint_pages(base_cr, canvas, &local);
  cairo_destroy(base_cr);

  const size_t canvas_floats = (size_t)band->width * band->height * 4;
  float *canvas_rgba = NULL;
  gboolean canvas_owned = FALSE;
  if(!IS_NULL_PTR(local.cache)) canvas_rgba = dt_canvas_surface_cache_scratch(local.cache, canvas_floats * sizeof(float));
  if(IS_NULL_PTR(canvas_rgba))
  {
    canvas_rgba = dt_alloc_align_float(canvas_floats);
    canvas_owned = TRUE;
  }
  if(IS_NULL_PTR(canvas_rgba))
  {
    cairo_surface_destroy(base);
    cairo_font_options_destroy(font_options);
    return;
  }
  _layer_linearise(base, 1.0f, canvas_rgba);
  cairo_surface_destroy(base);
  _stats.background_seconds += dt_get_wtime() - clock;
  clock = dt_get_wtime();

  // 2. Every object, back to front: its own layer, its cutout, its shadow, then over the canvas.
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(canvas, idx);
    if(IS_NULL_PTR(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    const float opacity = 1.0f - CLAMP(object->transparency, 0.0f, 1.0f);
    if(opacity <= 0.0f) continue;
    dt_canvas_shadow_t shadow;
    dt_canvas_object_effective_shadow(canvas, object, &shadow);
    const gboolean shadowed = dt_canvas_shadow_visible(&shadow);
    dt_canvas_box_t object_box = _object_box(matrix, canvas, object, &shadow, pixels_per_unit);
    if(_box_empty(&object_box)) continue;
    // The layer keeps the shadow's reach beyond the band, so a blur at the band's edge is whole.
    const int reach = shadowed ? (int)ceil((fabs(shadow.offset_x) + fabs(shadow.offset_y)
                                             + COMPOSE_SHADOW_SIGMAS * fabs(shadow.blur)) * pixels_per_unit) + 1
                               : 0;
    const dt_canvas_box_t band_reach = _box_grow(band, reach);
    const dt_canvas_box_t layer_box = _box_intersect(&object_box, &band_reach);
    const dt_canvas_box_t area = _box_intersect(&layer_box, band);
    if(_box_empty(&layer_box) || _box_empty(&area)) continue;

    cairo_surface_t *layer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, layer_box.width, layer_box.height);
    if(cairo_surface_status(layer) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy(layer);
      continue;
    }
    const gboolean cut = _object_cut(object);
    _stats.objects++;
    _stats.layers += cut ? 3 : 1;
    if(shadowed) _stats.shadows++;
    const double object_clock = dt_get_wtime();
    cairo_t *layer_cr = _layer_context(layer, matrix, &layer_box, font_options);
    if(cut)
      _paint_fill(layer_cr, canvas, object, &local);
    else
      _paint_object_pixels(layer_cr, canvas, object, &local);
    _apply_cutout(layer_cr, canvas, object, &local, pixels_per_unit, TRUE);
    cairo_destroy(layer_cr);

    const size_t layer_pixels = (size_t)layer_box.width * layer_box.height;
    float *layer_rgba = dt_alloc_align_float(layer_pixels * 4);
    if(IS_NULL_PTR(layer_rgba))
    {
      cairo_surface_destroy(layer);
      continue;
    }
    _layer_linearise(layer, opacity, layer_rgba);
    cairo_surface_destroy(layer);
    _stats.paint_seconds += dt_get_wtime() - object_clock;

    // A cut-out frame is three layers over each other in linear light: its background filling
    // the shape's whole support, its content feathered by the shape, its border past the feather.
    if(cut)
    {
      for(int part = 0; part < 2; part++)
      {
        cairo_surface_t *extra = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, layer_box.width, layer_box.height);
        if(cairo_surface_status(extra) != CAIRO_STATUS_SUCCESS)
        {
          cairo_surface_destroy(extra);
          continue;
        }
        cairo_t *extra_cr = _layer_context(extra, matrix, &layer_box, font_options);
        gboolean painted = TRUE;
        if(part == 0)
        {
          _paint_object_pixels(extra_cr, canvas, object, &local);
          _apply_cutout(extra_cr, canvas, object, &local, pixels_per_unit, FALSE);
        }
        else
        {
          painted = _paint_cut_border(extra_cr, canvas, object, &local, pixels_per_unit);
        }
        cairo_destroy(extra_cr);
        float *extra_rgba = painted ? dt_alloc_align_float(layer_pixels * 4) : NULL;
        if(!IS_NULL_PTR(extra_rgba))
        {
          _layer_linearise(extra, opacity, extra_rgba);
          _layer_over(layer_rgba, extra_rgba, layer_pixels);
          dt_free_align(extra_rgba);
        }
        cairo_surface_destroy(extra);
      }
    }
    const double shadow_clock = dt_get_wtime();
    if(shadowed && shadow.blur < 0.0f) _layer_inset_shadow(layer_rgba, &layer_box, &shadow, pixels_per_unit);
    if(shadowed && shadow.blur > 0.0f) _canvas_shadow(canvas_rgba, band, layer_rgba, &layer_box, &area, &shadow, pixels_per_unit);
    _stats.shadow_seconds += dt_get_wtime() - shadow_clock;
    _canvas_over(canvas_rgba, band, layer_rgba, &layer_box, &area);
    dt_free_align(layer_rgba);
  }

  _stats.objects_seconds += dt_get_wtime() - clock;
  clock = dt_get_wtime();

  // 3. Back to 8 bits, and onto the context, pixel for pixel: the band is in the surface's
  //    own pixels, so the device scale is undone on the way.
  cairo_surface_t *encoded = cairo_image_surface_create(CAIRO_FORMAT_RGB24, band->width, band->height);
  if(cairo_surface_status(encoded) == CAIRO_STATUS_SUCCESS)
  {
    _canvas_encode(canvas_rgba, encoded, local.for_display);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_scale(cr, 1.0 / scale_x, 1.0 / scale_y);
    cairo_set_source_surface(cr, encoded, band->x, band->y);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, band->x, band->y, band->width, band->height);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  cairo_surface_destroy(encoded);
  cairo_font_options_destroy(font_options);
  if(canvas_owned) dt_free_align(canvas_rgba);
  _stats.encode_seconds += dt_get_wtime() - clock;
}

/** The gutter frames: one gutter out from every frame, over everything, as a guide. */
static void _paint_gutters(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  if(!(canvas->grid_flags & DT_CANVAS_GUTTER_VISIBLE) || canvas->gutter <= 0.0f) return;
  if(canvas->gutter_color.alpha <= 0.0f) return;
  cairo_save(cr);
  _set_color(cr, &canvas->gutter_color, options->for_display);
  cairo_set_line_width(cr, options->units_per_pixel);
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(canvas, idx);
    if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
    const dt_canvas_rect_t reach = { bounds.x - canvas->gutter, bounds.y - canvas->gutter,
                                     bounds.width + 2.0 * canvas->gutter, bounds.height + 2.0 * canvas->gutter };
    if(!_rect_intersects(&options->clip, &reach)) continue;
    // Gutters overlap between neighbours one gutter apart: they are what the snapping keeps clear, not a margin.
    cairo_save(cr);
    cairo_translate(cr, object->x, object->y);
    cairo_rotate(cr, object->rotation);
    cairo_rectangle(cr, -object->width * 0.5 - canvas->gutter, -object->height * 0.5 - canvas->gutter,
                    object->width + 2.0 * canvas->gutter, object->height + 2.0 * canvas->gutter);
    cairo_restore(cr);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

/* --- entry points ------------------------------------------------------------- */

void dt_canvas_paint_object(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                            const dt_canvas_paint_options_t *options)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || IS_NULL_PTR(options)) return;
  if(object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN) return;
  if(object->kind != DT_CANVAS_OBJECT_CONNECTOR)
  {
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
    if(!_rect_intersects(&options->clip, &bounds)) return;
  }
  _paint_object_pixels(cr, canvas, object, options);
}

void dt_canvas_paint(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(canvas) || IS_NULL_PTR(options)) return;
  _luts_init();
  memset(&_stats, 0, sizeof(_stats));
  const double start = dt_get_wtime();
  // User space to the surface's PIXELS: cairo's device space stops short of the surface's own
  // device scale, and on a 2x screen a layer sized in device units is half the resolution.
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  double scale_x = 1.0;
  double scale_y = 1.0;
  cairo_surface_get_device_scale(cairo_get_group_target(cr), &scale_x, &scale_y);
  if(!(scale_x > 0.0) || !(scale_y > 0.0))
  {
    scale_x = 1.0;
    scale_y = 1.0;
  }
  cairo_matrix_t to_pixels;
  cairo_matrix_init_scale(&to_pixels, scale_x, scale_y);
  cairo_matrix_multiply(&matrix, &matrix, &to_pixels);

  // The device box to composite: the context's clip, narrowed to the area the caller asked for.
  double clip_x1 = 0.0;
  double clip_y1 = 0.0;
  double clip_x2 = 0.0;
  double clip_y2 = 0.0;
  cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
  const double clip_corners[8] = { clip_x1, clip_y1, clip_x2, clip_y1, clip_x2, clip_y2, clip_x1, clip_y2 };
  dt_canvas_box_t box = _box_of_points(&matrix, clip_corners, 4, 0.0);
  if(options->clip.width > 0.0 && options->clip.height > 0.0)
  {
    const double asked[8] = { options->clip.x, options->clip.y, options->clip.x + options->clip.width, options->clip.y,
                              options->clip.x + options->clip.width, options->clip.y + options->clip.height,
                              options->clip.x, options->clip.y + options->clip.height };
    const dt_canvas_box_t asked_box = _box_of_points(&matrix, asked, 4, 0.0);
    box = _box_intersect(&box, &asked_box);
  }
  if(_box_empty(&box)) return;

  // A page at print resolution can outgrow memory as floats: composite it in bands.
  const int rows_per_band = MAX(1, COMPOSE_BAND_MAX_PIXELS / MAX(box.width, 1));
  for(int top = box.y; top < box.y + box.height; top += rows_per_band)
  {
    const dt_canvas_box_t band = { box.x, top, box.width, MIN(rows_per_band, box.y + box.height - top) };
    _paint_band(cr, canvas, options, &matrix, &band, scale_x, scale_y);
  }
  if(options->draw_grid) _paint_gutters(cr, canvas, options);
  _stats.total_seconds = dt_get_wtime() - start;
  dt_print(DT_DEBUG_PERF,
           "[canvas paint] %" G_GINT64_FORMAT " px, %d objects (%d layers, %d shadows): background %.1f ms, objects %.1f ms "
           "(cairo %.1f, shadows %.1f), encode %.1f ms, total %.1f ms\n",
           _stats.pixels, _stats.objects, _stats.layers, _stats.shadows, _stats.background_seconds * 1000.0,
           _stats.objects_seconds * 1000.0, _stats.paint_seconds * 1000.0, _stats.shadow_seconds * 1000.0,
           _stats.encode_seconds * 1000.0, _stats.total_seconds * 1000.0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
