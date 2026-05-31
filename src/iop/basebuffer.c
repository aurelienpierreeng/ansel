/*
    This file is part of Ansel.
    Copyright (C) 2026 Guillaume Stutin.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/imagebuf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/dev_pixelpipe.h"
#include "iop/iop_api.h"

#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_basebuffer_params_t)

typedef struct dt_iop_basebuffer_params_t
{
  int dummy;
} dt_iop_basebuffer_params_t;

typedef dt_iop_basebuffer_params_t dt_iop_basebuffer_data_t;

const char *name()
{
  return _("base buffer");
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_TAKE_NO_INPUT | IOP_FLAGS_CPU_WRITES_OPENCL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return pipe->dev->image_storage.dsc.cst;
}

// Full-size RAW in and out
void modify_roi_in(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = (dt_iop_roi_t){ 0, 0, pipe->iwidth, pipe->iheight, 1.f };
}

void modify_roi_out(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out, const dt_iop_roi_t *const roi_in)
{
  *roi_out = (dt_iop_roi_t){ 0, 0, pipe->iwidth, pipe->iheight, 1.f };
}


__DT_CLONE_TARGETS__
int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_mipmap_buffer_t buf;

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, pipe->imgid, pipe->size, DT_MIPMAP_BLOCKING, 'r');

  // Catch out-of-bounds here because roi_in -> roi_out conversions
  // use float scaling that may not always respect initial size.

  // Crop rectangle offset
  const size_t x = MAX(roi_in->x, 0);
  const size_t y = MAX(roi_in->y, 0);

  // Crop rectangle size
  const size_t in_width = MIN(roi_in->width, pipe->iwidth - x);
  const size_t in_height = MIN(roi_in->height, pipe->iheight - y);
  const size_t in_stride = in_width * piece->dsc_in.bpp;
  const size_t out_stride = roi_out->width * piece->dsc_out.bpp;

  // Crop offset translated in memory sizes
  const size_t y_offset = y * in_stride;
  const size_t x_offset = x * piece->dsc_in.bpp;

  /* This stage copies the immutable mipmap-cache payload into the pixelpipe cacheline that will
   * be consumed by the first real processing stage. Keeping the source copy local here makes the
   * cache ownership and lifetime visible in the recursion instead of in a hidden bootstrap path. */
  const void *const restrict input = buf.buf;

  __OMP_PARALLEL_FOR__()
  for(size_t j = 0; j < MIN(roi_out->height, in_height); j++)
    memcpy(ovoid + j * out_stride, input + x_offset + y_offset + j * in_stride, MIN(in_stride, out_stride));

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_mipmap_buffer_t buf;

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, pipe->imgid, pipe->size, DT_MIPMAP_BLOCKING, 'r');

  // Catch out-of-bounds here because roi_in -> roi_out conversions
  // use float scaling that may not always respect initial size.

  // Crop rectangle offset
  const size_t x = MAX(roi_in->x, 0);
  const size_t y = MAX(roi_in->y, 0);

  // Crop rectangle size
  const size_t in_width = MIN(roi_in->width, pipe->iwidth - x);
  const size_t in_stride = in_width * piece->dsc_in.bpp;

  // Crop offset translated in memory sizes
  const size_t y_offset = y * in_stride;
  const size_t x_offset = x * piece->dsc_in.bpp;

  /* This stage copies the immutable mipmap-cache payload into the pixelpipe cacheline that will
   * be consumed by the first real processing stage. Keeping the source copy local here makes the
   * cache ownership and lifetime visible in the recursion instead of in a hidden bootstrap path. */
  const void *const restrict input = buf.buf;

  size_t origin[] = { x, y, 0 };
  size_t region[] = { MIN(roi_out->width, roi_in->width), MIN(roi_out->height, roi_in->height), 1 };

  int err = dt_opencl_write_host_to_device_raw(pipe->devid, input + x_offset + y_offset, dev_out, origin,
                                               region, in_stride, CL_TRUE);


  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  return err == CL_SUCCESS;
}
#endif


void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_basebuffer_data_t));
  piece->data_size = sizeof(dt_iop_basebuffer_data_t);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_basebuffer_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_basebuffer_params_t));
  self->default_enabled = 1;
  self->hide_enable_button = 1;
  self->params_size = sizeof(dt_iop_basebuffer_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  dt_free(self->params);
  dt_free(self->default_params);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
