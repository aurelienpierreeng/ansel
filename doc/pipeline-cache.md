# Pipeline cache, GUI fetching and the cache-wait retry protocol

This document describes how the pipeline cache stores module outputs, how GUI code fetches
those buffers without blocking the interface, and the asynchronous **retry protocol** used when a
buffer is not available yet. It complements:

- `reorganisation.md`, which gives the high-level cache taxonomy (database / image / mipmap /
  pipeline) and the global locking model;
- `resizing-scaling.md`, which describes the ROI passes (`modify_roi_out()` / `modify_roi_in()`)
  and the difference between `piece->buf_*` and `piece->roi_*`.

The code lives in `src/develop/pixelpipe_cache.c` (the cache itself), `src/develop/dev_pixelpipe.c`
(the GUI fetch wrapper and the cache-wait manager), `src/develop/pixelpipe_hb.c` (the recompute
that publishes image cachelines), `src/develop/pixelpipe_raster_masks.c` (raster-mask side-band
retrieval), and `src/gui/color_picker_proxy.c` (the module color-picker's own `input_wait` /
`output_wait` consumer).

## 1. What the pipeline cache holds

Each enabled pipeline node (`dt_dev_pixelpipe_iop_t`, also called a *piece*) produces one output
buffer. That buffer is stored in the global pipeline cache, keyed by the piece's
`dt_dev_pixelpipe_iop_t.global_hash`. The hash is a checksum over everything the output depends on:
module parameters, blend/mask parameters, the input/output ROI, buffer descriptors, GUI states
(mask preview, cache bypass), and — for modules that opt in via `runtime_data_hash()` — the
committed runtime data blob `piece->data`. The exact chain is documented in `reorganisation.md`
(section *Pipeline cache*).

Two consequences matter for everything below:

- **The hash is the contract.** A consumer that knows a piece's `global_hash` can fetch its output
  from any thread. It does not need to know which pipeline run produced it, nor wait for a full
  pipeline to finish.
- **A piece's *input* is the previous enabled piece's *output*.** To read the buffer feeding a
  module, fetch the output of `dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece)`, not the
  module's own cacheline.

### Raster masks are dedicated side-band cachelines

A module may also publish raster masks for downstream modules or multi-page export. These masks
are not stored in `dt_dev_pixelpipe_iop_t` and are not embedded in the module's RGBA output
cacheline. They are independent single-channel float cachelines in the same global pipeline cache.

`dt_dev_pixelpipe_raster_mask_hash(piece, mask_id)` derives their key from:

1. the provider's `piece->global_mask_hash`, which already covers its upstream image state,
   blend parameters and ROI;
2. a raster-mask namespace tag, preventing aliasing with image outputs;
3. the provider-local mask identifier.

CPU and OpenCL blend paths publish the final provider mask under this key. A consumer calls
`dt_dev_get_raster_mask()`, which retains and read-locks the canonical cacheline, copies it into a
caller-owned working buffer, then applies the `distort_mask()` callbacks of enabled modules between
the provider and the consumer. The canonical cached mask remains immutable.

The pipeline keeps references to the raster-mask hashes required by its current graph in
`dt_dev_pixelpipe_t.raster_mask_hashes`. On a new render it first retains the new set, then releases
the previous set. This ordering prevents an unchanged mask from becoming briefly evictable between
provider publication and downstream consumption or export.

An image cache hit can therefore reuse its associated raster mask without recomputing the provider
or the modules before it. If the side-band mask was nevertheless evicted while the provider image
survived, an interactive consumer requests one bounded `DT_DEV_PIPE_REENTRY` pass. Immediately
before that retry, the pipe invalidates image cachelines from the provider through the end of the
graph, keeps upstream cachelines, and reruns without rebuilding synchronized nodes. A second miss
stops instead of scheduling a loop. Export enumerates the provider's declared mask IDs and fetches
the same dedicated cachelines; if one is unavailable, the format backend handles it as a missing
export mask instead of starting an interactive retry.

### Locking model recap

The cache has one short-lived manager mutex (held only while adding/removing/looking up cachelines)
and one read/write lock *per cacheline*. Reads are concurrent; writes are exclusive and wait for
all readers. A consumer that copies a cacheline must hold a reference and a read lock for the
duration of the copy:

```
dt_dev_pixelpipe_cache_ref_count_entry(cache, TRUE,  entry);  // pin: prevent eviction
dt_dev_pixelpipe_cache_rdlock_entry  (cache, TRUE,  entry);  // block writers while we read
... memcpy out of the cacheline ...
dt_dev_pixelpipe_cache_rdlock_entry  (cache, FALSE, entry);
dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, entry);
```

When a producer releases the *write* lock of a cacheline, `dt_dev_pixelpipe_cache_wrlock_entry()`
raises `DT_SIGNAL_CACHELINE_READY` with that cacheline's hash. This is the wake-up that the retry
protocol below is built on.

## 2. Two ways to read a cacheline

| Caller | Function | On a miss |
| --- | --- | --- |
| Backend / pipeline | `dt_dev_pixelpipe_cache_peek()` | returns `FALSE`, does nothing |
| GUI | `dt_dev_pixelpipe_cache_peek_gui()` | queues a *cache-wait* and asks the pipe to publish the buffer |

`dt_dev_pixelpipe_cache_peek()` is a pure probe. It is correct for pipeline code, which can simply
recompute what it needs, and for opportunistic GUI reads that have a fallback. **It is the wrong
tool for GUI code that *requires* a specific intermediate buffer**, because intermediate cachelines
are evicted as soon as nothing references them: the probe can keep missing forever even though the
final image is on screen. That failure mode is what produced the ashift *"Data pending – Please
repeat"* bug (issue #710): structure detection probed ashift's input with a raw `cache_peek()`,
missed because the input cacheline was not retained, and never recovered.

`dt_dev_pixelpipe_cache_peek_gui()` is the race-free GUI counterpart and the subject of the rest of
this document.

Backend code that must keep a cacheline past the lookup uses
`dt_dev_pixelpipe_cache_ref_entry_by_hash()`. Raster-mask retrieval follows this contract: retain
under the cache mutex, read-lock while copying, then release the temporary reference.

## 3. Requesting a partial recompute

The GUI fetch can ask a pipe to (re)publish one specific buffer without rendering the whole image.
Two request kinds exist (`dt_dev_pixelpipe_cache_request_t`):

- `DT_DEV_PIXELPIPE_CACHE_REQUEST_BACKBUF` — the pipe's final output;
- `DT_DEV_PIXELPIPE_CACHE_REQUEST_MODULE` — one named module's output in the middle of the graph.

`dt_dev_pixelpipe_cache_peek_gui()` sets the request with `dt_dev_pixelpipe_set_cache_request()` and
flags the pipe changed with `dt_dev_pixelpipe_or_changed(pipe, DT_DEV_PIPE_CACHE_REQUEST)`. On its
next run, `dt_dev_pixelpipe_process()` reads the request, resolves the target piece with
`_get_requested_piece_node()`, and runs **only up to that piece** (`requested_pos`). The piece's
output is published under its `global_hash`, which releases its write lock and raises
`DT_SIGNAL_CACHELINE_READY`.

This is why fetching a module *input* passes the *previous* piece to `peek_gui()`: the request then
targets the previous module, and the partial recompute stops exactly where the wanted buffer is
produced.

## 4. The cache-wait manager

The retry bookkeeping is centralised in a process-wide singleton, `_cache_wait_manager`
(`src/develop/dev_pixelpipe.c`). It owns a pending list of `dt_dev_pixelpipe_cache_wait_record_t`,
diagnostic counters (`queued_requests`, `served_requests`, `cancelled_requests`, `immediate_hits`,
`misses`), the global `DT_SIGNAL_CACHELINE_READY` subscription, and the darkroom busy-cursor state.

Each consumer owns a small, persistent handle, `dt_dev_pixelpipe_cache_wait_t`:

```c
typedef struct dt_dev_pixelpipe_cache_wait_t
{
  struct dt_dev_pixelpipe_t      *pipe;        // pipe that must publish the buffer
  const struct dt_iop_module_t   *module;      // target piece's module (NULL for backbuf)
  uint64_t                        hash;        // the cacheline hash being awaited
  dt_dev_pixelpipe_cache_ready_callback_t restart;   // resume callback
  gpointer                        user_data;   // passed back to restart()
  const char                     *owner_tag;   // debug label
  gpointer                        owner_object;
  uint64_t                        request_id;
  gboolean                        connected;   // TRUE while queued
} dt_dev_pixelpipe_cache_wait_t;
```

The handle is **caller-owned** and must outlive the request (store it in the consumer's GUI data,
not on the stack). It is the identity the manager uses to deduplicate repeated requests from the
same consumer.

### Hit / miss flow of `peek_gui()`

1. **Unsupported target** (realtime pipe, `no_cache`, or the target piece is in `bypass_cache`
   mode) → return `FALSE` immediately, no wait. Note: bypass only blocks fetching a *bypassed
   piece's own* output; upstream targets (a module's input) stay fetchable.
2. **Hit** — the cacheline exists: cancel any stale wait for this handle
   (`dt_dev_pixelpipe_cache_wait_cleanup()`), hand back the buffer and entry, return `TRUE`.
3. **Miss** — register/refresh the wait:
   - if the handle already targets the same `(pipe, module, hash, restart)`, do **not** re-emit the
     cache request (`request_cacheline = FALSE`); the previous request is still in flight. This is
     what stops an unsatisfiable target from being re-requested on every expose;
   - otherwise clean up the old target, fill the handle, append a record to the pending list,
     connect the manager to `DT_SIGNAL_CACHELINE_READY` (once, lazily), and raise the busy cursor;
   - emit the cache request (section 3) and return `FALSE`.

### Serving waiters

`_dt_dev_pixelpipe_cache_wait_ready_callback()` runs on every `DT_SIGNAL_CACHELINE_READY`. Under the
manager lock it removes every pending record whose `hash` matches the published hash, disconnects
the signal and clears the busy cursor when the queue drains, then **releases the lock before
invoking the restart callbacks**. Running callbacks outside the lock is deliberate: a restart
handler typically queues a redraw or issues a brand-new cache request, so holding the lock would
serialise unrelated GUI wake-ups and risk re-entrant deadlock.

A served handle is reset to an inert state (`connected = FALSE`, fields cleared) before its
`restart()` runs. The restart should simply *retry the original operation*: call `peek_gui()` again
(now usually a hit) and proceed. If it misses again — the cacheline was evicted between the signal
and the GUI getting scheduled — the retry transparently re-registers the wait, so the protocol is
self-healing.

### Threading

`DT_SIGNAL_CACHELINE_READY` and `DT_SIGNAL_HISTORY_RESYNC` are declared asynchronous in
`src/control/signal.c`, so even when raised from a pipeline worker thread their handlers are
marshalled to the GUI main thread via `g_main_context_invoke()`. Restart callbacks therefore run on
the GUI thread and may touch GTK widgets, set parameters, and flag pipes changed exactly like any
other GUI callback.

## 5. Recipe for a GUI consumer

To read a module's **input** buffer from the GUI without blocking:

```c
// 1. persistent handle in the consumer's GUI data
dt_dev_pixelpipe_cache_wait_t my_wait;   // zero-initialised; never on the stack

// 2. fetch
const dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(dev->preview_pipe, module);
const dt_dev_pixelpipe_iop_t *prev  = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);

void *buf = NULL;
dt_pixel_cache_entry_t *entry = NULL;
dt_dev_pixelpipe_cache_wait_set_owner(&my_wait, "my-consumer", module);   // debug label
if(dt_dev_pixelpipe_cache_peek_gui(dev->preview_pipe, prev, &buf, &entry,
                                   &my_wait, _my_restart_cb, user_data))
{
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
  dt_dev_pixelpipe_cache_rdlock_entry  (darktable.pixelpipe_cache, TRUE, entry);
  /* ... copy out of buf ... */
  dt_dev_pixelpipe_cache_rdlock_entry  (darktable.pixelpipe_cache, FALSE, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, entry);
}
// else: a wait is queued; _my_restart_cb() will be called when the buffer is ready.

// 3. teardown / reset / image change
dt_dev_pixelpipe_cache_wait_cleanup(&my_wait, "my-consumer-cleanup");
```

The restart callback retries the same operation:

```c
static void _my_restart_cb(gpointer user_data) { /* re-run the fetch + the work it gated */ }
```

Reference consumers:

- the **histogram** (`src/libs/histogram.c`) keeps several `dt_dev_pixelpipe_cache_wait_t` handles
  (`scope_wait`, `picker_wait`, `module_wait`) and is the canonical example;
- the **color picker** state lives in `dt_develop_t.color_picker` with its own `input_wait` /
  `output_wait`.

This API fits consumers that sample a buffer which the pipe naturally retains (a module output that
also serves as a histogram source or backbuffer). It is the **wrong** tool when the wanted buffer is
a transient intermediate that only exists while a specific module runs — see ashift in section 6.

## 6. Pitfalls and lessons (issue #710)

The perspective/horizon module (`src/iop/ashift.c`) exercises every sharp edge of this
infrastructure; its regressions are worth recording. Its needs differ from the histogram's: it wants
its *own input* while it is the focused, actively-edited module, and that buffer is a transient
intermediate. The lessons below explain why it ends up **not** using the cache-wait API.

- **A module cache request never runs the requesting module's `process()`.** `peek_gui()` resolves a
  module target to *the previous enabled piece* and the pipe runs only up to it (section 3). So a
  module that fills a GUI buffer inside its *own* `process()` (ashift captures `g->buf` there) cannot
  obtain it through `peek_gui()`: the partial render stops one module short. Worse, the published
  intermediate is unreferenced and can be evicted before the GUI restart reads it; the restart then
  re-requests, the partial render republishes-and-evicts, and the preview pipe aborts at
  *initialscale* and restarts **forever** (an intermittent hang, depending on cache pressure). This
  is why raw `cache_peek()`/`peek_gui()` on ashift's input was removed.
- **Prefer the module's own `process()` capture for its own input.** During edit ashift is in
  cache-bypass mode, so its `process()` runs on *every* preview render and copies its input into
  `g->buf` for free. The robust pattern is therefore: capture in `process()`; when a GUI action needs
  the buffer and it is not ready yet, queue the job and resume it from
  `DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED` (raised *after* `process()` ran, unlike
  `DT_SIGNAL_HISTORY_RESYNC` which fires *before* the render). No cache poking, no partial render, no
  spin. Reserve `peek_gui()` for buffers the pipe naturally retains (histogram/picker sources).
- **The `process()`-time "is this the full preview image?" guard must compare dimensions the *same
  way* the worker computes them — and must be orientation-aware.** ashift captures `g->buf` (and
  refreshes `g->isflipped`) only when `dt_dev_pixelpipe_has_preview_output(dev, pipe, roi_out)` is
  TRUE; that guard compares the module's `roi_out` to `dev->roi.preview_{width,height}`. Two
  independent defects each broke it on a *subset* of images — which is why "#710 acting crazy"
  reproduced on some pictures but not others, and why **resetting the module to neutral did not help**:
  1. *Round vs. truncate (primary, parameter-independent).* `dt_dev_get_thumbnail_size()` set
     `preview_width = (int)(natural_scale*processed_width)` — a C truncation — while
     `_update_darkroom_roi()`, which determines the ROI the worker actually requests (hence the
     backbuffer size, hence ashift's back-propagated `roi_out`), uses `roundf()`. They disagree by 1px
     whenever the fractional part is ≥ 0.5. Whether a given image lands there is luck of its
     dimensions and fit-scale, and is *independent of the module parameters*, so a reset cannot fix it.
     Fix: round `preview_{width,height}` in `dt_dev_get_thumbnail_size()` too, so both sides agree. The
     match is then exact — ashift's `roi_out` is back-propagated from the requested ROI through the
     `flip` swap, **not** taken from its own preview-scale `modify_roi_out()` `floorf`, so both sides
     ultimately derive from `processed_{width,height}`.
  2. *Portrait swap.* ashift runs *before* the `flip` (orientation) module, so on a portrait image its
     own `roi_out` is **landscape** while `dev->roi.preview_{width,height}` is the **post-flip portrait**
     size. Fix: `has_preview_output()` also accepts the **swapped** dims
     (`width==preview_height && height==preview_width`), which un-breaks every pre-flip consumer
     (demosaic, highlights, denoise, atrous, nlmeans, lens), not just ashift.
  The guard also carries a ±2px tolerance now: it is purely defensive, since the genuine
  discriminators are `x==0 && y==0 && scale≈natural_scale` (a zoomed/panned ROI has a non-zero origin
  and a scale strictly above `natural_scale`, so loosening the size test cannot misclassify it).
- **A clipping module must render the full uncropped image while editing — by neutralizing its crop
  in `commit_params()`, the way the crop module does.** When a cache-bypass module is focused, the
  darkroom view already expects the uncropped output (`_darkroom_gui_module_requests_uncropped_full_image()`
  keys on `dt_iop_get_cache_bypass()`). The matching pipeline side is to commit a *neutral* crop while
  `g->editing` (`crop.c` `commit_params()`: `cx=cy=0, cw=ch=1`; ashift does the same with `cl/cr/ct/cb`).
  Then `modify_roi_out()` produces the full output, `modify_roi_in()` requests the full input, and
  `process()` captures the whole image into `g->buf` — all the GUI size caches agree on the *full*
  size because they are derived from the same neutralized pipeline data. The crop is reapplied from
  params on commit/cancel. Because the module opts into `runtime_data_hash()`, the neutralized edit
  state hashes distinctly, so the cache never confuses the edit buffer with the committed one.
- **Do not widen `roi_in` alone while leaving `roi_out` cropped.** It seems clever (the displayed
  output stays cropped, only the captured buffer grows), but it makes the pipe produce a *cropped*
  output while the darkroom view wants the *uncropped* full image (cache-bypass is on), and the two
  never reconcile — the preview keeps aborting at *initialscale* and restarting (the intermittent
  hang). Neutralize the crop in `commit_params()` so output, input, view and size caches all describe
  the same full frame; never split them.
- **Geometry must not come from a crop-dependent ROI.** `piece->roi_in` is the *minimal* input region
  a module needs for its (possibly cropped) output, so it shrinks with the crop. Anything that
  reasons about the *whole* image — auto-crop fitting, aspect ratios — must use `piece->buf_in` (the
  full input at the pipe's scale, crop-independent), never `roi_in`. Feeding `roi_in` back into a crop
  computation made ashift's auto-crop converge only after several manual toggles. `buf_in` is a pure
  geometry query; auto-crop does not read pixels at all, so it should never wait on a buffer. See
  `resizing-scaling.md` for `buf_*` vs `roi_*`.
- **Do not gate buffer-independent GUI work on the pixel buffer.** Only pixel-reading work
  (auto-detection) needs `g->buf`. Manual line/perspective drawing and auto-crop only need geometry,
  yet the old code routed all of them through the same "fetch the buffer or bail" guard. With the
  module already carrying parameters the buffer was momentarily unavailable and *manual drawing
  silently stopped accepting input* until the module was reset. Run geometry-only jobs immediately.
- **GUI overlays must invalidate on the geometry they actually track.** The control-line overlay
  caches screen coordinates keyed on the *preview-pipe* hash, which the worker publishes
  asynchronously. While editing, the geometry that the overlay transforms through (the virtual pipe)
  and the displayed size are updated *synchronously* when the crop/params change, so the cache lagged
  a frame and "did not adjust" to a new crop mode. ashift now also invalidates that cache when it
  changes the crop and after each UI-pipe render, so the overlay follows the synchronous geometry.
- **Always clean up a cache-wait handle if you do use one.** Histogram/picker consumers must call
  `dt_dev_pixelpipe_cache_wait_cleanup()` in their teardown/reset paths: a served wait calls back into
  the consumer, so a freed consumer would be a use-after-free. (ashift no longer keeps a handle.)
- **A "won't-settle" recompute loop is driven by a *continuous* dirty signal, not by the editing
  module.** `dt_dev_darkroom_pipeline()` re-runs a pipe only while either `pipe_hash != dev_hash`
  (cleared the moment `dt_dev_pixelpipe_change()` sets the pipe's history hash to the dev's) or
  `pipe->shutdown` is raised *during* `process()` (then neither `runs` nor `reentries` advances and
  `needs_update` stays TRUE, so the same pass repeats). `_change_pipe()` raises `shutdown` on every
  zoom/ROI change, and `configure()` → `dt_dev_configure()` → `dt_dev_pixelpipe_update_zoom_{main,preview}()`
  fires it on each GTK *configure-event*. So a loop that keeps spinning after the mouse is released is
  a **continuous `configure` stream** — a GTK layout/allocation feedback (e.g. in the resizable-panel
  handle code), amplified while editing because cache-bypass makes every recompute uncached. It is not
  fixable inside the editing IOP; trace the configure path. Nothing in ashift's *settled* edit state
  re-marks the preview pipe.

## 7. Lifecycle of one retried GUI fetch

```
GUI thread                         pipeline worker                 cache-wait manager
----------                         ---------------                 ------------------
peek_gui(prev_piece) ── miss ──▶ set_cache_request(MODULE,prev)
        │                         or_changed(CACHE_REQUEST)
        └── register wait ──────────────────────────────────────▶ pending += {hash}
                                                                   connect CACHELINE_READY
                                                                   busy cursor ON
                                   process(): run up to prev_piece
                                   publish prev->global_hash
                                   wrlock_entry(FALSE)
                                   raise CACHELINE_READY(hash) ────▶ match hash in pending
                                                                   pop record, busy cursor OFF
                                   (marshalled to GUI thread)  ◀────  restart(user_data)
restart(): peek_gui() ── hit ──▶ copy buffer, do the work
```

Note: ashift (section 6) does **not** follow this lifecycle anymore; it captures its input in its own
`process()` and resumes from `PREVIEW_PIPE_FINISHED`. The diagram describes the histogram/picker path.

## 8. Failure mode: a wait that is never served (issues #955, #957)

Two field reports converge here: the color picker eye-dropper produces no value on
some modules (filmicrgb, colorcalibration) while working on others (exposure, tone
EQ) — issue #955 — and the scopes/histogram stay blank — issue #957. Both are the
same defect: **a cache-wait whose awaited hash the pipeline never publishes**, so
`DT_SIGNAL_CACHELINE_READY` never matches the pending record and the request "is
never processed and never finishes." (Neither reproduces on the maintainer's
machine, which is why the diagnosis lives in instrumentation — see below.)

### The invariant the protocol depends on

The retry protocol is correct **only if the hash the GUI awaits equals the hash the
worker publishes**. Those two hashes are computed at different times, on different
threads, by different code:

- **GUI (`peek_gui`)** reads `piece->global_hash` from the *currently synchronized*
  node graph — call it `H_gui`. It registers a pending record keyed on `H_gui` and
  emits a `CACHE_REQUEST`.
- **Worker (`dt_dev_pixelpipe_process`)** calls `dt_pixelpipe_get_global_hash(pipe)`,
  which **recomputes** every piece's `global_hash` from the pipe's *current* state
  (module params, blend/mask params, ROI, buffer descriptors, GUI states — mask
  preview, cache bypass, color-picker request — and `runtime_data_hash()` blobs),
  runs up to the requested piece, and publishes under that recomputed hash `H_pipe`.

If any hash input changed between the GUI capture and the worker recompute,
`H_pipe ≠ H_gui`. The worker raises `CACHELINE_READY(H_pipe)`; the manager scans the
pending list for `hash == H_pipe`, finds only the record holding `H_gui`, and serves
nobody. The wait stays queued; the `request_cacheline` dedup then correctly
suppresses re-emission on every subsequent expose (to avoid a request storm), so the
pipe is never re-asked either. Queued forever, busy cursor stuck, no cacheline.

### Why it is module- and picture-dependent

Enabling the eye-dropper sets `module->request_color_pick` and flags the pipe
changed — which triggers exactly the recompute that re-derives the hashes. Whether
that perturbs the *target module's own* `global_hash`, and whether the picker samples
the module's **output** (needs the module's own cacheline) or only its **input**
(needs the previous, already-stable module's cacheline), differs per module. A module
whose picker-request or mask-preview state folds into its own `global_hash` between
capture and publish is inherently exposed to the race; one that samples only an
upstream cacheline is not. `runtime_data_hash()` modules (colorbalancergb,
colorcalibration's committed data, and the filmic family's) widen the window further,
because `piece->hash` folds committed `piece->data` the GUI thread has not committed
yet at capture time. This is the same family as the #710 "acting crazy on some
pictures" defects (§6): a *geometry* input to the hash (the round-vs-truncate 1px ROI
disagreement, the portrait `flip` swap) that the two sides compute differently is
just another way to make `H_gui ≠ H_pipe`.

The scopes (issue #957) are the same failure one hop downstream: they read the
preview backbuffer via `peek_gui(piece == NULL)`, keyed on the published
`backbuf.hash`. If the preview pipe is itself wedged in the never-served loop (the
picker keeps re-dirtying it, or the awaited module hash never publishes), the
backbuffer is never refreshed and the scopes stay blank.

A **secondary** way the same wait never settles, even when `H_gui == H_pipe`: an
OpenCL device-only publish. If the worker writes the output only to vRAM and keeps it
device-only, `peek_gui` (`preferred_devid = -1`) reports a miss on restart,
re-requests, the pipe republishes device-only, and the loop never converges. The
reporters saw the bug with OpenCL both on and off, so this is not the primary cause
here, but it is the same symptom and the same instrumentation catches it (a
served → re-queued → served cycle on one hash, the cacheline present but device-only).

### Confirming it with the supervisor

The cache-wait manager is now instrumented as its own supervisor domain
(`cache-wait`, see `supervisor.md`). Each queued wait carries an `awaits` edge to the
cacheline hash it is blocked on, so the invisible hang becomes a one-click diagnosis:

1. *Help → Event supervisor*, enable **Record**, trigger the eye-dropper on the
   stuck module.
2. In **Timeline**, find the `cache-wait` `create` for owner `color-picker-input` /
   `color-picker-output`; note its `awaits` hash. A run of `read` (`dedup-poll`)
   events with no matching `delete` (`served`) is the stuck signature.
3. Click / search the `awaits` hash. If there is **no** cacheline `create` under it,
   but there **is** a `node` `update` + cacheline `create` for the same module under
   a *different* hash, that is the mismatch — compare the two hashes' `params` / `roi`
   facets to find which input diverged. The `comm -23` recipe in `supervisor.md`
   lists every orphaned awaited hash in one shot.

### Structural fix in place: serve waiters by producing node, not just by hash

The exact-hash match is a fragile *primary* signal, not a safe *only* signal. The
cache-wait manager (`dev_pixelpipe.c`) now serves a pending waiter on **either** an
exact hash match **or** a producing-node match — which fixes every consumer that goes
through the manager at once (color picker `input_wait`/`output_wait`, histogram
`scope_wait`/`module_wait`/`picker_wait`, autoset `input_wait`):

- Every cacheline is stamped at publish with the identity of the node that produced
  it — `dt_pixel_cache_entry_t.producer_node_key`, a
  `dt_supervisor_node_key(pipe_type, op, multi_priority)`. When the write lock is
  released, `DT_SIGNAL_CACHELINE_READY` now carries **both** the published hash *and*
  that producer key (the key is computed on the worker thread and value-copied through
  the async signal, so no live object or evictable entry is dereferenced on the GUI
  thread).
- Each waiter records the producer key of the output it wants
  (`wait->target_node_key`, set in `peek_gui()`; `INVALID` for a backbuf target, which
  has no single producing node and therefore keeps to exact-hash).
- `_dt_dev_pixelpipe_cache_wait_ready_callback()` serves a waiter when
  `wait->hash == published_hash` **or** `wait->target_node_key == producer_node_key`.
  The drift case is exactly "the awaited hash never published but the target module
  did": the node match then serves the right consumer, and its restart re-reads the
  module's *current* output hash and hits. The exact-hash match is kept as the fast
  path *and* because it is a pure value comparison independent of the just-published
  entry (which may already be evicted by the time the GUI-thread callback runs), so the
  existing served-then-re-miss self-healing is preserved. A node-key serve is logged /
  supervised as `served (drift: node-key)`.

**The early-return that swallowed the wake-up.** The node match above only helps if a
`CACHELINE_READY` actually fires. But `dt_dev_pixelpipe_process()` has a fast path: if
the requested target is *already host-cached*, it returns immediately (after refreshing
the backbuf reference for a backbuf target) **without taking a write lock**, so no
`CACHELINE_READY` is raised. Combined with drift this is the exact "no node update,
nothing" hang seen in the field (issue #955, color picker on filmicrgb /
colorcalibration): the picker predicted `H_gui`, the module's output is sitting in the
cache under `H_pipe`, the pipe finds that hit and returns, and the waiter — keyed on
`H_gui` — is never woken even though its buffer exists. `dt_dev_pixelpipe_process()`
now raises `CACHELINE_READY(requested_hash, producer_node_key)` on that early-return
**when a GUI cache request was pending** (`cache_request != NONE`), so the manager's
producer-node match serves the waiter; its restart re-reads the module's current hash
and hits. It is gated on the pending request so ordinary cache-hit renders add no
signal traffic. This top-level check, in `dt_dev_pixelpipe_process()` before
`process_rec()` even starts, does not treat a *device-only* cached target as a hit: it
uses host-only `devid == -1`. That only means `process_rec()` gets invoked next — its own
handling of an existing device-only entry is the separate mechanism described below.

The color picker additionally re-samples on `DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED`
(`_iop_color_picker_pipe_finished_callback`) as belt-and-suspenders: a module-only
`CACHE_REQUEST` still queues the backbuffer continuation in `develop.c`, guaranteeing a
`PREVIEW_PIPE_FINISHED`. It is gated on the picker still being `update_pending`, so it
is a no-op once a sample has landed.

### The writable-acquire exact-hit and host-residency policy

`process_rec()`'s cache acquisition for a node's own output tries two lookups in order:

1. `exact_output_cache_hit` (`dt_dev_pixelpipe_cache_ref_entry_by_hash()`) requires
   non-NULL *host* data — a device-only entry does not satisfy it.
2. If (1) misses, `dt_dev_pixelpipe_cache_get_writable()` is asked for a *writable*
   handle on the same hash. Its `EXACT_HIT` status only reflects whether an entry exists
   for that hash (`_non_threadsafe_cache_get_entry()`), independent of whether it carries
   host data or of the caller's `cache_ram_output` request.

Host-residency requirements (`piece->cache_output_on_ram`, set by
`_seal_opencl_cache_policy()` — a color picker or histogram starting to sample a piece,
`active_in_gui` turning on, etc.) are otherwise only consulted when *creating* a cache
entry. So the `EXACT_HIT` branch also compares `cache_ram_output` against the entry's
current residency: when a host copy is wanted and the entry has none, it calls
`dt_dev_pixelpipe_cache_restore_host_payload(cache, exact_entry, pipe->devid, &data)` —
the same helper `dt_dev_pixelpipe_cache_peek()` uses for its own device-owning callers —
to read the GPU payload back to host in place, without recomputing the module.
`pipe->devid` is a real, locked OpenCL device id at this point in `process_rec()` (the
lock is taken before the recursion starts).

This matters whenever a module's cachelines were computed device-only before anything
needed a host copy of them — e.g. focusing a color-picker/histogram-consuming module for
the first time on an image where the module (and its upstream piece) already rendered
once, collapsed, before the picker ever engaged `_seal_opencl_cache_policy()`'s host
requirement for it.

**Field signature** (`-d dev`): `[pipeline] module=X writable-exact-hit ... has_host_data=0
cache_ram_output=1` followed immediately by `[pipeline] module=X exact-hit was
device-only, host materialize succeeded ...`, both logged from this branch.

This mechanism is independent of the hash-drift race described earlier in this section
and does not apply when OpenCL is disabled, since no device-only entries can exist there.

### Check the output wait before the input wait

`_sample_picker_from_cache()` (`color_picker_proxy.c`) needs two cachelines: the target
module's **input** (the previous enabled piece's output) and its own **output**. It
checks output first, and only falls back to requesting input on its own when output can
never resolve (`bypass_cache`/`no_cache`/realtime — the color-equalizer-style "input
only" case, where there is nothing downstream to recurse through for us).

The reason: `process_rec()` always recurses upstream to obtain a node's input before
producing that node's own output, so requesting the module's **output** as the
`CACHE_REQUEST_MODULE` target makes a single recompute produce and cache *both*
cachelines. Checking input first and returning on a miss without looking at output would
cost two sequential `CACHE_REQUEST` → `PIPE needs update` → recompute round-trips
whenever both are cold — the common case right after focusing a module whose input/output
were never sampled on the current image (fresh image load, or a module that stayed
collapsed since darkroom opened) — instead of one.

This ordering also means `_sample_picker_from_cache()` only reaches its final
`wait_output_hash`/`update_pending` cleanup once output is either sampled or structurally
blocked, so that cleanup can stay unconditional: it is never reached with output "missing
but expected to arrive later."

### Remaining fix direction (design, not yet implemented)

The node-key serve makes the drift *recover* rather than removing it. A backbuf waiter
still relies on exact hash, and consumers that subscribe to `CACHELINE_READY` directly
(the histogram's `initialscale`/`colorout` backbuf refresh triggers, toneequal,
colorequal) still match their own captured hash and could adopt the same producer-node
match if they prove fragile. The cleaner long-term fix removes the GUI-precomputed hash
entirely: (a) have the worker report the actually-published hash for a given
`CACHE_REQUEST` and match waiters on **request identity**; or (b) capture `H_gui` from
the same synchronized graph state the worker will use, i.e. after the pending
pipe-change is synchronized. Either breaks the strict `H_gui == H_pipe` dependency at
the source.

## 9. Known limitations / TODO

- The wait manager is a process-wide singleton with a single pending list. It scales with the small
  number of concurrent GUI consumers (pickers, histogram), but there is no per-pipe partitioning;
  `dt_dev_pixelpipe_cache_wait_dump_pending()` is the only introspection.
- A target that the pipe can *never* publish (e.g. an OpenCL allocation that keeps failing) leaves
  the wait queued and the busy cursor active until the consumer cancels it. The `request_cacheline`
  dedup prevents a request storm but does not time the request out.
- ashift renders the full uncropped image (and so captures the full `g->buf`) only while it is the
  focused module in edit mode (`commit_params()` neutralizes the crop then). Outside that window —
  e.g. a script or future caller that drives auto-detection without entering edit — detection would
  again see the crop-dependent sub-region. This is acceptable today because the interactive controls
  are edit-only.
- As noted in `reorganisation.md`, the long-term direction is to let modules self-trigger their
  `process()` and publish directly to the cache, which would let interactive modules refresh their
  own GUI buffers without a full preview round-trip at all.
- The writable-acquire `EXACT_HIT` materialize (§8, "The writable-acquire exact-hit and
  host-residency policy") is synchronous on the worker thread (a GPU→host read), so the first render
  after a policy tightens (a picker/histogram starts consuming a previously device-only-cached
  piece) pays that transfer cost once; subsequent hits are already host-resident and skip it.
  `dt_dev_pixelpipe_cache_get_writable()` has a single call site today (`pixelpipe_hb.c`), so this
  is not yet a reusable pattern — a second caller needing the same policy-aware-hit behavior should
  factor it into the cache layer instead of duplicating it.

## 10. System memory pressure handling (issue #1083)

The startup budgets (`dt_configure_runtime_performance()`: headroom / mipmap / pixelpipe split of
the detected RAM, or of `host_memory_limit`) are only *plans*. They say nothing about what the
system can actually back at any given moment, with other applications competing for the same
physical pages. Historically three compounding problems turned that gap into OOM kills that users
see as "Ansel crashed without message": the budgets were trusted blindly (a `host_memory_limit`
above physical RAM was even honored as-is), freed arena runs never returned their physical pages
to the OS (RSS was a permanent high-water mark), and nothing at runtime ever looked at the
system-wide available memory.

The defense has four layers, from planning to last resort:

1. **Envelope-aware startup budgets** (`darktable.c`). The detected total RAM is clamped
   by the physical RAM (so a misconfigured `host_memory_limit` can only shrink, never grow it) and
   by the tightest cgroup-v2 `memory.max` on our own path (containers, Flatpak, systemd slices —
   the kernel OOM-enforces that envelope no matter how much RAM the machine has). A **pressure
   floor** is derived (conf key `memory_pressure_floor`, `0 = auto` = half the OS headroom,
   bounded to the envelope): the system-wide available RAM under which we start shedding caches.

2. **Lazy page release on every arena free** (`common/memory_arena.c`). `dt_cache_arena_free()`
   marks the freed run `MADV_FREE`: the pages stay mapped and re-dirtying them before reclaim
   costs nothing (the per-frame temp-buffer churn is unaffected), but the kernel may take them
   back *at will* under pressure. Measured on a 24 Mpx export: ~44 % of the process RSS is
   LazyFree at steady state — memory the kernel can have instantly, which starves the OOM killer
   of a reason to pick us. `dt_cache_arena_trim()` is the hard variant (`MADV_DONTNEED` /
   `VirtualFree(MEM_DECOMMIT)`) that hands all free runs back NOW; the 3-minute GC and the idle
   shedder below call it so an idle Ansel doesn't sit on its high-water mark.

3. **Allocation-time pressure valve** (`_system_memory_pressure_valve()`,
   `develop/pixelpipe_cache.c`). Every arena allocation funnels through
   `_arena_alloc_with_defrag()`, which first checks a rate-limited probe of the system-wide
   available RAM (`dt_get_system_available_mem()`: MemAvailable on Linux — min'd with the cgroup
   slack including its reclaimable `inactive_file`, where our own MADV_FREE'd pages land —
   `ullAvailPhys` on Windows, free+inactive on macOS; `0` means "no information", which disables
   the valve, not the allocation). If the allocation would push available RAM under the floor,
   LRU entries are evicted until the deficit is covered, the arena is hard-trimmed, and the probe
   is then **re-read from the OS** rather than credited with the bytes we think we released: how
   much of an eviction actually reaches the system is not ours to guess (the per-free `MADV_FREE`
   is a no-op on Windows, and elsewhere the kernel decides when those pages stop counting against
   us), and an over-credit would wave the allocation through on a system that is still just as
   full. If even shedding everything cannot keep
   HALF the floor, the allocation is refused: the pipeline fails with a clear message, which
   beats a silent SIGKILL. A 5-second GUI timer (`_memory_pressure_shedder()`) covers the case
   where *another* application creates the pressure while we sit idle and never allocate.

   Two traps live in this valve. The probe costs ~61 µs (16 µs `/proc/meminfo` + 45 µs walking
   the cgroup tree) — enough that the tiling planners alone would spend ~1.8 ms of a 16 ms
   realtime frame on it — so `dt_get_system_available_mem()` caches its answer for 50 ms, and any
   caller that just changed the situation itself must call `dt_invalidate_system_available_mem()`
   first or it reads back its own pre-change snapshot. And the valve's running estimate (last
   probe, minus our own allocations since) legitimately reaches 0 under sustained pressure, so
   "does this platform answer at all" is a separate `sys_probe_valid` flag: deriving it from an
   `est == 0` sentinel would silently disable the valve at exactly the moment it matters most.

4. **Pressure-aware tiling** (`dt_get_available_mem()`, `darktable.c`). The planning value
   tiled modules size their working set from is capped by the live system availability (plus half
   our own cache, which is LRU-evictable on demand — our own hoard must never force tiling), so
   under pressure modules split into smaller tiles instead of planning allocations the valve
   would refuse mid-pipe.

Verified by running the same export with and without these layers inside
`systemd-run --user --scope -p MemoryMax=1536M -p MemorySwapMax=0`: before, SIGKILL by the
kernel OOM killer mid-export (journal: `Failed with result 'oom-kill'`), no output, no message;
after, the envelope is detected at startup, budgets and floor scale to it, the cache sheds under
pressure and the export completes — bit-identical pixels to the unconstrained run — down to a
1280 MB envelope for a 24 Mpx image. Below that (1 GB) the *live working set* of a single
pipeline pass (2–3 full-resolution float buffers that must coexist) simply doesn't fit, and no
amount of eviction can compress it: that residual floor can only move with more aggressive
tiling of the always-untiled modules, not with cache policy.

Testing lever: the cgroup probe honors whatever limit `systemd-run -p MemoryMax=` sets, so
pressure behavior is reproducible without actually starving the machine.
