# `darktable_t` globals — usage evaluation and dependency-injection migration plan

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
`conf` 37/3 (fully encapsulated — the model outcome), the app-lifetime constants below.

## 2. Structural observations

- **Eight members are already wrapped in handle-first APIs** (`signals`, `db`, `bauhaus`,
  `image_cache`, `selection`, `collection`, `undo`, `mipmap_cache`): the API conversion is
  done; only the *handle source* is still the global. That is ~1,900 references of nearly
  mechanical work.
- **`conf` shows what "done" looks like** for a genuinely global-by-nature service: 37 refs
  in 3 files, every consumer goes through `dt_conf_get_*()` free functions with no handle.
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
  `darktable.c`; precedent: `dt_pixelpipe_cache_get_global()`,
  `dt_get_num_openmp_threads()`): interim step that already frees lib headers from
  darktable.h.
- **C — relocate ownership into the subsystem**: a file-static the subsystem sets at init,
  reached only through its own API. This is the end state for anything the application does
  not need to name; `src/colorprofiles` is the worked example (§3b).

## 3b. Progress (updated as the migration lands)

Done (started on `refactor/strip-darktable-h`, continued on the branches after it):

| Member | Result | Note |
|---|---|---|
| the 9 path constants | 0 refs outside `file_location.c` | interned `dt_loc_datadir()` family |
| `utc_tz`, `origin_gdt` | 0 outside `datetime.c` | `dt_datetime_utc_tz()`, `dt_datetime_origin()` |
| `unmuted`, `unmuted_signal_dbg*` | 0 outside `darktable.c` | `dt_get_debug_flags()`, `dt_get_signal_debug{,_acts}()` |
| `dtresources`, `start_wtime` | 0 outside `darktable.c` | `dt_get_total_mem()`, `dt_get_start_wtime()`, existing mem getters |
| `num_openmp_threads` | 0 | `dt_get_num_openmp_threads()` (end state: it is a constant) |
| `develop` **in `iop/`** | 289 → 0 | `self->dev` |
| `image_cache`, `mipmap_cache`, `selection`, `undo` | 0 | `dt_*_get_global()` accessors (interim, Strategy B) |
| `collection` | 0 | idem; `libs/tools/filter.c` still reads `->params`/`->tagid` directly — wrap in named API later |
| `db` | 0 | two accessors: `dt_database_get_sqlite3_global()` (353 sites) + `dt_database_get_global()` (56) |
| `signals` | 0 | `dt_control_signal_get_global()` — **end state**, not interim (see below) |
| `develop` **in `develop/blend_gui.c`** | 52 → 0 | `module->dev`/`self->dev`, `data->module->dev`; 4 context-less helpers gained a dev parameter |
| `gui` | 786 → 0 outside its owner | split by consumer: `dt_gui_main_window()`/`dt_gui_center_widget()` (148 sites), `dt_gui_get_ui()` (125), `dt_gui_get_accels()` (110), `dt_gui_get_global()` (197). The four sub-accessors are implemented in `gui/application.c`, which owns the struct; `dt_gui_get_global()` is in `darktable.c` with the other whole-member accessors |
| `lib`, `imageio`, `l10n`, `dbus`, `points`, `noiseprofile_parser` | 0 | `dt_*_get_global()` — end state (process-wide singletons). Fixed a latent break: `common/points.h`'s inlines dereferenced the global without including darktable.h |
| `opencl` | 0 **outside `common/opencl.c`** | `dt_opencl_get_global()`; external `->inited` reads use `dt_opencl_is_inited()`. The owner TU keeps direct access — see below |
| `color_profiles` | **member deleted from `darktable_t`** | ownership relocated instead of wrapped: the single `dt_colorspaces_t` is file-static in `colorprofiles/colorspaces.c`, and `dt_colorspaces_get_global()` is `static` there. `dt_colorspaces_t` is named by no header outside `src/colorprofiles/` — see below |
| `develop` **everywhere else** | 4 refs left tree-wide, from 962 | `dt_dev_get_global()` (296 sites) where no carrier exists, `self->dev`/`module->dev` where one does. The 4 are `control/control.c` reading `develop->progress` to draw the progress bar |
| `control` | 0 outside `control/control.c` | `dt_control_get_global()` (122 sites) |
| `bauhaus`, `pixelpipe_cache` | 0 | `dt_bauhaus_get_global()` (348), `dt_pixelpipe_cache_get_global()` (294) — end state for both, see the §3b correction below for the cache |
| `view_manager` | 0 outside the dispatch points | `dt_view_manager_get_global()` (133). The direct reads left are `control/control.c` (13) and `gui/application.c` (3), i.e. the two event dispatchers |
| `iop` (module SO list) | 0 outside `develop/imageop.c` | that TU loads and unloads the list, so it is the owner |
| `guides`, `themes`, `iop_order_list/rules`, `capabilities` | 0 | getters (`dt_gui_get_themes()`, …) |
| process-wide mutexes | 2 refs left | `exiv2_threadsafe` is the only one with direct external callers, both in `common/exif.cc`'s RAII `Lock`. `plugin_threadsafe`, `capabilities_threadsafe`, `readFile_mutex`, `pipeline_threadsafe` and `database_threadsafe` have none |

Member references tree-wide (excluding `darktable.c` and include lines): **~5,400 → ~440** —
and 395 of those 440 are a translation unit reading the member it owns (`control/control.c`
135, `common/opencl.c` 125, `gui/application.c` 65, `common/conf.c` 32,
`common/file_location.c` 25, `develop/imageop.c` 13). 25 are live cross-references; the rest
occur in comments.

**Where `develop` now stands.** The carrier-based conversion is done everywhere a carrier
exists (`iop/` via `self->dev`, the masks subsystem, `blend_gui.c`), and everywhere else goes
through `dt_dev_get_global()`. `libs/` and `views/` call that accessor rather than a carrier
on purpose: they are the **dispatch points** the target architecture allows — a view or a
top-level panel may resolve the current develop instance; what must not happen is a leaf
module reaching for it. `dt_lib_module_t` carries no dev, so giving panels one would mean
adding it at lib-init — worth doing only if the goal is to make panels testable against a
non-global develop, not as global-count reduction.

**Scope rule applied to subsystem-owned singletons**: the harm this migration targets is
*distant* modules reaching into application state. A subsystem reading its own singleton
(`common/opencl.c` → `darktable.opencl`, `control/control.c` → `darktable.control`,
`common/conf.c` → `darktable.conf`, `gui/application.c` → `darktable.gui`) is a smaller,
different problem, and its correct fix is **relocating ownership** into the subsystem (a
file-static set at init by the subsystem itself), not an accessor indirection. Those
owner-internal references are therefore deliberately left.

`src/colorprofiles` is the worked example of that relocation, and shows what it buys. The
accessor stage (`dt_colorspaces_get_global()`, public) left the profile list, its rwlock and
its cached `cmsHTRANSFORM`s one dereference from anywhere; relocating the instance to a
file-static in `colorprofiles/colorspaces.c` and making the accessor `static` is what forced
every consumer onto an API, and the API is where the invariants could finally be stated —
metadata crosses as value copies with no lock, pixel data crosses under
`dt_colorspaces_lock_profiles()`/`_unlock_profiles()` with the transform never leaving the
module. Several of the bugs closed on the way (use-after-free on the cached display
transforms, torn reads of the display/soft-proof settings, an unsynchronised append to the
derived-profile memo) were reachable *only* because the state was shared, and none of them
was visible while it was. `tools/check_module_boundaries.sh` keeps both counts — external
`dt_colorspaces_get_global()` calls, external `xprofile_lock` acquisitions — at zero.

**Refinement learned while migrating**: not every member should end up threaded through
arguments. Three categories have emerged, and classifying a member *before* touching it
avoids double churn:

1. **App-lifetime constants** (paths, timezone, debug mask, thread count) — getters are the
   final answer; threading them would add parameters carrying a value that cannot differ.
2. **Process-wide buses with no per-call context** (`signals`, and `conf` already) — an
   accessor/free-function API is the final answer for the same reason.
3. **Service handles with a natural carrier** (`develop` → `self->dev`) — these are the real injection targets, and for
   them the interim accessor is *churn*: convert them straight to the carrier instead.

**Correction — `pixelpipe_cache` is category 2, not category 3.** §4 below ordered it for
Strategy A (carry the handle on `dt_dev_pixelpipe_t`, which is indeed threaded everywhere).
Examining the semantics instead of the reference counts shows that would advertise ownership
that does not exist:

- one cache serves **all** pipes — `dt_dev_pixelpipe_cache_flush(cache, id)` takes the owning
  pipe id as a *parameter* precisely because entries from many pipes share one cache, and
  lookups are keyed by a global content hash rather than by pipe;
- consumers legitimately operate across pipes, or with no pipe at all: `iop/toneequal.c`'s
  `invalidate_luminance_cache()` releases an entry held in GUI state from a function that has
  only the module pointer, and the GUI peek path reads entries produced by a *different* pipe
  than the caller's.

Cross-pipe callers would have to pick an arbitrary pipe just to reach the shared object, which
is worse than an accessor because it misleads. `dt_pixelpipe_cache_get_global()` is therefore
its **end state**. Generalisation: decide a member's category from its *ownership semantics*,
not from where a convenient carrier happens to be threaded.

## 4. Recommended order

Every row below has had its **call sites** converted — no distant module names any of these
members any more, bar `common/exif.cc`'s two acquisitions of `exiv2_threadsafe` in row 14
(§3b is the record of what each became). What is still open is the second half,
**Strategy C**: `color_profiles` is so far the only member whose state was actually
relocated out of `darktable_t`; `opencl`, `control`, `gui`, `conf`, `view_manager` and the
mutexes still live on the application struct, read directly by their owner TU. So rows
marked `B→C` or `C` below have landed their B half only.

The table is kept for the *why*, which is what the next member of `darktable_t` needs: row 5
is why a handle with no carrier at its call sites cannot take Strategy A, row 7 is why a
cross-pipe singleton must not pretend to be pipe-owned. Files/refs/risk are the baseline
estimates the order was planned from.

| Order | Item | Strategy | Files | Refs | Risk |
|---|---|---|---:|---:|---|
| 0 | path constants, `utc_tz`/`origin_gdt`, `start_wtime`, `dtresources`, startup lists | getters | ~25 | ~370 | none |
| 0b | `unmuted*` | accessor; landed as `dt_get_debug_flags()` in `common/logging.h`, tested as `dt_get_debug_flags() & DT_DEBUG_XXX` | 44 | 240 | none |
| 1 | `develop` in `iop/` | A (`self->dev`) | 54 | ~330 | very low |
| 2 | `image_cache`, `undo`, `selection`, `mipmap_cache` | B→C | 98 | ~470 | low |
| 3 | `collection` | B→C | 27 | 97 | low-med (import jobs mutate from workers) |
| 4 | `db` | B (`dt_database_get_global()` collapses 353 sites) | 58 | 423 | medium (transaction rwlock semantics untouched) |
| ~~5~~ | `bauhaus` | **DONE via accessor** — Strategy A infeasible: 71 of the constructor call sites pass `DT_GUI_MODULE(NULL)`, so there is no module to carry the handle. Note `widgets/bauhaus.c` itself had zero global refs (already fully parameterized). Follow-up, still open: ~110 theme-field reads (`graph_fg` 36, `pango_font_desc` 25, `quad_width` 18, …) want `dt_bauhaus_theme_*()` getters | 67 | 354 | — |
| 6 | `signals` | B interim + context-sourced macros where `self` exists | 90 | 424 | medium (worker-thread raises) |
| ~~7~~ | `pixelpipe_cache` | **DONE via accessor** — Strategy A rejected, see the §3b correction (cross-pipe singleton) | 28 | 282 | — |
| 8 | `develop` outside `iop/` | A (darkroom keeps its refs: it IS a dispatch point) | 45 | ~630 | medium |
| 9 | `opencl` | B for now (`dt_opencl_get_global()`) + `dt_opencl_is_inited()` for the `inited` flag; C — relocating the instance into `common/opencl.c` — is still open | 21 | 211 | medium |
| 10 | `color_profiles` | C, and it went further than planned: the member is deleted, not wrapped. The display/soft-proof settings and `xprofile_lock` did move together, but into a file-static instance behind a CRUDE-metadata / lock-and-apply API, so no caller names the state at all | 18 | 124 | med-high |
| 11 | `gui` | B, split by consumer rather than by sub-service: four narrow accessors (window, center widget, `ui`, `accels`) carry most sites, `dt_gui_get_global()` the remainder | 113 | 934 | low per-site, high volume |
| 12 | `control` | B (`dt_control_get_global()`). The planned 3-way split (log-toast / progress / pointer) was not needed to close the call sites and has not been done | 36 | 291 | high (job-system core) |
| 13 | `view_manager` — fix upward references from libs/ and common/ only | B (`dt_view_manager_get_global()`); `control/control.c` and `gui/application.c` keep direct reads, being the event dispatchers | ~16 | ~90 | low |
| 14 | process-wide mutexes (`plugin_threadsafe`, `exiv2_threadsafe`, `readFile_mutex`, …) | relocate to owning TU as file-static + `dt_<x>_lock()` accessors — threading a process-global mutex through args only makes it easier to pass the wrong one | ~15 | ~80 | low |

## 5. App-lifetime constants: getters, not parameters

Written once at startup, never mutated; threading them would add parameters to hundreds of
signatures for a value that provably cannot differ between callers:

- The 9 path members (`datadir`, `sharedir`, `moduledir`, `localedir`, `tmpdir`, `configdir`,
  `cachedir`, `kerneldir`, `progname`) are read only by `common/file_location.c`, which owns
  their init. The 8 directories have interned getters there — `dt_loc_datadir()`,
  `dt_loc_sharedir()`, … in `common/file_location.h`; `progname` has none and is read only by
  `darktable.c`. The older `char*,size_t` copy-outs (`dt_loc_get_datadir()`, …) still exist
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
