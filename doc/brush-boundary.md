# The brush and polygon boundary: one geometry, two consumers

Status: the brush landed in #1381 (issues #1352, #1360); the polygon, and the shared boundary
pass, on `polygon-boundary`. Measured with `tests/masks/masks_geometry.c`; every number below
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
| sample pitch | the pipe's `mask_rasterization_step` | 1 px |
| what it does with the arrays | stamps every spoke | draws `border` as a dashed polyline, `points` as the centreline, hit-tests against both |

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
separates them is their **index** along the walk, not their position:

- **Near.** A disc within twice the largest radius of the sample's own spine position can
  contain it and no other can, so a window of discs either side of the sample's own is
  exhaustive for folds, joints and caps. Consecutive discs move at most a step, so the
  window is scanned in blocks of eight and a block whose first disc is out of reach is
  skipped whole.
- **Far.** A hairpin, a crossing or a spiral brings discs from any distance along the walk
  to within one radius in the plane. A bucket grid of one reach per cell finds them, and
  each bucket holds its discs as *runs* of consecutive indices: a run inside the window is
  the near part, already answered, dismissed in one comparison; only runs from far along
  the walk are tested disc by disc.

Cost is bounded by decimation, not by the sample count: consecutive samples closer than
half a pixel with the same radius are one disc, and a sample is only *probed* when it has
moved half a pixel from the last probe, the samples between two agreeing probes taking
their answer. Measured on the corpus, the probed version produces the same skip ranges to
the sample as probing everything, at a third of the probes.

A first version used a coarse occupancy map of the union for the far part. It was
conservative, so it left every sample within a few pixels of a far boundary undecided,
and refining those exactly meant refining every sample, because every boundary sample is
within a few pixels of its *own* stroke's interior. Position cannot tell near from far;
the index can. Measured: 30 ms → 250 ms with the map and its band, back to 3–32 ms with
the runs.

| case | kept samples | inside the union | outside the permitted region | build |
|---|---|---|---|---|
| brush-1313-cusp (5184×3888) | 33,753 | 0 | 0 | 12 ms |
| brush-cusp | 45,009 | 0 | 0 | 14 ms |
| brush-hairpin | 41,629 | 0 | 0 | 19 ms |
| brush-zigzag | 88,011 | 0 | 0 | 24 ms |
| brush-selfcross | 70,833 | 0 | 0 | 24 ms |
| brush-concave | 62,264 | 0 | 0 | 18 ms |
| brush-1360-pressure-ramp | 36,927 | 0 | 0 | 32 ms |
| brush-1352-radius-step | 18,822 | 0 | 0 | 5 ms |

"Build" is the whole GUI-side call, `dt_masks_get_points_border()`: producer, transform
and boundary pass. Before the rework, kept outline samples that sat two to five pixels
inside the union numbered 47 to 206 per case; the old display cut through self-crossings
without stopping at all.

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

The tilt is capped at `DT_MASKS_OUTLINE_TILT_MAX = 0.7` (the sine of the angle). At a rate
of 1 the discs nest and there is no envelope; between 0.7 and 1 the spokes of a family tilted
to the envelope lean so far along the spine that nothing paints the width. With the cap, every
point across the width at a sample is reached by the spoke of a sample one radius further on
(r_j ≥ 1.41 r_i whenever the rate exceeds 0.41), so the union stays painted whatever the rate;
past the cap the sample is a little inside the union and the outline skips it, which is the
right answer for a radius growing almost as fast as the pen moves.

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

`_outline_sample_repeats()` drops a sample that repeats an earlier one: the same spine point to
a hundredth of a pixel, a border position within three quarters of a pixel, and at least sixteen
samples earlier along the walk. The other side of the stroke, which the backward pass lays on
the very same spine points, is a diameter away and never matches; polygons, which bridge each
joint once, and constant-radius brushes are bit-identical.

Three corpus rounds shaped the clauses. Keyed on the disc rather than the sample's spine point
the rule never fired, because a disc's centre is its first sample's and a stamp's samples merge
into the disc the last moving sample opened, half a pixel off and differently in each pass. With
half a pixel of centre tolerance it fused consecutive discs of a segment, which the builder
separates by exactly that much, and thinned every segment to fragments. Without the index
exclusion a dense arc filler matched its own predecessor and shredded every arc. The
measurements that told those apart were the harness's skipped-sample counts per case — 2,821
ranges on the zigzag against 4 — and the kept-sample count on the node's circle: 4,020, then
3,980, then 1,804.

## What is unified, and what is not

The API did not change shape: `dt_masks_functions_t.get_points_border` still returns
`points`, `border`, `payload` and `border_skips`, and no consumer was touched. What
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

Cases carry eleven columns now — both radii, density, fading, state — because #1360
cannot be expressed without a per-node density ramp. Two reported shapes were added
verbatim from the sidecars: `_brush_1360` (43 nodes) and `_brush_1352` (7 nodes).

`MASKS_DUMP_OUTLINE=1` writes every case's outline CSV, skip ranges included, whether or
not it fails. The baselines live in the private sample bank and were regenerated for this
change; the raster ones differ only on cap perimeters, the overlay ones wherever the old
outline was not the boundary.
