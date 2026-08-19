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
#include "config.h"
#include "system/simd.h"
#endif
#include "develop/imageop_gui.h"
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include "colorprofiles/colorspaces.h"
#include "colorprofiles/conversion.h"
#include "math/matrices.h"
#include "common/imagebuf.h"
#include "develop/iop_profile.h"
#include "common/opencl.h"
#include "common/conf.h"
#include "control/user_message.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

#include "iop/iop_api.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "imageio/imageio_profile.h"

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512

DT_MODULE_INTROSPECTION(5, dt_iop_colorout_params_t)

typedef struct dt_iop_colorout_data_t
{
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_mode_t mode;
  /* Identity of the conversion below, and the only thing in this struct that describes it.
   * runtime_data_hash() folds this prefix into the pipeline cache key, and the conversion is
   * opaque heap state whose address says nothing about the pixels it produces -- so the
   * address is deliberately kept OUT of the hashed prefix and its identity carried here
   * instead. See dt_colorspaces_conversion_identity(). */
  uint64_t conversion_id;

  /* --- runtime pointers, deliberately after the hashed prefix (see init_pipe) --- */

  /* The whole of "how do we get from the pipeline's working space to the output space" --
   * matrices, tone curves, extrapolation fits, or an lcms2 proofing transform. Built by
   * colorprofiles/conversion.c, which is where the lifetime and locking rules for the
   * profiles behind it are written down and enforced. NULL when the module is a nop
   * (a Lab output). */
  dt_colorspaces_conversion_t *conversion;
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

static dt_iop_colorspace_type_t _colorout_input_format_cst(dt_iop_module_t *self,
                                                           const dt_dev_pixelpipe_t *pipe)
{
  const dt_iop_colorout_params_t *const p = (dt_iop_colorout_params_t *)self->params;
  dt_colorspaces_color_profile_type_t type = p->type;

  /* Export overrides are applied in commit_params(), but the sealed pipeline needs the buffer
   * contract before that. Mirror the only colorspace-affecting override here so colorout
   * advertises the current RGB/Lab contract instead of the previous image runtime state. */
  if(pipe->type == DT_DEV_PIXELPIPE_EXPORT && pipe->icc_type != DT_COLORSPACE_NONE)
    type = pipe->icc_type;

  return (type == DT_COLORSPACE_LAB) ? IOP_CS_LAB : IOP_CS_RGB;
}

static dt_iop_colorspace_type_t _colorout_output_format_cst(dt_iop_module_t *self,
                                                            const dt_dev_pixelpipe_t *pipe)
{
  const dt_iop_colorspace_type_t input_cst = _colorout_input_format_cst(self, pipe);
  return input_cst == IOP_CS_LAB ? IOP_CS_LAB : IOP_CS_RGB_DISPLAY;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorout_input_format_cst(self, pipe);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorout_output_format_cst(self, pipe);
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
  if(IS_NULL_PTR(gd)) return;
  module->data = gd;
  gd->kernel_colorout = dt_opencl_create_kernel(program, "colorout");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorout);
  dt_free(module->data);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  dt_iop_colorout_global_data_t *gd = (dt_iop_colorout_global_data_t *)self->global_data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int devid = pipe->devid;
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

  /* The kernel needs the numbers the conversion reduced to. It cannot call back into
   * colorprofiles, so this is the one place the module reads them -- to upload them, never
   * to run the conversion itself; that is dt_colorspaces_apply_conversion()'s job on the
   * host side. `linear_ramp` stands in for a channel with no tone curve: the kernel reads
   * the first sample as the "is this channel linear" marker, exactly as the CPU path does. */
  dt_colormatrix_t conversion_matrix;
  if(!dt_colorspaces_conversion_matrix(d->conversion, conversion_matrix)) goto error;

  float cmatrix[12];
  pack_3xSSE_to_3x4(conversion_matrix, cmatrix);
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, cmatrix);
  if(IS_NULL_PTR(dev_m)) goto error;

  const float *const curve_r = dt_colorspaces_conversion_target_curve(d->conversion, 0);
  const float *const curve_g = dt_colorspaces_conversion_target_curve(d->conversion, 1);
  const float *const curve_b = dt_colorspaces_conversion_target_curve(d->conversion, 2);
  const float *const coeffs = dt_colorspaces_conversion_target_coeffs(d->conversion);
  if(IS_NULL_PTR(curve_r) || IS_NULL_PTR(curve_g) || IS_NULL_PTR(curve_b) || IS_NULL_PTR(coeffs)) goto error;

  dev_r = dt_opencl_copy_host_to_device(devid, (void *)curve_r, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_r)) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, (void *)curve_g, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_g)) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, (void *)curve_b, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_b)) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (void *)coeffs);
  if(IS_NULL_PTR(dev_coeffs)) goto error;
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
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
            void *const ovoid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_colorout_data_t *const d = (dt_iop_colorout_data_t *)piece->data;

  /* Lab output is a nop -- the pipe already carries Lab -- and leaves `conversion` NULL. */
  if(d->type == DT_COLORSPACE_LAB || IS_NULL_PTR(d->conversion))
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
  else
    dt_colorspaces_apply_conversion(d->conversion, DT_IS_ALIGNED((const float *)ivoid),
                                    DT_IS_ALIGNED((float *)ovoid), roi_out->width, roi_out->height);

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
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

  /* One snapshot for the whole of commit_params. The display triple and the soft-proof
   * pair were read as six separate loads spread over ~170 lines, on a pipeline thread,
   * while the GUI thread wrote them unlocked: a resolve could see a new type with the
   * previous filename, or build a proofing transform against a profile that had already
   * been replaced since `mode` was read. It also makes out_filename below a copy rather
   * than a pointer into the module's mutable state. */
  dt_colorprofiles_settings_t settings;
  dt_colorprofiles_get_settings(&settings);

  d->mode = (pipe->type == DT_DEV_PIXELPIPE_FULL) ? settings.mode : DT_PROFILE_NORMAL;

  // Softproof and gamut check take input from GUI and don't write it in internal parameters.
  // The cacheline integrity hash will not be meaningful in this scenario,
  // we need to bypass the cache entirely in these modes.
  dt_iop_set_cache_bypass(self, (d->mode != DT_PROFILE_NORMAL));

  dt_colorspaces_free_conversion(&d->conversion);
  /* Cleared here rather than beside each prepare below, so that every path which returns
   * without building a conversion -- a Lab output -- leaves a hashed prefix that says so. */
  d->conversion_id = 0;
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
    out_intent = settings.display_intent;
  }
  else
  {
    /* we are not exporting, using display profile as output */
    out_type = settings.display_type;
    out_filename = settings.display_filename;
    out_intent = settings.display_intent;
  }

  // when the output type is Lab then process is a nop, so we can avoid creating a transform
  // and the subsequent error messages
  d->type = out_type;
  if(out_type == DT_COLORSPACE_LAB)
    return;

  // Resolve the working profile currently carried by the pipe.
  if(!IS_NULL_PTR(work_profile))
  {
    work_type = work_profile->type;
    work_filename = work_profile->filename;
  }
  else
  {
    dt_ioppr_get_work_profile_type(self->dev, &work_type, &work_filename);
  }

  /* The output profile may not be in the profile list at all: an export that names no colour
   * space uses the ICC embedded in the source file, which belongs to that one image. Resolve
   * that case into a container the conversion can take as an endpoint, and release it after
   * the conversion is built -- an lcms2 transform does not retain the profiles it was made
   * from, so the container only has to outlive the preparation. */
  const dt_colorspaces_profile_role_t output_role
      = DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR;

  struct dt_colorspaces_color_profile_t *image_output = NULL;
  if(!dt_colorspaces_profile_exists(output_role, out_type, out_filename))
  {
    if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
      image_output = dt_image_get_embedded_output_profile(pipe->dev->image_storage.id, &out_type);

    if(IS_NULL_PTR(image_output))
    {
      dt_control_log(_("missing output profile has been replaced by sRGB!"));
      fprintf(stderr, "missing output profile `%s' has been replaced by sRGB!\n",
              dt_colorspaces_get_name(out_type, out_filename));
      out_type = DT_COLORSPACE_SRGB;
      out_filename = "";
    }
  }

  dt_colorspaces_endpoint_t source = { .type = work_type,
                                       .filename = work_filename ? work_filename : "",
                                       .role = DT_PROFILE_ROLE_ANY };
  dt_colorspaces_endpoint_t target = { .type = out_type,
                                       .filename = out_filename,
                                       .role = output_role,
                                       .resolved = image_output };
  dt_colorspaces_endpoint_t proof = { .type = settings.softproof_type,
                                      .filename = settings.softproof_filename,
                                      .role = output_role };

  /* No working profile on this pipe at all: convert nothing, the buffer is taken to be in
   * the output space already. That is what "input = output" meant before. */
  if(work_type == DT_COLORSPACE_NONE)
  {
    dt_print(DT_DEBUG_DEV,
             "[colorout] could not resolve pipeline work profile, assuming input is already in output profile\n");
    source = target;
  }

  const gboolean softproofing = (d->mode != DT_PROFILE_NORMAL && pipe->type == DT_DEV_PIXELPIPE_FULL);
  if(softproofing
     && !dt_colorspaces_profile_exists(output_role, settings.softproof_type, settings.softproof_filename))
  {
    dt_control_log(_("missing softproof profile has been replaced by sRGB!"));
    fprintf(stderr, "missing softproof profile `%s' has been replaced by sRGB!\n",
            dt_colorspaces_get_name(settings.softproof_type, settings.softproof_filename));
    proof.type = DT_COLORSPACE_SRGB;
    proof.filename = "";
  }

  /* The kernel this module owns applies the OUTPUT profile's encoding curves after the
   * matrix and has no stage for the input side, so a non-linear working profile has to go
   * through lcms2 -- which is what the old `!work_profile->nonlinearlut` gate said, in the
   * form of a condition the module had to remember to write. */
  dt_colorspaces_conversion_flags_t flags = DT_CONVERSION_TARGET_CURVES;
  if(force_lcms2) flags |= DT_CONVERSION_FORCE_LCMS2;
  if(d->mode == DT_PROFILE_GAMUTCHECK) flags |= DT_CONVERSION_GAMUTCHECK;

  d->conversion = dt_colorspaces_prepare_conversion(&source, &target, NULL,
                                                    softproofing ? &proof : NULL, out_intent, flags);

  if(IS_NULL_PTR(d->conversion))
  {
    /* Whatever the user asked for, we cannot render it. sRGB is the fallback that has always
     * been here; saying so out loud is the point, since the exported file will not be in the
     * profile its name claims. */
    dt_control_log(_("unsupported output profile has been replaced by sRGB!"));
    fprintf(stderr, "unsupported output profile `%s' has been replaced by sRGB!\n",
            dt_colorspaces_get_name(out_type, out_filename));
    target.type = DT_COLORSPACE_SRGB;
    target.filename = "";
    target.resolved = NULL;
    if(work_type == DT_COLORSPACE_NONE) source = target;
    d->conversion = dt_colorspaces_prepare_conversion(&source, &target, NULL,
                                                      softproofing ? &proof : NULL, out_intent, flags);
  }

  /* After BOTH prepare attempts: what the sRGB fallback built is what will render. */
  d->conversion_id = dt_colorspaces_conversion_identity(d->conversion);

  dt_colorspaces_free_image_profile(image_output);

  /* Only the vectorised branch has a kernel. The lcms2 one is host-only, and saying so here
   * is the whole of what `process_cl_ready` needs to know. */
  if(!dt_colorspaces_conversion_is_matrix(d->conversion)) piece->process_cl_ready = 0;

  dt_ioppr_set_pipe_output_profile_info(self->dev, pipe, d->type, out_filename, p->intent);
}

gboolean runtime_data_hash(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  (void)piece;
  return TRUE;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_colorout_data_t));
  // Hash the rendering contract, stopping before the runtime pointer.
  piece->data_size = G_STRUCT_OFFSET(dt_iop_colorout_data_t, conversion);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorout_data_t *d = (dt_iop_colorout_data_t *)piece->data;
  dt_colorspaces_free_conversion(&d->conversion);

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
  self->gui->widget = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(self->gui->widget),_("Convert images to the display or export RGB color space. "
                                                 "The color profile is set in the export module or in the display preferences. "));
  gtk_widget_set_halign(self->gui->widget, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(self->gui->widget), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(self->gui->widget), TRUE);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
