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
| `canvas_export.h/.c` | the pages, colour-managed, as PDF, PNG, JPEG or TIFF |
| `canvas_actions.h` | the action vocabulary shared by the view and its toolbar |
| `canvas_props.h/.c` | every per-object property described once, its reader and its writer, the double-click rule |
| `canvas_handles.h/.c` | every place an object is taken hold of, as one list of sites the hit tests walk |
| `canvas_place.h/.c` | where an object's floating properties go: a search over rectangles |
| `canvas_place_shapes.h/.c` | an object's handle sites and body as the rectangles that search keeps clear of |

Six kinds of object share one struct, `dt_canvas_object_t`, and the stored kind is an index,
so a kind is APPENDED and never inserted:

| kind | what it is |
| --- | --- |
| `IMAGE` | a library render, with the identity that finds the original again |
| `TEXT` | Markdown, or the `.txt` sidecar of an image frame |
| `CONNECTOR` | a route between two frames, or -- with both ends free -- a line or a curve of its own (see below) |
| `MAP` | a slippy map around a point (see below) |
| `SVG` | a drawing read from a file, carried in the archive (see below) |
| `SHAPE` | a drawn rectangle, polygon or star: an outline the atelier fills, strokes or both (see below) |

Every one of them but the connector is a **frame**: it has a centre, a size, a rotation, a
draw order, a fill, an optional border, corners, a shadow and a cutout of its own -- which is
why a shape needs no fields beyond the geometry and the three numbers its outline is made of,
and why `dt_canvas_object_is_frame()` is the question nearly everything asks rather than the
kind itself. The canvas carries the default border, the grid, the background and the saved
viewport, and a frame inherits them until it takes its own. Every object has an id unique
within the canvas, never reused, which is what connectors and sidecar links refer to; **the id
0 is never an object's**, and that is what a connector's free end holds.

Coordinates are canvas units, and one unit is one POINT -- see "Pages, spreads and the unit"
below. At zoom 1 it is also one screen pixel, which is what makes a point-measured plane
legible on a screen. The origin is the centre of the plane, y grows downwards. The view
converts through `_to_canvas()` and nothing else, so the document never learns what a pixel
is.

The four layouts leave two paddings between frames -- each keeps one all round -- and, with
snapping on, start on the grid and round every cell up to whole grid steps, frames sitting
top-left in their cell. A WHOLE-CANVAS arrangement gathers the pictures and what goes with them
and skips shapes: a shape is decoration, placed where it is against something else, so sweeping
it into the grid with the photographs would move it away from the thing it was drawn for. A shape
named in a selection is arranged like any other frame.

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
canvas itself, in linear Adobe RGB (1998) with premultiplied alpha, in 32-bit floats, and
hands cairo one finished image. Adobe RGB is the canvas's own encoding end to end: the
renders leave the pipeline in it, with the profile embedded in each JPEG
(`dt_canvas_image_t.colorspace` records it; a file from before says sRGB and is converted
when decoded, as a map's sRGB tiles are); every colour cairo paints goes through
`dt_canvas_render_color()` into it, and the paper fields through
`dt_canvas_render_srgb8_to_layer8()`. So every layer decodes through the 563/256 gamma alone,
with no matrix, and colour management happens once, at the end: the finished canvas is
encoded back to 8-bit Adobe RGB and, for the screen, converted in place to the display
profile by `dt_colorprofiles_adobergb_bgrx8_to_display()` -- the module's prepared 8-bit
transform, row-parallel, a fraction of what the same conversion cost in floats -- or left as
it is for the PDF exporter, which converts the page to the output profile from an Adobe RGB
source. Wider than sRGB and what a print can use; Rec2020 would buy nothing in eight bits.

The float canvas is sized in the surface's own PIXELS, not cairo's device units: cairo's
device space stops short of the surface's device scale, so on a 2x screen a layer sized
in device units is half the resolution and comes back blurred and aliased (the trap
`doc/overlay-raster.md` recorded first). The painter folds `cairo_surface_get_device_scale()`
into its matrix and undoes it when the encoded image is blitted, pixel for pixel; every layer
also carries the target's font options, so text is hinted and antialiased the way the screen
asks. The device box being painted (the context's clip, narrowed to the area asked for) is the
float canvas; a page at print resolution is cut into bands of at most 24 million pixels so
the floats fit in memory, and a layer keeps a shadow's reach past its band so a blur at the
band's edge is whole. The background, the grid and the pages go into a base layer with
cairo and are decoded into the canvas. Then every object, back to front: cairo paints it
into an 8-bit layer of its own, sized to its device box (its shadow's reach included). A
plain frame -- no cutout, no shadow -- goes straight from that layer over the float canvas,
decoded on the way: unpremultiplied in 8 bits (all the precision the layer ever had), through
the gamma table, premultiplied again, scaled by the object's opacity. A cut or shadowed
frame is decoded into a float layer first; its cutout is composited there in one parallel
pass (`_cut_compose()`: the background over the shape's support, the content through the
feathered shape, the border band over both, each read where the layer's pixel lands in the
mask's raster through the inverse of the frame's transform); its shadow is the layer's
alpha, blurred by three box blurs of the shadow's sigma and offset, tinted, laid "over" the
canvas first; then the layer goes over. The finished canvas is encoded back to 8 bits
through a table indexed by the square root of the value, dense at the dark end where a
gamma curve is steepest, so every code round-trips to itself: an opaque pixel comes back as
the code it held, which `test_canvas_cutout` pins, along with the 186 that half of white
over black must give under that gamma.

Nothing is display-managed before the composite: `dt_canvas_render_decode()` and
`dt_canvas_render_color()` keep the working encoding whatever the target, so what blends is
one space and not one per input.

### Shadows

A `dt_canvas_shadow_t` is a colour whose alpha is the strength, an offset and a signed
radius, in canvas units. The radius is the blur's sigma and its sign says where the shadow
falls: positive drops it outside the object, negative casts it inside along the object's
edges (what the object leaves uncovered, blurred and offset, laid over the object within its
own coverage), and zero is no shadow at all -- there is no on/off toggle, the radius is it.
The inset plane is padded with ones past the layer's box before it is blurred: the world
outside the frame is uncovered, and a zero padding read it as covered and thinned the shadow
wherever a cutout came near its own frame. An outset plane pads with zeros: nothing casts
there. The canvas carries a default one, set from the toolbar's
Shadow popover, and an object overrides it with its own under
`DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE`, from its properties -- the same shape as the borders, and
`dt_canvas_object_effective_shadow()` resolves it the same way. Connectors get shadows too.
The shadow is derived from the object's alpha after its cutout, its border and its opacity,
so it starts at the solid border, a feathered frame casts a feathered shadow and a translucent
one a fainter one.

**The extent grows the silhouette before the blur.** A blur spreads the ink it is given over a
wider area and never adds any, so a thin silhouette under a wide blur has almost nothing left:
measured beside a bar four units wide under a blur of twenty, ten units out, the shadow was code
247 on white -- gone. `dt_canvas_shadow_t.extent` (canvas units, 0 to
`DT_CANVAS_SHADOW_EXTENT_MAX`) grows the plane first -- the object outward for an outset shadow,
the uncovered world inward for an inset one -- so the colour keeps its full strength that much
further and the blur then fades a solid shadow: code 186 at the same spot with an extent of 15, 246
to 194 thirty units inside a frame under an inset blur of twenty with an extent of 25
(`test_canvas_cutout`). Like the radius's sign it switches nothing on by itself for a frame; a
text's glyph shadow grown with no blur and no offset IS drawn, and is the outline that keeps a
caption legible over a picture. The canvas's default carries one (the Shadow popover), an object's
own shadow and a text's glyph shadow each carry theirs, and each record keeps it where its
reserved bytes began, so a document from before reads 0 and is drawn as it was.

The grow is `canvas/canvas_dilate.c`, and three things about it were decided by measurement:

- **A maximum, not a threshold.** Every pixel takes the largest value within reach -- a
  grayscale dilation -- rather than a distance transform of the silhouette cut at half coverage,
  which is what the cutout's border band does and is right there, because that band is solid by
  definition. A shadow's silhouette is not binary: a feather is a ramp, and a threshold hardens
  it the moment the extent leaves zero, a jump from the slider's first step; a glyph's hairline
  is a pixel of partial coverage, and a threshold drops it. The maximum moves every level out
  together: a feather stays a feather, further out, and a faint stroke spreads at its own density.
- **A sum of segments, not a disc.** A maximum over a true disc costs a read per ROW of the disc
  per pixel, where the three box blurs after it cost nothing that grows with the radius. The grow
  is a sum of segments along eight lattice directions -- the axes, the diagonals, the knight's
  moves -- each a running maximum at a constant cost per pixel (van Herk / Gil-Werman), with the
  lengths chosen per radius to match the disc's reach in every direction: within 0.76 px up to a
  radius of 20 px, a pixel up to 40, 2.4 % at worst beyond and 1.4 % past 640 px. The axes and
  diagonals alone make an octagon, 4 % off; the square a separable filter makes is 41 % off, the
  error the occupancy grid's rule already forbids.
- **A margin instead of edge logic.** A window is computed only where it lies inside the plane,
  so the working loop is two reads and a maximum; nearer an edge than `dt_canvas_dilate_margin()`
  a pixel ends somewhere between its own value and the right one. The first version handled the
  edges exactly, per pixel, and cost two to five times the blur; the second measured about the
  blur's own cost at a megapixel. The painter pays for the margin only where it must: an inset
  plane is padded with ones by it (ones stay ones under a maximum), and an outset plane only by
  what it asks beyond the room the layer box already leaves past the grown silhouette -- three
  blurs and the offset -- where the right answer is nothing and so is the approximate one.

- **Tiles where the grow moves nothing.** The eight passes are bound by memory, and over a
  shadow's plane most of that memory is spent where the grow changes nothing: a frame's inside
  stays solid, the room around it stays empty. A plane whose margin is at most 48 px is cut into
  tiles at least four margins wide; a tile whose neighbourhood holds one value is left alone and
  every other is grown in buffers of its own that stay in the cache, exact everywhere, edges
  included. When the tiles that move would cost more than the plane -- a text's glyphs, a lattice
  of small frames -- it is swept whole. The first tiling, with tiles two margins wide and no cap,
  lost wherever the margin was large: at an extent of 60 points it cost 345 ms where the sweep
  cost 198. The brute force the test holds it to had to be padded too: a sum of segments reaches
  some of its points only through points outside the plane, and the passes run on the plane
  itself cut those paths, so the reference read short along the edges where the tiles were right.

What it costs, measured on five shadowed objects filling a 2560 x 1440 view: shadows cost 2.8 ms
at half zoom, 11 at 1:1 and 37 at 2:1 with no extent; 7, 27 and 81 with an extent of 10 points;
13, 53 and 184 with 60. The last column is the one still open: a large extent's margin is too
wide to tile. A shadow wide enough to want a large extent is smooth at the scale of its blur, so
the remedy there is to compute it at a fraction of the resolution, not a cheaper grow.

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
(`CANVAS_CHUNK_MASK_NODES`), ten floats per node: position, two control points, a smooth
flag, the fall-off's own radius either side of the node, and a spare. **The chunk's size
divided by the node count is the stride it was written with**, so the record may gain a field
without the format moving and without an older document losing a node: fewer floats than this
version keeps read as zero, more are stepped over. The fixed fields (shape, flags, feather, centre, radii, rotation) took reserved bytes.
The raster is cached in the surface cache by the mask's hash, size and inset
(`dt_canvas_surface_cache_get_mask()`), as an 8-bit alpha surface at the frame's size on
screen, capped at 3072 pixels a side. **One rule sizes every frame: the frame is the
object's outer size, border included.** A rectangular frame's border sits inside its edge
with the content inset; a cut frame's shape is confined to the frame less the border's width
on every side (`dt_canvas_mask_geometry_t.inset`, applied in `_mask_raster_fine()`), so the
shape stops where the border must begin and the border, dilated from it, ends exactly at the
frame's edge -- a gradient cutout, which covers the whole frame, gets the same border as an
uncut frame. The band is dilated from the shape AS DESCRIBED and then stopped at the frame,
never from the confined shape: a disc dilation of the confined rectangle would round its
corners, and a shape that fills its frame must get the frame's own corners. Only a shadow
reaches past the frame, and the object's device box grows for it alone.

Every frame has **rounded corners** as a parameter: `dt_canvas_t.corner_radius` is the
default, an object overrides it under `DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE`, and
`dt_canvas_object_effective_corner_radius()` clamps it to half the shorter side. A
rectangular frame's outline, border, background and content clip all follow `_frame_path()`,
the radius shrinking with the inset; a cut frame's room and its band's stop are the same
rounded rectangles, in the raster (`_mask_clip_rounded()`). Every mask surface -- the feathered shape, its support, its border band -- is
rasterised at three times the resolution (two past a megapixel) and box-filtered down, which
is the anti-aliasing of their edges; the band's distance transform runs at the fine
resolution, so its two edges are anti-aliased too.

The view edits a cutout with handles over the frame while Edit is on in the Cutout section of its
properties: the centre or anchor, the radius or radii (the ellipse's first radius handle also
sets its rotation), the feather on the circle's or the ellipse's dashed ring, the gradient's
reach across its line, and the polygon's nodes.

**A polygon node owns two things the shape does not**, both the darkroom's own, and both
reached from the node the pointer is working near -- one node's at a time, since every node's
at once buries the shape under its handles. Its **fall-off** hangs on a dashed tether along
the node's outward normal, the perpendicular to the line through its neighbours turned away
from the average of the nodes; how far it is pulled is the radius, written to both of the
darkroom's per-node borders. A node with none takes the shape's, so a polygon reads as it
always did until one node's feather is pulled out. Its two **control points** are round and
tethered either side, drawn where the curve actually goes -- the stored points, or the
Catmull-Rom tangent through an automatic node.

**A node has THREE kinds, not two** (`dt_canvas_mask_node_kind_t`): a **cusp**, whose two
control points are its own and free of each other; an **automatic** node, smooth, whose
tangent is computed from its neighbours; and a **steered** node, smooth, whose tangent is the
one the user dragged. Steering a handle on a cusp moves that handle alone. Steering one on a
smooth node keeps it smooth: the opposite control turns with it, through the node, and keeps
the length it had, so **one handle sets the direction the curve leaves in and the tension it
leaves with**. Without that third kind the first touch of a handle turned every node into a
cusp and a smooth node could never be given a tangent of its own. A control sitting on its
node has no length to keep and takes the dragged one's, so a tangent pulled out of a node
that never had one comes out symmetric instead of collapsed on one side.

The distinction is not a flag at the far end: `dt_masks_node_is_cusp()` decides it by
geometry, two control points that coincide, so the cutout entry hands a cusp and a steered
node down the same way and only an automatic one asks the shape for a tangent. Reading that
field as "non-zero means computed" threw away every steered tangent -- the outline moved on
screen and the cut did not follow -- which `test_canvas_cutout` now pins. Nothing ever wrote
the third value before it existed, so every document written before this reads as it did.

Four shapes for four jobs: a square is a cusp node, a circle a smooth one, a circle on a
short tether its curve, the dashed tether's end its fall-off. Over the frame, the wheel sets the feather, with
Shift the opacity, with Ctrl the gradient's curvature or the ellipse's rotation. On a
polygon, Ctrl+click on an edge inserts a node, Shift+click on a node removes it and a double
click switches it between cusp and smooth -- the same one call the context menu's entry uses,
so the two cannot drift. The shape, the feather, the inversion and the edit mode are the
properties' (Cutout section); the context menu offers only what a pointer names, the node or
edge under it: while the shape is edited, switch the node's kind, give a steered one its
computed curve back, remove it, or add one on the edge; while it is not, the way into the edit
mode for the node under the pointer.

**The menu asks its own question of the geometry, not the drag's.** `_mask_handle_at()`
answers what a drag would grab and refuses everything while the shape is not being edited,
which is right for a drag and wrong for a menu: for as long as the cutout submenu keyed its
node entries on it, they were a duplicate of the top-level ones whenever the shape was being
edited and unreachable the rest of the time. `_mask_node_at()` is the geometric question, and
it is what lets the menu offer the way into the edit mode on a node it does not draw. Every drag and
every wheel step is one undo record.

A text frame carries **four inner margins** rather than one, top, right, bottom, left. All
four zero takes the uniform `padding` on every side -- what a document from before them holds
-- and any one of them set makes all four literal, so a side really can be zero; the canvas's
texture weights follow the same rule for the same reason (`dt_canvas_text_margins()`).

**Optical margins** hang punctuation outside the measured edge, so a column reads from its
STEMS rather than from a quote or a full stop. The layout is therefore drawn line by line,
each nudged by a fraction of the hanging character's own advance -- a quote is nearly all
white space and hangs almost whole, a full stop hangs a little. Which edge may hang is the one
the alignment pins: the left for ragged-right and justified text, the right for ragged-left.
`_show_layout()` is `pango_cairo_show_layout()` spelled out when the flag is off, deliberately
so, since one path cannot drift from itself.

**Auto height** makes a frame take the height its text needs, and it is applied when the frame
is EDITED, never while it paints. Measuring in the view's expose looks cheaper -- the height
depends on the laid-out text and the text depends on everything that can change it, so letting
the painter settle it seems to catch every case for free -- and for a frame that also flows
around what is laid over it, it closes a loop. An object is anchored at its centre, so writing
a new height lifts the top edge by half the growth; the first lines then have a different set
of obstacles above them, the paragraph re-wraps, and it asks for a different height again.
Measured on a column set beside a cut picture: 616 -> 1191 -> 681 -> 1191, a two-cycle the
frame flipped between on every repaint, and a zoom, a pan or a hover is one repaint each. It
was reported as the text jumping, "re-rendered with different size, wrapping and line width",
and in the tall state the frame's top sits 288 units higher, so the opening lines clear the
picture and run straight across it -- the same defect wearing its other face.

`dt_canvas_paint_text_fit_height()` is therefore the whole of it. It measures on a scratch
context of its own, because a height is a property of the DOCUMENT and must not depend on
which viewport, zoom or screen asked; it grows the frame DOWNWARD, so the edge the user placed
stays where they put it; and it iterates to a fixed point rather than taking one step per
paint, which downward growth makes monotone -- every existing line keeps the obstacles it had
and the height only opens room below. The same run settles in two: 616 -> 1191 -> 1045.

Every path that changes what the text or its box is owes that call: every edit in the properties
(the property table's writer refits the frame it edits and reports `DT_CANVAS_EFFECT_SETTLE_ALL`
when it moved what other frames flow around), the text editor -- whose "fit height" button was
this measurement open-coded, growing about the centre -- the sidecar note frames, and the end of any gesture
that moved geometry, since a frame dragged over a column changes that column's flow as surely
as editing the column does. Measured after: the same paragraph breaks at the same fourteen
byte offsets at zoom 0.42, 0.55 and 1.1, at full and interactive quality alike.

## Drawings

It is reached three ways, all through one action (`DT_CANVAS_ACTION_ADD_SVG`): the toolbar's
drawing button and the `d` key place one at the centre of the view, and the plane's context
menu places one where the menu was opened. The action is APPENDED to the enum rather than
slotted in beside the other "add" actions, because those values are what the toolbar's buttons
and the shortcut table carry and inserting one renumbers every action after it.

An SVG is read from disk, sized to what the file itself says it is, and carried in the archive
like a photograph's JPEG -- so a document holds the drawing and opens on a machine that has
never seen the file. The path it came from travels beside it (`dt_canvas_svg_path()`), and
`dt_canvas_svg_reload()` reads it again for when the drawing has been edited since; the frame
stays where the user put it, because where a drawing sits and how big it is on the page are
theirs and not the file's.

`rsvg_handle_set_dpi(72)` makes one SVG user unit one point, which is one canvas unit, so a
file stating two inches by one arrives as a frame of 144 by 72 points with no scale factor
anywhere between the file and the paper. A file stating only a viewBox has no physical size to
honour and takes its viewBox, which is the convention every browser applies.

**It is rasterised ATOMICALLY.** The specification composites an SVG in sRGB with the transfer
function applied -- its overlaps, its gradients and its anti-aliased edges are all defined
there -- and this canvas composites in linear Adobe RGB. Those are different pictures, so
`rsvg_handle_render_document()` draws the whole document in one pass, exactly as its author saw
it, and only the finished image is converted. Rendering its pieces into our space one at a time
would be a different drawing. The conversion divides cairo's PREMULTIPLIED alpha back out
before the transfer function and folds it in after; measured, that buys a single code (110
against 109) because sRGB's curve and Adobe RGB's are both near a gamma of 2.2 and the alpha
then factors straight out of `encode(k * eotf(a * c)) = a * encode(k * eotf(c))` -- it is kept
because that cancellation is a property of these two spaces, not of the code, and the day
either one is not that gamma it is the only version that stays right.

A drawing is given **no border and no shadow**: it is ink on nothing -- a logo, a diagram, an
arrow -- and a card behind it with a rule around it turns it into a rectangle, which is the one
thing it is not. Both are the user's to switch on afterwards.

**A drawing is drawn at the size it is shown at, never rescaled to it.** An SVG has no
resolution of its own -- that is the whole point of one -- so the sprite cache renders the
document at whatever pixel size it is about to be blitted at rather than resampling a raster of
it. Rasterising once and stretching throws away the only thing a drawing had over a photograph:
measured on a 32-point file shown at 512 pixels, the edge of a square is a full step of 255
drawn at size and under 60 stretched from its own. There is no oversample factor to guess at
either, only a ceiling (`CANVAS_SVG_MAX_EDGE`) so a drawing across a wall-sized page cannot ask
for a raster nobody has the memory for.

Two things the sprite path demands of that, both of which a photograph is forgiving about and a
drawing is not. **The surface returned is EXACTLY the size asked for**: the painter blits one
pixel to one at a corner it worked out itself, so a surface of any other size lands small in the
corner of where it belongs -- which is what an internal ceiling on the raster did as the zoom
crossed it, reported as a drawing that vanishes or jumps at some zoom levels. The ceiling is
still there, since a drawing across a wall-sized page must not ask for a raster nobody has the
memory for; it applies to what is RENDERED, and the result is brought back to the size that was
asked for. And **the drawing is drawn at the BOX's size and centred in whatever is left over**,
never stretched over the sprite and never shrunk to make room in it: the painter asks for a
sprite larger than the box so that the clip and not the sprite's edge ends the picture, a
photograph stretched over that loses a sliver nobody sees, and a drawing stretched over it has
its last row or two of ink pushed outside the clip -- the missing rows at the bottom of a
drawing.

The guard is headroom, and it is there because a drawing is FITTED to its frame. A frame whose
proportions differ from the document's is filled along one axis and letterboxed along the other,
so a frame proportionally taller than its drawing has the ink running edge to edge down it --
measured on a real document, a 2341 x 1600 frame around a 340.3 x 243.2 diagram fills the height
exactly and leaves 51 px of air each side. An author who drew to the edge of the page, which is
most of them, then has the last line of type sitting exactly on the frame's boundary, and this
one does: the diagram's ink touches all four sides of its own viewBox, with about a quarter of a
unit of anti-aliasing beyond it that librsvg clips at the viewport whatever we ask for, since
that is what an SVG's own `overflow` means. Flush against the frame it reads as shaved off, and
was reported as text clipped on a drawing.

The guard is separately what a sprite blitted at a WHOLE pixel owes a box that sits at a
fractional one -- the caller's clip lies up to a pixel inside the sprite's own edge -- but that
is not what was taking the ink, and the measurement says so: sweeping sixteen sub-pixel pan
alignments, the bottom line of type keeps its ink to within **0.07%** both with the guard and
without it, and with one surface cache across the sweep (what the atelier does) exactly as with a
fresh one per frame. An earlier reading of 2.1% did not survive a clean A/B against a verified
binary and is not evidence of anything. So this is headroom, not a repair. The alternative,
placing the drawing at the box's exact sub-pixel position, would need a render per alignment:
13 ms at this drawing's screen size and 160 ms at four times it, per frame of a pan, against a
sprite cache of a few slots.

**The guard is the CALLER's air, added around the sprite and paid back at the blit -- it may not
come out of the drawing.** The painter asks for a sprite `DT_CANVAS_SVG_GUARD` larger on every
side and blits it that much further out; the renderer draws the document at the box's own size
and centres it in what is left. Taken out of the drawing instead, which is what it did when it
was first written, it is a fixed number of SCREEN pixels charged to a box whose size is the
ZOOM's: the same drawing filled **84.0%** of its frame at a quarter zoom and **98.7%** at three
times, so it breathed against its own border on every wheel click, and subtracting it from both
axes changed the drawn box's proportions as well -- rsvg's default `xMidYMid meet` then
letterboxed the document inside it, a second inset, also the zoom's, that moved the drawing off
its own corner. `_a_drawing_fills_its_frame_at_every_zoom` sweeps zoom x quality x device scale
x sub-pixel phase and asks for the frame's span to within two device pixels; the renderer's half
is `_a_drawing_fills_its_box_and_sits_in_the_callers_air`. The one thing the padding then costs
is the fast path that hands back the intrinsic decode when it happens to be the size asked for:
it carries no padding, so it may not answer a padded request.

**A sprite is blitted at a WHOLE pixel, so a DRAWING is rendered at the fraction.** Blitting one
pixel to one under an identity matrix is what makes the sprite path cheap, and it quantises the
picture's position to the pixel grid. Measured on a drawing panned in eighth-pixel steps, an edge
inside the document stood at the same column for **eight frames** and then jumped a whole one,
hard-edged, while the frame's own border and every glyph beside it slid smoothly by an eighth
each time -- a picture crabbing against the page it is on, and the "content transiently shifts"
half of the glitch report. The frame's clip hides it at the frame's own edges, since the clip was
always at its true sub-pixel position, which is why it has to be looked for inside the drawing.

So `dt_canvas_render_phase_snap()` splits the device corner into the whole pixel the sprite is
blitted at and the fraction the document is rendered at, and rsvg draws the drawing at that
fraction with real anti-aliasing -- a shifted blit would have to resample, which is the cost the
sprite path exists to avoid. The fraction is snapped to `DT_CANVAS_SVG_PHASE_STEPS` (two per
axis) because **a sprite is now keyed on its phase as well as its size**: four sprites per size,
at most a quarter of a pixel of residue instead of a whole one, and `CANVAS_SPRITE_SLOTS` raised
from 2 to 8 in the same change so a diagonal pan does not become one render per frame. Measured
over a 32-frame pan with one cache, with the phase and without it: 106 ms against 108 ms.

**Those eight are a drawing's; a picture keeps two** (`_sprite_slots()`). A picture has no phase,
so the extra six could only hold extra SIZES, at 14 MB apiece for a full-screen one -- and
`_cache_evict_to_budget()` sheds whole entries and never a cold slot of the object it is being
asked to paint, so the budget cannot take them back. `_a_picture_keeps_fewer_sprites_than_a_drawing`
pins both halves, by identity, holding a reference so a freed sprite cannot come back at the same
address and read as a hit.

**Above the renderer's ceiling the drawing is placed in the units that were ASKED for, and the
ceiling is a cairo TRANSFORM.** `CANVAS_SVG_MAX_EDGE` renders a huge sprite smaller and scales it
back, and the box, the padding and the fit inside it are then the same arithmetic capped or not.
Derived a second time in the smaller raster's own integers -- which is how this was first written
-- three truncations do not cancel and the guard comes back anywhere between nothing and two and a
half pixels; and because a capped surface's two dimensions are independently truncated integers,
its aspect is not quite the one asked for, so rsvg's `xMidYMid meet` letterboxes the drawing
inside its own box. Measured on a 25:1 drawing in a 25:1 box at a 5004-pixel sprite, where there
is nothing to letterbox at all: 15 pixels either side, the drawing 4970 wide where its box was
5000. Under a transform the viewport carries the box's own aspect, `meet` has nothing to do, and a
drawing whose frame really is a different shape is still letterboxed by exactly the right amount.
The one residue left is the ceiling's own: the guard survives while it is still worth half a pixel
of the smaller raster, i.e. a sprite up to 16384 px, and past that it is sub-pixel there and the
scale back smears the ink into it.
`_a_drawing_slides_with_the_page_instead_of_crabbing_against_it` reads the edge's sub-pixel
position out of the coverage code and asks that it never be more than 0.3 px from where it
belongs and never move backwards.

**A photograph keeps the whole pixel**, and its phase is 0. Its sprite is a resample rather than
a render, so a fraction there means shifting the resampler's own sampling grid -- one term in the
enlarging branch and a fractional weighted box in the shrinking one, which is the hot parallel
loop. A photograph is also continuous tone, where a drawing's edges are vector-hard: the same
whole-pixel step that reads as a jump in a diagram is invisible in a picture. It remains a real
residue and is written down here rather than fixed.

**A picture and a drawing keep their proportions unless told not to**
(`dt_canvas_object_keeps_ratio()`, `DT_CANVAS_OBJECT_FLAG_FREE_RATIO`). The flag is stated the
FREE way round so that zero is the careful answer: a photograph always kept its shape, a
drawing needs it more -- a stretched logo is almost always a mistake -- and the one time it is
not, the flag says so. One predicate answers for the corner drag and for the properties' Width
and Height alike, so the two cannot disagree; in the properties, whichever of the pair was edited
leads and the other follows.

**Text flows around what the drawing DRAWS.** `dt_canvas_render_svg_coverage()` renders the
document to an A8 coverage surface at the occupancy map's own pitch -- a fraction of a full
render -- and the obstacle map reads that exactly as it reads a cut frame's cutout. Measured on
a file whose ink fills half its viewBox against one that fills all of it: 41.91 units of column
against 97.78, where treating both as their box gives the same number twice.

One thing this cost elsewhere: `dt_canvas_render_rescale()` built every sprite as RGB24 and
forced the top byte opaque. A photograph never notices -- a JPEG has no alpha to lose -- but
every hole in a drawing filled with whatever that byte then meant, which on this canvas is a
black card behind the logo. It keeps the source's format now. Premultiplied values resample by
a plain weighted mean, so the alpha rides along with the colour and nothing needs
un-premultiplying; and carrying four channels rather than three turned out FASTER, 1.35 s
against 1.58 s on the same document, because four floats is a natural SIMD width and a
three-float stride breaks the alignment.

**Which OpenType features are offered is asked of the FONT.** A face carries whatever tags its
designer cut, and no two agree: measured on one machine, FreeSerif answers with 45 of them --
historical ligatures and forms, small capitals, four stylistic sets -- DejaVu Serif with 11,
Liberation Serif with 6, Bitstream Vera with none at all. A fixed list therefore offers a plain
face everything it has not got and hides a rich one's own, which is how a font's historical
ligatures came to be unreachable. `dt_canvas_paint_text_font_features()` (`canvas_paint.c`)
loads the face on a scratch context of its own -- what a face ships is a property of the face,
not of the viewport asking -- takes its `hb_font_t` through `pango_font_get_hb_font()` and
enumerates `hb_ot_layout_table_get_feature_tags()` over GSUB and GPOS, sorted and without
repeats. HarfBuzz costs no build change: pango requires it publicly, so its include and
`-lharfbuzz` are already on the line, and `tools/mingw_syntax_check.py` compiles the file
(verified by a tripwire, since a skipped file reports success just as loudly).

The properties' feature checkboxes are keyed on the four-character TAG, never on a position in a
table. `dt_canvas_text_feature_label()` names a tag where this build has a name for it, names
the numbered families from their number -- `ss04` is "Stylistic set 4", `cv12` "Character
variant 12", since only the font knows what they draw -- and answers NULL otherwise, where the
tag itself is shown. `dt_canvas_text_feature_offered()` keeps the features the layout ENGINE
owns out of the panel altogether: glyph composition, mark placement, cursive joining forms and
the language's own substitutions are what make text shapeable at all, HarfBuzz turns them on
and off as the script requires, and a checkbox overriding that breaks the rendering rather than
styling it. Linux Libertine ships five of them among its 32, so without the filter the panel
would invite exactly that. The list is rebuilt only when the face changes, not its size, so
ticking a box does not destroy the box being ticked.

**The feature string outgrew the record's fixed field, and did it silently.** 64 bytes holds
eight tags, and a document with seven set refused the ninth with no error -- reported as the
checkboxes having stopped working. The whole string travels as a tagged CHUNK beside the record
now, the mechanism the polygon's nodes already used, and the fixed field keeps as many WHOLE
tags as still fit, for a reader that predates the chunk: half a tag is not a feature, and Pango
reads a malformed feature string as nothing at all.

**OpenType features** are stored as the string Pango reads -- `"liga 1, onum 1, smcp 1"` --
which is the only way to reach a font's alternates, figures and ligature sets, since a font
description cannot name them, and are SHOWN as a list of names to tick
(`dt_canvas_text_feature_name()`, `..._features_parse()`, `..._features_compose()`). The
string stays the stored form because that is what the renderer wants and what a file can carry
without a table of its own; the names are the menu's business alone, so the list may be
appended to freely. Measured with `kern 0`, which every font has: a line of AVATAR Ta Wa Yo
goes from 167 to 179 pixels wide. A feature the font does NOT ship is silently nothing, which
is why a no-op here says more about the font than about the code.

**The layout is computed with METRICS HINTING OFF, and glyph grid-fitting with it.** The
layer's context carries the target's font options and its matrix carries the ZOOM, so with
hinting on every advance is rounded to a whole device pixel and the same paragraph is set
differently at every zoom -- justified text, which redistributes the rounding across the line,
is where it shows worst. Measured on a six-line justified paragraph, the last line's right
edge wandered over four pixels between zoom 0.6 and 4, and holds to one -- the downsample's
own noise -- unhinted.

**Text flows around what is laid over it, and hangs at both edges, through ONE capability**:
a line set at a width this code chooses rather than the paragraph's (`_flow_text()`). That is
what both asks need. A line can be laid inside the clear run beside an object standing over
the frame; and a line can be set to a measure slightly wider than its column so its final
comma ends past the edge instead of sitting on it -- shifting a finished line, which is all
the paragraph painter can do, hangs the leading edge only.

The obstacles are the frames drawn ABOVE the text in draw order -- something behind the text
is behind the text -- and what each covers is its SILHOUETTE, so a circular cutout pushes the
text along its curve and leaves the empty corner beside it usable. A CUT frame is asked for
its raster: `dt_canvas_object_silhouette_reach()` casts a ray against the straight polygon
through the nodes, which is near enough for deciding where a connector should stop and short
of the drawn curve wherever the shape bulges. An uncut frame has no curve to miss and still
answers through `dt_canvas_object_covers()`. That raster is taken at ONE PIXEL PER OCCUPANCY
CELL, with a flat number kept only as a ceiling: a flat cap reads as prudence and is coarser
than the grid on any large frame -- 192 px over a 1680-unit frame is 8.75 units to a sample
against a 3-unit cell -- which squares off a curve and lets a line in by most of a step, which
is a shape's rounded edge coming out straight.

What an obstacle covers is wherever it paints ANYTHING, which is more than its silhouette and
more than the half of it. A cut frame's edge is FEATHERED, the cutout fading out rather than
stopping, so the raster is sampled at `TEXT_FLOW_MASK_FAINT` -- 12 of 255, the faintest of a
fade the eye still reads -- and not at half: half is the middle of the fade, and the text
cleared the shape only to sit under the visible half of its own soft rim, which is what "the
text intersects the border of the cutout image" was. Measured on a circle of radius 0.10 with a
fall-off of 0.30, against hard circles of 0.40 and 0.10: the column pays 111.75 units where the
full reach costs 130.38 and the bare shape 74.50, so well over half of the fade is counted and
its faintest tail, which has nothing to see in it, is not.

The border it adds is the EFFECTIVE one, `dt_canvas_object_effective_border()`: a frame without
`DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE` takes the canvas's, not whatever sits in its own
`border_width`, and reading the field gave nought for every such frame -- the text ran clean
under the white edge of one picture while the picture beside it, which had been given a border
of its own, was cleared correctly. The shadow already went through
`dt_canvas_object_effective_shadow()` for the same reason. A test that sets `border_width` on
an obstacle must set the override flag with it, or it is testing the canvas default.

The rest of it: a border band is
dilated outward from the cut edge, and a shadow is the one thing allowed to reach past a frame
at all, so each obstacle is grown by `border_width` plus its visible outset shadow before it is
merged into the map. Text set flush against the silhouette otherwise lands under both -- on a
cut picture over a column the chosen run started exactly on the cutout edge, to 0.0 units, and
the first word of five lines still vanished into a 75-unit white border band. That the layout
agreed with its own map *perfectly* and the picture still overlapped is the tell: a map that
describes the wrong thing cannot be found by checking the layout against it.

They are baked into a coarse occupancy map in the frame's own local coordinates, three units
to a cell, spanning the text area GROWN by the furthest anything can reach into it -- coverage
is only ever sampled AT a cell, so a map stopping at the text area cannot know about a frame
standing just outside it, and such a frame pushed the text not at all however wide a gap was
asked for. Every growth of that map is a DISC, through the Euclidean distance to the covered
cells (Felzenszwalb-Huttenlocher, two passes, one scale per axis since the cells are oblong).
A separable max filter is a square, and a square grows an edge by the reach along the axes and
by `reach * sqrt(2)` along a diagonal: the clear space is then widest exactly where the shape's
edge slants and tightest where it runs straight, which reads as a gutter that will not hold
still along the cut. Measured on a 75-unit border, the square reached 34.2 units past the disc
against the 31.1 predicted for a 45-degree edge. This module learned the same thing once
already for a cut frame's border band; the text flow reintroduced it.

A line asks that map for EVERY clear stretch across the band it is about to occupy, and is set
across all of them, left to right. A picture standing in the middle of a column leaves clear
space on both sides of it and the line carries on past it; taking only the widest stretch --
"the largest area", which is one of the choices a page-layout application offers -- abandons
the far side, and is what "the text is flowing only on one side" reported. Measured on a
600-unit column with a 200-unit picture over it: an obstacle in the CENTRE costs exactly what
the same obstacle against the right edge costs (ratio 1.000, both stretches used) where taking
the widest of them cost 1.833 times as much. A stretch narrower than one em is dropped rather
than given a letter or two.

The layout is still reused across lines, which is what keeps a plain paragraph at one layout
rather than one per line, but the test for it is no longer the width alone: it also asks that
the cached layout's next line begin where the text now stands, give or take the whitespace the
last break ate. A line set across several stretches leaves the cache describing text that is
already on the page, and width alone would hand it back. Two traps in that check, each of
which cost a test: it must accept a next line starting BEFORE the text stands (the difference
is what the break ate) or the empty line Pango draws for a blank line between paragraphs is
skipped and every document that had one silently loses it; and a line that took no text must
still advance the layout, or the band is asked for again with nothing changed and the walk
never ends.

The map is anchored on the TEXT AREA's corner, less the margin above, and
not on the frame's -- taken from the frame while the extent is the inner size, every obstacle
sits one padding to the left of where the lines think it is. The GAP the text keeps around what it
avoids is the user's ("Gap" in the Text box section of the properties, `wrap_standoff` in the
document) and is grown on the merged map, on top of every obstacle's own reach, by a separable dilation rather than
asked of each shape: it then costs the same
whatever the obstacle is and reaches a raster as well as a rectangle, and the corner of an
obstacle keeps the gap along its diagonal too, which is what a rectangular offset does in
every layout application.

**The band is a line's INK, not its logical box, and the leading is not part of it.** What has
to clear a picture is the glyphs, and a logical box carries the font's full ascent above the
tallest of them -- measured, 72.96 units of box around 59.65 of ink. A band `h` tall narrows
the run by `h * tan(theta)` wherever the edge slants, so that surplus is charged straight to
the gutter. Measured perpendicular clearance beside a cut picture, line by line: spread 31.1
canvas units and sd 8.3 taking the logical box, 24.2 and 6.2 taking the ink. What is left is
geometric and not a defect -- horizontal lines set against a diagonal always clear it at the
line's own height by more than the gap asks for, by about `(band / 2) * tan(theta) *
cos(theta)`, which on the reported picture is the difference between 5 units of clearance where
the cut runs vertical and 15 along its slant. Widening the Gap raises both and narrows the
ratio; nothing short of letting glyphs overlap removes it.

**Paragraphs are separated by a BLANK LINE, as Markdown has it.** A single line break is a
SOFT break and joins the lines into one paragraph -- measured through the converter: a blank
line leaves two newlines in the text and the paragraph controls act on it, a lone newline
leaves none at all and they do not. That is Markdown's rule and not this code's, but it is
surprising enough at a keyboard to be worth saying in the tooltip, which it now is: it was
reported as the paragraph spacing not working.

**A paragraph's first line and the space before it are this engine's too.** Pango indents the
first line of every paragraph in a layout, which is exactly the rule -- so the plain path uses
`pango_layout_set_indent()` and the flowing one cannot, since every line there is the first of
its own layout: it moves the run's start in by the indent and leaves its end, so the line comes
out indented under every alignment and justified text keeps its right edge, and a NEGATIVE
indent hangs the line out of the measure the way a bibliography wants. Space between paragraphs
Pango cannot do at all -- its spacing is between LINES and it has no notion of a paragraph --
so a frame that asks for it is set line by line whether or not it wraps (`_text_flows()`).

**Which line opens a paragraph is a question about the TEXT, never about the cached layout.**
The character before where the text stands is the line terminator the last break ate, so a
newline there is a paragraph boundary; and a line beginning ON a newline is the blank line
between two paragraphs, which opens nothing and takes neither the indent nor the space. A run
of newlines is therefore ONE break however many it holds -- the markdown converter separates
its blocks with a blank line, and Pango renders the second newline as a line of its own with
no ink -- and the gap lands once, on the first line with ink. The blank line itself is left
alone, so a document laid out before the control looks as it did and the space is extra.

Asking the cached Pango layout instead ("is its next line preceded by a newline") answers
correctly only while that layout survives from line to line. A line set across two stretches
rebuilds it on almost every line, and the rebuild starts AFTER the break: the empty line
carrying it is never reached and the paragraph after it is never asked about. Measured on a
real document with the indent and the space both at 50 units, not one of its twenty-three
lines got either. For the same reason `_flow_piece()` steps over ONE newline and no more --
swallowing a run makes the blank line appear or vanish according to whether the layout happened
to be reused, which with two stretches is almost never.

**The leading is space BETWEEN lines, so this engine advances it by hand.**
`pango_layout_set_spacing()` puts it between the lines of one layout, and every line here is
line zero of a layout of its own, so no line's extents ever carry it: setting a line height did
exactly nothing to a frame that wrapped around something or hung its punctuation, while the
plain paragraph beside it honoured it. It is advanced once per GAP, and the "is there more
text" test reads `consumed` AFTER the line's own text is accounted for -- before it, the
paragraph ends on a trailing gap Pango would not have left.

**A line is offered a band a LINE tall, and the first line has no previous line to measure.**
The height of a line is not known until it is laid out, so the band is asked for with the last
line's -- which on line zero is nothing at all, and a band of nothing is clear of everything:
the opening lines were placed against a sliver of the map, given the full measure, and drawn
straight through whatever stood just below the frame's top. The band starts at the font's own
ascent plus descent times the leading, and a line that comes out taller than the band it was
placed against is asked again and set once more; the band only grows and the run only narrows,
so one extra pass settles it. Measured on the reported document, the first line was set as
"em ipsum dolor" with its "Lor" under the picture, and reads "Lorem ipsum" now.

Three more things about that engine that are not obvious. **Justification comes out right for
free**: Pango never justifies the last line of a layout, and each layout here holds all the
text that is left, so line zero is the last one exactly when the remainder fits on one line --
exactly when it should not be justified. **A line ends on the space it broke at**, so the last
byte of it is whitespace and never the comma that should hang; the walk back over what the
break ate is what makes a trailing hang appear at all, and without it the measurement is a
flat zero. And **the layout is only rebuilt when the run's width changes**, so a paragraph
with nothing over it costs one layout and not one per line.

**A line is drawn through the ITER's extents, never its own.** A line's own extents are
relative to where the line starts; it is `pango_layout_iter_get_line_extents()` that knows
where the ALIGNMENT put it. Taken from the line, every line begins at the layout's left edge:
invisible in ragged-right text, and centred text quietly stops being centred.

A text frame also carries two things its font description cannot say. Its **line height** is a
multiple of the leading the font asks for, and reaches Pango as the EXTRA space between lines
-- `pango_layout_set_spacing()` against the context's own metrics, rather than a newer call,
so it works wherever the rest of the application builds. Its **letter spacing** is a tracking
in thousandths of an em, so it follows the type size rather than the plane: negative condenses
a line, positive opens it out, and a condensed CUT is a different thing chosen in the font
name, since Pango can only reach one the family actually ships. The tracking is INSERTED into
a copy of the markup's own attributes; `pango_attr_list_splice()` is the call that looks right
there and is not, since it opens a hole of the length it is given and a zero-length one
collapses the attribute it is carrying. Both are 0 when unset, which reads as the font's own,
so a document from before them looks exactly as it did.

A cut-out frame is three layers over each other in linear light, composited in one pass
over the frame's float layer. Its **background** (every object has one, alpha 0 for none; a
text frame keeps its own field) fills the shape's whole support -- everywhere the cutout has
any coverage, hard-edged, out to the feather's outer edge
(`dt_canvas_render_mask_support()`) -- so the content's feather dissolves into that colour,
or into nothing. Its **content** is feathered by the shape. Its **border** starts where the
feather ends: the support dilated outward by the border width -- a disc, through the
Euclidean distance transform of the support (`dt_canvas_render_mask_band()`) -- and nothing
inside the support, so the band is solid and never mixed with the fall-off. A rectangular
frame keeps its border inside its edge with the content inset and the background under the
content, as before; both are the same rule seen from the shape's edge. The three rasters are
read by the compositor's own sampler, never through `cairo_mask_surface()`: cairo scales a
mask on one core, and did so three times per cut frame per frame. The raster's longer side is
the power of two at or above the frame's size on screen, so it is between one and two raster
pixels per layer pixel -- and four at the half resolution a gesture paints at, since the
gesture reuses the full frame's raster rather than rasterising every cutout again. Reading
such a raster at each pixel's centre alone throws away every other sample along an edge, so
the sampler averages it over the pixel's footprint, two a side and no more: a 2x2 box is what
the quantisation leaves over, and sixteen reads a pixel on a frame nobody is looking at yet
is not worth its cost.

### Padding frames

The word **gutter** now means what it means in print -- the fold's own allowance, see spreads
below -- so what every frame keeps around itself is the **padding**.
`DT_CANVAS_PADDING_VISIBLE` draws a frame one padding out around every frame, in the canvas's
padding colour, over everything -- a guide, so it is drawn with the grid and not exported.

**The padding is a margin around ONE frame, not a gap between two**, so two frames sit side by
side when their margin boxes TOUCH and the clear space between them is TWO paddings. Keyed on
one, as it was, a frame's own box landed exactly on its neighbour's edge and the two boxes
overlapped across the whole gap, each drawing its line on top of the other frame's border --
box against frame, which is what read as odd and crossing. Box against box they share one
line. The snapping, the masonry run detection and `dt_canvas_layout_apply()` all carry the
same factor of two, or an arranged layout would not be one the snapping can reproduce by hand.

### Shapes

A shape is a frame like any other -- it has a fill, a border, corners, a shadow, a cutout, an
opacity, a box, a rotation and a place in the order -- and it draws nothing else. That is why
one kind covers the rectangle, the polygon and the star instead of three, and why its record
holds only what its outline is made of: a geometry (`RECTANGLE` or `POLYGON`, stored by index
and so appended, never inserted), a side count, a notch depth and a roundness, taken from the
object record's reserved bytes with the format unchanged. A document a newer build wrote keeps
the geometry this one does not know and draws it as its frame.

**The outline is one function and every question asks it.** `dt_canvas_shape_outline()` answers
in the FRAME's own coordinates, and the hit test, the coverage raster, the silhouette reach a
connector stops against and the painter all read the same points -- a shape whose click test and
whose ink disagreed would be a shape that cannot be grabbed where it is seen.

#### The envelope, and why the depth is what is stored

A polygon and a star are one polar curve, `src/math/polygon_envelope.h`, the same closed form
the lens blur draws its diaphragm with: `n` blades, a concavity `m` and a linearity `k`. It is a
regular n-gon at `m = 1, k = 1`, a star for larger `m`, a circle at `k = 0`.

**`m` is only valid while `2 asin k + pi m < n pi`**, so a stored `m` would turn invalid the
moment the user lowered the number of sides under it -- at `n = 3, m = 2, k = 1` the shape
collapses to its centre and past it the radius goes negative. What the document stores is
therefore a **depth**: the fraction of the way the notch between two tips is pushed from the
straight edge towards the centre, so the inner radius is `(1 - depth) cos(pi / n)` and `m` is
derived for the sides in force. Any depth below 1 keeps the inner radius above zero, hence `m`
below `n - 1` for every `n`. The **roundness** is `1 - k`, so 0 is straight sides and 1 a
circle. Depth 0 is the convex polygon, and a depth under `DT_POLYGON_MIN_DEPTH` (1e-6) is read
as none, for two reasons at two scales: a notch pushed in by less than a millionth of the tip
radius still lies on the straight edge to rounding, which is what sets the threshold where it is;
and below about 1e-16 `1 - depth` IS 1, so the two edges meeting at a notch are exactly opposite
and anything taking their bisector divides nought by nought.

`DT_POLYGON_PENTAGRAM_DEPTH` (0.527864) is the depth at which a five-pointed star's edges run
straight through -- the figure's own number, not a choice of a good-looking star -- and it lives
with the geometry, so the canvas's `DT_CANVAS_SHAPE_STAR_DEPTH` and the toolbar's star glyph are
the same number; `test_canvas_document` pins the two against each other rather than making
`canvas.h` depend on `math/` for a constant.

At `k = 1` the curve is straight lines and is drawn as its vertices alone: sampling would only
lay points along a segment its two ends already describe. A rounded outline IS sampled, and not
evenly -- close to straight the curve turns nearly its whole corner within a sliver of angle
either side of a tip, so the samples crowd towards the tips, which brings the worst polyline
error over 3 to 12 sides, every depth and every roundness down to 1.75e-3 of the tip radius
from 1.36e-2.

#### Fitting the outline to the frame

The unit shape's tips sit on the unit circle; its own box is stretched onto the frame's, so the
shape touches all four edges and the frame's handles, the snapping and the layouts hug what is
drawn rather than a box around it. A frame of the outline's own ratio
(`dt_canvas_shape_unit_aspect()`: `2 / sqrt(3)` for a triangle, `sqrt(3) / 2` for a hexagon)
scales both axes alike, and that is the ratio `dt_canvas_object_keeps_ratio()` holds a regular
shape at. Every write to the sides, the depth or the roundness therefore owes
`dt_canvas_shape_refit_height()`, which takes the height back to the one the outline asks for
about the shape's own centre.

**`dt_canvas_shape_hold_minimum()` is the one place a box is held up to the smallest a shape may
have, and it lifts BOTH sides by the one factor the smaller needs.** Held up side by side, a
hexagon dragged out three units across lands in a SQUARE box -- and since a shape keeps whatever
ratio it is given, that square then survives every later resize. The drag in flight, the birth of
the shape it draws and the refit all go through it.

#### Fillets

The corner radius rounds a straight shape's vertices, and only a straight one's: a rounded shape
is smooth everywhere already and has no corner for a radius to take, so `_polygon_outline()`
drops the radius once the roundness leaves zero.

A fillet of radius `r` at a corner of interior angle `beta` touches both edges at
`r / tan(beta / 2)`. Where two neighbouring fillets would overlap, **the two ends of an edge
SHARE it in proportion to what each asked for** -- never a cap at half the edge, which
`polygon_envelope.h` warns this caller against by name: a star's segment is only half of the
convex polygon's side, so such a cap would halve a tip's largest fillet the instant the depth
left zero while the shape itself has barely moved. Measured continuity across depth zero: a
hexagon's tip fillet 25.9989 units at depth 0 against 26.1794 at 1e-3.

**The sharing is taken from the UNSHARED demands of a vertex and its two neighbours, in one
symmetric formula, never from a walk that rewrites the array as it goes.** A walk hands the
second end of every edge a reach the first end was already cut down to, and never revisits the
edge that wraps: measured, an equilateral triangle 400 units wide at a radius of 150 came out
with arcs of 108.4, 100.5 and 122.5 units at three corners that are the same corner three times
over, and a 12-point star at its deepest notch with a spread of 190 to 1 between its notches --
across sides 3 to 12 the worst ratio reached 1700. Read symmetrically, the two ends of an edge
still sum to at most its length, so nothing overlaps.

The arc's sample count is nudged off the exact multiple it lands on: every corner of a regular
shape turns by `2 pi / n`, which the six-degree step divides, and `atan2` falls either side of
the multiple by an ulp, so a plain ceiling gives one corner thirteen samples and the next twelve
for two arcs that are the same arc. `DT_CANVAS_SHAPE_OUTLINE_MAX` is 1024 because the worst case
over every side count, depth and radius is a straight 12-point star filleted, at 684 points.

#### Fill, border, and what a shape is picked by

**A shape's fill is not a backdrop, it IS the shape.** An outline box is a hole with a rule
round it, so an unfilled shape is picked by the BAND it paints and text flows through the middle
of it: measured on a 400-unit column, a filled 220-unit box costs it 247.5 units of height, the
same box unfilled 121.8, and a column with nothing over it 93.8. Whether the fill is there is
decided by an ALPHA, and so is whether the border is -- which makes both colours obstacle edits,
so a frame flowing round the shape is refitted whichever of the two ways the fill was emptied,
the Filled switch or the colour well, and a border painted in nothing paints no band and takes
no click either. A filled rectangle and an image frame given the same ground, border and radius
paint identically: 0 of 40000 pixels differ.

The border is a BAND, not a stroke: the outline clipped to itself and stroked at twice the
width, so the half that would fall outside is cut away. It is laid with `CAIRO_OPERATOR_SOURCE`,
which replaces rather than composites, so **it must be painted inside a group bounded by the
frame -- and only when there IS a band.** A fill is laid with OVER and isolating it changes not
one byte, measured on a transparent destination and on a filled one at three fill opacities,
while a group costs a frame-sized allocation, a clear and a composite: 0.05 ms on a small
shape's layer and 0.77 ms on a large one, per shape per repaint. The join is MITRED for the
NOTCHES -- a tip's join sits outside the shape and the clip discards it, while a notch's lies
inside and rounding it would blunt the one corner of a star's band that shows. Measured on a
pentagram 320 units wide with a 12-unit band, 362 pixels differ between a mitred and a bevelled
band and none of them is at a tip; the limit is never reached, a notch's miter needing at most
3.9 of it.

A polygon's silhouette -- what a connector stops against -- is capped against the frame's PLAIN
box, never the box rounded by the corner radius, since the outline does not take that radius
when it is rounded: capping with it cut the silhouette back inside a shape the painter fills to
the frame's edges, measured at up to 23.7% of the reach on a three-pointed star at roundness
0.25, which `dt_canvas_object_covers()` then reads as uncovered. And a polygon is never **cut**:
`dt_canvas_object_is_cut()` answers FALSE for one whatever mask it carries, the cutout chooser
is offered on a shape only while its geometry is the rectangle, and a polygon reads its cutout
back as NONE so the rows chained to it leave the card with it.

For text flow, `dt_canvas_shape_needs_coverage()` says a shape must be given its RASTER rather
than its frame: a polygon's outline is nothing a rectangle's reach can describe, filled or not,
and an unfilled rectangle covers only the band it paints. `dt_canvas_render_shape_coverage()`
rasterises the same outline -- filled when the fill is there, stroked as the band otherwise -- at
the occupancy grid's own pitch.

### Connectors, lines and curves

A connector joins two frames at **anchors**, or has ends of its own and joins nothing: an end
whose `from_id`/`to_id` is 0 is **free**, and a connector with both ends free is what the
atelier calls a line or a curve. There is no kind for it, because everything that reads a
connector reads its route and the route resolves a free end first -- see "Free ends" below.

An anchored end takes one of nine anchors per frame -- the four edge midpoints, the
four corners, and the centre -- all of them the frame's own points, so they rotate with it;
or `AUTO`, which picks of the four midpoints the one nearest the other end's frame. A
corner's normal is its diagonal, so a route leaves it at 45 degrees rather than running
beside an edge. **The centre is the one anchor whose handle is not where the route touches**:
the handle is the frame's centre, and the attachment slides to wherever faces the other end,
which is what to reach for when the side a route leaves by is the layout's business rather
than the user's. **It stops where the object actually draws something**, not on its bounding
box: `dt_canvas_object_silhouette_reach()` answers a ray out of the centre with the rounded
rectangle, or -- where a cutout replaces it -- the cut shape grown by its fall-off and by the
border band dilated from it, never past the frame. A disc and an ellipse are solved in closed
form (the ellipse in its own axes, where it is the unit circle), a polygon against the
straight run of its nodes, which the curve through them leaves by a fraction of a segment at
most. An inverted cutout is a hole, and a gradient covers the frame, so both give the frame's
own edge back. So a line to a round picture meets the picture, where before it stopped in the
empty corner of the rectangle around it. `AUTO` keeps its four candidates so a document
laid out before the corners existed keeps the routes it had. Anchors are stored by index, so
a new one is appended and never inserted. `dt_canvas_connector_route()` resolves a connector to its geometry once,
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
is the triangle's alone and stays sharp. All of it is in the connector's properties: route,
arrowheads, waypoint and direction on the strip, dashes, colour and width in its Line section.

**Ctrl while dragging a handle locks it.** A handle free to go anywhere -- a cutout's centre,
radius, feather or node, a connector's waypoint -- keeps to one axis, the one it has
travelled furthest along since the press, so the user chooses which by moving. A handle that
sets a direction rather than a place -- a connector's tangents, the gradient its curve leaves
by -- snaps that direction to 45 degree steps about the point it turns around, keeping how
far out it was pulled; the axes are among those steps, so it is the same lock said in the
terms an angle has. A frame's rotation reads it the same way, 45 degree steps, where Shift
reads 15. A handle already confined to a line, like a connector's reach along its anchor's
normal, has nothing to lock.

Connectors are drawn with the connector tool, armed from the toolbar or with C: the frame
under the pointer shows its four cardinal anchor dots, the first click picks the source
anchor, the second the target anchor -- the user chooses the anchors, nothing is resolved
automatically -- and the connector is selected as soon as it exists. The tool stays armed for
the next connector, as every drawing tool does; Escape gives back a first anchor already
chosen, a second Escape or a right click puts the tool away. See "The drawing tools" below.

#### Free ends

A free end holds the id 0, and ids start at 1, so no document written before free ends
existed can have one: the fields they need sit in what was the connector's reserved block (48
of its 72 bytes), the record keeps its size, and the format is not bumped. A free end carries
its own point and its own control offset -- a tangent of (0, 0) meaning "automatic", the way a
waypoint's does -- and is read only while the id is 0.

`dt_canvas_connector_route()` resolves them in **four steps, in this order, so that nothing is
circular**: which ends are free; where the free ends are; where the anchored ends are, each
aiming at the other end's frame centre or, when that end is free, at the point step two placed;
and only then which way the free ends leave, toward what steps two and three placed. An
anchored connector therefore routes to exactly the bytes it always did, whatever its free-end
fields hold.

**The fixed lengths of the anchored world must not reach a free end.** A waypoint's automatic
tangent had a 40-unit floor and a square routing's stub a 20-unit one, both sized to clear a
frame a free end does not have: a ten-unit line through a waypoint at its middle put its control
points at -35 and 45, and a square-routed line of no length painted a 40-unit dash. A free end's
own tangent spans a fraction of the distance to where the route heads next, its square stub stops
halfway to it, and a waypoint's automatic tangent -- floored to the START leg's share between two
frames, which is what every such route has always drawn -- takes the SHORTER leg's share once
either end is free, so a waypoint near one end cannot throw the curve out past it.

**The extent of a line is its ink**: the true Bezier extremes grown by half the stroke width,
plus each arrowhead's own triangle, which the painter fills from the same function. Growing the
arrowhead's reach on all four sides and bounding a curve by its control points is what made a
line ten units above a page edge export a blank page below it.

**Reverse walks the same curve back.** Swapping the two ends' ids, anchors, points and tangents
is not enough: a tangent handle's reach belongs to its end, and the waypoint's tangent points
towards the finish, which is now the other end. Reverse swaps `from_reach`/`to_reach` and
negates `via_tangent` as well. Measured as the largest gap between the reversed route and the
old one walked backwards: a free cubic through a dragged waypoint 62.09 units before, 7.1e-14
after; a steered cubic between two frames 68.89 before, 1.7e-13 after. One asymmetry survives
and is pinned rather than hidden -- between two frames the automatic waypoint tangent's length
is 0.4 of the leg leaving the START, so a waypoint slid towards one end walks back 46.5 units
off. No Reverse writer can mend that, and making the length symmetric would move every anchored
cubic in every existing document.

A free end is edited like any other handle: it has a site of its own on the selected line,
listed ahead of the control points so that a press near both takes the end, and the end marks
are painted in a pass after the rest of the selection, last selected first, so the mark on top
is the one a press takes. Dragging one snaps to the grid, or holds 45 degrees under Ctrl and 15
under Shift about the other end (`dt_canvas_constrain_line_end()`, which is GTK-free and is what
the drawing tools use too). A line with a free end also moves with a drag, the arrow keys, a
rubber band (tested against the painted extent, arrowheads included) and Select All, and its
context menu offers Duplicate -- a connector anchored at both ends has no place of its own to be
copied to. A move snaps by the selection's first unlocked FRAME, else its first unlocked line: a
locked leader never moves, so the offset it measured was handed to the rest of the selection on
every motion. And Auto arrange counts the selected FRAMES to decide between the selection and the
whole canvas, since a rubber band now gathers lines as well.

### The drawing tools

A press on the plane picks, moves and selects; with a **tool** armed it draws instead. At most
one tool is armed at a time (`dt_canvas_tool_t`, never stored -- a document knows nothing of how
its objects were drawn), and **it stays armed across the objects it draws**: C the connector, L a
line, Shift+L a curve, B a rectangle, P a polygon, Shift+P a star. A tool is put away by Escape
with no drawing in flight, a right click, arming another tool, pressing its own key or toggle
again, leaving the atelier, or a new document.

**Every arming and disarming the user asks for goes through `_tool_set()`, and it raises
`DT_SIGNAL_CANVAS_CHANGED`**, whatever path the change took -- a key, the toolbar's toggle,
Escape, a right click. That is what lets the toolbar own no state and still show the tool the
view holds. Leaving the atelier and replacing the document put the tool away without a word,
since the toolbar is refilled with the view and with the document anyway.

Escape unwinds in one order, and the gesture comes before the tool because a tool stays armed for
as long as the user wants it: the drawing in flight, then any other gesture, then a connector
waiting for its second anchor (which gives the first one back and keeps the tool), then the tool,
then the properties, then the selection.

**The handles of what is SELECTED answer a press first**, so the line or the shape just drawn is
adjusted by its own ends and corners without putting the tool away. Everything else under the
pointer draws, an object included -- that is what having a tool armed means, and the crosshair
says so where the hand would promise a pick that no longer happens.

**The drag draws the object itself.** It exists in the document from the first frame past the
threshold, so what the drag shows is the painter's own line or shape rather than a sketch of one.
A far end follows the pointer through `dt_canvas_constrain_line_end()` -- the grid, Ctrl at 45
degrees, Shift at 15 -- and a shape's box follows it with Ctrl holding it square and Shift taking
the press point for its centre.

**"The pointer really moved" is decided on the CONSTRAINED geometry, not on screen pixels.** The
threshold is three pixels and a grid cell is twelve units, so a drag shorter than a cell made a
line of NO length: measured through the constraint, a pointer 8 units from an on-grid origin comes
back AT the origin, and the axis locks land on it too. A press that ends there still places its own
object at the release. The same rule the other way round for a shape: refusing a box that opened on
one axis only answered a deliberate three-hundred-unit drag half a grid cell tall with the box a
CLICK places, at the press and nowhere near the pointer. Each side is held up to the smallest a
shape may have instead.

A regular shape's drag is always regular, and **it takes whichever side the pointer went further
along, measured in the shape's own proportions** -- taken from the horizontal travel alone, a drag
straight down drew nothing at all, the snapping putting both ends of it on the same grid line.
Ctrl has nothing left to constrain there: a regular shape is already square in the only sense it
can be.

A click that never moved **places** an object of its own: a line 160 units long whose far end goes
through the same constraint, level, so it is the only line end in the atelier that would otherwise
sit off the grid (with the asked-for length standing where a grid coarser than the line would
collapse it); or a box centred on the click, at the shape's own ratio. Either way it is one undo
step, recorded at the release, and the new object is selected.

A drawing whose object is taken out from under it -- Delete or Ctrl+Z reached by the keyboard with
the button still down -- leaves the document as that edit made it and records nothing: restoring
the press's snapshot would take the edit back, and recording a step from it would reinstate what
was undone.

Every gesture ends through `_cursor_for_armed_tool()`, because a gesture leaves the tool exactly
as it found it: an ending that named the arrow outright would stop naming a tool that is still
armed, and after an Escape there is no motion coming to name it again. The same call moves the
start marker, the ring where the armed tool would begin its next object. The floating properties
close whenever a tool is armed: drawing is pressing on the plane over and over, about a different
object each time.

#### The style memory

**A new object is born with the style the last one of its sort was left at.** A line takes the
width, colour, dashes and arrowheads of the last free line drawn or edited; a shape takes the
fill, the border override and its width and colour, the corner override and its radius, and the
shadow override and its values. How a shape is COLOURED is one memory for all three shape tools;
what its outline is MADE OF is one per tool -- the polygon tool remembers its sides and roundness,
the star tool its points, notch depth and roundness -- since a polygon and a star are two
different things to reach for.

The memory lives in the configuration (`plugins/canvas/new_line/*`,
`plugins/canvas/new_shape/*`) and **is the VIEW's alone**: `src/canvas` knows nothing of conf, and
what crosses the boundary is a GTK-free `dt_canvas_line_style_t` / `dt_canvas_shape_style_t`,
sanitised on the way in because what comes back from a configuration was written by whatever wrote
it. The property writer says which edits are worth remembering
(`DT_CANVAS_EFFECT_COMMIT_CONF`), and only a line with BOTH ends free teaches a line style --
a connector holding a frame is born with the defaults and styling one teaches the next line
nothing. Remembering is not a change to the document and is no part of its undo step: one undo
still takes the object away, and the style it taught stays taught.

Two rules that are not obvious from either side. **Which groups a shape owns is as much a part of
its style as their values are**: taking one is always followed by a value write that asks to be
remembered, but handing one back stands alone, so `dt_canvas_group_set_own()` reports
`COMMIT_CONF` for a shape either way -- without it the next shape was born with the override the
user had just removed. And **a shape drawn with a regular tool is born owning a corner radius of
NOTHING whenever the memory says to inherit the canvas's**, which is the rule the card's own
geometry writer already states where a rectangle is turned into a polygon: on a canvas whose
Corners had been raised, Shift+P drew a star with filleted points while the card's Geometry gave
sharp ones, and the Corners row read "inherited" and said nothing about why.

### The overlays

Selection handles, hover outlines, the rubber band, the connector being drawn, the status
line and the navigation flower are the view's and are painted after the document. The status
line is inked dark or light against the plane's luminance, with a halo of the opposite, so
it reads on any background colour or paper and over a picture.

A tool armed adds two of its own (`_paint_tool_overlay()`), and no more, because the line or the
shape being dragged is the painter's own and needs no sketch of it: the **start marker**, a small
ringed dot under any tool that draws an object of its own -- so every one but the connector --
while nothing is being dragged, sitting with snapping on at the grid point the object will take,
which is the one thing the crosshair cannot say; and a **dashed hairline box** around the frame of
the shape a shape tool has begun, since a shape born with neither fill nor border draws nothing of
its own and the drag would otherwise show nothing at all. The connector tool paints neither: it
shows the hovered frame's anchor dots and, once a first anchor is chosen, a dashed lead line from
it to the pointer.

**Every overlay line carries its own opposite**, for the same reason and by the same trick
seen three ways: the status line's halo; the selected frame's solid light rectangle under a
dashed dark one; and the hovered frame's two adjacent hairlines, light against the frame and
dark just outside it. A single pale line is legible on a dark plane and gone on a bright one,
and a canvas is as often one as the other. The hover pair sits wholly past the frame's edge,
so it never covers what it is outlining.

### The floating properties

One object's properties float beside it, as **one widget: a STRIP, one row, that can grow a
CARD** below or above it. The strip holds the kind's glyph or a line about the object, the
kind's everyday controls (a text frame's font, size, colour and alignment; a connector's route,
arrowheads, waypoint and direction; a map's zoom; a shape's geometry and its Filled switch), then
the content action, the card button and
a close button. The card is an accordion of sections in one fixed order for every kind -- the
kind's own sections, then Arrange, Fill, the stroke (a frame's Border, a connector's Line, the
same slot), Corners, Shadow and Cutout -- each a folded header with a one-line summary of what
it holds, its essentials, a rule, and what an expert reaches for. **A section is present when
one of its rows APPLIES to the object in front of it, not when the KIND owns rows in the table**:
a section every one of whose rows a geometry or a switch has closed has nothing left to show, and
a heading over nothing is a heading that lies. A section a kind lacks is absent, never greyed
out. One section is open at a time, and the one left open is remembered per
kind (`plugins/canvas/props/section/<kind>`, stored by NAME, since the enum's order is the
screen's and may change); the card itself is never remembered open, and an override section or
the cutout never opens by itself. Measured offscreen at 96 dpi against the shipped theme, the strip
is 35 px tall for every kind and 456 px wide for a text frame, 381 for a picture or a drawing, and
340 for a map, a connector or a shape -- 340 being `PROPS_MIN_WIDTH_PIXELS`, the floor the width is
held up to, which is why those three kinds report the same number rather than three close ones. A
connector reports the same number anchored and free. The card is the strip's width, so the card
button and the close button do not move when it opens, whichever side it grows on.

**Those numbers are what the placement solver is given, so a stale one is worse than none**: it
decides how much room a spot must have before the properties will go there, and a strip measured
before a row was added or removed is a spot that turns out too small. Re-measure whenever a strip
row changes -- the text frame's 456 was 472 until its type size stopped carrying a "pt" label of
its own, the unit now living in the field -- and record the number here rather than leaving the
solver to a guess.

**Everything about a property is described once, in `canvas/canvas_props.c`**: its label, the
kinds that have it, its section and tier, the control its nature gets, its range, what "left as
it is" means for it, the override group it belongs to and what row it depends on. Edits go
through `dt_canvas_prop_write()`, GTK-free and conf-free, which applies every rule the edit comes
with -- a picture that keeps its proportions answers a width with a height, a font equal to the
canvas's is stored as none, a text frame whose height follows its text is refitted -- and
returns what the caller owes as effect bits: an undo step, a refit of the frames flowing around
it, a map render, the next map's defaults, a restructure of the rows. `test_canvas_props`
round-trips every property on every kind. The GTK face, `views/canvas_props_gtk.c`, builds every
row of every kind ONCE, and a refill only shows, hides and fills them; it lives in `views/`
because `gui/` sits below `canvas/` in the layering. **The same nature is the same control
wherever it sits** -- a spin button for an exact number (a position, a size, a type size), a
bauhaus slider for a bounded perceptual one, a row of glyphs for a few choices -- so a property
does not change shape between the strip and the card, or between two kinds. No slider sits on
the strip: a bauhaus slider is a line and seven tenths tall, and every strip is one button tall.

**A property may appear on the strip AND in its section, and only a colour may.** A row's tier
says where it lives, and a tier is one place; the mirror is the exception, declared on the row
itself as `strip_kinds` -- which kinds show it on their strip -- with an optional `strip_if`
naming the row that has to be on for it to appear. A shape's outline and fill colours are there,
and a connector's line colour, because reaching a colour is the whole of what those strips are
for, while the row itself belongs in Stroke or Fill beside the width it goes with.

**Only a colour, and that is a rule the table enforces rather than a habit.** A mirror is two
controls showing one property, so whichever the user is holding, the other is being refilled
underneath them -- and a colour is the one nature that holds no gesture state to lose: a well
reports a colour and is done, where a slider being dragged, a spin button with digits typed and
not yet applied, or a combo with its list open all carry something a refill would take away.
`_a_strip_mirror_is_a_colour_and_nothing_else` holds it: every row with `strip_kinds` set must be
a colour, must not itself be a strip row, must not name a kind the property does not apply to,
and must not be half of a `pair_with` couple -- a paired row shares its partner's widget, and a
shared widget cannot be in two places.

The strip's own row count is capped at six, and a mirror counts against it -- it costs the same
width as any other control -- so `_every_kind_reads_its_sections_in_screen_order` counts the
mirrors alongside the `STRIP` rows. A cap that is exactly the number in use is the shape of the
next silent defect: the toolbar's own `DT_CANVAS_TOOLBAR_NUMBERS` sat at precisely five while
five sliders were built, and a sixth would have been refused by `_prop_slider()` and simply not
appeared, with nothing said anywhere.

**An override group reads what the object is DRAWN with.** Border, corners, shadow and font
are the canvas's until the object takes its own. Editing one field while the object inherits
seeds the whole group from what is on screen, applies the edit and sets the flag; writing the
inherited value while inheriting changes nothing at all, so a control reset to the canvas's
value leaves the object following the canvas. The section's own switch takes the group without
changing anything on screen, and gives it back to the canvas. The floating bar these properties
replaced carried those rules in its handlers, one copy each, behind an in-band `-1` "default" in
its spin buttons, and two copies had drifted into data bugs: an offset edited while the blur
read "default" was written into a shadow the object did not own, and picking a border colour
made the frame own a border of whatever width its field happened to hold.

#### When they show

A single click, a drag or a rubber band **never** shows anyone's properties: laying a page out is
clicking and dragging all day, and a panel that answered every click stood over the next thing
to grab. Three things open them -- a double click on an object, the `I` key and the context
menu's "Properties" entry -- and entering the atelier after a darkroom round trip shows them
again, as a strip, while their object is still the whole selection; `I` pressed while they show
takes the keyboard to their first control. Everything else goes through `_props_sync()`, which
refills, places, hides or closes them and never opens them. They close when the selection
stops being exactly that object, on Escape (after the drawing in flight and the armed tool,
before the selection) and whenever a tool is armed. Leaving the atelier does NOT close them:
it commits what they hold and takes the widget down but keeps them open, so `enter()` shows
them again as a strip while their object is still the whole selection. They hide while a gesture really moves
something (a move or a scale past the threshold, a pan, the wheel, a rubber band past 3 px) and
come back when it settles, so the first click of a double click never makes them blink.

**The drill rule**: a double click on an object whose properties are ALREADY showing goes into
it instead -- a text frame's Markdown editor, a picture in the darkroom, a drawing's file read
again -- and so does Return. What decides is `dt_canvas_click_sequence_t`, pure logic that
follows GDK 3.24's own pairing, read from `_gdk_event_button_generate` rather than assumed: the
same button, strictly sooner than `gtk-double-click-time`, within `gtk-double-click-distance` on
each axis, timed by the events' own timestamps, never the handler's clock. A double click is
read against what showed before ITS OWN first press, which is the press before the one GDK
reports -- not the run's first press, which may be a click beside the object that closed its
properties. A run answers once, so four or five fast clicks open the properties and never
drill, though GDK reports the fifth press as another double click. The opening and the content
action run from an idle at `G_PRIORITY_HIGH_IDLE`, ahead of the redraw the press queued, and a
press handled since takes them back: the old double-click branch ran the text editor's modal
dialog from the press handler while the second press's move and its snapshot were still armed.

#### Where they go

`canvas/canvas_place.c` answers it, GTK-free and document-free: plain rectangles in, a rectangle
out. What is on screen reaches it as shapes with a CLASS, already grown by how far each catches
the pointer:

- **HARD is never covered**, whatever else fails: frame corners, the rotation knob and its stem,
  a connector's tangents and tethers, its waypoint, the band its line is picked in, its
  arrowheads, a cutout's handles and a polygon's nodes while the shape is edited, the navigation
  flower, and at an opening the pointer that asked;
- **PREDICTED** is what one click in the properties would add -- a straight connector's cubic
  tangents and waypoint, a cutout's handles before Edit -- and is covered only when nothing else
  fits;
- **BODY** is the object itself, cut into slabs along its turned quadrilateral, and is covered
  only once every spot clear of it has failed;
- **SOFT** -- the other frames, the status line, the toast -- only weighs in the cost.

**Every site comes from one list.** `canvas/canvas_handles.c` enumerates an object's handle
sites -- each a square, a disc or a segment, with a reach in screen pixels that does not zoom
and one in canvas units that does -- in the order that is the hit tests' priority, and the
view's `_handle_at()`, `_tangent_handle_at()`, `_via_handle_at()`, `_mask_handle_at()`,
`_mask_node_at()` and `_mask_segment_at()` are loops over it. `canvas/canvas_place_shapes.c`
turns the same list into the placement's HARD shapes, so the placement avoids exactly what a
press catches. Before it, six hand-written hit tests each carried their own copy of where a
handle is and how far it catches, and the old bar's box read only the route's points: it covered
a cubic connector's tangents, its waypoint and the knob 28 px above a frame, then clamped itself
back over the object when there was no room below. A scratch harness compiled the old hit tests
against the new ones: 25,035,084 comparisons over 3000 random scenes, no mismatch. PREDICTED
shapes are asked of a COPY of the connector with the click applied, which only the route and the
site list read: the real edits touch the document, and setting a cutout's shape on a shallow copy
frees the nodes the original still points at.

Two things the list inherited are not fixed. A circle lists four cutout points like an ellipse, so
that its feather keeps index 3, and its unused [2] is left at the frame's centre: while the shape is
edited a dot is painted there, a press there drags a second radius the circle does not have --
which, walked outer points first, also shadows the centre handle of a circle left where it is
born -- and the placement keeps clear of it like any handle. `test_canvas_handles` leaves that
point unpinned on purpose; the fix is for the circle to list only the points it has. And the view's
priorities between roles -- the hovered node's own handles before the nodes, a cutout's outer
points before its centre -- live in static functions of the view plugin, where no committed test
reaches them.

The search runs level by level, from clear of HARD, PREDICTED and BODY to clear of HARD alone,
and a candidate top is only ever moved **within a stretch of the view proven free, never clamped
onto the object** -- which is what makes the old bar's failure impossible by construction rather
than by care. Three reasons ask for a placement. OPEN searches everything. RESOLVE -- a pan, a
zoom, a refill -- keeps the placement for a motion of two pixels or less and translates it
rigidly with the object otherwise, while it is still clear at the level it was found at, so the
properties follow the object instead of jumping; one that had to cover the body is kept only
while no spot clear of it exists and it covers no more of it. GROW -- the card opened, a section
grew -- keeps the strip where it is and fits the card on whichever side holds all of it, so
opening the card never moves the button under the pointer unless neither side can. **A card is
placed whole or not at all**: the first version capped it at 420 px and 60 % of the view and
scrolled it in whatever room the strip's column had, and a drawing's card showed six of its
seven sections over a scrollbar in a view with room for all of them. Only a view shorter than
the strip and the card scrolls it, with `GTK_POLICY_EXTERNAL`, so the wheel scrolls it and no
scrollbar is drawn. A view narrower or shorter than the strip itself does not hide them either:
they are placed as if they were the view's size and the overlay cuts off what does not fit.
Hiding them there, with a toast telling the user to zoom out, asked for what could not help, since
no zoom makes the view any larger. They hide, and say so once per showing, only where the HARD
shapes leave them no room at all.

Inputs are snapped to whole pixels -- the view inward, every shape outward, the air and the
widget up -- and the anchor and the press to a 256th of a pixel: from a raw anchor, moving a
whole scene by whole pixels changed the rounding of a near tie, and the same pan resolved to two
spots. A placement that found no room is no previous placement: its all-zero strip once made the
next search pay a movement cost from the view's top-left corner and go there.
`test_canvas_place` checks every placement of 10,000 scenes against a brute force over every
pixel -- never a handle, the body only when nothing clear of it holds the strip or the card --
and `bench_canvas_place` holds each case to 10 ms: 2048 shapes open in 0.32 ms, over the body in
0.35, and a view crowded with handles everywhere but a corner, card open, in 3.0.

**`canvas/debug/placement` paints what the last placement kept clear** -- HARD red, PREDICTED
orange, BODY yellow, the footprint green -- and a Debug build asserts that no HARD rectangle,
grown by the air, overlaps the footprint. A report of the properties landing on a handle is
answered by switching it on, not by reasoning about the solver.

**They never move while the user is in them.** A placement waits while the pointer is over
them, while digits typed into a spin button are not applied yet, and while an edit is LIVE; it
runs on leave, on Return, on a focus change or on canvas motion. The keyboard focus ALONE holds
nothing: a slider and a spin button keep the focus once clicked, so a hold on the focus never
ended, and after a click on a width's + the properties stayed over the corners the frame had just
grown into until the canvas was clicked. The root is an event box, so crossing between its
controls is an INFERIOR crossing and not the pointer leaving; grab crossings (a dialog, a popup)
are not leaving either.

The widget is **one overlay child of the centre**, placed by answering the overlay's own
`get-child-position` with the solved rectangle -- a margin change is a resize that climbs to the
toplevel and lays the window out again -- measured height-for-width and never below its minimum.
A placement runs from idles and from the card's and the sections' own handlers, never from a
draw (moving an overlay child from inside a draw glitches) nor per motion, and a placement that
moved nothing queues no redraw of it, since invalidating an overlay child repaints the canvas
under it. GtkOverlay sizes by its main child only, so opening the card never resizes the window.

**What was not used, and why.** Overlay pass-through: GDK still delivers events to a child's
subwindows, so the buttons would catch clicks while the gaps between them leaked to the canvas --
inconsistent, and unneeded, since the rectangle covers no handle anyway. An input shape: the
footprint is a rectangle already. Popovers, revealers, stacks and notebooks: the old bar's three
popovers were each a surface with a placement of its own, which nothing kept clear of the
handles; the card is the one place a property lives, at a nesting depth of two at fixed places --
the card button, then a section header -- and only visibility changes inside it. The font button
opens a MODAL `GtkFontChooserDialog` of its own (`widgets/chooser_button.c`): the dialog
GtkFontButton opens cannot be relied on to be modal, and a modal one leaves no canvas click that
could change the object a pick lands on. It reports a pick once, after the dialog is gone. Neither
button takes the focus on click, so the focus is not left on a button holding the plain keys.

**Every colour of the atelier is picked in a colour well** (`widgets/color_well.c`), which a colour
button opens in a small modal window beside it -- the object properties' five colours and the
toolbar's eight alike. GtkColorChooser showed a palette first and hid the precise colour, opacity
included, behind "Custom"; the well shows at once the recent colours (one list for the whole
atelier, `plugins/canvas/color_history`, most recent first, at most ten), a saturation/value field
beside a hue strip, an opacity strip with a checkerboard under it, and the colour as
#RRGGBB[AA] beside the colour it opened with. The window is a POPUP holding the grab, as bauhaus's
popup is: keys reach its entry through the grab, a click anywhere else in the application closes
it and goes no further, and it is placed under the button (above where the screen has no room) --
by `gdk_window_move_to_rect()` on Wayland, where no client places a window, and by moving itself
elsewhere. **One opening of the window is ONE gesture**: its changes reach the properties as LIVE
steps (the canvas follows every motion from one snapshot), closing it -- its × button, a click
outside, Return, the application losing the focus -- COMMITs once however many drags it held, and
Escape CANCELs: the document goes back to the snapshot and nothing is recorded, a border the object
inherited until the first drag included. A colour dragged away and back closes as a cancel too,
rather than as an undo step that undoes nothing -- and back means back TO THE BYTE: a recent colour
is stored as bytes and a document colour as floats, so the swatch naming the colour at hand is a hair
away from it, and compared exactly it made a change nothing on screen showed. The well applies the
same rule to its own commits. The window takes no focus from the window system on X11 or Wayland, so
"the application lost the focus" is read from the button's toplevel; where a backend does give the
popup the focus (Broadway, the moment it maps), the popup's own losing it is what counts, or it would
close the instant it opened. A menu opened inside the window (the number's context menu) grabs the
keyboard, which X11 reports as the parent losing the focus: the close waits while another grab than
the window's is current. A close because the application lost the focus hands no focus back, where
every other close does: the parent would be raised over the program the user went to.

Every control of the well takes the keyboard: Tab moves between them, the arrows move the field and
the strips by a hundredth (a tenth with Shift), each key a whole gesture, and walk the recent colours,
where space or Return picks one -- Return then closes the window on it, the control with the keyboard
having the key before the window. A well without opacity (the background's) offers only the opaque
recent colours: the list is shared, and a transparent one would be picked as another, opaque colour.

#### How an edit reaches the document

Every change reaches the view tagged with its phase (`dt_canvas_edit_phase_t`): **LIVE** follows
a control while it moves, **COMMIT** ends that gesture with the control's final value, **ONCE**
is a whole gesture in one call -- a typed number, a click -- and **CANCEL** abandons a LIVE gesture:
the document is restored from the session's snapshot by `dt_canvas_abandon()`, and nothing is
recorded or announced (no LIVE step announced anything to give back). Only the colour window sends
it, and a colour window stays open as long as the user likes, so the restore keeps what landed
meanwhile: a picture's or a map's render (put back as the snapshot had it, the picture would read
RENDERING with no job left to finish it), and the saved state -- a document with nothing to save
before the gesture has nothing after it, unless a render landed. The view pays for it as one edit
session per gesture: one snapshot at its first step; at every step the frames flowing around
what moved refitted and the document touched ONCE, because the painter keeps the frame it last
composited for a generation it has already seen; at its end one undo record, one map fetch, one
configuration write and one `DT_SIGNAL_CANVAS_CHANGED`. A held button -- a slider dragged, a spin
arrow held past the debounce -- is one session until it comes up; a wheel burst, a run of arrow
keys or a combobox's notches are debounced 400 ms into one; and a click on a slider that did not
drag commits after the double-click time, so the double click that resets it lands in the same
gesture.

**What the properties hold is committed before anything else reaches the document**
(`_props_commit_pending()`): a press on the canvas, a key that acts, an undo, a toolbar setter
that records undo, a change of object, a close, leaving the atelier. The binder's own debounced
session is committed through the host and its timer removed FIRST: left armed, a Ctrl+Z inside
the 400 ms window was undone and then re-done by the timer, wiping the redo list, and a corner
drag right after a wheel step got the old width written into it. The Edit menu's undo is the one
place that cannot record, since the undo stack is locked while it pops; it forgets the session
instead (`dt_canvas_props_gtk_forget()`).

**A refill never writes back.** Every handler a refill could wake is blocked by its stored id
while it writes, with `dt_gui_widget_freeze()` around the bauhaus sets, and the control being
dragged is left alone; a drag still held when the object changes is marked stale and reports
nothing until release. The properties refill on `DT_SIGNAL_CANVAS_CHANGED`, so a canvas default
set from the toolbar shows at once in every inheriting summary.

**Keys.** While the focus is inside the properties, a key with no modifier or Shift alone reaches
the control rather than a single-letter shortcut (`dt_accels_block_plain_keys_inside()`, read on
every keystroke so nothing can stick), and Primary shortcuts and the function keys still fire; a
text field there keeps only the modified keys its own class binds (Ctrl+A, Ctrl+C, a word jump).
The view swallows Delete, BackSpace, the arrows and Return the controls did not take -- they
would delete, nudge or drill into the object being edited -- and Escape gives the focus back to
the canvas, so a second Escape closes the properties.

**The properties are the one home of a property.** The context menu keeps what they cannot
offer -- what only a pointer can name, the polygon node or edge under it, and the object-level
actions: Order, Rotate, Duplicate, Delete, and the content actions with their Return shortcut.
The cutout's shape, its inversion, its edit mode and its sliders (feather, opacity, size,
rotation, extent, curvature) left the menu when the properties went live, as did the Markdown
editor's Font button: a second writer of the same field, with its own copy of the rule that a
font equal to the canvas's is stored as none. The menu's Size was not quite a duplicate: on an
ellipse it scaled both radii, and nothing else did -- a handle drags one radius, the wheel sets
the feather -- so the properties' Size now scales an ellipse whole and Size Y alone changes its
proportions (`test_canvas_props`). **A menu entry that changes
what the properties show or what they keep clear of calls `_props_sync()`**, like any other edit.

### Borders, the padding, and snapping to neighbours

A frame's width and height are its outer size, border included: the border is stroked
inside the edge and the picture (or the text) is inset by it, so widening a border shrinks
the picture and never grows the frame, and the anchors, which sit on the frame's edge, stay
on the outer border.

The canvas carries a **padding**, the margin every frame keeps around itself, and a **snapping
mode** chosen in the toolbar: any combination of three rules, applied in this order to a
move and to a resize, each later rule that triggers replacing the earlier answer. The grid
rounds positions and sizes to the grid step. The padding lands an edge next to a neighbour
**two paddings away** -- each frame keeping its own, so their margin boxes meet on one line --
or in line with a neighbour's edge, within eight screen pixels
(`dt_canvas_snap_to_neighbours()`; on a resize only the dragged edges may snap). Same size
gives a resized frame a neighbour's width or height within reach (`dt_canvas_snap_size()`),
or the combined width or height of a run of neighbours stacked two paddings apart (masonry
style); while it snaps, the frame(s) the size was taken from are outlined and a guide line
runs along the matched dimension on both.
An image frame resizes proportionally and follows its width; a text frame resizes freely.
The layouts leave two paddings between frames too.

### The plane: background, grid, paper

The canvas's background colour is the paper's colour too: the relief is a zero-mean
modulation of it -- the colour is the fundamental the texture rides on -- so choosing a paper
sets the background to the colour that paper is sold in (`dt_canvas_background_tint()`) and
the colour patch stays live to recolour it. The relief comes in two parts the user weighs
from the Texture popover, in `dt_canvas_t.texture_*`: **contrast** scales the body (mottle,
tooth, clouds), **detail** the fine structure (fibres, pores, grain, wrinkles, the mesh),
**scale** the size of every feature (every knee divided by it; the only one that rebuilds
the fields), **grain** the finishing dither. 1 everywhere is the paper as designed.

**The Reset button refills the sliders itself.** Every other call to the texture setter is
one of the four sliders sending its own value, and refilling under a slider the user is still
holding would fight the pointer -- so the setter stays quiet and the one caller that writes
all four behind their backs refreshes them. Without that the reset reached the document and
nothing else: the sliders kept their positions, so it read as doing nothing, and the next
touch of any slider sent all four stale values back and undid it.

**All FOUR at zero is an old file and reads as 1; one weight at zero is zero**
(`dt_canvas_texture_get()`). The rule used to be per field, which conflated the migration
with a deliberate setting: a user who turned the grain -- or the detail -- down to nothing
got the default back instead, so the bottom of those two sliders did nothing at all. A canvas
with all four weights at zero is a plain colour by another name, so that one combination is
what the migration costs. `scale` keeps a floor of its own because it divides every knee.

The dither's own strength was the other half of "the grain has no effect". It is applied to
the LINEAR canvas and read on a gamma-encoded one, so a fraction there arrives as roughly
half of it in code values, and it competes with the paper's own pixel-level content: measured
on the moleskine, that content is 1.45 codes while the dither at `PAPER_DITHER_SIGMA` = 0.008
was 0.646, so moving the weight from nothing to its default changed the pixel texture by 10%
-- invisible. At 0.018 the two are comparable and the weight spans what its range promises:
1.43 codes at 0, then 1.62, 2.05, 3.24 and 5.83 at 0.5, 1, 2 and 4. The quadrature blend
above is what made this worth fixing now: it raised the paper's own content over most of the
sheet, and the dither, which used to stand out wherever the composition had attenuated the
paper, no longer did anywhere. The two composed
fields are kept per resolution and scale, the coloured and weighed tile per key.

The canvas is painted with its background colour, with a transparent plane, or with one of
eight procedural papers:
**Moleskine** (cream, fine soft clouds and short fibres in every direction), **watercolour**
(white, a tooth that only darkens so the paper is white at its peaks, rounded pores),
**embossed** (a wove sheet dried on a metallic mesh: a mottle and fibres, and the mesh's
grooves stamped over the blended field in absolute coordinates, so it stays one mesh across
placements; the weft threads closer and deeper than the warp, and the sheet's own relief
bending the threads and varying their pressure) and **Japanese** (washi: large soft clouds and long wrinkles, the zero crossings
of a low-frequency field lit as ridges, the same lines at every zoom). **Psychedelic washi**
is that last sheet with its wrinkles dyed instead of lit: the same clouds from the same knees
and the same ridges, but the ridge reaches the pixels bare -- a coverage in [0, 1] rather
than a depth -- and each channel takes its own share of it, a third of a turn apart. It has
to SUBTRACT: a paper sits near the top of the scale, so a ridge added to every channel only
clips to white, which is exactly what makes the achromatic washi's ridge read as a highlight
and forbids the same arithmetic here. Two channels are pulled down and the third is spared,
hard enough to reach zero at a ridge's core -- that clamp is the look, a saturated thread
rather than a pastel one -- and which channel is spared comes from the cloud underneath, so
neighbouring wrinkles are different colours and one wrinkle drifts along its length. The
cloud is read as about a fifth of a turn either side of the paper's own hue: a steeper turn
spins the hue faster than a wrinkle is wide and comes out as fringing, not as dye (measured;
the first attempt swept eleven turns across one sheet and read as chromatic aberration). It
carries no grain field of its own -- a grain added to the cloud would scramble the hue at
pixel scale -- and takes the same doubled dither the achromatic washi does.

Three more were added after a survey of what real papers look like and of which of their
signatures these primitives can actually carry. **Laid** (verge) is the mould's own imprint:
a wild, cloudy formation under two families of wires stamped in absolute coordinates like
the embossed paper's mesh, both reading LIGHTER because the pulp settles thinner where a
wire touched it -- a ripple along the close-set laid wires and a narrow line along each
chain wire. Their pitches are the real ones **and** divide the composed field's 3072-unit
period, which is what lets the tile still wrap: laid wires every 3 units (1.06 mm against a
measured 1 mm) and chains every 64 (22.6 mm against 23). **Kraft** is unbleached softwood
pulp: a broad blotchiness from uneven cooking, long fibres nothing bleached out, and shives
-- flecks of bark -- as the far tail of a band-passed field. **Charcoal card** is the mirror
of the watercolour's rule: that paper is white at its peaks so its tooth may only carve,
this one is black in its hollows where no light reaches so its tooth may only LIFT, and its
relief is one-sided positive with a mean the tint already allows for.

Two things that round settled. **A threshold on a field is taken in the field's own
deviations, never in absolute value**: kraft's shives were first cut at a guessed absolute
level, which depends on how `_paper_field_band()` happens to normalise, and covered the
sheet in flecks that read as cork; just under three of the field's own deviations is the few
tenths of a percent a fleck should be. And **the laid ripple is the one feature whose period
is near the tile's own sampling, so it is the one that has to fade when it cannot be drawn**:
without that, the coarse tile (half a sample per unit, 1.5 per period) carried it at three
quarters of full amplitude into a 2.4-pixel period -- an alias, measured, not the wires. It
now fades below three samples per period and is gone below two. The residual case is
everyone's: between half zoom and full, the tile is built finer than the screen and cairo
shrinks it with `CAIRO_FILTER_BILINEAR`, which attenuates a fine structure rather than
filtering it, the same trade the moleskine's fibres and the embossed mesh already make.

Considered and left out: an artist's **canvas weave**, whose plain or duck weave the embossed
paper's mesh already is, at a different pitch; and **Mi-Teintes' honeycomb**, which is a
crisscross of the same machinery again. Both would have been a third and fourth mesh rather
than a new kind of structure.

These are random fields synthesised in the
frequency domain: white noise shaped by a radial filter (a plateau below a knee frequency, a
power-law fall-off above it) and transformed back with a small radix-2 FFT of our own. The
discrete transform is periodic by construction, so a sprite wraps without a seam. One sprite
repeated shows its period, and sprites sharing a border repeat that border, so six sprites
per paper are laid on a half-overlapping grid, each placement a random sprite in one of eight
orientations at a random phase, its centre jittered off the cell's, blended by
two-dimensional Hann windows, into a field six sprites (3072 units) wide that is itself
periodic: no seam and no border band.

**Those weights are normalised in quadrature, not linearly, and that is the whole of it.**
The sprites are independent draws of one process, so a weighted sum of them has variance
`sigma^2 * sum(w^2)`; dividing by `sum(w)` leaves `sigma * sqrt(sum(w^2)) / sum(w)`, which is
1 where a cell's Hann window stands alone and equal to one -- at the placement's own centre --
and 1/2 where four windows meet at a quarter each. That is a two-fold amplitude lattice at the
cell pitch, and it is exactly the lattice of window centres the jitter was supposed to hide:
jitter moves the lobes, it does not flatten them. Reported as "a repeated area with more high
frequencies than the rest", most striking on the kraft paper; measured over one sheet, the
local high-frequency RMS ran 3.12 to 6.50, a ratio of 2.08, with the strongest modulation at a
period of 533 px against a 512-unit cell. Dividing the deviations by `sqrt(sum(w^2))` gives
the same variance everywhere and still reproduces one sprite exactly wherever one window
stands alone: 2.08 becomes 1.28, the spread across the sheet 15.5% becomes 3.8%, and the
modulation leaves the cell pitch altogether. The deviations are taken about the sprites'
common mean and the mean is added back linearly, because a relief is not always zero-mean --
the watercolour's tooth only carves, the charcoal card's only lifts -- and it is only the
fluctuation about that mean whose size must not vary.

One consequence to keep in mind for any field read as a **coverage** rather than as a signed
relief: a quadrature blend overshoots both ends of [0, 1], so the psychedelic washi clamps its
ridge at the point of use. Left alone, a slightly negative coverage turned its subtraction
into a lift, two channels clipped to white against a spared third, and the paper between the
wrinkles picked up a faint wash of the complementary colour (measured: 18.1% of pixels with a
clipped channel, against 9.4% once clamped). The Moleskine carries short fibres in random directions over its clouds:
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
**The guides are drawn in the prepress palette, which is InDesign's and therefore every
print shop's template**: the page border is the **trim** and is black, the **bleed** red, the
**margin** violet. All three are solid lines -- on a dieline a cut is solid and a crease is
dashed, so the dash is reserved for the fold and means something. The **padding** is no
prepress object at all, being a layout aid rather than anything that reaches the press, so it
takes the one family the convention leaves free here, the blue of the slug. Every guide is
stroked twice, a white keyline under its own colour: the convention assumes a light
pasteboard and this plane can be a charcoal card or a hole, and a black trim on a black plane
is no guide. `canvas/trim_color` and the `canvas/guide_*_color` keys carry them; they were
renamed from `canvas/page_color` and friends precisely so the new defaults reach a
configuration that already holds the old ones.

The page guides are drawn UNDER the content by default and over it with
`DT_CANVAS_GUIDES_OVER`, which is what makes a frame deliberately crossing a page break
placeable against a trim line it is covering. The padding boxes are always over: they belong
to the frames, not to the sheet.

The grid dots have a colour of their own and a radius that is a fraction of the grid step,
so they scale with the zoom too, floored at three quarters of a pixel so they never vanish.

A canvas may be divided into **pages** -- ISO A0 to A6, US Letter, or one of the screen
formats a picture is made for (Instagram square and portrait, a story/reel/Short, a Facebook
post or cover, a YouTube thumbnail or channel banner), portrait or landscape -- tiled from
the origin and outlined with dashed lines in their own colour: one line per
border, never one rectangle per page -- a shared edge stroked twice with two dash phases
fills its own gaps and reads as solid -- each line starting on a multiple of the dash
period from the origin, so the dashes neither crawl under a pan nor differ between the
horizontal and the vertical. Page borders are a snapping rule of their own, applied after
the padding and before the size.

**One canvas unit is a POINT** -- a seventy-second of an inch, the typographer's own -- and
every length on the plane is one: a page's size, a frame's, a border's width, a text frame's
padding, and the size in a font's own description. `dt_canvas_resolution()` is then the density
the page is RASTERISED at and nothing else: it moves nothing on the plane and only decides how
many pixels an export carries, `points * dpi / 72`.

It was not always. A unit used to be a display pixel at that density, so a sheet of paper was
scaled by it and a screen format was not -- and since nothing ON the page was scaled with it,
raising the density shrank the whole layout against its own paper. Measured on A4: a
twelve-point line is 7.0% of the page's height at 72 dpi, 3.4% at 150 and 1.7% at 300, for the
same nominal twelve points. The number was usable only if it was chosen before anything was
laid out.

**A pixel is a physical length as soon as a density is named for it**, and the one to name is
the W3C's reference pixel, 96 to the inch (`DT_CANVAS_REFERENCE_PIXEL_DPI`), which is what
every browser and toolkit means by one. So a 1080 x 1920 story is 810 x 1440 points, and
exporting it at 96 dpi gives back exactly the 1080 x 1920 it is named for -- pinned end to end
in `test_canvas_export`. That is what lets a story and a sheet of A4 be the same kind of thing:
twelve points is twelve points on both, and converting a design from one to the other moves
nothing by itself. `dt_canvas_paper_is_physical()` no longer says how a size reaches the plane,
only how it is written down, so a panel can show one in points and the other in pixels.

**A point is what the document stores; it is not what the user has to type.** Every length field
in the atelier -- the properties' sizes and positions, the page margin, the bleed, the paddings --
takes `210mm`, `8.5in`, `21,5 cm`, `12"`, `1080px` or a bare number, converts it to points and
stores that. `src/common/length.{h,c}` is the whole of it: one table of units, GTK-free and
conf-free, with `dt_length_parse()` and `dt_length_format()` either side of it, and
`widgets/length_field.c` is the spin button that uses them. A pixel is the reference pixel again,
so `1080px` is 810 points and nothing else in the file knows a second answer.

Three rules the table and the field carry, each paid for once. The unit table is searched
**longest spelling first**, or `8.5inch` matches `in` and leaves `ch` behind as a parse error the
user cannot see the cause of. A field **remembers the unit it was last given**, per field, so a
margin can be set in millimetres while the bleed beside it is set in points and neither reformats
the other; the unit is a display property and never reaches the document. And a spin button's
`input` handler **must never return `GTK_INPUT_ERROR`**: GTK answers that by zeroing the value, so
a typo in a margin field silently wrote 0 rather than leaving 42 alone -- the handler returns the
current value instead, and the field snaps back to what the document holds. For the same family of
reason `dt_length_field_set_unit()` does not call `gtk_spin_button_set_digits()`, which emits
`value-changed`: choosing a unit became an undo step that wrote every field the handler could
read.

**What a design does NOT do is resize itself to a new page.** Twelve points stays twelve
points, which is what a point is for; making the layout fill a different page is a deliberate
act and belongs in an explicit, undoable action rather than in a rule that fires behind the
user. No page-layout application does it implicitly, because there is no correct implicit
answer.

Pango is pinned to the same unit: `pango_cairo_context_set_resolution(context, 72.0)` in
`_text_layout_styled()`. Pango means points by a font's size already but turns them into its
context's units at the context's own density, 96 unless told otherwise -- so "12" arrived on
the plane as sixteen units, a type size meant nothing measurable against a page, and it stayed
put while the page moved. Pinned, a line of N-point type is 1.1667 N units at every size,
which is the font's own line height and no scale factor hiding behind it.

One table holds every size, and the toolbar reads it rather than repeating it. **The stored
value is a code, not the row it is shown on**: a size is appended to `dt_canvas_paper_t` so no
saved document changes page, and the table's order is where the list shows it, which is how A0
and A1 came to sit above A2 while carrying the highest codes. `dt_canvas_paper_code()` turns a
row into the value to store and `dt_canvas_paper_position()` turns it back; only
`dt_canvas_paper_points()` speaks codes.

### Spreads, folds and the bind gutter

**A SPREAD is the block of pages that stays on one sheet**: `spread_cols` across by
`spread_rows` down. A book is 2 by 1, a zine folded both ways 2 by 2, a poster printed at home
and taped together as many as it takes. **Zero is a plane tiled uniformly** -- what every
document written before the fields holds, and exactly the geometry it was laid out with -- and
one is every page on its own sheet.

Inside a spread the pages are contiguous and the borders between them are **folds**, drawn
dashed, which on a dieline is what tells a crease from a cut. Between two spreads the plane
opens by **twice the bleed**, so each sheet carries its own all round and no two bleeds
overlap; that is the one thing the uniform tiling could never express, since there a page's
bleed reached into its neighbour. The trim is therefore the outline of the SHEET, not of the
page, and so is the bleed: a page in the middle of a spread has no bleed at its folds.

The **bind gutter** is the binding's own allowance, added inside a page AT A FOLD only -- what
a perfect binding swallows out of the middle of a picture crossing it. It is not the page
margin, which is uniform all round; it is the extra the fold side needs on top of it, which is
why `dt_canvas_page_margin_rect()` exists beside the symmetric `dt_canvas_page_guide_rect()`.

Three consequences a reader should expect. **The plane no longer tiles evenly**, so the page
under a point is asked for (`dt_canvas_page_at()`) rather than divided out, and it answers with
the page on the left for a point that falls in the gap between two sheets. **The page snapping
cannot use a period** either: it gathers the real lines the pages around each edge offer --
their borders, their margins with the bind gutter where it applies, and their sheet's bleed.
And **the export cuts at the FOLDS, one leaf per canvas page** -- a spread is how the plane is
laid out, not how the press prints, because the press prints leaves and the binder folds them.
What the fold gets instead is the **bind gutter, which behaves exactly as a bleed does**:
content carried past the cut line, only facing inward. The strip either side of a fold is
therefore printed on BOTH leaves, so the part of a picture the binding swallows is still there
on each. Nothing is scaled for it -- a frame lands exactly where the canvas shows it, and the
leaf simply comes out that much wider, the same way a bleed already makes it wider -- which is
the answer to the question the geometry poses: the page keeps its trim size and the raster
grows, rather than the content being squeezed into the trim and distorted. A fold is not cut,
so it takes no bleed; each of a leaf's four sides owes whichever of the two applies to it.

**A canvas with no page size exports as ONE page around everything on it**, grown by the
canvas's margin -- the margin has no page edge to sit inside there, so it becomes the white
space the single sheet keeps around its content -- and not one page per frame, which is what
the dialog's old wording said it did.

Two more guides ride on the pages, each with its own show, snap, size and colour, and each one
rectangle per page rather than a grid of shared lines: the **margin** inside every page edge,
which is a guide and a snapping rule and moves nothing; and the **bleed** outside it, which is
also what the export writes. Both are the document's -- `dt_canvas_page_guide_rect()` is the
page rectangle grown by a signed outset, negative for the margin and positive for the bleed --
so what the atelier shows is what comes out, and the export dialog does not ask again.

### A transparent plane

**Transparent** heads the background list, and a canvas set to it has no plane at all: the
compositor's float canvas is premultiplied RGBA already, so what is left uncovered simply
stays uncovered. On screen that is shown as a chequerboard of one grid step, in the two greys
every editor uses for the same thing, drawn into the base layer -- so the composite stays
opaque and nothing else in the painter changes. Zoomed out past three pixels a square the two
greys would average to one, and the lighter one alone is painted instead.

An export keeps the hole. The encode writes ARGB32 rather than RGB24, straight from the
premultiplied canvas, which is cairo's own convention; the band is blitted with
`CAIRO_OPERATOR_SOURCE`, since a hole laid *over* an opaque page would stop being one. The
writers then unpremultiply before the colour transform -- a profile is not linear in coverage,
and transforming a premultiplied value drags every edge towards black -- and put the coverage
back afterwards, LCMS having been asked for three channels. PNG becomes RGBA, TIFF gains an
`EXTRASAMPLE_UNASSALPHA` sample, and a PDF page is written as its colour plus a
`/DeviceGray` **soft mask** named in the image's own dictionary, which is the only way a PDF
carries coverage. Such a page stays a flate stream whatever quality was asked for: a lossy one
would blur the mask's own edges. **JPEG has no alpha channel**, so it is not offered for a
transparent canvas and refuses one if asked, rather than filling the holes with a colour
nobody chose.

Background styles are stored by value like the page sizes, so Transparent was appended to the
enum and shows at the head of the list through `dt_canvas_background_position()`.

### Exporting

`dt_canvas_export()` writes the canvas's pages as a PDF, a TIFF (both hold every page in the
one file) or a series of PNG or JPEG files numbered from the name given (`book_01.png`); a
single page keeps the name itself. **The page is the document's, never the exporter's**: a
canvas divided into pages gives one page per page that holds a frame, empty ones skipped, at
that size; a canvas without gives one page around every frame. Page size and orientation are
set in the atelier, so what was laid out is what comes out, and the dialog does not ask again.

**A page is rasterised at exactly the resolution asked for times its own physical size** --
an A3 page at 300 dpi is 4962 by 3508 pixels -- and the whole raster goes through LCMS into
the output profile, which is embedded. What used to make the file heavy was not the raster
but the stream: a lossless Flate stream over a page of photographs kept every code of a
picture that was already a lossy JPEG on the way in. A PDF page is therefore carried as a
`/DCTDecode` stream at the quality asked for (`dt_pdf_add_image_jpeg()`), which took a
six-page A3 book from 87 MB to 14.5 MB and halved the time; quality 100 keeps the lossless
stream for anyone who wants every code. PNG and TIFF are always lossless.

**The bleed** is the canvas's own, set in the atelier beside the page size and drawn there.
It grows every sheet on all four sides and grows the canvas rectangle with it, so a frame a
page break cut in two carries on into the bleed on both sheets. That is what a binding folds
around and a trim cuts into. **It is not a margin**: nothing is moved and no room is kept
clear, the sheet is simply larger than the page. The page's own margin is the guide that keeps
room clear, and it changes nothing about what is exported.

### Text frames

A text frame has a font, a text colour, a background that can be transparent, and a
horizontal (left, centred, right, justified) and vertical (top, middle, bottom) alignment,
all in its properties: font, size, colour and horizontal alignment on the strip, the rest in the
card's Character, Paragraph and Text box sections.

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
from its geotag; the frame's properties edit the place, the zoom and the provider, each
change fetching the tiles again. A map keeps its own ratio: it covers the frame, centred,
and is cropped by it rather than stretched, and a resized frame fetches again at its new
size so the crop it shows is at full detail.

### Waypoints

A connector may pass by one point, to go around other frames: the Waypoint toggle on the strip
of its properties adds it at the middle of the current route, so nothing moves until it is dragged; it is drawn
as a diamond on the selected connector. Straight routes bend at it, square routes reach it
with one elbow and leave it with another, cubic routes become two curves sharing a tangent
there. It took 20 more of the connector record's reserved bytes, again without a format bump.

### The navigation flower

Bottom right of the view, painted in screen space and hit-tested before anything on the
plane: four petals pan by a quarter of the view, the inner disc zooms in (upper half) and
out (lower half) about the view centre, the core fits the view to the canvas. It hovers
and is never printed.

### Colour management

Frames are Adobe RGB JPEGs with the profile embedded, and every colour the canvas draws --
borders, text, backgrounds, grid dots -- is put into that encoding by
`dt_canvas_render_color()`, so a border matches its picture. On screen the finished 8-bit
canvas goes through the module's prepared Adobe-RGB-to-display transform; an export keeps the
raster in Adobe RGB and converts the whole page to the chosen output profile with LCMS,
embedding that profile in the file it writes -- an `/ICCBased` colour space in a PDF, an
`iCCP` chunk in a PNG, an APP2 marker in a JPEG, the ICC tag in a TIFF; the intent is the
user's. Text and connectors are therefore pixels in an export, not vectors: a trade for having one painter and one colour path for the
screen and the print. The compositor above runs on both targets; only the last step differs.

## Instrumentation and performance

`dt_canvas_paint()` times its phases -- the base layer, the objects (cairo painting and
decoding, shadows), the encode -- and prints them under `-d perf` as one line per paint,
with the pixel count and the number of objects, layers and shadows;
`dt_canvas_paint_last_stats()` returns the same numbers for tuning.
`tests/unittests/bench_canvas_paint` paints a document (`CANVAS_BENCH_FILE`) at 2560x1440
and device scale 2 -- cold, cached, panned, at half quality, zoomed, for export -- and prints
those lines; `CANVAS_BENCH_WARM_LOOPS=n` repeats the warm frame for a profiler, since the
cold frame's paper and cutout synthesis otherwise dominates every sample. Profile the main
thread alone and with children (`perf report --tid <main> --children`): the OpenMP workers'
samples are mostly barrier spin, and what the wall clock waits for is whatever runs on one
core between the parallel regions.

On the maintainer's test document (14 frames, four of them cut, three shadowed, embossed
paper, 14.8 million pixels at 2x, 8 cores) a warm frame went from about 1.8 s to about
0.25 s, in this order of yield:

- **Cairo's scaled compositing was the serial bottleneck**, 40% of the main thread: every
  picture scaled onto its frame with the separable convolution `CAIRO_FILTER_GOOD` runs, and
  every cutout applied three times through `cairo_mask_surface()` with a scaled mask. Now a
  picture is rescaled once per size the screen shows (`dt_canvas_surface_cache_get_scaled()`,
  two sizes kept per frame, area-averaged when shrinking) and blitted pixel for pixel with
  `CAIRO_FILTER_NEAREST` under an identity matrix -- the sprite is a pixel larger than the
  box so the frame's clip, not the sprite's edge, ends the picture -- and a cutout is
  composited in float by the painter's own loop.
- **The encode goes to 8 bits before colour management**: the float XYZ-to-display
  conversion was 700-900 ms; the 8-bit Adobe-RGB-to-display transform is about 50.
- **Working buffers are kept between frames** (`dt_canvas_scratch_slot_t` in the surface
  cache: the float canvas, one 8-bit layer, one float layer, the shadow plane and its blur;
  the encoded frame and the one before it in the painter). A 60 to 240 MB allocation is
  returned to the kernel on free, so every frame paid its page faults again -- a cost no
  profile attributes to a symbol. One 8-bit layer serves the base and every object in turn.
- **A plain frame is decoded straight onto the canvas**, no float layer of its own; a partly
  covered pixel is unpremultiplied in 8 bits and read through the gamma table, where it used
  to take three `powf()` calls.
- **The same frame is not composited twice**: the encoded frame is kept with its key (the
  document's serial and generation, the display generation, the view matrix, the box, the
  quality) and blitted again on a hit, which is what a context menu or a hover costs -- 7 ms
  against a full paint. The key is the document's `serial`, never its address: a freed
  document's address is reused, its serial is not. The cache serves the atelier only (paints
  with a surface cache); an export, or a test editing the struct by hand between two paints,
  always composites.
- **Frames mid-gesture are composited at half the resolution** (`options.quality`; the view
  sets `interacting` on every pan, zoom, drag and flower press, a 180 ms idle brings the full
  frame) and scaled up bilinearly: a quarter of the pixels, about 100 ms. Such a frame asks
  for the cutout rasters at the FULL frame's size (`_mask_geometry()` divides by the quality)
  and cairo scales them down, so the gesture's first frame does not rasterise every cutout
  again; the mask cache keeps two rasters per frame for the same reason.
- **The cutout rasteriser is parallel** (the clip, the downsample, the alpha surface, the
  distance transform's two passes with per-thread scratch), which is what the first full
  frame after a zoom step that crosses a power of two pays.

Still on the table, for the record: the base layer's round trip through 8 bits (about 60 ms:
cairo's paper blit, the decode, the dither), the 8-bit display transform (about 50 ms), and
the cutout re-rasterisation at each power of two (about 100 ms per cut frame on 8 cores).

## The view

The toolbar (`libs/tools/canvas_toolbar.c`) reads left to right: the three flat menus --
**Canvas** (new, open, save, save as, export as PDF), **Object** (check against the library,
refresh the stale images and notes, refresh every image) and **Guides** (a popover of seven groups:
the grid, the page borders with the export DPI and whether they are drawn over the content, the
spread with its bind gutter, the page margins, the bleed, the paddings, and snapping sizes to
neighbours -- each guide with the show, snap, size and colour it has), each ending in an ellipsis -- then
two linked groups of glyph buttons, then **Background** (style, colour, Texture), the frame
defaults (Borders, with the corners, and Shadows: what every frame inherits until its properties
say otherwise), Fit and 1:1, and **Arrange** (the layout, a "Sort by" like the lighttable's --
canvas order, filename, captured, id, full path -- and Auto to apply). The sort is a
`dt_canvas_sort_t` handed to `dt_canvas_layout_apply()`: images compare on the key, then on
their draw order, and frames that are not images follow in draw order.

**The two groups offer an object as a picture of it.** One PLACES an object -- text, notes, map,
drawing -- and one ARMS a tool to draw one -- connector, line, curve, rectangle, polygon, star.
Each icon shows what it makes, so the "Add", "Frames" and "Zoom" captions that used to tell the
words under them apart are gone; what a picture cannot say, the gesture and the key, is in the
tooltip, and the user's own binding is appended to it at hover time from the accel path each
button carries. Measured against the same toolbar built of words, it asks for 1511 px where that
one asked for 1747 -- 236 px and 13.5% narrower. **Fit stays a word**: the one picture the toolkit has for zooming is a
magnifying glass, which says "zoom" over an action that means "fit", beside a "1:1" that is also
a zoom.

**A glyph draws in the unit square it is handed, and everything it draws must fit there.**
`widgets/paint.c` gives each icon a context already scaled to a one-by-one box, and the same code
answers at 16 px and at 32 px, at device scale 1 and 2. Two of these icons say what their button
adds, and each had to solve that differently.

A map frame's icon is the teardrop every map application draws, with the eye cut out
**even-odd** rather than filled again in the ground's colour -- a second fill would be right on
one theme and wrong on the other, and these buttons sit on both. Its tail is the pair of
TANGENTS from the tip to the head, so the outline turns smoothly into the circle instead of
meeting it at a corner. The map VIEW's own `..._map_pin()` is a different picture for a different
job -- the callout tail under a thumbnail, correctly a triangle, since it points at a place
already on screen -- and is untouched.

A drawing has no picture of itself: it is whatever its author drew. So its icon sets the
format's name, the three letters a file manager and a browser both put on it. **Text in a glyph
is measured at size 1 and fitted to the box afterwards**, never set at a size chosen in advance,
so the icon does not depend on which sans-serif the system resolves; and the measuring is done
with **metrics hinting OFF**, because cairo quantises a hinted glyph's metrics to whole device
pixels under the transform in force -- so the extents solved for would not be the metrics the
glyphs are then hinted to under the fit computed from them. Measured, the same string came back
2.250 wide in an 8 px box, 2.188 at 16, 2.150 at 20 and 2.208 at 24, against 2.169 unhinted: a
different fit at every size, which cost 3.4% of the ink off the edge at 20 px and 10.7% at 8.
The canvas's own text is laid out with metrics hinting off for exactly the same reason. The ink's
own box is what gets centred, not the logical one, whose ascent no letter here reaches.

**The tool toggles are a VIEW on the armed tool and keep no state.** `tool_toggles[]` is indexed
by the tool each toggle arms, its action lives in the button's object data, the handler asks the
view only when the button and the view disagree, and `_refill()` presses the one `armed_tool`
names with every handler blocked by stored id. Exclusivity lives in the view. Both halves are
load-bearing and each covers the other, so neither may be simplified away because the tests pass
without it: dropping both at once does not merely miscount, it recurses until the process dies.
And **a toolbar button gives the keyboard straight back** (`focus_on_click` FALSE, as the
properties strip already does): GtkWindow offers a key to the focus widget and to its own
move-focus bindings before the application's handler sees it, so a focused toggle answered Space
and ate the arrow keys the plane nudges with.

**A popover keeps the keyboard for its own controls, which takes three things and not one.**
`dt_accels_block_plain_keys_inside()` (`_popover_around()`) stops the single letters and digits the
view binds as shortcuts, and only those: Delete, KP_Delete and BackSpace the view reads from the
main window itself, so a popover swallows those three from a handler connected AFTER its own --
after, so the focused control is offered the key first and a spin button still edits its number
with both. A popover also has to name the control that takes the focus on opening, since a bauhaus
slider answers GTK's focus walk without taking the focus and a popover full of them opened holding
none, leaving every key to act on the plane behind it. And a popover's handler compares its value
with the document before writing, or one drag is announced once per motion event.

`src/views/canvas.c` owns one document and everything about editing it. It registers the
`canvas` accelerator group, exposes its actions through `proxy.canvas` for the toolbar
(`libs/tools/canvas_toolbar.c`), and raises `DT_SIGNAL_CANVAS_CHANGED` whenever the
document is replaced, saved or reconfigured, so the toolbar refills every control that mirrors a
document setting, and so do the floating properties. **The toolbar blocks every handler a refill
could wake, by its stored id** (`_connect_refilled()` is the only way such a handler is
connected), rather than raising a flag the handlers check: a handler that reads SEVERAL controls
-- the margin with the bleed, the shadow's three sliders -- woken halfway through a refill sends the
ones not refilled yet into the document. Measured offscreen with the blocking removed: 9 writes
during one refill, one of them a shadow radius of -500, the hard minimum of that row in the
property table. The margin and bleed controls were never refilled at all until this was checked, so
the first edit of the bleed after a restart wrote the margin's GTK default of 0 over the document's.

The Borders and Shadows popovers hold bauhaus sliders, and a dragged control asks for two things a
spin button did not. What it SENDS is one value per motion event, so the six numbers they edit --
the frames' default border width and corner radius, the default shadow's two offsets, its blur and
its extent --
have no plain setter in `proxy.canvas` at all: `set_border`, `set_shadow` and `set_corner_radius`
were withdrawn from the proxy and are now static to the view, and the only way in is the
phase-aware `proxy.canvas.edit_number()`, taking the `dt_canvas_prop_id_t` and a phase the way
`edit_color()` takes a colour target. A LIVE step writes the field, touches the document and raises
`DT_SIGNAL_CANVAS_CHANGED` -- no configuration write, no undo record -- and the COMMIT that ends the
gesture puts the number it found back first, then hands the kept one to the setting's own setter, so
that setter's undo step spans the whole gesture. Measured, a 40-position drag: 39 undo steps through
the old per-call setter, every one before the button came up; 1 through `edit_number()`, on the
release. A gesture nothing holds -- a wheel step, an arrow key, the fine-tune popup -- ends on a
400 ms debounce instead, and a click that did not drag waits out `gtk-double-click-time` so that the
double click which resets a slider is one undo step and not two (measured: 2 against 1).

What a slider is GIVEN is the other half, and here blocking the handler is NOT enough. A refill
must leave alone any slider that already shows what the document holds, because
`dt_bauhaus_slider_set()` rewrites the display range around the value it is handed: a slider showing
a number past its soft end has that range collapse onto the value under the pointer. Measured on a
document holding a 300 pt shadow blur against a soft maximum of 100, pressed at half the bar and
then eight motion events at the same x: 127.30 then 26.00 with the refill writing back, 127.30
throughout without it. Mid-gesture the document holds exactly what the slider shows, so that test is
precisely the slider being dragged; a `pressed` flag covers a refill raised from elsewhere -- an
undo, a document opened -- while a button is down. The texture sliders need none of this only
because `set_texture()` raises no signal, so no refill ever runs under them.

The toolbar's colours go through `proxy.canvas.edit_color()` rather than their setters while their
window is open. A LIVE change that changes the field writes it and touches the document -- no
configuration, no undo record -- and remembers the colour the window found; CANCEL puts it back,
leaving the document as saved as it was when nothing else touched it meanwhile. The default border
and shadow raise `DT_SIGNAL_CANVAS_CHANGED` on each LIVE change and on the CANCEL, as their setters
do: the properties of a frame inheriting them show them, and would go on showing the old colour
while every inheriting frame is drawn in the new one. COMMIT
puts it back FIRST and then calls the colour's own setter with the kept colour, so whatever that
setter records and announces spans the whole visit: the default border and shadow keep their one
undo step each (undoing to the colour before the window, not to a live one), the guide colours and
the background still record none, and a colour kept equal to the one found records nothing. The
background's window offers no opacity: a transparent canvas is one of its styles.

The cursor names the action under the pointer: a hand over a frame or a connector, a corner
cursor over a scale handle (turned with the frame), the exchange cursor over the rotation
handle, a crosshair over an anchor while the connector tool is armed and anywhere else a drawing
tool would draw, a hand over the flower, a cross-arrows cursor over a waypoint and while moving.

Gestures: drag a frame to move it (the whole selection follows; snapping puts it next to a
neighbour two paddings away, in line with a neighbour, or on the grid), drag a corner handle to scale it around the
opposite corner keeping its aspect ratio, drag the handle above it to rotate (Shift snaps to
15°), drag on empty space for a rubber band, middle button or Alt-drag to pan, wheel to
zoom about the pointer, Shift-wheel to pan sideways. A single click, a drag or a rubber band
never shows an object's properties: a double-click does (so do I and the context menu's
"Properties"), and a double-click on an object whose properties are already showing goes
into it -- a text frame's editor, an image in the darkroom, a drawing's file read again.
Return does the same from the keyboard. What was on screen before the double click's own first
press decides, and properties closed since do not count; a run of clicks -- presses of one
button, each within the toolkit's double-click delay and distance of the previous one, the
conditions GDK pairs presses with -- answers one double click at most, so a burst of four or
five fast clicks opens the properties and never drills. The second double click, the one that
drills, has to begin at least twice the delay after the first one began: GDK reports a press
sooner than that as a triple click, and the press after it as a first press, so no double click
reaches the view at all. The opening and the content action run from an idle, after the press,
and a press handled before that idle -- or Escape -- takes them back. With Shift or Ctrl held a double click is two
selection toggles and opens nothing. A handle takes a double click only when the first press
took it too, since that press can only take the handles of what was already selected. The properties
hide while a gesture moves things and come back when it settles; Escape closes them before
it drops the selection. Right-click opens the context menu for what is under the pointer -- and, with a tool armed, puts
the tool away instead.
Every edit is one undo record (`DT_UNDO_CANVAS`), a snapshot of the document before and
after: objects are small and JPEG bytes are shared by reference, so a snapshot costs the
records, not the pixels. A drag records its undo on release, and Escape mid-drag restores
the pre-press snapshot.

Drops from the filmstrip arrive on the centre widget as the `image-id` target (the same
payload the map view reads), with `GDK_ACTION_MOVE` -- the only action the filmstrip offers,
so the destination must accept it or GTK refuses every drop without a word: each id becomes
an image frame at the drop point, staggered so a multi-drop is not one pile, and a render is
started for each.

The **notes** button on the toolbar (or Shift+T) adds, under each selected image frame -- every
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

- Text, connectors and shapes are rasterised in the PDF, as everything else on the page is. A
  vector export would need a second painter or a cairo PDF surface with its own colour path.
- Sidecar text frames are refreshed on "Refresh", not watched.
- The image render is one size per canvas (`image_long_edge`), chosen when the canvas is
  created; changing it takes a "Refresh all".
- The bleed is uniform on all four sides; a binding usually wants more on the spine.
