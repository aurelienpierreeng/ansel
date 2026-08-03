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
#pragma once

#include <cairo.h>
#include <glib.h>
#include <stdint.h>

struct dt_develop_t;

// A still render of one image's pipeline output, decoupled from any live dt_dev_pixelpipe_t,
// plus the resampled-crop cache needed to composite it into a darkroom viewport at an arbitrary
// pan/zoom. Used to show an image other than (or a frozen past state of) the one currently open
// in darkroom, positioned as if it were the live pipe's own output -- see libs/snapshots.c
// (compare current edit against a past history state) and libs/duplicate.c (preview another
// version of the same shot without leaving darkroom).
typedef struct dt_dev_snapshot_t
{
  cairo_surface_t *image;          // full-resolution render, owned. NULL if nothing captured.
  cairo_surface_t *display_image;  // Mitchell-resampled viewport crop cache, owned. Rebuilt by
                                    // dt_dev_snapshot_draw() as pan/zoom moves; never set directly.
  float display_scale;
  int32_t crop_x, crop_y, crop_w, crop_h;
  float sample_scale;              // scale `image` was rendered at (see dt_dev_snapshot_capture)
} dt_dev_snapshot_t;

// Renders imgid's pipeline output into snap->image, replacing any previous content. `scale` is
// the fraction of the processed image size to render at (1.0 = full size).
//
// history_override/iop_order_override, if non-NULL, are used verbatim instead of imgid's own
// on-disk history -- e.g. to capture a *live*, possibly-uncommitted edit from a dt_develop_t
// that currently has imgid open. Ownership of both lists transfers to this call (freed
// internally, win or lose) -- duplicate them first (dt_history_duplicate() /
// dt_ioppr_iop_order_copy_deep()) if the caller still needs its own copy afterwards. Pass
// NULL/NULL/-1 to render imgid's own persisted history as-is, with no live override.
//
// Returns FALSE on failure, in which case snap is left cleared.
gboolean dt_dev_snapshot_capture(dt_dev_snapshot_t *snap, int32_t imgid, float scale,
                                  GList *history_override, GList *iop_order_override,
                                  int32_t history_end_override);

// Frees snap's surfaces and resets it to empty. Safe to call on an already-empty snapshot.
void dt_dev_snapshot_clear(dt_dev_snapshot_t *snap);

// Paints snap into cri as if it were dev's own pipeline output, matching dev's current pan and
// zoom (dev->roi, dt_dev_get_zoom_level()), clipped to (clip_x, clip_y, clip_w, clip_h) in widget
// space. width/height must be the full darkroom center-view widget size -- needed to locate
// source pixel (0,0) -- independently of how small the clip rect is. No-op if snap holds no
// image. Rebuilds/reuses snap->display_image as needed; callers must not free it directly.
void dt_dev_snapshot_draw(dt_dev_snapshot_t *snap, cairo_t *cri, struct dt_develop_t *dev,
                           int32_t width, int32_t height,
                           double clip_x, double clip_y, double clip_w, double clip_h);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
