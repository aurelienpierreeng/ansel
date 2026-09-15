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

/* The polygon envelope, against geometry that owes nothing to it.
 *
 * Every expectation is a fact about regular polygons and stars written independently of the
 * header: a tip on the unit circle, an edge's midpoint at the apothem cos(pi / n), a
 * pentagram's notch on the chord joining the tips either side of it, the width of a triangle
 * over its height. The curve is only trusted where it agrees with them, and its validity
 * domain -- the part that decides whether a star can collapse onto its centre -- is swept
 * over every number of sides the canvas offers rather than sampled. */

#include "math/polygon_envelope.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#define TEST_PI (3.14159265358979323846)
#define ANGLE_COUNT 3600

/* assert_float_equal() casts to float, which cannot hold a 1e-12 tolerance. Written out, the
 * comparison can also say what it compared and on which shape, which a sweep over ten side
 * counts needs to be read at all. */
static void _assert_near(const double actual, const double expected, const double tolerance, const char *what,
                         const int sides, const int index)
{
  const double error = fabs(actual - expected);
  if(!(error <= tolerance))
  {
    fail_msg("%s (sides %d, index %d): %.17g, expected %.17g within %g, off by %g", what, sides, index, actual,
             expected, tolerance, error);
  }
}

static double _radius_of(const double *const points, const int index)
{
  return hypot(points[2 * index], points[2 * index + 1]);
}

/* Twice the shoelace area. With y down, a positive value is an outline turning clockwise on
 * screen. */
static double _signed_double_area(const double *const points, const int point_count)
{
  double area = 0.0;
  for(int i = 0; i < point_count; i++)
  {
    const int next = (i + 1) % point_count;
    area += points[2 * i] * points[2 * next + 1] - points[2 * next] * points[2 * i + 1];
  }
  return area;
}

/* Distance from a point to the infinite line through two others. */
static double _distance_to_line(const double point_x, const double point_y, const double from_x,
                                const double from_y, const double to_x, const double to_y)
{
  const double along_x = to_x - from_x;
  const double along_y = to_y - from_y;
  const double cross = along_x * (point_y - from_y) - along_y * (point_x - from_x);
  return fabs(cross) / hypot(along_x, along_y);
}

static void _a_regular_polygon_has_its_tips_on_the_circle_and_its_edges_at_the_apothem(void **state)
{
  (void)state;
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
  {
    const int point_count = dt_polygon_unit_outline(sides, 0.0, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
    assert_int_equal(point_count, sides);

    const double apothem = cos(TEST_PI / sides);
    const double concavity = dt_polygon_concavity_for_depth(sides, 0.0);
    for(int i = 0; i < sides; i++)
    {
      const double theta = 2.0 * TEST_PI * i / sides;
      _assert_near(_radius_of(points, i), 1.0, 1e-12, "tip radius", sides, i);
      _assert_near(points[2 * i], sin(theta), 1e-12, "tip x", sides, i);
      _assert_near(points[2 * i + 1], -cos(theta), 1e-12, "tip y", sides, i);

      const int next = (i + 1) % sides;
      const double middle_x = 0.5 * (points[2 * i] + points[2 * next]);
      const double middle_y = 0.5 * (points[2 * i + 1] + points[2 * next + 1]);
      _assert_near(hypot(middle_x, middle_y), apothem, 1e-12, "edge midpoint radius", sides, i);

      /* the curve itself reaches the apothem half way between two tips */
      const double middle_theta = (2.0 * i + 1.0) * TEST_PI / sides;
      _assert_near(dt_polygon_envelope(sides, concavity, 1.0, middle_theta), apothem, 1e-12,
                   "envelope at the edge midpoint", sides, i);
    }
    assert_true(_signed_double_area(points, point_count) > 0.0);
  }

  /* the design's worked triangle, read off the drawing */
  const int triangle_count = dt_polygon_unit_outline(3, 0.0, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
  assert_int_equal(triangle_count, 3);
  _assert_near(points[0], 0.0, 1e-12, "triangle top x", 3, 0);
  _assert_near(points[1], -1.0, 1e-12, "triangle top y", 3, 0);
  _assert_near(points[2], sqrt(3.0) / 2.0, 1e-12, "triangle right x", 3, 1);
  _assert_near(points[3], 0.5, 1e-12, "triangle right y", 3, 1);
  _assert_near(points[4], -sqrt(3.0) / 2.0, 1e-12, "triangle left x", 3, 2);
  _assert_near(points[5], 0.5, 1e-12, "triangle left y", 3, 2);
}

static void _a_star_alternates_tips_and_notches(void **state)
{
  (void)state;
  /* 1e-5 is written out: a notch a hundred-thousandth of the radius deep is still a star, whatever
   * the header decides its shallowest one is */
  const double depths[] = { DT_POLYGON_MIN_DEPTH, 1e-5, 0.1, 0.25, 0.5, 0.75, DT_POLYGON_MAX_DEPTH };
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
  {
    for(int j = 0; j < depth_count; j++)
    {
      const double depth = depths[j];
      const int point_count = dt_polygon_unit_outline(sides, depth, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
      assert_int_equal(point_count, 2 * sides);

      const double notch_radius = (1.0 - depth) * cos(TEST_PI / sides);
      _assert_near(dt_polygon_inner_radius(sides, depth), notch_radius, 1e-15, "inner radius", sides, j);
      for(int i = 0; i < point_count; i++)
      {
        const double theta = TEST_PI * i / sides;
        const double radius = (i % 2 == 0) ? 1.0 : notch_radius;
        _assert_near(points[2 * i], radius * sin(theta), 1e-12, "star vertex x", sides, i);
        _assert_near(points[2 * i + 1], -radius * cos(theta), 1e-12, "star vertex y", sides, i);
      }

      /* the concavity derived from the depth puts the straight curve's notch on that vertex */
      const double concavity = dt_polygon_concavity_for_depth(sides, depth);
      _assert_near(dt_polygon_envelope(sides, concavity, 1.0, TEST_PI / sides), notch_radius, 1e-12,
                   "envelope at the notch", sides, j);
      assert_true(_signed_double_area(points, point_count) > 0.0);
    }
  }
}

static void _the_pentagram_is_the_five_point_star_whose_notches_meet_the_far_tips_chords(void **state)
{
  (void)state;
  /* the golden-ratio inner radius of the pentagram, (3 - sqrt 5) / 2, and the depth giving it */
  const double pentagram_radius = (3.0 - sqrt(5.0)) / 2.0;
  const double exact_depth = 1.0 - pentagram_radius / cos(TEST_PI / 5.0);
  _assert_near(exact_depth, 0.527864, 1e-6, "pentagram depth", 5, 0);
  _assert_near(dt_polygon_inner_radius(5, 0.527864), 0.381966, 1e-6, "pentagram inner radius", 5, 0);
  _assert_near(dt_polygon_concavity_for_depth(5, exact_depth), 3.0, 1e-12, "pentagram concavity", 5, 0);
  _assert_near(dt_polygon_concavity_for_depth(5, 0.527864), 3.0, 1e-6, "rounded pentagram concavity", 5, 0);

  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  const int point_count = dt_polygon_unit_outline(5, exact_depth, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
  assert_int_equal(point_count, 10);

  /* what makes it a pentagram: the notch after tip j sits on the straight line joining tip
   * j - 1 to tip j + 1, the two tips either side of the notch's own pair */
  for(int j = 0; j < 5; j++)
  {
    const int notch = 2 * j + 1;
    const int previous_tip = 2 * ((j + 4) % 5);
    const int next_tip = 2 * ((j + 1) % 5);
    const double distance
        = _distance_to_line(points[2 * notch], points[2 * notch + 1], points[2 * previous_tip],
                            points[2 * previous_tip + 1], points[2 * next_tip], points[2 * next_tip + 1]);
    _assert_near(distance, 0.0, 1e-12, "notch off the far tips' chord", 5, j);
  }

  /* the design's worked vertices, to the three decimals it prints */
  const double worked[10][2] = { { 0.0, -1.0 },     { 0.225, -0.309 }, { 0.951, -0.309 }, { 0.363, 0.118 },
                                 { 0.588, 0.809 },  { 0.0, 0.382 },    { -0.588, 0.809 }, { -0.363, 0.118 },
                                 { -0.951, -0.309 }, { -0.225, -0.309 } };
  for(int i = 0; i < 10; i++)
  {
    _assert_near(points[2 * i], worked[i][0], 5e-4, "worked pentagram x", 5, i);
    _assert_near(points[2 * i + 1], worked[i][1], 5e-4, "worked pentagram y", 5, i);
  }
}

/* A notch that has not left the straight edge is the edge's midpoint, where the two edges are
 * opposite and have no bisector. Such a depth -- a denormal read from a float, or anything
 * under a millionth -- must give the convex polygon, bit for bit, in every function. */
static void _a_notch_that_does_not_leave_the_edge_is_no_notch(void **state)
{
  (void)state;
  const double shallow_depths[] = { 1e-20, 1e-16, (double)1e-40f, 0.5 * DT_POLYGON_MIN_DEPTH };
  const int depth_count = sizeof(shallow_depths) / sizeof(shallow_depths[0]);
  const double roundnesses[] = { 0.0, 0.5 };
  double convex[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  double shallow[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
  {
    for(int j = 0; j < depth_count; j++)
    {
      const double depth = shallow_depths[j];
      assert_true(depth > 0.0);
      assert_true(dt_polygon_concavity_for_depth(sides, depth) == 1.0);
      assert_true(dt_polygon_inner_radius(sides, depth) == dt_polygon_inner_radius(sides, 0.0));
      for(int k = 0; k < 2; k++)
      {
        const int convex_count
            = dt_polygon_unit_outline(sides, 0.0, roundnesses[k], convex, DT_POLYGON_OUTLINE_MAX_POINTS);
        const int shallow_count
            = dt_polygon_unit_outline(sides, depth, roundnesses[k], shallow, DT_POLYGON_OUTLINE_MAX_POINTS);
        assert_int_equal(shallow_count, convex_count);
        if(roundnesses[k] == 0.0) assert_int_equal(shallow_count, sides);
        for(int i = 0; i < 2 * shallow_count; i++)
        {
          if(shallow[i] != convex[i])
            fail_msg("depth %g moved coordinate %d of a %d-sided outline, roundness %g", depth, i, sides,
                     roundnesses[k]);
        }
        assert_true(dt_polygon_unit_aspect(sides, depth, roundnesses[k])
                    == dt_polygon_unit_aspect(sides, 0.0, roundnesses[k]));
      }
    }

    /* and the shallowest notch that is one is a star whose every notch has a bisector */
    const int star_count = dt_polygon_unit_outline(sides, DT_POLYGON_MIN_DEPTH, 0.0, shallow,
                                                   DT_POLYGON_OUTLINE_MAX_POINTS);
    assert_int_equal(star_count, 2 * sides);
    for(int notch = 1; notch < star_count; notch += 2)
    {
      const int before = notch - 1;
      const int after = (notch + 1) % star_count;
      const double to_before_x = shallow[2 * before] - shallow[2 * notch];
      const double to_before_y = shallow[2 * before + 1] - shallow[2 * notch + 1];
      const double to_after_x = shallow[2 * after] - shallow[2 * notch];
      const double to_after_y = shallow[2 * after + 1] - shallow[2 * notch + 1];
      const double before_length = hypot(to_before_x, to_before_y);
      const double after_length = hypot(to_after_x, to_after_y);
      const double bisector_x = to_before_x / before_length + to_after_x / after_length;
      const double bisector_y = to_before_y / before_length + to_after_y / after_length;
      /* each edge turns by more than the depth (measured, the bisector is 1.15 depths long at
       * three sides and longer with more), and a reflex notch's bisector points out */
      assert_true(hypot(bisector_x, bisector_y) > 0.5 * DT_POLYGON_MIN_DEPTH);
      assert_true(bisector_x * shallow[2 * notch] + bisector_y * shallow[2 * notch + 1] > 0.0);
    }
  }
}

static void _depth_zero_is_concavity_one_exactly(void **state)
{
  (void)state;
  for(int sides = 3; sides <= 12; sides++)
  {
    assert_true(dt_polygon_concavity_for_depth(sides, 0.0) == 1.0);
    assert_true(dt_polygon_concavity_for_depth(sides, -0.5) == 1.0);
    /* and the concavity grows with the depth, so a deeper star is a sharper one */
    assert_true(dt_polygon_concavity_for_depth(sides, 0.01) > 1.0);
    assert_true(dt_polygon_concavity_for_depth(sides, 0.5) > dt_polygon_concavity_for_depth(sides, 0.25));
  }
}

static void _the_envelope_stays_valid_up_to_the_deepest_star(void **state)
{
  (void)state;
  const double linearities[] = { 0.0, 0.5, 1.0 };
  /* the last four are out of range and must clamp into it */
  const double depths[] = { 0.0, 0.5, DT_POLYGON_MAX_DEPTH, 1.0, 3.0, -1.0, NAN };
  const int linearity_count = sizeof(linearities) / sizeof(linearities[0]);
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  for(int sides = 3; sides <= 12; sides++)
  {
    for(int j = 0; j < depth_count; j++)
    {
      const double concavity = dt_polygon_concavity_for_depth(sides, depths[j]);
      assert_true(isfinite(concavity));
      assert_true(concavity >= 1.0);
      assert_true(concavity < sides - 1.0);
      for(int k = 0; k < linearity_count; k++)
      {
        const double linearity = linearities[k];
        assert_true(2.0 * asin(linearity) + TEST_PI * concavity < sides * TEST_PI);
        for(int i = 0; i < ANGLE_COUNT; i++)
        {
          const double theta = 2.0 * TEST_PI * i / ANGLE_COUNT;
          const double radius = dt_polygon_envelope(sides, concavity, linearity, theta);
          if(!(isfinite(radius) && radius > 0.0 && radius <= 1.0))
          {
            fail_msg("envelope %.17g out of (0, 1] at sides %d, depth %g, linearity %g, angle %d", radius, sides,
                     depths[j], linearity, i);
          }
        }
      }
    }
  }
}

static void _any_arguments_give_a_bounded_clockwise_outline(void **state)
{
  (void)state;
  const int side_counts[] = { -5, 0, 2, 3, 7, 12, 13, 100 };
  const double depths[] = { -1.0, 0.0, 0.5, DT_POLYGON_MAX_DEPTH, 1.0, NAN };
  const double roundnesses[] = { -1.0, 0.0, 0.5, 1.0, 2.0, NAN };
  const int side_count = sizeof(side_counts) / sizeof(side_counts[0]);
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  const int roundness_count = sizeof(roundnesses) / sizeof(roundnesses[0]);
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int i = 0; i < side_count; i++)
    for(int j = 0; j < depth_count; j++)
      for(int k = 0; k < roundness_count; k++)
      {
        const int sides = side_counts[i];
        const int point_count
            = dt_polygon_unit_outline(sides, depths[j], roundnesses[k], points, DT_POLYGON_OUTLINE_MAX_POINTS);
        assert_true(point_count >= DT_POLYGON_MIN_SIDES);
        assert_true(point_count <= DT_POLYGON_OUTLINE_MAX_POINTS);
        for(int idx = 0; idx < point_count; idx++)
        {
          const double radius = _radius_of(points, idx);
          if(!(isfinite(radius) && radius > 0.0 && radius <= 1.0 + 1e-12))
          {
            fail_msg("radius %.17g at sides %d, depth %g, roundness %g, point %d", radius, sides, depths[j],
                     roundnesses[k], idx);
          }
        }
        assert_true(_signed_double_area(points, point_count) > 0.0);

        const double aspect = dt_polygon_unit_aspect(sides, depths[j], roundnesses[k]);
        assert_true(isfinite(aspect));
        assert_true(aspect > 0.0);
      }

  /* the sides clamp to what the canvas offers */
  assert_int_equal(dt_polygon_unit_outline(2, 0.0, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS), 3);
  assert_int_equal(dt_polygon_unit_outline(20, 0.0, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS), 12);
  assert_int_equal(dt_polygon_unit_outline(20, 0.5, 0.5, points, DT_POLYGON_OUTLINE_MAX_POINTS),
                   DT_POLYGON_OUTLINE_MAX_POINTS);

  /* a buffer one point short gets nothing, not a truncated shape */
  const double sentinel = 12345.0;
  for(int i = 0; i < 2 * DT_POLYGON_OUTLINE_MAX_POINTS; i++)
    points[i] = sentinel;
  assert_int_equal(dt_polygon_unit_outline(6, 0.5, 0.0, points, 11), 0);
  assert_int_equal(dt_polygon_unit_outline(6, 0.5, 0.25, points, 6 * 32 - 1), 0);
  for(int i = 0; i < 2 * DT_POLYGON_OUTLINE_MAX_POINTS; i++)
    assert_true(points[i] == sentinel);
  assert_int_equal(dt_polygon_unit_outline(6, 0.5, 0.0, points, 12), 12);
}

static void _no_linearity_is_a_circle(void **state)
{
  (void)state;
  const double depths[] = { 0.0, 0.5, DT_POLYGON_MAX_DEPTH };
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
  {
    for(int j = 0; j < depth_count; j++)
    {
      const double concavity = dt_polygon_concavity_for_depth(sides, depths[j]);
      for(int i = 0; i < ANGLE_COUNT; i++)
      {
        const double theta = 2.0 * TEST_PI * i / ANGLE_COUNT;
        assert_true(dt_polygon_envelope(sides, concavity, 0.0, theta) == 1.0);
      }

      /* a circle has no corner for the samples to crowd towards, so they are even, and the
       * four compass points among them make its box square */
      const int point_count
          = dt_polygon_unit_outline(sides, depths[j], 1.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
      assert_int_equal(point_count, 2 * sides * DT_POLYGON_SAMPLES_PER_HALF_BLADE);
      for(int i = 0; i < point_count; i++)
      {
        const double theta = 2.0 * TEST_PI * i / point_count;
        _assert_near(points[2 * i], sin(theta), 1e-12, "circle x", sides, i);
        _assert_near(points[2 * i + 1], -cos(theta), 1e-12, "circle y", sides, i);
      }
      _assert_near(dt_polygon_unit_aspect(sides, depths[j], 1.0), 1.0, 1e-12, "circle aspect", sides, j);
    }
  }
}

static void _full_linearity_is_straight_lines(void **state)
{
  (void)state;
  const double depths[] = { 0.0, 0.3, 0.527864, DT_POLYGON_MAX_DEPTH };
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
  {
    const double blade = 2.0 * TEST_PI / sides;
    for(int j = 0; j < depth_count; j++)
    {
      const double depth = depths[j];
      const double concavity = dt_polygon_concavity_for_depth(sides, depth);
      const double tip_to_notch = TEST_PI * (concavity + 1.0) / (2.0 * sides);

      /* the straight outline gives the tips and, for a star, the notches between them */
      const int straight_count = dt_polygon_unit_outline(sides, depth, 0.0, points, DT_POLYGON_OUTLINE_MAX_POINTS);
      assert_int_equal(straight_count, (depth > 0.0) ? 2 * sides : sides);
      const double segment_angle = 2.0 * TEST_PI / straight_count;

      for(int i = 0; i < ANGLE_COUNT; i++)
      {
        const double theta = 2.0 * TEST_PI * i / ANGLE_COUNT;
        const double from_tip = remainder(theta, blade);
        const double radius = dt_polygon_envelope(sides, concavity, 1.0, theta);

        /* asin(cos x) = pi / 2 - |x|: the polar equation of a straight line */
        const double line = cos(tip_to_notch) / cos(tip_to_notch - fabs(from_tip));
        _assert_near(radius, line, 1e-12, "straight envelope", sides, i);

        /* and that line is the one through the outline's vertices either side */
        const double segments = floor(theta / segment_angle);
        const int first = ((int)segments) % straight_count;
        const int second = (first + 1) % straight_count;
        const double distance = _distance_to_line(radius * sin(theta), -radius * cos(theta), points[2 * first],
                                                  points[2 * first + 1], points[2 * second],
                                                  points[2 * second + 1]);
        _assert_near(distance, 0.0, 1e-12, "envelope off its straight edge", sides, i);
      }
    }
  }
}

static void _a_rounded_outline_meets_the_straight_one_at_its_tips_and_notches(void **state)
{
  (void)state;
  double rounded[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  double straight[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  const int per_half_blade = DT_POLYGON_SAMPLES_PER_HALF_BLADE;
  for(int sides = 3; sides <= 12; sides++)
  {
    const int straight_count = dt_polygon_unit_outline(sides, 0.5, 0.0, straight, DT_POLYGON_OUTLINE_MAX_POINTS);
    assert_int_equal(straight_count, 2 * sides);

    /* any roundness keeps every tip on the unit circle, where the straight star has it */
    const int rounded_count = dt_polygon_unit_outline(sides, 0.5, 0.5, rounded, DT_POLYGON_OUTLINE_MAX_POINTS);
    assert_int_equal(rounded_count, 2 * sides * per_half_blade);
    for(int j = 0; j < sides; j++)
    {
      const int sample = 2 * j * per_half_blade;
      _assert_near(rounded[2 * sample], straight[4 * j], 1e-12, "rounded tip x", sides, j);
      _assert_near(rounded[2 * sample + 1], straight[4 * j + 1], 1e-12, "rounded tip y", sides, j);
    }

    /* the crowded samples leave the notch where it is: exactly on the notch's own ray */
    for(int j = 0; j < sides; j++)
    {
      const int sample = (2 * j + 1) * per_half_blade;
      const double notch_theta = (2.0 * j + 1.0) * TEST_PI / sides;
      const double off_ray = rounded[2 * sample] * cos(notch_theta) + rounded[2 * sample + 1] * sin(notch_theta);
      _assert_near(off_ray, 0.0, 1e-12, "rounded notch off its ray", sides, j);
    }

    /* and as the roundness vanishes the notches close on the straight star's. asin is
     * square-root singular at 1, so a roundness of 1e-12 still shifts the curve's argument by
     * sqrt(2e-12) / n, about 1.4e-6 / n: measured, it moves the notches by at most 5.1e-7 over 3
     * to 12 sides. The tolerance is that scale, not a rounding error */
    const int barely_count = dt_polygon_unit_outline(sides, 0.5, 1e-12, rounded, DT_POLYGON_OUTLINE_MAX_POINTS);
    assert_int_equal(barely_count, 2 * sides * per_half_blade);
    for(int j = 0; j < sides; j++)
    {
      const int sample = (2 * j + 1) * per_half_blade;
      const int notch = 2 * j + 1;
      _assert_near(rounded[2 * sample], straight[2 * notch], 1e-6, "vanishing roundness notch x", sides, j);
      _assert_near(rounded[2 * sample + 1], straight[2 * notch + 1], 1e-6, "vanishing roundness notch y", sides,
                   j);
    }
  }
}

/* The lens blur's curve, written out here from blurs.c rather than read from the header, with the
 * concavity from the design's own form of the tip-to-notch angle. Every other check pins the
 * curve at k = 0, at k = 1 or at its tips, where M = 1 whatever k is; a formula wrong only in
 * between -- k squared, say, which is exact at both ends -- passes all of them. */
static double _lens_blur_radius(const int sides, const double depth, const double linearity, const double theta)
{
  const double inner_radius = (1.0 - depth) * cos(TEST_PI / sides);
  const double tip_to_notch
      = atan2(1.0 - inner_radius * cos(TEST_PI / sides), inner_radius * sin(TEST_PI / sides));
  const double concavity = 2.0 * sides * tip_to_notch / TEST_PI - 1.0;
  return cos((2.0 * asin(linearity) + TEST_PI * concavity) / (2.0 * sides))
         / cos((2.0 * asin(linearity * cos(sides * theta)) + TEST_PI * concavity) / (2.0 * sides));
}

static void _a_rounded_outline_is_the_lens_blur_curve(void **state)
{
  (void)state;
  const double depths[] = { 0.0, 0.25, 0.5, DT_POLYGON_MAX_DEPTH };
  const double roundnesses[] = { 0.1, 0.5, 0.8 };
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
    for(int j = 0; j < 4; j++)
      for(int k = 0; k < 3; k++)
      {
        const int point_count
            = dt_polygon_unit_outline(sides, depths[j], roundnesses[k], points, DT_POLYGON_OUTLINE_MAX_POINTS);
        assert_int_equal(point_count, 2 * sides * DT_POLYGON_SAMPLES_PER_HALF_BLADE);
        for(int i = 0; i < point_count; i++)
        {
          /* every sample, wherever the header chose to put it, lies on the curve at its own angle */
          const double theta = atan2(points[2 * i], -points[2 * i + 1]);
          const double expected = _lens_blur_radius(sides, depths[j], 1.0 - roundnesses[k], theta);
          _assert_near(_radius_of(points, i), expected, 1e-12, "sample off the lens blur curve", sides, i);
        }
      }

  /* and two values computed once, outside this code, from the same formula: the first notch of
   * a five-point star at depth 0.5 and of an eight-point one at depth 0.95 */
  const int notch_sample = DT_POLYGON_SAMPLES_PER_HALF_BLADE;
  const int five_count = dt_polygon_unit_outline(5, 0.5, 0.5, points, DT_POLYGON_OUTLINE_MAX_POINTS);
  assert_int_equal(five_count, 160);
  const double five_point_notch = 0.758783135176054;
  const double five_notch_theta = TEST_PI / 5.0;
  _assert_near(points[2 * notch_sample], five_point_notch * sin(five_notch_theta), 1e-12, "five-point notch x", 5,
               0);
  _assert_near(points[2 * notch_sample + 1], -five_point_notch * cos(five_notch_theta), 1e-12,
               "five-point notch y", 5, 0);
  const int eight_count
      = dt_polygon_unit_outline(8, DT_POLYGON_MAX_DEPTH, 0.8, points, DT_POLYGON_OUTLINE_MAX_POINTS);
  assert_int_equal(eight_count, 256);
  _assert_near(_radius_of(points, notch_sample), 0.793099771691145, 1e-12, "eight-point notch radius", 8, 0);
}

/* How far the polyline strays from the curve it samples, as a fraction of the tip radius. Even
 * spacing in angle strayed up to 1.36e-2, nearly all of it in the segment either side of a tip of
 * a nearly straight shape; the crowded samples stay under 1.75e-3 on a finer grid than this. */
#define CHORD_TOLERANCE 2e-3
#define CHORD_PROBES 32

static double _distance_to_segment(const double point_x, const double point_y, const double from_x,
                                   const double from_y, const double to_x, const double to_y)
{
  const double along_x = to_x - from_x;
  const double along_y = to_y - from_y;
  const double length_squared = along_x * along_x + along_y * along_y;
  const double projection = ((point_x - from_x) * along_x + (point_y - from_y) * along_y) / length_squared;
  const double clamped = fmin(fmax(projection, 0.0), 1.0);
  return hypot(point_x - from_x - clamped * along_x, point_y - from_y - clamped * along_y);
}

static void _a_rounded_outline_stays_close_to_its_curve(void **state)
{
  (void)state;
  const double depths[] = { 0.0, 0.25, 0.5, 0.75, DT_POLYGON_MAX_DEPTH };
  const double roundnesses[] = { 0.002, 0.005, 0.01, 0.02, 0.04, 0.08, 0.15, 0.3, 0.6, 1.0 };
  const int depth_count = sizeof(depths) / sizeof(depths[0]);
  const int roundness_count = sizeof(roundnesses) / sizeof(roundnesses[0]);
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
    for(int j = 0; j < depth_count; j++)
      for(int k = 0; k < roundness_count; k++)
      {
        const double linearity = 1.0 - roundnesses[k];
        const double concavity = dt_polygon_concavity_for_depth(sides, depths[j]);
        const int point_count
            = dt_polygon_unit_outline(sides, depths[j], roundnesses[k], points, DT_POLYGON_OUTLINE_MAX_POINTS);
        for(int i = 0; i < point_count; i++)
        {
          const int next = (i + 1) % point_count;
          /* the outline turns clockwise, so the angle grows along a segment; one that seems to
           * shrink has crossed the branch cut of atan2 */
          const double from_theta = atan2(points[2 * i], -points[2 * i + 1]);
          double to_theta = atan2(points[2 * next], -points[2 * next + 1]);
          if(to_theta < from_theta) to_theta += 2.0 * TEST_PI;
          assert_true(to_theta > from_theta);

          for(int probe = 1; probe < CHORD_PROBES; probe++)
          {
            const double theta = from_theta + (to_theta - from_theta) * probe / CHORD_PROBES;
            const double radius = dt_polygon_envelope(sides, concavity, linearity, theta);
            const double distance
                = _distance_to_segment(radius * sin(theta), -radius * cos(theta), points[2 * i], points[2 * i + 1],
                                       points[2 * next], points[2 * next + 1]);
            if(!(distance <= CHORD_TOLERANCE))
            {
              fail_msg("curve %.3g off its polyline at sides %d, depth %g, roundness %g, segment %d", distance,
                       sides, depths[j], roundnesses[k], i);
            }
          }
        }
      }
}

static void _the_aspect_is_the_box_of_the_outline(void **state)
{
  (void)state;
  _assert_near(dt_polygon_unit_aspect(3, 0.0, 0.0), 1.1547, 1e-4, "triangle aspect", 3, 0);
  _assert_near(dt_polygon_unit_aspect(3, 0.0, 0.0), 2.0 / sqrt(3.0), 1e-12, "triangle aspect", 3, 0);
  _assert_near(dt_polygon_unit_aspect(6, 0.0, 0.0), 0.8660, 1e-4, "hexagon aspect", 6, 0);
  _assert_near(dt_polygon_unit_aspect(6, 0.0, 0.0), sqrt(3.0) / 2.0, 1e-12, "hexagon aspect", 6, 0);
  _assert_near(dt_polygon_unit_aspect(5, 0.0, 0.0), 1.0515, 1e-4, "pentagon aspect", 5, 0);
  _assert_near(dt_polygon_unit_aspect(5, 0.0, 0.0), 2.0 * sin(2.0 * TEST_PI / 5.0) / (1.0 + cos(TEST_PI / 5.0)),
               1e-12, "pentagon aspect", 5, 0);
  _assert_near(dt_polygon_unit_aspect(4, 0.0, 0.0), 1.0, 1e-12, "square aspect", 4, 0);
  /* a star's notches sit inside its tips' box, so the pentagram is as wide as the pentagon */
  _assert_near(dt_polygon_unit_aspect(5, 0.527864, 0.0), dt_polygon_unit_aspect(5, 0.0, 0.0), 1e-12,
               "pentagram aspect", 5, 0);

  const double depths[] = { 0.0, 0.4, DT_POLYGON_MAX_DEPTH };
  const double roundnesses[] = { 0.0, 0.3, 0.8 };
  double points[2 * DT_POLYGON_OUTLINE_MAX_POINTS];
  for(int sides = 3; sides <= 12; sides++)
    for(int j = 0; j < 3; j++)
      for(int k = 0; k < 3; k++)
      {
        const int point_count
            = dt_polygon_unit_outline(sides, depths[j], roundnesses[k], points, DT_POLYGON_OUTLINE_MAX_POINTS);
        double box_left = HUGE_VAL;
        double box_right = -HUGE_VAL;
        double box_top = HUGE_VAL;
        double box_bottom = -HUGE_VAL;
        for(int i = 0; i < point_count; i++)
        {
          box_left = fmin(box_left, points[2 * i]);
          box_right = fmax(box_right, points[2 * i]);
          box_top = fmin(box_top, points[2 * i + 1]);
          box_bottom = fmax(box_bottom, points[2 * i + 1]);
        }
        const double box_aspect = (box_right - box_left) / (box_bottom - box_top);
        _assert_near(dt_polygon_unit_aspect(sides, depths[j], roundnesses[k]), box_aspect, 1e-15,
                     "aspect against the outline's box", sides, j * 3 + k);
      }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_regular_polygon_has_its_tips_on_the_circle_and_its_edges_at_the_apothem),
    cmocka_unit_test(_a_star_alternates_tips_and_notches),
    cmocka_unit_test(_the_pentagram_is_the_five_point_star_whose_notches_meet_the_far_tips_chords),
    cmocka_unit_test(_a_notch_that_does_not_leave_the_edge_is_no_notch),
    cmocka_unit_test(_depth_zero_is_concavity_one_exactly),
    cmocka_unit_test(_the_envelope_stays_valid_up_to_the_deepest_star),
    cmocka_unit_test(_any_arguments_give_a_bounded_clockwise_outline),
    cmocka_unit_test(_no_linearity_is_a_circle),
    cmocka_unit_test(_full_linearity_is_straight_lines),
    cmocka_unit_test(_a_rounded_outline_meets_the_straight_one_at_its_tips_and_notches),
    cmocka_unit_test(_a_rounded_outline_is_the_lens_blur_curve),
    cmocka_unit_test(_a_rounded_outline_stays_close_to_its_curve),
    cmocka_unit_test(_the_aspect_is_the_box_of_the_outline),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
