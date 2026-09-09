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

#include "canvas/canvas_markdown.h"

#include <glib.h>
#include <pango/pango.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

static void _assert_markup(const char *markdown, const char *expected)
{
  gchar *markup = dt_canvas_markdown_to_pango(markdown);
  assert_string_equal(markup, expected);
  // Whatever comes out must be markup Pango accepts.
  GError *error = NULL;
  assert_true(pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, &error));
  assert_null(error);
  g_free(markup);
}

static void _inline_spans(void **state)
{
  (void)state;
  _assert_markup("plain", "plain");
  _assert_markup("**bold** and *italic* and _also_ and `code` and ~~gone~~",
                 "<b>bold</b> and <i>italic</i> and <i>also</i> and <tt>code</tt> and <s>gone</s>");
  _assert_markup("nested **bold *italic* bold**", "nested <b>bold <i>italic</i> bold</b>");
  _assert_markup("a [link](http://x) here", "a <u>link</u> here");
}

static void _unbalanced_markers_and_markup_characters_print_literally(void **state)
{
  (void)state;
  _assert_markup("2 * 3 * 4", "2 * 3 * 4");
  _assert_markup("unclosed **bold", "unclosed **bold");
  _assert_markup("a < b & c > d", "a &lt; b &amp; c &gt; d");
  _assert_markup("escaped \\*star\\*", "escaped *star*");
  _assert_markup("`<tag>`", "<tt>&lt;tag&gt;</tt>");
}

static void _blocks(void **state)
{
  (void)state;
  _assert_markup("# Title\n\nBody line one\nline two\n\nNext paragraph",
                 "<span size=\"xx-large\" weight=\"bold\">Title</span>\n\nBody line one line two\n\nNext paragraph");
  _assert_markup("## Sub\n- one\n- two\n\n1. first\n2. second",
                 "<span size=\"x-large\" weight=\"bold\">Sub</span>\n\n  •  one\n  •  two\n\n  1.  first\n  2.  second");
  _assert_markup("before\n\n---\n\nafter", "before\n\n<span alpha=\"50%\">────────</span>\n\nafter");
  _assert_markup("hard  \nbreak", "hard\nbreak");
  _assert_markup("windows\r\nlines", "windows lines");
}

static void _empty_input(void **state)
{
  (void)state;
  _assert_markup(NULL, "");
  _assert_markup("", "");
  _assert_markup("\n\n\n", "");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_inline_spans),
    cmocka_unit_test(_unbalanced_markers_and_markup_characters_print_literally),
    cmocka_unit_test(_blocks),
    cmocka_unit_test(_empty_input),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
