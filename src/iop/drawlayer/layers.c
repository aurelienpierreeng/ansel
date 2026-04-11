/** @file
 *  @brief Private drawlayer layer cache, sidecar sync and widget cache state.
 *
 *  This file is text-included from drawlayer.c on purpose. The goal is to keep
 *  the main module file readable while preserving one translation unit, so
 *  cache ownership, worker synchronization and GUI side effects stay visible to
 *  the caller instead of being hidden behind a separate API boundary.
 */

typedef struct drawlayer_dir_info_t
{
  gboolean found;
  int index;
  int count;
  uint32_t width;
  uint32_t height;
  char name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
} drawlayer_dir_info_t;

typedef struct drawlayer_layer_cache_key_t
{
  int32_t imgid;
  int layer_width;
  int layer_height;
  const char *layer_name;
  int layer_order;
} drawlayer_layer_cache_key_t;

static void _layerio_append_error(GString *errors, const char *message)
{
  if(!errors || !message || message[0] == '\0') return;
  if(errors->len > 0) g_string_append(errors, "; ");
  g_string_append(errors, message);
}

static void _layerio_log_errors(GString *errors)
{
  if(IS_NULL_PTR(errors)) return;
  if(errors->len > 0) dt_control_log("%s", errors->str);
}

static void _populate_layer_list(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(IS_NULL_PTR(g)) return;
  if(darktable.gui) ++darktable.gui->reset;

  while(dt_bauhaus_combobox_length(g->controls.layer_select) > 0)
    dt_bauhaus_combobox_remove_at(g->controls.layer_select, dt_bauhaus_combobox_length(g->controls.layer_select) - 1);

  if(!self->dev)
  {
    dt_bauhaus_combobox_set(g->controls.layer_select, -1);
    if(darktable.gui) --darktable.gui->reset;
    return;
  }

  char path[PATH_MAX] = { 0 };
  char **names = NULL;
  int count = 0;
  if(dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path)))
    dt_drawlayer_io_list_layer_names(path, &names, &count);

  int active = -1;
  const int listed_count = count;
  for(int i = 0; i < count; i++)
  {
    const char *page_name = names[i] ? names[i] : "";
    dt_bauhaus_combobox_add(g->controls.layer_select, page_name);
    if(params->layer_order == i || (params->layer_name[0] && !g_strcmp0(page_name, params->layer_name)))
      active = i;
  }

  dt_drawlayer_io_free_layer_names(&names, &count);
  if(active >= 0)
    dt_bauhaus_combobox_set(g->controls.layer_select, active);
  else if(listed_count > 0 && !_layer_name_non_empty(params->layer_name))
    dt_bauhaus_combobox_set(g->controls.layer_select, 0);
  else
    /* The combobox is reserved for layers that already exist in the TIFF
     * sidecar. Keep unsaved or missing module attachments out of the shared
     * list so switching layers never invents phantom entries. */
    dt_bauhaus_combobox_set(g->controls.layer_select, -1);
  if(darktable.gui) --darktable.gui->reset;
}

static void _reset_stroke_session(dt_iop_drawlayer_gui_data_t *g)
{
  if(IS_NULL_PTR(g)) return;
  g->stroke.stroke_sample_count = 0;
  g->stroke.stroke_event_index = 0;
  g->stroke.last_dab_valid = FALSE;
  dt_drawlayer_process_state_reset_stroke(&g->process);
  dt_drawlayer_worker_reset_backend_path(g->stroke.worker);
  dt_drawlayer_worker_reset_live_publish(g->stroke.worker);
}

static gboolean _layer_cache_matches(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_layer_cache_key_t *key)
{
  /* Cache identity belongs to the module orchestration layer because it
   * combines sidecar identity with GUI/module state (active image, selected
   * layer, in-memory patch ownership). The low-level TIFF primitives stay in
   * drawlayer/io.c. */
  if(!g || !key || !g->process.cache_valid || !g->process.base_patch.pixels) return FALSE;
  if(g->process.cache_imgid != key->imgid || g->process.base_patch.width != key->layer_width
     || g->process.base_patch.height != key->layer_height)
    return FALSE;

  /* Once a layer is cached in memory, `process()` should treat that payload as
   * authoritative until we explicitly reload or flush it. The page index in the
   * params is only a sidecar lookup hint and may legitimately drift after sidecar
   * rewrites or page reordering. Layer names are unique and stable by design, so
   * prefer them for cache identity. Falling back to the page index here causes
   * `process()` to re-open the TIFF and re-scan directories even though the
   * correct pixels are already in memory. */
  const char *target_name = key->layer_name ? key->layer_name : "";
  if(target_name[0] != '\0' || g->process.cache_layer_name[0] != '\0') return !g_strcmp0(g->process.cache_layer_name, target_name);

  if(key->layer_order >= 0 && g->process.cache_layer_order >= 0)
    return g->process.cache_layer_order == key->layer_order;
  return TRUE;
}

gboolean dt_drawlayer_ensure_layer_cache(dt_iop_module_t *self)
{
  /* Make sure `base_patch` mirrors the currently selected sidecar layer.
   * The low-level TIFF read/write primitives live in drawlayer/io.c, while this
   * function stays here because it orchestrates cache lifetime, widget state,
   * prompting, and history/UI side effects around those I/O operations. */
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return FALSE;

  _sanitize_params(self, params);
  int layer_width = 0;
  int layer_height = 0;
  const int32_t imgid = self->dev->image_storage.id;
  GMainContext *const ui_ctx = g_main_context_default();
  const gboolean ui_thread = ui_ctx && g_main_context_is_owner(ui_ctx);
  if(!_resolve_layer_geometry(self, NULL, NULL, &layer_width, &layer_height, NULL, NULL))
  {
    layer_width = self->dev->roi.raw_width;
    layer_height = self->dev->roi.raw_height;
  }
  if(imgid <= 0 || layer_width <= 0 || layer_height <= 0) return FALSE;
  if(!_layer_name_non_empty(params->layer_name))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    g->process.cache_dirty = FALSE;
    dt_drawlayer_paint_runtime_state_reset(&g->process.cache_dirty_rect);
    g->process.cache_imgid = -1;
    g->process.cache_layer_name[0] = '\0';
    g->process.cache_layer_order = -1;
    dt_drawlayer_process_state_invalidate(&g->process);
    if(ui_thread) _refresh_layer_widgets(self);
    return TRUE;
  }
  const drawlayer_layer_cache_key_t cache_key = {
    .imgid = imgid,
    .layer_width = layer_width,
    .layer_height = layer_height,
    .layer_name = params->layer_name,
    .layer_order = params->layer_order,
  };

  GString *errors = g_string_new(NULL);

  char current_profile[DRAWLAYER_PROFILE_SIZE] = { 0 };
  const gboolean have_current_profile = _get_current_work_profile_key(self, self->dev->iop, self->dev->pipe,
                                                                      current_profile, sizeof(current_profile));
  if(!have_current_profile)
    _layerio_append_error(errors, _("failed to resolve drawlayer working profile"));
  else if(params->work_profile[0] == '\0')
    g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  else if(g_strcmp0(params->work_profile, current_profile))
    _layerio_append_error(errors, _("drawlayer working profile mismatch"));

  if(_layer_cache_matches(g, &cache_key))
  {
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(!_flush_layer_cache(self))
  {
    _layerio_append_error(errors, _("failed to write drawing layer sidecar"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  /* We are about to replace/rebind `g->process.base_patch`; drop all explicit extra
   * refs from the previous entry first so counters never leak across entries. */
  _release_all_base_patch_extra_refs(g);

  int created = 0;
  if(!dt_drawlayer_cache_patch_alloc_shared(&g->process.base_patch,
                                            _drawlayer_params_cache_hash(imgid, params, layer_width, layer_height),
                                            (size_t)layer_width * layer_height, layer_width, layer_height,
                                            "drawlayer sidecar cache", &created))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  if(!dt_drawlayer_cache_ensure_mask_buffer(&g->process.stroke_mask, layer_width, layer_height,
                                            "drawlayer stroke mask"))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    _layerio_append_error(errors, _("failed to allocate drawlayer stroke mask"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  if(created)
  {
    dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
    dt_drawlayer_cache_clear_transparent_float(g->process.base_patch.pixels, (size_t)layer_width * layer_height);
    dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
  }

  gboolean ok = TRUE;
  gboolean cache_loaded = FALSE;
  gboolean file_exists = FALSE;
  char path[PATH_MAX] = { 0 };

  if(!created)
  {
    cache_loaded = TRUE;
  }
  else if(!dt_drawlayer_io_sidecar_path(imgid, path, sizeof(path)))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(created) file_exists = g_file_test(path, G_FILE_TEST_EXISTS);

  if(created && !file_exists)
  {
    cache_loaded = TRUE;
    params->layer_order = -1;
    params->sidecar_timestamp = 0;
  }
  else if(created)
  {
    const gboolean pending_new_layer = (params->layer_order < 0 && params->sidecar_timestamp == 0);
    drawlayer_dir_info_t info = { 0 };
    dt_drawlayer_io_layer_info_t io_info = { 0 };
    if(!dt_drawlayer_io_find_layer(path, params->layer_name, -1, &io_info))
    {
      if(pending_new_layer)
      {
        cache_loaded = TRUE;
      }
      else
      {
        g_snprintf(g->session.missing_layer_error, sizeof(g->session.missing_layer_error),
                   _("The drawing layer \"%s\" was not found in the sidecar TIFF."),
                   params->layer_name);
        params->layer_name[0] = '\0';
        params->layer_order = -1;
        params->sidecar_timestamp = 0;
        memset(params->work_profile, 0, sizeof(params->work_profile));
        _release_all_base_patch_extra_refs(g);
        dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
        g->process.cache_valid = FALSE;
        g->process.cache_dirty = FALSE;
        dt_drawlayer_paint_runtime_state_reset(&g->process.cache_dirty_rect);
        g->process.cache_imgid = -1;
        g->process.cache_layer_name[0] = '\0';
        g->process.cache_layer_order = -1;
        _reset_stroke_session(g);
        dt_drawlayer_process_state_invalidate(&g->process);
      }
    }
    else
    {
      info.found = io_info.found;
      info.index = io_info.index;
      info.count = io_info.count;
      info.width = io_info.width;
      info.height = io_info.height;
      g_strlcpy(info.name, io_info.name, sizeof(info.name));
      g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
      if(g) g->session.missing_layer_error[0] = '\0';
      params->layer_order = info.index;
      if(info.work_profile[0] != '\0' && params->work_profile[0] == '\0')
        g_strlcpy(params->work_profile, info.work_profile, sizeof(params->work_profile));

      if(have_current_profile && info.work_profile[0] != '\0' && g_strcmp0(info.work_profile, current_profile))
        _layerio_append_error(errors, _("drawlayer sidecar profile mismatch"));

      dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
      dt_drawlayer_io_patch_t io_patch = {
        .x = g->process.base_patch.x,
        .y = g->process.base_patch.y,
        .width = g->process.base_patch.width,
        .height = g->process.base_patch.height,
        .pixels = g->process.base_patch.pixels,
      };
      const gboolean loaded
          = dt_drawlayer_io_load_layer(path, params->layer_name, info.index, layer_width, layer_height, &io_patch);
      dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
      if(!loaded)
      {
        _layerio_append_error(errors, _("failed to read drawing layer sidecar"));
        ok = FALSE;
      }
      else
      {
        params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
        cache_loaded = TRUE;
      }
    }
  }

  if(cache_loaded)
  {
    g->process.cache_valid = TRUE;
    g->process.cache_dirty = FALSE;
    dt_drawlayer_paint_runtime_state_reset(&g->process.cache_dirty_rect);
    g->process.cache_imgid = imgid;
    g_strlcpy(g->process.cache_layer_name, params->layer_name, sizeof(g->process.cache_layer_name));
    g->process.cache_layer_order = params->layer_order;
    if(ui_thread && g->controls.layer_select) _populate_layer_list(self);
    if(created) _retain_base_patch_loaded_ref(g);
  }
  else if(_layer_name_non_empty(params->layer_name))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    g->process.cache_dirty = FALSE;
    dt_drawlayer_paint_runtime_state_reset(&g->process.cache_dirty_rect);
  }

  if(ui_thread && !cache_loaded) _refresh_layer_widgets(self);
  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return ok || !cache_loaded;
}

gboolean dt_drawlayer_flush_layer_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  const dt_iop_drawlayer_params_t *params = (const dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev || !g->process.cache_valid || !g->process.cache_dirty || !g->process.base_patch.pixels) return TRUE;
  if(!_layer_name_non_empty(params ? params->layer_name : NULL)) return TRUE;
  if(!_layer_name_non_empty(g->process.cache_layer_name)) return FALSE;
  if(dt_drawlayer_worker_any_active(g->stroke.worker)) _wait_worker_idle(self, g->stroke.worker);

  char path[PATH_MAX] = { 0 };
  const int32_t flush_imgid = (g->process.cache_imgid > 0) ? g->process.cache_imgid : self->dev->image_storage.id;
  if(flush_imgid <= 0) return TRUE;
  if(!dt_drawlayer_io_sidecar_path(flush_imgid, path, sizeof(path))) return FALSE;

  int final_order = g->process.cache_layer_order;
  const char *work_profile = params ? params->work_profile : "";
  dt_drawlayer_cache_patch_rdlock(&g->process.base_patch);
  dt_drawlayer_io_patch_t io_patch = {
    .x = g->process.base_patch.x,
    .y = g->process.base_patch.y,
    .width = g->process.base_patch.width,
    .height = g->process.base_patch.height,
    .pixels = g->process.base_patch.pixels,
  };
  const gboolean ok
      = dt_drawlayer_io_store_layer(path, g->process.cache_layer_name, g->process.cache_layer_order, work_profile, &io_patch,
                                    g->process.base_patch.width, g->process.base_patch.height, FALSE, &final_order);
  dt_drawlayer_cache_patch_rdunlock(&g->process.base_patch);
  if(!ok) return FALSE;

  g->process.cache_layer_order = final_order;
  g->process.cache_dirty = FALSE;
  dt_drawlayer_paint_runtime_state_reset(&g->process.cache_dirty_rect);

  dt_iop_drawlayer_params_t *mutable_params = (dt_iop_drawlayer_params_t *)self->params;
  if(mutable_params)
  {
    if(!g_strcmp0(mutable_params->layer_name, g->process.cache_layer_name)) mutable_params->layer_order = final_order;
    mutable_params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
    _rekey_shared_base_patch(&g->process.base_patch, flush_imgid, mutable_params);
  }
  _release_all_base_patch_extra_refs(g);
  return TRUE;
}

static gboolean _ensure_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  drawlayer_view_patch_info_t view = { 0 };
  if(!dt_drawlayer_compute_view_patch(self, 0.0f, &view)) return FALSE;

  float wx0 = 0.0f, wy0 = 0.0f, wx1 = 0.0f, wy1 = 0.0f;
  if(!dt_drawlayer_layer_bounds_to_widget_bounds(self, view.layer_x0, view.layer_y0, view.layer_x1,
                                                 view.layer_y1, &wx0, &wy0, &wx1, &wy1))
    return FALSE;

  const dt_drawlayer_damaged_rect_t live_view_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(view.layer_x0), (int)floorf(view.layer_y0) },
    .se = { (int)ceilf(view.layer_x1), (int)ceilf(view.layer_y1) },
  };
  const dt_drawlayer_damaged_rect_t preview_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(wx0), (int)floorf(wy0) },
    .se = { (int)ceilf(wx1), (int)ceilf(wy1) },
  };

  const gboolean same_view = (memcmp(&g->session.live_patch, &view.patch, sizeof(view.patch)) == 0
                              && memcmp(&g->session.live_view_rect, &live_view_rect, sizeof(live_view_rect)) == 0
                              && memcmp(&g->session.preview_rect, &preview_rect, sizeof(preview_rect)) == 0);

  if(same_view)
  {
    g->session.last_view_x = self->dev->roi.x;
    g->session.last_view_y = self->dev->roi.y;
    g->session.last_view_scale = self->dev->roi.scaling;
    return TRUE;
  }

  g->session.live_patch = view.patch;
  g->session.live_view_rect = live_view_rect;
  g->session.preview_rect = preview_rect;

  dt_drawlayer_worker_reset_backend_path(g->stroke.worker);
  g->session.last_view_x = self->dev->roi.x;
  g->session.last_view_y = self->dev->roi.y;
  g->session.last_view_scale = self->dev->roi.scaling;
  return TRUE;
}

void dt_drawlayer_set_pipeline_realtime_mode(dt_iop_module_t *self, const gboolean state)
{
  if(!self || !self->dev || !self->dev->pipe) return;
  const gboolean was_realtime = dt_dev_pixelpipe_get_realtime(self->dev->pipe);
  dt_dev_pixelpipe_set_realtime(self->dev->pipe, state);
  if(was_realtime != state)
    dt_dev_pixelpipe_resync_history_main(self->dev);
  if(IS_NULL_PTR(self->dev->preview_pipe)) return;

  self->dev->preview_pipe->pause = state;
  if(state)
    dt_atomic_set_int(&self->dev->preview_pipe->shutdown, TRUE);
}

gboolean dt_drawlayer_sync_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  _pause_worker(self, g->stroke.worker);
  if(!_ensure_widget_cache(self))
  {
    _resume_worker(self, g->stroke.worker);
    return FALSE;
  }

  g->session.live_padding = _current_live_padding(self);
  _resume_worker(self, g->stroke.worker);
  return TRUE;
}
