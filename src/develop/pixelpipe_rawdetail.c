/**
 * @file pixelpipe_rawdetail.c
 * @brief Raw-detail mask transport helpers.
 *
 * @details
 * These helpers transport the side-band detail mask written by the hidden `detailmask` module and later
 * consumed by blend operators. They are private implementation details of the pixelpipe and are included from
 * `pixelpipe_hb.c` to keep raw-detail specific geometry logic out of the main pipeline recursion code.
 */

#include "common/interpolation.h"

float *dt_dev_retrieve_rawdetail_mask(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *target_module)
{
  const dt_dev_pixelpipe_iop_t *detailmask_piece = NULL;
  for(GList *iter = g_list_first(pipe->nodes); iter; iter = g_list_next(iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (const dt_dev_pixelpipe_iop_t *)iter->data;
    if(!candidate) continue;
    if(candidate->module == target_module) break;
    if(candidate->enabled && !strcmp(candidate->module->op, "detailmask"))
      detailmask_piece = candidate;
  }

  dt_dev_pixelpipe_t *mutable_pipe = (dt_dev_pixelpipe_t *)pipe;
  const uint64_t mask_hash = detailmask_piece ? dt_dev_pixelpipe_rawdetail_mask_hash(detailmask_piece)
                                              : DT_PIXELPIPE_CACHE_HASH_INVALID;

  if(mask_hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return NULL;

  if(pipe->rawdetail_mask_hash != mask_hash)
  {
    dt_pixel_cache_entry_t *entry = dt_dev_pixelpipe_cache_get_entry(darktable.pixelpipe_cache, mask_hash);
    if(IS_NULL_PTR(entry)) return NULL;

    dt_dev_clear_rawdetail_mask(mutable_pipe);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
    mutable_pipe->rawdetail_mask_hash = mask_hash;
    memcpy(&mutable_pipe->rawdetail_mask_roi, &detailmask_piece->roi_out, sizeof(dt_iop_roi_t));
  }

  void *mask = NULL;
  if(!dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, pipe->rawdetail_mask_hash, &mask, NULL, pipe->devid, NULL))
    return NULL;

  return (float *)mask;
}

float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src,
                                  const dt_iop_module_t *target_module)
{
  if(dt_dev_retrieve_rawdetail_mask(pipe, target_module) == NULL) return NULL;
  gboolean valid = FALSE;
  const dt_dev_pixelpipe_iop_t *target_piece = NULL;
  GList *source_iter = NULL;
  for(GList *iter = g_list_first(pipe->nodes); iter; iter = g_list_next(iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)iter->data;
    if(candidate->module == target_module) target_piece = candidate;
    if((IS_NULL_PTR(source_iter)) && (!strcmp(candidate->module->op, "detailmask")) && candidate->enabled)
    {
      valid = TRUE;
      source_iter = iter;
    }
  }

  if(!valid) return NULL;
  dt_vprint(DT_DEBUG_MASKS, "[dt_dev_distort_detail_mask] (%ix%i) for module %s\n",
            pipe->rawdetail_mask_roi.width, pipe->rawdetail_mask_roi.height, target_module->op);

  float *resmask = src;
  float *inmask = src;
  dt_iop_roi_t current_roi = pipe->rawdetail_mask_roi;
  if(source_iter)
  {
    /* The side-band detail mask is stored in the output geometry of the
     * hidden `detailmask` stage. Start warping at the next node exactly like
     * raster masks do, otherwise we would re-apply the source transform to a
     * buffer that is already expressed in source output coordinates. */
    for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;
      if(module->enabled
         && !dt_dev_pixelpipe_activemodule_disables_currentmodule(module->module->dev, module->module))
      {
        if(module->module->distort_mask
           && !(!strcmp(module->module->op, "finalscale")
                && module->roi_in.width == 0
                && module->roi_in.height == 0))
        {
          float *tmp = dt_pixelpipe_cache_alloc_align_float_cache(
              (size_t)module->roi_out.width * module->roi_out.height, 0);
          dt_vprint(DT_DEBUG_MASKS, "   %s %ix%i -> %ix%i\n", module->module->op,
                    module->roi_in.width, module->roi_in.height,
                    module->roi_out.width, module->roi_out.height);
          module->module->distort_mask(module->module, (dt_dev_pixelpipe_t *)pipe, module, inmask, tmp,
                                       &module->roi_in, &module->roi_out);
          resmask = tmp;
          if(inmask != src) dt_pixelpipe_cache_free_align(inmask);
          inmask = tmp;
          current_roi = module->roi_out;
        }
        else if(!module->module->distort_mask
                && (module->roi_in.width != module->roi_out.width
                    || module->roi_in.height != module->roi_out.height
                    || module->roi_in.x != module->roi_out.x
                    || module->roi_in.y != module->roi_out.y))
          fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                          "distort_mask() implemented!\n", module->module->op, module->roi_in.width,
                          module->roi_in.height, module->roi_in.x, module->roi_in.y,
                          module->roi_out.width, module->roi_out.height, module->roi_out.x,
                          module->roi_out.y);

        if(module->module == target_module) break;
      }
    }
  }

  if(target_piece
     && (current_roi.scale != target_piece->roi_out.scale
         || current_roi.width != target_piece->roi_out.width
         || current_roi.height != target_piece->roi_out.height
         || current_roi.x != target_piece->roi_out.x
         || current_roi.y != target_piece->roi_out.y))
  {
    /* The cached raw-detail mask is kept at the full-resolution source ROI.
     * When the downstream walk stops before matching the consumer output ROI,
     * finish by resampling/cropping once into the consumer geometry, just like
     * initialscale would do for the pixel buffer. */
    float *tmp = dt_pixelpipe_cache_alloc_align_float_cache(
        (size_t)target_piece->roi_out.width * target_piece->roi_out.height, 0);
    const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample_roi_1c(itor, tmp, &target_piece->roi_out, inmask, &current_roi);
    if(inmask != src) dt_pixelpipe_cache_free_align(inmask);
    inmask = tmp;
    resmask = tmp;
  }

  return resmask;
}
