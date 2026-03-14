/*
    This file is part of darktable,
    Copyright (C) 2009-2014, 2018 johannes hanika.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010-2018, 2020 Tobias Ellinghaus.
    Copyright (C) 2011-2012 Antony Dovgal.
    Copyright (C) 2011 Edouard Gomez.
    Copyright (C) 2011, 2013-2015 Jérémy Rosen.
    Copyright (C) 2011 Karl Mikaelsson.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Steven Carter.
    Copyright (C) 2012-2016, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012-2013 José Carlos García Sogo.
    Copyright (C) 2012-2022 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014 Ulrich Pegelow.
    Copyright (C) 2013 Dennis Gnad.
    Copyright (C) 2013 Pascal de Bruijn.
    Copyright (C) 2013-2017, 2020 Roman Lebedev.
    Copyright (C) 2014, 2017, 2020-2021 Dan Torop.
    Copyright (C) 2014, 2017, 2019 parafin.
    Copyright (C) 2014 Pedro Côrte-Real.
    Copyright (C) 2014 Stéphane Gimenez.
    Copyright (C) 2015 Guillaume Subiron.
    Copyright (C) 2016 Asma.
    Copyright (C) 2016 itinerarium.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017 Matthieu Moy.
    Copyright (C) 2017-2019 Peter Budai.
    Copyright (C) 2017 Žilvinas Žaltiena.
    Copyright (C) 2018 Hans Rosenfeld.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019 Alexander Blinne.
    Copyright (C) 2019 Alexis Mousset.
    Copyright (C) 2019-2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2019, 2021 Bill Ferguson.
    Copyright (C) 2019-2022 Diederik Ter Rahe.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019, 2021 luzpaz.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Marco.
    Copyright (C) 2020-2021 Mark-64.
    Copyright (C) 2020 Miloš Komarčević.
    Copyright (C) 2020 Reinout Nonhebel.
    Copyright (C) 2020 U-DESKTOP-HQME86J\marco.
    Copyright (C) 2021 darkelectron.
    Copyright (C) 2021 lhietal.
    Copyright (C) 2021-2022 Nicolas Auffray.
    Copyright (C) 2021-2022 Ralf Brown.
    Copyright (C) 2021-2022 Sakari Kapanen.
    Copyright (C) 2021 Victor Forsiuk.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2023 Maurizio Paglia.
    Copyright (C) 2023 Ricky Moon.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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
/** this is the view for the darkroom module.  */

#include "common/extra_optimizations.h"

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"

#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/gui_throttle.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"
#include "libs/modulegroups.h"
#include "views/view.h"
#include "views/view_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#ifdef USE_LUA
#include "lua/image.h"
#endif

#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef G_SOURCE_FUNC // Defined for glib >= 2.58
#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void)) (f))
#endif

DT_MODULE(1)

typedef struct coords_t
{
  double roi_coord[2];  // coordinates in ROI space
  double roi_delta[2];  // delta in ROI space
  double full_delta[2]; // delta in image space
  double full[2];       // coordinates in image space
}coords_t;

static void _update_softproof_gamut_checking(dt_develop_t *d);

/* signal handler for filmstrip image switching */
static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data);
static void _darkroom_image_loaded_callback(gpointer instance, guint request_id, guint result, gpointer payload,
                                            gpointer user_data);
static void _darkroom_cancel_image_load_job(void);

static void _dev_change_image(dt_view_t *self, const int32_t imgid);

static int _change_scaling(dt_develop_t *dev, const float x, const float y, const float new_scaling);
static void _release_expose_source_caches(void);

typedef struct dt_darkroom_image_load_job_t
{
  int32_t imgid;
  guint request_id;
} dt_darkroom_image_load_job_t;

typedef struct dt_darkroom_image_load_result_t
{
  int32_t imgid;
  int32_t raw_width;
  int32_t raw_height;
  gboolean raw_inited;
  dt_image_t image_storage;
} dt_darkroom_image_load_result_t;

static dt_job_t *_darkroom_image_load_job = NULL;
static guint _darkroom_image_load_serial = 0;
static guint _darkroom_image_load_active_request = 0;
static int32_t _darkroom_pending_imgid = UNKNOWN_IMAGE;
static dt_iop_module_t *_darkroom_pending_focus_module = NULL;

const char *name(const dt_view_t *self)
{
  return _("Darkroom");
}


void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_develop_t));
  dt_dev_init((dt_develop_t *)self->data, 1);
  darktable.develop = (dt_develop_t *)self->data;
  darktable.view_manager->proxy.darkroom.view = self;

#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  const int my_type = dt_lua_module_entry_get_type(L, "view", self->module_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, display_image_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "display_image");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "darkroom-image-loaded");
#endif
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_DARKROOM;
}

void cleanup(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  _release_expose_source_caches();
  dt_gui_throttle_cancel(dev);

  // unref the grid lines popover if needed
  if(darktable.view_manager->guides_popover) g_object_unref(darktable.view_manager->guides_popover);
  if(darktable.signals)
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_darkroom_image_loaded_callback), (gpointer)self);
  _darkroom_cancel_image_load_job();
  _darkroom_pending_imgid = UNKNOWN_IMAGE;
  _darkroom_pending_focus_module = NULL;
  dt_dev_cleanup(dev);
  dt_free(dev);
}

static cairo_status_t _write_snapshot_data(void *closure, const unsigned char *data, unsigned int length)
{
  const int fd = GPOINTER_TO_INT(closure);
  ssize_t res = write(fd, data, length);
  if(res != length)
    return CAIRO_STATUS_WRITE_ERROR;
  return CAIRO_STATUS_SUCCESS;
}

static dt_darkroom_layout_t _lib_darkroom_get_layout(dt_view_t *self)
{
  return DT_DARKROOM_LAYOUT_EDITING;
}

static gboolean _darkroom_is_only_selected_sample(gboolean is_primary_sample, dt_colorpicker_sample_t *selected_sample, gboolean display_samples)
{
  return !is_primary_sample && selected_sample && !darktable.lib->proxy.colorpicker.display_samples;
}

/**
 * @brief Draw colorpicker samples overlays in darkroom view
 * 
 * @param self actual view
 * @param cri cairo context
 * @param width width of the widget
 * @param height height of the widget
 * @param pozx x pointer
 * @param pozy y pointer
 * @param samples list of samples to draw
 * @param is_primary_sample whether we are drawing the primary sample or live samples
 */
static void _darkroom_pickers_draw(dt_view_t *self, cairo_t *cri,
                                   int32_t width, int32_t height, int32_t pozx, int32_t pozy,
                                   GSList *samples, gboolean is_primary_sample)
{
  if(!samples) return;

  dt_develop_t *dev = (dt_develop_t *)self->data;

  cairo_save(cri);
  // The colorpicker samples bounding rectangle should only be displayed inside the visible image

  const double wd = dev->roi.preview_width;
  const double ht = dev->roi.preview_height;
  const double scale = dt_dev_get_fit_scale(dev);
  const double lw = 1.0 / scale;
  const double dashes[1] = { lw * 4.0 };

  dt_dev_rescale_roi(dev, cri, width, height);

  // makes point sample crosshair gap look nicer
  cairo_set_line_cap(cri, CAIRO_LINE_CAP_SQUARE);

  dt_colorpicker_sample_t *selected_sample = darktable.lib->proxy.colorpicker.selected_sample;
  const gboolean only_selected_sample = _darkroom_is_only_selected_sample(is_primary_sample, selected_sample, darktable.lib->proxy.colorpicker.display_samples);
  
  for( ; samples; samples = g_slist_next(samples))
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(only_selected_sample && (sample != selected_sample))
      continue;

    // The picker is at the resolution of the preview pixelpipe. This
    // is width/2 of a preview-pipe pixel in (scaled) user space
    // coordinates. Use half pixel width so rounding to nearest device
    // pixel doesn't make uneven centering.
    double half_px = 0.5;
    const double min_half_px_device = 4.0;
    // FIXME: instead of going to all this effort to show how error-prone a preview pipe sample can be, just produce a better point sample
    gboolean show_preview_pixel_scale = TRUE;

    // overlays are aligned with pixels for a clean look
    if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
    {
      double x = sample->box[0] * wd, y = sample->box[1] * ht,
        w = sample->box[2] * wd, h = sample->box[3] * ht;
      cairo_user_to_device(cri, &x, &y);
      cairo_user_to_device(cri, &w, &h);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      w=round(w+0.5)-0.5;
      h=round(h+0.5)-0.5;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user(cri, &w, &h);
      cairo_rectangle(cri, x, y, w - x, h - y);
      if(is_primary_sample)
      {
        // handles
        const double hw = 5. / scale;
        cairo_rectangle(cri, x - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, x - hw, h - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, y - hw, 2. * hw, 2. * hw);
        cairo_rectangle(cri, w - hw, h - hw, 2. * hw, 2. * hw);
      }
    }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      // FIXME: to be really accurate, the colorpicker should render precisely over the nearest pixelpipe pixel, but this gets particularly tricky to do with iop pickers with transformations after them in the pipeline
      double x = sample->point[0] * wd;
      double y = sample->point[1] * ht;
      cairo_user_to_device(cri, &x, &y);
      x=round(x+0.5)-0.5;
      y=round(y+0.5)-0.5;
      // render picker center a reasonable size in device pixels
      half_px = round(half_px * scale);
      if(half_px < min_half_px_device)
      {
        half_px = min_half_px_device;
        show_preview_pixel_scale = FALSE;
      }
      // crosshair radius
      double crosshair = (is_primary_sample ? 4. : 5.) * half_px;
      if(sample == selected_sample) crosshair *= 2;
      cairo_device_to_user(cri, &x, &y);
      cairo_device_to_user_distance(cri, &crosshair, &half_px);

      // "handles"
      if(is_primary_sample)
        cairo_arc(cri, x, y, crosshair, 0., 2. * M_PI);
      // crosshair
      cairo_move_to(cri, x - crosshair, y);
      cairo_line_to(cri, x + crosshair, y);
      cairo_move_to(cri, x, y - crosshair);
      cairo_line_to(cri, x, y + crosshair);
    }

    // default is to draw 1 (logical) pixel light lines with 1
    // (logical) pixel dark outline for legibility
    const double line_scale = (sample == selected_sample ? 2.0 : 1.0);
    cairo_set_line_width(cri, lw * 3.0 * line_scale);
    cairo_set_source_rgba(cri, 0.0, 0.0, 0.0, 0.4);
    cairo_stroke_preserve(cri);

    const gboolean draw_dashed = !is_primary_sample
                   && sample != selected_sample
                   && sample->size == DT_LIB_COLORPICKER_SIZE_BOX;
    cairo_set_line_width(cri, lw * line_scale);
    cairo_set_dash(cri, dashes, draw_dashed, 0.0);

    cairo_set_source_rgba(cri, 1.0, 1.0, 1.0, 0.8);
    cairo_stroke(cri);

    // draw the actual color sampled
    // FIXME: if an area sample is selected, when selected should fill it with colorpicker color?
    // NOTE: The sample may be based on outdated data, but still
    // display as it will update eventually. If we only drew on valid
    // data, swatches on point live samples would flicker when the
    // primary sample was drawn, and the primary sample swatch would
    // flicker when an iop is adjusted.
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      if(sample == selected_sample)
        cairo_arc(cri, sample->point[0] * wd, sample->point[1] * ht, half_px * 2., 0., 2. * M_PI);
      else if(show_preview_pixel_scale)
        cairo_rectangle(cri, sample->point[0] * wd - half_px, sample->point[1] * ht - half_px, half_px * 2., half_px * 2.);
      else
        cairo_arc(cri, sample->point[0] * wd, sample->point[1] * ht, half_px, 0., 2. * M_PI);

      set_color(cri, sample->swatch);
      cairo_fill(cri);
    }
  }

  cairo_restore(cri);
}

void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
  cmsDoTransform(darktable.color_profiles->transform_xyz_to_display, XYZ, RGB, 1);
}

static void _render_iso12646(cairo_t *cr, int width, int height, int border)
{
  // draw the white frame around picture
  cairo_rectangle(cr, -border * .5f, -border * .5f, width + border, height + border);
  cairo_set_source_rgb(cr, 1., 1., 1.);
  cairo_fill(cr);
}

/* Debug-only darkroom expose mode:
 * - no persistent surface locks,
 * - no fallback cache,
 * - no preview substitution,
 * - only main backbuffer if currently available.
 *
 * Set to 1 for baseline debugging when troubleshooting display regressions. */
#ifndef DARKROOM_EXPOSE_DUMB_DEBUG
#define DARKROOM_EXPOSE_DUMB_DEBUG 0
#endif

#if DARKROOM_EXPOSE_DUMB_DEBUG
static gboolean _render_main_direct_debug(cairo_t *cr, dt_develop_t *dev, const int width, const int height,
                                          const int border, const dt_aligned_pixel_t bg_color)
{
  if(!cr || !dev || !dev->pipe) return FALSE;

  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  if(!dt_dev_pixelpipe_is_backbufer_valid(dev->pipe, dev)) return FALSE;
  const uint64_t hash = dt_dev_backbuf_get_hash(&dev->pipe->backbuf);
  if(hash == (uint64_t)-1) return FALSE;

  dt_pixel_cache_entry_t *entry = NULL;
  void *data = dt_dev_pixelpipe_cache_get_ref_unlocked(darktable.pixelpipe_cache, hash, &entry);
  if(!data || !entry)
  {
    if(entry) dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    return FALSE;
  }

  const int bw = (int)dev->pipe->backbuf.width;
  const int bh = (int)dev->pipe->backbuf.height;
  if(bw <= 0 || bh <= 0)
  {
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    return FALSE;
  }

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, bw);
  const size_t required_size = (size_t)stride * (size_t)bh;
  const size_t entry_size = dt_pixel_cache_entry_get_size(entry);
  if(entry_size < required_size || dt_pixel_cache_entry_get_data(entry) != data)
  {
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    return FALSE;
  }

  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)data, CAIRO_FORMAT_RGB24,
                                                                  bw, bh, stride);
  if(!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    if(surface) cairo_surface_destroy(surface);
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    return FALSE;
  }

  int wd = bw / darktable.gui->ppd;
  int ht = bh / darktable.gui->ppd;
  cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));
  if(dev->iso_12646.enabled) _render_iso12646(cr, wd, ht, border);
  cairo_surface_set_device_scale(surface, darktable.gui->ppd, darktable.gui->ppd);
  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_fill(cr);

  cairo_surface_destroy(surface);
  dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
  return TRUE;
}
#endif

typedef struct darkroom_locked_surface_t
{
  uint64_t hash;
  int width;
  int height;
  void *data;
  struct dt_pixel_cache_entry_t *entry;
  cairo_surface_t *surface;
} darkroom_locked_surface_t;

static darkroom_locked_surface_t _darkroom_main_locked = { .hash = (uint64_t)-1 };
static darkroom_locked_surface_t _darkroom_preview_locked = { .hash = (uint64_t)-1 };
static cairo_surface_t *_darkroom_preview_fallback_surface = NULL;
static int32_t _darkroom_preview_fallback_imgid = UNKNOWN_IMAGE;
static uint64_t _darkroom_preview_fallback_zoom_hash = 0;
static uint64_t _darkroom_preview_fallback_backbuf_hash = 0;
static int _darkroom_preview_fallback_width = 0;
static int _darkroom_preview_fallback_height = 0;

static void _release_locked_surface(darkroom_locked_surface_t *locked)
{
  if(!locked) return;

  if(locked->surface)
  {
    cairo_surface_destroy(locked->surface);
    locked->surface = NULL;
  }

  if(locked->entry && locked->hash != (uint64_t)-1)
  {
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, locked->hash, locked->entry);
  }

  locked->entry = NULL;
  locked->data = NULL;
  locked->hash = (uint64_t)-1;
  locked->width = 0;
  locked->height = 0;
}

static void _release_preview_fallback_surface(void)
{
  if(_darkroom_preview_fallback_surface)
  {
    cairo_surface_destroy(_darkroom_preview_fallback_surface);
    _darkroom_preview_fallback_surface = NULL;
  }

  _darkroom_preview_fallback_imgid = UNKNOWN_IMAGE;
  _darkroom_preview_fallback_zoom_hash = 0;
  _darkroom_preview_fallback_backbuf_hash = 0;
  _darkroom_preview_fallback_width = 0;
  _darkroom_preview_fallback_height = 0;
}

static void _release_expose_source_caches(void)
{
  _release_locked_surface(&_darkroom_main_locked);
  _release_locked_surface(&_darkroom_preview_locked);
  _release_preview_fallback_surface();
}

static gboolean _lock_pipe_surface(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, darkroom_locked_surface_t *locked,
                                   const gboolean keep_previous_on_fail, const gboolean lock_read)
{
  if(!dev || !pipe || !locked) return FALSE;
  (void)lock_read;

  const uint64_t hash = dt_dev_backbuf_get_hash(&pipe->backbuf);
  if(hash == (uint64_t)-1) return keep_previous_on_fail && (locked->surface != NULL);

  /* Fast-path reuse is only valid if the cacheline identity/data pointer are
   * still the same behind the hash key. This avoids reusing stale cairo views
   * when an entry is recreated or replaced under the same key. */
  dt_pixel_cache_entry_t *live_entry = NULL;
  void *live_data = NULL;
  if(locked->surface && locked->hash == hash
     && dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, hash, &live_data, NULL, &live_entry)
     && live_entry == locked->entry && live_data == locked->data)
  {
    locked->width = pipe->backbuf.width;
    locked->height = pipe->backbuf.height;
    return TRUE;
  }

  struct dt_pixel_cache_entry_t *entry = NULL;
  /* Never keep long-lived read locks on cache-backed GUI surfaces.
   * Recomputing the same image can reuse the same cache hash and needs a write
   * lock on that entry; if darkroom still holds a read lock on the old surface,
   * the worker can block forever waiting for the GUI to replace the surface. */
  void *data = dt_dev_pixelpipe_cache_get_ref_unlocked(darktable.pixelpipe_cache, hash, &entry);
  if(!data)
  {
    /* Keep previous frame only while waiting for a *different* target hash.
     * If requested hash equals the currently locked one but cache lookup fails,
     * the cached line was likely flushed/invalidated: drop stale lock so the
     * line can be recreated and displayed again. */
    if(keep_previous_on_fail && locked->surface && locked->hash != hash) return TRUE;
    _release_locked_surface(locked);
    return FALSE;
  }

  const int width = pipe->backbuf.width;
  const int height = pipe->backbuf.height;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  const size_t required_size = (size_t)stride * (size_t)height;
  const size_t entry_size = dt_pixel_cache_entry_get_size(entry);
  if(width <= 0 || height <= 0 || entry_size < required_size || dt_pixel_cache_entry_get_data(entry) != data)
  {
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    if(keep_previous_on_fail && locked->surface && locked->hash != hash) return TRUE;
    _release_locked_surface(locked);
    return FALSE;
  }

  if(locked->surface && locked->data == data && locked->width == width && locked->height == height)
  {
    if(locked->entry && locked->hash != (uint64_t)-1)
    {
      dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, locked->hash, locked->entry);
    }
    locked->hash = hash;
    locked->entry = entry;
    locked->data = data;
    return TRUE;
  }

  cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, width, height, stride);
  if(!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    if(surface) cairo_surface_destroy(surface);
    dt_dev_pixelpipe_cache_unref_unlocked(darktable.pixelpipe_cache, hash, entry);
    if(keep_previous_on_fail && locked->surface) return TRUE;
    _release_locked_surface(locked);
    return FALSE;
  }

  _release_locked_surface(locked);
  locked->hash = hash;
  locked->width = width;
  locked->height = height;
  locked->data = data;
  locked->entry = entry;
  locked->surface = surface;
  return TRUE;
}

static gboolean _render_main_locked_surface(cairo_t *cr, dt_develop_t *dev, darkroom_locked_surface_t *locked,
                                            const int width, const int height, const int border,
                                            const dt_aligned_pixel_t bg_color)
{
  if(!cr || !dev || !locked || !locked->surface) return FALSE;
  if(!locked->entry || locked->hash == (uint64_t)-1) return FALSE;

  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  int wd = locked->width;
  int ht = locked->height;
  if(wd <= 0 || ht <= 0) return FALSE;

  wd /= darktable.gui->ppd;
  ht /= darktable.gui->ppd;
  cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));

  if(dev->iso_12646.enabled) _render_iso12646(cr, wd, ht, border);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, locked->hash, TRUE, locked->entry);
  cairo_surface_set_device_scale(locked->surface, darktable.gui->ppd, darktable.gui->ppd);
  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_set_source_surface(cr, locked->surface, 0, 0);
  cairo_fill(cr);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, locked->hash, FALSE, locked->entry);

  return TRUE;
}

static gboolean _build_preview_fallback_surface(dt_develop_t *dev, const int width, const int height, const int border,
                                                const dt_aligned_pixel_t bg_color, const uint64_t zoom_hash)
{
  if(!_darkroom_preview_locked.surface) return FALSE;
  if(!_darkroom_preview_locked.entry || _darkroom_preview_locked.hash == (uint64_t)-1) return FALSE;
  if(width <= 0 || height <= 0) return FALSE;

  if(!_darkroom_preview_fallback_surface
     || _darkroom_preview_fallback_width != width
     || _darkroom_preview_fallback_height != height)
  {
    _release_preview_fallback_surface();
    _darkroom_preview_fallback_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if(!_darkroom_preview_fallback_surface) return FALSE;
    _darkroom_preview_fallback_width = width;
    _darkroom_preview_fallback_height = height;
  }

  cairo_t *cr = cairo_create(_darkroom_preview_fallback_surface);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);

  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  const int wd = _darkroom_preview_locked.width;
  const int ht = _darkroom_preview_locked.height;

  if(dev->iso_12646.enabled)
  {
    // Preview backbuffer is unclipped: draw ISO frame over the ROI-projected region.
    const float scale = dt_dev_get_fit_scale(dev);
    const float roi_wd = fminf(wd * scale, dev->roi.width);
    const float roi_ht = fminf(ht * scale, dev->roi.height);

    cairo_save(cr);
    cairo_translate(cr, .5f * (width - roi_wd), .5f * (height - roi_ht));
    _render_iso12646(cr, roi_wd, roi_ht, border);
    cairo_restore(cr);
  }

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, _darkroom_preview_locked.hash, TRUE,
                                      _darkroom_preview_locked.entry);
  cairo_surface_set_device_scale(_darkroom_preview_locked.surface, 1., 1.);
  dt_dev_clip_roi(dev, cr, width, height);
  dt_dev_rescale_roi(dev, cr, width, height);
  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_set_source_surface(cr, _darkroom_preview_locked.surface, 0, 0);
  cairo_fill(cr);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, _darkroom_preview_locked.hash, FALSE,
                                      _darkroom_preview_locked.entry);
  cairo_destroy(cr);

  _darkroom_preview_fallback_imgid = dev->image_storage.id;
  _darkroom_preview_fallback_zoom_hash = zoom_hash;
  _darkroom_preview_fallback_backbuf_hash = dt_dev_backbuf_get_hash(&dev->preview_pipe->backbuf);
  return TRUE;
}

static gboolean _render_preview_fallback_surface(cairo_t *cr)
{
  if(!cr || !_darkroom_preview_fallback_surface) return FALSE;
  cairo_set_source_surface(cr, _darkroom_preview_fallback_surface, 0, 0);
  cairo_paint(cr);
  return TRUE;
}

static inline gboolean _darkroom_gui_module_requests_uncropped_full_image(const dt_develop_t *dev)
{
  return dev && dev->gui_attached && dev->gui_module && dt_iop_get_cache_bypass(dev->gui_module);
}

static void _paint_all(cairo_t *cri, cairo_t *cr, cairo_surface_t *image_surface)
{
  cairo_destroy(cr);
  if(!image_surface) return;
  cairo_set_source_surface(cri, image_surface, 0, 0);
  cairo_paint(cri);
}

typedef struct darkroom_expose_state_t
{
  int image_surface_width;
  int image_surface_height;
  int32_t image_surface_imgid;
  gboolean image_surface_has_main;
  uint64_t main_zoom_hash;
  uint64_t main_hash;
} darkroom_expose_state_t;

static inline gboolean _darkroom_preview_fallback_valid(const dt_develop_t *dev, const int width,
                                                        const int height, const uint64_t zoom_hash)
{
  return dev
         && _darkroom_preview_fallback_surface
         && _darkroom_preview_fallback_imgid == dev->image_storage.id
         && _darkroom_preview_fallback_zoom_hash == zoom_hash
         && _darkroom_preview_fallback_width == width
         && _darkroom_preview_fallback_height == height;
}

static inline gboolean _darkroom_locked_main_valid_for_zoom(const darkroom_expose_state_t *state,
                                                            const uint64_t zoom_hash)
{
  return state
         && _darkroom_main_locked.surface
         && _darkroom_main_locked.hash != (uint64_t)-1
         && state->main_zoom_hash == zoom_hash;
}

static inline void _darkroom_reset_expose_state(darkroom_expose_state_t *state)
{
  if(!state) return;
  state->image_surface_imgid = UNKNOWN_IMAGE;
  state->image_surface_has_main = FALSE;
  state->main_zoom_hash = 0;
  state->main_hash = 0;
}

static void _darkroom_prepare_image_surface(dt_develop_t *dev, const int width, const int height,
                                            darkroom_expose_state_t *state)
{
  if(!dev || !state) return;

  if(state->image_surface_imgid != dev->image_storage.id
     && state->image_surface_imgid != UNKNOWN_IMAGE)
  {
    _release_expose_source_caches();
    _darkroom_reset_expose_state(state);
  }

  if(state->image_surface_width == width
     && state->image_surface_height == height
     && dev->image_surface != NULL)
    return;

  state->image_surface_width = width;
  state->image_surface_height = height;
  if(dev->image_surface) cairo_surface_destroy(dev->image_surface);
  dev->image_surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  state->image_surface_imgid = UNKNOWN_IMAGE;
  state->image_surface_has_main = FALSE;
  _release_preview_fallback_surface();
}

void expose(
    dt_view_t *self,
    cairo_t *cri,
    int32_t width,
    int32_t height,
    int32_t pointerx,
    int32_t pointery)
{
  dt_times_t start;
  dt_get_times(&start);

  cairo_save(cri);

  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_dev_start_all_pipelines(dev);

  const int32_t border = dev->roi.border_size;

#if DARKROOM_EXPOSE_DUMB_DEBUG
  dt_aligned_pixel_t bg_color_dbg;
  if(dev->iso_12646.enabled)
    _colormanage_ui_color(50., 0., 0., bg_color_dbg);
  else
    _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color_dbg);
  _render_main_direct_debug(cri, dev, width, height, border, bg_color_dbg);
  cairo_restore(cri);
  return;
#endif
  static darkroom_expose_state_t expose_state = {
    .image_surface_width = 0,
    .image_surface_height = 0,
    .image_surface_imgid = UNKNOWN_IMAGE,
    .image_surface_has_main = FALSE,
    .main_zoom_hash = 0,
    .main_hash = 0,
  };
  const uint64_t zoom_hash = dt_hash(5381, (char *)&dev->roi, sizeof(dev->roi));
  const gboolean allow_uncropped_full_image = _darkroom_gui_module_requests_uncropped_full_image(dev);
  const gboolean roi_changed = !_darkroom_locked_main_valid_for_zoom(&expose_state, zoom_hash);

  _darkroom_prepare_image_surface(dev, width, height, &expose_state);

  cairo_t *cr = cairo_create(dev->image_surface);
  const int full_width = (int)lround((double)dev->roi.preview_width * darktable.gui->ppd);
  const int full_height = (int)lround((double)dev->roi.preview_height * darktable.gui->ppd);
  const uint64_t main_backbuf_hash = dt_dev_backbuf_get_hash(&dev->pipe->backbuf);
  const uint64_t preview_backbuf_hash = dt_dev_backbuf_get_hash(&dev->preview_pipe->backbuf);
  const gboolean main_has_backbuf = main_backbuf_hash != DT_PIXELPIPE_CACHE_HASH_INVALID;
  const gboolean preview_has_backbuf = preview_backbuf_hash != DT_PIXELPIPE_CACHE_HASH_INVALID;
  const gboolean main_backbuf_is_newer = main_has_backbuf && main_backbuf_hash != expose_state.main_hash;
  const gboolean main_ready_for_current_view
      = main_has_backbuf && (!roi_changed || main_backbuf_is_newer || !_darkroom_main_locked.surface);
  const gboolean showing_full_image = allow_uncropped_full_image || fabsf(dev->roi.scaling - 1.0f) < 1e-6f;
  const gboolean preview_matches_full_image
      = preview_has_backbuf && showing_full_image
        && (allow_uncropped_full_image
            || (full_width > 0 && full_height > 0
                && dev->preview_pipe->backbuf.width == full_width
                && dev->preview_pipe->backbuf.height == full_height));
  const gboolean full_image_backbuf_ready = main_ready_for_current_view || preview_matches_full_image;

  dt_aligned_pixel_t bg_color = { 0.0f };
  const char *draw_source = "background only";
  uint64_t draw_hash = (uint64_t)-1;
  gboolean drawn = FALSE;
  gboolean drawn_from_main = FALSE;

  // user param is Lab lightness/brightness (which is they same for greys)
  if(dev->iso_12646.enabled)
    _colormanage_ui_color(50., 0., 0., bg_color);
  else
    _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);

  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);

  /* Selection policy, kept intentionally linear:
   * 1. Prefer the main pipe whenever it already has the backbuf for the
   *    current darkroom view. We do not guess that from size. If ROI did not
   *    change, the existing main frame stays authoritative. If ROI changed, a
   *    new main backbuf hash means the updated main frame arrived.
   * 2. If preview produced the exact same full-image buffer size first
   *    (zoom-to-fit / same ROI), treat it exactly like main.
   * 3. Only when zoom/pan changed and main has not caught up yet, use a scaled
   *    preview fallback surface. This gives immediate ROI feedback without
   *    discarding the last valid main image for pure history changes.
   * 4. If nothing newer is ready, keep showing the last valid composed frame to
   *    avoid flashing the background. */

  /* Once a full-image backbuf exists again, the ROI-scaled preview fallback is
   * obsolete. Keep main/preview selection simple and never let the fallback
   * outlive a valid full-image source. */
  if(full_image_backbuf_ready)
    _release_preview_fallback_surface();

  /* Rule 1: main wins whenever it is ready for the current view. During a
   * zoom/pan/widget-size transition, the previous main hash stays visible until
   * a different main hash is published; only then does main override preview
   * fallback again. */
  if(main_ready_for_current_view)
  {
    if(_lock_pipe_surface(dev, dev->pipe, &_darkroom_main_locked, FALSE, FALSE)
       && _darkroom_main_locked.surface
       && _render_main_locked_surface(cr, dev, &_darkroom_main_locked, width, height, border, bg_color))
    {
      expose_state.main_hash = _darkroom_main_locked.hash;
      expose_state.main_zoom_hash = zoom_hash;
      expose_state.image_surface_imgid = dev->image_storage.id;
      expose_state.image_surface_has_main = TRUE;
      _release_preview_fallback_surface();
      drawn = TRUE;
      drawn_from_main = TRUE;
      draw_source = "fresh main backbuf";
      draw_hash = _darkroom_main_locked.hash;
    }
    else if(main_backbuf_is_newer)
      dt_control_queue_redraw_center();
  }

  /* Rule 2: preview is directly equivalent to main only in full-image view
   * (fit scale / uncropped edit modes). Otherwise it must go through the
   * ROI-scaled fallback path below. */
  if(!drawn && preview_matches_full_image)
  {
    if(_lock_pipe_surface(dev, dev->preview_pipe, &_darkroom_preview_locked, FALSE, TRUE)
       && _darkroom_preview_locked.surface
       && _render_main_locked_surface(cr, dev, &_darkroom_preview_locked, width, height, border, bg_color))
    {
      expose_state.main_hash = _darkroom_preview_locked.hash;
      expose_state.main_zoom_hash = zoom_hash;
      expose_state.image_surface_imgid = dev->image_storage.id;
      expose_state.image_surface_has_main = TRUE;
      _release_preview_fallback_surface();
      drawn = TRUE;
      drawn_from_main = TRUE;
      draw_source = "fresh full-size preview backbuf";
      draw_hash = _darkroom_preview_locked.hash;
    }
  }

  /* Rule 3: scaled preview fallback is only for ROI changes. If the user just
   * zoomed/panned and main has not recomputed yet, rebuild a ROI-scaled preview
   * placeholder so navigation remains responsive. */
  if(!drawn && roi_changed && !full_image_backbuf_ready && preview_has_backbuf)
  {
    if(_lock_pipe_surface(dev, dev->preview_pipe, &_darkroom_preview_locked, TRUE, TRUE)
       && _build_preview_fallback_surface(dev, width, height, border, bg_color, zoom_hash)
       && _render_preview_fallback_surface(cr))
    {
      expose_state.image_surface_imgid = dev->image_storage.id;
      expose_state.image_surface_has_main = FALSE;
      drawn = TRUE;
      drawn_from_main = FALSE;
      draw_source = "fresh preview fallback";
      draw_hash = _darkroom_preview_fallback_backbuf_hash;
    }
  }

  /* Reuse the cached ROI-scaled preview fallback if it is still geometrically
   * valid for this zoom/pan. This avoids rebuilding it every expose while main
   * is still catching up. */
  if(!drawn && roi_changed && !full_image_backbuf_ready
     && _darkroom_preview_fallback_valid(dev, width, height, zoom_hash)
     && _render_preview_fallback_surface(cr))
  {
    expose_state.image_surface_imgid = dev->image_storage.id;
    expose_state.image_surface_has_main = FALSE;
    drawn = TRUE;
    drawn_from_main = FALSE;
    draw_source = "reused preview fallback";
    draw_hash = _darkroom_preview_fallback_backbuf_hash;
  }

  /* Rule 4: if no newer source is ready, keep the last valid main frame for
   * the same zoom/pan. History-only changes should not glitch to preview or
   * background while the new main backbuf is still being produced. */
  if(!drawn && _darkroom_locked_main_valid_for_zoom(&expose_state, zoom_hash)
     && _render_main_locked_surface(cr, dev, &_darkroom_main_locked, width, height, border, bg_color))
  {
    expose_state.image_surface_imgid = dev->image_storage.id;
    expose_state.image_surface_has_main = TRUE;
    drawn = TRUE;
    drawn_from_main = TRUE;
    draw_source = "reused last main backbuf";
    draw_hash = _darkroom_main_locked.hash;
  }

  if(drawn)
  {
    /* Persist the last composed frame so later exposes can reuse it if the
     * next requested source is temporarily unavailable. */
    expose_state.image_surface_has_main = drawn_from_main;
    _paint_all(cri, cr, dev->image_surface);
    dt_print(DT_DEBUG_DEV, "[darkroom] expose drew %s (backbuf hash=%" PRIu64 ")\n",
             draw_source, draw_hash);
    cairo_restore(cri);
  }
  else if(dev->image_surface && expose_state.image_surface_imgid == dev->image_storage.id)
  {
    /* No fresh source this time: repaint the last composed image surface
     * rather than clearing to background and waiting for another expose. */
    draw_source = expose_state.image_surface_has_main ? "reused last main surface"
                                                      : "reused last preview surface";
    draw_hash = expose_state.image_surface_has_main ? expose_state.main_hash
                                                    : _darkroom_preview_fallback_backbuf_hash;
    _paint_all(cri, cr, dev->image_surface);
    dt_print(DT_DEBUG_DEV, "[darkroom] expose drew %s (backbuf hash=%" PRIu64 ")\n",
             draw_source, draw_hash);
    cairo_restore(cri);
  }
  else
  {
    /* Cold-start / no valid frame cached anywhere yet. */
    cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
    cairo_paint(cr);
    _paint_all(cri, cr, dev->image_surface);
    dt_print(DT_DEBUG_DEV, "[darkroom] expose drew %s (backbuf hash=%" PRIu64 ")\n",
             draw_source, draw_hash);
    cairo_restore(cri);
  }

  /* check if we should create a snapshot of view */
  if(darktable.develop->proxy.snapshot.request)
  {
    /* reset the request */
    darktable.develop->proxy.snapshot.request = FALSE;

    /* validation of snapshot filename */
    g_assert(darktable.develop->proxy.snapshot.filename != NULL);

    /* Store current image surface to snapshot file.
       FIXME: add checks so that we don't make snapshots of preview pipe image surface.
    */
    const int fd = g_open(darktable.develop->proxy.snapshot.filename, O_CREAT | O_WRONLY | O_BINARY, 0600);
    cairo_surface_write_to_png_stream(dev->image_surface, _write_snapshot_data, GINT_TO_POINTER(fd));
    close(fd);
  }

  // Displaying sample areas if enabled
  if(darktable.lib->proxy.colorpicker.live_samples
     && (darktable.lib->proxy.colorpicker.display_samples
         || (darktable.lib->proxy.colorpicker.selected_sample &&
             darktable.lib->proxy.colorpicker.selected_sample != darktable.lib->proxy.colorpicker.primary_sample)))
  {
    _darkroom_pickers_draw(self, cri, width, height, pointerx, pointery, darktable.lib->proxy.colorpicker.live_samples, FALSE);
  }

  // draw guide lines if needed
  if(!dev->gui_module || !(dev->gui_module->flags() & IOP_FLAGS_GUIDES_SPECIAL_DRAW))
  {
    const float wd = dev->roi.preview_width;
    const float ht = dev->roi.preview_height;
    const float scaling = dt_dev_get_overlay_scale(dev);

    cairo_save(cri);
    // don't draw guides on image margins
    dt_dev_clip_roi(dev, cri, width, height);
    // place origin at top-left corner of image
    dt_dev_rescale_roi(dev, cri, width, height);

    // draw guides with backbuffer dimensions, positioning and scaling handled by transformations
    dt_guides_draw(cri, 0, 0, wd, ht, scaling);
    cairo_restore(cri);
  }

  // draw colorpicker for in focus module or execute module callback hook
  // FIXME: draw picker in gui_post_expose() hook in libs/colorpicker.c -- catch would be that live samples would appear over guides, softproof/gamut text overlay would be hidden by picker
  if(dt_iop_color_picker_is_visible(dev))
  {
    GSList samples = { .data = darktable.lib->proxy.colorpicker.primary_sample, .next = NULL };
    _darkroom_pickers_draw(self, cri, width, height, pointerx, pointery, &samples, TRUE);
  }
  else
  {
    // display mask if we have a current module activated or if the masks manager module is expanded
    const gboolean display_masks = (dev->gui_module && dev->gui_module->enabled)
                                 || dt_lib_gui_get_expanded(dt_lib_get_module("masks"));

    if(dt_masks_get_visible_form(dev) && display_masks)
      dt_masks_events_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
      
    // module
    if(dev->gui_module && dev->gui_module->enabled && dev->gui_module->gui_post_expose)
      dev->gui_module->gui_post_expose(dev->gui_module, cri, width, height, pointerx, pointery);
  }

  // indicate if we are in gamut check or softproof mode
  if(darktable.color_profiles->mode != DT_PROFILE_NORMAL)
  {
    gchar *label = darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK ? _("gamut check") : _("soft proof");
    cairo_set_source_rgba(cri, 0.5, 0.5, 0.5, 0.5);
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cri);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(20) * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, label, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cri, ink.height * 2, height - (ink.height * 3));
    pango_cairo_layout_path(cri, layout);
    cairo_set_source_rgb(cri, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cri);
    cairo_set_line_width(cri, 0.7);
    cairo_set_source_rgb(cri, 0.3, 0.3, 0.3);
    cairo_stroke(cri);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }

  dt_show_times_f(&start, "[darkroom]", "redraw");
}

void reset(dt_view_t *self)
{
  dt_dev_reset_roi((dt_develop_t *)self->data);
}

static void _darkroom_log_image_load_error(const int ret)
{
  switch(ret)
  {
    case DT_DEV_IMAGE_STORAGE_MIPMAP_NOT_FOUND:
      dt_control_log(_("Could not load the image source data."));
      break;
    case DT_DEV_IMAGE_STORAGE_DB_NOT_READ:
      dt_control_log(_("Could not read image information from the database."));
      break;
    default:
      dt_control_log(_("We could not load the image."));
      break;
  }
}

static gboolean _darkroom_attach_missing_iop_guis(dt_develop_t *dev)
{
  if(!dev) return FALSE;

  gboolean attached = FALSE;

  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

    if(!module || dt_iop_is_hidden(module) || module->expander) continue;

    if(!module->widget) dt_iop_gui_init(module);
    dt_iop_gui_set_expander(module);
    dt_iop_gui_update(module);
    attached = TRUE;
  }

  return attached;
}

static void _darkroom_image_load_job_state(dt_job_t *job, dt_job_state_t state)
{
  if(job != _darkroom_image_load_job) return;

  if(state == DT_JOB_STATE_CANCELLED)
  {
    _darkroom_image_load_active_request = 0;
  }
  if(state >= DT_JOB_STATE_FINISHED) _darkroom_image_load_job = NULL;
}

static int32_t _darkroom_image_load_job_run(dt_job_t *job)
{
  const dt_darkroom_image_load_job_t *params = dt_control_job_get_params(job);
  if(!params) return 0;

  dt_darkroom_image_load_result_t *loaded = g_new0(dt_darkroom_image_load_result_t, 1);
  loaded->imgid = params->imgid;

  int ret = DT_DEV_IMAGE_STORAGE_OK;
  if(params->imgid <= 0 || !darktable.mipmap_cache || !darktable.image_cache)
    ret = DT_DEV_IMAGE_STORAGE_DB_NOT_READ;

  dt_mipmap_buffer_t buf = { 0 };
  if(ret == DT_DEV_IMAGE_STORAGE_OK)
  {
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, params->imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
    if(buf.buf != NULL && buf.width != 0 && buf.height != 0)
    {
      loaded->raw_width = buf.width;
      loaded->raw_height = buf.height;
      loaded->raw_inited = TRUE;
    }
    else
    {
      ret = DT_DEV_IMAGE_STORAGE_MIPMAP_NOT_FOUND;
    }
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

  if(ret == DT_DEV_IMAGE_STORAGE_OK)
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, params->imgid, 'r');
    if(image)
    {
      loaded->image_storage = *image;
      dt_image_cache_read_release(darktable.image_cache, image);
    }
    else
    {
      ret = DT_DEV_IMAGE_STORAGE_DB_NOT_READ;
    }
  }

  if(!darktable.signals || !dt_control_running())
  {
    dt_free(loaded);
    return 0;
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_LOADED, params->request_id, (guint)ret, loaded);

  return 0;
}

static void _darkroom_cancel_image_load_job(void)
{
  if(!_darkroom_image_load_job) return;

  dt_control_job_cancel(_darkroom_image_load_job);
  _darkroom_image_load_job = NULL;
  _darkroom_image_load_active_request = 0;
  _darkroom_pending_focus_module = NULL;
}

static void _darkroom_start_image_load(const int32_t imgid)
{
  if(imgid <= UNKNOWN_IMAGE)
  {
    dt_control_log(_("No image to open !"));
    return;
  }

  _darkroom_cancel_image_load_job();

  dt_job_t *job = dt_control_job_create(&_darkroom_image_load_job_run, "darkroom load image %d", imgid);
  if(!job)
  {
    dt_control_log(_("We could not queue the image load."));
    return;
  }

  dt_darkroom_image_load_job_t *params = g_new0(dt_darkroom_image_load_job_t, 1);
  params->imgid = imgid;
  params->request_id = ++_darkroom_image_load_serial;

  _darkroom_image_load_active_request = params->request_id;
  _darkroom_image_load_job = job;

  dt_control_job_set_params_with_size(job, params, sizeof(*params), g_free);
  dt_control_job_set_state_callback(job, _darkroom_image_load_job_state);
  dt_control_job_add_progress(job, _("loading image"), TRUE);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
}

static void _darkroom_image_loaded_callback(gpointer instance, guint request_id, guint result, gpointer payload,
                                            gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_develop_t *dev = (dt_develop_t *)self->data;
  const dt_darkroom_image_load_result_t *loaded = (dt_darkroom_image_load_result_t *)payload;
  (void)instance;

  if(request_id == 0 || request_id != _darkroom_image_load_active_request) return;
  if(darktable.view_manager->current_view != self) return;

  _darkroom_image_load_active_request = 0;

  if(!loaded) return;

  if(result)
  {
    _darkroom_log_image_load_error((int)result);
    return;
  }

  dev->image_storage = loaded->image_storage;
  if(dev->gui_attached && loaded->raw_inited)
  {
    dev->roi.raw_width = loaded->raw_width;
    dev->roi.raw_height = loaded->raw_height;
    dev->roi.raw_inited = TRUE;
  }

  const int ret = dt_dev_load_image_finish(dev, loaded->imgid);
  if(ret)
  {
    _darkroom_log_image_load_error(ret);
    return;
  }

  darktable.develop->proxy.wb_coeffs[0] = 0.f;

#ifdef USE_LUA

  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "darkroom-image-loaded",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(dev->image_storage.id),
      LUA_ASYNC_DONE);

#endif

  // synch gui and flag pipe as dirty
  // this is done here and not in dt_read_history, as it would else be triggered before module->gui_init.
  // locks history mutex internally
  dt_dev_pop_history_items(dev);
  dt_dev_history_gui_update(dev);
  if(_darkroom_attach_missing_iop_guis(dev))
    dt_dev_history_gui_update(dev);

  dt_dev_pixelpipe_rebuild_all(dev);
  dt_dev_get_thumbnail_size(dev);

  if(_darkroom_pending_focus_module && g_list_find(dev->iop, _darkroom_pending_focus_module))
    dt_iop_request_focus(_darkroom_pending_focus_module);
  _darkroom_pending_focus_module = NULL;

  // Clean & Init the starting point of undo/redo
  dt_undo_clear(darktable.undo, DT_UNDO_DEVELOP);
  dt_dev_undo_start_record(dev);
  dt_dev_undo_end_record(dev);

  /* signal that darktable.develop is initialized and ready to be used */
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE);

  dt_image_check_camera_missing_sample(&dev->image_storage);

  // change active image for global actions (menu)
  dt_view_active_images_reset(FALSE);
  dt_view_active_images_add(dev->image_storage.id, FALSE);

  dt_control_queue_redraw_center();

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);

  dt_view_image_info_update(dev->image_storage.id);
}

int try_enter(dt_view_t *self)
{
  uint32_t num_selected = dt_selection_get_length(darktable.selection);
  int32_t imgid = dt_control_get_mouse_over_id();
  (void)self;

  _darkroom_pending_imgid = UNKNOWN_IMAGE;

  if(imgid != UNKNOWN_IMAGE)
  {
    ; // Needed to open image from filmstrip
  }
  else if(num_selected > 1)
  {
    dt_control_log(_("The current selection contains more than one image, which is ambiguous.\n"
                     "Select exactly one image to enter the darkroom."));
    return 1;
  }
  else if(num_selected == 0 && imgid == UNKNOWN_IMAGE)
  {
    dt_control_log(_("There is no image selected.\n"
                    "Select exactly one image to enter the darkroom."));
    return 1;
  }
  else
  {
    // Needed to open image at startup
    imgid = dt_selection_get_first_id(darktable.selection);
  }

  dt_view_active_images_reset(FALSE);

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("No image to open !"));
    return 1;
  }

  _darkroom_pending_imgid = imgid;
  return 0;
}

static void _dev_change_image(dt_view_t *self, int32_t imgid)
{
  // Lazy trick to cleanup, reset, reinit, reload everything without
  // having to duplicate most of (but not all) the code in leave(),
  // try_enter() and enter() : simulate a roundtrip through lighttable.
  // This way, all images are loaded through the same path, handled at an higher level.
  // It's more robust, although slightly slower than re-initing only what is needed.
  dt_view_manager_switch(darktable.view_manager, "lighttable");
  dt_control_set_mouse_over_id(imgid);
  dt_view_manager_switch(darktable.view_manager, "darkroom");
}

static void _view_darkroom_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid > UNKNOWN_IMAGE)
  {
    // switch images in darkroom mode:
    _dev_change_image(user_data, imgid);
  }
}

/** toolbar buttons */

static gboolean _toolbar_show_popup(gpointer user_data)
{
  GtkPopover *popover = GTK_POPOVER(user_data);

  GtkWidget *button = gtk_popover_get_relative_to(popover);
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

  int x, y;
  GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
  gpointer   pointer_widget = NULL;
  if(pointer_window)
    gdk_window_get_user_data(pointer_window, &pointer_widget);

  GdkRectangle rect = { gtk_widget_get_allocated_width(button) / 2, 0, 1, 1 };

  if(pointer_widget && button != pointer_widget)
    gtk_widget_translate_coordinates(pointer_widget, button, x, y, &rect.x, &rect.y);

  gtk_popover_set_pointing_to(popover, &rect);

  // for the guides popover, it need to be updated before we show it
  if(darktable.view_manager && GTK_WIDGET(popover) == darktable.view_manager->guides_popover)
    dt_guides_update_popover_values();

  gtk_widget_show_all(GTK_WIDGET(popover));

  // cancel glib timeout if invoked by long button press
  return FALSE;
}

static void _get_final_size_with_iso_12646(dt_develop_t *d)
{
  if(d->iso_12646.enabled)
  {
    // For ISO 12646, we want portraits and landscapes to cover roughly the same surface
    // no matter the size of the widget. Meaning we force them to fit a square
    // of length matching the smaller widget dimension. The goal is to leave
    // a consistent perceptual impression between pictures, independent from orientation.
    const int main_dim = MIN(d->roi.orig_width, d->roi.orig_height);
    d->roi.border_size = 0.125 * main_dim;
  }
  else
  {
    d->roi.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  dt_dev_configure(d, d->roi.orig_width - 2 * d->roi.border_size, d->roi.orig_height - 2 * d->roi.border_size);
}

/* colour assessment */
static void _iso_12646_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if (!d->gui_attached) return;

  d->iso_12646.enabled = !d->iso_12646.enabled;
  _get_final_size_with_iso_12646(d);

  // This is already called in _get_final_size_... but it's not enough
  dt_control_queue_redraw_center();
}

/* overlay color */
static void _guides_quickbutton_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_guides_button_toggled(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  dt_control_queue_redraw_center();
}

static void _guides_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, dt_lib_module_t *self)
{
  dt_guides_update_button_state();
}

/**
 * DOC
 * Overexposed and gamut modules are inserted inplace in pipeline at runtime,
 * only for the main preview, and don't add history items.
 * They all need a full history -> pipeline resynchronization.
 */

/* display */
static void _display_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
}

static void display_brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_conf_set_int("display/brightness", (int)(dt_bauhaus_slider_get(slider)));
  dt_control_queue_redraw_center();
}

static void display_borders_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  dt_conf_set_int("plugins/darkroom/ui/border_size", (int)dt_bauhaus_slider_get(slider));
  _get_final_size_with_iso_12646(d);
  dt_dev_pixelpipe_change_zoom_main(d);
}

/* overexposed */
static void _overexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.enabled = !d->overexposed.enabled;
  dt_dev_pixelpipe_resync_history_main(d);
}

static void colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

static void lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

static void upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

static void mode_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->overexposed.mode = dt_bauhaus_combobox_get(slider);
  if(d->overexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->overexposed.button));

  dt_dev_pixelpipe_update_history_main(d);
}

/* rawoverexposed */
static void _rawoverexposed_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.enabled = !d->rawoverexposed.enabled;
  dt_dev_pixelpipe_resync_history_main(d);
}

static void rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

static void rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

static void rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  d->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);
  if(d->rawoverexposed.enabled == FALSE)
    gtk_button_clicked(GTK_BUTTON(d->rawoverexposed.button));

  dt_dev_pixelpipe_resync_history_main(d);
}

/* softproof */
static void _softproof_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_SOFTPROOF;

  _update_softproof_gamut_checking(d);
  dt_dev_pixelpipe_resync_history_main(d);
}

/* gamut */
static void _gamut_quickbutton_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  if(darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK)
    darktable.color_profiles->mode = DT_PROFILE_NORMAL;
  else
    darktable.color_profiles->mode = DT_PROFILE_GAMUTCHECK;

  _update_softproof_gamut_checking(d);

  dt_dev_pixelpipe_resync_history_main(d);
}

/* set the gui state for both softproof and gamut checking */
static void _update_softproof_gamut_checking(dt_develop_t *d)
{
  g_signal_handlers_block_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_block_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.softproof_button), darktable.color_profiles->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->profile.gamut_button), darktable.color_profiles->mode == DT_PROFILE_GAMUTCHECK);

  g_signal_handlers_unblock_by_func(d->profile.softproof_button, _softproof_quickbutton_clicked, d);
  g_signal_handlers_unblock_by_func(d->profile.gamut_button, _gamut_quickbutton_clicked, d);
}


static void softproof_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *d = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->out_pos == pos)
    {
      if(darktable.color_profiles->softproof_type != pp->type
        || (darktable.color_profiles->softproof_type == DT_COLORSPACE_FILE
            && strcmp(darktable.color_profiles->softproof_filename, pp->filename)))

      {
        darktable.color_profiles->softproof_type = pp->type;
        g_strlcpy(darktable.color_profiles->softproof_filename, pp->filename,
                  sizeof(darktable.color_profiles->softproof_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to sRGB. shouldn't happen
  fprintf(stderr, "can't find softproof profile `%s', using sRGB instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->softproof_type != DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_type = DT_COLORSPACE_SRGB;
  darktable.color_profiles->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_pixelpipe_resync_history_main(d);
  }
}

/** end of toolbox */

#if 0

static void _overlay_cycle_callback(dt_action_t *action)
{
  const int currentval = dt_conf_get_int("darkroom/ui/overlay_color");
  const int nextval = (currentval + 1) % 6; // colors can go from 0 to 5
  dt_conf_set_int("darkroom/ui/overlay_color", nextval);
  dt_guides_set_overlay_colors();
  dt_control_queue_redraw_center();
}

static void _toggle_mask_visibility_callback(dt_action_t *action)
{
  if(darktable.gui->reset) return;

  dt_develop_t *dev = dt_action_view(action)->data;
  dt_iop_module_t *mod = dev->gui_module;

  //retouch and spot removal module use masks differently and have different buttons associated
  //keep the shortcuts independent
  if(mod && strcmp(mod->so->op, "spots") != 0 && strcmp(mod->so->op, "retouch") != 0)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)mod->blend_data;

    ++darktable.gui->reset;

    dt_iop_color_picker_reset(mod, TRUE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, mod->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      if(bd->masks_shown == DT_MASKS_EDIT_OFF)
        bd->masks_shown = DT_MASKS_EDIT_FULL;
      else
        bd->masks_shown = DT_MASKS_EDIT_OFF;

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
      dt_masks_set_edit_mode(mod, bd->masks_shown);

      // set all add shape buttons to inactive
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
    }

    --darktable.gui->reset;
  }
}


#endif

static gboolean _quickbutton_press_release(GtkWidget *button, GdkEventButton *event, GtkWidget *popover)
{
  static guint start_time = 0;

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS && event->button == 3) ||
     (event->type == GDK_BUTTON_RELEASE && event->time - start_time > delay))
  {
    gtk_popover_set_relative_to(GTK_POPOVER(popover), button);

    g_object_set(G_OBJECT(popover), "transitions-enabled", FALSE, NULL);

    _toolbar_show_popup(popover);
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

void connect_button_press_release(GtkWidget *w, GtkWidget *p)
{
  g_signal_connect(w, "button-press-event", G_CALLBACK(_quickbutton_press_release), p);
  g_signal_connect(w, "button-release-event", G_CALLBACK(_quickbutton_press_release), p);
}

gboolean _focus_main_image(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                           GdkModifierType modifier, gpointer data)
{
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  return TRUE;
}

gboolean _switch_to_next_picture(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  dt_view_t *view = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)view->data;
  int32_t current_img = dev->image_storage.id;
  GList *current_collection = dt_collection_get_all(darktable.collection, -1);
  GList *current_item = g_list_find(current_collection, GINT_TO_POINTER(current_img));

  if(current_item && current_item->next)
  {
    int32_t next_img = GPOINTER_TO_INT(current_item->next->data);
    g_list_free(current_collection);
    current_collection = NULL;
    _dev_change_image(data, next_img);
  }
  else
  {
    g_list_free(current_collection);
    current_collection = NULL;
  }

  return TRUE;
}

gboolean _switch_to_prev_picture(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  dt_view_t *view = (dt_view_t *)data;
  dt_develop_t *dev = (dt_develop_t *)view->data;
  int32_t current_img = dev->image_storage.id;
  GList *current_collection = dt_collection_get_all(darktable.collection, -1);
  GList *current_item = g_list_find(current_collection, GINT_TO_POINTER(current_img));

  if(current_item && current_item->prev)
  {
    int32_t prev_img = GPOINTER_TO_INT(current_item->prev->data);
    g_list_free(current_collection);
    current_collection = NULL;
    _dev_change_image(data, prev_img);
  }
  else
  {
    g_list_free(current_collection);
    current_collection = NULL;
  }

  return TRUE;
}


void gui_init(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  dt_accels_new_darkroom_action(_switch_to_next_picture, self, N_("Darkroom/Actions"),
                                N_("Switch to the next picture"), GDK_KEY_Right, GDK_MOD1_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_action(_switch_to_prev_picture, self, N_("Darkroom/Actions"),
                                N_("Switch to the previous picture"), GDK_KEY_Left, GDK_MOD1_MASK, _("Triggers the action"));

  gchar *path = dt_accels_build_path(_("Darkroom/Actions"), _("Give focus to the main image"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Return, 0);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move up"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Up, 0);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move up (coarse step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Up, GDK_SHIFT_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move up (fine step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Up, GDK_CONTROL_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move down"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Down, 0);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move down (coarse step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Down, GDK_SHIFT_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move down (fine step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Down, GDK_CONTROL_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move left"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Left, 0);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move left (coarse step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Left, GDK_SHIFT_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move left (fine step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Left, GDK_CONTROL_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move right"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Right, 0);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move right (coarse step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Right, GDK_SHIFT_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Move right (fine step)"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_Right, GDK_CONTROL_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Zoom in"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_plus, GDK_CONTROL_MASK);
  dt_free(path);

  path = dt_accels_build_path(_("Darkroom/Main image"), _("Zoom out"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->darkroom_accels,
                                 path, dt_ui_center(darktable.gui->ui), GDK_KEY_minus, GDK_CONTROL_MASK);
  dt_free(path);
  /*
   * Add view specific tool buttons
   */

  /* Enable ISO 12646-compliant colour assessment conditions */
  dev->iso_12646.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, 0, NULL);
  gtk_widget_set_tooltip_text(dev->iso_12646.button,
                              _("toggle ISO 12646 color assessment conditions"));
  g_signal_connect(G_OBJECT(dev->iso_12646.button), "clicked", G_CALLBACK(_iso_12646_quickbutton_clicked), dev);
  dt_view_manager_module_toolbox_add(darktable.view_manager, dev->iso_12646.button, DT_VIEW_DARKROOM);

  /* display background options */
  {
    dev->display.button = dtgtk_button_new(dtgtk_cairo_paint_display, 0, NULL);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->display.button, DT_VIEW_DARKROOM);
    gtk_widget_set_tooltip_text(dev->display.button, _("Picture display options"));
    g_signal_connect(G_OBJECT(dev->display.button), "clicked",
                     G_CALLBACK(_display_quickbutton_clicked), dev);

    // and the popup window
    dev->display.floating_window = gtk_popover_new(dev->display.button);
    connect_button_press_release(dev->display.button, dev->display.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->display.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    GtkWidget *brightness = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), 0, 100, 5, 50, 0);
    dt_bauhaus_slider_set(brightness, (int)dt_conf_get_int("display/brightness"));
    dt_bauhaus_widget_set_label(brightness, N_("Background brightness"));
    dt_bauhaus_slider_set_format(brightness, "%");
    g_signal_connect(G_OBJECT(brightness), "value-changed", G_CALLBACK(display_brightness_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(brightness), TRUE, TRUE, 0);

    GtkWidget *borders = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), 0, 250, 5, 10, 0);
    dt_bauhaus_slider_set(borders, dt_conf_get_int("plugins/darkroom/ui/border_size"));
    dt_bauhaus_widget_set_label(borders, N_("Picture margins"));
    dt_bauhaus_slider_set_format(borders, "px");
    g_signal_connect(G_OBJECT(borders), "value-changed", G_CALLBACK(display_borders_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(borders), TRUE, TRUE, 0);
  }

  GtkWidget *colorscheme, *mode;

  /* create rawoverexposed popup tool */
  {
    // the button
    dev->rawoverexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, 0, NULL);
    gtk_widget_set_tooltip_text(dev->rawoverexposed.button,
                                _("toggle raw over exposed indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->rawoverexposed.button), "clicked",
                     G_CALLBACK(_rawoverexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->rawoverexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->rawoverexposed.button, dt_get_help_url("rawoverexposed"));

    // and the popup window
    dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
    connect_button_press_release(dev->rawoverexposed.button, dev->rawoverexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* mode of operation */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(darktable.bauhaus, mode, NULL, N_("mode"),
                                 _("select how to mark the clipped pixels"),
                                 dev->rawoverexposed.mode, rawoverexposed_mode_callback, dev,
                                 N_("mark with CFA color"), N_("mark with solid color"), N_("false color"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    /* color scheme */
    // FIXME can't use DT_BAUHAUS_COMBOBOX_NEW_FULL because of (unnecessary?) translation context
    colorscheme = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
    dt_bauhaus_widget_set_label(colorscheme, N_("color scheme"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "red"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "green"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "blue"));
    dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "black"));
    dt_bauhaus_combobox_set(colorscheme, dev->rawoverexposed.colorscheme);
    gtk_widget_set_tooltip_text(
        colorscheme,
        _("select the solid color to indicate over exposure.\nwill only be used if mode = mark with solid color"));
    g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(rawoverexposed_colorscheme_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* threshold */
    GtkWidget *threshold = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), 0.0, 2.0, 0.01, 1.0, 3);
    dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
    dt_bauhaus_widget_set_label(threshold, N_("clipping threshold"));
    gtk_widget_set_tooltip_text(
        threshold, _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
    g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(rawoverexposed_threshold_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create overexposed popup tool */
  {
    // the button
    dev->overexposed.button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, 0, NULL);
    gtk_widget_set_tooltip_text(dev->overexposed.button,
                                _("toggle clipping indication\nright click for options"));
    g_signal_connect(G_OBJECT(dev->overexposed.button), "clicked",
                     G_CALLBACK(_overexposed_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->overexposed.button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->overexposed.button, dt_get_help_url("overexposed"));

    // and the popup window
    dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
    connect_button_press_release(dev->overexposed.button, dev->overexposed.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    /* preview mode */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(darktable.bauhaus, mode, NULL, N_("clipping preview mode"),
                                 _("select the metric you want to preview\nfull gamut is the combination of all other modes"),
                                 dev->overexposed.mode, mode_callback, dev,
                                 N_("full gamut"), N_("any RGB channel"), N_("luminance only"), N_("saturation only"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

    /* color scheme */
    DT_BAUHAUS_COMBOBOX_NEW_FULL(darktable.bauhaus, colorscheme, NULL, N_("color scheme"),
                                 _("select colors to indicate clipping"),
                                 dev->overexposed.colorscheme, colorscheme_callback, dev,
                                 N_("black & white"), N_("red & blue"), N_("purple & green"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

    /* lower */
    GtkWidget *lower = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), -32., -4., 1., -12.69, 2);
    dt_bauhaus_slider_set(lower, dev->overexposed.lower);
    dt_bauhaus_slider_set_format(lower, _(" EV"));
    dt_bauhaus_widget_set_label(lower, N_("lower threshold"));
    gtk_widget_set_tooltip_text(lower, _("clipping threshold for the black point,\n"
                                         "in EV, relatively to white (0 EV).\n"
                                         "8 bits sRGB clips blacks at -12.69 EV,\n"
                                         "8 bits Adobe RGB clips blacks at -19.79 EV,\n"
                                         "16 bits sRGB clips blacks at -20.69 EV,\n"
                                         "typical fine-art mat prints produce black at -5.30 EV,\n"
                                         "typical color glossy prints produce black at -8.00 EV,\n"
                                         "typical B&W glossy prints produce black at -9.00 EV."
                                         ));
    g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(lower_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

    /* upper */
    GtkWidget *upper = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL), 0.0, 100.0, 0.1, 99.99, 2);
    dt_bauhaus_slider_set(upper, dev->overexposed.upper);
    dt_bauhaus_slider_set_format(upper, "%");
    dt_bauhaus_widget_set_label(upper, N_("upper threshold"));
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(upper, _("clipping threshold for the white point.\n"
                                         "100% is peak medium luminance."));
    g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(upper_callback), dev);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);

    gtk_widget_show_all(vbox);
  }

  /* create profile popup tool & buttons (softproof + gamut) */
  {
    // the softproof button
    dev->profile.softproof_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, 0, NULL);
    gtk_widget_set_tooltip_text(dev->profile.softproof_button,
                                _("toggle softproofing\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.softproof_button), "clicked",
                     G_CALLBACK(_softproof_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.softproof_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.softproof_button, dt_get_help_url("softproof"));

    // the gamut check button
    dev->profile.gamut_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, 0, NULL);
    gtk_widget_set_tooltip_text(dev->profile.gamut_button,
                 _("toggle gamut checking\nright click for profile options"));
    g_signal_connect(G_OBJECT(dev->profile.gamut_button), "clicked",
                     G_CALLBACK(_gamut_quickbutton_clicked), dev);
    dt_view_manager_module_toolbox_add(darktable.view_manager, dev->profile.gamut_button, DT_VIEW_DARKROOM);
    dt_gui_add_help_link(dev->profile.gamut_button, dt_get_help_url("gamut"));

    // and the popup window, which is shared between the two profile buttons
    dev->profile.floating_window = gtk_popover_new(NULL);
    connect_button_press_release(dev->profile.softproof_button, dev->profile.floating_window);
    connect_button_press_release(dev->profile.gamut_button, dev->profile.floating_window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);

    /** let's fill the encapsulating widgets */
    char datadir[PATH_MAX] = { 0 };
    char confdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(confdir, sizeof(confdir));
    dt_loc_get_datadir(datadir, sizeof(datadir));

    GtkWidget *softproof_profile = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
    dt_bauhaus_widget_set_label(softproof_profile, N_("softproof profile"));
    dt_bauhaus_combobox_set_entries_ellipsis(softproof_profile, PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);

    for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
    {
      dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
      // the system display profile is only suitable for display purposes
      if(prof->out_pos > -1)
      {
        dt_bauhaus_combobox_add(softproof_profile, prof->name);
        if(prof->type == darktable.color_profiles->softproof_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->softproof_filename)))
          dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
      }
    }

    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("softproof ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(softproof_profile, tooltip);
    dt_free(tooltip);
    dt_free(system_profile_dir);
    dt_free(user_profile_dir);

    g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(softproof_profile_callback), dev);

    _update_softproof_gamut_checking(dev);

    gtk_widget_show_all(vbox);
  }

  /* create grid changer popup tool */
  {
    // the button
    darktable.view_manager->guides_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_grid, 0, NULL);
    gtk_widget_set_tooltip_text(darktable.view_manager->guides_toggle,
                                _("toggle guide lines\nright click for guides options"));
    darktable.view_manager->guides_popover = dt_guides_popover(self, darktable.view_manager->guides_toggle);
    g_object_ref(darktable.view_manager->guides_popover);
    g_signal_connect(G_OBJECT(darktable.view_manager->guides_toggle), "clicked",
                     G_CALLBACK(_guides_quickbutton_clicked), dev);
    connect_button_press_release(darktable.view_manager->guides_toggle, darktable.view_manager->guides_popover);
    dt_view_manager_module_toolbox_add(darktable.view_manager, darktable.view_manager->guides_toggle,
                                       DT_VIEW_DARKROOM);
    // we want to update button state each time the view change
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                                    G_CALLBACK(_guides_view_changed), dev);
  }

  darktable.view_manager->proxy.darkroom.get_layout = _lib_darkroom_get_layout;
  dev->roi.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
}

enum
{
  DND_TARGET_IOP,
};

/** drag and drop module list */
static const GtkTargetEntry _iop_target_list_internal[] = { { "iop", GTK_TARGET_SAME_WIDGET, DND_TARGET_IOP } };
static const guint _iop_n_targets_internal = G_N_ELEMENTS(_iop_target_list_internal);

static dt_iop_module_t *_get_dnd_dest_module(GtkBox *container, gint x, gint y, dt_iop_module_t *module_src)
{
  dt_iop_module_t *module_dest = NULL;

  GtkAllocation allocation_w = {0};
  gtk_widget_get_allocation(module_src->header, &allocation_w);
  const int y_slop = allocation_w.height / 2;
  // after source in pixelpipe, which is before it in the widgets list
  gboolean after_src = TRUE;

  GtkWidget *widget_dest = NULL;
  GList *children = gtk_container_get_children(GTK_CONTAINER(container));
  for(GList *l = children; l != NULL; l = g_list_next(l))
  {
    GtkWidget *w = GTK_WIDGET(l->data);
    if(w)
    {
      if(w == module_src->expander) after_src = FALSE;
      if(gtk_widget_is_visible(w))
      {
        gtk_widget_get_allocation(w, &allocation_w);
        // If dragging to later in the pixelpipe, we will insert after
        // the destination module. If dragging to earlier in the
        // pixelpipe, will insert before the destination module. This
        // results in two code paths here and in our caller, but can
        // handle all cases from inserting at the very start to the
        // very end.
        if((after_src && y <= allocation_w.y + y_slop) ||
           (!after_src && y <= allocation_w.y + allocation_w.height + y_slop))
        {
          widget_dest = w;
          break;
        }
      }
    }
  }
  g_list_free(children);
  children = NULL;

  if(widget_dest)
  {
    for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->expander == widget_dest)
      {
        module_dest = mod;
        break;
      }
    }
  }

  return module_dest;
}

static dt_iop_module_t *_get_dnd_source_module(GtkBox *container)
{
  dt_iop_module_t *module_source = NULL;
  gpointer *source_data = g_object_get_data(G_OBJECT(container), "source_data");
  if(source_data) module_source = (dt_iop_module_t *)source_data;

  return module_source;
}

// this will be used for a custom highlight, if ever implemented
static void _on_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
}

// FIXME: default highlight for the dnd is barely visible
// it should be possible to configure it
static void _on_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  if(module_src && module_src->expander)
  {
    GdkWindow *window = gtk_widget_get_parent_window(module_src->header);
    if(window)
    {
      GtkAllocation allocation_w = {0};
      gtk_widget_get_allocation(module_src->header, &allocation_w);
      // method from https://blog.gtk.org/2017/04/23/drag-and-drop-in-lists/
      cairo_surface_t *surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, allocation_w.width, allocation_w.height);
      cairo_t *cr = cairo_create(surface);

      // hack to render not transparent
      dt_gui_add_class(module_src->header, "iop_drag_icon");
      gtk_widget_draw(module_src->header, cr);
      dt_gui_remove_class(module_src->header, "iop_drag_icon");

      // FIXME: this centers the icon on the mouse -- instead translate such that the label doesn't jump when mouse down?
      cairo_surface_set_device_offset(surface, -allocation_w.width * darktable.gui->ppd / 2, -allocation_w.height * darktable.gui->ppd / 2);
      gtk_drag_set_icon_surface(context, surface);

      cairo_destroy(cr);
      cairo_surface_destroy(surface);
    }
  }
}

static void _on_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                              GtkSelectionData *selection_data, guint info, guint time,
                              gpointer user_data)
{
  gpointer *target_data = g_object_get_data(G_OBJECT(widget), "target_data");
  guint number_data = 0;
  if(target_data) number_data = GPOINTER_TO_UINT(target_data[DND_TARGET_IOP]);
  gtk_selection_data_set(selection_data, gdk_atom_intern("iop", TRUE), // type
                                        32,                            // format
                                        (guchar*)&number_data,         // data
                                        1);                            // length
}

static gboolean _on_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  GdkAtom target_atom = GDK_NONE;

  target_atom = gdk_atom_intern("iop", TRUE);

  gtk_drag_get_data(widget, dc, target_atom, time);

  return TRUE;
}

static gboolean _on_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  gboolean can_moved = FALSE;
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  if(!module_src) return FALSE;

  dt_iop_module_t *module_dest = _get_dnd_dest_module(container, x, y, module_src);

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      can_moved = dt_ioppr_check_can_move_after_iop(darktable.develop->iop, module_src, module_dest);
    else
      can_moved = dt_ioppr_check_can_move_before_iop(darktable.develop->iop, module_src, module_dest);
  }

  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  if(can_moved)
  {
    if(module_src->iop_order < module_dest->iop_order)
      dt_gui_add_class(module_dest->expander, "iop_drop_after");
    else
      dt_gui_add_class(module_dest->expander, "iop_drop_before");

    gdk_drag_status(dc, GDK_ACTION_COPY, time);
    GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
    if(w) gtk_drag_unhighlight(w);
    g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)module_dest->expander);
    gtk_drag_highlight(module_dest->expander);
  }
  else
  {
    gdk_drag_status(dc, 0, time);
    GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
    if(w)
    {
      gtk_drag_unhighlight(w);
      g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)FALSE);
    }
  }

  return can_moved;
}

static void _on_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                   GtkSelectionData *selection_data,
                                   guint info, guint time, gpointer user_data)
{
  int moved = 0;
  GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  dt_iop_module_t *module_src = _get_dnd_source_module(container);
  dt_iop_module_t *module_dest = _get_dnd_dest_module(container, x, y, module_src);

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
    {
      /* printf("[_on_drag_data_received] moving %s %s(%f) after %s %s(%f)\n",
          module_src->op, module_src->multi_name, module_src->iop_order,
          module_dest->op, module_dest->multi_name, module_dest->iop_order); */
      moved = dt_ioppr_move_iop_after(darktable.develop, module_src, module_dest);
    }
    else
    {
      /* printf("[_on_drag_data_received] moving %s %s(%f) before %s %s(%f)\n",
          module_src->op, module_src->multi_name, module_src->iop_order,
          module_dest->op, module_dest->multi_name, module_dest->iop_order); */
      moved = dt_ioppr_move_iop_before(darktable.develop, module_src, module_dest);
    }
  }
  else
  {
    if(module_src == NULL)
      fprintf(stderr, "[_on_drag_data_received] can't find source module\n");
    if(module_dest == NULL)
      fprintf(stderr, "[_on_drag_data_received] can't find destination module\n");
  }

  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  gtk_drag_finish(dc, TRUE, FALSE, time);

  if(moved)
  {
    // we move the headers
    GValue gv = { 0, { { 0 } } };
    g_value_init(&gv, G_TYPE_INT);
    gtk_container_child_get_property(
        GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), module_dest->expander,
        "position", &gv);
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
        module_src->expander, g_value_get_int(&gv));

    // we update the headers
    dt_dev_modules_update_multishow(module_src->dev);

    // add_history_item recomputes a pipeline
    // so we need to flag it for rebuild before
    dt_dev_pixelpipe_rebuild_all(module_src->dev);
    dt_dev_add_history_item(module_src->dev, module_src, TRUE, TRUE);

    dt_ioppr_check_iop_order(module_src->dev, 0, "_on_drag_data_received end");

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);
  }
}

static void _on_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data)
{
  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    if(module->expander)
    {
      dt_gui_remove_class(module->expander, "iop_drop_after");
      dt_gui_remove_class(module->expander, "iop_drop_before");
    }
  }

  GtkWidget *w = g_object_get_data(G_OBJECT(widget), "highlighted");
  if(w)
  {
    gtk_drag_unhighlight(w);
    g_object_set_data(G_OBJECT(widget), "highlighted", (gpointer)FALSE);
  }
}

static void _register_modules_drag_n_drop(dt_view_t *self)
{
  if(darktable.gui)
  {
    GtkWidget *container = GTK_WIDGET(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER));

    gtk_drag_source_set(container, GDK_BUTTON1_MASK | GDK_SHIFT_MASK, _iop_target_list_internal, _iop_n_targets_internal, GDK_ACTION_COPY);

    g_object_set_data(G_OBJECT(container), "targetlist", (gpointer)_iop_target_list_internal);
    g_object_set_data(G_OBJECT(container), "ntarget", GUINT_TO_POINTER(_iop_n_targets_internal));

    g_signal_connect(container, "drag-begin", G_CALLBACK(_on_drag_begin), NULL);
    g_signal_connect(container, "drag-data-get", G_CALLBACK(_on_drag_data_get), NULL);
    g_signal_connect(container, "drag-end", G_CALLBACK(_on_drag_end), NULL);

    gtk_drag_dest_set(container, 0, _iop_target_list_internal, _iop_n_targets_internal, GDK_ACTION_COPY);

    g_signal_connect(container, "drag-data-received", G_CALLBACK(_on_drag_data_received), NULL);
    g_signal_connect(container, "drag-drop", G_CALLBACK(_on_drag_drop), NULL);
    g_signal_connect(container, "drag-motion", G_CALLBACK(_on_drag_motion), NULL);
    g_signal_connect(container, "drag-leave", G_CALLBACK(_on_drag_leave), NULL);
  }
}

static void _unregister_modules_drag_n_drop(dt_view_t *self)
{
  if(darktable.gui)
  {
    gtk_drag_source_unset(dt_ui_center(darktable.gui->ui));

    GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);

    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_begin), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_data_get), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_end), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_data_received), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_drop), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_motion), NULL);
    g_signal_handlers_disconnect_matched(container, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_on_drag_leave), NULL);
  }
}

static gboolean _is_scroll_captured_by_widget()
{
  dt_accels_t *accels = darktable.gui->accels;
  if(!darktable.gui->has_scroll_focus || accels->active_key.accel_key == 0) return FALSE;

  // When declaring shortcuts, bauhaus widgets write their accel path into a private data field
  gchar *accel_path = g_object_get_data(G_OBJECT(darktable.gui->has_scroll_focus), "accel-path");

  // Find if the registered accel keys matches currently pressed keys
  GtkAccelKey key = { 0 };
  return gtk_accel_map_lookup_entry(accel_path, &key)
    && key.accel_key == accels->active_key.accel_key
    && key.accel_mods == accels->active_key.accel_mods;
}


// If a bauhaus widget has the scroll focus from a keyboard shortcut,
// and the combination of keys attached to its accel path
// is still pressed, then we redirect any scroll event in the window to this widget.
// Warning: if mouse is over the central widget, central widget takes precedence over scrolling.
gboolean _scroll_on_focus(GdkEventScroll event, void *data)
{
  if(_is_scroll_captured_by_widget())
  {
    // Pass-through the scrolling event to the scrolling handler of the widget
    gboolean ret;
    g_signal_emit_by_name(G_OBJECT(darktable.gui->has_scroll_focus), "scroll-event", &event, &ret);
    return ret;
  }

  return FALSE;
}


void enter(dt_view_t *self)
{
  // Flush all background jobs (thumbnails generation) to spare resources for interactivity
  dt_control_flush_jobs_queue(darktable.control, DT_JOB_QUEUE_SYSTEM_FG);
  
  dt_print(DT_DEBUG_CONTROL, "[run_job+] 11 %f in darkroom mode\n", dt_get_wtime());
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dev->exit = 0;
  _darkroom_pending_focus_module = NULL;

  // We need to init forms before we init module blending GUI
  dt_masks_gui_init(dev);
  dev->gui_module = NULL;

  if(!dev->iop)
    dev->iop = dt_dev_load_modules(dev);

  // Add IOP modules to the plugin list
  char option[1024];
  const char *active_plugin = dt_conf_get_string_const("plugins/darkroom/active");
  for(const GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    /* initialize gui if iop have one defined */
    if(!dt_iop_is_hidden(module))
    {
      dt_iop_gui_init(module);
      dt_iop_gui_set_expander(module);

      if(module->multi_priority == 0)
      {
        snprintf(option, sizeof(option), "plugins/darkroom/%s/expanded", module->op);
        module->expanded = dt_conf_get_bool(option);
        dt_iop_gui_update_expanded(module);

        if(active_plugin && !strcmp(module->op, active_plugin))
          _darkroom_pending_focus_module = module;
      }
    }
  }

  _register_modules_drag_n_drop(self);

  // just make sure at this stage we have only history info into the undo, all automatic
  // tagging should be ignored.
  dt_undo_clear(darktable.undo, DT_UNDO_TAGS);

  dt_iop_color_picker_init();

  // Reset focus to center view
  dt_gui_refocus_center();

  // Attach shortcuts to new widgets
  dt_accels_connect_accels(darktable.gui->accels);
  dt_accels_connect_active_group(darktable.gui->accels, "darkroom");
  dt_accels_attach_scroll_handler(darktable.gui->accels, _scroll_on_focus, dev);

  // Attach bauhaus default signal callback to IOP
  darktable.bauhaus->default_value_changed_callback = dt_bauhaus_value_changed_default_callback;

  // This gets the first selected ID to scroll where relevant, so
  // runs it before clearing the selection
  dt_thumbtable_show(darktable.gui->ui->thumbtable_filmstrip);
  gtk_widget_show(dt_ui_center(darktable.gui->ui));
  dt_thumbtable_update_parent(darktable.gui->ui->thumbtable_filmstrip);

  // clear selection, we don't want selections in darkroom
  dt_selection_clear(darktable.selection);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_LOADED,
                                  G_CALLBACK(_darkroom_image_loaded_callback), self);

  /* connect signal for filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                                  G_CALLBACK(_view_darkroom_filmstrip_activate_callback), self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui)); // ensure the center view has focus for keybindings to work

  const int32_t imgid = _darkroom_pending_imgid;
  _darkroom_pending_imgid = UNKNOWN_IMAGE;
  _darkroom_start_image_load(imgid);
}

void leave(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_gui_throttle_cancel(dev);

  _release_expose_source_caches();
  if(dev->image_surface) cairo_surface_destroy(dev->image_surface);
  dev->image_surface = NULL;

  // Send all pipeline shutdown signals first
  dev->exit = 1;
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);
  if(dev->virtual_pipe) dt_atomic_set_int(&dev->virtual_pipe->shutdown, TRUE);
  dev->pipelines_started = FALSE;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_darkroom_image_loaded_callback), (gpointer)self);
  _darkroom_cancel_image_load_job();
  _darkroom_pending_focus_module = NULL;

  // While we wait for possible pipelines to finish,
  // do the GUI cleaning.

  // store last active plugin:
  if(darktable.develop->gui_module)
    dt_conf_set_string("plugins/darkroom/active", darktable.develop->gui_module->op);
  else
    dt_conf_set_string("plugins/darkroom/active", "");

  // Hide the popover floating windows
  gtk_widget_hide(dev->overexposed.floating_window);
  gtk_widget_hide(dev->rawoverexposed.floating_window);
  gtk_widget_hide(dev->profile.floating_window);

  // Detach the default callback for bauhaus widgets
  dt_accels_detach_scroll_handler(darktable.gui->accels);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_darkroom_filmstrip_activate_callback),
  (gpointer)self);

  dt_iop_color_picker_cleanup();

  if(darktable.lib->proxy.colorpicker.picker_proxy)
    dt_iop_color_picker_reset(darktable.lib->proxy.colorpicker.picker_proxy->module, FALSE);

  _unregister_modules_drag_n_drop(self);

  // Detach shortcuts
  dt_accels_disconnect_active_group(darktable.gui->accels);

  // Restore the previous selection
  dt_selection_select_single(darktable.selection, dt_view_active_images_get_first());
  dt_view_active_images_reset(FALSE);

  dt_thumbtable_hide(darktable.gui->ui->thumbtable_filmstrip);
  gtk_widget_hide(dt_ui_center(darktable.gui->ui));

  // Pipeline nodes reference modules from dev->iop
  // we need to destroy objects referencing modules
  // before destroying the actual modules being referenced.
  dt_pthread_mutex_lock(&dev->pipe->busy_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_pthread_mutex_unlock(&dev->pipe->busy_mutex);

  dt_pthread_mutex_lock(&dev->preview_pipe->busy_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_pthread_mutex_unlock(&dev->preview_pipe->busy_mutex);

  dt_pthread_mutex_lock(&dev->virtual_pipe->busy_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->virtual_pipe);
  dt_pthread_mutex_unlock(&dev->virtual_pipe->busy_mutex);

  /* Device-side cache payloads are only an acceleration layer. Once darkroom
   * leaves and all pipe workers are quiescent, drop all cached cl_mem objects
   * so a later reopen can only exact-hit host-authoritative cachelines. */
  dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, -1, NULL);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_free_history(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  // Not sure why using g_list_free_full() here shits the bed
  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);
    dt_iop_cleanup_module(module);
    dt_free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    dt_free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  dev->iop = dev->alliop = NULL;

  // cleanup visible masks
  if(dev->form_gui)
  {
    dev->gui_module = NULL; // modules have already been g_free()
    dt_masks_gui_cleanup(dev);
  }

  // clear masks
  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  // Fetch the new thumbnail if needed. Ensure it runs after we save history.
  dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, darktable.develop->image_storage.id, TRUE);
  darktable.develop->image_storage.id = -1;

  // Release the cache entries for histogram buffers
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->raw_histogram));
  dt_dev_backbuf_set_hash(&dev->raw_histogram, -1);

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->output_histogram));
  dt_dev_backbuf_set_hash(&dev->output_histogram, -1);

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->display_histogram));
  dt_dev_backbuf_set_hash(&dev->display_histogram, -1);

  // Release the cache entries for GUI backbufs
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->pipe->backbuf));
  dt_dev_backbuf_set_hash(&dev->pipe->backbuf, -1);

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->preview_pipe->backbuf));
  dt_dev_backbuf_set_hash(&dev->preview_pipe->backbuf, -1);

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&dev->virtual_pipe->backbuf));
  dt_dev_backbuf_set_hash(&dev->virtual_pipe->backbuf, -1);


  dt_print(DT_DEBUG_CONTROL, "[run_job-] 11 %f in darkroom mode\n", dt_get_wtime());
}

void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // masks
  gboolean handled = FALSE;
  if(dt_masks_get_visible_form(dev) && dt_masks_events_mouse_leave(dev->gui_module))
    handled = TRUE;
  // module
  else if(dev->gui_module && dev->gui_module->mouse_leave
          && dev->gui_module->mouse_leave(dev->gui_module))
    handled = TRUE;

  if(handled)
    dt_control_queue_redraw_center();

  // reset any changes the selected plugin might have made.
  dt_control_set_cursor(GDK_LEFT_PTR);
  dt_control_change_cursor(GDK_LEFT_PTR);
}

static gboolean _is_in_frame(const int width, const int height, const int x, const int y)
{
  return !((x < -DT_PIXEL_APPLY_DPI(2)) ||
           (x > (width + DT_PIXEL_APPLY_DPI(4))) ||
           (y < -DT_PIXEL_APPLY_DPI(2)) ||
           (y > (height + DT_PIXEL_APPLY_DPI(4))));
}

/* This helper function tests for a position to be within the displayed area
   of an image. To avoid "border cases" we accept values to be slightly out of area too.
*/
static gboolean mouse_in_imagearea(dt_view_t *self, double x, double y)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  const double pwidth = (double)(dev->pipe->backbuf.width) / (double)darktable.gui->ppd;
  const double pheight = (double)(dev->pipe->backbuf.height) / (double)darktable.gui->ppd;

  x -= (double)(self->width - pwidth) / 2.;
  y -= (double)(self->height - pheight) / 2.;

  return _is_in_frame(pwidth, pheight, round(x), round(y));
}

static gboolean mouse_in_actionarea(dt_view_t *self, double x, double y)
{
  return _is_in_frame(self->width, self->height, round(x), round(y));
}

static void _set_default_cursor(dt_view_t *self, double x, double y)
{
  if(mouse_in_imagearea(self, x, y))
    dt_control_set_cursor(GDK_DOT);
  else if(mouse_in_actionarea(self, x, y))
    dt_control_set_cursor(GDK_CROSSHAIR);
  else
    dt_control_set_cursor(GDK_LEFT_PTR);
}

void mouse_enter(dt_view_t *self)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_masks_events_mouse_enter(dev->gui_module);
}

static void _delayed_history_commit(gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;

  // Figure out if an history item needs to be added
  // aka drawn masks have changed somehow. This is more expensive
  // but more reliable than handling individually all editing operations
  // in all callbacks in all possible mask types.
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_masks_update_hash(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  if(dev->forms_changed)
    dt_dev_add_history_item(dev, dev->gui_module, FALSE, TRUE);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;
  dt_control_t *ctl = darktable.control;

  // change cursor appearance by default
  _set_default_cursor(self, x, y);
  gboolean ret = FALSE;

  if(dt_iop_color_picker_is_visible(dev) && ctl->button_down && ctl->button_down_which == 1)
  {
    // module requested a color box
    if(mouse_in_imagearea(self, x, y))
    {
      dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
      gboolean sample_changed = FALSE;
      // Make sure a minimal width/height
      float delta_x = 1 / (float) dev->pipe->processed_width;
      float delta_y = 1 / (float) dev->pipe->processed_height;

      float mouse_point[2] = { (float)x, (float)y };
      dt_dev_coordinates_widget_to_image_norm(dev, mouse_point, 1);
      float mouse_x = mouse_point[0];
      float mouse_y = mouse_point[1];

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        const float box[4] = {
          fmaxf(0.0, MIN(sample->point[0], mouse_x) - delta_x),
          fmaxf(0.0, MIN(sample->point[1], mouse_y) - delta_y),
          fminf(1.0, MAX(sample->point[0], mouse_x) + delta_x),
          fminf(1.0, MAX(sample->point[1], mouse_y) + delta_y)
        };

        for(int k = 0; k < 4; k++)
        {
          if(sample->box[k] != box[k])
          {
            sample->box[k] = box[k];
            sample_changed = TRUE;
          }
        }
      }
      else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      {
        sample_changed = (sample->point[0] != mouse_x) || (sample->point[1] != mouse_y);
        sample->point[0] = mouse_x;
        sample->point[1] = mouse_y;
      }

      if(sample_changed)
      {
        /* Picker drags only move the sampling area.
         * Mark the preview dirty so the worker resamples the active module
         * without touching history or recomputing node hashes. */
        dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      }
    }
    ret = TRUE;
  }

  // masks
  else if(dt_masks_get_visible_form(dev)
          && dt_masks_events_mouse_moved(dev->gui_module, x, y, pressure, which))
  {
    dt_gui_throttle_queue(dev, _delayed_history_commit, dev);
    ret = TRUE;
  }

  // module
  else if(dev->gui_module && dev->gui_module->mouse_moved
    &&dev->gui_module->mouse_moved(dev->gui_module, x, y, pressure, which))
  {
    ret = TRUE;
  }

  dt_control_commit_cursor();

  if(ret)
  {
    dt_control_queue_redraw_center();
    return;
  }

  // panning with left mouse button
  if(darktable.control->button_down && darktable.control->button_down_which == 1 && dev->roi.scaling > 1)
  {
    const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
    const double clicked_x = ctl->button_x;
    const double clicked_y = ctl->button_y;
    // delta in roi scale
    const double roi_dx = x - clicked_x;
    const double roi_dy = y - clicked_y;
    // convert to full image scale
    const double img_dx = roi_dx / scale;
    const double img_dy = roi_dy / scale;

    // new roi position in full image scale
    float new_x = dev->roi.x - (img_dx / (float)dev->pipe->processed_width);
    float new_y = dev->roi.y - (img_dy / (float)dev->pipe->processed_height);
  
    //fprintf(stderr, "BOUNDS new_x: %2.2f, new_y: %2.2f\n", new_x, new_y);
    dt_dev_check_zoom_pos_bounds(dev, &new_x, &new_y, NULL, NULL); 

    dev->roi.x = new_x;
    dev->roi.y = new_y;

    // update clicked position
    ctl->button_x = x;
    ctl->button_y = y;

    dt_dev_pixelpipe_change_zoom_main(dev);
  }
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  dt_print(DT_DEBUG_INPUT, "[darkroom] button released which: %d state: %d x: %.2f y: %.2f\n",
           which, state, x, y);

  if(dt_iop_color_picker_is_visible(dev) && which == 1)
  {
    // only sample box picker at end, for speed
    if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      dt_control_set_cursor(GDK_LEFT_PTR);

    /* Final picker placement changes only the sample geometry.
     * Keep the current history and ask for one fresh preview sampling pass. */
    dt_dev_pixelpipe_update_history_preview(dev);
    dt_control_queue_redraw_center();
    return 1;
  }
  // masks
  if(dt_masks_get_visible_form(dev)
     && dt_masks_events_button_released(dev->gui_module, x, y, which, state))
  {
    // Change on mask parameters and image output.
    dt_gui_throttle_queue(dev, _delayed_history_commit, dev);
    dt_dev_pixelpipe_update_history_preview(dev); // Needed if mask selection changed
    dt_control_queue_redraw_center();
    return 1;
  }

  // module
  if(dev->gui_module && dev->gui_module->enabled && dev->gui_module->button_released
     && dev->gui_module->button_released(dev->gui_module, x, y, which, state))
  {
    // Click in modules should handle history changes internally.
    return 1;
  }

  return 0;
}


int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;
  dt_develop_t *dev = (dt_develop_t *)self->data;

  dt_print(DT_DEBUG_INPUT, "[darkroom] button pressed  which: %d  type: %d x: %.2f y: %.2f pressure: %f\n",
           which, type, x, y, pressure);

  // Grab focus on any click so we can interact from keyboard
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  if(dt_iop_color_picker_is_visible(dev))
  {
    float pt_xy[2] = { (float)x, (float)y };
    dt_dev_coordinates_widget_to_image_norm(dev, pt_xy, 1);
    float pzx = pt_xy[0];
    float pzy = pt_xy[1];

    float zoom_scale = dev->roi.scaling;
    const int procw = dev->roi.preview_width;
    const int proch = dev->roi.preview_height;

    if(which == 1)
    {
      if(mouse_in_imagearea(self, x, y))
      {
        // The default box will be a square with 1% of the image width
        const float delta_x = 0.01f;
        const float delta_y = delta_x * (float)dev->pipe->processed_width / (float)dev->pipe->processed_height;

        // FIXME: here and in mouse move use to dt_lib_colorpicker_set_{box_area,point} interface? -- would require a different hack for figuring out base of the drag
        // hack: for box pickers, these represent the "base" point being dragged
        sample->point[0] = pzx;
        sample->point[1] = pzy;

        if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
        {
          // this is slightly more than as drawn, to give room for slop
          const float handle_px = 6.0f;
          float hx = handle_px / (procw * zoom_scale);
          float hy = handle_px / (proch * zoom_scale);
          gboolean on_corner_prev_box = TRUE;
          // initialized to calm gcc-11
          float opposite_x = 0.f, opposite_y = 0.f;

          if(fabsf(pzx - sample->box[0]) <= hx)
            opposite_x = sample->box[2];
          else if(fabsf(pzx - sample->box[2]) <= hx)
            opposite_x = sample->box[0];
          else
            on_corner_prev_box = FALSE;

          if(fabsf(pzy - sample->box[1]) <= hy)
            opposite_y = sample->box[3];
          else if(fabsf(pzy - sample->box[3]) <= hy)
            opposite_y = sample->box[1];
          else
            on_corner_prev_box = FALSE;

          if(on_corner_prev_box)
          {
            sample->point[0] = opposite_x;
            sample->point[1] = opposite_y;
          }
          else
          {
            sample->box[0] = fmaxf(0.0, pzx - delta_x);
            sample->box[1] = fmaxf(0.0, pzy - delta_y);
            sample->box[2] = fminf(1.0, pzx + delta_x);
            sample->box[3] = fminf(1.0, pzy + delta_y);
          }
          dt_control_set_cursor(GDK_FLEUR);
        }
      }
      return 1;
    }

    if(which == 3)
    {
      // apply a live sample's area to the active picker?
      // FIXME: this is a naive implementation, nicer would be to cycle through overlapping samples then reset
      dt_iop_color_picker_t *picker = darktable.lib->proxy.colorpicker.picker_proxy;
      if(darktable.lib->proxy.colorpicker.display_samples && mouse_in_imagearea(self, x, y))
        for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples; samples; samples = g_slist_next(samples))
        {
          dt_colorpicker_sample_t *live_sample = samples->data;
          if(live_sample->size == DT_LIB_COLORPICKER_SIZE_BOX && picker->kind != DT_COLOR_PICKER_POINT)
          {
            if(pzx < live_sample->box[0] || pzx > live_sample->box[2]
               || pzy < live_sample->box[1] || pzy > live_sample->box[3])
              continue;
            dt_lib_colorpicker_set_box_area(darktable.lib, live_sample->box);
          }
          else if(live_sample->size == DT_LIB_COLORPICKER_SIZE_POINT && picker->kind != DT_COLOR_PICKER_AREA)
          {
            // magic values derived from _darkroom_pickers_draw
            float slop_px = MAX(26.0f, roundf(3.0f * zoom_scale));
            const float slop_x = slop_px / (procw * zoom_scale);
            const float slop_y = slop_px / (proch * zoom_scale);
            if(fabsf(pzx - live_sample->point[0]) > slop_x || fabsf(pzy - live_sample->point[1]) > slop_y)
              continue;
            dt_lib_colorpicker_set_point(darktable.lib, live_sample->point);
          }
          else
            continue;
          return 1;
        }

      if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        // default is hardcoded this way
        // FIXME: color_pixer_proxy should have an dt_iop_color_picker_clear_area() function for this
        dt_boundingbox_t reset = { 0.01f, 0.01f, 0.99f, 0.99f };
        dt_lib_colorpicker_set_box_area(darktable.lib, reset);
      }
      return 1;
    }
  }

  // masks
  if(dt_masks_get_visible_form(dev)
     && dt_masks_events_button_pressed(dev->gui_module, x, y, pressure, which, type, state))
  {
    dt_gui_throttle_queue(dev, _delayed_history_commit, dev);
    return 1;
  }
  // module
  if(dev->gui_module && dev->gui_module->enabled && dev->gui_module->button_pressed
     && dev->gui_module->button_pressed(dev->gui_module, x, y, pressure, which, type, state))
     return 1;

  if(which == 2)
  {
    // Incremental zoom-in on middle button click, from fit to 800% 
    // by power of 2 increments (100%, 200%, 400%, 800%).
    float new_scale = 1.f;
    if(dev->roi.scaling < dev->roi.natural_scale || dev->roi.scaling > 7.f / dev->roi.natural_scale)
      new_scale = dev->roi.natural_scale; // zoom to fit
    else if(dev->roi.scaling * dev->roi.natural_scale < 1.f)
      new_scale = 1.f; // 100 %
    else
      new_scale = floorf(dev->roi.scaling * dev->roi.natural_scale) * 2.f;

    // Actual pixelpipe scaling is dev->roi.scaling * dev->roi.natural_scale,
    // where dev->roi.natural_scale ensures the images fits within viewport
    new_scale /= dev->roi.natural_scale;

    return _change_scaling(dev, x, y, new_scale);
  }

  return 0;
}

static int _change_scaling(dt_develop_t *dev, const float x, const float y, const float new_scaling)
{
  const float old_scaling = dev->roi.scaling;

  // Round scaling to 1.0 (fit) if close enough
  const float epsilon = fabsf(old_scaling - new_scaling);
  if(fabsf(new_scaling - 1.0f) < epsilon)
    dev->roi.scaling = 1.0f;
  else
    dev->roi.scaling = new_scaling;

  if(!dt_dev_check_zoom_scale_bounds(dev))
  { 
    // Calculate zoom position offset to keep mouse position fixed during zoom
    const float mouse_off_x = x - 0.5f * dev->roi.orig_width;
    const float mouse_off_y = y - 0.5f * dev->roi.orig_height;
    
    // To keep the point under the mouse fixed, calculate the adjustment
    const float scale = dt_dev_get_zoom_level(dev);
    const float ppd = darktable.gui->ppd;
    const float scaling = dev->roi.scaling;
    const float scale_delta = ppd * (scaling - old_scaling);
    const float scale_product = old_scaling * scale;
    
    // Adjust the center to compensate for the scale change
    int proc_w = 0.f, proc_h = 0.f;
    dt_dev_get_processed_size(dev, &proc_w, &proc_h);
    dev->roi.x += mouse_off_x * scale_delta / (proc_w * scale_product);
    dev->roi.y += mouse_off_y * scale_delta / (proc_h * scale_product);
    
    dt_dev_check_zoom_pos_bounds(dev, &dev->roi.x, &dev->roi.y, NULL, NULL);
    dt_dev_pixelpipe_change_zoom_main(dev);
    return 1;
  }
  else
  {
    // Invalid zoom level, keep previous value
    dev->roi.scaling = old_scaling;
    return 0;
  }
}

static gboolean _center_view_free_zoom(dt_view_t *self, double x, double y, int up, int state, int flow)
{
  dt_develop_t *dev = darktable.develop;

  // Commit the new scaling
  const float step = 1.02f;
  const float new_scaling = dev->roi.scaling * powf(step, (float)-flow);
  return _change_scaling(dev, x, y, new_scaling);
}


int scrolled(dt_view_t *self, double x, double y, int up, int state, int delta_y)
{
  if(_is_scroll_captured_by_widget()) return FALSE;

  dt_develop_t *dev = (dt_develop_t *)self->data;

  dt_print(DT_DEBUG_INPUT, "[darkroom] scrolled: up: %i x: %.2f y: %.2f state: %i flow: %i\n",
           up, x, y, state, delta_y);

  // masks
  if(dt_masks_get_visible_form(dev)
     && dt_masks_events_mouse_scrolled(dev->gui_module, x, y, up, state, delta_y))
  {
    // Scroll on masks changes their size, therefore mask parameters and image output.
    dt_gui_throttle_queue(dev, _delayed_history_commit, dev);
    return TRUE;
  }

  // module
  if(dev->gui_module && dev->gui_module->enabled && dev->gui_module->scrolled && dev->gui_module->scrolled(dev->gui_module, x, y, up, state))
  {
    // Scroll in modules should handle history changes internally.
    return TRUE;
  }

  // free zoom
  return _center_view_free_zoom(self, x, y, up, state, delta_y);
}

static void _key_scroll(dt_develop_t *dev)
{
  dt_dev_check_zoom_pos_bounds(dev, &dev->roi.x, &dev->roi.y, NULL, NULL);
  dt_control_queue_redraw_center();
  dt_dev_pixelpipe_change_zoom_main(dev);
}


int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  dt_develop_t *dev = (dt_develop_t *)self->data;
  const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;

  if(dt_masks_get_visible_form(dev) && dt_masks_events_key_pressed(dev->gui_module, event))
  {
    dt_gui_throttle_queue(dev, _delayed_history_commit, dev);
    return 1;
  }

  const gboolean shift = dt_modifier_is(event->state, GDK_SHIFT_MASK);
  const gboolean ctrl = dt_modifier_is(event->state, GDK_CONTROL_MASK);
  const gboolean ctrl_any = dt_modifiers_include(event->state, GDK_CONTROL_MASK);

  if(ctrl_any)
  {
    const float zoom_step = 1.1f;
    const float center_x = 0.5f * dev->roi.orig_width;
    const float center_y = 0.5f * dev->roi.orig_height;

    switch(event->keyval)
    {
      case GDK_KEY_plus:
      case GDK_KEY_KP_Add:
        return _change_scaling(dev, center_x, center_y, dev->roi.scaling * zoom_step);
      case GDK_KEY_minus:
      case GDK_KEY_KP_Subtract:
        return _change_scaling(dev, center_x, center_y, dev->roi.scaling / zoom_step);
    }
  }

  float multiplier = (shift) ? 4.f :
                     (ctrl) ? 0.5f :
                     1.f;

  float delta = 10.f / scale * multiplier;

  switch(event->keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    {
      dev->roi.y -= delta / (float)dev->pipe->processed_height;
      _key_scroll(dev);
      return 1;
    }
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    {
      dev->roi.y += delta / (float)dev->pipe->processed_height;
      _key_scroll(dev);
      return 1;
    }
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
    {
      dev->roi.x -= delta / (float)dev->pipe->processed_height;
      _key_scroll(dev);
      return 1;
    }
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
    {
      dev->roi.x += delta / (float)dev->pipe->processed_height;
      _key_scroll(dev);
      return 1;
    }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // "Give focus to the main image"
      gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
      return 1;
    }
  }

  return 0;
}

void configure(dt_view_t *self, int wd, int ht)
{
  dt_develop_t *dev = (dt_develop_t *)self->data;

  // Configure event is called when initing the view AND upon window resizes events (through Gtk widget/window resize commands).
  // At init time, final window size may not be correct just yet.
  // It will be when we call dt_ui_restore_panels(), which will resize stuff properly,
  // but that will be only when entering the current view.
  // Until we run dt_dev_configure(), main preview pipe gets output size -1×-1 px
  // which aborts the pipe recompute early. As soon as we init
  // sizes with something "valid" with regard to the pipe, pipeline runs.
  // Problem is it will not be valid with regard to the window size and the output will be thrown out
  // until we get the final size.
  // TD;DR: until we get the final window size, which happens
  // only when entering the view, don't configure the main preview pipeline, which will disable useless recomputes.
  if(dt_view_manager_get_current_view(darktable.view_manager) == self)
  {
    // Reference dimensions before ISO 12646 mode
    dev->roi.orig_height = ht;
    dev->roi.orig_width = wd;
    _get_final_size_with_iso_12646(dev);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
