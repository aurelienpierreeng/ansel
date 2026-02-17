/*
    This file is part of darktable,
    Copyright (C) 2009-2012 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011, 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Pierre Le Magourou.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Aurélien PIERRE.
    
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

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "control/signal.h"

#include <inttypes.h>

typedef char dt_dev_operation_t[20];

#define DEV_NUM_OP_PARAMS 10

typedef union dt_dev_operation_params_t
{
  int32_t i[DEV_NUM_OP_PARAMS];
  float f[DEV_NUM_OP_PARAMS];
} dt_dev_operation_params_t;

typedef enum dt_lib_filter_t
{
  DT_LIB_FILTER_ALL = 0,
  DT_LIB_FILTER_STAR_NO = 1,
  DT_LIB_FILTER_STAR_1 = 2,
  DT_LIB_FILTER_STAR_2 = 3,
  DT_LIB_FILTER_STAR_3 = 4,
  DT_LIB_FILTER_STAR_4 = 5,
  DT_LIB_FILTER_STAR_5 = 6,
  DT_LIB_FILTER_REJECT = 7
} dt_lib_filter_t;

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
