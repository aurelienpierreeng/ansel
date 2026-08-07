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
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_IOP_HIGHLIGHTS_SEGMENTATION_H
#define DT_IOP_HIGHLIGHTS_SEGMENTATION_H

// Connected-component segmentation of the clipped regions (host, both paths).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "iop/highlights/common.h"
#include <stdint.h>

int _segment_clipped_regions(const uint8_t *const restrict maskb, const float *const restrict depth,
                             const int width, const int height, const float pad_factor, const int pad_min,
                             const int pad_max, _hl_region_t **regions_out);
#endif // DT_IOP_HIGHLIGHTS_SEGMENTATION_H
