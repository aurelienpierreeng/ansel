/*
    This file is part of Ansel,
    Copyright (C) 2026 - Aurélien PIERRE.

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

#include "canvas/canvas_dilate.h"

#include "system/macros.h" // IS_NULL_PTR

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* A lattice step: one sample of a line along it to the next. */
typedef struct dt_canvas_dilate_step_t
{
  int vx;
  int vy;
} dt_canvas_dilate_step_t;

/* The axes, the diagonals and the knight's moves, in the order `_family_length()` reads them. */
static const dt_canvas_dilate_step_t _steps[8] = {
  { 1, 0 }, { 0, 1 }, { 1, 1 }, { -1, 1 }, { 2, 1 }, { -2, 1 }, { 1, 2 }, { -1, 2 },
};

#define DILATE_SAMPLES 65               ///< directions over [0, pi/4]; the steps' symmetry covers the rest
#define DILATE_EXHAUSTIVE_RADIUS 24.0f  ///< below it every small combination is tried
#define DILATE_LOCAL_WINDOW 6           ///< above it, this many either side of the proportional guess
#define DILATE_DIAGONAL_SHARE 0.300f    ///< the continuous optimum, per axis sample (see the header)
#define DILATE_KNIGHT_SHARE 0.440f
#define DILATE_RADIUS_MAX 1.0e6f        ///< keeps the lengths inside an int, far past any plane

float dt_canvas_dilate_reach(const dt_canvas_dilate_lengths_t lengths, const float angle)
{
  const float c = cosf(angle);
  const float s = sinf(angle);
  const float axis = fabsf(c) + fabsf(s);
  const float diagonal = fabsf(c + s) + fabsf(c - s);
  const float knight = fabsf(2.0f * c + s) + fabsf(2.0f * c - s) + fabsf(c + 2.0f * s) + fabsf(c - 2.0f * s);
  return (float)lengths.axis * axis + (float)lengths.diagonal * diagonal + (float)lengths.knight * knight;
}

/* The reach of one sample of each family, at every direction the fit is judged at. */
static void _family_reaches(float axis[DILATE_SAMPLES], float diagonal[DILATE_SAMPLES], float knight[DILATE_SAMPLES])
{
  const dt_canvas_dilate_lengths_t one_axis = { 1, 0, 0 };
  const dt_canvas_dilate_lengths_t one_diagonal = { 0, 1, 0 };
  const dt_canvas_dilate_lengths_t one_knight = { 0, 0, 1 };
  for(int sample = 0; sample < DILATE_SAMPLES; sample++)
  {
    const float angle = (float)M_PI_4 * (float)sample / (float)(DILATE_SAMPLES - 1);
    axis[sample] = dt_canvas_dilate_reach(one_axis, angle);
    diagonal[sample] = dt_canvas_dilate_reach(one_diagonal, angle);
    knight[sample] = dt_canvas_dilate_reach(one_knight, angle);
  }
}

dt_canvas_dilate_lengths_t dt_canvas_dilate_lengths(const float radius)
{
  dt_canvas_dilate_lengths_t best = { 0, 0, 0 };
  if(!(radius >= 0.5f)) return best;
  const float target = fminf(radius, DILATE_RADIUS_MAX);
  float axis[DILATE_SAMPLES];
  float diagonal[DILATE_SAMPLES];
  float knight[DILATE_SAMPLES];
  _family_reaches(axis, diagonal, knight);

  int axis_low = 0;
  int axis_high = 0;
  int diagonal_low = 0;
  int diagonal_high = 0;
  int knight_low = 0;
  int knight_high = 0;
  if(target <= DILATE_EXHAUSTIVE_RADIUS)
  {
    // Small enough to try everything plausible: at a few pixels the best sum is not the
    // proportional one (a radius of 9 is best served by three axis and three diagonal samples).
    axis_high = (int)ceilf(target / 2.0f) + 1;
    diagonal_high = axis_high;
    knight_high = (int)ceilf(target / 4.0f) + 1;
  }
  else
  {
    // The continuous optimum's proportions, scaled to the radius, and a window around them. MEASURED
    // against trying every combination up to 160 px: the same answer to 0.03 px at worst.
    float lowest = INFINITY;
    float highest = 0.0f;
    for(int sample = 0; sample < DILATE_SAMPLES; sample++)
    {
      const float reach = axis[sample] + DILATE_DIAGONAL_SHARE * diagonal[sample] + DILATE_KNIGHT_SHARE * knight[sample];
      lowest = fminf(lowest, reach);
      highest = fmaxf(highest, reach);
    }
    const float per_axis = target / (0.5f * (lowest + highest));
    const int axis_guess = (int)lroundf(per_axis);
    const int diagonal_guess = (int)lroundf(per_axis * DILATE_DIAGONAL_SHARE);
    const int knight_guess = (int)lroundf(per_axis * DILATE_KNIGHT_SHARE);
    axis_low = MAX(axis_guess - DILATE_LOCAL_WINDOW, 0);
    axis_high = axis_guess + DILATE_LOCAL_WINDOW;
    diagonal_low = MAX(diagonal_guess - DILATE_LOCAL_WINDOW, 0);
    diagonal_high = diagonal_guess + DILATE_LOCAL_WINDOW;
    knight_low = MAX(knight_guess - DILATE_LOCAL_WINDOW, 0);
    knight_high = knight_guess + DILATE_LOCAL_WINDOW;
  }

  float best_error = INFINITY;
  for(int a = axis_low; a <= axis_high; a++)
    for(int b = diagonal_low; b <= diagonal_high; b++)
      for(int c = knight_low; c <= knight_high; c++)
      {
        if(a == 0 && b == 0 && c == 0) continue;
        float error = 0.0f;
        for(int sample = 0; sample < DILATE_SAMPLES && error < best_error; sample++)
        {
          const float reach = (float)a * axis[sample] + (float)b * diagonal[sample] + (float)c * knight[sample];
          error = fmaxf(error, fabsf(reach - target));
        }
        // Strictly better only, so the first of equals wins and the choice is the same every time.
        if(error < best_error)
        {
          best_error = error;
          best.axis = a;
          best.diagonal = b;
          best.knight = c;
        }
      }
  return best;
}

int dt_canvas_dilate_margin(const float radius)
{
  const dt_canvas_dilate_lengths_t lengths = dt_canvas_dilate_lengths(radius);
  // Along x, every step's horizontal part: 1 + 0 for the axes, 1 + 1 for the diagonals and
  // 2 + 2 + 1 + 1 for the knight's moves -- and the same along y, the steps being symmetric.
  return lengths.axis + 2 * lengths.diagonal + 6 * lengths.knight;
}

static int _family_length(const dt_canvas_dilate_lengths_t *lengths, const int step)
{
  if(step < 2) return lengths->axis;
  if(step < 4) return lengths->diagonal;
  return lengths->knight;
}

/*
 * A running maximum along the rows, `half` samples either side. van Herk / Gil-Werman: the row is
 * cut into blocks of `2 half + 1` samples, a maximum is run forward and one backward inside each
 * block, and any window of that length is the end of one block and the start of the next -- two
 * reads, whatever `half` is. A window that would leave the row is not computed: that pixel keeps
 * its own value, which is the margin's bargain.
 */
static void _dilate_rows(float *plane, float *prefix, float *suffix, const int width, const int height, const int half)
{
  const int span = 2 * half + 1;
  if(2 * half >= width) return;
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = 0; row < height; row++)
  {
    float *line = plane + (size_t)row * width;
    float *ahead = prefix + (size_t)row * width;
    float *behind = suffix + (size_t)row * width;
    for(int start = 0; start < width; start += span)
    {
      const int end = MIN(start + span, width);
      ahead[start] = line[start];
      for(int col = start + 1; col < end; col++) ahead[col] = fmaxf(ahead[col - 1], line[col]);
      behind[end - 1] = line[end - 1];
      for(int col = end - 2; col >= start; col--) behind[col] = fmaxf(behind[col + 1], line[col]);
    }
    for(int col = half; col < width - half; col++) line[col] = fmaxf(behind[col - half], ahead[col + half]);
  }
}

/* One row of a running maximum along a line: `target[col] = max(previous[col - vx], source[col])`,
 * restarting where the line enters the plane through a side (no previous sample to take). */
static inline void _run_row(float *target, const float *previous, const float *source, const int width, const int vx)
{
  if(IS_NULL_PTR(previous))
  {
    for(int col = 0; col < width; col++) target[col] = source[col];
    return;
  }
  // The columns whose previous sample lies inside the plane, and the one or two at a side that restart.
  const int inside_first = MIN(MAX(vx, 0), width);
  const int inside_last = MAX(width + MIN(vx, 0), inside_first);
  for(int col = 0; col < inside_first; col++) target[col] = source[col];
  for(int col = inside_first; col < inside_last; col++) target[col] = fmaxf(previous[col - vx], source[col]);
  for(int col = inside_last; col < width; col++) target[col] = source[col];
}

/*
 * The same running maximum along a step that climbs rows (vy of 1 or 2). A line's sample index is
 * its row divided by vy, so a block of `span` samples is `span * vy` WHOLE ROWS, the first vy of
 * them starting it and the last vy ending it, for every line at once. Blocks share nothing, so
 * they run in parallel, and each walks full rows in memory order: handing the threads slanted
 * bands of whole lines instead, which the recurrence also allows, read each row a few cache lines
 * at a time and measured slower.
 */
static void _dilate_lines(float *plane, float *prefix, float *suffix, const int width, const int height, const int vx,
                          const int vy, const int half)
{
  const int span = 2 * half + 1;
  const int margin_x = half * abs(vx);
  const int margin_y = half * vy;
  if(2 * margin_x >= width || 2 * margin_y >= height) return;
  const int block_rows = span * vy;
  const int blocks = (height + block_rows - 1) / block_rows;
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int block = 0; block < blocks; block++)
  {
    const int top = block * block_rows;
    const int bottom = MIN(top + block_rows, height);
    for(int row = top; row < bottom; row++)
    {
      const gboolean starts = row < top + vy;
      _run_row(prefix + (size_t)row * width, starts ? NULL : prefix + (size_t)(row - vy) * width,
               plane + (size_t)row * width, width, vx);
    }
    for(int row = bottom - 1; row >= top; row--)
    {
      const gboolean ends = row + vy >= bottom;
      _run_row(suffix + (size_t)row * width, ends ? NULL : suffix + (size_t)(row + vy) * width,
               plane + (size_t)row * width, width, -vx);
    }
  }
  // Both ends of each window, read from the two runs, wherever the window lies inside the plane.
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static)
#endif
  for(int row = margin_y; row < height - margin_y; row++)
  {
    float *target = plane + (size_t)row * width;
    const float *behind = suffix + (size_t)(row - margin_y) * width;
    const float *ahead = prefix + (size_t)(row + margin_y) * width;
    for(int col = margin_x; col < width - margin_x; col++)
      target[col] = fmaxf(behind[col - half * vx], ahead[col + half * vx]);
  }
}

gboolean dt_canvas_dilate(float *plane, float *prefix, float *suffix, const int width, const int height,
                          const float radius)
{
  if(IS_NULL_PTR(plane) || IS_NULL_PTR(prefix) || IS_NULL_PTR(suffix) || width <= 0 || height <= 0) return FALSE;
  const dt_canvas_dilate_lengths_t lengths = dt_canvas_dilate_lengths(radius);
  for(int step = 0; step < 8; step++)
  {
    const int half = _family_length(&lengths, step);
    if(half <= 0) continue;
    if(_steps[step].vy == 0)
      _dilate_rows(plane, prefix, suffix, width, height, half);
    else
      _dilate_lines(plane, prefix, suffix, width, height, _steps[step].vx, _steps[step].vy, half);
  }
  return TRUE;
}
