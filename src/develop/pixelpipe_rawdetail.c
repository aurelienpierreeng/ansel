/**
 * @file pixelpipe_rawdetail.c
 * @brief Raw-detail mask production and transport helpers.
 *
 * @details
 * These helpers manage the side-band detail mask written by `rawprepare` or `demosaic` and later consumed by
 * blend operators. They are private implementation details of the pixelpipe and are included from
 * `pixelpipe_hb.c` to keep raw-detail specific logic out of the main pipeline recursion code.
 */

void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe)
{
  dt_pixelpipe_cache_free_align(pipe->rawdetail_mask_data);
  pipe->rawdetail_mask_data = NULL;
}

gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece, float *const rgb,
                                     const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;
  float *mask = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
  float *tmp = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
  if((mask == NULL) || (tmp == NULL)) goto error;

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                            piece->pipe->dsc.temperature.coeffs[1],
                            piece->pipe->dsc.temperature.coeffs[2] };
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
    wb[0] = wb[1] = wb[2] = 1.0f;

  dt_masks_calc_rawdetail_mask(rgb, mask, tmp, width, height, wb);
  dt_pixelpipe_cache_free_align(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask] %i (%ix%i)\n", mode, roi_in->width, roi_in->height);
  return FALSE;

error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask] couldn't write detail mask\n");
  dt_pixelpipe_cache_free_align(mask);
  dt_pixelpipe_cache_free_align(tmp);
  return TRUE;
}

#ifdef HAVE_OPENCL
gboolean dt_dev_write_rawdetail_mask_cl(dt_dev_pixelpipe_iop_t *piece, cl_mem in,
                                        const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }

  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem out = NULL;
  cl_mem tmp = NULL;
  float *mask = NULL;
  const int devid = p->devid;

  cl_int err = CL_SUCCESS;
  mask = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
  if(mask == NULL) goto error;
  out = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  if(out == NULL) goto error;
  tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(tmp == NULL) goto error;

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_Y0_mask;
    dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                              piece->pipe->dsc.temperature.coeffs[1],
                              piece->pipe->dsc.temperature.coeffs[2] };
    if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
      wb[0] = wb[1] = wb[2] = 1.0f;

    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &in);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &wb[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &wb[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &wb[2]);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  {
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_write_scharr_mask;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  {
    err = dt_opencl_read_host_from_device(devid, mask, out, width, height, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask_cl] mode %i (%ix%i)", mode, roi_in->width, roi_in->height);
  return FALSE;

error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask_cl] couldn't write detail mask: %i\n", err);
  dt_dev_clear_rawdetail_mask(p);
  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_pixelpipe_cache_free_align(mask);
  return TRUE;
}
#endif

float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src,
                                  const dt_iop_module_t *target_module)
{
  if(!pipe->rawdetail_mask_data) return NULL;
  gboolean valid = FALSE;
  const int check = pipe->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if((!strcmp(candidate->module->op, "demosaic")) && candidate->enabled && (check == DT_DEV_DETAIL_MASK_DEMOSAIC))
    {
      valid = TRUE;
      break;
    }
    if((!strcmp(candidate->module->op, "rawprepare")) && candidate->enabled && (check == DT_DEV_DETAIL_MASK_RAWPREPARE))
    {
      valid = TRUE;
      break;
    }
  }

  if(!valid) return NULL;
  dt_vprint(DT_DEBUG_MASKS, "[dt_dev_distort_detail_mask] (%ix%i) for module %s\n",
            pipe->rawdetail_mask_roi.width, pipe->rawdetail_mask_roi.height, target_module->op);

  float *resmask = src;
  float *inmask = src;
  if(source_iter)
  {
    for(GList *iter = source_iter; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;
      if(module->enabled
         && !dt_dev_pixelpipe_activemodule_disables_currentmodule(module->module->dev, module->module))
      {
        if(module->module->distort_mask
           && !(!strcmp(module->module->op, "finalscale")
                && module->planned_roi_in.width == 0
                && module->planned_roi_in.height == 0))
        {
          float *tmp = dt_pixelpipe_cache_alloc_align_float_cache(
              (size_t)module->planned_roi_out.width * module->planned_roi_out.height, 0);
          dt_vprint(DT_DEBUG_MASKS, "   %s %ix%i -> %ix%i\n", module->module->op,
                    module->planned_roi_in.width, module->planned_roi_in.height,
                    module->planned_roi_out.width, module->planned_roi_out.height);
          module->module->distort_mask(module->module, module, inmask, tmp,
                                       &module->planned_roi_in, &module->planned_roi_out);
          resmask = tmp;
          if(inmask != src) dt_pixelpipe_cache_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask
                && (module->planned_roi_in.width != module->planned_roi_out.width
                    || module->planned_roi_in.height != module->planned_roi_out.height
                    || module->planned_roi_in.x != module->planned_roi_out.x
                    || module->planned_roi_in.y != module->planned_roi_out.y))
          fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                          "distort_mask() implemented!\n", module->module->op, module->planned_roi_in.width,
                          module->planned_roi_in.height, module->planned_roi_in.x, module->planned_roi_in.y,
                          module->planned_roi_out.width, module->planned_roi_out.height, module->planned_roi_out.x,
                          module->planned_roi_out.y);

        if(module->module == target_module) break;
      }
    }
  }
  return resmask;
}
