# `darktable_t` globals — usage evaluation and dependency-injection migration plan

Goal (2026-08): the `darktable` global must be dispatched **once** to high-level callers
(views, main loops, job entry points); all internal modules inherit what they need through
**function input arguments**. This document is the evaluation of how each member is actually
used today, and the migration order derived from it. Counts are from the
`refactor/strip-darktable-h` branch, matched as `\bdarktable\.<member>\b` (XMP keys, D-Bus
names and `#include` lines excluded).

## 1. Per-member usage (top of ~5,400 total references; top 12 ≈ 81%)

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
- **The masks precedent** (dev threaded through the masks API) is half-done by design: the
  core API takes `dev`, but ~131 GUI call sites in masks.c still fetch it from the global —
  call-site conversion is the remaining half.
- **`darktable.develop` in `iop/`** (54 files, ~330 refs) has a zero-cost seam:
  `dt_iop_module_t.dev` already exists and most files already use `self->dev` elsewhere.

## 3. Migration strategies

- **A — thread through existing args** (`dev`/`pipe`/`self`/`module`): the real injection.
- **B — orchestrator-implemented accessor** (declared by the owning lib, implemented in
  `common/darktable.c`; precedent: `dt_pixelpipe_cache_get_global()`,
  `dt_get_num_openmp_threads()`): interim step that already frees lib headers from
  darktable.h.
- **C — pass at init, store in the subsystem's own struct**: for services with a natural
  owner (bauhaus handle on `dt_gui_module_t`, pixelpipe cache handle on `dt_dev_pixelpipe_t`).

## 3b. Progress (updated as the migration lands)

Done, on `refactor/strip-darktable-h`:

| Member | Result | Note |
|---|---|---|
| the 9 path constants | 0 refs outside `file_location.c` | interned `dt_loc_datadir()` family |
| `utc_tz`, `origin_gdt` | 0 outside `datetime.c` | `dt_datetime_utc_tz()`, `dt_datetime_origin()` |
| `unmuted`, `unmuted_signal_dbg*` | 0 outside `darktable.c` | `dt_get_debug_flags()`, `dt_get_signal_debug{,_acts}()` |
| `dtresources`, `start_wtime` | 0 outside `darktable.c` | `dt_get_total_mem()`, `dt_get_start_wtime()`, existing mem getters |
| `num_openmp_threads` | 0 | `dt_get_num_openmp_threads()` (end state: it is a constant) |
| `develop` **in `iop/`** | 289 → 4 | `self->dev`; the 4 are drawlayer's lifetime-decoupled job callback |
| `image_cache`, `mipmap_cache`, `selection`, `undo` | 0 | `dt_*_get_global()` accessors (interim, Strategy B) |
| `collection` | 0 | idem; `libs/tools/filter.c` still reads `->params`/`->tagid` directly — wrap in named API later |
| `db` | 0 | two accessors: `dt_database_get_sqlite3_global()` (353 sites) + `dt_database_get_global()` (56) |
| `signals` | 0 | `dt_control_signal_get_global()` — **end state**, not interim (see below) |
| `develop` **in `develop/blend_gui.c`** | 52 → 0 | `module->dev`/`self->dev`, `data->module->dev`; 4 context-less helpers gained a dev parameter |
| `gui` | 786 → 0 outside owners | split by consumer: `dt_gui_main_window()`/`dt_gui_center_widget()` (153 sites), `dt_gui_get_ui()` (128), `dt_gui_get_accels()` (157), `dt_gui_get_global()` (rest). Sub-accessors implemented in gui/gtk.c, which owns the struct |
| `lib`, `imageio`, `l10n`, `dbus`, `pwstorage`, `points`, `noiseprofile_parser` | 0 | `dt_*_get_global()` — end state (process-wide singletons). Fixed a latent break: `common/points.h`'s inlines dereferenced the global without including darktable.h |
| `opencl`, `color_profiles` | 0 **outside their own TU** | `dt_opencl_get_global()`, `dt_colorspaces_get_global()`; external `->inited` reads now use `dt_opencl_is_inited()`. `opencl.c`/`colorspaces.c` keep direct access — see below |

Member references tree-wide (excluding `darktable.c` and include lines): **~5,400 → ~3,100**.

**Where `develop` now stands (383 refs left, from 962).** The carrier-based conversion is
done everywhere a carrier exists: `iop/` (via `self->dev`, 289 → 4), the masks subsystem, and
`blend_gui.c`. What remains is concentrated in `libs/` (~250, led by masks.c 87 and
histogram.c 84) and `views/` (darkroom.c 38, studio_capture.c 11) — and these are the
**dispatch points** the target architecture explicitly allows: a view or a top-level panel may
resolve the current develop instance; what must not happen is a leaf module reaching for it.
`dt_lib_module_t` carries no dev today, so giving panels one would mean adding it at
lib-init — worth doing only if the goal is to make panels testable against a non-global
develop, not as global-count reduction. The residual in `develop/imageop.c` (19) and
`gui/color_picker_proxy.c` (14) is the genuinely remaining leaf work.

**Scope rule applied to subsystem-owned singletons**: the harm this migration targets is
*distant* modules reaching into application state. A subsystem reading its own singleton
(`opencl.c` → `darktable.opencl`, `colorspaces.c` → `darktable.color_profiles`) is a smaller,
different problem, and its correct fix is **relocating ownership** into the subsystem (a
file-static set at init by the orchestrator), not an accessor indirection. Those owner-internal
references are therefore deliberately left, and the relocation is tracked as follow-up.

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

| Order | Item | Strategy | Files | Refs | Risk |
|---|---|---|---:|---:|---|
| 0 | path constants, `utc_tz`/`origin_gdt`, `start_wtime`, `dtresources`, startup lists | getters | ~25 | ~370 | none |
| 0b | `unmuted*` | inline `dt_is_debug()` accessor (keep hot-path inlining) | 44 | 240 | none |
| 1 | `develop` in `iop/` | A (`self->dev`) | 54 | ~330 | very low |
| 2 | `image_cache`, `undo`, `selection`, `mipmap_cache` | B→C | 98 | ~470 | low |
| 3 | `collection` | B→C | 27 | 97 | low-med (import jobs mutate from workers) |
| 4 | `db` | B (`dt_database_get_global()` collapses 353 sites) | 58 | 423 | medium (transaction rwlock semantics untouched) |
| ~~5~~ | `bauhaus` | **DONE via accessor** — Strategy A infeasible: 71 of the constructor call sites pass `DT_GUI_MODULE(NULL)`, so there is no module to carry the handle. Note bauhaus.c itself had zero global refs (already fully parameterized). Follow-up: ~111 theme-field reads want `dt_bauhaus_theme_*()` getters | 67 | 354 | — |
| 6 | `signals` | B interim + context-sourced macros where `self` exists | 90 | 424 | medium (worker-thread raises) |
| ~~7~~ | `pixelpipe_cache` | **DONE via accessor** — Strategy A rejected, see the §3b correction (cross-pipe singleton) | 28 | 282 | — |
| 8 | `develop` outside `iop/` | A (darkroom keeps its refs: it IS a dispatch point) | 45 | ~630 | medium |
| 9 | `opencl` | C + `dt_opencl_is_available()` for the `inited` flag | 21 | 211 | medium |
| 10 | `color_profiles` | C (display/softproof settings + xprofile_lock move together) | 18 | 124 | med-high |
| 11 | `gui` (3-way split: ui / accels / scroll-state) | A + C | 113 | 934 | low per-site, high volume |
| 12 | `control` (3-way split: log-toast / progress / pointer) | C | 36 | 291 | high (job-system core) |
| 13 | `view_manager` — fix upward references from libs/ and common/ only | A | ~16 | ~90 | low |
| 14 | process-wide mutexes (`plugin_threadsafe`, `exiv2_threadsafe`, `readFile_mutex`, …) | relocate to owning TU as file-static + `dt_<x>_lock()` accessors — threading a process-global mutex through args only makes it easier to pass the wrong one | ~15 | ~80 | low |

## 5. App-lifetime constants: getters, not parameters

Written once at startup, never mutated; threading them would add parameters to hundreds of
signatures for a value that provably cannot differ between callers:

- The 9 path members (`datadir`, `sharedir`, `moduledir`, `localedir`, `tmpdir`, `configdir`,
  `cachedir`, `kerneldir`, `progname`): each already has exactly ONE read site, inside its
  own `dt_loc_get_*()` copy-out in `common/file_location.c`. Replace the `char*,size_t`
  copy-out with `const char *dt_get_datadir(void)`-style interned getters. Blast radius: 2 files.
- `utc_tz`/`origin_gdt` → `dt_datetime_utc_tz()`/`dt_datetime_origin()` in `common/datetime.c`
  (which already owns their init). 5 files.
- `dtresources` is near-constant with 2 getters already; 2 more close it out.
- Startup lists (`iop`, `guides`, `themes`, `iop_order_list/rules`): getters. ~12 files.

Total for §5: ~370 refs across ~25 files with ~20 trivial getters — front-load it.
