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

/**
 * @file common/length.h
 * @brief A physical length, read and written in whatever unit its writer thinks in.
 *
 * One parser and one formatter, so that a field in the canvas's properties, a guide in its
 * toolbar and a page size typed into a dialog cannot come to disagree about what "210mm" is.
 * There is no widget here and no display: this is arithmetic over strings, and
 * `tests/unittests/test_length.c` exercises all of it headless.
 *
 * THE UNIT OF ACCOUNT IS THE POINT, 1/72 inch, which is also the canvas's own unit (see the
 * note on `dt_canvas_resolution()` in `canvas/canvas.h`). Everything here converts to and from
 * points and nothing stores a unit: a unit is how a person spelled a length, never what the
 * document holds.
 *
 * `px` IS 0.75 pt, not 1. The W3C reference pixel is 96 to the inch
 * (`DT_CANVAS_REFERENCE_PIXEL_DPI`), and the canvas applies exactly that to every page size
 * written in pixels -- so a 1080 x 1920 story is 810 x 1440 points. A table that made a pixel a
 * point would answer "1080px" with a page a third larger than the story preset sitting three
 * rows above it in the same menu. It surprises whoever expects a pixel to be a pixel, and it is
 * the only answer consistent with the rest of the application.
 *
 * NOT `dt_pdf_parse_length()` (`common/pdf.c`), which this is descended from: that one knows
 * neither `in` nor `pt`, compares the remainder with `g_strcmp0` so `"8.5 in"` fails, and
 * REFUSES a bare number -- the opposite of what a field remembering its own unit needs. It also
 * lives in a header that drags the PDF writer in behind it, and `dt_pdf_parse_paper_size()`
 * depends on its exact behaviour. It is named here so nobody merges the two; leave it alone.
 */

#ifndef DT_COMMON_LENGTH_H
#define DT_COMMON_LENGTH_H

#include <glib.h>

/** @brief One spelling of a unit, and what one of it is worth in points. */
typedef struct dt_length_unit_t
{
  const char *name;       ///< as it is typed, lower case
  const char *canonical;  ///< as it is printed, or NULL when a unit is its own name
  double points;          ///< points in one of these
  int digits;             ///< decimals a field shows so that one point is still resolved
} dt_length_unit_t;

/**
 * @brief Every unit this knows, longest name first.
 *
 * What actually disambiguates `"8.5inch"` is that the parser matches the WHOLE of what follows
 * the number, so a shorter name leaving a stray "ch" is refused whatever order it is tried in.
 * The order is kept, and pinned by the tests, so that the table reads the way the parser walks
 * it and a later change to a prefix match cannot silently become a bug.
 * A caller listing units for a menu wants `dt_length_unit_canonical()` to fold the aliases.
 *
 * @param count where to write how many, or NULL.
 * @return the table, static and never freed.
 */
const dt_length_unit_t *dt_length_units(size_t *count);

/**
 * @brief The one spelling this unit is printed with, or NULL if it is not a unit.
 *
 * `"inch"` and `"\""` both answer `"in"`. The answer points into the static table and is never
 * owned by the caller, so it may be stored and compared by address.
 */
const char *dt_length_unit_canonical(const char *name);

/** @brief Points in one of this unit, or 0 for a name that is not one -- which is the test for
 * whether a string names a unit at all. */
double dt_length_unit_points(const char *unit);

/**
 * @brief Decimals a field showing this unit needs, so that one point is still reachable.
 *
 * pt 0, px 0 (a pixel is already finer than a point), mm 1, cm 2. An inch is given 3 rather
 * than the 2 that rule alone asks for, because a thousandth of an inch is the figure drawings
 * are dimensioned in, and because 0.01 in is 0.72 pt -- a quantum the eye cannot distinguish
 * from a point's, so the extra figure costs nothing and buys the convention.
 *
 * A field also carries its own precision, and shows whichever is finer: a border stored to a
 * tenth of a point must not read as "0 pt" merely because the unit is points.
 *
 * @return 0 for a name that is not a unit.
 */
int dt_length_unit_digits(const char *unit);

/**
 * @brief Read a length, in points.
 *
 * Accepts a number with or without a unit, in either decimal separator: `"210mm"`, `"8.5 in"`,
 * `"21,5 cm"`, `"12\""`, `"612pt"`, `"1080px"`, `"12"`. The unit is matched case-insensitively
 * against the whole of what follows the number, so `"12 furlongs"` is refused rather than read
 * as 12 of something.
 *
 * @param text what was typed. NULL, empty or blank is refused.
 * @param fallback_unit the unit a BARE number is in -- a field's remembered one. NULL means the
 *        number is already points. A name that is not a unit is refused outright rather than
 *        quietly read as points: it means the caller is confused, and answering it would hide
 *        that at the cost of a length nobody asked for.
 * @param points where the length goes. **Left untouched unless this returns TRUE**, so a field
 *        keeps what it had when the person typed nonsense.
 * @param unit_used where the canonical unit goes, or NULL. Points into the static table.
 * @return TRUE when a finite length was read. The check is made on the length IN POINTS, after
 *         the unit has been applied, which is the only place that catches all three ways it can
 *         fail: `"inf"` and `"nan"`, which `strtod` reads as numbers, and a number that is
 *         finite until its unit multiplies it (`"1e308in"`). A length that is not a number is
 *         not a length.
 */
gboolean dt_length_parse(const char *text, const char *fallback_unit, double *points,
                         const char **unit_used);

/**
 * @brief Write a length in the given unit, with its unit after it.
 *
 * @param unit the unit to write it in. NULL, or a name that is not a unit, writes the number of
 *        points with no unit after it.
 * @param digits decimals to show. Negative takes the unit's own.
 */
void dt_length_format(double points, const char *unit, int digits, char *out, size_t length);

#endif // DT_COMMON_LENGTH_H
