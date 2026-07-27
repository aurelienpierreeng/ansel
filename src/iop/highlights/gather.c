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

// Shared CFA gather/remosaic helpers for the highlights module: bilinear interpolation of the
// raw mosaic into [R,G,B,norm] planes + clip masks, the guided-laplacian normalization channel,
// and the remosaic back to the CFA. Used by both the guided-laplacian mode (highlights.c) and the
// harmonic-transposition driver (process.c). (implementation; see gather.h for the public API.)

#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/gather.h"
#include <math.h>
#include <string.h>

__DT_CLONE_TARGETS__
void _interpolate_and_mask(const float *const restrict input, float *const restrict interpolated,
                           float *const restrict clipping_mask, const dt_aligned_pixel_t clips_in,
                           const dt_aligned_pixel_t det_scale, const dt_aligned_pixel_t white_balance,
                           const uint32_t filters, const size_t width, const size_t height)
{
  // Per-channel effective detection thresholds. det_scale = 1 is the plain clip detection;
  // below 1 it extends the reconstructable set down into the sensor's rolloff band
  // (the BAND OVERRIDE: the knee can restore the band's level but not the slope the sensor
  // never recorded, while the colour-line model, anchored on truly linear data below the
  // band, can -- the measured band value then acts as the per-pixel floor).
  dt_aligned_pixel_t clips;
  for_four_channels(c) clips[c] = clips_in[c] * det_scale[c];

  // Step 1 (article "The algorithm"): bilinear demosaic of the raw CFA to a throwaway [R,G,B,norm]
  // buffer, plus a binary per-channel validity mask keyed on the clip flag v > 0.995*c (here
  // clips[] already folds in the 0.995 detection factor and the det_scale band override above).
  // Every channel gets a value at every pixel so the downstream guided fit is a regression, not
  // an inpainting, problem. Refinements: masks stay binary (0/1), borders mirror (see below).
  // Bilinear interpolation
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t i_center = i * width;
      const float center = input[i_center + j];

      float R = 0.f;
      float G = 0.f;
      float B = 0.f;

      int R_clipped = 0;
      int G_clipped = 0;
      int B_clipped = 0;

      {
        // Mirrored neighbour indexing on the image border ring: reflection preserves each
        // neighbour's CFA colour (the Bayer pattern is 2-periodic), so the per-channel
        // interpolation and clip flags below stay valid on the borders. The previous shortcut
        // (R = G = B = center, all three clip flags keyed on the centre's own channel)
        // corrupted the guide planes and produced dashed per-channel masks along the border
        // ring : fits anchored on them dragged the border-row reconstruction down to the clip
        // level (a one-pixel V-dip through the raw value at every contour on the border rows).
        const size_t i_prev = ((i == 0) ? 1 : i - 1) * width;
        const size_t i_next = ((i == height - 1) ? height - 2 : i + 1) * width;
        const size_t j_prev = (j == 0) ? 1 : j - 1;
        const size_t j_next = (j == width - 1) ? width - 2 : j + 1;

        const float north = input[i_prev + j];
        const float south = input[i_next + j];
        const float west = input[i_center + j_prev];
        const float east = input[i_center + j_next];

        const float north_east = input[i_prev + j_next];
        const float north_west = input[i_prev + j_prev];
        const float south_east = input[i_next + j_next];
        const float south_west = input[i_next + j_prev];

        if(c == GREEN) // green pixel
        {
          G = center;                          // channel measured here: pass the raw value through
          G_clipped = (center > clips[GREEN]); // clip flag: raw > 0.995*c
        }
        else // non-green pixel
        {
          // interpolate inside an X/Y cross: green sits on the 4 orthogonal neighbours (Bayer),
          // so G = mean of {N,S,E,W} = equal 1/4 bilinear weights over the valid green support.
          G = (north + south + east + west) / 4.f;
          // validity is the OR of the neighbours' clip flags (a channel counts as clipped if ANY
          // photosite that fed its interpolation was itself clipped).
          G_clipped = (north > clips[GREEN] || south > clips[GREEN] || east > clips[GREEN] || west > clips[GREEN]);
        }

        if(c == RED) // red pixel
        {
          R = center;
          R_clipped = (center > clips[RED]);
        }
        else // non-red pixel
        {
          if(FC(i + 1, j, filters) == RED)
          {
            // red neighbours are directly above/below: R = mean of {N,S}, equal 1/2 weights
            // we are on a red column (FC(i-1) == FC(i+1) on Bayer), interpolate column-wise
            R = (north + south) / 2.f;
            R_clipped = (north > clips[RED] || south > clips[RED]); // OR of the 2 contributors
          }
          else if(FC(i, j + 1, filters) == RED)
          {
            // red neighbours are left/right: R = mean of {W,E}, equal 1/2 weights
            // we are on a red row, interpolate row-wise
            R = (west + east) / 2.f;
            R_clipped = (west > clips[RED] || east > clips[RED]); // OR of the 2 contributors
          }
          else
          {
            // red neighbours are the 4 diagonal corners: R = mean of the square, equal 1/4 weights
            // we are on a blue row, so interpolate inside a square
            R = (north_west + north_east + south_east + south_west) / 4.f;
            R_clipped = (north_west > clips[RED] || north_east > clips[RED] || south_west > clips[RED]
                         || south_east > clips[RED]); // OR of the 4 contributors
          }
        }

        if(c == BLUE) // blue pixel
        {
          B = center;
          B_clipped = (center > clips[BLUE]);
        }
        else // non-blue pixel
        {
          if(FC(i + 1, j, filters) == BLUE)
          {
            // blue neighbours are directly above/below: B = mean of {N,S}, equal 1/2 weights
            // we are on a blue column (FC(i-1) == FC(i+1) on Bayer), interpolate column-wise
            B = (north + south) / 2.f;
            B_clipped = (north > clips[BLUE] || south > clips[BLUE]); // OR of the 2 contributors
          }
          else if(FC(i, j + 1, filters) == BLUE)
          {
            // blue neighbours are left/right: B = mean of {W,E}, equal 1/2 weights
            // we are on a blue row, interpolate row-wise
            B = (west + east) / 2.f;
            B_clipped = (west > clips[BLUE] || east > clips[BLUE]); // OR of the 2 contributors
          }
          else
          {
            // blue neighbours are the 4 diagonal corners: B = mean of the square, equal 1/4 weights
            // we are on a red row, so interpolate inside a square
            B = (north_west + north_east + south_east + south_west) / 4.f;

            B_clipped = (north_west > clips[BLUE] || north_east > clips[BLUE] || south_west > clips[BLUE]
                         || south_east > clips[BLUE]); // OR of the 4 contributors
          }
        }
      }

      // ALPHA slot carries the magnitude norm = sqrt(R^2 + G^2 + B^2) (Euclidean, as coded);
      // the any-clip opacity is the OR of the three per-channel validity flags.
      dt_aligned_pixel_t RGB = { R, G, B, sqrtf(sqf(R) + sqf(G) + sqf(B)) };
      dt_aligned_pixel_t clipped = { R_clipped, G_clipped, B_clipped, (R_clipped || G_clipped || B_clipped) };

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, white_balance))
      {
        const size_t idx = (i * width + j) * 4 + k;
        // Local channel normalization (article "Local channel normalization"): divide each channel
        // by white_balance[k] = the tile-average of that CFA colour (from _compute_laplacian_
        // normalization), a crude local white balance so the guide-selection variance is not biased
        // toward whichever channel carries the largest raw numbers. Clamp >= 0 (raw can dip negative).
        interpolated[idx] = fmaxf(RGB[k] / white_balance[k], 0.f);
        clipping_mask[idx] = clipped[k]; // store the binary flag; no feathering here (masks stay hard)
      }
    }
}

__DT_CLONE_TARGETS__
void _compute_laplacian_normalization(const float *const restrict input, const dt_iop_roi_t *const roi_in,
                                      const uint32_t filters, const uint8_t (*const xtrans)[6],
                                      dt_aligned_pixel_t normalization)
{
  // Local channel normalization (article "Local channel normalization"): for each CFA colour,
  // compute its plain average over the whole ROI, sum_c = (1/N) * sum over photosites of colour c.
  // Note the division by n_pixels here uses the FULL pixel count N (not the per-colour count), so
  // these factors also carry the CFA fill fraction of each colour -- they are the exact divisors
  // that _interpolate_and_mask/_remosaic later divide by / multiply back.
  float sum_R = 0.f;
  float sum_G = 0.f;
  float sum_B = 0.f;
  const float n_pixels = roi_in->height * roi_in->width;
  __OMP_PARALLEL_FOR__(collapse(2) reduction(+ : sum_R, sum_G, sum_B))
  for(size_t i = 0; i < roi_in->height; i++)
    for(size_t j = 0; j < roi_in->width; j++)
    {
      const int c = (filters == 9u) ? FCxtrans((int)i, (int)j, roi_in, xtrans) : FC(i, j, filters);
      if(c < 0 || c > 2) continue;

      const float value = input[i * roi_in->width + j] / n_pixels; // accumulate value/N into its colour
      if(c == RED)
        sum_R += value;
      else if(c == GREEN)
        sum_G += value;
      else
        sum_B += value;
    }

  normalization[RED] = sum_R;
  normalization[GREEN] = sum_G;
  normalization[BLUE] = sum_B;
  normalization[ALPHA] = 1.f; // norm/opacity slot is untouched by the local white balance
}

__DT_CLONE_TARGETS__
void _build_xtrans_bilinear_lookup(int32_t lookup[6][6][32], const dt_iop_roi_t *const roi_in,
                                   const uint8_t (*const xtrans)[6])
{
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int row = 0; row < 6; row++)
    for(int col = 0; col < 6; col++)
    {
      int32_t *ip = &(lookup[row][col][1]);
      int sum[3] = { 0 };
      const int f = FCxtrans(row, col, roi_in, xtrans);

      // Loop over the local 3x3 support and keep every weighted contributor of
      // the missing colors visible in the lookup table.
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          // Separable bilinear tent weight: 1<<((y==0)+(x==0)) gives 4 on-axis-both (the centre,
          // excluded below), 2 for an edge neighbour (one axis aligned), 1 for a diagonal corner --
          // i.e. the {1,2,1}x{1,2,1} kernel of the same bilinear demosaic used on Bayer above.
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = FCxtrans(row + y, col + x, roi_in, xtrans);
          if(color == f) continue; // skip the centre's own colour: it is passed through as measured
          *ip++ = (y << 16) | (x & 0xffffu);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }

      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3;
      for(int c = 0; c < 3; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }
}

__DT_CLONE_TARGETS__
void _interpolate_and_mask_xtrans(const float *const restrict input, float *const restrict interpolated,
                                  float *const restrict clipping_mask, const dt_aligned_pixel_t clips,
                                  const dt_aligned_pixel_t white_balance, const dt_iop_roi_t *const roi_in,
                                  const int32_t lookup[6][6][32], const uint8_t (*const xtrans)[6],
                                  const size_t width, const size_t height)
{
  // Step 1 (article "The algorithm"), X-Trans twin of _interpolate_and_mask: bilinear demosaic to
  // [R,G,B,norm] + a binary per-channel validity mask keyed on v > 0.995*c. The 6x6 X-Trans phase
  // is 3x3-periodic in support geometry, resolved via the precomputed lookup for interior pixels.
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const float center = input[idx];

      dt_aligned_pixel_t RGB = { 0.f };
      dt_aligned_pixel_t clipped = { 0.f };

      if(i == 0 || j == 0 || i == height - 1 || j == width - 1)
      {
        dt_aligned_pixel_t sum = { 0.f };
        int count[3] = { 0 };
        int used_clipped[3] = { 0 };
        const int f = FCxtrans((int)i, (int)j, roi_in, xtrans);

        // Along tile borders we average only the available neighbours because
        // the full 3x3 support would otherwise leave the current ROI.
        for(int y = MAX((int)i - 1, 0); y <= MIN((int)i + 1, (int)height - 1); y++)
          for(int x = MAX((int)j - 1, 0); x <= MIN((int)j + 1, (int)width - 1); x++)
          {
            const int color = FCxtrans(y, x, roi_in, xtrans);
            const float value = input[(size_t)y * width + x];
            sum[color] += value;
            count[color]++;
            used_clipped[color] |= (value > clips[color]);
          }

        for(int c = 0; c < 3; c++)
        {
          const int has_samples = (count[c] > 0);
          // c==f: the measured centre colour passes through. Otherwise plain average over the
          // available same-colour neighbours (equal weights on the shrunken border support), with
          // the clip flag = OR of those neighbours' flags (or the centre's own for c==f).
          RGB[c] = (c == f || !has_samples) ? center : sum[c] / count[c];
          clipped[c] = (c == f || !has_samples) ? (center > clips[c]) : used_clipped[c];
        }
      }
      else
      {
        const int32_t *ip = &(lookup[i % 6][j % 6][0]);
        dt_aligned_pixel_t sum = { 0.f };
        int used_clipped[3] = { 0 };
        const int neighbours = *ip++;

        // We are looping on every neighbour that contributes to a missing color
        // so the interpolation follows the X-Trans CFA geometry exactly.
        for(int k = 0; k < neighbours; k++, ip += 3)
        {
          const int32_t offset = ip[0];
          const int x = (int16_t)(offset & 0xffffu);
          const int y = (int16_t)(offset >> 16);
          const size_t neighbour = ((size_t)((int)i + y) * width + (size_t)((int)j + x));
          const int color = ip[2];
          const float value = input[neighbour];
          sum[color] += value * ip[1];
          used_clipped[color] |= (value > clips[color]);
        }

        // Normalize the two missing colors from the accumulated weights, then
        // restore the measured center color unchanged.
        // RGB[color] = (sum of weight*value) / (sum of weights) = weighted bilinear mean.
        for(int k = 0; k < 2; k++, ip += 2)
        {
          const int color = ip[0];
          const int total = ip[1];
          RGB[color] = (total > 0) ? sum[color] / total : center; // weighted mean, else fall back
          clipped[color] = used_clipped[color];                   // OR of the contributors' flags
        }

        const int f = *ip;
        RGB[f] = center;                  // centre colour: measured raw value passes through
        clipped[f] = (center > clips[f]); // clip flag: raw > 0.995*c
      }

      // ALPHA slot = Euclidean magnitude norm sqrt(R^2+G^2+B^2); opacity = OR of the per-channel flags.
      // NOTE (article cross-reference): this is the interpolated buffer's shared "norm" channel, and it
      // is NOT the harmonic method's magnitude L_sum. The 2021 guided-laplacian mode consumes it (its
      // a-trous ratio/norm split, see wavelets_process LAST_SCALE); the harmonic path only carries it and
      // never reads it as a magnitude -- forcing it to R+G+B leaves all six ground-truth scenes
      // bit-identical (verified). The article's L_sum = R+G+B is computed separately in the all-clip core
      // (lum_accum in _region_guided_filter / the hl_lsb_hole kernel), which already matches the article.
      RGB[ALPHA] = sqrtf(sqf(RGB[RED]) + sqf(RGB[GREEN]) + sqf(RGB[BLUE]));
      clipped[ALPHA] = (clipped[RED] || clipped[GREEN] || clipped[BLUE]);

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, white_balance))
      {
        const size_t index = idx * 4 + k;
        // Local channel normalization: divide by white_balance[k] = tile-average of that colour;
        // clamp >= 0. Same crude local white balance as the Bayer path above.
        interpolated[index] = fmaxf(RGB[k] / white_balance[k], 0.f);
        clipping_mask[index] = clipped[k]; // binary flag, no feathering (masks stay hard)
      }
    }
}

__DT_CLONE_TARGETS__
void _remosaic_and_replace(const float *const restrict input, const float *const restrict input_raw,
                           const float *const restrict interpolated, const float *const restrict clipping_mask,
                           float *const restrict output, const dt_aligned_pixel_t white_balance,
                           const dt_aligned_pixel_t clips, const int clip_is_floor, const uint32_t filters,
                           const size_t width, const size_t height)
{
  // Remosaic + composite (article "The algorithm", step "remosaic + composite").
  // Compositing rule:  out = opacity*rec + (1 - opacity)*base.
  // Refinement 2 (clipped raw is a FLOOR): with clip_is_floor set, for a clipped photosite
  // base = max(raw, rec) instead of raw -- under sensor rolloff the raw reading of a just-detected
  // photosite sits at the detection threshold, below the true signal, so it is a lower bound, not a
  // measurement. Feathering the reconstruction toward it printed a V-shaped dip at every contour
  // down to the biased reading. The 2021 mode keeps the historical blend (flag 0).
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const float opacity = clipping_mask[index + ALPHA]; // any-clip mask -> blend weight (0 or 1)
      // Undo the local channel normalization: multiply the reconstructed channel back by its
      // tile-average white_balance[c] to return to raw scale, clamp >= 0.
      const float reconstructed = fmaxf(interpolated[index + c] * white_balance[c], 0.f);
      float base = input[idx];
      if(clip_is_floor && input_raw[idx] >= clips[c]) base = fmaxf(base, reconstructed); // floor
      output[idx] = opacity * reconstructed + (1.f - opacity) * base; // out = a*rec + (1-a)*base
    }
}

__DT_CLONE_TARGETS__
void _remosaic_and_replace_xtrans(const float *const restrict input, const float *const restrict input_raw,
                                  const float *const restrict interpolated,
                                  const float *const restrict clipping_mask, float *const restrict output,
                                  const dt_aligned_pixel_t white_balance, const dt_aligned_pixel_t clips,
                                  const int clip_is_floor, const dt_iop_roi_t *const roi_in,
                                  const uint8_t (*const xtrans)[6], const size_t width, const size_t height)
{
  // see _remosaic_and_replace for the clip_is_floor semantics and the compositing rule
  //   out = opacity*rec + (1 - opacity)*base,  base = max(raw, rec) on a clipped floor.
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const int c = FCxtrans((int)i, (int)j, roi_in, xtrans);
      const float opacity = clipping_mask[index + ALPHA]; // any-clip mask -> blend weight (0 or 1)
      // undo local channel normalization (x tile-average white_balance[c]), clamp >= 0
      const float reconstructed = fmaxf(interpolated[index + c] * white_balance[c], 0.f);
      float base = input[idx];
      if(clip_is_floor && input_raw[idx] >= clips[c]) base = fmaxf(base, reconstructed); // floor
      output[idx] = opacity * reconstructed + (1.f - opacity) * base; // out = a*rec + (1-a)*base
    }
}
