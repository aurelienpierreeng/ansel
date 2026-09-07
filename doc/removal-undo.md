# Undoing "remove from library"

Removing an image from the library deletes its `main.images` row, and the foreign keys take
its history, masks, tags, colour labels, metadata, module order and history hash down with
it. The raw file and its XMP stay on disk, but the database entry is the only place several
of those things ever existed — a group membership, an edit made before the XMP was last
written, a colour label — so "the file is still there" is not the same as "nothing was
lost". Ctrl+Z in the lighttable therefore has to put the rows back, not re-import the file.

Deleting an image *from disk* is a different action and has no undo: the file is in the
system trash, or gone, and that is the operating system's business, not the database's.

## How it works

The rows are staged before they are deleted, and copied back to undo. Three pieces:

- `memory.removed_*` — one twin per table a removed image owns rows in, created in
  `_create_memory_schema()` (`database/database.c`) by `CREATE TABLE ... AS SELECT` over the
  live tables. The twins' columns therefore follow whatever `main` holds, and each carries
  two extra leading columns, `snap_id` and `undo_imgid`.
- `database/removed_image_repository.c` — the bulk table-to-table copies. It names its
  columns from `PRAGMA table_info()` on the twins, so a schema migration reaches it without
  an edit. The rows never become C structs, for the same reason
  `database/history_snapshot_repository.c` gives.
- `common/image.c` — the undo bookkeeping. `dt_image_remove_undoable()` takes the snapshot
  and records a `DT_UNDO_REMOVE` item; `_pop_undo()` restores on undo and re-removes on redo;
  `_remove_undo_data_free()` drops the snapshot when the record is discarded, which is what
  makes the removal permanent.

`dt_control_remove_images_job_run()` (`control/jobs/control_jobs.c`) wraps its loop in a
`dt_undo_start_group()` / `dt_undo_end_group()` pair, so one Ctrl+Z takes a whole batch back.

`dt_image_remove()` keeps its old behaviour and records nothing. It is what the
delete-from-disk job and the duplicate-undo path call.

## Things that are not obvious

### The undo window closes when the view is left

Leaving is enough, and coming back is not required: `dt_view_manager_switch_by_view()`
(`views/view.c`) clears `DT_UNDO_ALL` on EVERY switch, before the old view's `leave()` has
even run. `views/lighttable.c`'s `enter()` then clears `DT_UNDO_LIGHTTABLE` as well, and
`DT_UNDO_REMOVE` is in both masks. Either way the record is discarded, which frees the
snapshot and makes the removal permanent.

That is the same lifetime every other lighttable undo has; the difference is that here it is
also the point of no return for data the database was the only holder of.

### `DT_IMAGE_REMOVE` is written before the removal, so it is part of the snapshot

`dt_control_remove_images_job_run()` sets `DT_IMAGE_REMOVE` on every image of the batch
before deleting anything, so the grid stops showing them while the job runs. Every collection
query filters that flag out (`database/collection_query.c`). The flag is therefore in the row
that gets staged, and restoring the row verbatim brings the image back into the database and
into no view at all — present, correct, and invisible. `_pop_undo()` clears it with
`dt_image_repository_clear_flag_among()` before re-running the collection query. An image
being restored is by definition no longer marked for deletion.

### Group membership is rewritten outside the removed image's own rows

`dt_grouping_remove_from_group()` hands a group to a new leader when its current one is
removed, rewriting the `group_id` of images nobody asked to remove. That rewrite lives in no
table the removed image owns, so `memory.removed_groups` stages the `(id, group_id)` of every
member of the affected group, and the restore replays it. This is why the snapshot has to be
taken at the very top of the removal, before anything else runs.

### Foreign keys are deferred, and a dangling group leader is repointed at itself

`main.images.group_id` has a foreign key on `main.images.id`. A whole group removed in one go
comes back one image per undo record, in whatever order the undo list holds, so an image's
leader is regularly still missing when its own row goes back in. The restore runs with
`PRAGMA defer_foreign_keys = ON` and, at the end, repoints at itself any `group_id` that
still names an absent image. A group left split by a partial undo is the honest outcome — the
alternative is to invent a leader or to fail the commit.

### The schema does not cascade uniformly, so the restore clears before it copies

Only four of the child tables carry a foreign key on `images(id)` -- `history`,
`masks_history`, `tagged_images` and `history_hash`. `module_order`, `color_labels` and
`meta_data` carry none, in a fresh database and in a migrated one alike; `dt_image_repository_delete()`
deletes `meta_data` by hand, which is why that second statement is in it, and the other two
are simply left behind by every removal, undone or not.

The restore therefore deletes each child table's rows for the image before copying the
staged ones back. For the four that cascade this is a no-op. For the others it removes the
survivor, which matters because `main.color_labels` has no unique constraint either: copying
the staged row on top of one that never left would duplicate it, and duplicate it again on
every further remove/undo cycle. Clearing first also makes a restore idempotent, and keeps
the feature from depending on which tables the schema happens to cascade today.

### The film roll comes back too

`dt_film_remove_empty()` runs after the batch, so removing a roll's last image takes the roll
with it, and `main.images.film_id` has a foreign key on it. Every image stages a copy of its
roll, and the restore inserts it with `OR IGNORE` — only one image of the roll actually needs
to recreate it, and the others must not fail on the duplicate.

Whoever shows the folder list has to be told, and `common/image.c` states that as a fact rather
than as a signal: `dt_film_notify_rolls_changed()` (`common/film.h`) carries it, and
`gui/common/film_gui.c` is what turns it into `DT_SIGNAL_FILMROLLS_CHANGED`. Raising the signal
from `common/` would be a layer-1 file calling into `control/`, which `tools/check_layering.sh`
counts; the same inversion is what `common/image_notify.h` and `common/thumbnail_notify.h`
already do for theirs, and a headless run with no handler installed simply drops the fact.

### The caches are emptied on the way out, and neither refills itself

`dt_image_history_changed()` runs on the restore: the removal emptied the image cache and the
mipmap cache, the mipmap cache regenerates only after an explicit removal, and the image cache's
`history_items` — the "altered" flag choosing raw processing over the unedited embedded JPEG —
would otherwise be whatever the entry held before. Reloading also re-reads the flags cleared
just above it, so no stale entry can write `DT_IMAGE_REMOVE` back over the restored row.

The removal itself uses `dt_mipmap_cache_remove_all_sizes()` rather than
`dt_mipmap_cache_remove()`, because the latter drops the thumbnails and deliberately keeps
`DT_MIPMAP_F` and `DT_MIPMAP_FULL`, the decoded raw input. Keeping them is right for a
development change and wrong for a removal: the buffer outlives the row, and on the way back
`basebuffer` slices a zero-sized buffer out of the stale entry and the thumbnail becomes an
8x8 husk that no later render replaces. Only a developed image shows it — an unaltered one
comes back from the embedded JPEG and never asks for the input.

### The staging tables die with the connection, and the guard for that is unreachable

`memory.` is per-connection, so the twins exist only as long as the database is open, and
`_remove_undo_data_free()` (`common/image.c`) checks `dt_database_is_open()` before trying to
drop a snapshot: there would be nothing to drop, the tables having gone with the connection.

`dt_undo_cleanup()` does run after `dt_database_close()` at shutdown, which is what the check
reads as its reason. Measured on a debug build, it is not: the four records of a removal that
was never undone are freed 1.6 s EARLIER, with the database still open. The GUI teardown calls
`dt_ctl_switch_mode_to("")` (`darktable.c`) well before the close; an empty view name is the
`switching_to_none` case of `dt_view_manager_switch()`, which calls
`dt_view_manager_switch_by_view()` with a NULL view anyway, and its first act is
`dt_undo_clear(..., DT_UNDO_ALL)`. `dt_undo_cleanup()` then finds an empty list.
(`dt_view_manager_cleanup()` is not involved -- it only unloads the view modules.)

Without a GUI the order does reverse, but nothing can have recorded a removal either: both
callers of `dt_control_remove_images()` -- `libs/collect.c` and `gui/dtgtk/thumbtable.c` --
are GUI ones. So no reachable path runs that callback against a closed connection today.

The check stays regardless. It costs one call, it cannot be exercised by a test, and it is
what stops a debug build from aborting on quit inside `DT_DEBUG_SQLITE3_PREPARE_V2`'s assert
-- and a SQLite built without API armor from crashing -- the day either half of that ordering
changes.

### The image cache entry of a removed image, and the deadlock it used to cause

Removing an image deletes its row while the lighttable is still showing it, so the next
thumbnail refresh asks the image cache for something the database no longer has. The entry is
re-created, `dt_image_repository_load()` fails, and it stays in the cache with
`id == UNKNOWN_IMAGE` -- locked, because that is how it was asked for.

Releasing it used to do nothing: both release functions guarded on `img->id <= 0`. The entry
stayed locked for the life of the process, and Ctrl+Z then hung the GUI thread inside
`dt_image_history_changed()`, waiting on a write lock nobody held -- `dt_cache_get()` spins on
`trywrlock`, which never yields. It looks like a crash and is not: the application is frozen and
has to be killed, which is what actually loses the snapshots, `memory.` going with the process.

Both release functions now guard the pointer only, and `dt_image_cache_testget()` refuses to
hand out an invalid entry at all. See CLAUDE.md, "Releasing an image cache entry returns the
LOCK, not the image".

### What it costs

A snapshot is a full copy of every row the image owns, held in RAM (`memory.` is an in-memory
database) until the undo record is discarded. A mask-heavy history is the bulk of it, and a
removal of several thousand such images holds several thousand copies at once. Two things
bound that: any view switch discards the records, and the copies are only of what was about to
be deleted anyway. Each image also stages its own copy of the shared film roll row, so that any
single undo can recreate it without depending on the others.

Measured on a library of 991 images carrying 16582 history rows, through the `-d memory` traces
in `removed_image_repository.c`: staging 984 of them took `sqlite3_memory_used()` from 2.6 MB to
20.8 MB, i.e. **about 19 kiB per image**. A removal of ten thousand would be on the order of
190 MB, for as long as the undo record lives.

**The purge is proved by a second cycle, not by watching memory fall.** Neither `VmRSS` nor
`sqlite3_memory_used()` drops when the records are discarded, and neither can: deleting rows
from an in-memory database returns pages to that database's own free list, and glibc rarely
returns a freed heap to the kernel. Both instruments read flat whether the rows were released
or leaked. What settles it is doing the whole thing twice -- the second run staged the same 984
snapshots and `sqlite3_memory_used()` grew by **0.0 MB**, reusing the pages the first purge had
freed. Had they leaked, it would have needed 18 MB more.

## What a restore does not put back

The selection. `main.selected_images` rows are cascaded away with the image and are not
staged: which images are selected is transient GUI state, not something an undo owes the
user.
