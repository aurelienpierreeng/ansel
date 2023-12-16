/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_restorescans_t)

typedef struct dt_iop_restorescans_t
{
  float C_c; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "input cyan"
  float C_m; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input magenta"
  float C_y; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input yellow"
  float C_o; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "cyan offset"
  float M_c; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input cyan"
  float M_m; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "input magenta"
  float M_y; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input yellow"
  float M_o; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "magenta offset"
  float Y_c; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input cyan"
  float Y_m; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "input magenta"
  float Y_y; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "input yellow"
  float Y_o; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "yellow offset"
  float diffusion; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "sharpening"
  float regularization; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "edge avoiding"
  int iterations; // $MIN: 1 $MAX: 32 $DEFAULT: 1 $DESCRIPTION: "iterations"
} dt_iop_restorescans_t;

typedef struct dt_iop_restorescans_gui_data_t
{
  GtkWidget *C_c, *C_m, *C_y, *C_o;
  GtkWidget *M_c, *M_m, *M_y, *M_o;
  GtkWidget *Y_c, *Y_m, *Y_y, *Y_o;
  GtkWidget *diffusion, *regularization, *iterations;
} dt_iop_restorescans_gui_data_t;

typedef struct dt_iop_restorescans_data_t
{
  dt_colormatrix_t CMY;
  dt_aligned_pixel_t offsets;
  float diffusion, regularization, iterations;
} dt_iop_restorescans_data_t;


// this returns a translatable name
const char * name()
{
  // make sure you put all your translatable strings into _() !
  return _("scan restore");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_FILM;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

#define SET_PIXEL(array, x, y, z, w)                                                                              \
  array[0] = x;                                                                                                   \
  array[1] = y;                                                                                                   \
  array[2] = z;                                                                                                   \
  array[3] = w;

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_restorescans_data_t *d = (dt_iop_restorescans_data_t *)piece->data;
  dt_iop_restorescans_t *p = (dt_iop_restorescans_t *)p1;

  SET_PIXEL(d->CMY[0], p->C_c, p->C_m, p->C_y, 0.f);
  SET_PIXEL(d->CMY[1], p->M_c, p->M_m, p->M_y, 0.f);
  SET_PIXEL(d->CMY[2], p->Y_c, p->Y_m, p->Y_y, 0.f);
  SET_PIXEL(d->offsets, p->M_o, p->C_o, p->Y_o, 0.f);

  d->diffusion = p->diffusion;
  d->iterations = p->iterations;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(64, sizeof(dt_iop_restorescans_data_t));
  piece->data_size = sizeof(dt_iop_restorescans_data_t);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;    // input buffer + output buffer; increase if additional memory allocated
  tiling->factor_cl = 2.0f; // same, but for OpenCL code path running on GPU
  tiling->maxbuf = 1.0f;    // largest buffer needed regardless of how tiling splits up the processing
  tiling->maxbuf_cl = 1.0f; // same, but for running on GPU
  tiling->overhead = 0;     // number of bytes of fixed overhead
  tiling->overlap = 1;      // how many pixels do we need to access from the neighboring tile?
  tiling->xalign = 1;
  tiling->yalign = 1;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_restorescans_data_t *d = (dt_iop_restorescans_data_t *)piece->data;
  const float scale = piece->iscale / roi_in->scale;

  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;
  float *const restrict cmy = dt_alloc_align_float(roi_in->width * roi_in->height * 4);

  const float sharpen = d->diffusion / (scale * scale) / d->iterations;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(ivoid, ovoid, roi_in, roi_out, in, out, cmy)
#endif
  for(int i = 0; i < roi_out->height; i++)
    for(int j = 0; j < roi_out->width; j++)
    {
      const size_t index = ((i * roi_out->width) + j) * 4;
      for_four_channels(c, aligned(in, cmy)) cmy[index + c] = 1.f - in[index + c];
    }

  float *restrict temp_in = cmy;
  float *restrict temp_out = out;

  for(int iter = 0; iter < d->iterations; iter++)
  {
    if(iter == 0)
    {
      temp_in = cmy;
      temp_out = out;
    }
    else if(iter % 2 != 0)
    {
      temp_in = out;
      temp_out = cmy;
    }
    else
    {
      temp_in = cmy;
      temp_out = out;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(d, temp_in, roi_in, roi_out)
#endif
    for(int i = 0; i < roi_out->height; i++)
      for(int j = 0; j < roi_out->width; j++)
      {
        const size_t index = ((i * roi_out->width) + j) * 4;
        dt_aligned_pixel_t temp1, temp2;
        for_four_channels(c, aligned(temp1, temp_in)) temp1[c] = temp_in[index + c] - d->offsets[c];
        dot_product(temp1, d->CMY, temp2);
        for_four_channels(c, aligned(temp2, temp_in)) temp_in[index + c] = CLAMP(((d->iterations - 1) * temp_in[index + c] + temp2[c]) / (d->iterations), 0.f, 1.f);
      }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(d, temp_in, temp_out, roi_in, roi_out, sharpen)
#endif
    for(int i = 0; i < roi_out->height; i++)
      for(int j = 0; j < roi_out->width; j++)
      {
        const size_t index = ((i * roi_out->width) + j) * 4;

        // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
        // for references (Oono & Puri)
        const float kernel[3][3] = { { 0.25f, 0.5f, 0.25f }, { 0.5f, -3.f, 0.5f }, { 0.25f, 0.5f, 0.25f } };

        dt_aligned_pixel_t laplacian = { 0.f };

        for(int ii = 0; ii < 3; ii++)
          for(int jj = 0; jj < 3; jj++)
          {
            const int row = CLAMP(i + ii - 1, 0, roi_in->height - 1);
            const int column = CLAMP(j + jj - 1, 0, roi_in->width - 1);
            const size_t idx = ((row * roi_in->width) + column) * 4;
            for_each_channel(c) laplacian[c] += temp_in[idx + c] * kernel[ii][jj];
          }

        for_each_channel(c) temp_out[index + c] = CLAMP(temp_in[index + c] - laplacian[c] * sharpen, 0.f, 1.f);
      }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(roi_in, roi_out, out, temp_out)
#endif
  for(int i = 0; i < roi_out->height; i++)
    for(int j = 0; j < roi_out->width; j++)
    {
      const size_t index = ((i * roi_out->width) + j) * 4;
      for_four_channels(c, aligned(temp_out, out)) out[index + c] = 1.f - temp_out[index + c];
    }

  dt_free_align(cmy);
}


#if 0
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_restorescans_t *p = (dt_iop_restorescans_t *)self->params;
  dt_iop_restorescans_gui_data_t *g = (dt_iop_restorescans_gui_data_t *)self->gui_data;

  // This automatically gets called when any of the color pickers set up with
  // dt_color_picker_new in gui_init is used. If there is more than one,
  // check which one is active first.
  if(picker == g->factor)
  {
    p->factor = self->picked_color[1];
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_control_queue_redraw_widget(self->widget);
}
#endif

void gui_init(dt_iop_module_t *self)
{
  dt_iop_restorescans_gui_data_t *g = IOP_GUI_ALLOC(restorescans);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->C_c = dt_bauhaus_slider_from_params(self, "C_c");
  g->C_m = dt_bauhaus_slider_from_params(self, "C_m");
  g->C_y = dt_bauhaus_slider_from_params(self, "C_y");
  g->C_o = dt_bauhaus_slider_from_params(self, "C_o");

  g->M_c = dt_bauhaus_slider_from_params(self, "M_c");
  g->M_m = dt_bauhaus_slider_from_params(self, "M_m");
  g->M_y = dt_bauhaus_slider_from_params(self, "M_y");
  g->M_o = dt_bauhaus_slider_from_params(self, "M_o");

  g->Y_c = dt_bauhaus_slider_from_params(self, "Y_c");
  g->Y_m = dt_bauhaus_slider_from_params(self, "Y_m");
  g->Y_y = dt_bauhaus_slider_from_params(self, "Y_y");
  g->Y_o = dt_bauhaus_slider_from_params(self, "Y_o");

  g->diffusion = dt_bauhaus_slider_from_params(self, "diffusion");
  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
