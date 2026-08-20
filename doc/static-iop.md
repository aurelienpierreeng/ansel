# IOP modules are linked in, not dlopen'd

## Why this changed

`src/iop` was loaded the same way `src/libs` and `src/views` are: one shared object per
module, installed into `lib/ansel/plugins`, discovered by scanning that directory, bound
with `g_module_symbol()`. For libs and views that is defensible — a user may genuinely not
want a panel, and the set is small. For IOP modules it bought nothing and cost three things.

**It bought nothing, because the module set is not discoverable.** Every one of the 90
`add_iop()` calls in `src/iop/CMakeLists.txt` is unconditional; the only `if()` in that file
switches lut3d's *sources*. More importantly, the application already carries a hardcoded
census of module names that a module on disk cannot join:

* the five order tables in `src/develop/iop_order.c` and `dt_ioppr_insert_missing_modules()`,
* the `_roster[]` in `src/develop/geometry/geometry.c`, which decides who may publish a
  geometry record,
* the `@@_NEW_MODULE` markers that tie the two together.

A module without an entry in those is not a working module. So a genuinely third-party IOP
was never possible without recompiling the core, and the directory scan could only ever
discover a *disagreement* with the tables.

**It cost a hard failure at startup.** `dt_ioppr_check_so_iop_order()` aborts
initialisation — GUI included — when a loaded module has no iop-order entry:

```
[dt_ioppr_check_so_iop_order] missing iop_order for module experimental
ERROR: iop order looks bad, aborting.
```

One `.so` left behind by an experimental branch was enough. `DT_MODULE_VERSION` is a
hand-bumped constant, so any build sharing its value loads. The quiet variant is worse: a
stale module whose op name *does* have an order loads fine and reads a struct whose layout
has since shifted.

**It cost two thirds of startup.** Measured headless (`ansel-cltest -d control`, 3 runs,
warm cache): startup 0.373–0.388 s total, of which the IOP load span was 253–260 ms. 108 ms
of that is `lens` alone (lensfun's database, in `init_global`, and unaffected by any of
this). The remaining ~136 ms for 90 modules is almost entirely the dynamic loader: an
isolated benchmark of 91 `dlopen()`s of the same files with `libansel.so` preloaded costs
120 ms lazy / 122 ms bound, so symbol binding is not the expense — mapping and relocating 91
objects is.

## What replaced it

The constraint that shaped the design: **the API must not change.** Every module defines
`process`, `name`, `commit_params` with those exact names, and that sameness is the point —
it is what makes a module read as an implementation of an abstract class. No module source
was touched.

### Symbol namespacing by asm label

`common/module_api.h`'s `FULL_API_H` branch attaches an asm label to each declaration when
`DT_MODULE_SYMBOL_PREFIX` is defined:

```c
#define OPTIONAL(return_type, function_name, ...) \
    return_type function_name(__VA_ARGS__) DT_MODULE_SYM(function_name)
```

so `iop/exposure.c` still writes `int process(...)` and emits `dt_iop_exposure__process`.
An asm label renames the *symbol* and leaves the *identifier* alone, which a
`#define process ...` could not: struct members, locals and `->name` are untouched by
construction. The label attaches to the first declaration and the later definition inherits
it; re-declaring it identically is fine, which matters because `iop_api.h` has no include
guard and most modules include it again themselves.

Verified on GCC and Clang, with and without `-flto`, through a static archive. It is a
compiler feature, so it needs nothing from the linker — which is what makes it work on the
Windows nightly, where clang drives **lld** in COFF mode: there is no `-r` partial link and
COFF has no symbol visibility, so the `objcopy --localize-hidden` / `--redefine-sym` route
that works on ELF is unavailable.

`DT_MODULE_SYMBOL_PREFIX` is set per module by `add_iop()`. It is undefined for `src/libs`
and `src/views`, which are still one shared object each and still dlopen'd, so nothing there
changes.

**macOS:** an asm label is emitted verbatim and Mach-O prefixes symbols with an underscore,
so `DT_MODULE_SYM` adds it under `__APPLE__`. ELF and PE/COFF x86-64 do not.

### Answering what `g_module_symbol()` used to answer

Binding needs to know, per module, which entry points it actually defines — `OPTIONAL` ones
legitimately absent, `DEFAULT` ones falling back to `default_<fn>`. `tools/generate_iop_static.py`
answers it by running the **real compiler's preprocessor** over the module's real sources
with the module's real flags, and emitting `DT_MODULE_HAS_<fn>` as 0 or 1 for every name in
the API.

It has to be the preprocessor, not a regex. A regex over the raw sources gets 90 of the 91
modules right and then meets `iop/censorize.c`, which parks four functions behind
`#if FALSE` and writes `name()` with its return type on the previous line. Preprocessed
output, filtered by line marker to the module's own files, matches `nm -D` on all 91 of the
previously shipped `.so`s exactly.

The presence header defines a macro for *every* API name, so a name the generator did not
consider is a compile error in `DT_MODULE_PICK()`, never a silently NULL function pointer.
`REQUIRED` entries are bound unconditionally: a module missing one fails to link.

### Two halves, and why they cannot be merged

`<module>_static.c` is generated into the module's own object library, because that is the
only translation unit where the plain API names resolve to *that* module's symbols. It fills
everything the module defines and leaves NULL elsewhere.

The `DEFAULT` fallbacks are applied afterwards, in `develop/imageop.c`, because
`default_process`, `default_group` and the rest are `static` to that file. Hence the two
X-macro branches, `INCLUDE_API_FROM_MODULE_STATIC` and
`INCLUDE_API_FROM_MODULE_STATIC_DEFAULTS`.

### Where the objects live

Each module is an `OBJECT` library whose objects are added to `lib_ansel` via
`target_sources`. They cannot go in the executable: `lib_ansel` is a shared library, and on
Windows a DLL may not leave symbols for its executable to resolve. The generated
`iop_registry.c` — also compiled into `lib_ansel` — names every module's binder, which is
what pulls the objects into the link.

The object libraries link `ansel_deps` and OpenMP directly rather than `lib_ansel`; naming
`lib_ansel` would be a dependency cycle, and its symbols need no linking now that the
objects are inside it.

## Latent bugs this surfaced

One link instead of 91 turns "each `.so` gets its own copy" into "duplicate definition".
The linker found 30 such symbols, and nearly all of them are the defect `CLAUDE.md` already
warns about — a header that *defines* rather than declares. Each was given internal linkage,
which is exactly the lifetime they had before (one copy per shared object) and so is not a
behaviour change:

* `common/colorchecker.h` — 24 of the 30. Ten `dt_colorchecker_*` functions and fourteen
  built-in chart tables (`spyder_*`, `xrite_*`, `CGATS_types`,
  `colorchecker_material_types`), all defined at file scope in a header included by both
  `common/colorchecker.c` and `iop/channelmixerrgb.c`. Now `static inline` / `static`.
  **The properly correct fix is to move these into `common/colorchecker.c`, which already
  exists and already defines the header's other functions.** That is a separate cleanup and
  deliberately not done here.
* `pixel/chromatic_adaptation.h` defined eight `const dt_colormatrix_t` matrices at file
  scope, alongside four siblings that were already `static const`. Now all twelve are.
* `pixel/locallaplacian.h` defined `local_laplacian()` — a one-line wrapper — at file scope,
  among neighbours that are declarations. Now `static inline`.
* `pixel/illuminants.h` defined `pair_min()` at file scope. Now `static inline`.
* `iop/initialscale.c`, `iop/finalscale.c` and `iop/rotatepixels.c` each had a file-scope
  `dummy`. Now `static`.
* `iop/channelmixerrgb_shared.c` was `#include`d textually into two modules, with a comment
  saying it had to be, "to avoid duplicate globals from a separate compiled object". That
  constraint is exactly what this change reverses, so it is a translation unit again,
  compiled once.

A note on finding these: counting duplicate symbol names across the *pre-LTO* object files
of a partial build undercounts badly — it reported 37 names, mostly the shared-implementation
file, and missed `common/colorchecker.h` entirely. The link is the only reliable census.

**And the Release link is not census enough either.** Giving a header-defined object internal
linkage silences the duplicate but creates an *unused* one in every translation unit that does
not read it, which `-Werror=unused-variable` rejects — and `-Werror` is on in Debug only, where
LTO is off. Three dead `dummy` variables (`initialscale`, `finalscale`, `rotatepixels`) turned
out to be referenced by nothing at all, since `IOP_GUI_ALLOC()` only takes `sizeof` of the type,
and were deleted; `CGATS_types` and `colorchecker_material_types` had exactly one consumer each
and moved into `common/colorchecker.c`, which is where they should have been. **Build BOTH
configurations before pushing**: Release is the only one with LTO, Debug the only one with
`-Werror`, and each hides a different half of this.

## Does it make pixel processing faster? No.

The interesting hypothesis was that folding the modules into `lib_ansel` lets LTO inline
across a boundary it could not cross before. Measured, `ansel-cli` full-resolution export,
`--disable-opencl -t 8`, master and this branch interleaved run-by-run to cancel thermal
drift, `-d perf` for the pipeline's own timing (no startup, no I/O):

**Default pipeline, 15 runs per side** (medians, Mann-Whitney two-sided p):

| | master | static | delta | p |
|---|---|---|---|---|
| **TOTAL pipeline** | 6.738 s | 6.524 s | **-3.2%** | **0.59** |
| Highlight reconstruction | 2.917 | 2.807 | -3.8% | 0.19 |
| Filmic | 1.478 | 1.404 | -5.0% | 0.007 |
| Lens correction | 0.797 | 0.863 | +8.3% | 0.004 |
| Color calibration | 0.563 | 0.565 | +0.4% | 0.84 |
| Demosaic | 0.187 | 0.178 | -4.8% | 0.04 |

**Heavy sidecar, 3 runs per side** (a different module set):

| | master | static | delta |
|---|---|---|---|
| **TOTAL pipeline** | 70.48 s | 71.23 s | **+1.1%** |
| Diffuse or sharpen | 64.71 | 65.65 | +1.4% |
| Color calibration | 0.613 | 0.490 | -20.1% |
| Horizon and perspective | 0.314 | 0.259 | -17.5% |
| Color calibration 1 | 0.541 | 0.475 | -12.2% |
| Color balance | 1.794 | 1.631 | -9.1% |
| Filmic | 0.871 | 0.802 | -7.9% |
| Lens correction | 0.972 | 1.017 | +4.6% |

**The whole-pipeline number does not move.** -3.2% on one workload with p = 0.59 is noise;
+1.1% on the other. Individual modules move by real amounts in *both* directions, and they
cancel — and in each workload the total is dominated by one module (`highlights`, then
`diffuse`) that barely moved.

The likely reason there is nothing to win: this codebase already inlines the hot paths
through headers. `pixel/colorspaces_inline_conversions.h`, `math/*.h`, `pixel/*.h` are full
of `static inline`, so a module's pixel loop had already inlined everything it calls before
LTO was ever asked. What crossing the boundary newly exposes is mostly cold glue.

Two caveats on reading the per-module rows. First, this branch also changed the linkage of a
few shared helpers (`pair_min`, `local_laplacian`, `channelmixerrgb_shared.c` becoming a real
translation unit), so a per-module delta is this branch's net effect and not attributable to
LTO alone. Second, **`lens` is consistently slower** — +8.3% (p = 0.004, n = 15) and +4.6% on
the other workload. It is the C++ module; nobody has looked at why. On the default pipeline
that is about 66 ms of a 6.5 s export.

So: take this change for the startup time, the failure mode it removes and the honesty of the
module boundary. Do not take it expecting faster pixels.

## What this costs

Editing one module used to relink one small `.so`. It now relinks `lib_ansel`, which takes
34–42 s with LTO. That is the real price, it is a developer-workflow price rather than a
runtime one, and it is the thing to revisit if it becomes painful — giving the IOP objects
their own non-LTO target would recover most of it at the cost of cross-module inlining.

## Things that follow from this

* The 91 `plugins/<name>/enable` conf keys are gone. They were written on first run by
  `dt_module_load_modules()`, read by nothing, exposed in no preferences page, and made up
  91 of the 155 lines of a fresh `anselrc`.
* `dt_module_dt_version` / `dt_module_mod_version` still exist and are still per-module, but
  the ABI check they served is now structural: a module and its application are compiled
  together or not at all.
* Module order in `darktable.iop` is the registry's, which is alphabetical and stable, rather
  than `readdir()`'s. Pipe order comes from `iop_order` and never depended on it.
