/*
    This file is part of darktable,
    Copyright (C) 2015 johannes hanika.
    Copyright (C) 2015-2016 Roman Lebedev.
    Copyright (C) 2015 Ulrich Pegelow.
    Copyright (C) 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018, 2020-2021, 2023-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Hanno Schwalm.
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
#include "common/interpolation.h"
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"
#include "gui/gtk.h"

DT_MODULE_INTROSPECTION(1, dt_iop_finalscale_params_t)

typedef struct dt_iop_finalscale_params_t
{
  int dummy;
} dt_iop_finalscale_params_t;

typedef dt_iop_finalscale_params_t dt_iop_finalscale_data_t;

const char *name()
{
  return C_("modulename", "Final resampling");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


// see ../../doc/resizing-scaling.md for details
void modify_roi_in(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  int scaling_pref = dt_conf_get_int("darkroom/render_size");

  // Use cases :
  // 1. we run an export pipeline. We mandatorily get a 1:1 image, process it whole, downscale at the end.
  // 2. we run a GUI (darkroom) pipeline. If we upsample it, we want it done at the end of the pipe,
  // so sharpening and blurring is at most 1:1.
  if(roi_in->scale > 1.f)
  {
    roi_in->x = (int)roundf((float)roi_in->x / roi_out->scale);
    roi_in->y = (int)roundf((float)roi_in->y / roi_out->scale);
    roi_in->width  = (int)roundf(roi_out->width / roi_out->scale);
    roi_in->height = (int)roundf(roi_out->height / roi_out->scale);
    roi_in->scale = 1.0f;
  }
  // 3. we run the pipeline in full-res mode : force scale = 1 no matter what.
  else if(scaling_pref == 0)
  {    
    // x, y are in original-image coordinates — leave them unchanged.
    roi_in->width  = (int)roundf(roi_out->width / roi_out->scale);
    roi_in->height = (int)roundf(roi_out->height / roi_out->scale);
    roi_in->scale = 1.0f;
    const float resample_scale = roi_out->scale / roi_in->scale;
    roi_in->x = (int)roundf(roi_in->x / resample_scale);
    roi_in->y = (int)roundf(roi_in->y / resample_scale);
  }
  // else if(scaling_pref == 1) : we run the pipeline scaled at display resolution
  // nothing to resample and anyway commit_params() will self-disable
}

void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece,
                  const float *const in, float *const out, const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
}

int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  dt_iop_roi_t roi_in = piece->roi_in;
  dt_iop_roi_t roi_out = piece->roi_out;
  // ROI (x,y) are not used here since we don't crop but only scale.
  // Leaving them will offset the result, which is not what we want.
  roi_in.x = 0;
  roi_in.y = 0;
  roi_out.x = 0;
  roi_out.y = 0;
  dt_iop_clip_and_zoom_roi(ovoid, ivoid, &roi_out, &roi_in, roi_out.width, roi_in.width);
  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  dt_iop_roi_t roi_in = piece->roi_in;
  dt_iop_roi_t roi_out = piece->roi_out;
  // ROI (x,y) are not used here since we don't crop but only scale.
  // Leaving them will offset the result, which is not what we want.
  roi_in.x = 0;
  roi_in.y = 0;
  roi_out.x = 0;
  roi_out.y = 0;
  const int devid = pipe->devid;
  cl_int err = -999;

  err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_in, &roi_out, &roi_in);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_finalscale] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  // Depending on ROI zoom level, we may need to enable finalscale so the pipeline runs
  // at most at 100%, for pixel-level accuracy, and we upscale at the end. 
  // This is important for consistency with exports.
  const float darkroom_zoom = pipe->dev->roi.scaling * pipe->dev->roi.natural_scale;
  piece->enabled = (darkroom_zoom > 1.f && pipe->type != DT_DEV_PIXELPIPE_THUMBNAIL) 
    || (pipe->type == DT_DEV_PIXELPIPE_EXPORT)
    || (dt_conf_get_int("darkroom/render_size") != 1 && darkroom_zoom != 1.f && pipe->type != DT_DEV_PIXELPIPE_THUMBNAIL);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_finalscale_data_t));
  piece->data_size = sizeof(dt_iop_finalscale_data_t);
  piece->enabled = (pipe->type == DT_DEV_PIXELPIPE_EXPORT);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_enabled = 1;
  self->hide_enable_button = 1;
  self->params_size = sizeof(dt_iop_finalscale_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  dt_free(self->params);
  dt_free(self->default_params);
}

typedef struct dt_iop_finalscale_gui_data_t
{ } dt_iop_finalscale_gui_data_t;

dt_iop_finalscale_gui_data_t dummy;

void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(finalscale);
  self->widget = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(self->widget),_("This module is used to downscale images at export time. "
                                                 "Moving it along the pipeline will have diffent effects on exported images. "
                                                 "<a href='https://ansel.photos/en/doc/modules/processing-modules/finalscale/'>Learn more</a>"));
  gtk_widget_set_halign(self->widget, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(self->widget), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(self->widget), TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
