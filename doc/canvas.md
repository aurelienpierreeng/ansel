# Canvas

Canvas is a view (an "atelier") for laying images out: an infinite plane where the
frames of a photo book, the walls of an exhibition or the order of a series are tried
out, with notes and arrows between them. It answers the question the lighttable cannot:
do these images help or hurt each other at this size, in this order, next to that one.

Nothing about a canvas touches the library database. A canvas is a file, and the file
is complete on its own: it opens on a machine that has neither the raws nor the library,
and it can be handed to someone who has neither.

## The document

`src/canvas/` is the backend, layer 6.5 in `tools/include_graph.py`: above `imageio/`,
which it needs to render, below `views/` and `libs/`, which are its only consumers.

| file | owns |
| --- | --- |
| `canvas.h/.c` | `dt_canvas_t`: the objects, their geometry, draw order, layouts, load and save |
| `canvas_format.h/.c` | the binary index and the archive entry names |
| `canvas_zip.h/.c` | a ZIP writer and reader over zlib |
| `canvas_render.h/.c` | the library boundary: identity, sync status, the render job, decoding |
| `canvas_paint.h/.c` | drawing a canvas into a cairo context |
| `canvas_markdown.h/.c` | Markdown to Pango markup |
| `canvas_pdf.h/.c` | the one-page, colour-managed PDF |
| `canvas_actions.h` | the action vocabulary shared by the view and its toolbar |

Four kinds of object share one struct, `dt_canvas_object_t`: an **image frame** (a
library render), a **text frame** (Markdown, or the `.txt` sidecar of an image frame), a
**connector** (a line from one frame to another, see below) and a **map frame** (a slippy
map around a point, see below). Every object has an id unique within
the canvas, never reused, which is what connectors and sidecar links refer to. Frames have
a centre, a size, a rotation, a draw order and an optional border of their own; the canvas
carries the default border, the grid, the background and the saved viewport.

Coordinates are canvas units: one unit is one screen pixel at zoom 1, the origin is the
centre of the plane, y grows downwards. The view converts through `_to_canvas()` and nothing
else, so the document never learns what a pixel is.

The four layouts space frames by the gutter and, with snapping on, start on the grid and
round every cell up to whole grid steps, frames sitting top-left in their cell.

### The file

A `.anselcanvas` is a ZIP archive:

```
mimetype            application/x-ansel-canvas, stored first and uncompressed
canvas.bin          the binary index, deflated
images/<id>.jpg     one sRGB JPEG per image frame, with the sRGB profile embedded, stored
texts/<id>.md       one Markdown file per text frame, deflated
```

The JPEGs and the Markdown are ordinary files inside an ordinary archive, so a file
manager, a print shop or a script gets at them without Ansel.

The index is the part that is not self-describing, and it is built to be extended without
a migration. Every record carries **reserved bytes**, written as zeros, read back verbatim
and kept in memory, so a later version can claim them for a new field; and every object
record is prefixed with its own size, so a reader positions the next record without
knowing this one's fields, and a record longer than the reader expects is skipped past
rather than misread. The unit test `test_canvas_document` grows a record by sixteen bytes
and checks the next one still parses. The fields that ARE stored for an image frame are
what a library needs to find the original again: id, version, film roll id, folder, file
name, history hash, source dimensions, orientation, and the EXIF a caption needs (maker,
model, lens, exposure, aperture, ISO, focal length, exposure bias, date taken).

The connector's anchors and routing are the first fields added this way: 12 bytes taken
from the front of its 128 reserved bytes, the record size unchanged, no version bump.

`DT_CANVAS_FORMAT_VERSION` is bumped only when an existing field changes meaning. A newer
version's file is refused with `DT_CANVAS_ERROR_VERSION`; an older one reads, with its
missing trailing fields as zeros.

### Why a hand-written ZIP

No archive library is linked, and adding one touches every packaging path (Windows,
macOS, Flatpak, the nightly manifests). zlib is already a hard dependency through libpng,
and the subset of the format a self-produced archive needs -- `store`, `deflate`, one
central directory, no ZIP64 -- is four hundred lines. The writer produces the archive in
a sibling `.part` file and renames over the destination on commit, so an interrupted save
never leaves half a canvas where a whole one was; the reader keeps the file open and reads
entries on demand, so opening costs the central directory, not the JPEGs. `test_canvas_zip`
round-trips both entry kinds, refuses a flipped byte through the CRC, and, when `unzip` is
installed, has it verify what we wrote.

## The library boundary

`canvas_render.h` is the only place the canvas meets the library, and it meets it three ways.

**Identity.** `dt_canvas_render_describe_source()` copies what the library knows about an
image into the frame; `dt_canvas_render_locate_source()` finds it again -- by id when the
id still names the same file and version, by folder and file name when the file was
re-imported under a new id, and nothing when it left the library. `dt_canvas_render_check()`
turns that into a **sync status**: current when the library's history hash equals the one
the frame was rendered with, stale when it differs, missing when nothing matches. The status
is runtime only. The view checks every frame on entering the atelier and on opening a file,
draws a badge on the frames that are not current (orange stale, red missing, blue
rendering), and re-renders the stale ones automatically when `canvas/auto_refresh` is set.

**Rendering.** `dt_canvas_render_start()` queues a job on `DT_JOB_QUEUE_USER_EXPORT` that
runs the full pipeline through `dt_imageio_export_with_flags()` into an in-memory format
(the `mime() == "memory"` idiom `libs/print_settings.c` uses), at the canvas's long edge,
to sRGB 8-bit, and encodes a JPEG with the sRGB profile embedded. The job never touches the
canvas: it is given a library id and an object id, and the callback runs on the GUI thread
with a **token** the view chose when it opened the document, so a render finishing after
the document was replaced is recognised and dropped. The history hash is read before the
export, so a commit landing during the render is seen as stale by the next check rather
than claimed by this render.

**Decoding.** `dt_canvas_surface_cache_t` turns a frame's JPEG into a cairo surface, keyed on
the JPEG bytes' identity and, for the display, on the display profile generation, so a
re-render or a monitor change decodes afresh; it evicts least-recently-used surfaces past
a byte budget (`canvas/surface_cache_mb`). The atelier's cache converts to the display
profile through `dt_colorprofiles_rgba8_to_display_bgra8()`; the PDF export's keeps sRGB.

## Painting

`canvas_paint.h` draws a canvas into a cairo context whose user space is already canvas
units. The same painter serves the centre view and the PDF, so what is printed is what
was shown; the options differ only in the colour target, the grid and the placeholders.
Borders are stroked inside the frame's edge and the content is inset by them; the image
pattern's filter is set after `cairo_set_source_surface()`, on the pattern that scales (the
trap `doc/darkroom-redraw.md` records). Grid dots halve their density until they are at
least six pixels apart. Text frames are laid out with PangoCairo from the converted Markdown
at the frame's inner width and clipped to the frame; "fit the frame to the text" measures
the same layout.

### The compositor

Cairo paints in the encoding its sources arrive in, and blends there: half of white over
black comes out as code 128, which is a quarter of the light. Feathered cutouts,
translucent frames and drop shadows are all blends, so the painter composites the whole
canvas itself, in linear light with premultiplied alpha, in 32-bit floats, and hands cairo
one finished image.

The device box being painted (the context's clip, narrowed to the area asked for) is the
float canvas; a page at print resolution is cut into bands of at most 24 million pixels so
the floats fit in memory, and a layer keeps a shadow's reach past its band so a blur at the
band's edge is whole. The background, the grid and the pages go into a base layer with
cairo and are decoded into the canvas. Then every object, back to front: cairo paints it
into a layer of its own, sized to its device box (its shadow's reach included); its cutout
multiplies the layer's alpha (`CAIRO_OPERATOR_DEST_IN` with the mask as the source, under
the frame's transform); the layer is decoded -- unpremultiplied, through the sRGB curve,
premultiplied again, scaled by the object's opacity; its shadow is the layer's alpha, blurred
by three box blurs of the shadow's sigma and offset, tinted, laid "over" the canvas first;
then the layer goes over. The finished canvas is encoded back to 8 bits through a 16384-step
table dense enough that every code round-trips to itself: an opaque pixel comes back as the
code it held, which `test_canvas_cutout` pins, along with the 188 that half of white over
black must give.

The conversion loops are OpenMP-parallel and the float canvas lives in a scratch buffer the
surface cache keeps between frames, so a repaint does not page in a fresh allocation.
On screen the layers hold display-encoded colours (each render is colour-managed as it is
decoded), so the sRGB curve stands in for the display's own transfer function: exact on an
sRGB display, and on any other it only shapes the blending at feathered and translucent
pixels, since opaque ones decode and re-encode to the code they held. The export keeps sRGB
throughout and is converted to the output profile afterwards, as before.

### Shadows

A `dt_canvas_shadow_t` is a colour whose alpha is the strength (0 is no shadow), an offset
and a blur, in canvas units. The canvas carries a default one, set from the toolbar's
Shadow popover, and an object overrides it with its own under
`DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE`, from its bar -- the same shape as the borders, and
`dt_canvas_object_effective_shadow()` resolves it the same way. Connectors get shadows too.
The shadow is derived from the object's alpha after its cutout and opacity, so a feathered
frame casts a feathered shadow and a translucent one a fainter one.

### Cutouts

A frame can be cut out of its rectangle by a drawn-mask shape: a circle, an ellipse, a
polygon or a gradient, with a feather past the edge and an invert flag, in `object->mask`.
The shapes, their fall-off and their parameters are the darkroom's own: `src/develop/masks`
rasterises them through `dt_masks_cutout_rasterise()` (`develop/masks_cutout.h`), a headless
entry that describes a shape in the frame's unit square -- (0, 0) the top-left corner,
(1, 1) the bottom-right, radii and feather as fractions of the shorter side -- builds the
form the darkroom would build and rasterises it against a throwaway dev whose only geometry
is the raster's size. The masks module is being enclosed and the canvas never reaches into
a `dt_masks_form_t`; the entry is inside the module, so the ratchet stays where it is.

The polygon's nodes are variable-length and follow the object's record as a tagged chunk
(`CANVAS_CHUNK_MASK_NODES`), eight floats per node: position, two control points, a smooth
flag. The fixed fields (shape, flags, feather, centre, radii, rotation) took reserved bytes.
The raster is cached in the surface cache by the mask's hash and size
(`dt_canvas_surface_cache_get_mask()`), as an 8-bit alpha surface at the frame's size on
screen, capped at 3072 pixels a side.

The view edits a cutout with handles over the frame when the bar's Edit toggle is on: the
centre or anchor, the radius or radii (the ellipse's first radius handle also sets its
rotation), the gradient's reach across its line, and the polygon's nodes; Ctrl+click on an
edge inserts a node, Shift+click on a node removes it. Every drag is one undo record.

### Gutter frames

`DT_CANVAS_GUTTER_VISIBLE` draws a frame one gutter out around every frame, in the canvas's
gutter colour, over everything -- a guide, so it is drawn with the grid and not exported.
Two neighbours one gutter apart share it: the gutter is what the snapping keeps clear, not a
margin each frame owns, so the frames overlap where the frames meet.

### Connectors

A connector joins two frames at **anchors**: each frame's own cardinal points (top, right,
bottom, left), which rotate with it, or `AUTO`, which picks of the four the one nearest the
other end's frame. `dt_canvas_connector_route()` resolves a connector to its geometry once,
for the painter and the hit test alike: the two anchor points, the outward normal at each,
and a polyline. Three **routings**: straight (one segment); square (a stub along each
normal, then horizontal and vertical legs, with a middle leg when both normals point the
same way); cubic (a Bezier whose control points lie along the normals, drawn with
`cairo_curve_to()` and flattened to 40 points for the hit test). Arrow heads sit on the
anchor and point along the route's last leg -- the chord itself for a straight connector --
at the end, the start, both, or neither (a flat line); "Reverse the direction" swaps the
ends and their anchors. A new connector is a cubic spline, and a selected cubic connector
shows its **tangent handles**: one at each end, held on the anchor's normal (orthogonal to
the frame's edge) so only its length is dragged, and two about the waypoint, whose direction
and length are free; a handle left alone stays automatic. The line stops short of an arrow's
tip: a disc of the head's length around the arrowed end is cut out of the stroke, so the tip
is the triangle's alone and stays sharp. All of it is in the connector's floating bar, with
dashes, colour and width.

Connectors are drawn from the toolbar's **Connect** button (or C): in that mode the frame
under the pointer shows its four cardinal anchor dots, the first click picks the source
anchor, the second the target anchor -- the user chooses the anchors, nothing is resolved
automatically -- and the mode ends with the connector selected. Escape or a right click
leaves it.

Selection handles, hover outlines, the rubber band, the connector being drawn, the status
line and the navigation flower are the view's and are painted after the document. The status
line is inked dark or light against the plane's luminance, with a halo of the opposite, so
it reads on any background colour or paper and over a picture.

### The floating property bar

Selecting exactly one object floats an opaque bar immediately below it (above, when there is
no room below) with that object's properties: a text frame's font family and size, text
colour and background; an image frame's border width and colour and a "Canvas default"
button that drops its override; a connector's route, arrow heads, direction, width, dashes,
colour and waypoint; text and image bars carry the border width, colour with opacity, and a
button back to the canvas's uniform border. One bar per kind, overlay children of the centre,
positioned through the overlay's `get-child-position` signal from a stored position, so a
move is one allocation pass of the overlay rather than a margin change, which is a resize
that climbs to the toplevel and lays the whole window out again -- what made panning and
clicking sluggish. They are hidden for the length of a drag, placed at its end, and a click
on the background dismisses them at once. Placement is never done from the draw path --
moving an overlay child from inside a draw glitches -- but from
an idle scheduled by every event that moves the object or the viewport; the bar is refilled
only when the selection or the document changed (a signature of both), with its handlers
blocked during a refill so a refill never writes back. The canvas-level defaults (grid,
gutter, border) stay in the toolbar.

### Borders, the gutter, and snapping to neighbours

A frame's width and height are its outer size, border included: the border is stroked
inside the edge and the picture (or the text) is inset by it, so widening a border shrinks
the picture and never grows the frame, and the anchors, which sit on the frame's edge, stay
on the outer border.

The canvas carries a **gutter**, the margin frames keep from each other, and a **snapping
mode** chosen in the toolbar: any combination of three rules, applied in this order to a
move and to a resize, each later rule that triggers replacing the earlier answer. The grid
rounds positions and sizes to the grid step. The gutter lands an edge next to a neighbour
one gutter away or in line with a neighbour's edge, within eight screen pixels
(`dt_canvas_snap_to_neighbours()`; on a resize only the dragged edges may snap). Same size
gives a resized frame a neighbour's width or height within reach (`dt_canvas_snap_size()`),
or the combined width or height of a run of neighbours stacked one gutter apart (masonry
style); while it snaps, the frame(s) the size was taken from are outlined and a guide line
runs along the matched dimension on both.
An image frame resizes proportionally and follows its width; a text frame resizes freely.
The layouts space frames by the gutter too.

### The plane: background, grid, paper

The canvas is painted with its background colour or with one of four procedural papers:
**Moleskine** (cream, fine soft clouds and short fibres in every direction), **watercolour**
(white, a tooth that only darkens so the paper is white at its peaks, rounded pores),
**embossed** (a wove sheet dried on a metallic mesh: a mottle and fibres, and the mesh's
grooves stamped over the blended field in absolute coordinates, so it stays one mesh across
placements; the weft threads closer and deeper than the warp, and the sheet's own relief
bending the threads and varying their pressure) and **Japanese** (washi: large soft clouds and long wrinkles, the zero crossings
of a low-frequency field lit as ridges, the same lines at every zoom). Both are random fields synthesised in the
frequency domain: white noise shaped by a radial filter (a plateau below a knee frequency, a
power-law fall-off above it) and transformed back with a small radix-2 FFT of our own. The
discrete transform is periodic by construction, so a sprite wraps without a seam. One sprite
repeated shows its period, and sprites sharing a border repeat that border, so six sprites
per paper are laid on a half-overlapping grid, each placement a random sprite in one of eight
orientations at a random phase, its centre jittered off the cell's, blended by
two-dimensional Hann windows whose summed weights are divided out, into a field six sprites
(3072 units) wide that is itself periodic: no seam, no border band, and no lattice of window
centres either. The Moleskine carries short fibres in random directions over its clouds:
segments stamped at random positions, angles and lengths, defined in the sprite's units so a
fibre is the same fibre at every zoom and only sharper. The watercolour carries a band-passed
layer of rounded pores, and its tooth saturates at four and a half percent, so a deep hollow
is a shallow shadow, not a pit. The composed field is kept at up to 512 pixels per sprite;
zoomed past that, the painter scales each cell up rather than growing a tile with the square
of the zoom. Every spectral coefficient is drawn
from a hash of its frequency, so a sprite synthesised at a higher resolution keeps the same
broad features and only adds finer ones: the grain sharpens as the zoom grows (256 to 1024
pixels per sprite) instead of the same texture being enlarged. Sprites are kept per
resolution, and the scaled, colour-managed copies per target; the plane is filled cell by
cell in device space at integer offsets, cairo's fastest blit, then finished with a gentle
achromatic multiplicative dither, one device pixel wide at every zoom, whose deviation grows
with the square root of the zoom: a repeated noise tile blended with the multiply operator,
anchored to the canvas origin so it does not shimmer under a pan.
The grid dots have a colour of their own and a radius that is a fraction of the grid step,
so they scale with the zoom too, floored at three quarters of a pixel so they never vanish.

A canvas may be divided into **pages** of an ISO A paper (A2 to A6, portrait or landscape),
tiled from the origin and outlined with dashed lines in their own colour: one line per
border, never one rectangle per page -- a shared edge stroked twice with two dash phases
fills its own gaps and reads as solid -- each line starting on a multiple of the dash
period from the origin, so the dashes neither crawl under a pan nor differ between the
horizontal and the vertical. Page borders are a snapping rule of their own, applied after
the gutter and before the size. One canvas unit is
one point, so an A4 page is 595 by 842 units. The PDF export then writes one PDF page per
canvas page that holds a frame, skipping empty ones, at the requested resolution; without
paper it fits the frames' box on the page the dialog chooses, as before.

### Text frames

A text frame has a font, a text colour, a background that can be transparent, and a
horizontal (left, centred, right, justified) and vertical (top, middle, bottom) alignment,
all in its floating bar.

### Map frames

A map frame holds a latitude, a longitude, a slippy zoom level and a tile provider, and its
render travels in the archive as a JPEG like an image's (`maps/<id>.jpg`), so the canvas
opens with its maps and needs the network only to fetch them again. Fetching is a
background job (`dt_canvas_render_map_start()`): the tiles covering twice the frame's size
on the canvas are pulled over HTTP with libcurl into a disk cache under the user's cache
directory, composed in Web Mercator, and delivered through the same callback as an image
render. Providers are the map view's when it is built (`osm-gps-map`'s valid sources and
their URI templates), OpenStreetMap otherwise; the provider's attribution is painted along
the frame's bottom edge, as its terms ask. The toolbar's Map button adds one at the centre
of the view at the last place used; an image's context menu adds one of where it was taken,
from its geotag; the frame's floating bar edits the place, the zoom and the provider, each
change fetching the tiles again. A map keeps its own ratio: it covers the frame, centred,
and is cropped by it rather than stretched, and a resized frame fetches again at its new
size so the crop it shows is at full detail.

### Waypoints

A connector may pass by one point, to go around other frames: the bar's "Waypoint" toggle
adds it at the middle of the current route, so nothing moves until it is dragged; it is drawn
as a diamond on the selected connector. Straight routes bend at it, square routes reach it
with one elbow and leave it with another, cubic routes become two curves sharing a tangent
there. It took 20 more of the connector record's reserved bytes, again without a format bump.

### The navigation flower

Bottom right of the view, painted in screen space and hit-tested before anything on the
plane: four petals pan by a quarter of the view, the inner disc zooms in (upper half) and
out (lower half) about the view centre, the core fits the view to the canvas. It hovers
and is never printed.

### Colour management

Frames are sRGB JPEGs. On screen they go through the module's prepared sRGB-to-display
transform, and so does every colour the canvas draws -- borders, text, backgrounds, grid
dots -- through `dt_canvas_render_color()`, one pixel through the same transform, so a border
matches its picture. The PDF export rasterises the page in sRGB and converts the whole raster
to the chosen output profile with LCMS, embedding that profile; the intent is the user's.
Text and connectors are therefore pixels in the PDF, not vectors: a trade for having one
painter and one colour path for the screen and the print. The compositor above runs on both
targets; only the layers' encoding differs.

## The view

The toolbar (`libs/tools/canvas_toolbar.c`) is three menus and a row of controls: **Canvas**
(new, open, save, save as, export as PDF), **Object** (check against the library, refresh
the stale images and notes, refresh every image), **Guides** (a popover: the grid's show,
snap, size and colour; the page borders' show, snap, size, orientation and colour; the
gutters' snap and size, and snapping sizes to neighbours), then Text, Notes, the Connect
toggle, the background, the default border, Fit and 1:1, the layout chooser.

`src/views/canvas.c` owns one document and everything about editing it. It registers the
`canvas` accelerator group, exposes its actions through `proxy.canvas` for the toolbar
(`libs/tools/canvas_toolbar.c`), and raises `DT_SIGNAL_CANVAS_CHANGED` whenever the
document is replaced, saved or reconfigured, so the toolbar refills its grid and border
controls from the document -- with its handlers blocked, so a refill never writes back.

The cursor names the action under the pointer: a hand over a frame or a connector, a corner
cursor over a scale handle (turned with the frame), the exchange cursor over the rotation
handle, a crosshair over an anchor in connect mode, a hand over the flower, a cross-arrows
cursor over a waypoint and while moving.

Gestures: drag a frame to move it (the whole selection follows; snapping puts it next to a
neighbour one gutter away, in line with a neighbour, or on the grid), drag a corner handle to scale it around the
opposite corner keeping its aspect ratio, drag the handle above it to rotate (Shift snaps to
15°), drag on empty space for a rubber band, middle button or Alt-drag to pan, wheel to
zoom about the pointer, Shift-wheel to pan sideways. Double-click opens a text frame's
editor or an image in the darkroom. Right-click opens the context menu for what is under
the pointer; "Connect to..." arms a connector whose end is the next frame clicked.
Every edit is one undo record (`DT_UNDO_CANVAS`), a snapshot of the document before and
after: objects are small and JPEG bytes are shared by reference, so a snapshot costs the
records, not the pixels. A drag records its undo on release, and Escape mid-drag restores
the pre-press snapshot.

Drops from the filmstrip arrive on the centre widget as the `image-id` target (the same
payload the map view reads), with `GDK_ACTION_MOVE` -- the only action the filmstrip offers,
so the destination must accept it or GTK refuses every drop without a word: each id becomes
an image frame at the drop point, staggered so a multi-drop is not one pile, and a render is
started for each.

The **Notes** toolbar button (or Shift+T) adds, under each selected image frame -- every
image frame when none is selected -- a text frame linked to it, showing the `.txt` note the
library keeps next to the raw; images without a note are skipped, and an image whose note is
already on the canvas is not duplicated. "Refresh" reloads the linked notes.

The document outlives a view switch. A dirty untitled canvas is written to
`<configdir>/canvas-recovery.anselcanvas` at exit and read back, still dirty, at the next
start. New and Open ask before discarding unsaved changes.

## Registration

What a new view needs, as this one did it: the `DT_VIEW_CANVAS` bit and the `proxy.canvas`
block in `views/view.h`; the module in `views/CMakeLists.txt`; `MACRO_VIEW(canvas)` and its
two branches in `gui/actions/views.c`; the undo dispatch in `gui/actions/edit.c`;
`DT_UNDO_CANVAS` in `common/undo.h`; the `canvas_accels` group in `widgets/accelerators.h/.c`
and `gui/application.h`, plus the focus-accel branch in `libs/lib.c`; the signal in
`control/signal.h/.c`; the conf keys in `data/anselconfig.xml.in`; `po/POTFILES.in`; the
layer in `tools/include_graph.py`.

## What is not there yet

- Text and connectors are rasterised in the PDF. A vector export would need a second
  painter or a cairo PDF surface with its own colour path.
- Sidecar text frames are refreshed on "Refresh", not watched.
- The image render is one size per canvas (`image_long_edge`), chosen when the canvas is
  created; changing it takes a "Refresh all".
- No multi-page PDF: a book is one canvas per spread today.
