# The brush and polygon boundary: one geometry, two consumers

> **Corrected against `fa8e8b86fa` on 2026-09-29.** The audit before that found 4 claim(s)
> in this file wrong of the tree and 4 stale. Three of the four are about the corpus and the API's shape, not the geometry — the geometry claims held. Re-measure
> before acting on a claim older than the code you are changing, and re-date this line when
> you do.

Status: **all landed on master** (verified `f37105c227`, 2026-09-29). The brush came in #1381
(issues #1352, #1360); the polygon and the shared boundary pass followed in #1383 and after
(`ee75df0d12`, `13164d17c5`, `0df01f6443`, `e3ecf4b242`, `2162fd259a`, all ancestors of HEAD).
`dt_masks_outline_boundary_skips()` is the one shared pass, defined in
`develop/masks/masks_outline.c` and called from both `brush.c` and `polygon.c`.

*This line said the polygon work was "on `polygon-boundary`" until 2026-09-29. That branch no
longer exists — `git rev-parse --verify polygon-boundary` fails — so the status read as
"unmerged, do not rely on it" for work that had been on master for three weeks.* Measured with `tests/masks/masks_geometry.c`; every number below
comes from that corpus or from the reporters' own files.

## What a brush is

A brush stroke is the Minkowski sum of its centreline with a disc: the union, over every
point of the Bézier spine, of a disc of the local radius. The spine is a chain of nodes,
each carrying a radius (`border[0]`/`border[1]`, always written together), a density and a
fading; the radius is interpolated along a segment with a smoothstep.

The pixel pipeline paints that union as **spokes**: for every sample of the spine, one
segment from the sample out to its border sample, on each side, with the fading profile
along the spoke. The spine is walked twice, forward with the border on the right and
backward with the border on the right again, which is the other side. A cap closes each
end, and where two segments meet at a node with a different direction, radius or payload,
the wedge the spokes leave open is filled by an arc or a disc centred on that node.

Coverage never needed an envelope. The union of spokes is the union of discs wherever the
spine is sampled densely enough, whatever the spokes do to each other: a spoke inside
another spoke's disc paints nothing new. That is why the rasteriser is correct with
self-crossing outlines, and why it must paint every spoke it is given.

## The two consumers, and where they diverged

Both consumers share one producer, `_brush_get_pts_border()` in `develop/masks/brush.c`.
It returns index-aligned arrays: `points` (the spine samples, header of three entries per
node first), `border` (one border sample per point), `payload` (fading, density per
sample). They differ in what they hand it and in what they do with the result.

| | pipeline (`_brush_get_mask_roi`, `_brush_get_mask`) | GUI (`_brush_get_points_border`) |
|---|---|---|
| coordinate frame | module input space: `dt_masks_distort_for_pipe()`, `DT_DEV_TRANSFORM_DIR_BACK_INCL` | display space: `dt_masks_distort_for_gui()`, `DT_DEV_TRANSFORM_DIR_ALL` |
| sample pitch | the pipe's `mask_rasterization_step` | the published view density — `dt_masks_gui_outline_step(dev)`, one sample per device pixel (`masks_distort.h:117`, fed by `dt_masks_gui_set_outline_density()` from the expose), **not** a fixed 1 px |
| what it does with the arrays | stamps every spoke | draws the edge where the fading starts solid and, on the selected brush, `points` as the dotted centreline and `border` dashed, hit-tests against `points` and `border` |

The first two differences are legitimate: a mask is rendered in the module's input frame
at the pipe's resolution and drawn in the viewer's frame at the screen's. The third is
where the divergence lived.

**Before.** The GUI could not draw the raw `border` array: at every fold where the spine
bends tighter than its radius the offset curve loops over itself, every joint arc reads
as a circle, and a stroke crossing itself draws through its own other pass. So the GUI
ran a second algorithm on the arrays the pipe never saw: the producer recorded the join
arcs it appended as out-of-band spans for the display to hide, then
`dt_masks_border_find_self_intersections()` intersected the outline with itself and
`_select_disjoint_cuts()` chose which crossings to cut, longest first under a cap that
guessed which pairings were folds and which were the two sides of the stroke meeting.

That made the drawn outline a *different object* from the painted mask: a heuristic subset
of it, decided by an algorithm the mask never went through. The user sees both objects
at once, because the mask preview overlay in the darkroom is the pipe's raster and the
dashed border is the GUI's polyline, so every disagreement between the two algorithms is
visible as "the outline does not match the glow". Issue #1352 is exactly that picture:
a correct mask with straight chords drawn across its end cap.

**Where each side failed.** In #1352 the producer was right and the display's cuts were
wrong. In #1360 the producer was wrong and the display's cuts *hid part of it*: of
131,261 garbage spokes the raster painted, the cuts removed 25,864 from the outline, so
the screen showed a smaller circle than the export contained. A second algorithm on the
output cannot fix the first; it can only disagree with it.

## The producer, rebuilt

The walk is explicit now: forward over the segments, backward over them, one pass at a
time. Every quantity a joint or a cap needs is taken from the two segment end samples that
meet there and from the node data. Nothing is read back out of the buffers.

That last sentence is the whole of issue #1360. The old walk took "the last centreline
sample and the last border sample written" as the centre and radius of every cap, arc and
stamp, assuming they belonged to one spoke. A pen resting under rising pressure produces
a run of coincident nodes (seventeen of them within half a pixel in the reporter's file,
density ramping 0.05 → 0.91), and the segments between them are points: no direction to
offset along. The recursion's leaf, finding no border at either end, wrote its caller's
zero-initialised scratch into the buffer, and the next stamp measured its radius as the
distance from the stroke to the image origin: 2058 px on the reporter's file. Ten such
discs followed, one per density step, because each took its radius from the last.

Measured on the reporter's sidecar (`brush-1360-pressure-ramp`, 5184×3888):

| | before | after |
|---|---|---|
| spokes longer than 1.5 × the radius | 131,261 of 303,733 (43 %) | 0 |
| border samples exactly at the image origin | 49 | 0 |
| mask coverage no disc owes (excess) | 9,738,604 px, one region | 0 |
| coverage owed and missing | 0 | 0 |

The same file, exported with `ansel-cli --export_masks 1` through the real pipeline: the
mask's bounding box is the stroke's nodes ± radius to within a few pixels.

Rules the new walk follows, each of which the old one broke somewhere:

- A **degenerate segment** — four control points at one point — contributes nothing. Its
  disc is the cap of whichever neighbour has a direction; the walk joins the segments
  either side of it as if it were not there, which for the disc union is exactly right.
- An end that has a radius but no direction **borrows the other end's direction**, never
  a position. A spoke of the right length in a borrowed direction is part of the union; a
  spoke to a copied position is of no particular length at all.
- The **disc stamp takes its radius as an argument** (`_brush_points_stamp()`), from the node
  data. It used to measure it.
- The **cap** starts at the last segment's own end border and sweeps π to its reflection.
  The old cap started wherever the previous lookahead left the buffer: at the last node
  the "next segment" of the cyclic walk was the node-to-node curve, whose *handles* gave
  it a direction, so a 14° arc was appended before the cap and every cap spoke was
  phase-shifted by it. That is the only difference in the raster of a well-behaved stroke
  before and after: alternating ±9/255 on cap perimeters, coverage identical.
- The **joint arc sweeps the short way** (`_brush_joint_arc()`). A fixed rotation covered the
  exterior wedge on one side of a turn and went the long way round on the other, a
  near-full circle of interior spokes per joint. At a cusp, where the two are equal, the
  pass's rotation decides, the same rule the caps follow, so each pass covers one half of
  the tip disc. Verified on the #1313 cusp case at all eight frame sizes.

## The boundary, by definition

The drawn outline is the boundary of the union. A border sample is on it if and only if
it is not strictly inside any other sample's disc. `_brush_outline_boundary_skips()`
answers that per sample, from the same two arrays the pipe paints, and publishes the
answer as the skip ranges every consumer of the outline already reads
(`dt_masks_skip_range_t`, out-of-band). Nothing is intersected, nothing is chosen between,
and the shared detector is no longer called for a brush.

Two searches, because the discs that can hide a sample come from two places, and what
separates them is where they sit **along the walk**, not where they sit in the plane:

- **Near.** A disc can contain a border sample only if its centre is within its own radius
  of it, and the sample is one radius from its own spine point, so the two centres are
  within two radii of each other in the plane and, the spine being a continuous line,
  within two radii of each other along it. That much walk either side of the sample's own
  disc is exhaustive for folds, joints and caps. The window is a *length* of walk, found by
  bisection on the walk to each disc's centre, never a count of discs: a disc is made
  wherever the spine moves half a pixel *or the radius steps*, so a count is a different
  length at every sampling density and at every flare, and the count this used to be —
  four times the largest radius, plus eight — was ten times too wide once the outline was
  sampled at the density the screen shows.
- **Far.** A hairpin, a crossing or a spiral — and the brush's own second pass down the
  other side — brings discs from any distance along the walk to within one radius in the
  plane. A bucket grid of one reach per cell finds them, and each bucket holds its discs as
  *runs* of consecutive indices: a run inside the window is the near part, already
  answered, dismissed in one comparison; only runs from far along the walk are tested disc
  by disc.

The test itself is arranged to be counted rather than computed. The discs are flat arrays —
centre x, centre y, and the squared radius less the tolerance, negative for a disc the
tolerance leaves nothing of — so a probe is one squared distance against each, no square
root anywhere, in blocks of eight the compiler can vectorise; and each block carries the
box its centres span and its largest radius, so a block that cannot reach the probe costs
four comparisons. The previous test took a hypot per disc for the copy test below, and
dismissed a block on its first disc's distance against a reach padded by the largest step
between discs. Measured on the corpus, per probe: 430 disc tests at 4.5 ns each became 126
to 470 at about 2.5 ns, and the pass on the cusp went from 19 ms to 7.

Cost is bounded by decimation, not by the sample count: consecutive samples closer than
half a pixel with the same radius are one disc, and a sample is only *probed* when it has
moved half a pixel from the last probe, the samples between two agreeing probes taking
their answer. Measured on the corpus, the probed version produces the same skip ranges to
the sample as probing everything, at a third of the probes. `-d masks -d perf` prints what
a pass cost and how many discs it tested.

A first version used a coarse occupancy map of the union for the far part. It was
conservative, so it left every sample within a few pixels of a far boundary undecided,
and refining those exactly meant refining every sample, because every boundary sample is
within a few pixels of its *own* stroke's interior. Position cannot tell near from far;
the index can. Measured: 30 ms → 250 ms with the map and its band, back to 3–32 ms with
the runs.

| case | kept samples | inside the union | outside the permitted region | build at step 1 | kept at step 3 | build at step 3 |
|---|---|---|---|---|---|---|
| brush-1313-cusp (5184×3888) | 44,549 | 0 | 0 | 11 ms | 2,057 | 1.3 ms |
| brush-cusp | 45,000 | 0 | 0 | 14 ms | 3,989 | 2.3 ms |
| brush-hairpin | 41,612 | 0 | 0 | 20 ms | 5,584 | 4.0 ms |
| brush-zigzag | 87,969 | 0 | 0 | 25 ms | 7,172 | 3.6 ms |
| brush-selfcross | 70,707 | 0 | 0 | 24 ms | 6,217 | 3.7 ms |
| brush-concave | 62,058 | 0 | 0 | 21 ms | 5,605 | 3.8 ms |
| brush-1360-pressure-ramp | 36,682 | 0 | 0 | 34 ms | 2,187 | 5.3 ms |
| brush-1352-radius-step | 20,319 | 0 | 0 | 5 ms | 1,111 | 0.9 ms |
| brush-1074-flare | 27,432 | 0 | 0 | 26 ms | 2,475 | 4.9 ms |

"Build" is the whole GUI-side call, `dt_masks_get_points_border()`: producer, transform
and boundary pass; the step is the density the outline is sampled at, see the next section.
At every step the band check reads the same: zero kept samples inside the union, zero
outside the permitted region. Before the rework, kept outline samples that sat two to five
pixels inside the union numbered 47 to 206 per case; the old display cut through
self-crossings without stopping at all.

## The outline is built at the density the screen shows

The GUI outline used to be sampled at one image pixel whatever the zoom: "pixel-accurate",
which at fit zoom on a 24 Mpx raw in a 2560 px window is five samples per device pixel —
and the recursion that places them stops on integer parts, so it lays several samples
around every integer crossing and a 1313 cusp came to 53,917 samples for a border some
10,000 px long. Everything downstream is paid per sample: the distortion transform, the
boundary pass, the stroke, and the hit test that walks every sample on every pointer
motion.

The step is now the view's. The expose reads what one device pixel spans in image pixels
from the transformed context (`dt_draw_min_emit_step()`, the same measure the circle already
decimated its strokes by) and publishes it through `dt_masks_gui_set_outline_density()`;
`dt_masks_form_gui_t::outline_step` is the density in force and `outline_step_built` the one
the cached outlines were built at, and a difference between the two is a rebuild, exactly
like a geometry move. `dt_masks_distort_for_gui()`, the GUI supplier every outline build
composes through, reads the density back with `dt_masks_gui_outline_step(dev)` — so a
drag's rebuild, an expose's and the creation session's all sample at the density in force
without their callers knowing it. The brush and the polygon sample their curves, their arcs
and their stamps at it; the pipe's walk is untouched,
its arcs and stamps still one sample per pixel whatever spoke budget it was given (the walk
carries a separate `arc_step`), so no raster changes under a preference. The polygon used to
pin its threshold at one pixel for the pipe's scanline fill, which needs every row crossed;
the GUI fills nothing.

What a step buys, measured with `ansel-test-masks-geometry --time-overlay` on the corpus,
the first frame after a rebuild of the selected shape:

| view | step | cusp-1313 | flare | pressure ramp | comb (polygon) | group of 11, all |
|---|---|---|---|---|---|---|
| 1:1 | 1 | 18 ms | 33 ms | 39 ms | 28 ms | 218 ms |
| fit, 2560×1440 | 2 | 4.7 ms | 12.9 ms | 13.6 ms | 15.6 ms | 80 ms |
| quarter | 4 | 2.9 ms | 5.4 ms | 6.2 ms | 6.1 ms | 30 ms |

Before both changes the same first frames read 29, 103, 69, 49 and 459 ms at step 1, which
was the only step there was. On a HiDPI screen the device pixels are the physical ones, so a
2x screen at fit zoom sits one step below a 1x one.

The corpus judges pixel-accurate outlines by default and `MASKS_OUTLINE_STEP=<n>` has it
judge the outline every n pixels against the same full-resolution maps: at 3 and at 5 every
case still reads zero inside, zero outside. The baselines are step-1 pictures and are not
compared at any other step; the timing run takes its density from its view
(`MASKS_OVERLAY_VIEW`), the way the darkroom does.

## The border sample is on the envelope, not on the normal

The boundary pass decides per sample, and it is only as good as the samples it is given. A
border sample was placed on the normal of its spine sample, `c + r N`, and that point is on
the boundary of the union only while the radius is constant. Where the radius changes along
the spine the union's boundary is the envelope of the discs,

    c + r · ( −r′ T ± √(1 − r′²) N ),   r′ = dr/ds

tilted off the normal by asin(r′) toward the smaller radius, and the normal sample sits
inside the union by about r′² r / 2. At the rates a pen draws that is a fraction of a pixel:
invisible to the eye, and precisely what the boundary pass rejects. Measured on the #1313
corpus brush at the darkroom's fit zoom, 19,785 of 53,709 border samples were skipped in 25
ranges, most of them along both long sides of the stroke, and the dashed outline was simply
absent there — reported as "discontinuities in the dashed border". The pixels around every
sample were checked in the cairo render and in the rasterised one: kept samples painted,
skipped ones not, both renders 18 pixels apart, so the drawing was never the suspect.

The raster had the same defect with the same cause and nobody had noticed: the spokes end on
the same samples, so the shoulders of a fat node — where the radius grows fastest — were
painted only out to the normal offset, a flat shelf tens of pixels short of the round lobe
the union of discs actually is (the #1313 brush again: 13,333 pixels brighter after, none
darker).

`dt_masks_outline_envelope_offset()` in `masks_outline.c` now places every border sample of
the brush and of the polygon, from the tangent and the radius rate by the same parameter. The
rate is the derivative of the smoothstep the radius follows along a segment,
`(r2 − r1) · 6t(1 − t)`, zero at both ends, so a segment's end samples stay on the normal and
every cap, joint arc and stamp built from them is unchanged; a limit direction (a cusp end)
carries no speed and is given rate zero. Every constant-radius corpus case stayed
bit-identical; the two whose radius varies moved by the crescents above and their baselines
were regenerated.

`DT_MASKS_OUTLINE_TILT_MAX` is 1.0: the exact envelope, the clamp only keeping the square root
real where the discs nest. A first version capped the tilt at 0.7, reasoning that a family of
spokes tilted further leans too far along the spine to paint the width. That reasoning was not
measured, and it cost a real defect. `_MG_1074.CR2` brush #4, as the user drew it, flares from
a 132 px node to a 342 px one over 287 px, and the radius rate along that segment peaks near 1:
the discs almost nest, and the union's top is the rear envelope of the flare, 40 px outside the
wide node's circle at a tilt of 64°. No capped sample reached it, the wide node's circle was
correctly found inside, and the outline lost the whole top of the shape — found only from the
darkroom's own frame dump (`MASKS_DUMP_OVERLAY`), since the sidecar on disk no longer held the
shape. At the exact envelope the raster oracle reports no missing pixel on any corpus case,
this one included, and the top closes.

Two traps from the round that landed it. The corpus judges only the samples that were KEPT —
a stretch the skips swallow whole passes the outline band check, so the skipped fraction has
to be measured: `ansel-test-masks-geometry --time-overlay` prints it per case and
`MASKS_DUMP_SKIPS=<dir>` dumps every spoke with the range that skips it. And a parameter added
to the evaluator landed, by regex, before the radius instead of after it on two of its five
callers: every segment end was evaluated at radius zero, every cap collapsed into two spirals
through its centre, and the result read for an hour like a cap defect the tilt had exposed.
Trace the helper's inputs before theorising about its geometry.

## A copy of a boundary stretch is not a boundary sample

The walk stamps a full disc at a node whose radius steps, in both passes, and bridges every
joint with an arc about the node at the same radius. On a flaring node all of those trace the
same circle, and the boundary pass kept them all: a copy is not strictly inside any disc. Each
copy is stroked as its own run with its own dash phase, so the copies fill each other's gaps and
the circle comes out as a near-solid line — `_MG_1074.CR2` brush #4, reported as a missing
dashed border: 4,020 kept samples on a 2,114 px circumference.

`_outline_sample_repeats()` drops a sample within three quarters of a pixel of an earlier one
that lies at least four pixels of border behind it along the walk, whatever discs the two belong
to: for drawing, two boundary samples that close are one line. The samples are hashed by pixel
cell and the test reads the nine cells around the probe; it used to ride inside the disc test,
walking the samples of every disc whose circle passed near the probe, which is what put a
square root in that loop. The other side of the stroke,
which the backward pass lays on the very same spine points, is a diameter away and never
matches. The same rule takes the stretch where a segment leaves a stamped node, whose envelope
runs within the boundary tolerance of the node's circle for tens of pixels — a second dash over
the first — which no identity of discs or spine points could pair. A dropped run of one or two
samples between kept ones is kept again (`_outline_keep_specks()`): hiding it changes nothing on
screen and cuts the run in two.

Five corpus rounds shaped the clauses, each measured by the harness's skipped-sample and range
counts per case before the next. Keyed on the disc the rule never fired, because a disc's centre
is its first sample's and a stamp's samples merge into the disc the last moving sample opened,
half a pixel off and differently in each pass. Keyed on the spine point it dropped the copies
and missed the junctions. With half a pixel of centre tolerance it fused consecutive discs of a
segment, which the builder separates by exactly that much, and thinned every segment to
fragments. Excluding the sample's own run by a count of sixteen samples shredded every arc,
because the recursion samples a hundredth of a pixel apart around every integer crossing, where
sixteen samples are less than a pixel; only the length of border walked between two samples
tells a run from its copy, a copy being the other pass or another stamp, thousands of pixels
away along the walk. The kept-sample count on the node's circle went 4,020, 3,980, 1,804; the
zigzag's ranges 4, 2,821, 4.

The dashes are the other half of the same report. The rasteriser cut them by arc length but
restarted the pattern at every sub-path, and an outline is one sub-path per kept run, so every
run boundary bunched or stretched a dash. The pattern's state now travels across all the
sub-paths of one stroke: a dash is a function of the pixels of border drawn before it, and a
dash cut by a hidden stretch shows as a stub, which that metric owes.

## What is unified, and what is not

The API did not change shape for this: `dt_masks_functions_t.get_points_border`
(`masks_functions.h:103-105`) still fills `points`/`points_count`, `border`/`border_count` and
`border_skips`/`border_skip_count`, members of the `dt_masks_outline_out_t` it is handed,
returns a `dt_masks_raster_result_t`, and no consumer was touched. (There is no `payload`
out-parameter; an earlier draft listed one. The `fading` one it has since grown carries a
contour made from the payload, not the payload: see
[Where the fading starts](#where-the-fading-starts).) What
changed is *where the truth comes from*. The skip ranges are now derived from the raster
arrays by a definition, so the outline the GUI draws is the boundary of the mask the pipe
paints by construction, not by a second algorithm agreeing with the first.

What still differs, and why it should:

- The **frame and the pitch**. A mask is rendered in the module's input frame at the pipe's
  resolution; an outline is drawn in the viewer's frame at one sample per pixel. Both go
  through the same producer with different distortion sets; that is the geometry service's
  job, not the brush's.
- The **boundary pass runs on the GUI side only**. The pipe has no use for it: every spoke
  is a radius of a disc it owes. If a consumer ever needs the boundary in pipe space — a
  vector export of masks, say — the same function applies to the pipe's arrays unchanged.

## Where the fading starts

*Measured on top of `a8997b8e28`, 2026-11-11, on a RelWithDebInfo build: the costs with
`tests/masks/masks_geometry.c --time-overlay 30`, twice each way, the lower kept, and the
module's own with `MASKS_DEBUG=1`; the edge against the mask by the corpus's `fading edge` judge
and, at one cell per pixel, by dumps of each corpus brush's spokes, edge and mask from a
temporary probe in the harness.*

`_brush_falloff()` and `_brush_falloff_roi()` paint each spoke at its sample's full density over
the first `fading` of its length, from the spine sample to the border sample, and ramp to zero
over the rest. The mask is at full density out to `points[i] + fading_i × (border[i] − points[i])`
on every spoke, and fades past it. A circle draws that contour solid and its border dashed. A
brush drew its spine solid, a line that carries the nodes but marks no change in the mask, so
nothing showed where its fading started, or that a scroll on the fading had changed it.

`_brush_get_points_border()` asks the producer for the payload as well and turns it into that
contour in place: a payload holds two values per sample as an outline holds two coordinates, so
each sample's fading is read, then its two cells are overwritten with the point. The header
repeats the outline's. `get_points_border` hands the contour out through `fading`,
index-aligned with the border and as long; every other shape leaves it NULL, its outline being
that contour already. Over 18 builds of the corpus, counted with a temporary probe, the payload
was always as long as the border. The point is taken on the spoke between two display-space
samples, as the pipe takes it between two module-space ones: a distortion downstream of the
module that bends a spoke moves the painted point off the straight one the overlay draws.

That contour is not what the overlay draws. `dt_masks_gui_form_create()` traces from it the edge
of the part the brush paints at full density, `dt_masks_solid_edge()` (`masks_solid.c`), into
`gui_points->fading` as closed loops, and frees it. `_brush_events_post_expose()` strokes those
loops solid on every brush, selected or not, before the spine; the dashed border stays on the
selected shape.

### The edge is traced, not walked

The solid part is the union of the bands (c_k, f_k, f_k+1, c_k+1) between neighbouring spokes,
c a spine sample and f its contour sample. Where the spokes lie along the spine -- a size that
climbs steeply, a small fading on a wide node -- the edge of that union is made of the spokes'
sides, not of the line through their ends: on `brush-fading-neck`, 94 px of its 872 px of edge
lie more than 2 px from that line, on `brush-1074-flare-b` 9 of 4375, on the other brushes of the
corpus none. A pass over the contour samples can keep or hide them; it cannot draw a spoke's
side. A boundary pass run on the spine and the contour, each sample tested against the bands,
was the first approach, and left the ends' contour short of the middle node's on that brush.

So `masks_solid.c` fills the union and traces its edge, without cairo:
`tools/check_module_boundaries.sh` counts 20 files in `develop/` that name a toolkit type, and a
twenty-first fails it.

- **Runs.** A band's orientation is its area's sign, taken about its own spine sample: about the
  origin, thousands of pixels out, float cancellation decides it. Consecutive bands of one
  orientation make a run, one polygon -- spine forward, contour back, a spoke at each end --
  wound positively, and the union is where the winding is not zero. Two runs that meet share a
  spoke, added once with both windings. A band with a vertex that is not finite is skipped and
  cuts the run. The band from the last sample back to the first is only there when the two are
  within a cell.
- **Turns.** A run turns over only once its bands have swept back a tenth of a cell, as width,
  area over L1 perimeter. A long spoke trembling by a hundredth of a pixel turned it at every
  band, and every turn adds two spokes as long as the fading to the fill: on
  `brush-1360-pressure-ramp` at one cell per pixel, 4621 runs and 4.7 M crossings, against 1399
  runs and 2.8 M with the tenth.
- **Bow ties.** Where a band's two spokes cross, its two lobes are wound opposite ways; the one
  against its run counts −1, cancels the band it lies on, and leaves a hole. Left alone, that
  gave `brush-1352-radius-step` three holes, two of them 15 to 20 px long, and each flare one.
  The lobe is added again the other way, twice. These lobes are slivers -- on
  `brush-1360-pressure-ramp` 99 % of them thinner than 0.2 px -- and they make about 1.8 M of its
  2.8 M crossings, counted from their height, but none can be left out: dropping those thinner
  than a tenth of a cell opened loops up to 86 px inside the solid part of both flares and
  `brush-1352-radius-step`, slivers along neighbouring spokes stacking.
- **Fill.** A grid of `dt_masks_gui_outline_step()` cells -- one device pixel in image pixels,
  never less than one -- over the box with two empty cells round it; past 8 M cells or 32 M
  crossings, the cells grow. Four sub-scanlines per row, exact crossings, non-zero spans, and
  exact coverage along x, into one byte per cell. A sub-scanline's crossings are sorted by
  insertion up to 32, and past that a byte at a time on the bits of their x, which are positive:
  at one cell per pixel on `brush-1360-pressure-ramp`, some 830 crossings a sub-scanline, the
  fill took 125.8 ms with `qsort()` and takes 57.7, its edge the same to the bit on every brush
  dumped.
- **Spine.** Every cell the spine of a kept band crosses is then set full, the segment walked from
  cell to cell across their sides, so the cells it marks share a side: each step leaves the cell
  by the side the segment reaches first, computed from the segment's start, so no error builds up
  along it. The spokes start on the spine, so the solid part holds it; where they lie along it,
  that part is a thread round it, and a thread thinner than half a cell is under half coverage.
  Traced from the fill alone, `brush-fading-neck`'s neck, some 0.5 px thick -- across it, cells of
  50 to 175 out of 255, read with a probe on the fill, the level at 128 -- breaks into islands
  that float rounding chooses: 9 loops in the RelWithDebInfo build, with `-ffast-math`, and 7 in a
  Debug build, without it, where 9 px of the neck have no edge point within 4 px and the judge
  below fails. With the spine set full, the neck is one loop round the spine in both builds (on
  top of `aff63448d6`, 2026-11-12). The walk takes 0.59 ms in median over the corpus's 39 fills
  and 1.88 to 2.00 ms at most, on 178,887 bands, over two runs with a probe in the RelWithDebInfo
  build (on top of `c34db68a44`, 2026-11-12).
- **Trace.** Marching squares at half coverage with linear interpolation, a saddle resolved by
  the mean of its corners. Each loop is followed from a horizontal crossing among the cells the
  fill wrote and closed on its first point, and a zero-length break starts each loop but the
  first, which `dt_masks_draw_outline_runs()` strokes as one path.

In the RelWithDebInfo build, `brush-selfcross` traces 2 loops, the stroke enclosing a hole where
it crosses itself, and every other brush of the corpus traces one, `brush-fading-pinch` among
them: its solid part narrows to a point at the middle node, where the fading is 0.00001, and the
spine joins its two halves there, under the node's disc. In a Debug build, `brush-1074-flare`
traces 4: one of the three more is a hole about a pixel wide inside its solid part, a few pixels
from its edge at (3703, 3096), which the judge below does not see (on top of `aff63448d6`,
2026-11-12).

### How close it is

The raster lays each spoke on whole pixels and paints its left and upper neighbours too, so its
full part (`mask >= 1`) is wider than the union by 0.5 to 1.25 px on average, and fills a one-pixel
gap between two arms. Against it, at one cell per pixel, on every brush of the corpus but
`brush-1360-pressure-ramp`, whose density varies so that its full part is not where the fading
starts:

- an edge point inside the frame lies 0.69 to 0.98 px from the centre of the nearest pixel on
  the full part's edge, in median, and at most 2.53 px, except where `brush-hairpin`'s edge goes
  round the gap the raster fills between its arms (7.25 px);
- 0 to 27 px a brush are inside the edge and outside the full part;
- the corpus judges the other way, which a gap or a cut in the line breaks
  (`_judge_fading_edge()`): every pixel on the full part's edge within 4 px of an edge point. All
  are within 2.47 px on `brush-fading-neck` and 2.65 px on the 18 other brushes and frames, in
  the RelWithDebInfo build and the Debug one alike (on top of `aff63448d6`, 2026-11-12).

### What it costs

The lower of two runs, in ms, at a 2560×1440 view, fit zoom -- an outline step of 2 on the
5 kpx frames, 1 on the 3 kpx ones -- without the fading edge and with it; the frame includes the
harness's own background, about 1 ms, and the build is the first frame, which builds the outline:

| case | member frame | selected frame | build, member |
|---|---:|---:|---:|
| brush-1313-cusp | 2.34 → 3.26 | 3.68 → 4.55 | 9.7 → 11.8 |
| brush-zigzag | 4.70 → 8.63 | 7.88 → 11.39 | 21.6 → 32.5 |
| brush-selfcross | 4.44 → 7.63 | 7.08 → 10.08 | 23.4 → 29.4 |
| brush-1360-pressure-ramp | 3.58 → 4.53 | 5.57 → 6.50 | 24.4 → 32.7 |
| brush-1074-flare | 3.92 → 5.24 | 5.35 → 6.55 | 26.7 → 31.1 |
| brush-fading-neck | 3.15 → 3.63 | 3.76 → 4.22 | 18.7 → 21.0 |
| polygon-comb (no fading edge) | 5.91 → 5.82 | 8.57 → 8.58 | 19.9 → 19.9 |
| group-11 (eleven shapes) | 3.10 → 3.22 | 5.39 → 6.51 | 149.2 → 198.1 |

A frame strokes one more line about as long as the border, solid where the border is dashed;
stroking a quarter of its points saved 0.5 to 0.8 ms, so it is drawn whole. In a build, the
module fills and traces in 0.8 to 7 ms at fit zoom (`brush-1360-pressure-ramp` 6.5 + 0.5 ms,
268 k crossings). At one cell per pixel it fills in up to 57.7 ms on that brush, 21.8 on
`brush-1074-flare`, 16.2 on `brush-1074-flare-b` and 15.5 on `brush-zigzag`, the others under
10, and traces in at most 8.8.

## The polygon

A polygon's mask is its path's interior, filled by an even-odd scanline over the path
samples, plus a feather: the union of a disc of the local radius over every point of the
path, painted as spokes from each path sample to its border sample with a linear falloff.
The border is the path offset outward, so it folds wherever a concave run bends tighter than
the radius, exactly as a brush's does. The polygon had grown its own answer to that: a
detector that intersected the border with itself on a pixel grid, and cut ranges that every
consumer honoured — the GUI to draw, the hit-test to count crossings, and **the rasteriser**,
which sent every spoke inside a cut to the fold's crossing point instead of its own border
sample. The producer also fed its joint arcs from the buffer's tail and from ten samples
before it, and its recursion wrote its caller's scratch — NaN at the top level, the origin
below it — into the border wherever a segment had no direction: the brush's #1360, waiting
for a pen on a polygon.

Three things follow the brush's design now, and one more turned out to be wrong before:

- **The walk takes everything from the node data and the segment end samples.** The path is
  closed, so it is walked once with the border outside (the winding folds into the sign of
  the radius); a degenerate segment contributes nothing and the joint that closes over it is
  between its two live neighbours; the joint that closes the path is made explicitly at the
  end, between the last live segment and the first. Joint arcs sweep the short way through
  `dt_masks_outline_short_way()`, the winding deciding a tie.
- **The outline is the boundary by the same definition**, through the same function
  (`dt_masks_outline_boundary_skips()`, now in `masks_outline.c`): a border sample is on it iff
  it is not strictly inside any other path sample's disc. That is sufficient for a polygon
  without a point-in-path test: a border sample that lies inside the interior got there by
  crossing the path, and the crossing point is a path sample less than a radius away.
- **The rasteriser paints every spoke.** A spoke inside another disc paints nothing new under
  the max-over-spokes fill, so the cuts had nothing to protect the raster from — and what
  they did to it was measurably wrong. A redirected spoke ran from a fold sample to the
  crossing point, which is √(r² + t²) away, farther than the radius, so its falloff was
  stretched and every reflex notch came out brighter than the ideal `1 − d/r` feather:

  | `polygon-comb`, pixels the two rasters disagree on | mean error | RMS error |
  |---|---|---|
  | before, redirected spokes | +0.0052 | 0.0090 |
  | after, every spoke to its own border | −0.0013 | 0.0065 |

  measured against the distance transform of the path. The cuts, the detector, the pixel
  grid it ran on, the fill-gaps helper and `dt_masks_skip_ranges_build()` are gone, with the
  unit test that pinned the latter's invariants; `dt_masks_skip_contains()` stays for the
  brush's handle finder.

The reported polygon (`polygon-1788045925`, issue #1313's second shape), judged the way the
brush is now:

| frame | outline samples inside the union, before | after |
|---|---|---|
| 5198×3904 | 633 | 0 |
| 4000×3000 | 486 | 0 |
| 2137×1603 | 189 | 0 |

with 0 missing and 0 excess coverage in both states: the raster was complete before, the
outline was not the boundary. The comb's folds the old cuts happened to handle (0 before and
after), which is what made the previous detector look sufficient.

## Circle and ellipse

Nothing to propagate. A circle's border is a concentric circle of radius `r + feather`; an
ellipse's is an ellipse with both semi-axes enlarged (or scaled, in proportional mode) —
neither is a normal offset, so neither can fold, and the drawn curve is the contour the
raster computes from the same parameters. Their outline is the boundary of their raster by
construction.

## Judging it: the corpus

`tests/masks/masks_geometry.c` builds the disc union of a stroke twice from the node
list: **owed** (the smaller of each segment's two radii, shrunk two pixels) which the mask
must cover entirely, and **permitted** (the larger, grown three pixels) outside which the
mask must be empty. Excess is what issue #1360 was, and an owed-only oracle cannot see it:
every owed pixel was painted, and then some.

The drawn outline is judged against the same two maps: every kept border sample must
land between them. A sample inside the owed map is a fold, an arc or a crossing the
outline failed to hide (issue #1352's chords); a sample outside the permitted map is a
spoke to nowhere.

A case is a table of **nine columns** per node — `node.x, node.y, ctrl1.x, ctrl1.y, ctrl2.x,
ctrl2.y, border, density, fading` (`tests/masks/masks_geometry.c:118`) — and #1360 is the
reason `density` is per node rather than per shape: it cannot be expressed without a per-node
density ramp. (`_brush_1313[11][9]` is eleven *nodes* of nine columns; an earlier draft read
that shape as "eleven columns … both radii, density, fading, state", and there is neither a
second radius column nor a state one.) Two reported shapes were added
verbatim from the sidecars: `_brush_1360` (43 nodes) and `_brush_1352` (7 nodes).

`MASKS_DUMP_OUTLINE=1` writes every case's outline CSV, skip ranges included, whether or
not it fails. The baselines live in the private sample bank and were regenerated for this
change; the raster ones differ only on cap perimeters, the overlay ones wherever the old
outline was not the boundary.
