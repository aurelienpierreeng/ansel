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
/* The pipeline-facing half of the colour-profile module: resolving which profile a module,
 * pipe or export should use, and transforming an image with it. Everything here takes
 * dt_develop_t, dt_dev_pixelpipe_t or dt_iop_module_t, which is why it sits at layer 5.
 *
 * The profile maths it delegates to lives in colorprofiles/iop_profile.c -- see the note there.
 */

#include "colorprofiles/iop_profile.h"
#include "config.h"
#endif

#include "colorprofiles/colorspaces.h"
#include "common/pixelpipe_cache_alloc.h"
#include "develop/iop_profile.h"
#include "math/matrices.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "develop/develop.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system/target_clones.h"
#include "common/times.h"


static int dt_ioppr_generate_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int type, const char *filename, const int intent)
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

  dt_colorspaces_t *const profiles = dt_colorspaces_get_global();
  if(type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_rdlock(&profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);
  if(profile) rgb_profile = profile->profile;

  if(type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_unlock(&profiles->xprofile_lock);

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

  return err_code;
}

__DT_CLONE_TARGETS__
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev,
                                    const dt_colorspaces_color_profile_type_t profile_type,
                                    const char *profile_filename)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;

  for(GList *profiles = dev->allprofile_info; profiles; profiles = g_list_next(profiles))
  {
    dt_iop_order_iccprofile_info_t *prof = (dt_iop_order_iccprofile_info_t *)(profiles->data);
    if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
    {
      profile_info = prof;
      break;
    }
  }

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev,
                                  const dt_colorspaces_color_profile_type_t profile_type,
                                  const char *profile_filename,
                                  const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_get_profile_info_from_list(dev, profile_type, profile_filename);
  if(IS_NULL_PTR(profile_info))
  {
    profile_info = dt_alloc_align(sizeof(dt_iop_order_iccprofile_info_t));
    dt_ioppr_init_profile_info(profile_info, 0);
    const int err = dt_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent);
    if(err == 0)
    {
      dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);
    }
    else
    {
      dt_free_align(profile_info);
      profile_info = NULL;
    }
  }
  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list)
{
  dt_iop_order_iccprofile_info_t *profile = NULL;

  // first check if the module is between colorin and colorout
  gboolean in_between = FALSE;

  for(GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    // we reach the module, that's it
    if(strcmp(mod->op, module->op) == 0) break;

    // if we reach colorout means that the module is after it
    if(strcmp(mod->op, "colorout") == 0)
    {
      in_between = FALSE;
      break;
    }

    // we reach colorin, so far we're good
    if(strcmp(mod->op, "colorin") == 0)
    {
      in_between = TRUE;
      break;
    }
  }

  if(in_between)
  {
    dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_NONE;
    const char *filename = NULL;
    dt_develop_t *dev = module->dev;

    dt_ioppr_get_work_profile_type(dev, &type, &filename);
    if(filename) profile = dt_ioppr_add_profile_info_to_list(dev, type, filename, DT_INTENT_PERCEPTUAL);
  }

  return profile;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev,
                                    struct dt_dev_pixelpipe_t *pipe,
                                    const dt_colorspaces_color_profile_type_t type,
                                    const char *filename,
                                    const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(IS_NULL_PTR(profile_info) || isnan(profile_info->matrix_in[0][0]) || isnan(profile_info->matrix_out[0][0]))
  {
    fprintf(stderr, "[dt_ioppr_set_pipe_work_profile_info] unsupported working profile %i %s, it will be replaced with linear Rec2020\n", type, filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  pipe->work_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_input_profile_info(struct dt_develop_t *dev,
                                     struct dt_dev_pixelpipe_t *pipe,
                                     const dt_colorspaces_color_profile_type_t type,
                                     const char *filename,
                                     const int intent,
                                     const dt_colormatrix_t matrix_in)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(IS_NULL_PTR(profile_info))
  {
    fprintf(stderr,
            "[dt_ioppr_set_pipe_input_profile_info] unsupported input profile %i %s, it will be replaced with "
            "linear Rec2020\n",
            type, filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", intent);
  }

  if(profile_info->type >= DT_COLORSPACE_EMBEDDED_ICC && profile_info->type <= DT_COLORSPACE_ALTERNATE_MATRIX)
  {
    /* We have a camera input matrix, these are not generated from files but in colorin,
    * so we need to fetch and replace them from somewhere.
    */
    memcpy(profile_info->matrix_in, matrix_in, sizeof(profile_info->matrix_in));
    mat3SSEinv(profile_info->matrix_out, profile_info->matrix_in);
    transpose_3xSSE(profile_info->matrix_in, profile_info->matrix_in_transposed);
    transpose_3xSSE(profile_info->matrix_out, profile_info->matrix_out_transposed);
  }
  pipe->input_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_output_profile_info(struct dt_develop_t *dev,
                                      struct dt_dev_pixelpipe_t *pipe,
                                      const dt_colorspaces_color_profile_type_t type,
                                      const char *filename,
                                      const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);

  if(IS_NULL_PTR(profile_info) || isnan(profile_info->matrix_in[0][0]) || isnan(profile_info->matrix_out[0][0]))
  {
    if (type != DT_COLORSPACE_DISPLAY)
    {
      // ??? this error output has been disabled for a display profile.
      // see discussion in https://github.com/darktable-org/darktable/issues/6774
      fprintf(stderr,
              "[dt_ioppr_set_pipe_output_profile_info] unsupported output"
              " profile %i %s, it will be replaced with sRGB\n",
              type, filename);
    }
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_SRGB, "", intent);
  }
  pipe->output_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->work_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_input_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->input_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_output_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->output_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_current_profile_info(dt_iop_module_t *module,
                                                                       const struct dt_dev_pixelpipe_t *pipe)
{
  dt_iop_order_iccprofile_info_t *restrict color_profile;

  const int colorin_order = dt_ioppr_get_iop_order(module->dev->iop_order_list, "colorin", 0);
  const int colorout_order = dt_ioppr_get_iop_order(module->dev->iop_order_list, "colorout", 0);
  const int current_module_order = module->iop_order;

  if(current_module_order < colorin_order)
    color_profile = dt_ioppr_get_pipe_input_profile_info(pipe);
  else if(current_module_order < colorout_order)
    color_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  else
    color_profile = dt_ioppr_get_pipe_output_profile_info(pipe);

  return color_profile;
}

// returns a pointer to the filename of the work profile instead of the actual string data
// pointer must not be stored
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev,
                                    dt_colorspaces_color_profile_type_t *profile_type,
                                    const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorin_so = NULL;
  dt_iop_module_t *colorin = NULL;
  for(const GList *modules = dt_iop_get_modules_so(); modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorin"))
    {
      colorin_so = module_so;
      break;
    }
  }
  if(colorin_so && colorin_so->get_p)
  {
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorin"))
      {
        colorin = module;
        break;
      }
    }
  }
  if(colorin)
  {
    dt_colorspaces_color_profile_type_t *_type = colorin_so->get_p(colorin->params, "type_work");
    char *_filename = colorin_so->get_p(colorin->params, "filename_work");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't get colorin parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't find colorin iop\n");
}

void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev,
                                      dt_colorspaces_color_profile_type_t *profile_type,
                                      const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorout_so = NULL;
  dt_iop_module_t *colorout = NULL;
  for(const GList *modules = g_list_last(dt_iop_get_modules_so()); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorout"))
    {
      colorout_so = module_so;
      break;
    }
  }
  if(colorout_so && colorout_so->get_p)
  {
    for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
    }
  }
  if(colorout)
  {
    dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
    char *_filename = colorout_so->get_p(colorout->params, "filename");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't get colorout parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't find colorout iop\n");
}

void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
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
    dt_ioppr_transform_matrix(self->op, self->multi_name, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f CPU) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    dt_ioppr_transform_lcms2(self->op, self->multi_name, image_in, image_out, width, height, cst_from, cst_to, converted_cst, profile_info);

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f lcms2) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }

  if(*converted_cst == cst_from)
    fprintf(stderr, "[dt_ioppr_transform_image_colorspace] invalid conversion from %i to %i\n", cst_from, cst_to);
}

#ifdef HAVE_OPENCL
int dt_ioppr_transform_image_colorspace_cl(struct dt_iop_module_t *self, const int devid, cl_mem dev_img_in,
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
      kernel_transform = dt_opencl_get_global()->colorspaces->kernel_colorspaces_transform_rgb_matrix_to_lab;
    }
    else if(cst_from == IOP_CS_LAB && dt_iop_colorspace_is_rgb(cst_to))
    {
      kernel_transform = dt_opencl_get_global()->colorspaces->kernel_colorspaces_transform_lab_to_rgb_matrix;
    }
    else
    {
      err = CL_INVALID_KERNEL;
      *converted_cst = cst_from;
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] invalid conversion from %i to %i\n", cst_from, cst_to);
      goto cleanup;
    }

    dt_ioppr_get_profile_info_cl(profile_info, &profile_info_cl);
    lut_cl = dt_ioppr_get_trc_cl(profile_info);

    dev_profile_info = dt_opencl_copy_host_to_device_constant(devid, sizeof(profile_info_cl), &profile_info_cl);
    if(IS_NULL_PTR(dev_profile_info))
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 5\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    dev_lut = dt_opencl_copy_host_to_device(devid, lut_cl, 256, 256 * 6, sizeof(float));
    if(IS_NULL_PTR(dev_lut))
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 6\n");
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
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error %i enqueue kernel for color transformation\n", err);
      goto cleanup;
    }

    *converted_cst = cst_to;

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
    {
      dt_get_times(&end_time);
      fprintf(stderr, "image colorspace transform %s-->%s took %.3f secs (%.3f GPU) [%s %s]\n",
          dt_iop_colorspace_is_rgb(cst_from) ? "RGB" : "Lab",
          dt_iop_colorspace_is_rgb(cst_to) ? "RGB" : "Lab",
          end_time.clock - start_time.clock, end_time.user - start_time.user, self->op, self->multi_name);
    }
  }
  else
  {
    // no matrix, call lcms2
    src_buffer = dt_pixelpipe_cache_alloc_align_float_cache(ch * width * height, 0);
    if(IS_NULL_PTR(src_buffer))
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 1\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    err = dt_opencl_copy_device_to_host(devid, src_buffer, dev_img_in, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 2\n");
      goto cleanup;
    }

    // just call the CPU version for now
    dt_ioppr_transform_image_colorspace(self, src_buffer, src_buffer, width, height, cst_from, cst_to,
                                        converted_cst, profile_info);

    err = dt_opencl_write_host_to_device(devid, src_buffer, dev_img_out, width, height, ch * sizeof(float));
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_ioppr_transform_image_colorspace_cl] error allocating memory for color transformation 3\n");
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

#undef DT_IOP_ORDER_PROFILE
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
