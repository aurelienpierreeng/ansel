# Include hygiene: findings and roadmap

Scope note: **nothing in this document is implemented by the `darktable.h` PR.** That PR is
deliberately limited to removing the orchestrator umbrella and its direct ramifications
(cycle breaking, `#pragma once` removal, the fallout repairs). This file records what the
diagnostics found *afterwards*, so the follow-up work can be planned and split without
re-deriving it. The tooling described here is committed; the changes it suggests are not.

Measured on `refactor/strip-darktable-h` after the DAG landed: 693 files, 292 headers,
401 translation units, 0 cycles.

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
| `doxygen doc/Doxyfile` | per-file include / included-by / directory graphs, interactive SVG |

### Use Doxygen for drilling in, these tools for ranking

`doc/Doxyfile` already sets `HAVE_DOT`, `INCLUDE_GRAPH`, `INCLUDED_BY_GRAPH`,
`DIRECTORY_GRAPH`, `DOT_IMAGE_FORMAT = svg` and `INTERACTIVE_SVG`. For "what does *this*
header pull, and who pulls it", Doxygen's output is better than anything scripted here and
should be the first stop.

**Caveat before you trust one of those pictures:** `DOT_GRAPH_MAX_NODES` is **100**. Every
header this document ranks as a problem has a closure larger than that, so its graph renders
*truncated* — dot silently drops nodes and marks the box red. Raise `DOT_GRAPH_MAX_NODES`
(and consider bounding `MAX_DOT_GRAPH_DEPTH`, currently `0` = unlimited) before concluding
anything from a god-header's graph. The scripts here do not truncate, which is exactly why
they are complementary rather than redundant.

---

## 2. Unneeded includes

`tools/include_unused.py` indexes every project header by the names it *declares*, then
flags any `#include` whose declared names the including file never mentions.

**Current count: 764 candidates across 316 files — 86 in headers, 678 in sources.**

By directory: `iop` 289, `common` 172, `libs` 77, `develop` 64, `views` 43, `gui` 41,
`dtgtk` 27, `control` 23, `imageio` 19.

Most often included without using any of its names:

| count | header |
|---:|---|
| 73 | `control/control.h` |
| 66 | `common/debug.h` |
| 44 | `control/conf.h` |
| 43 | `common/opencl.h` |
| 33 | `gui/gtk.h` |
| 29 | `dtgtk/button.h` |
| 22 | `common/colorspaces_inline_conversions.h` |

### A candidate is a question, not a verdict

The static pass cannot distinguish "not used" from "used to reach a header that *this*
header includes". Both look identical; only the second breaks the build when removed. That
is why `--verify` exists: it comments the include out, rebuilds that one object with ninja,
and keeps the removal only if it still compiles.

**Measured precision on a verified sample: 26 of 30 candidates (~87%) genuinely removable.**
The four rejects were transitive dependencies — e.g. `bauhaus.c` → `gui/color_picker_proxy.h`
and `database.c` → `common/file_location.h`. Budget for roughly one in eight being wrong, and
never remove in bulk without the verify pass.

### Three traps this tool is already defended against, and one it is not

1. **Side-effect headers.** `common/poison.h`, `win/win.h`, `config.h`,
   `external/ThreadSafetyAnalysis.h` and the five X-macro `*_api.h` headers declare nothing
   and are included for what they *do*. They are excluded by name; do not "clean" them.
2. **X-macro headers.** `common/module_api.h`, `views/view_api.h`, `libs/lib_api.h` and the
   two `imageio/*_api.h` are re-included several times per TU and expanded *inside struct
   bodies*. They must have no guard and no includes of their own. This was learned the
   expensive way — see `CLAUDE.md`.
3. **Transitive reach**, above.
4. **NOT defended: platform-conditional use.** An include used only under `#ifdef _WIN32`
   or `#ifdef __APPLE__` looks unused on a Linux build and compiles fine without it — then
   breaks that platform's CI. The tool flags files containing any conditional as
   `platform_guarded`, which is coarse. **Any removal in a file with platform conditionals
   must be checked on all three CI targets**, not on a local Linux build.

   Every cross-platform breakage in this series was this shape, and none was visible on
   Linux:

   | symptom | real cause |
   |---|---|
   | macOS: `dt_util_str_replace` undeclared in `common/opencl.c` | called only inside the `#else` of `#ifndef __APPLE__`; dead code on Linux |
   | MinGW: parse error at a `#define` in `common/points.h` | its only includer that dropped `darktable.h` also dropped the `win/win.h` shim |
   | MinGW: `near` / `grp2` as identifiers | same lost shim; the `#undef` came from `darktable.h` |
   | MinGW: unrelated parse errors | `<immintrin.h>` accidentally placed inside an `extern "C"` block |

   The structural fix applied: the Windows legacy-macro shim moved from `darktable.h`
   to `common/macros.h`, so it sits at the bottom of the stack where every TU reaches it,
   instead of riding on the orchestrator that low-level code is supposed to stop including.

### Suggested staging for the follow-up PR

Smallest blast radius first, so a mistake is cheap:

1. `iop/` sources (289 candidates, leaf modules, nothing includes them).
2. `libs/`, `views/`, `gui/`, `dtgtk/` sources (188).
3. `common/`, `develop/`, `control/`, `imageio/` sources (278) — these are included *by*
   things, so a removal that changes what they re-export can break consumers.
4. **Headers last (86).** Removing an include from a header changes what every consumer
   inherits; expect a second wave of "add the header you actually needed" fixes, exactly like
   the umbrella removal in this series.

---

## 3. God-headers and blast radius

Two different questions, and the answers disagree — which is the point.

**By fan-in** (how many TUs rebuild when it changes) the top is now entirely the small
extracted leaf headers, which is the healthy shape:

| TUs rebuilt | drags in | own lines | header |
|---:|---:|---:|---|
| 373 | 0 | 87 | `common/macros.h` |
| 369 | 1 | 180 | `common/mem_alloc.h` |
| 350 | 0 | 629 | `common/dtpthread.h` |
| 348 | 0 | 133 | `common/openmp.h` |
| 347 | 3 | 233 | `common/simd.h` |
| 337 | 0 | 98 | `common/logging.h` |

A header that 370 TUs depend on is not a problem if it is 87 lines and drags in nothing.
Touching it costs a full rebuild, but reading it costs nothing and it couples no subsystems.

**Weighted by the header's own size** (fan-in × lines) the real targets appear:

| cost | header |
|---:|---|
| 465,310 | `common/colorspaces_inline_conversions.h` |
| 261,248 | `gui/gtk.h` |
| 232,170 | `common/image.h` |
| 220,150 | `common/dtpthread.h` |
| 204,930 | `common/opencl.h` |
| 202,725 | `develop/pixelpipe_cache.h` |
| 171,841 | `develop/imageop.h` |
| 168,530 | `develop/develop.h` |
| 165,830 | `develop/masks.h` |

`colorspaces_inline_conversions.h` is the standout: a large body of `static inline` colour
maths preprocessed into a third of the codebase, and (see §2) included 22 times by files that
use none of it. It is the best single candidate for splitting by colour space, so a module
converting Lab↔XYZ does not also parse every RGB primary transform.

`dtgtk/button.h` deserves a look too: 29 files include it without using it, which usually
means it was once the way to reach something else.

---

## 4. Layering inversions and modularity

`tools/include_graph.py` checks each edge against a declared layer order:
`external`/`win` → `common` → `control` → `gui`/`dtgtk`/`bauhaus` → `develop` →
`iop`/`imageio` → `libs`/`views`/`chart` → `cli`.

**Current: 372 inversions.** This number went *up* during the umbrella removal (from 331) and
that is not a regression — the inversions always existed, `darktable.h` was hiding them behind
one edge. They are now individually attributable.

| inversion | count | what it usually is |
|---|---:|---|
| `common/ → develop/` | 124 | `common/` code reaching into pipeline/iop types |
| `common/ → control/` | 104 | `common/` calling `dt_control_*` / `dt_conf_*` |
| `common/ → gui/` | 32 | `common/` calling `dt_gui_*`, freeze guards, GTK helpers |
| `gui/ → develop/` | 28 | GUI reaching into `develop.h` / `imageop.h` |
| `iop/ → libs/` | 7 | modules reaching into `libs/colorpicker.h` |
| `develop/ → libs/` | 4 | pipeline reaching into `libs/lib.h`, `libs/colorpicker.h` |
| `common/ → iop/`, `common/ → chart/`, `develop/ → iop/` | 2 each | |

`common/` is the base layer and accounts for **260 of the 372**. Three distinguishable causes,
each with a different fix, and they must not be conflated:

1. **Configuration reads.** `common/` calling `dt_conf_get_*`. Arguably `control/conf` is
   mis-layered rather than the callers: configuration is a base-layer concern that happens to
   live in `control/`. It is the single largest target inside `common/ → control/` —
   measured breakdown of that edge:

   | target | count |
   |---|---:|
   | `control/conf.h` | 44 |
   | `control/control.h` | 36 |
   | `control/signal.h` | 9 |
   | `control/jobs.h` + `control/jobs/*.h` | 11 |
   | `control/crawler.h` | 1 |

   So moving `conf` to `common/` erases **44** inversions, not the whole edge — worth doing,
   and worth deciding before touching any caller, but it does not settle `common/ → control/`
   on its own. The `control.h` half (36) is category (2) below: base-layer code driving the
   control loop.
2. **Genuine upward calls.** `common/imageio_module.c` calling `dt_gui_freeze_*`,
   `common/act_on.c` reaching `views/view.h`. These are real modularity breaks: base-layer
   code driving the GUI. Fix by inverting the dependency (callback/signal), not by moving the
   header.
3. **Type-only dependencies.** `common/histogram.h` → `develop/imageop.h`,
   `common/nlmeans_core.h` → `iop/iop_api.h`. Often satisfiable with a tag declaration —
   the same fix that cut `common/iop_profile.h → develop/imageop.h` in this series and
   removed a cycle as a side effect. Cheapest category; do it first.

### Recommended order

`(3)` type-only tag declarations → decide `(1)` conf placement → `(2)` genuine inversions
case by case. `(2)` is design work, not mechanical, and should not be batched with the rest.

---

## 5. What to watch so this does not come back

Add to CI once the follow-up work lands:

```sh
python3 tools/pragma_once_to_guards.py --verify        # no #pragma once
python3 tools/include_graph.py --summary | grep -q '^cycles\s0$'   # still a DAG
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

Measured on the current build (721 objects, 7488 exported symbols, 1023 cross-module edges):

| inversion | symbols |
|---|---:|
| `common → develop` | 95 |
| `common → control` | 53 |
| `gui → develop` | 45 |
| `common → gui` | 36 |
| `dtgtk → develop` | 12 |
| `common → bauhaus` | 12 |

`--edge common control` lists the 53, and they are overwhelmingly **`dt_conf_*`**
(`dt_conf_get_bool`, `dt_conf_get_int`, `dt_conf_key_exists`, `dt_conf_init`, ...). That
settles the question §4 left open: the `common/ → control/` edge is dominated by
configuration reads, so **moving `conf` into `common/` erases most of it** — confirmed at
the linker level, not inferred from include counts.

`common → develop` (95) is the larger and harder one: base-layer code reaching into
pipeline and iop internals. That is genuine design work, not a relocation.

---

## 7. Next: hoist the GUI out of the backend

Measured with `tools/symbol_coupling.py` on the current build. **94 distinct GUI symbols
are reached from `common/` and `develop/`, across 23 backend files.**

| edge | symbols |
|---|---:|
| `develop → gui` | 45 |
| `develop → dtgtk` | 41 |
| `common → gui` | 36 |
| `common → bauhaus` | 12 |
| `common → dtgtk` | 2 |

Where the calls actually are:

| file | GUI calls |
|---|---:|
| `develop/blend_gui.c` | 172 |
| `develop/imageop.c` | 66 |
| `common/lut_viewer.c` | 47 |
| `darktable.c` | 25 |
| `develop/imageop_gui.c` | 17 |
| `develop/masks/masks_gui.c` | 13 |
| `common/history_merge_gui.c` | 12 |
| `common/import.c` | 12 |

This splits into three very different jobs, and conflating them is what makes it look
daunting:

1. **Already split by filename, wrong directory.** `blend_gui.c`, `imageop_gui.c`,
   `masks_gui.c`, `history_merge_gui.c` are GUI files sitting in backend directories —
   214 of the calls. No splitting needed, only relocation. Simulated: moving the two
   `common/` ones (`history_merge_gui`, `lut_viewer`) is **−5** violations.
   `common/lut_viewer.c` is a GUI widget living in `common/` outright.

   Caveat measured, not assumed: these files are not one-directional. `blend_gui.c` also
   makes 308 backend calls in 5103 lines, so relocating it moves an edge rather than
   removing it — it becomes `gui → develop`, which is only an improvement if the layer
   model says so. Simulate each before moving.

2. **Genuinely mixed, needs splitting.** `develop/imageop.c` makes 66 GUI calls inside
   the module that also owns pipeline logic. This is the real work: separate the module
   API from its widget plumbing.

3. **Legitimate.** `darktable.c` is the orchestrator; initialising the GUI is its
   job. Not a defect.

The pattern to apply is the one that worked for the two solvers in `math/`: the backend
should not *decide* to show UI. `choleski.h` called `dt_control_log()` on OOM when it
already returned an error code the caller checked — the reporting belonged to the caller.
Most of the 94 are likely to be that shape: a toast, a redraw request, or a widget
update issued from code whose job is to compute and return.

---

## 8. The orchestrator lives at `src/`

`darktable.c` and `darktable.h` moved out of `src/common/` to `src/`, beside `main.c`.

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
| `bauhaus/` -> `gui/bauhaus.{c,h}` | two files do not earn a directory |
| `common/{lut_viewer,import,history_merge_gui}` -> `gui/` | GUI by content: 69, 243 and 281 GTK tokens |
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
depends on `math/` instead of on a dead tool. The other 11 files are still built by nothing.

**`ppc64le/altivec.h` has zero include sites.** It is an include-PATH shim, activated by
`include_directories(.../ppc64le)` in `src/CMakeLists.txt` on that arch, and works by
`#include_next`. A move that only rewrote `#include` lines would have silently disabled it,
on an architecture with no CI job and no local cross toolchain.

**`FILE(GLOB)` hides mistakes.** The source lists are glob *patterns*, so an entry matching
no file is dropped without a configure error. Six had accumulated (`dtgtk/culling.c`,
`common/matrices.c`, ...). The corollary matters more than the cleanup: a path this kind of
refactor gets wrong does not fail at configure time -- it drops out of the build and
surfaces as an undefined reference at link, or not at all.

### Known and deliberate

* `system/` sits at layer 0 and has four upward includes into `common/`
  (`dtpthread.h`, `macros.h`, `logging.h`). Pre-existing coupling that was invisible while
  both ends lived in `common/`; the +1 in the count is visibility, not regression.
* `common/history_actions.c` (604 lines, 17 GTK tokens) stayed put. It is genuinely mixed,
  and wants splitting rather than relocating.
* `system/simd.h` overlaps conceptually with `math/matrices.h` and `math/math.h` --
  `dt_mat3x4_mul_vec4` is a matrix op living among load/store primitives, while the matrix
  headers hand-roll intrinsics instead of using them. Unification is outstanding.

---

## 10. Backlog: subsystems still to be extracted from `common/`

Maintainer's list, each entry verified against the tree rather than taken on trust. Ordered
by how much of `common/` it removes.

| # | proposed home | files | lines | status |
|---|---|---|---:|---|
| 1 | `src/metadata/` | tags, ratings, colorlabels, grouping, gpx, exif.cc, metadata, dng_opcode | 10091 | all present in `common/` |
| 2 | `src/database/` | database, sqliteicu (+ the `DT_DEBUG_SQLITE3_*` macros in `common/debug.h`) | 5536 | all present |
| 3 | `src/caches/` | cache (the core), image_cache, mipmap_cache | 3354 | core + 2 of 3; `develop/pixelpipe_cache.{c,h}` is the third and lives elsewhere |
| 4 | `src/math/` | colormatrices.c, curve_tools (1D interpolation) | 1264 | present |
| 5 | `src/system/` | resource_limits, dtpthread | 925 | present |
| 6 | `src/views/` | cups_print — used only by the print view | 813 | 4 consumers, all print-related |
| 7 | `src/pixel/` | `common/lut3d.{c,h}` (distinct from `iop/lut3d.c`) | 368 | present |

Two that are analysis, not moves:

* **`iop_order.{c,h}` and `iop_profile.{c,h}`** belong with `develop/`, but overlap
  `colorspaces.{c,h}`. The overlap has to be resolved before either can move cleanly.
* **`common/colormatrices.c`** is a third maths library, colliding with `system/simd.h` and
  `math/matrices.h`. Same unresolved overlap noted in section 9.

### Do NOT judge these by the layering counter

Simulated with `--what-if` against a 224 baseline:

```
curve_tools + colormatrices -> math/      224 -> 224   (+0)
resource_limits + dtpthread -> system/    224 -> 225   (+1)
cups_print -> views/                      224 -> 225   (+1)
common/lut3d -> pixel/                    224 -> 224   (+0)
```

None of them helps, and two make it marginally worse -- `system/` sits BELOW `common/`, so
moving a file down there turns its existing `common/` includes into upward edges, exactly
as happened when `system/` was created.

That is not an argument against the moves. It means the layering metric has largely done its
job (315 -> 224 over this series) and the remaining work is organisational legibility:
`common/` should stop being the drawer everything shared gets put in. Judge these by what
leaves `common/` -- roughly 22000 lines across the seven groups -- not by the counter.

---

## 11. Done: the redraw throttle moved to the history commit

`widgets/gui_throttle.c` currently exists to serve one caller. `bauhaus.c` defers its
`value-changed` emission so that scrolling a slider or combobox does not trigger a pipeline
recompute per step. Every other widget that could want the same behaviour would have to
reimplement it.

It belongs at the **history-commit bottleneck** instead, where it serves every widget without
any of them knowing it exists.

### Confirmed enabler

`dt_dev_add_history_item_ext()` **already reuses the last history entry for the same module**
(see `add_new_pipe_node` at `dev_history.c:863` and the "history entry reused" log). Consecutive
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
  were the only reason it included `gui/hist_dialog.h`, `gui/gtk.h` and `gui/actions/menu.h`.
  Both callers already live above this layer, so no handler slot was needed.
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
darktable.c:1244   gtk_init()              <- GTK is up
darktable.c:1259   dt_database_init()      <- the prompts happen here
darktable.c:1402   dt_gui_gtk_init()       <- too late to register from
```

It goes in `darktable.c` between the first two, guarded by `init_gui` — legitimate, since
that is the only thing which knows this early whether there will be anybody to ask.

With no handler every prompt answers `CLOSE`. That is not a fallback that guesses: a corrupt
database is not deleted or restored on the strength of a question nobody was asked. It also
closes a latent headless bug — these dialogs had **no `has_gui` guard at all**, so a run
without a GUI reached `gtk_dialog_new_with_buttons()` on a GTK that `ansel-cli` never
initialises. Registration is the guard now.

`common/database.c` is down to zero GTK tokens. Its remaining `gui/legacy_presets.h` include
is a different problem (preset migration, not a dialog) and is left for its own pass.

## 13. Done: `common/history_merge.c` stops calling the GUI

This file's `#include "gui/common/history_merge_gui.h"` was two problems wearing one include.

**Misplaced ownership.** `_hm_make_node_id()`, `_hm_id_to_op_name()` and
`_hm_build_last_history_by_id()` are *defined in* `history_merge.c` but were *declared in*
the GUI header — so the backend included a GUI header to see its own functions. The
declarations moved to `common/history_merge.h`; the GUI half includes that.

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

`_hm_collect_labels_from_history_map()` is defined in `gui/common/history_merge_gui.c` and
was *called from* `history_merge.c`, so the include could not go. It contains no GTK, which
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
`dt_gui_gtk_init()`, with the no-handler defaults described above — and
`#include "gui/common/history_merge_gui.h"` left `common/history_merge.c`. The
`dt_hm_constraint_choice_t` enum moved to `common/history_merge.h`, since the merge
algorithm is what branches on it.

With #1108, #1109 and this one all landed, **`common/*.c` is down to a single `gui/`
include**: `database.c`'s `gui/legacy_presets.h`. That one is preset migration rather than a
dialog, so it is a different kind of problem and gets its own pass. Layering violations are
at 219.

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

This is the argument for the clang-tidy `misc-include-cleaner` gate in §10: nothing about
this include was visible at the point of use, and it survived every previous audit in this
series because the audits grepped `.c` files.

### `dt_gui_gtk_t.selection_stacked` was selection state parked on the GUI struct

Removing the above exposed `common/selection.c` reaching for `dt_gui_get_global()` — not for
anything GUI, but to read and write a flag of its own that happened to live on
`dt_gui_gtk_t`. Three touch points, no GUI code among them. It is `dt_selection_t.stacked`
now, and the field is gone from `dt_gui_gtk_t` (same treatment as `has_scroll_focus`).

Note this also corrects the claim in #1109 that `selection.c`'s `gui/gtk.h` include was
"dead": it was *redundant* — `dt_gui_get_global()` was arriving through
`metadata.h` → `gui/gtk.h` — not unused.

### Where that leaves it

`common/` has **one** `gui/` include: `history_merge.c`, removed by #1110. Layering
violations 219 → 218, and 244 at the start of this series.
## 15. CI gates: what each one can and cannot see

Two checks, because neither covers the other's cases.

### `tools/check_layering.sh` — a ratchet on the include graph

Layering violations may fall, never rise; cycles must stay at zero. Baseline in
`tools/include_baseline.txt`, updated with `--update` when the number improves.

A ratchet rather than a threshold because the tree carries ~217 inherited violations:
demanding zero would mean the check gets switched off. "No worse than yesterday" costs
nothing to comply with and cannot be quietly eroded. Cycles are *not* ratcheted — the
explicit include guards this repository uses instead of `#pragma once` exist precisely so a
cycle is a hard error, and a baseline there would hand that back.

Verified by injecting `#include "gui/gtk.h"` into `common/image_extensions.h`: 220 → 221,
exit 1, restored → exit 0. And again in the other direction, unplanned: rebasing this branch
onto a master that had gained #1110 and #1111 made the check fail with *fell 220 → 217*,
which is the ratchet working — an improvement that is not recorded is an improvement the
next regression gets to spend.

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

### The gap, stated plainly

**The unused-include check cannot see headers, and that is not a configuration mistake.**
include-cleaner analyses the symbols referenced by a translation unit's *main file*; a header
is not one. `--header-filter` does not help — measured, it selects which files' diagnostics
are printed, not which are analysed, and reports the `.c`'s unused includes while saying
nothing about the `.h`. Compiling the header as a synthetic translation unit is worse: that
unit references nothing, so every one of the header's includes comes out "unused".

This matters because **the case that motivated both checks is exactly the case this one
cannot see** — `common/metadata.h` including `gui/gtk.h` and using no GTK symbol (§14). The
layering ratchet is what catches that class, which is why both exist.

## 16. Backlog, and the end goal it serves

### Why any of this

**Ansel intends to move from GTK to Qt.** That is what the whole series is for, and it sets
the bar for "done":

1. **Backend and frontend entirely decoupled**, so the backend is untouched by a toolkit swap.
2. **Within the frontend, pure-toolkit overlays separated from implementation and config** —
   so what has to be rewritten for Qt is a thin, identifiable layer rather than smeared
   through the application.

There is a second, nearer benefit: **the blast radius of the migration cannot even be
estimated today.** `gui/gtk.h` is a god-header and GTK calls run through `libs/`, `views/`
and `iop/`. Layering first is what makes the question answerable.

### 16.1 Definitions that live far from their declaration

Find symbols declared in `example.h` but defined somewhere other than `example.c`. These are
maintenance traps: the definition is not where anyone looks for it, and nothing warns.

`common/history_merge.c` already produced two of these (§13) — `_hm_make_node_id()` and
friends declared in the *GUI* header while defined in the backend, and
`_hm_collect_labels_from_history_map()` the other way round. Both were invisible until an
include had to be removed.

Mechanical to detect: for every declaration in `X.h`, locate the definition and flag anything
not in `X.c`/`X.cc`. Worth a tool alongside `check_layering.sh`, since the answer is a list,
not a judgement.

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

Sweep for code that belongs in the existing low layers rather than where history left it:
solvers, vector algebra and interpolation to `src/math`; generic image filters to
`src/pixel`. `math/homography.{c,h}` (extracted from `apps/ansel-chart`) is the pattern.

### 16.4 Break up the `gtk.{c,h}` god-header

Two halves, and they belong in different directories:

* **stateless GTK wrappers** — helpers that only wrap toolkit calls and carry no application
  state → `src/widgets`, under the rule in its README;
* **implementation** — anything that knows about views, panels, config or `dt_*_get_global()`
  → stays in `src/gui`.

This is 16.4 rather than 16.1 because it is the largest single lever on the goal above: the
stateless half is roughly the part a Qt port must rewrite, and the implementation half is
roughly the part that should survive it. Splitting them is how the estimate gets made.
