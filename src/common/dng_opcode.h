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
    Copyright (C) 2022 paolodepetrillo.
    Copyright (C) 2025 Alynx Zhou.
    
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

#include <stdint.h>
#include "image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_dng_gain_map_t
{
  uint32_t top;
  uint32_t left;
  uint32_t bottom;
  uint32_t right;
  uint32_t plane;
  uint32_t planes;
  uint32_t row_pitch;
  uint32_t col_pitch;
  uint32_t map_points_v;
  uint32_t map_points_h;
  double map_spacing_v;
  double map_spacing_h;
  double map_origin_v;
  double map_origin_h;
  uint32_t map_planes;
  float map_gain[];
} dt_dng_gain_map_t;

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t size, dt_image_t *img);

#ifdef __cplusplus
}
#endif
