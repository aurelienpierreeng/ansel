/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "common/darktable.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>


/*
static inline gchar* get_os_path_separator()
{
#if defined(__APPLE__)
return "/";

// #elif defined(_WIN32)
// return "\\";

#else
return "/";

#endif
}
*/

// remove trail and lead space of each folders and file name. Result should be freed.
static inline char* dt_remove_trail_lead_space(const gchar *text)
{
  gchar **split = g_strsplit(text, G_DIR_SEPARATOR, -1);
  for(int i = 0; i < g_strv_length(split); i++)
    if(g_strdup(split[i]) != NULL ) g_strstrip(split[i]);
  
  char* result = g_strjoinv(G_DIR_SEPARATOR, split);
  g_strfreev(split);

  return result;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
