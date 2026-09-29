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

#include "canvas/canvas_text_breaks.h"

#include "system/macros.h" // IS_NULL_PTR

gboolean dt_canvas_text_clings_before(const gunichar character)
{
  switch(character)
  {
    // The marks French sets a space before.
    case ';':
    case ':':
    case '!':
    case '?':
    case 0x00BB: // closing guillemet
    case 0x203A: // closing single guillemet
    // And the signs that belong to the number in front of them.
    case '%':
    case 0x2030: // per mille
    case 0x00B0: // degree
      return TRUE;
    default:
      return FALSE;
  }
}

gboolean dt_canvas_text_clings_after(const gunichar character)
{
  // The opening guillemets own the space after them. UAX #14 already forbids a break after an
  // opening bracket or quote, so those need no guard of their own.
  return character == 0x00AB || character == 0x2039;
}

/** Is this one of the spaces a typographer sets before a mark? */
static gboolean _is_guarded_space(const gunichar character)
{
  return character == 0x0020 || character == 0x00A0 || character == 0x202F || character == 0x2009;
}

/** Forbid every break between these two byte offsets. */
static void _forbid(PangoAttrList *attributes, const int start, const int end)
{
  if(end <= start) return;
  PangoAttribute *guard = pango_attr_allow_breaks_new(FALSE);
  guard->start_index = (guint)start;
  guard->end_index = (guint)end;
  pango_attr_list_insert(attributes, guard);
}

void dt_canvas_text_guard_breaks(PangoAttrList *attributes, const char *plain, const gsize length)
{
  if(IS_NULL_PTR(attributes) || IS_NULL_PTR(plain) || length == 0) return;
  const char *end = plain + length;
  for(const char *at = plain; at < end;)
  {
    const gunichar character = g_utf8_get_char(at);
    const char *next = g_utf8_next_char(at);
    if(next > end) break;

    if(_is_guarded_space(character))
    {
      /*
       * A space whose next character clings: guard from the SPACE through the clinging run, so
       * the break before the word in front of the space stays open and "mot ;" goes down whole.
       */
      const char *run = next;
      while(run < end)
      {
        const gunichar following = g_utf8_get_char(run);
        if(following == '\n' || !dt_canvas_text_clings_before(following)) break;
        run = g_utf8_next_char(run);
      }
      if(run > next) _forbid(attributes, (int)(at - plain), (int)(run - plain));
    }
    else if(dt_canvas_text_clings_after(character) && next < end && _is_guarded_space(g_utf8_get_char(next)))
    {
      // An opening guillemet and the space it owns, so the quote never ends a line alone.
      const char *after = g_utf8_next_char(next);
      if(after <= end) _forbid(attributes, (int)(at - plain), (int)(after - plain));
    }
    at = next;
  }
}
