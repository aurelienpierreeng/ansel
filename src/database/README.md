# `src/database` — the SQLite layer

Everything that speaks SQL belongs here. Nothing else does.

That is the destination, not yet the state of the tree: **270 call sites outside this
directory still hold a raw `sqlite3 *`**, and 31 files still write queries. Both numbers
are ratcheted downwards by `tools/check_module_boundaries.sh`, and this file is the map of
where they are going.

---

## What the module is

| | |
|---|---|
| `database.c/h` | the connection: open, close, schema creation and migration, lock files, maintenance, snapshots, transactions |
| `sql_debug.h` | the checked `DT_DEBUG_SQLITE3_*` wrappers — **scaffolding, counted, deleted at zero** |
| `legacy_presets.c/h` | 1100 lines of pre-auto-apply darktable presets, inserted into `main.legacy_presets` at schema build time |
| `sqliteicu.c/h` | the vendored ICU collation extension, built only when `HAVE_ICU` |
| `image_repository.c/h` | one `dt_image_t` to and from `main.images`; grouping; ratings |
| `colorlabel_repository.c/h` | `main.color_labels` |
| `selection_repository.c/h` | `main.selected_images`, `memory.selected_backup` |
| `history_snapshot_repository.c/h` | the three `memory.undo_*` tables |
| `metadata_repository.c/h` | `main.meta_data` |
| `tag_repository.c/h` | `data.tags`, `main.tagged_images` — partial, see its file comment |
| `location_repository.c/h` | `data.locations` |
| `preset_repository.c/h` | `data.presets` — partial, see its file comment |


`dt_database_t` is defined in `database.c` and declared nowhere. There is one connection,
the module owns it, and no function takes it as an argument.

## The API shape

**CRUDE** on things the caller may own a copy of — a `dt_image_t`, a tag, a style — and
**Lock + Apply** on the connection itself, which the caller may not own at all.

That second half is the whole design. A caller cannot be handed the connection and be
expected to give it back: it will keep it in a local, use it three functions later, and
hold it across a `sqlite3_step` loop. So the connection does not leave, and the work comes
in instead — as a named function per query, grouped into one repository per table family.

```c
/* lifecycle -- one connection, told what it needs at open time */
dt_database_open_result_t dt_database_open(const dt_database_params_t *params);
void      dt_database_close(void);
gboolean  dt_database_is_open(void);

/* policy -- user preferences, read from conf by the orchestrator and pushed in */
void dt_database_set_settings(const dt_database_settings_t *settings);
void dt_database_get_settings(dt_database_settings_t *settings);

/* questions the module must ask -- it states facts, the handler writes the prose */
void dt_database_set_prompt_handler(dt_database_prompt_handler_t handler);
```

### No conf, no debug flags

`database.c` calls neither `dt_conf_*` nor `dt_get_debug_flags()`, and the gate checks it.

- The **maintenance and snapshot policy** (`maintenance_check`,
  `maintenance_freepage_ratio`, `create_snapshot`, `keep_snapshots`) crosses as one
  `dt_database_settings_t`, read from conf by `darktable.c` at startup and again on
  `DT_SIGNAL_PREFERENCES_CHANGE`. Read as one snapshot under a mutex: the GUI thread
  replaces them from the preferences dialog while a maintenance decision may be part-way
  through reading them, and four fields read one at a time can be a mix of old and new.
- The **SQL trace flag** is a session constant, told to `dt_database_open()`. `-d sql`
  behaves exactly as before — measured at 113 trace lines with it and 0 without.
- Nothing here writes conf either. When the XDG migration moves a legacy pre-XDG library
  out of `$HOME`, the module reports the new name through `dt_database_set_renamed_handler()`
  and lets whoever owns the configuration persist it.

### No dialogs

`dt_database_prompt_handler_t` takes a `dt_database_prompt_context_t` of **facts** — the
filename, sqlite's `quick_check` output, how many bytes a VACUUM would reclaim, whether
the user would be asked again at startup or at close. It never passes prose. Composing a
sentence, translating it, and escaping it into markup is `gui/common/database_gui.c`'s
job, because only the handler knows what it will be rendered into.

With no handler registered every prompt answers `CLOSE`. A corrupt database is not deleted
on the strength of a question nobody was asked, and a headless run never reaches GTK.

---

## The lock, and why there is no `dt_database_swap()` yet

`_db_lock` is a private rwlock, taken for reading by the TRANSACTION machinery
(begin/end and their batch variants) and for writing by `dt_database_close()` — so close
waits for open transactions, and for nothing else yet. No per-query read lock exists: a
repository mid-`sqlite3_step` on another thread is invisible to it.

**It does not yet make closing safe, and the module does not pretend it does.** The
handle-escape count is now zero — no call site outside the module holds the raw connection
any more — but the per-query read side of this lock is the remaining piece: until every
repository query runs under it, close must be preceded by stopping the workers, exactly as
shutdown does today.

So the pieces a workspace swap needs are here — one owned connection, a lock with the
right shape, an open/close pair that is symmetric and reentrant — but `dt_database_swap()`
is deliberately **not** implemented. Shipping a function that corrupts memory whenever a
background job happens to be mid-query is worse than not having one. It lands when the
ratchet reaches zero, and the ratchet is what gets it there.

Each repository extracted also has to finalise its prepared statements on close: a
connection cannot be closed out from under a live `sqlite3_stmt`. That is what
`dt_image_repository_cleanup()` is, and every repository needs its equivalent.

---

## Adding a query

**Do not include `database/sql_debug.h` from new code.** Put the query in a repository
here and give it a name:

```c
/* database/tag_repository.h */
gboolean dt_tag_repository_attach(const guint tagid, const int32_t imgid);
```

One repository per table family, named after the tables it owns, holding its own prepared
statements and its own `*_cleanup()`. It knows about rows; it knows nothing about caching,
refcounting, or what the caller intends. `image_repository.c` is the worked example — it
came out of `caches/image_cache.c`, which was an LRU and, in 107 of its lines, also the
only code in the tree that knew the shape of a `main.images` row.

Seven `common/` files, plus `libs/lib.c`, are already at zero SQL and are the pattern to copy: `colorlabels.c`,
`grouping.c`, `selection.c`, `history_snapshot.c`, `metadata.c`, `map_locations.c`,
`presets.c`.

Four things that came up doing those, and will come up again:

- **A function that dispatches on a key can span repositories.** `dt_metadata_get()` takes
  an XMP name, and three of those names are not metadata at all — the rating is in
  `main.images.flags`, the subject in `data.tags`, the colour labels in
  `main.color_labels`. Each read went to the repository owning its table; there is no
  repository that could have answered all four.
- **Escaping is this side of the boundary.** `common/metadata.c` built its insert's VALUES
  clause and quoted each value with `sqlite3_mprintf("%q", …)`. A module that escapes its
  own strings for SQL is still writing SQL, so `dt_metadata_repository_add()` takes rows
  and does the quoting.
- **A query can narrow without deciding.** `map_locations.c` asks which images fall inside
  a shape. SQL answers that completely for an ellipse or a rectangle, and cannot for a
  polygon — so the query bounds the polygon by its box and returns *candidates*, and
  `_is_point_in_polygon()` decides. Geometry is not storage. When a repository function
  returns candidates rather than answers, say so in its doc comment.

- **Sometimes the right shape is a struct.** When both directions touch the whole row --
  `presets.c` exports a preset to a file and imports one back -- a `dt_preset_t` beats a
  set of narrow queries, and it is what deletes helpers like
  `dt_preset_encode(sqlite3_stmt *, int)`. A domain function taking a `sqlite3_stmt` is the
  database leaking a *type*, which is worse than it leaking a query.

**Verifying a move you cannot run.** Geotagging has no headless entry point. Rather than
claim a functional test, extract every query string from the old file and from its new
home and diff them — nine of thirteen were byte-identical and the four that differed were
each an intended change. That is a cheap check and it belongs in every one of these
commits where the runtime path is hard to reach.

The families still to extract, by where their SQL lives today:

| repository | tables | mostly from |
|---|---|---|
| `tag_repository` (extend) | `data.tags`, `main.tagged_images`, `memory.taglist` | `common/tags.c` (258) |
| `preset_repository` (extend) | `data.presets` | `gui/presets.c` (281) |
| `history_repository` | `main.history`, `main.masks_history`, `main.module_order` | `common/history.c` (217) |
| `style_repository` | `data.styles`, `data.style_items` | `common/styles.c` (187) |
| `film_repository` | `main.film_rolls`, `memory.film_folder` | `common/film.c` (103) |
| `collection_repository` | `memory.collected_images` and the query builder | `common/collection.c` (95) |

One of those rows is still a rule violation that predates this work: CLAUDE.md says
`src/libs/` and `src/views/` contain no raw SQL. `libs/lib.c` is done -- it held 141
references and holds none -- leaving `gui/presets.c`.

---

## Layer

`src/database` is **layer 1**, beside `common/`.

It has one include from a higher layer, and it is named in the gate: the v1 → v2 iop-order
schema migration calls `dt_ioppr_get_iop_order_list_version()` to rewrite
`main.history.iop_order`. That migration genuinely needs the module priority table.

The layer question gets interesting once the repositories land: a repository that persists
a `dt_image_t` must know what one is, so the **types** it stores have to sit at or below
it, while the **domain code** that calls it sits above. That is the split the planned
`src/metadata` and `src/history` are for — types and pure C methods below, SQL here, and
the domain modules calling in rather than issuing their own queries.
