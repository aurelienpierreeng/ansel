# Decomposing `src/develop` — pipeline / params-history / GUI

Measured 2026-08-12 on `refactor/history-presentation`, by per-file symbol density, the
include graph, and five parallel deep-reads of the entanglement knots (each finding cited
by line number below; the full structured survey is in the PR discussion). This is the
plan the `src/history` work must be weighed against, and the answer to "split
`src/develop` before carrying on with `src/history`?" is at the end.

## What this actually is: a re-stratification, not a split

The layer table (`tools/include_graph.py`) today puts `develop` at **5, above `gui` (4),
`widgets` (4) and `control` (3)** — the darktable legacy ordering in which the pipeline may
call the GUI as a substrate. That is why `develop→gui` edges are *not even counted* in the
189-violation baseline. The GTK→Qt goal needs the opposite: a backend (pixel pipeline +
params/history engine) that builds without GTK, below a frontend that can be swapped. So the
develop decomposition ends with the layer table changing — backend layers below `gui` — and
every tranche below is chosen to survive that flip.

## The measured shape

`src/develop` is 62k lines (subdirs included). The entanglement is **concentrated, not
smeared** — whole-file symbol density:

* **Already-pure pipeline** (~8.6k): `tiling.c`, `pixelpipe_hb.c`, `pixelpipe_gpu.c`,
  `pixelpipe_cpu.c`, `pixelpipe.c`, `pixelpipe_raster_masks.c`, `pixelpipe_rawdetail.c`,
  `imageop_math.c`, `dev_pixelpipe.c` — near-zero GUI symbols each.
* **Already-pure GUI** (~6.8k): `blend_gui.c` (5118), `masks/masks_gui.c`, `imageop_gui.c`,
  `gui_throttle.c` — the `_gui` sibling pattern already exists, and `gui/common/` +
  `gui/develop/` already hold six GUI halves of backend files.
* **Already-pure params/history** (~12k): `iop_order.c`, `history_merge.c`, `supervisor.c`,
  `lightroom.c`, `iop_profile.c`, `dev_snapshot.c`, `masks/masks_history.c`.
* **The five knots**, per the deep-reads (lines classified per axis):

| knot | pipeline | params_history | gui |
|---|---|---|---|
| `develop.{h,c}` | 720 | 620 | 1135 |
| `imageop.{h,c}` | 390 | 1350 | 2040 |
| `dev_history.c` | 70 | 3370 | 330 |
| `masks.{h,c}` (+ shape files' GUI) | 280 | 1960 | 3810 |
| `blend.h` + pipeline files' upcalls | 7480 | 257 | 290 |

The three worst supply lines are **headers**: `imageop.h` feeds `widgets/togglebutton.h` +
`control/settings.h` to **122 consumers (90 IOPs)**; `blend.h` feeds three GUI headers to
18; `masks.h` feeds `widgets/draw.h` + `control/control.h` to 14.

## The five knots, and what the deep-reads settled

**`dt_develop_t` (develop.h:158-507) is three objects.** The classification is member by
member in the survey; the highlights: the history/iop/forms/transient blocks and
`image_storage` are the params engine; the three pipes and the worker loop are pipeline; the
toolbar structs (GtkWidget*, "yes, having gtk stuff in here is ugly", h:455), color_picker
("GUI-only", h:384), proxy hooks, `form_gui`, `image_surface` are GUI. `gui_attached` itself
is the axis-split sentinel — it disappears when the GUI half is a separate object whose
existence IS the flag. **Two members are dead**: `histogram_pre_tonecurve/levels`
(allocated, freed, zero readers tree-wide) and `loading_cache` (declared, never referenced).
Delete, don't migrate.

**The hardest single object is the anonymous `dev->roi` struct (develop.h:185-246).** It
fuses backend facts (`raw_width/raw_height/processed_*` — needed headless; mistaking them
for GUI state was exactly the raw_width==0 masks bug in CLAUDE.md), GUI viewport state
(`scaling/x/y/border_size`, ppd-relative), and derived pipeline inputs (`preview_*`,
`natural_scale`) — **unlocked and unversioned**, written by the GUI thread
(`dt_dev_get_thumbnail_size`, c:354-394) while the worker plans ROIs from it (c:469-487).
The sanctioned pattern to copy already exists one field above:
`mask_preview_settings_revision` (h:178), an atomic revision the GUI bumps and the pipe
hashes. The split needs three artifacts: a backend image-geometry record, a GUI viewport
object, and a versioned ROI-request channel.

**`dt_iop_module_t` is a params/history record + a GUI surface + a bidirectional pipeline
mailbox** — GUI-owned request flags folded into the pipe hash, pipeline-written picker
results on the GUI object. Three specific facts make its cut tractable:
`widgets/togglebutton.h` exists for ONE member (`off`, h:346) whose ~25 external users all
cast via `GTK_TOGGLE_BUTTON()` anyway — declare it `GtkWidget*` and the include falls off
122 consumers. `control/settings.h` exists for one typedef (`dt_dev_operation_t`,
settings.h:37) — relocate it. And the `dev` backpointer's own FIXME (h:296-302) already
prescribes the inversion: backend has `pipe->dev`, frontend has `darktable.develop`;
deleting the member is the forcing function. One behavioural trap:
`dt_iop_commit_params` rehashes from **live** `module->dev->forms` (c:1965) — thread the
caller's snapshot (`pipe->forms`/`hist->forms`) instead, per the CLAUDE.md threading rule.

**`dev_history.c` is 92% engine already** (3370 of 3770 lines) and its GUI content is
enumerable: 4 notify-style raises (already have `src/history/notify.h` as the template),
the freeze/thumbnail-size wrappers, `dt_dev_history_gui_update` (whole function moves),
the pending-commit throttle queue (c:981-1164 — GLib main-loop input coalescing, GUI by
nature; headless timeout is already 0 so the engine path is `_commit_history_item_now`
directly), and one genuinely hard function: `_check_deleted_instances` (c:2713-2824)
destroys GTK widgets **inside `history_mutex` as writer**, mid-loop, while parking modules
in `dev->alliop` because an in-flight pipe may still hold them. The cut: engine emits a
removed-instances list, GUI destroys after unlock. Also: `dt_undo_history_t` smuggles
presentation state (`mask_edit_mode`/`request_mask_display`, c:568-569) through engine undo
records. And `gui/presets.h` (dev_history.c:77) supplies **no symbol** — auto-presets go
through the repository (c:1826); deletable after the four-config supply-line check.
**The thread contract (history_mutex writer/reader sites, the `_ext`-means-locked
convention, COW protection of slow readers) must survive every move intact.**

**`masks.h` mixes three things one vtable deep.** `dt_masks_functions_t` (h:315-377)
carries pipeline (`get_mask*`), GUI (`mouse_*`, `post_expose`, `draw_shape`) and data
members in one per-shape table — that is the only reason every IOP that wants `get_mask`
sees cairo and GTK. The split is a parallel `dt_masks_gui_functions_t` registered per shape
type. `control/control.h` in masks.h supplies **zero symbols** (verified) — pure supply
line, deletable with the consumer audit. Two traps: the spline sampler
(`_polygon_get_pts_border`, ~700 lines, twin in brush.c) serves BOTH rasterisation
(`TRANSFORM_DIR_BACK_INCL`) and GUI display (`TRANSFORM_DIR_ALL`) from the same recursion —
it is backend geometry despite cairo living nearby, and cutting it by "cairo means GUI"
would be wrong; and `dt_toast_log` fires from *inside* the value-mutation path
(polygon.c:1610 etc.) — mutators should return the new value, callers toast.

**`blend.h` cuts at line 264, and the pipeline files need inversions, not splits.**
Everything from `dt_iop_gui_blendif_colorstop_t` down is GUI (~60 GtkWidget fields; the
gradientslider and collapsible_section includes each exist for one struct member) → new
`develop/blend_gui.h`. TRAP: the blend-mode name tables are extern'd in blend.h but
**defined in blend_gui.c:87 while backend `supervisor.c` consumes them** — definitions move
to blend.c or headless linking breaks. The five pipeline files' upward reaches are three
families: `dt_control_log` from worker threads (tiling.c and blend.c include control.h for
that single symbol), the progress banner + forced redraws in pixelpipe_hb.c, and the
cache-wait manager. Two includes are vestigial NOW: `gui/color_picker_proxy.h` in
pixelpipe_hb.c:80 (zero symbols) and `widgets/label.h` in pixelpipe_raster_masks.c — the
latter only to strip GTK mnemonics from toast text, so it vanishes with the message
inversion.

**The hardest knot overall: the cache-wait manager** (dev_pixelpipe.c:60-1020). The
pipeline is both publisher (pixelpipe_hb.c:1520) and subscriber of the *control-owned*
`DT_SIGNAL_CACHELINE_READY`, with GUI cursor lifecycle dispatched via
`g_main_context_invoke` from pipeline code, and the deadlock-avoidance re-raise
(pixelpipe_hb.c:1503-1522) threading GUI request state through the recursion. The
ready-notification transport belongs to the **cache layer**, not control/; waiter
bookkeeping stays pipeline; cursor/redraw reaction becomes a GUI subscriber. Three
inversions that must land together (doc/pipeline-cache.md §8 spans both files and threads).
Second place: `_seal_opencl_cache_policy` querying the live GUI picker (c:370) — cache
policy already had one documented erasure bug, so any inversion must preserve exact
per-piece `cache_output_on_ram` outcomes.

## Tranches, in dependency order

Every tranche is a PR-sized unit that builds and passes the gates on its own. T1–T3 are
mechanical with the surveys in hand; T4+ need design.

* **T0 — deletions** (hours): the two dead `dt_develop_t` members; `gui/presets.h` from
  dev_history.c; `gui/color_picker_proxy.h` from pixelpipe_hb.c. Each after the
  supply-line check across all four configs.
* **T1 — header purge** (days; the largest fan-out win): `imageop.h` drops both upward
  includes (`off` → GtkWidget*, `dt_dev_operation_t` relocates); `blend.h` splits at :264
  into `blend_gui.h` (+ name-table definitions blend_gui.c→blend.c); `masks.h` drops
  control.h outright and moves `dt_masks_form_gui_t` + GUI vtable + draw.h into a new
  `masks_gui.h`. After T1, **90 IOPs stop receiving GTK through develop headers**.
* **T2 — pipeline message/notification inversions**: `dt_control_log` → a pipe-registered
  message handler (the `history/notify.h` shape, third time); progress + redraw →
  changed-notifications the darkroom view subscribes to. tiling.c and blend.c lose
  control.h entirely; pixelpipe_raster_masks.c loses widgets/label.h.
* **T3 — GUI halves move out of engine files**: `dev_history_gui.c` (gui_update, throttle
  queue, undo GUI tail, instance-teardown subscriber); masks.c → masks_gui.c (~3000
  lines) + per-shape `_gui` halves (events/menus/post_expose; the spline sampler stays
  backend); imageop.c's ~1790 GUI lines join imageop_gui.c. Established pattern, biggest
  diffs — each file its own PR, each verified the exif-split way (byte-conservation cut +
  decoded-pixel A/B where pixels are touched).
* **T4 — struct splits**: `dt_develop_t` → dev core + darkroom-view object (the `roi`
  three-way split with the versioned ROI channel); `dt_iop_module_t` → params core +
  `dt_iop_module_gui_t` (the `common_fields`/`dt_gui_module_t` casting pattern already
  demonstrates the shape); delete the `dev` backpointer per its FIXME. This is where
  `gui_attached` dies.
* **T5 — the cache-wait ownership move** + progress/backbuf taps: cache layer owns the
  notifier; `_seal_opencl_cache_policy` reads pipe-owned flags. Gate with the
  pipeline-cache regression discipline (issue #817, #1069 lineage).
  **Half of that last clause is blocked on T6 and was measured, not guessed.** The seal's two
  GUI-owned inputs are `dev->gui_module` and `dev->color_picker.module`. Having the GUI
  *publish* them — the T4b viewport shape — means `gui/`(4) calling into `develop/`(5), and
  `tools/check_layering.sh` fails that (+1, 187 → 188) because today the pipeline sits ABOVE
  the GUI. The publication is legal only once T6 inverts the table, and is then the natural
  first use of it. What T5 can do meanwhile is sample both facts ONCE per seal instead of
  per node, and bring the predicates that read them home from `gui/`.
* **T6 — re-stratify**: flip the layer table (pipeline + params engine below gui), then
  the IOP question — each IOP is operator + panel in one file; the X-macro API already
  separates the hook groups (survey classified all ~60 hooks into the three axes), so an
  IOP splits mechanically once T1/T4 land. That is the Qt door.

## What this means for `src/history` — the sequencing answer

`src/history` as merged in #1130 (records, presets, snapshots, notify, vocabulary — layer
1, names no develop type) is **compatible with every tranche above** and already supplies
the inversion template T2/T3 reuse. Nothing in it needs revisiting.

But the *rest* of point 3 — sealing `history_actions.c`, `styles.c`'s apply half,
`xmp_sidecar.cc`, `history_merge.c` — should **wait**. Those files are the params/history
engine axis of THIS decomposition: `dev_history.c` (3370 engine lines) is their natural
sibling, and the module boundary that seals them is the one T1–T4 create. Sealing them now,
against today's layer table where develop sits above gui, would draw a boundary this work
immediately redraws.

So: merge #1130 as it stands; stop growing `src/history`; start T0+T1 — they are the
cheapest cuts with the largest fan-out, they are fully specified by the survey, and nothing
in them blocks on a design decision.

## T4b field survey: what `dev->roi` actually is, measured

The tranche list above assigns `dev->roi` three artifacts. Before writing them, every one of
its 416 access sites was traced to a writer, a reader and a thread — six independent passes,
each classification then attacked by a checker whose brief was to find one counter-example.
Six of the checker's attacks succeeded, and they are the reason this section exists: the
first-pass classification was right about *meaning* and wrong about *reach*, which is the
same mistake that shipped the `raw_width == 0` masks bug.

**The site count is larger than a grep suggests.** `grep 'dev->roi'` finds 349; the real
figure is 416, because the struct is also reached through `self->dev->roi`,
`mask_gui->dev->roi`, `pipe->dev->roi`, `module->dev->roi` and `d->dev->roi`. Of those, 57
are writes, and they sit in just 17 functions — `_zoom_preset_change()` (12),
`_change_scaling()` (6), `mouse_moved()`/`key_pressed()` (4 each), `dt_dev_get_thumbnail_size()`
(4), `dt_dev_reset_roi()` (4). That concentration is what makes the D1=A2 setter API
tractable: seventeen call sites to reroute, not fifty-seven.

**The classification, after the checkers.** `raw_width`/`raw_height`/`raw_inited` are backend
and reached headless (confirmed: `_dt_dev_mipmap_prefetch_full()` still writes all three
unconditionally, the CLAUDE.md fix intact). `processed_width`/`height` survived the
refutation attempt as GUI-populated: nothing reads them with `gui_attached == FALSE`, and the
backend keeps its own copy in `pipe->processed_*` (pixelpipe_hb.h:239) — the mask coordinate
path reaches absolute positions through the virtual pipe, not through these. `scaling`, `x`,
`y`, `border_size`, `orig_width`, `orig_height`, `gui_inited` are viewport; `preview_*`,
`natural_scale`, `output_inited` are derived. `main_width`/`main_height` were neither: zero
readers tree-wide, deleted in T4b-1.

**The hazard is a torn pair, not a stale scalar.** `x` and `y` are written as two separate
statements at every one of the eight write sites, and read as two separate statements by
`_update_darkroom_roi()` on the worker thread — so a frame can be planned from half the old
pan and half the new one. `preview_width`/`preview_height` have the identical shape. Any
channel that only guarantees freshness, without publishing the tuple atomically, leaves this
in place.

**Three defects the survey found, which are not bookkeeping:**

* `iop/finalscale.c`'s `commit_params()` reads `pipe->dev->roi.scaling` and `natural_scale`
  to decide `piece->enabled` — on the pipeline thread, unsynchronised, and for the export and
  thumbnail pipes as well, where the dev is headless and those fields hold their `calloc`
  zero. The later clauses of the predicate decide those two pipe types on their own, so the
  read is load-bearing only for the darkroom pipes, which are exactly the ones that race.
* The drawlayer paint thread reads viewport state and drives the virtual pipe. Chain:
  `_drawlayer_worker_main` (a pthread, worker.c:1378) → `process_sample` →
  `_backend_worker_process_sample` → `_process_backend_input` →
  `dt_drawlayer_paint_interpolate_path` → the `layer_to_widget` callback →
  `dt_dev_coordinates_image_norm_to_widget()`, which reads `roi.orig_width`/`orig_height`
  (develop.c:1100-1101) and composes the GUI's own geometry — at the time of writing, a
  pixel-less pipe owned by the GUI main thread; since the geometry service replaced it
  (`doc/geometry-service.md`), the GUI-thread-only geometry chain. Either way it runs once per
  dab, so a window resize or panel toggle during a stroke races it.
* `dt_focus_draw_clusters()` (gui/dtgtk/focus.h:288) reads
  `dt_dev_get_global()->roi.border_size` from `_get_image_buffer`, a `dt_control_job` body
  (thumbnail.c:657). A lighttable thumbnail render job thus reads darkroom viewport state
  through the global, from a view that is not darkroom. After the split that viewport may not
  exist, so this call site needs rewriting rather than re-pointing.

## T4b, as landed

`dev->roi` is gone. The anonymous struct that fused eighteen members of backend fact, GUI
viewport state and derived values -- unlocked, unversioned, written by the GUI thread and read
by the pipeline worker -- is three published records, in five PRs (#1140, #1141, #1143, #1144).

* **`develop/dev_geometry.{h,c}`** -- `raw_*`, `processed_*` and their two validity bits. Every
  dev has one, headless included, which is the half CLAUDE.md's `raw_width == 0` bug was about.
* **`develop/dev_viewport.{h,c}`** -- widget allocation, borders, box, zoom, centre. Allocated
  only for a `gui_attached` dev; that NULL *is* the flag, replacing `roi.gui_inited`, and a dev
  without one reads a neutral state (`scaling 1`, centre `0.5/0.5`) that reproduces exactly what
  `dt_dev_reset_roi()` used to leave everywhere.
* **`develop/dev_roi_request.{h,c}`** -- everything derived from the other two, published as one
  record and latched onto each pipe once per worker iteration.

All three publish under the same single-writer seqlock, and each mutator publishes a whole
state, so no reader can observe half of one.

**What the tranche actually fixed** is narrower and sharper than "the struct was messy". `x` and
`y` were written as two statements at all eight write sites and read as two by the worker, so a
frame could be planned from half the old pan and half the new -- internally consistent, correctly
hashed, undetectable downstream. The ROI planner then made four separate reads of the record, so
even a coherent record could not stop one frame being planned from two different reads. The latch
closes that.

**Three defects the survey found, none of them bookkeeping**: `finalscale` consulting GUI zoom on
the pipeline thread, including on headless export pipes where it read a `calloc` zero (fixed);
the drawlayer paint pthread reading viewport state and driving the GUI-owned virtual pipe, once
per dab (documented, not fixed); and a lighttable thumbnail job reading the darkroom's
`border_size` through the global (documented, not fixed).

**The step that was planned and not taken.** The tranche was to end by folding the request's
generation into the FULL pipe hash. It does not, and `dev_pixelpipe.c` says why at the hash site:
every field of the record already reaches that hash through `piece->roi_in`/`roi_out` -- zoom and
fit scale as `roi.scale`, centre as `roi.x/y`, box and processed size as the dimensions -- so the
fold would add no discriminating power and could only over-invalidate, rekeying the whole cache
chain for republications that produce identical ROIs. Content, not version, is the right key
there; the contrast with `mask_preview_settings_revision` thirty lines below, which *is* a
revision fold precisely because its state reaches no ROI, is the part worth remembering.

**What the pixel harness was worth here**: nothing, and that is the calibration to carry into T5.
The decoded-pixel export A/B printed 0 differing pixels through every real defect found in this
tranche -- a `memcmp` over struct padding, a self-cancelling invalidation, an inverted initial
value, a NULL deref hoisted above its guard, and a cross-thread torn read. Every one came from a
reviewer or from reading the code. For work whose subject is cache keys, thread ownership and
validity flags, the A/B confirms "rendering still works" and nothing more.

**Left behind, deliberately**, each a behaviour change rather than a relocation: `_change_scaling`
keeps its own bounds/revert arithmetic instead of being absorbed into a `set_zoom()` with an
anchor; the navigation draw handler keeps the centre clamp it performs as a paint-path side
effect; `dev_toolbox` keeps the `orig - 2*border` arithmetic the viewport should own; `focus.h`
still reads the darkroom's border from a lighttable job; and the geometry setters do not publish
the request themselves, which is what would close the stale-`processed_*` window for producers
other than the darkroom's own path.

## T6 field survey: the flip is not blocked by the IOP split, and never was

The tranche list says "flip the layer table, **then** the IOP question". Measurement inverts it.

Pricing the flip first (`develop` 5→3.4, `iop`/`imageio` 6→3.6), before `widgets/` moved:
**187 → 580, +393 new violations**, of which `iop→widgets` 231, `iop→gui` 102, `develop→widgets`
50, `imageio→widgets` 26, `develop→gui` 18, `imageio→gui` 3 — 393 edges over 113 files, median 3
each, and 84 of `src/iop`'s 95 top-level `.c`/`.cc` touching `gui/` or `widgets/`. That is the
number behind "78% of the cost is IOPs", and it is an artifact of where `widgets/` sat.

`widgets/` was at layer 4 beside `gui/` on the assumption that "GTK" and "the application's GUI"
are one layer. Its own includes reach only `system/`, `common/`, `metadata/` and `pixel/`
(`focus_peaking.c` → `pixel/eigf.h`), so it is a leaf library written against GTK. Moving it to
**2.5** — above `pixel/`, below `control/` — creates ZERO new violations and removes three
(`control/ → widgets/`): **187 → 184**. At 1.5 it costs one, because `pixel/` would then be above
it. The four `widgets/ → osx/osx.h` edges never counted either way: `osx` is absent from `LAYERS`,
so `layer_of()` returns `None` and the edge is skipped in both directions.

That one line retires 307 of the 393. What remains is **123 edges, and 96 of them land in four
headers**: `gui/presets.h` 35, `gui/color_picker_proxy.h` 33, `gui/screen_metrics.h` 25,
`gui/application.h` 19. So what the flip needs is not "split 84 IOPs" but "stop operator code
reaching four headers", three of which are misfiled by their own `#include` lines.

**The knife edge.** Flip with no relocation: **+86** (184 → 270). Flip with all three of
`presets`, `color_picker_proxy`, `screen_metrics` re-homed: **185**, i.e. free. Drop any one and
it is +25 minimum — measured `presets`+`screen_metrics` 214, `presets`+`picker` 210,
`picker`+`screen_metrics` 216. There is no partial credit and no "flip now, tidy later".

**Do not bank the `develop` flip early.** `develop`→3.4 *alone* measures **166** — a fall of 18
for one table line, the cheapest-looking commit on the board. It then makes the `iop`/`imageio`
flip a **+19 rise** against that new baseline, which the ratchet refuses. One flip, after the
relocations.

**Only one of the three relocations is honest on its own terms.** `gui/screen_metrics.{h,c}`
contains zero GTK references and includes only `system/surface_scaling.h`: genuinely misfiled.
`gui/presets.c` has 179 GTK references and its header 8; `gui/color_picker_proxy.h` has 5. Moving
those two *down* would drag GTK below `develop/` to make a number fall — the same trade the
`widgets/` move makes, and the reason the toolkit ratchet below exists. They are relocations of
*declarations*, not of GTK code.

**Dependency order and toolkit-freedom are different properties, and only the first was gated.**
Making 307 edges downward moves no GTK out of the pixel engine. A metric improving while the
thing it stood for does not is worse than no metric, so `tools/check_module_boundaries.sh` gained
a fourth rule counting files that *name* `GtkWidget`, `GdkEvent`, `cairo_t`, a `GTK_`/`GDK_`
macro, or a toolkit header: `develop` 20/73, `iop` 97/164, `imageio` 13/55, with `pixel` 0/39,
`caches` 0/11, `database` 0/31 pinned at zero. Comments are stripped first — this tree's doc
comments discuss GTK constantly — and the gate is mutation-tested both ways.

That gate has a stated blind spot: `src/iop/CMakeLists.txt:5-6` force-includes `iop/iop_api.h`
into every IOP translation unit and `iop_api.h:44-45` includes `<cairo/cairo.h>` and
`<gtk/gtk.h>`, so **no file partition yields a toolkit-free IOP object** until that is dealt
with. `include_graph.py` cannot see it either: `INCLUDE_RE` matches the quote form only, so
angle-bracket includes are outside the graph entirely.

**A live defect found while surveying, unrelated to T6.** `iop/toneequal.c:621` `sanity_check()`
is reached from `process()` via `toneeq_process()` — the pipeline thread — and there writes
`self->enabled`, calls `dt_dev_add_history_item()`, and calls `gtk_toggle_button_set_active()` on
`self->gui->off`. A GTK call off the GUI thread plus a history write from the pipeline, against
the rule in CLAUDE.md that history is the only thread-safe interface between the two.
