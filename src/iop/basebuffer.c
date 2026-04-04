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
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_TAKE_NO_INPUT;
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

static int _fetch_base_buffer(const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out, 
                              const dt_mipmap_buffer_t buf, const void *const ivoid, void *const ovoid)
{
  if(!buf.buf || buf.width != pipe->iwidth || buf.height != pipe->iheight || !ovoid) return 1;
  if(roi_out->width <= 0 || roi_out->height <= 0) return 1;
  if(roi_out->x + roi_out->width > pipe->iwidth || roi_out->y + roi_out->height > pipe->iheight) return 1;

  const size_t height = roi_out->height;
  const size_t stride = roi_out->width * piece->dsc_out.bpp;
  const void *const restrict input = buf.buf;

  /* This stage copies the immutable mipmap-cache payload into the pixelpipe cacheline that will
   * be consumed by the first real processing stage. Keeping the source copy local here makes the
   * cache ownership and lifetime visible in the recursion instead of in a hidden bootstrap path. */
  __OMP_PARALLEL_FOR__()
  for(size_t j = 0; j < height; j++)
    memcpy(ovoid + j * stride, input + j * stride, stride);

  return 0;
}

int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  int err = 1;
  dt_mipmap_buffer_t buf;

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, pipe->imgid, pipe->size, DT_MIPMAP_BLOCKING, 'r');
  err = _fetch_base_buffer(pipe, piece, roi_out, buf, ivoid, ovoid);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

#ifdef HAVE_OPENCL
  if(!err && pipe->opencl_enabled)
  {
    /**
     * Put the RAM output on the GPU memory right now. This is optional, as it would be done by the next
     * OpenCL-able module anyway, but on some shitty GPU (AMD), that copy is expensive and it would look
     * as if the next module was slow. So we pay the memory expense now for the sake of accurate benchmarking
     * in later modules.
     */
    dt_pixel_cache_entry_t *cache_entry = dt_dev_pixelpipe_cache_get_entry_by_data(darktable.pixelpipe_cache, ovoid);
    void *cl_mem_base = dt_dev_pixelpipe_cache_get_pinned_image(darktable.pixelpipe_cache, ovoid,
                                                                cache_entry, pipe->devid, roi_out->width,
                                                                roi_out->height, piece->dsc_out.bpp,
                                                                CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                                                NULL);
    if(cl_mem_base != NULL)
    {
      if(dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, ovoid, cl_mem_base, roi_out, CL_MAP_WRITE,
                                               piece->dsc_out.bpp, NULL, "base buffer preload to device") == 0)
        dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, ovoid, cache_entry, &cl_mem_base);
      else
        dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_base, cache_entry, NULL, FALSE);
    }
  }
#endif

  return err;
}

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
