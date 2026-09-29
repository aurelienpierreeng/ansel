# Retouch: memoising each shape's result (designed, measured, rejected) {#retouch_result_memo}

[TOC]

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. Nothing was found wrong by those checks.

## Status

**Not implemented, and not worth implementing.** The design below is sound and the dependency
graph it needs is sparse; what kills it is where the cost actually sits at the zoom people retouch
at. The reasoning is kept because it is the map of how this module composes shapes, and because
the first two attempts to price this work were both wrong in ways the document records.

The short version. A drag at 1:1 to 4:1 puts three of a hundred and forty shapes inside the
viewport, and of those three the cost is the one being dragged -- its heal re-solved at a new
position, 9 to 168 ms depending on the content it lands on -- while the two static shapes beside it
cost 8 to 30 ms together. That 8 to 30 ms is all a result memo could buy, out of a 550 ms frame
whose 92 % is a downstream `diffuse or sharpen` instance. See
[Why it was rejected](#retouch_result_memo_verdict).

## What a result patch is

A shape's effect is not a function of the image alone. `dwt_decompose()` (`pixel/dwt.c`) calls
`rt_process_forms()` once per wavelet scale with that scale's buffer, and the callback walks the
module's mask group applying each member **sequentially, in place, into that one buffer**. So a
shape reads whatever the shapes before it left there.

The unit to memoise is therefore not "the shape's effect" but **the content of the shape's
destination box after that shape has been applied** — a patch, laid out in layer coordinates,
that can be blitted over the layer in place of running the algorithm. Replaying a frame becomes:
walk the members in order and, per member, blit its patch on a hit or run its algorithm on a
miss. Regions no shape writes keep the base content the decomposition produced, so a frame where
every shape hits still renders correctly without any shape being computed.

## What each algorithm reads and writes

`D` is the shape's destination box — `shape->roi_mask`, the scaled mask's ROI in layer
coordinates. `S` is `D` translated by `(-dx, -dy)`: `rt_copy_in_to_out()` reads at
`roi_out->x - roi_in->x - dx`, so the source sits at the destination *minus* the offset.

| algorithm | reads | writes | note |
|---|---|---|---|
| fill | `D` | `D` | blends, `d·(1−f) + colour·f`, so it reads its own destination |
| clone | `S` and `D` | `D` | copies `S` to a temp, then blends it over `D` through the mask |
| blur | `D` only | `D` | `rt_copy_in_to_out(..., 0, 0)`: the gaussian runs inside `D` with a zero boundary, so the `4·radius` growth in `rt_compute_roi_in()` is about the pipeline's input ROI, not about reading outside `D` here |
| heal | `S` and `D` | `D` | the Laplace solve, `max_heal_iter` iterations — the expensive one, and the one a memo pays for |

**Every algorithm reads its own destination**, because all four blend through the mask and the
opacity: a partially-masked pixel keeps part of what was underneath. So `D` is always a read box,
not only a write box.

## The dependency structure has three levels

**Scale 0 is special and contaminates everything.** `dwt_wavelet_decompose()` calls
`layer_func(img, p, 0)` on the full image *before* the decomposition loop, and then sets
`buffer[0] = img`. So the shapes of scale 0 modify the image every later scale is decomposed
from. An edit at scale 0 invalidates every scale.

**Scales at or above 1 are mutually independent.** In the loop,
`dwt_decompose_layer(buffer[lpass], buffer[hpass], …)` writes the low-pass into `buffer[lpass]`
and the callback then modifies `buffer[hpass]`, the detail. The next iteration decomposes the
low-pass, which the callback never touched. A shape at scale N therefore reaches the output only
through that scale's detail, which `dt_iop_image_add_image()` adds to `layers`.

**Merge mode breaks that independence.** On the `merge_from_scale` branch the callback is handed
`merged_layers`, an accumulator that keeps growing across scales, so a shape at scale N modifies
what scale N+1 is handed. The key must fold `merge_from_scale` and, in that mode, treat the
scales at or above it as one chain.

**Within one scale the order is the composition order** — `rt_pipe_group_members()` walked
forward — and that is where the interesting dependencies live.

## The key: transitive, and restricted to boxes that actually meet

Define, per scale:

    root  = hash(piece->upstream_hash, roi_layer, scale index, wavelet params, mask_display)
    deps_i = [ key_j for j < i where W_j meets (D_i union S_i) ]
    key_i = hash(root, shape_i's geometry hash, its algorithm parameters, opacity, dx, dy, deps_i)

and memoise shape `i`'s patch under `key_i`. `W_j` is `D_j`, shape `j`'s destination box.

Two properties make this the right shape, and both are the point of the whole design:

**A cumulative hash would be useless here, and this is the trap to avoid.** Folding every
preceding shape into a running hash — what `upstream_hash` correctly does for *modules* — would
make every shape below the dragged one change key. That is exactly the failure this memo exists
to prevent: a drag would invalidate the tail of the list and save nothing. The dependency set has
to be the shapes that genuinely meet this one, not the shapes that merely precede it.

**Moving a shape changes more than its own key, and that is correct.** When shape `j` moves, `W_j`
moves with it: shapes that now meet it gain a dependency and shapes that met its old position lose
one. Both sets change keys and recompute. Nothing else does.

The root carries `piece->upstream_hash` rather than `global_hash` for the reason the "module
memoising its own intermediates" section of `CLAUDE.md` gives: `global_hash` folds this module's
own parameters, so it moves on every frame of a drag and keys nothing that survives one.

## Boxes, not masks — and why that is the risk rather than the shortcut

The dependency test is a box intersection, not a mask intersection. A box is conservative: it can
claim a dependency that does not exist, never miss one that does, so a false edge costs a
recompute and never a wrong pixel. It is also arithmetic on quantities the geometry memo already
serves, which is what makes computing the graph cheap at all — the same `O(shapes²)` pass that
used to regenerate outlines is now integer comparisons.

**But over-approximation is self-amplifying through transitivity, and that is the one thing that
can kill this design.** If A falsely depends on B, and C truly depends on A, then moving B
invalidates C. On a densely retouched portrait the transitive closure of a hundred and forty
shapes could plausibly cover the whole list, in which case a drag invalidates everything and the
memo buys nothing while costing memory and code.

Nothing in the source answers how dense that graph is. It is a property of how people actually
place shapes.

## The dependency density, measured {#retouch_result_memo_measurement}

Measured with a throwaway probe in `rt_process_forms()` that builds the graph and reports, per
scale, how many shapes one move would invalidate. Two images from a real library, exported at
2000 px through `ansel-cli`:

| image | shapes | direct edges | dependents per move, mean | max | shapes invalidating nothing | patches / layer |
|---|---|---|---|---|---|---|
| 141-shape retouch (id 19092) | 140 | 135 | **2.3** | 21 | 86 of 140 (61 %) | 0.1x |
| 137-shape retouch (id 19327) | 128 | 20 | **0.2** | 3 | 112 of 128 (88 %) | 0.0x |

**The graph is sparse, so nothing about the dependency structure stands in the way.** What does is
where the cost sits at a working zoom, measured afterwards and recorded under
[Why it was rejected](#retouch_result_memo_verdict). Moving one shape invalidates two or
three others on average and twenty-one in the worst case seen, against a hundred and forty
shapes in the list. On the denser of the two images, three shapes in five invalidate *nothing*:
moving one of those recomputes that shape alone.

That is the regime this design needs. The fear it was measured against -- that box
over-approximation would amplify through transitivity until a drag invalidated everything -- does
not materialise on real work, and the reason is visible in the numbers: 135 direct edges over 140
shapes means shapes barely touch each other. People place them on distinct blemishes.

**Memory is a non-issue.** The sum of the destination boxes is a tenth of the layer on the denser
image, so the patches for a whole frame cost about 1 MB against the layer's 11.6 MB -- two orders
of magnitude below the checkpoint arithmetic below.

Two limits of this measurement, stated so they are not mistaken for coverage. Both images carry
all their shapes at **scale 0** (`num_scales` is 0), so nothing here exercises the multi-scale
partitioning or merge mode; those still have to be reasoned about from the decomposition, as
above. And it is the export pipe at full frame, which is the conservative case: a zoomed darkroom
ROI drops the shapes outside it entirely, so the graph there is smaller, never larger.

The probe was removed once it had answered. The graph the implementation needs is not a
diagnostic but the key itself, and it will be built as such.

## Why it was rejected {#retouch_result_memo_verdict}

The density measurement above was taken on the **export pipe at full frame**, where all 140 shapes
apply. That is the case the design flatters, and it is not the case people work in. Measured in the
darkroom at a working zoom of 1:1 to 4:1, with a temporary counter recording the costliest single
member beside the existing `algorithms` sum -- a few lines around `algo_start` in both
`rt_process_forms()` and its OpenCL twin, not kept:

| | |
|---|---|
| shapes the FULL pipe applies | **3** of 140 -- the rest are outside the viewport |
| of those three, the dragged one | 9 to 168 ms, always the costliest, always heal |
| the two static ones together | 8 to 30 ms |
| retouch's share of the frame | **5 %** |
| `diffuse or sharpen Sharpen`, downstream | 504 ms, **92 %** of the frame |

**The dependency graph is ROI-relative**, and that is what makes the cancellation structural rather
than incidental: a shape outside the layer ROI writes nothing into it, so it can be nobody's
ancestor there. Zooming in shrinks the shape count and the graph together. The memo is worth most
at fit zoom, where 140 shapes apply -- and worth almost nothing at the zoom that makes a drag feel
slow. There is no operating point where it pays.

What it cannot buy at any zoom is the dragged shape's own cost, which is the part that varies:
`_heal_laplace_loop()` (`pixel/heal.c`) runs Gauss-Seidel to `max_iter` but breaks once the squared
residual falls under its threshold, so the same shape costs nineteen times more over hard content
than over easy content. That work is real -- the shape genuinely moved -- and no memo addresses it.

### Two readings of `-d perf` that were wrong first

Recorded because both produced a confident and mistaken estimate of this memo's value, and the
lines still invite them.

**The aggregate `algorithms` figure hides which shape is expensive.** It put the target at 86-88 %
of a drag frame. That figure was a fit-zoom aggregate over 140 shapes read as if it described the
interactive case. Separating the costliest member from the sum of the others is what answers "would
a memo help", and `-d perf` does not do it: whoever asks this question again has to add those few
lines back.

**The FULL and preview pipes do not both render per drag frame.** Only FULL does; the preview
renders once, when the button comes up. A preview figure is therefore a per-gesture cost, and
adding it to a FULL figure -- as the 86-88 % did -- double-counts the gesture.

### What was checked and found not worth doing either

The mask memo (the merged half) misses throughout a drag at high zoom, because its key includes the
layer ROI and `modify_roi_in()` moves that ROI as the dragged shape's source area moves -- measured
at `1027x812 -> 1007x796 -> 968x765 -> 949x750 -> 894x707 -> 877x693` over eight frames, with
`masks 3 (0 memo / 3 rasterised)` on every one of them. The boxes still hit 140 of 140, being
ROI-free by construction.

Quantising the planned ROI onto a grid would stabilise it, in about five lines placed after the
stabilisation loop so it cannot oscillate, and a wider input is always safe. It buys 13 to 17 ms of
a 550 ms frame: two `modify_roi_in()` passes at 1 ms, three mask rasterisations at 2 to 6 ms, and
`initialscale` recomputing at 3 to 6 ms -- everything above `initialscale` is already insulated,
since its own `modify_roi_in()` resets to the full buffer. And the same cancellation applies: the
ROI only wobbles at high zoom, which is where few shapes apply. Not done.

## Per-shape patches, not layer checkpoints

The obvious alternative — snapshot the whole layer after each shape, and replay from the
checkpoint before the one that moved — is ruled out by arithmetic, not by taste. The FULL pipe's
layer measured 823×885 in the `-d perf` line, i.e. 11.6 MB at four floats per pixel. A hundred and
forty checkpoints is 1.6 GB **per scale**, before the preview pipe asks for its own. The patches
are bounded by the sum of the destination boxes instead, which is the quantity to measure
alongside the graph density.

## Open problems

These are the problems the implementation would have had to solve. They are recorded because they
are properties of the module, not of the abandoned memo.

**OpenCL costs less than it looks.** `rt_process_forms_cl()` works on a `cl_mem dev_layer` and
`_retouch_heal_cl()` has no kernel of its own -- it round-trips to the host for `dt_heal()` -- which
invites the guess that the GPU path is transfer-bound and that a host-side patch would be free.
Measured at an identical ROI, the same export costs 0.072 s of algorithms on CPU and 0.106 s on GPU:
a factor of 1.5, not the several-fold penalty that guess predicts. A patch memo would still have had
to choose between per-shape uploads and device-side entries, which the cache can hold, but the
transfers are not where this module's GPU time goes. The CPU and GPU paths must in any case compute the same
graph from the same helper, the way `rt_prepare_shape()` already keeps their preamble from
drifting.

**Mask display writes after the algorithm.** `rt_copy_mask_to_alpha()` runs on the layer once the
algorithm has, so a patch captured after it would carry the overlay's alpha. Either capture the
patch before that call, or fold `mask_display` into the key — the root above does the latter,
which is the safer of the two and costs a memo per preview state.

**The memory budget is shared.** These patches live in the same arena as every pipe's
intermediates, under the pressure valve described in `CLAUDE.md`. A memo that evicts the FULL
pipe's own cachelines to hold a hundred and forty patches would trade a visible 8 s recompute for
an invisible one, which is the mistake the host-memory fit probe already made once.
