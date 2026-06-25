# Ansel — Developer Notes for AI Assistants

This file captures non-obvious architectural rules and hard-won bug knowledge for the Ansel
codebase. Read it before touching the areas it covers.

---

## Architectural rules

### No SQL in GUI modules

`src/libs/` and `src/views/` modules must contain no raw SQL. Database access belongs behind
named functions in `src/common/` (e.g. `common/collection.c`, `common/film.c`). When a GUI
module needs data, add or extend a `dt_collection_*` / `dt_film_*` / `dt_tag_*` function and
call it. Reuse existing helpers (`dt_collection_get_extended_where`, `dt_film_get_id`,
`dt_selection_select_list`) rather than re-issuing SQL.

Examples added during the collect rewrite: `dt_collection_get_property_values()`,
`dt_collection_get_images_for_rule()`, `dt_film_relocate()`.

### Pipeline↔module interface is history

The ONLY thread-safe interface between the pixel pipeline and an IOP module is **history**
(guarded by `dev->history_mutex`). `module->params` and `module->blend_params` belong to the
GUI thread and are NOT thread-safe — the pipeline thread must never read or write them.

Do NOT call `dt_iop_commit_params(module, module->params, ...)` from pipeline code. Commit
from the history snapshot (`hist->params`), never the live module params.

To push live/transient state to the pipe (e.g. drawlayer realtime stroke, ashift edit mode),
either (a) write it through history under `history_mutex`, or (b) use the transient-resync
interface `dt_dev_transient_params_{set,clear,get,active}` in `dev_history.{h,c}`.

See `doc/reorganisation.md` for the threading model (GUI diamond nodes vs. pipeline round nodes).

---

## Preferences system

Adding a Preferences entry requires **three** edits, not one:

1. `data/anselconfig.xml.in` — the `<dtconfig prefs="..." section="...">` entry.
2. `data/anselconfig.dtd` — the `section` attribute is an enumerated list; a new section value
   must be added here or `xmllint` fails the build (`USE_XMLLINT=ON`).
3. `tools/generate_prefs.xsl` — the GUI is generated into `build/src/preferences_gen.h`; each
   tab renders only **explicitly enumerated** `<xsl:for-each select="...@section='X'">` blocks.
   A section not listed in the XSL is silently dropped from the UI even if valid in XML/DTD.

Conf defaults: `dt_conf_key_exists()` returns true for any confgen key even on first run (defaults
are loaded at startup). To detect "user has never decided", use a **non-confgen** sentinel key
written only after the user acts — see the Sentry consent flow in `src/common/sentry.c`
(`sentry/consent_asked`).

---

## Pipeline & cache system

### GUI backbuf must use the published hash, not the planned hash

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

### Mipmap invalidation is explicit, not hash-driven

The mipmap cache get path (`_generate_blocking` in `common/mipmap_cache.c`) does NOT compare
`history_hash` vs `mipmap_hash` to detect staleness. Regeneration only happens after an explicit
`dt_mipmap_cache_remove(cache, imgid, TRUE)`.

Every operation that mutates an image's history/development MUST explicitly:
1. `dt_mipmap_cache_remove`
2. Refresh the cached image metadata so `history_items` is correct (`_write_mipmap_to_disk` uses
   `img->history_items > 0` as the "altered" flag for the embedded-JPEG-vs-raw decision)
3. `dt_thumbtable_refresh_thumbnail`

The darkroom/paste path does this via `dt_dev_history_notify_change` (`dev_history.c`). Paths that
write history straight to DB (XMP load, `dt_image_set_flip`) bypass it and need the fix pattern:
`dt_image_cache_get_reload`, then remove mipmap + refresh thumbnail.

Do NOT refresh the filmstrip from darkroom write paths — it competes with the realtime main
preview pipeline. Lighttable ops may refresh both.

### OpenCL GUI-thread materialization hazard

`dt_dev_pixelpipe_cache_peek_gui` must pass `preferred_devid = -1` (CPU caller signal). Passing
a real GPU id causes the GUI thread to enqueue a GPU read without owning the device, racing the
pipeline's OpenCL events → SIGSEGV in `clReleaseEvent`. Device-only entries then report a miss
to the GUI, which waits for the pipeline to publish a host copy instead.

---

## IOP modules

### ashift: preview buffer and crop geometry

The reference for ashift edit-mode is the **crop module** (`src/iop/crop.c`) — same "show the
full uncropped image while editing a clipping module" problem.

**Show the full image by neutralizing the crop in `commit_params()`** (crop: `cx=cy=0,cw=ch=1`;
ashift: `cl=0,cr=1,ct=0,cb=1`). Output, input, view, and size caches must all describe the same
full frame. Do NOT widen only `roi_in` while leaving `roi_out` cropped — the pipe renders a
cropped output while the view wants the full image → preview aborts at `initialscale` and
restarts forever.

**`g->buf` comes from `process()` capture, not `peek_gui`.** During edit ashift is cache-bypass,
so `process()` runs on every render and copies its input into `g->buf`. Do NOT use
`dt_dev_pixelpipe_cache_peek_gui()` for ashift's own input — it never runs `process()`, the
intermediate is evicted before the GUI reads it → re-request loop.

**Auto-crop geometry needs only size, not pixels.** Use `piece->buf_in.width/height`
(crop-independent), NOT `roi_in`/`g->buf` dims. Use the preview pipe's `buf_in`, not the virtual
pipe.

**`has_preview_output()` requires matching both portrait and landscape.** ashift runs before
`flip` (iop_order 16 < 20), so on portrait images its `roi_out` is landscape while preview dims
are post-flip portrait. The guard must also accept the swapped match
(`width==preview_height && height==preview_width`).

### drawlayer: realtime stroke correctness

**Stroke truncation:** `dt_drawlayer_commit_dabs` must guard on `painting_active` for BOTH quiet
and record-history commits. `_build_runtime_schedule` schedules quiet commits for `GUI_SCROLL`
and `GUI_SYNC_TEMP_BUFFERS` — these fire during active strokes and will truncate the path if
`commit_dabs` does not early-return. The fix: `if(g->manager.painting_active){ ... return TRUE; }`.

**Realtime trigger / hover thrash:** `_update_realtime_state` must track `painting_active` only.
The `GUI_RAW_INPUT` / `SAMPLE` override that set `realtime_active=TRUE` regardless of
`painting_active` caused hover mouse-moves to toggle realtime ON/OFF on every pixel → ~44ms
`resync_history_main` at each stroke boundary. Only an actual stroke (`STROKE_BEGIN`/`STROKE_END`)
should enter/leave realtime.

**Partial composite gate:** gate the damage-limited resample on the stable per-layer identity
`process->base_patch.cache_hash` (NOT on `piece->global_hash` — the heartbeat bumps
`stroke_commit_hash` every frame, so `global_hash` changes every realtime frame).

**Transient-params channel:** the realtime heartbeat (`_publish_backend_progress` in `worker.c`)
publishes via `dt_dev_transient_params_set` instead of `add_history_item`, avoiding per-heartbeat
undo/DB churn. History is written only at the real commit. Crop/ashift use `resync_history_all`
(full, all pipes); drawlayer heartbeat raises `TOP_CHANGED` + redraw (fast, non-geometry). The
two must NOT be mixed — routing crop's geometry through `_sync_focused_in_place` (partial)
mishandles the warm cropped→uncropped geometry change.

---

## Collection / Library module

`src/libs/collect.c` is the left-panel "Library" GUI. It does NOT build the collection query —
that is `src/common/collection.c`. The GUI's only job is to write the conf keys
`plugins/lighttable/collect/{num_rules, item<N>, mode<N>, string<N>, tab}` and call
`dt_collection_update_query()`.

Three tabs: **Folders** (film-roll list / folder tree; relocate + remove in batches),
**Collections** (tag browser + delete + rename), **Queries** (multi-rule builder + raw SQL via
`DT_COLLECTION_PROP_QUERY`).

Drag-and-drop of lighttable images onto tree rows was attempted and abandoned — a GtkTreeView
with a manual `gtk_drag_dest_set` reliably receives motion but does not deliver the drop on tree
models. DnD was removed entirely at the maintainer's request; do not re-add without a
non-tree drop target or `tagging.c`-style full source+dest.

---

## GTK / UI

### Thumbtable scrolled-window sizing

Three separate, mostly-invisible per-cell overheads must be budgeted for the thumbtable
`GtkFixed` grid to fit flush:

1. **scrollbar-spacing** — GtkScrolledWindow legacy GtkWidget *style property* (default 3px),
   NOT a CSS box property. Zero it via `-GtkScrolledWindow-scrollbar-spacing: 0` in CSS on
   `#thumbtable-scroll` / `#panel-scroll`.
2. **frame borders** — `GTK_SHADOW_ETCHED_IN` + the implicit GtkViewport's `GTK_SHADOW_IN` both
   add a `.frame` class. Set both to `GTK_SHADOW_NONE`.
3. **per-cell decoration** — `.thumb-cell { border: 4px transparent; margin: -2px }` makes each
   cell ~4px wider than the `thumb_width` stride. Budget it:
   `thumb_width = floor((new_width - deco) / cols)`.

**Critical:** `dt_thumbtable_configure` is the single source of truth for thumb geometry. Pass
the already-computed `new_thumbs_per_row/new_thumb_width/new_thumb_height` to `_grid_configure`,
which must only STORE them, never re-derive. If two code paths compute thumb geometry with
different formulas, `thumbs_changed` is true on every idle tick → full grid repopulate every
tick → ~20% idle CPU.

**Filmstrip-specific:** the filmstrip `scroll_window` must be the MAIN child of `parent_overlay`
(via `gtk_container_add`), NOT an overlay child. Overlay children on Wayland use an offscreen
path and go stale/blank until a pointer event invalidates them. The filmstrip vertical scroll
policy must be `GTK_POLICY_EXTERNAL` + `set_min_content_height(1)` +
`set_propagate_natural_height(FALSE)` to allow the resize handle to shrink the panel.

**Re-entry init:** `dt_thumbtable_show` must reset `last_parent_width/height` and
`last_h_scrollbar_height/last_v_scrollbar_width` to -1 so the next `size-allocate` always
reconfigures (the table persists across view enter/leave; the guard would otherwise skip the
reconfigure on same-size re-entry).

---

## Interpolation

Mitchell-Netravali (B=C=1/3) is the pipeline interpolator. Lanczos has been removed entirely.
Rationale: Lanczos has large negative side-lobes → halos at high-contrast edges and pushes
premultiplied alpha out of [0,1]. Mitchell is near-halo-free (~3% residual undershoot), sharp,
and a separable partition-of-unity kernel that fits the existing tap machinery for CPU and GPU.

The pipeline's interpolation architecture in `src/common/interpolation.c` is separable — each
kernel registers a 1D `maketaps`, and both `dt_interpolation_resample` (CPU) and
`dt_interpolation_resample_cl` (GPU) consume the same CPU-computed taps. A new separable kernel
is automatically CPU+GPU.

The drawlayer brush matte still forces `BILINEAR` explicitly — premultiplied alpha wants strictly
zero overshoot.

Config option strings in `anselconfig.xml.in` MUST equal the kernel `.name` field exactly;
`USERPREF` resolves by strcmp. A mismatch (e.g. `"bicubic (Catmull-Rom)"` vs `"bicubic"`)
silently falls back to default instead of erroring.

---

## Tools

**Sentry crash issues:** `tools/sentry-fetch-issue.sh <issue-id|url>` pulls a Sentry issue's
backtrace locally (writes `summary.txt`, `event.json`, attachments). The region host is
`https://de.sentry.io` (EU data residency) — `sentry.io`/`us.sentry.io` give 403/401.
Reading issues needs a **User Auth Token** (not the org token used for symbol upload).
See `doc/sentry.md` for setup details.

---

## General engineering

Ansel carries the burden of Darktable legacy, which made it a principle to entangle all
application layers (GUI, pipeline, history, database) and imported the whole software
into the whole software through `#include "common/darktable.h"`. This voids the modularity
principle, creates many bugs, data races, and makes any maintenance tedious and prone to 
edge effects, since the app is heavily asynchronous and parallel.

The Ansel codebase should move toward more enclosed modularity, making data structure private
to each translation unit and exposing only API to the outside (getters/setters/init/cleanup). 
Direct value changes on data not owned by the current TU are forbidden. The dependency graph 
should be simplified and only a minimal set of `#include` should be kept per TU. In particular,
`src/common/darktable.h` should inherit from lower-level modules, but lower-level modules
should not inherit it, so it should stop being the glue of all common helpers throughout
the software.

CRUD operations should have one central entry point for the whole software and run only
once, for as long as user didn't send new input, so the data lifecycle is legible and
cacheable.

Since every data flow in the software is a pipeline, issues should be tracked to their root
cause by climbing the call tree up until the source is found, instead of being fixed where
they are visible.