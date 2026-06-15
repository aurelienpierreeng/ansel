# Pipeline cache, GUI fetching and the cache-wait retry protocol

This document describes how the pipeline cache stores module outputs, how GUI code fetches
those buffers without blocking the interface, and the asynchronous **retry protocol** used when a
buffer is not available yet. It complements:

- `reorganisation.md`, which gives the high-level cache taxonomy (database / image / mipmap /
  pipeline) and the global locking model;
- `resizing-scaling.md`, which describes the ROI passes (`modify_roi_out()` / `modify_roi_in()`)
  and the difference between `piece->buf_*` and `piece->roi_*`.

The code lives in `src/develop/pixelpipe_cache.c` (the cache itself), `src/develop/dev_pixelpipe.c`
(the GUI fetch wrapper and the cache-wait manager) and `src/develop/pixelpipe_hb.c` (the recompute
that publishes cachelines).

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

## 8. Known limitations / TODO

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
