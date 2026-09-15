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

#ifndef DT_MATH_POLYGON_ENVELOPE_H
#define DT_MATH_POLYGON_ENVELOPE_H

/**
 * @file polygon_envelope.h
 * @brief Regular polygons, stars and their rounded forms, as one polar envelope.
 *
 * @details The lens blur (`iop/blurs.c`) draws its diaphragm from a closed-form polar curve
 * (https://math.stackexchange.com/a/4160104/498090) in three numbers: `n` blades, a
 * concavity `m` and a linearity `k`,
 *
 *     M(theta) = cos((2 asin k + pi m) / 2n) / cos((2 asin(k cos(n theta)) + pi m) / 2n)
 *
 * which is a regular n-gon at `m = 1, k = 1`, a star for larger `m`, a circle at `k = 0`,
 * and smooth lobes in between. This header is that curve, written once for everything that
 * draws such a shape -- the canvas and the toolbar glyphs -- and kept apart from the blur,
 * which it does not touch.
 *
 * Three things about the curve decide how it is exposed here.
 *
 * **The concavity is valid only on a domain that depends on the other two numbers.** The
 * curve holds only while `2 asin k + pi m < n pi`: at `n = 3, m = 2, k = 1` the numerator is
 * `cos(pi / 2)` and the whole shape collapses to its centre, and past it the radius goes
 * negative. A stored `m` would therefore turn invalid the moment the number of sides was
 * lowered under it. The shapes are described instead by a DEPTH, the fraction of the way the
 * notch between two tips is pushed from the straight edge towards the centre: the inner
 * radius is `(1 - depth) cos(pi / n)`, and `m` is derived from it for the sides in force.
 * Depth 0 is `m = 1`, the convex polygon. Any depth below 1 keeps the inner radius above
 * zero, which keeps `m` below `n - 1` for every `n`; the header clamps it at
 * `DT_POLYGON_MAX_DEPTH`, so no combination of its arguments can leave the valid domain.
 * Inside that domain the denominator's argument lies between 0 and the numerator's, both
 * under `pi / 2`, so the radius stays in (0, 1] at every angle. At the other end a depth
 * under `DT_POLYGON_MIN_DEPTH` is no depth: a notch pushed in by less than a millionth of
 * the tip radius still lies on the straight edge to rounding, and below about 1e-16
 * `1 - depth` IS 1, so the two edges meeting at the notch are exactly opposite and anything
 * taking their bisector divides nought by nought (measured: a bisector of exactly zero at 5,
 * 6, 7, 8 and 10 sides for a depth of 1e-20, and one 6e-5 rad off at 1e-12). Such a shape is
 * drawn as the convex polygon it is indistinguishable from.
 *
 * **At `k = 1` the curve is straight lines, and is drawn as its vertices.** There
 * `asin(cos x) = pi / 2 - |x|` turns the envelope into `cos(alpha) / cos(alpha - |phi|)`,
 * `phi` being the angle from the nearest tip and `alpha = pi (m + 1) / 2n`: the polar
 * equation of a straight line. Sampling it would only lay `DT_POLYGON_SAMPLES_PER_HALF_BLADE`
 * points along each tip-to-notch segment (twice as many along a convex polygon's side) that
 * the segment's two ends already describe, so the straight outline is the exact tips and
 * notches, and a caller filleting its corners finds them there. Such a caller should not cap
 * a tip's fillet at half of the segments either side of it: a star's segment is only half of
 * the convex polygon's side, so that cap would halve the largest radius a tip can take the
 * moment the depth leaves 0, while the shape itself has barely moved.
 *
 * **A rounded outline is sampled, and not evenly.** It is smooth everywhere, and
 * `DT_POLYGON_SAMPLES_PER_HALF_BLADE` samples from each tip to the next notch put one on every
 * tip and every notch, so it meets the straight form where the two agree. But close to
 * straight the curve turns nearly its whole corner within a sliver of angle either side of a
 * tip -- `asin` is square-root singular at 1 -- and even spacing left almost all of the
 * polyline's error in the one segment either side of each tip, while the notches, which lie
 * nearer the centre, were sampled far finer than they needed: measured, up to 1.36e-2 of the
 * tip radius (3 sides, depth 0.95, roundness 0.04), five units of curve missing at the tip of a
 * shape 600 units tall. The samples therefore crowd towards the tips, at
 * `1 - k (DT_POLYGON_TIP_CROWDING cos(2 pi t) + DT_POLYGON_CORNER_CROWDING cos(4 pi t))` of the
 * even density, `t` being the fraction of the blade, which brings the worst case over 3 to 12
 * sides, every depth and roundness 1e-4 to 1 down to 1.75e-3. Both coefficients were chosen by
 * that measurement. The crowding scales with `k`, so a circle, which has no corner to crowd
 * towards, keeps even samples and an exactly square box, and the samples move continuously
 * with the roundness.
 *
 * Coordinates are y down, the angle `theta` runs clockwise from straight up, and every
 * outline starts at the top tip and turns clockwise on screen, whatever its parameters, so a
 * nonzero-winding fill never depends on them. The unit shape's tips sit on the unit circle;
 * fitting it to a frame is the caller's business. `blurs.c` swaps its row and column axes;
 * that is not copied.
 */

#include <math.h>

/** Fewer than three sides encloses nothing. */
#define DT_POLYGON_MIN_SIDES 3
/** The most sides an outline is built for, which is what bounds its point count. */
#define DT_POLYGON_MAX_SIDES 12
/** The deepest notch: keeps the inner radius, hence the shape, away from its centre. */
#define DT_POLYGON_MAX_DEPTH 0.95
/** The shallowest notch that is one; anything shallower is read as 0, the convex polygon. */
#define DT_POLYGON_MIN_DEPTH 1e-6
/** Samples of a rounded outline from one tip to the next notch. */
#define DT_POLYGON_SAMPLES_PER_HALF_BLADE 16
/** How far a rounded outline's samples crowd towards its tips (and away from its notches). */
#define DT_POLYGON_TIP_CROWDING 0.6
/** How far they crowd towards its tips and its notches alike. */
#define DT_POLYGON_CORNER_CROWDING 0.3
/** Enough points for any outline this header builds, whatever its arguments. */
#define DT_POLYGON_OUTLINE_MAX_POINTS (2 * DT_POLYGON_MAX_SIDES * DT_POLYGON_SAMPLES_PER_HALF_BLADE)

/* M_PI is not ISO C: <math.h> declares it only under the POSIX or GNU feature macros, or
 * _USE_MATH_DEFINES for MSVC, and nothing this header includes promises to set them. */
#define DT_POLYGON_PI (3.14159265358979323846)

static inline int _dt_polygon_clamp_sides(const int sides)
{
  if(sides < DT_POLYGON_MIN_SIDES) return DT_POLYGON_MIN_SIDES;
  if(sides > DT_POLYGON_MAX_SIDES) return DT_POLYGON_MAX_SIDES;
  return sides;
}

/* fmax() and fmin() return the other argument for a NaN, so a NaN reads as the bound it was
 * compared against first and never reaches the curve. A depth too shallow to move the notch off
 * the edge reads as none, so every function below agrees that the shape is convex. */
static inline double _dt_polygon_clamp_depth(const double depth)
{
  const double bounded_depth = fmin(fmax(depth, 0.0), DT_POLYGON_MAX_DEPTH);
  return (bounded_depth < DT_POLYGON_MIN_DEPTH) ? 0.0 : bounded_depth;
}

static inline double _dt_polygon_clamp_roundness(const double roundness)
{
  return fmin(fmax(roundness, 0.0), 1.0);
}

/**
 * @brief The envelope's radius at one angle, `blurs.c`'s formula in double.
 *
 * @param sides Number of tips `n`, not clamped: the condition on @p concavity is the whole
 * contract, and with `m >= 1` it admits no `n` below 2.
 * @param concavity `m`, at least 1 and with `2 asin(linearity) + pi m < sides pi`; derive it
 * from a depth with dt_polygon_concavity_for_depth() rather than choosing it.
 * @param linearity `k` in [0, 1]: 1 draws straight sides, 0 a circle.
 * @param theta Angle, clockwise from straight up.
 * @return The radius, in (0, 1] on the valid domain, 1 at every tip.
 */
static inline double dt_polygon_envelope(const int sides, const double concavity, const double linearity,
                                         const double theta)
{
  const double blade_count = (double)sides;
  const double numerator = cos((2.0 * asin(linearity) + DT_POLYGON_PI * concavity) / (2.0 * blade_count));
  const double denominator
      = cos((2.0 * asin(linearity * cos(blade_count * theta)) + DT_POLYGON_PI * concavity) / (2.0 * blade_count));
  return numerator / denominator;
}

/**
 * @brief Radius of the notches of a straight-sided shape: `(1 - depth) cos(pi / n)`.
 *
 * @details Depth 0 puts the notch on the middle of the straight edge between two tips, which
 * is where a convex polygon's edge passes. Sides and depth are clamped to what an outline
 * accepts.
 */
static inline double dt_polygon_inner_radius(const int sides, const double depth)
{
  const int clamped_sides = _dt_polygon_clamp_sides(sides);
  const double clamped_depth = _dt_polygon_clamp_depth(depth);
  return (1.0 - clamped_depth) * cos(DT_POLYGON_PI / (double)clamped_sides);
}

/**
 * @brief The envelope's concavity `m` that puts a straight-sided shape's notches at
 * dt_polygon_inner_radius().
 *
 * @details The straight envelope reaches `cos(alpha) / cos(alpha - pi / n)` at a notch, so
 * `tan(alpha) = (1 - r cos(pi / n)) / (r sin(pi / n))` for the inner radius `r`, and
 * `m = 2 n alpha / pi - 1`. `r` is substituted and the quotient divided through by
 * `sin(pi / n)`, so neither argument of the arctangent is a difference of nearly equal
 * numbers. The result lies in [1, n - 1).
 *
 * Depth 0 returns 1 as such rather than through the arctangent, which gives it back only to
 * an ulp or so (measured: 1 + 4.4e-16 at n = 8). The lower bound is a promise the envelope
 * leans on -- `m >= 1` is what keeps its denominator's argument between 0 and the
 * numerator's -- so it holds exactly, not approximately.
 */
static inline double dt_polygon_concavity_for_depth(const int sides, const double depth)
{
  const int clamped_sides = _dt_polygon_clamp_sides(sides);
  const double clamped_depth = _dt_polygon_clamp_depth(depth);
  if(clamped_depth <= 0.0) return 1.0;

  const double half_blade = DT_POLYGON_PI / (double)clamped_sides;
  const double sine = sin(half_blade);
  const double cosine = cos(half_blade);
  const double tip_to_notch
      = atan2(sine + clamped_depth * cosine * cosine / sine, (1.0 - clamped_depth) * cosine);
  return 2.0 * tip_to_notch / half_blade - 1.0;
}

/* How many points the outline has: the tips alone for a convex straight polygon, tips and
 * notches for a straight star, and the samples of every half blade for a rounded shape. The
 * arguments are already clamped. */
static inline int _dt_polygon_point_count(const int sides, const double depth, const double linearity)
{
  if(linearity < 1.0) return 2 * sides * DT_POLYGON_SAMPLES_PER_HALF_BLADE;
  if(depth > 0.0) return 2 * sides;
  return sides;
}

/* The index-th point of the outline whose count _dt_polygon_point_count() gave. One function
 * serves the outline and its aspect, so the aspect is the box of exactly the points drawn. */
static inline void _dt_polygon_unit_point(const int sides, const double depth, const double linearity,
                                          const double concavity, const double inner_radius, const int index,
                                          double *const point_x, double *const point_y)
{
  double theta = 0.0;
  double radius = 1.0;
  if(linearity < 1.0)
  {
    /* The crowding terms vanish at the tip and the notch of every blade: their sines are 0 at the
     * tip, and within an ulp of 0 at the notch, too little to move the half they are taken from. */
    const int blade_samples = 2 * DT_POLYGON_SAMPLES_PER_HALF_BLADE;
    const double blade_fraction = (double)(index % blade_samples) / (double)blade_samples;
    const double turn = 2.0 * DT_POLYGON_PI * blade_fraction;
    const double tip_term = DT_POLYGON_TIP_CROWDING * sin(turn);
    const double corner_term = 0.5 * DT_POLYGON_CORNER_CROWDING * sin(2.0 * turn);
    const double crowding = linearity * (tip_term + corner_term) / (2.0 * DT_POLYGON_PI);
    const double blades = (double)index / (double)blade_samples - crowding;
    theta = blades * 2.0 * DT_POLYGON_PI / (double)sides;
    radius = dt_polygon_envelope(sides, concavity, linearity, theta);
  }
  else if(depth > 0.0)
  {
    theta = (double)index * DT_POLYGON_PI / (double)sides;
    radius = (index % 2 == 0) ? 1.0 : inner_radius;
  }
  else
  {
    theta = 2.0 * (double)index * DT_POLYGON_PI / (double)sides;
  }
  *point_x = radius * sin(theta);
  *point_y = -radius * cos(theta);
}

/**
 * @brief The outline of a unit shape, tips on the unit circle, as interleaved x, y pairs.
 *
 * @param sides Tips, clamped to [DT_POLYGON_MIN_SIDES, DT_POLYGON_MAX_SIDES].
 * @param depth Notch depth, clamped to [0, DT_POLYGON_MAX_DEPTH]; 0 is a convex polygon.
 * @param roundness `1 - k`, clamped to [0, 1]; 0 is straight sides, 1 a circle.
 * @param points Receives `2 * count` doubles.
 * @param max_points How many points @p points can hold. DT_POLYGON_OUTLINE_MAX_POINTS
 * always suffices.
 * @return The number of points written: n for a straight convex polygon, 2n for a straight
 * star, `2 n DT_POLYGON_SAMPLES_PER_HALF_BLADE` for a rounded shape; 0, with nothing written,
 * when @p max_points cannot hold them. A truncated outline would be a different shape, so
 * none is returned.
 */
static inline int dt_polygon_unit_outline(const int sides, const double depth, const double roundness,
                                          double *const points, const int max_points)
{
  const int clamped_sides = _dt_polygon_clamp_sides(sides);
  const double clamped_depth = _dt_polygon_clamp_depth(depth);
  const double linearity = 1.0 - _dt_polygon_clamp_roundness(roundness);
  const int point_count = _dt_polygon_point_count(clamped_sides, clamped_depth, linearity);
  if(point_count > max_points) return 0;

  const double concavity = dt_polygon_concavity_for_depth(clamped_sides, clamped_depth);
  const double inner_radius = dt_polygon_inner_radius(clamped_sides, clamped_depth);
  for(int i = 0; i < point_count; i++)
  {
    _dt_polygon_unit_point(clamped_sides, clamped_depth, linearity, concavity, inner_radius, i,
                           &points[2 * i], &points[2 * i + 1]);
  }
  return point_count;
}

/**
 * @brief Width over height of the box around dt_polygon_unit_outline()'s points.
 *
 * @details A frame of that ratio holds the shape with every side touching it and nothing
 * stretched: a triangle is `2 / sqrt(3)` wide for its height, a hexagon `sqrt(3) / 2`. Taken
 * over the same points the outline returns, so fitting the outline to a frame of this ratio
 * scales both axes alike. The arguments are clamped as the outline clamps them.
 */
static inline double dt_polygon_unit_aspect(const int sides, const double depth, const double roundness)
{
  const int clamped_sides = _dt_polygon_clamp_sides(sides);
  const double clamped_depth = _dt_polygon_clamp_depth(depth);
  const double linearity = 1.0 - _dt_polygon_clamp_roundness(roundness);
  const int point_count = _dt_polygon_point_count(clamped_sides, clamped_depth, linearity);
  const double concavity = dt_polygon_concavity_for_depth(clamped_sides, clamped_depth);
  const double inner_radius = dt_polygon_inner_radius(clamped_sides, clamped_depth);

  double box_left = HUGE_VAL;
  double box_right = -HUGE_VAL;
  double box_top = HUGE_VAL;
  double box_bottom = -HUGE_VAL;
  for(int i = 0; i < point_count; i++)
  {
    double point_x = 0.0;
    double point_y = 0.0;
    _dt_polygon_unit_point(clamped_sides, clamped_depth, linearity, concavity, inner_radius, i, &point_x,
                           &point_y);
    box_left = fmin(box_left, point_x);
    box_right = fmax(box_right, point_x);
    box_top = fmin(box_top, point_y);
    box_bottom = fmax(box_bottom, point_y);
  }
  return (box_right - box_left) / (box_bottom - box_top);
}

#endif // DT_MATH_POLYGON_ENVELOPE_H
