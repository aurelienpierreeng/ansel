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
