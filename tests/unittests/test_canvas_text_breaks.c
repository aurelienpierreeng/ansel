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

/* Where a line of type may not break.
 *
 * Judged on the LINES PANGO ACTUALLY SETS, over a sweep of measures, rather than on the attribute
 * list and rather than at one width: a guard that is present but ineffective would pass a test
 * that only read the list back, and a mark strands at some measures and not at others.
 *
 * What the sweep measured, and what this file therefore asserts. With the wrap at
 * PANGO_WRAP_WORD -- which is the painter's -- the family splits in two, and not where reading
 * the code alone would put the line:
 *
 * - The marks a space is set before, `;` `:` `!` `?` and the closing guillemets, are held by
 *   PANGO'S OWN UAX #14 at every measure of the sweep: they are classed IS, EX and CL, which
 *   forbid a break before them outright, so the guard is a no-op for all six.
 * - The signs that belong to the NUMBER in front of them -- per cent, degree, per mille -- are
 *   class PO, and a break after the space before them IS allowed: plain WORD strands them at 24,
 *   10 and 29 of the sweep's 91 measures respectively. Those three are what this file holds.
 *
 * So the first group is asserted as "the guard never loosens what Pango already holds" and the
 * second as "the guard is what holds it". One measure cannot tell the two apart -- at 45 points
 * the per mille strands and the per cent does not -- which is why every case sweeps.
 */

#include "canvas/canvas_text_breaks.h"

#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

/** The measures the sweep runs at, in points. Narrow enough to break a short phrase several ways. */
#define FIRST_MEASURE 30
#define LAST_MEASURE 120

/**
 * The lines `text` is set on at a measure of `width`, as a NULL-terminated array, each line's own
 * bytes and no others -- so a line that begins with a mark begins with that mark's bytes.
 */
static gchar **_set_lines(const char *text, const int width, const gboolean guarded)
{
  PangoFontMap *fonts = pango_cairo_font_map_get_default();
  PangoContext *context = pango_font_map_create_context(fonts);
  PangoLayout *layout = pango_layout_new(context);
  pango_layout_set_text(layout, text, -1);
  // WORD, as `_flow_plan()` and the plain paragraph painter both set it: a word too long for the
  // measure overflows rather than being cut in half at a place UAX #14 forbids.
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
  pango_layout_set_width(layout, width * PANGO_SCALE);
  if(guarded)
  {
    PangoAttrList *attributes = pango_attr_list_new();
    dt_canvas_text_guard_breaks(attributes, text, strlen(text));
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
  }

  GPtrArray *lines = g_ptr_array_new();
  PangoLayoutIter *iter = pango_layout_get_iter(layout);
  do
  {
    PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
    g_ptr_array_add(lines, g_strndup(text + line->start_index, (gsize)line->length));
  } while(pango_layout_iter_next_line(iter));
  pango_layout_iter_free(iter);
  g_ptr_array_add(lines, NULL);

  g_object_unref(layout);
  g_object_unref(context);
  return (gchar **)g_ptr_array_free(lines, FALSE);
}

/** Does any line begin with `mark`, i.e. is the mark stranded at the head of a line? */
static gboolean _a_line_begins_with(gchar **lines, const char *mark)
{
  for(gchar **line = lines; *line != NULL; line++)
    if(g_str_has_prefix(*line, mark)) return TRUE;
  return FALSE;
}

/** The lines as one string, for a failure message. */
static gchar *_as_one(gchar **lines)
{
  return g_strjoinv("|", lines);
}

static void _the_marks_a_space_is_set_before_are_named(void **state)
{
  (void)state;
  // What French sets a space before, and what belongs to the number in front of it.
  static const gunichar clinging[] = { ';', ':', '!', '?', 0x00BB, 0x203A, '%', 0x2030, 0x00B0 };
  for(size_t at = 0; at < G_N_ELEMENTS(clinging); at++) assert_true(dt_canvas_text_clings_before(clinging[at]));
  // And what does not: a full stop and a comma take no space before them, so they never strand.
  static const gunichar free_standing[] = { '.', ',', 'a', ' ', '(', 0x00AB, 0x2019 };
  for(size_t at = 0; at < G_N_ELEMENTS(free_standing); at++)
    assert_false(dt_canvas_text_clings_before(free_standing[at]));

  assert_true(dt_canvas_text_clings_after(0x00AB));
  assert_true(dt_canvas_text_clings_after(0x2039));
  assert_false(dt_canvas_text_clings_after(0x00BB));
  assert_false(dt_canvas_text_clings_after('('));
}

static void _a_sign_goes_down_with_its_number(void **state)
{
  (void)state;
  /*
   * The three marks this file is here for, and the reason it is not merely a statement of the
   * rule. MEASURED over the sweep: plain WORD sets "aaa 5 <per mille> bbb" as
   * "aaa 5 |<per mille> |bbb" at 29 of its 91 measures -- the sign alone on a line, under the
   * number it belongs to -- and the per cent and the degree strand at 24 and 10 of them.
   * Guarded, none of the three ever begins a line at any measure.
   */
  static const struct
  {
    const char *text;
    const char *mark;
  } signs[] = {
    { "aaa 50 % bbb", "%" },
    { "aaa 20 \xc2\xb0 bbb", "\xc2\xb0" },
    { "aaa 5 \xe2\x80\xb0 bbb", "\xe2\x80\xb0" },
  };
  for(size_t at = 0; at < G_N_ELEMENTS(signs); at++)
  {
    gboolean ever_stranded_loose = FALSE;
    for(int width = FIRST_MEASURE; width <= LAST_MEASURE; width++)
    {
      gchar **loose = _set_lines(signs[at].text, width, FALSE);
      gchar **held = _set_lines(signs[at].text, width, TRUE);
      if(_a_line_begins_with(loose, signs[at].mark)) ever_stranded_loose = TRUE;
      if(_a_line_begins_with(held, signs[at].mark))
      {
        gchar *shown = _as_one(held);
        print_error("at %d the guarded sign still began a line: %s\n", width, shown);
        g_free(shown);
      }
      assert_false(_a_line_begins_with(held, signs[at].mark));
      g_strfreev(loose);
      g_strfreev(held);
    }
    // And unguarded it does strand, somewhere in the sweep -- which is what makes it load-bearing.
    if(!ever_stranded_loose) print_error("\"%s\" never stranded unguarded either\n", signs[at].text);
    assert_true(ever_stranded_loose);
  }
}

static void _pango_already_holds_the_marks_and_the_guard_never_loosens_them(void **state)
{
  (void)state;
  /*
   * The six marks a space is set before: Pango's own line breaker refuses a break before every
   * one of them once the wrap is WORD -- they are classed IS, EX and CL -- so the guard is a
   * no-op for them at every measure of the sweep. That is a fact about PANGO rather than about
   * this file, and it is pinned here so a change to it is noticed rather than guessed at: the day
   * a case stops matching, this test says which mark and at what measure, and the guard already
   * written for it starts earning its place.
   */
  static const struct
  {
    const char *text;
    const char *mark;
  } marks[] = {
    { "aaa mot ; bbb", ";" },
    { "aaa mot : bbb", ":" },
    { "aaa mot ! bbb", "!" },
    { "aaa mot ? bbb", "?" },
    { "aaa mot \xc2\xbb bbb", "\xc2\xbb" },
    { "aaa mot \xe2\x80\xba bbb", "\xe2\x80\xba" },
  };
  for(size_t at = 0; at < G_N_ELEMENTS(marks); at++)
    for(int width = FIRST_MEASURE; width <= LAST_MEASURE; width++)
    {
      gchar **loose = _set_lines(marks[at].text, width, FALSE);
      gchar **held = _set_lines(marks[at].text, width, TRUE);
      // Neither way strands the mark, and the guard sets the same lines Pango already set.
      assert_false(_a_line_begins_with(loose, marks[at].mark));
      assert_false(_a_line_begins_with(held, marks[at].mark));
      gchar *loose_shown = _as_one(loose);
      gchar *held_shown = _as_one(held);
      if(g_strcmp0(loose_shown, held_shown) != 0)
        print_error("\"%s\" at %d: %s became %s\n", marks[at].text, width, loose_shown, held_shown);
      assert_string_equal(loose_shown, held_shown);
      g_free(loose_shown);
      g_free(held_shown);
      g_strfreev(loose);
      g_strfreev(held);
    }

  // An opening guillemet is the mirror case: it must never END a line, and Pango holds that too.
  for(int width = FIRST_MEASURE; width <= LAST_MEASURE; width++)
  {
    const char *text = "il dit \xc2\xab mot \xc2\xbb ensuite";
    gchar **held = _set_lines(text, width, TRUE);
    for(gchar **line = held; *line != NULL; line++)
    {
      // The guillemet is never the last thing on a line, whitespace aside.
      gchar *trimmed = g_strchomp(g_strdup(*line));
      if(g_str_has_suffix(trimmed, "\xc2\xab")) print_error("at %d a line ended on a guillemet: %s\n", width, trimmed);
      assert_false(g_str_has_suffix(trimmed, "\xc2\xab"));
      g_free(trimmed);
    }
    g_strfreev(held);
  }
}

static void _a_long_word_is_never_cut_in_half(void **state)
{
  (void)state;
  /*
   * The other half of the fix, and the half that was doing the damage. PANGO_WRAP_WORD_CHAR,
   * which the painter used to set, falls back to breaking ANYWHERE as soon as a word does not fit
   * -- at places UAX #14 forbids included -- so at a narrow measure it set "Le titre est long ;
   * il continue" as "long |; il |cont|inue": the semicolon at the head of a line and a word cut
   * in two with no hyphen. Under WORD the pair stays whole and a long word overflows the measure
   * instead, which is the accepted price.
   */
  gchar **set = _set_lines("un anticonstitutionnellement ici", 60, FALSE);
  gchar *shown = _as_one(set);
  assert_non_null(g_strstr_len(shown, -1, "anticonstitutionnellement"));
  g_free(shown);
  g_strfreev(set);

  set = _set_lines("Le titre est long ; il continue", 40, FALSE);
  assert_false(_a_line_begins_with(set, ";"));
  g_strfreev(set);
}

static void _a_guard_never_swallows_a_paragraph(void **state)
{
  (void)state;
  /*
   * `allow-breaks FALSE` also clears the MANDATORY break, so a guard reaching over a newline would
   * join two paragraphs into one -- every guard stops at one. Set at a measure wide enough to hold
   * either paragraph whole, the only break left is the newline's, so two lines means it survived.
   */
  const char *text = "premier ;\nsecond ; fin";
  gchar **held = _set_lines(text, 600, TRUE);
  assert_int_equal(g_strv_length(held), 2);
  assert_true(g_str_has_prefix(held[1], "second"));
  g_strfreev(held);

  // And a space at the very end of the text, with nothing after it to cling, guards nothing.
  gchar **trailing = _set_lines("mot ", 600, TRUE);
  assert_int_equal(g_strv_length(trailing), 1);
  g_strfreev(trailing);
}

static void _nothing_is_guarded_where_nothing_clings(void **state)
{
  (void)state;
  // Ordinary prose breaks exactly as it did: the rule costs a paragraph with no French spacing
  // nothing at all, at any measure.
  static const char *const plain[] = { "a plain sentence with several words in it",
                                       "commas, full stops. and nothing else", "" };
  for(size_t at = 0; at < G_N_ELEMENTS(plain); at++)
    for(int width = FIRST_MEASURE; width <= LAST_MEASURE; width++)
    {
      gchar **loose = _set_lines(plain[at], width, FALSE);
      gchar **held = _set_lines(plain[at], width, TRUE);
      gchar *loose_shown = _as_one(loose);
      gchar *held_shown = _as_one(held);
      assert_string_equal(loose_shown, held_shown);
      g_free(loose_shown);
      g_free(held_shown);
      g_strfreev(loose);
      g_strfreev(held);
    }

  // A NULL list or text is not a crash.
  dt_canvas_text_guard_breaks(NULL, "mot ;", 5);
  PangoAttrList *attributes = pango_attr_list_new();
  dt_canvas_text_guard_breaks(attributes, NULL, 5);
  dt_canvas_text_guard_breaks(attributes, "mot ;", 0);
  pango_attr_list_unref(attributes);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_the_marks_a_space_is_set_before_are_named),
    cmocka_unit_test(_a_sign_goes_down_with_its_number),
    cmocka_unit_test(_pango_already_holds_the_marks_and_the_guard_never_loosens_them),
    cmocka_unit_test(_a_long_word_is_never_cut_in_half),
    cmocka_unit_test(_a_guard_never_swallows_a_paragraph),
    cmocka_unit_test(_nothing_is_guarded_where_nothing_clings),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
