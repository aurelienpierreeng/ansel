<!-- Provenance: every finding carries the commit it was established against. -->

# Masks: brush and polygon geometry

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> How a brush's and a polygon's outline and raster are derived from their nodes, and the defects that shaped the current design.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

The full account is [`brush-boundary.md`](brush-boundary.md); this file is the rules and the traps.

## A brush point array records the centerline twice, and only the forward half is drawn

*Found `178a7e413e`, 2026-08-28.*

`gui_points->points` for a brush holds, in this order: three header points per node (`ctrl1`,
node, `ctrl2`), then the centerline sampled **forward** from the first node to the last, then the
**same** centerline sampled backward. The border wraps around the stroke, so the line under it is
walked there and back (`_brush_get_pts_border()`); the backward half is never drawn.

The forward half ends at `_brush_centerline_end()` — `node_count * 3 + (points_count - node_count
* 3) / 2`, i.e. half of the **samples**. Half of the whole array is a different number: the header
belongs to neither pass, so counting it in falls one and a half points per node short, and the
last node's own coordinate sits at the very end of the forward pass. Everything that walks the
drawn centerline (the outline stroking, the source shape, the clone link's midpoint) uses that
helper.

## `inside_border` is proximity to the feather's outer line, not the feather band

*Found `447c35d38e`, 2026-09-07.*

Two curves are in play and the vocabulary runs them together. `gui_points->points` is the **form
line** — the shape itself, whose segments are hoverable and draggable. `gui_points->border` is the
**outer line of the feathering**, and the band between the two is what "border" names everywhere
else in this code. `inside_border` names neither the band nor the form line: it is the cursor
being **on the outer line**.

That matters because `dt_masks_find_closest_handle_common()` (`masks_gui.c`) answers in one fixed
order — source, border, segment, shape — so whatever a shape's `get_distance()` reports as
`inside_border` preempts its segment. For a brush and a polygon alike, `inside` is "enclosed by
the outer line" — the feathering is part of the shape and drags it — and `inside_border` is
"within cursor reach of the outer line itself, and no form-line segment is closer". Neither line
carries a drag action of its own (the feathering is edited through the per-node handle and the
wheel), so where both are within reach at once, the segment wins.

Reporting the **band** instead cuts the segment's reach in half along the form line, and each
shape loses a different half. A brush's `points` are the centerline walked there and back (zero
area) while its `border` wraps the whole painted band, so a point-in-polygon test on that border
is true across the entire stroke: the segment — drag to move it, Ctrl+Click to insert a node —
became unreachable everywhere, while the shape still looked perfectly hoverable. A polygon's
feathering lies wholly *outside* its form line, so reporting the band left the segment reachable
from the inside of the form line alone; the whole outer half of the cursor's reach, every pixel of
it inside the feathering, went to the border instead. The wider the fading, the more one-sided it
looks, and approaching a segment from the feathering grabbed the whole shape.

`_brush_get_distance()`'s source pass walks a different outer line, so its distances need their
own accumulator — sharing one running minimum lets a clone source near the form veto every segment
hit on the form itself.

## A drawing pass must not leave a path in the cairo context

*Found `178a7e413e`, 2026-08-28.*

Cairo keeps the current path across calls, so a leftover is painted by the next `cairo_stroke()`
**anywhere**, in that stroke's own style. `dt_masks_draw_path_seg_by_seg()` strokes one segment per
node and stops on a node boundary, so it ends with `cairo_new_path()`; the creation session's
per-shape loop (`masks_gui.c`) does the same between shapes.

The symptom shape is a piece of one shape adopting another shape's style, or appearing only when
something else happens to be drawn: a leftover tail comes out dashed when the shape's own dashed
border is stroked next, comes out highlighted when another shape is hovered, and is invisible when
nothing is drawn after it. That is a path leak, not a style or selection bug.

Which segments exist at all is decided by the caller's shape: an open path (a brush — the only
caller asking for round ends, since only an open path has two true ends) has `node_count - 1`
segments, a closed one has one more, and a shape still being created has no closing segment yet.
The walk stops once it has stroked them all, so it must be handed a point count that lets it
**reach** the last node.

## Brush masks rasterize as radial spokes — wedge holes across the stroke (CLOSED)

*Found `8400a289b4`, 2026-08-08.*

> **CLOSED. This said `(OPEN)` until 2026-09-29 and it should not have.** The mechanism the
> report hypothesised was real and was fixed on 2026-08-31 by `55e5ec79a8`, *"brush: the V hole
> at a cusp — restore the join arcs, and stop testing a float for zero"*: the joint arcs had been
> switched off behind an always-FALSE `allow_border_gap_rounding` flag, so a joint had no border
> samples across it and no spokes were stamped there. That flag had 3 occurrences at the report
> commit and has **0** today; joints now go through `_brush_joint_arc()`
> (`develop/masks/brush.c:947`) unconditionally, and the corpus runs a two-sided oracle with an
> explicit MISSING map. Left marked OPEN, this section sent a reader to re-derive a landed fix.
>
> *One honest caveat:* the reporter's `_DSC9410.NEF` 57-node brush was never added to the corpus,
> so this is **fixed-and-unverified-on-that-file** rather than formally re-closed against it. The
> original report is kept below because its diagnosis method is the reusable part.

Reported 2026-08-08 on `_DSC9410.NEF` (sidecar alongside it): a 57-node brush leaves four
wedge-shaped holes in its mask, the largest 1336x966 px. **Not caused by the gtk.h/widgets
refactor** — exports from that branch and from master are bit-identical, image and mask
channel alike (0 differing pixels of 24,160,256).

Two things make the diagnosis quick, and both were got wrong on the first pass:

- **They are not the interiors of self-crossing loops.** A brush paints a stroke of finite
  width, so a loop's interior legitimately stays unpainted, and the largest hole looks exactly
  like that at a glance. Look at the small ones at full resolution instead: each has a
  *perfectly straight* edge cutting across the stroke, which no smooth outline produces.
- **They are not the sparse-sampling path.** `use_sparse` is now
  `pipe->mask_rasterization_step > 1` (`develop/masks/brush.c:3269`); the six hand-rolled copies
  of `has_preview_output() || type == THUMBNAIL` were deliberately removed because one pipe could
  be classified two ways inside a single frame (`develop/pixelpipe_hb.h:491`). The conclusion is
  unchanged and now holds structurally: the step defaults to 1 and only `dev->gui_attached`
  raises it, so an `ansel-cli` export runs the full-sampling branch and the holes were present
  there. *This paragraph named the old predicate until 2026-09-29; grepping brush.c for it finds
  nothing.*
  `!dev->gui_attached`. An `ansel-cli` export therefore runs the full-sampling branch
  (`sparse_step = 1`), and the holes are present there.

The mechanism the shape points at: the mask is not a filled polygon but the **union of radial
spokes** — for each border sample `i`, `_brush_falloff_roi()` stamps a segment from centreline
point `points[i]` out to `border[i]`. Where consecutive spokes fan out faster than the border
sampling density, the wedge between them is never stamped, and both its straight edges are
spokes. That is precisely "holes orthogonal to the path".

Where to look: `_brush_get_pts_border()`, and the two arc-filling helpers
`_brush_points_recurs_border_gaps()` / `_brush_points_recurs_border_small_gaps()`. Both bail
out with `if(l < 2) return;` where `l = |delta_angle| * max(r1, r2)` — a pixel-count test on
arc length, which is the shape of a threshold that is right at the resolution it was tuned for
and too coarse elsewhere. **Measure before theorising**: dump `points`/`border` for this brush
and check actual spoke spacing at the four hole coordinates. This file's history (see the
highlights sections above) is full of plausible-but-wrong theories that survived source
reading and died on the first measurement.

Reproduce: export with `--export_masks 1`; page 1 of the TIFF is the mask. Flood-fill from the
border and anything left unset is a hole.

## A brush's or polygon's geometry comes from its nodes, and its outline is the boundary of its raster

*Found `9d054ca5b4`, 2026-09-06.*

A brush is the union of a disc of the local radius over every point of its spine, and the
pipe paints it as spokes from every spine sample to its border sample; a polygon is its path's
interior plus the same feather outside it. `doc/brush-boundary.md` is the full account; the
rules that were each paid for by a reported defect:

- **Nothing in `_brush_get_pts_border()` is read back out of the buffers.** Every cap, joint
  arc and stamp takes its centre and radius from the segment end samples that meet there and
  from the node data. The old walk measured a stamp's radius as "the distance from the last
  centreline sample to the last border sample written"; a degenerate segment (a pen resting
  under rising pressure: seventeen coincident nodes) once left the image origin in the border
  buffer, the radius came out as 2058 px, and ten discs of that size followed (#1360).
- **A degenerate segment contributes nothing**; its disc is the neighbour's cap. An end with a
  radius but no direction borrows the other end's *direction*, never a position.
- **The drawn outline is decided per sample by a definition** — a border sample is on the
  boundary iff it is not strictly inside any other sample's disc
  (`dt_masks_outline_boundary_skips()` (`develop/masks/masks_outline.c:735`)) — not by intersecting the outline with itself and
  choosing cuts. Every ordering of those cuts moved the artefact somewhere else (#1352). The
  answer travels as the same skip ranges every consumer already reads; the rasteriser keeps
  every spoke, because a spoke inside another disc paints nothing new and costs nothing.
- **Near and far discs are told apart by index, never by position.** A window along the walk
  is exhaustive for folds, joints and caps; a bucket grid of index *runs* finds the discs a
  hairpin or a crossing brings back from far along the walk, dismissing near runs in one
  comparison. A coarse occupancy map was tried first and made the build eight times slower:
  every boundary sample is within a few pixels of its *own* interior, so a map's band is
  everything.
- **Joint arcs sweep the short way; at a cusp the pass's rotation decides.** A fixed rotation
  went the long way round on one side of every turn. Do not "fix" the cusp tie-break to
  shortest-path: the two passes each cover one half of the tip disc, and which half is which
  is the pass's rotation. The #1313 cusp corpus, at all eight frame sizes, is the check.
- **A border sample lies on the ENVELOPE of the discs, not on the normal.** Where the radius
  changes along the spine, the boundary of the union is `c + r·(−r′ T + √(1 − r′²) N)`, r′ being
  dr/ds: tilted off the normal by asin(r′). The normal sample sits inside the union by about
  r′² r / 2 — a fraction of a pixel at a pen's rates, invisible to the eye and exactly what the
  boundary detector (rightly) rejects, so the outline of any stroke whose radius varied lost
  whole stretches of both sides (37% of the #1313 brush's border, reported as "discontinuities
  in the dashed border"), and the raster's spokes, which end on the same samples, left the
  shoulders of a fat node as a flat shelf tens of pixels deep. `dt_masks_outline_envelope_offset()`
  (`masks_outline.c`) places every brush and polygon sample; the rate is the smoothstep's
  derivative, zero at both ends of a segment, so caps and joint arcs are untouched and every
  constant-radius corpus case stayed bit-identical. `DT_MASKS_OUTLINE_TILT_MAX` is 1.0, the exact
  envelope: a first version capped the tilt at 0.7 to keep the spokes painting across the width,
  and that cap cost a real defect — a segment whose rate peaks near 1 (a 132 px node flaring to
  342 px over 287 px, `_MG_1074.CR2` brush #4 as the user drew it) has its union's top on the
  rear envelope, 40 px outside the wide node's circle at a 64° tilt, which no capped sample
  reached, so the outline lost the whole top of the shape; at the exact envelope the raster oracle
  reports no missing pixel anywhere in the corpus. Two traps from the round that landed it: the
  corpus judges only the samples that were
  KEPT, so a boundary stretch the skips swallow whole passes it — measure the skipped fraction
  (`ansel-test-masks-geometry --time-overlay` now prints it; `MASKS_DUMP_SKIPS=<dir>` dumps every
  spoke with its range) — and an argument added to the evaluator by regex landed before the
  radius instead of after it on two of five callers, which zeroed every segment end's spoke,
  killed every cap, and read for an hour like a cap defect the tilt had exposed.
- **A repeat of a boundary sample is not a boundary sample.** The walk stamps a full disc at a
  node whose radius steps in BOTH passes and bridges joints with arcs about the same node, so a
  flaring node's circle was traced two or three times, each copy dashed from its own phase, and
  the copies filled each other's gaps: a near-solid line (`_MG_1074.CR2` brush #4, 4,020 kept
  samples on a 2,114 px circumference); and where a segment leaves such a node its envelope runs
  within the boundary tolerance of the circle for tens of pixels, a second dash over the first.
  `_outline_sample_repeats()` drops a sample within three quarters of a pixel of an earlier one
  that is at least `OUTLINE_REPEAT_MIN_WALK` (4 px) of border BEHIND it along the walk, whatever
  discs the two belong to: for drawing, two boundary samples that close are one line. Five corpus
  rounds shaped it, each measured by the harness's skipped/ranges counts before the next: keyed
  on the disc it never fired (a disc's centre is its first sample's, half a pixel off and
  differently per pass); on the spine point it missed the junctions; a half-pixel centre
  tolerance fused consecutive segment discs and thinned every segment; an exclusion by sixteen
  SAMPLES shredded every arc, because the recursion samples a hundredth of a pixel apart around
  every integer crossing, where sixteen samples are less than a pixel — only the walked length
  tells a run from its copy, a copy being the other pass or another stamp, thousands of pixels
  away along it. The other side of the stroke, which the backward pass lays on the same spine
  points, is a diameter away and never matches; `_outline_keep_specks()` keeps a dropped run of
  one or two samples between kept ones, since hiding it changes nothing and cuts the run.
- **The boundary pass is counted, not computed, and its window is a length of walk.** A disc can
  hide a border sample only if the two centres are within two radii in the plane, hence within
  two radii along the spine; the near window is that much WALK either side of the sample's own
  disc, found by bisection on the walk to each disc's centre. It used to be a count of discs
  (four times the largest radius, plus eight), which is a different length wherever the radius
  steps and was ten times too wide once outlines were sampled at the screen's density. The discs
  are flat arrays (centre x, centre y, squared radius less the tolerance) tested by squared
  distance in blocks of eight, each block carrying its centres' box and largest radius; the copy
  test lives in a hash of the samples by pixel cell and reads nine cells, where it used to walk
  the samples of every disc whose circle passed near the probe, with a hypot per disc. Measured:
  the same skip ranges to the sample on the whole corpus, 4.5 ns per disc test to 2.5, the pass
  on the 1313 cusp 19 ms to 7. `-d masks -d perf` prints `[masks] boundary pass: ...` per build.
- **The GUI outline is sampled at the density the screen shows, not at one image pixel.** The
  expose reads what one device pixel spans (`dt_draw_min_emit_step()` on the transformed
  context) and publishes it with `dt_masks_gui_set_outline_density()`; `dt_masks_distort_for_gui()`
  reads it back through `dt_masks_gui_outline_step(dev)`, so every build -- a drag's, an
  expose's, the creation session's -- composes at the density in force without its caller
  knowing it, and `outline_step_built` is part of the outline cache key beside the geometry
  generation. At fit zoom on a 24 Mpx raw the old fixed step was five samples per device pixel
  (the recursion stops on integer parts, so it lays several around every integer crossing):
  53,917 samples for a border 10,000 px long, each paid in the transform, the boundary pass, the
  stroke and the hit test. The pipe's walk is untouched -- its arcs and stamps stay one sample
  per pixel through the walk's own `arc_step`, whatever spoke budget it was given -- so no
  raster changes under a preference; the polygon's pixel threshold stays pinned at 1 for the
  pipe's scanline fill and follows the density for the GUI. Measured, the first frame after a
  rebuild of the selected shape: the 1074 flare 103 ms -> 33 at 1:1, 13 at fit, 5 at quarter;
  a group of 11 members 459 -> 218 / 80 / 30. `MASKS_OUTLINE_STEP=<n>` runs the corpus's band
  check at that density (0 inside / 0 outside at 3 and 5); the baselines are step-1 pictures
  and are not compared at any other step. The corpus dev has no expose, so the harness
  re-applies its density before every build: the overlay it writes beside each case goes
  through the darkroom's expose at full resolution, which publishes 1.
- **A hit test asks the shape's box before walking a sample.** A motion hit-tests the SELECTED
  member only (nodes, handles, then every sample through `get_distance`), throttled to half a
  cursor radius; a press hit-tests every member to choose one. #1391's "a million distance tests
  per motion" was a press at the raw density; measured with `--time-overlay`'s `[HIT]` sweep
  (20x20 positions), a motion after the density change is 0.01-0.8 ms and a press on 11 members
  0.6-3.6 ms. `dt_masks_form_gui_points_t::bbox` spans points, border and source, filled by
  `dt_masks_gui_points_update_bbox()` when `dt_masks_gui_form_create()` builds the entry and
  emptied by `dt_masks_gui_form_remove()`; the four sample-walking hit tests (brush, polygon,
  circle, ellipse) return their initialised "nothing" answers when `dt_masks_gui_points_reach()`
  says the cursor, grown by twice its radius, cannot touch the box -- twice because the ellipse
  tests its border at 1.5 radii. Those answers are exactly the walk's for such a cursor, which
  is why every consumer reads the flags and none the distance. The gradient walks no samples and
  has no box test. Measured: a motion 0.003-0.5 ms, a press on the group 0.26 ms at fit and
  1.6 at 1:1, the same hits at every position.
- **The dash phase is the arc length along the whole stroke, not along each sub-path.** An
  outline is one cairo path of many sub-paths, one per kept run between skips, and cairo (and
  the rasteriser, at first) restarts the dash pattern at every sub-path, so dashes bunched and
  stretched at every run boundary. `dt_stroke_raster_path()` now carries one `_dash_t` across
  all its sub-paths: a dash is a function of the pixels of border drawn before it. A dash cut by
  a hidden stretch shows as a stub, which that metric owes.
- **`MASKS_DUMP_OVERLAY=<dir>` makes the darkroom write what each frame drew**: the canvas
  before it is composited and cleared, the frame and dirty rectangles, and every cached outline
  with its skip ranges (`outline-<n>.txt`, the harness's format). It exists because a darkroom
  cannot be driven from this machine, and a report the harness cannot reproduce needs the
  darkroom's own frame, not a guess at it.

- **`ansel-test-masks-geometry --time-overlay` renders at fit zoom; `MASKS_OVERLAY_VIEW=
  "scale,ox,oy[,ppd]"` renders the view the darkroom shows**, in user units with the surface's
  device scale, which is how the 1074 report was reproduced at 100% on a 2x screen. When adding
  a comparison surface to that harness, give it the same device scale: the first leftover check
  at ppd 2 compared a scaled frame against an unscaled one and reported 50,000 ghost pixels that
  were the harness's own.

The corpus (`tests/masks/masks_geometry.c`) judges a brush and a polygon in **both directions** — owed
coverage missing, and coverage no disc owes — and judges the drawn outline against the same
two maps. `MASKS_DUMP_OUTLINE=1` dumps every outline. An owed-only oracle passed #1360 while
half the frame was painted.

- **The polygon's rasteriser paints every spoke too.** It used to send every spoke inside a
  self-intersection cut to the fold's crossing point, which is √(r² + t²) from the sample —
  farther than the radius — so every reflex notch came out brighter than the `1 − d/r` feather
  (measured: mean error +0.0052 → −0.0013 against the path's distance transform). The boundary
  pass is `masks_outline.c`, one function for both shapes; the shared detector, the polygon's
  own, and `dt_masks_skip_ranges_build()` are gone. Circle and ellipse need none of this: their
  borders are concentric or enlarged curves, never a normal offset, and cannot fold.

## `dev->roi.raw_width`/`raw_height` must be set for every `dev`, not just `gui_attached` ones — every drawn shape's absolute position depends on it

*Found `af036c7f1b`, 2026-08-09.*

`dev->roi.raw_width`/`raw_height` (`develop.h`, doc-commented "Dimensions of the full-resolution
RAW image being worked on") are read by `dt_dev_coordinates_raw_norm_to_raw_abs()`
(`develop/develop.c`) to convert a shape's normalized (0..1) center/points into absolute pixel
coordinates — the first step of every drawn-mask shape's own area/mask function
(`masks/circle.c`, `ellipse.c`, `brush.c`, `gradient.c`, `polygon.c`: all read
`dev->roi.raw_width`/`raw_height` directly, several also route through the same coordinates
helper). `_dt_dev_mipmap_prefetch_full()` (`develop/develop.c`), called from every
`dt_dev_load_image()` through `dt_dev_ensure_image_storage()` → `_dt_dev_load_raw()`, is the only
place that sets them — but it used to do so **only `if(dev->gui_attached)`**, left over from a
commit that gated the surrounding GUI-viewport fields (`orig_width`, `preview_width`, ...) the
same way without noticing `raw_width`/`raw_height` aren't GUI state — they're an objective fact
about the loaded raw buffer, needed by any `dev`, headless or not.

For a `gui_attached` dev (the live darkroom) this was invisible: `raw_width`/`raw_height` get set,
shapes resolve correctly. For a throwaway, non-interactive `dev` — `imageio_core.c`'s export
`dev`, `dev_snapshot.c`'s `frozen`, thumbnail generation — `dev->roi.raw_width` stays `0` (its
calloc default), and `dt_dev_coordinates_raw_norm_to_raw_abs()` early-returns on `raw_width==0`
**without transforming the points at all**. A shape's normalized center (e.g. `(0.85, 0.35)`) then
masquerades as if it were already in absolute pixel coordinates, added to a radius term that
*is* correctly scaled to pixels (`radius * MIN(pipe->iwidth, pipe->iheight)`, passed as a
parameter, not read from `dev->roi`) — so the resulting bounding box lands within a few hundred
pixels of the image origin, its position dominated by the (correct, large) radius term and the
shape's own (tiny, ~0..1) normalized center contributing almost nothing. Two shapes at wildly
different real positions on the image collapse to nearly the **same** wrong bounding box, since
their normalized centers differ by less than 1.0 while the radius term is hundreds of pixels —
this is the tell that distinguishes this bug from an ordinary ROI/ROI-offset mismatch. Depending
on whether that degenerate bounding box happens to overlap the module's own `roi_in` for the
current render, a shape's effect either gets rejected outright (empty ROI intersection, e.g.
`iop/retouch.c`'s `rt_build_scaled_mask()`) or gets "applied" against mask values sampled from
the wrong, mostly out-of-bounds region of the source mask buffer (silently zero-filled by
`rt_build_scaled_mask()`'s own `dt_iop_image_fill(mask_tmp, 0.0f, ...)`), producing a real kernel
call that visibly changes nothing. Both outcomes were observed on the same image, same run: one
circle rejected, the other "applied" with zero measured pixel difference in the export.

Fixed by setting `raw_width`/`raw_height`/`raw_inited` unconditionally in
`_dt_dev_mipmap_prefetch_full()`. The two `dev->roi.raw_inited` checks that also gate on
`dev->roi.gui_inited` (`dev_pixelpipe.c`'s virtual-pipe resync, `develop.c`'s own zoom-scale
getters) stay correctly GUI-only through that second, genuinely-GUI flag — this fix doesn't
change their behavior. This was found chasing a report that `iop/retouch.c`'s clone/heal/blur/fill
had no visible effect at export and in the darkroom "before/after" snapshot compare: two
narrower, real fixes were needed first (`rt_process_forms()`/`_cl()` must resolve shapes through
`pipe->forms`, and `dev_snapshot.c`'s `history_override` path must resync `frozen->forms` — both
documented below) before this coordinate bug became the sole remaining, and actually dominant,
cause — restoring it alone (verified by CLI export pixel-diff against a debug build) turned two
previously invisible edits, including one healing over a blown bokeh highlight, fully visible.

