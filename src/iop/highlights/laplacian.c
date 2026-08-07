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

// Guided-laplacian (2021 a-trous) highlight reconstruction, CPU + OpenCL. (implementation; see laplacian.h for the
// public API.)

#include "pixel/box_filters.h"
#include "iop/highlights/common.h"
#include "pixel/bspline.h"
#include "common/logging.h"
#include "common/macros.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"
#include "system/simd.h"
#include "system/target_clones.h"
#include "common/pixelpipe_cache_alloc.h"
#include "pixel/dwt.h"
#include "pixel/fast_guided_filter.h"
#include "common/opencl.h"
#include "develop/imageop_math.h"
#include "iop/noise_generator.h"
#include "iop/highlights/gather.h"
#include "iop/highlights/laplacian.h"
#include <math.h>
#include <string.h>

static inline __attribute__((always_inline)) uint8_t scale_type(const int s, const int scales)
{
  uint8_t scale = ANY_SCALE;
  if(s == 0) scale |= FIRST_SCALE;
  if(s == scales - 1) scale |= LAST_SCALE;
  return scale;
}

__DT_CLONE_TARGETS__
static inline void guide_laplacians(const float *const restrict high_freq, const float *const restrict low_freq,
                                    const float *const restrict clipping_mask, float *const restrict output,
                                    const size_t width, const size_t height, const int mult,
                                    const float noise_level, const int salt, const uint8_t scale,
                                    const float radius_sq)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);
  const dt_aligned_pixel_simd_t zero = dt_simd_set1(0.f);
  const dt_aligned_pixel_simd_t ones = dt_simd_set1(1.f);
  const dt_aligned_pixel_simd_t inv_patch = dt_simd_set1(1.f / 9.f);
  const dt_aligned_pixel_simd_t scale_multiplier = dt_simd_set1(1.f / radius_sq);
  const float eps = 1e-12f;
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const int i = dwt_interleave_rows(row, height, mult);
    const float *const row0 = HF + 4 * ((size_t)MAX(i - mult, 0) * width);
    const float *const row1 = HF + 4 * ((size_t)i * width);
    const float *const row2 = HF + 4 * ((size_t)MIN(i + mult, (int)height - 1) * width);
    const float *const rows[3] = { row0, row1, row2 };
    const int max_col = (int)width - 1;

    for(int j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;
      const float alpha = clipping_mask[index + ALPHA];
      const float alpha_comp = 1.f - alpha;
      dt_aligned_pixel_simd_t high_frequency = dt_load_simd_aligned(HF + index);

      if(alpha > 0.f) // reconstruct
      {
        const int col_offsets[3] = { 4 * MAX(j - mult, 0), 4 * j, 4 * MIN(j + mult, max_col) };
        dt_aligned_pixel_simd_t sum = zero;
        dt_aligned_pixel_simd_t sum_sq = zero;
        dt_aligned_pixel_simd_t prod_r = zero;
        dt_aligned_pixel_simd_t prod_g = zero;
        dt_aligned_pixel_simd_t prod_b = zero;

        // Walk the dense 3x3 neighbourhood as counted loops so GCC keeps the
        // fit as a regular reduction instead of fully unrolling all 9 taps and
        // spilling the intermediate moments to the stack.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
        for(int jj = 0; jj < 3; ++jj)
        {
          const float *const row_ptr = rows[jj];
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
          for(int ii = 0; ii < 3; ++ii)
          {
            const dt_aligned_pixel_simd_t sample = dt_load_simd_aligned(row_ptr + col_offsets[ii]);

            sum += sample;
            sum_sq += sample * sample;
            prod_r += sample * dt_simd_set1(sample[RED]);
            prod_g += sample * dt_simd_set1(sample[GREEN]);
            prod_b += sample * dt_simd_set1(sample[BLUE]);
          }
        }

        dt_aligned_pixel_simd_t means = sum * inv_patch;
        dt_aligned_pixel_simd_t variance = sum_sq * inv_patch - means * means;
        variance = dt_simd_max_zero(variance);
        variance[ALPHA] = 0.f;

        size_t guiding_channel = RED;
        float guide_variance = variance[RED];
        if(variance[GREEN] > guide_variance)
        {
          guiding_channel = GREEN;
          guide_variance = variance[GREEN];
        }
        if(variance[BLUE] > guide_variance)
        {
          guiding_channel = BLUE;
          guide_variance = variance[BLUE];
        }

        if(guide_variance > eps)
        {
          const float guide_mean = means[guiding_channel];
          dt_aligned_pixel_simd_t covariance
              = (guiding_channel == RED ? prod_r : (guiding_channel == GREEN ? prod_g : prod_b)) * inv_patch
                - means * dt_simd_set1(guide_mean);
          dt_aligned_pixel_simd_t slope = covariance / dt_simd_set1(guide_variance);
          slope = dt_simd_max_zero(slope);
          dt_aligned_pixel_simd_t intercept = means - slope * dt_simd_set1(guide_mean);
          const dt_aligned_pixel_simd_t blend = dt_load_simd_aligned(clipping_mask + index) * scale_multiplier;
          const dt_aligned_pixel_simd_t guide = dt_simd_set1(high_frequency[guiding_channel]);
          high_frequency = blend * (slope * guide + intercept) + (ones - blend) * high_frequency;
        }
      }

      dt_aligned_pixel_simd_t out_pixel = high_frequency;
      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
      }
      else
      {
        // just accumulate HF
        out_pixel += dt_load_simd_aligned(out + index);
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        out_pixel = dt_simd_max_zero(out_pixel + dt_load_simd_aligned(LF + index));
      }

      // Last step of RGB reconstruct : add noise
      if((scale & LAST_SCALE) && salt && alpha > 0.f)
      {
        // Init random number generator
        uint32_t DT_ALIGNED_ARRAY state[4]
            = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        dt_aligned_pixel_t noise = { 0.f };
        dt_aligned_pixel_t sigma = { 0.20f };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };

        sigma[RED] = out_pixel[RED] * noise_level;
        sigma[GREEN] = out_pixel[GREEN] * noise_level;
        sigma[BLUE] = out_pixel[BLUE] * noise_level;
        sigma[ALPHA] = out_pixel[ALPHA] * noise_level;

        // create statistical noise
        dt_aligned_pixel_t current = { out_pixel[RED], out_pixel[GREEN], out_pixel[BLUE], out_pixel[ALPHA] };
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, current, sigma, flip, state, noise);

        // Save the noisy interpolated image
        for_each_channel(c, aligned(noise, current)) noise[c] = current[c] + fabsf(noise[c] - current[c]);

        out_pixel[RED] = fmaxf(alpha * noise[RED] + alpha_comp * current[RED], 0.f);
        out_pixel[GREEN] = fmaxf(alpha * noise[GREEN] + alpha_comp * current[GREEN], 0.f);
        out_pixel[BLUE] = fmaxf(alpha * noise[BLUE] + alpha_comp * current[BLUE], 0.f);
        out_pixel[ALPHA] = fmaxf(alpha * noise[ALPHA] + alpha_comp * current[ALPHA], 0.f);
      }

      if((scale & LAST_SCALE))
      {
        // Break the RGB channels into ratios/norm for the next step of reconstruction
        const float norm = fmaxf(sqrtf(sqf(out_pixel[RED]) + sqf(out_pixel[GREEN]) + sqf(out_pixel[BLUE])), 1e-6f);
        out_pixel /= dt_simd_set1(norm);
        out_pixel[ALPHA] = norm;
      }

      dt_store_simd_aligned(out + index, out_pixel);
    }
  }
}

__DT_CLONE_TARGETS__
static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const float *const restrict clipping_mask, float *const restrict output,
                                      const size_t width, const size_t height, const int mult, const uint8_t scale,
                                      const float first_order_factor)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure
  //  one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3] = { MAX((int)(i - mult), (int)0) * width,            // x - mult
                                     i * width,                                       // x
                                     MIN((int)(i + mult), (int)height - 1) * width }; // x + mult

    static const float DT_ALIGNED_ARRAY anisotropic_kernel_isophote[9]
        = { 0.25f, 0.5f, 0.25f, 0.5f, -3.f, 0.5f, 0.25f, 0.5f, 0.25f };

    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const dt_aligned_pixel_t alpha = { clipping_mask[index + RED], clipping_mask[index + GREEN],
                                         clipping_mask[index + BLUE], clipping_mask[index + ALPHA] };

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      // The for_each_channel macro uses 4 floats SIMD instructions or 3 float regular ops,
      // depending on system. Since we don't want to diffuse the norm, make sure to store and restore it later.
      // This is not much of an issue when processing image at full-res, but more harmful since
      // we reconstruct highlights on a downscaled variant
      const float norm_backup = high_frequency[3];

      if(alpha[ALPHA] > 0.f) // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3] = { MAX((int)(j - mult), (int)0),           // y - mult
                                         j,                                      // y
                                         MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF : 16))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the laplacian in the direction parallel to the steepest gradient on the norm
        // Convolve the filter to get the laplacian
        dt_aligned_pixel_t laplacian_HF = { 0.f, 0.f, 0.f, 0.f };
        for(int k = 0; k < 9; k++)
        {
          for_each_channel(c, aligned(laplacian_HF, neighbour_pixel_HF : 16)
                                  aligned(anisotropic_kernel_isophote : 64)) laplacian_HF[c]
              += neighbour_pixel_HF[k][c] * anisotropic_kernel_isophote[k];
        }

        // Diffuse
        const dt_aligned_pixel_t multipliers_HF
            = { 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 0.f };
        for_each_channel(c, aligned(high_frequency, multipliers_HF, laplacian_HF, alpha)) high_frequency[c]
            += alpha[c] * multipliers_HF[c] * (laplacian_HF[c] - first_order_factor * high_frequency[c]);

        // Restore. See above.
        high_frequency[3] = norm_backup;
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64)) out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64)) out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64)) out[index + c]
            = fmaxf(out[index + c] + LF[index + c], 0.f);

        // renormalize ratios
        if(alpha[ALPHA] > 0.f)
        {
          const float norm = sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE]));
          for_each_channel(c, aligned(out, LF, high_frequency : 64)) out[index + c]
              /= (c != ALPHA && norm > 1e-4f) ? norm : 1.f;
        }

        // Last scale : reconstruct RGB from ratios and norm - norm stays in the 4th channel
        // we need it to evaluate the gradient
        for_four_channels(c, aligned(out)) out[index + c]
            = (c == ALPHA) ? out[index + ALPHA] : out[index + c] * out[index + ALPHA];
      }
    }
  }
}

static inline int wavelets_process(const float *const restrict in, float *const restrict reconstructed,
                                   const float *const restrict clipping_mask, const size_t width,
                                   const size_t height, const int scales, float *const restrict HF,
                                   float *const restrict LF_odd, float *const restrict LF_even,
                                   const diffuse_reconstruct_variant_t variant, const float noise_level,
                                   const int salt, const float first_order_factor)
{
  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,

  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const tempbuf = dt_pixelpipe_cache_alloc_perthread_float(4 * width, &padded_size); // TODO: alloc in caller
  if(IS_NULL_PTR(tempbuf)) return 1;

  for(int s = 0; s < scales; ++s)
  {
    // fprintf(stderr, "CPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline(buffer_in, HF, buffer_out, width, height, mult, tempbuf, padded_size);

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));

    if(variant == DIFFUSE_RECONSTRUCT_RGB)
      guide_laplacians(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, noise_level, salt,
                       current_scale_type, radius);
    else
      heat_PDE_diffusion(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, current_scale_type,
                         first_order_factor);

  }
  dt_pixelpipe_cache_free_align(tempbuf);

  return 0;
}

__DT_CLONE_TARGETS__
int process_laplacian(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                      void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                      const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  int err = 0;
  (void)roi_out;

  // Every CFA helper below (normalization, Bayer gather, remosaic) reads FC(row, col, filters) with
  // tile-local row/col (0-based within this buffer, no roi offset added), so filters must be
  // pre-shifted for roi_in's crop position here -- mirrors demosaic.c's tile-local algorithms.
  // dt_dev_get_roi_filters() returns the shifted Bayer word, 9u unchanged for X-Trans, and 0 for
  // already-demosaiced (non-raw / sRAW) input. Only the gather and the remosaic branch on `cfa`; the
  // wavelet reconstruction between them is CFA-agnostic.
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);
  const dt_hl_cfa_t cfa = _hl_cfa_strategy(filters);
  const uint8_t(*const xtrans)[6]
      = (cfa == HL_CFA_XTRANS) ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  const size_t ds_height = height / DS_FACTOR;
  const size_t ds_width = width / DS_FACTOR;
  const size_t ds_size = ds_height * ds_width;

  float *const restrict interpolated
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel
  float *const restrict clipping_mask
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *const restrict LF_odd = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict LF_even = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict temp = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  const float scale = DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  float *restrict HF = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_interpolated = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_clipping_mask = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(LF_even)
     || IS_NULL_PTR(temp) || IS_NULL_PTR(HF) || IS_NULL_PTR(ds_interpolated) || IS_NULL_PTR(ds_clipping_mask))
  {
    err = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(input, roi_in, filters, xtrans, normalization);

  // FLOW step 1a (gather): the only CFA-branching endpoint before the shared wavelet reconstruction.
  // Guided laplacians run without the knee, so det_scale is unit and both CFA gathers take `clips` directly.
  switch(cfa)
  {
    case HL_CFA_BAYER:
    {
      const dt_aligned_pixel_t det_unit = { 1.f, 1.f, 1.f, 1.f };
      _interpolate_and_mask(input, interpolated, clipping_mask, clips, det_unit, normalization, filters, width,
                            height);
      break;
    }
    case HL_CFA_XTRANS:
    {
      int32_t lookup[6][6][32] = { { { 0 } } };
      _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
      _interpolate_and_mask_xtrans(input, interpolated, clipping_mask, clips, normalization, roi_in, lookup,
                                   xtrans, width, height);
      break;
    }
    case HL_CFA_PASSTHROUGH:
      // Non-raw / sRAW: no demosaic, just copy the RGB planes through + build masks.
      _interpolate_and_mask_passthrough(input, interpolated, clipping_mask, clips, normalization, width, height);
      break;
  }
  if(dt_box_mean(clipping_mask, height, width, 4, 2, 1) != 0)
  {
    err = 1;
    goto error;
  }

  // Downsample
  interpolate_bilinear(clipping_mask, width, height, ds_clipping_mask, ds_width, ds_height, 4);
  interpolate_bilinear(interpolated, width, height, ds_interpolated, ds_width, ds_height, 4);

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    if(wavelets_process(ds_interpolated, temp, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd, LF_even,
                        DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
    if(wavelets_process(temp, ds_interpolated, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd, LF_even,
                        DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
  }

  // Upsample
  interpolate_bilinear(ds_interpolated, ds_width, ds_height, interpolated, width, height, 4);
  // FLOW: remosaic + composite -- second and last CFA-branching endpoint (clip_is_floor = FALSE here).
  switch(cfa)
  {
    case HL_CFA_BAYER:
      _remosaic_and_replace(input, input, interpolated, clipping_mask, output, normalization, clips, FALSE,
                            filters, width, height);
      break;
    case HL_CFA_XTRANS:
      _remosaic_and_replace_xtrans(input, input, interpolated, clipping_mask, output, normalization, clips,
                                   FALSE, roi_in, xtrans, width, height);
      break;
    case HL_CFA_PASSTHROUGH:
      // Non-raw / sRAW: composite the reconstructed RGB straight back, per channel (clip_is_floor FALSE).
      _remosaic_and_replace_passthrough(input, input, interpolated, clipping_mask, output, normalization, clips,
                                        FALSE, width, height);
      break;
  }

error:;
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  dt_pixelpipe_cache_free_align(temp);
  dt_pixelpipe_cache_free_align(LF_even);
  dt_pixelpipe_cache_free_align(LF_odd);
  dt_pixelpipe_cache_free_align(HF);
  dt_pixelpipe_cache_free_align(ds_interpolated);
  dt_pixelpipe_cache_free_align(ds_clipping_mask);
  return err;
}

#ifdef HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid, cl_mem in, cl_mem reconstructed,
                                         cl_mem reconstructed_scratch, cl_mem clipping_mask, const size_t sizes[3],
                                         const int width, const int height,
                                         dt_iop_highlights_global_data_t *const gd, const int scales, cl_mem HF,
                                         cl_mem LF_odd, cl_mem LF_even,
                                         const diffuse_reconstruct_variant_t variant, const float noise_level,
                                         const int salt, const float solid_color)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem reconstruct_read = reconstructed_scratch;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  for(int s = 0; s < scales; ++s)
  {
    // fprintf(stderr, "GPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    // Compute wavelets low-frequency scales
    const int clamp_lf = 1;
    int hblocksize;
    dt_opencl_local_buffer_t hlocopt = (dt_opencl_local_buffer_t){ .xoffset = 2 * mult,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 16,
                                                                   .sizey = 1 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_horizontal_local, &hlocopt))
      hblocksize = hlocopt.sizex;
    else
      hblocksize = 1;

    if(hblocksize > 1)
    {
      const size_t horizontal_sizes[3] = { ROUNDUP(width, hblocksize), ROUNDUPDHT(height, devid), 1 };
      const size_t horizontal_local[3] = { hblocksize, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 0, sizeof(cl_mem),
                               (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 1, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 5, sizeof(int),
                               (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 6,
                               (hblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_horizontal_local,
                                                   horizontal_sizes, horizontal_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 1, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    }
    if(err != CL_SUCCESS) return err;

    int vblocksize;
    dt_opencl_local_buffer_t vlocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 2 * mult,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1,
                                                                   .sizey = 1 << 16 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_vertical_local, &vlocopt))
      vblocksize = vlocopt.sizey;
    else
      vblocksize = 1;

    if(vblocksize > 1)
    {
      const size_t vertical_sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUP(height, vblocksize), 1 };
      const size_t vertical_local[3] = { 1, vblocksize, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 0, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 1, sizeof(cl_mem),
                               (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 5, sizeof(int), (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 6,
                               (vblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_vertical_local, vertical_sizes,
                                                   vertical_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 0, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    }
    if(err != CL_SUCCESS) return err;

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));
    cl_mem reconstruct_write = (s == scales - 1)
                                   ? reconstructed
                                   : (reconstruct_read == reconstructed ? reconstructed_scratch : reconstructed);

    // Keep the accumulation image read/write handles distinct at each scale.
    // Some AMD OpenCL drivers get unstable when the same image is bound for both roles.
    if(variant == DIFFUSE_RECONSTRUCT_RGB)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 0, sizeof(cl_mem),
                               (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 1, sizeof(cl_mem),
                               (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 2, sizeof(cl_mem),
                               (void *)&clipping_mask);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 3, sizeof(cl_mem),
                               (void *)&reconstruct_read);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 4, sizeof(cl_mem),
                               (void *)&reconstruct_write);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 5, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 6, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 7, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 8, sizeof(float),
                               (void *)&noise_level);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 9, sizeof(int), (void *)&salt);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 10, sizeof(uint8_t),
                               (void *)&current_scale_type);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 11, sizeof(float), (void *)&radius);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_guide_laplacians, sizes);
      if(err != CL_SUCCESS) return err;
    }
    else // DIFFUSE_RECONSTRUCT_CHROMA
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 2, sizeof(cl_mem),
                               (void *)&clipping_mask);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 3, sizeof(cl_mem),
                               (void *)&reconstruct_read);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 4, sizeof(cl_mem),
                               (void *)&reconstruct_write);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 5, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 6, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 7, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 8, sizeof(uint8_t),
                               (void *)&current_scale_type);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 9, sizeof(float), (void *)&solid_color);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_diffuse_color, sizes);
      if(err != CL_SUCCESS) return err;
    }

    reconstruct_read = reconstruct_write;
  }

  return err;
}
#endif // HAVE_OPENCL

#ifdef HAVE_OPENCL

cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                  const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  // kernel_highlights_normalize_reduce_first is self-correcting: it takes the raw filters
  // below PLUS roi_in->x/y as separate kernel args and adds them itself. interpolate_and_mask
  // and remosaic_and_replace have no roi offset args at all -- they need filters pre-shifted
  // for roi_in's crop position instead (mirrors the CPU process_laplacian fix).
  const uint32_t filters = piece->dsc_in.filters;
  const uint32_t filters_shifted = dt_dev_get_roi_filters(piece, roi_in);

  cl_mem interpolated
      = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // [R, G, B, norm] for each pixel
  cl_mem clipping_mask
      = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // [R, G, B, norm] for each pixel
  cl_mem normalization = NULL;
  cl_mem normalization_tmp = NULL;
  cl_mem normalization_partials = NULL;
  cl_mem normalization_final = NULL;

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp
      = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // need full size here for blurring

  const float scale = DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem reconstructed_scratch = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(LF_even)
     || IS_NULL_PTR(temp) || IS_NULL_PTR(HF) || IS_NULL_PTR(ds_interpolated) || IS_NULL_PTR(ds_clipping_mask)
     || IS_NULL_PTR(reconstructed_scratch) || IS_NULL_PTR(clips_cl))
    goto error;

  {
    dt_opencl_local_buffer_t flocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 4,
                                                                   .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_first, &flocopt)) goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);
    const int bufsize = (int)((bwidth / flocopt.sizex) * (bheight / flocopt.sizey));

    normalization_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * (size_t)bufsize);
    normalization = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    normalization_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    if(!normalization_partials || !normalization || !normalization_tmp) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 3, sizeof(cl_mem),
                             &normalization_partials);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 4, sizeof(uint32_t), &filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 5, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 6, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 7,
                             sizeof(float) * 4 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_first, fsizes,
                                                 flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 16,
                                                                   .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_second, &slocopt)) goto error;

    int current_length = bufsize;
    cl_mem reduce_in = normalization_partials;
    cl_mem reduce_out = normalization;

    while(TRUE)
    {
      const int reducesize = MIN(REDUCESIZE, ROUNDUP(current_length, slocopt.sizex) / slocopt.sizex);
      size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
      size_t slocal[3] = { slocopt.sizex, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 0, sizeof(cl_mem), &reduce_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 1, sizeof(cl_mem),
                               &reduce_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 2, sizeof(int),
                               &current_length);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 3,
                               sizeof(float) * 4 * slocopt.sizex, NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_second, ssizes,
                                                   slocal);
      if(err != CL_SUCCESS) goto error;

      if(reducesize == 1) break;
      current_length = reducesize;
      cl_mem swap = reduce_in;
      reduce_in = reduce_out;
      reduce_out = (swap == normalization_partials) ? normalization_tmp : normalization;
    }

    normalization_final = reduce_out;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 1, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 2, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 3, sizeof(cl_mem), (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 4, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 5, sizeof(int),
                           (void *)&filters_shifted);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 6, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 7, sizeof(int),
                           (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 0, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 1, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 2, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 3, sizeof(int), (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  // Downsample
  const int RGBa = TRUE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    err = wavelets_process_cl(devid, ds_interpolated, temp, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_RGB,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_CHROMA,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  // Upsample
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  // Remosaic
  const int clip_floor_off = FALSE; // 2021 mode keeps the historical blend
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 1, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 2, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 3, sizeof(cl_mem),
                           (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 4, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 5, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 6, sizeof(cl_mem), (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 7, sizeof(int),
                           (void *)&clip_floor_off);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 8, sizeof(int),
                           (void *)&filters_shifted);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 9, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 10, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace, sizes);
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);
  return err;

error:
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return err;
}
#endif // HAVE_OPENCL

#ifdef HAVE_OPENCL

cl_int process_laplacian_xtrans_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                   const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                   const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem normalization = NULL;
  cl_mem normalization_tmp = NULL;
  cl_mem normalization_partials = NULL;
  cl_mem normalization_final = NULL;
  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  const float scale = DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
  const float noise_level = data->noise_level / scale;

  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem reconstructed_scratch = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);

  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);
  cl_mem dev_xtrans
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans), (void *)piece->dsc_in.xtrans);
  int32_t lookup[6][6][32] = { { { 0 } } };
  _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
  cl_mem lookup_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(lookup), lookup);

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(LF_even)
     || IS_NULL_PTR(temp) || IS_NULL_PTR(HF) || IS_NULL_PTR(ds_interpolated) || IS_NULL_PTR(ds_clipping_mask)
     || IS_NULL_PTR(reconstructed_scratch) || IS_NULL_PTR(clips_cl) || IS_NULL_PTR(dev_xtrans)
     || IS_NULL_PTR(lookup_cl))
    goto error;

  {
    dt_opencl_local_buffer_t flocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 4,
                                                                   .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);
    const int bufsize = (int)((bwidth / flocopt.sizex) * (bheight / flocopt.sizey));

    normalization_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * (size_t)bufsize);
    normalization = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    normalization_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    if(!normalization_partials || !normalization || !normalization_tmp) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 0, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 3, sizeof(cl_mem),
                             &normalization_partials);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 4, sizeof(int),
                             &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 5, sizeof(int),
                             &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 6, sizeof(cl_mem),
                             &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 7,
                             sizeof(float) * 4 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_first_xtrans,
                                                 fsizes, flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 16,
                                                                   .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_second, &slocopt)) goto error;

    int current_length = bufsize;
    cl_mem reduce_in = normalization_partials;
    cl_mem reduce_out = normalization;

    while(TRUE)
    {
      const int reducesize = MIN(REDUCESIZE, ROUNDUP(current_length, slocopt.sizex) / slocopt.sizex);
      size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
      size_t slocal[3] = { slocopt.sizex, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 0, sizeof(cl_mem), &reduce_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 1, sizeof(cl_mem),
                               &reduce_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 2, sizeof(int),
                               &current_length);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 3,
                               sizeof(float) * 4 * slocopt.sizex, NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_second, ssizes,
                                                   slocal);
      if(err != CL_SUCCESS) goto error;

      if(reducesize == 1) break;
      current_length = reducesize;
      cl_mem swap = reduce_in;
      reduce_in = reduce_out;
      reduce_out = (swap == normalization_partials) ? normalization_tmp : normalization;
    }

    normalization_final = reduce_out;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 0, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 1, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 2, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 3, sizeof(cl_mem),
                           (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 4, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 5, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 6, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 7, sizeof(int),
                           (void *)&roi_in->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 8, sizeof(int),
                           (void *)&roi_in->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 9, sizeof(cl_mem),
                           (void *)&dev_xtrans);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 10, sizeof(cl_mem),
                           (void *)&lookup_cl);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 0, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 1, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 2, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 3, sizeof(int), (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  const int RGBa = TRUE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1);
    err = wavelets_process_cl(devid, ds_interpolated, temp, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_RGB,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_CHROMA,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  const int clip_floor_off = FALSE; // 2021 mode keeps the historical blend
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 0, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 1, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 2, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 3, sizeof(cl_mem),
                           (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 4, sizeof(cl_mem),
                           (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 5, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 6, sizeof(cl_mem),
                           (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 7, sizeof(int),
                           (void *)&clip_floor_off);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 8, sizeof(int),
                           (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 9, sizeof(int),
                           (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 10, sizeof(int),
                           (void *)&roi_in->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 11, sizeof(int),
                           (void *)&roi_in->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 12, sizeof(cl_mem),
                           (void *)&dev_xtrans);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(lookup_cl);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);
  return err;

error:
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(lookup_cl);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return err;
}
cl_int process_laplacian_passthrough_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                        const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const dt_aligned_pixel_t clips)
{
  // Non-raw / sRAW guided-laplacian, fully on-device: already-demosaiced RGB input, so the gather is a
  // plane copy (interpolate_and_mask_passthrough), the normalization sums all three colours per pixel
  // (highlights_normalize_reduce_first_passthrough), and the remosaic is a per-channel composite
  // (remosaic_and_replace_passthrough). Between them the downsample -> a-trous wavelets -> upsample is
  // the same CFA-agnostic device path as the Bayer/X-Trans drivers. No FC, no xtrans lookup, no roi phase.
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem normalization = NULL;
  cl_mem normalization_tmp = NULL;
  cl_mem normalization_partials = NULL;
  cl_mem normalization_final = NULL;

  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  const float scale = DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
  const float noise_level = data->noise_level / scale;

  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem reconstructed_scratch = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(LF_even)
     || IS_NULL_PTR(temp) || IS_NULL_PTR(HF) || IS_NULL_PTR(ds_interpolated) || IS_NULL_PTR(ds_clipping_mask)
     || IS_NULL_PTR(reconstructed_scratch) || IS_NULL_PTR(clips_cl))
    goto error;

  {
    dt_opencl_local_buffer_t flocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 4,
                                                                   .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);
    const int bufsize = (int)((bwidth / flocopt.sizex) * (bheight / flocopt.sizey));

    normalization_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * (size_t)bufsize);
    normalization = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    normalization_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    if(!normalization_partials || !normalization || !normalization_tmp) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, 0, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, 1, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, 2, sizeof(int),
                             &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, 3, sizeof(cl_mem),
                             &normalization_partials);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_passthrough, 4,
                             sizeof(float) * 4 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_first_passthrough,
                                                 fsizes, flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                                                   .xfactor = 1,
                                                                   .yoffset = 0,
                                                                   .yfactor = 1,
                                                                   .cellsize = 4 * sizeof(float),
                                                                   .overhead = 0,
                                                                   .sizex = 1 << 16,
                                                                   .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_second, &slocopt)) goto error;

    int current_length = bufsize;
    cl_mem reduce_in = normalization_partials;
    cl_mem reduce_out = normalization;

    while(TRUE)
    {
      const int reducesize = MIN(REDUCESIZE, ROUNDUP(current_length, slocopt.sizex) / slocopt.sizex);
      size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
      size_t slocal[3] = { slocopt.sizex, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 0, sizeof(cl_mem),
                               &reduce_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 1, sizeof(cl_mem),
                               &reduce_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 2, sizeof(int),
                               &current_length);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 3,
                               sizeof(float) * 4 * slocopt.sizex, NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_second, ssizes,
                                                   slocal);
      if(err != CL_SUCCESS) goto error;

      if(reducesize == 1) break;
      current_length = reducesize;
      cl_mem swap = reduce_in;
      reduce_in = reduce_out;
      reduce_out = (swap == normalization_partials) ? normalization_tmp : normalization;
    }

    normalization_final = reduce_out;
  }

  // gather: plane copy + per-channel clip mask (mask written to `temp`, then 5x5-boxed into clipping_mask)
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 0, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 1, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 2, sizeof(cl_mem),
                           (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 3, sizeof(cl_mem),
                           (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 4, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 5, sizeof(int),
                           (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, 6, sizeof(int),
                           (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask_passthrough, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 0, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 1, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 2, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 3, sizeof(int), (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  // Downsample
  const int RGBa = TRUE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    err = wavelets_process_cl(devid, ds_interpolated, temp, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_RGB,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, reconstructed_scratch, ds_clipping_mask, ds_sizes,
                              ds_width, ds_height, gd, scales, HF, LF_odd, LF_even, DIFFUSE_RECONSTRUCT_CHROMA,
                              noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  // Upsample
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  // Remosaic: per-channel composite (clip_is_floor FALSE, historical blend, mirrors the CPU path)
  const int clip_floor_off = FALSE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 0, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 1, sizeof(cl_mem),
                           (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 2, sizeof(cl_mem),
                           (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 3, sizeof(cl_mem),
                           (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 4, sizeof(cl_mem),
                           (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 5, sizeof(cl_mem),
                           (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 6, sizeof(cl_mem),
                           (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 7, sizeof(int),
                           (void *)&clip_floor_off);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 8, sizeof(int),
                           (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, 9, sizeof(int),
                           (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace_passthrough, sizes);
  if(err != CL_SUCCESS) goto error;

error:
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return err;
}
#endif // HAVE_OPENCL
