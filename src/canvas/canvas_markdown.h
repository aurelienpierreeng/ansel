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

#ifndef DT_CANVAS_CANVAS_MARKDOWN_H
#define DT_CANVAS_CANVAS_MARKDOWN_H

/**
 * @file canvas_markdown.h
 * @brief Markdown to Pango markup, for text frames drawn with PangoCairo.
 *
 * @details A text frame is painted, not laid out in a GtkTextView, so it needs markup
 * Pango understands. This converter covers the subset a caption, a note or a sidecar
 * text uses: ATX headings (`#` to `###`), paragraphs separated by blank lines, `*` / `-`
 * bullet lists, `1.` numbered lists, `**bold**`, `*italic*` / `_italic_`, `` `code` ``,
 * `~~strike~~`, `[text](url)` (the text is kept, the URL dropped), horizontal rules and
 * hard line breaks. Everything else is text, escaped. It never fails: unbalanced markers
 * are printed literally.
 *
 * No Markdown library is a hard dependency of the application (cmark is optional and
 * only libs/textnotes.c uses it), and a text frame must render on every build.
 */

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert Markdown to Pango markup.
 * @param markdown the source, UTF-8. NULL is an empty document.
 * @return newly allocated markup the caller frees with g_free(). Never NULL.
 */
gchar *dt_canvas_markdown_to_pango(const gchar *markdown);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_MARKDOWN_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
