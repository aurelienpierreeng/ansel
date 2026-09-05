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

`views/lighttable.c`'s `enter()` calls `dt_undo_clear(dt_undo_get_global(),
DT_UNDO_LIGHTTABLE)`, and `DT_UNDO_REMOVE` is part of that mask. Going to the darkroom and
coming back discards the record, which frees the snapshot and makes the removal permanent.
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

### The staging tables die with the connection, and the undo stack outlives it

`memory.` is per-connection, so the twins exist only as long as the database is open. At
shutdown `dt_database_close()` runs BEFORE `dt_undo_cleanup()`, which is what frees every
undo record still held -- so the free callback of a removal the user never undid runs against
a closed connection. `_remove_undo_data_free()` (`common/image.c`) checks
`dt_database_is_open()` first: there is nothing to drop, the tables went with the connection.
Without that check the ten DELETEs fire on a NULL handle, which the debug build's
`assert(x == SQLITE_OK)` turns into an abort on quit, and which a platform whose SQLite is
built without API armor turns into a crash.

### What it costs

A snapshot is a full copy of every row the image owns, held in RAM (`memory.` is an in-memory
database) until the undo record is discarded. A mask-heavy history is the bulk of it, and a
removal of several thousand such images holds several thousand copies at once. Two things
bound that: the lighttable discards the records on every view entry, and the copies are only
of what was about to be deleted anyway. Each image also stages its own copy of the shared
film roll row, so that any single undo can recreate it without depending on the others.

## What a restore does not put back

The selection. `main.selected_images` rows are cascaded away with the image and are not
staged: which images are selected is transient GUI state, not something an undo owes the
user.
