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
 * @file canvas/canvas_text_breaks.h
 * @brief Where a line of type may NOT break: punctuation that must stay with its word.
 *
 * French, and every typography descended from it, sets a space before `;` `:` `!` `?` and inside
 * guillemets, and every typography sets one before a per cent, a degree or a per mille. That
 * space is a space to a line breaker, so a paragraph set to a narrow measure can strand the mark
 * at the start of the next line -- a lone per mille under the number it belongs to.
 *
 * MEASURED (`test_canvas_text_breaks`, sweeping 91 measures per case): with the wrap at
 * PANGO_WRAP_WORD, Pango's own UAX #14 already refuses a break before the PUNCTUATION -- `;` `:`
 * `!` `?` and the closing guillemets are classed IS, EX and CL -- so this rule changes nothing
 * for those six. It is load-bearing for the three signs that belong to a NUMBER: per cent, degree
 * and per mille are class PO, a break after the space before them is allowed, and plain WORD
 * strands them at 24, 10 and 29 of the sweep's measures respectively.
 *
 * The punctuation is kept in the table regardless, so the rule is stated once and in full, and so
 * that the day a Pango release loosens one of those classes the guard is already written -- the
 * test pins Pango's behaviour rather than assuming it.
 *
 * The rule is expressed as PANGO ATTRIBUTES over byte ranges and never as a rewrite of the
 * text. The flow engine's own attribute list is byte-indexed into the plain text and shifted
 * per chunk, and the optical-margin walk-back counts bytes too, so substituting a no-break
 * space for a space -- two bytes where there was one -- would move every index in it.
 *
 * No GTK and no configuration here, so the rule is testable without a painter.
 */

#ifndef DT_CANVAS_CANVAS_TEXT_BREAKS_H
#define DT_CANVAS_CANVAS_TEXT_BREAKS_H

#include <glib.h>
#include <pango/pango.h>

/**
 * @brief Must this character never BEGIN a line?
 *
 * The punctuation a space is set before, plus the signs that belong to the number in front of
 * them. Shared with the optical margins, which hang the same family.
 */
gboolean dt_canvas_text_clings_before(gunichar character);

/** @brief Must this character never END a line? An opening guillemet, which owns the space after it. */
gboolean dt_canvas_text_clings_after(gunichar character);

/**
 * @brief Forbid a break wherever punctuation must stay with its word.
 *
 * Adds `allow-breaks FALSE` over the byte ranges that would otherwise strand a mark:
 * - a space followed by a clinging character, from the SPACE through the clinging run;
 * - an opening guillemet followed by a space, from the guillemet through the space.
 *
 * Every guard starts at a space or at the mark that owns one, so the break opportunity BEFORE
 * the preceding word stays open and the pair moves down to the next line together rather than
 * overflowing.
 *
 * Guards stop at every newline: `allow-breaks FALSE` also clears the mandatory break, so a
 * guard spanning one would swallow a paragraph.
 *
 * @param attributes the list to add to; nothing is done if it is NULL.
 * @param plain the layout's PLAIN text, which the byte ranges index into.
 * @param length its length in bytes.
 */
void dt_canvas_text_guard_breaks(PangoAttrList *attributes, const char *plain, gsize length);

#endif // DT_CANVAS_CANVAS_TEXT_BREAKS_H
