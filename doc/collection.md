<!-- Provenance: every finding carries the commit it was established against. -->

# The collection and library module

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> What `src/libs/collect.c` owns, what it must not, and what happens to the user's view after an import or a removal.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## What the module owns, and what it does not

*Found `22f623c0be`, 2026-06-25.*

`src/libs/collect.c` is the left-panel "Library" GUI. It does NOT build the collection query —
that is `src/common/collection.c`. The GUI's only job is to write the conf keys
`plugins/lighttable/collect/{num_rules, item<N>, mode<N>, string<N>, tab}` and call
`dt_collection_update_query()`.

Three tabs: **Folders** (film-roll list / folder tree; relocate + remove in batches),
**Collections** (tag browser + delete + rename), **Queries** (multi-rule builder + raw SQL via
`DT_COLLECTION_PROP_QUERY`).

**Drag-and-drop of lighttable images onto tree rows IS implemented**, and the text that stood
here said the opposite. It claimed the attempt had been abandoned, that "a GtkTreeView with a
manual `gtk_drag_dest_set` reliably receives motion but does not deliver the drop on tree
models", and that it must not be re-added without a non-tree drop target. Every clause was false
of this tree. `f8fa7ceb7f` (2026-06-13) implemented it: `libs/collect.c:3220` calls
`gtk_drag_dest_set` with `GTK_DEST_DEFAULT_DROP`, and `_view_drag_data_received()` (`:2281`)
does receive the drop — it stops GtkTreeView's own emission, calls `_do_drop()` and
`gtk_drag_finish()`, and carries a fix for the synthetic row-activation that follows it (issue
#905, `d->suppress_row_activated`). Dropping on a folder row moves the files; on a tag row it
attaches the tag. Motion and highlighting are driven manually so only the row under the pointer
takes the native highlight.

That entry was worse than merely stale: it told a reader to rip out or refuse to touch working,
deliberately-built code, and handed them a wrong technical premise to justify doing so.

The removal-undo design is [`removal-undo.md`](removal-undo.md).

## A GtkTreeView holds ONE deferred scroll target, and expanding a row defers it

*Found `f7b2f49a1e`, 2026-09-24.*

`gtk_tree_view_scroll_to_cell()` scrolls immediately only when the rows it needs are already
validated; expanding a row invalidates everything below it, so a scroll asked for right after an
expansion is *stored* instead, and the next such call **replaces** the stored one. The last
request wins, whatever its alignment.

That is what makes the folder tree's two ways of opening a folder one question, not two. Clicking
the expander only expands, so `_view_row_expanded()` — which top-aligns the node so its children
come on screen — is the only request. Clicking the folder *name* goes through `row_activated()` →
`update_view()` → `tree_expand()`, whose exact-match branch expands the node itself and then asks
for a minimal scroll (`use_align == FALSE`) to reveal the row: that second request displaces the
handler's reveal and then does nothing at all, the row being visible already — the user just
clicked it. So `tree_expand()` issues its minimal scroll only when it did NOT expand a folder with
children, and the reveal is left to the one handler that owns it.

Measure this class of thing offscreen rather than reading it out of the source: a
`gtk_offscreen_window_new()` holding the treeview, the row scrolled into the middle, then the
expansion, then the vertical adjustment printed — it separates "no scroll was asked for" from "the
scroll was asked for and replaced" in seconds.

## After an import: which image opens, and which folder the library shows

*Found `3b9d3b50e9`, 2026-08-27.*

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

## "Remove from library" is undoable, and the flag that hides an image survives the snapshot

*Found `3b3957d5ea`, 2026-09-04.*

Removing an image from the library stages every row it owns into `memory.removed_*` twins
before the foreign keys delete them, and Ctrl+Z copies them back — `doc/removal-undo.md` is
the full map, `database/removed_image_repository.c` the SQL, `dt_image_remove_undoable()` and
`_pop_undo()` (`common/image.c`) the bookkeeping. `dt_image_remove()` still records nothing
and is what delete-from-disk uses: a trashed file has nothing to restore.

**The trap that costs a whole test round is `DT_IMAGE_REMOVE`.**
`dt_control_remove_images_job_run()` sets it on the batch *before* deleting anything, so the
grid stops showing the images while the job runs, and `database/collection_query.c` filters
that flag out of every collection query. It is therefore in the row that gets staged, and a
verbatim restore brings the image back into the database and into **no view at all** — the
row is present, complete and correct, `PRAGMA foreign_key_check` is clean, and the image is
simply never selected by any query again. Reading "the rows came back" as "the undo works" is
exactly the mistake this bug rewards: the check that separates the two is `flags & 256` on
the restored row, not the row's existence. `_pop_undo()` clears it through
`dt_image_repository_clear_flag_among()` before re-running the collection query.

Two more things a reviewer would otherwise simplify away. The snapshot must be taken at the
very top of the removal, before `dt_grouping_remove_from_group()` runs: that call rewrites the
`group_id` of images **nobody asked to remove**, which lives in no table the removed image
owns and is staged separately in `memory.removed_groups`. And the restore runs under `PRAGMA
defer_foreign_keys = ON`, because a group removed in one go comes back one undo record at a
time and an image regularly precedes the leader it points at; any `group_id` still dangling at
the end is repointed at the image itself rather than allowed to fail the commit.

The folder list learns about a restored roll through `dt_film_notify_rolls_changed()`
(`common/film.h`), which `gui/common/film_gui.c` turns into `DT_SIGNAL_FILMROLLS_CHANGED`.
`common/` is layer 1 and `control/` is layer 3, so raising the signal from `common/image.c`
is a layering inversion `tools/check_layering.sh` counts against the baseline; the notifier
is the same inversion `common/image_notify.h` and `common/thumbnail_notify.h` already use.

**The schema does not cascade uniformly.** Only `history`, `masks_history`, `tagged_images`
and `history_hash` carry a foreign key on `images(id)`; `module_order`, `color_labels` and
`meta_data` carry none, in a fresh database as in a migrated one — `dt_image_repository_delete()`
deletes `meta_data` by hand, and the other two are left behind by every removal, undone or not.
So the restore DELETEs each child table's rows before copying the staged ones back: a no-op for
the four that cascade, and the only thing stopping `color_labels` — which has no unique
constraint either — from gaining a duplicate row on every remove/undo cycle.

**`memory.` dies with the connection, and `_remove_undo_data_free()` checks
`dt_database_is_open()` before dropping a snapshot.** `dt_undo_cleanup()` does run after
`dt_database_close()`, but measurement says it finds an empty list: the GUI teardown calls
`dt_ctl_switch_mode_to("")` (`darktable.c`, well before the close), switching to no view enters
`dt_view_manager_switch_by_view()`, and its first act is `dt_undo_clear(..., DT_UNDO_ALL)` —
database still open. Without a GUI the order reverses, but no removal can have been recorded
either, both callers of `dt_control_remove_images()` being GUI ones. **So the guarded branch is
unreachable today and the check stays anyway**, for the cost of one call: it is the day either
half of that changes that a debug build would otherwise abort on quit, inside
`DT_DEBUG_SQLITE3_PREPARE_V2`'s assert, and a SQLite built without API armor would crash.
`dt_view_manager_cleanup()` is not what clears the list — it only unloads the view modules.

**Any view switch closes the undo window, not just re-entering the lighttable.**
`dt_view_manager_switch_by_view()` clears `DT_UNDO_ALL` on every switch, and the lighttable's
own `enter()` additionally clears `DT_UNDO_LIGHTTABLE`. `DT_UNDO_REMOVE` is in both masks, and
discarding the record frees the snapshot. That is every lighttable undo's lifetime, but here it
is also the point of no return for data the database was the only holder of: a trip to the
darkroom and back makes a removal permanent.

## The metadata panel writes a field when it stops being edited, to the images it shows

*Found `6b4dd7d712`, 2026-09-11.*

`libs/metadata.c` shows the values of the images to act on (`d->last_act_on`, refreshed by
`_update()` when that list changes) and writes to **those same images**, never to the selection.
The two differ exactly when it matters: with nothing selected the panel shows the image under the
cursor, and a click on another thumbnail has already moved the selection by the time the field it
leaves is written — writing to the selection then lands the edit on the image just clicked.

A field is written as soon as it stops being edited — focus lost (a click elsewhere), another
image taking over the panel (`_update()` commits the field still being typed in before switching
lists), Tab, Enter or "apply" — the way any text field behaves; only Escape discards it, by
clearing `d->editing` *before* the focus leaves. `d->editing` means "the user typed": every
programmatic fill goes through `_set_text_buffer()`, which blocks `_textbuffer_changed()`, so
emptying a `<leave unchanged>` field on focus never counts as an edit and never erases a value
across the selection. `_refresh()` re-reads what the images hold; `_update()` only follows the
list, so calling it to "redraw after a write" does nothing — the list has not changed.

Escape reaches the module only because `_key_pressed()` is connected **before**
`dt_accels_disconnect_on_text_input()`: that helper's own key handler takes Escape to hand the
focus back (`dt_widget_refocus()`) and stops the emission, so connected first it turns every
Escape into a plain focus-out — which, with focus-out committing, writes what Escape was meant to
discard. Any text field that commits on focus-out and cancels on Escape owes the same order.

---
