/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2014 Roman Lebedev.
    Copyright (C) 2020 Andreas Schneider.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 This code is taken from http://git.gnome.org/browse/gobject-introspection/tree/giscanner/grealpath.h .
 According to http://git.gnome.org/browse/gobject-introspection/tree/COPYING it's licensed under the LGPLv2+.
*/

#ifndef DT_COMMON_GREALPATH_H
#define DT_COMMON_GREALPATH_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#include "system/macros.h"   // IS_NULL_PTR

#ifdef _WIN32
#include <fileapi.h>
#endif

/**
 * g_realpath:
 *
 * this should be a) filled in for win32 and b) put in glib...
 */

static inline gchar *g_realpath(const char *path)
{
#ifndef _WIN32
  /* POSIX.1-2008: a NULL second argument makes realpath() allocate a buffer of exactly
   * the size it needs. The alternative form wants a caller buffer of at least the system
   * PATH_MAX, which is the one place in this tree that genuinely needs that macro -- so
   * this form removes the last reason to name it. */
  char *resolved = realpath(path, NULL);

  if(IS_NULL_PTR(resolved))
  {
    fprintf(stderr, "path lookup '%s' fails with: '%s'\n", path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  gchar *result = g_strdup(resolved);
  free(resolved);
  return result;
#else
  /* GetFullPathNameA caps at MAX_PATH and mangles any path the active ANSI code page
   * cannot spell, which is most user names outside us-ascii. The wide entry point has
   * neither problem; glib owns the UTF-8 <-> UTF-16 conversion. */
  wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
  if(IS_NULL_PTR(wpath)) return g_strdup(path);

  /* Called with a zero-length buffer, it returns the length it wants, terminator
   * included. Anything else is a failure we answer by handing the input back. */
  const DWORD needed = GetFullPathNameW(wpath, 0, NULL, NULL);
  if(needed == 0)
  {
    g_free(wpath);
    return g_strdup(path);
  }

  wchar_t *wbuffer = g_new(wchar_t, needed);
  const DWORD written = GetFullPathNameW(wpath, needed, wbuffer, NULL);
  g_free(wpath);

  if(written == 0 || written >= needed)
  {
    g_free(wbuffer);
    return g_strdup(path);
  }

  gchar *result = g_utf16_to_utf8(wbuffer, -1, NULL, NULL, NULL);
  g_free(wbuffer);
  return IS_NULL_PTR(result) ? g_strdup(path) : result;
#endif
}

#endif // DT_COMMON_GREALPATH_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
