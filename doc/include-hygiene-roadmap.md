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

   The structural fix applied: the Windows legacy-macro shim moved from `common/darktable.h`
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

