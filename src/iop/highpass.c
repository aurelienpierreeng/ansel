/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define MAX_RADIUS 16

DT_MODULE_INTROSPECTION(1, dt_iop_highpass_params_t)

typedef struct dt_iop_highpass_params_t
{
  float sharpness; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float contrast;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "contrast boost"
} dt_iop_highpass_params_t;

typedef struct dt_iop_highpass_gui_data_t
{
  GtkWidget *sharpness, *contrast;
} dt_iop_highpass_gui_data_t;

typedef struct dt_iop_highpass_data_t
{
  float sharpness;
  float contrast;
} dt_iop_highpass_data_t;

typedef struct dt_iop_highpass_global_data_t
{
  int kernel_highpass_invert;
  int kernel_highpass_hblur;
  int kernel_highpass_vblur;
  int kernel_highpass_mix;
} dt_iop_highpass_global_data_t;


const char *name()
{
  return _("highpass");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("isolate high frequencies in the image"),
                                      _("creative"),
                                      _("linear or non-linear, Lab, scene-referred"),
                                      _("frequential, Lab"),
                                      _("special, Lab, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;

  const int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale));

  const float sigma = sqrtf((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);

  tiling->factor = 2.1f; // in + out + small slice for box_mean
  tiling->factor_cl = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = wdh;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)self->global_data;

  cl_int err = -999;
  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrtf((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);
  const int wd = 2 * wdh + 1;
  const size_t mat_size = (size_t)wd * sizeof(float);
  float *mat = malloc(mat_size);
  float *m = mat + wdh;
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -wdh; l <= wdh; l++) weight += m[l] = expf(-(l * l) / (2.f * sigma * sigma));
  for(int l = -wdh; l <= wdh; l++) m[l] /= weight;

  // for(int l=-wdh; l<=wdh; l++) printf("%.6f ", (double)m[l]);
  // printf("\n");

  float contrast_scale = ((d->contrast / 100.0f) * 7.5f);

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * wdh, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_highpass_hblur, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * wdh, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_highpass_vblur, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  size_t sizes[3];
  size_t local[3];

  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, mat_size, mat);
  if(dev_m == NULL) goto error;

  /* invert image */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_invert, sizes);
  if(err != CL_SUCCESS) goto error;

  if(rad != 0)
  {
    /* horizontal blur */
    sizes[0] = bwidth;
    sizes[1] = ROUNDUPDHT(height, devid);
    sizes[2] = 1;
    local[0] = hblocksize;
    local[1] = 1;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 6, sizeof(int), (void *)&hblocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 7, (hblocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_hblur, sizes, local);
    if(err != CL_SUCCESS) goto error;


    /* vertical blur */
    sizes[0] = ROUNDUPDWD(width, devid);
    sizes[1] = bheight;
    sizes[2] = 1;
    local[0] = 1;
    local[1] = vblocksize;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 6, sizeof(int), (void *)&vblocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 7, (vblocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_vblur, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  /* mixing tmp and in -> out */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 5, sizeof(float), (void *)&contrast_scale);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  free(mat);
  dt_print(DT_DEBUG_OPENCL, "[opencl_highpass] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highpass_data_t *data = (dt_iop_highpass_data_t *)piece->data;
  const float *const in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = 4;

  /* the blend code at the end assumes at least 4 channels, and we never get more than four */
  assert(piece->colors == ch);

/* create inverted image and then blur */
/* since we use only the L channel, pack the values together instead of every fourth float */
/* to reduce cache pressure and memory bandwidth during the blur operation */
  const size_t npixels = (size_t)roi_out->height * roi_out->width;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(in) \
  shared(out) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)npixels; k++)
    out[k] = 100.0f - LCLIP(in[4 * k]); // only L in Lab space

  const int rad = MAX_RADIUS * (fmin(100.0, data->sharpness + 1) / 100.0);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale));

  /* horizontal blur out into out */
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  dt_box_mean(out, roi_out->height, roi_out->width, 1, hr, BOX_ITERATIONS);

  const float contrast_scale = ((data->contrast / 100.0) * 7.5);
  /* Blend the inverted blurred L channel with the original input.  Because we packed the L values */
  /* and are inserting the result in the same buffer containing the L values, we need to work in */
  /* reverse order */
  /* We can only do the final 3/4 in parallel here, because updating the first quarter in one thread */
  /* would clobber values still needed by other threads. */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, contrast_scale, npixels) \
  dt_omp_sharedconst(in) \
  shared(out, data) \
  schedule(static)
#endif
  for(size_t k = npixels - 1; k > npixels/4; k--)
  {
    size_t index = ch * k;
    // Mix out and in
    const float L = out[k] * 0.5 + in[index] * 0.5;
    out[index] = LCLIP(50.0f + ((L - 50.0f) * contrast_scale));
    out[index + 1] = out[index + 2] = 0.0f; // desaturate a and b in Lab space
    out[index + 3] = in[index + 3]; // copy the alpha channel in case it is in use
  }
  /* process the final quarter of the pixels */
  for(ssize_t k = npixels/4; k >= 0; k--)
  {
    size_t index = ch * k;
    // Mix out and in
    const float L = out[k] * 0.5 + in[index] * 0.5;
    out[index] = LCLIP(50.0f + ((L - 50.0f) * contrast_scale));
    out[index + 1] = out[index + 2] = 0.0f; // desaturate a and b in Lab space
    out[index + 3] = in[index + 3]; // copy the alpha channel in case it is in use
  }

}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)p1;
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;

  d->sharpness = p->sharpness;
  d->contrast = p->contrast;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_highpass_data_t));
  piece->data_size = sizeof(dt_iop_highpass_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 4; // highpass.cl, from programs.conf
  dt_iop_highpass_global_data_t *gd
      = (dt_iop_highpass_global_data_t *)malloc(sizeof(dt_iop_highpass_global_data_t));
  module->data = gd;
  gd->kernel_highpass_invert = dt_opencl_create_kernel(program, "highpass_invert");
  gd->kernel_highpass_hblur = dt_opencl_create_kernel(program, "highpass_hblur");
  gd->kernel_highpass_vblur = dt_opencl_create_kernel(program, "highpass_vblur");
  gd->kernel_highpass_mix = dt_opencl_create_kernel(program, "highpass_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highpass_invert);
  dt_opencl_free_kernel(gd->kernel_highpass_hblur);
  dt_opencl_free_kernel(gd->kernel_highpass_vblur);
  dt_opencl_free_kernel(gd->kernel_highpass_mix);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_highpass_gui_data_t *g = IOP_GUI_ALLOC(highpass);

  g->sharpness = dt_bauhaus_slider_from_params(self, N_("sharpness"));
  dt_bauhaus_slider_set_format(g->sharpness, "%");
  gtk_widget_set_tooltip_text(g->sharpness, _("the sharpness of highpass filter"));

  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");
  dt_bauhaus_slider_set_format(g->contrast, "%");
  gtk_widget_set_tooltip_text(g->contrast, _("the contrast of highpass filter"));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
