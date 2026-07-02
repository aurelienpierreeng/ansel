/*
    This file is part of darktable,
    Copyright (C) 2011 Alexandre Prokoudine.
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011, 2014, 2016 johannes hanika.
    Copyright (C) 2011-2012, 2014-2017 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Wolfgang Goetz.
    Copyright (C) 2015 parafin.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2019, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Bill Ferguson.
    Copyright (C) 2021 Philippe Weyland.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    
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

#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/interpolation.h"
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_hb.h"
#include "dtgtk/paint.h"

#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_LIB_SNAPSHOTS_COUNT 4
#define SNAP_LOG(...) dt_print(DT_DEBUG_DEV, __VA_ARGS__)
#define HANDLE_SIZE DT_PIXEL_APPLY_DPI_DPP(36)

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
  GtkWidget *row;           // container for button + delete_button; shown/hidden as a unit
  GtkWidget *button;
  GtkWidget *delete_button;
  float sample_scale;
  cairo_surface_t *image;        // full-resolution, rendered once at capture time
  cairo_surface_t *display_image; // Mitchell-resampled crop of `image`, rebuilt on zoom change
                                  // or when panning moves the viewport out of the cached crop
  float display_scale;
  int32_t crop_x, crop_y;        // top-left of the cached crop within `image`, source pixels
  int32_t crop_w, crop_h;        // size of the cached crop within `image`, before resampling
  int32_t imgid;
  int32_t history_end;
} dt_lib_snapshot_t;


typedef struct dt_lib_snapshots_t
{
  GtkWidget *snapshots_box;

  uint32_t selected;

  /* current active snapshots */
  uint32_t num_snapshots;

  /* size of snapshots */
  uint32_t size;

  /* snapshots */
  dt_lib_snapshot_t *snapshot;


  /* change snapshot overlay controls */
  gboolean dragging, vertical, inverted;
  double vp_x, vp_y, vp_width, vp_height, vp_xpointer, vp_ypointer, vp_xrotate, vp_yrotate;
  gboolean on_going;
  gboolean hover_rotation;

  GtkWidget *take_button;
} dt_lib_snapshots_t;

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data);
static void _lib_snapshots_delete_button_clicked_callback(GtkWidget *widget, gpointer user_data);

// Reset the value fields to "empty" without destroying any cairo surface or touching GTK
// widgets. Used when a snapshot's surfaces are being handed off to another slot (compacting
// the list after a delete) rather than dropped -- the caller is responsible for the surfaces.
static void _lib_snapshot_reset_fields(dt_lib_snapshot_t *snap)
{
  snap->image = NULL;
  snap->display_image = NULL;
  snap->display_scale = 0.0f;
  snap->crop_x = snap->crop_y = snap->crop_w = snap->crop_h = 0;
  snap->imgid = UNKNOWN_IMAGE;
  snap->history_end = 0;
  snap->sample_scale = 1.0f;
}

static void _lib_snapshot_clear_state(dt_lib_snapshot_t *snap)
{
  if(IS_NULL_PTR(snap)) return;
  if(!IS_NULL_PTR(snap->display_image)) cairo_surface_destroy(snap->display_image);
  if(!IS_NULL_PTR(snap->image)) cairo_surface_destroy(snap->image);
  _lib_snapshot_reset_fields(snap);
}

// Render the frozen snapshot history into a full-resolution cairo surface.
// Returns NULL on failure. Called once at capture time; result stored in snap->image.
static cairo_surface_t *_render_snapshot_image(dt_develop_t *frozen, float scale)
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
    SNAP_LOG("[snapshots] render failed: imgid=%d reason=%s\n", frozen->image_storage.id, fail_reason);
  return surface;
}

// Freeze the current darkroom develop state and render a full-resolution snapshot image.
// The frozen context is used only during capture and freed immediately after.
// Returns 0 on success, 1 on failure.
static int _lib_snapshot_capture_state(dt_lib_snapshot_t *snapshot, dt_develop_t *source)
{
  if(IS_NULL_PTR(snapshot) || IS_NULL_PTR(source))
  {
    SNAP_LOG("[snapshots] capture failed: invalid inputs snapshot=%p source=%p\n", (void *)snapshot,
             (void *)source);
    return 1;
  }
  if(source->image_storage.id <= 0)
  {
    SNAP_LOG("[snapshots] capture failed: invalid source imgid=%d\n", source->image_storage.id);
    return 1;
  }

  _lib_snapshot_clear_state(snapshot);

  dt_develop_t *frozen = (dt_develop_t *)calloc(1, sizeof(dt_develop_t));
  if(IS_NULL_PTR(frozen)) return 1;
  dt_dev_init(frozen, 0);

  if(dt_dev_load_image(frozen, source->image_storage.id))
  {
    SNAP_LOG("[snapshots] capture failed: dt_dev_load_image failed for imgid=%d\n", source->image_storage.id);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    return 1;
  }

  GList *history_copy = NULL;
  GList *iop_order_copy = NULL;
  int32_t history_end = 0;

  dt_pthread_rwlock_rdlock(&source->history_mutex);
  history_copy = dt_history_duplicate(source->history);
  iop_order_copy = dt_ioppr_iop_order_copy_deep(source->iop_order_list);
  history_end = dt_dev_get_history_end_ext(source);
  dt_pthread_rwlock_unlock(&source->history_mutex);

  dt_dev_history_free_history(frozen);
  frozen->history = history_copy;
  g_list_free_full(frozen->iop_order_list, dt_free_gpointer);
  frozen->iop_order_list = iop_order_copy;

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
      SNAP_LOG("[snapshots] capture failed: unresolved module op=%s multi=%s priority=%d for imgid=%d\n",
               hist->op_name, hist->multi_name, hist->multi_priority, source->image_storage.id);
      dt_dev_cleanup(frozen);
      dt_free(frozen);
      return 1;
    }
  }

  dt_dev_set_history_end_ext(frozen, history_end);
  dt_dev_set_history_hash(frozen, dt_dev_history_compute_hash(frozen));

  snapshot->imgid = source->image_storage.id;
  snapshot->history_end = history_end;

  dt_control_change_cursor_by_name_and_flush("progress");
  snapshot->image = _render_snapshot_image(frozen, 1.0f);
  snapshot->sample_scale = 1.0f;
  dt_dev_cleanup(frozen);
  dt_free(frozen);
  dt_control_commit_cursor();

  SNAP_LOG("[snapshots] capture: imgid=%d history_end=%d image=%s\n",
           snapshot->imgid, snapshot->history_end,
           IS_NULL_PTR(snapshot->image) ? "FAILED" : "ok");

  return IS_NULL_PTR(snapshot->image) ? 1 : 0;
}

const char *name(struct dt_lib_module_t *self)
{
  return _("Snapshots");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 800;
}

// Rebuild snap->display_image by Mitchell-resampling a crop of snap->image at render_scale.
// The crop covers [vis_x0,vis_x1] x [vis_y0,vis_y1] (the visible viewport, in snap->image
// source-pixel space), padded by half a viewport on each side so subsequent panning can reuse
// the cached crop, plus the interpolator's tap margin. Noop if the cached crop already covers
// the requested window at the same scale.
static void _snapshot_build_display_image(dt_lib_snapshot_t *snap, float render_scale,
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
  // so the resampler must not be told its absolute position in the source image — passing
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

// draw snapshot sign
static void _draw_sym(cairo_t *cr, float x, float y, gboolean vertical, gboolean inverted)
{
  const double inv = inverted ? -0.1 : 1.0;

  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, C_("snapshot sign", "S"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  if(vertical)
    cairo_move_to(cr, x - (inv * ink.width * 1.2f), y - (ink.height / 2.0f) - DT_PIXEL_APPLY_DPI(3));
  else
    cairo_move_to(cr, x - (ink.width / 2.0), y + (-inv * (ink.height * 1.2f) - DT_PIXEL_APPLY_DPI(2)));

  dt_draw_set_color_overlay(cr, FALSE, 0.9);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;
  dt_develop_t *dev = darktable.develop;

  if(d->selected >= 1 && d->selected <= d->size)
  {
    dt_lib_snapshot_t *snap = d->snapshot + (d->selected - 1);
    if(!IS_NULL_PTR(snap->image))
    {
      const float snapshot_scale = snap->sample_scale > 1e-6f ? snap->sample_scale : 1.0f;
      const float zoom_level = dt_dev_get_zoom_level(dev);
      const float render_scale = zoom_level / snapshot_scale;
      const float ppd = darktable.gui->ppd;

      // tx/ty map snap->image source-pixel (0,0) to widget space; this only depends on the
      // full source size, never on whether we display a crop, so compute it from snap->image
      // directly and do it before building the display image (which needs the reverse mapping).
      const int src_w = cairo_image_surface_get_width(snap->image);
      const int src_h = cairo_image_surface_get_height(snap->image);
      const float disp_logical_w = src_w / ppd;
      const float disp_logical_h = src_h / ppd;
      const double tx = 0.5 * width - dev->roi.x * disp_logical_w * render_scale;
      const double ty = 0.5 * height - dev->roi.y * disp_logical_h * render_scale;

      float image_box[4] = { 0.0f };
      dt_dev_get_image_box_in_widget(dev, width, height, image_box);
      if(image_box[2] <= 0.0f || image_box[3] <= 0.0f) return;
      d->vp_x = image_box[0];
      d->vp_y = image_box[1];
      d->vp_width = image_box[2];
      d->vp_height = image_box[3];

      // Map the visible viewport back to snap->image source-pixel space so we only ever
      // Mitchell-resample the part of the snapshot that can actually be painted on screen.
      // Falls back to the whole image when the current zoom is degenerate (should not happen
      // once darkroom is showing an image, but avoids a division by ~0 turning into an
      // out-of-range int cast below).
      float vis_x0 = 0.0f, vis_y0 = 0.0f, vis_x1 = (float)src_w, vis_y1 = (float)src_h;
      if(isfinite(render_scale) && render_scale > 1e-6f)
      {
        vis_x0 = (d->vp_x - tx) * ppd / render_scale;
        vis_y0 = (d->vp_y - ty) * ppd / render_scale;
        vis_x1 = (d->vp_x + d->vp_width - tx) * ppd / render_scale;
        vis_y1 = (d->vp_y + d->vp_height - ty) * ppd / render_scale;
      }

      _snapshot_build_display_image(snap, render_scale, vis_x0, vis_y0, vis_x1, vis_y1);
      cairo_surface_t *disp = IS_NULL_PTR(snap->display_image) ? snap->image : snap->display_image;

      // If Mitchell resampling succeeded, the scaling is baked into disp, and its origin is
      // offset by the cached crop's top-left corner — no cairo_scale needed, but tx/ty must be
      // shifted accordingly. Fallback to snap->image draws the full, uncropped frame as before.
      const gboolean use_mitchell = !IS_NULL_PTR(snap->display_image);
      const double crop_tx = use_mitchell ? snap->crop_x * (double)render_scale / ppd : 0.0;
      const double crop_ty = use_mitchell ? snap->crop_y * (double)render_scale / ppd : 0.0;

      const double split_x = CLAMP(d->vp_xpointer, 0.0, 1.0);
      const double split_y = CLAMP(d->vp_ypointer, 0.0, 1.0);

      /* set x,y,w,h of surface depending on split align and invert */
      double x = d->vp_x;
      double y = d->vp_y;
      double w = d->vp_width;
      double h = d->vp_height;
      if(d->vertical)
      {
        x = d->inverted ? d->vp_x + d->vp_width * split_x : d->vp_x;
        w = d->inverted ? d->vp_width * (1.0 - split_x) : d->vp_width * split_x;
      }
      else
      {
        y = d->inverted ? d->vp_y + d->vp_height * split_y : d->vp_y;
        h = d->inverted ? d->vp_height * (1.0 - split_y) : d->vp_height * split_y;
      }

      const double size = DT_PIXEL_APPLY_DPI(d->inverted ? -15 : 15);

      cairo_save(cri);
      cairo_rectangle(cri, x, y, w, h);
      cairo_clip(cri);
      cairo_translate(cri, tx + crop_tx, ty + crop_ty);
      if(!use_mitchell) cairo_scale(cri, render_scale, render_scale);
      cairo_set_source_surface(cri, disp, 0.0, 0.0);
      // Mitchell already baked the exact target size into disp, so nearest is an exact
      // 1:1 copy there. Below 100% zoom (no Mitchell, see _snapshot_build_display_image),
      // cairo needs to downsample itself, so ask it for its area-averaging filter instead
      // of nearest, which would alias.
      cairo_pattern_set_filter(cairo_get_source(cri), use_mitchell ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
      cairo_paint(cri);
      cairo_restore(cri);

      // draw the split line using the selected overlay color
      dt_draw_set_color_overlay(cri, TRUE, 0.7);
      cairo_set_line_width(cri, 1.);

      if(d->vertical)
      {
        const double lx = d->vp_x + d->vp_width * split_x;
        const double center = d->vp_y + 0.5 * d->vp_height;

        cairo_move_to(cri, lx, d->vp_y);
        cairo_line_to(cri, lx, d->vp_y + d->vp_height);
        cairo_stroke(cri);

        if(!d->dragging)
        {
          cairo_move_to(cri, lx, center - size);
          cairo_line_to(cri, lx - (size * 1.2), center);
          cairo_line_to(cri, lx, center + size);
          cairo_close_path(cri);
          cairo_fill(cri);
          _draw_sym(cri, lx, center, TRUE, d->inverted);
        }
      }
      else
      {
        const double ly = d->vp_y + d->vp_height * split_y;
        const double center = d->vp_x + 0.5 * d->vp_width;

        cairo_move_to(cri, d->vp_x, ly);
        cairo_line_to(cri, d->vp_x + d->vp_width, ly);
        cairo_stroke(cri);

        if(!d->dragging)
        {
          cairo_move_to(cri, center - size, ly);
          cairo_line_to(cri, center, ly - (size * 1.2));
          cairo_line_to(cri, center + size, ly);
          cairo_close_path(cri);
          cairo_fill(cri);
          _draw_sym(cri, center, ly, FALSE, d->inverted);
        }
      }

      /* if mouse over control, draw center rotate handle (hidden while dragging) */
      if(!d->dragging)
      {
        const double half_handle_size = HANDLE_SIZE * 0.5;
        const gint rx = (d->vertical ? d->vp_x + d->vp_width * split_x : d->vp_x + d->vp_width * 0.5)
                        - half_handle_size;
        const gint ry = (d->vertical ? d->vp_y + d->vp_height * 0.5 : d->vp_y + d->vp_height * split_y)
                        - half_handle_size;

        dt_draw_set_color_overlay(cri, TRUE, d->hover_rotation ? 1.0 : 0.3);
        cairo_set_line_width(cri, 0.5);
        dtgtk_cairo_paint_refresh(cri, rx, ry, HANDLE_SIZE, HANDLE_SIZE, 0, NULL);
      }

      d->on_going = FALSE;

      if(d->hover_rotation) dt_control_queue_cursor_by_name("exchange");
      else if(d->dragging) dt_control_queue_cursor_by_name("grabbing");
      else
      {
        dt_view_t *view = darktable.view_manager->proxy.darkroom.view;
        if(!IS_NULL_PTR(view) && !IS_NULL_PTR(darktable.view_manager->proxy.darkroom.set_default_cursor))
          darktable.view_manager->proxy.darkroom.set_default_cursor(view, pointerx, pointery);
        else
          dt_control_queue_cursor_by_name("left_ptr");
      }
    }
  }
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);

  if(!visible_picker && d->selected > 0 && which == 1)
  {
    if(d->dragging)
    {
      d->dragging = FALSE;
      d->hover_rotation = FALSE;
    }
    // Refresh mouse_moved event
    return mouse_moved(self, x, y, 0.0, which);
  }
  return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  // only react to left click
  if(which != 1) return 0;

  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);
  
  if(!visible_picker && d->selected > 0)
  {
    if(d->on_going) return 1;
    if(d->vp_width <= 0.0 || d->vp_height <= 0.0) return 0;
    if(x < d->vp_x || x > d->vp_x + d->vp_width || y < d->vp_y || y > d->vp_y + d->vp_height) return 0;

    const double xp = CLAMP((x - d->vp_x) / d->vp_width, 0.0, 1.0);
    const double yp = CLAMP((y - d->vp_y) / d->vp_height, 0.0, 1.0);

    if(d->hover_rotation)
    {
      /* let's rotate */
      _lib_snapshot_rotation_cnt++;

      d->vertical = !d->vertical;
      if(_lib_snapshot_rotation_cnt % 2) d->inverted = !d->inverted;

      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      d->vp_xrotate = xp;
      d->vp_yrotate = yp;
      d->on_going = TRUE;
      dt_control_queue_redraw_center();
    }
    /* do the dragging !? */
    else
    {
      d->dragging = TRUE;
      d->vp_ypointer = yp;
      d->vp_xpointer = xp;
      d->vp_xrotate = 0.0;
      d->vp_yrotate = 0.0;
      dt_control_queue_redraw_center();
    }
    return 1;
  }
  return 0;
}

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);

  if(!visible_picker && d->selected > 0)
  {
    if(d->vp_width <= 0.0 || d->vp_height <= 0.0) return 0;
    const double xp = CLAMP((x - d->vp_x) / d->vp_width, 0.0, 1.0);
    const double yp = CLAMP((y - d->vp_y) / d->vp_height, 0.0, 1.0);
    d->hover_rotation = FALSE;

    if(!d->dragging)
    {
      const double split_x = CLAMP(d->vp_xpointer, 0.0, 1.0);
      const double split_y = CLAMP(d->vp_ypointer, 0.0, 1.0);
      const double handle_mouse = (DT_GUI_MOUSE_EFFECT_RADIUS + HANDLE_SIZE) * 0.5;
      const double rxc = d->vertical ? d->vp_x + d->vp_width * split_x : d->vp_x + d->vp_width * 0.5;
      const double ryc = d->vertical ? d->vp_y + d->vp_height * 0.5 : d->vp_y + d->vp_height * split_y;
      const double dx = x - rxc;
      const double dy = y - ryc;
      d->hover_rotation = (dx * dx + dy * dy) < (handle_mouse * handle_mouse);
    }
    else
    {
      /* update pointer pos */
      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
    }
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  d->num_snapshots = 0;
  d->selected = 0;
  d->hover_rotation = FALSE;

  for(uint32_t k = 0; k < d->size; k++)
  {
    _lib_snapshot_clear_state(d->snapshot + k);
    gtk_widget_hide(d->snapshot[k].row);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);
  }

  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)g_malloc0(sizeof(dt_lib_snapshots_t));
  self->data = (void *)d;

  /* initialize snapshot storages */
  d->size = 4;
  d->snapshot = (dt_lib_snapshot_t *)g_malloc0_n(d->size, sizeof(dt_lib_snapshot_t));
  d->vp_x = 0.0;
  d->vp_y = 0.0;
  d->vp_width = 1.0;
  d->vp_height = 1.0;
  d->vp_xpointer = 0.5;
  d->vp_ypointer = 0.5;
  d->vp_xrotate = 0.0;
  d->vp_yrotate = 0.0;
  d->vertical = TRUE;
  d->on_going = FALSE;
  d->hover_rotation = FALSE;
  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  /* create take snapshot button */
  d->take_button = dt_action_button_new(self, N_("take snapshot"), _lib_snapshots_add_button_clicked_callback, self,
                                        _("take snapshot to compare with another image "
                                          "or the same image at another stage of development"), 0, 0);

  for(int k = 0; k < d->size; k++)
  {
    d->snapshot[k].button = gtk_toggle_button_new_with_label("");
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(d->snapshot[k].button));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(d->snapshot[k].button, TRUE);

    g_signal_connect(G_OBJECT(d->snapshot[k].button), "clicked",
                     G_CALLBACK(_lib_snapshots_toggled_callback), self);

    g_object_set_data(G_OBJECT(d->snapshot[k].button), "snapshot", GINT_TO_POINTER(k + 1));

    // Same trash icon as the shapes list under "drawn mask" in develop/blend_gui.c
    // (group_delete_col): themed "user-trash-symbolic", not a dtgtk cairo glyph.
    d->snapshot[k].delete_button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(d->snapshot[k].delete_button), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(d->snapshot[k].delete_button),
                         gtk_image_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(d->snapshot[k].delete_button, _("remove this snapshot"));
    g_object_set_data(G_OBJECT(d->snapshot[k].delete_button), "snapshot", GINT_TO_POINTER(k + 1));
    g_signal_connect(G_OBJECT(d->snapshot[k].delete_button), "clicked",
                     G_CALLBACK(_lib_snapshots_delete_button_clicked_callback), self);

    d->snapshot[k].row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(d->snapshot[k].row), d->snapshot[k].button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(d->snapshot[k].row), d->snapshot[k].delete_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(d->snapshots_box), d->snapshot[k].row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(d->snapshot[k].row, TRUE);
  }

  /* add snapshot box and take snapshot button to widget ui*/
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->snapshots_box, 1, "plugins/darkroom/snapshots/windowheight",
                                       DT_UI_RESIZE_DYNAMIC), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->take_button), TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  for(uint32_t k = 0; k < d->size; k++) _lib_snapshot_clear_state(d->snapshot + k);
  dt_free(d->snapshot);

  dt_free(self->data);
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;

  /* backup last snapshot slot */
  if(d->size <= 0) return;
  dt_lib_snapshot_t last = d->snapshot[d->size - 1];

  /* rotate slots down to make room for new one on top */
  for(int k = d->size - 1; k > 0; k--)
  {
    GtkWidget *r = d->snapshot[k].row;
    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *db = d->snapshot[k].delete_button;
    d->snapshot[k] = d->snapshot[k - 1];
    d->snapshot[k].row = r;
    d->snapshot[k].button = b;
    d->snapshot[k].delete_button = db;
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k - 1].button)))));
  }

  /* update top slot with new snapshot */
  char label[64];
  GtkWidget *r = d->snapshot[0].row;
  GtkWidget *b = d->snapshot[0].button;
  GtkWidget *db = d->snapshot[0].delete_button;
  d->snapshot[0] = last;
  d->snapshot[0].row = r;
  d->snapshot[0].button = b;
  d->snapshot[0].delete_button = db;
  const gchar *name = _("original");
  gchar *dynamic_name = NULL;
  if(dt_dev_get_history_end_ext(darktable.develop) > 0)
  {
    dt_dev_history_item_t *history_item = g_list_nth_data(darktable.develop->history,
                                                          dt_dev_get_history_end_ext(darktable.develop) - 1);
    if(!IS_NULL_PTR(history_item) && !IS_NULL_PTR(history_item->module))
    {
      dynamic_name = dt_history_item_get_name(history_item->module);
      if(!IS_NULL_PTR(dynamic_name)) name = dynamic_name;
    }
    else
      name = _("unknown");
  }
  g_snprintf(label, sizeof(label), "%s (%d)", name, dt_dev_get_history_end_ext(darktable.develop));
  if(!IS_NULL_PTR(dynamic_name)) dt_free(dynamic_name);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[0].button))), label);

  dt_lib_snapshot_t *s = d->snapshot + 0;
  if(_lib_snapshot_capture_state(s, darktable.develop))
  {
    _lib_snapshot_clear_state(s);
    return;
  }

  /* update slots used */
  if(d->num_snapshots != d->size) d->num_snapshots++;

  /* show active snapshot slots. row has no-show-all set (so ambient show_all() calls on
   * an ancestor leave inactive slots hidden), which means show_all() on row itself is
   * *also* a no-op -- it must be shown explicitly, and so must its children since they
   * were never individually shown either. */
  for(uint32_t k = 0; k < d->num_snapshots; k++)
  {
    gtk_widget_show(d->snapshot[k].row);
    gtk_widget_show(d->snapshot[k].button);
    gtk_widget_show(d->snapshot[k].delete_button);
  }
}

static void _lib_snapshots_delete_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;

  const uint32_t which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));
  if(which < 1 || which > d->num_snapshots) return;
  const uint32_t p = which - 1;

  if(d->selected == which) d->selected = 0;
  else if(d->selected > which) d->selected--;

  // Free the deleted snapshot's surfaces now, before its slot gets overwritten by the shift
  // below -- otherwise they would be silently leaked (overwritten with no destroy call).
  _lib_snapshot_clear_state(d->snapshot + p);

  /* shift every older slot up by one to fill the gap, keeping row/button/delete_button
   * pinned to their screen position (same pattern as the "take snapshot" rotation). Each
   * `d->snapshot[k] = d->snapshot[k + 1]` *moves* ownership of the cairo surfaces (struct
   * assignment, no refcounting) rather than copying it, so nothing here may destroy them:
   * the slot being overwritten already handed its own surfaces to k-1 in the previous
   * iteration (or, for k == p, they were just freed above). */
  for(uint32_t k = p; k < d->num_snapshots - 1; k++)
  {
    GtkWidget *r = d->snapshot[k].row;
    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *db = d->snapshot[k].delete_button;
    d->snapshot[k] = d->snapshot[k + 1];
    d->snapshot[k].row = r;
    d->snapshot[k].button = b;
    d->snapshot[k].delete_button = db;
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k + 1].button)))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), (k + 1) == d->selected);
  }

  // The last active slot's surfaces (if any) were just relocated into the slot above it by
  // the loop's last iteration, so only reset its fields here -- destroying them would be a
  // double-free of surfaces now owned by that other slot.
  const uint32_t last = d->num_snapshots - 1;
  _lib_snapshot_reset_fields(d->snapshot + last);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[last].button), FALSE);
  gtk_widget_hide(d->snapshot[last].row);
  d->num_snapshots--;

  dt_control_queue_redraw_center();
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  /* get current snapshot index */
  int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));

  /* check if snapshot is activated */
  if(gtk_toggle_button_get_active(widget))
  {
    /* lets deactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->size; k++)
      if(GTK_WIDGET(widget) != d->snapshot[k].button)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);

    const dt_lib_snapshot_t *s = d->snapshot + (which - 1);
    d->selected = (!IS_NULL_PTR(s->image)) ? which : 0;
  }
  else if(d->selected == (uint32_t)which)
  {
    d->selected = 0;
  }

  /* redraw center view */
  dt_control_queue_redraw_center();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
