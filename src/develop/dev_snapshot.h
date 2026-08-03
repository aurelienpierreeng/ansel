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

// A live render of one image's pipeline output, scoped to dev's current viewport (ROI) and
// recomputed as pan/zoom change -- decoupled from any *other* dt_dev_pixelpipe_t, so it can show
// an image other than (or a frozen past state of) the one currently open in darkroom, positioned
// as if it were the live pipe's own output. See libs/snapshots.c (compare current edit against a
// past history state) and libs/duplicate.c (preview another version of the same shot without
// leaving darkroom).
//
// Opaque handle: the real state lives in a heap-allocated, refcounted dt_dev_snapshot_engine_t
// (private to dev_snapshot.c) so that copying/moving a dt_dev_snapshot_t -- e.g. libs/snapshots.c
// shuffling its fixed-size slot array on take/delete -- only ever copies a stable pointer, never
// the engine itself. This matters because the "main" tier's accurate reprocess runs on a
// background job (see dev_snapshot.c): the engine can outlive the dt_dev_snapshot_t handle that
// created it for as long as that job still references it, and is only actually freed once both
// the handle and any in-flight job have released their reference.
typedef struct dt_dev_snapshot_t
{
  struct dt_dev_snapshot_engine_t *engine; // refcounted, owned (one reference held here). NULL if nothing captured.
} dt_dev_snapshot_t;

// Sets up snap to render imgid's pipeline output, matching `dev`'s current pan/zoom/scale, and
// keeps recomputing on subsequent dt_dev_snapshot_draw() calls as dev's viewport changes -- same
// ROI formula as dev->pipe itself, so only the visible window is ever processed, never the whole
// image. The accurate reprocess after a pan/zoom change runs on a background job (Ansel's
// existing control/jobs.h system), so it never blocks the GUI thread; a cheap fit-scale fallback
// is shown while it is in flight.
//
// history_override/iop_order_override, if non-NULL, are used verbatim instead of imgid's own
// on-disk history -- e.g. to capture a *live*, possibly-uncommitted edit from a dt_develop_t
// that currently has imgid open. Ownership of both lists transfers to this call (freed
// internally, win or lose) -- duplicate them first (dt_history_duplicate() /
// dt_ioppr_iop_order_copy_deep()) if the caller still needs its own copy afterwards. Pass
// NULL/NULL/-1 to render imgid's own persisted history as-is, with no live override.
//
// Returns FALSE on failure (e.g. the first, current-viewport render failed), in which case snap
// is left cleared.
gboolean dt_dev_snapshot_capture(dt_dev_snapshot_t *snap, struct dt_develop_t *dev, int32_t imgid,
                                  GList *history_override, GList *iop_order_override,
                                  int32_t history_end_override);

// Releases snap's own reference to its engine (best-effort cancelling an in-flight recompute job)
// and resets it to empty. Never blocks: if a background job is still running, it holds its own
// reference and frees the engine itself once it finishes. Safe to call on an already-empty
// snapshot.
void dt_dev_snapshot_clear(dt_dev_snapshot_t *snap);

// TRUE once snap holds a captured, successfully-rendered image.
gboolean dt_dev_snapshot_is_valid(const dt_dev_snapshot_t *snap);

// Paints snap into cri as if it were dev's own pipeline output, matching dev's current pan and
// zoom, clipped to (clip_x, clip_y, clip_w, clip_h) in widget space. width/height must be the
// full darkroom center-view widget size. Purely resizing/moving the clip rect (e.g. dragging a
// compare split line) never triggers a reprocess, since it never touches dev->roi.
//
// If dev's viewport (pan/zoom) changed since the "main" tier's last successful render, an
// accurate reprocess is requested on a background job (same immediacy as
// dev->pipe's own worker loop, throttled only by "one job in flight at a time") instead of
// running inline, and the fit-scale "preview" tier is drawn (cairo-transformed to approximate the
// new viewport) in the meantime -- same fallback idea as darkroom.c's own main/preview cascade.
// No-op if snap holds no image.
void dt_dev_snapshot_draw(dt_dev_snapshot_t *snap, cairo_t *cri, struct dt_develop_t *dev,
                           int32_t width, int32_t height,
                           double clip_x, double clip_y, double clip_w, double clip_h);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
