# The mask overlay is rasterised, not stroked

Status: implemented on `overlay-raster`. Measured with `tests/masks/masks_geometry.c --time-overlay`;
every number below comes from that harness at a 2560×1440 view, fit zoom, 30 frames per figure.

## What the overlay asked of cairo

A shape's outline arrives at the drawing code already sampled at the resolution it is shown
at: the producers walk it one point per raw pixel, the drawer keeps one per device pixel
(`dt_draw_min_emit_step()`). Until now that polyline was handed to cairo as a path and
stroked twice — a wide dark pass under a narrow bright one — with fast antialiasing, one of
two dash patterns, round or flat caps. Raster in, vector path, raster out: cairo tessellated
a few thousand one-pixel segments into trapezoids and rasterised them, twice, for every shape,
every frame. Then the whole thing was composited from a `cairo_push_group()` the size of the
view, painted or not.

Of everything cairo offers, the overlays use exactly that: two widths, two colours with
alpha, `DASH_STICK` (12/12 px) and `DASH_ROUND` (3/12 px), `CAIRO_LINE_CAP_ROUND` or
`BUTT`, `CAIRO_ANTIALIAS_FAST`. Nodes, handles, arrows, the clone source shape, the brush's
creation trace and cursor discs are a handful of small primitives each, and stay with cairo.

## The rasteriser

`src/widgets/stroke_raster.{h,c}` — a widget-layer primitive, cairo and glib only, no masks
vocabulary. `dt_stroke_raster_path(cr, style)` takes cairo's current path (already built by
the shape's own `draw_shape_func`), flattens it, maps it through `cr`'s matrix into the pixels
of the surface `cr` draws on — the current group's, if one is pushed, offset included — and
strokes it; the path is consumed as `cairo_stroke()` would consume it. When that surface is
not an ARGB32 image the call returns FALSE with the path intact and the caller strokes with
cairo, so nothing changes for a target this cannot write.

The coverage model is a distance field over the polyline's bounding box: each segment stamps
`reach² − d²` (MAX, so the nearest point wins; no square root until compositing) into a
scratch plane whose resting state is zero, over the pixels within `reach` — the widest pass's
half-width plus the antialiasing pixel. Joins and caps are the capsule's own, round; a flat
cap drops the pixels past the segment's end plane. Dashes are cut along the polyline's arc
length before stamping, so a dash end is a cap exactly as cairo's is. Compositing walks only
the spans each row wrote — `clamp(R + ½ − d, 0, 1)` per pass, dark then bright, premultiplied
OVER — then zeroes those spans again, so the plane is never cleared as a whole. The surface
records what was touched as cairo user data, for whoever composites it.

`dt_draw_shape_lines()` and `dt_draw_stroke_line()` (`widgets/draw.h`) build the style from
the same numbers they always passed to cairo — widths in user units, the dash lengths through
`dt_draw_dash_lengths()`, the overlay colour and contrast — and try the rasteriser first.

## The canvas

`dt_masks_events_post_expose_with()` no longer pushes a group. It keeps one ARGB32 image
surface the size of the clip in pixels (created once, kept across frames, given the target's
device scale), draws every shape into it through a `cairo_t` carrying `cr`'s matrix shifted to
the clip's origin, and composites onto the view only the rectangle that was painted: the
rasteriser's own record, plus a bound on what cairo may draw on top — the node header of every
outline, node and both control points, three per node, with a margin for a node's disc and an
arrow head. That bound is computed *before* anything is drawn and the canvas context is clipped
to it, so a handle that lands outside the estimate is not painted rather than left behind for
the next frame (the harness draws a panned frame after every state and fails on any pixel a
fresh surface would not show). The rectangle is then cleared by hand for the next frame. A creation
session, which paints through its own cached pattern, takes the whole canvas.

## Measured

Per shape per frame, the two states a drag frame is made of:

| case | member (path only) | selected (path, dashed border, nodes) |
|---|---:|---:|
| brush-1313-cusp (11 nodes) | 7.56 → **1.49** ms | 9.49 → **2.49** ms |
| brush-1360-pressure-ramp (43 nodes) | 8.47 → **2.51** ms | 11.67 → **4.27** ms |
| brush-zigzag (6 nodes, 98 k samples) | 12.37 → **2.88** ms | 17.99 → **5.33** ms |
| polygon-1788045925 (15 nodes) | 7.01 → **1.72** ms | 8.97 → **2.65** ms |
| polygon-comb (12 nodes) | 12.65 → **4.04** ms | 16.96 → **6.47** ms |

The harness paints its own background each frame (~1 ms), which the darkroom pays as its
image blit anyway; it is inside both columns. The steps, from the traces (`MASKS_DEBUG=1`):
the strokes alone fell from ~6 ms to ~3 ms on the pressure ramp; the full-view composite of the
group, 4 ms per frame, became a composite of the painted rectangle.

The pixels: on the corpus's screen renders, painted-pixel counts agree within 1–3 %, no
pixel differs by more than 51/255 except single end-cap pixels — a one-pixel stub under a
node whose flat end is round now, the first pixel of one dash — and no raster (alpha)
baseline moved at all. The 23 overlay baselines were regenerated.

## Traps

- **Disc stamping scallops thin lines.** Stamping a disc per vertex leaves necks of `s²/8R`
  between vertices `s` apart; at the bright pass's half-width that shows. The capsule per
  segment is the fix, and per-pixel it costs a dot product more than the disc.
- **Inside a pushed group, `cairo_get_target()` is the ORIGINAL target.** The group's surface
  is `cairo_get_group_target()`, and it carries a device offset. The unit test paints inside a
  clipped group and checks the composite lands where cairo puts it.
- **Cairo's device space is NOT the pixel grid, and `cairo_user_to_device()` stops there.** A
  surface applies its own device transform after the CTM: pixel = device × device_scale +
  device_offset, the scale being what a HiDPI widget carries (2 on a 2x screen) and the offset
  what a pushed group carries (minus its clip's origin, already in pixels). Measured with
  pycairo: on a surface with device scale 2, `user_to_device(10, 10)` is `(10, 10)` and
  `get_matrix()` is the identity; a group pushed under a clip at (20, 20) reports offset
  (−40, −40). The first version mapped device units straight to pixels, which is invisible on
  every 1x screen and every unit test, and put the whole overlay at half size in the top-left
  quadrant of a 2x darkroom. So every place that derives a pixel from `cairo_user_to_device()`
  — the rasteriser's vertices and widths, the canvas frame, the cairo bound — multiplies by the
  surface's device scale and adds its offset; the canvas takes the target's device scale, its
  matrix is `cr`'s followed by a translation of the pixel shift divided by that scale, and the
  composite positions it in the same units. `test_stroke_raster` paints on a device-scaled
  surface, plain and inside a clipped group, and the harness renders every case at device
  scale 2 and compares the painted bounding box with the 1x render's.
- **`cairo_copy_path_flat()` is the cheap bridge.** The shapes' path builders are unchanged;
  extracting the flattened path costs microseconds and keeps one implementation of every
  outline walk.
