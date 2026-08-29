# Masks enclosure, phase P2: the group-membership API {#masks_enclosure_p2}

[TOC]

## Status

**Design only — no API code has been written yet.** This document exists so the design survives
between sessions and contributors, in the same spirit as `doc/masks_history_dedup.md`. Phases P0
and P1 of the plan have shipped (typed rasterisation result; the section-9 ratchet in
`tools/check_module_boundaries.sh`); P2 is what drains the counts that ratchet holds.

It was produced by surveying every one of the ~385 external accesses in the five consumer files,
grouping them by *intent* rather than by struct mechanics, then designing three independent APIs
(minimal-orthogonal, CRUD-shaped, opaque-handle-and-iterator) and judging them against the survey.
Everything below marked **verified** was checked against the code, and several of those checks
overturned what the proposals assumed.

## The convention: the first parameter names the world

Three suffix schemes were proposed (`_in`, `_frozen`, `_ext`). All three are unnecessary, because
the tree already distinguishes the cases positionally:

- **`GList *forms` first** → resolve against a borrowed refcounted snapshot (`pipe->forms`,
  `hist->forms`). No lock, no copy-on-write, read-only.
- **`const dt_masks_form_t *` first** → an already-resolved handle. Thread-neutral: it reads only
  that object's own memory. No lock, no COW, and no `_in` twin is needed.
- **`dt_develop_t *dev` first** → touches the live list. Returns `dt_masks_result_t` ⟹ it writes,
  locks and COW-touches internally; returns a value ⟹ it reads under the read lock.

Only the *resolvers* come in pairs. That removes about six functions relative to the CRUD proposal,
and it makes `iop/spots.c:441/561` — which resolves `self->dev->forms` from the pipeline thread, the
retouch bug that never got migrated — visibly wrong at the call site.

Handles are non-negotiable for reads. **Verified:** `dt_masks_get_visible_form()` can return a
`formid == 0` transient display group that is in neither `dev->forms` nor `dev->allforms`
(`masks_gui.c:4009-4051`), so no id-keyed reader can reach it — yet `retouch.c:986`, `spots.c:289`
and `blend_gui.c:2159` all read it.

## Headers

Neither `masks.h` nor `masks_functions.h` can host the new API.

- **Not `develop/masks.h`.** Its 886 lines drag in `develop/develop.h`, `develop/pixelpipe.h`,
  `caches/pixelpipe_cache_alloc.h`, `common/logging.h`, `system/atomic.h`, `system/simd.h` and
  `common/times.h`. Declaring the API there means no consumer can ever drop the include, which is
  the entire objective.
- **Not `develop/masks/masks_functions.h`.** It is the private per-shape vtable header and it
  includes both `masks.h` and `masks_gui.h` (lines 51-52), so anything declared there is
  unreachable from outside `src/develop/masks/`.

Two new headers, modelled exactly on the `colorprofiles/profile_types.h` cut and on
`masks/masks_history.h` (which already compiles against an opaque `dt_masks_form_t` using tag
declarations and `<glib.h>` alone):

- **`src/develop/masks_types.h`** — `<glib.h>` only, guard `DT_DEVELOP_MASKS_TYPES_H`. *Moves*
  `dt_masks_type_t`, `dt_masks_state_t`, `dt_masks_increment_t`, `dt_masks_edit_mode_t`,
  `dt_masks_interaction_t` and `dt_masks_form_group_t` out of `masks.h`; adds
  `DT_MASKS_FORM_NAME_LEN`, `dt_masks_result_t`, `dt_masks_member_t`, `dt_masks_form_info_t`.
  `masks.h` includes it first, so all 24 current includers keep compiling unchanged — that is the
  safety property that makes the move reviewable on its own.
- **`src/develop/masks_group.h`** — includes `masks_types.h` and nothing else; tag-declares
  `struct dt_develop_t`, `struct dt_masks_form_t`, `struct dt_iop_module_t`. **No GTK.**
  (`masks_gui.h` really does pull `widgets/draw.h` and `<gtk/gtk.h>` — verified at :50-53 — for six
  IOPs that want one call.) GUI-state accessors stay in `masks_gui.h`.

Implementation goes in `src/develop/masks/masks.c`, the data-model translation unit. No CMake change
is needed.

### Value types crossing the boundary

```c
#define DT_MASKS_FORM_NAME_LEN 128

typedef enum dt_masks_result_t
{
  DT_MASKS_OK = 0,     /* the model changed */
  DT_MASKS_UNCHANGED,  /* legal request, nothing to do -- caller skips the history commit */
  DT_MASKS_NOT_FOUND,  /* no such group, or no such member in it */
  DT_MASKS_INVALID     /* refused: not a group, self-inclusion, bad argument */
} dt_masks_result_t;

typedef struct dt_masks_member_t
{
  int              formid;
  int              parentid;   /* the row's AUTHORED origin; != holder inside a display group */
  guint            index;      /* position == compositing order == GTK row identity */
  dt_masks_state_t state;      /* tightened from the struct's plain `int` */
  float            opacity;
} dt_masks_member_t;

typedef struct dt_masks_form_info_t
{
  int             formid;
  dt_masks_type_t type;
  int             version;
  gboolean        is_group;
  gboolean        is_retouch;   /* (type & DT_MASKS_IS_RETOUCHE) != 0 */
  guint           member_count; /* 0 unless is_group; NOT recursive */
  char            name[DT_MASKS_FORM_NAME_LEN];
} dt_masks_form_info_t;
```

**Why `dt_masks_form_group_t` moves rather than going opaque.** `common/xmp_sidecar.cc:1203-1204`
casts an XMP binary blob straight to `dt_masks_form_group_t *` and validates
`entry->mask_nb * sizeof(dt_masks_form_group_t) == entry->mask_points_len`. **The struct's size and
field order are the on-disk format in every user's sidecars and database.** It can never be made
opaque and its layout can never change — which is exactly why the value-typed `dt_masks_member_t`
must be what everyone else consumes.

## The API

### Resolvers — the only `dev` / snapshot pair

```c
dt_masks_form_t *dt_masks_get_group_from_id   (struct dt_develop_t *dev, int group_id);
dt_masks_form_t *dt_masks_get_group_from_id_in(GList *forms, int group_id);   /* pipe->forms */
```

NULL unless it resolves *and* is a group. Collapses ~30 sites that write `dt_masks_get_from_id(...)`
followed by `(x->type & DT_MASKS_GROUP)`. **Do not collapse the pair** — that reintroduces the
`spots.c` cross-thread bug as the path of least resistance.

### Reads — handle-taking, thread-neutral, no lock, no COW

```c
gboolean dt_masks_form_get_info(const dt_masks_form_t *form, dt_masks_form_info_t *out);

guint    dt_masks_group_copy_members (const dt_masks_form_t *group,
                                      dt_masks_member_t *out, guint out_max);
gboolean dt_masks_group_get_member   (const dt_masks_form_t *group, int formid,
                                      dt_masks_member_t *out);
gboolean dt_masks_group_get_member_at(const dt_masks_form_t *group, guint index,
                                      dt_masks_member_t *out);

gboolean dt_masks_form_get_anchor(const dt_masks_form_t *form, float anchor[2], float source[2]);

const char *dt_masks_type_name(dt_masks_type_t type);   /* untranslated token; takes a VALUE */
```

- `get_info` retires ~40 `->formid`, ~30 `->type`, 24 `->name` and supervisor's `->version` reads,
  plus every hand-written `type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)` (8 copies; `masks.h:422`
  already names that predicate). `name` is **copied**: the borrowed `const char *` is exactly the
  hazard `blend_gui.c:1701` already trips, caching `parent_form->name` and using it down to :1712.
- `copy_members` returns the total always and fills `min(total, out_max)`; `out == NULL` makes it a
  count. **Order is the contract**, and it must be stated at the declaration: stored order ==
  compositing order == GTK row order == `retouch.c:725`'s `p->rt_forms[]` index ==
  `spots.c:565`'s `d->clone_algo[pos]` index, *which is persisted in every user's database*. A
  member that fails to resolve **still consumes its index** — verified: `spots.c:569` `continue`s
  but `pos++` sits in the for-increment. Never filter, never recurse, never reorder.
- All four assert the group type internally and return 0/FALSE otherwise — that is what makes the
  polymorphic `->points` unreachable — and leave `*out` **completely untouched** on FALSE
  (the `dt_colorspaces_profile_at()` convention).
- `get_anchor` is the only reason `dt_masks_node_*` is public today. It dispatches through the
  already-private `form->functions` vtable and replaces `retouch.c:893-930` and its byte-for-byte
  copy at `spots.c:513-538`.

### Reads that must resolve children — take `dev`, read lock internally

```c
gboolean dt_masks_group_has_leaf_shape         (struct dt_develop_t *dev, int group_id);
gboolean dt_masks_group_is_single_group_wrapper(struct dt_develop_t *dev, int group_id);
int      dt_masks_form_owner_group             (struct dt_develop_t *dev, int formid); /* 0 = none */
guint    dt_masks_forms_list_ids               (struct dt_develop_t *dev, int **out_ids);
gchar   *dt_masks_group_member_label           (struct dt_develop_t *dev, int group_id, int formid);
```

The first two delete `blend_gui.c:1482-1521` outright — pure masks-graph queries with zero GUI
content in a GUI file. **Depth-cap both at 32**, as `_masks_find_any_parent_group()` already does
and neither copy does. `owner_group` replaces `dt_masks_form_group_find_any()` and **drops its COW
side effect** — verified: that function copy-on-write-touches inside a *lookup*.

### Writes — all id-keyed, all COW internal, all returning `dt_masks_result_t`

```c
dt_masks_result_t dt_masks_group_set_member_operation(struct dt_develop_t *dev, int group_id,
                                                      int formid, dt_masks_state_t operation);
dt_masks_result_t dt_masks_group_set_member_opacity  (struct dt_develop_t *dev, int group_id,
                                                      int formid, float opacity);
dt_masks_result_t dt_masks_group_nudge_member_opacity(struct dt_develop_t *dev, int group_id,
                                                      int formid, float amount,
                                                      dt_masks_increment_t increment, int flow);
float             dt_masks_group_set_member_property (struct dt_develop_t *dev, int group_id,
                                                      int formid, dt_masks_interaction_t property,
                                                      float value, dt_masks_increment_t increment,
                                                      int flow);  /* NAN = not applicable */
dt_masks_result_t dt_masks_group_move_member  (struct dt_develop_t *dev, int group_id,
                                               int formid, int delta); /* -1 up, +1 down */
dt_masks_result_t dt_masks_group_add_member   (struct dt_develop_t *dev, int group_id, int formid,
                                               dt_masks_state_t state, float opacity);
dt_masks_result_t dt_masks_group_remove_member(struct dt_develop_t *dev,
                                               struct dt_iop_module_t *module /* nullable */,
                                               int group_id, int formid);
dt_masks_result_t dt_masks_form_set_name        (struct dt_develop_t *dev, int formid, const char *name);
dt_masks_result_t dt_masks_form_set_retouch_mode(struct dt_develop_t *dev, int formid, gboolean is_clone);
int               dt_masks_group_create_for_module(struct dt_iop_module_t *module, const char *name);
```

Every one: resolve → `dt_masks_cow_touch()` → **re-resolve the row from the touched group** →
mutate → compare. An id-keyed signature is the only shape that *can* be written this way, which is
the whole point: cloning a parent also clones its `dt_masks_form_group_t` blocks, so any entry
pointer taken before the touch belongs to the abandoned copy.

- `set_member_operation` closes **six verified COW holes** (`libs/masks.c` 502/557/612/667/722 and
  `blend_gui.c:2091`) and collapses five ~50-line handlers into one. Do it in a single commit —
  fixing it in five places separately is how one gets missed.
- `add_member` becomes the **sole constructor** for `dt_masks_form_group_t`, retiring the three
  hand-rolled `malloc`s (`libs/masks.c` 365-369 / 970-974, `retouch.c` 685-690). It keeps the
  self-inclusion guard and the `dt_masks_form_update_gravity_center()` all three skip, and
  `parentid` is never a parameter — always stamped `group_id`.
- `move_member` is **keyed on formid, not index**: the GTK model already carries
  `BLENDOP_MASKS_GROUP_COL_FORMID`, so the stale-index hazard disappears.

## Corrections the survey overturned

These are the findings that contradict what the proposals — and in one case this project's own
assumptions — took for granted. They are the reason to read this document before implementing.

1. **`dt_masks_form_set_interaction_value()` does *not* copy-on-write for opacity.** All three
   proposals claimed it does and wanted to route opacity writes through it. Verified false:
   `masks_gui.c:4361-4365` dispatches to `_change_opacity()` (:4920-4928), a bare in-place write
   plus a toast; the touch lives in the *caller* `dt_masks_form_change_opacity()` (:4939-4941).
   Only the geometry branch touches, and it touches the *shape*. Reusing the existing setter as-is
   would ship the `retouch.c:598` bug into the new API.
2. **The locking rationale is misdiagnosed.** The framing "~21 unlocked external walks of
   `dev->forms`" is wrong about which end is dangerous: every *writer* of `dev->forms` is on the GUI
   thread, and the only cross-thread reader is `pixelpipe_hb.c:1629`'s snapshot on the worker.
   Single writer ⟹ the unlocked GUI reads are benign today. The genuinely racy site is the unlocked
   **write** at `libs/masks.c:377`. Read-side locking in the new enumerators is cheap insurance, not
   a bug fix — and it is still a behaviour change that needs approval.
3. **P2 does not make `dt_masks_form_t` opaque, and no phase of P2 will.** Three blockers survive:
   the rasterisers take *non-const* `dt_masks_form_t *` because they lazily fill cached geometry (12
   external sites); `spots.c:583-595` reads `circle->center`/`->radius` for a fast path and
   `:149-158` mallocs a node for the v1 migration; and `dt_masks_get_form_size_from_nodes()` casts
   each `points` element to `const float *` assuming `float node[2]` is the first member — a node
   layout hard-coded in a public header. Budget a **P2b per-shape geometry axis** and freeze it
   against retouch *and* spots together.
4. **The type-token set feeds persisted conf keys.** `_get_mask_type()` feeds
   `dt_masks_get_set_conf_value()`, which builds `plugins/darkroom/<plugin>/<type>/<feature>` — keys
   declared in `data/anselconfig.xml.in` as `.../polygon/fading` etc. So the token is **polygon**,
   never supervisor's "path": adopting supervisor's spelling in a unified function would silently
   repoint every polygon shape at a non-confgen key defaulting to 0.
5. **Two ratchet mechanics decide how the API is *consumed*.** Section 9 counts `->` only, so a
   converted site that reads `m[i].formid` by value is invisible to the gate while one that takes
   `dt_masks_member_t *p = &m[i]` puts the count straight back — this is why value structs, not
   accessor-per-field, are what actually drain the number. And `masks_strip()` drops lines starting
   with `*` or `//` but **not** `/*` (verified: `blend.c:570` is counted), so do not reflow comments
   in a commit that moves a baseline.

## The first pull request

**Convert `develop/supervisor.c` and `views/studio_capture.c`.** Not the biggest consumers — the
two that can reach *zero* direct accesses. `supervisor.c` is the only pure consumer in the tree: 8
member reads, no `->forms` touches, no allocations, no COW requirement, no lock, all inside three
functions, behind a `dt_supervisor_active()` gate. It exercises all three read functions and both
value structs, including the ordered members walk, on real code with a debug-only blast radius.

Three commits, because each moves a different set of baselines and must stay bisectable:

1. **Headers and the three read functions** (moves no count): create `masks_types.h` and
   `masks_group.h`, move the enums, implement `get_info` / `copy_members` / `type_name`, fold
   `_get_mask_type()` into the last of these, extend `test_masks_raster_contract.c` with read-API
   cases reusing its stack-built fixtures.
2. **`dt_masks_release_all_forms()`** in `masks_history.h` (moves 2 baselines): replaces the
   four-line free-both-lists block copy-pasted verbatim into `develop.c`, `darkroom.c` and
   `studio_capture.c`; `studio_capture.c` then drops `masks.h`. Baselines → includers 23,
   `->forms` 76.
3. **Convert `supervisor.c`** (moves 3 baselines): the three functions, plus renaming its own
   `dt_sv_entry_t.formid` → `form_id` so the gate's documented false positive genuinely disappears
   — and **delete that paragraph from section 9 in the same commit**. Baselines → includers 22,
   member reads 94, writes 26.

Expected ratchet movement: `24 → 22` includers, `11 → 11` masks_gui.h (unchanged), `102 → 94`
reads, `27 → 26` writes, `4 → 4` allocations, `88 → 76` `->forms`.

**Honesty the PR description must carry**, because the numbers overstate the result: neither file
becomes `masks.h`-free — `supervisor.c` still reaches it through `blend.h:43` and
`studio_capture.c` through `masks_gui.h:50`. The literal count falls; the edge does not die until
the `blend.h` supply line is cut in its own PR.

### Verification

`tools/check_module_boundaries.sh` after **each** commit (a "ROSE"/"fell" line means the baseline
edit is in the wrong commit and bisect will fail); three build configurations sequentially — Release,
Debug and **build-nofeatures**, which is the one that caught the last include-supply-line break;
`tools/check_unused_includes.sh`; `pragma_once_to_guards.py --verify` and `include_graph.py
--summary` (cycles 0); `ctest`; `tools/check_it_runs.sh` — the gate that caught the colorprofiles
double-free after every static check had passed; and a pixel comparison with
`tools/check_export_pixels.sh` on a raw carrying **both** a drawn mask group and a retouch clone,
`--disable-opencl`. Nothing in PR 1 is on the pixel path, but the claim has to be measured.

The biggest silent risk is the **ordering contract**: supervisor only emits ids, so a `copy_members`
that filtered or reordered would pass PR 1 unnoticed and silently re-pair every shape with the wrong
clone algorithm the moment `spots.c` converts.

## Later tranches

The rule that keeps this honest: **a function lands in the same commit as its first caller**, and
every commit that moves a count moves its baseline. (The survey found
`dt_masks_form_get/set_interaction_value` already do what five external sites hand-roll, and have
zero callers, because they shipped ahead of their consumers.)

- **PR 2 — the detail-mask math is not the forms model.** Split `dt_masks_extend_border`,
  `dt_masks_blur_9x9`, `dt_masks_calc_rawdetail_mask`, `dt_masks_calc_detail_mask` into
  `masks_detail.h`: pure pixel math, no form involvement. `iop/detailmask.c` and `iop/demosaic.c`
  name nothing else from `masks.h` and both drop it. Includers 22 → 20.
- **PR 3 — `blend.h` stops being the masks module's delivery van.** *(Done.)* Remove `masks.h`
  from `blend.h:43`. `blend.h` names zero masks symbols, but it does declare functions taking
  `dt_iop_module_t` and `dt_develop_t`, so it takes `develop/develop.h` directly instead — the
  house rule that a header includes what its own declarations need. Three supply lines broke and
  were fixed by naming what the file actually uses: `common/times.h` in `common/opencl.c` and
  `darktable.c`, and `DEVELOP_MASKS_NB_SHAPES` — pure vocabulary — moved into `masks_types.h` so
  `blend_gui.h` can take the light header. `masks_types.h` also gained the forward typedef of
  `dt_masks_form_t`, so a file that only passes a form along needs the name and not the layout.

  **Measured correction to this plan.** The claim that this edge is "worth more than the other
  twenty put together" does not survive measurement: `masks.h`'s transitive fan-in is **50 before
  and 50 after**. The files that reached it through `blend.h` reach it another way. What the change
  actually achieves is that **only two headers still include `masks.h`** — `masks_gui.h`, which is
  external-facing, and `masks/masks_functions.h`, which is module-private. So the reach will not
  drop until the `masks_gui.h` edge is cut, and that — not `blend.h` — is the one carrying the
  surface to the rest of the tree. Retarget the next header tranche accordingly.
- **Then** the write API against `libs/masks.c` and `blend_gui.c` (the six COW holes), and finally
  the P2b per-shape geometry axis for `retouch.c` and `spots.c`.
