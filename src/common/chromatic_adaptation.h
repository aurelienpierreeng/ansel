/*
    This file is part of darktable,
    Copyright (C) 2020-2021, 2025 Aurélien PIERRE.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Sakari Kapanen.
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

#include "common/math.h"
#include "common/colorspaces_inline_conversions.h"

typedef enum dt_adaptation_t
{
  DT_ADAPTATION_LINEAR_BRADFORD = 0, // $DESCRIPTION: "linear Bradford (ICC v4)"
  DT_ADAPTATION_CAT16           = 1, // $DESCRIPTION: "CAT16 (CIECAM16)"
  DT_ADAPTATION_FULL_BRADFORD   = 2, // $DESCRIPTION: "non-linear Bradford"
  DT_ADAPTATION_XYZ             = 3, // $DESCRIPTION: "XYZ"
  DT_ADAPTATION_RGB             = 4, // $DESCRIPTION: "none (bypass)"
  DT_ADAPTATION_LAST
} dt_adaptation_t;


// modified LMS cone response space for Bradford transform
// explanation here : https://onlinelibrary.wiley.com/doi/pdf/10.1002/9781119021780.app3
// but coeffs are wrong in the above, so they come from :
// http://www2.cmp.uea.ac.uk/Research/compvis/Papers/FinSuss_COL00.pdf
// At any time, ensure XYZ_to_LMS is the exact matrice inverse of LMS_to_XYZ
const dt_colormatrix_t XYZ_to_Bradford_LMS = { {  0.8951f,  0.2664f, -0.1614f, 0.f },
                                               { -0.7502f,  1.7135f,  0.0367f, 0.f },
                                               {  0.0389f, -0.0685f,  1.0296f, 0.f } };

const dt_colormatrix_t Bradford_LMS_to_XYZ = { {  0.9870f, -0.1471f,  0.1600f, 0.f },
                                               {  0.4323f,  0.5184f,  0.0493f, 0.f },
                                               { -0.0085f,  0.0400f,  0.9685f, 0.f } };

static const dt_colormatrix_t XYZ_to_Bradford_LMS_transposed = { {  0.8951f, -0.7502f,  0.0389f, 0.f },
                                                                  {  0.2664f,  1.7135f, -0.0685f, 0.f },
                                                                  { -0.1614f,  0.0367f,  1.0296f, 0.f } };

static const dt_colormatrix_t Bradford_LMS_to_XYZ_transposed = { {  0.9870f,  0.4323f, -0.0085f, 0.f },
                                                                  { -0.1471f,  0.5184f,  0.0400f, 0.f },
                                                                  {  0.1600f,  0.0493f,  0.9685f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_aligned_pixel_simd_t convert_XYZ_to_bradford_LMS(const dt_aligned_pixel_simd_t XYZ)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  return dt_mat3x4_mul_vec4(XYZ,
                            dt_colormatrix_row_to_simd(XYZ_to_Bradford_LMS_transposed, 0),
                            dt_colormatrix_row_to_simd(XYZ_to_Bradford_LMS_transposed, 1),
                            dt_colormatrix_row_to_simd(XYZ_to_Bradford_LMS_transposed, 2));
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_aligned_pixel_simd_t convert_bradford_LMS_to_XYZ(const dt_aligned_pixel_simd_t LMS)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  return dt_mat3x4_mul_vec4(LMS,
                            dt_colormatrix_row_to_simd(Bradford_LMS_to_XYZ_transposed, 0),
                            dt_colormatrix_row_to_simd(Bradford_LMS_to_XYZ_transposed, 1),
                            dt_colormatrix_row_to_simd(Bradford_LMS_to_XYZ_transposed, 2));
}


// modified LMS cone response for CAT16, from CIECAM16
// reference : https://ntnuopen.ntnu.no/ntnu-xmlui/bitstream/handle/11250/2626317/CCIW-23.pdf?sequence=1
// At any time, ensure XYZ_to_LMS is the exact matrice inverse of LMS_to_XYZ
const dt_colormatrix_t XYZ_to_CAT16_LMS = { {  0.401288f, 0.650173f, -0.051461f, 0.f },
                                            { -0.250268f, 1.204414f,  0.045854f, 0.f },
                                            { -0.002079f, 0.048952f,  0.953127f, 0.f } };

const dt_colormatrix_t CAT16_LMS_to_XYZ = { {  1.862068f, -1.011255f,  0.149187f, 0.f },
                                            {  0.38752f ,  0.621447f, -0.008974f, 0.f },
                                            { -0.015841f, -0.034123f,  1.049964f, 0.f } };

static const dt_colormatrix_t XYZ_to_CAT16_LMS_transposed = { {  0.401288f, -0.250268f, -0.002079f, 0.f },
                                                               {  0.650173f,  1.204414f,  0.048952f, 0.f },
                                                               { -0.051461f,  0.045854f,  0.953127f, 0.f } };

static const dt_colormatrix_t CAT16_LMS_to_XYZ_transposed = { {  1.862068f,  0.38752f , -0.015841f, 0.f },
                                                               { -1.011255f,  0.621447f, -0.034123f, 0.f },
                                                               {  0.149187f, -0.008974f,  1.049964f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_aligned_pixel_simd_t convert_XYZ_to_CAT16_LMS(const dt_aligned_pixel_simd_t XYZ)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  return dt_mat3x4_mul_vec4(XYZ,
                            dt_colormatrix_row_to_simd(XYZ_to_CAT16_LMS_transposed, 0),
                            dt_colormatrix_row_to_simd(XYZ_to_CAT16_LMS_transposed, 1),
                            dt_colormatrix_row_to_simd(XYZ_to_CAT16_LMS_transposed, 2));
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline dt_aligned_pixel_simd_t convert_CAT16_LMS_to_XYZ(const dt_aligned_pixel_simd_t LMS)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  return dt_mat3x4_mul_vec4(LMS,
                            dt_colormatrix_row_to_simd(CAT16_LMS_to_XYZ_transposed, 0),
                            dt_colormatrix_row_to_simd(CAT16_LMS_to_XYZ_transposed, 1),
                            dt_colormatrix_row_to_simd(CAT16_LMS_to_XYZ_transposed, 2));
}


#ifdef _OPENMP
#pragma omp declare simd uniform(kind)
#endif
static inline dt_aligned_pixel_simd_t convert_any_LMS_to_XYZ(const dt_aligned_pixel_simd_t LMS,
                                                             const dt_adaptation_t kind)
{
  switch(kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
      return convert_bradford_LMS_to_XYZ(LMS);
    case DT_ADAPTATION_CAT16:
      return convert_CAT16_LMS_to_XYZ(LMS);
    case DT_ADAPTATION_XYZ:
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      return LMS;
  }
}


#ifdef _OPENMP
#pragma omp declare simd uniform(kind)
#endif
static inline dt_aligned_pixel_simd_t convert_any_XYZ_to_LMS(const dt_aligned_pixel_simd_t XYZ,
                                                             const dt_adaptation_t kind)
{
  switch(kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
      return convert_XYZ_to_bradford_LMS(XYZ);
    case DT_ADAPTATION_CAT16:
      return convert_XYZ_to_CAT16_LMS(XYZ);
    case DT_ADAPTATION_XYZ:
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      return XYZ;
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(RGB, LMS:16) uniform(kind)
#endif
static inline void convert_any_LMS_to_RGB(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t RGB, dt_adaptation_t kind)
{
  // helper function switching internally to the proper conversion
  dt_aligned_pixel_t XYZ = { 0.f };
  dt_store_simd_aligned(XYZ, convert_any_LMS_to_XYZ(dt_load_simd_aligned(LMS), kind));

  // Fixme : convert to RGB display space instead of sRGB but first the display profile should be global in dt,
  // not confined to colorout where it gets created/destroyed all the time.
  dt_XYZ_to_Rec709_D65(XYZ, RGB);

  // Handle gamut clipping
  float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
  for(int c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c] / max_RGB, 0.f);

}


/* Bradford adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t bradford_adapt_D65(const dt_aligned_pixel_simd_t lms_in,
                                                         const dt_aligned_pixel_simd_t origin_illuminant,
                                                         const float p, const int full)
{
  static const dt_aligned_pixel_simd_t D65 = { 0.941238f, 1.040633f, 1.088932f, 0.f };
  dt_aligned_pixel_simd_t temp = lms_in / origin_illuminant;
  if(full) temp[2] = (temp[2] > 0.f) ? powf(temp[2], p) : temp[2];
  return D65 * temp;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t bradford_adapt_D50(const dt_aligned_pixel_simd_t lms_in,
                                                         const dt_aligned_pixel_simd_t origin_illuminant,
                                                         const float p, const int full)
{
  static const dt_aligned_pixel_simd_t D50 = { 0.996078f, 1.020646f, 0.818155f, 0.f };
  dt_aligned_pixel_simd_t temp = lms_in / origin_illuminant;
  if(full) temp[2] = (temp[2] > 0.f) ? powf(temp[2], p) : temp[2];
  return D50 * temp;
}


/* CAT16 adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t CAT16_adapt_D65(const dt_aligned_pixel_simd_t lms_in,
                                                      const dt_aligned_pixel_simd_t origin_illuminant,
                                                      const float D, const int full)
{
  static const dt_aligned_pixel_simd_t D65 = { 0.97553267f, 1.01647859f, 1.0848344f, 0.f };
  return full ? lms_in * D65 / origin_illuminant
              : lms_in * (dt_simd_set1(D) * D65 / origin_illuminant + dt_simd_set1(1.f - D));
}


#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t CAT16_adapt_D50(const dt_aligned_pixel_simd_t lms_in,
                                                      const dt_aligned_pixel_simd_t origin_illuminant,
                                                      const float D, const int full)
{
  static const dt_aligned_pixel_simd_t D50 = { 0.994535f, 1.000997f, 0.833036f, 0.f };
  return full ? lms_in * D50 / origin_illuminant
              : lms_in * (dt_simd_set1(D) * D50 / origin_illuminant + dt_simd_set1(1.f - D));
}

/* XYZ adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t XYZ_adapt_D65(const dt_aligned_pixel_simd_t lms_in,
                                                    const dt_aligned_pixel_simd_t origin_illuminant)
{
  static const dt_aligned_pixel_simd_t D65 = { 0.9504285453771807f, 1.0f, 1.0889003707981277f, 0.f };
  return lms_in * D65 / origin_illuminant;
}

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant)
#endif
static inline dt_aligned_pixel_simd_t XYZ_adapt_D50(const dt_aligned_pixel_simd_t lms_in,
                                                    const dt_aligned_pixel_simd_t origin_illuminant)
{
  static const dt_aligned_pixel_simd_t D50 = { 0.9642119944211994f, 1.0f, 0.8251882845188288f, 0.f };
  return lms_in * D50 / origin_illuminant;
}

/* Pre-solved matrices to adjust white point for triplets in CIE XYZ 1931 2° observer */

const dt_colormatrix_t XYZ_D50_to_D65_CAT16
    = { { 9.89466254e-01f, -4.00304626e-02f, 4.40530317e-02f, 0.f },
        { -5.40518733e-03f, 1.00666069e+00f, -1.75551955e-03f, 0.f },
        { -4.03920992e-04f, 1.50768030e-02f, 1.30210211e+00f, 0.f } };

const dt_colormatrix_t XYZ_D50_to_D65_Bradford
    = { { 0.95547342f, -0.02309845f, 0.06325924f, 0.f },
        { -0.02836971f, 1.00999540f, 0.02104144f, 0.f },
        { 0.01231401f, -0.02050765f, 1.33036593f, 0.f } };

const dt_colormatrix_t XYZ_D65_to_D50_CAT16
    = { { 1.01085433e+00f, 4.07086103e-02f, -3.41445825e-02f, 0.f },
        { 5.42814201e-03f, 9.93581926e-01f, 1.15592039e-03f, 0.f },
        { 2.50722468e-04f, -1.14918759e-02f, 7.67964947e-01f, 0.f } };

const dt_colormatrix_t XYZ_D65_to_D50_Bradford
    = { { 1.04792979f, 0.02294687f, -0.05019227f, 0.f },
        { 0.02962781f, 0.99043443f, -0.0170738f, 0.f },
        { -0.00924304f, 0.01505519f, 0.75187428f, 0.f } };

static const dt_colormatrix_t XYZ_D50_to_D65_CAT16_transposed
    = { { 9.89466254e-01f, -5.40518733e-03f, -4.03920992e-04f, 0.f },
        { -4.00304626e-02f, 1.00666069e+00f, 1.50768030e-02f, 0.f },
        { 4.40530317e-02f, -1.75551955e-03f, 1.30210211e+00f, 0.f } };

static const dt_colormatrix_t XYZ_D65_to_D50_CAT16_transposed
    = { { 1.01085433e+00f, 5.42814201e-03f, 2.50722468e-04f, 0.f },
        { 4.07086103e-02f, 9.93581926e-01f, -1.14918759e-02f, 0.f },
        { -3.41445825e-02f, 1.15592039e-03f, 7.67964947e-01f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_in, XYZ_out:16)
#endif
static inline void XYZ_D50_to_D65(const dt_aligned_pixel_t XYZ_in, dt_aligned_pixel_t XYZ_out)
{
  dt_apply_transposed_color_matrix(XYZ_in, XYZ_D50_to_D65_CAT16_transposed, XYZ_out);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_in, XYZ_out:16)
#endif
static inline void XYZ_D65_to_D50(const dt_aligned_pixel_t XYZ_in, dt_aligned_pixel_t XYZ_out)
{
  dt_apply_transposed_color_matrix(XYZ_in, XYZ_D65_to_D50_CAT16_transposed, XYZ_out);
}

/* Helper function to directly chroma-adapt a pixel in CIE XYZ 1931 2° */

static inline dt_aligned_pixel_simd_t _downscale_vector_simd(const dt_aligned_pixel_simd_t vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  return vector / dt_simd_set1(valid ? (scaling + NORM_MIN) : NORM_MIN);
}

static inline dt_aligned_pixel_simd_t _upscale_vector_simd(const dt_aligned_pixel_simd_t vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  return vector * dt_simd_set1(valid ? (scaling + NORM_MIN) : NORM_MIN);
}

static inline dt_aligned_pixel_simd_t chroma_adapt_pixel(const dt_aligned_pixel_simd_t in,
                                                         const dt_aligned_pixel_simd_t illuminant,
                                                         const dt_adaptation_t adaptation, const float p)
{

  /* WE START IN XYZ */
  const float Y = in[1];

  switch(adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
      return convert_bradford_LMS_to_XYZ(
          _upscale_vector_simd(bradford_adapt_D50(
                                   _downscale_vector_simd(convert_XYZ_to_bradford_LMS(in), Y),
                                   illuminant, p, TRUE),
                               Y));
    case DT_ADAPTATION_LINEAR_BRADFORD:
      return convert_bradford_LMS_to_XYZ(
          _upscale_vector_simd(bradford_adapt_D50(
                                   _downscale_vector_simd(convert_XYZ_to_bradford_LMS(in), Y),
                                   illuminant, p, FALSE),
                               Y));
    case DT_ADAPTATION_CAT16:
      return convert_CAT16_LMS_to_XYZ(
          _upscale_vector_simd(CAT16_adapt_D50(
                                   _downscale_vector_simd(convert_XYZ_to_CAT16_LMS(in), Y),
                                   illuminant, 1.0f, TRUE),
                               Y));
    case DT_ADAPTATION_XYZ:
      return _upscale_vector_simd(XYZ_adapt_D50(_downscale_vector_simd(in, Y), illuminant), Y);
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      return in;
  }
}

/* Helper to get the D50 white point coordinates in LMS spaces */
static inline void convert_D50_to_LMS(const dt_adaptation_t adaptation, dt_aligned_pixel_t D50)
{
  switch(adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      dt_store_simd_aligned(D50, (dt_aligned_pixel_simd_t){ 0.996078f, 1.020646f, 0.818155f, 0.f });
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      dt_store_simd_aligned(D50, (dt_aligned_pixel_simd_t){ 0.994535f, 1.000997f, 0.833036f, 0.f });
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      dt_store_simd_aligned(D50, (dt_aligned_pixel_simd_t){ 0.9642119944211994f, 1.0f, 0.8251882845188288f, 0.f });
      break;
    }
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      dt_store_simd_aligned(D50, (dt_aligned_pixel_simd_t){ 1.f, 1.f, 1.f, 0.f });
      break;
    }
  }
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
