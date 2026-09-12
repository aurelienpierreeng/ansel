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

#include "system/macros.h"
#include "system/mem_alloc.h"

#include <string.h>

typedef enum dt_md_block_t
{
  DT_MD_BLOCK_NONE = 0,
  DT_MD_BLOCK_PARAGRAPH,
  DT_MD_BLOCK_LIST,
} dt_md_block_t;

typedef struct dt_md_state_t
{
  GString *out;
  dt_md_block_t block;
  gboolean need_separator; ///< a blank line is owed before the next block
  gboolean after_break;    ///< the paragraph's last line ended with a hard break
} dt_md_state_t;

/* --- inline ----------------------------------------------------------------- */

static void _append_escaped(GString *out, const char *text, const gsize length)
{
  gchar *escaped = g_markup_escape_text(text, (gssize)length);
  g_string_append(out, escaped);
  dt_free(escaped);
}

/** Find the closing `marker` after `start`, not preceded by a backslash. -1 when absent. */
static gssize _find_closing(const char *text, const gsize start, const char *marker)
{
  const gsize marker_len = strlen(marker);
  const gsize length = strlen(text);
  for(gsize pos = start; pos + marker_len <= length; pos++)
  {
    if(text[pos] == '\\')
    {
      pos++;
      continue;
    }
    if(strncmp(text + pos, marker, marker_len) == 0 && pos > start) return (gssize)pos;
  }
  return -1;
}

static void _render_inline(GString *out, const char *text, const gsize length);

static gboolean _try_span(GString *out, const char *text, const gsize length, gsize *pos, const char *marker,
                          const char *open_tag, const char *close_tag, const gboolean literal_inner)
{
  const gsize marker_len = strlen(marker);
  if(*pos + marker_len >= length || strncmp(text + *pos, marker, marker_len) != 0) return FALSE;
  // The opener must be followed by a non-space, or it is a literal star.
  const char next = text[*pos + marker_len];
  if(next == ' ' || next == '\n' || next == '\0') return FALSE;
  gchar *scoped = g_strndup(text, length);
  const gssize close = _find_closing(scoped, *pos + marker_len, marker);
  dt_free(scoped);
  if(close < 0) return FALSE;
  const gsize inner_start = *pos + marker_len;
  const gsize inner_length = (gsize)close - inner_start;
  if(inner_length == 0) return FALSE;
  g_string_append(out, open_tag);
  if(literal_inner)
    _append_escaped(out, text + inner_start, inner_length);
  else
    _render_inline(out, text + inner_start, inner_length);
  g_string_append(out, close_tag);
  *pos = (gsize)close + marker_len;
  return TRUE;
}

static gboolean _try_link(GString *out, const char *text, const gsize length, gsize *pos)
{
  if(text[*pos] != '[') return FALSE;
  gsize close_bracket = *pos + 1;
  while(close_bracket < length && text[close_bracket] != ']' && text[close_bracket] != '\n') close_bracket++;
  if(close_bracket >= length || text[close_bracket] != ']') return FALSE;
  if(close_bracket + 1 >= length || text[close_bracket + 1] != '(') return FALSE;
  gsize close_paren = close_bracket + 2;
  while(close_paren < length && text[close_paren] != ')' && text[close_paren] != '\n') close_paren++;
  if(close_paren >= length || text[close_paren] != ')') return FALSE;
  g_string_append(out, "<u>");
  _render_inline(out, text + *pos + 1, close_bracket - *pos - 1);
  g_string_append(out, "</u>");
  *pos = close_paren + 1;
  return TRUE;
}

static void _render_inline(GString *out, const char *text, const gsize length)
{
  gsize pos = 0;
  gsize literal_start = 0;
  while(pos < length)
  {
    const char current = text[pos];
    gboolean consumed = FALSE;
    if(current == '\\' && pos + 1 < length)
    {
      // Escaped punctuation prints literally.
      _append_escaped(out, text + literal_start, pos - literal_start);
      _append_escaped(out, text + pos + 1, 1);
      pos += 2;
      literal_start = pos;
      continue;
    }
    if(current == '`' || current == '*' || current == '_' || current == '~' || current == '[')
    {
      _append_escaped(out, text + literal_start, pos - literal_start);
      gsize span_pos = pos;
      if(current == '`') consumed = _try_span(out, text, length, &span_pos, "`", "<tt>", "</tt>", TRUE);
      else if(current == '*')
      {
        consumed = _try_span(out, text, length, &span_pos, "**", "<b>", "</b>", FALSE)
                   || _try_span(out, text, length, &span_pos, "*", "<i>", "</i>", FALSE);
      }
      else if(current == '_')
      {
        consumed = _try_span(out, text, length, &span_pos, "__", "<b>", "</b>", FALSE)
                   || _try_span(out, text, length, &span_pos, "_", "<i>", "</i>", FALSE);
      }
      else if(current == '~') consumed = _try_span(out, text, length, &span_pos, "~~", "<s>", "</s>", FALSE);
      else if(current == '[') consumed = _try_link(out, text, length, &span_pos);
      if(consumed)
      {
        pos = span_pos;
        literal_start = pos;
        continue;
      }
      // Not a span after all: the marker itself is text.
      _append_escaped(out, text + pos, 1);
      pos++;
      literal_start = pos;
      continue;
    }
    pos++;
  }
  _append_escaped(out, text + literal_start, length - literal_start);
}

/* --- blocks ----------------------------------------------------------------- */

static void _close_block(dt_md_state_t *state)
{
  if(state->block != DT_MD_BLOCK_NONE) state->need_separator = TRUE;
  state->block = DT_MD_BLOCK_NONE;
}

static void _open_block(dt_md_state_t *state, const dt_md_block_t block)
{
  if(state->block == block && block == DT_MD_BLOCK_LIST)
  {
    g_string_append_c(state->out, '\n');
    return;
  }
  if(state->block == DT_MD_BLOCK_PARAGRAPH && block == DT_MD_BLOCK_PARAGRAPH)
  {
    // Consecutive lines of one paragraph are joined with a space, like Markdown does,
    // unless the previous line ended with a hard break, which already emitted its newline.
    if(!state->after_break) g_string_append_c(state->out, ' ');
    state->after_break = FALSE;
    return;
  }
  _close_block(state);
  if(state->out->len > 0)
  {
    g_string_append(state->out, state->need_separator ? "\n\n" : "\n");
  }
  state->need_separator = FALSE;
  state->after_break = FALSE;
  state->block = block;
}

static const char *_skip_spaces(const char *text)
{
  while(*text == ' ' || *text == '\t') text++;
  return text;
}

static gboolean _is_rule(const char *line)
{
  const char *cursor = _skip_spaces(line);
  const char marker = *cursor;
  if(marker != '-' && marker != '*' && marker != '_') return FALSE;
  int count = 0;
  for(; *cursor != '\0'; cursor++)
  {
    if(*cursor == marker) count++;
    else if(*cursor != ' ') return FALSE;
  }
  return count >= 3;
}

static int _heading_level(const char *line, const char **content)
{
  int level = 0;
  while(line[level] == '#' && level < 6) level++;
  if(level == 0 || (line[level] != ' ' && line[level] != '\0')) return 0;
  *content = _skip_spaces(line + level);
  return level;
}

static gboolean _bullet_item(const char *line, const char **content)
{
  const char *cursor = _skip_spaces(line);
  if((cursor[0] == '-' || cursor[0] == '*' || cursor[0] == '+') && cursor[1] == ' ')
  {
    *content = _skip_spaces(cursor + 2);
    return TRUE;
  }
  return FALSE;
}

static gboolean _numbered_item(const char *line, const char **content, int *number)
{
  const char *cursor = _skip_spaces(line);
  int digits = 0;
  int value = 0;
  while(cursor[digits] >= '0' && cursor[digits] <= '9' && digits < 9)
  {
    value = value * 10 + (cursor[digits] - '0');
    digits++;
  }
  if(digits == 0 || cursor[digits] != '.' || cursor[digits + 1] != ' ') return FALSE;
  *content = _skip_spaces(cursor + digits + 2);
  *number = value;
  return TRUE;
}

static void _render_line(dt_md_state_t *state, const char *line)
{
  const char *content = NULL;
  int number = 0;
  const gsize line_length = strlen(line);
  const gboolean hard_break = line_length >= 2 && line[line_length - 1] == ' ' && line[line_length - 2] == ' ';

  if(_skip_spaces(line)[0] == '\0')
  {
    _close_block(state);
    return;
  }
  if(_is_rule(line))
  {
    _open_block(state, DT_MD_BLOCK_PARAGRAPH);
    g_string_append(state->out, "<span alpha=\"50%\">────────</span>");
    _close_block(state);
    return;
  }
  const int level = _heading_level(line, &content);
  if(level > 0)
  {
    static const char *sizes[] = { "xx-large", "x-large", "large", "medium", "medium", "medium" };
    _open_block(state, DT_MD_BLOCK_PARAGRAPH);
    g_string_append_printf(state->out, "<span size=\"%s\" weight=\"bold\">", sizes[level - 1]);
    _render_inline(state->out, content, strlen(content));
    g_string_append(state->out, "</span>");
    _close_block(state);
    return;
  }
  if(_bullet_item(line, &content))
  {
    _open_block(state, DT_MD_BLOCK_LIST);
    g_string_append(state->out, "  •  ");
    _render_inline(state->out, content, strlen(content));
    return;
  }
  if(_numbered_item(line, &content, &number))
  {
    _open_block(state, DT_MD_BLOCK_LIST);
    g_string_append_printf(state->out, "  %d.  ", number);
    _render_inline(state->out, content, strlen(content));
    return;
  }
  const char *text = _skip_spaces(line);
  const gsize text_length = strlen(text) - (hard_break ? 2 : 0);
  _open_block(state, DT_MD_BLOCK_PARAGRAPH);
  _render_inline(state->out, text, text_length);
  if(hard_break)
  {
    // The next line continues the paragraph after the break, not after a joining space.
    g_string_append_c(state->out, '\n');
    state->after_break = TRUE;
  }
}

gchar *dt_canvas_markdown_to_pango(const gchar *markdown)
{
  dt_md_state_t state;
  state.out = g_string_new("");
  state.block = DT_MD_BLOCK_NONE;
  state.need_separator = FALSE;
  state.after_break = FALSE;
  if(IS_NULL_PTR(markdown)) return g_string_free(state.out, FALSE);

  gchar **lines = g_strsplit(markdown, "\n", -1);
  for(gchar **line = lines; !IS_NULL_PTR(*line); line++)
  {
    // A CR left by a Windows editor is not content.
    const gsize length = strlen(*line);
    if(length > 0 && (*line)[length - 1] == '\r') (*line)[length - 1] = '\0';
    _render_line(&state, *line);
  }
  g_strfreev(lines);
  return g_string_free(state.out, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
