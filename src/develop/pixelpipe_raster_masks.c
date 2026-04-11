/**
 * @file pixelpipe_raster_masks.c
 * @brief Raster-mask retrieval and transport through already-processed pipeline nodes.
 *
 * @details
 * These routines are private to the pixelpipe implementation. They operate on pipeline node state that is already
 * planned and partially processed, so they stay in the develop subsystem and are included from `pixelpipe_hb.c`.
 *
 * The goal is to keep raster-mask specific lifecycle code out of the main pixel-processing recursion file:
 * raster masks are not part of normal pixel transport, they are side-band buffers fetched from providers and
 * optionally distorted through downstream modules until they reach the consumer.
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
  gchar *clean_target_name = delete_underscore(target_module->name());
  gchar *target_name = g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name);

  if(IS_NULL_PTR(source_piece) || IS_NULL_PTR(current_piece))
  {
    fprintf(stderr,"[raster masks] ERROR: source: %s, current: %s\n",
            (source_piece != NULL) ? "is defined" : "is undefined",
            (current_piece != NULL) ? "is definded" : "is undefined");

    gchar *hint = NULL;
    if(IS_NULL_PTR(source_piece))
    {
      hint = g_strdup_printf(
            _("- Check if the module providing the masks for the module %s has not been deleted.\n"),
            target_name);
    }
    else if(IS_NULL_PTR(current_piece))
    {
      hint = g_strdup_printf(_("- Check if the module %s (%s) providing the masks has not been moved above %s.\n"),
                             delete_underscore(source_piece->module->name()),
                             source_piece->module->multi_name, clean_target_name);
    }

    dt_control_log(_("The %s module is trying to reuse a mask from a module but it can't be found.\n"
                     "\n%s"),
                   target_name, hint ? hint : "");
    dt_free(hint);

    fprintf(stderr, "[raster masks] no source module for module %s could be found\n", target_name);
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

    fprintf(stderr, "[raster masks] module %s trying to reuse a mask from disabled instance of %s\n",
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
                              gboolean *free_mask, int *error)
{
  if(!error) return NULL;
  *error = 0;

  gchar *clean_target_name = delete_underscore(target_module->name());
  gchar *target_name = g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name);

  if(!raster_mask_source)
  {
    fprintf(stderr, "[raster masks] The source module of the mask for %s was not found\n", target_name);
    dt_free(clean_target_name);
    dt_free(target_name);
    return NULL;
  }

  *free_mask = FALSE;
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
  *error = err_ret;

  if(!err_ret)
  {
    const uint64_t raster_hash = current_piece->global_mask_hash;

    gchar *clean_source_name = delete_underscore(source_piece->module->name());
    gchar *source_name = g_strdup_printf("%s (%s)", clean_source_name, source_piece->module->multi_name);
    raster_mask = dt_pixelpipe_raster_get(source_piece->raster_masks, raster_mask_id);

    gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
    if(raster_mask)
    {
      dt_print(DT_DEBUG_MASKS,
               "[raster masks] found in %s mask id %i from %s for module %s in pipe %s with hash %" PRIu64 "\n",
               "internal", raster_mask_id, source_name, target_name, type, raster_hash);
      dt_dev_pixelpipe_unset_reentry(pipe, raster_hash);
    }
    else
    {
      fprintf(stderr,
              "[raster masks] mask id %i from %s for module %s could not be found in pipe %s. Pipe re-entry will be attempted.\n",
              raster_mask_id, source_name, target_name, type);

      if(dt_dev_pixelpipe_set_reentry(pipe, raster_hash))
        pipe->flush_cache = TRUE;

      if(error) *error = 1;

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
          if(!transformed_mask)
          {
            fprintf(stderr, "[raster masks] could not allocate memory for transformed mask\n");
            if(error) *error = 1;
            dt_free(clean_target_name);
            dt_free(target_name);
            return NULL;
          }

          module->module->distort_mask(module->module, pipe, module, raster_mask, transformed_mask,
                                       &module->roi_in, &module->roi_out);
          if(*free_mask) dt_pixelpipe_cache_free_align(raster_mask);
          *free_mask = TRUE;
          raster_mask = transformed_mask;
          dt_print(DT_DEBUG_MASKS, "[raster masks] doing transform\n");
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
      }

      if(module->module == target_module)
      {
        dt_print(DT_DEBUG_MASKS, "[raster masks] found mask id %i from %s for module %s (%s) in pipe %s\n",
                 raster_mask_id, source_name, delete_underscore(module->module->name()),
                 module->module->multi_name, dt_pixelpipe_get_pipe_name(pipe->type));
        break;
      }
    }
  }

  dt_free(clean_target_name);
  dt_free(target_name);
  return raster_mask;
}
