<!-- Provenance: every finding carries the commit it was established against. -->

# Masks: forms, history and the module enclosure

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> How shapes are stored, shared and persisted, and how far the enclosure of `src/develop/masks` has got.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## "Mask" and "shape" name two different levels, and the tree does not yet say so

*Found `aded0a5493`, 2026-09-12.*

A **mask** is what a module blends with: the raster mask, the drawn mask, the parametric mask.
A **shape** is the level below the drawn mask — a circle, an ellipse, a brush, a polygon, a
gradient, and a group of those. So a module has one drawn mask, that mask is a group, and what the
group holds are shapes.

New code uses that vocabulary. `form` is NOT a third level: it is the legacy darktable spelling of
a shape, it is what the struct is called (`dt_masks_form_t`), and it stays — renaming it is not
part of this rule.

**The existing tree is not consistent with this yet**, and the sweep that makes it so is deferred
to a commit of its own. Until then, read every existing `mask` in an identifier as saying nothing
about which level it means, and do not take a name as evidence: `_module_create_own_mask()` makes
a group, `mask_form` is usually a shape. What IS already right, and must stay, is `mask` wherever
it genuinely names the blending level — `blend_params->mask_id`, `raster mask`, `parametric mask`,
`request_mask_display`, and the persisted `masks_history` table and its XMP key, which are a
storage format and may never be renamed at all.

**`develop/masks/masks.c` holds both levels and should be split along the same seam, in the same
pass.** Its 2000 lines are two files wearing one name, and the seam is already visible in the
layout rather than something that has to be invented:

- the SHAPE half — a shape's own life and nothing about who holds it: `dt_masks_create()` /
  `_ext()` / `dt_masks_free_form()` / `dt_masks_dup_masks_form()`, `dt_masks_form_duplicate()`,
  `dt_masks_duplicate_points()`, `dt_masks_form_move()`, the gravity centre, the rasterising entry
  points (`dt_masks_get_points_border()`, `dt_masks_get_area()`, `dt_masks_get_mask()` /
  `_roi()`), the sample-grid helpers, `dt_masks_form_get_own_hash()`, `dt_masks_version()` /
  `dt_masks_type_name()`, and the ~360-line `dt_masks_legacy_params_v1..v6` migration block, which
  is per-shape params and nothing else;
- the GROUP half — membership, i.e. what a module's drawn mask is made of: the whole
  `dt_masks_group_*` family and its `_resolve_member()` / `_find_holder()` / `_find_in_group()`
  helpers, `_group_create()` / `_group_from_module()` / `_set_group_name_from_module()`,
  `dt_masks_form_duplicate_in_group()`, `dt_masks_form_delete()`, `dt_masks_iop_use_same_as()`,
  `dt_masks_copy_used_forms_for_module()` and the unused-shape sweep.

Two practical notes for whoever does it. The obvious name for the second file, `masks/group.c`, is
TAKEN — its 1000 lines are the group shape's own `dt_masks_functions_t` table: the rasteriser that
folds the members with the combine operators, plus the group's mouse/key/expose handlers. That is
the group at the SHAPE level (a group is a shape like any other, which is exactly why it has a
functions table), not the membership graph; the membership half should take the name of the public
header it implements, `masks_group.c`. And the
DB/XMP persistence (`_read_mask_row()`, `dt_masks_read_masks_history()`,
`dt_masks_write_masks_history_item()`) is a third thing again, belonging with neither: it
serialises a shape's blob but is driven by the history, so it is the natural first piece to move
out and the one that settles whether the split is two files or three.

## Forms are refcounted, not deep-copied

*Found `137fed9830`, 2026-07-10.*

`dev->forms` (`dt_develop_t`) is the live, mutable `GList` of every mask shape and group
(`dt_masks_form_t*`) in the current image. Groups don't nest forms directly — a group's `points`
is a list of `dt_masks_form_group_t` entries (`{formid, parentid, state, opacity}`) referencing
sibling forms in the same flat `dev->forms` list by ID.

Every history commit that touches masks used to deep-copy the *entire* `dev->forms` list into
`hist->forms` (`dt_dev_history_item_t`), even when only one shape on one module changed. Forms
are now refcounted (`dt_masks_form_t.refcount`, `src/develop/masks/masks_history.{h,c}`) instead:

- `dt_masks_snapshot_current_forms()` takes a reference on each current `dev->forms` element
  instead of copying it. Multiple `hist->forms` snapshots (and `dev->forms` itself) can share the
  exact same `dt_masks_form_t*`.
- `dt_masks_cow_touch(dev, form)` is the copy-on-write gate: before *mutating* a form (move,
  resize, remove a group member...), check its refcount. If it's 1 (only `dev->forms` holds it),
  mutate in place. If it's shared (an undo/redo or history snapshot also references it), clone it
  first, splice the clone into `dev->forms` in place of the original, and mutate the clone —
  never mutate a form that might be observed by a frozen snapshot. Every mutation call site
  (mouse/keyboard event dispatchers in `masks.c`, `dt_masks_form_delete`, group add/move/ungroup,
  `blend_gui.c` group operations, the shape-manager panel in `libs/shape_manager.c`) must route through
  this before touching `form->points` or any other field. `dt_masks_cow_touch` also re-points
  `dev->form_gui->form_visible` if it was the form that got cloned — that's the only other raw
  `dt_masks_form_t*` cached outside `dev->forms`.
- `dt_masks_replace_current_forms()` swaps `dev->forms` wholesale (used when history navigation
  rebuilds it) by releasing the old references and taking new ones — never a raw deep copy.
- `pipe->forms` (the pixel-pipeline's own snapshot, taken once per `dt_dev_pixelpipe_process()`
  call, `pixelpipe_hb.c`) is shared by reference the same way. It has exactly one real consumer
  (`iop/retouch.c`, read-only), so no COW gate is needed on that side — `dt_masks_cow_touch`
  already guarantees a GUI-side edit clones instead of mutating a form an in-flight pipeline run
  is holding.

## Each module owns its mask group; what modules share is a shape

*Found `595ebac668`, 2026-09-02.*

`blend_params->mask_id` lives inside each module's own params blob, so several modules naming the
same group is *representable* — but it is not what anything builds, and code here should not
create it. A module owns ONE mask group of its own, named "Group <module>", and a shape or shape
group used by several modules is nested as a member of each of their masks.

That name is the masks module's own convention, not the caller's: `dt_masks_group_name_for_module()`
builds the string (published for the two consumers that have no form to write it into — the blend
panel's name-entry placeholder, and what it puts back when the user empties the entry), and
`dt_masks_group_set_name_from_module()` writes it, **id-keyed**, so the rename copies on write like
every other group mutation. It is NOT translated: it goes into the form, the database and the XMP,
so it may not depend on the language the group happened to be created in. The two creation paths
(`libs/shape_manager.c`'s `_module_create_own_mask()`, `develop/blend_gui.c`'s
`_blendop_masks_group_create()`) name the group AFTER `dt_masks_append_form()`, since an id resolves
against `dev->forms` and nowhere else — and they re-resolve the group by id afterwards, because a
fresh group already carries two claims (its creator's and the list's), so the touch inside the
setter genuinely clones it and the pointer they created would otherwise be an orphan that
`dt_masks_cow_touch()` leaves alone and every later mutation writes into the void.

That separation is what keeps the modules independent. A module's own mask carries its own combine
operators, opacities and member order, so attaching the same shape group to a second module cannot
disturb the first, and detaching it from one removes the membership from that module's mask alone.
Pointing several modules at one mask group would make every one of those settings — and every
detach — common to all of them. `_tree_row_assign_to_modules()` (`libs/shape_manager.c`) is the
path that creates a module's mask when it has none, and `dt_iop_gui_blend_set_drawn_mask_group()`
(`develop/blend_gui.h`) is the one that points `mask_id` at it, raises `DEVELOP_MASK_ENABLED |
DEVELOP_MASK_SHAPE`, refreshes the raster-mask source table and repaints whatever of the blend GUI
exists. It tolerates a module the user has never expanded (no `dt_iop_gui_blend_data_t`) and
commits nothing, so a caller wiring several modules commits per module and once for the forms.

There is no back-reference from a group to the modules using it, and there must not be one: the
answer is derived by walking `dev->iop`, which is what `_modules_owning_group()` does. Caching it
would buy nothing over ~80 modules and would cost an invalidation problem, since `dev->iop` is
rebuilt on module add/remove and on history navigation — stored raw module pointers would dangle.
`_module_owning_group()` returns the first of them, which is all a tree row storing one module can
show; anything asking *who renders this group* wants the list.

## The same shape applied twice in one mask is legal, and not always a no-op

*Found `de50cecde7`, 2026-09-02.*

A module renders one group, that group can nest others, and nothing stops a shape from sitting in
both. Whether the second application changes anything depends entirely on the combine operator
(`develop/masks/group.c`): union is `max(dst, src·opacity)` and intersection `min(b1, b2·opacity)`,
both idempotent — but difference is `b1·(1−b2)`, so applying it twice gives `b1·(1−b2)²`, and
exclusion does not settle either. **So a duplicate instance must never be greyed out or refused as
redundant**: the panel would claim an absence of effect the pipeline does not honour. Mark it and
leave it alone. `dt_masks_group_first_use()` answers which application the later ones are read
against, walking in compositing order — a group's members in their own order, descending into a
member group at the position that group sits at.

Compositing order is that walk, forward: `dt_masks_group_get_mask()` fills its buffers walking
`points` forward and folds them in the same index order, so the topmost member is the base and each
later one combines onto the result of those above it. A list that does not show that order does not
show what the mask does — which is why the Drawn tab's mask tree is built in one pass
(`_blendop_masks_group_tree_append`). Its *available shapes* list below deliberately keeps groups
first instead: that one is a catalogue to pick from, not a rendering order.

Note `nb_ok` is only incremented for members that actually rasterize something: a shape that draws
nothing (`DT_MASKS_RASTER_EMPTY`) occupies a row in the list but no rank in the composition, so the
displayed rank and the effective one can differ by one.

## The unused-shape sweep's used-set is not a subset of the snapshot it sweeps

*Found `51b5f9dbd3`, 2026-08-31.*

"Delete unused shapes" in the shape manager (`libs/shape_manager.c`, `dt_masks_cleanup_unused()`) keeps a
form when some history entry's `blend_params->mask_id` names it, or names a group that
transitively contains it. Those ids are collected by walking history from the bottom up, and they
are **not** a subset of the `hist->forms` snapshot being swept: a module whose drawn mask was
since dropped keeps its old `mask_id` in every history entry it ever wrote, and no form in a
later snapshot answers to it. The set is therefore unbounded with respect to the snapshot, and
must live in a hash set — `_cleanup_unused_recurs()` sizing a table on `g_list_length(forms)`
filled it with ids that match nothing and ran out of slots before the one live group's membership
was walked, deleting shapes from the tail of that group inward while the module was still using
them. Measured on a 20-step history: four departed groups (`colorbalancergb`, two `toneequal`, an
older `exposure`) took four of the eight slots an 8-form snapshot allowed, the live group and its
first three members took the rest, and the group's last two shapes were swept.

Marking an id and recursing into it are now the same step, which also bounds a group that
contains itself through a chain of member groups — the old code broke out of its scan on an
already-seen id but recursed regardless, so such a cycle did not terminate.

A swept form's snapshot reference is **handed to `dev->allforms`**, not released: a form read back
from `masks_history` is built by `dt_masks_create()` and its snapshot membership is its only
claim, so unref-ing at that point would free an object `dev->forms` may still hold by address. One
`allforms` entry per transferred claim is what keeps teardown balanced; do not "fix" the missing
unref.

The sweep is history-wide by necessity: `main.masks_history` stores one row per (history step,
form), so a shape only really leaves the database once **every** snapshot has stopped naming it —
hence the rewrite of every `hist->forms` in place, after which `dev->forms` is re-pointed at the
topmost surviving snapshot.

That is still undoable, and the menu handler opens the undo record itself, **before** the sweep.
`dt_dev_add_history_item()` opens one of its own, but by then every snapshot has been rewritten
and the "before" state it would capture is the swept one; `dt_dev_history_undo_start_record()`'s
depth counter makes the inner pair a no-op, so the recorded pair spans the whole operation. What
makes the restore work is that `dt_history_duplicate()` copies each item's forms **list**
(`g_list_copy` plus one reference per form) instead of aliasing it: the record owns its own
cells, the sweep's `g_list_remove()` on the live items cannot reach them, and every swept shape
stays alive as long as the record holds it. `_pop_undo()` rewrites the database from the restored
history, so the `masks_history` rows come back too. Measured round trip on one image:
65 rows / 23 forms → 70 / 22 after the sweep → 65 / 23 after undo, the swept shape restored and
nothing else moved.

That rewrite is in-memory only. `main.history` and `main.masks_history` are deleted and
re-inserted wholesale from `dev->history` by `_write_history_from_state()`, and nothing on this
path triggers it — so the menu handler commits a mask-manager history entry
(`dt_dev_add_history_item(dev, NULL, FALSE, TRUE)`) after the sweep, the way every other forms
mutation in `libs/shape_manager.c` must. Without it the swept shapes stay in the database and come back
on the next read, and the pipeline never resyncs. Measured on a 20-step history: 60 rows / 24
forms before, 58 / 22 after, with exactly the two orphans gone and one extra history step.

The sweep also takes `dev->history_mutex` as **writer** for its whole span. The async DB write
job walks `hist->forms` with the lock released; its snapshot references keep each history *item*
alive but say nothing about the list cells `g_list_remove()` frees under it. Order is
`history_mutex` outer, `masks_mutex` (taken by `dt_masks_replace_current_forms()`) inner — the
same way a history commit takes them.

## A form mutation that never reaches a history commit is invisible to undo/redo

*Found `137fed9830`, 2026-07-10.*

`dt_dev_add_history_item_ext()` (`dev_history.c`) is the only place that turns the current
`dev->forms` state into a `hist->forms` snapshot, and only when
`dt_iop_module_needs_mask_history(module)` is true for the committing module. **Any code path
that mutates `dev->forms` (directly or via `dt_masks_form_delete`/group helpers) must be followed
by a `dt_dev_add_history_item()` call**, or the mutation only ever exists in live memory.

Undo/redo (`_pop_undo`, `dev_history.c`) replaces `dev->history` with a duplicate of the
recorded `before_snapshot`/`after_snapshot` (`dt_history_duplicate`, itself ref-sharing) and calls
`dt_dev_pop_history_items_ext()`, which rebuilds `dev->forms` from the `hist->forms` of the
**last history item that actually has one** — walking backwards over items with
`hist->forms == NULL`. If a mutation was never committed, every subsequent history navigation
silently falls back to whatever was last actually recorded and the live edit is lost. Confirmed
bug instances, found by auditing every handler in `libs/shape_manager.c` for a trailing
`dt_dev_add_history_item()`/`_add_masks_history_item()` call: `_tree_delete_shape` (delete),
`_tree_moveup`/`_tree_movedown` (reorder inside a group — silently lost on next undo/redo), and
`_tree_duplicate_shape` (the duplicate was also never attached to the source shape's parent group
via `dt_masks_group_add_form`, so it was an orphan on top of being uncommitted). All four are
fixed; audit any *new* handler in `libs/shape_manager.c` / `blend_gui.c` that mutates forms without a
trailing commit before trusting its undo/redo behavior.

## Same-thread rwlock reentrancy

*Found `137fed9830`, 2026-07-10.*

Committing masks more often surfaces a pre-existing, unrelated hazard: `dt_dev_pixelpipe_change()`
can be re-entered by the same thread while it already holds `history_mutex` as writer (a
history-commit path resyncing the virtual pipe mid-commit). glibc's default
`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` policy self-deadlocks such a thread as soon as a
second thread is queued for the write lock. Fixed by porting the same-thread recursive-writer
tracking that already existed in the `_DEBUG` build of `dt_pthread_rwlock_t`
(`system/dtpthread.h`: `writer` + `writer_depth` fields) into the release path too — a thread
that already holds the write lock cannot race itself, so letting it re-enter (as reader or
writer) is safe. `try*` locks keep their "is it locked by anyone?" probe contract and still
report busy on same-thread reentry, so callers relying on that semantic are unaffected.

## DB/XMP persistence still duplicates content per history step

*Found `137fed9830`, 2026-07-10.*

`masks_history` (SQL table) and `Xmp.darktable.masks_history[N]` (XMP array) store one
row/entry per (history step, formid), with no dedup — the in-memory refcounting above stops at
the persistence boundary, so a form shared unchanged across 100 history steps still gets its
points BLOB serialized 100 times on every commit (`dt_dev_write_history_ext` rewrites the whole
image's history + masks_history every time). Known, not yet fixed — see
`doc/masks_history_dedup.md` for the full design (developed on a dedicated branch, merged only
when Ansel 1.0 is prepared, per explicit instruction not to migrate any user's live DB
prematurely).

---

## The masks module is being enclosed, and the ratchet counts the way out

*Found `70f377b5d2`, 2026-08-28.*

`src/develop/masks` is not a closed module yet: **four** files outside it
(`develop/blend_gui.c` 34, `libs/shape_manager.c` 13, `iop/retouch.c` 12, `iop/spots.c` 9) reach
directly into `dt_masks_form_t` and friends, **one** place `malloc`s a masks type by hand
(`iop/spots.c:151`), and `->forms` is walked
as a plain `GList` all over `develop/`. The audit behind that is issue #1299; the plan is to drain
it phase by phase rather than in one break.

**`tools/check_module_boundaries.sh` section 9 counts the remaining leaks, and the counts may only
FALL.** Adding a new external struct access, a new includer of `masks.h`, a new hand-rolled
allocation or a new raw `->forms` walk fails CI. So does *removing* one without lowering the
baseline in the same commit — that is deliberate: it is what stops a phase from half-landing and
the ground being quietly given back later.

Two things about those counters a future editor should not "improve":

- **The member list is curated to names no other struct in the tree uses** (`formid`,
  `form_dragging`, `creation_formids`, …) and deliberately omits the ambiguous ones a masks form
  shares with everything else (`points`, `type`, `name`, `state`, `opacity`). It therefore
  undercounts — the full census found ~385 accesses by reading declarations, the gate reports
  **68** (16 of them writes) — and that is the right error for a gate. Widening it buys a bigger number and loses the
  property that every match is real.
- **There is no longer a known false positive, and all 16 write matches are real.** This
  previously said `supervisor.c`'s own event struct carried a `formid` that matched on the left
  of `e->formid = form->formid`, and invited discounting one write. That field was renamed to
  `form_id` when the file was converted (`bb79688107`, 2026-08-28), and `supervisor.c` now asks
  `dt_masks_form_get_info()` instead of reading the struct — which is also why the file count
  above fell from five to four and the member count from 102 to 68.

## Which blending spaces a module may use follows the profile each conversion uses

*Found `7279e56f50`, 2026-09-17.*

`dt_develop_blend_default_module_blend_colorspace()` (`develop/blend.c`) answers from the module's
`blend_colorspace()` alone: Lab for a Lab module, RGB (scene) for an RGB one, whatever its place in
the pipe.

`dt_develop_blend_colorspace_is_compatible()` answers "may this module blend in that space" from the
profile each conversion actually uses, not from the space's name. The Lab conversion always goes
through the pipe's working profile (`pixelpipe_cpu.c`), so Lab is offered only to Lab modules and
RGB modules whose position answers to the working profile. RGB (scene) takes the profile of the
module's own position -- input, working or output, TRC included -- in
`dt_develop_blendif_init_masking_profile()`, so it is valid everywhere, display-encoded data
included. Which profile a position answers to is decided once, by
`dt_ioppr_get_module_profile_stage()` (`develop/iop_profile.c`), which
`dt_ioppr_get_pipe_current_profile_info()` also picks its profile from: the blending code asks it
rather than comparing orders against `colorin`/`colorout` itself, so the menu cannot drift from the
conversion. An instance parked at `INT_MAX`, not placed in the pipe yet, counts as the working
space. Where a module sits against a tone mapper is deliberately NOT a rule:
which module tone-maps, if any, is the edit's choice, so RGB (display) stays offered before filmic
and RGB (scene) after it. The mask options menu (`_blendif_options_callback()`, `blend_gui.c`) lists
all three spaces and greys out the incompatible ones, except the one the edit already uses: an edit
loaded from an older version, or a module moved since, must still show its own space as selectable.

Generated presets name no blending space: `init_presets()` is handed the module TYPE, and only an
instance answers `blend_colorspace()`. `dt_gui_presets_add_generic()` stores
`DEVELOP_BLEND_CS_NONE`, and `_resolve_presets_blend_colorspace()` (`develop/imageop.c`, end of
`_init_presets()`) rewrites every such row with the space of an instance built without a pipe,
through `dt_develop_blend_resolve_default_colorspace()` (space plus the boost factors that space
starts from). NONE must not survive in the database: auto-applied presets are copied into history
rows and XMP as they are, and several readers compare blend params byte for byte against a module's
own -- `_process_history_db_entry()` clears an auto-applied preset's `DEVELOP_MASK_ENABLED` only on
an exact match with `default_blendop_params`, and the presets menu finds the active and the default
preset the same way.

## A mask or channel preview is converted back like any output; the conversion keeps alpha

*Found `b233b15241`, 2026-09-16.*

The blend authors a preview in the BLENDING space so that the ordinary conversion back to the
module's output space lands it where `gamma` expects it: Lab blending renders its grey channel
values in RGB and converts them to Lab on purpose (`blendif_lab.c`, the `is_lab` branch of
`blendop_display_channel`), and the mask itself rides in alpha. `pixelpipe_cpu.c` and
`pixelpipe_gpu.c` therefore convert a preview back exactly as they convert pixels. Skipping that
conversion for previews -- a raw copy of the blend buffer -- is correct only when the blending space
equals the module's, and flattens every preview otherwise: a Lab module blended in RGB (scene)
hands RGB greys to a downstream that reads them as Lab. The copy existed to dodge
`_transform_rgb_to_lab_matrix()` (`colorprofiles/iop_profile.c`) dropping alpha, which is now
preserved there as its Lab-to-RGB sibling and the OpenCL kernels already did. Any colorspace
transform a preview can cross owes the same: carry channel 3 through.

The picker behind the blending tabs converts the sampled buffer in two steps
(`_color_picker_convert_buffer()`, `common/color_picker.c`): first into the family the tab derives
from -- Lab for Lab/LCh, RGB for RGB/HSL/JzCzhz, the only step needing the profile -- then into the
tab's space. Enumerating direct pairs missed Lab -> JzCzhz, Lab -> HSL and RGB -> LCh, i.e. every
module blended outside its own family, and those tabs fell back to raw statistics of the wrong space.

Those pickers are fed by `DT_SIGNAL_CONTROL_PICKERDATA_READY`, the same signal as a module's own
picker, dispatched by `_iop_color_picker_data_ready_callback()` (`develop/imageop_gui.c`) to
`blend_color_picker_apply()` first. Every module that blends subscribes, not only the ones with a
`color_picker_apply` of their own: gated on the latter, the blend pickers of some forty modules
(atrous, sharpen, vignette, ...) sampled on every move and never showed it, refreshing only when
re-activated through another path.
