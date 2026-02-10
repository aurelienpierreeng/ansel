/*
    This file is part of darktable,
    This file is part of Ansel,
    Copyright (C) 2019-2021 darktable developers.
    Copyright (C) 2019-2025 Aurélien Pierre

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

#include "common/eigf.h"
#include "develop/openmp_maths.h"

/* NOTE: this code complies with the optimizations in "common/extra_optimizations.h".
 * Consider including that at the beginning of a *.c file where you use this
 * header (provided the rest of the code complies).
 **/

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float uint8_to_float(const uint8_t i)
{
  return (float)i / 255.0f;
}

int dt_focuspeaking(cairo_t *cr,
                    uint8_t *const restrict image,
                    const int buf_width, const int buf_height,
                    gboolean draw,
                    float *x, float *y)
{
  float *const restrict luma = dt_pixelpipe_cache_alloc_align_float_cache((size_t)buf_width * buf_height, 0);
  float *const restrict luma_ds = dt_pixelpipe_cache_alloc_align_float_cache((size_t)buf_width * buf_height, 0);
  uint8_t *restrict focus_peaking = NULL;
  int err = 0;
  if(luma_ds == NULL || luma == NULL)
  {
    err = 1;
    goto error_early;
  }

  const size_t npixels = (size_t)buf_height * buf_width;
  // Create a luma buffer as the euclidian norm of RGB channels
#ifdef _OPENMP
#pragma omp parallel for simd default(none)             \
  dt_omp_firstprivate(image, luma, npixels)             \
  schedule(static) aligned(image, luma:64)
#endif
  for(size_t index = 0; index < npixels; index++)
    {
      const size_t index_RGB = index * 4;

      // remove gamma 2.2 and take the square is equivalent to this:
      const float exponent = 2.0f * 2.2f;

      luma[index] = sqrtf( powf(uint8_to_float(image[index_RGB]), exponent) +
                           powf(uint8_to_float(image[index_RGB + 1]), exponent) +
                           powf(uint8_to_float(image[index_RGB + 2]), exponent) );
    }

  // Prefilter noise
  if(fast_eigf_surface_blur(luma, buf_width, buf_height, 12, 0.00005f, 4, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 1.0f) != 0)
  {
    err = 1;
    goto error_early;
  }

  // Compute the laplacian of a gaussian
  float mass = 0.f;
  float x_integral = 0.f;
  float y_integral = 0.f;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(luma, luma_ds, buf_height, buf_width) \
schedule(static) collapse(2) reduction(+:mass, x_integral, y_integral)
#endif
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      size_t index = i * buf_width + j;
      if(i < 8 || i >= buf_height - 8 || j < 8 || j > buf_width - 8)
      {
        // ensure defined value for borders
        luma_ds[index] = 0.0f;
      }
      else
      {
        // Laplacian of a Gaussian kernel with sigma = 1.05
        static const float kernel[7][7]
            = { { 0.00053449f, 0.00352729f,  0.00992912f,  0.01362207f,  0.00992912f, 0.00352729f, 0.00053449f },
                { 0.00352729f, 0.01828379f,  0.03437727f,  0.03474665f,  0.03437727f, 0.01828379f, 0.00352729f },
                { 0.00992912f, 0.03437727f, -0.00982925f, -0.09093110f, -0.00982925f, 0.03437727f, 0.00992912f },
                { 0.01362207f, 0.03474665f, -0.09093110f, -0.26187433f, -0.09093110f, 0.03474665f, 0.01362207f },
                { 0.00992912f, 0.03437727f, -0.00982925f, -0.09093110f, -0.00982925f, 0.03437727f, 0.00992912f },
                { 0.00352729f, 0.01828379f,  0.03437727f,  0.03474665f,  0.03437727f, 0.01828379f, 0.00352729f },
                { 0.00053449f, 0.00352729f,  0.00992912f,  0.01362207f,  0.00992912f, 0.00352729f, 0.00053449f } };

        // The close laplacian is the local-local contrast
        // The far laplacian is the far local contrast, sampled 2 times farther in an a-trous fashion.
        // If far / 2 = close, we are on a slowly-varying gradient, aka on a contrasted edge that is not sharp.
        float laplacian_close = 0.f;
        float laplacian_far = 0.f;

        for(int ii = 0; ii < 7; ii++)
          for(int jj = 0; jj < 7; jj++)
          {
            laplacian_close += luma[(i - 3 + ii) * buf_width + (j - 3 + jj)] * kernel[ii][jj];
            laplacian_far += luma[(i + (-3 + ii) * 2) * buf_width + (j + (-3 + jj) * 2)] * kernel[ii][jj];
          }

        // gradient on principal directions
        const float gradient_1_y = (luma[(i - 2) * buf_width + (j)] - luma[(i + 2) * buf_width + (j)]) / 4.f;
        const float gradient_1_x = (luma[(i) * buf_width + (j - 2)] - luma[(i) * buf_width + (j + 2)]) / 4.f;
        const float TV_1 = dt_fast_hypotf(gradient_1_x, gradient_1_y);

        // gradient on diagonals
        const float gradient_2_y = (luma[(i - 2) * buf_width + (j - 2)] - luma[(i + 2) * buf_width + (j + 2)]) / (2.f * sqrtf(2.f));
        const float gradient_2_x = (luma[(i - 2) * buf_width + (j + 2)] - luma[(i + 2) * buf_width + (j - 2)]) / (2.f * sqrtf(2.f));
        const float TV_2 = dt_fast_hypotf(gradient_2_x, gradient_2_y);

        // gradient on principal directions
        const float gradient_3_y = (luma[(i - 1) * buf_width + (j)] - luma[(i + 1) * buf_width + (j)]) / 2.f;
        const float gradient_3_x = (luma[(i) * buf_width + (j - 1)] - luma[(i) * buf_width + (j + 1)]) / 2.f;
        const float TV_3 = dt_fast_hypotf(gradient_3_x, gradient_3_y);

        // gradient on diagonals
        const float gradient_4_y = (luma[(i - 1) * buf_width + (j - 1)] - luma[(i + 1) * buf_width + (j + 1)]) / (sqrtf(2.f));
        const float gradient_4_x = (luma[(i - 1) * buf_width + (j + 1)] - luma[(i + 1) * buf_width + (j - 1)]) / (sqrtf(2.f));
        const float TV_4 = dt_fast_hypotf(gradient_4_x, gradient_4_y);

        // Total Variation = norm(grad_x, grad_y). We use it as a metric of global contrast since it doesn't use the current pixel.
        // Laplacian = div(grad). We use it as a metric of local contrast, aka difference with current pixel and local average value.
        // The ratio of both is meant to catch local contrast NOT correlated with global contrast, aka sharp edges.
        // The TV is averaged from both directions, its coeff is made-up to balance local contrast detection.
        const float TV = 100.f * (TV_1 + TV_2 + TV_3 + TV_4) / 4.f;
        luma_ds[index] = (laplacian_close > 1e-15f) ? fmaxf(fabsf(laplacian_close) - 0.5f * fabsf(laplacian_far), 0.f) / (TV + 1.f) : 0.f;

        // Compute the mass and integrals over x and y
        mass += luma_ds[index];
        x_integral += ((float)j) * luma_ds[index];
        y_integral += ((float)i) * luma_ds[index];
      }
    }

  // Compute the coordinates of the details barycenter
  if(x) *x = CLAMP(x_integral / mass, 0, buf_height);
  if(y) *y = CLAMP(y_integral / mass, 0, buf_height);

  // Stop there if no drawing is requested
  if(!draw)
  {
    dt_pixelpipe_cache_free_align(luma);
    dt_pixelpipe_cache_free_align(luma_ds);
    return 0;
  }

  focus_peaking = dt_pixelpipe_cache_alloc_align_cache(
      sizeof(uint8_t) * buf_width * buf_height * 4,
      0);
  if(focus_peaking == NULL)
  {
    err = 1;
    goto error;
  }

  // Dilate the mask to improve connectivity
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(luma, luma_ds, buf_height, buf_width) \
schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      size_t index = i * buf_width + j;
      if(i < 8 || i >= buf_height - 8 || j < 8 || j > buf_width - 8)
      {
        // ensure defined value for borders
        luma[index] = 0.0f;
      }
      else
      {
        // Dilating kernel
        static const float kernel[3][3] = { { 1.f } };
        luma[index] = 0.f;
        for(int ii = 0; ii < 3; ii++)
          for(int jj = 0; jj < 3; jj++)
            luma[index] += luma_ds[(i - 1 + ii) * buf_width + (j - 1 + jj)] * kernel[ii][jj];
      }
    }

  // Anti-aliasing
  if(dt_box_mean(luma, buf_height, buf_width, 1, 3, 1) != 0)
  {
    err = 1;
    goto error;
  }

  // Postfilter to connect isolated dots and draw lines
  if(fast_eigf_surface_blur(luma, buf_width, buf_height, 12, 0.000005f, 1, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 1.0f) != 0)
  {
    err = 1;
    goto error;
  }

  // Compute the laplacian mean over the picture
  float TV_sum = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(luma, buf_height, buf_width) \
schedule(static) collapse(2) aligned(luma:64) reduction(+:TV_sum)
#endif
  for(size_t i = 8; i < buf_height - 8; ++i)
    for(size_t j = 8; j < buf_width - 8; ++j)
      TV_sum += luma[i * buf_width + j] / ((float)(buf_height - 16) * (float)(buf_width - 16));

  // Compute the standard deviation
  float sigma = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, luma, buf_height, buf_width, TV_sum) \
schedule(static) collapse(2) aligned(focus_peaking, luma:64) reduction(+:sigma)
#endif
  for(size_t i = 8; i < buf_height - 8; ++i)
    for(size_t j = 8; j < buf_width - 8; ++j)
       sigma += sqf(luma[i * buf_width + j] - TV_sum) / ((float)(buf_height - 16) * (float)(buf_width - 16));

  sigma = sqrtf(sigma);

  // Set the sharpness thresholds
  const float six_sigma = TV_sum + 4.f * sigma;
  const float four_sigma = TV_sum + 3.f * sigma;
  const float two_sigma = TV_sum + 2.f * sigma;

  // Prepare the focus-peaking image overlay
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(focus_peaking, luma, buf_height, buf_width, six_sigma, four_sigma, two_sigma) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      static const uint8_t yellow[4] = { 0, 255, 255, 255 };
      static const uint8_t green[4]  = { 0, 255,   0, 255 };
      static const uint8_t blue[4]   = { 255, 0,   0, 255 };

      const size_t index = (i * buf_width + j) * 4;
      const float TV = luma[(i * buf_width + j)];

      if(TV > six_sigma)
      {
        // Very sharp : paint yellow, BGR = (0, 255, 255)
        for_four_channels(c) focus_peaking[index + c] = yellow[c];
      }
      else if(TV > four_sigma)
      {
        // Mediun sharp : paint green, BGR = (0, 255, 0)
        for_four_channels(c) focus_peaking[index + c] = green[c];
      }
      else if(TV > two_sigma)
      {
        // Little sharp : paint blue, BGR = (255, 0, 0)
        for_four_channels(c) focus_peaking[index + c] = blue[c];
      }
      else
      {
        // Not sharp enough : paint 0
        for_four_channels(c) focus_peaking[index + c] = 0;
      }
    }

  // draw the focus peaking overlay
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, buf_width, buf_height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)focus_peaking,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 buf_width, buf_height,
                                                                 cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, buf_width));
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_surface(cr, surface, 0.0, 0.0);
  cairo_pattern_set_filter(cairo_get_source (cr), darktable.gui->filter_image);
  cairo_fill(cr);
  cairo_restore(cr);

  // cleanup
  cairo_surface_destroy(surface);

error:
  if(focus_peaking) dt_pixelpipe_cache_free_align(focus_peaking);
error_early:
  if(luma) dt_pixelpipe_cache_free_align(luma);
  if(luma_ds) dt_pixelpipe_cache_free_align(luma_ds);
  return err;
}
