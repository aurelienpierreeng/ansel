# Masks history deduplication (planned, not yet implemented) {#masks_history_dedup}

[TOC]

## Status

**Implemented on the dedicated `masks-history-dedup` branch, not yet merged to `master`** (see
the hard constraint below — merge waits for the project version to actually reach 0.1). This
document is kept up to date with what the branch does, not a forward-looking design anymore; see
`CLAUDE.md`'s "Masks / forms history" section for the in-memory refcounting/copy-on-write refactor
this design builds on.

2026-08: revised to (a) reuse the existing `masks_history` table instead of splitting it in two,
which shrinks the migration to a single nullable column with no data transform, and (b) fold in
write-time savings that are independent of dedup but were previously left as a "measure it"
verification item — the actual write path had two real, separate inefficiencies found by reading
the code (see "Write-time cost, independent of dedup" below). Implemented shortly after: see the
"Trap found during implementation" note under history_snapshot.c for one correctness issue the
design missed (rowid preservation across undo/redo snapshots) and the write path for the exact
final hash-table-based dedup shape.

## Problem

The in-memory forms-history refactor (`src/develop/masks/masks_history.{h,c}`) made `dev->forms`/
`hist->forms` share the same `dt_masks_form_t*` objects by reference across history steps,
cloning only on write (copy-on-write). This sharing **stops at the persistence boundary**. Two
places still serialize one row/entry **per (history step, formid)**, with no dedup, even when the
exact same form is unchanged across dozens of consecutive steps:

- SQL table `masks_history` (`src/common/database.c:1086`, current schema):
  `(imgid, num, formid, form, name, version, points, points_count, source)`, no `UNIQUE`
  constraint. Written by `dt_masks_write_masks_history_item()` (`src/develop/masks/masks.c:2227`),
  called once per form per history item by `dt_dev_write_history_item()`
  (`src/develop/dev_history.c:1488-1518`), itself called for **every** history step by
  `dt_dev_write_history_ext()` (`src/develop/dev_history.c:1527-1554`). That function deletes and
  rewrites the **entire** history + masks_history for the image on **every single commit**
  (`_cleanup_history` → `dt_history_db_delete_dev_history` → `dt_history_db_delete_masks_history`,
  `src/common/history.c:488-536`). A form shared unchanged across 100 history steps (the common
  case, enabled by the refactor above) still gets its full points BLOB serialized 100 times, on
  every commit.
- XMP array `Xmp.darktable.masks_history[N]` (`src/common/exif.cc:3515-3553`): reads directly
  from the SQL table above and writes one XMP array entry per row — same duplication, downstream.

`dt_dev_mask_history_overload()` (`src/develop/dev_history.c:1452`) already warns users about
this via a toast ("consider compressing history...") without fixing it. The maintainer already
flagged the intended direction as a code comment near `dt_masks_read_masks_history()`
(`src/develop/masks/masks.c:2205-2209`): attach the forms snapshot to its own object, linked by ID
to the history item, instead of duplicating it inline.

**Historical precedent**: before XMP format version 3, `read_masks()` (`src/common/exif.cc`,
legacy path) stored a single entry per `mask_id` for the whole image — already deduplicated, with
no `num` dimension. The current "one row per step" scheme is a v3 regression. This design
reverses it.

## Write-time cost, independent of dedup

Read while designing this: two costs in the current write path have nothing to do with the
missing dedup and are worth fixing at the same time, since they're touched by the same code.

1. **No transaction around the per-image rewrite.** `dt_dev_write_history_ext()`
   (`src/develop/dev_history.c:1527`) issues one `DELETE` (main.history), one `DELETE`
   (masks_history), then one `INSERT` per history item plus one `INSERT` per form — all as
   separate statements with no `dt_database_start_transaction()`/`dt_database_release_transaction()`
   around them, unlike the equivalent batch-write pattern already used elsewhere (e.g.
   `src/develop/imageop.c:3073-3084`, prepared statement reused in a loop inside one transaction).
   Under SQLite's default autocommit mode each statement is its own transaction; for a history of
   any depth with masks this is N+M+2 separate commits where one would do. This is very likely the
   dominant cost of a commit, well above anything BLOB-size-related — should be fixed regardless
   of the dedup schema work, by wrapping the delete+rewrite loop in
   `dt_database_start_transaction(dt_database_get_global())` /
   `dt_database_release_transaction(dt_database_get_global())` (reentrant via `_trx_batch_level`,
   safe to nest inside a caller that's already mid-transaction — see `database.c:4625-4781`).
2. **No statement caching in the masks write path.** `dt_masks_write_masks_history_item()`
   (`src/develop/masks/masks.c:2227`) calls `DT_DEBUG_SQLITE3_PREPARE_V2` + `sqlite3_finalize`
   fresh on every single call, unlike `src/common/history.c`'s equivalent functions for
   `main.history` (`dt_history_db_write_history_item` and friends), which cache a `static
   sqlite3_stmt*` behind a mutex and just `sqlite3_reset`/`sqlite3_clear_bindings` it
   (`history.c:488-505` for the pattern). Re-preparing the same INSERT for every form on every
   commit is pure waste; should follow the same cached-statement pattern.

Neither of these needs the schema change below to fix, and both are cheap/low-risk (no stored
data changes). They compound with the dedup design: once large point BLOBs stop being
re-serialized for unchanged forms (next section), the per-statement/transaction overhead becomes
proportionally even more of what's left, so it's worth fixing in the same pass.

## Hard constraint: must not touch any user's live database before Ansel 0.1 ships

Both persistence formats need a real version bump for this design — there's no way around either
one (`CURRENT_DATABASE_VERSION_LIBRARY` 36→37, `DT_XMP_EXIF_VERSION` 5→6; see "Why one table
instead of two" and the XMP section below for why each is unavoidable). It's the bump *landing on
`master`* that must wait for 0.1, not the bump itself — the whole point of this design is that it
still requires one.

**SQLite (`main.masks_history`)**: no partial merge is possible. The moment
`CURRENT_DATABASE_VERSION_LIBRARY` reaches `master`, `_upgrade_library_schema_step()` runs the
migration on every user's database the next time they start the app — even if none of the new
write-dedup logic is active yet, this already touches live data (irreversibly, since a build older
than the bump then refuses to open the migrated database — `database.c:3408`,
`db_version > CURRENT_DATABASE_VERSION_LIBRARY`). Schema, read path, and write path stay bundled
on the dedicated branch together; there is nothing here that's safe to land early.

**XMP (`masks_history`/`masks_history_forms` array)**: asymmetric, because `DT_XMP_EXIF_VERSION`
only affects files *written from this point on* — it doesn't touch anything already on disk the
way the DB version counter does. `read_masks_v6()` and the two widened dispatch conditions
(`exif.cc:3284`, `exif.cc:3366`) are inert as long as `DT_XMP_EXIF_VERSION` stays at 5: nothing
produces a v6 sidecar, so that code never runs on `master`. It would be *possible* to land that
read-side code ahead of 0.1 as dormant forward-compat. Default here is to keep it bundled with the
rest anyway — a fragmented merge history buys little for a branch that isn't tracked by any build
in the meantime — but note the option in case an early partial merge is ever useful.

Both are developed and merged on a **dedicated branch**, kept out of `master` until the project's
version is actually bumped to 0.1 (current tag: `v0.0.0`). Enforced by merge discipline (process,
not code): no compile-time/`PROJECT_VERSION` guard is added, since the branch holding this work
simply never gets merged to `master` before that point.

## Design

### Why one table instead of two

An earlier version of this design split `masks_history` into a deduplicated content table
(`masks_history_forms`) plus a thin per-step reference table. That works, but it means: a new
table, a `FOREIGN KEY`, and — critically — a migration that does `INSERT ... SELECT DISTINCT`
data transformation across every existing user's `masks_history` rows the moment the version
bumps.

There is no way to avoid the version bump itself: every schema change to an already-shipped table
in this codebase goes through the version-gated `_upgrade_library_schema_step()`
(`src/common/database.c:482`, `else if(version == N) { ...; new_version = N+1; }`), which forces
`CURRENT_DATABASE_VERSION_LIBRARY` (currently 36, `database.c:84`) to bump. The unconditional
`ALTER TABLE ... ADD COLUMN` calls seen elsewhere in `database.c` (e.g. around line 163-310) are
not a general-purpose escape hatch — they only exist inside `_migrate_schema()`, a one-time bridge
for pre-versioning darktable databases, not a pattern available for ongoing schema evolution.

But the bump doesn't have to carry a data migration. Reusing `masks_history` **in place**, via a
single self-referencing nullable column, needs no data transform at all: every existing row simply
gets `content_ref = NULL`, which is already the correct interpretation ("this row carries its own
content") for historical data. That is strictly safer for the "don't touch live user data"
constraint than a table split, and it's a smaller surface for everything downstream (undo
snapshots, XMP) — see below.

### SQL schema

```sql
ALTER TABLE main.masks_history ADD COLUMN content_ref INTEGER;
```

That's the entire migration. `masks_history` keeps its existing columns and its existing job (one
row per `(history step, formid)` — this already *is* the "pointer per history line" structure).
`content_ref` is nullable and, when set, points to the SQLite `rowid` (implicit, no need for an
explicit `id` column) of an **earlier row in the same table** that carries the real content:

- `content_ref IS NULL` → this row's own `form`/`name`/`version`/`points`/`points_count`/`source`
  are authoritative (either the first time this content appeared, or dedup wasn't attempted for
  it).
- `content_ref = <rowid>` → this row is a pointer; `points`/`points_count`/`source` are left NULL
  (no BLOB duplicated), and the real content lives at the referenced row (which is itself
  guaranteed to have `content_ref IS NULL` — see write path below, dedup always resolves to the
  root, never chains through another pointer).

Migration: new `else if(version == 36)` block in `_upgrade_library_schema_step`
(`src/common/database.c:482`), bumping `CURRENT_DATABASE_VERSION_LIBRARY` 36 → 37. One
`ALTER TABLE`, no `INSERT ... SELECT`, no new table, no new index required by the schema itself
(see write path for why no content-lookup index is needed either).

`_create_library_schema()` (`database.c:2355`, fresh installs) needs `content_ref` added directly
to its `CREATE TABLE main.masks_history(...)` literal (currently ~line 2423) — same one-column
addition, just written directly instead of via `ALTER TABLE`.

The ephemeral `memory.undo_masks_history` table (`database.c:2546`, recreated on every startup,
**not** subject to the version counter) needs the same column added to its `CREATE TABLE` literal
— no migration needed, and no second mirror table to maintain (unlike the two-table design):
`src/common/history_snapshot.c` (create/restore/clear, ~lines 95/168/228) keeps copying exactly
one table for lighttable undo/redo snapshots, just with one more column along for the ride.

**Trap found during implementation, not anticipated at design time**: `content_ref` is a `rowid`
into `main.masks_history` itself. A plain `INSERT ... SELECT` does **not** preserve rowids — the
destination gets fresh, auto-assigned ones — so a naive column-for-column copy of `content_ref`
into `memory.undo_masks_history` and back would, on restore, point every dedup row at whatever
unrelated row later happened to claim that old rowid value, not at its actual sibling. Fixed by
also capturing the row's own `rowid` into a new `memory.undo_masks_history.orig_rowid` column at
snapshot-create time, then restoring with an **explicit column list that includes `rowid`**
(`INSERT INTO main.masks_history (rowid, imgid, ...) SELECT orig_rowid, imgid, ... FROM
memory.undo_masks_history ...` — `main.masks_history` has no declared `INTEGER PRIMARY KEY` alias
for it, but the bare `rowid` pseudo-column can always be read and written explicitly). This is
safe to reuse because `dt_history_delete_on_image_ext()` already cleared this image's own rows
before the restore INSERT runs, and SQLite's default rowid allocator never reissues a value once
used elsewhere in the table (barring the table going fully empty, which does not have a real
codepath here since some image's masks_history row almost always still exists). Cross-image paths
(`dt_history_copy_and_paste_on_image`, `dt_masks_copy_used_forms_for_module`) don't have this
hazard — checked in `src/common/history_merge.c`, they route through in-memory
`dt_masks_form_t*`/`dev->forms` objects and call `dt_dev_write_history_ext()` on the destination,
which computes its own fresh `content_ref` values from scratch rather than copying rows or their
rowids across images.

### Write path: dedup by pointer identity, not by BLOB comparison

The naive way to detect "is this content already stored?" is a SQL lookup comparing BLOB columns
(what the two-table design's `UNIQUE` constraint did implicitly). That works but needs an index
over BLOB columns and a query per form per commit.

There's a cheaper option that falls out of work already done: the in-memory COW refactor
(`src/develop/masks/masks_history.{h,c}`, see `CLAUDE.md`) guarantees that when a form is
*unchanged* between two history steps, `hist->forms` for both steps hold the **literal same
`dt_masks_form_t*` pointer** — copy-on-write only clones on an actual mutation. So "did this
form's content change since the last time I wrote it?" is answerable by a pointer comparison, in
memory, for free — no SQL round-trip, no BLOB comparison, no index.

`dt_dev_write_history_ext()` already walks `dev->history` once, in `num` order, to write every
item. Extend that single pass with a local `GHashTable *last_written` (key: `formid`, value:
`{dt_masks_form_t *ptr, sqlite3_int64 rowid}`, scoped to the function call, freed at the end):

- For each form being written at this step: look up `last_written[form->formid]`.
  - If present and `entry->ptr == form` (same object — guaranteed identical content by the COW
    invariant): insert a pointer row (`content_ref = entry->rowid`, points/points_count/source
    NULL). No malloc, no memcpy, no BLOB bind for the points buffer.
  - Otherwise: full write as today (`content_ref = NULL`), then
    `last_written[form->formid] = {form, sqlite3_last_insert_rowid(...)}`.

This only catches the dominant real case (the same shape persisted unchanged across consecutive
commits — exactly what `dt_dev_mask_history_overload()`'s toast is warning about) rather than
every possible content coincidence (e.g. two independently-drawn shapes that happen to end up with
identical points would not be deduped against each other). That trade is fine here: the toast and
the actual DB bloat both come from the *same-form-persisted* case, and this catches all of it,
transitively — if a form is unchanged for 100 steps, steps 2-100 all point at step 1's rowid, they
never chain through each other. It also means no new index and no schema support beyond the one
column: the dedup decision is made entirely in application memory before any SQL runs.

### Read path (in-memory dedup bonus)

`dt_masks_read_masks_history()` (`src/develop/masks/masks.c:2115`): change the query to resolve
`content_ref` via a self-join, ordered by `num` as today:

```sql
SELECT m.imgid, m.formid, m.form, COALESCE(c.form, m.form),
       COALESCE(c.name, m.name), COALESCE(c.version, m.version),
       COALESCE(c.points, m.points), COALESCE(c.points_count, m.points_count),
       COALESCE(c.source, m.source), m.num
FROM main.masks_history m
LEFT JOIN main.masks_history c ON c.rowid = m.content_ref
WHERE m.imgid = ?1
ORDER BY m.num
```

(`form`/`name`/`version` are tiny and could just be duplicated on every row instead of coalesced,
but coalescing costs nothing extra here and keeps every column dedup-consistent.)

Keep a local `GHashTable` (resolved content rowid → `dt_masks_form_t*`) while looping: if a
content rowid was already materialized, call `dt_masks_form_ref()` on the existing object instead
of allocating a new one. This extends the in-memory refactor's benefit to **loading** an image,
not just editing an already-open session — unchanged from the previous version of this design.

### XMP: real size reduction, gated behind a new `xmp_version`

Two options were considered here.

**Rejected: purely additive `mask_content_ref`, no version bump.** Add the attribute to the
existing v3 array and always still write full `mask_points` on every entry, using
`mask_content_ref` only as an optional read-time hint to reuse an already-parsed
`dt_masks_form_t*`. Fully backward/forward compatible in both directions by construction (an
unknown attribute is ignored by RDF; a reader that ignores it just reads `mask_points` as always),
but it does **not** shrink the XMP sidecar — the whole point of dedup for DB writes was avoiding
re-serializing large point BLOBs on every commit, and that specific pressure doesn't exist for
XMP (written far less often, on export/sidecar-write, not on every darkroom parameter tweak). If
XMP file size doesn't actually matter here, this option is simpler and safer; it was dropped only
because the goal below was chosen instead.

**Chosen: bump `DT_XMP_EXIF_VERSION` (`src/common/exif.cc:133`) 5 → 6.** Referencing entries omit
`mask_points`/`mask_nb`/`mask_src` entirely and carry `mask_content_ref` (an index into the same
`Xmp.darktable.masks_history` array) instead — the actual size reduction. This mirrors exactly how
this file already handles real format changes (the iop-order-list rework was v3→v4/v5 the same
way), so it's consistent with existing precedent rather than a new mechanism. The cost: `xmp_version`
is a single counter for the **whole sidecar** (history, iop-order, timestamps — not scoped to
masks), and the final dispatch `else` (`exif.cc:3370-3373`) rejects the **entire file** — all
history included, not just masks — when it doesn't recognize the version:

```cpp
std::cerr << "error: Xmp schema version " << xmp_version << " in " << filename << " not supported" << std::endl;
g_hash_table_destroy(mask_entries);
return 1;
```

So any older Ansel/darktable build (or a downgrade) that encounters a v6 sidecar loses the whole
file's history, not a gracefully-degraded mask. Accepted trade-off for the real size win, but
worth being explicit about since it's a bigger blast radius than "just the masks."

Concrete changes, from an exhaustive grep of every `xmp_version` comparison in `exif.cc` (14
sites — most need no change, listed here for completeness):

- `#define DT_XMP_EXIF_VERSION` (`exif.cc:133`): 5 → 6.
- **Writer**, `dt_set_xmp_dt_history()` masks block (`exif.cc:3609-3669`): extend the `SELECT` to
  include `content_ref, rowid`; keep a local `GHashTable` (DB `rowid` → position already written
  in the XMP array) while looping `ORDER BY num` (this order guarantees a `content_ref` target was
  already visited, since it always points to a row written earlier in the same
  delete+rewrite pass — see the write-path section above). Rows with `content_ref IS NULL` write
  exactly as today and register their array position; rows with `content_ref` set write
  `mask_content_ref` (the referenced row's array position) and omit `mask_points`/`mask_nb`/
  `mask_src`.
- **New `read_masks_v6()`** next to `read_masks_v3()` (~`exif.cc:3018`): resolves
  `mask_content_ref` against entries already parsed earlier in the same array pass and
  materializes full `points`/`nb`/`src` into the returned `mask_entry_t` — same output shape as
  `read_masks_v3()`. This means `add_mask_entry_to_db()` and everything downstream of mask parsing
  need **no changes**: they always receive fully-populated entries, v3 or v6 alike.
- **Mask reader dispatch** (`exif.cc:3329-3332`, currently `if(xmp_version < 3) ... else
  read_masks_v3(...)`): three-way — legacy / `xmp_version >= 6` → `read_masks_v6()` / else →
  `read_masks_v3()`.
- **Two exact-match version lists must be widened to include 6**, or the new code rejects its own
  files:
  - `exif.cc:3284`, `if(xmp_version == 4 || xmp_version == 5)` (iop-order-list handling) → add
    `|| xmp_version == 6`.
  - `exif.cc:3366`, `else if(xmp_version == 2 || xmp_version == 3 || xmp_version == 4 ||
    xmp_version == 5)` (selects `read_history_v2()`) → add `|| xmp_version == 6`. Missing this
    sends v6 files straight into the "not supported" `return 1` at `exif.cc:3370-3373`.
- **No change needed** at the other 8 comparison sites (`exif.cc:3359,3388,3392,3419,3431,3477`:
  all `xmp_version < N` bounds already correctly exclude 6 into the modern branch;
  `exif.cc:3803,3810,3817`: `xmp_version > 5` for timestamps already treats 6 as "current" —
  confirmed by reading each one, not assumed from the pattern.

## Files to touch

- `src/common/database.c` — new `version==36` migration block (single `ALTER TABLE ADD COLUMN`),
  bump `CURRENT_DATABASE_VERSION_LIBRARY`, add the column to `_create_library_schema()`'s
  `masks_history` literal and to `memory.undo_masks_history`'s literal.
- `src/develop/dev_history.c` — `dt_dev_write_history_ext()`: wrap the delete+rewrite loop in
  `dt_database_start_transaction()`/`dt_database_release_transaction()`; add the per-call
  `last_written` hash table and thread it through to the masks write calls (needs `formid` →
  `{ptr, rowid}` visibility at the point `dt_masks_write_masks_history_item()` is called from
  `dt_dev_write_history_item()`).
- `src/develop/masks/masks.c` — `dt_masks_write_masks_history_item()`: accept the dedup decision
  (or the `last_written` table) from the caller, cache its prepared statement like
  `src/common/history.c` does, write pointer rows when applicable. `dt_masks_read_masks_history()`:
  self-join query + read-side `GHashTable` dedup.
- `src/common/history.c` — no schema-shape change needed here (`dt_history_db_delete_masks_history`
  still just deletes by `imgid`, one table, unchanged).
- `src/common/history_snapshot.c` — lighttable undo snapshot create/restore/clear: still one table
  to mirror, one more column.
- `src/common/exif.cc` — bump `DT_XMP_EXIF_VERSION` 5→6; extend `dt_set_xmp_dt_history()`'s masks
  block to resolve/write `content_ref`/`mask_content_ref` and omit content on referencing entries;
  new `read_masks_v6()`; widen the two exact-match version lists at `exif.cc:3284` and
  `exif.cc:3366` to include 6; three-way mask-reader dispatch at `exif.cc:3329-3332`.
- `src/iop/spots.c` — no signature change if the dedup hash table is threaded through
  `dt_dev_write_history_ext()`'s caller rather than made a parameter of
  `dt_masks_write_masks_history_item()` itself; verify in testing only either way.

## Verification plan

**All items below confirmed 2026-08-07**, manual testing on the dedicated branch.

1. On the dedicated branch, against a **copy** of a test database (never a real user DB): run the
   migration, verify existing images still load identically (every pre-existing row has
   `content_ref = NULL`, so this should be a pure no-op read-wise).
2. Write a new commit on a heavily-masked image, confirm `points IS NULL AND content_ref IS NOT
   NULL` rows appear for forms that didn't change, and that reload reconstructs identical
   `dev->forms`. **Confirmed**: shape loading, shape editing, lighttable undo/redo, copying/
   duplicating history to another image, XMP export then reimport, history compression (see the
   deadlock note below). `iop/spots.c` (legacy "spots" compatibility path) intentionally **not**
   tested — the module is deprecated (`deprecated_msg()` points users at retouch), not worth the
   manual-test time for this branch.
   - **Unrelated deadlock found and fixed while testing compression**: compressing history on the
     image open in darkroom hung. Root-caused via gdb to a pre-existing bug in
     `common/dtpthread.h`'s temporary `find_history_mutex_blocker` diagnostic instrumentation —
     `dt_pthread_rwlock_wrlock()`'s named-lock branch never set `writer_depth = 1` on success,
     breaking the same-thread reentrant-writer fast path for `dev->history_mutex` (the only
     currently-named lock) and self-deadlocking `dt_apply_dev_history_update()`'s write-lock-then-
     nested-read-lock sequence. Not caused by this design; fixed on `master` directly (commit
     `16fe4f739f`), independent of the 0.1 gate, since it's a general concurrency bug unrelated to
     masks/history schema.
3. Measure `dt_dev_write_history_ext()` wall time before/after on a heavily-masked image (e.g. the
   140-shape retouch test image used for pipeline performance work) — expect most of the win from
   the transaction-wrapping fix, with the pointer-identity skip mattering most on deep histories
   with large point counts per shape. **Measured 2026-08-07** (`-d history` log,
   `_dt_dev_write_history_job_run`, image 2298, live editing across several threads/commits):
   - Before (pre-dedup code, N+M separate autocommits per commit): 22 samples, 6.93-10.44 ms,
     mean ≈ 8.79 ms.
   - After (transaction-wrapped + pointer-identity dedup): 14 samples, 1.96-4.01 ms, mean ≈
     2.47 ms.
   - ≈ 3.6× faster, matching the diagnosis that the missing transaction was the dominant cost.
4. Confirm XMP files written by older Ansel/darktable versions (`xmp_version` 2-5) still import
   correctly through the unchanged `read_masks_v3()`/legacy paths — regression only, no new code
   involved for those.
5. Round-trip a v6 sidecar: export, reimport, verify `dev->forms` matches what was exported
   (referencing entries correctly resolve through `mask_content_ref`). **Confirmed**: "load
   history from XMP" round-trip with masks works.
6. Confirm the version-rejection path still fires cleanly for a genuinely unknown version (not a
   silent partial read) — write a sidecar with an out-of-range `xmp_version` and check the
   `exif.cc:3370-3373` `return 1` triggers, no crash, no partial DB write. **Confirmed**: sidecar
   edited to `xmp_version=99`, reimport hits the exact expected message ("error: Xmp schema
   version 99 in ... not supported") and surfaces as a clean GTK error dialog
   (`gui/actions/edit.c:338`) instead of a crash or partial write.
7. Merge to `master` only when the project's version is actually bumped to 0.1 — see the hard
   constraint above.
