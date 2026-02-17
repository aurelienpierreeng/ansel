/*
    This file is part of darktable,
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2018 luzpaz.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Aurélien PIERRE.
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

#if !defined(_RELEASE) && !defined(__cplusplus) && !defined(_WIN32)

//
// We needed to poison certain functions in order to disallow their usage
// but not in bundled libs
//

// this is ugly, but needed, because else compilation will fail with:
// ansel/src/common/poison.h:16:20: error: poisoning existing macro "strncat" [-Werror]
//  #pragma GCC poison strncat  // use g_strncat
#pragma GCC system_header

//#pragma GCC poison sprintf  // use snprintf
#pragma GCC poison vsprintf // use vsnprintf
#pragma GCC poison strcpy   // use g_strlcpy
//#pragma GCC poison strncpy  // use g_strlcpy
#pragma GCC poison strcat  // use g_strncat
#pragma GCC poison strncat // use g_strncat
#pragma GCC poison pthread_create // use dt_pthread_create, musl issues
#pragma GCC poison fopen // use g_fopen
// #pragma GCC poison open // use g_open -- this one doesn't work
#pragma GCC poison unlink // use g_unlink

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
