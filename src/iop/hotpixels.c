/*
    This file is part of darktable,
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011-2013, 2016 johannes hanika.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012-2013 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013, 2020 Aldric Renaudin.
    Copyright (C) 2014, 2016 Dan Torop.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015-2016 Pedro Côrte-Real.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018, 2020, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018, 2020, 2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2018 Stéphane Gourichon.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    
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
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/develop.h"
#include "dtgtk/resetlabel.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#ifdef HAVE_OPENCL
#include "common/opencl.h"
#endif

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_hotpixels_params_t)

typedef struct dt_iop_hotpixels_params_t
{
  float strength;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.25
  float threshold; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.05
  gboolean markfixed;  // $DEFAULT: FALSE $DESCRIPTION: "mark fixed pixels"
  gboolean permissive; // $DEFAULT: FALSE $DESCRIPTION: "detect by 3 neighbors"
} dt_iop_hotpixels_params_t;

typedef struct dt_iop_hotpixels_gui_data_t
{
  GtkWidget *threshold, *strength;
  GtkToggleButton *permissive;
} dt_iop_hotpixels_gui_data_t;

typedef struct dt_iop_hotpixels_data_t
{
  uint32_t filters;
  float threshold;
  float multiplier;
  gboolean permissive;
} dt_iop_hotpixels_data_t;

typedef struct dt_iop_hotpixels_global_data_t
{
  int kernel_hotpixels_bayer;
  int kernel_hotpixels_xtrans;
} dt_iop_hotpixels_global_data_t;


const char *name()
{
  return _("hot pixels");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remove abnormally bright pixels by dampening them with neighbours"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("reconstruction, raw"),
                                      _("linear, raw, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_REPAIR;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 1;
  dt_iop_buffer_dsc_update_bpp(dsc);
}

static inline void hotpixels_testone(const float *const in, const float mid, const int offset,
                                     int *const count, float *const maxin)
{
  const float other = in[offset];
  if(mid > other)
  {
    (*count)++;
    if(other > *maxin) *maxin = other;
  }
}

/* Detect hot sensor pixels based on the 4 surrounding sites. Pixels
 * having 3 or 4 (depending on permissive setting) surrounding pixels that
 * than value*multiplier are considered "hot", and are replaced by the maximum of
 * the neighbour pixels. The permissive variant allows for
 * correcting pairs of hot pixels in adjacent sites. Replacement using
 * the maximum produces fewer artifacts when inadvertently replacing
 * non-hot pixels.
 * This is the Bayer sensor variant. */
__DT_CLONE_TARGETS__
static void process_bayer(const dt_iop_hotpixels_data_t *data,
                         const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_out)
{
  const float threshold = data->threshold;
  const float multiplier = data->multiplier;
  const int min_neighbours = data->permissive ? 3 : 4;
  const int width = roi_out->width;
  const int widthx2 = width * 2;
  __OMP_PARALLEL_FOR__()
  for(int row = 2; row < roi_out->height - 2; row++)
  {
    const float *in = (float *)ivoid + (size_t)width * row + 2;
    float *out = (float *)ovoid + (size_t)width * row + 2;
    for(int col = 2; col < width - 2; col++, in++, out++)
    {
      float mid = *in * multiplier;
      if(*in > threshold)
      {
        int count = 0;
        float maxin = 0.0;
        hotpixels_testone(in, mid, -2, &count, &maxin);
        hotpixels_testone(in, mid, -widthx2, &count, &maxin);
        hotpixels_testone(in, mid, +2, &count, &maxin);
        hotpixels_testone(in, mid, +widthx2, &count, &maxin);
        if(count >= min_neighbours)
        {
          *out = maxin;
        }
      }
    }
  }
}

/* X-Trans sensor equivalent of process_bayer(). */
__DT_CLONE_TARGETS__
static void process_xtrans(const dt_iop_hotpixels_data_t *data,
                          const void *const ivoid, void *const ovoid,
                          const dt_iop_roi_t *const roi_out, const uint8_t (*const xtrans)[6])
{
  // for each cell of sensor array, pre-calculate, a list of the x/y
  // offsets of the four radially nearest pixels of the same color
  int offsets[6][6][4][2];
  // increasing offsets from pixel to find nearest like-colored pixels
  const int search[20][2] = { { -1, 0 }, { 1, 0 },  { 0, -1 },  { 0, 1 },  { -1, -1 }, { -1, 1 },  { 1, -1 },
                              { 1, 1 },  { -2, 0 }, { 2, 0 },   { 0, -2 }, { 0, 2 },   { -2, -1 }, { -2, 1 },
                              { 2, -1 }, { 2, 1 },  { -1, -2 }, { 1, -2 }, { -1, 2 },  { 1, 2 } };

  for(int j = 0; j < 6; ++j)
  {
    for(int i = 0; i < 6; ++i)
    {
      const uint8_t c = FCxtrans(j, i, roi_out, xtrans);
      for(int s = 0, found = 0; s < 20 && found < 4; ++s)
      {
        if(c == FCxtrans(j + search[s][1], i + search[s][0], roi_out, xtrans))
        {
          offsets[j][i][found][0] = search[s][0];
          offsets[j][i][found][1] = search[s][1];
          ++found;
        }
      }
    }
  }

  const float threshold = data->threshold;
  const float multiplier = data->multiplier;
  const int min_neighbours = data->permissive ? 3 : 4;
  const int width = roi_out->width;
  __OMP_PARALLEL_FOR__()
  for(int row = 2; row < roi_out->height - 2; row++)
  {
    const float *in = (float *)ivoid + (size_t)width * row + 2;
    float *out = (float *)ovoid + (size_t)width * row + 2;
    for(int col = 2; col < width - 2; col++, in++, out++)
    {
      float mid = *in * multiplier;
      if(*in > threshold)
      {
        int count = 0;
        float maxin = 0.0;
        for(int n = 0; n < 4; ++n)
        {
          int xx = offsets[row % 6][col % 6][n][0];
          int yy = offsets[row % 6][col % 6][n][1];
          float other = *(in + xx + yy * (size_t)width);
          if(mid > other)
          {
            count++;
            if(other > maxin) maxin = other;
          }
        }
        // NOTE: it seems that detecting by 2 neighbors would help for extreme cases
        if(count >= min_neighbours)
        {
          *out = maxin;
        }
      }
    }
  }
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_hotpixels_data_t *data = (dt_iop_hotpixels_data_t *)piece->data;

  // The processing loop should output only a few pixels, so just copy everything first
  dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 1);

  if(piece->dsc_in.filters == 9u)
  {
    process_xtrans(data, ivoid, ovoid, roi_out, (const uint8_t(*const)[6])piece->dsc_in.xtrans);
  }
  else
  {
    process_bayer(data, ivoid, ovoid, roi_out);
  }

  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
               const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_hotpixels_data_t *const data = (dt_iop_hotpixels_data_t *)piece->data;
  dt_iop_hotpixels_global_data_t *const gd = (dt_iop_hotpixels_global_data_t *)self->global_data;

  const int devid = pipe->devid;
  const int min_neighbours = data->permissive ? 3 : 4;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const uint32_t filters = piece->dsc_in.filters;
  cl_mem dev_xtrans = NULL;
  cl_int err = 0;
  int kernel = filters == 9u ? gd->kernel_hotpixels_xtrans : gd->kernel_hotpixels_bayer;

  if(filters == 9u)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans), (void *)piece->dsc_in.xtrans);
    if(IS_NULL_PTR(dev_xtrans)) goto error;
  }

  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&data->threshold);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&data->multiplier);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), (void *)&min_neighbours);

  if(filters == 9u)
  {
    const int rx = roi_out->x;
    const int ry = roi_out->y;
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), (void *)&rx);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), (void *)&ry);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), (void *)&dev_xtrans);
  }

  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_xtrans);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_xtrans);
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl
  dt_iop_hotpixels_global_data_t *gd = (dt_iop_hotpixels_global_data_t *)malloc(sizeof(dt_iop_hotpixels_global_data_t));
  module->data = gd;
  gd->kernel_hotpixels_bayer = dt_opencl_create_kernel(program, "hotpixels_bayer");
  gd->kernel_hotpixels_xtrans = dt_opencl_create_kernel(program, "hotpixels_xtrans");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_hotpixels_global_data_t *gd = (dt_iop_hotpixels_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_hotpixels_bayer);
  dt_opencl_free_kernel(gd->kernel_hotpixels_xtrans);
  dt_free(module->data);
}
#endif

void reload_defaults(dt_iop_module_t *module)
{
  const dt_image_t *img = &module->dev->image_storage;
  const gboolean enabled = dt_image_is_raw(img) && !dt_image_is_monochrome(img);
  // can't be switched on for non-raw images:
  module->hide_enable_button = !enabled;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)params;
  dt_iop_hotpixels_data_t *d = (dt_iop_hotpixels_data_t *)piece->data;
  d->filters = pipe->dev->image_storage.dsc.filters;
  d->multiplier = p->strength / 2.0;
  d->threshold = p->threshold;
  d->permissive = p->permissive;

  const dt_image_t *img = &pipe->dev->image_storage;
  const gboolean enabled = dt_image_is_raw(img) && !dt_image_is_monochrome(img);

  if(!enabled || p->strength == 0.0) piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_hotpixels_data_t));
  piece->data_size = sizeof(dt_iop_hotpixels_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}


void gui_update(dt_iop_module_t *self)
{
  dt_iop_hotpixels_gui_data_t *g = (dt_iop_hotpixels_gui_data_t *)self->gui_data;
  dt_iop_hotpixels_params_t *p = (dt_iop_hotpixels_params_t *)self->params;
  gtk_toggle_button_set_active(g->permissive, p->permissive);

  const dt_image_t *img = &self->dev->image_storage;
  const gboolean enabled = dt_image_is_raw(img) && !dt_image_is_monochrome(img);
  // can't be switched on for non-raw images:
  self->hide_enable_button = !enabled;

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->hide_enable_button ? "non_raw" : "raw");
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_hotpixels_gui_data_t *g = IOP_GUI_ALLOC(hotpixels);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_from_params(self, N_("threshold"));
  dt_bauhaus_slider_set_digits(g->threshold, 4);
  gtk_widget_set_tooltip_text(g->threshold, _("lower threshold for hot pixel"));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_digits(g->strength, 4);
  gtk_widget_set_tooltip_text(g->strength, _("strength of hot pixel correction"));

  // 3 neighbours
  g->permissive = GTK_TOGGLE_BUTTON(dt_bauhaus_toggle_from_params(self, "permissive"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("hot pixel correction\nonly works for raw images."));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
