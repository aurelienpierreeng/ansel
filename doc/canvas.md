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

The four layouts leave two paddings between frames -- each keeps one all round -- and, with
snapping on, start on the grid and round every cell up to whole grid steps, frames sitting
top-left in their cell.

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
`DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE`, from its bar -- the same shape as the borders, and
`dt_canvas_object_effective_shadow()` resolves it the same way. Connectors get shadows too.
The shadow is derived from the object's alpha after its cutout, its border and its opacity,
so it starts at the solid border, a feathered frame casts a feathered shadow and a translucent
one a fainter one.

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

The view edits a cutout with handles over the frame when the bar's Edit toggle is on: the
centre or anchor, the radius or radii (the ellipse's first radius handle also sets its
rotation), the feather on the circle's or the ellipse's dashed ring, the gradient's reach
across its line, and the polygon's nodes.

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
so the two cannot drift. The context menu offers the shapes, editing, inverting, and the node
actions for the node or edge under the pointer: switch the node's kind, give a steered one its
computed curve back, remove it, or add one on the edge.

**The menu asks its own question of the geometry, not the drag's.** `_mask_handle_at()`
answers what a drag would grab and refuses everything while the shape is not being edited,
which is right for a drag and wrong for a menu: for as long as the cutout submenu keyed its
node entries on it, they were a duplicate of the top-level ones whenever the shape was being
edited and unreachable the rest of the time. `_mask_node_at()` is the geometric question, and
the submenu offers the way into the edit mode instead. Every drag and
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

**Auto height** makes a frame take the height its text needs, applied once per frame in the
view's expose rather than at every edit: the height depends on the laid-out text and the text
depends on everything that can change it. Only a height that actually moved touches the
canvas, so it settles on the first frame instead of handing the painter a new generation for
ever.

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
is behind the text -- and what each covers is its SILHOUETTE, `dt_canvas_object_covers()`
answering through `dt_canvas_object_silhouette_reach()`, so a circular cutout pushes the text
along its curve and leaves the empty corner beside it usable. They are baked into a coarse
occupancy map in the frame's own local coordinates, three units to a cell, and a line asks it
for the widest clear run across the band it is about to occupy. ONE run per line,
deliberately: a line split either side of something standing in the middle of a column is a
different feature, and this is the choice a page-layout application offers as "the largest
area".

Three things about that engine that are not obvious. **Justification comes out right for
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

### Connectors

A connector joins two frames at **anchors**: nine per frame -- the four edge midpoints, the
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
is the triangle's alone and stays sharp. All of it is in the connector's floating bar, with
dashes, colour and width.

**Ctrl while dragging a handle locks it.** A handle free to go anywhere -- a cutout's centre,
radius, feather or node, a connector's waypoint -- keeps to one axis, the one it has
travelled furthest along since the press, so the user chooses which by moving. A handle that
sets a direction rather than a place -- a connector's tangents, the gradient its curve leaves
by -- snaps that direction to 45 degree steps about the point it turns around, keeping how
far out it was pulled; the axes are among those steps, so it is the same lock said in the
terms an angle has. A frame's rotation reads it the same way, 45 degree steps, where Shift
reads 15. A handle already confined to a line, like a connector's reach along its anchor's
normal, has nothing to lock.

Connectors are drawn from the toolbar's **Connect** button (or C): in that mode the frame
under the pointer shows its four cardinal anchor dots, the first click picks the source
anchor, the second the target anchor -- the user chooses the anchors, nothing is resolved
automatically -- and the mode ends with the connector selected. Escape or a right click
leaves it.

Selection handles, hover outlines, the rubber band, the connector being drawn, the status
line and the navigation flower are the view's and are painted after the document. The status
line is inked dark or light against the plane's luminance, with a halo of the opposite, so
it reads on any background colour or paper and over a picture.

**Every overlay line carries its own opposite**, for the same reason and by the same trick
seen three ways: the status line's halo; the selected frame's solid light rectangle under a
dashed dark one; and the hovered frame's two adjacent hairlines, light against the frame and
dark just outside it. A single pale line is legible on a dark plane and gone on a bright one,
and a canvas is as often one as the other. The hover pair sits wholly past the frame's edge,
so it never covers what it is outlining.

### The floating property bar

The bar is one vertical box of rows, one per topic, so the bars of two kinds differ only by
their first row: the kind's own properties (font, alignment and colour; route, arrows and
waypoint; place, zoom and provider), then **Geometry** (centre, size, angle), **Opacity**
with the background colour, **Frame** (border width, corner radius, colour -- a connector's
is its **Line**: width, dashes, colour), **Shadow** (the two offsets, the blur, the colour),
and **Cutout**. Rows a kind has no use for are hidden at refill; every row keeps a
70-pixel topic label so the controls line up from row to row, and where a row carries a
colour it is the last control on it.

**A property with a canvas-wide default has no toggle: the value is the switch.** The border
width, the corner radius and the shadow's blur read `default` at -1, which is the object
inheriting the canvas's; any other value is the object's own. Leaving the sentinel seeds the
object with the property that was on screen -- the colour and the offsets come across with
it -- so an edit starts from what the user was looking at rather than from zero. It is the
same shape as the shadow's radius, which has always been its own on/off at 0, and it is why
there is no "Canvas default" button anywhere on the bar. **Nor is there a "Transparent"
button**: every colour on the bar is an alpha-capable picker, so an alpha of zero is how a
background, a border or a text is made to disappear.

While a cutout is being edited, the context menu opens on the shape's properties as sliders
-- feather, opacity, size, rotation, extent and curvature, whichever the shape has -- the way
the darkroom's mask menu does: a scale inside a menu item, the item's pointer events
forwarded to it, its activation blocked so the menu stays open, and one undo record for the
whole menu taken when it opens and written when it closes if anything moved.

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
padding, border) stay in the toolbar.

### Borders, the padding, and snapping to neighbours

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
dashed, so the dash is reserved for the fold and means something. The **gutter** is no
prepress object at all, being a layout aid rather than anything that reaches the press, so it
takes the one family the convention leaves free here, the blue of the slug. Every guide is
stroked twice, a white keyline under its own colour: the convention assumes a light
pasteboard and this plane can be a charcoal card or a hole, and a black trim on a black plane
is no guide. `canvas/trim_color` and the `canvas/guide_*_color` keys carry them; they were
renamed from `canvas/page_color` and friends precisely so the new defaults reach a
configuration that already holds the old ones.

The page guides are drawn UNDER the content by default and over it with
`DT_CANVAS_GUIDES_OVER`, which is what makes a frame deliberately crossing a page break
placeable against a trim line it is covering. The gutter boxes are always over: they belong
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
the gutter and before the size.

**One canvas unit is a display pixel, and the canvas says how many go to the inch**
(`dt_canvas_t.resolution`, `dt_canvas_resolution()`, 300 on a new canvas). That is what tells
the two kinds of page size apart. A **sheet of paper** is held in points and scaled by the
resolution, so an A4 is 2480 units wide at 300; a **screen format** is its pixel size outright
and does not move, so a story page is 1080 by 1920 units whatever the resolution says. Read as
points, as both were, a story came out 1080 units against an A4's 595 -- nearly twice the
sheet, for something that fits in a hand, which is the defect this field exists to fix.
`dt_canvas_paper_is_physical()` answers which kind a size is.

A document written before the field holds zero, which `dt_canvas_resolution()` reads as **72**:
one unit to the point, exactly the geometry it was laid out with. The export converts through
the same number -- the page's physical size is its units divided by the resolution, and the
output pixel count is `units * export_dpi / resolution`, so exporting at the canvas's own
resolution is one output pixel per unit and asking for more resamples.

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

The toolbar (`libs/tools/canvas_toolbar.c`) reads left to right as labelled groups: the
three flat menus (Canvas, Object, Guides, each ending in an ellipsis), **Add** (Text, Notes,
Map, Connector), **Background** (style, colour, Texture), **Frames** (Borders, Shadows),
**Zoom** (Fit, 1:1) and **Arrange** (the layout, a "Sort by" like the lighttable's --
canvas order, filename, captured, id, full path -- and Auto to apply). The sort is a
`dt_canvas_sort_t` handed to `dt_canvas_layout_apply()`: images compare on the key, then on
their draw order, and frames that are not images follow in draw order. Historically: **Canvas**
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
- The bleed is uniform on all four sides; a binding usually wants more on the spine.
