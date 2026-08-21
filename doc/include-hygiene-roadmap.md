# Include hygiene: findings and roadmap

This file records what the include diagnostics measure, so follow-up work can be planned and
split without re-deriving it. Sections headed **Done** describe work that has landed, kept
for the traps it left behind rather than as a changelog; everything else is backlog.

Current tree, from `tools/include_graph.py --summary`: 742 files, 318 headers, 424
translation units, 0 cycles, 206 layering violations.

---

## 1. Tooling

| tool | answers |
|---|---|
| `tools/include_graph.py --summary` | metrics for before/after diffing (cycles, closures, inversions, `darktable_h_reach`) |
| `tools/include_graph.py --mermaid` | directory-level graph, renderable in a GitHub comment |
| `tools/include_unused.py` | candidate unneeded `#include`s, static pass over the whole tree |
| `tools/include_unused.py --verify` | confirms candidates by actually recompiling without them |
| `tools/include_report.py` | one self-contained HTML page: rankings, blast radius, inversion heat-map, SVG charts, no dependencies |
| `tools/pragma_once_to_guards.py --verify` | fails if `#pragma once` reappears |
| `tools/symbol_coupling.py` | what actually *links* between modules, per object file — see §6 |
| `tools/header_consumers.py` | per includer of a header: what it takes from it vs. what it only forwards |
| `tools/decl_def_audit.py` | declarations whose definition sits outside the matching `.c` — see §16.1 |
| `tools/misplaced_files.py` | files whose only consumers live in one other subsystem |
| `doxygen doc/Doxyfile` | per-file include / included-by / directory graphs, interactive SVG |

### Use Doxygen for drilling in, these tools for ranking

`doc/Doxyfile` already sets `HAVE_DOT`, `INCLUDE_GRAPH`, `INCLUDED_BY_GRAPH`,
`DIRECTORY_GRAPH`, `DOT_IMAGE_FORMAT = svg` and `INTERACTIVE_SVG`. For "what does *this*
header pull, and who pulls it", Doxygen's output is better than anything scripted here and
should be the first stop.

**Caveat before you trust one of those pictures:** `DOT_GRAPH_MAX_NODES` is **100**. No
header's *include* graph exceeds that any more (the largest closure in the tree is 74), but
every *included-by* graph in §3 does — those headers are pulled in by 300–500 files — so
that direction still renders *truncated*, with dot silently dropping nodes and marking the
box red. Raise `DOT_GRAPH_MAX_NODES` (and consider bounding `MAX_DOT_GRAPH_DEPTH`, currently
`0` = unlimited) before concluding anything from a god-header's included-by graph. The
scripts here do not truncate, which is exactly why they are complementary rather than
redundant.

---

## 2. Unneeded includes

`tools/include_unused.py` indexes every project header by the names it *declares*, then
flags any `#include` whose declared names the including file never mentions.

**Current count: 98 candidates across 78 files — 22 in headers, 76 in sources.**

By directory: `common` 25, `imageio` 16, `gui` 12, `iop` 11, `develop` 10, `libs` 10,
`control` 4, `colorprofiles` 2, `pixel` 2, `views` 2, `widgets` 2, `apps` 1, `system` 1.

Most often included without using any of its names:

| count | header |
|---:|---|
| 11 | `control/settings.h` |
| 11 | `develop/imageop.h` |
| 6 | `system/dtpthread.h` |
| 4 | `common/opencl.h` |
| 4 | `common/colorspaces_inline_conversions.h` |
| 4 | `common/module_versioning.h` |

### A candidate is a question, not a verdict

The static pass cannot distinguish "not used" from "used to reach a header that *this*
header includes". Both look identical; only the second breaks the build when removed. That
is why `--verify` exists: it comments the include out, rebuilds that one object with ninja,
and keeps the removal only if it still compiles.

**Measured precision on a verified sample: 26 of 30 candidates (~87%) genuinely removable.**
The four rejects were transitive dependencies — `common/database.c` → `common/file_location.h`
is one that still stands. Budget for roughly one in eight being wrong, and never remove in
bulk without the verify pass.

### Three traps this tool is already defended against, and one it is not

1. **Side-effect headers.** `common/poison.h`, `win/win.h`, `config.h`,
   `external/ThreadSafetyAnalysis.h` and the five X-macro `*_api.h` headers declare nothing
   and are included for what they *do*. They are excluded by name in `SIDE_EFFECT_HEADERS`
   (as is `darktable.h`, whose removal is its own migration); do not "clean" them.
2. **X-macro headers.** `common/module_api.h`, `views/view_api.h`, `libs/lib_api.h` and the
   two `imageio/*_api.h` are re-included several times per TU and expanded *inside struct
   bodies*. They must have no guard and no includes of their own. This was learned the
   expensive way — see `CLAUDE.md`.
3. **Transitive reach**, above.
4. **NOT defended: platform-conditional use.** An include used only under `#ifdef _WIN32` or
   `#ifdef __APPLE__` looks unused on a Linux build and compiles fine without it — then
   breaks that platform's CI. The tool flags files containing any conditional as
   `platform_guarded`, which is coarse. **Any removal in a file with platform conditionals
   must be checked on all three CI targets**, not on a local Linux build. The converse
   mistake — adding an include *inside* such a conditional — is gated by
   `tools/check_conditional_includes.sh` (§15).

   Every cross-platform breakage in this series was this shape, and none was visible on
   Linux:

   | symptom | real cause |
   |---|---|
   | macOS: `dt_util_str_replace` undeclared in `common/opencl.c` | called only inside the `#else` of `#ifndef __APPLE__`; dead code on Linux |
   | MinGW: parse error at a `#define` in `common/points.h` | its only includer that dropped `darktable.h` also dropped the `win/win.h` shim |
   | MinGW: `near` / `grp2` as identifiers | same lost shim; the `#undef` came from `darktable.h` |
   | MinGW: unrelated parse errors | `<immintrin.h>` accidentally placed inside an `extern "C"` block |

   The structural fix applied: the Windows legacy-macro shim lives in `system/macros.h`
   (which `#include`s `win/win.h`), at the bottom of the stack where every TU reaches it,
   instead of riding on the orchestrator that low-level code is supposed to stop including.

### Staging, for whatever is left

Smallest blast radius first, so a mistake is cheap:

1. Leaf sources — `iop/`, `libs/`, `views/`, `gui/`, `apps/`: nothing includes them.
2. `common/`, `develop/`, `control/`, `imageio/`, `colorprofiles/`, `pixel/` sources — these
   are included *by* things, so a removal that changes what they re-export can break
   consumers.
3. **Headers last (22).** Removing an include from a header changes what every consumer
   inherits; expect a second wave of "add the header you actually needed" fixes, exactly like
   the umbrella removal in this series.

---

## 3. God-headers and blast radius

Two different questions, and the answers disagree — which is the point.

**By fan-in** (how many files pull it in transitively, and so rebuild when it changes) the
top is entirely small leaf headers from the bottom layers, which is the healthy shape:

| files rebuilt | drags in | own lines | header |
|---:|---:|---:|---|
| 502 | 0 | 60 | `win/win.h` |
| 500 | 1 | 97 | `system/macros.h` |
| 485 | 2 | 180 | `system/mem_alloc.h` |
| 447 | 0 | 133 | `system/openmp.h` |
| 432 | 4 | 233 | `system/simd.h` |
| 422 | 0 | 630 | `system/dtpthread.h` |
| 407 | 5 | 141 | `pixel/format.h` |
| 396 | 0 | 77 | `common/paths.h` |

A header that 500 files depend on is not a problem if it is 97 lines and drags in one thing.
Touching it costs a full rebuild, but reading it costs nothing and it couples no subsystems.

**Weighted by the header's own size** (fan-in × lines) the real targets appear:

| cost | header |
|---:|---|
| 307,238 | `colorprofiles/colorspaces.h` |
| 281,781 | `common/image.h` |
| 265,860 | `system/dtpthread.h` |
| 227,048 | `common/opencl.h` |
| 213,855 | `develop/pixelpipe_cache.h` |
| 178,619 | `common/colorspaces_inline_conversions.h` |
| 176,400 | `develop/imageop.h` |
| 170,240 | `develop/develop.h` |
| 167,580 | `colorprofiles/profile_types.h` |
| 160,380 | `math/math.h` |

`colorprofiles/colorspaces.h` tops the list because it is 1031 lines and 298 files reach it —
and including it drags in `<lcms2.h>` and `<pthread.h>`, which is the real cost. A file that
only names a `dt_colorspaces_color_profile_type_t` wants `colorprofiles/profile_types.h`
instead: enums only, no lcms2, no lock.

`common/colorspaces_inline_conversions.h` is the largest single header in the tree (1501
lines of `static inline` colour maths) and is still the best candidate for splitting by
colour space, so a module converting Lab↔XYZ does not also parse every RGB primary
transform.

---

## 4. Layering inversions and modularity

`tools/include_graph.py` checks each edge against a declared layer order (the authoritative
list is `LAYERS` in that file): `external`/`win`/`system` (0) → `common`/`math`/
`colorprofiles` (1) → `pixel` (2) → `control` (3) → `gui`/`widgets` (4) → `develop` (5) →
`iop`/`imageio` (6) → `libs`/`views`/`chart` (7) → `app` (9, files directly in `src/`) →
`apps` (10).

**Current: 206 inversions.** The number went *up* during the umbrella removal and that was
not a regression — the inversions always existed, `darktable.h` was hiding them behind one
edge. They are individually attributable now, which is what made them fixable.

| inversion | count | what it usually is |
|---|---:|---|
| `gui/ → develop/` | 38 | GUI reaching into `develop.h` / `imageop.h` / `dev_history.h` |
| `common/ → develop/` | 38 | `common/` code reaching into pipeline/iop types |
| `common/ → control/` | 28 | `common/` calling `dt_control_*` / raising signals |
| `gui/ → views/` | 12 | GUI reaching `views/view.h` |
| `common/ → pixel/` | 10 | 7 of them `common/opencl.c` calling each filter's `*_init_cl_global()` |
| `common/ → imageio/` | 8 | `image.c` / `image_cache.c` / `collection.c` → `imageio_core.h` |
| `control/ → gui/`, `gui/ → libs/` | 7 each | |
| `common/ → views/` | 6 | `act_on.c`, `image.c`, `collection.c` → `views/view.h` |

`common/` is the base layer and accounts for **103 of the 206**; `gui/` for 59. Two
distinguishable causes inside `common/`, each with a different fix, and they must not be
conflated:

1. **Genuine upward calls.** `common/act_on.c` reaching `views/view.h` for
   `dt_view_active_images_*`, `common/collection.c` reading `culling_mode` off the GUI
   struct, and the whole of the `common/ → control/` edge. These are real modularity breaks:
   base-layer code driving the GUI or the control loop. Fix by inverting the dependency
   (callback/signal), not by moving the header — `dt_film_confirm_rmdir_handler_t`,
   `dt_folder_survey_confirm_import_handler_t` and `dt_database_prompt_handler_t` are the
   worked examples.
2. **Type-only dependencies.** `common/histogram.h` → `develop/imageop.h` +
   `develop/pixelpipe.h`, `pixel/nlmeans_core.h` → `iop/iop_api.h` + `develop/develop.h`.
   Often satisfiable with a tag declaration — the same fix that cut
   `iop_profile.h → develop/imageop.h` and removed a cycle as a side effect. Cheapest
   category; do it first.

A third cause is settled and should not be re-opened: **configuration reads are not a
layering problem.** `conf.{c,h}` lives in `common/`, which is where a base-layer concern
belongs, and it used to be the largest single target of the `common/ → control/` edge. What
is left on that edge is genuinely the control loop:

| target | count |
|---|---:|
| `control/control.h` | 16 |
| `control/settings.h` | 4 |
| `control/signal.h` | 3 |
| `control/jobs.h` | 3 |
| `control/jobs/*.h` | 2 |

Confirmed at the linker level too (§6): the symbols `common/` takes from `control/` are
`dt_control_*` / `dt_ctl_*`, with no `dt_conf_*` left among them.

### Recommended order

`(2)` type-only tag declarations → `(1)` genuine inversions case by case. `(1)` is design
work, not mechanical, and should not be batched with the rest.

---

## 5. What to watch so this does not come back

`tools/check_layering.sh` runs in CI (see §15) and already enforces the cycle count as an
absolute zero. The guard check is not wired up and is worth running by hand after any header
churn:

```sh
python3 tools/pragma_once_to_guards.py --verify        # no #pragma once
```

The cycle check is the important one: every cycle in this codebase was created by a trailing
convenience `#include` at the bottom of a header, and each one silently inflated the closure
of every file that touched the subsystem. Catching the first one costs a grep; catching the
sixth costs a week.

---

## 6. Two independent checks, added after the first sweep

### clang-tidy `misc-include-cleaner` corroborates, and covers what our tool cannot

`clang-tidy -p build --checks="-*,misc-include-cleaner"` is an AST-based implementation of
the same idea, written by someone else. Run over the files this branch changed it flagged
**none of the ~690 removals as wrong** — useful corroboration, since a text heuristic
agreeing with itself proves nothing.

It also found **122 further unused includes**, concentrated in a category
`tools/include_unused.py` never looks at: **system `<...>` headers** (`strings.h`,
`config.h`, `memory.h`, `unistd.h`). Our tool only resolves project `"..."` includes. That
is the next easy tranche.

Its other findings are IWYU-strict "no header providing X is directly included" —
`int32_t`, `GList`, `IS_NULL_PTR`, `PATH_MAX` used without including the header that
declares them. That is exactly the rule this series applies to project headers, extended
to system ones; worth doing, but it is a large mechanical change of its own.

### `tools/symbol_coupling.py` — inversions as a work list, not a number

`include_graph.py` measures what a file *reads*. This measures what actually *links*: per
object file, defined vs undefined symbols, resolved to the module that defines them. An
include edge can be an accident; a symbol edge is a call that exists at runtime.

Measured on the current build:

| inversion | symbols |
|---|---:|
| `gui → develop` | 74 |
| `common → develop` | 70 |
| `common → control` | 25 |
| `gui → views` | 16 |
| `common → pixel` | 15 |
| `common → imageio` | 14 |
| `common → gui` | 4 |

`--edge common control` lists the 25, and **not one of them is `dt_conf_*`** any more: they
are `dt_control_*`, `dt_ctl_*`, `dt_toast_log` — the control loop itself. That is the
question §4 left open, settled by the move of `conf.{c,h}` into `common/` and confirmed at
the linker level rather than inferred from include counts. What remains on this edge is
design work, not a relocation.

`common → develop` (70) is the larger and harder one: base-layer code reaching into
pipeline and iop internals. Also design work.

**Two cautions before quoting a number from this tool.** It reads `.o` files, so a build
directory carrying objects from a previous layout reports edges that no longer exist —
delete stale objects, or configure a fresh build tree, before believing an edge. And its
`module_of()` derives the subsystem from the path *after* `<target>.dir/`, which is the
source path only for targets built from `src/` (`lib_ansel`); a module built as its own
target (`src/widgets`, the `libs/`, `views/` and `imageio/` plugins) loses its directory and
is attributed to a bare file name instead.

---

## 7. Hoist the GUI out of the backend

Measured with `tools/symbol_coupling.py` on the current build, counting references from
`common/` and `develop/` objects to symbols defined in `gui/` or `widgets/`. (`control/`
reaches the GUI too — 20 references, 9 of them redraw and cursor requests from `control.c` —
but `control/` sits *below* `gui/`, so those are plain layering inversions and belong to §4.)

`common/` is essentially done — six distinct symbols across five files, all of them narrow:

| file | reaches |
|---|---|
| `common/sentry.c`, `common/telemetry.c`, `common/utility.c` | `gui/screen_metrics.h` (`dt_screen_dpi`, `dt_screen_ppd`, `dt_screen_metrics_probed`) |
| `common/database.c` | `dt_gui_show_standalone_yes_no_dialog` for the schema-migration prompts |
| `common/history.c` | `delete_underscore()` — a string helper that happens to live in `widgets/label.h` |

§14 lists the same thing at *include* level, where it is seven sites rather than five. The
two extra ones are worth understanding, because they show what this measurement cannot see:
`collection.c` and `variables.c` include a GUI header to read one field off a GUI struct,
through `dt_gui_get_global()` / `dt_bauhaus_get_global()` — accessors **declared** in
`gui/application.h` and `widgets/bauhaus.h` but **defined** in `darktable.c`, so the linker
attributes them to the orchestrator and no `common → gui` edge appears. That is the §16.1
decl/def mismatch producing a blind spot in §6's tool. Use both views.

What is left is `develop/`:

| file | references |
|---|---:|
| `develop/blend_gui.c` | 68 |
| `develop/imageop.c` | 51 |
| `develop/masks/masks_gui.c` | 28 |
| `develop/imageop_gui.c` | 12 |
| `develop/masks/{brush,polygon,ellipse,gradient,circle}.c` | 30 |

This splits into three very different jobs, and conflating them is what makes it look
daunting:

1. **Already split by filename, wrong directory.** `blend_gui.c`, `imageop_gui.c` and
   `masks/masks_gui.c` are GUI files sitting in backend directories — 108 of the references.
   No splitting needed, only relocation. The pattern to follow is the one the `common/`
   files went through: `lut_viewer.c` and `import.c` are `gui/`, `history_merge_gui.c` is
   `gui/develop/`, and the `*_gui.c` counterparts of `common/` backends are `gui/common/`.

   Caveat measured, not assumed: these files are not one-directional. `blend_gui.c` is 5108
   lines and makes several hundred backend calls, so relocating it moves an edge rather than
   removing it — it becomes `gui → develop`, which is only an improvement if the layer model
   says so. Simulate each with `include_graph.py --what-if` before moving.

2. **Genuinely mixed, needs splitting.** `develop/imageop.c` makes 51 GUI references inside
   the module that also owns pipeline logic, and the mask shapes (`brush.c`, `polygon.c`,
   `ellipse.c`, `gradient.c`, `circle.c`) draw their own on-canvas handles. This is the real
   work: separate the module API from its widget plumbing.

3. **Legitimate.** `darktable.c` is the orchestrator; initialising the GUI is its job, and
   it sits at layer `app`, above everything. Not a defect.

Note that `widgets/` sits at layer 4 and `develop/` at 5, so these references are not
layering *violations* — the layer model permits them. The reason to move them anyway is
§16: what a Qt port has to rewrite must be identifiable, and a GUI file filed under
`develop/` is not.

The pattern to apply is the one that worked for the two solvers in `math/`: the backend
should not *decide* to show UI. `choleski.h` called `dt_control_log()` on OOM when it
already returned an error code the caller checked — the reporting belonged to the caller,
and the call is gone. Most of what is left is likely to be that shape: a toast, a redraw
request, or a widget update issued from code whose job is to compute and return.

---

## 8. The orchestrator lives at `src/`

`darktable.c` and `darktable.h` sit directly in `src/`, not in `src/common/`. The entry
points live in `apps/ansel*/main.c`.

They are the application root, not a common library: `darktable.h` declares the
`darktable_t` struct that owns every subsystem, and `darktable.c` initialises them. Being
filed under `common/` — the BOTTOM layer — inverted that relationship on paper, and made
the tooling count the orchestrator's legitimate reach into `gui/`, `control/` and
`develop/` as `common/` violating the layer order.

The layer model gains `app` (9), above every module: nothing the orchestrator includes can
be an inversion, because there is nothing above it. Layering violations 253 -> 247, and the
six that disappeared were misclassifications rather than fixes.

It also settles the spelling trap recorded in `CLAUDE.md`: while the header lived in
`src/common/`, files in that directory could spell it `#include "darktable.h"` relative to
themselves, which hid them from audits grepping for `darktable.h`. That spelling is
now the canonical root-relative one.

---

## 9. Directory structure: one module, one job

A round of restructuring, driven by the maintainer, on top of section 8. None of it
changes behaviour; all of it changes what the directory tree asserts.

| move | why |
|---|---|
| `dtgtk/` -> `gui/dtgtk/` | a widget toolkit is GUI; top-level only by history |
| `bauhaus/` -> `widgets/bauhaus.{c,h}` | two files do not earn a directory; it is a stateless widget, so `widgets/` (§16.4) |
| `common/{lut_viewer,import}` -> `gui/`, `common/history_merge_gui` -> `gui/develop/` | GUI by content: 69, 243 and 281 GTK tokens |
| `{cli,cltest,cmstest,generate-cache,chart}` -> `apps/ansel-*` | separate programs, separate build targets |
| `main.c` -> `apps/ansel/main.c` | the entry point joins the other entry points |
| hardware/platform/memory code -> `system/` | see below |

| metric | before | after |
|---|---:|---:|
| layering violations | 247 | 233 |
| include cycles | 0 | 0 |
| files in `common/` | 183 | 162 |

### Three things measurement caught that reading would not have

**`src/chart` was not a program.** Of its 13 files, exactly one compiled -- `chart/common.c`,
pulled into the `channelmixerrgb` IOP. Moving the directory into `apps/` wholesale would
have made a live IOP (layer 6) depend on an app (layer 10). What that file actually held
was 103 lines of projective geometry, so it became `math/homography.{c,h}` and the IOP now
depends on `math/` instead of on a dead tool. The remaining 12 files in `apps/ansel-chart/`
are still built by nothing: that directory has no `CMakeLists.txt`.

**`ppc64le/altivec.h` has zero include sites.** It is an include-PATH shim, activated by
`include_directories(.../system/ppc64le)` in `src/CMakeLists.txt` on that arch, and works by
`#include_next`. A move that only rewrote `#include` lines would have silently disabled it,
on an architecture with no CI job and no local cross toolchain.

**`FILE(GLOB)` hides mistakes.** The source lists are glob *patterns*, so an entry matching
no file is dropped without a configure error. Six had accumulated (`dtgtk/culling.c`,
`common/matrices.c`, ...). The corollary matters more than the cleanup: a path this kind of
refactor gets wrong does not fail at configure time -- it drops out of the build and
surfaces as an undefined reference at link, or not at all.

### Known and deliberate

* `system/` sits at layer 0 and has **zero** upward includes — `dtpthread.h` and `macros.h`
  are themselves in `system/` now, and nothing there includes `common/`. Anything added here
  must keep it that way: a `#include "common/..."` from `system/` is an inversion the ratchet
  reports. `system/memory_arena.c` shows the technique for the awkward case — it does its own
  diagnostics rather than reach up for `common/logging.h`.
* `system/simd.h` overlaps conceptually with `math/matrices.h` and `math/math.h` --
  `dt_mat3x4_mul_vec4` is a matrix op living among load/store primitives, while `math/math.h`
  hand-rolls `__m128` intrinsics instead of using them. Unification is outstanding.

---

## 10. Backlog: subsystems still to be extracted from `common/`

Maintainer's list, each entry verified against the tree rather than taken on trust. Ordered
by how much of `common/` it removes.

| # | proposed home | files | lines | status |
|---|---|---|---:|---|
| 1 | `src/metadata/` | tags, ratings, colorlabels, grouping, gpx, exif.cc, metadata, dng_opcode | 10097 | all still in `common/` |
| 2 | `src/database/` | database, sqliteicu, legacy_presets (+ the `DT_DEBUG_SQLITE3_*` macros in `common/debug.h`) | 6634 | all still in `common/` |
| 3 | `src/caches/` | cache (the core), image_cache, mipmap_cache | 3322 | core + 2 of 3 in `common/`; `develop/pixelpipe_cache.{c,h}` is the third and stays where it is |
| 4 | `src/math/` | curve_tools (1D interpolation) | 902 | still in `common/` |
| 5 | `src/views/` | cups_print — used by the print view | 813 | 5 include sites: `views/print.c`, `views/view.h`, `libs/print_settings.c`, `control/jobs/control_jobs.h`, `common/printing.h` |
| — | `src/system/` | resource_limits, dtpthread | 926 | **done** for `resource_limits.{c,h}` and `dtpthread.h`; `common/dtpthread.c` has not followed |
| — | `src/pixel/` | `lut3d.{c,h}` (distinct from `iop/lut3d.c`) | 368 | **done** — `pixel/lut3d.{c,h}` |
| — | `src/colorprofiles/` | colorspaces, colormatrices, printprof, the transform half of iop_profile | — | **done** — see `src/colorprofiles/README.md` |

`colormatrices.c` is not a maths library despite the name: it is 362 lines of camera
colour-matrix presets, `#include`d **as data** by `iop/colorin.c` and
`colorprofiles/colorspaces.c`. It went to `colorprofiles/` with the rest of them, not to
`math/`.

`iop_profile` is deliberately **two** files with the same basename in two directories, which
is a trap if you go looking for one of them: `colorprofiles/iop_profile.{c,h}` is the
transform engine (applying a profile to pixels, SIMD and OpenCL, layer 1);
`develop/iop_profile.{c,h}` resolves *which* profile a module or pipe should use and takes
`develop/` types throughout (layer 5). Both are built. `iop_order.{c,h}` stays in `develop/`.

### Do NOT judge these by the layering counter

Simulated with `--what-if` (which reports its own baseline — read the delta, not the
absolute, since it classifies `src/`-level files differently from `--summary`):

```
curve_tools     -> math/     (+0)
cups_print      -> views/    (+1)
dtpthread.c     -> system/   (+2)
```

None of them helps, and two make it worse -- `system/` sits BELOW `common/`, so moving a
file down there turns its existing `common/` includes into upward edges.

That is not an argument against the moves. It means the layering metric has largely done its
job (315 -> 206 over this series) and the remaining work is organisational legibility:
`common/` should stop being the drawer everything shared gets put in. Judge these by what
leaves `common/` -- roughly 21800 lines across the five groups left -- not by the counter.

---

## 11. Done: the redraw throttle moved to the history commit

The throttle used to sit in the widget: `bauhaus.c` deferred its own `value-changed` emission
so that scrolling a slider or combobox did not trigger a pipeline recompute per step, and
every other widget wanting the same behaviour had to reimplement it.

It belongs at the **history-commit bottleneck** instead (`develop/gui_throttle.{c,h}`), where
it serves every widget without any of them knowing it exists.

### Confirmed enabler

`dt_dev_add_history_item_ext()` **already reuses the last history entry for the same module**
(see `add_new_pipe_node` in `dev_history.c` and the "history entry reused" log). Consecutive
edits of one module therefore coalesce into a single undo step *today*. Widgets can commit on
every step without spamming the undo stack — only the pipeline recompute needs throttling.

### The design: a queue, processed in order

Each `dt_dev_add_history_item_real()` call enqueues its resync request and returns. The queue
drains in FIFO order on the throttle's schedule.

**Do NOT merge queued requests.** An earlier draft of this plan proposed collapsing N pending
requests into one that takes the union of their `add_new_pipe_node` / `has_forms` /
`has_raster` flags. That is the wrong shape: masks, module enable/disable, the mask manager
and ordinary parameter edits all commit history with different resync needs, and any merging
rule is a place to silently drop one. Keep every request, process them in order, stay boring.

### The hazard that must be handled deliberately

`CLAUDE.md` documents that darkroom `leave()` must join the worker before tearing down pipe
state, because `dev->exit` / `pipe->shutdown` do not preempt it. **A queued resync draining
after `leave()` is exactly that bug** — it would touch freed `dev->iop` / `pipe->nodes` and
crash somewhere unrelated, as Sentry issue 133807805 did. `leave()` needs an explicit
flush-or-drop of the pending queue, written as part of this change rather than discovered
afterwards.

### What the first attempt got wrong

The first implementation deferred **only the resync** and kept the commit synchronous,
on the theory that the commit was cheap because `dt_dev_add_history_item_ext()` reuses the
last entry for the same module. Tested, it failed on both counts:

* **The GUI lagged behind the scroll.** The commit runs on the GUI thread, so blocking in
  it once per scroll tick starves the widget's own `gtk_widget_queue_draw()`. The value the
  user is scrolling could not repaint.
* **The queue filled with transient values and hung.** Every intermediate step of a gesture
  became a queued request. Nothing was merged, as required -- but every one of them still
  ran a full commit.

The measurement that was right: the resync calls themselves are only an OR onto
`pipe->changed` plus an atomic store, and `dt_dev_write_history()` already coalesces. The
measurement that was **missing**: everything else in a commit -- an undo record, the
`history_mutex` writer lock, a full history rehash, an image-cache write, a masks-list
rebuild -- is not cheap, and none of it coalesces. Deferring the resync alone deferred the
one part that was already free.

### The design that followed

Defer the **whole** commit, with two rules that must not be confused:

1. **Nothing is merged.** As before, and for the same reason.
2. **A repeat of the pending tail is not a new request.** If the tail of the queue is
   already "commit this module, this enable state", asking for exactly that again adds
   nothing: the queued request reads `module->params` when it drains, so it necessarily
   commits the newest value. Suppression is against the **tail only** -- never a search of
   the queue, never a combination of two different requests -- so ordering is exact. This is
   what stops a 300-tick scroll from queueing 300 commits. A transient intermediate value of
   an ongoing gesture is not a history event, which is the thing the old per-widget throttle
   knew and the first attempt threw away.

`dt_dev_add_history_item_ext()` is not throttled and never was, so everything that must land
before the next statement (bulk history loads, style application, image duplication) is
unaffected -- it already goes through `_ext`.

`dt_dev_history_flush_pending_commits()` **runs** the pending requests at darkroom `leave()`,
before any teardown: a pending request is the user's last edit, and dropping it would lose
the value they left a slider on. `dt_dev_history_drop_pending_commits()` is the last-resort
counterpart for a `dev` already too far gone.

### Scope this actually reached

Bigger than "bauhaus": the same throttle-my-own-commit dance existed in **nine IOP modules**
with curve or graph editors, wrapping a one-line helper (`dt_iop_throttled_history_update`,
now deleted) plus matching cancels in `gui_update()`/`gui_cleanup()`. All gone. Separating
history throttling from GUI throttling in `colorequal` exposed a real bug: `gui_cleanup()`
cancelled the history task and not the `&g->viewer_lut` one, so a queued LUT rebuild could
fire after the `gui_data` it reads was freed.

`gui_throttle.{c,h}` then had no consumer under `widgets/` at all, and half of it is a
rolling average of pixelpipe render times -- application state that directory forbids. It
moved to `develop/`.

### Verification this still needs

Not a compile, and not the metrics: realtime slider drags, combobox scrolling, mask edits,
and darkroom exit under load.

### Left deliberately alone

`views/darkroom.c`'s mask-edit throttle (`_delayed_history_commit` on `dev`). It does not
merely defer a commit: it defers `dt_dev_masks_update_hash()`, a scan over every form, and
only commits if that scan reports a change. Removing it would run that scan on every
mouse-motion event of a mask drag. Different mechanism, different cost, no request behind
touching it.


## 12. The last GUI calls in `common/`

Two of the three remaining files are done (PR after #1106):

* **`history_actions.c`** — a *relocation*. Three of its functions asked the user something
  (`dt_history_copy_parts`, `dt_history_paste_parts_prepare`, `delete_history_callback`) and
  were the only reason it included any `gui/` header at all. Both callers already live above
  this layer, so no handler slot was needed; the GUI half is `gui/common/history_actions_gui.c`.
* **`folder_survey.c`** — one relocation and one *inversion*. The resume-at-startup prompt is
  GUI orchestration end to end and moved whole; the pending-import prompt is interleaved with
  private session state under the survey lock, so it goes through a handler registered from
  `dt_gui_gtk_init()`, next to the film and collection ones.

### `common/database.c` — done

Three dialogs (~120 lines, 88 GTK tokens), all inside `dt_database_init()`: "database is
read-only", and two "error opening database" variants offering to restore a snapshot or
delete and start over.

They were left out of the `dt_database_take_error()` inversion in #1103 for a concrete
reason: that pattern has the backend *record* an error for the caller to report afterwards,
and these are **interactive mid-init** — the answer decides whether init aborts, restores or
starts over, so there is no "afterwards" to report to. The backend therefore states the
question and takes back a value: `dt_database_prompt_t` in, `dt_database_response_t` out.

Registration cannot go where the film/collection/folder-survey handlers go:

```
darktable.c:1239   gtk_init()              <- GTK is up
darktable.c:1260   dt_database_init()      <- the prompts happen here
darktable.c:1403   dt_gui_gtk_init()       <- too late to register from
```

It goes in `darktable.c` between the first two, guarded by `init_gui` — legitimate, since
that is the only thing which knows this early whether there will be anybody to ask.

With no handler every prompt answers `CLOSE`. That is not a fallback that guesses: a corrupt
database is not deleted or restored on the strength of a question nobody was asked. It also
closes a latent headless bug — these dialogs had **no `has_gui` guard at all**, so a run
without a GUI reached `gtk_dialog_new_with_buttons()` on a GTK that `ansel-cli` never
initialises. Registration is the guard now.

`common/database.c` is down to zero GTK tokens. Its `legacy_presets` include was a different
problem (preset migration, not a dialog) and got its own pass — see §14.

## 13. Done: `history_merge.c` stops calling the GUI

The pair lives at `develop/history_merge.{c,h}` (backend) and
`gui/develop/history_merge_gui.{c,h}` (GUI). The backend's include of the GUI header was two
problems wearing one include.

**Misplaced ownership.** `_hm_make_node_id()`, `_hm_id_to_op_name()` and
`_hm_build_last_history_by_id()` are *defined in* `history_merge.c` but were *declared in*
the GUI header — so the backend included a GUI header to see its own functions. The
declarations moved to `history_merge.h`; the GUI half includes that.

**Four real GUI calls**, now handler slots:

| call | sites |
|---|---|
| `_hm_show_merge_report_popup()` | 3 |
| `_hm_ask_user_constraints_choice()` | 1 |
| `_hm_warn_missing_raster_producers()` | 1 |
| `_hm_show_toposort_cycle_popup()` | 1 |

Each took the handler-slot pattern already used by `dt_film_confirm_rmdir_handler`,
`dt_folder_survey_confirm_import_handler_t` and `dt_database_prompt_handler_t`. Note the
shapes differ: the report popup and the two warnings are one-way notifications (`void`,
no-op with no handler), while `_hm_ask_user_constraints_choice()` returns a
`dt_hm_constraint_choice_t` the merge algorithm branches on — so that one needs a defined
no-handler answer, chosen the same way as the database prompts: the conservative option that
does not silently discard the user's history.

### Done — but not the way this section first said to do it

Two earlier attempts, and the advice written here after them, were wrong in the same way,
and it is worth recording because the wrong answer was the plausible one.

`_hm_collect_labels_from_history_map()` is defined in the GUI half and was *called from* the
backend, so the include could not go. It contains no GTK, which
made it look like a backend helper stranded on the GUI side, and this section said to move
it — with `_hm_clean_module_name()`, `_hm_module_row_label()`, `_hm_label_t` and
`_hm_label_cmp()` behind it — down into `common/`.

**That would have been a new violation, not a fix.** `_hm_clean_module_name()` calls
`dt_capitalize_label()`, which lives in `widgets/widget_style.h` — layer 4. The whole chain
is *presentation*: it builds the display strings the merge report's rows are made of. "No
GTK in it" is not the same as "belongs below the GUI", and taking the first for the second
is what cost two broken builds.

The actual defect was one level up. `_hm_backup_dest()` snapshots the destination before a
merge, and among that snapshot it captured `orig_labels` / `orig_styles` — **ready-made
display strings, held by the backend purely so a dialog could show them later**. They were
passed straight through to the report popup and used nowhere else.

So the fix was to delete them from `_hm_dest_backup_t` entirely. The report derives them
itself now, at report time, from `orig_ids` (the pre-merge module map, which is genuine
backend data and was already being passed to it). That shortened the report signature by two
parameters, and left nothing in `history_merge.c` needing anything from the GUI header.

The four handler slots then went in as planned — `dt_hm_set_{constraints_choice,
missing_raster,toposort_cycle,merge_report}_handler()`, registered from
`dt_gui_gtk_init()`, with the no-handler defaults described above — and the GUI include left
the backend. The `dt_hm_constraint_choice_t` enum moved to `history_merge.h`, since the merge
algorithm is what branches on it.

## 14. Done: the last `gui/` includes leave `common/`

Four things, found by pulling on the one thread §12 left dangling.

### `gui/legacy_presets.h` was a database migration in the wrong directory

A **1144-line header** holding ~1100 lines of SQL string literals, plus the function that
runs them, so every translation unit including it got a private copy of the array. Presets
are a GUI concept, which is presumably how it landed in `gui/`; creating a table of them
from hard-coded SQL is a database migration and nothing else. No GUI code ever included it —
`common/database.c` did, and was its only consumer.

Now `common/legacy_presets.{c,h}`, with the data in the `.c` and one declaration in the `.h`.

### …and it never committed its transaction

The loop was bounded by a hand-maintained `static const int num_sql_lines = 99;` against an
array of **100** elements. The last one is `"COMMIT"`. So every statement ran inside the
transaction opened by the leading `"BEGIN TRANSACTION"`, which was then left dangling on the
connection for whatever came next to commit or roll back.

Bounded by `G_N_ELEMENTS(sql_lines)` now, which fixes it and makes the class of bug
unrepresentable. Worth noting the shape: a hand-maintained count next to the array it counts
is a bug waiting for someone to append an element.

### `common/metadata.h` included `gui/gtk.h` and used no GTK at all

A dead include **in a header**, so it propagated `gui/gtk.h` into all 16 of its includers.
It was, however, where several of them were getting `<glib.h>` and `<stdint.h>` from —
including `metadata.h` itself, for its own declarations. Those are declared directly now.

This is the argument for the layering ratchet in §15 rather than for the unused-include gate:
nothing about this include was visible at the point of use, it survived every previous audit
in this series because the audits grepped `.c` files, and clang-tidy structurally cannot see
headers (§15, "The gap, stated plainly").

### `dt_gui_gtk_t.selection_stacked` was selection state parked on the GUI struct

Removing the above exposed `common/selection.c` reaching for `dt_gui_get_global()` — not for
anything GUI, but to read and write a flag of its own that happened to live on
`dt_gui_gtk_t`. Three touch points, no GUI code among them. It is `dt_selection_t.stacked`
now, and the field is gone from `dt_gui_gtk_t` (same treatment as `has_scroll_focus`).

Note this also corrects the claim in #1109 that `selection.c`'s `gui/gtk.h` include was
"dead": it was *redundant* — `dt_gui_get_global()` was arriving through
`metadata.h` → `gui/gtk.h` — not unused.

### Where that leaves it

`common/*.c` now reaches `gui/` and `widgets/` in seven places, and every one is narrow
enough to name:

| file | includes | for |
|---|---|---|
| `sentry.c`, `telemetry.c`, `utility.c` | `gui/screen_metrics.h` | screen DPI / PPD |
| `collection.c` | `gui/application.h` | `dt_gui_get_global()->culling_mode` |
| `database.c` | `widgets/dialog.h` | the standalone yes/no dialog for two schema-migration prompts |
| `history.c` | `widgets/label.h` | `delete_underscore()`, a string helper that happens to live there |
| `variables.c` | `widgets/bauhaus.h` | `dt_bauhaus_get_global()->colorlabels` |

Two of those — `collection.c` and `variables.c` — are a *state* read, not a GUI call: the
field lives on a GUI struct because that is where it was parked. Same shape as
`selection_stacked` above, same fix. See §7 for the symbol-level view and for why those two
are invisible to it.

## 15. CI gates: what each one can and cannot see

Three checks, because none of them covers the others' cases.

### `tools/check_layering.sh` — a ratchet on the include graph

Layering violations may fall, never rise; cycles must stay at zero. Baseline in
`tools/include_baseline.txt`, updated with `--update` when the number improves.

A ratchet rather than a threshold because the tree still carries a couple of hundred
inherited violations: demanding zero would mean the check gets switched off. "No worse than
yesterday" costs nothing to comply with and cannot be quietly eroded. Cycles are *not*
ratcheted — the explicit include guards this repository uses instead of `#pragma once` exist
precisely so a cycle is a hard error, and a baseline there would hand that back.

Verified by injecting an upward include (`#include "develop/imageop.h"`) into
`common/image_extensions.h`: +1, exit 1, restored → exit 0. And again in the other direction,
unplanned: rebasing onto a master that had gained two merged layering PRs made the check fail
with *fell 220 → 217*, which is the ratchet working — an improvement that is not recorded is
an improvement the next regression gets to spend. That is why the baseline file is committed
and why `--update` must be run whenever the number drops.

### `tools/check_unused_includes.sh` — clang-tidy on the diff

`misc-include-cleaner`, filtered to the "is not used directly" half and run only on the `.c`
files a pull request touches.

**Filtered**, because the other half ("no header providing X is directly included") is
unusable on a glib/GTK codebase: include-cleaner attributes `g_strrstr()` to
`glib/gstrfuncs.h` rather than to the `<glib.h>` umbrella everyone includes. Measured 46
warnings on one file, 45 of them that half. `IgnoreHeaders` in `.clang-tidy` covers the
umbrella and system headers for the same reason.

**On the diff**, because the measured density is ~0.78 unused includes per translation unit —
several hundred tree-wide. Gating the diff means every file anyone touches comes out clean,
with no baseline file to drift.

### `tools/check_conditional_includes.sh` — includes added inside a platform `#ifdef`

The §2 trap 4 case, gated. An `#include` added inside `#ifdef _WIN32` / `__APPLE__` /
`GDK_WINDOWING_WAYLAND` compiles on whichever machine defines that macro and fails everywhere
else, hundreds of lines from the include, and neither of the other two checks looks at
reachability. Pull requests only, and only for includes the diff *adds* where the header is
not already included unconditionally in the same file.

**It compares `<base-ref>..HEAD`, so uncommitted work is invisible to it.** Running it on a
dirty tree and reading "OK" as approval is the documented way to get a green local run and a
red CI matrix.

### The gap, stated plainly

**The unused-include check cannot see headers, and that is not a configuration mistake.**
include-cleaner analyses the symbols referenced by a translation unit's *main file*; a header
is not one. `--header-filter` does not help — measured, it selects which files' diagnostics
are printed, not which are analysed, and reports the `.c`'s unused includes while saying
nothing about the `.h`. Compiling the header as a synthetic translation unit is worse: that
unit references nothing, so every one of the header's includes comes out "unused".

This matters because **the case that motivated these checks is exactly the case this one
cannot see** — a header including a GUI header it uses no symbol of, and propagating it to
every consumer (§14). The layering ratchet is what catches that class, which is why they all
exist.

## 16. Backlog, and the end goal it serves

### Why any of this

**Ansel intends to move from GTK to Qt.** That is what the whole series is for, and it sets
the bar for "done":

1. **Backend and frontend entirely decoupled**, so the backend is untouched by a toolkit swap.
2. **Within the frontend, pure-toolkit overlays separated from implementation and config** —
   so what has to be rewritten for Qt is a thin, identifiable layer rather than smeared
   through the application.

There is a second, nearer benefit: **estimating the blast radius of the migration.** GTK
calls still run through `libs/`, `views/`, `iop/` and parts of `develop/`; the split of the
old `gui/gtk.{c,h}` into `widgets/` and `gui/` (§16.4) is what makes the toolkit half
countable. Layering is what makes the question answerable at all.

### 16.1 Definitions that live far from their declaration

Find symbols declared in `example.h` but defined somewhere other than `example.c`. These are
maintenance traps: the definition is not where anyone looks for it, and nothing warns.

`history_merge.c` already produced two of these (§13) — `_hm_make_node_id()` and friends
declared in the *GUI* header while defined in the backend, and
`_hm_collect_labels_from_history_map()` the other way round. Both were invisible until an
include had to be removed.

`tools/decl_def_audit.py` does this: Universal Ctags rather than a regex, cross-module
mismatches by default and `--all` for the same-directory ones, which are often deliberate.
It is an audit that produces a list to read, not a CI gate — which is why it depends on
`ctags` being on `PATH` and nothing breaks if it is not.

### 16.2 Split `common/` and parts of `develop/` into real modules

Following §10, three named subsystems:

| module | contents |
|---|---|
| `src/database` | everything touching SQLite, and the whole SQLite↔C conversion layer: image metadata rows, edit histories, styles, presets, tags |
| `src/caches` | mipmap, image, pixelpipe caches |
| `src/metadata` | XMP, IPTC, EXIF, ratings, colour labels, tags, titles — i.e. what happens *after* `database` has converted rows to C structures |

The `database`/`metadata` boundary is the one to get right: `database` owns persistence and
the row↔struct conversion, `metadata` owns the meaning of the fields and the sidecar formats.

### 16.3 More `math/` and `pixel/` candidates

Both directories exist and are populated: solvers, vector algebra and curve fitting in
`src/math` (`choleski.h`, `QR_decomp.h`, `svd.h`, `nelder_mead_simplex.h`,
`polar_decomposition.h`, `splines.cpp`, `topological_sort.c`, `homography.{c,h}`); resampling
and generic image filters in `src/pixel` (`interpolation.c`, the guided/bilateral/gaussian
filters, wavelets, `lut3d`). `math/homography.{c,h}`, extracted from what is now
`apps/ansel-chart`, is the pattern for the remaining sweep: find code that belongs in a low
layer rather than where history left it, and check the direction with
`tools/misplaced_files.py` before moving it.

Note the split between the two: `math/` is arithmetic on numbers, `pixel/` is arithmetic on
images. Interpolation is in `pixel/` for that reason, not in `math/`.

### 16.4 Done: the `gtk.{c,h}` god-header is split

Two halves, in two directories:

* **stateless GTK wrappers** — helpers that only wrap toolkit calls and carry no application
  state → `src/widgets`, under the rule in its README: no `darktable.*` globals, no
  `dt_*_get_global()`, no `dt_conf_*`, and no include from `common/`, `develop/`, `control/`,
  `views/`, `libs/` or `imageio/` beyond pure macro headers;
* **implementation** — anything that knows about views, panels, config or `dt_*_get_global()`
  → `src/gui` (`application.{c,h}`, `window_manager.{c,h}`, `screen_metrics.{c,h}`, …).

`gui/gtk.h` no longer exists; every file names what it uses. That was the largest single lever
on the goal above: the `widgets/` half is roughly the part a Qt port must rewrite, and the
`gui/` half is roughly the part that should survive it, so the estimate is now a matter of
counting one directory rather than auditing the tree.
