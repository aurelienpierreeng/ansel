/*
    This file is part of ansel,
    Copyright (C) 2025-2026 Guillaume STUTIN.

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

#include "develop/dev_snapshot.h"

#include "common/darktable.h"
#include "common/history.h"
#include "common/interpolation.h"
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_hb.h"

#include <math.h>

void dt_dev_snapshot_clear(dt_dev_snapshot_t *snap)
{
  if(IS_NULL_PTR(snap)) return;
  if(!IS_NULL_PTR(snap->display_image)) cairo_surface_destroy(snap->display_image);
  if(!IS_NULL_PTR(snap->image)) cairo_surface_destroy(snap->image);
  snap->image = NULL;
  snap->display_image = NULL;
  snap->display_scale = 0.0f;
  snap->crop_x = snap->crop_y = snap->crop_w = snap->crop_h = 0;
  snap->sample_scale = 1.0f;
}

// Run a dedicated, one-shot pixelpipe over `frozen`'s history and copy its output into a new
// full-resolution cairo surface. `frozen` is never dev->pipe/dev->preview_pipe and is not shared
// with any other thread, so this can safely run on the caller's thread (typically the GUI
// thread, behind a busy cursor -- see the callers in libs/snapshots.c and libs/duplicate.c).
// Returns NULL on failure.
static cairo_surface_t *_render(dt_develop_t *frozen, float scale)
{
  cairo_surface_t *surface = NULL;
  dt_mipmap_buffer_t buf = { 0 };
  gboolean pipe_ready = FALSE;
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  const char *fail_reason = "unknown";
  const dt_dev_pixelpipe_t *live_preview = NULL;
  dt_iop_roi_t roi = { 0 };
  uint64_t hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  int bw = 0, bh = 0, src_stride = 0, dst_stride = 0;
  uint8_t *dst = NULL;

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, frozen->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(IS_NULL_PTR(buf.buf) || buf.width <= 0 || buf.height <= 0)
  {
    fail_reason = "mipmap full unavailable";
    goto cleanup;
  }

  if(!dt_dev_pixelpipe_init_preview(&pipe, frozen))
  {
    fail_reason = "pixelpipe init failed";
    goto cleanup;
  }
  pipe_ready = TRUE;

  // Reuse the live darkroom's ICC settings if any image is currently open in darkroom, so a
  // captured snapshot/preview soft-proofs the same way the live pipe does. Harmless when frozen
  // is the same image dev->preview_pipe already reflects; still correct when it's a different
  // one, since ICC intent/profile are a display-wide GUI setting, not per-image state.
  live_preview = darktable.develop ? darktable.develop->preview_pipe : NULL;
  dt_dev_pixelpipe_set_input(&pipe, frozen->image_storage.id, buf.width, buf.height, buf.iscale, DT_MIPMAP_FULL);
  dt_dev_pixelpipe_create_nodes(&pipe);
  if(!IS_NULL_PTR(live_preview))
    dt_dev_pixelpipe_set_icc(&pipe, live_preview->icc_type, live_preview->icc_filename, live_preview->icc_intent);
  dt_dev_pixelpipe_synch_all(&pipe);
  dt_dev_pixelpipe_propagate_formats(&pipe);
  dt_dev_pixelpipe_get_roi_out(&pipe, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);

  roi = (dt_iop_roi_t){ .x = 0, .y = 0,
                        .width  = MAX(1, (int)roundf(scale * pipe.processed_width)),
                        .height = MAX(1, (int)roundf(scale * pipe.processed_height)),
                        .scale  = scale };

  if(dt_dev_pixelpipe_process(&pipe, roi))
  {
    fail_reason = "pixelpipe process failed";
    goto cleanup;
  }

  hash = dt_dev_backbuf_get_hash(&pipe.backbuf);
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    fail_reason = "backbuffer hash invalid";
    goto cleanup;
  }

  if(!dt_dev_pixelpipe_cache_ref_entry_by_hash(darktable.pixelpipe_cache, hash, &data, &entry)
     || IS_NULL_PTR(data) || IS_NULL_PTR(entry))
  {
    fail_reason = "cache peek failed";
    goto cleanup;
  }

  bw = pipe.backbuf.width;
  bh = pipe.backbuf.height;
  src_stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, bw);
  if(bw <= 0 || bh <= 0
     || dt_pixel_cache_entry_get_size(entry) < (size_t)src_stride * (size_t)bh)
  {
    fail_reason = "invalid backbuffer";
    goto cleanup;
  }

  surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bw, bh);
  if(IS_NULL_PTR(surface))
  {
    fail_reason = "cairo surface create failed";
    goto cleanup;
  }
  cairo_surface_set_device_scale(surface, darktable.gui->ppd, darktable.gui->ppd);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);
  dst = cairo_image_surface_get_data(surface);
  dst_stride = cairo_image_surface_get_stride(surface);
  for(int y = 0; y < bh; y++)
    memcpy(dst + (size_t)y * dst_stride, (const uint8_t *)data + (size_t)y * src_stride, (size_t)src_stride);
  cairo_surface_mark_dirty(surface);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);

cleanup:
  if(entry) dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, entry);
  if(pipe_ready) dt_dev_pixelpipe_cleanup(&pipe);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  if(IS_NULL_PTR(surface))
    dt_print(DT_DEBUG_DEV, "[dev_snapshot] render failed: imgid=%d reason=%s\n",
             frozen->image_storage.id, fail_reason);
  return surface;
}

gboolean dt_dev_snapshot_capture(dt_dev_snapshot_t *snap, int32_t imgid, float scale,
                                  GList *history_override, GList *iop_order_override,
                                  int32_t history_end_override)
{
  dt_develop_t *frozen = NULL;

  dt_dev_snapshot_clear(snap);
  if(IS_NULL_PTR(snap) || imgid <= 0) goto fail;

  frozen = (dt_develop_t *)calloc(1, sizeof(dt_develop_t));
  if(IS_NULL_PTR(frozen)) goto fail;
  dt_dev_init(frozen, 0);

  if(dt_dev_load_image(frozen, imgid))
  {
    dt_print(DT_DEBUG_DEV, "[dev_snapshot] capture failed: dt_dev_load_image failed for imgid=%d\n", imgid);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }

  if(history_override)
  {
    dt_dev_history_free_history(frozen);
    frozen->history = history_override;
    history_override = NULL; // ownership transferred to frozen; do not free again below
    g_list_free_full(frozen->iop_order_list, dt_free_gpointer);
    frozen->iop_order_list = iop_order_override;
    iop_order_override = NULL;

    for(GList *history = g_list_first(frozen->history); history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(IS_NULL_PTR(hist)) continue;
      hist->module = dt_dev_get_module_instance(frozen, hist->op_name, hist->multi_name, hist->multi_priority);
      if(IS_NULL_PTR(hist->module))
        hist->module = dt_dev_create_module_instance(frozen, hist->op_name, hist->multi_name, hist->multi_priority, FALSE);
      if(IS_NULL_PTR(hist->module))
        hist->module = dt_iop_get_module_by_op_priority(frozen->iop, hist->op_name, -1);
      if(IS_NULL_PTR(hist->module))
      {
        dt_print(DT_DEBUG_DEV,
                 "[dev_snapshot] capture failed: unresolved module op=%s multi=%s priority=%d for imgid=%d\n",
                 hist->op_name, hist->multi_name, hist->multi_priority, imgid);
        dt_dev_cleanup(frozen);
        dt_free(frozen);
        goto fail;
      }
    }

    dt_dev_set_history_end_ext(frozen, history_end_override);
    dt_dev_set_history_hash(frozen, dt_dev_history_compute_hash(frozen));
  }

  snap->image = _render(frozen, scale);
  snap->sample_scale = scale;
  dt_dev_cleanup(frozen);
  dt_free(frozen);
  return !IS_NULL_PTR(snap->image);

fail:
  if(history_override) g_list_free_full(history_override, dt_free_gpointer);
  if(iop_order_override) g_list_free_full(iop_order_override, dt_free_gpointer);
  return FALSE;
}

// Rebuild snap->display_image by Mitchell-resampling a crop of snap->image at render_scale.
// The crop covers [vis_x0,vis_x1] x [vis_y0,vis_y1] (the visible viewport, in snap->image
// source-pixel space), padded by half a viewport on each side so subsequent panning can reuse
// the cached crop, plus the interpolator's tap margin. Noop if the cached crop already covers
// the requested window at the same scale.
static void _build_display_image(dt_dev_snapshot_t *snap, float render_scale,
                                  float vis_x0, float vis_y0, float vis_x1, float vis_y1)
{
  if(IS_NULL_PTR(snap->image)) return;

  // Below 100%, we're discarding detail anyway: cairo's own scaling is cheap and good
  // enough once downsampling, so reserve the manual float round-trip + Mitchell for
  // zoom >= 100%, where reconstruction quality actually matters.
  if(render_scale < 1.0f)
  {
    if(!IS_NULL_PTR(snap->display_image))
    {
      cairo_surface_destroy(snap->display_image);
      snap->display_image = NULL;
    }
    snap->display_scale = 0.0f;
    snap->crop_x = snap->crop_y = snap->crop_w = snap->crop_h = 0;
    return;
  }

  const int src_w = cairo_image_surface_get_width(snap->image);
  const int src_h = cairo_image_surface_get_height(snap->image);

  const struct dt_interpolation *mitchell = dt_interpolation_new(DT_INTERPOLATION_MITCHELL);

  // Taps span `mitchell->width` samples on the output side; render_scale >= 1 here, so the
  // support never widens past that half-width (see _prepare_resampling_plan for the general,
  // downscale-widening case).
  const int tap_margin = (int)mitchell->width + 1;
  const float pad_x = 0.5f * (vis_x1 - vis_x0);
  const float pad_y = 0.5f * (vis_y1 - vis_y0);

  int want_x0 = (int)floorf(vis_x0 - pad_x) - tap_margin;
  int want_y0 = (int)floorf(vis_y0 - pad_y) - tap_margin;
  int want_x1 = (int)ceilf(vis_x1 + pad_x) + tap_margin;
  int want_y1 = (int)ceilf(vis_y1 + pad_y) + tap_margin;

  want_x0 = CLAMP(want_x0, 0, src_w);
  want_y0 = CLAMP(want_y0, 0, src_h);
  want_x1 = CLAMP(want_x1, want_x0, src_w);
  want_y1 = CLAMP(want_y1, want_y0, src_h);

  const int vis_ix0 = (int)floorf(vis_x0);
  const int vis_iy0 = (int)floorf(vis_y0);
  const int vis_ix1 = (int)ceilf(vis_x1);
  const int vis_iy1 = (int)ceilf(vis_y1);

  const gboolean scale_ok = !IS_NULL_PTR(snap->display_image) && fabsf(snap->display_scale - render_scale) < 1e-4f;
  const gboolean crop_ok = scale_ok
                          && snap->crop_x <= vis_ix0 && snap->crop_y <= vis_iy0
                          && snap->crop_x + snap->crop_w >= vis_ix1
                          && snap->crop_y + snap->crop_h >= vis_iy1;
  if(crop_ok) return;

  if(!IS_NULL_PTR(snap->display_image))
  {
    cairo_surface_destroy(snap->display_image);
    snap->display_image = NULL;
  }
  snap->display_scale = 0.0f;

  const int crop_w = MAX(1, want_x1 - want_x0);
  const int crop_h = MAX(1, want_y1 - want_y0);
  const int dst_w = MAX(1, (int)roundf((float)crop_w * render_scale));
  const int dst_h = MAX(1, (int)roundf((float)crop_h * render_scale));

  float *in_f = dt_alloc_align_float((size_t)crop_w * crop_h * 4);
  if(IS_NULL_PTR(in_f)) return;
  float *out_f = dt_alloc_align_float((size_t)dst_w * dst_h * 4);
  if(IS_NULL_PTR(out_f)) { dt_free_align(in_f); return; }

  // uint8 Cairo RGB24 (BGRa on LE) → float RGBa, restricted to the crop window
  cairo_surface_flush(snap->image);
  const uint8_t *src = cairo_image_surface_get_data(snap->image);
  const int src_stride = cairo_image_surface_get_stride(snap->image);
  for(int y = 0; y < crop_h; y++)
  {
    const uint8_t *row = src + (size_t)(y + want_y0) * src_stride + (size_t)want_x0 * 4;
    float *frow = in_f + (size_t)y * crop_w * 4;
    for(int x = 0; x < crop_w; x++)
    {
      frow[x * 4 + 0] = (float)row[x * 4 + 2] * (1.0f / 255.0f); // R
      frow[x * 4 + 1] = (float)row[x * 4 + 1] * (1.0f / 255.0f); // G
      frow[x * 4 + 2] = (float)row[x * 4 + 0] * (1.0f / 255.0f); // B
      frow[x * 4 + 3] = 0.0f;
    }
  }

  // roi_in/roi_out both origin at (0,0): the crop was copied into its own zero-based buffer,
  // so the resampler must not be told its absolute position in the source image -- passing
  // the crop's true offset here would offset the resampled result (see the comment in
  // iop/finalscale.c process() about roi.x/y needing to stay at 0 for a pure resample).
  const dt_iop_roi_t roi_in  = { .x = 0, .y = 0, .width = crop_w, .height = crop_h, .scale = 1.0f };
  const dt_iop_roi_t roi_out = { .x = 0, .y = 0, .width = dst_w, .height = dst_h, .scale = render_scale };
  dt_interpolation_resample(mitchell, out_f, &roi_out, in_f, &roi_in);
  dt_free_align(in_f);

  // float RGBa → uint8 Cairo RGB24 (BGRa on LE)
  cairo_surface_t *display = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dst_w, dst_h);
  if(!IS_NULL_PTR(display))
  {
    cairo_surface_set_device_scale(display, darktable.gui->ppd, darktable.gui->ppd);
    uint8_t *dst = cairo_image_surface_get_data(display);
    const int dst_stride = cairo_image_surface_get_stride(display);
    for(int y = 0; y < dst_h; y++)
    {
      const float *frow = out_f + (size_t)y * dst_w * 4;
      uint8_t *drow = dst + (size_t)y * dst_stride;
      for(int x = 0; x < dst_w; x++)
      {
        drow[x * 4 + 2] = (uint8_t)CLAMP(frow[x * 4 + 0] * 255.0f + 0.5f, 0.0f, 255.0f); // R
        drow[x * 4 + 1] = (uint8_t)CLAMP(frow[x * 4 + 1] * 255.0f + 0.5f, 0.0f, 255.0f); // G
        drow[x * 4 + 0] = (uint8_t)CLAMP(frow[x * 4 + 2] * 255.0f + 0.5f, 0.0f, 255.0f); // B
      }
    }
    cairo_surface_mark_dirty(display);
    snap->display_image = display;
    snap->display_scale = render_scale;
    snap->crop_x = want_x0;
    snap->crop_y = want_y0;
    snap->crop_w = crop_w;
    snap->crop_h = crop_h;
  }
  dt_free_align(out_f);
}

void dt_dev_snapshot_draw(dt_dev_snapshot_t *snap, cairo_t *cri, struct dt_develop_t *dev,
                           int32_t width, int32_t height,
                           double clip_x, double clip_y, double clip_w, double clip_h)
{
  if(IS_NULL_PTR(snap) || IS_NULL_PTR(snap->image) || IS_NULL_PTR(dev) || IS_NULL_PTR(cri)) return;
  if(clip_w <= 0.0 || clip_h <= 0.0) return;

  const float snapshot_scale = snap->sample_scale > 1e-6f ? snap->sample_scale : 1.0f;
  const float zoom_level = dt_dev_get_zoom_level(dev);
  const float render_scale = zoom_level / snapshot_scale;
  const float ppd = darktable.gui->ppd;

  // tx/ty map snap->image source-pixel (0,0) to widget space; this only depends on the full
  // source size, never on the clip rect, so compute it before building the display crop (which
  // needs the reverse mapping to know what part of snap->image is actually visible).
  const int src_w = cairo_image_surface_get_width(snap->image);
  const int src_h = cairo_image_surface_get_height(snap->image);
  const float disp_logical_w = src_w / ppd;
  const float disp_logical_h = src_h / ppd;
  const double tx = 0.5 * width - dev->roi.x * disp_logical_w * render_scale;
  const double ty = 0.5 * height - dev->roi.y * disp_logical_h * render_scale;

  // Map the clip rect back to snap->image source-pixel space so we only ever Mitchell-resample
  // the part of the snapshot that can actually be painted on screen. Falls back to the whole
  // image when the current zoom is degenerate (should not happen once darkroom is showing an
  // image, but avoids a division by ~0 turning into an out-of-range int cast below).
  float vis_x0 = 0.0f, vis_y0 = 0.0f, vis_x1 = (float)src_w, vis_y1 = (float)src_h;
  if(isfinite(render_scale) && render_scale > 1e-6f)
  {
    vis_x0 = (clip_x - tx) * ppd / render_scale;
    vis_y0 = (clip_y - ty) * ppd / render_scale;
    vis_x1 = (clip_x + clip_w - tx) * ppd / render_scale;
    vis_y1 = (clip_y + clip_h - ty) * ppd / render_scale;
  }

  _build_display_image(snap, render_scale, vis_x0, vis_y0, vis_x1, vis_y1);
  cairo_surface_t *disp = IS_NULL_PTR(snap->display_image) ? snap->image : snap->display_image;

  // If Mitchell resampling succeeded, the scaling is baked into disp, and its origin is offset
  // by the cached crop's top-left corner -- no cairo_scale needed, but tx/ty must be shifted
  // accordingly. Fallback to snap->image draws the full, uncropped frame as before.
  const gboolean use_mitchell = !IS_NULL_PTR(snap->display_image);
  const double crop_tx = use_mitchell ? snap->crop_x * (double)render_scale / ppd : 0.0;
  const double crop_ty = use_mitchell ? snap->crop_y * (double)render_scale / ppd : 0.0;

  cairo_save(cri);
  cairo_rectangle(cri, clip_x, clip_y, clip_w, clip_h);
  cairo_clip(cri);
  cairo_translate(cri, tx + crop_tx, ty + crop_ty);
  if(!use_mitchell) cairo_scale(cri, render_scale, render_scale);
  cairo_set_source_surface(cri, disp, 0.0, 0.0);
  // Mitchell already baked the exact target size into disp, so nearest is an exact 1:1 copy
  // there. Below 100% zoom (no Mitchell, see _build_display_image), cairo needs to downsample
  // itself, so ask it for its area-averaging filter instead of nearest, which would alias.
  cairo_pattern_set_filter(cairo_get_source(cri), use_mitchell ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
  cairo_paint(cri);
  cairo_restore(cri);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
