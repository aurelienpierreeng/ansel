/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 johannes hanika.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2014 Jérémy Rosen.
    Copyright (C) 2014-2015, 2020 Pascal Obry.
    Copyright (C) 2015-2016 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2020 Aurélien PIERRE.
    Copyright (C) 2021 Ralf Brown.
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

#ifndef DT_PIXEL_FORMAT_H
#define DT_PIXEL_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include "system/simd.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Region of interest passed through the pixelpipe.
 *
 * @details `scale` must stay consistent with `x`, `y`, `width` and `height`,
 * which all describe the same raster ROI seen by the current pipeline stage.
 */
typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  double scale;
} dt_iop_roi_t;

typedef enum dt_iop_buffer_type_t {
  TYPE_UNKNOWN,
  TYPE_FLOAT,
  TYPE_UINT16,
  TYPE_UINT8,
} dt_iop_buffer_type_t;

/** colorspace enums, must be in synch with dt_iop_colorspace_type_t in color_conversion.cl */
typedef enum dt_iop_colorspace_type_t
{
  IOP_CS_NONE = -1,
  IOP_CS_RAW = 0,
  IOP_CS_LAB = 1,
  IOP_CS_RGB = 2,
  IOP_CS_LCH = 3,
  IOP_CS_HSL = 4,
  IOP_CS_JZCZHZ = 5,
  IOP_CS_RGB_DISPLAY = 6,
} dt_iop_colorspace_type_t;

/* Trivial predicate over the enum above; it lived in develop/imageop.h. */
static inline gboolean dt_iop_colorspace_is_rgb(const dt_iop_colorspace_type_t cst)
{
  return cst == IOP_CS_RGB || cst == IOP_CS_RGB_DISPLAY;
}

typedef struct dt_iop_buffer_dsc_t
{
  /** how many channels the data has? 1 or 4 */
  unsigned int channels;
  /** what is the datatype? */
  dt_iop_buffer_type_t datatype;
  /** bytes per pixel, derived from channels and datatype when the descriptor is updated */
  size_t bpp;
  /** Bayer demosaic pattern */
  uint32_t filters;
  /** filter for Fuji X-Trans images, only used if filters == 9u.
   * The coefficients in the filter represent the channel (R, G, B)
   * associated with a pixel spatial coordinate. The filter itself is a 6x6
   * tile that we shift on the image, starting at the top-left corner,
   * when demosaicing, so we know what "color" we are looking at, at any
   * given pixel coordinate, in the raw image. When images are cropped at the 
   * raw level, we need to shift the coefficients properly to take care of the
   * phase shift introduced by trimming. See rawprepare.c:_update_output_cfa_descriptor()
  */
  uint8_t xtrans[6][6];

  struct
  {
    uint16_t raw_black_level;
    uint16_t raw_white_point;
  } rawprepare;

  struct
  {
    int enabled;
    dt_aligned_pixel_t coeffs;
  } temperature;

  /** sensor saturation, propagated through the operations */
  dt_aligned_pixel_t processed_maximum;

  /** colorspace of the image */
  int cst;

} dt_iop_buffer_dsc_t;

void dt_iop_buffer_dsc_update_bpp(struct dt_iop_buffer_dsc_t *dsc);
size_t dt_iop_buffer_dsc_to_bpp(const struct dt_iop_buffer_dsc_t *dsc);

/* default_input_format(), default_output_format() and default_blend_colorspace() used to be
 * declared here. They are IOP-module defaults, they take dt_iop_module_t and
 * dt_dev_pixelpipe_t, and declaring them here is what forced this layer-2 header to
 * forward-declare three develop/ types. They live in develop/imageop.h now, beside their
 * definitions' layer and beside the module API every caller already includes. */

#ifdef __cplusplus
}
#endif

#endif // DT_PIXEL_FORMAT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
