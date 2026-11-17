# The drawlayer module

> **Verified against `f37105c227` on 2026-09-29.** That pass found 9 claim(s) in this file
> that were wrong of the tree and 17 that had gone stale; the ones corrected since carry a note
> saying what they used to say. Anything not yet corrected is flagged inline. Re-measure before
> acting on a claim older than the code you are changing, and re-date this line when you do.

`src/iop/drawlayer.c` plus `src/iop/drawlayer/` is a painting layer: a full-resolution RGBA
canvas per layer, a brush that stamps dabs into it, a sidecar TIFF that stores it, and a
composite into the pipe. This note is the map — what the parts are, which thread owns what,
and where a realtime stroke's time goes. It was written from a full read of the module; every
number below is derived from the code and cites it.

**Re-verified against the tree on 2026-09-29, after #1418, #1430, #1431 and #1471, and
corrected again the same day against `1cfa1551c8`.** The second pass found six claims still
wrong — two of them describing defects the refactor had already *fixed*, which is the worse
direction: §2's `self->params` finding and §7's dead-function list. Citations drift as the code
moves, and this note has now shipped a finding that named the wrong struct, one whose count was
stale, and two that survived their own repair — so re-measure before acting on any claim, and
say so here when you do.

A mechanical check worth re-running: every `file.ext:NNN` in this document should resolve to a
line that exists. All of them do as of `1cfa1551c8` — but note a **bare** `:NNN`, continuing a
previous citation, is invisible to that check and is how `drawlayer.c:4238` survived in §2 for a
file of 4169 lines. Write the filename out. Sections 1, 6 and 7 carry a note
where their earlier version was wrong rather than being quietly corrected, because the *way*
each was wrong is the more useful thing to know.

---

## 1. Eleven translation units, one per file

*This section used to be titled "the module is not the directory" and described four files
text-`#include`d into `drawlayer.c`. #1431 removed that; the numbers below are 2026-09-29.*

`src/iop/CMakeLists.txt` compiles **all eleven** `.c` files. There is no `#include` of a `.c`
anywhere in the module, so a `static` really is private to its file and the directory buys the
encapsulation its layout suggests.

What that cost, and why it is worth keeping: as one unit the module was 7130 lines, a `static`
in `worker.c` shared a namespace with one in `drawlayer.c`, and ten alias macros existed so
pasted code could call a public function by a private-looking name (`_commit_dabs` for
`dt_drawlayer_commit_dabs`). Compiled alone the four spliced files raised 819 errors, every one
a header they had been borrowing from their host without naming it. Three headers could not be
included first either: `coordinates.h` declared twelve functions over `dt_iop_module_t`,
`dt_iop_roi_t` and `dt_drawlayer_brush_dab_t` and included **nothing**; `worker.h` named types
it never pulled in; `runtime.h` held a `dt_drawlayer_cache_patch_t` by value without
`cache.h`.

Actual units, by role:

| file | lines | role |
|---|---|---|
| `drawlayer.c` | 4169 | module API, GUI, the whole OpenCL composite, `process`/`process_cl` |
| `worker.c` | 1828 | the `draw-back` thread, the ring, batching, the heartbeat |
| `brush.c` | 1196 | the per-pixel rasterizer |
| `io.c` | 1101 | sidecar TIFF read/write |
| `paint.c` | 994 | pointer samples → dabs (interpolation, spacing, smoothing) |
| `runtime.c` | 973 | the event→schedule→action dispatcher |
| `widgets.c` | 851 | widget construction helpers |
| `layers.c` | 489 | layer CRUD, the GUI-side canvas loader |
| `conf.c` | 375 | preferences, and the tablet-mapping widget↔key pairing |
| `coordinates.c` | 366 | coordinate spaces |
| `cache.c` | 246 | patch allocation over the pixelpipe cache arena |

---

## 2. Four threads, and what each may touch

```mermaid
flowchart LR
  GUI["GTK main thread<br/>events, widgets, conf,<br/>layer CRUD, sidecar writes"]
  RING(["raw-input ring<br/>65536 x 128 B = 8 MiB<br/>worker.c:1245"])
  WK["draw-back thread<br/>interpolate, rasterize,<br/>publish heartbeat"]
  OMP["OpenMP team<br/>forked per batch"]
  PIPE["DT_CTL_WORKER_DARKROOM<br/>process() / process_cl()"]
  CANVAS[("base_patch<br/>W*H*16 B<br/>pixelpipe cache entry")]

  GUI -->|push| RING --> WK
  WK --> OMP --> CANVAS
  WK -->|wrlock, damage rows| CANVAS
  PIPE -->|rdlock, whole process| CANVAS
  WK -->|transient params<br/>+ TOP_CHANGED| PIPE
  GUI -->|alloc / clear / replace| CANVAS
```

**Ownership is not clean, and that is the module's central defect.** The same `base_patch` is
written by the worker (`worker.c:961-970`), read by the pipeline across a whole `process()`
(rdlock `drawlayer.c:1621`, released through the `dt_drawlayer_runtime_release_t` set at
`drawlayer.c:4049`, `:4129` and `:4149`), and replaced by the GUI (`layers.c:199-213`). The
entry's rwlock guards the *pixels*; nothing guards the *patch struct* that holds the lock.

Two things cross a thread boundary they should not:

- ~~**`self->params` is written by the worker thread.**~~ **Corrected 2026-09-29: it is not,
  and has not been since the realtime rework.** `_publish_backend_progress` reads
  `&ctx->worker->publish_params` (`worker.c:326`) — a worker-private blob declared at
  `worker.c:118`, seeded from `self->params` by the GUI thread at stroke begin
  (`dt_drawlayer_worker_snapshot_params`) and owned by the worker thereafter. Its own doc
  comment states the rule this finding was written about: "The heartbeat must never touch
  `self->params`: that blob belongs to the GUI thread." The finding is kept, struck through,
  because the *shape* is right and recurs — feeding the transient channel by mutating the
  module's live params is not the sanctioned route — but the module does not do it. The GUI
  thread writes the same blob from `_widget_changed` with no lock in common.
- **`cache_dirty_rect` has no synchronisation at all.** Written by the worker
  (`worker.c:364`, `:973`), read *and reset* by the pipeline (`drawlayer.c:741`, `:746`,
  `:952`). Plain 20-byte struct, no mutex, no atomic, no barrier.
- **The pipeline writes `self->params` and opens the sidecar.** `PROCESS_*_BEFORE` schedules
  `ensure_layer_cache` (`runtime.c:641-647`), which begins by `_sanitize_params` on
  `self->params` from the pipeline thread (`layers.c:136`).

---

## 3. A live stroke, end to end

> Two labels in this diagram were corrected on 2026-09-29. The **26 conf reads per event** were
> real when first measured but are not what `_fill_input_brush_settings` does: they live in
> `_refresh_brush_settings_cache` (`drawlayer.c:206-247`), which the filler calls at
> `drawlayer.c:252` only when `g->ui.brush_settings_valid` is false. And the drain loop has
> honoured the input path's publish deadline since `worker.c:1282` — the "Fixed" list in §4 says
> so, and the diagram was still showing the state before it.


```mermaid
flowchart TD
  M["mouse_moved<br/>drawlayer.c:3690"] --> BRS["_fill_input_brush_settings<br/>drawlayer.c:248<br/>cache refill only when stale"]
  BRS --> PUSH["ring push"]
  PUSH --> PBI["_process_backend_input<br/>worker.c:325"]
  PBI --> LUT["arc-length LUT<br/>25 full dab structs<br/>paint.c:239"]
  LUT --> EMIT["emit D dabs<br/>D = travel / spacing<br/>spacing = 1 px at defaults"]
  EMIT --> Q{"publish deadline?<br/>>= 20 ms"}
  Q -->|no| WAIT["accumulate in pending_dabs"]
  Q -->|yes| BATCH
  WAIT -.->|worker idle| DRAIN["_backend_worker_on_idle<br/>drains, same 20 ms deadline<br/>worker.c:1282"]
  DRAIN --> BATCH
  BATCH["_rasterize_pending_dab_batch<br/>B = 2 x nthreads dabs<br/>worker.c:885"]
  BATCH --> CP1["copy base_patch -> heartbeat_patch<br/>batch bbox"]
  CP1 --> RAST["_rasterize_dab_batch_outer_loop<br/>parallel for over dabs<br/>+ per-tile omp locks<br/>worker.c:790"]
  RAST --> CP2["wrlock: copy damage back<br/>+ clear scratch"]
  CP2 --> PUB["_publish_backend_progress<br/>worker.c:296"]
  PUB --> TP["dt_dev_transient_params_set<br/>+ TOP_CHANGED + redraw"]
  TP --> PROC["process() / process_cl()<br/>drawlayer.c:4134 / :3983"]
  PROC --> SCREEN["screen"]
```

### The cost model

With `r` = brush radius (conf default **64**, `conf.c:78`), `s` = spacing
(`1 + distance/100 * (2r-1)`, `paint.c:64`; `distance` default **0** ⇒ **s = 1 px**), `T` =
`omp_get_max_threads()`, `B = 2T` dabs per batch (`worker.c:898`):

- dab footprint `A = (2r+2)² = 16 900 px`, of which `πr² = 12 868` are inside the disc;
- **overdraw = 2r/s = 128** — every stroke pixel is composited 128 times;
- per batch at `T=16`: `32 × 12 868 = 411 776` full RGBA read-modify-writes.

---

## 4. How the rasterizer works, and what it used to cost

A stroke pixel used to be composited about 128 times. It is now composited **once per
batch**, and the reason is algebra rather than tuning.

With the stroke mask present and the internal flow at 0 (UI Flow 100%, the default), the
per-pixel alpha is `capped = min(ba, (cap − s)/(1 − s))` with the mask update
`s' = 1 − (1 − capped)(1 − s)`. Both branches of that min collapse into one closed form —

> **s' = min( 1 − (1 − ba)(1 − s),  cap )**

because the cap is *absorbing*: once it binds, every later dab leaves `s` at `cap`. Over a
batch that composes to

> **s_final = min( 1 − Πᵢ (1 − baᵢ) · (1 − s₀),  cap )**

a product of per-dab transmittance factors, clamped once. So `dt_drawlayer_brush_rasterize_batch`
(`brush.c`) runs two passes: pass 1 accumulates `T ·= (1 − ba)` into a scalar plane — four
bytes per pixel, no destination load, no `powf` — and pass 2 walks the batch box once and
does a single float4 composite.

Three properties of that, in order of how easy they are to lose:

- **Transmittance, not coverage.** `T ·= (1 − ba)` reproduces the existing sequence of
  operations. The algebraically identical `A = 1 − (1 − A)(1 − ba)` would drift, because
  `1 − (1 − x)` is not exact in binary floating point. Measured against the per-dab path
  across dense, cap-binding, sparse and ERASE cases: **1.192e-07**, one ULP at 1.0 in float32.
- **The product is commutative, so order stops mattering** — which is what lets both passes
  run over disjoint rows with no locks at all, and makes the result bit-identical between one
  thread and many. The scheme it replaced used an `omp_lock_t` per 128 px tile, which gave
  mutual exclusion but never *ordering*; since the per-pixel alpha depends on the running
  stroke alpha, that path produced a different picture run to run.
- **The derivation is gated, because it is conditional.** `dt_drawlayer_brush_batch_is_uniform`
  requires one mode (PAINT or ERASE), one opacity, one colour and Flow at 100%. SMUDGE and
  BLUR never qualify — they read the destination per dab. Everything refused falls through to
  the per-dab loop unchanged.

`tests/unittests/test_drawlayer_batch_raster.c` pins all of this, including the gate's
refusals and the thread-count independence.

**Measured, one heartbeat batch at the defaults** (r=64, spacing 1, 32 dabs, 8 cores):

| | per-dab | batch |
|---|---|---|
| plain | 21.0 ms | 1.6 ms |
| sprinkles 0.6 | 131.9 ms | 3.3 ms |

The old figures are the interesting ones. A plain stroke was *above* the 20 ms heartbeat
budget, so the rasterizer alone could not keep up with its own publish rate; a textured one
was **6.6× over it**, which is what "the brush lags" means in practice.

### What the per-pixel loop no longer does

Three things used to sit in it and no longer do.

- **A `powf`**, feeding `accum_alpha`, which `_lerpf(·, ·, 0)` discarded at the default Flow.
- **Four per-dab constants recomputed per pixel** — `hardness`, `min_inner`, `inner` and the
  transition width — inside `dt_drawlayer_brush_profile_eval`.
  `dt_drawlayer_brush_profile_prepare` resolves them once per dab onto the runtime view and
  `..._eval_fast` performs the identical arithmetic on the identical operands, so the floats
  are unchanged. Worth ~20% of the per-dab path and ~2× of the batch path.
- **The sprinkle field, evaluated per dab per pixel.** `_cellular_grain_2d` is 9 cells × 4
  `splitmix32` per octave, up to three octaves — **up to 108 hashes per pixel** — and it is a
  function of *layer* position and the stroke seed alone, so every overlapping dab sampled the
  same value. The batch now evaluates it once over the batch box, which divides it by the
  overdraw factor; the per-dab `alpha_noise_gain` stays per dab. That is what the uniformity
  gate's sprinkle-parameter checks are for.

What remains per pixel is a `sqrtf`, a shape `switch`, and the transmittance multiply.

## 5. Where else the time goes

Fixed:

- The 26 `dt_conf_get_*` per pointer motion are now one resolve per *change*
  (`_refresh_brush_settings_cache`). `dt_conf_get_float` runs `dt_calculator_solve` — an
  expression parser — on the stored string at every call.
- The 32-point radial quadrature (32 `asinf`, 32 `sqrtf`, ~96 divisions) is memoised on
  (radius, hardness, shape, spacing); with no tablet map on size or softness it is computed
  once per stroke.
- The `dev->geometry_chain` walk that ran from the worker thread is deleted. (`wx`/`wy`
  themselves are **not** deleted and were never on `dt_drawlayer_brush_dab_t`: they are the
  pointer's widget coordinates on `dt_drawlayer_paint_raw_input_t` (`paint.h:51-52`), read at
  `worker.c:192` and in `drawlayer.c`. What went was the worker-side geometry walk that used
  to translate them.)
- The drain loop honours the same 20 ms publish deadline as the input path. It used to
  publish after every batch: a transient-params write, a `TOP_CHANGED` flag and an
  asynchronous signal raise apiece.
- Two unconditional `dt_get_wtime()` per dab now happen only when the trace is on.

Not fixed:

- **The batch bounding box is still computed twice**, in two different coordinate frames:
  `_collect_batch_bounds` works against the full canvas, the inner pass against the
  heartbeat patch. Deduplicating means translating between them.
- **`process()` composites the whole `roi_out` every frame** and never consults
  `cache_dirty_rect`, while `process_cl()` does. The win is smaller than it looks:
  `dt_interpolation_resample` takes a 1:1 fast path (`pixel/interpolation.c:946`) at the zoom
  levels people paint at, so
  only the alpha-over blend is full-frame there. It is worth most when zoomed *out*. A
  damage-limited CPU composite also cannot copy the GPU gate: that one depends on the output
  cacheline being rekeyed in place, which `cache_output_on_ram` prevents on the CPU path.
- **Per layer create: two complete sidecar rewrites** (`drawlayer.c`) — on a 6000×4000 canvas
  with 3 layers, ~1.15 GB of half-float traffic through zlib, twice, on the GUI thread.
  `io.c` takes no lock anywhere and uses a fixed `<path>.tmp` name while being reachable from
  three threads.

## 6. State is twelve booleans, not a state machine

*Re-measured 2026-09-29; the line numbers below are current, the previous ones had drifted
off their targets entirely. Check them before quoting them — this section has been wrong once.*

The logical state — idle / hovering / painting / draining / committing / loading / saving —
is spread across **twelve** independent booleans in five structs, with no enum and no asserted
invariant:

| struct | booleans | `runtime.h` |
| --- | --- | --- |
| `dt_drawlayer_session_state_t` | `pointer_valid`, `background_job_running` | `:20`, `:30` |
| `dt_drawlayer_process_state_t` | `cache_valid`, `cache_dirty`, `base_patch_loaded_ref`, `last_composite_valid` | `:44`, `:45`, `:51`, `:69` |
| `dt_drawlayer_stroke_state_t` | `last_dab_valid`, `finish_commit_pending` | `:77`, `:80` |
| `dt_drawlayer_ui_state_t` | `brush_color_valid`, `brush_settings_valid` | `:124`, `:126` |
| `dt_drawlayer_runtime_manager_t` | `realtime_active`, `painting_active` | `:255`, `:256` |

Beside them sits a 15-boolean schedule, `dt_drawlayer_runtime_schedule_t` — declared in
`runtime.c`, not the header. `_build_runtime_schedule` (`runtime.c:462`) maps an event onto
it and the dispatcher executes the set bits. It recomputes its whole state twice per dispatch
and pushes realtime mode twice.

**`realtime_active` is NOT a cached pure function**, which an earlier version of this section
claimed. It is computed from three inputs (`runtime.c:345`), then *overridden* to FALSE for
four event kinds (`:356`), and written FALSE again from two commit-time sites outside that
computation (`drawlayer.c:1878`, `:1894`). Replacing it with a predicate would lose both
overrides. Note the pairing at those two sites — the flag and
`dt_drawlayer_set_pipeline_realtime_mode(self, FALSE)` are set together, while four other
places derive the pipeline mode *from* the flag (`runtime.c:434`, `:443`, `:679`, `:880`).
Two spellings of one operation, and the shape this module keeps paying for.

`brush_color_valid` deserves its own warning: it is set TRUE once (`drawlayer.c`, in
`dt_drawlayer_sync_cached_brush_colors`) and **never cleared anywhere**. Nothing rebuilds the
cached brush colours lazily, so every path that changes what they depend on must refresh them
explicitly. Until #1471 the refresh happened only because refilling the HDR exposure slider
woke `_widget_changed` on the panel paths that were not frozen — an accident the frozen path
never had.

---

## 7. Dead weight

*Drained across #1430, #1431 and #1471. What is left is at the bottom; the rest is kept as a
record, because two of the original entries were WRONG and the way they were wrong is worth
knowing.*

**Removed:**

- `dt_drawlayer_runtime_host_t.collect_inputs` / `.perform_action`, never dereferenced,
  assigned at 19 sites (#1430). `runtime.h` keeps a comment where they were.
- 389 of `cache.c`'s 642 lines — a second "process patch" cache tier with no caller outside
  the file (#1430).
- Three of the nine uncalled exported functions (#1430).
- `drawlayer_process_scratch_t.flush_update_rgba`: a pointer, a size beside it and a free.
  Nothing ever allocated it, so nothing read it and the free was on NULL (#1471).
- `direct_copy`: one assignment in the whole tree, `FALSE`. The GPU fast branch was
  unreachable, the `!direct_copy` term in the partial-composite predicate always true, and
  the CPU path's `if(!source.direct_copy)` wrapped 25 lines that always ran (#1471).
- `_blend_layer_over_input_cl`'s constant arguments, then its parameter list (#1471). See the
  correction below.
- `gui_init`'s 332 lines, now 72, split into three tab builders that each connect their own
  widgets (#1471). The 3×4 tablet-mapping grid it was already table-driven for is now ONE
  list — `dt_drawlayer_mapping_rows()` — walked by the builder, by `gui_update` and by
  `sync_params_from_gui`, where each used to spell it out.
- The runtime manager's `background_job_running`, written twice and read never. The
  *session*'s identically-named field is the live one, which is why this survived: a grep for
  the name finds eleven uses and looks busy (#1471).
- Three `fill_*` widgets that had exactly create/pack/connect, whose handlers ignore the
  button they are given (#1471).

**Two entries in this list were wrong. Verify before acting on any of the rest.**

- `dt_drawlayer_brush_dab_t.wx`/`.wy` "written at three sites, read at none" named **the wrong
  struct**. The dab type has no such fields. The writes are to
  `dt_drawlayer_paint_raw_input_t`, and they ARE read — `paint.c` quantises them into the dab
  hash, and `worker.c` and `drawlayer.c` convert them to layer coordinates. Acting on this
  entry would have deleted the pointer coordinates that place every dab.
- `_blend_layer_over_input_cl` "takes 19 parameters, three provably constant" was 16 and two
  by the time anyone reached it, because `direct_copy` was the third and had already gone.
  Both remaining constants gated real code: `force_device_copy` selected an eager
  `dt_opencl_copy_host_to_device()`, and `source_mem_override` had a branch plus **four**
  guards written to work whether or not it was set. It now takes one named
  `drawlayer_blend_cl_request_t`.

**Still open:**

- ~~**Six exported functions have no caller anywhere**~~ — **three**, re-measured
  2026-09-29: `dt_drawlayer_io_background_layer_job_run`,
  `dt_drawlayer_paint_runtime_get_stroke_seed`, `dt_drawlayer_worker_raw_inputs`. The other
  three on the original list gained callers when the TU splice was undone and they stopped
  being reachable by private name: `dt_drawlayer_brush_transition_mass_primitive_eval`
  (`brush_profile.h:210`), `dt_drawlayer_brush_profile_prepare` (`brush.c:290`),
  `dt_drawlayer_brush_mass_primitive_eval` (`paint.c`). That is the splice's signature one last
  time — a "dead" export that was live all along, invisible because the caller and the callee
  shared a namespace.

---

## 8. Two hazards that were not performance

Both fixed; recorded because the shapes recur.

**The worker could deadlock itself and take the GUI down with it.**
`_backend_worker_on_idle` called `dt_drawlayer_commit_dabs` (`worker.c:1234`) →
`dt_drawlayer_worker_wait_idle` (`worker.c:1415`) — the private-looking spellings `_commit_dabs`
and `_wait_worker_idle` are from the splice era and exist nowhere in the tree — which blocks
while
`ring_count > 0` — and the ring's only consumer is the worker's own loop. It read the
"ready" predicate under the mutex, dropped it, then committed, so a GUI push in that window
made the wait real. The only escape, `worker->stop`, is written by `_stop_worker` *after* it
calls `_wait_worker_idle` too, so the next GUI-side commit hung the GUI thread on the same
predicate. The worker now posts the commit to the GUI thread and decides-and-posts under one
lock acquisition; `dt_drawlayer_worker_wait_idle` carries a tripwire. Note the cheap fix — making the
wait a no-op on the worker thread — is *wrong*: at that point the ring may hold a new
stroke's events, and the commit would wipe that stroke's session state.

**A never-cleared arena page was published as a valid canvas.**
`dt_drawlayer_cache_patch_alloc_shared` allocates but does not clear, and
`_refresh_piece_base_cache` set `cache_valid = TRUE` without clearing when it never attempted
a load. The GUI-side twin had always cleared; the two loaders had diverged. Both key the same
cache entry, so an uncleared page created by the pipeline is adopted by the GUI as the
layer's content and a later sidecar write makes it permanent. The clear stays at the call
site: moving it into the allocator would add a dead full-canvas memset on the rekey-conflict
path, which memcpys the whole buffer on its next statement.

Three more of the same family, also fixed: the worker wrote `self->params` (it now carries
its own blob, *seeded* from the GUI thread, because the hash is an accumulator); the damage
rectangle was published after releasing the write lock that wrote the pixels it describes;
and `dt_dev_pixelpipe_t.pause` was a plain `gboolean` written cross-thread beside three
fields that are atomics for exactly that reason.

Still open: the **pipeline thread writes `self->params` and opens the sidecar** —
`PROCESS_*_BEFORE` schedules `ensure_layer_cache`, whose first act is `_sanitize_params` on
`self->params`. Removing it is not a deletion: `g->process.cache_valid = TRUE` happens in
exactly one place, inside that function, so a display pipe would never composite the layer
again without a GUI-thread request path to replace it.

## 8b. The canvas was resolved before the orientation was knowable

*Established 2026-11-16 against `dcb82baf2a`, on a 7380x4928 Nikon raw held portrait with one
painted layer.*

A lipstick stroke on the lips came out in the hair above the temple, by the same offset at every
zoom, and a full-resolution export carried a washed-out band across the bottom third of the frame
starting at exactly row 4928. One cause.

`_flip_swaps_axes()` (`coordinates.c`) asks flip's size fold whether this image's axes are
swapped, reading `piece->buf_in`/`buf_out` on a pipe and the geometry chain's record off one. It
returned a plain `gboolean`, so **"upright" and "cannot tell yet" were the same answer**. Those
rectangles are written by `dt_dev_pixelpipe_get_roi_out()`, and the canvas is resolved before
that runs. Measured, in one CLI render, the same function on the same pipe answering twice:

```
[DL-FLIP]  found=1 in=0x0       out=0x0       -> swap=0     <- canvas resolved here
[DL-ALLOC] have_pipe_geom=1 resolved=7380x4928 info=4928x7380
[DL-FLIP]  found=1 in=7380x4928 out=4928x7380 -> swap=1     <- correct, too late
```

The flip piece *is* found, so the old `in.width <= 0` guard read "not written yet" as "no swap",
and `dt_drawlayer_layer_canvas_for_pipe()` reported **success** with a transposed canvas. Because
it succeeded, `_refresh_piece_base_cache`'s fallback — the sidecar page's own dimensions, right
there in `info` and correct — was never consulted. The patch was then allocated 7380x4928 and
cached, and the cache's own validity check re-hashes with the patch's *own* width and height, so
it is self-consistent and never notices.

Two symptoms, one transpose:

- **The paint moves.** The layer is sampled through a mapping whose axes are swapped, so every
  stroke lands somewhere else — by an offset that does not depend on the render scale, which is
  why it looked identical in the thumbnail, the preview and the export.
- **The export reads unallocated heap.** `_interpolation_resample_plain()` takes a `memcpy` fast
  path when `roi_out->scale == 1.f`, which is the export. It copies `roi_out->height` rows at
  `roi_in->width` stride **with no bounds check**, so 7380 rows were read out of a 4928-row
  buffer: everything from row 4928 on is whatever followed it in the arena. Below scale 1 the
  generic path runs instead, and it clips every tap into the source (`BORDER_REPLICATE`), so the
  smaller pipes showed misplaced paint but no garbage. That is the whole of "export only" — it
  was never export-only, it was *out-of-bounds* only at export.

The fix is that `_flip_swaps_axes()` now reports whether it could answer at all, and an
unresolved orientation fails the canvas rather than defaulting to upright. The two answers are a
transpose of each other, so guessing is not a near miss.

The same mistake was also sitting in the fallbacks: four callers substituted
`geometry.raw_width/raw_height` when resolution failed. The raw pair is **unrotated**, so that
names a transposed canvas for every portrait image — the identical defect, one level up, waiting
for resolution to start failing. They now go through `dt_drawlayer_canvas_from_sidecar()`, which
reads the dimensions the stored page was authored at, and fail when there is no page yet.

**Since closed, and it was not drawlayer's:** that `memcpy` fast path would read past the end of
its input for *any* caller whose output window exceeded its input, silently, at scale 1 — the
generic path clips, the fast path did not, and which one runs is decided by `roi_out->scale`
alone. `_interpolation_resample_plain()` now tests containment first: a contained window keeps
the straight-line copy it always had, and anything else replicates the edge, which is the answer
the generic path already gives. `tests/unittests/test_interpolation_resample_window.c` pins it.
The control matters more than the test: against the pre-fix resampler **6 of its 7 cases fail**,
and only the contained one passes, with valgrind naming the read — `Invalid read of size 8 ... at
memmove ... interpolation.c:955`, which is that `memcpy`. After the fix, 7/7 and `ERROR SUMMARY:
0 errors`.

## 8c. The brush colour is pipeline RGB; only the screen sees anything else

*Established 2026-11-16 against `53488dd8e5`. The eyedropper paragraphs were amended the same
day on top of `aad29756a2`, where the gesture moved into the picker proxy; what changed, and
what turned out to be wrong, is marked where it stands.*

The brush colour used to be kept in **display space** — conf held a triple converted through
`dt_ioppr_get_pipe_output_profile_info()`, and painting converted it back the other way. It is
now kept in **working (pipeline) RGB**, which is the space picking already produces and painting
already consumes, so the path that matters does no conversion at all.

What the old arrangement cost, in the order the symptoms appear:

- **Soft-proofing made picking and painting disagree.** `iop/colorout.c` puts the pipe in a
  soft-proof mode only for `DT_DEV_PIXELPIPE_FULL` (`d->mode = (pipe->type == FULL) ?
  settings.mode : DT_PROFILE_NORMAL`). The picker runs on one pipe and the paint conversion read
  `self->dev->pipe`, so with proofing on, the colour laid down was not the colour picked.
- **A stored colour meant something different on every monitor**, because the profile it was
  encoded against was not stored with it. Changing the display profile silently moved the saved
  brush colour and the entire history.
- **The clamp happened in the wrong space.** Display values were clamped to [0,1] *before* the
  HDR exposure gain, so a pick off a specular highlight was flattened at pick time.

The model now: conf and `brush_pipeline_color` hold working RGB, unclamped above 1 and only
floored at 0. The HDR gain multiplies once, where the dab colour is produced, and is divided back
out on the picking path so that picking a patch this brush painted returns the colour chosen for
it. Everything the user *sees* **of the colour** — history strip, on-canvas cursor stamp — reads a
cached display twin. *(This sentence listed the gradient widget too. That was wrong, and was a
bug rather than a slip of the pen: see "Two triples for one colour" at the end of this section.)*

**The twin is cached, and the cache key is `dt_colorprofiles_settings_t.generation`.** Converting
costs an LCMS transform (`dt_colorprofiles_xyz_to_display()`, which is documented for exactly
this and takes the profile read lock for the span of the call), and a redraw must not pay it.
Two things about that key, both load-bearing:

- `generation` is advanced by the settings setters **and** by `_notify_profile_changed()`
  (`colorprofiles/colorspaces.c`), so moving the window to a screen with a different profile
  bumps it too. That matters because the `DT_COLORSPACE_DISPLAY` entry keeps its name and
  filename across such a change — only the bytes behind them differ — so a cache keyed on the
  profile's *identity* would serve the previous monitor's colours indefinitely.
- The recompute has to reach the widgets, not just `brush_display_color`. The history strip
  holds its own screen copies; refreshing the module cache alone leaves the panel showing the
  old monitor's colours until something unrelated touches them. *(The gradient widget was
  listed here as well, and the code did push the twin into it. It must not: same section.)*

A matrix of the display profile would have been cheaper and wrong: it is correct only for a
matrix/TRC profile and silently wrong for a LUT-based one, which is what a real colord monitor
profile usually is.

**Ctrl+click is the eyedropper**, and it borrows the module's own image picker rather than
sampling anything itself: the pixel it wants only exists part-way through a pipe run, and
`color_picker_apply()` already receives it.

**Aiming the sample is not enough, and that is the trap.** Writing
`dev->color_picker.primary_sample->point` sets *where* to look, but the pipeline only samples for
the picker **registered on the dev**, so an unarmed pick returns nothing at all — no error, no
callback, just a gesture that does nothing. The first cut of this did exactly that. It compounded
the mistake by trying to arm the picker with
`gtk_toggle_button_set_active(picker_widget, TRUE)`: the picker button's arming lives on
**`button-press-event`**, not on `toggled`, so setting the toggle flips the button's appearance
and nothing else. The cursor looked armed and sampled nothing.

Arming was first `dt_iop_color_picker_sample_point()`, written for this module alone. **It is
gone**: the commit after this one made the same gesture available to every module with a single
picker, and drawlayer now begins it with `dt_iop_color_picker_gesture_begin()` like any other
caller ([`color-picker.md`](color-picker.md) has the whole account). What that function
established still holds for its replacement: it takes **image-normalised** coordinates and lets
`dt_lib_colorpicker_set_point()` convert to the raw-normalised space the sample is stored in —
passing raw-normalised coordinates there samples a different pixel on any cropped or rotated
image — and it asks for the sample unconditionally, because that setter is a no-op when the point
did not move, which is exactly what a pick repeated on the same pixel looks like.

**The gesture is modal on the key, not one-shot on the click.** While the modifier is held every
click samples; the moment it comes up the tool is a brush again. That is what makes
sample-then-paint fast, and it is also the only shape that is safe:
`views/darkroom.c:button_pressed` tests the picker *before* it offers a click to the focused
module, so a picker left armed eats every following click and the brush stops painting with no
visible cause.

Which means **the disarm has to be real**, and that is a second place the button's state lies.
`gtk_toggle_button_set_active(picker, FALSE)` turns the button's *appearance* off and leaves
`dev->color_picker.enabled` TRUE — the picker goes on intercepting while looking switched off.
The disarm is `dt_iop_color_picker_reset(self, FALSE)`, which clears `enabled`, `picker`,
`module`, `widget` and `request_color_pick` together.

Nothing in the module API could see a key *release* — `dt_view_t` had `key_pressed` and no
`key_released`, and an IOP got neither — so **the release is now plumbed**, mirroring the press
at every level it already existed at: `OPTIONAL(int, key_released, …)` in `iop_api.h`,
`view_api.h` and `lib_api.h`, `dt_view_manager_key_released()` beside its press twin in
`views/view.c`, `dt_control_key_released()`, a `key-release-event` handler on both widgets that
already carry `key-press-event`, and `key_released()` in `views/darkroom.c` handing the event to
the focused module. The press half of that chain was always there; only the release was missing,
because nothing had yet needed to know that a key came back *up*.

Two things about that plumbing, both deliberate:

- **Every handler in the chain returns 0/FALSE.** A release carries no shortcut of its own, and
  the picking modifier is the prefix of half the application's shortcuts — claiming it would eat
  them. The handlers observe; they never consume.
- **Adding a name to `iop_api.h` is not enough on its own.** `tools/generate_iop_static.py`
  writes a `DT_MODULE_HAS_<fn>` for every API name into each module's generated `_present.h`,
  and the static-IOP binding is a compile error rather than a silent NULL when one is missing.
  A build tree that does not regenerate that header fails with
  `call to undeclared function 'DT_MODULE_PICK_DT_MODULE_HAS_key_released'`, which reads like a
  macro bug and is a stale generated file. GCC let it through here and clang did not.

**The mode is a flag, not a keyboard read.** `session.picking_active` is set by `key_pressed()`,
cleared by `key_released()`, and cleared again by `gui_focus()` in *both* directions — leaving,
because the release is delivered to the focused module and will never arrive once focus has
moved; arriving, because the modifier may have gone down while another module had focus, so this
one never saw the press and must not assume a mode it did not enter. `button_pressed()`,
`button_released()`, `mouse_moved()` and `gui_post_expose()` all branch on that one flag, so the
cursor and the click cannot disagree about which tool is in the user's hand. `button_pressed()`
additionally accepts the modifier bit out of its own `state`, which covers the same
focus-arrived-late case from the other side.

*Amended 2026-11-17 on top of `bf17acb633`:* **the mode is that flag AND a brush that lays down a
colour** (`_brush_is_picking()`), which is what all four now ask. The flag alone was the mode in
erase, blur and smudge too, where there is no colour to pick and the module's picker is not even
shown. Read from the code by an automated review (Codex) and confirmed by reading, not measured:
there `key_pressed()` ended the stroke under way and raised the flag, the cursor became the
cross, `mouse_moved()` stopped extending strokes, and `_pick_brush_color_at()` refused the pick
-- a brush that neither erased nor picked for as long as the modifier was held. Now
`key_pressed()` leaves those modes alone. Measured after, in the GUI: in erase mode the cursor
with the modifier held is pixel-identical to the one without, and a drag made with it held
erased the stroke it crossed (the crossing reads (30, 27, 27), the stroke either side (240, 75,
69)); in paint mode the cross still replaces the circle and a modifier-click changed the conf
colour from (0.5, 0.1, 0.05) to the sky's (0.216, 0.232, 0.224).

The disarm is still `dt_iop_color_picker_reset()`, but **it is no longer this module's to
call**, and how that was found is the useful part. The module reset its picker the moment the
modifier came up. Driven in the GUI with Ctrl released in the same instant as the click, that
threw the click away whenever the sample had not landed yet — and a sample takes 12 to 70 ms on
an idle pipeline, 290 ms and more with a stroke under way. The picker proxy now owns the whole
life of a picker the modifier turned on, and keeps it registered until its sample lands while
giving the image back at once ([`color-picker.md`](color-picker.md), *The modifier comes up
before the sample does*). `picker_armed_by_gesture` went with it: the proxy knows whose picker it
is (`gesture_armed`).

**A second defect hid behind that one.** `mouse_moved()` aborted any stroke while
`self->request_color_pick` was set, meaning "the picker is active". That flag says the picker is
*registered*, which now outlives its owning the image by the latency above — so a stroke begun
right after a pick was started by `button_pressed()` and aborted on its first motion, leaving
one dab where a line had been painted. It asks `dt_iop_color_picker_is_visible()` now.

**The cursor has to promise the gesture.** While the modifier is held the brush circle is
replaced by a cross with a gap at its centre, so the sampled pixel stays visible and can be aimed
at — a circle exists to show an area, which is the wrong thing to show when the next click reads
one pixel. `gui_post_expose()` draws it from `session.picking_active`, the same flag the click
branches on, and `key_pressed()` queues the redraw that makes the swap immediate rather than
waiting for the next pointer motion. *(An earlier state of this paragraph said the expose read
`dt_key_modifier_state()` live. It did, for one revision; the flag replaced it so that the cursor
and the click could not disagree, and the paragraph was not updated with it.)* The modifier is
`DT_PRIMARY_MASK` rather than `GDK_CONTROL_MASK`, so it is Cmd on macOS.

**Verified in the GUI:** the colours match between the panel and the painted image, and the
eyedropper works as a mode held on the key. The first version of it was tried in the GUI and
sampled nothing, which is how the arming trap above was found.

### Two triples for one colour, and two callers that mixed them up

*Established 2026-11-17 on top of `6e2f787835`, where an automated review of the PR (Sentry's
Seer) flagged both. Each was measured before it was touched; the probes were `fprintf`s in
`dt_drawlayer_sync_cached_brush_colors()` and next to the chooser's setter, run on the private
display with the brush colour seeded in conf as working (0.5, 0.1, 0.05).*

The gradient chooser is **an sRGB-defined map**: `_color_picker_set_from_position()` reads the
triple under the pointer as sRGB and converts it to working RGB. Its marker is a position in
those coordinates. So this module carries, for one brush colour, **three** triples — working
RGB (conf, the cache, what is painted), the monitor twin (history strip, cursor stamp), and the
chooser's sRGB — and the change to pipeline RGB above only renamed two of them. The chooser's
API went on calling its own triple `display_rgb`, and two mistakes followed from that one name.

**1. Two callers handed the chooser's triple to a function that takes working RGB.**
`dt_drawlayer_sync_cached_brush_colors()` took display RGB before the change and working RGB
after it. `gui_update()` and the HDR-exposure branch of `_widget_changed()` still fed it
`dt_drawlayer_widgets_get_display_color()`. Measured at darkroom entry:

| | cache given | conf holds |
|---|---|---|
| first sync (from conf) | 0.5000 0.1000 0.0500 | 0.5000 0.1000 0.0500 |
| second sync (`gui_update`, from the chooser) | **0.8900 0.2493 0.2115** | 0.5000 0.1000 0.0500 |

So every refresh of the panel replaced the brush colour by its own encoded value, and the next
stroke laid that down, until a colour was chosen again; the HDR slider did it once per step.
Both blocks existed to re-apply the HDR gain to the cached colour, which this same section had
taken out of the cache ("the HDR gain multiplies once, where the dab colour is produced"), so
they were deleted rather than converted, and `..._get_display_color()` went with them. After:
one sync, given what conf holds. Painted after a restart without choosing a colour again, the
layer in the sidecar holds 0.2749 0.3623 0.6577 at alpha 1 for a conf colour of 0.2750 0.3624
0.6575 (half-float storage).

**2. The chooser's marker was placed from the monitor twin.** Same triple on an sRGB display,
a different one on any other. Measured with `ui_last/color/display_type=2` (Adobe RGB):
working (0.5, 0.1, 0.05) is sRGB 0.8900 0.2493 0.2115 and was placed as 0.7704 0.2575 0.2243.
A drag in one half of the chooser keeps the other half's coordinate from that state (the
plane keeps the hue, the disc keeps the lightness), so each motion event carried the
displacement into the next colour. The marker is now placed from
`dt_drawlayer_brush_picker_srgb()`, the inverse of the conversion the pick uses, and
`_refresh_brush_display_color()` no longer moves it when the monitor profile changes. After,
on the Adobe RGB profile: the chooser is given 0.8900 0.2493 0.2115 while the twin is 0.7704
0.2575 0.2243, and a drag released at (1440, 600) in the disc and at (1590, 560) in the plane
leaves the marker on those two pixels.

The setter is `dt_drawlayer_widgets_set_picker_srgb()` now, and the chooser's parameters say
`srgb`. **What was not measured:** the drag on the build before the fix (the displacement is
known as triples, not as pixels), and anything on a LUT-based monitor profile.

**Not changed, and worth knowing:** the chooser paints its sRGB triples into its cairo surface
as they are (`dt_drawlayer_widgets_draw_picker()`), with no conversion to the monitor profile,
while the history strip and the cursor stamp are converted. Read from the code, not seen: on a
monitor whose profile is wider than sRGB the map should look more saturated than the colour a
click on it picks, unless the display server colour-manages the window itself.

**And one thing this section had let be assumed:** the twin is the colour converted to the
display profile, not the colour as the image will show it. The module sits before filmic in
the pipeline (`iop_order.c`: 41.7 against 46.0), so paint goes through every later module with
the image. Measured on the test raw's default history: working 0.2750 0.3624 0.6575 has the
twin (135, 162, 214) and reads (212, 228, 255) on screen once painted.

*This section used to end: "there is no headless path that paints a stroke or repaints a
swatch." **That was wrong**, and believing it is what let three rounds of this gesture be
written from reading alone. A headless compositor, a rootful Xwayland and XTest drive the real
GUI on a display of its own; [`color-picker.md`](color-picker.md), *How this was measured*, has
the recipe. It painted strokes, picked colours and found two defects reading had not.*

The conf format changed meaning without a migration, by decision, so a brush colour and history
saved before this commit are reinterpreted as working RGB and will look different once.

## 9. "Move the rasterizer to the GPU" — the verdict

No, not as the next step, and the reasoning is not about kernel speed. The dab stream is
produced by pointer events on the GUI thread; CLAUDE.md's *OpenCL GUI-thread materialization
hazard* forbids the GUI thread enqueueing work on a device it does not own; the canvas would
then live on the device and the CPU-side base patch would need syncing for every `process()`,
export and sidecar write; and the current split already keeps a 384 MB canvas resident in vRAM
per layer. The CPU rasterizer had a **~26× algorithmic factor and a ~16× parallelism factor**
on the table (§4), and **the algorithmic half has since been spent**: `2875569cc8` composites a
stroke pixel once per batch instead of 128 times, and `6b16818e79` hoists the per-pixel
constants the dab and the stroke already fix (both 2026-09-19, i.e. before this document's
2026-09-29 rewrite, which is why the claim read as open when it was not). The parallelism factor
is still there. The argument stands either way: spend what is left on the CPU first, because it
costs no new synchronisation and no vRAM.
