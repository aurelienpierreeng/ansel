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
 * @file canvas/canvas_text_words.h
 * @brief The words of a text, measured once, from the paragraphs Pango shapes unwrapped.
 *
 * A text that flows around pictures is set line by line at widths this code chooses, and the
 * question every line asks is "how many of the next words fit this stretch?". Asking Pango to set
 * a line and reading its width back cannot answer it: a justified line is stretched to its
 * measure, a hanging comma is set past it, and neither says whether the FIRST word fitted. So the
 * words are measured before anything is asked. The whole text is shaped once with no wrapping,
 * one Pango line per paragraph, and each paragraph is cut into words at Pango's own break
 * opportunities, each word carrying the advance of its characters AS SHAPED IN ITS PARAGRAPH.
 *
 * MEASURED (Pango 1.56, DejaVu Serif 14, hinting off): a run of words summed this way equals the
 * same bytes set alone to the Pango unit, kerning pairs included; with tracking on it is wider by
 * exactly one letter-spacing (a line set alone drops half of it at each end), which errs on the
 * side of fitting less; and it holds across a change of direction, where the difference of two
 * x positions does not (37 units read where a Hebrew-and-Latin segment set alone is 75). The
 * per-character advances are what Pango's own line breaker adds up, so a fill that adds the same
 * integers under the same rule breaks where Pango breaks -- `test_canvas_text_words` sweeps the
 * measures to hold that.
 *
 * Everything here is in PANGO UNITS and byte offsets into the layout's own text. The module knows
 * nothing of the canvas, the obstacles or cairo.
 */

#ifndef DT_CANVAS_CANVAS_TEXT_WORDS_H
#define DT_CANVAS_CANVAS_TEXT_WORDS_H

#include <glib.h>
#include <pango/pango.h>

/** One word of a paragraph: the ink between two break opportunities, and the space after it. */
typedef struct dt_text_word_t
{
  guint start;      ///< first byte of the word
  guint end;        ///< past its last non-white byte
  guint next;       ///< first byte of the following word, or the paragraph's end
  int width;        ///< advance of [start, end) as shaped in its paragraph
  int space_after;  ///< advance of the whitespace [end, next): 0 for the paragraph's last word
  int lead;         ///< how far its first character may hang out of the measure
  int room;         ///< how far its last character may hang out of the measure
  glong characters; ///< of [start, end)
} dt_text_word_t;

/** One of Pango's lines of the unwrapped text: a paragraph, a hard-break line, or a blank one. */
typedef struct dt_text_paragraph_t
{
  guint start;             ///< first byte, into the layout's text
  guint length;            ///< without the newline that ends it
  dt_text_word_t *words;
  int word_count;          ///< 0: the blank line between two paragraphs, or a line of spaces
  int *cumulative;         ///< cumulative[k] = sum over m < k of (width_m + space_after_m); word_count + 1 entries
  int height;              ///< logical height of the line
  int ascent;              ///< the baseline below the logical top
  int ink_top;             ///< the first ink below the logical top
  int ink_height;
  PangoDirection direction;
} dt_text_paragraph_t;

/** The measured text: the layout it was shaped in, and its paragraphs. */
typedef struct dt_text_words_t
{
  const char *plain;       ///< the layout's own text, borrowed
  gsize length;
  PangoLayout *unwrapped;  ///< referenced, so `plain` and the lines stay valid
  dt_text_paragraph_t *paragraphs;
  int paragraph_count;
  int leading;             ///< the layout's spacing between lines
} dt_text_words_t;

/**
 * How far a character may hang outside the measured edge, as a fraction of its own advance.
 *
 * A quote is nearly all white space and hangs almost whole; a full stop hangs a little. The point
 * of it is that a column's edge is read from the STEMS, and a line beginning with a quote looks
 * indented when its box is flush.
 */
double dt_canvas_text_optical_hang(gunichar character);

/**
 * Measure the words of `unwrapped`, a layout holding the whole text at width -1.
 *
 * The layout must carry its final text and attributes (the break guards included), since the
 * break opportunities and the advances are read from it. It is referenced, not copied.
 * @return FALSE for a NULL or empty layout; `words` is then zeroed and needs no freeing.
 */
gboolean dt_canvas_text_words_build(dt_text_words_t *words, PangoLayout *unwrapped);

void dt_canvas_text_words_free(dt_text_words_t *words);

/**
 * The advance of the words [first, past) set on one line: their inks and the spaces BETWEEN them,
 * never the space after the last one.
 */
int dt_canvas_text_words_span(const dt_text_paragraph_t *paragraph, int first, int past);

/**
 * How many of the words from `cursor` on fit a stretch `available` wide.
 *
 * The largest `past` such that the words [cursor, past) fit -- their inks and the spaces between
 * them; the space the line breaks on hangs, as it does for Pango's own breaker, and
 * `test_canvas_text_words` holds the two to the same breaks over a sweep of measures. With
 * `hang_lead` the first word's opening
 * character may hang out of the measure by its `lead`; with `hang_room` the last word's closing
 * character may hang by its `room`: that is what optical margins are.
 * @return `cursor` when not even one word fits.
 */
int dt_canvas_text_words_fit(const dt_text_paragraph_t *paragraph, int cursor, int available,
                             gboolean hang_lead, gboolean hang_room);

#endif // DT_CANVAS_CANVAS_TEXT_WORDS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
