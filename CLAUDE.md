# Ansel — notes for AI assistants

**This file holds the rules. The knowledge lives in [`doc/`](doc/README.md).**

It used to be both, and grew to 3,177 lines of accumulated findings. A verification pass on
2026-09-29 against `42eca0e8fe` checked every falsifiable claim in it and found **sixteen that
were wrong** — including one marked `(OPEN)` that had been fixed a month earlier, one that
forbade a feature the code already implements, and one naming a struct field that does not
exist. A file nobody can date is a file nobody can trust, so the findings moved to `doc/`,
where each one carries the commit it was established against.

## How to use the documentation

- **Rules below are binding.** They are short on purpose. CI enforces most of them.
- **Everything else is in `doc/`.** Read the file for the area you are touching *before* you
  touch it — every one of them exists because something cost days to work out.
- **Every finding in `doc/` is dated and carries a commit hash.** A finding is only as good as
  its hash. Before acting on one older than the code you are changing, re-measure it — and
  re-date it when you confirm it. Where a claim was found to be **wrong**, that is recorded
  rather than quietly corrected: how a claim was wrong is usually the more useful thing to know.
- **When you learn something that cost real work, write it in `doc/`, not here** — with the date
  and the commit you established it against.

---

## The rules

### 1. `#pragma once` is FORBIDDEN — use an include guard

Named after the path relative to `src/` (`src/develop/masks/masks_history.h` →
`DT_DEVELOP_MASKS_MASKS_HISTORY_H`). `#pragma once` silently makes a cyclic include graph
compile; explicit guards make the same situation greppable. `src/darktable.h` has an `#error`
tripwire instead of a guard, and **no header may include it**. Five X-macro headers
deliberately have no guard at all and must never get one.

Enforced: `python3 tools/pragma_once_to_guards.py --verify`, and
`python3 tools/include_graph.py --summary` must keep reporting `cycles 0`.
→ [`doc/architecture-rules.md`](doc/architecture-rules.md), [`doc/include-graph.md`](doc/include-graph.md)

### 2. A header includes only what its own declarations need

Everything else belongs in the `.c`. A header that includes more becomes a supply line its
consumers never asked for and cannot see. Implementations do not belong in headers either; the
one exception is a header whose published interface *is* inline code.

Enforced: `tools/check_unused_includes.sh --changed <ref>`.
→ [`doc/architecture-rules.md`](doc/architecture-rules.md)

### 3. No SQL in GUI modules

`src/libs/` and `src/views/` contain no raw SQL. Database access belongs behind named functions
in `src/common/` and `src/database/`.
→ [`doc/architecture-rules.md`](doc/architecture-rules.md), [`doc/collection.md`](doc/collection.md)

### 4. The pipeline↔module interface is history

The ONLY thread-safe interface between the pixel pipeline and an IOP module is **history**,
guarded by `dev->history_mutex`. `module->params` and `module->blend_params` belong to the GUI
thread. Never call `dt_iop_commit_params(module, module->params, ...)` from pipeline code —
commit from the history snapshot. To push live state to the pipe, use the transient-resync
interface `dt_dev_transient_params_{set,clear,get,active}`.
→ [`doc/pipeline-history.md`](doc/pipeline-history.md), [`doc/reorganisation.md`](doc/reorganisation.md)

### 5. A stored format's version is bumped only once it has shipped

→ [`doc/architecture-rules.md`](doc/architecture-rules.md)

### 6. A new preferences SECTION takes three edits; an entry takes one

An entry in an existing section is **one** edit, to `data/anselconfig.xml.in`. A new *section
value* takes three: the XML, plus `data/anselconfig.dtd` (the `section` attribute is an
enumerated list; `xmllint` fails the build otherwise) and `tools/generate_prefs.xsl` (a section
not enumerated there is silently dropped from the UI, with no error).

Measured 2026-09-29 over the 108-commit history of that file: of the **45** commits that added a
`<dtconfig>` entry, **40 touched only the XML** and 5 touched the DTD or XSL — and those 5 are
exactly the ones introducing a new section.
→ [`doc/preferences.md`](doc/preferences.md)

### 7. Measure before theorising, and say what you measured

This tree's history is full of plausible-but-wrong theories that survived source reading and
died on the first measurement. Several entries in `doc/` exist only to record which theories
were killed and how. When you write a finding down, write down the number and the method.

### 8. What needs the user's attention goes in an alert window, not a toast

A toast (`dt_control_log()`, `dt_pipeline_message()`) fades within seconds, while what such a
message says — a module failed, an image was not updated, an export will not come — stays true.
Use `dt_control_alert(title, message)` from
[`src/control/user_message.h`](src/control/user_message.h), next to `dt_control_log()`: the GUI
shows it with `dt_gui_alert()` from [`src/gui/alert.h`](src/gui/alert.h) — a small window with
one OK button, kept above the others until it is clicked, one per title — and without a GUI
(`ansel-cli`) it falls back to the toast. It takes the finished text, not a printf format: build
a message with values in it with `g_strdup_printf()` and free it after the call. Any thread, any
layer from `common/` up; only `gui/` itself calls `dt_gui_alert()` directly. Repeated failures share a window by sharing a title, so
reuse one of the existing titles ("Module failed", "Export failed", "Not enough memory", …)
before inventing one. A window that needs
more than OK — other buttons, a list, a question — is a `dt_gui_alert_t` object
(`dt_gui_alert_new()` with a kind, then `_add_text/_add_widget/_add_button/_show/_destroy`), as
`gui/closing.c` uses it. The window itself — frame, modality, Escape, title bar, focus, the macOS
specifics — lives entirely in `gui/alert.c`; a caller never builds or configures it. Toasts
remain right for what is fine to miss: progress, confirmation, a passing state.
→ [`doc/pipeline-cache.md`](doc/pipeline-cache.md) (the first user: the memory pressure valve)

---

## Where the knowledge is

### Structure and process
| Read this | For |
|---|---|
| [`doc/README.md`](doc/README.md) | the documentation index and how to build |
| [`doc/reorganisation.md`](doc/reorganisation.md) | module map, layering, the direction of travel |
| [`doc/architecture-rules.md`](doc/architecture-rules.md) | the five rules above, with the evidence |
| [`doc/include-graph.md`](doc/include-graph.md) | include guards, cycles, how they are measured |
| [`doc/preferences.md`](doc/preferences.md) | the three-edit rule |

### Pipeline, cache and history
| Read this | For |
|---|---|
| [`doc/pipeline-cache.md`](doc/pipeline-cache.md) | the pixelpipe cache: keys, refcounts, peeks, memory pressure |
| [`doc/pipeline-history.md`](doc/pipeline-history.md) | history snapshots, the darkroom worker, the transient slot |
| [`doc/image-mipmap-cache.md`](doc/image-mipmap-cache.md) | thumbnail invalidation, image cache entry locks |
| [`doc/raw-roi-cfa.md`](doc/raw-roi-cfa.md) | RAW-domain ROI offsets, CFA phase, tile-grid dependence |
| [`doc/resizing-scaling.md`](doc/resizing-scaling.md) | ROI planning and scaling |

### Colour
| Read this | For |
|---|---|
| [`doc/colorprofiles.md`](doc/colorprofiles.md) | profile roles, locks, the derived-profile memo |
| [`src/colorprofiles/README.md`](src/colorprofiles/README.md) | the module's own map |
| [`doc/color.md`](doc/color.md) | colour spaces and the working space |
| [`doc/highlights-reconstruction.md`](doc/highlights-reconstruction.md) | harmonic highlight reconstruction |
| [`doc/interpolation.md`](doc/interpolation.md) | which resampling kernel, and why |

### Masks
| Read this | For |
|---|---|
| [`doc/masks-geometry.md`](doc/masks-geometry.md) | brush and polygon outlines and rasters |
| [`doc/brush-boundary.md`](doc/brush-boundary.md) | the full boundary account |
| [`doc/masks-gui.md`](doc/masks-gui.md) | gestures, the wheel mapping, the shape manager |
| [`doc/masks-history.md`](doc/masks-history.md) | refcounted forms, module mask groups, the enclosure |
| [`doc/masks-enclosure-p2.md`](doc/masks-enclosure-p2.md) | the enclosure plan |
| [`doc/masks_history_dedup.md`](doc/masks_history_dedup.md) | the persistence dedup design |
| [`doc/overlay-raster.md`](doc/overlay-raster.md) | how the overlay is drawn |

### Modules and GUI
| Read this | For |
|---|---|
| [`doc/iop-notes.md`](doc/iop-notes.md) | per-module findings (ashift, retouch, toneequal, …) |
| [`doc/drawlayer.md`](doc/drawlayer.md) | the drawlayer module in full |
| [`doc/retouch-result-memo.md`](doc/retouch-result-memo.md) | retouch's per-shape memo |
| [`doc/gtk-patterns.md`](doc/gtk-patterns.md) | layout, focus, repaint and threading patterns |
| [`doc/shutdown.md`](doc/shutdown.md) | what a quit waits for, and the window that says so |
| [`doc/darkroom-redraw.md`](doc/darkroom-redraw.md) | the centre repaint path, priced |
| [`doc/thumbtable.md`](doc/thumbtable.md) | the thumbnail grid |
| [`doc/accelerators.md`](doc/accelerators.md) | keyboard shortcuts end to end |
| [`doc/collection.md`](doc/collection.md) | the library module, import and removal |
| [`doc/removal-undo.md`](doc/removal-undo.md) | how removal is undone |
| [`doc/export.md`](doc/export.md) | export size resolution |

### Infrastructure
| Read this | For |
|---|---|
| [`doc/nightly-distribution.md`](doc/nightly-distribution.md) | nightlies, channels, the manifest |
| [`doc/sentry.md`](doc/sentry.md) | crash reports and how to fetch one |
| [`doc/telemetry.md`](doc/telemetry.md) | what is collected |
| [`doc/static-iop.md`](doc/static-iop.md) | static IOP linking |
| [`doc/exiv2.md`](doc/exiv2.md) | the bundled Exiv2 |

The full index, including everything not listed here, is [`doc/README.md`](doc/README.md).
