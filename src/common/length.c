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

#include "common/length.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * LONGEST NAME FIRST: the parser takes the first entry that matches the whole remainder, and
 * that order is what makes "8.5inch" an inch rather than a failed "in". The canonical spellings
 * -- the ones a length is printed in -- are the five a unit is its own name for; "inch" and the
 * double quote are aliases of "in" and are never written back.
 */
static const dt_length_unit_t _units[] = {
  { "inch", "in", 72.0, 3 },
  { "mm", NULL, 72.0 / 25.4, 1 },
  { "cm", NULL, 720.0 / 25.4, 2 },
  { "pt", NULL, 1.0, 0 },
  { "px", NULL, 0.75, 0 },  // the W3C reference pixel, 96 to the inch: see the note in length.h
  { "in", NULL, 72.0, 3 },
  { "\"", "in", 72.0, 3 },
};

/** Where a unit is printed from: an alias names the entry it is an alias OF, so a unit that
 * happens to be worth the same as another is not renamed into it. */
static const char *_canonical_name(const dt_length_unit_t *unit)
{
  if(unit == NULL) return NULL;
  return unit->canonical != NULL ? unit->canonical : unit->name;
}

/** The table entry this name IS, whatever case it was typed in, or NULL. */
static const dt_length_unit_t *_lookup(const char *name)
{
  if(name == NULL || name[0] == '\0') return NULL;
  for(size_t at = 0; at < G_N_ELEMENTS(_units); at++)
    if(g_ascii_strcasecmp(name, _units[at].name) == 0) return &_units[at];
  return NULL;
}

const dt_length_unit_t *dt_length_units(size_t *count)
{
  if(count != NULL) *count = G_N_ELEMENTS(_units);
  return _units;
}

const char *dt_length_unit_canonical(const char *name)
{
  return _canonical_name(_lookup(name));
}

double dt_length_unit_points(const char *unit)
{
  const dt_length_unit_t *found = _lookup(unit);
  return found != NULL ? found->points : 0.0;
}

int dt_length_unit_digits(const char *unit)
{
  const dt_length_unit_t *found = _lookup(unit);
  return found != NULL ? found->digits : 0;
}

gboolean dt_length_parse(const char *text, const char *fallback_unit, double *points,
                         const char **unit_used)
{
  if(text == NULL || points == NULL) return FALSE;

  /*
   * A bare number is in the field's own unit, and a caller that names one this does not know is
   * refused rather than read as points: silently reinterpreting it would answer with a length
   * nobody asked for, and the caller would never learn it was wrong.
   */
  const dt_length_unit_t *fallback = NULL;
  if(fallback_unit != NULL && fallback_unit[0] != '\0')
  {
    fallback = _lookup(fallback_unit);
    if(fallback == NULL) return FALSE;
  }

  // A decimal comma is what a French or German keyboard puts there, and it is what the PDF
  // writer's own parser has always accepted.
  gchar *copy = g_strdup(text);
  g_strdelimit(copy, ",", '.');

  const char *at = copy;
  while(*at != '\0' && g_ascii_isspace(*at)) at++;
  char *after = NULL;
  const double amount = g_ascii_strtod(at, &after);
  if(after == at)
  {
    // Nothing numeric at all. Whether what WAS read is a number is asked below, of the length
    // in points: a check here would miss a finite number its unit overflows.
    g_free(copy);
    return FALSE;
  }

  const char *rest = after;
  while(*rest != '\0' && g_ascii_isspace(*rest)) rest++;
  // And the trailing spaces, so that "8.5 in " is the length "8.5 in" is.
  size_t rest_length = strlen(rest);
  while(rest_length > 0 && g_ascii_isspace(rest[rest_length - 1])) rest_length--;

  const dt_length_unit_t *unit = fallback;
  if(rest_length > 0)
  {
    unit = NULL;
    for(size_t index = 0; index < G_N_ELEMENTS(_units); index++)
    {
      const size_t name_length = strlen(_units[index].name);
      // The WHOLE remainder, so "12 furlongs" is refused where a prefix match would read it as
      // twelve of something. Longest first, so "inch" is tried before "in".
      if(name_length == rest_length && g_ascii_strncasecmp(rest, _units[index].name, name_length) == 0)
      {
        unit = &_units[index];
        break;
      }
    }
    if(unit == NULL)
    {
      g_free(copy);
      return FALSE;
    }
  }

  g_free(copy);
  const double scale = unit != NULL ? unit->points : 1.0;
  const double answer = amount * scale;
  // "inf" and "nan", which strtod reads as numbers, and "1e308in", which is a number until its
  // unit is applied. A length that is not a number is not a length, and one that reached a page
  // size would make a sheet nothing could draw.
  if(!isfinite(answer)) return FALSE;
  *points = answer;
  if(unit_used != NULL) *unit_used = unit != NULL ? _canonical_name(unit) : NULL;
  return TRUE;
}

void dt_length_format(double points, const char *unit, int digits, char *out, size_t length)
{
  if(out == NULL || length == 0) return;
  const dt_length_unit_t *found = _lookup(unit);
  const int shown = digits >= 0 ? digits : (found != NULL ? found->digits : 0);
  if(found == NULL)
  {
    // No unit, so no unit written: the number IS the points.
    snprintf(out, length, "%.*f", shown, points);
    return;
  }
  snprintf(out, length, "%.*f %s", shown, points / found->points, _canonical_name(found));
}
