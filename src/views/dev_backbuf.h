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

#ifndef DT_VIEWS_DEV_BACKBUF_H
#define DT_VIEWS_DEV_BACKBUF_H

#include <cairo.h>
#include <glib.h>

#include "common/simd.h"
// dev_pixelpipe.h uses dt_dev_pixelpipe_t/dt_dev_pixelpipe_change_t (defined in pixelpipe_hb.h)
// without including it itself - include it first so this header is self-contained regardless
// of what a caller happened to include before it.
#include "develop/pixelpipe_hb.h"
#include "develop/dev_pixelpipe.h"

struct dt_develop_t;
struct dt_pixel_cache_entry_t;

/** A GUI-side view onto one pipe's currently published backbuffer: wraps the
 * pipe cache's own memory, never an independent copy (pixelpipe_hb.c owns
 * the keepalive ref). Callers declare and own their own instance(s) -
 * darkroom keeps one for dev->pipe and one for dev->preview_pipe, Studio
 * Capture owns exactly one, for its main pipe only. Initialize `.hash` to
 * DT_PIXELPIPE_CACHE_HASH_INVALID before first use - a zero-initialized
 * (calloc'd) instance looks like a valid hash of 0, not the sentinel. */
typedef struct dt_dev_locked_surface_t
{
  uint64_t hash;
  int width;
  int height;
  void *data;
  struct dt_pixel_cache_entry_t *entry;
  cairo_surface_t *surface;
} dt_dev_locked_surface_t;

/** (Re)bind `locked` to `pipe`'s currently published backbuffer if it is
 * newer/still valid. `wait` and `wait_owner_tag` are the caller's own
 * dt_dev_pixelpipe_cache_wait_t instance and a short static debug label
 * (e.g. "darkroom-main", "studio-capture-main"). `keep_previous_on_fail`:
 * on a miss, keep whatever `locked` already held instead of dropping it. */
gboolean dt_dev_lock_pipe_surface(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                  dt_dev_locked_surface_t *locked, dt_dev_pixelpipe_cache_wait_t *wait,
                                  const char *wait_owner_tag, gboolean keep_previous_on_fail);

/** Paint `locked`'s surface into `cr`, centered in a width x height widget
 * with `border` px margin and `bg_color` background; also draws the ISO
 * 12646 white surround iff dev->iso_12646.enabled. Pure paint - call
 * dt_dev_lock_pipe_surface() first. */
gboolean dt_dev_render_locked_surface(cairo_t *cr, const struct dt_develop_t *dev,
                                      dt_dev_locked_surface_t *locked, int width, int height, int border,
                                      const dt_aligned_pixel_t bg_color);

/** Convenience for a caller that only wants the main (FULL) pipe's tier,
 * with no separate preview-pipe substitution: lock + render dev->pipe in
 * one call. This is what Studio Capture uses; darkroom's own multi-tier
 * fallback cascade calls dt_dev_lock_pipe_surface()/dt_dev_render_locked_surface()
 * directly instead. */
gboolean dt_dev_paint_main_backbuf(dt_dev_locked_surface_t *locked, dt_dev_pixelpipe_cache_wait_t *wait,
                                   const char *wait_owner_tag, cairo_t *cr, struct dt_develop_t *dev,
                                   int width, int height, int border, const dt_aligned_pixel_t bg_color,
                                   gboolean keep_previous_on_fail);

/** Drop everything `locked` references. Does not touch the pipe cache entry
 * itself (pixelpipe_hb.c owns it). Callers MUST call this, on every
 * instance they own, before their dev's pipeline nodes/backbufs are torn
 * down. */
void dt_dev_release_locked_surface(dt_dev_locked_surface_t *locked);

/** Pick the background color for the center view: ISO 12646's fixed L=50
 * grey when enabled, the user's "display/brightness" conf otherwise. */
void dt_dev_get_background_color(const struct dt_develop_t *dev, dt_aligned_pixel_t bg_color);

/** Draw the ISO 12646 white surround rectangle around a `width` x `height`
 * image with `border` px margin. */
void dt_dev_draw_iso12646_border(cairo_t *cr, double width, double height, int border);

/** Draw the "soft proof" / "gamut check" text overlay in the bottom-left
 * corner when dt_colorspaces_get_global()->mode is not DT_PROFILE_NORMAL; a
 * no-op otherwise. */
void dt_dev_draw_profile_mode_label(cairo_t *cri, int height);
#endif // DT_VIEWS_DEV_BACKBUF_H
