/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

/** @file pixel/dust.c
 *
 * Everything here works on the *density* of the three colour channels, `-log10(value)`. Nothing
 * depends on the values' scale: a density difference is a ratio of values, so a scan's film base
 * or a raw's white level cancels out of it.
 *
 * Inside a painted zone, a speck is what stands above the density the zone would have without it:
 * the harmonic interpolation of the densities around the zone. Around a speck, for a source, the
 * other specks are told by the *top-hat*: the density minus its morphological opening, a peak
 * narrower than the opening standing out of it.
 *
 * The radii below are full-resolution pixels, as they were measured (doc/iop-notes.md); every
 * function scales them by dt_dust_image_t.scale.
 */

#include "pixel/dust.h"

#include "pixel/box_filters.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DUST_OPENING_RADIUS 15.0f    // the top-hat a source is checked with for other specks
#define DUST_RING 8.0f               // width of the ring a source is matched on
#define DUST_MIN_CONTRAST 5.0f       // a zone whose speck stands out less than this, in deviations, holds none
#define DUST_GROW_CONTRAST 3.0f      // the speck is what stands above this many deviations...
#define DUST_GROW_FRACTION 0.1f      // ... and above this fraction of its peak
#define DUST_BORDER_RADIUS 3.0f      // the densities around a zone are box means of this radius
#define DUST_MIN_BLOCKS 16           // a smoothing is used only on a zone holding this many of its boxes
#define DUST_OTHER_SPECK 6.0f        // a top-hat this many deviations above its median is a speck
#define DUST_SOLVER_TOLERANCE 1e-5f  // the background is solved until no density moves by more
#define DUST_VALUE_FLOOR 1e-6f       // values below are read as this, so that a black pixel is no peak
#define DUST_OUTLINE_MIN_TOLERANCE 0.75f // an outline strays at least this far from the pixel edges: their steps
#define DUST_OUTLINE_WIDTH_SHARE 3.0f    // ... and at most the speck's half width over this

/** The smoothings a faint speck is looked for at: the box radii, in full-resolution pixels. A wide
 * speck that blocks as little light as the grain varies stands out of a box mean of its size. */
static const float _dust_scales[] = { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 8.0f };

/** A scratch is a line of a few pixels, as faint per pixel as the grain: a box of any size averages
 * as much background as line, and only a mean along the line brings it out. Measured on a scratch
 * of a 35 mm scan, 3 px wide and 0.008 D over grain of 0.0047 D: 4 to 5 deviations out of a box,
 * 40 out of a mean over 270 px of it. Against the background interpolated from around the zone it
 * does not come out at all, whatever the smoothing: the image varies inside a long zone as much as
 * the scratch does, slowly, and the excess deviates by 0.006 D on a box of 17 px as on one pixel. A
 * line is therefore measured against its sides: along a segment, the excess averaged across a box of
 * DUST_LINE_RADIUS less the mean of the same boxes DUST_LINE_SIDE away on either side.
 *
 * Out of a box, a speck elsewhere in the zone stands out more than such a scratch does out of the
 * best segment (19 deviations against 7 on that scan), and both are defects: what tells which one
 * the user meant is the zone. A dab is painted over a speck; a stroke, DUST_STROKE_ELONGATION times
 * longer than wide or more, along a scratch, and the scratch is looked for within DUST_LINE_ANGLES
 * steps of DUST_LINE_ANGLE_STEP of the stroke's axis, along segments of DUST_LINE_HALF pixels each
 * way, full-resolution; at the best angle, it is measured again along DUST_LINE_HALF_LONG, which a
 * scratch varying in strength along it varies less over. Off the line by half a step, a segment
 * strays 0.4 px from it at its ends, the long one 0.8 px, within such a line's width.
 *
 * Along it, that scratch stands out over a third of the stroke: denser than the film for 200 px, at
 * 7 to 13 deviations per 68 px, then lighter, a third as strong, bending by up to 7 px off its
 * straight line; a segment of DUST_LINE_HALF pixels each way stands 2 to 4 deviations out there. Step
 * by step along the segment that stands out most, a pixel across and an angle step at most from the
 * last, the path left it: in that part, a speck 11 px beside it standing 4.7 deviations out, and the
 * grain, drew the path 13 px off over 120 px, to the stroke's side. The path kept is the one that
 * stands out most over the whole of each way from the peak instead, by steps of DUST_LINE_STEP, each a
 * pixel across at most from the last, darker or lighter: a step counts for how far its segment stands
 * out of its polarity beyond DUST_FOLLOW_FLOOR deviations, up to DUST_FOLLOW_CEILING, so that a speck
 * the segment crosses, a hundred deviations out, draws the path no more than the scratch does; a
 * pixel moved across costs DUST_FOLLOW_MOVE, a change of polarity DUST_FOLLOW_FLIP. It ends where it
 * would leave the stroke going on: where the scratch leaves it, or at its end, which the user painted
 * along the scratch. On the five strokes painted along that scratch, it runs along it from end to
 * end. The floor taken at a half or one and a half, the cost per pixel at a half or two, the cost of
 * a change at one or six, the path moves by 0.7 px on average at most, its ends by 33 px at most; a
 * ceiling of 6 or none, by a pixel at most. On a synthetic scratch, without a ceiling, a speck 9 or
 * 12 px beside it draws the path to it. Without the polarity, a change free, the path stays where it
 * is, but steps standing out of the other polarity lengthen where the scratch counts as seen by 50 px.
 *
 * Its end hooks: from straight up, it bends by 10 px over 75 px, then turns to run 60 px across, and
 * the user painted the stroke along it. Off a straight line by a pixel per step at most, the path
 * cannot turn so; it follows the stroke's points instead, averaged over DUST_STROKE_SMOOTH pixels each
 * way, the offsets taken along the stroke's normal and the segments bending as the stroke does. Along
 * the stroke rebuilt from that zone's mask, the path runs to the hook's end; allowed two pixels per
 * step instead of one, it is the same within a pixel. A zone given without its stroke is followed
 * along the straight line at the angle found.
 * The scratch lies between its sides across that path, where the median along it of its colour's
 * profile across it falls to DUST_LINE_FOOT of its extremum; DUST_LINE_BAND pixels each side of the
 * path at the least. Measured on the seven strokes the GUI dumped over the scratches of that scan,
 * against the scratch's centre and half width read across the stroke in blue - green, per block of
 * 16 px: the scratch is 3.8 to 4.8 px from its centre to where it falls to half (median of each
 * stroke), up to 8. With a fixed band of 3 pixels each side, a pixel wider, the shape held 62 to 87 %
 * of it, its centre out of the shape in 0 to 4 blocks of 5 to 35 and its sides in half of them or
 * more; between its sides at a quarter of the median's extremum, 85 to 100 %; at 15 %, 93 to 100 %,
 * the band 6 to 10 px each side of its middle. The mean instead of the median held 94 to 100 % at a
 * quarter, but a speck 9 px beside a synthetic scratch drew its sides 9 px off it; the median of how
 * far it departs either way, 77 to 100 %, the grain's own departure flattening the scratch in it.
 * What is left out at 15 % is where the path leaves the scratch, at the hook's corner and an end, by
 * 5 to 13 px. */
#define DUST_LINE_HALF 16.0f
#define DUST_LINE_HALF_LONG 32.0f
#define DUST_LINE_ANGLES 8
#define DUST_LINE_ANGLE_STEP ((float)M_PI / 64.0f)
#define DUST_LINE_RADIUS 1.0f
#define DUST_LINE_SIDE 4.0f
#define DUST_LINE_EVEN 0.5f // a line holds this fraction of its mean over each third of a segment
/** A line stands out of the grain this much, in deviations: searched at 17 angles over every pixel
 * of a stroke, the grain alone reaches more than DUST_MIN_CONTRAST does for a speck. Measured: 5.3
 * on a stroke over synthetic grain; 10.6 for the scratch of that scan, 14.5 for one made like it. */
#define DUST_MIN_LINE_CONTRAST 7.0f
#define DUST_LINE_STEP 8.0f
#define DUST_STROKE_SMOOTH 4.0f
#define DUST_FOLLOW_FLOOR 1.0f
#define DUST_FOLLOW_CEILING 4.0f
#define DUST_FOLLOW_MOVE 1.0f
#define DUST_FOLLOW_FLIP 3.0f
#define DUST_LINE_FOLLOW 2.5f // a step of the path standing out this much is where the scratch was seen
#define DUST_LINE_BAND 3.0f
#define DUST_LINE_REACH 18.0f // the scratch's profile is read this far from its path each way
#define DUST_LINE_FLANK 0.67f // beyond this share of DUST_LINE_REACH, its level away from the scratch
#define DUST_LINE_FOOT 0.15f  // a scratch's side, where its profile falls to this share of its extremum
#define DUST_STROKE_ELONGATION 3.0f

/** The colours a scratch is looked for in, as weights of the channels' densities. A speck blocks light
 * in the three channels alike enough for their mean to find it (doc/iop-notes.md: red 0.61 and blue
 * 1.09 of green). A scratch in the emulsion takes off the layers it cuts, and on a colour negative the
 * top one first, the yellow dye. Measured on the two hooked scratches of a 35 mm scan, across the path
 * the GUI followed, the density within a pixel of it less that 5 to 10 px away, averaged over blocks
 * of 32 px of its length: blue 15 to 51 mD lighter than its sides, red 15 to 31 mD denser along the
 * lower half and flat at the top, so that their mean changes sign along them and falls to 0.5
 * deviations on some blocks, against the same measure 30 to 50 px aside. Blue less green stood 4 to 12
 * deviations out on every block, of one sign, the two channels' grain being partly shared: 3.4 mD for
 * the difference against 4.3 to 6.1 for blue alone. In it the hook's corner, which the mean does not
 * show, is plain. Each colour is tried, its line measured against its own grain, and the scratch is
 * followed in the one its path stands out most in. A difference adds the upper of its two layers,
 * blue over green over red, which a scratch takes off first: the polarity a scratch is found with in
 * it is that layer's. */
static const float _dust_line_colours[][3] = {
  { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f }, // the mean, as a speck is looked for in
  { 1.0f, 0.0f, 0.0f },
  { 0.0f, 1.0f, 0.0f },
  { 0.0f, 0.0f, 1.0f },
  { 0.0f, -1.0f, 1.0f }, // blue less green
  { -1.0f, 0.0f, 1.0f }, // blue less red
  { -1.0f, 1.0f, 0.0f }, // green less red
};
#define DUST_LINE_COLOURS ((int)(sizeof(_dust_line_colours) / sizeof(_dust_line_colours[0])))

/** A rectangle of the buffer with the densities of its pixels, and what is computed over them. */
typedef struct _dust_window_t
{
  int x0;
  int y0;
  int width;
  int height;
  float *density; // three planes, width × height each
  float *excess;  // width × height
  float *light;   // width × height, in a source search only: how far a pixel's density lies under it
} _dust_window_t;

static int _scaled(const float radius, const float scale, const int minimum)
{
  return MAX(minimum, (int)lrintf(radius * scale));
}

static void _window_cleanup(_dust_window_t *w)
{
  dt_free_align(w->density);
  dt_free_align(w->excess);
  dt_free_align(w->light);
}

/** Allocate the window's planes and fill its densities, channel by channel. The window is clipped
 * to the buffer by the caller. */
static dt_dust_status_t _window_densities(const dt_dust_image_t *image, _dust_window_t *w)
{
  const size_t n = (size_t)w->width * w->height;
  w->density = dt_alloc_align_float(3 * n);
  w->excess = dt_alloc_align_float(n);
  if(IS_NULL_PTR(w->density) || IS_NULL_PTR(w->excess)) return DT_DUST_NO_MEMORY;

  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < w->height; y++)
    for(int x = 0; x < w->width; x++)
    {
      const float *const px = image->rgba + 4 * ((size_t)(w->y0 + y) * image->width + w->x0 + x);
      for(int c = 0; c < 3; c++)
        w->density[c * n + (size_t)y * w->width + x] = -log10f(fmaxf(px[c], DUST_VALUE_FLOOR));
    }
  return DT_DUST_OK;
}

/** Fill a window with the densities of its pixels, their top-hat over an opening of radius `opening`
 * in `excess` (how far a dark speck stands above its surroundings) and their black top-hat over the
 * closing of the same radius in `light` (how far a light one lies under them), the largest of each
 * over the channels. */
static dt_dust_status_t _window_top_hats(const dt_dust_image_t *image, _dust_window_t *w, const int opening)
{
  dt_dust_status_t status = _window_densities(image, w);
  if(status != DT_DUST_OK) return status;

  const size_t n = (size_t)w->width * w->height;
  w->light = dt_alloc_align_float(n);
  float *open = dt_alloc_align_float(n);
  float *close = dt_alloc_align_float(n);
  if(IS_NULL_PTR(w->light) || IS_NULL_PTR(open) || IS_NULL_PTR(close))
  {
    dt_free_align(open);
    dt_free_align(close);
    return DT_DUST_NO_MEMORY;
  }

  for(size_t i = 0; i < n; i++) w->excess[i] = w->light[i] = -FLT_MAX;
  for(int c = 0; c < 3 && status == DT_DUST_OK; c++)
  {
    const float *const plane = w->density + c * n;
    memcpy(open, plane, n * sizeof(float));
    memcpy(close, plane, n * sizeof(float));
    if(dt_box_min(open, w->height, w->width, 1, opening) || dt_box_max(open, w->height, w->width, 1, opening)
       || dt_box_max(close, w->height, w->width, 1, opening) || dt_box_min(close, w->height, w->width, 1, opening))
    {
      status = DT_DUST_NO_MEMORY;
      break;
    }
    __OMP_PARALLEL_FOR_SIMD__(aligned(open, close:64))
    for(size_t i = 0; i < n; i++)
    {
      w->excess[i] = fmaxf(w->excess[i], plane[i] - open[i]);
      w->light[i] = fmaxf(w->light[i], close[i] - plane[i]);
    }
  }
  dt_free_align(open);
  dt_free_align(close);
  return status;
}

static int _compare_floats(const void *a, const void *b)
{
  const float x = *(const float *)a;
  const float y = *(const float *)b;
  return (x > y) - (x < y);
}

/** Median and robust standard deviation (1.4826 × the median absolute deviation) of `n` values, so
 * that the specks, a few hundred pixels out of thousands, inflate neither. */
static float _noise(const float *values, const size_t n, float *scratch, float *median)
{
  memcpy(scratch, values, n * sizeof(float));
  qsort(scratch, n, sizeof(float), _compare_floats);
  *median = scratch[n / 2];
  for(size_t i = 0; i < n; i++) scratch[i] = fabsf(values[i] - *median);
  qsort(scratch, n, sizeof(float), _compare_floats);
  return 1.4826f * scratch[n / 2];
}

/** The pixels of `allowed` 8-connected to the seed and within `limit` of it, written to `out`.
 * `stack` holds width × height ints. */
static void _flood(const uint8_t *allowed, uint8_t *out, int *stack, const int width, const int height,
                   const int sx, const int sy, const int limit)
{
  memset(out, 0, (size_t)width * height);
  int top = 0;
  stack[top++] = sy * width + sx;
  out[sy * width + sx] = 1;
  while(top)
  {
    const int p = stack[--top];
    const int px = p % width;
    const int py = p / width;
    // the eight neighbours of p that are allowed, not yet reached, and close enough to the seed
    for(int dy = -1; dy <= 1; dy++)
      for(int dx = -1; dx <= 1; dx++)
      {
        const int x = px + dx;
        const int y = py + dy;
        const gboolean inside = x >= 0 && y >= 0 && x < width && y < height;
        if(!inside || out[y * width + x] || !allowed[y * width + x]) continue;
        if((x - sx) * (x - sx) + (y - sy) * (y - sy) > limit * limit) continue;
        out[y * width + x] = 1;
        stack[top++] = y * width + x;
      }
  }
}

/** `mask` grown by a disc of radius `radius`, written to `out`. */
static void _dilate(const uint8_t *mask, uint8_t *out, const int width, const int height, const int radius)
{
  memset(out, 0, (size_t)width * height);
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      // is any pixel of the mask within the disc around (x, y)?
      gboolean hit = FALSE;
      for(int dy = -radius; dy <= radius && !hit; dy++)
        for(int dx = -radius; dx <= radius && !hit; dx++)
        {
          const int sx = x + dx;
          const int sy = y + dy;
          hit = dx * dx + dy * dy <= radius * radius + radius && sx >= 0 && sy >= 0 && sx < width && sy < height
                && mask[sy * width + sx];
        }
      out[y * width + x] = hit;
    }
}

/** Copy the final region into the speck: its box, its centroid and its radius. */
static dt_dust_status_t _speck_from_region(const uint8_t *region, const _dust_window_t *w, dt_dust_speck_t *speck)
{
  int x0 = w->width, y0 = w->height, x1 = -1, y1 = -1;
  double sx = 0.0, sy = 0.0;
  int area = 0;
  for(int y = 0; y < w->height; y++)
    for(int x = 0; x < w->width; x++)
    {
      if(!region[y * w->width + x]) continue;
      x0 = MIN(x0, x);
      x1 = MAX(x1, x);
      y0 = MIN(y0, y);
      y1 = MAX(y1, y);
      sx += x;
      sy += y;
      area++;
    }

  speck->box_x = w->x0 + x0;
  speck->box_y = w->y0 + y0;
  speck->box_width = x1 - x0 + 1;
  speck->box_height = y1 - y0 + 1;
  speck->mask = dt_calloc_align((size_t)speck->box_width * speck->box_height);
  if(IS_NULL_PTR(speck->mask)) return DT_DUST_NO_MEMORY;

  const float cx = (float)(sx / area);
  const float cy = (float)(sy / area);
  // the long axis: the direction the pixels spread most along, from their second moments
  double vxx = 0.0, vyy = 0.0, vxy = 0.0;
  for(int y = y0; y <= y1; y++)
    for(int x = x0; x <= x1; x++)
    {
      if(!region[y * w->width + x]) continue;
      vxx += (x - cx) * (x - cx);
      vyy += (y - cy) * (y - cy);
      vxy += (x - cx) * (y - cy);
    }
  const float axis = 0.5f * atan2f((float)(2.0 * vxy), (float)(vxx - vyy));
  const float normal[2] = { -sinf(axis), cosf(axis) };
  float radius = 0.0f, half_width = 0.0f;
  for(int y = y0; y <= y1; y++)
    for(int x = x0; x <= x1; x++)
    {
      if(!region[y * w->width + x]) continue;
      speck->mask[(y - y0) * speck->box_width + x - x0] = 1;
      radius = fmaxf(radius, hypotf(x - cx, y - cy));
      half_width = fmaxf(half_width, fabsf((x - cx) * normal[0] + (y - cy) * normal[1]));
    }
  speck->center[0] = w->x0 + cx;
  speck->center[1] = w->y0 + cy;
  speck->radius = radius + 1.0f;
  speck->half_width = half_width + 1.0f;
  return DT_DUST_OK;
}

/** Fill `background` (three planes over the window) with the densities around the zone, as box means
 * of radius `border` over the pixels out of it, and over the zone with their harmonic interpolation:
 * fixed where the zone meets the rest of the window, free where it meets the window's edge, which is
 * then the image's. The box means keep the grain of the border out of the interpolation, where it
 * would make a smooth offset as strong as a faint speck once smoothed. Solved by successive
 * over-relaxation, channel by channel, tuned to the zone's thickness rather than to its box, which a
 * long stroke makes far wider. */
static dt_dust_status_t _background(const _dust_window_t *w, const uint8_t *inside, const int border,
                                    float *background)
{
  const size_t n = (size_t)w->width * w->height;

  // thickness: four times the area over the perimeter, the diameter of a disc
  size_t area = 0, perimeter = 0;
  for(int y = 0; y < w->height; y++)
    for(int x = 0; x < w->width; x++)
    {
      const size_t i = (size_t)y * w->width + x;
      if(!inside[i]) continue;
      area++;
      perimeter += x == 0 || y == 0 || x == w->width - 1 || y == w->height - 1 || !inside[i - 1] || !inside[i + 1]
                   || !inside[i - w->width] || !inside[i + w->width];
    }
  const int thickness = MAX(2, (int)(4 * area / MAX(perimeter, 1)));
  const float omega = 2.0f / (1.0f + sinf((float)M_PI / (float)(thickness + 1)));
  const int max_iter = 50 * thickness + 100;

  // around the zone, the box means of the densities out of it
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < w->height; y++)
    for(int x = 0; x < w->width; x++)
    {
      const size_t i = (size_t)y * w->width + x;
      for(int c = 0; c < 3; c++) background[c * n + i] = w->density[c * n + i];
      if(inside[i]) continue;
      float mean[3] = { 0.0f, 0.0f, 0.0f };
      int count = 0;
      for(int yy = MAX(0, y - border); yy <= MIN(w->height - 1, y + border); yy++)
        for(int xx = MAX(0, x - border); xx <= MIN(w->width - 1, x + border); xx++)
        {
          const size_t j = (size_t)yy * w->width + xx;
          if(inside[j]) continue;
          for(int c = 0; c < 3; c++) mean[c] += w->density[c * n + j];
          count++;
        }
      for(int c = 0; c < 3; c++) background[c * n + i] = mean[c] / count;
    }

  gboolean surrounded[3] = { TRUE, TRUE, TRUE };
  __OMP_PARALLEL_FOR__()
  for(int c = 0; c < 3; c++)
  {
    float *const b = background + c * n;
    // the zone starts at the mean of the pixels around it
    double sum = 0.0;
    size_t around = 0;
    for(size_t i = 0; i < n; i++)
    {
      if(inside[i]) continue;
      sum += b[i];
      around++;
    }
    if(around == 0)
    {
      surrounded[c] = FALSE;
      continue;
    }
    for(size_t i = 0; i < n; i++)
      if(inside[i]) b[i] = (float)(sum / around);

    for(int iter = 0; iter < max_iter; iter++)
    {
      float change = 0.0f;
      for(int y = 0; y < w->height; y++)
        for(int x = 0; x < w->width; x++)
        {
          const size_t i = (size_t)y * w->width + x;
          if(!inside[i]) continue;
          float neighbours = 0.0f;
          int count = 0;
          if(x > 0) { neighbours += b[i - 1]; count++; }
          if(x < w->width - 1) { neighbours += b[i + 1]; count++; }
          if(y > 0) { neighbours += b[i - w->width]; count++; }
          if(y < w->height - 1) { neighbours += b[i + w->width]; count++; }
          if(count == 0) continue;
          const float delta = omega * (neighbours / count - b[i]);
          b[i] += delta;
          change = fmaxf(change, fabsf(delta));
        }
      if(change < DUST_SOLVER_TOLERANCE) break;
    }
  }
  // a zone that covers the whole window has nothing around it to be interpolated from
  return surrounded[0] && surrounded[1] && surrounded[2] ? DT_DUST_OK : DT_DUST_NOT_FOUND;
}

/** The mean of `excess` over the zone pixels in the box of `radius` around each zone pixel, from
 * the integral images `sum` and `count`, (width + 1) × (height + 1) each. */
static void _smooth(const double *sum, const double *count, const uint8_t *inside, float *out, const int width,
                    const int height, const int radius)
{
  const int stride = width + 1;
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      out[i] = 0.0f;
      if(!inside[i]) continue;
      const int x0 = MAX(0, x - radius), x1 = MIN(width, x + radius + 1);
      const int y0 = MAX(0, y - radius), y1 = MIN(height, y + radius + 1);
      const double s = sum[(size_t)y1 * stride + x1] - sum[(size_t)y0 * stride + x1]
                       - sum[(size_t)y1 * stride + x0] + sum[(size_t)y0 * stride + x0];
      const double k = count[(size_t)y1 * stride + x1] - count[(size_t)y0 * stride + x1]
                       - count[(size_t)y1 * stride + x0] + count[(size_t)y0 * stride + x0];
      out[i] = (float)(s / k);
    }
}

/** The excess over the background in one of _dust_line_colours, averaged across the box of `radius`
 * around each zone pixel into `across`, through the integral image `sum`, whose first row and column
 * stay 0. */
static void _colour_across(const _dust_window_t *w, const float *background, const uint8_t *inside,
                           const float weights[3], double *sum, const double *count, float *across,
                           const int radius)
{
  const size_t n = (size_t)w->width * w->height;
  const int stride = w->width + 1;
  for(int y = 0; y < w->height; y++)
    for(int x = 0; x < w->width; x++)
    {
      const size_t i = (size_t)y * w->width + x;
      float e = 0.0f;
      for(int c = 0; c < 3 && inside[i]; c++) e += weights[c] * (w->density[c * n + i] - background[c * n + i]);
      const size_t o = (size_t)(y + 1) * stride + x + 1;
      sum[o] = sum[o - 1] + sum[o - stride] - sum[o - stride - 1] + e;
    }
  _smooth(sum, count, inside, across, w->width, w->height, radius);
}

/** A line through each zone pixel, against its sides: along the segment of `half` pixels each way at
 * angle `theta`, the mean of `across` (an excess already averaged across a small box) less its value
 * `side` pixels away on whichever side of the segment is nearer to it, where all of that lies in the
 * zone, marked in `fits`; 0 elsewhere. A line runs the whole segment, a speck it crosses only part of it, however
 * much it stands out there: a mean above 0 counts in `dark` only where each third of the segment
 * holds DUST_LINE_EVEN of it, and one below 0 in `light` likewise; elsewhere on its own side, 0. On
 * the other side each holds the mean as it is, the grain that side's noise is measured on. `mean`,
 * when given, gets the mean everywhere, which a region is grown on: along a line it does not dip
 * where a third of the segment does. `offsets` holds 2 × (2 × half + 1) ints, `response` and `valid`
 * width × height floats and bytes. Returns how many pixels fit. */
static int _smooth_line(const float *across, const uint8_t *inside, float *dark, float *light, float *mean,
                        uint8_t *fits, int *offsets, float *response, uint8_t *valid, const int width,
                        const int height, const int half, const int side, const float theta)
{
  const int samples = 2 * half + 1;
  const float c = cosf(theta), s = sinf(theta);
  // each pixel less the side of it nearer to it, across the segment: beside a line, or a speck, one
  // side lies on it and the other on the background, which a mean of both would read as a line of the
  // other sign
  const int left_x = (int)lrintf(-side * s), left_y = (int)lrintf(side * c);
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const int lx = x + left_x, ly = y + left_y, rx = x - left_x, ry = y - left_y;
      valid[i] = inside[i] && lx >= 0 && ly >= 0 && lx < width && ly < height && inside[(size_t)ly * width + lx]
                 && rx >= 0 && ry >= 0 && rx < width && ry < height && inside[(size_t)ry * width + rx];
      if(!valid[i]) continue;
      const float left = across[(size_t)ly * width + lx], right = across[(size_t)ry * width + rx];
      response[i] = across[i] - (fabsf(left - across[i]) < fabsf(right - across[i]) ? left : right);
    }
  // each sample along the segment
  for(int j = 0; j < samples; j++)
  {
    const float t = (float)(j - half);
    offsets[2 * j] = (int)lrintf(t * c);
    offsets[2 * j + 1] = (int)lrintf(t * s);
  }
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      dark[i] = light[i] = 0.0f;
      if(mean) mean[i] = 0.0f;
      fits[i] = 0;
      if(!inside[i]) continue;
      float thirds[3] = { 0.0f, 0.0f, 0.0f };
      int counts[3] = { 0, 0, 0 };
      gboolean all_in = TRUE;
      for(int j = 0; j < samples && all_in; j++)
      {
        const int sx = x + offsets[2 * j], sy = y + offsets[2 * j + 1];
        all_in = sx >= 0 && sy >= 0 && sx < width && sy < height && valid[(size_t)sy * width + sx];
        if(!all_in) break;
        const int third = 3 * j / samples;
        counts[third]++;
        thirds[third] += response[(size_t)sy * width + sx];
      }
      if(!all_in) continue;
      const float m = (thirds[0] + thirds[1] + thirds[2]) / samples;
      for(int t = 0; t < 3; t++) thirds[t] /= counts[t];
      const float lowest = fminf(thirds[0], fminf(thirds[1], thirds[2]));
      const float highest = fmaxf(thirds[0], fmaxf(thirds[1], thirds[2]));
      dark[i] = m <= 0.0f || lowest >= DUST_LINE_EVEN * m ? m : 0.0f;
      light[i] = m >= 0.0f || highest <= DUST_LINE_EVEN * m ? m : 0.0f;
      if(mean) mean[i] = m;
      fits[i] = 1;
    }
  int fitting = 0;
  for(size_t i = 0; i < (size_t)width * height; i++) fitting += fits[i];
  return fitting;
}

/** The noise of an excess over the zone, for a speck of `polarity`, from the side of the excess it
 * does not reach: only the grain goes there, a dark speck only adds to the excess and a light one
 * only takes from it, and there |excess| is half-normal, of median 0.6745 σ. Zero when too few
 * pixels are on that side to tell. */
static float _opposite_noise(const float *excess, const uint8_t *inside, const size_t n, const int polarity,
                             float *scratch)
{
  size_t count = 0;
  for(size_t i = 0; i < n; i++)
    if(inside[i] && polarity * excess[i] < 0.0f) scratch[count++] = -polarity * excess[i];
  if(count < DUST_MIN_BLOCKS) return 0.0f;
  qsort(scratch, count, sizeof(float), _compare_floats);
  return scratch[count / 2] / 0.6745f;
}

/** Whether the zone is a stroke, DUST_STROKE_ELONGATION times longer than wide or more, from the
 * second moments of its pixels, and the angle of its long axis, radians from the x axis towards y. */
static gboolean _zone_is_stroke(const uint8_t *inside, const int width, const int height, float *axis)
{
  double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      if(!inside[(size_t)y * width + x]) continue;
      n++;
      sx += x;
      sy += y;
      sxx += (double)x * x;
      syy += (double)y * y;
      sxy += (double)x * y;
    }
  if(n < 2.0) return FALSE;
  const double vxx = sxx / n - (sx / n) * (sx / n);
  const double vyy = syy / n - (sy / n) * (sy / n);
  const double vxy = sxy / n - (sx / n) * (sy / n);
  const double spread = sqrt((vxx - vyy) * (vxx - vyy) + 4.0 * vxy * vxy);
  const double major = 0.5 * (vxx + vyy + spread);
  const double minor = 0.5 * (vxx + vyy - spread);
  *axis = (float)(0.5 * atan2(2.0 * vxy, vxx - vyy));
  // the variances of a band are its length and width squared over 12
  return major >= DUST_STROKE_ELONGATION * DUST_STROKE_ELONGATION * minor;
}

/** How far a region reaches along `axis`: the spread of its pixels' projections on it. */
static float _reach(const uint8_t *region, const int width, const int height, const float axis)
{
  const float c = cosf(axis), s = sinf(axis);
  float lowest = FLT_MAX, highest = -FLT_MAX;
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      if(!region[(size_t)y * width + x]) continue;
      const float t = x * c + y * s;
      lowest = fminf(lowest, t);
      highest = fmaxf(highest, t);
    }
  return highest >= lowest ? highest - lowest : 0.0f;
}

/** The strongest speck among the smoothings tried so far. */
typedef struct _dust_best_t
{
  float contrast; // its peak over its noise
  float peak;
  float sigma;
  int polarity;
  int seed_x;
  int seed_y;
} _dust_best_t;

/** Try one smoothing of the excess, read where it `fits`: darker than the background, as dust on a
 * scan, in `dark`, or lighter, as a pinhole or dust on the film when it was exposed, in `light` (the
 * same one for a box), each measured against the grain on the side the other one reaches. What stands
 * out more than the best so far replaces it, and `best_field` with it, the smoothing signed so that
 * the speck is positive. */
static void _consider(const float *dark, const float *light, const uint8_t *fits, const size_t n, const int width,
                      float *scratch, float *best_field, _dust_best_t *best)
{
  for(int sign = 1; sign >= -1; sign -= 2)
  {
    // the smoothing a darker speck is looked for in, then the one a lighter one is
    const float *const smoothed = sign > 0 ? dark : light;
    const float noise = _opposite_noise(smoothed, fits, n, sign, scratch);
    if(noise <= 0.0f) continue;
    size_t top = 0;
    for(size_t i = 1; i < n; i++)
      if(fits[i] && (!fits[top] || sign * smoothed[i] > sign * smoothed[top])) top = i;
    if(sign * smoothed[top] / noise <= best->contrast) continue;

    best->contrast = sign * smoothed[top] / noise;
    best->peak = sign * smoothed[top];
    best->sigma = noise;
    best->polarity = sign;
    best->seed_x = (int)(top % width);
    best->seed_y = (int)(top / width);
    for(size_t i = 0; i < n; i++) best_field[i] = sign * smoothed[i];
  }
}

/** The speck in `field`: above its noise and a fraction of its peak, connected to the peak, a pixel
 * wider, in the zone, written to `region`. */
static void _grow(const float *field, const _dust_best_t *best, const uint8_t *inside, uint8_t *allowed,
                  uint8_t *region, int *stack, const int width, const int height)
{
  const size_t n = (size_t)width * height;
  const float threshold = fmaxf(DUST_GROW_CONTRAST * best->sigma, DUST_GROW_FRACTION * best->peak);
  for(size_t i = 0; i < n; i++) allowed[i] = inside[i] && field[i] > threshold;
  _flood(allowed, region, stack, width, height, best->seed_x, best->seed_y, width + height);
  _dilate(region, allowed, width, height, 1);
  for(size_t i = 0; i < n; i++) allowed[i] = allowed[i] && inside[i];
  _flood(allowed, region, stack, width, height, best->seed_x, best->seed_y, width + height);
}

/** The scratch in a segment `field` (_smooth_line()'s for its polarity) at `angle`: its peak, and its
 * noise on the other side of the field, as _opposite_noise() reads it, but only farther than `band`
 * from the line through the peak. Nearer, the noise takes the scratch in: the filter turns it into
 * two bands of the other sign along it, its side away, and in a stroke a few times wider than them
 * these are a good part of what fits (3.5 mD read on that scan, 2.2 mD off the stroke). Fills `best`
 * and `best_field` with the segment's plain `mean`, signed so that the scratch is positive; leaves
 * `best` at a contrast of 0 when too few pixels are left to tell. */
static void _measure_line(const float *field, const float *mean, const uint8_t *fits, const size_t n,
                          const int width, const int polarity, const float angle, const int band, float *scratch,
                          float *best_field, _dust_best_t *best)
{
  memset(best, 0, sizeof(*best));
  best->polarity = polarity;
  size_t top = n;
  for(size_t i = 0; i < n; i++)
    if(fits[i] && (top == n || polarity * field[i] > polarity * field[top])) top = i;
  if(top == n) return;

  const int seed_x = (int)(top % width), seed_y = (int)(top / width);
  const float c = cosf(angle), s = sinf(angle);
  size_t m = 0;
  for(size_t i = 0; i < n; i++)
  {
    if(!fits[i] || polarity * field[i] >= 0.0f) continue;
    const float across = (float)((int)(i % width) - seed_x) * -s + (float)((int)(i / width) - seed_y) * c;
    if(fabsf(across) > band) scratch[m++] = -polarity * field[i];
  }
  if(m < DUST_MIN_BLOCKS) return;
  qsort(scratch, m, sizeof(float), _compare_floats);
  const float noise = scratch[m / 2] / 0.6745f;
  if(noise <= 0.0f) return;

  best->peak = polarity * field[top];
  best->sigma = noise;
  best->contrast = best->peak / noise;
  best->seed_x = seed_x;
  best->seed_y = seed_y;
  for(size_t i = 0; i < n; i++) best_field[i] = polarity * mean[i];
}

/** The line a scratch is followed along: a point per pixel of its length, x, y pairs, and the unit
 * normal at each, x, y pairs, in the window's pixels. */
typedef struct _dust_frame_t
{
  float *point;
  float *normal;
  int count;
} _dust_frame_t;

static void _frame_cleanup(_dust_frame_t *frame)
{
  dt_free_align(frame->point);
  dt_free_align(frame->normal);
  frame->count = 0;
}

static void _unit(float *x, float *y)
{
  const float length = hypotf(*x, *y);
  if(length <= 0.0f) return;
  *x /= length;
  *y /= length;
}

/** How many points a pixel apart a stroke of `count` points, x, y pairs, is resampled to: 1 without a
 * stroke a pixel long. */
static int _stroke_samples(const float *stroke, const int count)
{
  float length = 0.0f;
  for(int k = 0; stroke && k + 1 < count; k++)
    length += hypotf(stroke[2 * k + 2] - stroke[2 * k], stroke[2 * k + 3] - stroke[2 * k + 1]);
  return length >= 1.0f ? (int)length + 1 : 1;
}

/** The frame of a stroke through `count` points, x, y pairs in the window's pixels: resampled a pixel
 * apart, each point the mean of those `smooth` pixels each way along it, which the pointer's steps and
 * the hand's tremor vary by, and extended straight by `extend` pixels at both ends, where the brush
 * reaches past its first and last points. Without a stroke a pixel long, the straight line through
 * (x0, y0) at `theta`, `extend` pixels each way. FALSE when out of memory. */
static gboolean _frame_new(const float *stroke, const int count, const float x0, const float y0, const float theta,
                           const int smooth, const int extend, _dust_frame_t *frame)
{
  const int along = _stroke_samples(stroke, count);
  frame->count = along + 2 * extend;
  frame->point = dt_alloc_align_float((size_t)2 * frame->count);
  frame->normal = dt_alloc_align_float((size_t)2 * frame->count);
  float *resampled = dt_alloc_align_float((size_t)2 * along);
  if(IS_NULL_PTR(frame->point) || IS_NULL_PTR(frame->normal) || IS_NULL_PTR(resampled))
  {
    dt_free_align(resampled);
    _frame_cleanup(frame);
    return FALSE;
  }

  float *const middle = frame->point + 2 * extend;
  if(along == 1)
  {
    middle[0] = x0;
    middle[1] = y0;
  }
  else
  {
    // a point at each pixel of length along the stroke
    resampled[0] = stroke[0];
    resampled[1] = stroke[1];
    int done = 1;
    float start = 0.0f;
    for(int k = 0; k + 1 < count && done < along; k++)
    {
      const float dx = stroke[2 * k + 2] - stroke[2 * k], dy = stroke[2 * k + 3] - stroke[2 * k + 1];
      const float piece = hypotf(dx, dy);
      for(; done < along && done <= start + piece; done++)
      {
        const float t = (done - start) / piece;
        resampled[2 * done] = stroke[2 * k] + t * dx;
        resampled[2 * done + 1] = stroke[2 * k + 1] + t * dy;
      }
      start += piece;
    }
    for(; done < along; done++)
    {
      resampled[2 * done] = stroke[2 * count - 2];
      resampled[2 * done + 1] = stroke[2 * count - 1];
    }
    // smoothed over as many pixels on both sides, fewer near the ends
    for(int i = 0; i < along; i++)
    {
      const int r = MIN(smooth, MIN(i, along - 1 - i));
      float sx = 0.0f, sy = 0.0f;
      for(int j = i - r; j <= i + r; j++)
      {
        sx += resampled[2 * j];
        sy += resampled[2 * j + 1];
      }
      middle[2 * i] = sx / (2 * r + 1);
      middle[2 * i + 1] = sy / (2 * r + 1);
    }
  }
  dt_free_align(resampled);

  // straight on past both ends, along the stroke's direction there
  const int reach = MIN(MAX(smooth, 1), along - 1);
  float ux = cosf(theta), uy = sinf(theta), vx = ux, vy = uy;
  if(reach > 0)
  {
    ux = middle[0] - middle[2 * reach];
    uy = middle[1] - middle[2 * reach + 1];
    vx = middle[2 * (along - 1)] - middle[2 * (along - 1 - reach)];
    vy = middle[2 * (along - 1) + 1] - middle[2 * (along - 1 - reach) + 1];
    _unit(&ux, &uy);
    _unit(&vx, &vy);
  }
  else
  {
    ux = -ux;
    uy = -uy;
  }
  for(int e = 1; e <= extend; e++)
  {
    frame->point[2 * (extend - e)] = middle[0] + e * ux;
    frame->point[2 * (extend - e) + 1] = middle[1] + e * uy;
    frame->point[2 * (extend + along - 1 + e)] = middle[2 * (along - 1)] + e * vx;
    frame->point[2 * (extend + along - 1 + e) + 1] = middle[2 * (along - 1) + 1] + e * vy;
  }

  // the normals, from the direction over as many pixels each way
  const int span = MAX(smooth, 1);
  for(int i = 0; i < frame->count; i++)
  {
    const int a = MAX(0, i - span), b = MIN(frame->count - 1, i + span);
    float tx = frame->point[2 * b] - frame->point[2 * a], ty = frame->point[2 * b + 1] - frame->point[2 * a + 1];
    _unit(&tx, &ty);
    frame->normal[2 * i] = -ty;
    frame->normal[2 * i + 1] = tx;
  }
  return TRUE;
}

/** The frame's point `at`, moved `offset` pixels along its normal. */
static inline void _frame_point(const _dust_frame_t *frame, const int at, const float offset, float *x, float *y)
{
  *x = frame->point[2 * at] + offset * frame->normal[2 * at];
  *y = frame->point[2 * at + 1] + offset * frame->normal[2 * at + 1];
}

/** The line response of one segment of the frame, around its point `at` and `half` points each way,
 * each moved `offset` pixels along its normal, as _smooth_line() reads it: the mean of `across` less
 * its value `side` pixels further along the normal on the nearer side, over the samples whose three
 * points lie in the zone. FALSE when fewer than half of them do, at the end of a stroke. */
static gboolean _segment_at(const float *across, const uint8_t *inside, const int width, const int height,
                            const _dust_frame_t *frame, const int at, const int offset, const int half,
                            const int side, float *value)
{
  float total = 0.0f;
  int count = 0;
  for(int j = MAX(0, at - half); j <= MIN(frame->count - 1, at + half); j++)
  {
    const float nx = frame->normal[2 * j], ny = frame->normal[2 * j + 1];
    float cx = 0.0f, cy = 0.0f;
    _frame_point(frame, j, (float)offset, &cx, &cy);
    float v[3] = { 0.0f, 0.0f, 0.0f };
    gboolean all_in = TRUE;
    for(int k = 0; k < 3 && all_in; k++)
    {
      const float off = k == 0 ? 0.0f : (k == 1 ? (float)side : -(float)side);
      const int x = (int)lrintf(cx + off * nx);
      const int y = (int)lrintf(cy + off * ny);
      all_in = x >= 0 && y >= 0 && x < width && y < height && inside[(size_t)y * width + x];
      if(all_in) v[k] = across[(size_t)y * width + x];
    }
    if(!all_in) continue;
    total += v[0] - (fabsf(v[1] - v[0]) < fabsf(v[2] - v[0]) ? v[1] : v[2]);
    count++;
  }
  if(2 * count < 2 * half + 1) return FALSE;
  *value = total / count;
  return TRUE;
}

static gboolean _in_zone(const uint8_t *inside, const int width, const int height, const float x, const float y)
{
  const int ix = (int)lrintf(x), iy = (int)lrintf(y);
  return ix >= 0 && iy >= 0 && ix < width && iy < height && inside[(size_t)iy * width + ix];
}

/** What a step of a followed path is, in a byte of _follow_line()'s `back`: the pixels it moved
 * across from the step before, plus one, in the two low bits; the polarity it came from, 1 for a
 * lighter line, in the third; whether its segment stood out by DUST_LINE_FOLLOW, in the fourth. */
#define DUST_BACK_MOVE 3u
#define DUST_BACK_LIGHT 4u
#define DUST_BACK_SEEN 8u

/** Follow a scratch from (x0, y0), where it stands out with `polarity`, along the frame each way, as
 * the path that stands out most: the Viterbi path over the frame's points `step` apart, each at an
 * offset along the normal of at most a pixel from the last, and a polarity. A step at offset v counts
 * for its segment of the frame moved v pixels (_segment_at()), of the step's polarity, in deviations of
 * `sigma`, over DUST_FOLLOW_FLOOR and under DUST_FOLLOW_CEILING; a pixel moved across costs
 * DUST_FOLLOW_MOVE, a change of polarity DUST_FOLLOW_FLIP. A way ends where going on at the same
 * offset would leave the zone, the point or its sides `side` pixels across, at the end that scores
 * best: where the scratch leaves the stroke, or the stroke ends. It starts at the frame's point
 * nearest (x0, y0), at the offset of (x0, y0) from it.
 *
 * The path, from one end to the other, goes to `path` as x, y pairs, at most `room` points each way
 * and the start; returns how many. `seen` gets how far apart along the frame lie the steps whose
 * segment stands out by DUST_LINE_FOLLOW deviations, of their polarity, the start included, and
 * `strength` what the path scores, both ways together. `score` holds 4 × (2 `room` + 1) floats, `back`
 * `room` × 2 × (2 `room` + 1) bytes. */
static int _follow_line(const float *across, const uint8_t *inside, const int width, const int height,
                        const _dust_frame_t *frame, const float x0, const float y0, const int polarity,
                        const int half, const int side, const int step, const float sigma, const int room,
                        float *score, uint8_t *back, float *path, float *seen, float *strength)
{
  // where the path starts: the frame's nearest point, and the offset from it
  int start = 0;
  float nearest = FLT_MAX;
  for(int j = 0; j < frame->count; j++)
  {
    const float d = hypotf(frame->point[2 * j] - x0, frame->point[2 * j + 1] - y0);
    if(d >= nearest) continue;
    nearest = d;
    start = j;
  }
  const int offset0 = (int)lrintf((x0 - frame->point[2 * start]) * frame->normal[2 * start]
                                  + (y0 - frame->point[2 * start + 1]) * frame->normal[2 * start + 1]);

  const int across_count = 2 * room + 1; // offsets offset0 - room..offset0 + room
  const size_t row = (size_t)2 * across_count; // a step's states: offsets of a dark line, then of a light one
  float lowest = 0.0f, highest = 0.0f;
  // the way back is written from the middle of the path down, the way on from it up
  int first = room, last = room;
  float path_score = 0.0f;
  _frame_point(frame, start, (float)offset0, &path[2 * room], &path[2 * room + 1]);
  for(int direction = -1; direction <= 1; direction += 2)
  {
    float *before = score, *now = score + row;
    for(size_t i = 0; i < 2 * row; i++) score[i] = -FLT_MAX;
    before[(polarity > 0 ? 0 : across_count) + room] = 0.0f;
    float end_score = -FLT_MAX;
    int end_step = 0, end_offset = 0, end_light = polarity < 0;
    for(int k = 1;; k++)
    {
      gboolean any = FALSE;
      const int at = start + direction * k * step;
      for(int v = -MIN(k, room); v <= MIN(k, room) && k <= room && at >= 0 && at < frame->count; v++)
      {
        const int i = v + room;
        now[i] = now[across_count + i] = -FLT_MAX;
        float px = 0.0f, py = 0.0f;
        _frame_point(frame, at, (float)(offset0 + v), &px, &py);
        const float nx = frame->normal[2 * at], ny = frame->normal[2 * at + 1];
        float value = 0.0f;
        if(!_in_zone(inside, width, height, px, py)
           || !_in_zone(inside, width, height, px + side * nx, py + side * ny)
           || !_in_zone(inside, width, height, px - side * nx, py - side * ny)
           || !_segment_at(across, inside, width, height, frame, at, offset0 + v, half, side, &value))
          continue;
        for(int light = 0; light < 2; light++)
        {
          const float z = (light ? -value : value) / sigma;
          float best = -FLT_MAX;
          uint8_t from = 0;
          for(int move = -1; move <= 1; move++)
          {
            if(v + move < -room || v + move > room) continue;
            for(int was_light = 0; was_light < 2; was_light++)
            {
              const float previous = before[was_light * across_count + i + move];
              if(previous == -FLT_MAX) continue;
              const float total = previous - DUST_FOLLOW_MOVE * abs(move)
                                  - (was_light != light ? DUST_FOLLOW_FLIP : 0.0f);
              if(total <= best) continue;
              best = total;
              from = (uint8_t)((move + 1) | (was_light ? DUST_BACK_LIGHT : 0u));
            }
          }
          if(best == -FLT_MAX) continue;
          now[light * across_count + i] = best + fmaxf(fminf(z, DUST_FOLLOW_CEILING) - DUST_FOLLOW_FLOOR, 0.0f);
          back[(size_t)(k - 1) * row + light * across_count + i]
              = from | (z >= DUST_LINE_FOLLOW ? DUST_BACK_SEEN : 0u);
          any = TRUE;
        }
      }
      // the ends of the step before: where going on at the same offset leaves the zone
      for(int v = -MIN(k - 1, room); v <= MIN(k - 1, room); v++)
      {
        const int i = v + room;
        if(k <= room && at >= 0 && at < frame->count && (now[i] != -FLT_MAX || now[across_count + i] != -FLT_MAX))
          continue;
        for(int light = 0; light < 2; light++)
        {
          if(before[light * across_count + i] <= end_score) continue;
          end_score = before[light * across_count + i];
          end_step = k - 1;
          end_offset = v;
          end_light = light;
        }
      }
      if(!any) break;
      float *const swap = before;
      before = now;
      now = swap;
    }
    // back from the end the way scored best
    path_score += end_score;
    int v = end_offset, light = end_light;
    for(int k = end_step; k >= 1; k--)
    {
      const uint8_t from = back[(size_t)(k - 1) * row + light * across_count + v + room];
      const int at = direction < 0 ? room - k : room + k;
      _frame_point(frame, start + direction * k * step, (float)(offset0 + v), &path[2 * at], &path[2 * at + 1]);
      if(from & DUST_BACK_SEEN)
      {
        lowest = fminf(lowest, (float)(direction * k * step));
        highest = fmaxf(highest, (float)(direction * k * step));
      }
      v += (int)(from & DUST_BACK_MOVE) - 1;
      light = (from & DUST_BACK_LIGHT) != 0;
    }
    if(direction < 0) first = room - end_step;
    else last = room + end_step;
  }
  *seen = highest - lowest;
  *strength = path_score;
  // moved to the start of the buffer
  const int points = last - first + 1;
  memmove(path, path + 2 * first, sizeof(float) * 2 * points);
  return points;
}

/** The zone pixels within `radius` of the polyline `path` of `points` points, a pixel wider, in the
 * zone, written to `region`. `scratch_region` is width × height bytes. */
static void _band(const float *path, const int points, const float radius, const uint8_t *inside,
                  uint8_t *scratch_region, uint8_t *region, const int width, const int height)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      scratch_region[i] = 0;
      if(!inside[i]) continue;
      for(int k = 0; k + 1 < points || (points == 1 && k == 0); k++)
      {
        const float ax = path[2 * k], ay = path[2 * k + 1];
        const float ex = points > 1 ? path[2 * k + 2] - ax : 0.0f, ey = points > 1 ? path[2 * k + 3] - ay : 0.0f;
        const float length2 = ex * ex + ey * ey;
        const float t
            = length2 > 0.0f ? fminf(fmaxf(((x - ax) * ex + (y - ay) * ey) / length2, 0.0f), 1.0f) : 0.0f;
        if(hypotf(x - ax - t * ex, y - ay - t * ey) > radius) continue;
        scratch_region[i] = 1;
        break;
      }
    }
  _dilate(scratch_region, region, width, height, 1);
  for(size_t i = 0; i < (size_t)width * height; i++) region[i] = region[i] && inside[i];
}

/** Where a scratch's sides lie across its path: the profile across the path, in steps of half a pixel
 * out to `reach` each way, of the median over the path's length of `excess` (its colour's, unsmoothed)
 * in the scratch's `polarity`: a speck beside the path, on a short part of it, does not move it. Less
 * its level away from the scratch (beyond DUST_LINE_FLANK of `reach`), from its extremum
 * within `centre` of the path, the sides are where it falls under DUST_LINE_FOOT of it each way.
 * `shift` gets how far their middle lies along the path's normal (-dy, dx), `half` half their
 * distance, kept at its own value at the least; both stay as they came when nothing stands out, or
 * out of memory. */
static void _line_sides(const float *excess, const uint8_t *inside, const int width, const int height,
                        const float *path, const int points, const int polarity, const float centre,
                        const float reach, float *shift, float *half)
{
  const int bins = 2 * (int)ceilf(2.0f * reach) + 1, middle = bins / 2;
  size_t samples = 0;
  for(int k = 0; k + 1 < points; k++)
    samples += (size_t)ceilf(hypotf(path[2 * k + 2] - path[2 * k], path[2 * k + 3] - path[2 * k + 1]));
  float *values = dt_alloc_align_float(samples * bins);
  float *profile = dt_calloc_align_float(bins);
  int *taken = dt_calloc_align((size_t)bins * sizeof(int));
  if(IS_NULL_PTR(values) || IS_NULL_PTR(profile) || IS_NULL_PTR(taken)) goto cleanup;
  for(int k = 0; k + 1 < points; k++)
  {
    const float ax = path[2 * k], ay = path[2 * k + 1];
    const float ex = path[2 * k + 2] - ax, ey = path[2 * k + 3] - ay;
    const float length = hypotf(ex, ey);
    if(length <= 0.0f) continue;
    const float nx = -ey / length, ny = ex / length;
    for(int s = 0; s < (int)ceilf(length); s++)
      for(int b = 0; b < bins; b++)
      {
        const float t = 0.5f * (b - middle);
        const int x = (int)floorf(ax + ex * s / length + nx * t), y = (int)floorf(ay + ey * s / length + ny * t);
        if(x < 0 || y < 0 || x >= width || y >= height || !inside[(size_t)y * width + x]) continue;
        values[(size_t)b * samples + taken[b]++] = polarity * excess[(size_t)y * width + x];
      }
  }
  float level = 0.0f, flank = 0.0f;
  for(int b = 0; b < bins; b++)
  {
    if(taken[b] == 0) continue;
    float *const bin = values + (size_t)b * samples;
    qsort(bin, taken[b], sizeof(float), _compare_floats);
    profile[b] = bin[taken[b] / 2];
    if(fabsf(0.5f * (b - middle)) < DUST_LINE_FLANK * reach) continue;
    level += profile[b];
    flank++;
  }
  if(flank > 0.0f) level /= flank;
  int peak = middle;
  for(int b = 0; b < bins; b++)
    if(fabsf(0.5f * (b - middle)) <= centre && taken[b] > 0 && profile[b] > profile[peak]) peak = b;
  if(profile[peak] > level)
  {
    const float foot = level + DUST_LINE_FOOT * (profile[peak] - level);
    int low = peak, high = peak;
    while(low > 0 && taken[low - 1] > 0 && profile[low - 1] > foot) low--;
    while(high + 1 < bins && taken[high + 1] > 0 && profile[high + 1] > foot) high++;
    *shift = 0.25f * (low + high - 2 * middle);
    *half = fmaxf(*half, 0.25f * (high - low + 1));
  }

cleanup:
  dt_free_align(values);
  dt_free_align(profile);
  dt_free_align(taken);
}

/** `path` moved by `shift` along its normal (-dy, dx), at each point the mean of its segments' normals,
 * into `moved`. */
static void _shift_path(const float *path, const int points, const float shift, float *moved)
{
  for(int k = 0; k < points; k++)
  {
    float nx = 0.0f, ny = 0.0f;
    for(int j = MAX(0, k - 1); j < MIN(points - 1, k + 1); j++)
    {
      const float ex = path[2 * j + 2] - path[2 * j], ey = path[2 * j + 3] - path[2 * j + 1];
      const float length = hypotf(ex, ey);
      if(length <= 0.0f) continue;
      nx -= ey / length;
      ny += ex / length;
    }
    const float norm = hypotf(nx, ny);
    moved[2 * k] = path[2 * k] + (norm > 0.0f ? shift * nx / norm : 0.0f);
    moved[2 * k + 1] = path[2 * k + 1] + (norm > 0.0f ? shift * ny / norm : 0.0f);
  }
}

dt_dust_status_t dt_dust_find_in_zone(const dt_dust_image_t *image, const dt_dust_zone_t *zone,
                                      dt_dust_speck_t *speck)
{
  memset(speck, 0, sizeof(*speck));

  // the window: the zone's box and the ring of pixels around it the background is read from
  const int border = _scaled(DUST_BORDER_RADIUS, image->scale, 1);
  const int margin = border + 1;
  _dust_window_t w = { .x0 = MAX(0, zone->box_x - margin), .y0 = MAX(0, zone->box_y - margin) };
  w.width = MIN(image->width, zone->box_x + zone->box_width + margin) - w.x0;
  w.height = MIN(image->height, zone->box_y + zone->box_height + margin) - w.y0;
  if(w.width < 1 || w.height < 1) return DT_DUST_NOT_FOUND;
  const size_t n = (size_t)w.width * w.height;
  const size_t integral = (size_t)(w.width + 1) * (w.height + 1);

  uint8_t *inside = dt_calloc_align(n);
  uint8_t *fits = dt_alloc_align(n);
  uint8_t *allowed = dt_alloc_align(n);
  uint8_t *region = dt_alloc_align(n);
  int *stack = dt_alloc_align(n * sizeof(int));
  float *background = dt_alloc_align_float(3 * n);
  uint8_t *line_region = dt_alloc_align(n);
  float *smoothed = dt_alloc_align_float(n);
  float *across = dt_alloc_align_float(n);
  float *best = dt_alloc_align_float(n);
  float *line_field = dt_alloc_align_float(n);
  float *line_dark = dt_alloc_align_float(n);
  float *line_light = dt_alloc_align_float(n);
  float *line_mean = dt_alloc_align_float(n);
  float *scratch = dt_alloc_align_float(n);
  double *sum = dt_calloc_align(integral * sizeof(double));
  double *count = dt_calloc_align(integral * sizeof(double));
  const int line_radius = _scaled(DUST_LINE_RADIUS, image->scale, 0);
  const int line_side = _scaled(DUST_LINE_SIDE, image->scale, 2);
  const int line_half = _scaled(DUST_LINE_HALF, image->scale, 1);
  const int line_half_long = _scaled(DUST_LINE_HALF_LONG, image->scale, 1);
  int *offsets = dt_alloc_align((size_t)2 * (2 * line_half_long + 1) * sizeof(int));
  float *line_response = dt_alloc_align_float(n);
  uint8_t *line_valid = dt_alloc_align(n);
  const int line_step = _scaled(DUST_LINE_STEP, image->scale, 2);
  const float line_band = (float)_scaled(DUST_LINE_BAND, image->scale, 1);
  float line_half_band = line_band;
  // what a scratch is followed along, once found: the stroke, the path and the Viterbi's states
  _dust_frame_t frame = { 0 };
  float *stroke_points = NULL;
  float *path = NULL;
  float *colour_path = NULL;
  float *follow_score = NULL;
  uint8_t *follow_back = NULL;
  dt_dust_status_t status = DT_DUST_NO_MEMORY;
  if(IS_NULL_PTR(inside) || IS_NULL_PTR(fits) || IS_NULL_PTR(allowed) || IS_NULL_PTR(region) || IS_NULL_PTR(stack)
     || IS_NULL_PTR(background) || IS_NULL_PTR(smoothed) || IS_NULL_PTR(across) || IS_NULL_PTR(best)
     || IS_NULL_PTR(scratch) || IS_NULL_PTR(sum) || IS_NULL_PTR(count) || IS_NULL_PTR(offsets)
     || IS_NULL_PTR(line_region) || IS_NULL_PTR(line_field) || IS_NULL_PTR(line_dark) || IS_NULL_PTR(line_light)
     || IS_NULL_PTR(line_mean) || IS_NULL_PTR(line_response) || IS_NULL_PTR(line_valid))
    goto cleanup;

  // the zone, in the window
  int area = 0;
  for(int y = 0; y < w.height; y++)
    for(int x = 0; x < w.width; x++)
    {
      const int zx = w.x0 + x - zone->box_x;
      const int zy = w.y0 + y - zone->box_y;
      const gboolean in_box = zx >= 0 && zy >= 0 && zx < zone->box_width && zy < zone->box_height;
      inside[(size_t)y * w.width + x] = in_box && zone->mask[(size_t)zy * zone->box_width + zx];
      area += inside[(size_t)y * w.width + x];
    }
  status = DT_DUST_NOT_FOUND;
  if(area == 0) goto cleanup;

  status = _window_densities(image, &w);
  if(status != DT_DUST_OK) goto cleanup;
  status = _background(&w, inside, border, background);
  if(status != DT_DUST_OK) goto cleanup;

  // the excess over the background, averaged over the channels: a speck darkens all three, and the
  // mean keeps the grain symmetric, which its noise is measured on
  for(size_t i = 0; i < n; i++)
  {
    float e = 0.0f;
    for(int c = 0; c < 3; c++) e += w.density[c * n + i] - background[c * n + i];
    w.excess[i] = inside[i] ? e / 3.0f : 0.0f;
  }
  const int stride = w.width + 1;
  for(int y = 0; y < w.height; y++)
    for(int x = 0; x < w.width; x++)
    {
      const size_t i = (size_t)y * w.width + x;
      const size_t o = (size_t)(y + 1) * stride + x + 1;
      sum[o] = sum[o - 1] + sum[o - stride] - sum[o - stride - 1] + w.excess[i];
      count[o] = count[o - 1] + count[o - stride] - count[o - stride - 1] + inside[i];
    }

  // the smoothing and the polarity the speck stands out most at, among the smoothings the zone is
  // wide enough to measure; the peak and the noise are read where the box fits in the zone, as a box
  // cut by the zone's edge averages fewer pixels and varies more
  _dust_best_t found = { .polarity = 1 };
  int previous_radius = -1;
  for(size_t k = 0; k < sizeof(_dust_scales) / sizeof(_dust_scales[0]); k++)
  {
    const int radius = (int)lrintf(_dust_scales[k] * image->scale);
    const int box = 2 * radius + 1;
    if(radius == previous_radius) continue;
    previous_radius = radius;

    int fitting = 0;
    for(int y = 0; y < w.height; y++)
      for(int x = 0; x < w.width; x++)
      {
        const size_t i = (size_t)y * w.width + x;
        const gboolean in_window = x >= radius && y >= radius && x + radius < w.width && y + radius < w.height;
        const double k_in = in_window ? count[(size_t)(y + radius + 1) * stride + x + radius + 1]
                                            - count[(size_t)(y - radius) * stride + x + radius + 1]
                                            - count[(size_t)(y + radius + 1) * stride + x - radius]
                                            + count[(size_t)(y - radius) * stride + x - radius]
                                      : 0.0;
        fits[i] = inside[i] && k_in == (double)(box * box);
        fitting += fits[i];
      }
    if(fitting < DUST_MIN_BLOCKS * box * box) continue;

    _smooth(sum, count, inside, smoothed, w.width, w.height, radius);
    _consider(smoothed, smoothed, fits, n, w.width, scratch, best, &found);
  }

  // a zone painted as a stroke, several times longer than wide, points along a scratch: its axis
  float axis = 0.0f;
  const gboolean stroke = _zone_is_stroke(inside, w.width, w.height, &axis);

  // the scratch, in each colour: the excess averaged across a small box, then along segments within a
  // few steps of the stroke, against their sides, where all of that fits in the zone; at the angle it
  // stands out most at, measured again along the long segment, or the short one where the long one
  // does not fit, and where that stands out enough, followed from its peak. The colour whose path
  // stands out most is kept: a step of it counts for DUST_FOLLOW_CEILING deviations at most, so that a
  // speck on the scratch, standing out in every colour, does not choose for it
  _dust_best_t line = { .polarity = 1 };
  int line_points = 0, line_colour = 0;
  float line_score = -FLT_MAX, line_reach = -1.0f;
  const int band = line_side + 2 * line_radius + 1;
  if(stroke)
  {
    // the stroke, in the window, or the line at the angle found without it; the path, its start and
    // each way as many steps as the frame holds, which is as long in every colour
    const int stroke_count = zone->stroke ? zone->stroke_count : 0;
    status = DT_DUST_NO_MEMORY;
    stroke_points = dt_alloc_align_float((size_t)2 * MAX(1, stroke_count));
    if(IS_NULL_PTR(stroke_points)) goto cleanup;
    for(int k = 0; k < stroke_count; k++)
    {
      stroke_points[2 * k] = zone->stroke[2 * k] - w.x0;
      stroke_points[2 * k + 1] = zone->stroke[2 * k + 1] - w.y0;
    }
    const int extend = w.width + w.height;
    const int room = (_stroke_samples(stroke_points, stroke_count) + 2 * extend) / line_step + 2;
    path = dt_alloc_align_float((size_t)2 * (2 * room + 1));
    colour_path = dt_alloc_align_float((size_t)2 * (2 * room + 1));
    follow_score = dt_alloc_align_float((size_t)4 * (2 * room + 1));
    follow_back = dt_alloc_align((size_t)room * 2 * (2 * room + 1));
    if(IS_NULL_PTR(path) || IS_NULL_PTR(colour_path) || IS_NULL_PTR(follow_score) || IS_NULL_PTR(follow_back))
      goto cleanup;
    for(int k = 0; k < DUST_LINE_COLOURS; k++)
    {
      _dust_best_t in_colour = { .polarity = 1 };
      float colour_angle = axis;
      _colour_across(&w, background, inside, _dust_line_colours[k], sum, count, across, line_radius);
      for(int a = -DUST_LINE_ANGLES; a <= DUST_LINE_ANGLES; a++)
      {
        const float theta = axis + a * DUST_LINE_ANGLE_STEP;
        if(_smooth_line(across, inside, line_dark, line_light, NULL, fits, offsets, line_response, line_valid,
                        w.width, w.height, line_half, line_side, theta)
           < DUST_MIN_BLOCKS * (2 * line_half + 1) * (2 * line_radius + 1))
          continue;
        const float before = in_colour.contrast;
        _consider(line_dark, line_light, fits, n, w.width, scratch, line_field, &in_colour);
        if(in_colour.contrast > before) colour_angle = theta;
      }
      if(in_colour.contrast <= 0.0f) continue;
      const int long_fits
          = _smooth_line(across, inside, line_dark, line_light, line_mean, fits, offsets, line_response,
                         line_valid, w.width, w.height, line_half_long, line_side, colour_angle);
      if(long_fits < DUST_MIN_BLOCKS * (2 * line_half_long + 1) * (2 * line_radius + 1))
        _smooth_line(across, inside, line_dark, line_light, line_mean, fits, offsets, line_response, line_valid,
                     w.width, w.height, line_half, line_side, colour_angle);
      _measure_line(in_colour.polarity > 0 ? line_dark : line_light, line_mean, fits, n, w.width,
                    in_colour.polarity, colour_angle, band, scratch, line_field, &in_colour);
      if(in_colour.contrast < DUST_MIN_LINE_CONTRAST) continue;

      // the noise of a short segment, which the scratch is followed along
      _dust_best_t short_line = { 0 };
      _smooth_line(across, inside, line_dark, line_light, line_mean, fits, offsets, line_response, line_valid,
                   w.width, w.height, line_half, line_side, colour_angle);
      _measure_line(in_colour.polarity > 0 ? line_dark : line_light, line_mean, fits, n, w.width,
                    in_colour.polarity, colour_angle, band, scratch, line_field, &short_line);
      if(short_line.sigma <= 0.0f) continue;
      if(!_frame_new(stroke_points, stroke_count, (float)in_colour.seed_x, (float)in_colour.seed_y, colour_angle,
                     _scaled(DUST_STROKE_SMOOTH, image->scale, 1), extend, &frame))
        goto cleanup;
      float reach = -1.0f, score = -FLT_MAX;
      const int points = _follow_line(across, inside, w.width, w.height, &frame, (float)in_colour.seed_x,
                                      (float)in_colour.seed_y, in_colour.polarity, line_half, line_side,
                                      line_step, short_line.sigma, room, follow_score, follow_back, colour_path,
                                      &reach, &score);
      _frame_cleanup(&frame);
      if(points == 0 || score <= line_score) continue;
      line = in_colour;
      line_points = points;
      line_score = score;
      line_reach = reach;
      line_colour = k;
      float *const swap = path;
      path = colour_path;
      colour_path = swap;
    }
    // the scratch: between its sides across its path, DUST_LINE_BAND pixels each side of it at the least
    if(line_points > 0)
    {
      _colour_across(&w, background, inside, _dust_line_colours[line_colour], sum, count, across, 0);
      float shift = 0.0f;
      _line_sides(across, inside, w.width, w.height, path, line_points, line.polarity, 2.0f * line_band,
                  (float)_scaled(DUST_LINE_REACH, image->scale, 1), &shift, &line_half_band);
      _shift_path(path, line_points, shift, colour_path);
      _band(colour_path, line_points, line_half_band, inside, allowed, line_region, w.width, w.height);
    }
  }

  // the speck grown from its peak; along a stroke, of the speck and the scratch, the one that stands
  // out farther along it is what the stroke was painted over
  status = DT_DUST_NOT_FOUND;
  const gboolean has_speck = found.contrast >= DUST_MIN_CONTRAST;
  if(!has_speck && line_points == 0) goto cleanup;
  float speck_reach = -1.0f;
  if(has_speck)
  {
    _grow(best, &found, inside, allowed, region, stack, w.width, w.height);
    speck_reach = _reach(region, w.width, w.height, axis);
  }
  const gboolean take_line = line_reach > speck_reach;

  status = _speck_from_region(take_line ? line_region : region, &w, speck);
  const _dust_best_t *taken = take_line ? &line : &found;
  speck->polarity = taken->polarity;
  speck->peak = taken->peak;
  speck->contrast = taken->contrast;
  // a scratch bends, which its long axis does not: its half width is its band's, a pixel wider, plus one
  if(take_line && status == DT_DUST_OK) speck->half_width = fminf(speck->half_width, line_half_band + 2.0f);

cleanup:
  _window_cleanup(&w);
  dt_free_align(inside);
  dt_free_align(fits);
  dt_free_align(allowed);
  dt_free_align(region);
  dt_free_align(stack);
  dt_free_align(background);
  dt_free_align(smoothed);
  dt_free_align(across);
  dt_free_align(offsets);
  dt_free_align(best);
  dt_free_align(line_field);
  dt_free_align(line_dark);
  dt_free_align(line_light);
  dt_free_align(line_mean);
  _frame_cleanup(&frame);
  dt_free_align(stroke_points);
  dt_free_align(path);
  dt_free_align(colour_path);
  dt_free_align(follow_score);
  dt_free_align(follow_back);
  dt_free_align(line_response);
  dt_free_align(line_valid);
  dt_free_align(line_region);
  dt_free_align(scratch);
  dt_free_align(sum);
  dt_free_align(count);
  if(status != DT_DUST_OK) dt_dust_speck_cleanup(speck);
  return status;
}

/** The pixels a source candidate is judged on, as offsets in the search window. */
typedef struct _dust_pixels_t
{
  int *at;
  int count;
} _dust_pixels_t;

/** What every source candidate is judged against. */
typedef struct _dust_source_search_t
{
  const _dust_window_t *w;
  const uint8_t *mask; // the speck, in the window
  const _dust_pixels_t *on_speck;
  const _dust_pixels_t *on_ring;
  int rmin;
  int rmax;
  int box[4];        // x0, y0, x1, y1 of the speck and its ring, in the window
  float dark_limit;  // a top-hat above this under the moved speck is another speck, a dark one...
  float light_limit; // ... and a black top-hat above this a light one
} _dust_source_search_t;

/** Zero-mean squared difference of the three densities between the ring and the ring moved by
 * `shift` (an index offset in the window): a heal corrects the level, so only the texture counts. */
static float _ring_score(const _dust_window_t *w, const _dust_pixels_t *ring, const int shift)
{
  const size_t n = (size_t)w->width * w->height;
  float score = 0.0f;
  for(int c = 0; c < 3; c++)
  {
    const float *const plane = w->density + c * n;
    double sum = 0.0, sum2 = 0.0;
    for(int k = 0; k < ring->count; k++)
    {
      const float d = plane[ring->at[k]] - plane[ring->at[k] + shift];
      sum += d;
      sum2 += d * d;
    }
    const double mean = sum / ring->count;
    score += (float)(sum2 / ring->count - mean * mean);
  }
  return score;
}

/** The score of the source at (dx, dy) from the speck, or FLT_MAX where no source may be: off the
 * annulus, its ring out of the window, the speck itself under it, which a shift along a scratch
 * leaves, or another speck. */
static float _source_score(const _dust_source_search_t *s, const int dx, const int dy)
{
  const int r2 = dx * dx + dy * dy;
  const gboolean in_annulus = r2 >= s->rmin * s->rmin && r2 <= s->rmax * s->rmax;
  const gboolean in_window = s->box[0] + dx >= 0 && s->box[1] + dy >= 0 && s->box[2] + dx < s->w->width
                             && s->box[3] + dy < s->w->height;
  if(!in_annulus || !in_window) return FLT_MAX;
  const int shift = dy * s->w->width + dx;
  for(int k = 0; k < s->on_speck->count; k++)
  {
    const int at = s->on_speck->at[k] + shift;
    if(s->mask[at] || s->w->excess[at] > s->dark_limit || s->w->light[at] > s->light_limit) return FLT_MAX;
  }
  return _ring_score(s->w, s->on_ring, shift);
}

dt_dust_status_t dt_dust_find_source(const dt_dust_image_t *image, const dt_dust_speck_t *speck, float offset[2])
{
  const float s = image->scale;
  const int ring_width = _scaled(DUST_RING, s, 1);
  // two to six times the speck's width away: its radius for a round one, which clears it, and for a
  // scratch half its width, the shifts that would leave it on itself refused (_source_score())
  const float reach = speck->half_width;
  const int rmin = (int)ceilf(2.0f * reach + 2.0f * s);
  const int rmax = (int)floorf(6.0f * reach + 10.0f * s);
  const int margin = rmax + ring_width + 1;
  // the annulus grows with the speck: a wide one is searched on a grid of a sixth of its width, then
  // around the best node of it pixel by pixel
  const int stride = MAX(1, (int)(reach / 6.0f));

  // the search window: the speck's box, grown by the farthest a ring pixel may be moved
  _dust_window_t w = { .x0 = MAX(0, speck->box_x - margin), .y0 = MAX(0, speck->box_y - margin) };
  w.width = MIN(image->width, speck->box_x + speck->box_width + margin) - w.x0;
  w.height = MIN(image->height, speck->box_y + speck->box_height + margin) - w.y0;
  const size_t n = (size_t)w.width * w.height;
  const int steps = rmax / stride;
  const int side = 2 * steps + 1;

  uint8_t *mask = dt_calloc_align(n);
  uint8_t *ring = dt_alloc_align(n);
  _dust_pixels_t on_speck = { .at = dt_alloc_align(n * sizeof(int)) };
  _dust_pixels_t on_ring = { .at = dt_alloc_align(n * sizeof(int)) };
  float *scores = dt_alloc_align_float((size_t)side * side);
  float *scratch = dt_alloc_align_float(n);
  dt_dust_status_t status = DT_DUST_NO_MEMORY;
  if(IS_NULL_PTR(mask) || IS_NULL_PTR(ring) || IS_NULL_PTR(on_speck.at) || IS_NULL_PTR(on_ring.at)
     || IS_NULL_PTR(scores) || IS_NULL_PTR(scratch))
    goto cleanup;
  status = _window_top_hats(image, &w, _scaled(DUST_OPENING_RADIUS, s, 1));
  if(status != DT_DUST_OK) goto cleanup;

  // the speck and its ring, as indices into the window, and the box they span
  for(int y = 0; y < speck->box_height; y++)
    for(int x = 0; x < speck->box_width; x++)
      mask[(size_t)(speck->box_y - w.y0 + y) * w.width + speck->box_x - w.x0 + x]
          = speck->mask[y * speck->box_width + x];
  _dilate(mask, ring, w.width, w.height, ring_width);
  _dust_source_search_t search = { .w = &w, .mask = mask, .on_speck = &on_speck, .on_ring = &on_ring,
                                   .rmin = rmin, .rmax = rmax, .box = { w.width, w.height, -1, -1 } };
  // the speck's own top-hat: the black one for a light speck
  const float *const own = speck->polarity < 0 ? w.light : w.excess;
  float speck_top_hat = -FLT_MAX;
  for(int i = 0; i < (int)n; i++)
  {
    if(!ring[i]) continue;
    if(mask[i])
    {
      on_speck.at[on_speck.count++] = i;
      speck_top_hat = fmaxf(speck_top_hat, own[i]);
    }
    else
      on_ring.at[on_ring.count++] = i;
    search.box[0] = MIN(search.box[0], i % w.width);
    search.box[2] = MAX(search.box[2], i % w.width);
    search.box[1] = MIN(search.box[1], i / w.width);
    search.box[3] = MAX(search.box[3], i / w.width);
  }

  // another speck is a top-hat, dark or light, that stands out of the grain, or, of the speck's own
  // polarity, half as far out as the speck does
  float dark_median = 0.0f, light_median = 0.0f;
  const float dark_sigma = _noise(w.excess, n, scratch, &dark_median);
  const float light_sigma = _noise(w.light, n, scratch, &light_median);
  search.dark_limit = dark_median + DUST_OTHER_SPECK * dark_sigma;
  search.light_limit = light_median + DUST_OTHER_SPECK * light_sigma;
  if(speck->polarity < 0)
    search.light_limit = fmaxf(search.light_limit, light_median + 0.5f * (speck_top_hat - light_median));
  else
    search.dark_limit = fmaxf(search.dark_limit, dark_median + 0.5f * (speck_top_hat - dark_median));

  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < side; j++)
    for(int i = 0; i < side; i++)
      scores[(size_t)j * side + i] = _source_score(&search, (i - steps) * stride, (j - steps) * stride);

  float best = FLT_MAX;
  int best_x = 0, best_y = 0;
  for(int k = 0; k < side * side; k++)
  {
    if(scores[k] >= best) continue;
    best = scores[k];
    best_x = (k % side - steps) * stride;
    best_y = (k / side - steps) * stride;
  }
  // the pixels around the best node of the grid
  const int node_x = best_x, node_y = best_y;
  for(int dy = 1 - stride; dy < stride && best < FLT_MAX; dy++)
    for(int dx = 1 - stride; dx < stride; dx++)
    {
      const float score = _source_score(&search, node_x + dx, node_y + dy);
      if(score >= best) continue;
      best = score;
      best_x = node_x + dx;
      best_y = node_y + dy;
    }
  offset[0] = (float)best_x;
  offset[1] = (float)best_y;
  status = best < FLT_MAX ? DT_DUST_OK : DT_DUST_NO_SOURCE;

cleanup:
  _window_cleanup(&w);
  dt_free_align(mask);
  dt_free_align(ring);
  dt_free_align(on_speck.at);
  dt_free_align(on_ring.at);
  dt_free_align(scores);
  dt_free_align(scratch);
  return status;
}

/** Douglas-Peucker on the open run of vertices first..last, marking in `keep` the ones that stray
 * more than `tolerance` from the chord. */
static void _simplify(const int *v, uint8_t *keep, const int first, const int last, const float tolerance)
{
  if(last - first < 2) return;
  const float ax = v[2 * first], ay = v[2 * first + 1];
  const float bx = v[2 * last], by = v[2 * last + 1];
  const float length = fmaxf(hypotf(bx - ax, by - ay), 1e-6f);
  int farthest = -1;
  float distance = tolerance;
  for(int k = first + 1; k < last; k++)
  {
    const float d = fabsf((bx - ax) * (ay - v[2 * k + 1]) - (ax - v[2 * k]) * (by - ay)) / length;
    if(d <= distance) continue;
    distance = d;
    farthest = k;
  }
  if(farthest < 0) return;
  keep[farthest] = 1;
  _simplify(v, keep, first, farthest, tolerance);
  _simplify(v, keep, farthest, last, tolerance);
}

static gboolean _on_speck(const dt_dust_speck_t *speck, const int x, const int y)
{
  return x >= 0 && y >= 0 && x < speck->box_width && y < speck->box_height
         && speck->mask[y * speck->box_width + x];
}

dt_dust_status_t dt_dust_outline(const dt_dust_speck_t *speck, const float tolerance, float **points, int *count)
{
  // The boundary is followed on the pixel corners, keeping the speck on the right. Ahead of a corner
  // lie two pixels, left and right of the edge going straight on: a speck pixel on the left turns
  // the walk left (that is what joins diagonal neighbours), no speck pixel on the right turns it
  // right, anything else goes straight. Directions: east, south, west, north, y going down.
  static const int step[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
  const int max_corners = 2 * (speck->box_width + 1) * (speck->box_height + 1);
  int *corners = dt_alloc_align((size_t)2 * (max_corners + 1) * sizeof(int));
  uint8_t *keep = dt_calloc_align(max_corners);
  if(IS_NULL_PTR(corners) || IS_NULL_PTR(keep))
  {
    dt_free_align(corners);
    dt_free_align(keep);
    return DT_DUST_NO_MEMORY;
  }

  // start on the top-left corner of the first speck pixel in raster order, heading east
  int x0 = 0, y0 = 0;
  while(!speck->mask[y0 * speck->box_width + x0])
  {
    x0 = (x0 + 1) % speck->box_width;
    y0 += x0 == 0;
  }
  int x = x0, y = y0, d = 0, corner_count = 0;
  corners[2 * corner_count] = x0;
  corners[2 * corner_count++ + 1] = y0;
  do
  {
    x += step[d][0];
    y += step[d][1];
    const int dx = step[d][0], dy = step[d][1];
    const gboolean left = _on_speck(speck, x + (dx + dy - 1) / 2, y + (dy - dx - 1) / 2);
    const gboolean right = _on_speck(speck, x + (dx - dy - 1) / 2, y + (dx + dy - 1) / 2);
    const int turn = left ? 3 : right ? 0 : 1;
    if(turn && !(x == x0 && y == y0) && corner_count < max_corners)
    {
      corners[2 * corner_count] = x;
      corners[2 * corner_count++ + 1] = y;
    }
    d = (d + turn) % 4;
  } while(!(x == x0 && y == y0));

  // simplify the closed polygon as two open runs, between the start and the corner farthest from it
  // (not named `far`: Windows headers define that away)
  int opposite = 0;
  for(int k = 1; k < corner_count; k++)
    if(hypotf(corners[2 * k] - x0, corners[2 * k + 1] - y0)
       > hypotf(corners[2 * opposite] - x0, corners[2 * opposite + 1] - y0))
      opposite = k;
  corners[2 * corner_count] = x0;
  corners[2 * corner_count + 1] = y0;
  keep[0] = keep[opposite] = 1;
  // no farther than a third of the speck's half width either, which a scratch is cut by otherwise:
  // measured on a scratch 8 px wide, a third kept 95 % of its mask with 14 nodes, a half 87 % with 7
  const float stray
      = fmaxf(DUST_OUTLINE_MIN_TOLERANCE, fminf(tolerance, speck->half_width / DUST_OUTLINE_WIDTH_SHARE));
  _simplify(corners, keep, 0, opposite, stray);
  _simplify(corners, keep, opposite, corner_count, stray);
  int kept = 0;
  for(int k = 0; k < corner_count; k++) kept += keep[k];
  // a speck of a few pixels simplifies to a segment: keep its corners then
  if(kept < 3) memset(keep, 1, corner_count);
  kept = kept < 3 ? corner_count : kept;

  *points = dt_alloc_align_float((size_t)2 * kept);
  if(IS_NULL_PTR(*points))
  {
    dt_free_align(corners);
    dt_free_align(keep);
    return DT_DUST_NO_MEMORY;
  }
  *count = 0;
  for(int k = 0; k < corner_count; k++)
  {
    if(!keep[k]) continue;
    (*points)[2 * *count] = (float)(speck->box_x + corners[2 * k]);
    (*points)[2 * (*count)++ + 1] = (float)(speck->box_y + corners[2 * k + 1]);
  }
  dt_free_align(corners);
  dt_free_align(keep);
  return DT_DUST_OK;
}

void dt_dust_speck_cleanup(dt_dust_speck_t *speck)
{
  dt_free_align(speck->mask);
  memset(speck, 0, sizeof(*speck));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
