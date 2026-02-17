/*
    This file is part of darktable,
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2011 johannes hanika.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Alexandre Prokoudine.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Gaspard Jankowiak.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2014-2016 Tobias Ellinghaus.
    Copyright (C) 2018, 2020 Pascal Obry.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018 Sam Smith.
    Copyright (C) 2020 Philippe Weyland.
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

typedef enum dt_lib_collect_mode_t
{
  DT_LIB_COLLECT_MODE_AND = 0,
  DT_LIB_COLLECT_MODE_OR,
  DT_LIB_COLLECT_MODE_AND_NOT
} dt_lib_collect_mode_t;

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

