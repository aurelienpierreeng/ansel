# Getting GTK out of the backend {#gtk_decoupling}

Written 2026-08-20 from measurement, as the companion to `doc/control-split.md` and
`doc/develop-split.md`. Those two re-stratify *directories*; this one removes a *dependency*. They
overlap but are not the same work, and doing either does not finish the other.

The end goal is the one in `doc/reorganisation.md`: a backend that a Qt front end could sit on
without rewriting the pixel engine. That is not achieved by a port — it is achieved by making the
backend stop naming a toolkit, at which point the port is a bounded amount of new code instead of an
archaeology project.

## 0. Two different dependencies, counted together today

`tools/check_module_boundaries.sh`'s toolkit gate matches
`Gtk*|Gdk*|GTK_*|GDK_*|cairo_*|PangoLayout*` and the corresponding includes. That is the right net
for a ratchet, but the plan has to separate what it catches:

- **GTK and GDK** are the *UI framework*: widgets, events, windows, the main loop. A Qt front end
  replaces all of it. Nothing below `src/gui` and `src/views` may name these.
- **cairo and pango** are a *2D rasteriser and a text shaper*. They are not GTK, they do not imply a
  UI framework, and a Qt port can keep them. `iop/watermark.c` renders an SVG into the pixel buffer
  with cairo inside `process()`; that is image processing, and it stays.

So the target is not "zero toolkit hits everywhere". It is:

| | GTK/GDK | cairo/pango |
|---|---|---|
| `src/pixel`, `src/caches`, `src/database`, `src/metadata`, `src/history`, `src/system`, `src/common` | **zero, enforced** | zero except where it renders pixels |
| `src/develop`, `src/imageio`, `src/iop` | **zero in the operator half** | allowed in the panel half and in genuine rasterisers |
| `src/gui`, `src/views`, `src/libs`, `src/widgets` | unrestricted | unrestricted |

## 1. Measured shape, 2026-08-20

Files naming a toolkit, by module (the gate counts files, not occurrences — the file is the unit
that gets split):

| module | files | headers | what it actually is |
|---|---|---|---|
| `iop` | 97 | 4 | module panels living in the same file as `process()` |
| `develop` | 20 | 7 | four GUI files, the mask shapes' overlay drawing, and header leaks |
| `imageio` | 13 | 1 | format/storage settings panels in the same file as the codec |
| `common` | 11 | 3 | logo/SVG helpers, conf, sentry/telemetry dialogs |
| `system` | 3 | 2 | `surface_scaling.h` — cairo helpers in layer 0 |
| `colorprofiles` | 2 | 1 | display-profile plumbing (already half-solved, see §5) |
| `metadata` | 2 | 2 | **`#include <gtk/gtk.h>` naming nothing** — delete |
| `history` | 1 | 1 | `GtkTreeView *items` in `history.h` |
| `pixel`, `caches`, `database` | 0 | | already free, pinned at zero |

### The `src/iop` breakdown, which is the surprise

Classifying every toolkit-naming line in `src/iop/*.c` by where it sits:

```
file scope (gui_data structs)          494 lines
inside GUI functions                  4982 lines
inside anything else                   818 lines across 180 function names
```

Reading the third bucket by hand: **almost all of it is the same two things misattributed** — a
`gui_data` struct declared immediately after a backend function, and GUI callbacks whose names do
not contain "gui" (`update_illuminants`, `color_picker_apply`, `_sync_layer_controls`,
`area_motion_notify`, `_enter_edit_mode`, …).

After that reading, the toolkit code genuinely inside a backend path in all of `src/iop` is:

- **`iop/watermark.c`'s `process()`** — ~40 cairo calls, rendering an SVG into the pixel buffer.
  This is a rasteriser and it stays;
- **`iop/liquify.c`'s `process()`** — one `cairo_rectangle_int_t` used as a plain integer rectangle.
  Replace the type, no behaviour.

**So the operator/panel split in `src/iop` is mechanical for 89 of 91 files.** That is the single
most important number in this document, and it is why this is worth doing.

## 2. What actually blocks it

`src/iop/CMakeLists.txt:5-6`

```cmake
add_definitions(-include common/module_api.h)
add_definitions(-include iop/iop_api.h)
```

and `iop/iop_api.h:44-45`, inside the `FULL_API_H` block:

```c
#include <cairo/cairo.h>
#include <gtk/gtk.h>
```

Every IOP translation unit therefore compiles with GTK in scope whether it names it or not. Two
consequences, both stated in the gate's own comment so the number is not over-read:

1. no file partition produces a genuinely toolkit-free IOP **object** until this is dealt with;
2. `tools/include_graph.py` cannot see it either — its `INCLUDE_RE` matches the quote form only, so
   angle-bracket system includes are outside the graph entirely.

`iop_api.h` needs GTK because the module vtable declares GUI entry points: `gui_init`,
`gui_update`, `gui_post_expose(…, cairo_t *cr, …)`, `gui_button_pressed(…, GdkEventButton *)`. **The
API is GTK-typed, so the file split cannot precede an API split.** That ordering is the whole
sequencing constraint of this plan.

## 3. Techniques already proven in this tree

Use these; do not invent a fourth.

**The opaque typedef.** `colorprofiles/colorspaces.h:91-98` already does it:

```c
/** @brief GtkWidget, opaque, spelled exactly as GTK spells it. */
typedef struct _GtkWidget GtkWidget;
```

A header can *name* `GtkWidget *` without including GTK, as long as it never dereferences one.
This is what lets a backend header keep an accessor that returns a widget during the transition,
instead of forcing every consumer to be converted at once.

**The X-macro API split.** `common/module_api.h`, `iop/iop_api.h`, `views/view_api.h`,
`libs/lib_api.h`, `imageio/format/imageio_format_api.h` are re-included several times per TU with
different macros and expand *inside struct bodies*. They have no include guards and must not get
any (CLAUDE.md). Real includes belong inside the `#ifdef FULL_API_H` block, which is defined only in
full-API mode; the struct-body expansion defines `INCLUDE_API_FROM_MODULE_H` and skips it. **This is
the mechanism that lets a vtable be split in two without an ABI change** — two headers, two
force-includes, one struct.

**One derivation, two callers.** The rule that came out of the geometry service: when the GUI and
the engine both need the same fact, they call the same function; a second derivation drifts and the
drift surfaces months later as an overlay that no longer sits on the thing it describes.

## 4. The sequence

Each phase is independently mergeable, lowers a named baseline in
`tools/check_module_boundaries.sh`, and is verified by the same four builds plus a pixel A/B where
it touches a render path.

### Phase 1 — delete what names nothing (1 PR, ~1 day)

`src/metadata/colorlabels.h` and `ratings.h` include `<gtk/gtk.h>` and name **zero** toolkit
symbols. `src/common/file_location.h` and `l10n.c` likewise. Delete the includes, fix whatever
compiled only because of them (expect a handful — this is the "a header includes only what its own
declarations need" rule, and its documented failure mode is that consumers were relying on the
supply line).

Baselines: `toolkit_metadata` 2 → 0, pin it.

### Phase 2 — split the module API in two (1 PR, the keystone)

`iop/iop_api.h` becomes:

- **`iop/iop_api.h`** — operator entry points only: `process`, `process_cl`, `commit_params`,
  `init_pipe`, `modify_roi_in/out`, `tiling_callback`, `legacy_params`, `introspection`,
  `default_colorspace`, `geometry_record`. No GTK, no cairo.
- **`iop/iop_gui_api.h`** — `gui_init`, `gui_update`, `gui_cleanup`, `gui_reset`, `gui_focus`,
  `gui_post_expose`, `gui_button_pressed`, `gui_motion_notify`, `gui_scrolled`, `gui_key_pressed`,
  the colour-picker hooks. Includes GTK and cairo.

Both expand into the same `dt_iop_module_so_t` / `dt_iop_module_t`, so **nothing about the ABI, the
loader, or `dlopen` changes**. `src/iop/CMakeLists.txt` force-includes the operator header for every
TU and the GUI header only for `*_gui.c`.

Verification that it did what it claims: a scratch `iop/<any>_gui.c` that names `GtkWidget` compiles;
the same line in `<any>.c` fails.

### Phase 3 — split the IOP files (89 mechanical + 2 by hand, ~10 PRs of ~9 modules each)

`iop/<name>.c` keeps `process`/`commit_params`/params/introspection. `iop/<name>_gui.c` takes the
`gui_data` struct, every `gui_*`, every callback, every `post_expose`. Both compile into the same
`.so`; `src/iop/CMakeLists.txt`'s per-module macro gains the second source.

Order them by density, cheapest first, so the ratchet moves early: the 44 gtk-only files before the
30 that also draw with cairo.

The two by hand:
- **`watermark.c`** — `process()` keeps cairo. Declare that explicitly in the plan and in the file:
  it is a rasteriser, not a panel. It will keep `cairo` in the operator half forever and that is
  correct.
- **`liquify.c`** — replace `cairo_rectangle_int_t` with a plain struct in the operator half.

Baseline: `toolkit_iop` 97 → ~2 (watermark, liquify), one per split module, measurable per PR.

### Phase 4 — `src/develop` (3 PRs)

1. **The four already-pure GUI files** — `blend_gui.{c,h}` (603 hits), `imageop_gui.{c,h}` (270),
   `masks/masks_gui.c` + `masks_gui.h` (206), `dev_history_gui.c`. They are already named `_gui`;
   what keeps them in `develop/` is that the backend still calls into them. That is
   `doc/develop-split.md`'s T3/T4 work, largely landed — finish it and move the files to `gui/`.
2. **The mask shapes** — `brush.c` (56), `gradient.c` (25), `polygon.c` (24), `ellipse.c` (19),
   `circle.c` (13), `group.c` (5). Each file holds both the rasteriser (`get_mask_roi`,
   `get_points_border`) and the overlay drawing (`post_expose`, `get_distance`, `key_pressed`,
   `populate_context_menu`). `masks/masks_functions.h` **already declares both halves in one
   vtable**, so the split is the same shape as Phase 2: two vtables, `<shape>.c` and
   `<shape>_draw.c`.
3. **The header leaks** — `develop.h:49` includes `<cairo.h>` and carries `GtkWidget *` members
   with the comment *"yes, having gtk stuff in here is ugly. live with it."*; `imageop.h:57`
   includes `<gtk/gtk.h>` and publishes `GtkWidget *dt_iop_gui_get_off()` and friends to 122
   consumers. Opaque typedef (§3) for the accessors, move the members to the GUI-side struct that
   T4 creates.

Baseline: `toolkit_develop` 20 → ~6.

### Phase 5 — `src/imageio` (2 PRs)

Same shape as Phase 3, 13 files: `format/{avif,pdf,png,tiff,webp,jpeg,j2k,exr,copy}.c` and
`storage/{disk,gallery}.c` each hold a codec and its settings panel.
`imageio/format/imageio_format_api.h` and `imageio/storage/imageio_storage_api.h` are already
X-macro headers with the same `FULL_API_H` discipline, so Phase 2's split applies verbatim.

`pdf.c` is the exception to check: it uses cairo to *write a PDF*, which is a rasteriser use like
watermark's.

Baseline: `toolkit_imageio` 13 → ~1.

### Phase 6 — the stragglers (1 PR)

- `system/surface_scaling.h` — cairo helpers in layer 0. Cairo is not GTK, but layer 0 is the wrong
  home; move to `widgets/` or `gui/`.
- `common/utility.{c,h}` — `dt_util_get_logo()`, `dt_render_svg()`: cairo/rsvg surface builders that
  belong with the widgets.
- `history/history.h:59` — `GtkTreeView *items` in a backend header.
- `common/{sentry,telemetry,conf,film,variables,history_actions}.c` — a handful of hits each,
  mostly dialogs and `dt_control_log`-shaped messaging. The messaging half inverts through
  `develop/pipeline_notify.h`, the pattern T2 established.
- `colorprofiles/` — display-profile plumbing. Half-solved already: the module never sees a
  `GdkPixbuf` (`colorspaces.h:588`), and the widget type is opaque. Finish it.

### Phase 7 — enforce (1 PR)

Add the gate that makes the property permanent, in the shape the tree already uses:

- pin every baseline that reached its floor at that floor;
- add a **GTK-only** counter alongside the existing toolkit counter, so cairo in a rasteriser does
  not read as a regression and GTK anywhere below `gui/` does;
- add the link-level check the current gate says it cannot make: with Phase 2 done, assert that no
  `iop/<name>.c.o` has an undefined `gtk_*` symbol (`nm -u`). That is the claim
  "toolkit-free objects", which is stronger than "no file names a toolkit symbol", and only
  becomes checkable after Phase 2.

## 5. Decisions taken, with the reasons

**The API split precedes the file split.** Phase 2 before Phase 3. The vtable is GTK-typed, so
splitting files first produces `*_gui.c` files that are still compiled with GTK forced in — the
ratchet would fall while the property it claims to measure did not change.

**cairo is not the enemy; GTK is.** Counting them together is right for a ratchet that must not
regress, wrong for a target. `watermark.c` and `pdf.c` rasterise with cairo in the backend and will
keep doing so. A plan that demanded zero cairo everywhere would have to rewrite an SVG renderer for
no benefit to a Qt port.

**Two vtables, one struct — not two module types.** Splitting `dt_iop_module_t` into an operator
object and a panel object was considered and rejected for now: every module's `gui_data` is reached
through `self`, the loader resolves one symbol table per `.so`, and a second object would need a
lifetime and an owner. The X-macro headers already give the compile-time separation the ratchet
measures, at zero runtime cost. Splitting the *object* is `doc/develop-split.md`'s T4/T6 question,
and it should be answered after this plan has removed the type coupling, not before.

**The mask shapes split by vtable, not by directory.** `masks_functions.h` already has both halves;
moving the drawing into `<shape>_draw.c` keeps the rasteriser and the overlay in the same directory
where they share the shape's geometry helpers, which is what stops the two derivations drifting
(§3).

**Not attempted here: replacing GTK in `src/gui`.** That is the port. This plan's success condition
is that the port becomes possible to scope — the backend stops naming a toolkit, and the count of
files that do is the estimate.

## 6. What to measure, every PR

```
bash tools/check_module_boundaries.sh     # the toolkit baselines, per module
python3 tools/include_graph.py --summary  # cycles 0, layering violations
bash tools/check_unused_includes.sh --changed origin/master
```

and, where a render path moved,
`tools/check_export_pixels.sh origin/master HEAD <a RAW>` — a raw, never the default PNG, and always
`--disable-opencl` (CPU/GPU parity noise fakes a regression; see the standing rules in CLAUDE.md).

A phase that lowers a baseline commits the new baseline **in the same commit**. The gate fails on a
fall as well as a rise, which is what makes partial progress safe.
