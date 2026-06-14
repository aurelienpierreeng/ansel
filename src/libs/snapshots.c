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
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_hb.h"

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
  GtkWidget *button;
  float zoom_x, zoom_y;
  float sample_scale;
  dt_develop_t *develop;
  int32_t imgid;
  int32_t history_end;
  char filename[PATH_MAX];
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

  /* snapshot cairo surface */
  cairo_surface_t *snapshot_image;
  int32_t snapshot_imgid;
  float snapshot_zoom_level;


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

static void _lib_snapshot_clear_state(dt_lib_snapshot_t *snap)
{
  if(IS_NULL_PTR(snap)) return;
  if(!IS_NULL_PTR(snap->develop))
  {
    dt_dev_cleanup(snap->develop);
    dt_free(snap->develop);
    snap->develop = NULL;
  }
  snap->imgid = UNKNOWN_IMAGE;
  snap->history_end = 0;
  snap->sample_scale = 1.0f;
}

/**
 * @brief Freeze the current darkroom develop state into one snapshot-local develop context.
 *
 * This deep-copies the in-memory history stack and module order from the live darkroom context, so each snapshot
 * remains stable even if the user later edits `darktable.develop` or rewrites history in the database.
 *
 * @param snapshot destination snapshot slot.
 * @param source live darkroom develop context.
 *
 * @return int 0 on success, 1 on failure.
 */
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

  snapshot->develop = frozen;
  snapshot->imgid = source->image_storage.id;
  snapshot->history_end = history_end;
  SNAP_LOG("[snapshots] capture success: imgid=%d history_end=%d items=%d\n", snapshot->imgid,
           snapshot->history_end, g_list_length(frozen->history));
  return 0;
}

/**
 * @brief Recompute the selected snapshot image from a dedicated preview pipe at one history fence.
 *
 * We intentionally process a preview-style full-image buffer and keep it in a cairo surface.
 * This lets ROI pan/zoom updates reuse the same pixels through the darkroom transform only,
 * so interactive navigation does not trigger expensive snapshot recomputations.
 *
 * @param self snapshots lib module.
 * @param snap selected snapshot metadata.
 *
 * @return int 0 on success, 1 on failure.
 */
static int _lib_snapshots_refresh_pipe_image(dt_lib_module_t *self, dt_lib_snapshot_t *snap)
{
  int status = 1;
  gboolean pipe_ready = FALSE;
  dt_dev_pixelpipe_t snapshot_pipe = { 0 };
  dt_mipmap_buffer_t buf = { 0 };
  gboolean input_ready = FALSE;
  const char *fail_reason = "unknown";
  if(IS_NULL_PTR(self) || IS_NULL_PTR(snap) || IS_NULL_PTR(self->data)) return 1;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  dt_develop_t *snapshot_dev = snap->develop;
  if(IS_NULL_PTR(dev) || !dev->gui_attached)
  {
    SNAP_LOG("[snapshots] refresh failed: darkroom dev unavailable\n");
    return 1;
  }
  if(IS_NULL_PTR(snapshot_dev) || snapshot_dev->image_storage.id <= 0)
  {
    SNAP_LOG("[snapshots] refresh failed: snapshot dev unavailable selected=%u imgid=%d\n", d->selected,
             IS_NULL_PTR(snapshot_dev) ? -1 : snapshot_dev->image_storage.id);
    return 1;
  }
  const dt_dev_pixelpipe_t *preview_pipe = dev->preview_pipe;
  if(IS_NULL_PTR(preview_pipe))
  {
    SNAP_LOG("[snapshots] refresh failed: preview pipe unavailable\n");
    return 1;
  }

  // Snapshot refresh is synchronous on the UI thread. Apply the busy cursor now,
  // then restore the queued cursor once the blocking pipe work is complete.
  dt_control_change_cursor_by_name_and_flush("progress");

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, snapshot_dev->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  input_ready = !IS_NULL_PTR(buf.buf) && buf.width > 0 && buf.height > 0;
  if(!input_ready)
  {
    fail_reason = "mipmap full unavailable";
    goto cleanup;
  }

  if(!dt_dev_pixelpipe_init_preview(&snapshot_pipe, snapshot_dev))
  {
    fail_reason = "pixelpipe init preview failed";
    goto cleanup;
  }
  pipe_ready = TRUE;
  dt_dev_pixelpipe_set_input(&snapshot_pipe, snapshot_dev->image_storage.id, buf.width, buf.height, buf.iscale,
                             DT_MIPMAP_FULL);
  dt_dev_pixelpipe_create_nodes(&snapshot_pipe);
  dt_dev_pixelpipe_set_icc(&snapshot_pipe, preview_pipe->icc_type, preview_pipe->icc_filename,
                           preview_pipe->icc_intent);
  dt_dev_pixelpipe_synch_all(&snapshot_pipe);
  dt_dev_pixelpipe_get_roi_out(&snapshot_pipe, snapshot_pipe.iwidth, snapshot_pipe.iheight,
                               &snapshot_pipe.processed_width, &snapshot_pipe.processed_height);

  dt_iop_roi_t roi = { 0 };
  roi.x = 0;
  roi.y = 0;
  roi.width = snapshot_pipe.processed_width;
  roi.height = snapshot_pipe.processed_height;
  roi.scale = 1.0f;
  if(roi.width <= 0 || roi.height <= 0 || roi.scale <= 0.0f)
  {
    fail_reason = "invalid output roi";
    goto cleanup;
  }

  if(dt_dev_pixelpipe_process(&snapshot_pipe, roi))
  {
    fail_reason = "pixelpipe process failed";
    goto cleanup;
  }

  const uint64_t hash = dt_dev_backbuf_get_hash(&snapshot_pipe.backbuf);
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    fail_reason = "backbuffer hash invalid";
    goto cleanup;
  }

  /*if(hash != pipe_hash || backbuf_hist != pipe_hist)
    SNAP_LOG("[snapshots] refresh note: non-strict backbuf validity hash=%" PRIu64 " pipe_hash=%" PRIu64
             " backbuf_hist=%" PRIu64 " pipe_hist=%" PRIu64 "\n",
             hash, pipe_hash, backbuf_hist, pipe_hist);
  */
  dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID
     || !dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, hash, &data, &entry,
                                     snapshot_pipe.devid, NULL)
     || IS_NULL_PTR(data) || IS_NULL_PTR(entry))
  {
    fail_reason = "cache peek failed";
    goto cleanup;
  }

  const int bw = snapshot_pipe.backbuf.width;
  const int bh = snapshot_pipe.backbuf.height;
  if(bw <= 0 || bh <= 0)
  {
    fail_reason = "invalid backbuffer size";
    goto cleanup;
  }

  const int src_stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, bw);
  const size_t required = (size_t)src_stride * (size_t)bh;
  if(dt_pixel_cache_entry_get_size(entry) < required)
  {
    fail_reason = "cache entry too small";
    goto cleanup;
  }

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bw, bh);
  if(IS_NULL_PTR(surface))
  {
    fail_reason = "cairo surface create failed";
    goto cleanup;
  }
  cairo_surface_set_device_scale(surface, darktable.gui->ppd, darktable.gui->ppd);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);
  uint8_t *dst = cairo_image_surface_get_data(surface);
  const int dst_stride = cairo_image_surface_get_stride(surface);
  for(int y = 0; y < bh; y++)
    memcpy(dst + (size_t)y * dst_stride, (const uint8_t *)data + (size_t)y * src_stride, (size_t)src_stride);
  cairo_surface_mark_dirty(surface);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);

  if(d->snapshot_image) cairo_surface_destroy(d->snapshot_image);
  d->snapshot_image = surface;
  d->snapshot_imgid = dev->image_storage.id;
  d->snapshot_zoom_level = dt_dev_get_zoom_level(dev);
  snap->sample_scale = roi.scale;
  status = 0;
  //SNAP_LOG("[snapshots] refresh success: snapshot_imgid=%d selected=%u roi=%dx%d backbuf=%dx%d hash=%" PRIu64 "\n",
  //         d->snapshot_imgid, d->selected, roi.width, roi.height, bw, bh, hash);

cleanup:
  if(status != 0)
    SNAP_LOG("[snapshots] refresh failed: reason=%s snapshot_imgid=%d selected=%u frozen_imgid=%d in=%ux%u "
             "pipe_in=%dx%d pipe_out=%dx%d\n",
             fail_reason, d->snapshot_imgid, d->selected, snapshot_dev->image_storage.id, buf.width, buf.height,
             snapshot_pipe.iwidth, snapshot_pipe.iheight, snapshot_pipe.processed_width, snapshot_pipe.processed_height);
  if(input_ready) dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  if(pipe_ready) dt_dev_pixelpipe_cleanup(&snapshot_pipe);
  dt_control_commit_cursor();

  return status;
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

  if(!IS_NULL_PTR(d->snapshot_image))
  {
    if(d->selected >= 1 && d->selected <= d->size)
    {
      dt_lib_snapshot_t *s = d->snapshot + (d->selected - 1);
      const float current_zoom_level = dt_dev_get_zoom_level(dev);
      if(d->snapshot_imgid != s->imgid
         || fabsf(d->snapshot_zoom_level - current_zoom_level) > 1e-6f)
      {
        _lib_snapshots_refresh_pipe_image(self, s);
        if(!d->snapshot_image) return;
      }
    }

    float snapshot_scale = 1.0f;
    if(d->selected >= 1 && d->selected <= d->size)
    {
      const dt_lib_snapshot_t *s = d->snapshot + (d->selected - 1);
      if(s->sample_scale > 1e-6f) snapshot_scale = s->sample_scale;
    }
    const float zoom_level = dt_dev_get_zoom_level(dev);
    const float render_scale = zoom_level / snapshot_scale;
    const float surface_width = cairo_image_surface_get_width(d->snapshot_image) / darktable.gui->ppd;
    const float surface_height = cairo_image_surface_get_height(d->snapshot_image) / darktable.gui->ppd;
    const double tx = 0.5 * width - dev->roi.x * surface_width * render_scale;
    const double ty = 0.5 * height - dev->roi.y * surface_height * render_scale;

    float image_box[4] = { 0.0f };
    dt_dev_get_image_box_in_widget(dev, width, height, image_box);
    if(image_box[2] <= 0.0f || image_box[3] <= 0.0f) return;
    d->vp_x = image_box[0];
    d->vp_y = image_box[1];
    d->vp_width = image_box[2];
    d->vp_height = image_box[3];
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
    cairo_translate(cri, tx, ty);
    cairo_scale(cri, render_scale, render_scale);
    cairo_set_source_surface(cri, d->snapshot_image, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cri), CAIRO_FILTER_NEAREST);
    cairo_paint(cri);
    cairo_restore(cri);

    // draw the split line using the selected overlay color
    dt_draw_set_color_overlay(cri, TRUE, 0.7);

    cairo_set_line_width(cri, 1.);

    if(d->vertical)
    {
      const double lx = d->vp_x + d->vp_width * split_x;
      const double center = d->vp_y + 0.5 * d->vp_height;

      // line
      cairo_move_to(cri, lx, d->vp_y);
      cairo_line_to(cri, lx, d->vp_y + d->vp_height);
      cairo_stroke(cri);

      if(!d->dragging)
      {
        // triangle
        cairo_move_to(cri, lx, center - size);
        cairo_line_to(cri, lx - (size * 1.2), center);
        cairo_line_to(cri, lx, center + size);
        cairo_close_path(cri);
        cairo_fill(cri);

        // symbol
        _draw_sym(cri, lx, center, TRUE, d->inverted);
      }
    }
    else
    {
      const double ly = d->vp_y + d->vp_height * split_y;
      const double center = d->vp_x + 0.5 * d->vp_width;

      // line
      cairo_move_to(cri, d->vp_x, ly);
      cairo_line_to(cri, d->vp_x + d->vp_width, ly);
      cairo_stroke(cri);

      if(!d->dragging)
      {
        // triangle
        cairo_move_to(cri, center - size, ly);
        cairo_line_to(cri, center, ly - (size * 1.2));
        cairo_line_to(cri, center + size, ly);
        cairo_close_path(cri);
        cairo_fill(cri);

        // symbol
        _draw_sym(cri, center, ly, FALSE, d->inverted);
      }
    }

    /* if mouse over control, lets draw center rotate control, hide if split is dragged */
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

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(d->snapshot_image && which == 1)
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

  if(d->snapshot_image)
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

  if(d->snapshot_image)
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
  d->hover_rotation = FALSE;
  if(d->snapshot_image)
  {
    cairo_surface_destroy(d->snapshot_image);
    d->snapshot_image = NULL;
  }
  d->snapshot_imgid = UNKNOWN_IMAGE;
  d->snapshot_zoom_level = -1.0f;

  for(uint32_t k = 0; k < d->size; k++)
  {
    _lib_snapshot_clear_state(d->snapshot + k);
    gtk_widget_hide(d->snapshot[k].button);
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
  d->snapshot_imgid = UNKNOWN_IMAGE;
  d->snapshot_zoom_level = -1.0f;

  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  /* create take snapshot button */
  d->take_button = dt_action_button_new(self, N_("take snapshot"), _lib_snapshots_add_button_clicked_callback, self,
                                        _("take snapshot to compare with another image "
                                          "or the same image at another stage of development"), 0, 0);

  /*
   * initialize snapshots
   */
  char wdname[32] = { 0 };
  char localtmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(localtmpdir, sizeof(localtmpdir));

  for(int k = 0; k < d->size; k++)
  {
    /* create snapshot button */
    d->snapshot[k].button = gtk_toggle_button_new_with_label(wdname);
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(d->snapshot[k].button));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);

    g_signal_connect(G_OBJECT(d->snapshot[k].button), "clicked",
                     G_CALLBACK(_lib_snapshots_toggled_callback), self);

    /* assign snapshot number to widget */
    g_object_set_data(G_OBJECT(d->snapshot[k].button), "snapshot", GINT_TO_POINTER(k + 1));

    /* setup filename for snapshot */
    gchar *snapshot_file = g_strdup_printf("dt_snapshot_%d.png", k);
    dt_concat_path_file(d->snapshot[k].filename, localtmpdir, snapshot_file);
    dt_free(snapshot_file);

    /* add button to snapshot box */
    gtk_box_pack_start(GTK_BOX(d->snapshots_box), GTK_WIDGET(d->snapshot[k].button), FALSE, FALSE, 0);

    /* prevent widget to show on external show all */
    gtk_widget_set_no_show_all(d->snapshot[k].button, TRUE);
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

  if(!IS_NULL_PTR(d) && !IS_NULL_PTR(d->snapshot_image))
  {
    cairo_surface_destroy(d->snapshot_image);
    d->snapshot_image = NULL;
  }
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
    GtkWidget *b = d->snapshot[k].button;
    d->snapshot[k] = d->snapshot[k - 1];
    d->snapshot[k].button = b;
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k - 1].button)))));
  }

  /* update top slot with new snapshot */
  char label[64];
  GtkWidget *b = d->snapshot[0].button;
  d->snapshot[0] = last;
  d->snapshot[0].button = b;
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

  // Save current darkroom ROI and zoom as the reference view for this history snapshot.
  dt_lib_snapshot_t *s = d->snapshot + 0;
  if(IS_NULL_PTR(s)) return;
  s->zoom_y = darktable.develop->roi.y;
  s->zoom_x = darktable.develop->roi.x;
  if(_lib_snapshot_capture_state(s, darktable.develop))
  {
    _lib_snapshot_clear_state(s);
    return;
  }

  /* update slots used */
  if(d->num_snapshots != d->size) d->num_snapshots++;

  /* show active snapshot slots */
  for(uint32_t k = 0; k < d->num_snapshots; k++) gtk_widget_show(d->snapshot[k].button);

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
    /* free previous snapshot image before loading the newly-selected one */
    if(!IS_NULL_PTR(d->snapshot_image))
    {
      cairo_surface_destroy(d->snapshot_image);
      d->snapshot_image = NULL;
    }

    /* lets deactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->size; k++)
      if(GTK_WIDGET(widget) != d->snapshot[k].button)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);

    /* setup snapshot */
    dt_lib_snapshot_t *s = d->snapshot + (which - 1);
    if(!IS_NULL_PTR(s->develop) && _lib_snapshots_refresh_pipe_image(self, s) == 0)
      d->selected = which;
    else
      d->selected = 0;
  }
  else if(d->selected == (uint32_t)which)
  {
    d->selected = 0;
    if(!IS_NULL_PTR(d->snapshot_image))
    {
      cairo_surface_destroy(d->snapshot_image);
      d->snapshot_image = NULL;
    }
  }

  /* redraw center view */
  dt_control_queue_redraw_center();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
