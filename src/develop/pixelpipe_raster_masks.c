/**
 * @file pixelpipe_raster_masks.c
 * @brief Raster-mask retrieval and transport through already-processed pipeline nodes.
 *
 * @details
 * These routines are private to the pixelpipe implementation. They operate on pipeline node state that is already
 * planned and partially processed, so they stay in the develop subsystem and are included from `pixelpipe_hb.c`.
 *
 * The goal is to keep raster-mask specific lifecycle code out of the main pixel-processing recursion file:
 * raster masks are not part of normal pixel transport. Providers publish them in the global pixelpipe cache
 * under dedicated keys; consumers copy those side-band buffers and optionally distort them through downstream
 * modules until they reach the requested stage.
 */

/**
 * @brief Check that the raster-mask provider/consumer relation is still valid in the current pipe.
 *
 * @details
 * We loop over the already-synchronized pipeline nodes looking for 2 things:
 * - the source module providing the raster mask,
 * - the current target module consuming it.
 *
 * If either end of the relation cannot be found, or if the source exists but is disabled, the mask cannot be
 * trusted and the caller needs to stop the blending path with an explanatory error.
 */
static gboolean _dt_dev_raster_mask_check(dt_dev_pixelpipe_iop_t *source_piece,
                                          dt_dev_pixelpipe_iop_t *current_piece,
                                          const dt_iop_module_t *target_module)
{
  gboolean success = TRUE;
  gchar *clean_target_name = !IS_NULL_PTR(target_module)
      ? delete_underscore(target_module->name())
      : g_strdup(_("export"));
  gchar *target_name = !IS_NULL_PTR(target_module)
      ? g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name)
      : g_strdup(clean_target_name);

  if(IS_NULL_PTR(source_piece)
     || (!IS_NULL_PTR(target_module) && IS_NULL_PTR(current_piece)))
  {
    dt_print(DT_DEBUG_MASKS,"[raster masks] ERROR: source: %s, current: %s\n",
            (!IS_NULL_PTR(source_piece)) ? "is defined" : "is undefined",
            (!IS_NULL_PTR(current_piece)) ? "is defined" : "is undefined");

    gchar *hint = NULL;
    if(IS_NULL_PTR(source_piece))
    {
      hint = g_strdup_printf(
            _("- Check if the module providing the masks for the module %s has not been deleted.\n"),
            target_name);
    }
    else if(IS_NULL_PTR(current_piece))
    {
      gchar *clean_source_name = delete_underscore(source_piece->module->name());
      hint = g_strdup_printf(_("- Check if the module %s (%s) providing the masks has not been moved above %s.\n"),
                             clean_source_name,
                             source_piece->module->multi_name, clean_target_name);
      dt_free(clean_source_name);
    }

    dt_control_log(_("The %s module is trying to reuse a mask from a module but it can't be found.\n"
                     "\n%s"),
                   target_name, hint ? hint : "");
    dt_free(hint);

    dt_print(DT_DEBUG_MASKS, "[raster masks] no source module for module %s could be found\n", target_name);
    success = FALSE;
  }

  if(success && !source_piece->enabled)
  {
    gchar *clean_source_name = delete_underscore(source_piece->module->name());
    gchar *source_name = g_strdup_printf("%s (%s)", clean_source_name, source_piece->module->multi_name);
    dt_control_log(_("The `%s` module is trying to reuse a mask from disabled module `%s`.\n"
                     "Disabled modules cannot provide their masks to other modules.\n"
                     "\n- Please enable `%s` or change the raster mask in `%s`."),
                   target_name, source_name, source_name, target_name);

    dt_print(DT_DEBUG_MASKS, "[raster masks] module %s trying to reuse a mask from disabled instance of %s\n",
            target_name, source_name);

    dt_free(clean_source_name);
    dt_free(source_name);
    success = FALSE;
  }

  dt_free(clean_target_name);
  dt_free(target_name);
  return success;
}

float *dt_dev_get_raster_mask(dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *raster_mask_source,
                              const int raster_mask_id, const dt_iop_module_t *target_module,
                              int *error)
{
  if(!IS_NULL_PTR(error)) *error = 0;

  gchar *clean_target_name = !IS_NULL_PTR(target_module)
      ? delete_underscore(target_module->name())
      : g_strdup(_("export"));
  gchar *target_name = !IS_NULL_PTR(target_module)
      ? g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name)
      : g_strdup(clean_target_name);

  if(IS_NULL_PTR(raster_mask_source))
  {
    dt_print(DT_DEBUG_MASKS, "[raster masks] The source module of the mask for %s was not found\n", target_name);
    dt_free(clean_target_name);
    dt_free(target_name);
    return NULL;
  }

  float *raster_mask = NULL;

  dt_dev_pixelpipe_iop_t *source_piece = NULL;
  dt_dev_pixelpipe_iop_t *current_piece = NULL;
  GList *source_iter = NULL;
  for(source_iter = g_list_last(pipe->nodes); source_iter; source_iter = g_list_previous(source_iter))
  {
    dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(candidate->module == target_module)
      current_piece = candidate;
    else if(candidate->module == raster_mask_source)
    {
      source_piece = candidate;
      break;
    }
  }

  const int err_ret = !_dt_dev_raster_mask_check(source_piece, current_piece, target_module);
  if(!IS_NULL_PTR(error)) *error = err_ret;

  if(!err_ret)
  {
    const uint64_t provider_hash = source_piece->global_hash;
    const uint64_t raster_hash
        = dt_dev_pixelpipe_raster_mask_hash(source_piece, raster_mask_id);

    gchar *clean_source_name = delete_underscore(source_piece->module->name());
    gchar *source_name = g_strdup_printf("%s (%s)", clean_source_name, source_piece->module->multi_name);
    dt_pixel_cache_entry_t *raster_entry = NULL;
    void *cache_data = NULL;
    const gboolean found = dt_dev_pixelpipe_cache_ref_entry_by_hash(
        darktable.pixelpipe_cache, raster_hash, &cache_data, &raster_entry);

    gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
    if(found && !IS_NULL_PTR(cache_data) && !IS_NULL_PTR(raster_entry))
    {
      const size_t mask_size
          = sizeof(float) * (size_t)source_piece->roi_out.width * source_piece->roi_out.height;
      raster_mask = dt_pixelpipe_cache_alloc_align_float_cache(
          (size_t)source_piece->roi_out.width * source_piece->roi_out.height, pipe->type);
      if(IS_NULL_PTR(raster_mask))
      {
        dt_dev_pixelpipe_cache_ref_count_entry(
            darktable.pixelpipe_cache, FALSE, raster_entry);
        if(!IS_NULL_PTR(error)) *error = 1;
        dt_free(clean_source_name);
        dt_free(source_name);
        dt_free(clean_target_name);
        dt_free(target_name);
        return NULL;
      }

      // The cache owns the canonical mask. Copy it under a read lock so the
      // caller can freely distort and release its private working buffer.
      dt_dev_pixelpipe_cache_rdlock_entry(
          darktable.pixelpipe_cache, TRUE, raster_entry);
      memcpy(raster_mask, cache_data, mask_size);
      dt_dev_pixelpipe_cache_rdlock_entry(
          darktable.pixelpipe_cache, FALSE, raster_entry);
      dt_dev_pixelpipe_cache_ref_count_entry(
          darktable.pixelpipe_cache, FALSE, raster_entry);

      dt_print(DT_DEBUG_MASKS,
               "[raster masks] found cached mask id %i from %s for module %s"
               " in pipe %s with hash %" PRIu64 "\n",
               raster_mask_id, source_name, target_name, type, raster_hash);
      dt_dev_pixelpipe_unset_reentry(pipe, provider_hash);
    }
    else
    {
      if(!IS_NULL_PTR(raster_entry))
        dt_dev_pixelpipe_cache_ref_count_entry(
            darktable.pixelpipe_cache, FALSE, raster_entry);

      dt_print(DT_DEBUG_DEV,
               "[raster masks] missing source=%s instance=%d target=%s instance=%d"
               " requested_id=%d cache_hash=%" PRIu64 " source_enabled=%d pipe=%s\n",
               source_piece->module->op, source_piece->module->multi_priority,
               !IS_NULL_PTR(target_module) ? target_module->op : "export",
               !IS_NULL_PTR(target_module) ? target_module->multi_priority : -1,
               raster_mask_id, raster_hash, source_piece->enabled,
               type);

      dt_print(DT_DEBUG_MASKS,
              "[raster masks] mask id %i from %s for module %s could not be found"
              " in the global cache for pipe %s.\n",
              raster_mask_id, source_name, target_name, type);

      // A cache miss can still happen after LRU eviction. Cached provider
      // pixels cannot reconstruct the side-band mask, so interactive pipes get
      // one targeted provider retry; export callers pass no error pointer and
      // simply report the missing mask.
      if(!IS_NULL_PTR(error))
      {
        if(dt_dev_pixelpipe_set_reentry(pipe, provider_hash))
          pipe->flush_cache = TRUE;
        *error = 1;
      }

      dt_free(clean_source_name);
      dt_free(source_name);
      dt_free(clean_target_name);
      dt_free(target_name);
      return NULL;
    }

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
          float *transformed_mask = dt_pixelpipe_cache_alloc_align_float_cache(
              (size_t)module->roi_out.width * module->roi_out.height, 0);
          if(IS_NULL_PTR(transformed_mask))
          {
            dt_print(DT_DEBUG_MASKS, "[raster masks] could not allocate memory for transformed mask\n");
            if(!IS_NULL_PTR(error)) *error = 1;
            dt_pixelpipe_cache_free_align(raster_mask);
            dt_free(clean_source_name);
            dt_free(source_name);
            dt_free(clean_target_name);
            dt_free(target_name);
            return NULL;
          }

          module->module->distort_mask(module->module, pipe, module, raster_mask, transformed_mask,
                                       &module->roi_in, &module->roi_out);
          dt_pixelpipe_cache_free_align(raster_mask);
          raster_mask = transformed_mask;
          dt_print(DT_DEBUG_MASKS, "[raster masks] doing transform\n");
        }
        else if(!module->module->distort_mask
                && (module->roi_in.width != module->roi_out.width
                    || module->roi_in.height != module->roi_out.height
                    || module->roi_in.x != module->roi_out.x
                    || module->roi_in.y != module->roi_out.y))
          dt_print(DT_DEBUG_MASKS, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                          "distort_mask() implemented!\n", module->module->op, module->roi_in.width,
                          module->roi_in.height, module->roi_in.x, module->roi_in.y,
                          module->roi_out.width, module->roi_out.height, module->roi_out.x,
                          module->roi_out.y);
      }

      if(module->module == target_module)
      {
        gchar *clean_module_name = delete_underscore(module->module->name());
        dt_print(DT_DEBUG_MASKS, "[raster masks] found mask id %i from %s for module %s (%s) in pipe %s\n",
                 raster_mask_id, source_name, clean_module_name,
                 module->module->multi_name, dt_pixelpipe_get_pipe_name(pipe->type));
        dt_free(clean_module_name);
        break;
      }
    }

    dt_free(clean_source_name);
    dt_free(source_name);
  }

  dt_free(clean_target_name);
  dt_free(target_name);
  return raster_mask;
}
