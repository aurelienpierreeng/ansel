/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

/** Shared pixelpipe-backbuffer-to-screen compositing for any view owning its
 * own dt_develop_t (darkroom, Studio Capture...). Extracted from darkroom.c,
 * which keeps its own multi-tier main/preview/fallback cascade on top of the
 * primitives here; a caller wanting just the main pipe's live output (no
 * separate preview-pipe substitution tier) uses dt_dev_paint_main_backbuf(). */

#include "views/dev_backbuf.h"
#include "common/colorspaces_inline_conversions.h"   // dt_Lab_to_XYZ(), dt_XYZ_to_sRGB()
#include "common/conf.h"

#include "widgets/bauhaus.h"
#include "colorprofiles/colorspaces.h"
#include "system/macros.h"
#include "system/simd.h"
#include "gui/application.h"
#include "control/control.h"
#include "develop/develop.h"
#include "caches/pixelpipe_cache.h"
#include "develop/pixelpipe_hb.h"
#include "develop/supervisor.h"

#include <pango/pangocairo.h>

static void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_colorprofiles_xyz_to_display(XYZ, RGB);
}

void dt_dev_get_background_color(const dt_develop_t *dev, dt_aligned_pixel_t bg_color)
{
  // user param is Lab lightness/brightness (which is the same for greys)
  if(dev->iso_12646.enabled)
    _colormanage_ui_color(50., 0., 0., bg_color);
  else
    _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);
}

void dt_dev_draw_iso12646_border(cairo_t *cr, double width, double height, int border)
{
  // draw the white frame around picture
  cairo_rectangle(cr, -border * .5f, -border * .5f, width + border, height + border);
  cairo_set_source_rgb(cr, 1., 1., 1.);
  cairo_fill(cr);
}

void dt_dev_draw_profile_mode_label(cairo_t *cri, int height)
{
  /* One snapshot: reading mode twice could print a label for a mode that has already
   * changed between the guard and the text. */
  dt_colorprofiles_settings_t settings;
  dt_colorprofiles_get_settings(&settings);
  if(settings.mode == DT_PROFILE_NORMAL) return;

  gchar *label = settings.mode == DT_PROFILE_GAMUTCHECK ? _("gamut check") : _("soft proof");
  cairo_set_source_rgba(cri, 0.5, 0.5, 0.5, 0.5);
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(dt_bauhaus_get_global()->pango_font_desc);
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

void dt_dev_release_locked_surface(dt_dev_locked_surface_t *locked)
{
  if(IS_NULL_PTR(locked)) return;

  if(locked->surface)
  {
    cairo_surface_destroy(locked->surface);
    locked->surface = NULL;
  }

  /* These cairo views only mirror whatever cacheline the pipeline currently exposes as backbuffer.
   * They never own the backbuffer keepalive ref themselves: `pixelpipe_hb.c` swaps that ownership
   * when publishing a new backbuffer. Releasing the surface must therefore only drop the GUI view. */
  locked->entry = NULL;
  locked->data = NULL;
  locked->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  locked->width = 0;
  locked->height = 0;
}

static void _dev_backbuf_restart_cache_wait(gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  dt_control_queue_redraw_center();
}

gboolean dt_dev_lock_pipe_surface(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, dt_dev_locked_surface_t *locked,
                                  dt_dev_pixelpipe_cache_wait_t *wait, const char *wait_owner_tag,
                                  gboolean keep_previous_on_fail)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(locked)) return FALSE;
  if(!IS_NULL_PTR(wait))
    dt_dev_pixelpipe_cache_wait_set_owner(wait, wait_owner_tag, dev);

  const uint64_t hash = dt_dev_backbuf_get_hash(&pipe->backbuf);
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return keep_previous_on_fail && (!IS_NULL_PTR(locked->surface));

  /* Fast-path reuse is only valid if the cacheline identity/data pointer are
   * still the same behind the hash key. This avoids reusing stale cairo views
   * when an entry is recreated or replaced under the same key. */
  dt_pixel_cache_entry_t *live_entry = NULL;
  void *live_data = NULL;
  if(!IS_NULL_PTR(locked->surface) && locked->hash == hash
     && dt_dev_pixelpipe_cache_peek_gui(pipe, NULL, &live_data, &live_entry, wait,
                                        _dev_backbuf_restart_cache_wait, dev)
     && live_entry == locked->entry && live_data == locked->data)
  {
    locked->width = pipe->backbuf.width;
    locked->height = pipe->backbuf.height;
    return TRUE;
  }

  struct dt_pixel_cache_entry_t *entry = NULL;
  /* GUI surfaces only borrow the currently published backbuffer. They rely on the backbuffer keepalive ref
   * owned by `pixelpipe_hb.c`, so they must not take or drop their own cache refs here. */
  void *data = NULL;
  if(!dt_dev_pixelpipe_cache_peek_gui(pipe, NULL, &data, &entry, wait, _dev_backbuf_restart_cache_wait, dev))
    data = NULL;
  if(IS_NULL_PTR(data))
  {
    /* Keep previous frame only while waiting for a *different* target hash.
     * If requested hash equals the currently locked one but cache lookup fails,
     * the cached line was likely flushed/invalidated: drop stale lock so the
     * line can be recreated and displayed again. */
    if(keep_previous_on_fail && !IS_NULL_PTR(locked->surface) && locked->hash != hash) return TRUE;
    dt_dev_release_locked_surface(locked);
    return FALSE;
  }

  const int width = pipe->backbuf.width;
  const int height = pipe->backbuf.height;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  const size_t required_size = (size_t)stride * (size_t)height;
  const size_t entry_size = dt_pixel_cache_entry_get_size(entry);
  if(width <= 0 || height <= 0 || entry_size < required_size || dt_pixel_cache_entry_get_data(entry) != data)
  {
    if(keep_previous_on_fail && !IS_NULL_PTR(locked->surface) && locked->hash != hash) return TRUE;
    dt_dev_release_locked_surface(locked);
    return FALSE;
  }

  // The widget is about to (re)bind its surface to this backbuffer cacheline.
  // Emitted off the fast-path, so it fires when the displayed buffer changes,
  // not on every identical expose.
  if(dt_supervisor_active())
    dt_supervisor_widget(DT_SV_READ, wait_owner_tag, hash, pipe->type, pipe->imgid);

  if(!IS_NULL_PTR(locked->surface) && locked->data == data && locked->width == width && locked->height == height)
  {
    locked->hash = hash;
    locked->entry = entry;
    locked->data = data;
    return TRUE;
  }

  cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, width, height, stride);
  if(IS_NULL_PTR(surface) || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    if(!IS_NULL_PTR(surface)) cairo_surface_destroy(surface);
    if(keep_previous_on_fail && !IS_NULL_PTR(locked->surface)) return TRUE;
    dt_dev_release_locked_surface(locked);
    return FALSE;
  }

  dt_dev_release_locked_surface(locked);
  locked->hash = hash;
  locked->width = width;
  locked->height = height;
  locked->data = data;
  locked->entry = entry;
  locked->surface = surface;
  return TRUE;
}

gboolean dt_dev_render_locked_surface(cairo_t *cr, const dt_develop_t *dev, dt_dev_locked_surface_t *locked,
                                      const int width, const int height, const int border,
                                      const dt_aligned_pixel_t bg_color)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(dev) || IS_NULL_PTR(locked) || IS_NULL_PTR(locked->surface)) return FALSE;
  if(IS_NULL_PTR(locked->entry) || locked->hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return FALSE;

  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  int wd = locked->width;
  int ht = locked->height;
  if(wd <= 0 || ht <= 0) return FALSE;

  wd /= dt_gui_get_global()->ppd;
  ht /= dt_gui_get_global()->ppd;
  cairo_translate(cr, .5f * (width - wd), .5f * (height - ht));

  if(dev->iso_12646.enabled) dt_dev_draw_iso12646_border(cr, wd, ht, border);

  dt_dev_pixelpipe_cache_rdlock_entry(TRUE, locked->entry);
  cairo_surface_set_device_scale(locked->surface, dt_gui_get_global()->ppd, dt_gui_get_global()->ppd);
  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_set_source_surface(cr, locked->surface, 0, 0);
  cairo_fill(cr);
  dt_dev_pixelpipe_cache_rdlock_entry(FALSE, locked->entry);

  return TRUE;
}

gboolean dt_dev_paint_main_backbuf(dt_dev_locked_surface_t *locked, dt_dev_pixelpipe_cache_wait_t *wait,
                                   const char *wait_owner_tag, cairo_t *cr, dt_develop_t *dev, int width,
                                   int height, int border, const dt_aligned_pixel_t bg_color,
                                   gboolean keep_previous_on_fail)
{
  if(IS_NULL_PTR(dev)) return FALSE;
  if(!dt_dev_lock_pipe_surface(dev, dev->pipe, locked, wait, wait_owner_tag, keep_previous_on_fail))
    return FALSE;
  if(IS_NULL_PTR(locked->surface)) return FALSE;
  return dt_dev_render_locked_surface(cr, dev, locked, width, height, border, bg_color);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
