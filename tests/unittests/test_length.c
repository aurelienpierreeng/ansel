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

/* A length, read and written in whatever unit its writer thinks in.
 *
 * Every expectation here is arithmetic done independently of the table under test: an inch is
 * 72 points because that is what a point is, a millimetre is 72/25.4 because that is what an
 * inch is, and a pixel is 0.75 because the reference pixel is 96 to the inch. Nothing is
 * checked against the header's own constants, or the test would agree with a table that had
 * been mistyped. */

#include "common/length.h"

#include <glib.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <locale.h>
#include <string.h>
#include <cmocka.h>

/* assert_float_equal() casts to float; these lengths are doubles and several of them differ in
 * the sixth figure, so the comparison is written out. */
static void _assert_close(const double got, const double wanted, const double tolerance, const char *what)
{
  if(fabs(got - wanted) > tolerance)
  {
    print_error("%s: got %.6f, wanted %.6f\n", what, got, wanted);
    fail();
  }
}

static void _a_length_is_read_in_the_unit_it_was_written_in(void **state)
{
  (void)state;
  /* The conversions, each computed here from the definition of a point rather than read out of
   * the table: a mistyped entry must fail, not agree with itself. */
  static const struct
  {
    const char *typed;
    double points;
  } lengths[] = {
    { "210mm", 210.0 * 72.0 / 25.4 },
    { "8.5in", 8.5 * 72.0 },
    { "8.5 in", 8.5 * 72.0 },
    { "612pt", 612.0 },
    { "21cm", 210.0 * 72.0 / 25.4 },
    { "21,5 cm", 215.0 * 72.0 / 25.4 },
    { "12\"", 12.0 * 72.0 },
    { "1080px", 1080.0 * 72.0 / 96.0 },
    // A length may be negative: a shadow is thrown one way or the other.
    { "-4.5mm", -4.5 * 72.0 / 25.4 },
    // And the unit is not case: a keyboard's shift key is not part of a measurement.
    { "210MM", 210.0 * 72.0 / 25.4 },
    { "8.5 IN", 8.5 * 72.0 },
    // Space either side of the number belongs to nobody.
    { "  612 pt  ", 612.0 },
  };
  for(size_t at = 0; at < G_N_ELEMENTS(lengths); at++)
  {
    double points = -12345.0;
    if(!dt_length_parse(lengths[at].typed, NULL, &points, NULL))
    {
      print_error("\"%s\" was refused\n", lengths[at].typed);
      fail();
    }
    _assert_close(points, lengths[at].points, 1e-9, lengths[at].typed);
  }
}

static void _a_bare_number_is_in_the_unit_the_field_remembers(void **state)
{
  (void)state;
  double points = 0.0;
  const char *unit = NULL;
  assert_true(dt_length_parse("12", "mm", &points, &unit));
  _assert_close(points, 12.0 * 72.0 / 25.4, 1e-9, "12 with a mm fallback");
  assert_string_equal(unit, "mm");

  // No fallback at all means the number is already points -- what every caller storing points
  // and showing them wants, and what a field with nothing remembered starts at.
  points = 0.0;
  unit = NULL;
  assert_true(dt_length_parse("12", NULL, &points, &unit));
  _assert_close(points, 12.0, 1e-9, "12 with no fallback");
  assert_null(unit);

  // A TYPED unit beats the remembered one, which is the whole point of typing it.
  points = 0.0;
  unit = NULL;
  assert_true(dt_length_parse("12pt", "mm", &points, &unit));
  _assert_close(points, 12.0, 1e-9, "12pt against a mm fallback");
  assert_string_equal(unit, "pt");

  /* A fallback this does not know is the CALLER being wrong, and is refused rather than read as
   * points: answering it would hand back a length nobody asked for and the caller would never
   * learn. A widget sanitises its remembered unit through dt_length_unit_canonical() instead. */
  points = 4321.0;
  assert_false(dt_length_parse("12", "furlong", &points, NULL));
  _assert_close(points, 4321.0, 0.0, "the length after a refused fallback");
}

static void _the_longest_spelling_wins(void **state)
{
  (void)state;
  /* An alias longer than the unit it stands for: what makes this work is that the parser matches
   * the WHOLE remainder, so a match on "in" leaving "ch" is no match at all. Measured -- putting
   * "inch" after "in" in the table does not break it, which is why the ordering is pinned as a
   * defence rather than claimed as the mechanism. */
  double points = 0.0;
  const char *unit = NULL;
  assert_true(dt_length_parse("8.5inch", NULL, &points, &unit));
  _assert_close(points, 8.5 * 72.0, 1e-9, "8.5inch");
  assert_string_equal(unit, "in");

  // And all three spellings of an inch are printed the one way.
  assert_string_equal(dt_length_unit_canonical("inch"), "in");
  assert_string_equal(dt_length_unit_canonical("\""), "in");
  assert_string_equal(dt_length_unit_canonical("in"), "in");
  assert_string_equal(dt_length_unit_canonical("IN"), "in");
  assert_null(dt_length_unit_canonical("furlong"));
  assert_null(dt_length_unit_canonical(""));
  assert_null(dt_length_unit_canonical(NULL));
}

static void _what_is_not_a_length_leaves_the_field_alone(void **state)
{
  (void)state;
  /* A field keeps what it had when somebody types nonsense into it, so the parser may not write
   * through its out-parameter on the way to refusing. */
  static const char *refused[] = {
    "",
    " ",
    "mm",          // a unit with no number is not a length
    "\"",
    "12 furlongs", // a unit this does not know is refused, never read as a prefix
    "12mmm",
    "abc",
    "-",
    ".",
    "12pt 4",      // something after the unit is not part of the length
    // strtod reads these as numbers. A length that is not a number is not a length, and one
    // that reached a page size would make a sheet nothing could draw.
    "inf",
    "-inf",
    "nan",
    "infinity",
    // And a number that is finite until its unit is applied, which is why the check is made on
    // the length in POINTS and not on what was read.
    "1e308in",
    "-1e308 cm",
  };
  for(size_t at = 0; at < G_N_ELEMENTS(refused); at++)
  {
    double points = 4321.0;
    const char *unit = (const char *)0x1;
    if(dt_length_parse(refused[at], NULL, &points, &unit))
    {
      print_error("\"%s\" was read as %.6f\n", refused[at], points);
      fail();
    }
    _assert_close(points, 4321.0, 0.0, refused[at]);
    assert_ptr_equal(unit, (const char *)0x1);
  }
  // And a NULL string, or nowhere to put the answer.
  double points = 0.0;
  assert_false(dt_length_parse(NULL, NULL, &points, NULL));
  assert_false(dt_length_parse("12mm", NULL, NULL, NULL));
}

static void _a_length_reads_back_wherever_the_decimal_separator_is(void **state)
{
  (void)state;
  /* A comma is a decimal point to half of Europe, and the field that shows it is the field it
   * is typed back into. Skipped where the locale is not installed, which is most build hosts. */
  if(setlocale(LC_NUMERIC, "fr_FR.UTF-8") == NULL && setlocale(LC_NUMERIC, "de_DE.UTF-8") == NULL)
  {
    setlocale(LC_NUMERIC, "C");
    return;
  }
  char text[64];
  dt_length_format(595.2756, "mm", -1, text, sizeof(text));
  assert_non_null(strchr(text, ','));
  double back = 0.0;
  const char *unit = NULL;
  assert_true(dt_length_parse(text, NULL, &back, &unit));
  assert_string_equal(unit, "mm");
  _assert_close(back, 595.2756, 0.05, text);
  // And a length typed with a POINT still reads, wherever the locale puts its separator.
  assert_true(dt_length_parse("210.5 mm", NULL, &back, NULL));
  _assert_close(back, 210.5 * 72.0 / 25.4, 1e-9, "210.5 mm under a comma locale");
  setlocale(LC_NUMERIC, "C");
}

static void _a_length_written_in_a_unit_reads_back_as_itself(void **state)
{
  (void)state;
  setlocale(LC_NUMERIC, "C");
  /* The round trip at each unit's own precision: written with the decimals that unit shows and
   * read back, a length may only have moved by the rounding that writing it cost. */
  static const char *units[] = { "pt", "px", "mm", "cm", "in" };
  static const double lengths[] = { 0.0, 1.0, 12.0, 72.0, 595.2756, 841.8898, -36.5, 2834.6456 };
  for(size_t which = 0; which < G_N_ELEMENTS(units); which++)
  {
    const double per_unit = dt_length_unit_points(units[which]);
    const int digits = dt_length_unit_digits(units[which]);
    // Half a quantum of the unit, in points: what one rounding of the printed figure costs.
    const double tolerance = 0.5 * per_unit * pow(10.0, -digits) + 1e-9;
    for(size_t at = 0; at < G_N_ELEMENTS(lengths); at++)
    {
      char text[64];
      dt_length_format(lengths[at], units[which], -1, text, sizeof(text));
      double back = 0.0;
      const char *unit = NULL;
      if(!dt_length_parse(text, NULL, &back, &unit))
      {
        print_error("%s written as \"%s\" was refused\n", units[which], text);
        fail();
      }
      assert_string_equal(unit, units[which]);
      _assert_close(back, lengths[at], tolerance, text);
    }
  }
}

static void _a_unit_names_what_it_is_worth_and_how_finely_it_is_shown(void **state)
{
  (void)state;
  _assert_close(dt_length_unit_points("pt"), 1.0, 1e-12, "a point");
  _assert_close(dt_length_unit_points("in"), 72.0, 1e-12, "an inch");
  _assert_close(dt_length_unit_points("mm"), 72.0 / 25.4, 1e-12, "a millimetre");
  _assert_close(dt_length_unit_points("cm"), 720.0 / 25.4, 1e-12, "a centimetre");
  // The W3C reference pixel, 96 to the inch: a 1080-pixel story is 810 points, which is what
  // the canvas's own page table says it is.
  _assert_close(dt_length_unit_points("px"), 0.75, 1e-12, "a pixel");
  _assert_close(dt_length_unit_points("furlong"), 0.0, 0.0, "a furlong");
  _assert_close(dt_length_unit_points(NULL), 0.0, 0.0, "nothing");

  /* Enough decimals to reach one point in every unit: the quantum a field shows must not be
   * coarser than a point, or a length cannot be typed exactly. */
  static const char *units[] = { "pt", "px", "mm", "cm", "in" };
  for(size_t at = 0; at < G_N_ELEMENTS(units); at++)
  {
    const double quantum = dt_length_unit_points(units[at]) * pow(10.0, -dt_length_unit_digits(units[at]));
    if(quantum > 1.0 + 1e-9)
    {
      print_error("%s shows %d decimals, a quantum of %.4f pt\n", units[at],
                  dt_length_unit_digits(units[at]), quantum);
      fail();
    }
  }
  assert_int_equal(dt_length_unit_digits("furlong"), 0);
}

static void _every_spelling_of_a_unit_answers_for_it(void **state)
{
  (void)state;
  size_t count = 0;
  const dt_length_unit_t *units = dt_length_units(&count);
  assert_non_null(units);
  assert_true(count > 0);
  for(size_t at = 0; at < count; at++)
  {
    assert_non_null(units[at].name);
    assert_true(units[at].points > 0.0);
    assert_true(units[at].digits >= 0);
    /* Longest first. This is NOT what disambiguates "8.5inch" -- matching the whole remainder
     * is, and it would still be right in any order -- but it is what would make a change to a
     * prefix match safe, so it is pinned here rather than left to be discovered. */
    if(at > 0 && strlen(units[at].name) > strlen(units[at - 1].name))
    {
      print_error("\"%s\" is longer than \"%s\" and comes after it\n", units[at].name, units[at - 1].name);
      fail();
    }
    // Every name is one the rest of the API answers for, whatever case it is asked in.
    gchar *upper = g_ascii_strup(units[at].name, -1);
    _assert_close(dt_length_unit_points(upper), units[at].points, 1e-12, upper);
    assert_non_null(dt_length_unit_canonical(upper));
    g_free(upper);
    // And its canonical spelling is itself a unit, so a stored unit always parses.
    const char *canonical = dt_length_unit_canonical(units[at].name);
    _assert_close(dt_length_unit_points(canonical), units[at].points, 1e-12, canonical);
  }
}

static void _a_formatted_length_says_which_unit_it_is_in(void **state)
{
  (void)state;
  /* The separator is the LOCALE's -- this is text a person reads and types back, and a French
   * keyboard puts a comma there. A test comparing literals therefore has to say which locale it
   * means; a bare test binary is already in "C", and this says so rather than relying on it. */
  setlocale(LC_NUMERIC, "C");
  char text[64];
  dt_length_format(595.2756, "mm", -1, text, sizeof(text));
  assert_string_equal(text, "210.0 mm");
  dt_length_format(612.0, "in", -1, text, sizeof(text));
  assert_string_equal(text, "8.500 in");
  dt_length_format(612.0, "pt", -1, text, sizeof(text));
  assert_string_equal(text, "612 pt");
  // An alias is printed in the one spelling, never as it was typed.
  dt_length_format(864.0, "\"", -1, text, sizeof(text));
  assert_string_equal(text, "12.000 in");
  dt_length_format(864.0, "inch", 1, text, sizeof(text));
  assert_string_equal(text, "12.0 in");
  // A field's own precision may be finer than the unit's: a border stored to a tenth of a point
  // must not read as "0 pt" merely because points are whole numbers.
  dt_length_format(0.5, "pt", 1, text, sizeof(text));
  assert_string_equal(text, "0.5 pt");
  // No unit, no unit written: the number IS the points.
  dt_length_format(612.0, NULL, 0, text, sizeof(text));
  assert_string_equal(text, "612");
  dt_length_format(612.0, "furlong", 0, text, sizeof(text));
  assert_string_equal(text, "612");
  // Nowhere to write it is not a crash.
  dt_length_format(612.0, "pt", 0, NULL, 16);
  dt_length_format(612.0, "pt", 0, text, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_length_is_read_in_the_unit_it_was_written_in),
    cmocka_unit_test(_a_bare_number_is_in_the_unit_the_field_remembers),
    cmocka_unit_test(_the_longest_spelling_wins),
    cmocka_unit_test(_what_is_not_a_length_leaves_the_field_alone),
    cmocka_unit_test(_a_length_written_in_a_unit_reads_back_as_itself),
    cmocka_unit_test(_a_length_reads_back_wherever_the_decimal_separator_is),
    cmocka_unit_test(_a_unit_names_what_it_is_worth_and_how_finely_it_is_shown),
    cmocka_unit_test(_every_spelling_of_a_unit_answers_for_it),
    cmocka_unit_test(_a_formatted_length_says_which_unit_it_is_in),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
