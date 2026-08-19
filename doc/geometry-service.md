# The geometry service — transforms as data, no virtual pipe

Decision record and tranche plan. The maintainer chose option C of `doc/gui-sizing.md`
(on the #1162 branch; its measurements are restated here where needed): replace
`dev->virtual_pipe` — a full pixel-less clone of all ~95 IOP modules plus history, resynced
0.10–0.33 s on the GUI thread per history commit — with a service that composes GUI-side
transforms and sizes from small per-module records. The pixel pipes keep their piece-based
`modify_roi_*` / `distort_*` machinery for rendering; the service replaces only the GUI side.

Everything below is grounded in a six-way source survey (2026-08-18); file:line citations
are from that survey and were spot-checked. Read the traps in §3 before touching anything —
each one killed a simpler design on paper.

## 1. What must be replaced (the inventory)

**Sizes.** `dt_dev_get_thumbnail_size()` (develop.c:347) resyncs the virtual pipe inline,
folds `modify_roi_out` at scale 1.0 from full raw dims (`dt_dev_pixelpipe_get_roi_out`,
dev_pixelpipe.c:512), publishes `processed_*` into the geometry seqlock record and derives
the ROI request. ~18 call sites (module GUIs, history navigation, view enter,
`_commit_gui`'s `modify_roi` gate).

**Point transforms.** `dt_dev_distort_transform_locked` / `_backtransform_locked`
(develop.c:1686/1715): an iop_order-bounded fold over enabled pieces, 5 direction modes
(ALL, FORW_INCL/EXCL, BACK_INCL/EXCL), gated per-module on
`!dt_dev_pixelpipe_activemodule_disables_currentmodule`. GUI consumers: the two coordinate
converters (raw↔image, DIR_ALL), every mask-GUI wrapper routed through them, and the
geometry-module GUIs (crop, clipping, ashift, graduatednd, liquify, drawlayer/coordinates,
lens `gui_update`) with module-relative FORW_EXCL/INCL bounds.

**Per-module dims.** `dt_dev_distort_get_iop_pipe(virtual, self)` + `buf_in`/`buf_out`
reads. NOT limited to geometry modules — `graduatednd` (no geometry callbacks) reads its
own `buf_out` for its overlay (graduatednd.c:294-330). The size fold's side effect
(writing per-piece `buf_in`/`buf_out`, dev_pixelpipe.c:527/551) is itself a consumed
product: the service must publish input/output rects for **every enabled module**.

**One data read.** `lens.cc:2547-2560` (`gui_update`) reads the committed
`dt_iop_lensfun_data_t` blob off its virtual piece — the single consumer of more than
dims+transforms. The lens record serves it, since the record *is* that data.

**The module roster.** 15 modules implement `modify_roi_out`; 11 implement
`distort_transform`; `retouch`/`spots` implement **neither** (identity `modify_roi_out`,
source-patch `modify_roi_in` used only by rendering pipes) and stay out of the service.
`initialscale`/`finalscale` have no `modify_roi_out` at all — complete no-ops in the
scale-1 fold. `demosaic` changes size only for its DOWNSAMPLE method (halves dims, no point
transform). `basebuffer` is pure plumbing. The roster cannot be computed at runtime: the
DEFAULT-macro binding gives every module non-NULL no-op geometry callbacks
(common/module_api.h:47-49), so the roster is a source-level audit, maintained in the
service.

## 2. Architecture

New directory module `src/develop/geometry/` (develop layer). One object per dev:

    dt_geometry_chain_t          — GUI-thread-only; no locks by design (see §3.7 for the
                                   one non-GUI pixel-less pipe that stays out of scope)
      └─ ordered list of dt_geometry_record_t:
           { op, instance (multi_priority), iop_order, enabled,
             data        — module-published blob, owned by the record,
             free_fn     — non-NULL where data owns resources (lens's deep lfLens copy),
             vtable      — pure evaluators:
               transform(data, dims, chain_ctx, points, n)
               backtransform(data, dims, chain_ctx, points, n)
               map_size(data, w_in, h_in → w_out, h_out) }
      └─ per-record in/out dims for EVERY module (geometry or not), filled by the
         chain's own size fold — this is what replaces buf_in/buf_out reads.

**Composition** reproduces today's walker exactly: the 5 direction modes, iop_order
bounds compared per instance, and the activemodule exception evaluated **at query time**
from live GUI state — never baked into records (§3.1). `chain_ctx` hands an evaluator the
ability to compose the upstream sub-chain: liquify requires it (§3.4).

**Publication.** A new optional module API — working name `geometry_record()` — invoked by
the chain rebuild on the GUI thread. THE ONE-CONSTRUCTOR RULE: a module does not get a
second derivation of its geometry. The fields `commit_params()` derives for geometry
(crop's clamped rect, flip's EXIF-resolved orientation + enabled bit, ashift's 11 floats,
lens's data struct) move into a shared static helper that BOTH `commit_params` (into
piece->data) and `geometry_record` (into the record) call. Same for the evaluators: the
existing `distort_transform` bodies become thin piece-unpacking wrappers around the pure
helpers the vtable uses. The math cannot fork — this is the CPU/GPU-divergence lesson
applied before the divergence exists.

**Rebuild sites** = every site that today calls `dt_dev_get_thumbnail_size()` or resyncs
the virtual pipe: the history drain, bulk history paths (pop, compress, truncate, load),
edit-mode toggles (crop/clipping/ashift — they already resync there), the transient-params
channel (§3.5), and undo (§3.6, currently missing). The rebuild is cheap by construction —
geometry records are POD-sized derivations, no CLUTs, no LCMS, no disk — so it lands in
the SAME step as the history write, which closes #1157's residual window (the 0.1–0.3 s
publish gap) by construction.

**The provider seam.** Mask border builders (brush.c:1238 vs 2661, polygon.c:1231 vs 24xx)
and crop's `_set_max_clip` take a pipe parameter and run with BOTH the virtual pipe (GUI)
and the worker's own pipe (rasterization). They get a
`dt_distort_provider_t { ctx, transform(), backtransform() }` parameter instead, with two
implementations: piece-based (workers — unchanged behavior) and chain-based (GUI). The
worker-side mask path never touches the service.

**Shadow mode.** Until G8, the virtual pipe stays alive and the chain runs beside it.
Under `-d geometry` (and in Debug builds), every chain query re-runs on the virtual pipe
and divergence is logged with module/instance/op detail; sizes are compared on every
rebuild. The chain becomes authoritative for a query only when every enabled
geometry-roster instance in the current history has a record — wholesale, never per-module
(mixing chain records with virtual pieces inside ONE composition is wrong by interleaving).

## 3. The traps (each killed a simpler design)

1. **Query-time GUI state.** `dt_dev_pixelpipe_activemodule_disables_currentmodule`
   (dev_pixelpipe.c:498-509) reads `dev->gui_module`, its `operation_tags_filter()` and
   its live cache-bypass flag — and the size fold *also* applies it, mutating
   `piece->enabled` mid-fold (:531-532), so the PROCESSED SIZE changes with focus, with no
   history commit. Records are data; the exception is a composition-time parameter, and
   edit-mode toggles are rebuild sites.
2. **Commit-time GUI state.** Crop neutralizes its rect while `g->editing`
   (crop.c:419-427); ashift likewise, from `g->new_params` that never exist in history
   (ashift.c:5530-5577). `geometry_record()` reads the same gui_data the shared
   constructor reads — publishing from history alone is wrong exactly when overlays
   matter most (edit mode).
3. **Derived-state modules.** `clipping` and `scalepixels` mutate `piece->data` inside
   `modify_roi_out`/`modify_roi_in`, and clipping's `distort_transform` re-runs
   `modify_roi_out` on a shallow piece copy writing through the shared data pointer. Their
   records need the derived fields (homography coefficients, tx/ty, enlarge_x/y, scales),
   which today have NO pure constructor — their tranche starts with that refactor. Two
   further leaks: `rotatepixels`' output size depends on the interpolator preference
   (`dt_interpolation_new(USERPREF)->width`) — captured at publish time; `clipping`'s
   transform has a ×100 precision hack gated on `dt_dev_pixelpipe_has_preview_output(pipe)`
   — a per-pipe-type behavior to resolve (likely: the GUI/chain path takes the
   high-precision branch unconditionally; decide in its tranche with a shadow diff).
4. **Liquify is self-referential.** Its warps live in RAW coordinates; `distort_transform`
   re-enters the walker (BACK_EXCL of its own iop_order) to bring paths into its input
   space, then rasterizes a dense O(warp-area) displacement map per call
   (liquify.c:631-655, 1176-1271). The chain must support nested composition
   (`chain_ctx`), and inherits the cost — same as today, no regression, but no win either.
5. **Transient/realtime params bypass history.** Crop/ashift edit drags publish through
   `dt_dev_transient_params_set` (dev_history.c:2820-2835); the drawlayer heartbeat too.
   The transient channel is a rebuild site, with the same defer-during-realtime policy
   `dt_dev_get_thumbnail_size` has today (develop.c:367-374) so strokes never pay it.
6. **Undo is inconsistent today.** `_pop_undo` never refreshes the published sizes — it
   relies on incidental later callers (dev_history.c:648-697 vs 1378-1386). The service
   fixes this as a G1 pre-fix rather than reproducing the hole.
7. **Non-GUI pixel-less pipes exist.** `gui/dtgtk/focus.h:267-283` builds its own throwaway
   dummy pipe on a control-job thread around a headless dev — it does NOT use
   `dev->virtual_pipe` and stays piece-based, out of scope. `studio_capture` runs a second
   gui_attached dev with its own virtual pipe — same dev machinery, migrates for free.
8. **A worker reads the virtual pipe.** `ashift`'s `process()`/`process_cl()`
   (ashift.c:3200, 3340) do a 2-point backtransform on `dev->virtual_pipe` from the
   pipeline thread for flip detection. G1 pre-fix: use the running pipe's own pieces.
9. **`enabled` is not pure history.** `propagate_formats` can auto-disable modules on
   contract mismatch (dev_pixelpipe.c:1142-1205) — dsc simulation a pipe-less service
   cannot replicate. Policy: the chain derives the only geometry-relevant case (demosaic
   & raw-domain modules enabled iff the image is mosaiced — image metadata) and shadow
   mode validates the policy; any divergence found in the wild is a policy bug to fix,
   not a reason to re-grow a pipe.
10. **Scale changes have no transform.** demosaic's DOWNSAMPLE halves dims with no point
   transform; consumers today compensate through per-piece buf dims. The chain's dims
   tables reproduce this; nothing new — but it is why dims, not just transforms, are part
   of the service contract.

## 4. Tranches — as executed

The original plan is preserved in this file's history; what follows is what actually
happened, because the order changed three times and each change was a measurement, not a
preference. Reading the old list as a map of the work would mislead.

Each is one PR. Shadow mode must be silent on the exercised paths before the next lands.
Ratchet: `grep -rc "virtual_pipe" src/` may only fall — it stays flat while both paths are
live and falls at deletion.

**Landed for review**

- **G1 — pre-fixes** (#1164). ashift's `process()`/`process_cl()` stop backtransforming
  through the GUI's pixel-less pipe from the worker thread (trap 8); `_pop_undo()` publishes
  the geometry like every other bulk history path — a real bug on its own, since undoing a
  crop left the fit scale describing the pre-undo image.
  *Dropped from this tranche:* drawlayer's raw `virtual_pipe->processed_*` reads. They are
  not a redundant duplicate of the geometry record — they prefer the pipe's field as the
  FRESHER of the two, and `_pop_undo` proves that was rational. Deferred to the consumer
  migration, where one source exists and the question dissolves.
- **G2 — skeleton** (#1165). `src/develop/geometry/`: record, chain, the two walkers with all
  five direction modes, the size fold, the query-time focus exception, the `geometry_record()`
  module API, the roster, and shadow mode. Non-authoritative; behaviour identical.
  Shadow mode is what makes an otherwise dead skeleton verifiable: it names, per image, which
  roster modules still owe a record.
- **G3 — first authority** (#1166): crop, flip, rawprepare, demosaic, basebuffer.
  **Composition chosen by the harness, not by this plan.** It named the five modules gating
  the test image, three of them always-enabled infrastructure the plan had scattered across
  two later tranches — and nothing can become authoritative without those three. Shadow mode
  gained five transform probes here, so it compares coordinates and not only sizes.
- **G4 — lens + the cost** (#1167). Lens jumped the queue because after G3 it was the ONLY
  module still gating every CR2, ARW and DNG tried. Its record is the one that is not plain
  data (a deep `lfLens` copy with a `free_fn`), which is why measuring the cost with it in is
  the honest test. Measured, five images: virtual pipe ~0.105 s against chain 0.03–5 ms, i.e.
  20× to 3500×, the worst case being images where lens is active — its lensfun database
  lookups are the whole of that 2–5 ms and are a memo waiting to happen.
- **G5 — records read history** (#1169). Not in the original plan at all: shadow mode caught
  the chain and the pipe disagreeing while crop's piece was mid-transition. Every
  `geometry_record()` was reading `self->params` and the rebuild was filtering on
  `module->enabled` — GUI-thread live values, ahead of the pipes across the 200 ms commit
  throttle. The chain now resolves each module exactly as the pipe does, and a disabled roster
  module owes nothing.
- **G6 — rotatepixels, borders, ashift** (#1170). The three that are pure functions of their
  parameters and the rectangle handed to them.
- **G7 — clipping and scalepixels** (#1171). The two that derived state inside their ROI
  callbacks and reached it, from paths that plan no ROI, through a shallow piece copy whose
  `data` pointer still aliased the real one. scalepixels turned out to need no piece at all —
  the dimensions cancel and its scales are a pure function of the pixel aspect ratio.
- **G8 — liquify** (#1172). The roster is complete. The only module needing the chain composed
  around itself: its warps live in RAW coordinates, so it re-enters the chain
  (`dt_geometry_chain_compose()`, bounded `BACK_EXCL` of its own iop_order) exactly as it
  re-enters the pipe walker. This is the first and only caller of the `chain` argument every
  evaluator has carried since G2.
- **G9 — the coordinate converters** (#1173). First consumer migration.
  `dt_dev_coordinates_raw_abs_to_image_abs()` and its inverse — the whole of the mask GUI's
  coordinate handling — read the chain, falling back to the pipe when it declines.
  `_sync_virtual_pipe()` rebuilds the chain alongside the pipe so the two cannot differ in
  freshness.

**Remaining**

- **G10 — module GUIs and per-module dimensions.** ~75 sites: ashift (27), clipping (16),
  drawlayer and drawlayer/coordinates (12), crop (6), liquify (4), graduatednd (4), lens (1).
  Most resolve their own piece through `dt_dev_distort_get_iop_pipe()` to read `buf_in`/
  `buf_out`; the chain already publishes per-module in/out rects for EVERY module, so those
  are repoints. `graduatednd` is the standing reminder that this is not only geometry modules.
  `lens.cc`'s `gui_update()` is the one site that reads a committed data blob rather than
  dimensions — served by the record, which is that data.
- **G11 — the dual-use folds.** brush, polygon, circle, ellipse (8 sites). These take a pipe
  as a parameter and run with BOTH the virtual pipe (GUI) and the worker's own pipe (mask
  rasterisation), so they need the provider seam rather than a repoint: one implementation
  backed by pieces for workers, one backed by the chain for the GUI.
- **G12 — the size path.** `dt_dev_get_thumbnail_size()` still folds the virtual pipe for the
  processed size. Only when every consumer above has moved can the resync be dropped — and
  that is where the measured cost difference stops being a table and becomes something a user
  feels, and where #1157's residual window closes by construction, the virtual resync finally
  leaving the history-commit path.
- **G13 — deletion.** `dev->virtual_pipe`, `_sync_virtual_pipe()`, the teardown boilerplate in
  darkroom's and studio_capture's `leave()`, the realtime hash fast-forward special case, and
  the fallbacks in the migrated consumers. Ratchet target: 0.

## What the measurements changed, and why that matters

Three times the harness overruled this document: G3's composition, lens jumping the queue,
and G5 existing at all. That is the design working as intended — shadow mode was built so the
source audit could be checked against reality per image, and each time reality had something
the audit did not. Keep ordering the remaining tranches the same way.

## The verification gap, stated plainly

Everything so far rests on export A/B, shadow agreement and darkroom-open runs. None of that
covers what a user would notice first: a mask drawn, dragged, and landing where it was put.
From G9 onward the migrated consumers are GUI-side, and an export never calls them — so the
bit-identical exports say only that nothing ELSE regressed. Interactive verification is owed,
and it gets more load-bearing with every further tranche.

## 5. Out of scope, recorded so nobody rediscovers them

- GUI-thread readers walking the **preview** pipe's worker-owned nodes lock-free
  (color_picker_proxy.c:206/295, toneequal/hazeremoval piece reads): a pre-existing
  hazard the virtual-pipe removal neither fixes nor worsens. The chain could serve some
  of these later; candidates, not commitments.
- focus.h's dummy pipe (trap 7).
- Worker-side mask rasterization and `distort_mask` (a pixel-buffer contract, asymmetric
  across modules): stays piece-based forever — it belongs to rendering, not to GUI
  geometry.
