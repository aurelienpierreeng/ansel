/*
   This file is part of darktable,
   Copyright (C) 2010 Bruce Guenter.
   Copyright (C) 2010-2011 Henrik Andersson.
   Copyright (C) 2010-2014, 2016 johannes hanika.
   Copyright (C) 2010 Stuart Henderson.
   Copyright (C) 2011 Antony Dovgal.
   Copyright (C) 2011 Robert Bieber.
   Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
   Copyright (C) 2011-2012, 2014, 2016-2017 Ulrich Pegelow.
   Copyright (C) 2012, 2015 Edouard Gomez.
   Copyright (C) 2012 Jérémy Rosen.
   Copyright (C) 2012 Richard Wonka.
   Copyright (C) 2013, 2020 Aldric Renaudin.
   Copyright (C) 2014, 2016 Dan Torop.
   Copyright (C) 2014-2016 Roman Lebedev.
   Copyright (C) 2015-2016 Pedro Côrte-Real.
   Copyright (C) 2017 Heiko Bauke.
   Copyright (C) 2017 luzpaz.
   Copyright (C) 2018, 2020-2026 Aurélien PIERRE.
   Copyright (C) 2018 Edgardo Hoszowski.
   Copyright (C) 2018 Maurizio Paglia.
   Copyright (C) 2018-2020, 2022 Pascal Obry.
   Copyright (C) 2018 rawfiner.
   Copyright (C) 2019 Andreas Schneider.
   Copyright (C) 2019 Diederik ter Rahe.
   Copyright (C) 2019-2020, 2022 Hanno Schwalm.
   Copyright (C) 2020 Chris Elston.
   Copyright (C) 2020, 2022 Diederik Ter Rahe.
   Copyright (C) 2020-2021 Ralf Brown.
   Copyright (C) 2021 Hubert Kowalski.
   Copyright (C) 2022 Martin Bařinka.
   Copyright (C) 2022 Philipp Lutz.
   Copyright (C) 2022 Victor Forsiuk.
   Copyright (C) 2023 Alynx Zhou.
   Copyright (C) 2023 Guillaume Stutin.
   Copyright (C) 2023 Luca Zulberti.

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

// Shared CFA gather/remosaic helpers (bilinear interpolation + clip masks, laplacian
// normalization channel, remosaic to CFA). Public API of the gather TU; see gather.c.

#include "develop/imageop.h"
#include "iop/highlights/common.h"
#include <stdint.h>

void _interpolate_and_mask(const float *const restrict input, float *const restrict interpolated,
                           float *const restrict clipping_mask, const dt_aligned_pixel_t clips_in,
                           const dt_aligned_pixel_t det_scale, const dt_aligned_pixel_t white_balance,
                           const uint32_t filters, const size_t width, const size_t height);

/** Compute channel normalization factors from the current raw ROI.
 *
 * Guided Laplacians only needs a relative RGB normalization before the temporary
 * bilinear reconstruction. Using the average measured value of each CFA color in
 * the current tile keeps the normalization explicit and local to the data being
 * reconstructed, instead of relying on the white balance declared upstream.
 */
void _compute_laplacian_normalization(const float *const restrict input, const dt_iop_roi_t *const roi_in,
                                      const uint32_t filters, const uint8_t (*const xtrans)[6],
                                      dt_aligned_pixel_t normalization);

/** Build the X-Trans bilinear interpolation lookup for the current ROI phase.
 *
 * The lookup keeps the contributing 3x3 neighbours explicit for each position of
 * the 6x6 X-Trans period so CPU and OpenCL guided-laplacian paths start from the
 * same simple bilinear reconstruction.
 */
void _build_xtrans_bilinear_lookup(int32_t lookup[6][6][32], const dt_iop_roi_t *const roi_in,
                                   const uint8_t (*const xtrans)[6]);

/** Bilinearly demosaic the X-Trans raw mosaic and record clipped colors.
 *
 * Guided Laplacians operates on temporary RGB data. For X-Trans we use the same
 * lightweight bilinear neighbourhood as the linear VNG stage so the diffusion
 * begins from a simple and explicit reconstruction.
 */
void _interpolate_and_mask_xtrans(const float *const restrict input, float *const restrict interpolated,
                                  float *const restrict clipping_mask, const dt_aligned_pixel_t clips,
                                  const dt_aligned_pixel_t white_balance, const dt_iop_roi_t *const roi_in,
                                  const int32_t lookup[6][6][32], const uint8_t (*const xtrans)[6],
                                  const size_t width, const size_t height);

void _remosaic_and_replace(const float *const restrict input, const float *const restrict input_raw,
                           const float *const restrict interpolated, const float *const restrict clipping_mask,
                           float *const restrict output, const dt_aligned_pixel_t white_balance,
                           const dt_aligned_pixel_t clips, const int clip_is_floor, const uint32_t filters,
                           const size_t width, const size_t height);

/** Reproject the reconstructed RGB back onto the X-Trans mosaic. */
void _remosaic_and_replace_xtrans(const float *const restrict input, const float *const restrict input_raw,
                                  const float *const restrict interpolated,
                                  const float *const restrict clipping_mask, float *const restrict output,
                                  const dt_aligned_pixel_t white_balance, const dt_aligned_pixel_t clips,
                                  const int clip_is_floor, const dt_iop_roi_t *const roi_in,
                                  const uint8_t (*const xtrans)[6], const size_t width, const size_t height);
