# Masks history deduplication (planned, not yet implemented) {#masks_history_dedup}

[TOC]

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. **Every file path in it was stale** and is corrected — see the note under *Status*.

## Status

**Design only — no code has been written yet.** This document exists so the design survives
between sessions and contributors; it is not a description of current behavior. See
`doc/masks-history.md` for what *is* currently implemented (the in-memory refcounting /
copy-on-write refactor this design builds on).

> **First checked against the tree on 2026-09-29, at `8f4638a04e`.** The design is unaffected;
> **every file path in it is not.** The re-stratification moved all three of its main targets —
> `common/database.c` → `src/database/`, `common/exif.cc` → `common/xmp_sidecar.cc`,
> `common/history_snapshot.c` → `src/database/history_snapshot_repository.c` — and, more
> importantly, **`src/database` is now a sealed module**: `tools/check_module_boundaries.sh`
> section 6 keeps SQL out of everything else, so the "pure SQL, no C-driven transform" parts of
> this plan can no longer be written where it says. The paths below are updated; read the
> *Files to touch* section as rewritten, not the original. `CURRENT_DATABASE_VERSION_LIBRARY` is
> still 36 (`src/database/database.c:89`), so the 36 → 37 bump still holds.

## Problem

The in-memory forms-history refactor (`src/develop/masks/masks_history.{h,c}`) made `dev->forms`/
`hist->forms` share the same `dt_masks_form_t*` objects by reference across history steps,
cloning only on write (copy-on-write). This sharing **stops at the persistence boundary**. Two
places still serialize one row/entry **per (history step, formid)**, with no dedup, even when the
exact same form is unchanged across dozens of consecutive steps:

- SQL table `masks_history` (schema in `src/database/database.c`):
  `(imgid, num, formid, form, name, version, points, points_count, source)`, no `UNIQUE`
  constraint. Written by `dt_masks_write_masks_history_item()`
  (`src/develop/masks/masks.c:1131`) — which no longer issues SQL itself but goes through the
  history repository — called once per form per history item from
  `src/develop/dev_history.c:1574`, inside the rewrite `dt_dev_write_history_ext()` performs for
  **every** history step.
  That function deletes and rewrites the **entire** history + masks_history for the image on
  **every single commit** (`_cleanup_history`, `src/develop/dev_history.c:1500`, →
  `dt_history_repository_delete_dev_history()` → `dt_history_repository_delete_masks_history()`
  — both `src/database/history_repository.h:87,90`; they were `dt_history_db_delete_*` before the
  database module was sealed). A form shared unchanged across 100 history steps (the
  common case, enabled by the refactor above) still gets its full points BLOB serialized 100
  times, on every commit.
- XMP array `Xmp.darktable.masks_history[N]` (`src/common/xmp_sidecar.cc`, around the
  `Xmp.darktable.masks_history[` handling at :1064): reads from the SQL table above and writes
  one XMP array entry per row — same duplication, downstream.

`dt_dev_mask_history_overload()` (`src/develop/dev_history.c:1505`) already warns users about
this via a toast ("consider compressing history...") without fixing it. The intended direction
is flagged as a code comment near `dt_masks_read_masks_history()`
(`src/develop/masks/masks.c:1118`): attach the forms snapshot to its own object, linked by ID
to the history item, instead of duplicating it inline.

**Historical precedent**: before XMP format version 3, `read_masks()` (now
`src/common/xmp_sidecar.cc`,
legacy path) stored a single entry per `mask_id` for the whole image — already deduplicated, with
no `num` dimension. The current "one row per step" scheme is a v3 regression. This design
reverses it.

## Hard constraint: must not touch any user's live database before Ansel 1.0 ships

This is a persistent-schema change to a table every existing user already has data in. It is
to be developed and merged on a **dedicated branch**, kept out of `master` until the Ansel 1.0
release is actually being prepared — never landed on the branch users' current builds track.
(No such branch exists yet, as of 2026-09-29: nothing matches `*dedup*`. An earlier summary of
this design read the sentence above as saying the work had been done on one; it has not.) This avoids
any risk of migrating a live database prematurely, without needing conditional compilation.

## Design

### SQL schema

Split the current table in two (same rename → create → copy-transform → drop migration pattern
already used elsewhere in the schema, e.g. the `masks_history` CREATE TABLE in `src/database/database.c`):

```sql
CREATE TABLE masks_history_forms (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  imgid INTEGER,
  formid INTEGER, form INTEGER, name VARCHAR(256), version INTEGER,
  points BLOB, points_count INTEGER, source BLOB,
  UNIQUE(imgid, formid, form, name, version, points, points_count, source),
  FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE TABLE masks_history (   -- becomes a thin reference table
  imgid INTEGER, num INTEGER, formid INTEGER, content_id INTEGER,
  FOREIGN KEY(imgid) REFERENCES images(id) ON DELETE CASCADE ON UPDATE CASCADE,
  FOREIGN KEY(content_id) REFERENCES masks_history_forms(id) ON DELETE CASCADE
);
CREATE INDEX masks_history_imgid_index ON masks_history(imgid, num);
```

The `UNIQUE` constraint on the content columns does the dedup **at the SQL level**, no
application-side hash needed (no new dependency, no collision risk): `INSERT OR IGNORE` into
`masks_history_forms`, then `SELECT id` (via the unique index, so fast) to get the `content_id`
to reference. This works correctly **regardless of caller** — important because
`dt_masks_write_masks_history_item()` has **two call sites**: the main per-commit loop
(`src/develop/dev_history.c:1355`) and the legacy "spots" compatibility path
(`src/iop/spots.c:189`) — both benefit automatically, with no cache to thread between them.

Since `dt_dev_write_history_ext()` deletes and rewrites the whole image's history on every
commit, dedup happens naturally on every full rewrite: `_cleanup_history` must empty **both**
tables for that `imgid`, then the rewrite loop repopulates `masks_history_forms` only with
content actually in use — self-cleaning, no separate GC pass needed.

Migration: new `else if(version == 36)` block in the library schema upgrade in
`src/database/database.c`, bumping `CURRENT_DATABASE_VERSION_LIBRARY` (`:89`) 36 → 37. Still
pure SQL and no C-driven transform: `INSERT ... SELECT DISTINCT` populates
`masks_history_forms`, then `INSERT ... SELECT ... JOIN` populates the new thin `masks_history`
resolving `content_id`. It now lives inside the sealed database module, which is where it
belongs anyway.

The ephemeral `memory.undo_masks_history` table (also `src/database/database.c`, recreated on
every startup, **not** subject to the version counter — no migration needed, just update its
`CREATE TABLE`) needs a `memory.undo_masks_history_forms` mirror.
`src/database/history_snapshot_repository.c` — which is where the create/restore/clear SQL now
lives (`:83-90` copies into `memory.undo_masks_history`, `:123-126` copies back, `:144` clears)
— must copy **both** tables for lighttable undo/redo snapshots.

### Read path (in-memory dedup bonus)

`dt_masks_read_masks_history()` (`src/develop/masks/masks.c:1118`) reads rows through
`dt_history_repository_foreach_mask_item()`, so the join goes in the **repository**, not here:
join the two tables on `content_id`, ordered by `num`, and keep handing `masks.c` one row at a
time. Keep a local `GHashTable` (`content_id` →
`dt_masks_form_t*`) while looping: if a `content_id` was already materialized, call
`dt_masks_form_ref()` on the existing object instead of allocating a new one. This extends the
in-memory refactor's benefit to **loading** an image, not just editing an already-open session.

### XMP (new v4 format, selected by file version)

The XMP reader already supports multiple mask-format generations, selected by the file's
declared version (legacy `read_masks()` vs `read_masks_v3()`, `src/common/xmp_sidecar.cc:1050`).
Add `read_masks_v4()` next to the existing readers, dispatched the same way. Writing switches to
the new format outright (no old-format writer to keep around, since this all lives on the
dedicated branch until merge):

```
Xmp.darktable.masks_history_forms[K]/darktable:{mask_id, mask_type, mask_name, mask_version, mask_points, mask_nb, mask_src}
Xmp.darktable.masks_history_refs[N]/darktable:{mask_num, mask_content_ref}   -- mask_content_ref = index K
```

Direct mirror of the SQL split (content array + thin reference array), reusing the same
in-memory dedup pass (one walk over `dev->history`, same pointer/content_id keyed hash table as
the SQL side). Touches the sidecar's mask writer and needs the new `read_masks_v4()` next to
`read_masks_v3()` — both in `src/common/xmp_sidecar.cc` now, not `exif.cc`.

## Files to touch

**Rewritten 2026-09-29: every path below moved, and the database is now sealed.** All SQL —
schema, migration, both repositories — must live under `src/database/`;
`tools/check_module_boundaries.sh` section 6 fails the build otherwise, and
`src/database/README.md` is the map.

- `src/database/database.c` — new `version==36` migration block, bump
  `CURRENT_DATABASE_VERSION_LIBRARY` (`:89`), new `memory.undo_masks_history_forms` ephemeral
  table, and the `masks_history_forms` schema.
- `src/database/history_repository.{h,c}` — the two-table read and write. `masks.c` reaches this
  through `dt_history_repository_foreach_mask_item()` and
  `dt_history_repository_delete_masks_history()`, which must empty **both** tables; the join and
  the `content_id` resolution belong here, not in `develop/`.
- `src/database/history_snapshot_repository.c` — lighttable undo snapshot create/restore/clear,
  mirrored to two tables (`:83-90`, `:123-126`, `:144`).
- `src/develop/masks/masks.c` — `dt_masks_write_masks_history_item()` (`:1131`) and
  `dt_masks_read_masks_history()` (`:1118`): the in-memory dedup on the read path, and passing
  the content key on the write path. No SQL.
- `src/common/xmp_sidecar.cc` — new XMP v4 writer; new `read_masks_v4()` selected by file version
  (v3 reader at `:1050` stays, for files written by older Ansel/darktable versions).
- `src/iop/spots.c` — no signature change needed (benefits automatically via
  `dt_masks_write_masks_history_item()`), verify in testing only.

## Verification plan

1. On the dedicated branch, against a **copy** of a test database (never a real user DB): run the
   migration, verify `SELECT COUNT(*) FROM masks_history_forms` is far below the old row count for
   an image with deep history, and that the image reloads identically (compare rendering
   before/after migration). Test: lighttable undo/redo, copying history to another image,
   `iop/spots.c` (image with legacy "spots"), XMP export then reimport, history compression.
2. Confirm XMP files written by older Ansel/darktable versions (v3 format) still import correctly
   through the existing `read_masks_v3()` path.
3. Measure `dt_dev_write_history_ext()` time before/after on a heavily-masked image (e.g. the
   140-shape retouch test image used for pipeline performance work), to quantify the win.
4. Merge to `master` only when Ansel 1.0 is actually being prepared.
