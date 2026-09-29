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

/*
 * The words of a text, measured once from the paragraph Pango shapes unwrapped.
 *
 * The one claim that carries the flow engine is that a planner adding the per-character
 * advances of the unwrapped paragraph breaks its lines exactly where Pango would, at every
 * measure. It is held here by SWEEPING the measure over six texts -- plain prose, French with
 * its guarded punctuation, tracked type, Hebrew, a Hebrew line with Latin words inside it, and
 * paragraphs -- and comparing the planner's line starts with the lines Pango sets at the same
 * width. One width cannot tell the two apart; a hundred and ninety can.
 */

#include "canvas/canvas_text_breaks.h"
#include "canvas/canvas_text_words.h"

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#define FIRST_MEASURE 30
#define LAST_MEASURE 600
#define MEASURE_STEP 3
#define MAX_LINES 256

static const char *PROSE
    = "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and the "
      "measure a line is set to belongs to the page rather than to the window looking at it.";

static const char *FRENCH
    = "Le prix : 12 % environ, soit 3 \xe2\x80\xb0 du total ; pourquoi ? Parce que \xc2\xab la mesure \xc2\xbb "
      "vaut 45 \xc2\xb0 et que la r\xc3\xa8gle : tenir la ponctuation avec son mot, vaut pour 100 % des lignes.";

static const char *HEBREW = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d, \xd7\x96\xd7\x94\xd7\x95 "
                            "\xd7\x9e\xd7\xa9\xd7\xa4\xd7\x98 \xd7\x91\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa \xd7\xa2\xd7\x9d "
                            "\xd7\x9b\xd7\x9e\xd7\x94 \xd7\x9e\xd7\x99\xd7\x9c\xd7\x99\xd7\x9d \xd7\xa9\xd7\xa0\xd7\xa9\xd7\x91"
                            "\xd7\xa8\xd7\x95\xd7\xaa \xd7\x9c\xd7\xa9\xd7\x95\xd7\xa8\xd7\x95\xd7\xaa.";

static const char *MIXED = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d Ansel \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d, \xd7\x96\xd7\x94\xd7\x95 "
                           "Pango \xd7\x9e\xd7\xa9\xd7\xa4\xd7\x98 \xd7\x91\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa \xd7\xa2\xd7\x9d "
                           "cairo \xd7\x95\xd7\x9e\xd7\x99\xd7\x9c\xd7\x99\xd7\x9d.";

static const char *PARAGRAPHS = "Alpha alpha alpha.\n\nBravo bravo bravo bravo, bravo.\nCharlie.";

static cairo_surface_t *_surface = NULL;
static cairo_t *_cr = NULL;

/** A layout the way the painter builds one: hinting off, a point per unit, WORD wrap. */
static PangoLayout *_layout(const char *text, const double width, const gboolean guards, const int tracking)
{
  PangoLayout *layout = pango_cairo_create_layout(_cr);
  PangoContext *context = pango_layout_get_context(layout);
  cairo_font_options_t *options = cairo_font_options_create();
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  pango_cairo_context_set_font_options(context, options);
  cairo_font_options_destroy(options);
  pango_cairo_context_set_resolution(context, 72.0);
  pango_layout_context_changed(layout);
  PangoFontDescription *font = pango_font_description_from_string("DejaVu Serif 14");
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
  pango_layout_set_text(layout, text, -1);
  PangoAttrList *attributes = pango_attr_list_new();
  if(guards) dt_canvas_text_guard_breaks(attributes, text, strlen(text));
  if(tracking != 0) pango_attr_list_insert(attributes, pango_attr_letter_spacing_new(tracking));
  pango_layout_set_attributes(layout, attributes);
  pango_attr_list_unref(attributes);
  pango_layout_set_width(layout, width > 0.0 ? (int)(width * PANGO_SCALE) : -1);
  return layout;
}

/** Where each of Pango's lines begins. */
static int _pango_starts(PangoLayout *layout, guint starts[MAX_LINES])
{
  int count = 0;
  const int lines = pango_layout_get_line_count(layout);
  for(int idx = 0; idx < lines && count < MAX_LINES; idx++)
    starts[count++] = (guint)pango_layout_get_line_readonly(layout, idx)->start_index;
  return count;
}

/** Where the planner's lines begin at one measure: a greedy fill, a word too wide set alone. */
static int _planned_starts(const dt_text_words_t *words, const int available, guint starts[MAX_LINES],
                           int spans[MAX_LINES])
{
  int count = 0;
  for(int idx = 0; idx < words->paragraph_count; idx++)
  {
    const dt_text_paragraph_t *paragraph = &words->paragraphs[idx];
    if(paragraph->word_count == 0)
    {
      if(count < MAX_LINES)
      {
        spans[count] = 0;
        starts[count++] = paragraph->start;
      }
      continue;
    }
    int cursor = 0;
    while(cursor < paragraph->word_count && count < MAX_LINES)
    {
      int past = dt_canvas_text_words_fit(paragraph, cursor, available, FALSE, FALSE);
      if(past == cursor) past = cursor + 1;
      spans[count] = dt_canvas_text_words_span(paragraph, cursor, past);
      starts[count++] = paragraph->words[cursor].start;
      cursor = past;
    }
  }
  return count;
}

/** Say where the two disagree, in enough detail to tell whose arithmetic is off. */
static void _explain(const char *label, const int measure, PangoLayout *wrapped, const char *text,
                     const guint pango[MAX_LINES], const int pango_count, const guint planned[MAX_LINES],
                     const int spans[MAX_LINES], const int planned_count)
{
  int at = 0;
  while(at < pango_count && at < planned_count && pango[at] == planned[at]) at++;
  const int show = MAX(at - 1, 0);
  const PangoLayoutLine *line = pango_layout_get_line_readonly(wrapped, show);
  PangoRectangle ink;
  PangoRectangle logical;
  pango_layout_line_get_extents((PangoLayoutLine *)line, &ink, &logical);
  const guint pango_end = show + 1 < pango_count ? pango[show + 1] : (guint)strlen(text);
  const guint planned_end = show + 1 < planned_count ? planned[show + 1] : (guint)strlen(text);
  gchar *pango_text = g_strndup(text + pango[show], pango_end - pango[show]);
  gchar *planned_text = g_strndup(text + planned[show], planned_end - planned[show]);
  print_message("%s at %d units (%d pango units): first divergence after line %d\n"
                "   pango   line %d: [%s] logical width %d\n"
                "   planner line %d: [%s] span %d\n",
                label, measure, measure * PANGO_SCALE, at, show, pango_text, logical.width, show, planned_text,
                spans[show]);
  g_free(pango_text);
  g_free(planned_text);
}

/** Sweep the measure and hold the planner's breaks to Pango's. */
static void _sweep(const char *label, const char *text, const gboolean guards, const int tracking)
{
  PangoLayout *unwrapped = _layout(text, -1.0, guards, tracking);
  dt_text_words_t words;
  assert_true(dt_canvas_text_words_build(&words, unwrapped));
  g_object_unref(unwrapped);
  int agreed = 0;
  for(int measure = FIRST_MEASURE; measure <= LAST_MEASURE; measure += MEASURE_STEP)
  {
    PangoLayout *wrapped = _layout(text, measure, guards, tracking);
    guint pango[MAX_LINES];
    guint planned[MAX_LINES];
    int spans[MAX_LINES];
    const int pango_count = _pango_starts(wrapped, pango);
    const int planned_count = _planned_starts(&words, (int)(measure * PANGO_SCALE), planned, spans);
    gboolean same = pango_count == planned_count;
    for(int idx = 0; idx < pango_count && same; idx++) same = pango[idx] == planned[idx];
    if(!same)
    {
      _explain(label, measure, wrapped, text, pango, pango_count, planned, spans, planned_count);
      g_object_unref(wrapped);
      fail_msg("%s at %d units: Pango sets %d lines, the planner %d", label, measure, pango_count, planned_count);
    }
    g_object_unref(wrapped);
    agreed++;
  }
  assert_true(agreed > 100);
  dt_canvas_text_words_free(&words);
}

static void _the_planner_breaks_where_pango_breaks(void **state)
{
  (void)state;
  _sweep("prose", PROSE, FALSE, 0);
  _sweep("french, guarded", FRENCH, TRUE, 0);
  _sweep("tracked", PROSE, FALSE, 2 * PANGO_SCALE);
  _sweep("hebrew", HEBREW, FALSE, 0);
  _sweep("mixed direction", MIXED, FALSE, 0);
  _sweep("paragraphs", PARAGRAPHS, FALSE, 0);
}

/** The width of the words [first, past) of one paragraph set alone, unwrapped. */
static int _alone(const dt_text_words_t *words, const dt_text_paragraph_t *paragraph, const int first,
                  const int past, const int tracking)
{
  const guint a = paragraph->words[first].start;
  const guint b = paragraph->words[past - 1].end;
  gchar *segment = g_strndup(words->plain + a, b - a);
  PangoLayout *layout = _layout(segment, -1.0, FALSE, tracking);
  PangoRectangle ink;
  PangoRectangle logical;
  pango_layout_line_get_extents(pango_layout_get_line_readonly(layout, 0), &ink, &logical);
  g_object_unref(layout);
  g_free(segment);
  return logical.width;
}

/**
 * A run of words measures what it measures set alone -- kerning inside it and all, in either
 * direction of writing, and across a change of direction, where the difference of two x positions
 * on the unwrapped line does NOT (measured 37 units read where the segment set alone is 75).
 */
static void _a_run_of_words_measures_as_it_is_set_alone(void **state)
{
  (void)state;
  const char *texts[] = { PROSE, HEBREW, MIXED };
  for(int which = 0; which < 3; which++)
  {
    PangoLayout *unwrapped = _layout(texts[which], -1.0, FALSE, 0);
    dt_text_words_t words;
    assert_true(dt_canvas_text_words_build(&words, unwrapped));
    g_object_unref(unwrapped);
    const dt_text_paragraph_t *paragraph = &words.paragraphs[0];
    assert_true(paragraph->word_count >= 5);
    for(int first = 0; first + 1 < paragraph->word_count; first += 2)
      for(int past = first + 1; past <= MIN(first + 4, paragraph->word_count); past++)
        assert_int_equal(dt_canvas_text_words_span(paragraph, first, past), _alone(&words, paragraph, first, past, 0));
    dt_canvas_text_words_free(&words);
  }
}

/**
 * With tracking on, the planner reads a run of words WIDER than the same words set alone -- a
 * line set alone gives some of its letter-spacing back at its ends, the unwrapped paragraph gives
 * it back only at its own -- never narrower, and never by more than one letter-spacing. Measured
 * here: half of one. Wider is the safe side: a fit decided on the planner's number cannot
 * overflow the stretch it was decided for.
 */
static void _tracking_is_counted_on_every_character(void **state)
{
  (void)state;
  const int tracking = 2 * PANGO_SCALE;
  PangoLayout *unwrapped = _layout(PROSE, -1.0, FALSE, tracking);
  dt_text_words_t words;
  assert_true(dt_canvas_text_words_build(&words, unwrapped));
  g_object_unref(unwrapped);
  const dt_text_paragraph_t *paragraph = &words.paragraphs[0];
  int narrowest = G_MAXINT;
  int widest = G_MININT;
  for(int first = 0; first + 3 < paragraph->word_count; first += 5)
  {
    const int surplus
        = dt_canvas_text_words_span(paragraph, first, first + 3) - _alone(&words, paragraph, first, first + 3, tracking);
    narrowest = MIN(narrowest, surplus);
    widest = MAX(widest, surplus);
  }
  assert_true(narrowest >= 0);
  assert_true(widest <= tracking);
  dt_canvas_text_words_free(&words);
}

static void _a_paragraph_is_one_of_pangos_lines(void **state)
{
  (void)state;
  PangoLayout *unwrapped = _layout(PARAGRAPHS, -1.0, FALSE, 0);
  dt_text_words_t words;
  assert_true(dt_canvas_text_words_build(&words, unwrapped));
  // "Alpha alpha alpha.", the blank line, "Bravo ... bravo.", "Charlie." -- a hard break is a
  // line of its own, as it is for Pango.
  assert_int_equal(words.paragraph_count, 4);
  assert_int_equal(words.paragraphs[0].word_count, 3);
  assert_int_equal(words.paragraphs[1].word_count, 0);
  assert_int_equal(words.paragraphs[1].length, 0);
  assert_int_equal(words.paragraphs[2].word_count, 5);
  assert_int_equal(words.paragraphs[3].word_count, 1);
  for(int idx = 0; idx < 4; idx++)
  {
    const PangoLayoutLine *line = pango_layout_get_line_readonly(unwrapped, idx);
    assert_int_equal(words.paragraphs[idx].start, (guint)line->start_index);
    assert_int_equal(words.paragraphs[idx].length, (guint)line->length);
    // A blank line still has a height: it is what separates two paragraphs on the page.
    assert_true(words.paragraphs[idx].height > 0);
  }
  // The last word of a paragraph has no space after it; the others carry the one they break on.
  const dt_text_paragraph_t *bravo = &words.paragraphs[2];
  assert_int_equal(bravo->words[bravo->word_count - 1].space_after, 0);
  assert_true(bravo->words[0].space_after > 0);
  assert_int_equal(bravo->words[0].next, bravo->words[1].start);
  dt_canvas_text_words_free(&words);
  g_object_unref(unwrapped);
}

/** No word of a guarded text opens on a mark the guard keeps with its number or its word. */
static void _the_guards_hold_in_the_words(void **state)
{
  (void)state;
  PangoLayout *unwrapped = _layout(FRENCH, -1.0, TRUE, 0);
  dt_text_words_t words;
  assert_true(dt_canvas_text_words_build(&words, unwrapped));
  g_object_unref(unwrapped);
  const dt_text_paragraph_t *paragraph = &words.paragraphs[0];
  for(int idx = 0; idx < paragraph->word_count; idx++)
    assert_false(dt_canvas_text_clings_before(g_utf8_get_char(words.plain + paragraph->words[idx].start)));
  dt_canvas_text_words_free(&words);
  // Unguarded, the per cent and the per mille DO open words: that is what the guard is for.
  PangoLayout *loose = _layout(FRENCH, -1.0, FALSE, 0);
  dt_text_words_t bare;
  assert_true(dt_canvas_text_words_build(&bare, loose));
  g_object_unref(loose);
  int stranded = 0;
  for(int idx = 0; idx < bare.paragraphs[0].word_count; idx++)
  {
    const gunichar opening = g_utf8_get_char(bare.plain + bare.paragraphs[0].words[idx].start);
    if(opening == '%' || opening == 0x2030 || opening == 0x00B0) stranded++;
  }
  assert_true(stranded > 0);
  dt_canvas_text_words_free(&bare);
}

/** The hang is the table's fraction of the character's own advance, at either end of a word. */
static void _a_word_hangs_by_the_tables_fraction(void **state)
{
  (void)state;
  assert_float_equal(dt_canvas_text_optical_hang(','), 0.35, 1e-9);
  assert_float_equal(dt_canvas_text_optical_hang('"'), 0.6, 1e-9);
  assert_float_equal(dt_canvas_text_optical_hang(0x2014), 0.5, 1e-9);
  assert_float_equal(dt_canvas_text_optical_hang('a'), 0.0, 1e-9);
  PangoLayout *unwrapped = _layout("\"Alpha, alpha\" alpha", -1.0, FALSE, 0);
  dt_text_words_t words;
  assert_true(dt_canvas_text_words_build(&words, unwrapped));
  g_object_unref(unwrapped);
  const dt_text_paragraph_t *paragraph = &words.paragraphs[0];
  assert_int_equal(paragraph->word_count, 3);
  assert_true(paragraph->words[0].lead > 0);  // opens on a quote
  assert_true(paragraph->words[0].room > 0);  // closes on a comma
  assert_int_equal(paragraph->words[1].lead, 0);
  assert_true(paragraph->words[1].room > 0);  // closes on a quote
  assert_int_equal(paragraph->words[2].lead, 0);
  assert_int_equal(paragraph->words[2].room, 0);
  // Given its room, a word fits a stretch that is short of it by that much; denied it, it does not.
  const int span = dt_canvas_text_words_span(paragraph, 0, 1);
  assert_int_equal(dt_canvas_text_words_fit(paragraph, 0, span - 1, FALSE, FALSE), 0);
  assert_int_equal(dt_canvas_text_words_fit(paragraph, 0, span - 1, FALSE, TRUE), 1);
  // The space it breaks on hangs: a stretch exactly its ink wide holds it.
  assert_int_equal(dt_canvas_text_words_fit(paragraph, 0, span, FALSE, FALSE), 1);
  // Nothing fits nothing; everything fits a mile.
  assert_int_equal(dt_canvas_text_words_fit(paragraph, 0, 1, FALSE, FALSE), 0);
  assert_int_equal(dt_canvas_text_words_fit(paragraph, 0, 1000 * PANGO_SCALE, FALSE, FALSE), 3);
  dt_canvas_text_words_free(&words);
}

static int _group_setup(void **state)
{
  (void)state;
  _surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
  _cr = cairo_create(_surface);
  return 0;
}

static int _group_teardown(void **state)
{
  (void)state;
  cairo_destroy(_cr);
  cairo_surface_destroy(_surface);
  return 0;
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_the_planner_breaks_where_pango_breaks),
    cmocka_unit_test(_a_run_of_words_measures_as_it_is_set_alone),
    cmocka_unit_test(_tracking_is_counted_on_every_character),
    cmocka_unit_test(_a_paragraph_is_one_of_pangos_lines),
    cmocka_unit_test(_the_guards_hold_in_the_words),
    cmocka_unit_test(_a_word_hangs_by_the_tables_fraction),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
