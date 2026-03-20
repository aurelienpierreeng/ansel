/*
    This file is part of darktable,
    Copyright (C) 2009-2013, 2016 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 Milan Knížek.
    Copyright (C) 2010, 2012-2014 Pascal de Bruijn.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2017, 2019 Tobias Ellinghaus.
    Copyright (C) 2011-2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2012, 2020 Aldric Renaudin.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 John Sheu.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Michal Babej.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014 Edouard Gomez.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018, 2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018, 2020-2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
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
#ifdef HAVE_CONFIG_H
#include "common/darktable.h"
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/matrices.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512
#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(5, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  float lut[3][LUT_SAMPLES];
  dt_colormatrix_t cmatrix;
  cmsHTRANSFORM *xform;
  float unbounded_coeffs[3][3]; // for extrapolation of shaper curves
} dt_iop_colorout_data_t;

typedef struct dt_iop_colorout_global_data_t
{
  int kernel_colorout;
} dt_iop_colorout_global_data_t;

typedef struct dt_iop_colorout_params_t
{
  dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_SRGB
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent; // $DEFAULT: DT_INTENT_PERCEPTUAL
} dt_iop_colorout_params_t;



const char *name()
{
  return _("output color profile");
}


const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("convert pipeline reference RGB to any display RGB\n"
                                        "using color profiles to remap RGB values"),
                                      _("mandatory"),
                                      _("linear or non-linear, RGB or Lab, display-referred"),
                                      _("defined by profile"),
                                      _("non-linear, RGB or Lab, display-referred"));
}


int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static dt_iop_colorspace_type_t _colorout_format_cst(dt_iop_module_t *self,
                                                     dt_dev_pixelpipe_iop_t *piece)
{
  if(piece)
  {
    const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
    if(d->type == DT_COLORSPACE_LAB) return IOP_CS_LAB;
  }
  else
  {
    dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)self->params;
    if(p->type == DT_COLORSPACE_LAB) return IOP_CS_LAB;
  }

  return IOP_CS_RGB;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorout_format_cst(self, piece);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorout_format_cst(self, piece);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
#define DT_IOP_COLOR_ICC_LEN_V4 100
  /*  if(old_version == 1 && new_version == 2)
  {
    dt_iop_colorout_params_t *o = (dt_iop_colorout_params_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memcpy(n,o,sizeof(dt_iop_colorout_params_t));
    n->seq = 0;
    return 0;
    }*/
  if((old_version == 2 || old_version == 3) && new_version == 5)
  {
    typedef struct dt_iop_colorout_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V4];
      char displayprofile[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t intent;
      dt_iop_color_intent_t displayintent;
      char softproof_enabled;
      char softproofprofile[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t softproofintent;
    } dt_iop_colorout_params_v3_t;


    dt_iop_colorout_params_v3_t *o = (dt_iop_colorout_params_v3_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memset(n, 0, sizeof(dt_iop_colorout_params_t));

    if(!strcmp(o->iccprofile, "sRGB"))
      n->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(o->iccprofile, "linear_rec709_rgb") || !strcmp(o->iccprofile, "linear_rgb"))
      n->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(o->iccprofile, "linear_rec2020_rgb"))
      n->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(o->iccprofile, "adobergb"))
      n->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(o->iccprofile, "X profile"))
      n->type = DT_COLORSPACE_DISPLAY;
    else
    {
      n->type = DT_COLORSPACE_FILE;
      g_strlcpy(n->filename, o->iccprofile, sizeof(n->filename));
    }

    n->intent = o->intent;

    return 0;
  }
  if(old_version == 4 && new_version == 5)
  {
    typedef struct dt_iop_colorout_params_v4_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V4];
      dt_iop_color_intent_t intent;
    } dt_iop_colorout_params_v4_t;


    dt_iop_colorout_params_v4_t *o = (dt_iop_colorout_params_v4_t *)old_params;
    dt_iop_colorout_params_t *n = (dt_iop_colorout_params_t *)new_params;
    memset(n, 0, sizeof(dt_iop_colorout_params_t));

    n->type = o->type;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    n->intent = o->intent;

    return 0;
  }

  return 1;
#undef DT_IOP_COLOR_ICC_LEN_V4
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorout_global_data_t *gd
      = (dt_iop_colorout_global_data_t *)calloc(1, sizeof(dt_iop_colorout_global_data_t));
  if(!gd) return;
  module->data = gd;
  gd->kernel_colorout = dt_opencl_create_kernel(program, "colorout");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  dt_free(module->data);
}

static void process_fastpath_apply_tonecurves(const dt_iop_colorout_data_t *const d,
                                              float *const restrict out, const size_t npixels)
{
  const int run_lut0 = d->lut[0][0] >= 0.0f;
  const int run_lut1 = d->lut[1][0] >= 0.0f;
  const int run_lut2 = d->lut[2][0] >= 0.0f;
  if(!(run_lut0 || run_lut1 || run_lut2)) return;

  const float *const lut0 = d->lut[0];
  const float *const lut1 = d->lut[1];
  const float *const lut2 = d->lut[2];
  const float *const coeff0 = d->unbounded_coeffs[0];
  const float *const coeff1 = d->unbounded_coeffs[1];
  const float *const coeff2 = d->unbounded_coeffs[2];

  if(run_lut0 && run_lut1 && run_lut2)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, lut0, lut1, lut2, coeff0, coeff1, coeff2) \
    schedule(static)
#endif
    for(size_t k = 0; k < npixels; k++)
    {
      const size_t idx = 4 * k;
      out[idx + 0] = dt_ioppr_eval_trc(out[idx + 0], lut0, coeff0, LUT_SAMPLES);
      out[idx + 1] = dt_ioppr_eval_trc(out[idx + 1], lut1, coeff1, LUT_SAMPLES);
      out[idx + 2] = dt_ioppr_eval_trc(out[idx + 2], lut2, coeff2, LUT_SAMPLES);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, run_lut0, run_lut1, run_lut2, lut0, lut1, lut2, coeff0, coeff1, coeff2) \
    schedule(static)
#endif
    for(size_t k = 0; k < npixels; k++)
    {
      const size_t idx = 4 * k;
      if(run_lut0) out[idx + 0] = dt_ioppr_eval_trc(out[idx + 0], lut0, coeff0, LUT_SAMPLES);
      if(run_lut1) out[idx + 1] = dt_ioppr_eval_trc(out[idx + 1], lut1, coeff1, LUT_SAMPLES);
      if(run_lut2) out[idx + 2] = dt_ioppr_eval_trc(out[idx + 2], lut2, coeff2, LUT_SAMPLES);
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)self->global_data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_in->width, roi_in->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  float cmatrix[12];
  pack_3xSSE_to_3x4(d->cmatrix, cmatrix);
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, cmatrix);
  if(dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 5, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 6, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 7, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorout, 8, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorout, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorout] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

__DT_CLONE_TARGETS__
static inline void process_fastpath_matrix(const float *const restrict in, float *const restrict out,
                                           const size_t npixels,
                                           const dt_aligned_pixel_simd_t cm0,
                                           const dt_aligned_pixel_simd_t cm1,
                                           const dt_aligned_pixel_simd_t cm2,
                                           const int use_nontemporal)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(in, out, npixels, cm0, cm1, cm2, use_nontemporal) \
    schedule(static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const size_t idx = 4 * k;
    const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in + idx);
    const dt_aligned_pixel_simd_t vout = dt_mat3x4_mul_vec4(vin, cm0, cm1, cm2);
    if(use_nontemporal)
      dt_store_simd_nontemporal(out + idx, vout);
    else
      dt_store_simd_aligned(out + idx, vout);
  }

  if(use_nontemporal)
    dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;
  const int gamutcheck = (d->mode == DT_PROFILE_GAMUTCHECK);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  float *const restrict out = (float *)ovoid;

  if(d->type == DT_COLORSPACE_LAB)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
  }
  else if(!isnan(d->cmatrix[0][0]))
  {
    const float *const restrict in = DT_IS_ALIGNED(ivoid);
    float *const restrict out_aligned = DT_IS_ALIGNED(out);
    const int use_nontemporal
        = (d->lut[0][0] < 0.0f) && (d->lut[1][0] < 0.0f) && (d->lut[2][0] < 0.0f);
    dt_colormatrix_t cmatrix;
    transpose_3xSSE(d->cmatrix, cmatrix);
    const dt_aligned_pixel_simd_t cm0 = dt_colormatrix_row_to_simd(cmatrix, 0);
    const dt_aligned_pixel_simd_t cm1 = dt_colormatrix_row_to_simd(cmatrix, 1);
    const dt_aligned_pixel_simd_t cm2 = dt_colormatrix_row_to_simd(cmatrix, 2);

    process_fastpath_matrix(in, out_aligned, npixels, cm0, cm1, cm2, use_nontemporal);
    process_fastpath_apply_tonecurves(d, out_aligned, npixels);
  }
  else
  {
// fprintf(stderr,"Using xform codepath\n");
    /* Alias the LCMS transform before the OpenMP region and share that alias
     * explicitly instead of reaching through `d->xform` inside the loop. */
    const cmsHTRANSFORM xform = d->xform;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(gamutcheck, ivoid, out, roi_out) \
    shared(xform) \
    schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)4 * k * roi_out->width;
      float *const restrict outp = out + (size_t)4 * k * roi_out->width;

      dt_colorspaces_transform_rgba_float_row(xform, in, outp, roi_out->width);

      if(gamutcheck)
      {
        for(int j = 0; j < roi_out->width; j++)
        {
          if(outp[4*j+0] < 0.0f || outp[4*j+1] < 0.0f || outp[4*j+2] < 0.0f)
          {
            outp[4*j+0] = 0.0f;
            outp[4*j+1] = 1.0f;
            outp[4*j+2] = 1.0f;
          }
        }
      }
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

static cmsHPROFILE _make_clipping_profile(cmsHPROFILE profile)
{
  cmsUInt32Number size;
  cmsHPROFILE old_profile = profile;
  profile = NULL;

  if(old_profile && cmsSaveProfileToMem(old_profile, NULL, &size))
  {
    char *data = malloc(size);

    if(cmsSaveProfileToMem(old_profile, data, &size))
      profile = cmsOpenProfileFromMem(data, size);

    dt_free(data);
  }

  return profile;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)p1;
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;

  d->type = p->type;

  const int force_lcms2 = dt_conf_get_bool("plugins/lighttable/export/force_lcms2");

  dt_colorspaces_color_profile_type_t out_type = DT_COLORSPACE_SRGB;
  gchar *out_filename = NULL;
  dt_iop_color_intent_t out_intent = DT_INTENT_PERCEPTUAL;
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  dt_colorspaces_color_profile_type_t work_type = DT_COLORSPACE_NONE;
  const char *work_filename = "";

  cmsHPROFILE input = NULL;
  cmsHPROFILE output = NULL;
  cmsHPROFILE softproof = NULL;
  cmsUInt32Number output_format = TYPE_RGBA_FLT;

  d->mode = (pipe->type == DT_DEV_PIXELPIPE_FULL) ? darktable.color_profiles->mode : DT_PROFILE_NORMAL;

  // Softproof and gamut check take input from GUI and don't write it in internal parameters.
  // The cacheline integrity hash will not be meaningful in this scenario,
  // we need to bypass the cache entirely in these modes.
  dt_iop_set_cache_bypass(self, (d->mode != DT_PROFILE_NORMAL));

  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }
  d->cmatrix[0][0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;

  /* if we are exporting then check and set usage of override profile */
  if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    if(pipe->icc_type != DT_COLORSPACE_NONE)
    {
      // User defined explicitly a color space in export box: use that
      p->type = pipe->icc_type;
      g_strlcpy(p->filename, pipe->icc_filename, sizeof(p->filename));

    }
    else
    {
      // No color space defined : save with input profile
      dt_iop_order_iccprofile_info_t *icc_input = dt_ioppr_get_pipe_input_profile_info(pipe);
      if(icc_input)
      {
        p->type = icc_input->type;
        g_strlcpy(p->filename, icc_input->filename, sizeof(p->filename));
      }
    }

    if((unsigned int)pipe->icc_intent < DT_INTENT_LAST)
    {
      // User defined explicitly an intent in export box: use that
      p->intent = pipe->icc_intent;
    }
    else
    {
      // No intent defined : save with input intent
      dt_iop_order_iccprofile_info_t *icc_input = dt_ioppr_get_pipe_input_profile_info(pipe);
      if(icc_input) p->intent = icc_input->intent;
    }

    out_type = p->type;
    out_filename = p->filename;
    out_intent = p->intent;
  }
  else if(pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
  {
    out_type = DT_COLORSPACE_ADOBERGB;
    out_filename = "";
    out_intent = darktable.color_profiles->display_intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    out_type = darktable.color_profiles->display_type;
    out_filename = darktable.color_profiles->display_filename;
    out_intent = darktable.color_profiles->display_intent;
  }

  // when the output type is Lab then process is a nop, so we can avoid creating a transform
  // and the subsequent error messages
  d->type = out_type;
  if(out_type == DT_COLORSPACE_LAB)
    return;

  // Resolve the working profile currently carried by the pipe.
  if(work_profile)
  {
    work_type = work_profile->type;
    work_filename = work_profile->filename;
  }
  else
  {
    dt_ioppr_get_work_profile_type(self->dev, &work_type, &work_filename);
  }

  /*
   * Setup transform flags
   */
  uint32_t transformFlags = 0;

  /* creating output profile */
  if(out_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

  const dt_colorspaces_color_profile_t *out_profile
      = dt_colorspaces_get_profile(out_type, out_filename,
                                   DT_PROFILE_DIRECTION_OUT
                                   | DT_PROFILE_DIRECTION_DISPLAY);
  if(out_profile)
  {
    // Path for internal profile or external ICC file
    output = out_profile->profile;
    if(out_type == DT_COLORSPACE_XYZ) output_format = TYPE_XYZA_FLT;
  }
  else if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
  {
    // Export with no explicit profile specified : use input file embedded profile
    gboolean new_profile;
    output = dt_colorspaces_get_embedded_profile(pipe->image.id, &out_type, &new_profile);
  }

  // We don't have an internal, embedded or external file profile,
  // just fall back to sRGB
  if(!output)
  {
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                        DT_PROFILE_DIRECTION_OUT
                                        | DT_PROFILE_DIRECTION_DISPLAY)
                 ->profile;
    dt_control_log(_("missing output profile has been replaced by sRGB!"));
    fprintf(stderr, "missing output profile `%s' has been replaced by sRGB!\n",
            dt_colorspaces_get_name(out_type, out_filename));
  }

  if(work_type != DT_COLORSPACE_NONE)
  {
    const dt_colorspaces_color_profile_t *in_profile
        = dt_colorspaces_get_profile(work_type, work_filename ? work_filename : "", DT_PROFILE_DIRECTION_ANY);
    if(in_profile) input = in_profile->profile;
  }
  if(!input)
  {
    input = output;
    dt_print(DT_DEBUG_DEV,
             "[colorout] could not resolve pipeline work profile, assuming input is already in output profile\n");
  }

  /* creating softproof profile if softproof is enabled */
  if(d->mode != DT_PROFILE_NORMAL && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    const dt_colorspaces_color_profile_t *prof = dt_colorspaces_get_profile
      (darktable.color_profiles->softproof_type,
       darktable.color_profiles->softproof_filename,
       DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY);

    if(prof)
      softproof = prof->profile;
    else
    {
      softproof = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                             DT_PROFILE_DIRECTION_OUT
                                             | DT_PROFILE_DIRECTION_DISPLAY)
                      ->profile;
      dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
      fprintf(stderr, "missing softproof profile `%s' has been replaced by sRGB!\n",
              dt_colorspaces_get_name(darktable.color_profiles->softproof_type,
                                      darktable.color_profiles->softproof_filename));
    }

    // some of our internal profiles are what lcms considers ideal profiles as they have a parametric TRC so
    // taking a roundtrip through those profiles during softproofing has no effect. as a workaround we have to
    // make lcms quantisize those gamma tables to get the desired effect.
    // in case that fails we don't enable softproofing.
    softproof = _make_clipping_profile(softproof);
    if(softproof)
    {
      /* TODO: the use of bpc should be userconfigurable either from module or preference pane */
      /* softproof flag and black point compensation */
      transformFlags |= cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION;

      if(d->mode == DT_PROFILE_GAMUTCHECK) transformFlags |= cmsFLAGS_GAMUTCHECK;
    }
  }

  /*
   * NOTE: theoretically, we should be passing
   * UsedDirection = LCMS_USED_AS_PROOF  into
   * dt_colorspaces_get_matrix_from_output_profile() so that
   * dt_colorspaces_get_matrix_from_profile() knows it, but since we do not try
   * to use our matrix codepath when softproof is enabled, this seemed redundant.
   */

  const gboolean can_use_fast_matrix = (d->mode == DT_PROFILE_NORMAL
                                        && !force_lcms2
                                        && work_profile
                                        && !work_profile->nonlinearlut
                                        && !isnan(work_profile->matrix_in[0][0]));

  // matrix fast path: work RGB -> XYZ from work profile, then XYZ -> output RGB from output profile
  if(can_use_fast_matrix)
  {
    dt_colormatrix_t output_matrix;
    if(!dt_colorspaces_get_matrix_from_output_profile(output, output_matrix, d->lut[0], d->lut[1], d->lut[2],
                                                      LUT_SAMPLES))
      dt_colormatrix_mul(d->cmatrix, output_matrix, work_profile->matrix_in);
    else
      d->cmatrix[0][0] = NAN;
  }

  if(isnan(d->cmatrix[0][0]))
  {
      d->cmatrix[0][0] = NAN;
      piece->process_cl_ready = 0;
      d->xform = cmsCreateProofingTransform(input, TYPE_RGBA_FLT, output, output_format, softproof,
                                          out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
  }

  // user selected a non-supported output profile, check that:
  if(!d->xform && isnan(d->cmatrix[0][0]))
  {
    const char *const unsupported_name
        = out_profile ? out_profile->name : dt_colorspaces_get_name(out_type, out_filename);
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    fprintf(stderr, "unsupported output profile `%s' has been replaced by sRGB!\n", unsupported_name);
    output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_OUT)->profile;

    if(can_use_fast_matrix)
    {
      dt_colormatrix_t output_matrix;
      if(!dt_colorspaces_get_matrix_from_output_profile(output, output_matrix, d->lut[0], d->lut[1], d->lut[2],
                                                        LUT_SAMPLES))
        dt_colormatrix_mul(d->cmatrix, output_matrix, work_profile->matrix_in);
      else
        d->cmatrix[0][0] = NAN;
    }

    if(isnan(d->cmatrix[0][0]))
    {
      d->cmatrix[0][0] = NAN;
      piece->process_cl_ready = 0;

      d->xform = cmsCreateProofingTransform(input, TYPE_RGBA_FLT, output, output_format, softproof,
                                            out_intent, INTENT_RELATIVE_COLORIMETRIC, transformFlags);
    }
  }

  if(out_type == DT_COLORSPACE_DISPLAY)
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

  // now try to initialize unbounded mode:
  // we do extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { extrapolate_lut(d->lut[k], x[0], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[1], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[2], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[3], LUT_SAMPLES) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  // softproof is never the original but always a copy that went through _make_clipping_profile()
  dt_colorspaces_cleanup_profile(softproof);

  dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe, d->type, out_filename, p->intent);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_colorout_data_t));
  piece->data_size = sizeof(dt_iop_colorout_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  if(d->xform)
  {
    cmsDeleteTransform(d->xform);
    d->xform = NULL;
  }

  dt_free_align(piece->data);
  piece->data = NULL;
}


void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

typedef struct dt_iop_colorout_gui_data_t
{ } dt_iop_colorout_gui_data_t;

dt_iop_colorout_gui_data_t dummy;

void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(colorout);
  self->widget = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(self->widget),_("Convert images to the display or export RGB color space. "
                                                 "The color profile is set in the export module or in the display preferences. "));
  gtk_widget_set_halign(self->widget, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(self->widget), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(self->widget), TRUE);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
