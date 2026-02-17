/*
    This file is part of darktable,
    Copyright (C) 2017 Tobias Ellinghaus.
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
#pragma once

#include <glib.h>

// On Windows the command line arguments are ANSI encoded. We want UTF-8 in dt though.
// including this file will add a wrapper that acts together with linker switch -municode

int main(int argc, char *argv[]);

int wmain(int argc, wchar_t *argv[])
{
  char **_argv = g_malloc0((argc + 1) * sizeof(char *));
  for(int i = 0; i < argc; i++)
    _argv[i] = g_utf16_to_utf8(argv[i], -1, NULL, NULL, NULL);
  int res = main(argc, _argv);
  g_strfreev(_argv);
  return res;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

