/*
    This file is part of darktable,
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    
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

/* The two helpers that operate on dt_iop_buffer_dsc_t alone, moved here from
 * develop/format.c so that the definitions sit beside the declarations in pixel/format.h.
 *
 * The other three functions that header used to declare -- default_input_format(),
 * default_output_format() and default_blend_colorspace() -- could NOT come with them: they
 * take dt_iop_module_t and dt_dev_pixelpipe_t, which develop/ owns. Moving those down would
 * have dragged layer 5 into layer 2. Their declarations moved up to develop/imageop.h
 * instead, which is the same fix in the other direction.
 */

#include "pixel/format.h"

#include "system/macros.h"

#include <stddef.h>
#include <stdint.h>

void dt_iop_buffer_dsc_update_bpp(dt_iop_buffer_dsc_t *dsc)
{
  dsc->bpp = dsc->channels;

  switch(dsc->datatype)
  {
    case TYPE_FLOAT:
      dsc->bpp *= sizeof(float);
      break;
    case TYPE_UINT16:
      dsc->bpp *= sizeof(uint16_t);
      break;
    case TYPE_UINT8:
      dsc->bpp *= sizeof(uint8_t);
      break;
    case TYPE_UNKNOWN:
      dsc->bpp = 0;
      break;
    default:
      dt_unreachable_codepath();
      break;
  }
}

size_t dt_iop_buffer_dsc_to_bpp(const struct dt_iop_buffer_dsc_t *dsc)
{
  return dsc->bpp;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
