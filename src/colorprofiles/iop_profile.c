/*
    This file is part of darktable,
    Copyright (C) 2018-2021, 2023, 2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Aldric Renaudin.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 Jacques Le Clerc.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2019, 2021 Philippe Weyland.
    Copyright (C) 2019, 2021 Sakari Kapanen.
    Copyright (C) 2019 Tobias Ellinghaus.
    Copyright (C) 2020-2021 Dan Torop.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 paolodepetrillo.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2024 Alynx Zhou.
    
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
#ifdef HAVE_CONFIG_H
/* The colour-profile maths: the profile struct's lifecycle, the LCMS2 and matrix transform
 * workers, and the OpenCL profile-parameter plumbing. Nothing here knows about dt_develop_t,
 * dt_dev_pixelpipe_t or dt_iop_module_t.
 *
 * Split out of develop/iop_profile.c. That file declared its API in common/, forward-declaring
 * three develop/ types to do it, and pixel/eaw.c, pixel/rgb_norms.h and
 * pixel/colorequal_shared.h all needed the low-level half -- so moving the whole thing up to
 * develop/ traded one layering problem for another. This half belongs at layer 2 with its
 * consumers; the pipeline-facing half stayed in develop/iop_profile.c.
 *
 * Five helpers that were static are now dt_ioppr_* and declared in the header: the develop/
 * half calls them, and they are the shared core rather than either side's private business.
 */

#include "develop/imageop_math.h"   // dt_iop_estimate_exp: pure curve fitting, misfiled at layer 5
#include "common/logging.h"
#include "common/opencl.h"

#include <glib/gi18n.h>
#include "config.h"
#endif

#include "colorprofiles/colorspaces.h"
#include "common/pixelpipe_cache_alloc.h"
#include "colorprofiles/iop_profile.h"
#include "math/matrices.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system/target_clones.h"
#include "common/times.h"


inline __attribute__((always_inline)) void dt_ioppr_mark_as_nonmatrix_profile(dt_iop_order_iccprofile_info_t *const profile_info)
{
  profile_info->matrix_in[0][0] = NAN;
  profile_info->matrix_in_transposed[0][0] = NAN;
  profile_info->matrix_out[0][0] = NAN;
  profile_info->matrix_out_transposed[0][0] = NAN;
}

__DT_CLONE_TARGETS__
void dt_ioppr_clear_lut_curves(dt_iop_order_iccprofile_info_t *const profile_info)
{
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i][0] = -1.0f;
  }
}

static void _transform_from_to_rgb_lab_lcms2(const float *const image_in, float *const image_out, const int width,
                                             const int height, const dt_colorspaces_color_profile_type_t type,
                                             const char *filename, const int intent, const int direction)
{
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *rgb_profile = NULL;
  cmsHPROFILE *lab_profile = NULL;

  /* Resolve first, then lock the entry we resolved: the entry pointer is stable for the
   * process, its ->profile is not, so the handle is read only under the lock. */
  const dt_colorspaces_color_profile_t *profile
      = (type != DT_COLORSPACE_NONE)
            ? dt_colorspaces_get_profile(type, filename, DT_PROFILE_ROLE_ANY)
            : dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_ROLE_WORKING);

  dt_colorspaces_lock_profile(profile);
  if(profile) rgb_profile = profile->profile;
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
        fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
                (char)(rgb_color_space>>24),
                (char)(rgb_color_space>>16),
                (char)(rgb_color_space>>8),
                (char)(rgb_color_space));
        rgb_profile = NULL;
    }
  }
  if(IS_NULL_PTR(rgb_profile))
  {
    rgb_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_ROLE_WORKING)->profile;
    fprintf(stderr, _("unsupported working profile %s has been replaced by Rec2020 RGB!\n"), filename);
  }

  lab_profile = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_ROLE_ANY)->profile;

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_LabA_FLT;

  if(direction == 1) // rgb --> lab
  {
    input_profile = rgb_profile;
    input_format = TYPE_RGBA_FLT;
    output_profile = lab_profile;
    output_format = TYPE_LabA_FLT;
  }
  else // lab -->rgb
  {
    input_profile = lab_profile;
    input_format = TYPE_LabA_FLT;
    output_profile = rgb_profile;
    output_format = TYPE_RGBA_FLT;
  }

  xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  dt_colorspaces_unlock_profile(profile);

  if(xform)
  {
    dt_colorspaces_transform_rgba_float_image(xform, image_in, image_out, width, height);
  }
  else
    fprintf(stderr, "[_transform_from_to_rgb_lab_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

static inline __attribute__((always_inline)) void _transform_rgb_to_rgb_lcms2(const float *const image_in, float *const image_out, const int width,
                                        const int height, const dt_colorspaces_color_profile_type_t type_from,
                                        const char *filename_from,
                                        const dt_colorspaces_color_profile_type_t type_to, const char *filename_to,
                                        const int intent)
{
  cmsHTRANSFORM *xform = NULL;
  cmsHPROFILE *from_rgb_profile = NULL;
  cmsHPROFILE *to_rgb_profile = NULL;

  /* Resolve both, then lock both, then read the handles. Two read locks in a fixed order;
   * readers do not exclude readers, so the pair cannot deadlock against a caller taking
   * them the other way round. */
  const dt_colorspaces_color_profile_t *profile_from
      = (type_from != DT_COLORSPACE_NONE)
            ? dt_colorspaces_get_profile(type_from, filename_from, DT_PROFILE_ROLE_ANY)
            : NULL;
  const dt_colorspaces_color_profile_t *profile_to
      = (type_to != DT_COLORSPACE_NONE)
            ? dt_colorspaces_get_profile(type_to, filename_to, DT_PROFILE_ROLE_ANY)
            : NULL;

  dt_colorspaces_lock_profile(profile_from);
  dt_colorspaces_lock_profile(profile_to);

  if(type_from != DT_COLORSPACE_NONE)
  {
    if(profile_from) from_rgb_profile = profile_from->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid from profile\n");
  }

  if(type_to != DT_COLORSPACE_NONE)
  {
    if(profile_to) to_rgb_profile = profile_to->profile;
  }
  else
  {
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] invalid to profile\n");
  }

  if(from_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(from_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      from_rgb_profile = NULL;
    }
  }
  if(to_rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(to_rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space >> 24), (char)(rgb_color_space >> 16), (char)(rgb_color_space >> 8),
              (char)(rgb_color_space));
      to_rgb_profile = NULL;
    }
  }

  cmsHPROFILE *input_profile = NULL;
  cmsHPROFILE *output_profile = NULL;
  cmsUInt32Number input_format = TYPE_RGBA_FLT;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  input_profile = from_rgb_profile;
  input_format = TYPE_RGBA_FLT;
  output_profile = to_rgb_profile;
  output_format = TYPE_RGBA_FLT;

  if(input_profile && output_profile)
    xform = cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, 0);

  dt_colorspaces_unlock_profile(profile_to);
  dt_colorspaces_unlock_profile(profile_from);

  if(xform)
  {
    dt_colorspaces_transform_rgba_float_image(xform, image_in, image_out, width, height);
  }
  else
    fprintf(stderr, "[_transform_rgb_to_rgb_lcms2] cannot create transform\n");

  if(xform) cmsDeleteTransform(xform);
}

void dt_ioppr_transform_lcms2(const char *op, const char *multi_name, const float *const image_in, float *const image_out,
                             const int width, const int height,
                             const dt_iop_colorspace_type_t cst_from, const dt_iop_colorspace_type_t cst_to,
                             dt_iop_colorspace_type_t *converted_cst,
                             const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(dt_iop_colorspace_is_rgb(cst_from) && cst_to == IOP_CS_LAB)
  {
    dt_print(DT_DEBUG_DEV,
             "[dt_ioppr_transform_lcms2] transfoming from RGB to Lab (%s %s)\n", op, multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, 1);
  }
  else if(cst_from == IOP_CS_LAB && dt_iop_colorspace_is_rgb(cst_to))
  {
    dt_print(DT_DEBUG_DEV,
             "[dt_ioppr_transform_lcms2] transfoming from Lab to RGB (%s %s)\n", op, multi_name);
    _transform_from_to_rgb_lab_lcms2(image_in, image_out, width, height, profile_info->type,
                                     profile_info->filename, profile_info->intent, -1);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[dt_ioppr_transform_lcms2] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}

static inline __attribute__((always_inline)) void _transform_lcms2_rgb(const float *const image_in, float *const image_out, const int width,
                                        const int height,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                        const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  _transform_rgb_to_rgb_lcms2(image_in, image_out, width, height, profile_info_from->type,
                              profile_info_from->filename, profile_info_to->type, profile_info_to->filename,
                              profile_info_to->intent);
}


inline int dt_ioppr_init_unbounded_coeffs(float *const lutr, float *const lutg, float *const lutb,
    float *const unbounded_coeffsr, float *const unbounded_coeffsg, float *const unbounded_coeffsb, const int lutsize)
{
  int nonlinearlut = 0;
  float *lut[3] = { lutr, lutg, lutb };
  float *unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };

  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(lut[k][0] >= 0.0f)
    {
      const dt_aligned_pixel_t x = { 0.7f, 0.8f, 0.9f, 1.0f };
      const dt_aligned_pixel_t y = { extrapolate_lut(lut[k], x[0], lutsize),
                                     extrapolate_lut(lut[k], x[1], lutsize),
                                     extrapolate_lut(lut[k], x[2], lutsize),
                                     extrapolate_lut(lut[k], x[3], lutsize) };
      dt_iop_estimate_exp(x, y, 4, unbounded_coeffs[k]);

      nonlinearlut++;
    }
    else
      unbounded_coeffs[k][0] = -1.0f;
  }

  return nonlinearlut;
}


static inline void _apply_tonecurves(const float *const image_in, float *const image_out,
                                     const int width, const int height,
                                     const float *const restrict lutr,
                                     const float *const restrict lutg,
                                     const float *const restrict lutb,
                                     const float *const restrict unbounded_coeffsr,
                                     const float *const restrict unbounded_coeffsg,
                                     const float *const restrict unbounded_coeffsb,
                                     const int lutsize)
{
  const int ch = 4;
  const float *const lut[3] = { lutr, lutg, lutb };
  const float *const unbounded_coeffs[3] = { unbounded_coeffsr, unbounded_coeffsg, unbounded_coeffsb };
  const size_t stride = (size_t)ch * width * height;

  // do we have any lut to apply, or is this a linear profile?
  if((lut[0][0] >= 0.0f) && (lut[1][0] >= 0.0f) && (lut[2][0] >= 0.0f))
  {
    __OMP_PARALLEL_FOR__(collapse(2))
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++) // for_each_channel doesn't vectorize, and some code needs image_out[3] preserved
      {
        image_out[k + c] = dt_ioppr_eval_trc(image_in[k + c], lut[c], unbounded_coeffs[c], lutsize);
      }
    }
  }
  else if((lut[0][0] >= 0.0f) || (lut[1][0] >= 0.0f) || (lut[2][0] >= 0.0f))
  {
    __OMP_PARALLEL_FOR__(collapse(2))
    for(size_t k = 0; k < stride; k += ch)
    {
      for(int c = 0; c < 3; c++) // for_each_channel doesn't vectorize, and some code needs image_out[3] preserved
      {
        if(lut[c][0] >= 0.0f)
        {
          image_out[k + c] = dt_ioppr_eval_trc(image_in[k + c], lut[c], unbounded_coeffs[c], lutsize);
        }
      }
    }
  }
}


__DT_CLONE_TARGETS__
static inline void _transform_rgb_to_lab_matrix(const float *const restrict image_in, float *const restrict image_out,
                                                const int width, const int height,
                                                const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const dt_colormatrix_t *matrix_ptr = &profile_info->matrix_in_transposed;
  const dt_aligned_pixel_simd_t m0 = dt_colormatrix_row_to_simd(*matrix_ptr, 0);
  const dt_aligned_pixel_simd_t m1 = dt_colormatrix_row_to_simd(*matrix_ptr, 1);
  const dt_aligned_pixel_simd_t m2 = dt_colormatrix_row_to_simd(*matrix_ptr, 2);

  if(profile_info->nonlinearlut)
  {
    // TODO : maybe optimize that path like _transform_matrix_rgb
    _apply_tonecurves(image_in, image_out, width, height, profile_info->lut_in[0], profile_info->lut_in[1],
                      profile_info->lut_in[2], profile_info->unbounded_coeffs_in[0],
                      profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2],
                      profile_info->lutsize);
    __OMP_PARALLEL_FOR_SIMD__(aligned(image_out:64))
    for(size_t y = 0; y < stride; y += ch)
    {
      float *const restrict in = __builtin_assume_aligned(image_out + y, 16);
      dt_aligned_pixel_t xyz;
      const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in);
      dt_store_simd_aligned(xyz, dt_mat3x4_mul_vec4(vin, m0, m1, m2));
      dt_XYZ_to_Lab(xyz, in);
    }
  }
  else
  {
    __OMP_PARALLEL_FOR_SIMD__(aligned(image_in, image_out:64))
    for(size_t y = 0; y < stride; y += ch)
    {
      const float *const restrict in = __builtin_assume_aligned(image_in + y, 16);
      float *const restrict out = __builtin_assume_aligned(image_out + y, 16);

      dt_aligned_pixel_t xyz;
      const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in);
      dt_store_simd_aligned(xyz, dt_mat3x4_mul_vec4(vin, m0, m1, m2));
      dt_XYZ_to_Lab(xyz, out);
    }
  }
}


__DT_CLONE_TARGETS__
static inline void _transform_lab_to_rgb_matrix(const float *const image_in, float *const image_out, const int width,
                                         const int height,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;
  const int use_nontemporal = !profile_info->nonlinearlut;
  const dt_colormatrix_t *matrix_ptr = &profile_info->matrix_out_transposed;
  const dt_aligned_pixel_simd_t m0 = dt_colormatrix_row_to_simd(*matrix_ptr, 0);
  const dt_aligned_pixel_simd_t m1 = dt_colormatrix_row_to_simd(*matrix_ptr, 1);
  const dt_aligned_pixel_simd_t m2 = dt_colormatrix_row_to_simd(*matrix_ptr, 2);
  __OMP_PARALLEL_FOR__()
  for(size_t y = 0; y < stride; y += ch)
  {
    const float *const restrict in = __builtin_assume_aligned(image_in + y, 16);
    float *const restrict out = __builtin_assume_aligned(image_out + y, 16);

    dt_aligned_pixel_t xyz;
    const float alpha = in[3]; // some code does in-place conversions and relies on alpha being preserved
    dt_Lab_to_XYZ(in, xyz);
    const dt_aligned_pixel_simd_t vxyz = dt_load_simd_aligned(xyz);
    dt_aligned_pixel_simd_t rgb = dt_mat3x4_mul_vec4(vxyz, m0, m1, m2);
    rgb[3] = alpha;
    if(use_nontemporal)
      dt_store_simd_nontemporal(out, rgb);
    else
      dt_store_simd_aligned(out, rgb);
  }

  if(use_nontemporal)
    dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output

  if(profile_info->nonlinearlut)
  {
    // TODO : maybe optimize that path like _transform_matrix_rgb
    _apply_tonecurves(image_out, image_out, width, height, profile_info->lut_out[0], profile_info->lut_out[1],
                      profile_info->lut_out[2], profile_info->unbounded_coeffs_out[0],
                      profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2],
                      profile_info->lutsize);
  }
}


__DT_CLONE_TARGETS__
static inline void _transform_matrix_rgb(const float *const restrict image_in,
                                         float *const restrict image_out,
                                         const int width, const int height,
                                         const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                         const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  const int ch = 4;
  const size_t stride = (size_t)width * height * ch;

  // RGB -> XYZ -> RGB are 2 matrices products, they can be premultiplied globally ahead
  // and put in a new matrix. then we spare one matrix product per pixel.
  dt_colormatrix_t _matrix;
  dt_colormatrix_mul(_matrix, profile_info_to->matrix_out, profile_info_from->matrix_in);
  dt_colormatrix_t matrix;
  transpose_3xSSE(_matrix, matrix);
  const dt_aligned_pixel_simd_t m0 = dt_colormatrix_row_to_simd(matrix, 0);
  const dt_aligned_pixel_simd_t m1 = dt_colormatrix_row_to_simd(matrix, 1);
  const dt_aligned_pixel_simd_t m2 = dt_colormatrix_row_to_simd(matrix, 2);

  if(profile_info_from->nonlinearlut || profile_info_to->nonlinearlut)
  {
    const int use_nontemporal = !profile_info_to->nonlinearlut;
    const int run_lut_in[3] DT_ALIGNED_PIXEL= { (profile_info_from->lut_in[0][0] >= 0.0f),
                                                (profile_info_from->lut_in[1][0] >= 0.0f),
                                                (profile_info_from->lut_in[2][0] >= 0.0f) };

    const int run_lut_out[3] DT_ALIGNED_PIXEL = { (profile_info_to->lut_out[0][0] >= 0.0f),
                                                  (profile_info_to->lut_out[1][0] >= 0.0f),
                                                  (profile_info_to->lut_out[2][0] >= 0.0f) };
    __OMP_PARALLEL_FOR__()
    for(size_t y = 0; y < stride; y += 4)
    {
      const float *const restrict in = __builtin_assume_aligned(image_in + y, 16);
      float *const restrict out = __builtin_assume_aligned(image_out + y, 16);
      dt_aligned_pixel_t rgb;

      // linearize if non-linear input
      if(profile_info_from->nonlinearlut)
      {
        for(size_t c = 0; c < 3; c++)
        {
          rgb[c] = (run_lut_in[c]
                    ? dt_ioppr_eval_trc(in[c], profile_info_from->lut_in[c],
                                        profile_info_from->unbounded_coeffs_in[c], profile_info_from->lutsize)
                    : in[c]);
        }
      }
      else
      {
        for_each_channel(c)
          rgb[c] = in[c];
      }

      if(profile_info_to->nonlinearlut)
      {
        // convert color space
        dt_aligned_pixel_t temp;
        const dt_aligned_pixel_simd_t vrgb = dt_load_simd_aligned(rgb);
        dt_store_simd_aligned(temp, dt_mat3x4_mul_vec4(vrgb, m0, m1, m2));

        // de-linearize non-linear output
        for(size_t c = 0; c < 3; c++)
        {
          out[c] = (run_lut_out[c]
                    ? dt_ioppr_eval_trc(temp[c], profile_info_to->lut_out[c],
                                        profile_info_to->unbounded_coeffs_out[c], profile_info_to->lutsize)
                    : temp[c]);
        }
      }
      else
      {
        // convert color space
        const dt_aligned_pixel_simd_t vrgb = dt_load_simd_aligned(rgb);
        if(use_nontemporal)
          dt_store_simd_nontemporal(out, dt_mat3x4_mul_vec4(vrgb, m0, m1, m2));
        else
          dt_store_simd_aligned(out, dt_mat3x4_mul_vec4(vrgb, m0, m1, m2));
      }
    }

    if(use_nontemporal)
      dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
  }
  else
  {
    __OMP_PARALLEL_FOR__()
    for(size_t y = 0; y < stride; y += 4)
    {
      const float *const restrict in = __builtin_assume_aligned(image_in + y, 16);
      float *const restrict out = __builtin_assume_aligned(image_out + y, 16);

      const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in);
      dt_store_simd_nontemporal(out, dt_mat3x4_mul_vec4(vin, m0, m1, m2));
    }
    dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
  }
}


inline void dt_ioppr_transform_matrix(const char *op, const char *multi_name,
                                     const float *const restrict image_in,
                                     float *const restrict image_out,
                                     const int width, const int height,
                                     const dt_iop_colorspace_type_t cst_from,
                                     const dt_iop_colorspace_type_t cst_to,
                                     dt_iop_colorspace_type_t *converted_cst,
                                     const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }

  *converted_cst = cst_to;

  if(dt_iop_colorspace_is_rgb(cst_from) && cst_to == IOP_CS_LAB)
  {
    _transform_rgb_to_lab_matrix(image_in, image_out, width, height, profile_info);
  }
  else if(cst_from == IOP_CS_LAB && dt_iop_colorspace_is_rgb(cst_to))
  {
    _transform_lab_to_rgb_matrix(image_in, image_out, width, height, profile_info);
  }
  else
  {
    *converted_cst = cst_from;
    fprintf(stderr, "[dt_ioppr_transform_matrix] invalid conversion from %i to %i\n", cst_from, cst_to);
  }
}


#define DT_IOPPR_LUT_SAMPLES 0x10000

__DT_CLONE_TARGETS__
void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize)
{
  profile_info->type = DT_COLORSPACE_NONE;
  profile_info->filename[0] = '\0';
  profile_info->intent = DT_INTENT_PERCEPTUAL;
  dt_ioppr_mark_as_nonmatrix_profile(profile_info);
  profile_info->unbounded_coeffs_in[0][0] = profile_info->unbounded_coeffs_in[1][0] = profile_info->unbounded_coeffs_in[2][0] = -1.0f;
  profile_info->unbounded_coeffs_out[0][0] = profile_info->unbounded_coeffs_out[1][0] = profile_info->unbounded_coeffs_out[2][0] = -1.0f;
  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.f;
  profile_info->lutsize = (lutsize > 0) ? lutsize: DT_IOPPR_LUT_SAMPLES;
  for(int i = 0; i < 3; i++)
  {
    profile_info->lut_in[i] = dt_alloc_align_float(profile_info->lutsize);
    profile_info->lut_in[i][0] = -1.0f;
    profile_info->lut_out[i] = dt_alloc_align_float(profile_info->lutsize);
    profile_info->lut_out[i][0] = -1.0f;
  }
}

#undef DT_IOPPR_LUT_SAMPLES

void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t **profile_info)
{
  /* The whole teardown, not just the LUTs. A dt_iop_order_iccprofile_info_t owns six
   * aligned float arrays, so releasing one is "free the curves, free the struct, drop the
   * pointer" -- three steps every caller was open-coding, and three chances to free the
   * struct while leaving 1.5 MB of curves behind. Takes the pointer by address so the
   * caller's variable cannot be left dangling. */
  if(IS_NULL_PTR(profile_info) || IS_NULL_PTR(*profile_info)) return;

  for(int i = 0; i < 3; i++)
  {
    dt_free_align((*profile_info)->lut_in[i]);
    (*profile_info)->lut_in[i] = NULL;
    dt_free_align((*profile_info)->lut_out[i]);
    (*profile_info)->lut_out[i] = NULL;
  }

  dt_free_align(*profile_info);
  *profile_info = NULL;
}

void dt_ioppr_transform_image_colorspace_rgb(const float *const restrict image_in, float *const restrict image_out, const int width,
                                             const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message)
{
  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(image_in != image_out)
      memcpy(image_out, image_in, sizeof(float) * 4 * width * height);

    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(dt_get_debug_flags() & DT_DEBUG_PERF) dt_get_times(&start_time);

  if(!isnan(profile_info_from->matrix_in[0][0]) && !isnan(profile_info_from->matrix_out[0][0])
     && !isnan(profile_info_to->matrix_in[0][0]) && !isnan(profile_info_to->matrix_out[0][0]))
  {
    _transform_matrix_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f CPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    _transform_lcms2_rgb(image_in, image_out, width, height, profile_info_from, profile_info_to);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f lcms2) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
}

#ifdef HAVE_OPENCL
/* The kernels this subsystem compiles, owned HERE. They used to be handed to
 * common/opencl.c, parked on the application-wide dt_opencl_t, and read back from it --
 * a round trip through a god-struct that added nothing but an ordering. opencl.c still
 * calls init/free, because the kernels must be built after the devices exist, but the
 * pointer never leaves this file. */
static dt_colorspaces_cl_global_t *_colorspaces_cl_global = NULL;

void dt_colorspaces_init_cl_global(void)
{
  dt_colorspaces_cl_global_t *g = (dt_colorspaces_cl_global_t *)malloc(sizeof(dt_colorspaces_cl_global_t));

  const int program = 23; // colorspaces.cl, from programs.conf
  g->kernel_colorspaces_transform_lab_to_rgb_matrix = dt_opencl_create_kernel(program, "colorspaces_transform_lab_to_rgb_matrix");
  g->kernel_colorspaces_transform_rgb_matrix_to_lab = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_lab");
  g->kernel_colorspaces_transform_rgb_matrix_to_rgb
      = dt_opencl_create_kernel(program, "colorspaces_transform_rgb_matrix_to_rgb");
  _colorspaces_cl_global = g;
}

void dt_colorspaces_free_cl_global(void)
{
  dt_colorspaces_cl_global_t *g = _colorspaces_cl_global;
  _colorspaces_cl_global = NULL;
  if(IS_NULL_PTR(g)) return;

  // destroy kernels
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_lab_to_rgb_matrix);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_lab);
  dt_opencl_free_kernel(g->kernel_colorspaces_transform_rgb_matrix_to_rgb);

  dt_free(g);
}

void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info, dt_colorspaces_iccprofile_info_cl_t *profile_info_cl)
{
  for(int i = 0; i < 9; i++)
  {
    profile_info_cl->matrix_in[i] = profile_info->matrix_in[i/3][i%3];
    profile_info_cl->matrix_out[i] = profile_info->matrix_out[i/3][i%3];
  }
  profile_info_cl->lutsize = profile_info->lutsize;
  for(int i = 0; i < 3; i++)
  {
    for(int j = 0; j < 3; j++)
    {
      profile_info_cl->unbounded_coeffs_in[i][j] = profile_info->unbounded_coeffs_in[i][j];
      profile_info_cl->unbounded_coeffs_out[i][j] = profile_info->unbounded_coeffs_out[i][j];
    }
  }
  profile_info_cl->nonlinearlut = profile_info->nonlinearlut;
  profile_info_cl->grey = profile_info->grey;
}

cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_float *trc = malloc(sizeof(cl_float) * 6 * profile_info->lutsize);
  if(trc)
  {
    int x = 0;
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_in[c][y];
    for(int c = 0; c < 3; c++)
      for(int y = 0; y < profile_info->lutsize; y++, x++)
        trc[x] = profile_info->lut_out[c][y];
  }
  return trc;
}

cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid, dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut)
{
  cl_int err = CL_SUCCESS;

  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = calloc(1, sizeof(dt_colorspaces_iccprofile_info_cl_t));
  cl_float *profile_lut_cl = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;

  if(profile_info)
  {
    dt_ioppr_get_profile_info_cl(profile_info, profile_info_cl);
    profile_lut_cl = dt_ioppr_get_trc_cl(profile_info);

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(*profile_info_cl), profile_info_cl);
    if(IS_NULL_PTR(dev_profile_info))
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 256, 256 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_profile_lut))
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }
  else
  {
    profile_lut_cl = malloc(sizeof(cl_float) * 1 * 6);

    dev_profile_lut = dt_opencl_copy_host_to_device(devid, profile_lut_cl, 1, 1 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_profile_lut))
    {
      fprintf(stderr, "[dt_ioppr_build_iccprofile_params_cl] error allocating memory 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
  }

cleanup:
  *_profile_info_cl = profile_info_cl;
  *_profile_lut_cl = profile_lut_cl;
  *_dev_profile_info = dev_profile_info;
  *_dev_profile_lut = dev_profile_lut;

  return err;
}

void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut)
{
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = *_profile_info_cl;
  cl_float *profile_lut_cl = *_profile_lut_cl;
  cl_mem dev_profile_info = *_dev_profile_info;
  cl_mem dev_profile_lut = *_dev_profile_lut;

  if(profile_info_cl)
  {
    dt_free(profile_info_cl);
  }
  dt_opencl_release_mem_object(dev_profile_info);
  dt_opencl_release_mem_object(dev_profile_lut);
  dt_free(profile_lut_cl);

  *_profile_info_cl = NULL;
  *_profile_lut_cl = NULL;
  *_dev_profile_info = NULL;
  *_dev_profile_lut = NULL;
}

int dt_ioppr_transform_image_colorspace_rgb_cl(const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                               const int width, const int height,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                               const char *message)
{
  cl_int err = CL_SUCCESS;

  if(profile_info_from->type == DT_COLORSPACE_NONE || profile_info_to->type == DT_COLORSPACE_NONE)
  {
    return FALSE;
  }
  if(profile_info_from->type == profile_info_to->type
     && strcmp(profile_info_from->filename, profile_info_to->filename) == 0)
  {
    if(dev_img_in != dev_img_out)
    {
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_img_out, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        return FALSE;
      }
    }

    return TRUE;
  }

  const size_t ch = 4;
  float *src_buffer_in = NULL;
  float *src_buffer_out = NULL;
  int in_place = (dev_img_in == dev_img_out);

  int kernel_transform = 0;
  cl_mem dev_tmp = NULL;

  cl_mem dev_profile_info_from = NULL;
  cl_mem dev_lut_from = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_from_cl;
  cl_float *lut_from_cl = NULL;

  cl_mem dev_profile_info_to = NULL;
  cl_mem dev_lut_to = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_to_cl;
  cl_float *lut_to_cl = NULL;

  cl_mem matrix_cl = NULL;

  // if we have a matrix use opencl
  if(!isnan(profile_info_from->matrix_in[0][0]) && !isnan(profile_info_from->matrix_out[0][0])
     && !isnan(profile_info_to->matrix_in[0][0]) && !isnan(profile_info_to->matrix_out[0][0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(dt_get_debug_flags() & DT_DEBUG_PERF) dt_get_times(&start_time);

    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };

    kernel_transform = _colorspaces_cl_global->kernel_colorspaces_transform_rgb_matrix_to_rgb;

    dt_ioppr_get_profile_info_cl(profile_info_from, &profile_info_from_cl);
    lut_from_cl = dt_ioppr_get_trc_cl(profile_info_from);

    dt_ioppr_get_profile_info_cl(profile_info_to, &profile_info_to_cl);
    lut_to_cl = dt_ioppr_get_trc_cl(profile_info_to);

    dt_colormatrix_t matrix;
    dt_colormatrix_mul(matrix, profile_info_to->matrix_out, profile_info_from->matrix_in);

    if(in_place)
    {
      dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
      if(IS_NULL_PTR(dev_tmp))
      {
        fprintf(
            stderr,
            "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 4\n");
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }

      err = dt_opencl_enqueue_copy_image(devid, dev_img_in, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS)
      {
        fprintf(stderr,
                "[dt_ioppr_transform_image_colorspace_rgb_cl] error on copy image for color transformation\n");
        goto cleanup;
      }
    }
    else
    {
      dev_tmp = dev_img_in;
    }

    dev_profile_info_from
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_from_cl), &profile_info_from_cl);
    if(IS_NULL_PTR(dev_profile_info_from))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_from = dt_opencl_copy_host_to_device(devid, lut_from_cl, 256, 256 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_lut_from))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    dev_profile_info_to
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_to_cl), &profile_info_to_cl);
    if(IS_NULL_PTR(dev_profile_info_to))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut_to = dt_opencl_copy_host_to_device(devid, lut_to_cl, 256, 256 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_lut_to))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 8\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    float matrix3x4[12];
    pack_3xSSE_to_3x4(matrix, matrix3x4);
    matrix_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(matrix3x4), matrix3x4);
    if(IS_NULL_PTR(matrix_cl))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 7\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut_from);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 6, sizeof(cl_mem), (void *)&dev_profile_info_to);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 7, sizeof(cl_mem), (void *)&dev_lut_to);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 8, sizeof(cl_mem), (void *)&matrix_cl);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error %i enqueue kernel for color transformation\n",
              err);
      goto cleanup;
    }

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform RGB-->RGB took %.3f secs (%.3f GPU) [%s]\n",
              end_time.clock - start_time.clock, end_time.user - start_time.user, (message) ? message : "");
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer_in  = dt_pixelpipe_cache_alloc_align_float_cache(ch * width * height, 0);
    src_buffer_out = dt_pixelpipe_cache_alloc_align_float_cache(ch * width * height, 0);
    if(IS_NULL_PTR(src_buffer_in) || IS_NULL_PTR(src_buffer_out))
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer_in, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace_rgb(src_buffer_in, src_buffer_out, width, height, profile_info_from,
                                            profile_info_to, message);

    err = dt_opencl_write_host_to_device(devid, src_buffer_out, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr,
              "[dt_ioppr_transform_image_colorspace_rgb_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  dt_pixelpipe_cache_free_align(src_buffer_in);
  dt_pixelpipe_cache_free_align(src_buffer_out);
  if(dev_tmp && in_place) dt_opencl_release_mem_object(dev_tmp);

  dt_opencl_release_mem_object(dev_profile_info_from);
  dt_opencl_release_mem_object(dev_lut_from);
  dt_free(lut_from_cl);

  dt_opencl_release_mem_object(dev_profile_info_to);
  dt_opencl_release_mem_object(dev_lut_to);
  dt_free(lut_to_cl);

  dt_opencl_release_mem_object(matrix_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

#undef DT_IOP_ORDER_PROFILE
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


/* ---------------------------------------------------------------------------
 * The derived-profile memo.
 *
 * Matrix + tone-curve LUTs extracted from a profile: expensive to build (two
 * 65536-entry extractions) and a pure function of (type, filename), so it is
 * memoised. This lived on dt_develop_t, which made it per-image, unsynchronised,
 * and duplicated between concurrent exports; and it made develop/ take this
 * module's rwlock by hand to build an entry.
 *
 * It is the module's now, and flushed by dt_colorprofiles_cleanup().
 *
 * What deliberately does NOT live here: profiles derived from one image --
 * DT_COLORSPACE_EMBEDDED_ICC through DT_COLORSPACE_ALTERNATE_MATRIX. They are not
 * registered in the profile list and cannot be resolved by identity at all, so
 * they are not a function of their key and would stomp each other here. They live
 * on the pipe that built them (dt_dev_pixelpipe_t.owned_input_profile_info).
 * ------------------------------------------------------------------------- */

static GList *_profile_info_memo = NULL;
/* Raw pthread type, like this module's other locks: dt_pthread_mutex_t is a struct
 * wrapper in Debug builds, so a static PTHREAD_MUTEX_INITIALIZER would be initialising
 * a subobject -- which clang rejects under -Werror and gcc silently accepts. */
static pthread_mutex_t _profile_info_lock = PTHREAD_MUTEX_INITIALIZER;

void dt_colorspaces_flush_profile_memo(void)
{
  pthread_mutex_lock(&_profile_info_lock);
  while(_profile_info_memo)
  {
    dt_iop_order_iccprofile_info_t *entry = (dt_iop_order_iccprofile_info_t *)_profile_info_memo->data;
    dt_ioppr_cleanup_profile_info(&entry);
    _profile_info_memo = g_list_delete_link(_profile_info_memo, _profile_info_memo);
  }
  pthread_mutex_unlock(&_profile_info_lock);
}

void dt_colorspaces_invalidate_display_profile_memo(void)
{
  /* The DISPLAY entry is derived from a cmsHPROFILE this module replaces whenever the
   * monitor profile changes. Nothing invalidated it before, so a session kept the previous
   * monitor's matrices and tone curves for as long as the memo lived. That was already
   * wrong per-image; now that the memo is process-wide it would persist for the whole run.
   *
   * Dropped rather than rebuilt: the next caller that wants it will build it, and doing it
   * here would mean building a profile under this lock from the profile-changed handler. */
  pthread_mutex_lock(&_profile_info_lock);
  for(GList *l = _profile_info_memo; l; )
  {
    GList *next = g_list_next(l);
    dt_iop_order_iccprofile_info_t *entry = (dt_iop_order_iccprofile_info_t *)l->data;
    if(entry && entry->type == DT_COLORSPACE_DISPLAY)
    {
      dt_ioppr_cleanup_profile_info(&entry);
      _profile_info_memo = g_list_delete_link(_profile_info_memo, l);
    }
    l = next;
  }
  pthread_mutex_unlock(&_profile_info_lock);
}

static int _generate_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int type, const char *filename, const int intent)
{
  int err_code = 0;
  cmsHPROFILE *rgb_profile = NULL;

  dt_ioppr_mark_as_nonmatrix_profile(profile_info);
  dt_ioppr_clear_lut_curves(profile_info);

  profile_info->nonlinearlut = 0;
  profile_info->grey = 0.1842f;

  profile_info->type = type;
  g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
  profile_info->intent = intent;

  /* The DISPLAY entry's cmsHPROFILE is the one thing in the list that is replaced at
   * runtime, so resolving it and DERIVING FROM IT have to happen under the same lock.
   * develop/ used to take this lock by hand -- and released it immediately after the
   * lookup, before cmsGetColorSpace() and the two 65536-entry extractions below, which
   * are the parts that actually touch the handle. Inside the module the whole span is
   * covered, which is what the lock was for. */
  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(type, filename, DT_PROFILE_ROLE_ANY);

  dt_colorspaces_lock_profile(profile);
  if(profile) rgb_profile = profile->profile;

  // we only allow rgb profiles
  if(rgb_profile)
  {
    cmsColorSpaceSignature rgb_color_space = cmsGetColorSpace(rgb_profile);
    if(rgb_color_space != cmsSigRgbData)
    {
      fprintf(stderr, "working profile color space `%c%c%c%c' not supported\n",
              (char)(rgb_color_space>>24),
              (char)(rgb_color_space>>16),
              (char)(rgb_color_space>>8),
              (char)(rgb_color_space));
      rgb_profile = NULL;
    }
  }

  // get the matrix
  if(rgb_profile)
  {
    if(dt_colorspaces_get_matrix_from_input_profile(rgb_profile, profile_info->matrix_in, profile_info->lut_in[0],
                                                    profile_info->lut_in[1], profile_info->lut_in[2],
                                                    profile_info->lutsize)
       || dt_colorspaces_get_matrix_from_output_profile(rgb_profile, profile_info->matrix_out,
                                                        profile_info->lut_out[0], profile_info->lut_out[1],
                                                        profile_info->lut_out[2], profile_info->lutsize))
    {
      dt_ioppr_mark_as_nonmatrix_profile(profile_info);
      dt_ioppr_clear_lut_curves(profile_info);
    }
    else if(isnan(profile_info->matrix_in[0][0]) || isnan(profile_info->matrix_out[0][0]))
    {
      dt_ioppr_mark_as_nonmatrix_profile(profile_info);
      dt_ioppr_clear_lut_curves(profile_info);
    }
    else
    {
      transpose_3xSSE(profile_info->matrix_in, profile_info->matrix_in_transposed);
      transpose_3xSSE(profile_info->matrix_out, profile_info->matrix_out_transposed);
    }
  }

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  if(!isnan(profile_info->matrix_in[0][0]) && !isnan(profile_info->matrix_out[0][0]))
  {
    profile_info->nonlinearlut = dt_ioppr_init_unbounded_coeffs(profile_info->lut_in[0], profile_info->lut_in[1], profile_info->lut_in[2],
        profile_info->unbounded_coeffs_in[0], profile_info->unbounded_coeffs_in[1], profile_info->unbounded_coeffs_in[2], profile_info->lutsize);
    dt_ioppr_init_unbounded_coeffs(profile_info->lut_out[0], profile_info->lut_out[1], profile_info->lut_out[2],
        profile_info->unbounded_coeffs_out[0], profile_info->unbounded_coeffs_out[1], profile_info->unbounded_coeffs_out[2], profile_info->lutsize);
  }

  if(!isnan(profile_info->matrix_in[0][0]) && !isnan(profile_info->matrix_out[0][0]) && profile_info->nonlinearlut)
  {
    const dt_aligned_pixel_t rgb = { 0.1842f, 0.1842f, 0.1842f };
    profile_info->grey = dt_ioppr_get_rgb_matrix_luminance(rgb, profile_info->matrix_in, profile_info->lut_in, profile_info->unbounded_coeffs_in, profile_info->lutsize, profile_info->nonlinearlut);
  }

  dt_colorspaces_unlock_profile(profile);

  return err_code;
}

/* Caller holds _profile_info_lock. */
static dt_iop_order_iccprofile_info_t *
_get_profile_info_from_list(
                                    const dt_colorspaces_color_profile_type_t profile_type,
                                    const char *profile_filename,
                                    const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;

  /* Caller holds _profile_info_lock: this walks a list the pipeline worker and the
   * GUI thread both append to. */
  for(GList *profiles = _profile_info_memo; profiles; profiles = g_list_next(profiles))
  {
    dt_iop_order_iccprofile_info_t *prof = (dt_iop_order_iccprofile_info_t *)(profiles->data);
    if(prof->type == profile_type && prof->intent == intent
       && strcmp(prof->filename, profile_filename) == 0)
    {
      profile_info = prof;
      break;
    }
  }

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_colorspaces_add_profile(const dt_colorspaces_color_profile_type_t profile_type,
                           const char *profile_filename,
                           const int intent)
{
  /* Find-or-create as ONE critical section. Reached from the pipeline worker -- iop/lut3d.c
   * and iop/tonecurve.c call it from process()/process_cl(), once per tile -- and from the
   * GUI thread via iop/colorin.c. The lock has to span the lookup as well as the append:
   * two threads missing the same key concurrently would otherwise each build an entry
   * (1.5 MB of tone-curve LUTs apiece) and append both. */
  pthread_mutex_lock(&_profile_info_lock);

  dt_iop_order_iccprofile_info_t *profile_info = _get_profile_info_from_list(profile_type, profile_filename, intent);
  if(IS_NULL_PTR(profile_info))
  {
    profile_info = dt_alloc_align(sizeof(dt_iop_order_iccprofile_info_t));
    dt_ioppr_init_profile_info(profile_info, 0);
    const int err = _generate_profile_info(profile_info, profile_type, profile_filename, intent);
    if(err == 0)
    {
      _profile_info_memo = g_list_append(_profile_info_memo, profile_info);
    }
    else
    {
      /* dt_ioppr_init_profile_info() has already allocated six DT_IOPPR_LUT_SAMPLES float
       * arrays -- 1.5 MB -- so freeing the struct alone leaked all of them on this path. */
      dt_ioppr_cleanup_profile_info(&profile_info);
    }
  }

  pthread_mutex_unlock(&_profile_info_lock);

  return profile_info;
}

/* ---------------------------------------------------------------------------
 * APPLY: the pixel loop.
 *
 * One entry point for converting a buffer between colour spaces, branching
 * internally on what the profile actually is. A matrix-shaper profile with tone
 * curves goes through our own vectorised matrix + LUT path; anything else -- a
 * CLUT profile, a v4 parametric curve lcms2 will not reduce -- falls back to
 * cmsDoTransform. Callers do not choose, and do not see either.
 *
 * These used to live in develop/, which is why every consumer had to know the
 * distinction existed. The two implementations they dispatch to were already
 * here; only the branch was upstairs.
 *
 * The op/instance names are for the -d perf trace only. They are plain strings
 * rather than the dt_iop_module_t they were read from, because this module sits
 * below develop/ and cannot name an iop.
 * ------------------------------------------------------------------------- */

void dt_colorspaces_apply_profile(const char *const op_name, const char *const instance_name, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return;
  }
  if(dt_iop_colorspace_is_rgb(cst_from) && dt_iop_colorspace_is_rgb(cst_to))
  {
    *converted_cst = cst_to;
    return;
  }
  if(IS_NULL_PTR(profile_info))
  {
    *converted_cst = cst_from;
    return;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return;
  }

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(dt_get_debug_flags() & DT_DEBUG_PERF) dt_get_times(&start_time);

  // matrix should be never NAN, this is only to test it against lcms2!
  if(!isnan(profile_info->matrix_in[0][0]) && !isnan(profile_info->matrix_out[0][0]))
  {
    dt_ioppr_transform_matrix(op_name, instance_name, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f CPU) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, op_name, instance_name);
    }
  }
  else
  {
    dt_ioppr_transform_lcms2(op_name, instance_name, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f lcms2) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, op_name, instance_name);
    }
  }

  if(*converted_cst == cst_from)
    fprintf(stderr, "[dt_colorspaces_apply_profile] invalid conversion from %i to %i\n", cst_from, cst_to);
}

#ifdef HAVE_OPENCL
int dt_colorspaces_apply_profile_cl(const char *const op_name, const char *const instance_name, const int devid, cl_mem dev_img_in,
                                           cl_mem dev_img_out, const int width, const int height,
                                           const int cst_from, const int cst_to, int *converted_cst,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  cl_int err = CL_SUCCESS;

  assert(!IS_NULL_PTR(dev_img_in));
  assert(!IS_NULL_PTR(dev_img_out));
  assert(dev_img_in != dev_img_out);

  if(cst_from == cst_to)
  {
    *converted_cst = cst_to;
    return TRUE;
  }
  if(dt_iop_colorspace_is_rgb(cst_from) && dt_iop_colorspace_is_rgb(cst_to))
  {
    *converted_cst = cst_to;
    return TRUE;
  }
  if(IS_NULL_PTR(profile_info))
  {
    *converted_cst = cst_from;
    return FALSE;
  }
  if(profile_info->type == DT_COLORSPACE_NONE)
  {
    *converted_cst = cst_from;
    return FALSE;
  }

  const size_t ch = 4;
  float *src_buffer = NULL;

  int kernel_transform = 0;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t profile_info_cl;
  cl_float *lut_cl = NULL;

  *converted_cst = cst_from;

  // if we have a matrix use opencl
  if(!isnan(profile_info->matrix_in[0][0]) && !isnan(profile_info->matrix_out[0][0]))
  {
    dt_times_t start_time = { 0 }, end_time = { 0 };
    if(dt_get_debug_flags() & DT_DEBUG_PERF) dt_get_times(&start_time);

    if(dt_iop_colorspace_is_rgb(cst_from) && cst_to == IOP_CS_LAB)
    {
      kernel_transform = _colorspaces_cl_global->kernel_colorspaces_transform_rgb_matrix_to_lab;
    }
    else if(cst_from == IOP_CS_LAB && dt_iop_colorspace_is_rgb(cst_to))
    {
      kernel_transform = _colorspaces_cl_global->kernel_colorspaces_transform_lab_to_rgb_matrix;
    }
    else
    {
      err = CL_INVALID_KERNEL;
      *converted_cst = cst_from;
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] invalid conversion from %i to %i\n", cst_from, cst_to);
      goto cleanup;
    }

    dt_ioppr_get_profile_info_cl(profile_info, &profile_info_cl);
    lut_cl = dt_ioppr_get_trc_cl(profile_info);

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_cl), &profile_info_cl);
    if(IS_NULL_PTR(dev_profile_info))
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut = dt_opencl_copy_host_to_device(devid, lut_cl, 256, 256 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_lut))
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error allocating memory for color transformation 6\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

    dt_opencl_set_kernel_arg(devid, kernel_transform, 0, sizeof(cl_mem), (void *)&dev_img_in);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 1, sizeof(cl_mem), (void *)&dev_img_out);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 4, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_transform, 5, sizeof(cl_mem), (void *)&dev_lut);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_transform, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error %i enqueue kernel for color transformation\n", err);
      goto cleanup;
    }

    *converted_cst = cst_to;

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f GPU) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, op_name, instance_name);
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer = dt_pixelpipe_cache_alloc_align_float_cache(ch * width * height, 0);
    if(IS_NULL_PTR(src_buffer))
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_colorspaces_apply_profile(op_name, instance_name, src_buffer, src_buffer, width, height, cst_from, cst_to,
                                        converted_cst, profile_info);

    err = dt_opencl_write_host_to_device(devid, src_buffer, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_colorspaces_apply_profile_cl] error allocating memory for color transformation 3\n");
      goto cleanup;
    }
  }

cleanup:
  dt_pixelpipe_cache_free_align(src_buffer);
  dt_opencl_release_mem_object(dev_profile_info);
  dt_opencl_release_mem_object(dev_lut);
  dt_free(lut_cl);

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif
