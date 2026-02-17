/*
    This file is part of darktable,
    Copyright (C) 2013, 2016 Tobias Ellinghaus.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2021 Miloš Komarčević.
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

#define XMD_H

#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

// ugly hack to make our code work. windows.h has some terrible includes which define these things
// that clash with our variable names. Including them can be omitted when adding
// #define WIN32_LEAN_AND_MEAN
// before including windows.h, but then we will miss some defines needed for libraries like libjpeg.
#undef near
#undef grp2
#undef interface

#define sleep(n) Sleep(1000 * n)
#define HAVE_BOOLEAN

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

