<!-- Provenance: every finding carries the commit it was established against. -->

# RAW-domain ROI, CFA phase and tiling

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> What goes wrong when a module reads the sensor mosaic at an offset it did not expect.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

Related: [`resizing-scaling.md`](resizing-scaling.md).

## `piece->iwidth`/`iheight` go stale on the export pipe specifically

*Found `c0cc14d008`, 2026-07-10.*

`dt_dev_pixelpipe_create_nodes()` copies `pipe->iwidth`/`iheight` into each `piece->iwidth`/`iheight`
once, at node-creation time — it is not refreshed on later ROI passes. Darkroom pipes call
`dt_dev_pixelpipe_set_input()` (which sets `pipe->iwidth`/`iheight`) before creating nodes; the
export pipe (`imageio/imageio_core.c`) does the reverse, so every piece was permanently stuck at 0 there
(issue #967: `iop/toneequal.c`'s blending radius and `iop/soften.c`'s glow radius silently collapsed
to 0 on export only, regardless of the module's params, while darkroom rendered correctly). Fixed by
having `dt_dev_pixelpipe_set_input()` re-sync `iwidth`/`iheight` onto any already-created nodes. See
`doc/resizing-scaling.md` for the full write-up; any other per-piece field seeded from `pipe->*` at
node-creation time is exposed to the same ordering hazard.

## `basebuffer` must crop using `roi_out`, not `roi_in`

*Found `07c2ce1d96`, 2026-07-16.*

`iop/basebuffer.c` is the first module in the pipe: it slices the requested window out of the
full-resolution mipmap-cache payload. Its `modify_roi_in()` unconditionally requests the whole
image (`{0, 0, pipe->iwidth, pipe->iheight}`) — `roi_in` never carries an offset, since basebuffer
needs the full frame available to crop from. The window actually requested downstream lives in
`piece->roi_out`, not `piece->roi_in`. `process()`/`process_cl()` must read the crop offset (and
the destination copy size) from `roi_out`, and use `pipe->iwidth`/`iheight` — not `roi_in->width`/
`height`, which is always the full frame too — for the source row stride. Reading the offset from
`roi_in` instead always crops from the sensor's true `(0,0)`: harmless whenever the requested
window is itself near `(0,0)` (a fit-to-screen view, a barely-cropping module), silently wrong by
the full requested offset otherwise (e.g. `iop/lens.c`'s `scale` slider, whose backward-pass
`roi_in.x/y` grows with the zoom amount) — every downstream module still looks internally
consistent (sizes match, ROI planning round-trips cleanly), because each of them only reads
buffer-relative pixels and never re-derives its own absolute position from `pipe->iwidth`/
`iheight`. Parametric masks/forms don't go through this buffer-cropping path at all, so they stay
correctly positioned even when the base image content is offset — a mismatch between a mask and
the image it's drawn on is a symptom of this class of bug, not of the masking code.

## A "CPU vs GPU parity bug" in a tiled module is usually a tile-grid dependence

*Found `21ccc1b8c5`, 2026-08-02.*

`tiling->xalign`/`yalign` do more than preserve the CFA phase: `develop/tiling.c` rounds tile
sizes *and* the overlap down/up to `lcm(xalign, yalign)`, so tile origins land on multiples of
that value and nothing else. Every lattice a module lays over its tile — the CFA, but also any
binning, pyramid or block grid — is anchored to the tile origin, because that is the only origin
`process()`/`process_cl()` is handed. If `xalign` is smaller than a lattice's period, two
different tile decompositions bin the same sensels into differently-phased cells, and the result
changes **everywhere inside the tile**, not just near the seams. No amount of overlap fixes it.

That is what looked like an OpenCL parity bug in `iop/rawdenoiseai.c`: the module's multi-scale
model bins to superpixels (period 4 Bayer / 6 X-Trans) for its coarse guide and fuses low bands
on a 16/32/64 px pyramid, while `xalign` was only the CFA period (2). CPU and GPU budget memory
differently, so they tile differently — GPU tile rows started at y = 986 (`986 % 4 == 2`) — and
the coarse guide was computed on a half-bin-shifted lattice. A second, independent defect
compounded it: `_apply_low_band_anchor()` chose its coarsest fusion band from the *padded tile
size* (64 if it divided, else 32), so a 2-tile grid fused at 64 while a 16-tile grid fused at 32,
diverging structurally from the training-time reference (`cfa.fuse_low_bands`, always 16/32/64).
Fixed by folding the lattice periods into `xalign`/`yalign` and `DT_NN_FUSION_COARSEST` into
`dt_nn_model_alignment()`, making the level count a constant. Exported 8-bit CPU-vs-GPU went from
mean 0.022 / max 29 to mean 0.0029 / p99 0 / max 10.

The diagnostic that settles this class of bug in one measurement: **run the same export twice on
the same device with different `host_memory_limit`.** If two CPU runs differ by the same amount
as CPU-vs-GPU, the device is irrelevant and the tiling is the variable. Chasing it as a
synchronisation problem instead — `dt_opencl_finish` at every plausible point, blocking readbacks
between stages, sleeps — costs hours and moves nothing, because the arithmetic was never wrong.

Two residual tile-grid dependencies in that module are known and *not* fixed: the fusion's
per-channel mean σ² is a whole-tile reduction (the torch reference is patch-global too, so the C
mirrors it faithfully — but at inference the "patch" is whatever tile the pipe chose), and
`roi_in->x/y` is not itself lattice-aligned, so a non-zero ROI offset shifts every lattice
relative to the sensor. The fully correct form is to anchor them to absolute sensor position from
`roi_in`, the same way the CFA phase rule below does.

Note *where* a non-zero RAW-domain ROI offset can come from, because the obvious guess is wrong:
**it is never the viewport.** `iop/initialscale.c` (iop_order 15.5) is `default_enabled` with
`IOP_FLAGS_NO_HISTORY_STACK`, so it runs in every pipe, and its `modify_roi_in()` hard-resets
`roi_in->x = roi_in->y = 0` at `piece->buf_in` dimensions and `scale = 1.0f`. ROI planning runs
backwards, so every module below it — `lens` (15.0), `demosaic` (8.0), `rawdenoiseai` (2.5),
`basebuffer` (0.5) — is handed offset 0 no matter how the user pans or zooms. A non-zero offset
reaches the RAW domain only from a module *between* it and `initialscale` that grows its own
`roi_in` on the backward pass: in practice `iop/lens.c` (distortion, TCA, and the `scale`
slider). So "it only misbehaves when zoomed in" is the wrong mental model for this whole class of
bug; "it only misbehaves with lens correction enabled" is the right one.

## CFA phase (Bayer/X-Trans) is computed fresh per crop, not snapped — demosaic and highlights alike

*Found `dda78fd161`, 2026-07-20.*

Once `basebuffer` honors the real crop offset, that offset reaches every pre-demosaic RAW-domain
module unrounded (`demosaic` itself, but also anything upstream of it in `iop_order` that reads the
CFA, e.g. `highlights`) — it is almost never a multiple of the sensor's repeating pattern (2 px for
Bayer, 6×6 px for X-Trans). Getting the per-pixel color identity wrong for such an offset produces
wrong colors or, if the wrongness compounds, fully scrambled blocks of pixels. The phase handling
is split across two places, each owning a different, non-overlapping part of the total shift:

- `iop/rawprepare.c`'s `_update_output_cfa_descriptor()` folds in only the **fixed sensor border
  trim** (`d->x`/`d->y`, constant for a given camera/image, independent of how the user pans,
  zooms, or crops). It writes the result to `piece->dsc_out.filters`/`xtrans`, which propagates
  forward to demosaic's `piece->dsc_in` through the normal `input_format()`/`output_format()`
  contract — never through a shared, pipe-wide field.
- Every consumer's `process()`/`process_cl()` folds in the **dynamic, ROI-dependent** part, fresh
  on every call, from the module's own current `roi_in->x/y`, via the shared helper
  `dt_dev_get_roi_filters(piece, roi_in)` (`develop/imageop.c`, next to `dt_dev_get_module_scale()`).
  This must happen in `process()`/`process_cl()`, not in `modify_roi_in()`/`output_format()`: those
  ROI-planning callbacks run before `piece->dsc_in` is guaranteed to be populated for this resync —
  and, more fundamentally, `dsc_in`/`dsc_out` are settled once by a single pipe-wide pass that runs
  independently of per-tile ROI refinement, so a value baked in there would be correct for at most
  one tile and silently wrong for every other tile once the piece is large enough to get tiled
  (`IOP_FLAGS_ALLOW_TILING` modules get `process()`/`process_cl()` called once per tile, each with
  its own `roi_in`, but `modify_roi_in()`/`output_format()` only once for the untiled request). It
  also must never write back into `piece->dsc_in`/`dsc_out` — those are sealed contracts, read-only
  once processing starts (see the `dt_dev_pixelpipe_iop_t` doc comment in `pixelpipe_hb.h`). The
  result — a locally rotated `xtrans_raw`-derived table, or the `filters` word `dt_dev_get_roi_filters()`
  returns — is a plain local variable, discarded at the end of the call, recomputed next time.
  Call `dt_dev_get_roi_filters()` for every new tile-local consumer instead of inlining
  `dt_rawspeed_crop_dcraw_filters(piece->dsc_in.filters, roi_in->x, roi_in->y)` again — it now has
  two call families (demosaic's own algorithms below, and `iop/highlights.c`'s laplacian/harmonic
  Bayer reconstruction, CPU and OpenCL) and duplicating the one-liner a third time is how this class
  of bug keeps reappearing instead of getting fixed once.

Every algorithm that reads the CFA — in demosaic or elsewhere — falls into exactly one of two
categories, and mixing them up is the recurring failure mode here:

- **Self-correcting**: the algorithm takes `roi_in`/explicit `x,y` alongside the filters/xtrans
  table and adds the offset itself at each color lookup (`FCxtrans(row, col, roi_in, xtrans)`,
  `FC(row + roi_in->y, col + roi_in->x, filters)`). These must receive the **unshifted**
  `piece->dsc_in.filters`/`xtrans` (`xtrans_raw` in `process()`) — passing them an already-shifted
  table double-applies the offset. VNG, Markesteijn/FDC, passthrough-color, the X-Trans downsample
  path, and green-equilibration (CPU functions and their OpenCL kernel counterparts) are all in
  this group.
- **Tile-local**: the algorithm addresses pixels in buffer-relative coordinates with no ROI
  awareness at all (`FC(row, col, filters)` where `row`/`col` are local loop indices). These need
  the **fully pre-shifted** `filters` from `dt_dev_get_roi_filters(piece, roi_in)`, computed once
  at the top of `process()`/`process_cl()`/`process_rcd_cl()` — passing them the raw, margin-only
  table silently drops the dynamic part of the shift. RCD, LMMSE, PPG, AMaZE, and the Bayer
  downsample path (CPU and OpenCL) are in this group, and so is `iop/highlights.c`'s laplacian and
  harmonic-transposition Bayer reconstruction (CPU, and the OpenCL host code that feeds the shared
  `interpolate_and_mask`/`remosaic_and_replace` kernels — those two kernels have no ROI-offset
  argument of their own, unlike `highlights_normalize_reduce_first`, which does and stays
  self-correcting on the raw table). Bayer has no xtrans-table equivalent of this split:
  `dt_rawspeed_crop_dcraw_filters()` already no-ops on X-Trans (`filters == 9u`), so
  `dt_dev_get_roi_filters()` is always safe to call regardless of sensor type.

  The GPU harmonic-transposition kernels (`hl_knee_bin`/`hl_knee_apply`,
  `data/kernels/highlights_harmonic.cl`) are a self-correcting design instead — they take the raw
  table plus explicit `region_x`/`region_y` args (bound to `roi_in->x/y` host-side) and are meant to
  add them at the lookup, same as `FCxtrans`. One CFA-identity ternary in each kernel
  (`is_xtrans ? FCxtrans(region_y + row, region_x + col, xtrans) : FC(row, col, filters)`) added the
  offset only on the X-Trans branch and left the Bayer `FC()` branch reading unshifted `row`/`col` —
  correct by construction on X-Trans, wrong on Bayer for any `roi_in` not itself CFA-aligned. Both
  branches of that ternary must add the same `region_y +`/`region_x +` offset.

A third, narrower trap lives inside `xtrans_markesteijn_interpolate()`/`xtrans_fdc_interpolate()`
(`iop/demosaic/markesteijn.c`, CPU and OpenCL builders alike): the `allhex[3][3][...]` neighbor-
geometry table is precomputed once per call from `FCxtrans(row, col, ·, xtrans)` for `row`/`col` in
`0..2`, then looked up later via `hexmap()`/`allhex[row][col]` using **tile-local** (`roi_in`-
relative) coordinates taken mod 3. Building that table with `NULL` (no offset) puts it in a
different phase than the tile-local lookup expects whenever `roi_in->x/y` isn't itself a multiple
of 3 — main per-pixel colors stay correct (they go through their own `roi_in`-aware lookup), but
the geometric neighbor relationships used to actually interpolate are wrong, producing a subtler,
locally-blotchy color artifact rather than a full scramble. The fix is to build `allhex` with
`roi_in` instead of `NULL`, matching the phase `hexmap()` will later assume.

Because every algorithm is now verified correct for an arbitrary, unaligned crop offset, demosaic's
`modify_roi_in()` no longer snaps the requested position to the sensor pattern (the old
`XTRANS_SNAPPER`/`BAYER_SNAPPER` rounding is gone). That snap was never a correctness requirement
of the phase math — it was a blunt instrument that kept the offset congruent to 0 mod its period,
which incidentally made every one of the bugs above unreachable by construction. Removing it is
what actually exercises non-aligned offsets and is how these bugs were found; reintroducing a
similar snap anywhere in this path would silently mask a regression here rather than fix one.

---
