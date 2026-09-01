# Ansel — Developer Notes for AI Assistants

This file captures non-obvious architectural rules and hard-won bug knowledge for the Ansel
codebase. Read it before touching the areas it covers.

---

## Architectural rules

### `#pragma once` is FORBIDDEN — use an include guard

Every header in `src/` uses an explicit `#ifndef DT_<PATH>_H` / `#define` / `#endif` guard,
named after the path relative to `src/` (`src/develop/masks/masks_history.h` →
`DT_DEVELOP_MASKS_MASKS_HISTORY_H`). **Do not add `#pragma once` to an existing header, and do
not start a new one with it.**

This is not a style preference. `#pragma once` and an include guard behave identically at the
preprocessor level, but `#pragma once` *silently* makes a cyclic include graph compile: a
header re-entered mid-definition is skipped, and the first inclusion finishes with whatever it
had at that point. That is how three include cycles survived unnoticed in this codebase for
years, each one a header trailing-including a header that includes it back (see
`doc/include-graph.md`). Explicit guards make the same situation greppable and reviewable
instead of invisible.

Enforcement: `python3 tools/pragma_once_to_guards.py --verify` exits non-zero if any
`#pragma once` reappears. `python3 tools/include_graph.py --summary` must keep reporting
`cycles 0`.

**`darktable.h` (at `src/`, not in a module) has no guard either — it has a TRIPWIRE.** It ends up included by
at most one path per translation unit (an entry point calling `dt_init()`, or a subsystem
that owns one of the `darktable` members), so a *second* inclusion is never legitimate: it
means the header arrived through a path nobody intended. A guard would absorb that
silently; instead the file `#error`s on re-inclusion. If you hit it, do not add a guard —
find who included it and give that code the specific lib it needs (`common/logging.h`,
`system/mem_alloc.h`, …) or the accessor for the global it wants (`dt_dev_get_global()`,
`dt_control_get_global()`, …). **No header may include it**; as of this writing none does.

**When auditing this, grep for `darktable\.h"` and check the spelling.** Includes can be
written relative to the including file's own directory, which is how several files hid from
earlier audits while the header still lived in `src/common/`. It now sits at `src/`, so
`#include "darktable.h"` IS the canonical root-relative spelling. Three files (and one *header*, `common/colorchecker.h`) hid behind
that spelling through several audits of this series; the compile-time tripwire is what
finally caught them. `tools/include_graph.py` resolves both spellings and was right when the
ad-hoc greps were wrong.

**Five headers deliberately have NO guard at all** and must never get one:
`common/module_api.h`, `views/view_api.h`, `libs/lib_api.h`,
`imageio/format/imageio_format_api.h`, `imageio/storage/imageio_storage_api.h`. They are
X-macro headers, re-included several times in the *same* translation unit with different macros
defined, and expanded *inside struct bodies* to generate members. For the same reason a
**top-level `#include` in one lands inside those structs**. The precise rule, as the imageio
pair actually implement it: real includes must sit inside the `#ifdef FULL_API_H` block —
that macro is defined only in full-API mode, while the struct-body expansion defines
`INCLUDE_API_FROM_MODULE_H` instead and skips the block — and only *other* X-macro headers
(`common/module_api.h`, which has no includes itself) may be included unguarded. Symbols used
outside that block (`dt_version()`, `dt_print()`, `IS_NULL_PTR`) are the consuming `.c` file's
responsibility.

### A header includes only what its own declarations need

Everything else belongs in the `.c`. A header that includes more becomes a supply line its
consumers never asked for and cannot see: they compile because something upstream happened to
pull in what they use, and the day anyone tidies that include away the breakage surfaces
somewhere else entirely, in a file that was never touched.

This is not theoretical. Removing `gui/gtk.h` broke a dozen IOPs because it had been the only
thing pulling `sqlite3.h` in ahead of `common/points.h` (whose vendored SFMT `#define N` then
collided with `sqlite3_compileoption_get(int N)`). And within the same series, deleting an
unused `widgets/label.h` from `widgets/dialog.c` removed `dt_free` — arriving through
`label.h` -> `system/mem_alloc.h` — from a file that had never named either header.

Concretely: if a header declares `void f(GtkWidget *w)`, it includes `<gtk/gtk.h>` and nothing
more. Implementations do not belong there either — `widgets/label.h` carried five `static
inline` helpers, and those five forced four extra includes on all ~30 of its consumers.
Moving them to `label.c` left the header needing only `<gtk/gtk.h>`.

The one legitimate exception is a header whose published interface *is* inline code
(`widgets/draw.h`), which necessarily includes what that code calls. Keep those rare, and keep
them honest: they are a deliberate performance trade, not a convenience.

`tools/check_unused_includes.sh` gates the include lines a change adds.
`tools/header_consumers.py` reports what each includer of a header actually takes from it,
separating a header's own symbols from what it merely forwards.

**`header_consumers.py`'s "files using nothing from it" bucket does NOT mean the file can
drop the include.** It means *this include is redundant — the file reaches those symbols
through one of its other includes*. Those are exactly the files that are relying on the
supply line described above, so under this tree's rule they need the include **added
explicitly**, not removed. Deleting all seven such includes when `colorprofiles/colorspaces.h`
was split still compiled in Release *and* Debug, and broke `build-nofeatures`, where
`control/jobs/control_jobs.h` lost the two types `dt_control_export()` is declared with — the
other supplier only existed in the feature-full configurations. Confirm against the symbols
the file actually names (`grep` for the header's types and functions) before removing
anything, and never trust one build configuration to prove an include is unnecessary.

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

### Duplicating an image races its own thumbnail generation against the history copy

Lighttable "Duplicate" (`dt_control_duplicate_images_job_run`, `control_jobs.c`) creates the new
DB row via `dt_image_duplicate()`, then copies the source's history onto it via
`dt_history_copy_and_paste_on_image(..., DT_HISTORY_MERGE_REPLACE, ...)`. `dt_image_duplicate()`
(`common/image.c`) used to call `dt_collection_update_query(..., DT_COLLECTION_CHANGE_RELOAD, ...)`
unconditionally, right after inserting the row — i.e. *before* the caller had copied any history
onto it. That reload makes the new image visible to the lighttable grid, which can create its
thumbnail widget and request a render immediately, against the row's momentary real state: zero
history.

Confirmed with `-d cache -d history -d lighttable`: for a freshly duplicated image, the first
`[mipmap_cache] compute mip size 0 ... from original file` log line landed ~650ms *before* the
matching `[dt_dev_write_history_ext] writing history for image N` line. The mipmap cache is not
hash-driven (previous section), so once that first, historyless render is cached, only an
explicit `dt_mipmap_cache_remove` + refresh recovers — and even when that recovery path runs
correctly and a second, correct render finishes and gets cached, nothing guarantees a timely
repaint of it (the thumbnail widget can be left showing the first render under a permanent "busy"
overlay for several seconds, until an unrelated GUI event forces a redraw). Patching the
recovery/notification side (adding a missing GUI-thread redraw request on one early-return path
in `gui/dtgtk/thumbnail.c`'s `_get_image_buffer()`) did not fix this reliably and was reverted — the
actual fix is to not let the race start in the first place.

Fixed by `dt_image_duplicate_no_reload()` (`common/image.c`): same as `dt_image_duplicate()` but
skips the immediate collection reload. Both call sites that duplicate-then-copy-history
(`dt_control_duplicate_images_job_run` in `control_jobs.c`, `_history_style_apply`'s
duplicate-and-apply-style branch in `history_actions.c`) now use it and trigger exactly one
`dt_collection_update_query(..., DT_COLLECTION_CHANGE_RELOAD, ...)` themselves, after the
history copy/delete completes — so the very first time the duplicate becomes visible, it already
carries its final history. Any future caller of `dt_image_duplicate()` that will mutate the new
image's history afterward (a style, a batch edit, ...) should do the same rather than let the
default immediate reload race its own follow-up write.

### OpenCL GUI-thread materialization hazard

`dt_dev_pixelpipe_cache_peek_gui` must pass `preferred_devid = -1` (CPU caller signal). Passing
a real GPU id causes the GUI thread to enqueue a GPU read without owning the device, racing the
pipeline's OpenCL events → SIGSEGV in `clReleaseEvent`. Device-only entries then report a miss
to the GUI, which waits for the pipeline to publish a host copy instead.

### A toggled-on GPU module can erase a host-cache requirement inherited from further downstream

`_seal_opencl_cache_policy()` (`develop/dev_pixelpipe.c`) walks `pipe->nodes` backwards once per
resync to decide, per module, whether its output must be copied from device to host RAM
(`piece->cache_output_on_ram`). It threads a `current_output_must_cache_host` flag upstream: each
enabled module recomputes it from its own properties (`!supports_opencl || active_in_gui || ...`)
and hands the result to whichever module sits before it in pipe order. A **disabled** module is
skipped via `continue` before that handoff, so the requirement from further downstream correctly
keeps flowing through it untouched.

The bug was in the handoff itself: it assigned (`=`) instead of combined (`||`), so an **enabled**
GPU-capable module that itself needs no host input silently replaced — rather than passed through —
whatever requirement was inherited from further downstream. Toggling such a module from disabled to
enabled (e.g. `iop/rawoverexposed.c`: `IOP_FLAGS_NO_HISTORY_STACK`, GPU-capable, no GUI/histogram
reason of its own to need host data) made it start participating in the loop and erased the
correctly-propagated `TRUE` requirement coming from a CPU-only module further downstream (`dither`),
even though that module's own need for host data never changed.

Concretely: with the overlay enabled, `colorout`'s GPU kernel computed correct new pixels on every
re-render (`process_cl()` ran fine), but the GPU→host readback
(`dt_dev_pixelpipe_cache_sync_cl_buffer`, gated by `if(*cache_output)` in `pixelpipe_gpu.c`) was
skipped because `colorout`'s `cache_output_on_ram` came out `FALSE`. Because pixelpipe cache entries
are reused in place via hash *rekey* rather than freshly allocated
(`_cache_try_rekey_reuse_locked`), the stale host bytes from the entry's previous life persisted
under the new hash key. Once the overlay was disabled again, `dither` (CPU-only) read directly from
`colorout`'s cacheline and got those stale, pre-toggle pixels — silently mismatched from the current
pan/zoom, even though every hash and ROI in the chain was individually correct. Only reproduced with
OpenCL enabled (`--disable-opencl` sidesteps it entirely: every module then unconditionally needs
host data, so the flag is always `TRUE`).

Fixed by propagating the OR of the inherited flag and the current module's own requirement
(`current_output_must_cache_host = previous_output_must_cache_host || current_output_must_cache_host`),
so a host requirement, once established anywhere downstream, survives every enabled module between
it and whichever module's cache policy is being decided — regardless of whether any of those modules
dynamically toggles.

### The darkroom worker thread must be joined before view `leave()` tears down pipe state

`dt_dev_darkroom_pipeline()` runs forever in the dedicated `DT_CTL_WORKER_DARKROOM` job thread,
servicing `dev->preview_pipe` then `dev->pipe` in a loop. `views/darkroom.c`'s `leave()` sets
`dev->exit = 1` and each pipe's `shutdown` atomic, then — still from the GUI thread — calls
`dt_dev_pixelpipe_cleanup_nodes()` on `dev->pipe`/`dev->preview_pipe`/`dev->virtual_pipe` and frees
`dev->iop`/`dev->history`. Neither flag actually preempts the worker: `dev->exit` is only checked
between loop iterations and between servicing each pipe, and `pipe->shutdown` is never polled inside
`dt_dev_pixelpipe_process()` to abort mid-flight — it's only read afterwards, in
`dt_dev_darkroom_pipeline()`, to decide whether the just-produced result is valid.

`leave()`'s `busy_mutex` locks around the pipe-nodes teardown look like they serialize against the
worker, but `busy_mutex` carries a comment that it must "NEVER be used from the GUI thread" for
exactly this reason: two worker-thread accesses bypass it entirely — `dt_dev_pixelpipe_set_input()`
(iterates `pipe->nodes` every loop tick to refresh `piece->iwidth`/`iheight`) and the history-hash
resync at the top of `_resync_pipe_with_history()` (can re-trigger `dt_dev_pixelpipe_change()`,
which rebuilds `pipe->nodes` from `pipe->dev->iop`). Either can still be touching a pipe's nodes, or
`dev->iop`, after `leave()`'s mutex-guarded section already freed them. The resulting heap corruption
does not crash where it happens — it crashes wherever the next unlucky reader lands (Sentry issue
133807805: a garbage `xform_cam_Lab` pointer inside `iop/colorin.c`'s `cleanup_pipe()`, reached via
the worker's *own* next `resync_pipe_with_history()` call, nowhere near the actual race).

Fixed by making `dt_dev_pixelpipe_t.running` (set at the very top/bottom of
`dt_dev_darkroom_pipeline()`) an actual `dt_atomic_int` — it existed before but was write-only.
`leave()` now polls it for both `dev->pipe` and `dev->preview_pipe` right after setting
`dev->exit`/`shutdown`, and blocks until both read `FALSE` before touching any node/iop/history
teardown. Any other GUI-thread code that tears down darkroom pipe state must wait on this flag the
same way — `dev->exit`/`pipe->shutdown` alone do not guarantee the worker has stopped touching a
pipe.

### History items are refcounted; the pipe resyncs against a snapshot, not under `history_mutex`

`dt_dev_history_item_t` carries a `refcount` and must be constructed exclusively through
`dt_dev_history_item_create()` — never a bare `calloc` (mirrors the masks-forms rule below;
`dt_dev_history_cow_touch()` clones a shared item before an in-place mutation, mirroring
`dt_masks_cow_touch()`).

That refcount exists for one consumer: `dt_dev_pixelpipe_change()` (worker thread, called from
`dt_dev_darkroom_pipeline()`) used to hold `dev->history_mutex` as **reader** for the entire
O(nodes × history) pipe resync — every module's `commit_params()` — measured at **204–227 ms**
on the load-time resync of a 47-item mask-heavy history, with no user input at all. The GUI
thread needs the *writer* side of that lock on every commit (each slider tick, and each
throttled mask-drag commit that `views/darkroom.c`'s `_queue_delayed_history_commit()` fires
mid-drag), and glibc's writer-preferring rwlock policy then blocks every **new reader** behind
the queued writer too — so one slow resync stalled the whole application until it finished.
That is discussion #1098's "the shape moves in steps": each step is one lock acquisition.

Fixed by resyncing against a **snapshot**. `dt_dev_history_snapshot_take()` (`dev_history.h`)
copies the list cells and takes one reference per item under the read lock — microseconds —
and `change()` releases the lock before any `commit_params()` runs. The same load-time resync
now logs `resynced from snapshot in 174 ms, lock-free` with **no lock hold above the 1 ms print
threshold**; the compute cost is unchanged, only the lock is gone. The three other sync entry
points (`dt_dev_pixelpipe_synch_all`/`synch_top`, used by export, snapshots and the focus
overlay on throwaway devs) take their own brief snapshot the same way. The writer's COW gate is
the other half of the contract: a snapshotted item has refcount > 1, so `cow_touch` clones it
and the snapshot never sees a half-rewritten item. `src/tests/unittests/test_history_snapshot.c`
pins that contract; `-d history` shows the hold times.

**Three things about this design that are not obvious from the code:**

- **Capture `history_end` and the hash inside the same brief lock as the list.**
  `dt_dev_set_history_end_ext()` writes both together under the write lock. Reading the atomic
  hash *after* releasing lets a commit land in between and mark the pipe as synced to history it
  never resynced against — a missed recompute, silently. The snapshot struct carries all three.
- **`pipe->last_history_item` must hold a reference and be exchanged atomically.** It is the
  identity marker `synch_top` uses to bound an in-place top-entry rewrite to one node instead of
  a full resync. `cow_touch` re-points it at the clone from the GUI thread while the worker
  writes it outside the lock, so it is a genuine cross-thread slot: `dt_atomic_exch_ptr` keeps
  every interleaving refcount-honest (each side releases exactly what it displaced), and the
  held reference means a compare against a possibly-departed item can never hit a **recycled
  address** — a hazard the old under-the-lock raw pointer already had in principle. Do not
  "simplify" it back to a plain assignment; the worst case of a lost exchange race is one full
  resync, never a leak or a double free.
- **The async DB write job (`_dt_dev_write_history_job_run`) was the *second* long reader** of
  this lock, and it IS on the interactive path: it runs after every commit and held the read
  lock across the whole history+masks rewrite (every row deleted and re-inserted), so the GUI
  thread's *next* commit queued behind it — the wait `dt_dev_history_commit_item_now()` logs as
  "blocked acquiring history_mutex". Same cure: `_history_write_state_take()` freezes the
  snapshot **plus a deep copy of `dev->iop_order_list`** (the one other thing the rewrite reads
  from `dev`) under a brief lock, and `_write_history_from_state()` writes lock-free. The trap
  specific to this one: **`history_write_pending` must be cleared at snapshot time, under the
  lock — not after the write.** `dt_dev_write_history()` skips queueing while that flag is set,
  on the promise that the pending write will still pick the commit up; with a snapshot that is
  true only for commits landing before the freeze. Clearing after the rewrite would silently
  drop every commit made while the rewrite ran — the coalescing comment there spells out the
  two cases. The seven other `dt_dev_write_history_ext()` callers hold the lock as writers
  mid-commit and expect the write done on return; they keep that contract (state taken and
  written under their lock) and were not touched.

The named-rwlock diagnostic lives in `system/dtpthread.h` (`dt_pthread_rwlock_set_name()`,
opt-in per lock, combine with `-d history`); `dev->history_mutex` is named in `dt_dev_init()`.

### Drawn-mask drags render from the transient slot; history is written once, on release

The other half of #1098. `views/darkroom.c` used to make a mask drag visible by committing
history on the GUI throttle — about once per pipe render, mid-drag: a history rewrite, a DB write
job and a full `synch_top` per tick, with the shape's owner re-hashed against `dev->forms` only as
a side effect. It now takes the drawlayer brush's route (`_publish_mask_edit_transient()`,
mirroring `_publish_backend_progress()` in `iop/drawlayer/worker.c`): on every drag motion and
every scroll step, publish the owner module's *own, unchanged* params through
`dt_dev_transient_params_set()` and flag the main pipe `TOP_CHANGED`. That is only a ticket onto
`_sync_focused_in_place()`, which re-commits the one focused piece against the **live**
`dev->forms` — `dt_iop_compute_blendop_hash()` folds the group's geometry in, and
`dt_dev_pixelpipe_process()` re-snapshots `pipe->forms` on every recompute — so the piece re-keys
from the moved shape and only it and its downstream recompute. Nothing about the shape lives in
params; the geometry is in `dev->forms`, which is why publishing unchanged params works.

Three things a reviewer would otherwise "simplify" away:

- **The throttled commit defers itself while `dt_masks_gui_is_dragging()`** by re-queueing, and
  runs after the button comes up. It must not be suppressed outright: the release path only
  *queues* the commit, it does not commit, so a suppressed callback would never write history.
- **Flag with `dt_dev_pixelpipe_or_changed()`, not `_change_pipe()`.** The latter also raises the
  killswitch; per-motion killswitches abort every in-flight render and a fast drag never gets a
  frame. The drawlayer heartbeat makes the same choice for the same reason.
- **Clear the transient slot before the commit, and even when nothing changed.** The commit's
  resync must come from history, and a no-op drag must not leave the slot occupied. When the slot
  was active but `forms_changed` is false, flag the main pipe once so it re-syncs from history
  rather than keeping a render keyed to a slot that no longer exists.

Scrolling (size / feather / opacity) has no release, so there the throttle marks the end of the
burst: each step renders live, one commit lands after the last. The mask-manager path
(`libs/masks.c`, no focused module) is unchanged and still commits directly; the focused-piece
path needs `dev->gui_module` to be the shape's owner.

### `_insert_default_modules` must check `dev->history` in memory, not the DB row for `dev->image_storage.id`

`dt_dev_init_default_history()` (`dev_history.c`) walks every loaded module and, for each one,
calls `_insert_default_modules()` to backfill a default-params history entry for any
`default_enabled`/`force_enable` module "missing" from history. Its "is this module already
covered?" check used to be `dt_history_check_module_exists(dev->image_storage.id, module->op, ...)`
— a DB query against `main.history` for whatever image `dev->image_storage.id` currently points
at.

That's correct for the common caller, `dt_dev_read_history_ext()`: `dev->history` is empty and
`dev->image_storage.id` is the same image whose real DB history is about to be read into it a few
lines later, so the DB accurately reflects "not yet loaded, but will be." It's wrong for
`dt_dev_replace_history_on_image()` (image duplication, history "replace" paste): there,
`dev->history` is loaded from a *source* image, then `dev->image_storage` is repointed at a
freshly created, still-DB-empty *destination* image before `dt_dev_init_default_history()` runs.
The DB check always answers "missing" for the destination — regardless of what the just-copied
`dev->history` already contains — so every `default_enabled` module (`temperature`, `colorin`,
`colorout`, `demosaic`, ...) got a second, default-params history entry silently appended *after*
the one correctly copied from the source. Since replay applies history front-to-back and the last
entry per module wins, duplicating an image quietly reset those modules to their defaults instead
of reproducing the source's actual settings — "duplicate" wasn't an identical copy.

Fixed by checking `dt_dev_history_get_first_item_by_module(dev->history, module) != NULL` (via
`IS_NULL_PTR`) in addition to the DB check — the in-memory list already holds the correct,
about-to-be-persisted state for the destination in the duplicate/replace case, and is equivalent
to the DB check (same image, nothing loaded yet) in the common case, so neither caller's
behavior regresses.

### `piece->iwidth`/`iheight` go stale on the export pipe specifically

`dt_dev_pixelpipe_create_nodes()` copies `pipe->iwidth`/`iheight` into each `piece->iwidth`/`iheight`
once, at node-creation time — it is not refreshed on later ROI passes. Darkroom pipes call
`dt_dev_pixelpipe_set_input()` (which sets `pipe->iwidth`/`iheight`) before creating nodes; the
export pipe (`common/imageio.c`) does the reverse, so every piece was permanently stuck at 0 there
(issue #967: `iop/toneequal.c`'s blending radius and `iop/soften.c`'s glow radius silently collapsed
to 0 on export only, regardless of the module's params, while darkroom rendered correctly). Fixed by
having `dt_dev_pixelpipe_set_input()` re-sync `iwidth`/`iheight` onto any already-created nodes. See
`doc/resizing-scaling.md` for the full write-up; any other per-piece field seeded from `pipe->*` at
node-creation time is exposed to the same ordering hazard.

### `basebuffer` must crop using `roi_out`, not `roi_in`

`iop/basebuffer.c` is the first module in the pipe: it slices the requested window out of the
full-resolution mipmap-cache payload. Its `modify_roi_in()` unconditionally requests the whole
image (`{0, 0, pipe->iwidth, pipe->iheight}`) — `roi_in` never carries an offset, since basebuffer
needs the full frame available to crop from. The window actually requested downstream lives in
`piece->roi_out`, not `piece->roi_in`. `process()`/`process_cl()` must read the crop offset (and
the destination copy size) from `roi_out`, and use `pipe->iwidth`/`iheight` — not `roi_in->width`/
`height`, which is always the full frame too — for the source row stride. Reading the offset from
`roi_in` instead always crops from the sensor's true `(0,0)`: harmless whenever the requested
window is itself near `(0,0)` (a fit-to-screen view, a barely-cropping module), silently wrong by
the full requested offset otherwise (e.g. `iop/lens.cc`'s `scale` slider, whose backward-pass
`roi_in.x/y` grows with the zoom amount) — every downstream module still looks internally
consistent (sizes match, ROI planning round-trips cleanly), because each of them only reads
buffer-relative pixels and never re-derives its own absolute position from `pipe->iwidth`/
`iheight`. Parametric masks/forms don't go through this buffer-cropping path at all, so they stay
correctly positioned even when the base image content is offset — a mismatch between a mask and
the image it's drawn on is a symptom of this class of bug, not of the masking code.

### Highlights (harmonic): per-channel saturation floors imprint the floors' own chroma — magenta

The "harmonic reconstruction leaves highlights magenta" complaint (MAC25640.RAF) survived three
plausible-but-wrong theories (X-Trans phase mismatch, demosaic bleed, coefficient-fit slope error —
each killed by measurement, per the retouch note's warning about reasoning from source alone). The
actual mechanism, found by dumping the module's raw output and comparing per-channel means over
clipped photosites against the per-channel clip thresholds:

- Highlights receives **WB-applied** data (`temperature` runs before it; `clips[c] = 0.995 * clip *
  processed_maximum[c]` and `processed_maximum` carries the WB coefficients, e.g. `{2.29, 1.0, 1.31}`).
  A *white* surround is therefore `R≈G≈B` in this domain, and each channel saturates at a *different*
  level — the WB coefficients themselves.
- Every reconstruction stage ended by flooring each clipped channel independently at its own
  saturation level (`max(est_c, clip0_c)`, soft or hard). On a multi-clip pixel this snaps the
  clipped subset to the floors' chroma — the WB coefficients — i.e. neutral-sensor ≡ **magenta**,
  regardless of the hue the solver produced (measured: clipped R snapped to its 2.29 floor while
  reconstructed G stayed at 1.16 → G the lowest channel). The solver's chroma was fine; the floors
  destroyed it. Laplacian looked whiter only because it **violates** the floor (outputs clipped R at
  1.67 < 2.29): dimmer but not magenta — a trade, not a reference.
- Fix candidate: the **joint (hue-preserving) floor** — one scalar lift of the clipped subset,
  `s = max_k(t_k/e_k)` (t_k the per-channel soft-floor target), then `e_k ← s·e_k`: brightness from
  the most-demanding floor, hue preserved; identical to the per-channel floor for 1-clip pixels;
  capped (8×) with the per-channel floor kept after as the safety net. An early attempt applied it
  UNGATED at all three floor sites: it fixed MAC visually but regressed the whole article bench (all
  six original synthetic cases run at UNIT WB with EQUAL clips, where the per-channel floor imprint
  is neutral ≈ truth), so it was reverted pending the gate below.
- STATUS 2026-08 (implemented, CPU + CL + python prototype): the **clip-asymmetry floor gate**
  `_hl_floor_gate(clips)` (`highlights/common.h`) — `g = smoothstep((A − 1.25)/0.75)`,
  `A = max_c(clip_c)/min_c(clip_c)` — blends per-channel ↔ joint at every floor site, plus three
  surround-chroma refinements at full gate: a cmean pull of the self-dome ratios (β = 0.5g), a
  chroma-decoupled self-dome recombine, and a clip0-rehue in the joint core (redistribute the
  all-clip floor to the mean valid chromaticity, sum-preserving). Every site takes a
  `g ≤ 1e-6 → per-channel` early-out that is bit-exact approved behavior. TWO TRAPS measured here:
  (1) the ramp must NOT start at A=1 — `clips` inherit `processed_maximum`, which carries a ~10%
  non-WB profile wiggle even at unit WB (measured A=1.145 on the AsShotNeutral=(1,1,1) bench DNGs);
  a ramp from 1.0 opened the gate to 0.2 on the unit-WB bench and silently contaminated a whole
  candidate-export round. Real cameras sit at A≈2–2.6, so the [1.25 → 2.0] ramp separates cleanly.
  (2) the bench DNGs CANNOT exercise the gate through ansel-cli: `temperature` ignores their
  AsShotNeutral (pmax constant across cases), so pipeline clips stay near-equal even for the
  wbclips case — gate validation lives on real raws and in the python prototype's WB-regime leg
  (`py_reconstruct(..., wb=…)`, which scales the working domain and drives the same smoothstep).
  Measured: MAC neons' G/((R+B)/2) top band 0.839 → 0.931 (the magenta fix), python WB-regime
  wbclips chroma-share RMSE halved; but the surround-importing refinements (cmean pull, rehue,
  decouple) HURT self-coloured emitters (magentasun WB-regime cRMSE 1.29 → 2.95) — they trust the
  surround like the cgrad continuation does. They are therefore gated by their own trusted-ring
  vote `_hl_ring_flat_mean_vote` (common.h; `hl_ring_vote` kernel; `py_flat_mean_vote`):
  t = |ring_mean_shares − bright_mean_shares|_L1 / max(ring_dispersion, 0.02), vote = exp(−(t/5)²),
  refinement strength = floor_gate × vote. Three designs died by measurement first: (1) per-pixel
  agreement averaging — real rings' noise/texture spread caps the vote at ~0.3 at ANY tolerance
  tight enough to keep the emitter closed (tol 0.10 strangled MAC/sunrise at 14/11% of the
  refinement span; tol 0.25 leaked magentasun and fully opened gradsky); (2) the ALL-valid window
  mean as reference — polluted by dark unrelated content, closes the vote on exactly the scenes
  the refinements are for (the cgrad-anchor lesson again: use BRIGHT valid pixels ≥ 0.35 × blown-
  zone plateau; in `_joint_core` this bright mean must be a SEPARATE quantity from the approved
  all-valid mean that seeds the screened-Poisson solver); (3) a bright-surround-dispersion factor
  to close gradient skies — measured INVERTED (synthetic gradsky's bright dispersion 0.059 is
  LOWER than MAC's 0.11–0.20). The t-form works because a self-coloured emitter shifts the whole
  ring COHERENTLY (magentasun bias 0.21 / dispersion 0.007) while real scenes scatter AROUND the
  mean (MAC/sunrise t = 0.3–1.5). Measured outcome: magentasun ≈ floors-only (protected), wbclips
  ≈ full gate, sunrise 100% of the refinement span, MAC 74%, gradsky opens (statistically
  indistinguishable from sunrise in every ring/surround statistic measured — accepted trade,
  flagged in review). The vote lives entirely inside `floor_gate > 1e-6` blocks: the unit-WB
  bench stays bit-identical (spot-checked at run-to-run noise).

Diagnostics that settle this class of bug in one pass: dump the module's raw output (`ovoid`) and
bucket per-CFA-channel means by "own channel clipped" vs the per-channel `clips[]` — if the output
means sit at the clip levels, the floors are authoring the color, not the solver.

### Highlights (harmonic): a 1-clip pixel is only "measured" if the fit actually lifted it

Third failure family, on a blown sky meeting a dark ridge (DSC_1267.NEF): a magenta rim hugging the
mountain's edge. The cgrad stage's survivor-anchored reprojection deliberately skipped 1-clip pixels
as "measured-correct where the fit spoke" — but *the fit does not always speak*. Where the sky dims
toward an occluder it drags the guides, the colour-line prediction lands **below** the pixel's own
saturation level, and `fmaxf(pred, clip0)` pins the channel AT the floor — the minimum compatible
value, carrying the floor's chroma (i.e. the WB coefficients: magenta) instead of the material's.
Measured: **96.5%** of the 1-clip-G pixels 4–6 px from the rock, and 90.4% at 8–12 px, exit the whole
chain still at their floor (median lift 1.000), against 27.9% far from it.

Such a pixel holds exactly what a *partial multi-clip* pixel holds — two measured channels plus a
surround chromaticity — so it takes the same survivor-anchored reprojection. Ring profile (R+B)/2G by
distance from the ridge: 1.032 / **1.046** / 1.030 → 1.004 / **1.005** / 1.007.

**Two traps, both paid for in a full bench round each:**

*Never a hard threshold.* The first version admitted pixels via `est <= 1.03 * clip0`. Every metric
went flat and the render grew a NEW hard-edged pink region — the test printing its own contour along
the boundary between mostly-floored and mostly-lifted pixels. It is now an inverted smoothstep on the
lift (`CF_AUTHORED_RAMP`). The numbers alone would have shipped the bad version; only the picture
caught it.

*Anything that imports surround chroma MUST be gated on `_hl_floor_gate(clips)`.* Ungated, this
regressed the whole unit-WB bench — pk1synth +140% RMSE, balls +26%, occluded +18% — because at
unit WB every channel saturates at the same level, so a floored channel's chroma is NEUTRAL ≈ truth
and reprojecting it replaces a correct value with a guess. This is the SECOND time this exact
omission cost a bench round (the joint floor's first attempt, documented above, died the same way).
Gated, the six unit-WB cases return **bit-identical** to baseline (A = 1.145 vs a ramp starting at
1.25 → gate exactly 0) while real raws (A ≈ 2–2.6) keep the full fix. Note the same function already
consulted `floor_gate` to decide which pixels may *vote* — consulting it for the voters and not for
the pixels being *acted on* is the shape of the mistake.

### Highlights (harmonic): value-continuing estimators inherit the fence-band chromaticity — blotches

Second failure family, found on a blown sunrise laced with branches (Brandon Woods RW2): blotchy
pink/lavender patches and branch-shaped chroma ghosting inside the reconstruction. Root cause chain,
each link established by measurement (fit-plane dumps, per-clip-count chroma buckets against a
bright-sky-only reference — the all-valid mean is contaminated by dark foreground and is the wrong
reference):

- The pair-fit slope field itself carries the blotches (dumped `a`/`d`/`R²` planes show branch
  skeletons and low-slope blobs), and even the windowed **mean-ratio** is off: every value-continuing
  estimator (colour-line fits, harmonic ratio fills) anchors on the **last unclipped band before the
  clip contour**, which is unrepresentative — sensor-rolloff/flare-whitened, and threaded with
  occluder penumbras. Hardening the dim-anchor occlusion gate makes it *worse* (starves windows;
  dark anchors were warm, not the contaminant). Shrinking slopes toward the mean-ratio prior
  plateaus ~30% short: the prior inherits the same fence hue.
- Fix: `_chromaticity_gradient` (core.c) + `_chromaticity_gradient_stage_cl` / `hl_cgrad_*` kernels, the LAST
  region stage. Extend the **bright** fully-valid surround's chroma shares into the blown zone with
  the **biharmonic** (gradient-extending) dome — the same operator the luminance already uses — then
  reproject each multi-clip pixel's clipped subset onto the extended field and re-assert the joint floor.
  Details that took iterations to get right: (1) anchors = fully-valid AND ≥35% of the blown-zone
  plateau luminance AND clear of a **thin** guard ring around the clip contour (σ=4 blur of the
  any-clip mask, threshold 0.05 — a wide moat exiles anchors to unrepresentative far content and
  inverts the fix); (2) partial pixels use **survivor-anchored** scaling (valid channels set the
  brightness against the field, clipped channels take the field shares outright) — a
  magnitude-preserving reprojection cannot work there because the clipped subset's magnitude IS the
  under-prediction; all-clip pixels keep the dome-luminance magnitude and redistribute; (3) 1-clip
  pixels are left untouched (measured chromaticity-correct through the 2-guide fits); (4) bail when there is
  no usable bright surround (emitter in darkness) — the fence chromaticity is then all there is.
- STATUS after the article-bench review: the stage is gated and defensive. Reprojection applies to
  PARTIAL multi-clip pixels only (all-clip pixels keep the joint-core dome — redistributing a poor
  core total by the field's shares is unstable: measured negative pixels on the gradsky bench case),
  and the survivor-anchored scale is capped at 4× the pixel's current magnitude. Note
  `_region_blur1_cl` (device gaussian) operates on **images**, not buffers — allocate with
  `dt_opencl_alloc_device`, write via `write_imagef` (the -1 selftest sentinel with a silent
  full-host fallback is what a buffer/image mismatch looks like).
- **Content gate (1-clip-annulus validation)**: the continuation prior is a scene assumption — true
  for gradient skies, false for self-coloured emitters whose blown core carries its OWN chromaticity
  (a magenta sun, coloured lamps: the article-bench regressions). The stage self-arbitrates against
  the method's trusted zone: at 1-clip pixels (reconstructed from TWO measured guides, measured
  chromaticity-correct everywhere), compare the extended field's shares to the solver's; weight
  `w = exp(-(L1_err/0.10)^2)`, diffused inward by normalized convolution (`σ = radius/4`), blends the
  reprojection, shrunk toward the REGION-LEVEL ring vote as local evidence thins
  (`w = (blur_w + λ·vote)/(blur_m + λ)`, λ=0.05 — a hard no-evidence→0 fallback kills the stage in
  the deep interior of large blown zones, beyond blur reach of the thin ring). Mirrored in
  `hl_cgrad_gate` + gated `hl_cgrad_reproject` (CL) and `py_chromaticity_gradient` (prototype).
  Measured: the gate correctly identifies MAC's white lamps as self-coloured and disables the
  continuation there — the MAC magenta fix therefore CANNOT come from this stage; it belongs to the
  joint-floor family and its clip-asymmetry gate (now implemented, see the previous section).

### The article bench (guided-laplacian-highlights-research) — traps and extensions

- `ansel-cli` NEVER overwrites an existing export — it silently appends `_01`. Any script that
  re-exports over old files reads STALE results (one debugging session was burned on catastrophic
  bench numbers that were v1-scene exports scored against v2 ground truth). Always `rm -f` the
  target (and its `_*.tif` siblings) before exporting.
- The bench runs with `python3.12` (cv2/scipy live in ~/.local for 3.12; plain `python3` is 3.13
  and lacks them).
- The original six synthetic cases all write `AsShotNeutral=(1,1,1)` → the pipeline runs with UNIT
  white balance and EQUAL per-channel clips, a regime no real camera occupies; and none of them
  models a blown gradient sky with occluders. Two cases added 2026-08 (`make_new_cases.py`):
  `wbclips` (white emitter over textured amber behind mullion occluders, AsShotNeutral for
  wb=[2.2,1,1.5] — the MAC magenta mechanism needs unreliable fits + truth-white core + WB clips,
  a smooth well-anchored WB scene does NOT reproduce it) and `gradsky` (the sunrise geometry).
  Linear RMSE is magnitude-dominated and nearly blind to the hue failures users report — judge
  chroma with a chromaticity-share RMSE alongside it.

### A "CPU vs GPU parity bug" in a tiled module is usually a tile-grid dependence

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
`roi_in` on the backward pass: in practice `iop/lens.cc` (distortion, TCA, and the `scale`
slider). So "it only misbehaves when zoomed in" is the wrong mental model for this whole class of
bug; "it only misbehaves with lens correction enabled" is the right one.

### CFA phase (Bayer/X-Trans) is computed fresh per crop, not snapped — demosaic and highlights alike

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

## Colour management (`src/colorprofiles`)

The module owns its state: a single `dt_colorspaces_t` is file-static in
`colorprofiles/colorspaces.c`, built by `dt_colorprofiles_init()` and torn down by
`dt_colorprofiles_cleanup()`. Nothing outside `src/colorprofiles/` names the profile list, its
`xprofile_lock` or its cached LCMS transforms, and `darktable_t` does not know profiles exist.
`tools/check_module_boundaries.sh` holds that: external `dt_colorspaces_get_global()` and
external `xprofile_lock` acquisitions are both ratcheted at baseline **0**, and a count that
*falls* must lower the baseline in the same commit. `src/colorprofiles/README.md` is the full map.

**The standing regression check is `tools/check_export_pixels.sh <ref-a> <ref-b>`**, which decodes
both exports and compares the pixel arrays. Do NOT sha256 the exported PNG: it carries the build's
version string in its metadata, so two builds differ by a few bytes of compressed text with every
pixel identical. For colour management "it still runs" is a very low bar — the actual failure mode
is a one-LSB hue shift.

`colorprofiles/colorspaces.h` drags `<lcms2.h>` and `<pthread.h>` in behind it. A translation unit
that only needs the vocabulary — a profile type to store in its params, an intent to pass along —
includes `colorprofiles/profile_types.h` instead, which is the reason that header exists.

### The `role` argument is load-bearing, never a formality

`DT_COLORSPACE_SRGB` is registered **twice** — a v4 parametric-curve entry valid only as INPUT,
and a v2 point-TRC entry carrying output/monitor/working — and the two are distinguished by
nothing but which `*_pos` field is `-1`. A multi-bit role mask resolves to the **first** match in
registration order, which for sRGB is the v4 input entry. Resolving the *working* profile with
`DT_PROFILE_ROLE_ANY` therefore hands back the input-only variant; pass
`DT_PROFILE_ROLE_WORKING`. The index-valued calls (`dt_colorspaces_profile_index()`,
`dt_colorspaces_profile_at()`) require a single bit outright — an index means nothing outside the
enumeration that produced it, and an index taken from `INPUT|OUTPUT` equals neither `in_pos` nor
`out_pos`.

**They are roles, not directions**, and the enum was renamed to say so
(`dt_colorspaces_profile_role_t`). A profile is RGB→PCS or PCS→RGB and nothing else; what these
bits select is which *menu* an entry appears in. The menus genuinely differ:
`DT_PROFILE_ROLE_MONITOR` is the curated eligibility list for the monitor-profile menu and
diverges from `DT_PROFILE_ROLE_OUTPUT` on 5 of the 21 built-in registrations (`DISPLAY`,
`REC709`, `ITUR_BT1886`, `XYZ`, `LAB`), so substituting one for the other is a behaviour change,
not a rename. Two further bits, `CATEGORY` and `DISPLAY2`, were declared and never tested by any
lookup — the first had a `category_pos` no predicate consulted, the second had no backing field
at all — so `ANY` claimed six meanings and had four. They are gone; nothing changed at runtime.

**Three registered entries have `profile == NULL`.** `DT_COLORSPACE_WORK`, `_EXPORT` and
`_SOFTPROOF` name a user *setting* rather than a colour space and exist only to occupy a combo
row. Dozens of call sites write `dt_colorspaces_get_profile(...)->profile` with no NULL check;
what keeps them safe is precisely that the lookup never tests `category_pos`, so a category entry
can never be returned. Do not "fix" the predicate to consult it, and do not give categories a
role of their own, without auditing those sites first.

### Lifetime is answered by a lock, not by a copy

There is no `cmsDupProfile` in lcms2. The only true deep copy is serialise-and-reopen — ~0.005 ms
for a built-in but 1.02 ms for a real colord display profile — and copying a prepared
`cmsHTRANSFORM` means rebuilding it, 2.2–38 ms, with nothing to amortise it against. So the
module's four prepared display transforms never leave it (`dt_colorprofiles_xyz_to_display()`,
`dt_colorprofiles_rgba8_to_display_bgra8()`, `dt_colorprofiles_srgb_to_display_strided()` run the
pixel loop inside), and a caller deriving from a profile handle holds
`dt_colorspaces_lock_profiles()` / `dt_colorspaces_unlock_profiles()` across the derivation. An
LCMS transform does not retain the profiles it was built from, so the span ends at the
`cmsCreateTransform()` call, not at the transform's lifetime.

That lock is not ceremony. The `DT_COLORSPACE_DISPLAY` entry's `cmsHPROFILE` and the four
transforms derived from it are the only things in the list that mutate after init, and they are
replaced on every window move or resize that lands on a different monitor — i.e. on exactly the
events that repaint. **The display setters take `xprofile_lock` for WRITING**: never call
`dt_colorprofiles_set_display_profile_choice()` / `dt_colorprofiles_set_display_intent()` while
holding `dt_colorspaces_lock_profiles()`, which is a *read* lock — rebuilding the transforms
`cmsDeleteTransform()`s four handles a legitimate reader may still be using.

**Lock order where both are involved: `xprofile_lock` OUTER, the settings lock (private to
`colorspaces.c`) INNER.** Nothing takes them the other way round.

### Read the display / soft-proof settings as one snapshot, and render from the one you hashed

The seven fields (colour mode, display triple, soft-proof pair) cross the module boundary only
together, as `dt_colorprofiles_settings_t` via `dt_colorprofiles_get_settings()`, and are written
only through the setters. Reading them one at a time lets a reader pair a new profile type with
the previous filename, and a 512-byte filename read while `g_strlcpy()` is writing it is a **torn**
string, not merely a stale one.

A pipeline module that folds this state into its cache key must then **render from the same
snapshot**. Snapshotting it in `commit_params()` for the hash and re-reading the live state from
`process()`/`process_cl()` — once per tile — renders from state the cache key does not describe.
`dt_colorprofiles_settings_t.generation` advances on every accepted change and is the cheap thing
to hash: one number that cannot go stale field by field.

Every setter returns "did it actually change", and that return value is the point. A caller that
decides for itself, against a value it read separately, is how re-selecting the **already active**
display profile came to reset the user to the system profile — an inherited "profile not found"
fallback firing on the one case where nothing should happen.

### The derived-profile memo is module-owned; image-derived profiles are not in it

`dt_iop_order_iccprofile_info_t` (two 3×3 matrices plus six eagerly allocated 65536-float LUTs,
~1.5 MB) is a pure function of `(type, filename)`, so `dt_colorspaces_add_profile()` memoises it
process-wide under its own mutex — find-or-create as one critical section, because it is reached
per tile from `process()`/`process_cl()` (`iop/lut3d.c`, `iop/tonecurve.c`) and from the GUI
thread (`iop/colorin.c`). It returns a pointer the **module** owns, valid until
`dt_colorspaces_flush_profile_memo()` (or, for `DT_COLORSPACE_DISPLAY`,
`dt_colorspaces_invalidate_display_profile_memo()`).

It must stay sole-owned: `develop/blend.c` shallow-`memcpy`s the struct, aliasing all six LUT
pointers. Tear one down only through `dt_ioppr_cleanup_profile_info()`, which frees the LUTs as
well as the struct — freeing the struct alone leaks 1.5 MB per failure.

`intent` is **not** part of the memo key, so the first caller to ask for a given
`(type, filename)` fixes the intent every later caller gets.

Profiles derived from ONE IMAGE — `DT_COLORSPACE_EMBEDDED_ICC` through
`DT_COLORSPACE_ALTERNATE_MATRIX`, enum 9..14 — are **not** registered in the profile list and
cannot be resolved by identity: their matrices come from that image's own camera data via
`iop/colorin.c`. They must not be memoised either, since a `(type, "")` key would be shared by
every image of the same camera-matrix kind. The pipe that builds one owns it
(`dt_dev_pixelpipe_t.owned_input_profile_info`, freed with the pipe).

### `dev->roi.raw_width`/`raw_height` must be set for every `dev`, not just `gui_attached` ones — every drawn shape's absolute position depends on it

`dev->roi.raw_width`/`raw_height` (`develop.h`, doc-commented "Dimensions of the full-resolution
RAW image being worked on") are read by `dt_dev_coordinates_raw_norm_to_raw_abs()`
(`develop/develop.c`) to convert a shape's normalized (0..1) center/points into absolute pixel
coordinates — the first step of every drawn-mask shape's own area/mask function
(`masks/circle.c`, `ellipse.c`, `brush.c`, `gradient.c`, `polygon.c`: all read
`dev->roi.raw_width`/`raw_height` directly, several also route through the same coordinates
helper). `_dt_dev_mipmap_prefetch_full()` (`develop/develop.c`), called from every
`dt_dev_load_image()` through `dt_dev_ensure_image_storage()` → `_dt_dev_load_raw()`, is the only
place that sets them — but it used to do so **only `if(dev->gui_attached)`**, left over from a
commit that gated the surrounding GUI-viewport fields (`orig_width`, `preview_width`, ...) the
same way without noticing `raw_width`/`raw_height` aren't GUI state — they're an objective fact
about the loaded raw buffer, needed by any `dev`, headless or not.

For a `gui_attached` dev (the live darkroom) this was invisible: `raw_width`/`raw_height` get set,
shapes resolve correctly. For a throwaway, non-interactive `dev` — `imageio_core.c`'s export
`dev`, `dev_snapshot.c`'s `frozen`, thumbnail generation — `dev->roi.raw_width` stays `0` (its
calloc default), and `dt_dev_coordinates_raw_norm_to_raw_abs()` early-returns on `raw_width==0`
**without transforming the points at all**. A shape's normalized center (e.g. `(0.85, 0.35)`) then
masquerades as if it were already in absolute pixel coordinates, added to a radius term that
*is* correctly scaled to pixels (`radius * MIN(pipe->iwidth, pipe->iheight)`, passed as a
parameter, not read from `dev->roi`) — so the resulting bounding box lands within a few hundred
pixels of the image origin, its position dominated by the (correct, large) radius term and the
shape's own (tiny, ~0..1) normalized center contributing almost nothing. Two shapes at wildly
different real positions on the image collapse to nearly the **same** wrong bounding box, since
their normalized centers differ by less than 1.0 while the radius term is hundreds of pixels —
this is the tell that distinguishes this bug from an ordinary ROI/ROI-offset mismatch. Depending
on whether that degenerate bounding box happens to overlap the module's own `roi_in` for the
current render, a shape's effect either gets rejected outright (empty ROI intersection, e.g.
`iop/retouch.c`'s `rt_build_scaled_mask()`) or gets "applied" against mask values sampled from
the wrong, mostly out-of-bounds region of the source mask buffer (silently zero-filled by
`rt_build_scaled_mask()`'s own `dt_iop_image_fill(mask_tmp, 0.0f, ...)`), producing a real kernel
call that visibly changes nothing. Both outcomes were observed on the same image, same run: one
circle rejected, the other "applied" with zero measured pixel difference in the export.

Fixed by setting `raw_width`/`raw_height`/`raw_inited` unconditionally in
`_dt_dev_mipmap_prefetch_full()`. The two `dev->roi.raw_inited` checks that also gate on
`dev->roi.gui_inited` (`dev_pixelpipe.c`'s virtual-pipe resync, `develop.c`'s own zoom-scale
getters) stay correctly GUI-only through that second, genuinely-GUI flag — this fix doesn't
change their behavior. This was found chasing a report that `iop/retouch.c`'s clone/heal/blur/fill
had no visible effect at export and in the darkroom "before/after" snapshot compare: two
narrower, real fixes were needed first (`rt_process_forms()`/`_cl()` must resolve shapes through
`pipe->forms`, and `dev_snapshot.c`'s `history_override` path must resync `frozen->forms` — both
documented below) before this coordinate bug became the sole remaining, and actually dominant,
cause — restoring it alone (verified by CLI export pixel-diff against a debug build) turned two
previously invisible edits, including one healing over a blown bokeh highlight, fully visible.

### An embedded ICC profile belongs to its image, not to the application

`dt_colorspaces_t.profiles` (`colorprofiles/colorspaces.h`) is the application-wide profile list.
It is built once by `dt_colorprofiles_init()` and **never appended to at runtime** — registration
order is what enumeration reproduces and what every stored combo index in every preset and conf
key refers to, and the whole CRUDE (metadata) half of the API reads the list lock-free on that
basis. **It must stay that way.**

It did not used to be. `_build_embedded_profile()` (`imageio/imageio_profile.c`), reached from
`dt_colorspaces_get_output_profile()`, appended a container for an image's embedded ICC to that
list at runtime — from export jobs, which run in parallel. Three defects in one function:

- an unsynchronised `g_list_append` against a list every reader walks without a lock
  (`xprofile_lock` does not cover this; it guards the *display* profile, and the readers of
  `profiles` never take it);
- unbounded growth — one entry per exported image, held until shutdown;
- an outright leak whenever the profile was not newly created, because only the `new_profile`
  branch ever registered the container it had already allocated.

An embedded profile is a property of one image, so the image owns it:
`dt_image_t.embedded_profile`, written under the image cache entry's own lock, freed by
`dt_image_cache_deallocate()`, and reused on the next export of the same image rather than
rebuilt. Its container is built with `dt_colorspaces_new_image_profile()` and released with
`dt_colorspaces_free_image_profile()` — that pair exists so an image-owned container never touches
the list. The list is init-only — verified by checking that every `profiles = g_list_append` sits
inside `_colorspaces_build()`.

**Two traps, both paid for once already:**

*Do not "fix" such a race by locking the append alone.* With that many unlocked readers it
relocates the unsynchronised write rather than removing it. Either lock every reader, or — better,
and what was done here — stop mutating the shared structure at runtime and give the data to
whatever actually owns it.

*A `cmsHPROFILE` in a container is not necessarily the container's to close.* Several branches
of `dt_image_find_best_color_profile()` (`imageio/imageio_profile.c`) return a profile **borrowed**
from the application-wide list (`dt_colorspaces_get_profile(...)->profile`) and leave its
`new_profile` out-parameter FALSE; only the branches that build one set it TRUE. Giving the
container to the image and closing its profile on eviction therefore double-freed every borrowed
profile — the list closes it again at shutdown. `dt_colorspaces_color_profile_t.owns_profile`
records which case a container is in, and only owning containers close.

That second one aborted all eight CI runners with `corrupted size vs prev_size in fastbins`
after passing four build configurations and every static gate. Nothing static can see it:
`tools/check_it_runs.sh` runs the binary once, which does.

The same "profile-creating function used as a predicate" shape is what leaked three profiles per
resolve in `imageio/imageio_profile.c`: calling a function that *builds* a profile to ask whether
one exists, then calling it again for the value, discards the first. And a `goto finish` that
skips every write to an out-parameter — including the sRGB fallback — returns an **uninitialised**
`cmsHPROFILE` straight into `cmsCreateTransform()`. Both live in the profile-for-an-image cascade;
check the early-out paths there before adding another branch to it.

---

## Masks / forms history

### A brush point array records the centerline twice, and only the forward half is drawn

`gui_points->points` for a brush holds, in this order: three header points per node (`ctrl1`,
node, `ctrl2`), then the centerline sampled **forward** from the first node to the last, then the
**same** centerline sampled backward. The border wraps around the stroke, so the line under it is
walked there and back (`_brush_get_pts_border()`); the backward half is never drawn.

The forward half ends at `_brush_centerline_end()` — `node_count * 3 + (points_count - node_count
* 3) / 2`, i.e. half of the **samples**. Half of the whole array is a different number: the header
belongs to neither pass, so counting it in falls one and a half points per node short, and the
last node's own coordinate sits at the very end of the forward pass. Everything that walks the
drawn centerline (the outline stroking, the source shape, the clone link's midpoint) uses that
helper.

### A brush's outline encloses the whole stroke, so "inside the border" cannot mean "on the border"

`dt_masks_find_closest_handle_common()` (`masks_gui.c`) answers in one fixed order — source,
border, segment, shape — so whatever a shape's `get_distance()` reports as `inside_border`
preempts its segment. For a closed shape the two are disjoint regions: a polygon's `inside_border`
is the feather ring, true only *between* the outline and the form, so a cursor on the centerline
falls through to the segment test.

A brush has no such ring. Its `points` are the centerline walked there and back (zero area) and
its `border` is the outline wrapping the whole painted band, so a point-in-polygon test on that
border is true across the entire stroke. Reporting that as `inside_border` makes the brush's
segment — drag to move it, Ctrl+Click to insert a node — unreachable everywhere, while the shape
still looks perfectly hoverable. For a brush, `inside` is "enclosed by the outline" and
`inside_border` is "within cursor reach of the outline itself, and no centerline segment is
closer": the outline carries no drag action of its own (border width is edited through the
per-node handle and the wheel), so on a thin stroke, where outline and centerline are both within
reach at once, the segment wins.

`_brush_get_distance()`'s source pass walks a different outline, so its distances need their own
accumulator — sharing one running minimum lets a clone source near the form veto every segment hit
on the form itself.

### A drawing pass must not leave a path in the cairo context

Cairo keeps the current path across calls, so a leftover is painted by the next `cairo_stroke()`
**anywhere**, in that stroke's own style. `dt_masks_draw_path_seg_by_seg()` strokes one segment per
node and stops on a node boundary, so it ends with `cairo_new_path()`; the creation session's
per-shape loop (`masks_gui.c`) does the same between shapes.

The symptom shape is a piece of one shape adopting another shape's style, or appearing only when
something else happens to be drawn: a leftover tail comes out dashed when the shape's own dashed
border is stroked next, comes out highlighted when another shape is hovered, and is invisible when
nothing is drawn after it. That is a path leak, not a style or selection bug.

Which segments exist at all is decided by the caller's shape: an open path (a brush — the only
caller asking for round ends, since only an open path has two true ends) has `node_count - 1`
segments, a closed one has one more, and a shape still being created has no closing segment yet.
The walk stops once it has stroked them all, so it must be handed a point count that lets it
**reach** the last node.

### Brush masks rasterize as radial spokes — wedge holes across the stroke (OPEN)

Reported 2026-08-08 on `_DSC9410.NEF` (sidecar alongside it): a 57-node brush leaves four
wedge-shaped holes in its mask, the largest 1336x966 px. **Not caused by the gtk.h/widgets
refactor** — exports from that branch and from master are bit-identical, image and mask
channel alike (0 differing pixels of 24,160,256).

Two things make the diagnosis quick, and both were got wrong on the first pass:

- **They are not the interiors of self-crossing loops.** A brush paints a stroke of finite
  width, so a loop's interior legitimately stays unpainted, and the largest hole looks exactly
  like that at a glance. Look at the small ones at full resolution instead: each has a
  *perfectly straight* edge cutting across the stroke, which no smooth outline produces.
- **They are not the sparse-sampling path.** `use_sparse` in `_brush_get_mask_roi()`
  (`develop/masks/brush.c`) is gated on `dt_dev_pixelpipe_has_preview_output() || pipe->type ==
  DT_DEV_PIXELPIPE_THUMBNAIL`, and that predicate returns FALSE immediately on
  `!dev->gui_attached`. An `ansel-cli` export therefore runs the full-sampling branch
  (`sparse_step = 1`), and the holes are present there.

The mechanism the shape points at: the mask is not a filled polygon but the **union of radial
spokes** — for each border sample `i`, `_brush_falloff_roi()` stamps a segment from centreline
point `points[i]` out to `border[i]`. Where consecutive spokes fan out faster than the border
sampling density, the wedge between them is never stamped, and both its straight edges are
spokes. That is precisely "holes orthogonal to the path".

Where to look: `_brush_get_pts_border()`, and the two arc-filling helpers
`_brush_points_recurs_border_gaps()` / `_brush_points_recurs_border_small_gaps()`. Both bail
out with `if(l < 2) return;` where `l = |delta_angle| * max(r1, r2)` — a pixel-count test on
arc length, which is the shape of a threshold that is right at the resolution it was tuned for
and too coarse elsewhere. **Measure before theorising**: dump `points`/`border` for this brush
and check actual spoke spacing at the four hole coordinates. This file's history (see the
highlights sections above) is full of plausible-but-wrong theories that survived source
reading and died on the first measurement.

Reproduce: export with `--export_masks 1`; page 1 of the TIFF is the mask. Flood-fill from the
border and anything left unset is a hole.

### The mouse wheel edits the property the user mapped it to; shapes never read modifiers

Which property the wheel edits is resolved **once**, by `dt_masks_events_mouse_scrolled()`
(`masks_gui.c`), and handed to the shape through the `interaction` parameter
`dt_masks_functions_t.mouse_scrolled` already carried. Each shape's handler is a `switch` on
`dt_masks_interaction_t`: it acts on the property it is given, ignores one it does not own (a
circle has no rotation), and `DT_MASKS_INTERACTION_UNDEF` means "this combination is unmapped,
do nothing". **No shape may go back to reading `state`** — the key state stays in the signature
only because the callback is shared.

The mapping is one conf key per wheel/modifier combination
(`plugins/darkroom/masks/scroll/{plain,shift,primary,primary_shift}`, enum values `none` |
`size` | `fading` | `opacity` | `rotation`, declared in `data/anselconfig.xml.in`) behind
`dt_masks_scroll_mapping_get/set()` and `dt_masks_scroll_get_interaction()` (`masks_gui.h`).
Those enum values are a storage format: never translate them, never reorder them against
`dt_masks_interaction_t`. The mapping is **application-wide** — which property the wheel edits
is a user habit, not a property of a shape or of the module owning the mask — and its defaults
reproduce the historical modifier behaviour, so a user who never opens the panel sees no change.

A gradient spells one of the shared properties its own way: `FADING` is the curvature. `SIZE`
is the fade extent but keeps the shared name, since it is the shape's size in the only sense a
gradient has one. That is also how the context-menu sliders name them, and why
`dt_masks_interaction_alias_name()` exists — it answers `NULL` for every property a gradient
names like everyone else.

Scope is the selection's business, not the wheel's: `dt_masks_gui_change_affects_selected_node_or_all()`
already restricts a size/fading change to the selected node. A shape must not additionally
*substitute* a property based on selection state — the polygon used to force fading on a plain
wheel whenever a node was selected, which made the mapping unreachable for that shape.

The GUI is the "Mouse wheel" collapsible section of the Drawn tab (`blend_gui.c`), one radio
group per row. It holds no state of its own and re-reads conf on `map`: every module's blending
panel shows the same application-wide mapping, so a panel that becomes visible must display what
is stored, not what it was built with. A display refresh must never write conf back, or a stale
panel becomes the authority.

Consequence for the on-canvas hints (`set_hint_message`, per shape): they document **gestures
only** — drag, ctrl+click, right-click, Del, Enter, Esc — never the wheel, which the panel now
shows. Their branches follow the hit-test order of `dt_masks_find_closest_handle_common()`
(border handle, curve handle, node, segment, then the shape), because the innermost target under
the cursor is what the next click acts on. Two traps when editing them: a node's Del and
ctrl+click only work once that node is *selected* (a mere hover gets the shorter message), and
`dt_hinter_set_message()` joins `\n` into `, `, so each line must read as a clause of one
sentence.

### A shape toolbar's pressed button is a view on the creation state, not a state of its own

Which shape button looks armed is derived, never remembered. `dt_masks_creation_mode_enter()`
(`masks/masks_gui.c`) ends by raising `DT_SIGNAL_MASK_SHAPE_BUTTONS_SYNC`
(`dt_masks_shape_buttons_sync_all()`), and every toolbar built by `dt_masks_shape_buttons_create()`
answers by recomputing each of its buttons from `_masks_shape_button_is_current_creation()` against
`dev->form_gui`. `dt_masks_form_exit_creation()` is the symmetric half and raises
`..._DEACTIVATE`. That is what lets creation be armed from places that own no button at all — the
shape manager's "Add new shape ..." context menu (`libs/masks.c`), the keyboard shortcuts,
`iop/spots.c` — without each of them having to find and press a widget.

So a new way to arm a shape needs no toolbar code, and a toolbar must not track what was clicked.
The buttons act on `button-press-event`, not `toggled`, which is why the sync may set them freely
without re-entering the press handler; and the sync is raised *after* the whole creation state is
written, so a handler that asks is told the truth.

**A toolbar with a NULL `creation_module` is the shape manager's and belongs to no module**, so it
reports any creation whose form is not a retouch/spot one (`DT_MASKS_IS_RETOUCHE`) — those never
appear in its tree. Every other toolbar carries its owner in `creation_module` and lights up only
for that module. The distinction matters because the manager's context menu arms creation with the
*selected group's* module, not with the manager's own NULL: a strict identity test would leave the
button that menu just chose unpressed.

`_masks_shape_button_defs` is the one table naming a shape's icon and its two button tooltips
(`label`, `ctrl_label`, phrased as actions) plus its bare `name`. Menu entries offering a shape are
built from it through `dt_masks_shape_menu_item_new()`, so a menu and the button offering the same
shape cannot drift apart — they did, as "add path" against "add polygon".

### Forms are refcounted, not deep-copied

`dev->forms` (`dt_develop_t`) is the live, mutable `GList` of every mask shape and group
(`dt_masks_form_t*`) in the current image. Groups don't nest forms directly — a group's `points`
is a list of `dt_masks_form_group_t` entries (`{formid, parentid, state, opacity}`) referencing
sibling forms in the same flat `dev->forms` list by ID.

Every history commit that touches masks used to deep-copy the *entire* `dev->forms` list into
`hist->forms` (`dt_dev_history_item_t`), even when only one shape on one module changed. Forms
are now refcounted (`dt_masks_form_t.refcount`, `src/develop/masks/masks_history.{h,c}`) instead:

- `dt_masks_snapshot_current_forms()` takes a reference on each current `dev->forms` element
  instead of copying it. Multiple `hist->forms` snapshots (and `dev->forms` itself) can share the
  exact same `dt_masks_form_t*`.
- `dt_masks_cow_touch(dev, form)` is the copy-on-write gate: before *mutating* a form (move,
  resize, remove a group member...), check its refcount. If it's 1 (only `dev->forms` holds it),
  mutate in place. If it's shared (an undo/redo or history snapshot also references it), clone it
  first, splice the clone into `dev->forms` in place of the original, and mutate the clone —
  never mutate a form that might be observed by a frozen snapshot. Every mutation call site
  (mouse/keyboard event dispatchers in `masks.c`, `dt_masks_form_delete`, group add/move/ungroup,
  `blend_gui.c` group operations, the shape-manager panel in `libs/masks.c`) must route through
  this before touching `form->points` or any other field. `dt_masks_cow_touch` also re-points
  `dev->form_gui->form_visible` if it was the form that got cloned — that's the only other raw
  `dt_masks_form_t*` cached outside `dev->forms`.
- `dt_masks_replace_current_forms()` swaps `dev->forms` wholesale (used when history navigation
  rebuilds it) by releasing the old references and taking new ones — never a raw deep copy.
- `pipe->forms` (the pixel-pipeline's own snapshot, taken once per `dt_dev_pixelpipe_process()`
  call, `pixelpipe_hb.c`) is shared by reference the same way. It has exactly one real consumer
  (`iop/retouch.c`, read-only), so no COW gate is needed on that side — `dt_masks_cow_touch`
  already guarantees a GUI-side edit clones instead of mutating a form an in-flight pipeline run
  is holding.

### The unused-shape sweep's used-set is not a subset of the snapshot it sweeps

"Delete unused shapes" in the shape manager (`libs/masks.c`, `dt_masks_cleanup_unused()`) keeps a
form when some history entry's `blend_params->mask_id` names it, or names a group that
transitively contains it. Those ids are collected by walking history from the bottom up, and they
are **not** a subset of the `hist->forms` snapshot being swept: a module whose drawn mask was
since dropped keeps its old `mask_id` in every history entry it ever wrote, and no form in a
later snapshot answers to it. The set is therefore unbounded with respect to the snapshot, and
must live in a hash set — `_cleanup_unused_recurs()` sizing a table on `g_list_length(forms)`
filled it with ids that match nothing and ran out of slots before the one live group's membership
was walked, deleting shapes from the tail of that group inward while the module was still using
them. Measured on a 20-step history: four departed groups (`colorbalancergb`, two `toneequal`, an
older `exposure`) took four of the eight slots an 8-form snapshot allowed, the live group and its
first three members took the rest, and the group's last two shapes were swept.

Marking an id and recursing into it are now the same step, which also bounds a group that
contains itself through a chain of member groups — the old code broke out of its scan on an
already-seen id but recursed regardless, so such a cycle did not terminate.

A swept form's snapshot reference is **handed to `dev->allforms`**, not released: a form read back
from `masks_history` is built by `dt_masks_create()` and its snapshot membership is its only
claim, so unref-ing at that point would free an object `dev->forms` may still hold by address. One
`allforms` entry per transferred claim is what keeps teardown balanced; do not "fix" the missing
unref.

The sweep is history-wide by necessity: `main.masks_history` stores one row per (history step,
form), so a shape only really leaves the database once **every** snapshot has stopped naming it —
hence the rewrite of every `hist->forms` in place, after which `dev->forms` is re-pointed at the
topmost surviving snapshot.

That is still undoable, and the menu handler opens the undo record itself, **before** the sweep.
`dt_dev_add_history_item()` opens one of its own, but by then every snapshot has been rewritten
and the "before" state it would capture is the swept one; `dt_dev_history_undo_start_record()`'s
depth counter makes the inner pair a no-op, so the recorded pair spans the whole operation. What
makes the restore work is that `dt_history_duplicate()` copies each item's forms **list**
(`g_list_copy` plus one reference per form) instead of aliasing it: the record owns its own
cells, the sweep's `g_list_remove()` on the live items cannot reach them, and every swept shape
stays alive as long as the record holds it. `_pop_undo()` rewrites the database from the restored
history, so the `masks_history` rows come back too. Measured round trip on one image:
65 rows / 23 forms → 70 / 22 after the sweep → 65 / 23 after undo, the swept shape restored and
nothing else moved.

That rewrite is in-memory only. `main.history` and `main.masks_history` are deleted and
re-inserted wholesale from `dev->history` by `_write_history_from_state()`, and nothing on this
path triggers it — so the menu handler commits a mask-manager history entry
(`dt_dev_add_history_item(dev, NULL, FALSE, TRUE)`) after the sweep, the way every other forms
mutation in `libs/masks.c` must. Without it the swept shapes stay in the database and come back
on the next read, and the pipeline never resyncs. Measured on a 20-step history: 60 rows / 24
forms before, 58 / 22 after, with exactly the two orphans gone and one extra history step.

The sweep also takes `dev->history_mutex` as **writer** for its whole span. The async DB write
job walks `hist->forms` with the lock released; its snapshot references keep each history *item*
alive but say nothing about the list cells `g_list_remove()` frees under it. Order is
`history_mutex` outer, `masks_mutex` (taken by `dt_masks_replace_current_forms()`) inner — the
same way a history commit takes them.

### A form mutation that never reaches a history commit is invisible to undo/redo

`dt_dev_add_history_item_ext()` (`dev_history.c`) is the only place that turns the current
`dev->forms` state into a `hist->forms` snapshot, and only when
`dt_iop_module_needs_mask_history(module)` is true for the committing module. **Any code path
that mutates `dev->forms` (directly or via `dt_masks_form_delete`/group helpers) must be followed
by a `dt_dev_add_history_item()` call**, or the mutation only ever exists in live memory.

Undo/redo (`_pop_undo`, `dev_history.c`) replaces `dev->history` with a duplicate of the
recorded `before_snapshot`/`after_snapshot` (`dt_history_duplicate`, itself ref-sharing) and calls
`dt_dev_pop_history_items_ext()`, which rebuilds `dev->forms` from the `hist->forms` of the
**last history item that actually has one** — walking backwards over items with
`hist->forms == NULL`. If a mutation was never committed, every subsequent history navigation
silently falls back to whatever was last actually recorded and the live edit is lost. Confirmed
bug instances, found by auditing every handler in `libs/masks.c` for a trailing
`dt_dev_add_history_item()`/`_add_masks_history_item()` call: `_tree_delete_shape` (delete),
`_tree_moveup`/`_tree_movedown` (reorder inside a group — silently lost on next undo/redo), and
`_tree_duplicate_shape` (the duplicate was also never attached to the source shape's parent group
via `dt_masks_group_add_form`, so it was an orphan on top of being uncommitted). All four are
fixed; audit any *new* handler in `libs/masks.c` / `blend_gui.c` that mutates forms without a
trailing commit before trusting its undo/redo behavior.

### Same-thread rwlock reentrancy

Committing masks more often surfaces a pre-existing, unrelated hazard: `dt_dev_pixelpipe_change()`
can be re-entered by the same thread while it already holds `history_mutex` as writer (a
history-commit path resyncing the virtual pipe mid-commit). glibc's default
`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` policy self-deadlocks such a thread as soon as a
second thread is queued for the write lock. Fixed by porting the same-thread recursive-writer
tracking that already existed in the `_DEBUG` build of `dt_pthread_rwlock_t`
(`common/dtpthread.h`: `writer` + `writer_depth` fields) into the release path too — a thread
that already holds the write lock cannot race itself, so letting it re-enter (as reader or
writer) is safe. `try*` locks keep their "is it locked by anyone?" probe contract and still
report busy on same-thread reentry, so callers relying on that semantic are unaffected.

### DB/XMP persistence still duplicates content per history step

`masks_history` (SQL table) and `Xmp.darktable.masks_history[N]` (XMP array) store one
row/entry per (history step, formid), with no dedup — the in-memory refcounting above stops at
the persistence boundary, so a form shared unchanged across 100 history steps still gets its
points BLOB serialized 100 times on every commit (`dt_dev_write_history_ext` rewrites the whole
image's history + masks_history every time). Known, not yet fixed — see
`doc/masks_history_dedup.md` for the full design (developed on a dedicated branch, merged only
when Ansel 1.0 is prepared, per explicit instruction not to migrate any user's live DB
prematurely).

---

### The masks module is being enclosed, and the ratchet counts the way out

`src/develop/masks` is not a closed module yet: five files outside it (`develop/blend_gui.c`,
`libs/masks.c`, `iop/retouch.c`, `iop/spots.c`, `develop/supervisor.c`) reach directly into
`dt_masks_form_t` and friends, four places `malloc` a masks type by hand, and `->forms` is walked
as a plain `GList` all over `develop/`. The audit behind that is issue #1299; the plan is to drain
it phase by phase rather than in one break.

**`tools/check_module_boundaries.sh` section 9 counts the remaining leaks, and the counts may only
FALL.** Adding a new external struct access, a new includer of `masks.h`, a new hand-rolled
allocation or a new raw `->forms` walk fails CI. So does *removing* one without lowering the
baseline in the same commit — that is deliberate: it is what stops a phase from half-landing and
the ground being quietly given back later.

Two things about those counters a future editor should not "improve":

- **The member list is curated to names no other struct in the tree uses** (`formid`,
  `form_dragging`, `creation_formids`, …) and deliberately omits the ambiguous ones a masks form
  shares with everything else (`points`, `type`, `name`, `state`, `opacity`). It therefore
  undercounts — the full census found ~385 accesses by reading declarations, the gate reports 102
  — and that is the right error for a gate. Widening it buys a bigger number and loses the
  property that every match is real.
- **One write match is a known false positive and stays counted**: `supervisor.c`'s own event
  struct also has a `formid`, so `e->formid = form->formid` matches on the left while genuinely
  reading a masks form on the right. It is stable, so it costs nothing; chasing it would mean
  excluding the file, which would hide real writes appearing there later.

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

### retouch: the pixel-processing callback must resolve shapes through `pipe->forms`, not `self->dev->forms`

`rt_process_forms()`/`rt_process_forms_cl()` (`iop/retouch.c`) are the `dwt_decompose()`/
`dwt_decompose_cl()` callbacks that actually apply each shape's clone/heal/blur/fill at every
wavelet scale — they run on the pipeline/worker/CL thread, not the GUI thread. They resolve the
module's mask group and each shape by id through `dt_masks_get_from_id_ext(pipe->forms, id)` —
the refcounted, frozen snapshot `dt_dev_pixelpipe_process()` takes once per run (see "Forms are
refcounted, not deep-copied" above) — never through `dt_masks_get_from_id(self->dev, id)`. The
latter reads the live, GUI-owned `dev->forms` with no lock and no reference held, which is safe
enough while `self->dev` is the long-lived darkroom `dev` continuously driven by the same GUI
thread, but not for a `dev` that is created, populated, and torn down around one pipeline run —
`imageio_core.c`'s export `dev` and `dev_snapshot.c`'s `frozen` both fit that shape. `commit_params()`
and `rt_resynch_params()` already followed the `pipe->forms`-first pattern (falling back to a
lock-guarded `self->dev->forms` only when `pipe->forms` is not yet populated); the two processing
callbacks are the only per-pixel consumers and must use the same source. `rt_masks_form_is_in_roi()`,
`rt_masks_get_delta_to_destination()`, `dt_masks_get_area()` and `dt_masks_get_mask()` all take an
already-resolved `dt_masks_form_t*` and don't re-lookup by id, so they need no equivalent change.

### dev_snapshot.c: the `history_override` path must resync `frozen->forms` too, not just `frozen->history`

The darkroom "Snapshot" feature (`libs/snapshots.c`'s `_lib_snapshot_capture_state()`) captures the
**live, possibly-uncommitted** `dev->history` — a duplicate taken under `history_mutex` — and hands
it to `dt_dev_snapshot_capture()` as `history_override`, precisely so a shape drawn a second ago,
before any history commit, still shows up in the frozen comparison. `dt_dev_snapshot_capture()`
splices that duplicate straight into a fresh `frozen` dev's `frozen->history`/`iop_order_list`, and
resolves each `hist->module` — but never touches `frozen->forms`. It stays at whatever
`dt_dev_load_image(frozen, imgid)` read a few lines earlier from the image's *saved*
`main.masks_history` — the on-disk state as of the last commit, not the override's live one.

Module params/blend_params don't have this problem: `dt_dev_pixelpipe_synch_all()` →
`_sync_pipe_nodes_from_history()` (`dev_pixelpipe.c`) walks `pipe->dev->history` itself and commits
`hist->params`/`hist->blend_params` per node independently of `dt_dev_load_image()`'s earlier read,
so a module's own param blob — retouch's `rt_forms[]` array included, with the freshly-drawn shape's
`formid`/`scale`/`algorithm` — is correctly the override's. But that blob only names the shape by
id; the geometry lives in `dev->forms`/`pipe->forms` (see the entry above), and a module needing mask
history resolves `blend_params->mask_id` against a group that `pipe->forms` (snapshotted from
`frozen->forms` at `dt_dev_pixelpipe_process()` start) doesn't contain. The shape's params exist,
its parent group doesn't — `dt_masks_get_from_id_ext(pipe->forms, mask_id)` returns `NULL`, and
`rt_process_forms()`/`_cl()` return early with no shapes applied, no error printed either (a
`grp == NULL` group lookup is a silent no-op by design, not a logged failure).

Fixed by re-deriving `frozen->forms` inside the `history_override` block with the same accumulation
rule `dt_dev_pop_history_items_ext()` uses elsewhere: walk the (just-spliced) `frozen->history` up to
`history_end_override`, keep the last non-`NULL` `hist->forms`, and call
`dt_masks_replace_current_forms(frozen, forms)` before `dt_dev_set_history_end_ext()`. `hist->forms`
is already a per-commit snapshot (refcounted, shared by reference — see "Forms are refcounted, not
deep-copied"), so this is a cheap re-point, not a copy. `duplicate.c`'s call site
(`dt_dev_snapshot_capture(&d->preview, dev, imgid, NULL, NULL, -1)`) passes no override and never
enters this block — it already gets correct forms from `dt_dev_load_image()`'s normal DB read, since
it is snapshotting an already-saved image, not a live in-progress edit.

### retouch: combining the mask/wavelet-scale/suppress preview toggles

`bt_showmask` (`g->mask_display`), `bt_display_wavelet_scale` (`g->display_wavelet_scale`), and
`bt_suppress` (`g->suppress_mask`, "temporarily switch off shapes") are three independent preview
toggles. Getting any *pair* of them to combine correctly required three separate fixes, found only
by adding `dt_print(DT_DEBUG_ALWAYS, ...)` traces (never raw `fprintf(stderr, ...)` — it isn't
flushed and is easily lost if the process doesn't exit cleanly) at each stage and, for the final
one, an actual GPU buffer readback (`dt_opencl_read_host_from_device_raw`) — reasoning about the
hash/cache chain from source alone kept landing on plausible-but-wrong theories.

**1. `bypass_cache_variant` must be gated to the FULL pipe, like `request_mask_display` already is.**
`dt_iop_module_t.bypass_cache` is a single shared boolean: switching between combinations of the
three toggles that all keep it `TRUE` (e.g. suppress toggled on top of an already-active
wavelet-scale preview) doesn't change it, so the pipeline hash doesn't change either, and the
stale pre-toggle frame keeps being served. Fixed by adding `dt_iop_module_t.bypass_cache_variant`
(an opaque per-module int any module can set to disambiguate *which* combination is active,
alongside `dt_iop_set_cache_bypass()`) and folding it into `dt_pixelpipe_get_global_hash()`. That
alone still wasn't enough: retouch's actual preview effect only ever applies to `pipe ==
self->dev->pipe` (the darkroom FULL pipe) — `preview`/`virtual-preview` always render as if none
of the toggles were active — but `bypass_cache`/`bypass_cache_variant` live on the shared
`dt_iop_module_t` and so read the same non-zero value for every pipe type. Left ungated, a
preview-pipe run with the same ROI (e.g. at zoom == fit) computes the identical hash chain despite
publishing different pixels, and the pixel cache's cross-pipe "another pipe already owns this
exact hash" reuse path (`DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT` in
`dt_dev_pixelpipe_cache_get_writable`) lets either pipe silently serve the other's stale content.
`bypass_cache_variant`'s hash contribution must be zeroed for non-FULL pipes exactly like
`request_mask_display` already is, in the same `if(pipe->type == DT_DEV_PIXELPIPE_FULL)` block in
`dt_pixelpipe_get_global_hash()`.

**2. `process_cl()`'s "expose mask" condition must match `process_internal()`'s exactly.** The CPU
path gates on `g->mask_display || display_wavelet_scale`; the OpenCL path had drifted to
`g->mask_display` alone. A wavelet-only OpenCL preview therefore never cleared alpha, never set
`pipe->mask_display`, and so never made the downstream color-pipeline modules take the
mask-display passthrough shortcut in `pixelpipe_hb.c` (~line 950) — they ran their normal
processing (color management etc.) on the wavelet-domain buffer instead of being skipped. Same
class of bug as the CFA-phase and highlights-reconstruction CPU/OpenCL divergences documented
above: any GUI-only branch condition duplicated between a module's `process()` and `process_cl()`
is a standing invitation for exactly this drift, since nothing forces the two to be reviewed
together.

**3. `rt_adjust_levels()` clobbers the alpha channel — the actual root cause of "mask + wavelet
scale together shows nothing but checkerboard."** This function (shared verbatim by both the CPU
path and `rt_adjust_levels_cl`, which round-trips through it on a host-side copy of the GPU
buffer) is called whenever *any* single wavelet scale is being previewed
(`dwt_p->return_layer > 0`), to contrast-stretch the near-zero detail coefficients into a viewable
image. It round-trips each pixel through `dt_linearRGB_to_XYZ`/`dt_XYZ_to_Lab` (or the
`work_profile` matrix equivalents) and back. Those conversions — like most of the
`dt_aligned_pixel_t`-based color primitives in `colorspaces_inline_conversions.h` — store their
result via 4-wide SIMD (`dt_apply_transposed_color_matrix`'s `dt_store_simd_aligned`), which writes
*all four* lanes even though the color math is only 3-channel; the 4th lane ends up holding
leftover matrix-multiply output, not the caller's original value. For most pipeline buffers that
4th channel is meaningless padding and nobody notices. Here it is retouch's own mask-display
alpha, painted a few lines up the call chain via `rt_copy_mask_to_alpha`/`_cl` — so every pixel's
alpha got silently reset by the *next* operation in the same `process()` call, regardless of
scale-matching or hash correctness upstream. This is why fixes #1 and #2 above were both real bugs
worth fixing but neither actually resolved the reported symptom: content was being computed
correctly and served fresh, then destroyed by `rt_adjust_levels()` before publish. Only triggers
when previewing a wavelet scale (`return_layer > 0`) *and* something reads alpha for display
(`show mask`, or — before fix #2 — a would-be-`PASSTHRU` OpenCL frame that never got the memo).
Fixed by saving `img_src[i+3]` before the round trip and restoring it after. Any other per-pixel
loop in this codebase that round-trips through these color conversion primitives on a buffer whose
4th channel is meaningful (alpha, a mask, anything other than padding) has the same exposure.

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

### After an import: which image opens, and which folder the library shows

`dt_collection_load_filmroll()` (`common/collection.c`) is what both import paths
(`control/jobs/import_jobs.c`, `control/jobs/film_jobs.c`) call to make a freshly imported image
visible. It runs on the **import job's thread**.

**Whether the user is moved at all** is the caller's decision, expressed as a
`dt_collection_import_view_t` policy (`common/collection.h`): `KEEP` (never move), `GRID`
(lighttable), `IMAGE` (open that one image in the darkroom). Every **automatic** import passes
`KEEP` — Studio Capture's folder survey (`data->folder_survey`, `common/folder_survey.c`) imports
on its own schedule, in whatever view the user happens to be, and displays the capture itself
from `DT_SIGNAL_IMAGE_IMPORT` without leaving its atelier. The policy has to come from what the
import *is*, not from what is on screen when the job ends: the survey keeps running after the
user leaves the Studio Capture atelier, so a capture landing mid-edit would otherwise throw them
out of the darkroom (or into it).

**Following the imported image's folder** asks two questions. Did the user ask for this import?
An automatic one follows the folder in Studio Capture's own atelier, whose filmstrip tracks the
shooting session, and nowhere else — the same reason `KEEP` does not switch views, applied to the
collection. Then, may rule 0 be overwritten with a folder? That is the Collect module's persisted
tab: legitimate on "Folders", destructive on "Collections" and "Queries" where the rules are the
user's. That second question is deliberately NOT about the current atelier — the collection is
global, so a manual import started from the darkroom must re-point it too, or the library still
shows the previously browsed folder when the user goes back to the grid.
`_collection_folder_ui_inactive()` is a different predicate for a different question (which
folder the import dialog considers "currently browsed") and does gate on the atelier; do not
merge the two.

**The hovered image and the selection** follow the same rule: `dt_collection_load_filmroll()`
points them at the imported image only for a user-requested import. Under `KEEP` it leaves both
alone — Studio Capture sets them itself for the capture it displays (`_studio_set_image()`), and
anywhere else adding an unrequested image to the selection also hands it to the next darkroom
entry, whose `try_enter()` reads the mouse-over id first.

**Opening a single imported image in the darkroom** goes through
`dt_ctl_open_image_in_darkroom(imgid)` (`control/control.c`), never through a view switch alone.
The darkroom's `try_enter()` picks its target from `dt_control_get_mouse_over_id()`, falling back
on the selection, and both are volatile across the lighttable round-trip the switch performs: any
pointer motion over the grid rewrites the mouse-over id, and the darkroom's own `leave()` calls
`dt_selection_select_single(dt_view_active_images_get_first())`, i.e. restores the selection to
the image it was editing. Publishing the target from the job thread and requesting the switch
separately therefore re-opens the previous image about as often as the intended one, depending on
where the pointer happens to sit. `dt_ctl_open_image_in_darkroom()` marshals the whole sequence
into one GUI-thread callback — leave the darkroom via the lighttable, publish mouse-over id and
selection, then enter the darkroom — so nothing can run in between. Any other worker-thread code
that needs a specific image opened must use it rather than setting those globals itself.

The import job only asks for `IMAGE` when it imported exactly one image *and* at most one XMP
(`index == 1 && xmps <= 1`): two or more sidecars mean the file produced several DB images
(duplicates) and none of them is the obvious one to open. Zero is the ordinary no-sidecar case
and still opens.

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

### A rotated GtkLabel sizes the column it sits in

A `GtkLabel` with `gtk_label_set_angle()` requests the width of its *slanted* bounding box, so a
diagonal column title makes its whole column that wide — measured on the masks wheel-mapping
grid: 102 px for "Fading/Curvature" at 45° against 24 px for the radio button underneath it.
Zeroing `column-spacing` does not help, because the spacing was never what separated the cells.

Two things fix it, and both are needed. Give the grid `GTK_ALIGN_START`: handed more width than
it needs (`gtk_box_pack_start(..., TRUE, TRUE, 0)` does exactly that), `GtkGrid` spreads the
surplus over its columns and re-centres every title in a cell wider than itself. Then attach each
title **spanning the columns to its right** — the direction it leans into, whose header space is
free — so its width constrains that sum rather than one column, with one extra `hexpand` column
at the end to absorb the last title's overhang. Measured: 24 px columns at any spacing, so the
panel's usual gutter can stay.

Verify this class of layout by measuring, not by looking: build the widget in a
`gtk_offscreen_window_new()`, pump `gtk_events_pending()`/`gtk_main_iteration()`, and print
`gtk_widget_get_allocation()` for the cells. It answers in seconds what several rebuild-and-look
round trips do not.

### A height/width request must cover the CSS border, not just the padding

A widget's size request is its whole CSS box: padding *and* border come out of the allocation
before the content sees any of it. Code that sizes an area to fit its content therefore has to
add both back, and `gtk_style_context_get_padding()` without the matching
`gtk_style_context_get_border()` leaves the content short by exactly one border.

Two pixels is enough to be visible, because the widgets that care answer a shortfall with a
whole scrollbar rather than a clipped pixel. `dt_ui_scroll_wrap()` (`widgets/scroll_wrap.c`)
sizes every list and textview in the application to `clamp(min(content, cap), min_size, 75%
window)`, snapped to whole rows so it never shows a half-row — no slack anywhere — and its
`GtkScrolledWindow` carries `.dt_recessed_scroll`, which the theme gives `padding: 2px` over a
`border: 1px`. Counting the padding alone handed the viewport 123 px for 125 px of content, and
`GTK_POLICY_AUTOMATIC` did the rest. Measured, same rows and same CSS, border omitted then
counted: `page=123 < upper=125, scrollbar` → `page=125 = upper=125, none`.

Reproduce this class of bug offscreen in seconds: build the widget with the theme's CSS on it,
pump the main loop, then compare the scrolled window's vadjustment `page_size` against `upper`.
A scrollbar that appears for a couple of pixels looks like a content-height miscount and is
usually a frame the request forgot.

### A UTILITY window must ask for its position, transient-for is not enough

`gtk_window_set_transient_for()` ties a window to its parent for stacking and focus, but the
window manager still decides *where* to map it. For an ordinary toplevel it places it over the
parent; give it `GDK_WINDOW_TYPE_HINT_UTILITY` and it drops the window at the root origin
instead — the leftmost monitor on a multi-head setup, whichever screen the application is
actually on. Measured on X11 with two monitors (primary at x=1920), same parent and same
transient hint throughout: transient alone lands at (2020, 129), transient + UTILITY at (0, 0),
and the focus flags (`set_focus_on_map`, `set_accept_focus`) change nothing either way.

So a UTILITY window states its position itself, `GTK_WIN_POS_CENTER_ON_PARENT`, on every
platform — not inside a `#ifdef GDK_WINDOWING_QUARTZ` block, which is how the shape manager
panel (`libs/masks.c`) came to open on the wrong screen while the module-order graph
(`libs/ioporder.c`), the tag manager (`libs/tagging.c`) and the event supervisor
(`gui/actions/supervisor_window.c`) — none of which set the UTILITY hint — opened correctly.

The hint costs nothing for a panel shown and hidden repeatedly: GTK consults it on the first
mapping only, so a window the user has dragged elsewhere keeps the place they gave it across
later hide/show cycles.

### A window a toggle button opens has exactly one state: the button's

`gtk_widget_hide_on_delete()` is the usual answer to a window whose widgets and state must
survive being closed, but it hides the window behind the back of whatever opened it. When the
opener is a `GtkToggleButton` — the shape manager panel's toolbox button (`libs/masks.c`) — the
button stays pressed after a window-manager close, and the next click reads that state as "the
panel is open" and hides an already-hidden window: it takes two clicks to bring the panel back.

So the button's `active` flag is the panel's only state. Visibility is driven from `toggled`,
never from `clicked`, and the `delete-event` handler hides nothing itself — it un-presses the
button and returns `TRUE`, leaving that same `toggled` handler to save the geometry and hide.
The order matters: `gtk_toggle_button_set_active()` emits `clicked` as well as `toggled`, so a
`clicked` handler flipping visibility would re-show the window it was just asked to close. The
handler compares `active` against the window's actual visibility and returns when the two agree,
which is what makes it safe to re-enter from the close path.

### Modal dialogs must explicitly refocus their parent on close

`gtk_window_set_transient_for()` at dialog creation is not enough to guarantee focus returns to
the parent window once the dialog is destroyed — on macOS/quartz in particular, GTK does not
reliably hand keyboard focus back the way X11 window managers do with transient hints.

Every top-level modal dialog (one whose transient parent is the main window, not another
still-open dialog) must call `dt_gui_refocus_parent()` (`gui/gtk.{h,c}`) right after
`gtk_widget_destroy()`. It falls back to the main window if no valid parent is passed, and
handles the macOS-specific `dt_osx_focus_window()` call internally. Mechanical pattern (capture
the parent *before* destroying the dialog, since the widget is invalid afterwards):

```c
GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
gtk_widget_destroy(dialog);
dt_gui_refocus_parent(dialog_parent);
```

Do NOT apply this to a nested dialog (e.g. a warning/confirm popup) whose transient parent is
another dialog still open at that point — GTK already hands focus back to a live parent window
correctly; this only matters for the final return to the application. Also skip
`GtkFileChooserDialog`/native choosers, which are a separate, already-correct subsystem. Any
dialog created without a transient parent at all (e.g. a popup menu action, which cannot
legitimately use the popup's own toplevel — see `gui/dtgtk/thumbnail.c`'s "Active modules" dialog)
should instead be parented directly to `dt_ui_main_window(darktable.gui->ui)` at creation time.

### Worker-thread → GUI-thread deferred callbacks referencing a shared struct need a refcount

The `g_main_context_invoke(NULL, callback, params)` pattern (worker thread schedules `callback` to
run later on the GUI thread) is used throughout the codebase to touch GTK widgets safely from a
non-GUI thread. When `params` carries a pointer into a struct that can also be torn down
independently through the same pattern (e.g. `libs/backgroundjobs.c`'s per-job
`dt_lib_backgroundjob_element_t`, updated via `.updated`/`.message_updated`/`.cancellable` and torn
down via `.destroyed`, all reachable concurrently from worker threads doing pixel/import work), a
"destroy" callback can run — and free the struct — while an "update" callback scheduled earlier for
the same struct is still queued, waiting for its turn on the GTK main loop. The queued update then
dereferences freed memory (Sentry issue 130394919: `EXCEPTION_ACCESS_VIOLATION` in
`gtk_label_set_text`, called from a stale `dt_lib_backgroundjob_element_t*`).

`control->progress_system.mutex` (`control/progress.c`) only serializes the *scheduling* of these
callbacks against each other — it says nothing about their relative *execution* order on the GUI
thread, and does not protect a struct shared across independent worker threads that don't otherwise
synchronize with each other before calling into the progress API.

Fix pattern (mirrors the forms/history-item refcounting above): give the shared struct a
`dt_atomic_int refcount`. Every proxy function that schedules a callback referencing it takes a
reference first; every callback drops its reference on the way out, freeing the struct only when
the count reaches zero — whichever callback that happens to be. The "destroy" callback additionally
NULLs every GTK widget pointer in the struct (right after removing/destroying them) instead of
freeing the struct outright, so any other callback still queued for the same struct sees NULL and
skips the now-invalid widgets instead of touching freed GTK objects.

---

## Keyboard shortcuts (accelerators)

### Widget shortcuts need their own closure — GTK's native accel-group activation is unreachable

`src/gui/accelerators.c` offers two ways to register a shortcut: a "generic" one
(`dt_accels_new_action_shortcut`, `dt_accels_new_virtual_shortcut`/`_instance`) that builds a
`GClosure` via `dt_shortcut_set_closure()`, and a "widget" one (`dt_accels_new_widget_shortcut`)
that instead calls `gtk_widget_add_accelerator(widget, signal, accel_group, key, mods, flags)`,
relying on GTK's own `gtk_window_activate_key()` to fire `widget`'s signal when the key is
pressed — which only works if `accel_group` is attached to a `GtkWindow` via
`gtk_window_add_accel_group()`.

That attachment was intentionally removed on 2025-04-02 (`2e693e6b3`, "Accels: do not use Gtk
window connection for accel groups... avoids crashes... Fix #484"): the app now handles every
keystroke itself through `dt_accels_dispatch()` → `_key_pressed()` → `_call_shortcut_cclosure()`,
which looks up `dt_shortcut_get_closure(shortcut)` and does nothing if it's `NULL` — it never
falls back to GTK's native accel-group activation. A same-day attempt to restore just the global
accel-group attachment (`c8770a367`, "Still connect global accels to window") was reverted two
minutes later (`7273a1371`, commit message "Nope"). Re-attaching accel groups to the window is a
dead end that was already tried and abandoned; it is not the way back in.

`dt_accels_new_widget_shortcut()` was never updated for the migration: it still leaves
`shortcut->closure = NULL`, so any shortcut registered only through it is keyboard-dead — clicking
the widget still works (plain `"clicked"`/`"toggled"` GTK signal), but the accelerator silently
does nothing, with no error anywhere. Confirmed dead in practice for the only two default-keybound
consumers of this path in the whole codebase: `src/libs/tools/filter.c`'s "Reload current
collection" (Ctrl+R) and "Toggle culling mode" (Ctrl+S).

Fixed by giving widget shortcuts a real closure too (`_widget_shortcut_callback()`, wired via
`dt_shortcut_set_closure()` inside `dt_accels_new_widget_shortcut()`), which just does
`g_signal_emit_by_name(shortcut->widget, shortcut->signal)` — the same activation path every other
shortcut type already uses. Any future direct caller of `gtk_widget_add_accelerator()` for a
keyboard shortcut in this codebase has the same problem: it needs a closure the internal
dispatcher can invoke, not just a GTK-level accelerator that no window will ever activate.

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

## Nightly distribution

The Sentry **environment** is `<build channel>-<platform>` (`nightly-windows`,
`self-build-linux`, `package-fedora-macos`): the sessions API groups crash-free rates by
release and environment and by nothing else, so this is how the website shows a build's crash
rate per package. Split on the LAST hyphen — a channel can contain hyphens. Sessions from before
this carry the bare channel; readers must accept both.

Nightlies land in one `nightly-YYYY-MM` pre-release per month (GitHub caps a *release* at
1000 assets; five formats a night filled the old rolling `v0.0.0` in under a year).
`tools/nightly_manifest.py` writes `nightly.json` — the newest build per format — after
every nightly, and that file drives the website buttons, the in-app check
(`src/common/updates.c`), the Homebrew cask and the Scoop manifest. The format keys in the
script's `FORMATS` and in `dt_updates_runtime_format()` must agree. `doc/nightly-distribution.md`
is the manual: secrets, retention, R2, signing costs, GHCR for later.

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
into the whole software through `#include "darktable.h"`. This voids the modularity
principle, creates many bugs, data races, and makes any maintenance tedious and prone to 
edge effects, since the app is heavily asynchronous and parallel.

The Ansel codebase should move toward more enclosed modularity, making data structure private
to each translation unit and exposing only API to the outside (getters/setters/init/cleanup). 
Direct value changes on data not owned by the current TU are forbidden. The dependency graph 
should be simplified and only a minimal set of `#include` should be kept per TU. In particular,
`src/darktable.h` should inherit from lower-level modules, but lower-level modules
should not inherit it, so it should stop being the glue of all common helpers throughout
the software.

CRUD operations should have one central entry point for the whole software and run only
once, for as long as user didn't send new input, so the data lifecycle is legible and
cacheable.

Since every data flow in the software is a pipeline, issues should be tracked to their root
cause by climbing the call tree up until the source is found, instead of being fixed where
they are visible.