# The darkroom repaint: what a frame costs, and what it may not cost

The darkroom's centre is repainted on the GUI thread, on the CPU, and it is repainted often:
on every pipe frame, on every mask hover and drag motion, on every guide toggle. Whatever a
frame paints is time the GUI thread cannot give to anything else, so the floor of a frame --
what it costs when nothing but an overlay has moved -- decides how the darkroom feels under
a drawing tablet. Issue #1198 measured that floor at about 15.7 ms of the 16.7 ms a 60 fps
frame allows and listed what it was made of. This is the account of what it was actually made
of, measured, and of what was done about it.

## The floor, priced

Every number below is a cairo operation timed on this machine at a 2560x1440 window, once at
device scale 1 and once at 2 (a 5120x2880 buffer of 59 MB), which is a 2x screen. The
operations are the ones the repaint path performed, in the order it performed them, on every
frame, whatever had changed.

| operation, per frame | at 1x | at 2x |
| --- | ---: | ---: |
| allocate, first-touch and free a full-window ARGB surface | 2.2 ms | 29.5 ms |
| fill the whole window with a colour | 1.8 ms | 5.9 ms |
| blit a full-window surface onto another, pixel for pixel | 1.2 ms | 5.8 ms |
| composite a mostly transparent full-window ARGB canvas | 0.0 ms | 0.0 ms |
| blit a 400x300 rectangle of it, clipped | 0.03 ms | 0.12 ms |
| scale a full-window preview by 0.73 with cairo's default filter | 66 ms | 262 ms |
| the same with the NEAREST filter | 1.0 ms | 3.7 ms |
| memset a full-window canvas row by row | 1.2 ms | 5.7 ms |

And the path, from the GTK draw signal down, before any overlay was drawn:

1. `dt_control_expose()` allocated a full-window ARGB surface and freed it at the end (row 1).
2. It filled it with the toplevel background (row 2).
3. The darkroom composed its image surface: a full-window background fill (row 2) and the
   image blit from the pipe's cacheline (row 3), on every frame, for an image that had not
   changed since the last one.
4. With ISO 12646 on, a white rectangle the size of the image plus its border was filled
   under the image (row 2 again, nearly).
5. `_paint_all()` blitted the composed image surface into the throwaway surface (row 3).
6. `dt_control_expose()` blitted the throwaway surface into a persistent pixmap (row 3).
7. The draw handler blitted the pixmap into GTK's own double buffer (row 3).

Seven passes over the window: about 10 ms at 1x, about 59 ms at 2x, before the first overlay
pixel. During a brush creation session the overlay canvas was composited and cleared over the
whole window as well (rows 3 and 8). And a zoom or a pan, while the main pipe catches up,
scaled the preview with the default filter (row 6): a third of a second per frame at 2x,
because the `CAIRO_FILTER_NEAREST` the code set was set on the context's default solid source,
which the surface then replaced.

Nothing on the request side could narrow any of it. `gtk_widget_queue_draw_area()` appeared
nowhere in the tree; every redraw invalidated the whole widget, so the clip GTK hands the
draw signal was always the whole window, and the draw handler discarded even that.

## What was done

**The centre paints into GTK's buffer.** GTK's `cr` is a double buffer already, and it
arrives clipped to the region that was invalidated. `dt_control_expose(cr, width, height)`
paints into it directly: rows 1, 6 and 7 are gone, and every remaining pass is clipped to the
region GTK asked for. The persistent pixmap and its copy on resize are gone with them. A view
whose expose paints every pixel of the centre says so with `VIEW_FLAGS_PAINTS_WHOLE_AREA`, and
the toplevel skips its background fill under it (row 2, once): the darkroom does.

**The image is composed once per source frame.** `dev->image_surface` is keyed on everything
it depends on -- the source frame's hash, the viewport, the colours, the border, the frame size
-- and an expose that finds the same key repaints from it without composing. A pipe frame
still costs the compose; a frame that only moved an overlay costs one clipped blit. The
background is filled where the image will not cover, as the four bands around it in one
even-odd fill whose hole is a pixel inside the image so no seam shows, and the ISO 12646
frame is a ring: 1.3 ms against 5.9 at 2x. The preview fallback sets its filter on the pattern
that scales.

**A motion the masks handled repaints what it touched.** The masks record the rectangle their
last frame composited, in the view's coordinates (`_overlay_damage`), and the darkroom asks
`dt_masks_overlay_queue_redraw()` for that rectangle grown by the pointer's motion since --
a dragged shape moves with the pointer, a hovered one does not move -- when the masks handled
the motion and nothing else did; a module's own overlay knows no rectangle and keeps the whole
widget. The invalidation is an estimate made before the frame is drawn: when a frame outgrows
it, `_overlay_damage_record()` compares the composited rectangle with the clip of the expose
and asks for the rest, and the next, small expose paints it. At worst that is one extra small
frame; it can never leave part of an overlay unpainted.

**The overlay canvas is the view, and a session frame is bounded.** The canvas was sized to
`cr`'s clip, which a rectangle redraw narrows: it would have been reallocated on every such
frame and held nothing beyond the clip. It covers the view and stays; what reaches the target
is the composite, and cairo clips that. A creation session's frame took the whole canvas
before -- a full-window composite and a full-window memset on every frame drawn -- and spans
the session's box and the live shape's now. The session's box covers the borders and not the
spines alone, which is also what a brush's dashed border, a radius outside its spine, needed
from the pattern's clip after a pan.

## The static layer: the members that did not change are not stroked again

After the four rules a pipe frame still re-stroked every member of the visible group: 1 to
8 ms a shape at fit zoom, and a drag makes a pipe frame per motion, so a mask-heavy edit paid
tens of milliseconds of GUI thread per motion for shapes that had not moved. The members that
are not the selected one are now stroked once into a view-sized surface, `_static_layer` in
`masks_gui.c`, and composited under the live canvas with one blit, clipped to what the redraw
asked for. Its key is the view matrix, the group, the selection, the overlay colours and a
signature of every other member's outline -- its counts and every 32nd sample -- so a member
an undo moved is caught, while the selected member, which a drag rebuilds on every motion, is
not in it at all. A pan, a zoom or a change of selection pays one full stroke of the others,
which is what every frame paid before; a rebuild also marks the whole frame dirty, so a
selection change under a small invalidation gets its one full repaint.

With it, cairo's own drawing -- nodes, handles, arrows -- is bounded to the selected member's
header alone, which is all cairo draws for a group: bounding every member's header had made
a spread-out group's dirty rectangle the whole window on every frame, and with it the
composite, the clear and the rectangle a motion asks a redraw of.

Measured by the harness's `group-11` case, every brush and polygon of the corpus in one group
at fit zoom on a 2560x1440 surface, RelWithDebInfo, per frame, of which about 1.8 ms is the
harness's own background paint:

| group of 11, per full frame | before | after |
| --- | ---: | ---: |
| nothing selected | 14.1 ms | 4.0 ms |
| one member selected | 16.4 ms | 5.7 ms |

The first frame of that group, 340 ms, is the rebuild of every member's outline and is the
next item (#1391).

## What a frame costs now

| frame | before, 2x | after, 2x |
| --- | ---: | ---: |
| a mask hover or drag motion, overlay only | ~59 ms + overlay | ~0.2 ms + overlay |
| a new pipe frame | ~59 ms | ~13 ms: bands 1.3, image 5.8, GTK blit 5.8 |
| a zoom or pan while the main pipe catches up | ~320 ms | ~10 ms |

The overlay itself -- the outlines rasterised directly, see `doc/overlay-raster.md` -- costs
1 to 8 ms per shape at fit zoom and is now the largest term of a motion frame, which is where
the next work belongs.

## Reading it

`-d perf` prints `[darkroom] surface prepared / image painted / overlay predicates / overlays
drawn / redraw` per expose; `-d perf -d masks` adds `[masks] overlay composited (WxH of WxH)`,
which now prints a small rectangle of the view on a motion frame and the whole view on a pipe
frame. `MASKS_DUMP_OVERLAY=<dir>` writes what a frame drew.
