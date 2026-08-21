# GUI sizing without a full pixel-less pipeline — analysis and plan

Written while fixing #1157, whose root cause is entangled with the virtual pipe's cost.

**Status: superseded by what was actually built.** The maintainer picked option C, and
`doc/geometry-service.md` is the canonical record of it: the virtual pipe no longer exists, and
the GUI composes sizes and coordinates from published per-module records. This file is kept for
the measurements and the consumer audit that produced that decision — everything below describes
the tree as it was, in the present tense it was written in.

## What the virtual pipe was, and what it cost

`dev->virtual_pipe` was a full clone of the module stack — every one of the ~95 IOPs gets a
node, params committed from history — that never processes a pixel. It exists so the GUI
thread can answer two questions synchronously, without waiting for a worker pipe:

1. **"How big is the processed image?"** — `dt_dev_get_thumbnail_size()` resyncs it and folds
   `modify_roi_out` over its nodes (`dt_dev_pixelpipe_get_roi_out`), publishing the result to
   the geometry record and the ROI request.
2. **"Where is this image point on screen?"** (and back) — `dt_dev_distort_transform_plus()` /
   `_backtransform_plus()` walk its pieces calling each distorting module's transform, for
   every overlay: mask outlines, crop handles, ashift lines, liquify widgets, the pointer.

Measured on a darkroom open of a 6016×4016 NEF (`-d perf`, isolated configdir):

- first virtual resync: **0.329 s (2.06 s CPU)** on the GUI thread;
- steady-state resync (per history commit, inside the throttled drain): **0.10–0.20 s**;
- of the steady-state 0.10 s, **≈73 ms is two modules building pixel state the virtual pipe
  can never read**: `colorequal`'s CLUT (55–86 ms) and `colorprimaries`' CLUT (17–22 ms).
  `lut3d`'s `commit_params` reads its LUT **from disk**. All of that is paid on the GUI
  thread, per commit, for a pipe that renders nothing.

## What actually consumes it (audited)

- `dt_dev_get_thumbnail_size()` — the ROI fold. Needs `piece->enabled` plus committed params
  for the **15 of 95** modules that implement `modify_roi_out` (crop, clipping, ashift, lens,
  flip, liquify, borders, rotatepixels, scalepixels, spots, retouch, demosaic, rawprepare,
  basebuffer, and the template).
- The transform walkers — need committed params for modules with `distort_transform`, a
  subset of the same 15.
- Module GUIs asking for **their own piece** (`dt_dev_distort_get_iop_pipe(virtual, self)`:
  graduatednd, ashift, crop, clipping, …) — every audited site reads only **dimensions**
  (`buf_in`/`buf_out`, `iwidth`/`iheight`), which come from the ROI fold, never
  `piece->data`. `graduatednd` is the reason a *node-filtered* pipe is wrong: it is not a
  geometry module, but its overlay needs its piece's dims, so every module must keep a node.
- `dev_history.c`'s `last_history_item` bookkeeping — pointer identity only.

**Nobody reads pixel state from the virtual pipe.** `iop/colorout.c` already encodes this as
a per-module early-out: on the virtual pipe it commits `d->type = DT_COLORSPACE_LAB` and
returns before building any LCMS transform.

## Why this is #1157's accomplice

The throttled history drain runs, on the GUI thread: shutdown → history write → pipe flags →
**virtual resync (0.1–0.3 s)** → publish of the matching ROI request. Everything between the
history write and the publish is a window in which the worker can sync the new history while
still holding a latch on the old geometry. The fix for #1157 (publish re-arms the pipes; the
worker re-checks its latch) makes a mixed frame transient instead of terminal — but the
window itself IS the virtual resync's duration. Shrink or remove it and the residual
one-frame flash goes with it.

## Options

### A. Keep the pipe, stop paying for pixels (small, incremental, recommended first)

Apply the `colorout` pattern to the measured offenders: `commit_params` early-outs on
`pipe == dev->virtual_pipe` for `colorequal`, `colorprimaries`, `lut3d` (and any module a
`-d perf` pass shows building derived pixel state). Each early-out must still commit the
fields its own `output_format()`/contract logic reads — that is why this is per-module and
audited, not a blanket skip: a blanket "skip commit_params for non-geometry modules" leaves
`piece->data` zeroed, and `dt_dev_pixelpipe_propagate_formats()` then reads garbage contracts
and can disable different modules on the virtual pipe than on the real ones — silent geometry
divergence.

Expected: steady-state virtual resync 0.10–0.20 s → **~0.03–0.05 s**. The #1157 residual
window shrinks proportionally. No consumer can notice: none reads what is skipped.

### B. Worker-authoritative sizes (the structural fix; next tranche)

The worker already computes the processed size at the end of **every** resync
(`dt_dev_pixelpipe_get_roi_out` at the tail of `dt_dev_pixelpipe_change()`); the virtual pipe
recomputes the same number on the GUI thread so it is available *immediately*. Invert the
authority:

- the worker publishes `(history_hash, processed_width, processed_height)` as one seqlock
  record after each resync — history and its geometry become **atomic**, which is the
  invariant #1157 was missing;
- `dt_dev_get_thumbnail_size()` stops resyncing the virtual pipe for sizes and shrinks to
  "derive and publish the request from the latest worker geometry";
- the drain's flag→publish gap collapses to microseconds;
- the virtual pipe remains only for transforms and own-piece dims, resynced **outside** the
  commit path (lazily on first overlay use, or where the drain does it today — but no longer
  blocking the publish).

Cost: the fit scale for a geometry commit is derived one worker-resync later (~0.15 s). With
the #1157 flags this is invisible — the worker re-plans as soon as the publish lands, which
is exactly what happens today anyway for the *pixels*; only the number arrives with them
instead of before them. Risk: every consumer of "processed size" must tolerate reading a
value one commit older than `dev->history` for that window — the audit of those consumers is
the real work of this tranche.

### C. A geometry service instead of a pipe (Qt-horizon)

Transforms as data: each of the 15 geometry modules publishes, at commit time, a serialized
transform (crop rect, homography, lens polynomial, …) into a per-image list; the GUI composes
transforms and sizes from the list with no pipe, no pieces, no nodes. Kills the virtual pipe
entirely and makes overlay math backend-independent — the shape a Qt frontend wants. This is
a refactor of all 15 modules plus the mask/overlay call sites; not a bug-fix-adjacent change.

## Recommendation

A now (three audited early-outs, verifiable from one `-d perf` run), B as its own tranche
with the consumer audit, C when the frontend split gets there. A and B compose: A makes the
remaining GUI-thread resync cheap for the transforms that stay, B removes it from the sizing
path where it does the #1157-class damage.
