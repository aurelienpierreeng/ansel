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

#include "canvas/canvas_text_words.h"

#include "system/macros.h" // IS_NULL_PTR

#include <math.h>
#include <string.h>

/*
 * Whether the space a line breaks on is counted against the measure with the words before it.
 *
 * It is not: the space HANGS. UAX #14 places the break opportunity after the spaces, so the
 * obvious reading of Pango's breaker is that it adds them into the width it compares -- and
 * that reading is wrong, by measurement. Counted, the planner set one line MORE than Pango at
 * a 36-unit measure on plain prose; uncounted, it agrees with Pango on every line at every
 * measure of `test_canvas_text_words`'s sweep, all six texts. That sweep is the only argument
 * this constant needs, and the only thing that may change it.
 */
#define DT_TEXT_FIT_COUNTS_BREAK_SPACE 0

double dt_canvas_text_optical_hang(const gunichar character)
{
  switch(character)
  {
    case '"':
    case '\'':
    case 0x2018: // ' '
    case 0x2019:
    case 0x201C: // " "
    case 0x201D:
    case 0x00AB: // guillemets
    case 0x00BB:
    case 0x2039:
    case 0x203A:
      return 0.6;
    case '-':
    case 0x2013: // en and em dash
    case 0x2014:
      return 0.5;
    case '.':
    case ',':
    case ';':
    case ':':
      return 0.35;
    default:
      return 0.0;
  }
}

/** The letter-spacing attribute in force at one byte of the text, in Pango units; 0 for none. */
static int _letter_spacing_at(PangoAttrList *attributes, const guint byte)
{
  if(IS_NULL_PTR(attributes)) return 0;
  int spacing = 0;
  PangoAttrIterator *iterator = pango_attr_list_get_iterator(attributes);
  do
  {
    gint start = 0;
    gint end = 0;
    pango_attr_iterator_range(iterator, &start, &end);
    if((gint)byte < start || (gint)byte >= end) continue;
    const PangoAttribute *attribute = pango_attr_iterator_get(iterator, PANGO_ATTR_LETTER_SPACING);
    if(!IS_NULL_PTR(attribute)) spacing = ((const PangoAttrInt *)attribute)->value;
    break;
  } while(pango_attr_iterator_next(iterator));
  pango_attr_iterator_destroy(iterator);
  return spacing;
}

/**
 * Give a paragraph's two end characters the letter-spacing Pango's breaker counts on them.
 *
 * Tracking is added to every glyph when a run is shaped, half before and half after, and that
 * is the width Pango's breaker adds up. A FINISHED line then gives the outer halves back at its
 * two ends, so the paragraph shaped unwrapped -- one finished line -- reads half a spacing short
 * on its first character and half on its last. Measured: at a 228-unit measure with two units of
 * tracking, the planner's first line summed to 233472 Pango units, the measure to the unit, and
 * Pango refused it by the 1024 units this puts back. The split is Pango's own: half and half,
 * the left half rounded to whole units when the spacing is one.
 */
static void _restore_end_spacing(int *widths, PangoAttrList *attributes, const glong first, const glong past,
                                 const guint first_byte, const guint last_byte)
{
  if(past <= first) return;
  const int opening = _letter_spacing_at(attributes, first_byte);
  if(opening != 0)
  {
    int left = opening / 2;
    if((opening & (PANGO_SCALE - 1)) == 0) left = PANGO_UNITS_ROUND(left);
    widths[first] += left;
  }
  const int closing = _letter_spacing_at(attributes, last_byte);
  if(closing != 0)
  {
    int left = closing / 2;
    if((closing & (PANGO_SCALE - 1)) == 0) left = PANGO_UNITS_ROUND(left);
    widths[past - 1] += closing - left;
  }
}

/** The advance of the characters [first, past), from the per-character table. */
static int _advance(const int *widths, const glong first, const glong past)
{
  int total = 0;
  for(glong at = first; at < past; at++) total += widths[at];
  return total;
}

/** Cut one of Pango's lines into words at its break opportunities. */
static void _paragraph_words(dt_text_paragraph_t *paragraph, const char *text, const PangoLogAttr *attrs,
                             const int *widths, const glong *char_of, const guint *byte_of)
{
  const glong first = char_of[paragraph->start];
  const glong past = char_of[paragraph->start + paragraph->length];
  // Upper bound on the word count: one per break opportunity, plus the opening one.
  int capacity = 1;
  for(glong at = first + 1; at < past; at++)
    if(attrs[at].is_line_break) capacity++;
  paragraph->words = g_new0(dt_text_word_t, capacity);
  paragraph->word_count = 0;

  glong unit_start = first;
  for(glong at = first + 1; at <= past; at++)
  {
    if(at < past && !attrs[at].is_line_break) continue;
    // The unit [unit_start, at): its ink, then the whitespace the break eats.
    glong ink_end = at;
    while(ink_end > unit_start && attrs[ink_end - 1].is_white) ink_end--;
    if(ink_end == unit_start)
    {
      // Whitespace alone. After a word it is that word's own trailing space; before any word it
      // is the paragraph's leading indentation, which the flow does not set.
      if(paragraph->word_count > 0)
      {
        dt_text_word_t *previous = &paragraph->words[paragraph->word_count - 1];
        previous->space_after += _advance(widths, unit_start, at);
        previous->next = byte_of[at];
      }
      unit_start = at;
      continue;
    }
    dt_text_word_t *word = &paragraph->words[paragraph->word_count++];
    word->start = byte_of[unit_start];
    word->end = byte_of[ink_end];
    word->next = byte_of[at];
    word->width = _advance(widths, unit_start, ink_end);
    word->space_after = _advance(widths, ink_end, at);
    word->characters = ink_end - unit_start;
    const gunichar opening = g_utf8_get_char(text + word->start);
    const gunichar closing = g_utf8_get_char(g_utf8_prev_char(text + word->end));
    word->lead = (int)lround(dt_canvas_text_optical_hang(opening) * widths[unit_start]);
    word->room = (int)lround(dt_canvas_text_optical_hang(closing) * widths[ink_end - 1]);
    unit_start = at;
  }

  paragraph->cumulative = g_new0(int, paragraph->word_count + 1);
  for(int idx = 0; idx < paragraph->word_count; idx++)
    paragraph->cumulative[idx + 1]
        = paragraph->cumulative[idx] + paragraph->words[idx].width + paragraph->words[idx].space_after;
}

gboolean dt_canvas_text_words_build(dt_text_words_t *words, PangoLayout *unwrapped)
{
  if(IS_NULL_PTR(words)) return FALSE;
  memset(words, 0, sizeof(*words));
  if(IS_NULL_PTR(unwrapped)) return FALSE;
  const char *text = pango_layout_get_text(unwrapped);
  if(IS_NULL_PTR(text)) return FALSE;
  const gsize length = strlen(text);
  const glong char_count = g_utf8_strlen(text, (gssize)length);
  int attr_count = 0;
  const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(unwrapped, &attr_count);
  // One attribute per character plus one past the end, whatever the text: anything else is a
  // layout this code does not understand.
  if(IS_NULL_PTR(attrs) || attr_count < char_count + 1) return FALSE;

  // The attributes are per CHARACTER and the words are byte ranges: both tables, once.
  glong *char_of = g_new0(glong, length + 1);
  guint *byte_of = g_new0(guint, char_count + 1);
  {
    glong character = 0;
    for(const char *at = text; *at != '\0'; character++)
    {
      const char *following = g_utf8_next_char(at);
      byte_of[character] = (guint)(at - text);
      for(const char *byte = at; byte < following; byte++) char_of[byte - text] = character;
      at = following;
    }
    byte_of[char_count] = (guint)length;
    char_of[length] = char_count;
  }

  // The advance of every character as shaped in its own paragraph -- the numbers Pango's breaker
  // adds up. A run's glyphs are in visual order; the call puts the widths back in logical order.
  int *widths = g_new0(int, char_count + 1);
  const int line_count = pango_layout_get_line_count(unwrapped);
  words->paragraphs = g_new0(dt_text_paragraph_t, MAX(line_count, 1));
  for(int idx = 0; idx < line_count; idx++)
  {
    PangoLayoutLine *line = pango_layout_get_line_readonly(unwrapped, idx);
    if(IS_NULL_PTR(line)) continue;
    dt_text_paragraph_t *paragraph = &words->paragraphs[words->paragraph_count++];
    paragraph->start = (guint)line->start_index;
    paragraph->length = (guint)line->length;
    for(GSList *node = line->runs; !IS_NULL_PTR(node); node = node->next)
    {
      const PangoLayoutRun *run = node->data;
      if(IS_NULL_PTR(run) || IS_NULL_PTR(run->item) || IS_NULL_PTR(run->glyphs)) continue;
      const PangoItem *item = run->item;
      pango_glyph_string_get_logical_widths(run->glyphs, text + item->offset, item->length, item->analysis.level,
                                            widths + char_of[item->offset]);
    }
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_line_get_extents(line, &ink, &logical);
    paragraph->height = logical.height;
    paragraph->ascent = -logical.y;
    paragraph->ink_top = ink.y - logical.y;
    paragraph->ink_height = ink.height;
#if PANGO_VERSION_CHECK(1, 50, 0)
    paragraph->direction = pango_layout_line_get_resolved_direction(line);
#else
    paragraph->direction = pango_find_base_dir(text + paragraph->start, (gint)paragraph->length);
#endif
    if(paragraph->length > 0)
    {
      const guint last_byte = (guint)(g_utf8_prev_char(text + paragraph->start + paragraph->length) - text);
      _restore_end_spacing(widths, pango_layout_get_attributes(unwrapped), char_of[paragraph->start],
                           char_of[paragraph->start + paragraph->length], paragraph->start, last_byte);
      _paragraph_words(paragraph, text, attrs, widths, char_of, byte_of);
    }
    else
      paragraph->cumulative = g_new0(int, 1);
  }
  g_free(widths);
  g_free(byte_of);
  g_free(char_of);

  words->plain = text;
  words->length = length;
  words->unwrapped = g_object_ref(unwrapped);
  words->leading = pango_layout_get_spacing(unwrapped);
  return TRUE;
}

void dt_canvas_text_words_free(dt_text_words_t *words)
{
  if(IS_NULL_PTR(words)) return;
  for(int idx = 0; idx < words->paragraph_count; idx++)
  {
    g_free(words->paragraphs[idx].words);
    g_free(words->paragraphs[idx].cumulative);
  }
  g_free(words->paragraphs);
  if(!IS_NULL_PTR(words->unwrapped)) g_object_unref(words->unwrapped);
  memset(words, 0, sizeof(*words));
}

int dt_canvas_text_words_span(const dt_text_paragraph_t *paragraph, const int first, const int past)
{
  if(IS_NULL_PTR(paragraph) || first < 0 || past <= first || past > paragraph->word_count) return 0;
  return paragraph->cumulative[past] - paragraph->cumulative[first] - paragraph->words[past - 1].space_after;
}

int dt_canvas_text_words_fit(const dt_text_paragraph_t *paragraph, const int cursor, const int available,
                             const gboolean hang_lead, const gboolean hang_room)
{
  if(IS_NULL_PTR(paragraph) || cursor < 0 || cursor >= paragraph->word_count) return cursor;
  const int lead = hang_lead ? paragraph->words[cursor].lead : 0;
  int fitted = cursor;
  for(int past = cursor + 1; past <= paragraph->word_count; past++)
  {
    const dt_text_word_t *last = &paragraph->words[past - 1];
    int needed = dt_canvas_text_words_span(paragraph, cursor, past);
#if DT_TEXT_FIT_COUNTS_BREAK_SPACE
    needed += last->space_after;
#endif
    const int room = hang_room ? last->room : 0;
    if(needed > available + lead + room) break;
    fitted = past;
  }
  return fitted;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
