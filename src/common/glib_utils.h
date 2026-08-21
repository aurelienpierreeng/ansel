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
#ifndef DT_COMMON_GLIB_UTILS_H
#define DT_COMMON_GLIB_UTILS_H

/* Small GLib convenience helpers (GList traversal, string splicing). Application-free
 * on purpose: include this instead of darktable.h. */

#include "system/macros.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

// a few macros and helper functions to speed up certain frequently-used GLib operations
#define g_list_is_singleton(list) ((list) && (!(list)->next))
static inline gboolean g_list_shorter_than(const GList *list, unsigned len)
{
  // instead of scanning the full list to compute its length and then comparing against the limit,
  // bail out as soon as the limit is reached.  Usage: g_list_shorter_than(l,4) instead of g_list_length(l)<4
  while (len-- > 0)
  {
    if (!list) return TRUE;
    list = g_list_next(list);
  }
  return FALSE;
}

// advance the list by one position, unless already at the final node
static inline GList *g_list_next_bounded(GList *list)
{
  return g_list_next(list) ? g_list_next(list) : list;
}

static inline const GList *g_list_next_wraparound(const GList *list, const GList *head)
{
  return g_list_next(list) ? g_list_next(list) : head;
}

static inline const GList *g_list_prev_wraparound(const GList *list)
{
  // return the prior element of the list, unless already on the first element; in that case, return the last
  // element of the list.
  return g_list_previous(list) ? g_list_previous(list) : g_list_last((GList*)list);
}

static inline gchar *dt_string_replace(const char *string, const char *to_replace)
{
  if(IS_NULL_PTR(string) || IS_NULL_PTR(to_replace)) return NULL;
  gchar **split = g_strsplit(string, to_replace, -1);
  gchar *text = g_strjoinv("", split);
  g_strfreev(split);
  return text;
}

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_GLIB_UTILS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
