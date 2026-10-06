# Pipeline cache, GUI fetching and the cache-wait retry protocol

This document describes how the pipeline cache stores module outputs, how GUI code fetches
those buffers without blocking the interface, and the asynchronous **retry protocol** used when a
buffer is not available yet. It complements:

- `reorganisation.md`, which gives the high-level cache taxonomy (database / image / mipmap /
  pipeline) and the global locking model;
- `resizing-scaling.md`, which describes the ROI passes (`modify_roi_out()` / `modify_roi_in()`)
  and the difference between `piece->buf_*` and `piece->roi_*`.

The code lives in `src/caches/pixelpipe_cache.c` (the cache itself — it was
`src/develop/pixelpipe_cache.c` until the caches module was extracted, `c889e94dc6`),
`src/develop/dev_pixelpipe.c`
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

### A module's cacheline is reused in place, and what that leaves in RAM

A piece keeps the metadata of the cacheline it wrote last (`piece->cache_entry`). When its hash
changes, `dt_dev_pixelpipe_cache_get_writable()` moves that same cacheline to the new hash instead
of allocating another one (*rekey reuse*): same arena slot, same OpenCL buffers, no allocation per
frame. Two consequences follow, each with its rule.

**The host buffer outlives the pixels it held.** An OpenCL output that is not read back to RAM
leaves the previous hash's pixels in the rekeyed cacheline's host buffer. The rekey marks it
(`dt_pixel_cache_entry_t.host_stale`), and until the host buffer is written for the new hash
`dt_pixel_cache_entry_get_data()` returns NULL, exactly as for a device-only cacheline. Readers
need nothing else: a CPU consumer copies the device payload back, a GUI reader reports a miss.
The producer clears the mark with `dt_dev_pixelpipe_cache_flag_host_written()` when it ran on the
CPU, tiled, or read its output back; every device-to-host copy clears it too. Only the producer
reaches the raw buffer, through `dt_pixel_cache_entry_get_buffer()`, since it still backs the
output's pinned OpenCL image.

**Reusing in place forgets the previous state.** That is right for a slider drag or a pan, whose
previous states do not come back, and wrong for a module toggle, an undo or a jump in history,
after which going back is the likely next step. Those raise `DT_DEV_PIPE_SWITCHED`, which sets
`pipe->keep_outputs` for the run that renders the new state: it writes every output into a new
cacheline, so the outputs of the state just left are still there, and switching back is a chain of
exact hits. The flag drops when that run completes. Only the source of a change knows it is a
switch; the pipe's change status does not (`SYNCH` also carries slider commits on masked modules).

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
dt_dev_pixelpipe_cache_ref_count_entry(TRUE,  entry);  // pin: prevent eviction
dt_dev_pixelpipe_cache_rdlock_entry  (TRUE,  entry);  // block writers while we read
... memcpy out of the cacheline ...
dt_dev_pixelpipe_cache_rdlock_entry  (FALSE, entry);
dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);  // the entry may not be named after this
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
`dt_dev_pixelpipe_cache_ref_entry_by_hash()`, or `dt_dev_pixelpipe_cache_ref_host_entry_by_hash()`
when it needs host pixels and must not wait on a line still being written. Raster-mask retrieval,
the raw-detail mask, the drawn-mask group's cached-prefix resume (`develop/masks/group.c`) and the
publication of the backbuffer follow this contract: the lookup itself takes the reference, under the
cache mutex; it is then kept as the long-lived one, or released once the pixels are copied (under a
read lock) or once the long-lived reference is taken.

**A peek retains nothing, so nothing may be released after one.** An entry nobody holds sits at
refcount 0, and any thread's eviction can free it between `dt_dev_pixelpipe_cache_peek()` returning
and the caller's next step, read lock included. Releasing a reference that was never taken is worse
than a leak: it drives the count below the number of real holders, the LRU then sees a held entry as
free and frees it under them, and every long-lived reference is released by pointer
(`dt_dev_pixelpipe_cache_unref_entry()`), so the holder's own release writes into freed memory --
wherever the allocator has put something else by then. The crash surfaces in that other object, far
from the release that caused it.

**Nor may one be referenced after one.** The eviction can fall between the lookup and the
reference, which then counts up freed memory; a backbuffer keepalive taken that way is released by
pointer at the next publication. `dt_dev_pixelpipe_cache_get_entry()` is the same kind of lookup:
it serves only the producer-to-consumer handoff inside one run, where the reference is already
held. When `dt_dev_pixelpipe_cache_get_writable()` finds the hash already published, it returns
that entry referenced for the same reason.

**Nor may one be named after its release.** A release returns with the cache mutex released, and
from then on any thread's eviction can free an entry nobody holds. A holder that wants a line gone
flags it with `dt_dev_pixelpipe_cache_flag_auto_destroy()` while it still holds it, then releases
it: releasing the last reference of a flagged line removes it, within the same hold of the mutex.
That is how the intermediates of a pipe that keeps no cache go, as well as a module output that
failed, a side-band line a module created and could not fill, and a no-cache pipe's last frame at
cleanup. Others still holding the line keep it until the last of them releases it. A line still
locked when released stays for the LRU, so a producer flags before it releases its write lock and
drops its reference last. There is no removal by pointer: it could only succeed on a line nobody
holds, which is one its caller has no right to name.

**A reserved reference is released on every way out.** `process_rec()` returns its output with one
reference reserved for its receiver: the next module, or `dt_dev_pixelpipe_process()` for the final
output. The receiver releases it whether it goes on or not, so a module aborting before it
processes releases it, and so does a run shut down or failed on OpenCL after it completed. A
reference left behind pins its line, which can then never be evicted.

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
current residency: when a host copy is wanted and the entry has none -- or only the stale one a
rekey left behind, which `dt_pixel_cache_entry_get_data()` reports as none -- it calls
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

The defense has five layers, from planning to last resort:

1. **Envelope-aware startup budgets** (`darktable.c`). The detected total RAM is clamped
   by the physical RAM (so a misconfigured `host_memory_limit` can only shrink, never grow it) and
   by the tightest cgroup-v2 `memory.max` on our own path (containers, Flatpak, systemd slices —
   the kernel OOM-enforces that envelope no matter how much RAM the machine has). A **pressure
   floor** is derived (conf key `memory_pressure_floor`, `0 = auto` = half the OS headroom,
   bounded to the envelope): the system-wide available RAM under which we start shedding caches.

2. **Lazy page release on every arena free** (`system/memory_arena.c`). `dt_cache_arena_free()`
   marks the freed run `MADV_FREE`: the pages stay mapped and re-dirtying them before reclaim
   costs nothing (the per-frame temp-buffer churn is unaffected), but the kernel may take them
   back *at will* under pressure. Measured on a 24 Mpx export: ~44 % of the process RSS is
   LazyFree at steady state — memory the kernel can have instantly, which starves the OOM killer
   of a reason to pick us. `dt_cache_arena_trim()` is the hard variant (`MADV_DONTNEED` /
   `VirtualFree(MEM_DECOMMIT)`) that hands all free runs back NOW; the 3-minute GC and the idle
   shedder below call it so an idle Ansel doesn't sit on its high-water mark.

3. **Allocation-time pressure valve** (`_system_memory_pressure_valve()`,
   `caches/pixelpipe_cache.c`). Every arena allocation funnels through
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
   beats a silent SIGKILL. The message goes through the cache's *alert* handler, not its warn
   handler: the GUI installs it (`dt_dev_pixelpipe_cache_set_alert_handler()`, from
   `dt_gui_gtk_init()`) and shows it with `dt_gui_alert()` (`gui/alert.c`), a window kept above Ansel's
   main window until its OK button is clicked, because what it says -- the module failed, the image was
   not updated -- stays true long after a toast is gone. Without a GUI it falls back to the warn
   handler. It is rate-limited to one every 10 s; the size refused, the module that asked for it,
   that module's image (its file name between backticks -- it can hold spaces -- and its image id,
   as text `pixelpipe_hb.c` writes and hands over with
   `dt_pixelpipe_cache_set_current_image()` beside the module's name: the cache prints it, and knows
   nothing of images) and the RAM left above the floor -- what the valve compared the
   size with -- are its item, listed under the message, which stays the same at every refusal.
   `dev_pixelpipe.c` names both around `modify_roi_in()` too, before any module processes: lens
   allocates its edge buffer there, and on 2026-10-06 a refusal of it (1795728 bytes, with the floor
   forced to 3000 MiB) listed "1 MiB: only 0 MiB available", with neither module nor image. Lens
   does not fail on it: it plans its input without the margin. The sizes have two decimals since
   then: in whole MiB those 1.71 MiB read "1 MiB", which the valve lets through. In
   `dt_dev_pixelpipe_process_rec()` the naming starts before `dt_dev_pixelpipe_cache_get_writable()`,
   not at `process()`: that call allocates the module's output when it stays in RAM, as the
   darkroom's preview does, and opening an image the same day listed "46,69 MiB: only 0,00 MiB
   available" first -- rawdenoiseai's preview output, refused there -- then the same size named,
   refused again in `pixelpipe_cpu.c`. The
   cache's other
   user-facing message, "The pipeline cache is full…" (the cache's own cap, `_free_space_to_alloc()`
   and `_log_arena_allocation_failure()`), takes the same path, under the same "Not enough
   memory" title but in a window of its own -- `dt_gui_alert()` opens one per title and message, and
   what was being allocated (the cacheline's name, its module) is the item, not part of the text.
   (Until 2026-10-06 the window was one per title and both messages carried their values, so every
   size and every module added a paragraph; changed on top of `8073021c02`.)
   `_log_arena_allocation_failure()` raises it only when the arena itself refused, not the valve
   (`_arena_alloc_with_defrag()` says which). Until 2026-10-06 a valve refusal raised it too, so one
   refusal opened two windows and the second was false. Measured on `8073021c02` by exporting the
   24 Mpx X-H1 raw of the image test bank with the installed `ansel-cli --conf
   memory_pressure_floor=1250`: lens was refused 370 MiB with 1495 MiB available, while the log
   read `cache=741/4461 MiB` -- 370 of those 741 being lens's own entry, counted before its buffer
   exists -- and nothing was evictable (`couldn't remove LRU, 2 items and all are used`: lens's
   input and lens's own entry). Lens is where that export peaks: the pipe stays at full resolution
   until `initialscale`, right after lens, so lens holds demosaic's 370 MiB output while asking for
   370 more. The pre-commit image test met the same refusal for real that day, with 445 MiB
   available. It is raised only when
   the allocation is actually
   refused, and then followed by what happens next (`_alert_cache_refused()`): the module fails,
   the pipe stops without publishing, nothing retries in tiles (whether to tile was decided before
   `process()` ran, in `pixelpipe_cpu.c`), and the darkroom keeps its last complete rendering until
   the next change runs the pipe again (`develop.c`). Read from the code on 2026-10-03, not
   measured. `_free_space_to_alloc()` also reaches its message when nothing is left to evict
   because the memory is held by working buffers, not cachelines: `error` stays 0 and the
   allocation goes ahead past the budget. That case stays a toast. Neither is rate-limited, as
   the toast was not. Written 2026-10-03 on
   top of `65e7a3adea`; the failure it describes was measured on that base with AI denoise on CPU
   (a 1182 MiB tile refused with 1255 MiB available, the preview left as it was). A 5-second GUI timer (`_memory_pressure_shedder()`) covers the case
   where *another* application creates the pressure while we sit idle and never allocate.

   Two traps live in this valve. The probe costs ~61 µs (16 µs `/proc/meminfo` + 45 µs walking
   the cgroup tree) — enough that the tiling planners alone would spend ~1.8 ms of a 16 ms
   realtime frame on it — so `dt_get_system_available_mem()` caches its answer for 50 ms, and any
   caller that just changed the situation itself must call `dt_invalidate_system_available_mem()`
   first or it reads back its own pre-change snapshot. And the valve's running estimate (last
   probe, minus our own allocations since) legitimately reaches 0 under sustained pressure, so
   "does this platform answer at all" is a separate `sys_probe_valid` flag: deriving it from an
   `est == 0` sentinel would silently disable the valve at exactly the moment it matters most.

4. **Kernel-pressure shedding** (`caches/pixelpipe_cache_pressure.{c,h}`). MemAvailable says
   how much is left, never what it costs to keep it: a machine reclaiming as fast as it allocates
   reports memory available while systemd-oomd is already counting down on it. Linux PSI reports
   that directly, and `system/memory_pressure.c` is where every platform detail of it lives — the
   cumulative "full" counters for the system and every cgroup above the process, plus a watcher
   that arms the kernel's own triggers and wakes a thread of its own, so the reaction does not
   wait for a cache that a thrashing machine has already stopped running. The pressure module
   turns two reads into the stall share of the 2 s window between them and decides what must go;
   `pixelpipe_cache.c` passes it a sink (what it holds, what evicting and trimming gives back) and
   takes `lock` around every call. Past 10 % stall a quarter of the cache goes (half past 30 %)
   and the budget allocations evict down to is lowered to what is left, climbing back only over
   minutes of calm and never past 7/8 of the footprint the stall struck at. See `CLAUDE.md` for
   the measurements each of those numbers came from.

5. **Pressure-aware tiling** (`dt_get_available_mem()`, `darktable.c`). The planning value
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

---

## Rules carried over from CLAUDE.md

*Verified against `42eca0e8fe`, 2026-09-29. Each finding carries the commit that established it.*

### GUI backbuf must use the published hash, not the planned hash

*Found `22f623c0be`, 2026-06-25.*

For final-backbuffer display (`dt_dev_pixelpipe_cache_peek_gui` with `piece == NULL`), GUI
consumers (center view, navigation thumbnail, scopes) must key the lookup on the **published**
`pipe->backbuf.hash`, not the **planned** `dt_dev_pixelpipe_get_hash(pipe)` (`pipe->hash`).

The pipeline plans the next frame's global hash before publishing pixels, so `pipe->hash` runs
ahead of `pipe->backbuf.hash` whenever a recompute is in flight. Realtime drawing makes this the
steady state. Peeking the planned hash misses the perfectly valid published frame → the main-surface
lock fails → darkroom expose falls back to the paused preview pipe → flicker.

In `peek_gui`, for `piece == NULL` use `dt_dev_backbuf_get_hash(&pipe->backbuf)` as the display
lookup hash when valid.

### OpenCL vRAM flush must not drop live entries

*Found `22f623c0be`, 2026-06-25.*

`darktable.pixelpipe_cache` is shared across all pipes. `dt_dev_pixelpipe_cache_flush_clmem`
iterates EVERY entry on a device, not just the calling pipe's own. The bug (issue #817): it
released the `cl_mem` of a buffer another pipe was mid-recursion on, leaving a husk (no RAM,
no vRAM) keyed in the cache, which then aborted downstream consumers → skull thumbnails.

The correct flush predicate: skip any entry where `dt_atomic_get_int(&entry->refcount) > 0` OR
`dt_pthread_rwlock_trywrlock` fails (never wait on writer locks). Idle entries (refcount 0,
unlocked) get their vRAM reclaimed. The flush must hold `cache->lock` for the entire iteration
so no consumer can mid-acquire a refcount==0 entry.

If a flushed entry is then empty (no host data + no vRAM on any device), remove it from the
hash table via `g_hash_table_iter_remove` — do NOT subtract `current_memory` manually, the
`_free_cache_entry` GDestroyNotify handles it.

### A peek retains nothing: keep or release only what a retained lookup handed you

*Found `f4d963f8ca`, 2026-09-27.*

`dt_dev_pixelpipe_cache_peek()` is non-owning. An entry nobody holds sits at refcount 0 and any
thread's eviction can free it between the peek and the caller's next line, so code that reads a
cacheline, keeps it, or releases one afterwards, looks it up with `dt_dev_pixelpipe_cache_ref_entry_by_hash()`
or `dt_dev_pixelpipe_cache_ref_host_entry_by_hash()` (lookup and reference under one hold of the cache
lock), read-locks while copying, and releases exactly that reference.

Referencing the entry after the peek does not close the window: the eviction can still land before
the reference, which then counts up freed memory. The backbuffer's keepalive is the case that costs
most: taken that way, it is released by pointer at the next publication. So the pipeline publishes
the backbuffer from a retained lookup and drops that reference once the keepalive is taken: the exact
hit before a run from `ref_host_entry_by_hash()`, the output after it from `ref_entry_by_hash()`
followed by `dt_dev_pixelpipe_cache_restore_host_payload(entry, pipe->devid, NULL)`.
`dt_dev_pixelpipe_cache_get_entry()` is a peek too: it serves the producer-to-consumer handoff inside
one run, where the reference is already held, and nothing else. For the same reason,
`dt_dev_pixelpipe_cache_get_writable()` hands its exact hit back already referenced.

*Wrong claim, found 2026-10-05 against `281fc20a37`:* that commit published the output after the run
from `ref_host_entry_by_hash()` too, on the premise that "after the run the final output always keeps
a host copy". It does not on an export pipe: `dt_imageio_export_with_flags()` drives the pipe without
`_seal_opencl_cache_policy()`, `dt_iop_commit_params()` leaves the last node at
`cache_output_on_ram = 0`, and `gamma` is disabled outside the GUI pipes, so an OpenCL export ends on
a vRAM-only cacheline. The peek it replaced, called with the still-reserved `pipe->devid`, had been
downloading it; the host-only lookup refused it, the backbuffer was never published, and the export
failed with only `[dt_imageio_export_with_flags] no valid output buffer` under `-d imageio`.
Measured by bisect: OpenCL export fails at `281fc20a37`, succeeds at its parent; CPU export succeeds
at both. The exact hit before the run is unaffected: `pipe->devid` is -1 there, so the peek never
materialized a device-only entry either.

A release nobody took is a use-after-free with a delay. It leaves the count below the number of real
holders; the LRU (`refcount > 0` is its only guard) frees a held entry; and since every long-lived
reference is released by POINTER (`dt_dev_pixelpipe_cache_unref_entry()`), the holder's own release
later decrements freed memory -- memory the allocator has meanwhile handed to something else. The
crash lands in that other object, typically as a pointer that reads back as itself minus one (the
refcount decrement landed on it), found by the next `free()` or dereference of it, nowhere near the
cache. ASAN only sees it when the eviction happens to fall inside the window; the imbalance itself is
deterministic, so the quick way to find one is to report, with a backtrace, every release that takes
a count below zero in `_non_thread_safe_cache_ref_count_entry()` and exercise the suspect path.

### A cacheline is dropped by flagging it while held; its release removes it

*Found `ad85705155`, 2026-09-28.*

Nothing may name a cache entry after releasing its reference: the release returns with the cache
mutex released, and from then on any thread's eviction can free an entry nobody holds. A second
call on the same pointer, to remove it or to flag it, is a second hold of the mutex, and the
eviction can land between the two.

So the removal happens inside the release. `dt_dev_pixelpipe_cache_ref_count_entry(FALSE, ...)` on
the last reference of an entry flagged with `dt_dev_pixelpipe_cache_flag_auto_destroy()` removes it
in the same hold of the mutex, and a holder drops a line by flagging it, then releasing it.
`tests/unittests/test_pipe_cache_auto_destroy.c` pins the contract. Four things a reviewer would
otherwise change:

- **Flag before the write lock goes, release last.** The release removes only an entry nobody holds
  or locks: one released while still write-locked stays, flagged, for the LRU. Flagged before the
  unlock, it is also refused by the retained lookups a waiter makes when `DT_SIGNAL_CACHELINE_READY`
  wakes it.
- **There is no public removal by pointer.** It could only succeed on an entry nobody holds, that
  is, one its caller has no right to name. The peek holds no reference either, so when it discards
  a line with neither host nor device payload, it looks the entry up again by hash and removes it
  within one hold of the mutex.
- **A line created and not filled goes too, even when no buffer could be allocated.** Left in
  place, the next `dt_dev_pixelpipe_cache_get()` of that hash finds it, allocates its buffer on
  demand and returns it as found, i.e. as written: the caller reads uninitialised memory as its
  mask.
- **The reference `process_rec()` reserves for its receiver is released on every way out.** The next
  module, or `dt_dev_pixelpipe_process()` for the final output, owns it from the moment the
  recursion returns: an abort after that point releases it as a completed run does, so
  `KILL_SWITCH_ABORT`, which releases nothing, has no place after the recursion. A reference left
  behind pins its line for good.

### A cache key is what a piece computes, never a runtime identity

*Found `b1a9a18ebd`, 2026-09-11.*

`dt_iop_compute_module_hash()` (`develop/imageop.c`) keys a module by its op, enabled state,
`multi_priority`, `iop_order`, params and blendop hash — and must not fold `module->instance`.
That field is the family id `dt_dev_module_duplicate()` matches on, assigned at load from
`dev->iop_instance++`, a counter `dt_dev_init()` zeroes once for the darkroom's long-lived dev and
nothing resets after: every darkroom entry reloads the modules into the same dev and numbers them
anew. With it in the key, a darkroom → lighttable → darkroom round trip rekeyed every cacheline of
the image, and the preview recomputed from `basebuffer` while the whole cache was still there —
measured: the entry it could have resumed from was present, and it asked for keys never seen
before. It stayed harmless as long as `dt_dev_load_modules()` zeroed the counter before numbering,
so every entry numbered the modules alike; only the increment is left there now. The same key feeds `hist->hash`, hence `img->history_hash` and the
`history_hash.current_hash` column, which changed per session for an unchanged history too.
`_hash_raster_masks()` folded it as well, into the blendop hash of every module consuming another
module's raster mask. `dt_iop_check_modules_equal()` still compares it, legitimately: that is an
identity test within one session, not a key.

Export paid for it too. `dt_imageio_export_with_flags()` builds a dev of its own, which
`dt_dev_init()` numbers from 0, while the darkroom's dev numbers from wherever its counter stands:
0 on the first darkroom entry of a session (its modules come from the `dt_dev_init()` of the view's
`init()`), further on after every `leave()` / `enter()`. With the id in the key, an export after
any darkroom entry but the first found none of the darkroom's cachelines and recomputed from
`basebuffer`.

*Re-measured 2026-09-29 against `1fb2281865`*, with `-d pipe -d perf -d verbose`, OpenCL on, a
6960x4640 raw: darkroom, export, lighttable, darkroom, export. `lens`'s global hash is
`2311758751283094755` in the preview, full and export pipes at every step. The first export runs
nothing up to and including `lens` and starts at `initialscale` (highlight reconstruction, 3.3 s in
the darkroom, is not rerun); the re-entry runs no module in either pipe; the second export starts
at `initialscale` again, an export keeping none of its own outputs (see "An export reads the
cache and keeps nothing of its own" below). What the export does recompute is everything after
`initialscale`, which the darkroom runs at the display scale (0.16 there) and the export at full
size: 3.8 s of that run, tone equalizer, a masked color balance rgb and dither the largest.
With `--disable-opencl`, darkroom, lighttable, darkroom, export: the same hashes, the re-entry runs
no module, and the export after it again starts at `initialscale`. On the CPU (i7-3770K) that
remainder is what an export costs: 19.8 s, of which color balance rgb 3.1 s and its masked
instance 9.0 s, filmic 3.3 s.

Anything else folded into a cache key owes the same test: would two sessions editing the same
image, with the same history, produce the same value?

### What a module reads from the pipe belongs in its key

*Found `540130f59d`, 2026-09-29.*

The key of a piece covers its parameters, not the pipe state its processing reads. Dither read
two things there: the precision and channels of the output format, `pipe->levels`, which its
automatic mode quantizes to, and whether the pipe is an export, which sets its step on the other
pipes from the display scale. Neither reached the key. Measured on two `ansel-cli` exports of one
raw, a JPEG and a 16-bit TIFF: the same dither global hash, two different output content hashes
(`-d pipe -d pipecache -d verbose`). A pipe keeping its outputs would have handed one export the
other's pixels.

`commit_params()` now copies both into `piece->data`, the processing reads them from there only,
and `runtime_data_hash()` folds `piece->data` into the key, as colorout already does for the
export's output profile. Same output content hashes as before, two global hashes. A module that
reads pipe state while processing owes the same: copy it at commit, read the copy, opt into
`runtime_data_hash()`.

### A module memoising its own intermediates keys them on `upstream_hash`, never on `global_hash`

*Found `1019fcd2e0`, 2026-09-24.*

`piece->global_hash` folds the module's own parameters, so it moves on every frame of a drag and
is useless as the key of anything the drag does not change. `piece->upstream_hash`
(`pixelpipe_hb.h`) is the other half: the cumulative hash of the PARAMETERS of the enabled
modules above this one, and of nothing else — no ROI, not this piece. It identifies the
transformation chain a drawn shape is back-transformed through, which is what a mask
rasterisation depends on, so a memo keyed on it survives an edit of the parameter being dragged.

Three things it has to get right, and each one is a way to key a memo on a lie:

- **It is published by a pass of its own** (`_publish_upstream_hashes()`, `dev_pixelpipe.c`),
  called from `dt_dev_pixelpipe_get_roi_in()` as well as from `dt_pixelpipe_get_global_hash()`.
  ROI planning is its first consumer and runs BEFORE the global hash is built, so a
  `modify_roi_in()` reading it would otherwise find either the value `dt_iop_commit_params()`
  invalidated or one describing a chain that has since changed. It reads no ROI, so running it
  twice per plan costs a walk and answers the same.
- **It is deliberately ROI-free**, for the same reason: what a consumer reads during ROI planning
  still describes the previous plan's ROI. It never describes the previous plan's *parameters*,
  since every commit path runs a hash pass before any planning.
- **It folds `dt_dev_pixelpipe_activemodule_disables_currentmodule()` per upstream piece.** That
  is GUI state, not history, so it is in no `piece->hash` — yet it decides whether a module takes
  part in `dt_dev_distort_transform_plus()`. Focusing crop moves every drawn shape's box without
  moving any parameter.

Because it carries no ROI and no pipe identity, the same value comes out for the FULL, preview
and export pipes of one image, and anything keyed on it that is genuinely pipe-independent — a
shape's bounding box in sensor coordinates — is computed once for all of them. Anything laid out
in the module's own ROI must fold that ROI into its own key on top; `iop/retouch.c` does both.

### The host-memory fit probe evicts: ask it only when its answer chooses something

*Found `5642f84519`, 2026-09-11.*

`dt_tiling_piece_fits_host_memory()` (`develop/tiling.c`) is not a pure question. To answer "does
this module's working set fit untiled?" it evicts LRU cache lines — any pipe's — until the byte
headroom covers `factor × roi × bpp` AND `0.9 ×` the largest contiguous arena run does too. The
contiguity term is what makes it expensive: with a fragmented arena it keeps evicting long after
the bytes are there, measured at ~8 GB shed for a 1.95 GB working set.

`pixelpipe_cpu.c` therefore asks it only when `piece->process_tiling_ready` — i.e. when the answer
picks `process_tiling()` over `process()`. For a module without `IOP_FLAGS_ALLOW_TILING` (tone
equalizer, among others) `process()` runs either way, and the probe used to throw the cache away
for nothing: with `darkroom/render_size = 0` the preview pipe runs tone equalizer at full sensor
resolution (see the toneequal section), so every edit emptied the cache down to ~3 GB, the FULL
pipe's intermediates went first (least recently used, since the preview had just re-read its own),
and the FULL pipe recomputed from `basebuffer` — 8 s of highlight reconstruction per edit — while
the preview resumed from the edited module. The module's real allocations still go through the
cache allocator, which evicts what each of them needs when it is made.

Diagnose this class with `-d dev -d perf -d pipecache`: the ``processed `Module' … [pipe]`` lines
say which modules each pipe actually ran, and a burst of `LRU … removed` lines right after one of
them names the allocation that emptied the cache.

### ...nor when the tiler would fall back to `process()`

*Found `1fb2281865`, 2026-09-29.*

A tileable module chooses nothing either when `default_process_tiling()` would not tile it: when
the module's own factor is under 2.2 with a small overhead, tiles save nothing over its input plus
its output, and both tiling paths fall back to `process()`. That is most pointwise modules. The
probe is therefore asked only when `dt_tiling_piece_can_save_memory()` is TRUE too, and that
function and both tiling paths share one test (`_tiling_saves_memory()`, `develop/tiling.c`). It
reads the module's OWN factor: the probe is handed the factor aggregated with blending, 3.5 for any
module whose blending is not disabled, colorin included, and blending is never tiled.

Measured on a CPU export of a 6959x4639 raw inside `systemd-run -p MemoryMax=5G` (a 2560 MiB
cache), with a trace in the probe. Asked for every tileable module, it answered "does not fit" from
demosaic to colorout, each time after evicting all it could (the cache left at 985 MiB: the
module's input and output), and every one of those modules but demosaic then fell back to
`process()`. Asked only where tiling can save memory, it runs once, for demosaic, which tiles as
before; the run takes the same time (24.3 s against 24.1 s) and the pixels do not move (they
differ between the two by what two runs of one binary differ by: the dither is random).

Still open: the probe's total, `factor × roi × bpp`, counts the input and output buffers the
factor includes, and the pipe has already allocated both when it asks. In that run it asked for
1724 MiB with 1575 MiB free, 985 MiB of the cache being those two buffers; the 740 MiB it lacked
would have fitted. Discounting them would change which modules tile, so it wants its own
measurement.

"`with tiling`" on a processed line means `process_tiling()` was called. It can still fall back
(an allocation failing, too many tiles), and `-d tiling` says when it does.

### The cache gives memory back on kernel pressure, not only on low available RAM

*Found `18f6f8f2c5`, 2026-09-12.*

The pixelpipe cache budget (`total − memory_os_headroom − memory_mipmap_cache`) is a plan made at
startup, and `_system_memory_pressure_valve()` guards a floor of available RAM (200 MiB). Neither
sees a machine that still reports memory available but spends its time reclaiming it: swap full,
other applications' pages evicted and faulted back in. That stall is what systemd-oomd acts on —
on an Ubuntu 24.04 session, past 50 % "full" stall of `user@.service` for 20 s it SIGKILLs a
cgroup under it, Ansel being the obvious one. Measured on a 24 GB machine with other
applications holding ~9.5 GB and a full swap: the cache reached 11 GB of its 14.5 GB budget,
pressure hit 73 %, and oomd killed Ansel with MemAvailable nowhere near the floor.

**Three modules, and the seam between them is what keeps each one readable.**
`system/memory_pressure.c` is the only file that knows what a kernel counter looks like: it reads
PSI's cumulative "full" `total` for the whole system and every cgroup above the process, and it
owns the watcher — `dt_memory_pressure_watch_start()` arms the kernel's own triggers, sleeps a
thread of its own in `poll()`, and calls back. Everything `#ifdef`-ed on a platform lives there
and nowhere else. `caches/pixelpipe_cache_pressure.c` turns two of those reads into the stall
share of the window between them (2 s), the highest over the levels; past 10 % it sheds a quarter
of the cache (half past 30 %) and lowers the ceiling, the budget allocations evict down to. Under
2 % the ceiling climbs back by 1/32 of the plan per window, but only to 7/8 of the mark, the
footprint pressure struck at, which itself rises by 1/1024 of the plan per calm window. It reads
no cache entry: `pixelpipe_cache.c` passes it a `dt_pixelpipe_cache_pressure_sink_t` — what the
cache holds, and what evicting down to a target and trimming the arena gives back — and takes
`lock` around every call, the watcher's callback included. `dt_pixelpipe_cache_pressure_react()`
runs in `_free_space_to_alloc()` and in the idle shedder, which ticks every 2 s;
`dt_pixelpipe_cache_pressure_triggered()` runs on the watcher's thread. The arithmetic of the
ceiling and the mark is inline in `caches/pixelpipe_cache_pressure.h`, kept pure and separate for
the same reason `develop/pipe_cache_policy.h` is: `tests/unittests/test_pipe_cache_pressure.c` is
the only thing that can see a policy which changes no pixel and no hash.

Six things a reviewer would otherwise change:

- **A cache holding less than an eighth of the plan sheds nothing and lowers nothing.** The stall
  is somebody else's then. Measured: four kernel wake-ups in the first 12 s of a run, before the
  cache held anything, recorded a pressure mark of 0 and pinned the budget at the floor, where it
  still sat a quarter of an hour later. The same eighth is the floor the ceiling never falls under.

- **The kernel wakes the watcher; the cache does not only poll.** A measured window needs the
  cache to be running something, and past a certain stall nothing of ours runs: a second test died
  with its last 32 seconds silent — no timer, no allocation, the process frozen at 13.6 GB while
  oomd counted its 20 seconds, and the valve never sampled the ramp that killed it. PSI *triggers*
  (`dt_memory_pressure_watch_start()`, `poll()` for `POLLPRI`) are raised by the kernel as soon
  as a window is stalled past the threshold. Unprivileged triggers need a window that is a
  multiple of 2 s, and only the levels the process may write to accept one: the whole system, its
  own cgroup, and `app.slice` — the session's `user@.service`, which is what oomd actually
  watches, belongs to root.
- **The ceiling does not climb straight back to the plan.** Measured on the machine above: with
  the ceiling restored in ~80 s, the stall came back within half a minute of each recovery, five
  times in five minutes, once at 49 % — one point under oomd's limit. The footprint that caused
  it (11.5–12.8 GB there) is what the cache must stay under, until minutes of calm say the rest
  of the machine has let go.

- **The share comes from the `total` counters, never from PSI's `avg10`.** That is a 10 s moving
  average, which keeps reading high for some 20 s after a stall has ended: shedding on it drains
  the whole cache for pressure that is already gone.
- **The ceiling is a target, not a limit.** An allocation it cannot make room for still goes
  ahead; only `max_memory` fails one. A hard ceiling would turn pressure into failed pipelines.
- **`dt_dev_pixelpipe_cache_get_usage()` reports the ceiling, floored at the current usage.**
  Tiling must plan against the lowered budget, and both of its readers compute `max − current`
  unsigned.

### OpenCL GUI-thread materialization hazard

*Found `22f623c0be`, 2026-06-25.*

`dt_dev_pixelpipe_cache_peek_gui` must pass `preferred_devid = -1` (CPU caller signal). Passing
a real GPU id causes the GUI thread to enqueue a GPU read without owning the device, racing the
pipeline's OpenCL events → SIGSEGV in `clReleaseEvent`. Device-only entries then report a miss
to the GUI, which waits for the pipeline to publish a host copy instead.

### A cacheline's host copy answers for its hash only once it was written for it

*Found `0c28ca29a5`, 2026-09-27.*

`_seal_opencl_cache_policy()` (`develop/dev_pixelpipe.c`) decides per module whether its output
must be copied from device to host RAM (`piece->cache_output_on_ram`), through the pure
`dt_dev_pipe_cache_policy_decide()` (`develop/pipe_cache_policy.h`, pinned by
`tests/unittests/test_pipe_cache_policy.c`). The requirement travels ONE hop: a node publishes to
RAM when the node consuming it reads RAM (CPU-only, focused, histogram, autoset), never because
some node further downstream does. Carried transitively, it would copy every output device->host
on every frame: measured on a painting stroke, 152 MB and 68 ms of a 113.8 ms frame, of which
gamma's 11.7 MB alone is read.

What makes one hop safe is the cache, not the policy. A module's output cacheline is reused for
its next output by rekeying it in place (`_cache_try_rekey_reuse_locked()`), host buffer included,
and an OpenCL output kept on the device leaves the previous hash's pixels in that buffer. The rekey
therefore sets `dt_pixel_cache_entry_t.host_stale`, and while it is set
`dt_pixel_cache_entry_get_data()` answers NULL, which every lookup (`ref_entry_by_hash`,
`ref_host_entry_by_hash`, `peek`, the fast-track exact hit in `process_rec()`) and every consumer
already treats as "the pixels are on the device": a CPU consumer copies the device payload back,
a GUI reader owning no device reports a miss. The flag is cleared where the host buffer is written
for the new hash: by `process_rec()` through `dt_dev_pixelpipe_cache_flag_host_written()` when the
module ran on CPU, tiled, or read its output back (`cache_ram_output`), and by every device->host
copy (the materialize path, `dt_dev_pixelpipe_cache_restore_cl_buffer()`, `_gpu_init_input()`).
The producer alone reads the raw buffer, through `dt_pixel_cache_entry_get_buffer()`: it still
backs the output's pinned OpenCL image. `-d pipecache` prints `stale 0|1` on every entry line;
`tests/unittests/test_pipe_cache_host_stale.c` pins the contract.

An overlay toggle is the case that exposes it: with `rawoverexposed` on, `colorout`'s requirement
drops and its rekeyed cacheline keeps pre-toggle host bytes under each new pan/zoom hash; once the
overlay is off, `dither` (CPU-only) needs `colorout`'s output in RAM, and only the flag stops it
from reading those bytes.

**Do not detect this in the seal.** Invalidating a node's cacheline when its requirement goes
FALSE->TRUE looks equivalent and is not. `dt_iop_commit_params()` resets `cache_output_on_ram` to
0 before every commit, so every recommitted node "transitions" on every resync; and at that point
`piece->global_hash` holds the module's LOCAL hash (the commit writes it, the cumulative key is
computed later). Such an invalidation would fire for every node of every resync and match no
cacheline at all.

### After a switch between history states, outputs are kept rather than rekeyed

*Found `0c28ca29a5`, 2026-09-27.*

Toggling a module, undo, redo and a jump in the history list are switches the user is likely to
make again in reverse. Rekeying in place overwrites the output each module produced for the
state being left, and switching back then recomputes everything downstream of the change: 2.0 s
per toggle on the CPU for tone equalizer on a 5198x3904 raw, the same ten modules every time,
with 1.7 GB of a 23 GB cache in use.

`DT_DEV_PIPE_SWITCHED` (`pixelpipe_hb.h`) marks such a change. `dt_dev_pixelpipe_change()` turns
it into `pipe->keep_outputs`, `process_rec()` then writes each output into a new cacheline instead
of rekeying (`allow_rekey_reuse`), and `dt_dev_pixelpipe_process()` lowers it once a run completes
without error or shutdown, so only the run rendering the new state keeps the old one. Measured
toggling color balance rgb in the darkroom: the first toggle recomputes its downstream once, every
later one is a chain of exact hits, 1-28 ms on the CPU and 3-24 ms with OpenCL. It is raised
by `dt_dev_history_commit_item_now()` when `add_new_pipe_node` (the module's first history entry,
or an entry enabled differently from its last one -- the top item rewritten in place included),
and by `dt_dev_history_pixelpipe_update()`, which every wholesale history replacement goes through.

Two things a reviewer would otherwise simplify:

- **The pipe's change status cannot tell a switch from a drag.** `SYNCH` also carries every slider
  commit on a module with a drawn or raster mask, and crop/ashift edits; the darkroom worker raises
  `TOP_CHANGED` on its own whenever the history hash moved (`_resync_pipe_with_history()`), toggles
  included. Keeping outputs on every `SYNCH` gives each step of a masked-module drag its own
  cachelines and its own vRAM, which fills a 6 GB card within seconds. Only the source knows.
- **`add_new_pipe_node` is computed from the module's last history entry in every case**, whether
  the top item is rewritten in place, a new item appended or one forced. A toggle it misses reaches
  the pipe as `TOP_CHANGED`, and nothing downstream knows it was a switch.

### An export reads the cache and keeps nothing of its own

*Found `1fb2281865`, 2026-09-29.*

`dt_dev_pixelpipe_init_export()` sets `no_cache`, as the thumbnail pipe does. Every output the
export computes is then flagged auto-destroy (`_bypass_cache()` in `process_rec()`) and goes once
the next module has consumed it; the final output goes at `dt_dev_pixelpipe_cleanup()`, after
`dt_imageio_export_with_flags()` has copied it. Lines other pipes hold are still reused:
`_bypass_cache()` skips the fast-track lookup, so the recursion walks up to `basebuffer`, and each
line that exists comes back from `dt_dev_pixelpipe_cache_get_writable()` as an exact hit, never
flagged (`-d dev` prints `writable-exact-hit` for it). The darkroom's full-resolution lines, up to
`lens`, are reused that way.

Kept, the export's outputs were gigabytes nothing reads again. Measured on a CPU export of a
6959x4639 raw after the darkroom, with `-d pipecache -d memory` and a 200 ms sampler of
`/proc/meminfo`, `/proc/vmstat` and PSI: the cache went from 1.6 to 9.0 GB in 19 s and, right after,
~550 ms of "full" stall in a second made the pressure reaction shed 5 GB, least recently used
first: the darkroom's full-resolution lines. The next export recomputed from `basebuffer`, 24.4 s
against 19.3 s, and pushed MemFree down to ~200 MiB while 11 GB still counted as available: kswapd
swapped other applications out (up to 17k pages per 0.2 s), the system-wide stall passed the
threshold again (~210 ms in a window, Ansel's own cgroup not stalled at all), and 4.5 GB more were
shed. With `no_cache`: the cache is at 2.1 GB when the export ends and back to 1.6 GB after it, RSS
3.7 GB at most, MemFree never under 905 MiB, no page swapped, 3 ms of stall, nothing shed, and the
second export hits the darkroom's lines again (19.4 s). Six raws exported by `ansel-cli` on the
CPU: peak RSS 9.6 GB -> 3.0 GB, cache between images 4.1-6.4 GB -> 0, pressure reactions 2 -> 0,
same run time (94 s, 90 s), pixels identical to each image exported alone.

What goes: an identical re-export no longer finds its final output.

### An arena allocation is not zeroed: a module must not read what it did not write

*Found `1fb2281865`, 2026-09-29. RCD and `lens` re-measured against `119b6c8cd1`, 2026-09-30.*

`dt_pixelpipe_cache_alloc_align_*()` hands out arena memory. Fresh pages read zero; a recycled
allocation holds whatever its last user left. A module reading elements it never wrote is
therefore deterministic only as long as its memory is fresh, and the bug shows up as an image
that exports differently depending on what ran before it -- more often the more the arena
recycles: after a pressure shed, or when a pipe drops its outputs as it goes.

RCD (`iop/demosaic/rcd.c`) did: full tiles read elements of the per-thread `rgb` scratch that no
tile writes, and only partial tiles cleared it. Found on a batch export: an image differed from
its isolated export (mean 0.36 on 8 bits, in patches); comparing each module's output across the
two runs (`-d pipecache -d verbose`, `[pixelpipe_stats] ... content_hash`), demosaic was the first
to differ, from the same input. Poisoning each scratch buffer at allocation, only `rgb` moved the
output (9.6 M pixels), besides `VH_Dir`, whose clearing the poison overwrote. Zeroing `rgb` once at
allocation, as `VH_Dir` already was, made the images of a six-raw `ansel-cli` batch whose demosaic
ran untiled identical to each one exported alone -- reproducible, not right: the zeroes still
reached the output. RCD's tiles kept one row and column too many; with a 10-pixel border, nothing
the scratch holds reaches the output any more, on the CPU and in OpenCL, whose kernels had the same
fault at the image edges. See [RCD's own tiles need a 10-pixel border](raw-roi-cfa.md#rcds-own-tiles-need-a-10-pixel-border-the-image-edge-included).

*Found wrong 2026-09-30:* this section called "demosaic run in tiles does not give the pixels it
gives untiled" still open *and separate*. In the same batch, a pressure shed had lowered the
budget and the last three images ran demosaic tiled (`-d tiling`: no fallback); those three
differed from their isolated exports (mean 0.33-0.49 on 8 bits), the three untiled ones did not. It
is the same defect: the zeroes sat on RCD's tile seams, and pipe tiling moves the seams. With the
border fixed, tiled and untiled differ by float rounding upstream of `dither`; the 8-bit output
still differs by one level on many pixels, because Floyd-Steinberg diffusion carries any
difference along the rows.

`lens` wrote the fourth channel of its output only when a mask is displayed. Elsewhere it was
whatever the arena slot last held: two isolated exports of the same raw published different `lens`
outputs (alpha -0.031..0.999 in one, -0.020..0.9997 in the other), and poisoning RCD's scratch put
±5.7e7 in `lens`'s alpha through the recycled slot. `colorin` rewrites alpha, which is why nothing
after it differed. `lens` now always writes the whole pixel, alpha 0 outside mask display; the same
two poisons leave its output bit-identical. RCD's image border had the same fault: the outer three
pixels' alpha was never written on the CPU (`rcd_ppg_border()`), and in OpenCL
`border_interpolate` wrote an uninitialised `w`.

*Found on top of `4306d5f952`, 2026-10-01.* In OpenCL, the three `lens_distort_*` kernels sample
alpha along with green and used to write it out whether or not a mask was displayed: their alpha
was the input's, resampled, where the CPU writes 0. Not a read of unwritten memory, but a GPU output
that is not the CPU's. On a NIKON Z 6 raw (6064x4040 out), `lens`'s input alpha is 0 everywhere, so
nothing showed. Overwriting the input's alpha with ±5.7e7 just before the kernel (read back with
`dt_opencl_copy_device_to_host()` in `process_cl()`, written with
`dt_opencl_write_host_to_device()`) put nonzero alpha on 23.9 M (bilinear) and all 24.5 M
(bicubic, mitchell) of the 24.5 M output pixels. The kernels now take the mask-display flag and
write 0 outside it: with the same poison their output is bit-identical to the unpoisoned one, and
with the flag forced on it is bit-identical to the old kernels', poisoned or not. The exported
image is the same in every case, `colorin` rewriting alpha. Where nothing moves (the copy and
`lens_vignette`), `lens` carries its input's alpha, on the CPU and in OpenCL alike.

To look for others: poison with a large FINITE value. NaN is absorbed by `fmaxf()` and by
comparisons, and `-d nan` fills only the output buffer and checks only its RGB channels. In OpenCL,
fill each buffer after allocation from a host buffer holding the poison
(`dt_opencl_write_buffer_to_device()`). To read an OpenCL module's output, its host copy has to be
forced for the measurement: the export pipe is `no_cache`, so `/plugins/<op>/cache` does not apply
there (`cache_ram_output` in `pixelpipe_hb.c` requires the cache not to be bypassed).

