# `darktable_t` globals — usage evaluation and dependency-injection migration plan

> **Corrected against `2959bd4ec1` on 2026-09-29.** The audit before that found 15 claims in
> this file wrong of the tree and 11 stale; every one of them was in §3b and §4, which
> described the migration mid-flight and had not been updated as it landed. Both sections are
> now a record of the finished state, with the commit that closed each member. §1 and §2 are
> the untouched **baseline** and are meant to read as history. Re-measure before acting on a
> claim older than the code you are changing, and re-date this line when you do.

Goal (2026-08): the `darktable` global must be dispatched **once** to high-level callers
(views, main loops, job entry points); all internal modules inherit what they need through
**function input arguments**.

§1 and §2 are the **baseline** the migration order was derived from — how each member was
used before any of it landed, measured on the `refactor/strip-darktable-h` branch and matched
as `\bdarktable\.<member>\b` (XMP keys, D-Bus names and `#include` lines excluded). They are
kept as the baseline, not refreshed: §3b is where the tree's current state lives.

## 1. Per-member usage at the baseline (~5,400 total references; top 12 ≈ 81%)

| # | Member | Refs | Files | Kind |
|---|--------|-----:|------:|------|
| 1 | `develop` | 962 | 99 | mutated state |
| 2 | `gui` | 934 | 113 | service handle + mutated state |
| 3 | `signals` | 424 | 90 | service handle (100% already handle-first API) |
| 4 | `db` | 423 | 58 | service handle (83% `dt_database_get(darktable.db)`) |
| 5 | `bauhaus` | 356 | 68 | service handle (65% already a parameter) |
| 6 | `pixelpipe_cache` | 299 | 30 | service handle (accessor exists) |
| 7 | `control` | 291 | 36 | mutated state + mutexes (119 self-refs in control.c) |
| 8 | `unmuted` | 220 | 44 | app-lifetime constant bitmask (102× DT_DEBUG_PERF) |
| 9 | `image_cache` | 220 | 39 | service handle (95% handle-first API) |
| 10 | `view_manager` | 214 | 36 | the dispatcher itself |
| 11 | `opencl` | 211 | 21 | service handle (119 self-refs in opencl.c) |
| 12 | `color_profiles` | 124 | 18 | mixed config + xprofile_lock |

Then: `collection` 97/27, `undo` 91/16, `selection` 88/24, `mipmap_cache` 70/19,
`plugin_threadsafe` 52/7, `lib` 45/11, `num_openmp_threads` 38/20 (accessor exists),
`conf` 37/3 (very nearly encapsulated — the model outcome; one handle-taking call survives,
see §3b), the app-lifetime constants below.

## 2. Structural observations

- **Eight members are already wrapped in handle-first APIs** (`signals`, `db`, `bauhaus`,
  `image_cache`, `selection`, `collection`, `undo`, `mipmap_cache`): the API conversion is
  done; only the *handle source* is still the global. That is ~1,900 references of nearly
  mechanical work.
- **`conf` shows what "done" looks like** for a genuinely global-by-nature service: 37 refs
  in 3 files, every consumer going through `dt_conf_get_*()` free functions with no handle.
  One call still takes the handle (`common/opencl.c:1011`), which is the whole remaining gap.
- **`gui` and `control` are bundles of ~3 sub-services each**, not single dependencies:
  `gui` = the `ui` handle (348 refs — 178 of them just `dt_ui_main_window()`/`dt_ui_center()`)
  + the write-once `accels` registry (177) + scroll/DPI/mouse state (~200). `control` =
  log/toast + progress system + pointer/button state. Treating them atomically is what makes
  them look intractable.
- **The masks precedent** (dev threaded through the masks API) shows that threading a handle
  into an API is only half the job: the core API took `dev` from the start, yet ~131 GUI call
  sites in `develop/masks/masks.c` still fetched it from the global. Call-site conversion is
  the other half, and it is the half that is easy to declare finished while it is not.
- **`darktable.develop` in `iop/`** (54 files, ~330 refs) has a zero-cost seam:
  `dt_iop_module_t.dev` already exists and most files already use `self->dev` elsewhere.

## 3. Migration strategies

- **A — thread through existing args** (`dev`/`pipe`/`self`/`module`): the real injection.
- **B — orchestrator-implemented accessor** (declared by the owning lib, implemented in
  `darktable.c`; the surviving precedent is `dt_get_num_openmp_threads()`, at
  `darktable.c:520`): interim step that already frees lib headers from darktable.h. Most of
  the accessors this plan created have since been **deleted** by Strategy C — B is a staging
  post, and a row that stops there has not finished.
- **C — relocate ownership into the subsystem**: a file-static the subsystem sets at init,
  reached only through its own API. This is the end state for anything the application does
  not need to name; `src/colorprofiles` is the worked example (§3b).

## 3b. Outcome (the migration has landed)

**The plan in §4 is done.** Measured on `2959bd4ec1` (2026-09-29), matched as
`\bdarktable\.<member>\b` with comments and string literals stripped: **~5,400 → 467**
references tree-wide, and **462 of those 467 are a translation unit reading the member it
owns**, or `darktable.c` itself, which allocates the struct.

Five cross-module references remain in the whole tree:

| Site | Member | Why it is still there |
|---|---|---|
| `control/control.c:656,660` (4) | `develop` | the progress bar reads `develop->progress.{total,completed}`; a carrier would mean giving `dt_control_t` a dev |
| `common/opencl.c:1011` (1) | `conf` | `dt_conf_save(darktable.conf)` on OpenCL shutdown — the one `dt_conf_*` call that still takes the handle |

`control/control.c` (16) and `gui/application.c` (3) also read `view_manager` directly. Those
are **deliberate**: they are the two event dispatchers, which is the one place the target
architecture allows resolving the current view.

### What Strategy C actually removed from `darktable_t`

The §4 table's closing note — "`color_profiles` is so far the only member whose state was
actually relocated" — was true when written and has not been true since 2026-08-10. Fifteen
members have left the struct entirely:

| Member(s) | Landed in | Where the state lives now |
|---|---|---|
| `opencl` | `9595da5396` (2026-08-10) | `static dt_opencl_t *_opencl` — `common/opencl.c:131`. `dt_opencl_get_global()` is **gone**, not interim: the API answers questions instead of handing out the struct |
| `image_cache`, `mipmap_cache`, `pixelpipe_cache` | `c889e94dc6`, `157ac60b8f` (2026-08-10) | file-statics in `caches/image_cache.c:81`, `caches/mipmap_cache.c:133`, `caches/pixelpipe_cache.c:99`. All three accessors gone; the API takes no handle — `dt_image_cache_get(imgid, mode)`, `dt_dev_pixelpipe_cache_flush(id)` |
| `db` | `44c7c0d2ad` (2026-08-10) | the member is still declared but has **zero** code references; `dt_database_get_global()` is gone, only `dt_database_get_sqlite3_global()` survives |
| `collection`, `selection` | `3b967590a7` (2026-08-25) | `common/collection.c:136`, `common/selection.c:78` — and created by the GUI that uses them, not by every process |
| `guides` | `abe99e8f43` (2026-08-25) | `static GList *_guides` — `gui/guides.c:47`; even the accessor is file-static |
| `noiseprofile_parser` | `c11f789610` (2026-08-25) | deleted with the eager startup parse; `dt_noiseprofile_get_matching()` parses on first use |
| `color_profiles` | see `doc/colorprofiles.md` | `static dt_colorspaces_t` in `colorprofiles/colorspaces.c`; `dt_colorspaces_get_global()` is `static` there and named by nothing outside |
| `plugin_threadsafe`, `readFile_mutex`, `exiv2_threadsafe`, `database_threadsafe` | `8726c61a2d`, `db6eacb8dd` (2026-08-27) | **deleted, not relocated** — each was found to have no process-wide consumer left. `readFile()` is thread-safe; `database_threadsafe` moved inside `src/database` |
| `utc_tz`, `origin_gdt` | earlier | `common/datetime.c` |

Five CI ratchets hold those closures at zero, each its own section of
`tools/check_module_boundaries.sh`: **3.** colorprofiles, **4.** `common/opencl`,
**5.** `src/caches`, **6.** `src/database`, **7.** `src/metadata`, **8.** `src/history`.
Section **9.** is the one that is *not* closed yet — `src/develop/masks`, see
`doc/masks-history.md`.

### What is still on the struct, and why

`darktable.h:172-231` still declares: `num_openmp_threads`, `unmuted`,
`unmuted_signal_dbg{,_acts}`, `iop`, `iop_order_list`, `iop_order_rules`, `capabilities`,
`conf`, `develop`, `lib`, `view_manager`, `control`, `signals`, `gui`, `bauhaus`, `db`,
`points`, `imageio`, `dbus`, `undo`, `l10n`, the two remaining mutexes, the nine paths,
`start_wtime`, `themes`, `dtresources`, `main_message`.

Every one of them is read by its owner TU and by `darktable.c`, and by nothing else. Under
the **scope rule** below that is the end state for most of them: the harm this migration
targeted is *distant* modules reaching into application state, and a subsystem reading its
own singleton is a different, smaller problem. Relocating the rest would be tidiness, not
decoupling — with one exception worth doing: `control` (127 self-refs) and `gui` (50) are the
two largest, and both are bundles of ~3 sub-services rather than single dependencies, so
relocating either is a split, not a move.

**Scope rule**: a subsystem reading its own singleton (`common/conf.c` → `darktable.conf`,
`control/control.c` → `darktable.control`, `gui/application.c` → `darktable.gui`,
`develop/imageop.c` → `darktable.iop`) is deliberately left. Its correct fix is relocating
ownership into the subsystem, not an accessor indirection — and the five rows above show what
that costs and what it buys.

**Where `develop` stands.** The carrier-based conversion is done everywhere a carrier exists
(`iop/` via `self->dev`, the masks subsystem, `blend_gui.c`); everywhere else goes through
`dt_dev_get_global()`. `libs/` and `views/` call that accessor rather than a carrier on
purpose: they are the **dispatch points** the target architecture allows. What must not
happen is a leaf module reaching for it.

### The three categories, and how to classify the next member

Not every member should end up threaded through arguments. Classifying one *before* touching
it is what avoids double churn:

1. **App-lifetime constants** (paths, timezone, debug mask, thread count) — getters are the
   final answer; threading them adds parameters carrying a value that cannot differ.
2. **Process-wide buses with no per-call context** (`signals`, `conf`) — an accessor or
   free-function API is the final answer, same reason.
3. **Service handles with a natural carrier** (`develop` → `self->dev`) — the real injection
   targets, and for them the interim accessor is *churn*: convert straight to the carrier.

**`pixelpipe_cache` is category 2, not category 3**, and §4 ordered it wrong. One cache serves
**all** pipes, lookups are keyed by a global content hash rather than by pipe, and consumers
legitimately operate across pipes or with no pipe at all — `iop/toneequal.c`'s
`invalidate_luminance_cache()` releases an entry from a function holding only the module
pointer. Carrying the handle on `dt_dev_pixelpipe_t` would have advertised ownership that does
not exist. It ended as a file-static behind a handle-free API: `dt_dev_pixelpipe_cache_flush()`
now takes `(const int id)` alone.

**Generalisation**: decide a member's category from its *ownership semantics*, not from where
a convenient carrier happens to be threaded.

### What the relocation bought, beyond the count

`src/colorprofiles` is the worked example. The accessor stage left the profile list, its
rwlock and its cached `cmsHTRANSFORM`s one dereference from anywhere; relocating the instance
to a file-static and making the accessor `static` is what forced every consumer onto an API,
and the API is where the invariants could finally be stated. Several of the bugs closed on the
way — use-after-free on the cached display transforms, torn reads of the display/soft-proof
settings, an unsynchronised append to the derived-profile memo — were reachable *only* because
the state was shared, and none of them was visible while it was.

## 4. The order it was done in, and the why worth keeping

Every row has landed. The table is kept for the reasoning, which is what the next member of
`darktable_t` needs: row 5 is why a handle with no carrier at its call sites cannot take
Strategy A, row 7 is why a cross-pipe singleton must not pretend to be pipe-owned. Files,
refs and risk are the **baseline** estimates the order was planned from, not current numbers.

| Order | Item | Strategy | Files | Refs | Risk |
|---|---|---|---:|---:|---|
| 0 | path constants, `utc_tz`/`origin_gdt`, `start_wtime`, `dtresources`, startup lists | getters | ~25 | ~370 | none |
| 0b | `unmuted*` | accessor; landed as `dt_get_debug_flags()` in `common/logging.h` | 44 | 240 | none |
| 1 | `develop` in `iop/` | A (`self->dev`) | 54 | ~330 | very low |
| 2 | `image_cache`, `undo`, `selection`, `mipmap_cache` | B→**C done** (all but `undo`) | 98 | ~470 | low |
| 3 | `collection` | B→**C done** | 27 | 97 | low-med (import jobs mutate from workers) |
| 4 | `db` | B, then **C**: connection sealed inside `src/database` | 58 | 423 | medium |
| ~~5~~ | `bauhaus` | accessor — Strategy A infeasible: 71 constructor call sites pass `DT_GUI_MODULE(NULL)`, so there is no module to carry the handle. `widgets/bauhaus.c` itself had zero global refs. **Still open**: ~110 theme-field reads want `dt_bauhaus_theme_*()` getters | 67 | 354 | — |
| 6 | `signals` | B interim + context-sourced macros where `self` exists | 90 | 424 | medium (worker-thread raises) |
| ~~7~~ | `pixelpipe_cache` | **C** — Strategy A rejected, see above (cross-pipe singleton) | 28 | 282 | — |
| 8 | `develop` outside `iop/` | A (darkroom keeps its refs: it IS a dispatch point) | 45 | ~630 | medium |
| 9 | `opencl` | B, then **C**: instance relocated into `common/opencl.c`, accessor deleted | 21 | 211 | medium |
| 10 | `color_profiles` | **C**, and further than planned: the member is deleted, not wrapped | 18 | 124 | med-high |
| 11 | `gui` | B, split by consumer: four narrow accessors (window, center widget, `ui`, `accels`) carry most sites, `dt_gui_get_global()` the remainder. **C not done** | 113 | 934 | low per-site, high volume |
| 12 | `control` | B (`dt_control_get_global()`). The planned 3-way split (log-toast / progress / pointer) was not needed to close the call sites and has not been done. **C not done** | 36 | 291 | high (job-system core) |
| 13 | `view_manager` | B; `control/control.c` and `gui/application.c` keep direct reads, being the event dispatchers | ~16 | ~90 | low |
| 14 | process-wide mutexes | **deleted rather than relocated** — each was found to have no process-wide consumer left | ~15 | ~80 | low |

## 5. App-lifetime constants: getters, not parameters

Written once at startup, never mutated; threading them would add parameters to hundreds of
signatures for a value that provably cannot differ between callers:

- The 9 path members (`datadir`, `sharedir`, `moduledir`, `localedir`, `tmpdir`, `configdir`,
  `cachedir`, `kerneldir`, `progname`) are read only by `common/file_location.c`, which owns
  their init. The 8 directories have interned getters there — `dt_loc_datadir()`,
  `dt_loc_sharedir()`, … in `common/file_location.h`; `progname` has none and is *written*
  once, at `darktable.c:911`, and read nowhere at all — the `progname` identifiers in
  `apps/*/main.c` are unrelated `usage()` parameters. The older `char*,size_t` copy-outs (`dt_loc_get_datadir()`, …) still exist
  and still have most of the callers: an interned getter is the one to reach for in new code,
  but the copy-out is not deprecated and both return the same string.
- `utc_tz`/`origin_gdt` → `dt_datetime_utc_tz()`/`dt_datetime_origin()` in `common/datetime.c`
  (which already owns their init). 5 files.
- `dtresources` → the `dt_get_total_mem()` family, declared in `system/sys_resources.h` and
  implemented in `darktable.c` (Strategy B: the owning lib declares, the orchestrator defines).
- Startup lists (`iop`, `guides`, `themes`, `iop_order_list/rules`): getters. `iop` is the
  exception — `develop/imageop.c` loads and unloads that list, so it reads the member directly
  as its owner rather than through a getter.

That tranche was ~370 refs across ~25 files closed by ~20 trivial getters: the cheapest work
in the whole migration, and the reason to identify it and do it first.
