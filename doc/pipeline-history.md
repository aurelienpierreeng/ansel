<!-- Provenance: every finding carries the commit it was established against. -->

# History, the darkroom worker, and the transient slot

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> How the pipeline reads history without holding its lock, when the worker must be joined, and how a live edit reaches the pipe without being committed.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

The threading model is in [`reorganisation.md`](reorganisation.md).

## The darkroom worker thread must be joined before view `leave()` tears down pipe state

*Found `b5bbe6b827`, 2026-07-12.*

`dt_dev_darkroom_pipeline()` runs forever in the dedicated `DT_CTL_WORKER_DARKROOM` job thread,
servicing `dev->preview_pipe` then `dev->pipe` in a loop. `views/darkroom.c`'s `leave()` sets
`dev->exit = 1` and each pipe's `shutdown` atomic, then — still from the GUI thread — calls
`dt_dev_pixelpipe_cleanup_nodes()` on `dev->pipe` and `dev->preview_pipe` and frees
`dev->iop`/`dev->history`. Neither flag actually preempts the worker: `dev->exit` is only checked
between loop iterations and between servicing each pipe.

> **Correction, 2026-09-29.** This used to continue: "and `pipe->shutdown` is never polled inside
> `dt_dev_pixelpipe_process()` to abort mid-flight — it's only read afterwards". That is false.
> `dt_dev_pixelpipe_has_shutdown()` (`pixelpipe_hb.h:691`) is read at five sites in
> `pixelpipe_hb.c`, and the `KILL_SWITCH_*` macros exist for exactly that mid-flight abort. The
> fix this section documents — joining on `running` — is real and correct; only the stated reason
> was wrong, and a reader might have set about "adding" a killswitch that already exists.

`leave()`'s `busy_mutex` locks around the pipe-nodes teardown look like they serialize against the
worker, but `busy_mutex` carries a comment that it must "NEVER be used from the GUI thread" for
exactly this reason: two worker-thread accesses bypass it entirely — `dt_dev_pixelpipe_set_input()`
(iterates `pipe->nodes` every loop tick to refresh `piece->iwidth`/`iheight`) and the history-hash
resync at the top of `_resync_pipe_with_history()` (can re-trigger `dt_dev_pixelpipe_change()`,
which rebuilds `pipe->nodes` from `pipe->dev->iop`). Either can still be touching a pipe's nodes, or
`dev->iop`, after `leave()`'s mutex-guarded section already freed them. The resulting heap corruption
does not crash where it happens — it crashes wherever the next unlucky reader lands (Sentry issue
133807805: a garbage transform pointer inside `iop/colorin.c`'s `cleanup_pipe()` — the field
was named `xform_cam_Lab` at the time and has since been folded into a single `d->conversion`
object, so grepping for the old name finds nothing — reached via
the worker's *own* next `resync_pipe_with_history()` call, nowhere near the actual race).

Fixed by making `dt_dev_pixelpipe_t.running` (set at the very top/bottom of
`dt_dev_darkroom_pipeline()`) an actual `dt_atomic_int` — it existed before but was write-only.
`leave()` now polls it for both `dev->pipe` and `dev->preview_pipe` right after setting
`dev->exit`/`shutdown`, and blocks until both read `FALSE` before touching any node/iop/history
teardown. Any other GUI-thread code that tears down darkroom pipe state must wait on this flag the
same way — `dev->exit`/`pipe->shutdown` alone do not guarantee the worker has stopped touching a
pipe.

## History items are refcounted; the pipe resyncs against a snapshot, not under `history_mutex`

*Found `77c8570b49`, 2026-08-28.*

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
and the snapshot never sees a half-rewritten item. `tests/unittests/test_history_snapshot.c`
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

## Drawn-mask drags render from the transient slot; history is written once, on release

*Found `532c0c24da`, 2026-08-28.*

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
(`libs/shape_manager.c`, no focused module) is unchanged and still commits directly; the focused-piece
path needs `dev->gui_module` to be the shape's owner.

## `_insert_default_modules` must check `dev->history` in memory, not the DB row for `dev->image_storage.id`

*Found `48f8e58e0a`, 2026-07-31.*

`dt_dev_init_default_history()` (`dev_history.c`) walks every loaded module and, for each one,
calls `_insert_default_modules()` to backfill a default-params history entry for any
`default_enabled`/`force_enable` module "missing" from history. Its "is this module already
covered?" check used to be a DB query, `dt_history_repository_module_exists(imgid, operation)`
(`database/history_repository.h:146`; this file named it `dt_history_check_module_exists` with a
third argument until 2026-09-29, and no such symbol exists)
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
