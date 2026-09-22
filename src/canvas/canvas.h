/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_CANVAS_CANVAS_H
#define DT_CANVAS_CANVAS_H

/**
 * @file canvas.h
 * @brief The Canvas document: an infinite plane of image frames, text frames and connectors.
 *
 * @details A canvas is what the Canvas atelier edits and what a `.anselcanvas` file holds:
 * a ZIP archive (see canvas_zip.h) carrying one binary index, one sRGB JPEG per image
 * frame and one Markdown file per text frame. It is self-contained on purpose. Opening it
 * needs neither the library database nor the original raws, so a canvas travels to
 * people who have neither; and every image frame records enough about its source (library
 * id, version, folder and file name, history hash, EXIF) that the same library can find the
 * original again and refresh the render when the development has moved on.
 *
 * Nothing about a canvas is written to the library database.
 *
 * Every record that reaches the disk carries `reserved` bytes. They are written as zeros,
 * read back verbatim and kept in memory, so a later version can claim them for a new field
 * without bumping the format or migrating anything; and each record is prefixed with its
 * own size, so a reader skips fields it does not know. See canvas_format.c for the layout.
 *
 * Coordinates are "canvas units", and ONE UNIT IS ONE POINT -- a seventy-second of an inch,
 * the typographer's own. Every length on the plane is that: a page's size, a frame's, a
 * border's width, a text frame's padding, and the size in a font's own description. So twelve
 * points is twelve points on A4, on A3 and on an Instagram story alike, and the density an
 * export is rasterised at (`dt_canvas_resolution()`) changes none of it -- it only decides how
 * many pixels come out. A format named in PIXELS is a physical size too, at the W3C's
 * reference density (`DT_CANVAS_REFERENCE_PIXEL_DPI`), so the two kinds of page are the same
 * kind of thing and converting a design between them moves nothing by itself.
 *
 * At zoom 1 a unit is one screen pixel, which is what makes a point-measured plane legible on
 * a screen. The origin is the centre of the plane, y grows downwards, and an object's `x`/`y`
 * is the centre of its frame. Rotation is in radians, clockwise on screen.
 *
 * Threading: a `dt_canvas_t` belongs to the GUI thread. Background renders never touch it;
 * they hand their JPEG back through a GUI-thread callback that checks the canvas is still
 * the one the render was started for (see canvas_render.h).
 */

#include "common/paths.h"

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** File extension of a saved canvas, dot included. */
#define DT_CANVAS_FILE_EXTENSION ".anselcanvas"

/** Current index format. Bumped only when a record's existing fields change meaning. */
#define DT_CANVAS_FORMAT_VERSION 1u

/** Length of the fixed text fields. */
#define DT_CANVAS_TITLE_LEN 256
#define DT_CANVAS_FONT_LEN 256
#define DT_CANVAS_EXIF_MAKER_LEN 64
#define DT_CANVAS_EXIF_MODEL_LEN 64
#define DT_CANVAS_EXIF_LENS_LEN 128

/** Reserved bytes per record, see the file comment. */
#define DT_CANVAS_HEADER_RESERVED 824 ///< 1024 at format 1, minus the padding (4), background style (4), grid colour (16), paper (8), page colour (16), shadow (28), padding colour (16), texture (16), corners (4), page margin (20), page bleed (20), resolution (4), spread (12), the line (20), the custom page (8), the shadow's extent (4)
#define DT_CANVAS_OBJECT_RESERVED 164 ///< 256 at format 1, minus the shadow (28), the transparency (4), the cutout mask (36), the background (16), the corners (4), the shadow's extent (4)
#define DT_CANVAS_IMAGE_RESERVED 508 ///< 512 at format 1, minus the render's colour space (4)
#define DT_CANVAS_TEXT_RESERVED 112 ///< 256 at format 1, minus the two alignments, the line height and the tracking, the four margins, the features, the flags, the standoff, the two paragraph settings, the glyphs' own shadow (28) and its extent (4)
/**
 * An OpenType feature string, as Pango spells it: "liga 1, onum 1".
 *
 * 256 in memory, but only the first whole tags of it reach the record's fixed field, which is
 * 64 bytes and cannot move; the whole string travels as a tagged chunk beside it. A rich face
 * ships tens of features -- Linux Libertine, 32 of them -- and 64 bytes holds eight: a
 * document with seven set silently refused the ninth, which read as the feature checkboxes
 * having stopped working.
 */
#define DT_CANVAS_TEXT_FEATURES_LEN 256
#define DT_CANVAS_TEXT_FEATURES_FIELD 64 ///< what the fixed record field holds, for an older reader

/** Which side of a text frame's inner margins an index names. */
enum
{
  DT_CANVAS_TEXT_MARGIN_TOP = 0,
  DT_CANVAS_TEXT_MARGIN_RIGHT = 1,
  DT_CANVAS_TEXT_MARGIN_BOTTOM = 2,
  DT_CANVAS_TEXT_MARGIN_LEFT = 3,
};

/** The inner margin a new text frame is born with, in canvas units: what an untouched inset reads. */
#define DT_CANVAS_TEXT_DEFAULT_PADDING 12.0f

typedef enum dt_canvas_text_flag_t
{
  DT_CANVAS_TEXT_AUTO_HEIGHT = 1 << 0,     ///< the frame's height follows its content
  DT_CANVAS_TEXT_OPTICAL_MARGINS = 1 << 1, ///< punctuation hangs into the margin so the edge reads straight
  DT_CANVAS_TEXT_WRAP_AROUND = 1 << 2,     ///< the text flows around the frames laid over it
} dt_canvas_text_flag_t;
#define DT_CANVAS_MAP_RESERVED 256
#define DT_CANVAS_SVG_RESERVED 480 ///< 512 at format 1, minus the intrinsic size (8) and the load time (8)
#define DT_CANVAS_CONNECTOR_RESERVED 24 ///< 128 at format 1, minus the anchors and routing (12), the waypoint (20), the handles (24), the free ends (48)
#define DT_CANVAS_SHAPE_RESERVED 108 ///< 128 at birth, minus the geometry, sides, depth, roundness and phase (4 each)

/** The colour space a stored JPEG is encoded in. A file from before the field says 0: sRGB. */
typedef enum dt_canvas_colorspace_t
{
  DT_CANVAS_COLORSPACE_SRGB = 0,
  DT_CANVAS_COLORSPACE_ADOBERGB = 1, ///< what the renders leave the pipeline in
} dt_canvas_colorspace_t;

/** An sRGB colour with straight alpha, each channel in [0, 1]. */
typedef struct dt_canvas_color_t
{
  float red;
  float green;
  float blue;
  float alpha;
} dt_canvas_color_t;

typedef enum dt_canvas_object_kind_t
{
  DT_CANVAS_OBJECT_NONE = 0,
  DT_CANVAS_OBJECT_IMAGE = 1,
  DT_CANVAS_OBJECT_TEXT = 2,
  DT_CANVAS_OBJECT_CONNECTOR = 3,
  DT_CANVAS_OBJECT_MAP = 4,
  DT_CANVAS_OBJECT_SVG = 5,
  DT_CANVAS_OBJECT_SHAPE = 6, ///< a drawn shape: a rectangle, a regular polygon or a star
  /**
   * How many kinds this build knows. RUNTIME ONLY, never stored: a file holds the kind's own
   * value and nothing else, so an unknown one arrives from a newer build, is kept whole and
   * draws nothing. It is here so an array indexed by kind cannot be sized by hand and then
   * forgotten when a kind is appended.
   */
  DT_CANVAS_OBJECT_KIND_COUNT = 7,
} dt_canvas_object_kind_t;

typedef enum dt_canvas_object_flags_t
{
  DT_CANVAS_OBJECT_FLAG_NONE = 0,
  /** The object's own `border_color`/`border_width` apply instead of the canvas defaults. */
  DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE = 1 << 0,
  /** The object cannot be moved, scaled or rotated from the canvas. */
  DT_CANVAS_OBJECT_FLAG_LOCKED = 1 << 1,
  /** The object is kept but not drawn. */
  DT_CANVAS_OBJECT_FLAG_HIDDEN = 1 << 2,
  /** The object's own `shadow` applies instead of the canvas default. */
  DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE = 1 << 3,
  /** The object's own `corner_radius` applies instead of the canvas default. */
  DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE = 1 << 4,
  /**
   * The frame may be resized without keeping its proportions.
   *
   * Stated the free way round so that ZERO is the careful answer: a picture and a drawing both
   * keep their shape unless they are told not to, which is what a photograph always did and
   * what a logo needs even more -- a stretched drawing is almost always a mistake, and the one
   * time it is not, this says so. It means nothing to a text frame, which has no proportions
   * to keep.
   */
  DT_CANVAS_OBJECT_FLAG_FREE_RATIO = 1 << 5,
} dt_canvas_object_flags_t;

typedef enum dt_canvas_grid_flags_t
{
  DT_CANVAS_GRID_NONE = 0,
  DT_CANVAS_GRID_VISIBLE = 1 << 0,
  DT_CANVAS_GRID_SNAP = 1 << 1,     ///< positions and sizes round to the grid
  DT_CANVAS_SNAP_PADDING = 1 << 2,   ///< edges land one padding from a neighbour, or in line with it
  DT_CANVAS_SNAP_SIZE = 1 << 3,     ///< a resized frame takes a neighbour's width or height
  DT_CANVAS_PAGE_VISIBLE = 1 << 4,  ///< the page borders are drawn
  DT_CANVAS_SNAP_PAGE = 1 << 5,     ///< edges land on a page border
  DT_CANVAS_PADDING_VISIBLE = 1 << 6, ///< a frame one padding out is drawn around every frame
  DT_CANVAS_MARGIN_VISIBLE = 1 << 7, ///< the page's inner margin is drawn
  DT_CANVAS_SNAP_MARGIN = 1 << 8,    ///< edges land on it
  DT_CANVAS_BLEED_VISIBLE = 1 << 9,  ///< the sheet's bleed, outside the page, is drawn
  DT_CANVAS_SNAP_BLEED = 1 << 10,    ///< edges land on it
  DT_CANVAS_GUIDES_OVER = 1 << 11,   ///< the page guides are drawn over the content, not under it
  DT_CANVAS_SNAP_ALL = DT_CANVAS_GRID_SNAP | DT_CANVAS_SNAP_PADDING | DT_CANVAS_SNAP_SIZE | DT_CANVAS_SNAP_PAGE
                       | DT_CANVAS_SNAP_MARGIN | DT_CANVAS_SNAP_BLEED,
} dt_canvas_grid_flags_t;

/** Which edges of a moving box may snap: all four for a move, the dragged ones for a resize. */
typedef enum dt_canvas_edges_t
{
  DT_CANVAS_EDGE_LEFT = 1 << 0,
  DT_CANVAS_EDGE_RIGHT = 1 << 1,
  DT_CANVAS_EDGE_TOP = 1 << 2,
  DT_CANVAS_EDGE_BOTTOM = 1 << 3,
  DT_CANVAS_EDGE_ALL = 0xF,
} dt_canvas_edges_t;

/**
 * A shadow: the object's silhouette, grown, blurred, offset and tinted. The radius is the blur's
 * standard deviation and its sign says where the shadow falls: positive drops it outside the
 * object, negative casts it inside along the object's edges, and zero is no shadow at all.
 * The EXTENT grows the silhouette before the blur -- outward for a dropped shadow, inward for an
 * inset one -- so the colour keeps its full density that much further and a wide blur softens a
 * shadow instead of washing it away; on its own it switches nothing on (a text's glyph shadow
 * aside, see dt_canvas_text_shadow_visible()). The colour's alpha is the strength. Offsets, extent
 * and radius are canvas units.
 */
typedef struct dt_canvas_shadow_t
{
  dt_canvas_color_t color;
  float offset_x;
  float offset_y;
  float blur;   ///< the signed radius; see above
  float extent; ///< how far the silhouette is grown before the blur, 0 to DT_CANVAS_SHADOW_EXTENT_MAX
} dt_canvas_shadow_t;

#define DT_CANVAS_SHADOW_EXTENT_MAX 500.0f ///< the most a shadow's extent is held to, in canvas units

/** How an image frame's render relates to the library. Runtime only, never saved. */
typedef enum dt_canvas_sync_status_t
{
  DT_CANVAS_SYNC_UNKNOWN = 0,   ///< not checked yet
  DT_CANVAS_SYNC_CURRENT = 1,   ///< the library holds the image with the same history hash
  DT_CANVAS_SYNC_STALE = 2,     ///< the library holds the image with a different history hash
  DT_CANVAS_SYNC_MISSING = 3,   ///< no library image matches
  DT_CANVAS_SYNC_RENDERING = 4, ///< a render job is running for this frame
} dt_canvas_sync_status_t;

typedef struct dt_canvas_image_t
{
  int32_t imgid;           ///< library id of the source at render time
  int32_t version;         ///< duplicate version of the source
  int32_t film_id;         ///< film roll id of the source at render time
  uint64_t history_hash;   ///< the source's history hash at render time
  int64_t rendered_at;     ///< unix time of the render, 0 when never rendered
  int32_t pixel_width;     ///< the JPEG's dimensions, 0 when there is no JPEG yet
  int32_t pixel_height;
  int32_t source_width;    ///< the source's own dimensions, as the library reports them
  int32_t source_height;
  int32_t orientation;     ///< the source's dt_image_orientation_t
  char folder[DT_PATH_MAX];               ///< the film roll folder
  char filename[DT_MAX_FILENAME_LEN];     ///< the file name inside it
  char exif_maker[DT_CANVAS_EXIF_MAKER_LEN];
  char exif_model[DT_CANVAS_EXIF_MODEL_LEN];
  char exif_lens[DT_CANVAS_EXIF_LENS_LEN];
  float exif_exposure;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  float exif_exposure_bias;
  int64_t exif_datetime_taken; ///< GTimeSpan, microseconds since the epoch
  uint32_t colorspace;     ///< dt_canvas_colorspace_t of the JPEG
  uint8_t reserved[DT_CANVAS_IMAGE_RESERVED];

  /* runtime, not serialised as fields: the JPEG travels as its own archive entry */
  GBytes *jpeg;                        ///< the sRGB JPEG, NULL until rendered
  dt_canvas_sync_status_t sync_status;
} dt_canvas_image_t;

typedef enum dt_canvas_text_source_t
{
  DT_CANVAS_TEXT_SOURCE_MARKDOWN = 0, ///< the frame's own Markdown, edited in place
  DT_CANVAS_TEXT_SOURCE_SIDECAR = 1,  ///< the `.txt` sidecar of `linked_object`'s source image
} dt_canvas_text_source_t;

typedef enum dt_canvas_text_align_t
{
  DT_CANVAS_ALIGN_START = 0,   ///< left, or top
  DT_CANVAS_ALIGN_CENTER = 1,
  DT_CANVAS_ALIGN_END = 2,     ///< right, or bottom
  DT_CANVAS_ALIGN_JUSTIFY = 3, ///< horizontal only
} dt_canvas_text_align_t;

typedef struct dt_canvas_text_t
{
  char font[DT_CANVAS_FONT_LEN];  ///< a Pango font description, empty for the canvas default
  dt_canvas_color_t text_color;
  dt_canvas_color_t background;   ///< alpha 0 is a transparent frame
  uint32_t source;                ///< dt_canvas_text_source_t
  uint32_t linked_object;         ///< image object id for a sidecar frame, 0 otherwise
  float padding;                  ///< inner margin in canvas units
  uint32_t align_h;               ///< dt_canvas_text_align_t
  uint32_t align_v;               ///< dt_canvas_text_align_t, never JUSTIFY
  /**
   * The leading, as a multiple of what the font asks for: 1 is the font's own, 1.5 is one and
   * a half. 0 means unset and reads as 1, which is what a document from before the field
   * holds and what keeps it looking as it did.
   */
  float line_height;
  /**
   * The tracking, in THOUSANDTHS OF AN EM, so it follows the type size rather than the plane:
   * -50 tightens a line, +100 opens it out. 0 is the font's own spacing. A true condensed cut
   * is a different thing and is chosen in the font name, since Pango can only reach one that
   * the family actually ships.
   */
  float letter_spacing;
  /**
   * The inner margins, top, right, bottom, left. ALL FOUR zero takes the uniform `padding` on
   * every side, which is what a document from before them holds; any one of them set makes
   * all four literal, so a side really can be zero. Same rule, same reason, as the canvas's
   * texture weights.
   */
  float margins[4];
  /** OpenType features as Pango spells them, "liga 1, onum 1, smcp 1"; empty is the font's own. */
  char features[DT_CANVAS_TEXT_FEATURES_LEN];
  uint32_t text_flags;  ///< dt_canvas_text_flag_t
  float wrap_standoff;  ///< how far the text keeps off a frame laid over it, in canvas units
  /**
   * The first line of every paragraph, moved in from the measure by this much, in canvas
   * units. Negative hangs it out instead, which is what a bibliography or a dictionary wants.
   * Zero is flush, and is what a document from before the field holds.
   */
  float first_line_indent;
  /**
   * Extra space before every paragraph but the first, in canvas units. This is the space a
   * typographer sets INSTEAD of an indent, not as well; the two are offered together because
   * which one a text wants is the designer's call.
   */
  float paragraph_spacing;
  /**
   * A shadow cast by the GLYPHS, not by the frame.
   *
   * The frame's own shadow is `dt_canvas_object_t.shadow` and is cast from its edge; this one
   * is cast from the letters, so a title over a picture can stand off it without the frame
   * being drawn at all. A text frame with no ground already shadowed its glyphs -- a shadow's
   * silhouette is the layer's alpha -- so what this adds is the same for a frame that HAS one.
   *
   * Its visibility is NOT `dt_canvas_shadow_visible()`: see `dt_canvas_text_shadow_visible()`.
   */
  dt_canvas_shadow_t shadow;
  uint8_t reserved[DT_CANVAS_TEXT_RESERVED];

  /* runtime: the Markdown travels as its own archive entry */
  char *markdown;
} dt_canvas_text_t;

typedef enum dt_canvas_connector_style_t
{
  DT_CANVAS_CONNECTOR_PLAIN = 0,
  DT_CANVAS_CONNECTOR_ARROW_END = 1 << 0,
  DT_CANVAS_CONNECTOR_ARROW_START = 1 << 1,
  DT_CANVAS_CONNECTOR_DASHED = 1 << 2,
} dt_canvas_connector_style_t;

/**
 * The line a canvas that has never been told otherwise is drawn with, in canvas units.
 *
 * A connector's OWN zero is not this: it means the connector carries no line of its own and
 * takes the canvas's -- which is this only while the canvas carries none either. Everything
 * that draws or measures a line asks `dt_canvas_object_effective_line()`; the one place a raw
 * zero is still a zero is `dt_canvas_line_style_get()`, where it is the memory "the last line
 * drawn inherited".
 */
#define DT_CANVAS_CONNECTOR_LINE_WIDTH 2.0f
/** The widest line the properties offer, and so the widest a line is ever born with. */
#define DT_CANVAS_LINE_WIDTH_MAX 100.0f
/** An arrowhead's length and half its base, in canvas units, for a line two units wide: the painter
 * scales both with a thicker line, and whatever must clear a head -- the bounds, the pages -- asks
 * the same triangle through dt_canvas_route_arrow_head(). */
#define DT_CANVAS_ARROW_LENGTH 14.0
#define DT_CANVAS_ARROW_HALF_WIDTH 5.0

/**
 * Where on a frame a connector attaches: the four edge midpoints, the four corners, or the
 * centre. They are the frame's own points, so they rotate with it. AUTO picks, of the four
 * cardinals, the one nearest the other end's frame -- the corners and the centre are not
 * among its candidates, so a document laid out before they existed keeps the routes it had.
 *
 * The values are stored in the document: NEW ANCHORS ARE APPENDED, never inserted.
 */
typedef enum dt_canvas_anchor_t
{
  DT_CANVAS_ANCHOR_AUTO = 0,
  DT_CANVAS_ANCHOR_NORTH = 1,
  DT_CANVAS_ANCHOR_EAST = 2,
  DT_CANVAS_ANCHOR_SOUTH = 3,
  DT_CANVAS_ANCHOR_WEST = 4,
  DT_CANVAS_ANCHOR_NORTH_EAST = 5,
  DT_CANVAS_ANCHOR_SOUTH_EAST = 6,
  DT_CANVAS_ANCHOR_SOUTH_WEST = 7,
  DT_CANVAS_ANCHOR_NORTH_WEST = 8,
  DT_CANVAS_ANCHOR_CENTRE = 9, ///< aims at the centre and touches the edge, wherever the other end is
  DT_CANVAS_ANCHOR_LAST = 10,
} dt_canvas_anchor_t;

/** How a connector travels between its anchors. */
typedef enum dt_canvas_routing_t
{
  DT_CANVAS_ROUTING_STRAIGHT = 0, ///< one segment
  DT_CANVAS_ROUTING_SQUARE = 1,   ///< leaves each anchor along its normal, then horizontal and vertical legs
  DT_CANVAS_ROUTING_CUBIC = 2,    ///< a cubic Bezier tangent to each anchor's normal
} dt_canvas_routing_t;

typedef struct dt_canvas_connector_t
{
  uint32_t from_id;     ///< object id the line starts at
  uint32_t to_id;       ///< object id the line ends at
  uint32_t style;       ///< dt_canvas_connector_style_t bits
  dt_canvas_color_t color;
  float line_width;
  uint32_t from_anchor; ///< dt_canvas_anchor_t
  uint32_t to_anchor;   ///< dt_canvas_anchor_t
  uint32_t routing;     ///< dt_canvas_routing_t
  uint32_t via_count;   ///< 0, or 1 when the route passes by (via_x, via_y)
  double via_x;         ///< the waypoint, canvas units
  double via_y;
  float from_reach;     ///< length of the start's tangent handle, along the anchor's normal; 0 is automatic
  float to_reach;       ///< the same at the end
  double via_tangent_x; ///< the waypoint's tangent handle, direction and length; (0, 0) is automatic
  double via_tangent_y;
  /*
   * A FREE end is one whose id is 0 -- ids start at 1, so no document before these fields has
   * one -- and it sits at its own point instead of on a frame. A line or a curve is a connector
   * with both ends free: every consumer reads the route, and the route resolves a free end first,
   * so a line needs no kind of its own. The point and the tangent are read only while the id is 0.
   */
  double from_x;        ///< the start's point when from_id is 0, canvas units
  double from_y;
  double to_x;          ///< the end's point when to_id is 0
  double to_y;
  float from_tangent_x; ///< a free start's control offset, direction and length; (0, 0) is automatic
  float from_tangent_y;
  float to_tangent_x;   ///< the same at a free end
  float to_tangent_y;
  uint8_t reserved[DT_CANVAS_CONNECTOR_RESERVED];
} dt_canvas_connector_t;

/**
 * How a line is drawn, as distinct from where it goes: the connector's own styling and nothing
 * else, so what the atelier remembers of the last line edited can be handed to the next one
 * drawn without the document knowing where that memory lives.
 */
typedef struct dt_canvas_line_style_t
{
  float line_width;        ///< canvas units; 0 means the line the canvas sets, which is what a
                           ///< connector's own zero means too
  dt_canvas_color_t color;
  gboolean dashed;
  gboolean arrow_start;    ///< a head at the line's first point
  gboolean arrow_end;      ///< a head at its last
} dt_canvas_line_style_t;

/** The most points a routed connector is flattened to, cubic included. */
#define DT_CANVAS_ROUTE_MAX_POINTS 40

/** A connector resolved to geometry: its ends, the normals it leaves them along, the
 * cubic's control points, and the polyline every routing is flattened to for hit tests. */
typedef struct dt_canvas_route_t
{
  uint32_t routing;
  double from_x;
  double from_y;
  double to_x;
  double to_y;
  double from_normal_x; ///< unit vector leaving the start frame
  double from_normal_y;
  double to_normal_x;   ///< unit vector leaving the end frame
  double to_normal_y;
  int segment_count;    ///< 1, or 2 when the route passes by a waypoint
  double via_x;         ///< the waypoint, when segment_count is 2
  double via_y;
  double control1_x;    ///< cubic control points of the first segment; unused by the other routings
  double control1_y;
  double control2_x;
  double control2_y;
  double control3_x;    ///< cubic control points of the second segment
  double control3_y;
  double control4_x;
  double control4_y;
  int point_count;
  double points[2 * DT_CANVAS_ROUTE_MAX_POINTS]; ///< x0,y0,x1,y1..., start to end
} dt_canvas_route_t;

/** A map frame: a rendered slippy map around a point, kept as a JPEG like an image frame. */
typedef struct dt_canvas_map_t
{
  double latitude;         ///< degrees
  double longitude;        ///< degrees
  int32_t zoom;            ///< slippy zoom level, 1..19
  uint32_t source;         ///< the tile provider: an OsmGpsMapSource_t value, 0 for the default
  int32_t pixel_width;     ///< the render's dimensions, 0 when there is none yet
  int32_t pixel_height;
  int64_t rendered_at;     ///< unix time of the render, 0 when never rendered
  uint8_t reserved[DT_CANVAS_MAP_RESERVED];

  /* runtime: the JPEG travels as its own archive entry */
  GBytes *jpeg;
  dt_canvas_sync_status_t sync_status; ///< RENDERING while the tiles are fetched, MISSING when they could not be
} dt_canvas_map_t;

/** The drawn-mask shape that cuts an object out of its rectangle. */
typedef enum dt_canvas_mask_shape_t
{
  DT_CANVAS_MASK_NONE = 0,
  DT_CANVAS_MASK_CIRCLE = 1,
  DT_CANVAS_MASK_ELLIPSE = 2,
  DT_CANVAS_MASK_POLYGON = 3,
  DT_CANVAS_MASK_GRADIENT = 4,
} dt_canvas_mask_shape_t;

typedef enum dt_canvas_mask_flags_t
{
  DT_CANVAS_MASK_INVERT = 1 << 0, ///< keep what is outside the shape
} dt_canvas_mask_flags_t;

/**
 * Floats per polygon node: x, y, the incoming control point x, y, the outgoing one x, y, the
 * smooth flag, the fall-off's own radius either side of the node (0 takes the shape's), and
 * one spare. The stride is written into the file's node chunk and read back from its size, so
 * it may grow again without a format bump and without losing a node of an older document.
 */
#define DT_CANVAS_MASK_NODE_FLOATS 10

/** Indices into a node record. */
enum
{
  DT_CANVAS_MASK_NODE_X = 0,
  DT_CANVAS_MASK_NODE_Y = 1,
  DT_CANVAS_MASK_NODE_CTRL1_X = 2, ///< the control point on the previous node's side
  DT_CANVAS_MASK_NODE_CTRL1_Y = 3,
  DT_CANVAS_MASK_NODE_CTRL2_X = 4, ///< the one on the next node's side
  DT_CANVAS_MASK_NODE_CTRL2_Y = 5,
  DT_CANVAS_MASK_NODE_SMOOTH = 6, ///< a dt_canvas_mask_node_kind_t
  DT_CANVAS_MASK_NODE_BORDER1 = 7,
  DT_CANVAS_MASK_NODE_BORDER2 = 8,
};

/**
 * What `DT_CANVAS_MASK_NODE_SMOOTH` holds. A cusp is zero, which is what a node is born as
 * and what every document written before the smooth ones carries, so an older file reads
 * exactly as it did. A reader that knows only "zero or not" treats a steered node as an
 * automatic one: it loses the tangent the user gave it and keeps the shape smooth there,
 * which is the graceful half of the two.
 */
typedef enum dt_canvas_mask_node_kind_t
{
  DT_CANVAS_MASK_NODE_CUSP = 0,    ///< the two stored control points are independent
  DT_CANVAS_MASK_NODE_AUTO = 1,    ///< smooth, its tangent computed from its neighbours
  DT_CANVAS_MASK_NODE_STEERED = 2, ///< smooth, its tangent the one the user dragged
} dt_canvas_mask_node_kind_t;

/**
 * A cutout, in the object's own unit square: (0, 0) is the top-left corner of the unrotated
 * frame and (1, 1) its bottom-right, whatever its size. Radii and the feather are fractions
 * of the frame's shorter side, the way the darkroom's drawn masks measure theirs. The
 * polygon's nodes live in `nodes`, saved after the object's record.
 */
typedef struct dt_canvas_mask_t
{
  uint32_t shape;     ///< dt_canvas_mask_shape_t
  uint32_t flags;     ///< dt_canvas_mask_flags_t bits
  float feather;      ///< the fall-off's extent
  float center_x;     ///< circle, ellipse: the centre; gradient: the anchor
  float center_y;
  float radius_x;     ///< circle: the radius; ellipse: the horizontal radius; gradient: the extent
  float radius_y;     ///< ellipse: the vertical radius; gradient: the curvature
  float rotation;     ///< ellipse, gradient: degrees
  float spare;
  /* runtime */
  uint32_t node_count;
  float *nodes;       ///< node_count * DT_CANVAS_MASK_NODE_FLOATS
} dt_canvas_mask_t;

/**
 * @brief A drawing read from an SVG file on disk.
 *
 * The file's own bytes travel in the archive, so a document is complete on its own, and the
 * path it came from travels beside them so it can be read again when the drawing changes --
 * the same bargain an image frame makes with the library.
 *
 * It is RASTERISED ATOMICALLY: the whole document in one pass, the way the SVG specification
 * says it must be composited, in sRGB with the transfer function applied. That is not how this
 * canvas composites -- linear Adobe RGB -- and the difference is not a detail: an SVG's own
 * overlaps, its gradients and its anti-aliased edges are all defined in that space, so
 * rendering its pieces into ours one at a time would draw a different picture from the one its
 * author saw. Rendered whole and then converted, it arrives as one finished image and this
 * canvas blends THAT correctly over the page.
 */
typedef struct dt_canvas_svg_t
{
  char folder[DT_PATH_MAX];           ///< where the file was read from, for reading it again
  char filename[DT_MAX_FILENAME_LEN]; ///< the file's own name inside it
  float source_width;                 ///< the drawing's intrinsic size, in points; 0 when it states none
  float source_height;
  int64_t loaded_at;                  ///< unix time the bytes were taken from the file
  uint8_t reserved[DT_CANVAS_SVG_RESERVED];

  /* runtime, not serialised as fields: the file travels as its own archive entry */
  GBytes *svg;                        ///< the file's own bytes, NULL when it could not be read
  dt_canvas_sync_status_t sync_status;
} dt_canvas_svg_t;

/**
 * What a shape's outline is made of. STORED in the document, so new geometries are APPENDED
 * and never inserted; a value this build does not know is kept verbatim and drawn as its frame.
 */
typedef enum dt_canvas_shape_geometry_t
{
  DT_CANVAS_SHAPE_RECTANGLE = 0, ///< the frame itself, with the frame's own rounded corners
  DT_CANVAS_SHAPE_POLYGON = 1,   ///< a regular polygon, or a star once its depth is above zero
  DT_CANVAS_SHAPE_LAST = 2,
} dt_canvas_shape_geometry_t;

/** The deepest a star's notches go: past this the inner radius reaches zero and the shape degenerates. */
#define DT_CANVAS_SHAPE_MAX_DEPTH 0.95f
/** How many sides a polygon may have, either end of the range. */
#define DT_CANVAS_SHAPE_MIN_SIDES 3u
#define DT_CANVAS_SHAPE_MAX_SIDES 12u
/** What a polygon is born with, and what a rectangle carries so that switching geometry has a value. */
#define DT_CANVAS_SHAPE_DEFAULT_SIDES 6u
/**
 * The pentagram's notch depth: five points, the depth at which the star's edges run straight
 * through. The geometry's own number, spelled here in the float a record holds; it is
 * `math/polygon_envelope.h`'s DT_POLYGON_PENTAGRAM_DEPTH, which the toolbar's star glyph uses
 * directly, and test_canvas_document pins the two against each other rather than this header
 * taking a dependency on that one for a constant.
 */
#define DT_CANVAS_SHAPE_STAR_DEPTH 0.527864f

/**
 * A drawn shape: an outline the atelier fills, strokes or both, with no content behind it.
 *
 * Everything else a shape needs -- its fill (`background`), its border band, its corner radius,
 * its shadow, its opacity, its cutout, its box and its rotation -- is a field every object
 * already has, which is why one kind covers the rectangle, the polygon and the star instead of
 * three. The record is written for every geometry, so the tagged chunks that follow the union
 * stay aligned whichever geometry a shape holds.
 */
typedef struct dt_canvas_shape_t
{
  uint32_t geometry;  ///< dt_canvas_shape_geometry_t
  uint32_t sides;     ///< 3..12, kept for a rectangle too so a geometry switch has a value
  float depth;        ///< 0..DT_CANVAS_SHAPE_MAX_DEPTH: 0 is a convex polygon, above it a star
  float roundness;    ///< 0..1: 0 straight sides, 1 a circle
  /**
   * Radians the shape is turned by INSIDE its frame, which does not move.
   *
   * A file from before this reads 0, which is the shape as it was drawn. See `_polygon_outline()`
   * for why the turn is applied after the fit and scaled back uniformly, and for what it costs.
   */
  float phase;
  uint8_t reserved[DT_CANVAS_SHAPE_RESERVED];
} dt_canvas_shape_t;

/**
 * How a shape is drawn, as distinct from where it sits: everything the atelier remembers of the
 * last shape edited and hands to the next one drawn, so the document never learns where that
 * memory lives. The geometry is NOT here -- it is what the tool asked for, not a style.
 */
typedef struct dt_canvas_shape_style_t
{
  dt_canvas_color_t fill;        ///< the shape's own colour; an alpha of 0 leaves it an outline
  gboolean border_override;      ///< the shape carries its own border instead of the canvas's
  float border_width;            ///< canvas units, read only while `border_override`
  dt_canvas_color_t border_color;
  gboolean corner_override;      ///< the shape carries its own corner radius
  float corner_radius;           ///< canvas units, read only while `corner_override`
  gboolean shadow_override;      ///< the shape carries its own shadow
  dt_canvas_shadow_t shadow;     ///< read only while `shadow_override`
  uint32_t sides;                ///< what a polygon or a star would be born with
  float depth;
  float roundness;
  float phase;                   ///< radians, turned inside its own frame
} dt_canvas_shape_style_t;

typedef struct dt_canvas_object_t
{
  uint32_t id;        ///< unique within the canvas, never reused
  uint32_t kind;      ///< dt_canvas_object_kind_t
  double x;           ///< centre, canvas units
  double y;
  double width;       ///< unrotated frame size, canvas units
  double height;
  double rotation;    ///< radians, clockwise on screen
  int32_t z;          ///< draw order, lower is further back
  uint32_t flags;     ///< dt_canvas_object_flags_t bits
  dt_canvas_color_t border_color;
  float border_width; ///< canvas units
  dt_canvas_shadow_t shadow;  ///< applies with DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE
  float transparency; ///< 0 opaque, 1 invisible; stored this way so an older file's zeros mean opaque
  dt_canvas_mask_t mask;
  dt_canvas_color_t background; ///< under the content, filling the frame or the cutout's whole shape; alpha 0 is none. A text frame keeps its own.
  float corner_radius; ///< the frame's rounded corners, canvas units; applies with DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE
  uint8_t reserved[DT_CANVAS_OBJECT_RESERVED];
  union
  {
    dt_canvas_image_t image;
    dt_canvas_text_t text;
    dt_canvas_connector_t connector;
    dt_canvas_map_t map;
    dt_canvas_svg_t svg;
    dt_canvas_shape_t shape;
  };
} dt_canvas_object_t;

/**
 * What the plane is painted with. Stored by value, so a background is APPENDED and never
 * inserted; where it is offered in the list is `dt_canvas_background_position()`'s business,
 * the same way the page sizes work.
 */
typedef enum dt_canvas_background_t
{
  DT_CANVAS_BACKGROUND_PLAIN = 0,       ///< the background colour
  DT_CANVAS_BACKGROUND_MOLESKINE = 1,   ///< ivory notebook paper, soft texture
  DT_CANVAS_BACKGROUND_WATERCOLOUR = 2, ///< white watercolour paper, thick texture
  DT_CANVAS_BACKGROUND_EMBOSSED = 3,    ///< paper dried on a metallic mesh, its imprint in the fibres
  DT_CANVAS_BACKGROUND_JAPANESE = 4,    ///< washi: large soft clouds and long wrinkles
  DT_CANVAS_BACKGROUND_TRANSPARENT = 5, ///< nothing at all: the plane is a hole the export carries
  DT_CANVAS_BACKGROUND_PSYCHEDELIC = 6, ///< washi whose wrinkles carry a colour instead of a brightness
  DT_CANVAS_BACKGROUND_LAID = 7,        ///< verge: the mould's laid and chain wires left in a cloudy sheet
  DT_CANVAS_BACKGROUND_KRAFT = 8,       ///< unbleached wrapping paper: brown, long fibres, dark shives
  DT_CANVAS_BACKGROUND_CHARCOAL = 9,    ///< a near-black card whose tooth catches light instead of casting shade
  DT_CANVAS_BACKGROUND_LAST = 10,
} dt_canvas_background_t;

/**
 * The page the canvas is divided into. One canvas unit is one point (1/72 inch), so a print
 * size is its size in points and a screen size is its size in pixels at 72 dpi -- export such
 * a page at 72 dpi and it comes out at exactly the pixel size it is named for.
 *
 * The stored value is this index. It was RENUMBERED ONCE, deliberately, while the format was
 * still R&D and every document holding one was local: the codes had been appended as sizes were
 * thought of, so A0 and A1 sat past the social formats and the stored order said nothing. From
 * here it is APPEND-ONLY again -- insert a size and every saved document changes page.
 *
 * Nothing may read a range of these: which sizes are ISO, which are physical and which name is
 * translated are all COLUMNS of the one table behind `dt_canvas_paper_name()` and
 * `dt_canvas_paper_points()`, so a row answers for itself and a later reorder cannot make a
 * numeric test quietly wrong. `dt_canvas_paper_known()` is what a stored or configured number
 * is held to.
 */
typedef enum dt_canvas_paper_t
{
  DT_CANVAS_PAPER_NONE = 0,
  DT_CANVAS_PAPER_CUSTOM = 1,             ///< the size in `custom_paper_width`/`_height`, in points
  DT_CANVAS_PAPER_A0 = 2,
  DT_CANVAS_PAPER_A1 = 3,
  DT_CANVAS_PAPER_A2 = 4,
  DT_CANVAS_PAPER_A3 = 5,
  DT_CANVAS_PAPER_A4 = 6,
  DT_CANVAS_PAPER_A5 = 7,
  DT_CANVAS_PAPER_A6 = 8,
  DT_CANVAS_PAPER_LETTER = 9,              ///< US Letter, 8.5 x 11 in
  DT_CANVAS_PAPER_INSTAGRAM_SQUARE = 10,   ///< 1080 x 1080 px
  DT_CANVAS_PAPER_INSTAGRAM_PORTRAIT = 11, ///< 1080 x 1350 px
  DT_CANVAS_PAPER_STORY = 12,              ///< reels and stories, 1080 x 1920 px
  DT_CANVAS_PAPER_FACEBOOK_POST = 13,      ///< 1200 x 630 px
  DT_CANVAS_PAPER_FACEBOOK_COVER = 14,     ///< 851 x 315 px
  DT_CANVAS_PAPER_YOUTUBE_THUMBNAIL = 15,  ///< 1280 x 720 px
  DT_CANVAS_PAPER_YOUTUBE_BANNER = 16,     ///< channel art, 2560 x 1440 px
  DT_CANVAS_PAPER_LAST = 17,
} dt_canvas_paper_t;

typedef struct dt_canvas_t
{
  uint32_t format_version;
  char title[DT_CANVAS_TITLE_LEN];
  dt_canvas_color_t background;
  dt_canvas_color_t border_color;   ///< default border for frames without an override
  float border_width;
  float grid_size;                  ///< canvas units between grid lines
  uint32_t grid_flags;              ///< dt_canvas_grid_flags_t bits
  /**
   * The clear margin EVERY frame keeps around itself, so two of them side by side are two of
   * these apart and their margin boxes meet on one line. It used to be called the gutter,
   * which in print is the fold's own allowance and is now `bind_gutter` below.
   */
  float padding;
  uint32_t background_style;        ///< dt_canvas_background_t
  dt_canvas_color_t grid_color;     ///< the grid dots
  uint32_t paper_size;              ///< dt_canvas_paper_t
  uint32_t paper_landscape;         ///< 0 portrait, 1 landscape
  dt_canvas_color_t page_color;     ///< the page borders
  dt_canvas_shadow_t shadow;        ///< default shadow for objects without an override
  dt_canvas_color_t padding_color;   ///< the padding frames, when DT_CANVAS_PADDING_VISIBLE
  float texture_contrast;           ///< the paper's relief: multipliers, 1 is the paper as designed; 0 reads as 1
  float texture_detail;             ///< its fine structure: fibres, pores, wrinkles, the mesh
  float texture_scale;              ///< the size of its features
  float texture_grain;              ///< the dither that finishes it
  float resolution;                 ///< canvas units per inch; 0 reads as 72, which is what a file from before held
  /**
   * A SPREAD is the block of pages that stays on one sheet: `spread_cols` across by
   * `spread_rows` down. A book is 2 by 1, a zine folded both ways 2 by 2, a poster printed at
   * home and taped together as many as it takes. Pages inside a spread are contiguous and the
   * borders between them are FOLDS; between two spreads the plane opens by twice the bleed, so
   * each sheet carries its own all round and no two bleeds overlap.
   *
   * ZERO is a plane tiled uniformly, which is what every document written before these fields
   * holds and exactly the geometry it was laid out with. One is every page on its own sheet,
   * two bleeds apart.
   */
  uint32_t spread_cols;
  uint32_t spread_rows;
  /**
   * The binding's own allowance, added inside a page AT A FOLD only -- what a perfect binding
   * swallows out of the middle of a picture that crosses it. It is not the page margin, which
   * is uniform all round; it is the extra the fold side needs on top of it.
   */
  float bind_gutter;
  /**
   * The line every connector and every free line is drawn with unless it carries its own.
   *
   * The same bargain as the border, the corners and the shadow: set once for the canvas, taken
   * by everything that has not been told otherwise. A connector owns its line exactly when its
   * own `line_width` is above zero -- there is no flag, because a width of zero was already the
   * "use the default" sentinel and simply became this. See `dt_canvas_object_effective_line()`.
   *
   * A document written before these existed holds zeros in BOTH, which is read as the built-in
   * line: the whole-record rule, never one field at a time.
   */
  dt_canvas_color_t line_color;
  float line_width;
  /** The page a CUSTOM size is, in points. Zeros are a size nobody has given, which every
   * consumer already reads as no page at all. */
  float custom_paper_width;
  float custom_paper_height;
  float corner_radius;              ///< default rounded corners of the frames, canvas units; 0 is square
  float page_margin;                ///< kept clear inside every page edge, canvas units
  dt_canvas_color_t margin_color;   ///< the margin lines
  float page_bleed;                 ///< how far past every page edge the sheet keeps going, canvas units
  dt_canvas_color_t bleed_color;    ///< the bleed lines
  double view_zoom;                 ///< the viewport the canvas was saved with
  double view_x;                    ///< canvas point shown at the centre of the view
  double view_y;
  char default_font[DT_CANVAS_FONT_LEN];
  int32_t image_long_edge;          ///< pixels on the long edge of a render
  int32_t jpeg_quality;
  uint8_t reserved[DT_CANVAS_HEADER_RESERVED];

  /* runtime */
  uint32_t next_id;
  GPtrArray *objects;   ///< dt_canvas_object_t *, kept sorted by z then id
  char *path;           ///< where it was loaded from or last saved, NULL for a new canvas
  gboolean dirty;       ///< unsaved changes
  uint64_t generation;  ///< bumps on every mutation
  uint64_t serial;      ///< names this document for the life of the process: a freed document's address is reused, its serial never is
} dt_canvas_t;

/** An axis-aligned rectangle in canvas units. */
typedef struct dt_canvas_rect_t
{
  double x;
  double y;
  double width;
  double height;
} dt_canvas_rect_t;

/* --- lifecycle -------------------------------------------------------------- */

/** @brief A new, empty canvas with defaults taken from conf. */
dt_canvas_t *dt_canvas_new(void);

/** @brief Free the canvas and every object. NULL-safe. */
dt_canvas_t *dt_canvas_free(dt_canvas_t *canvas);

/**
 * @brief Deep copy: every object, the JPEG references and the Markdown strings.
 * @details Undo snapshots are made of these. JPEG bytes are shared by reference, so the
 * copy costs the records, not the pixels.
 */
dt_canvas_t *dt_canvas_copy(const dt_canvas_t *canvas);

/**
 * @brief Replace `canvas`'s content by `snapshot`'s, keeping `canvas`'s path.
 * @details The undo restore. Both are left valid; `snapshot` is not consumed.
 */
void dt_canvas_restore(dt_canvas_t *canvas, const dt_canvas_t *snapshot);

/**
 * @brief Give back what an abandoned gesture changed: `canvas`'s content is `snapshot`'s again, except
 * for the renders that landed or started since the snapshot was taken, which stay.
 * @details A gesture can be held open for as long as the user likes -- a colour window, typically --
 * and a picture's render finishing meanwhile is no part of it: put back as the snapshot had it, the
 * picture would read RENDERING again with no job left to finish it. The document is left dirty only
 * if the snapshot was, or a render landed; the generation moves, so the painter composites again.
 * `snapshot` is not consumed.
 */
void dt_canvas_abandon(dt_canvas_t *canvas, const dt_canvas_t *snapshot);

/* --- persistence ----------------------------------------------------------- */

/**
 * @brief Read a canvas file.
 * @param path a `.anselcanvas` archive.
 * @param error receives what went wrong, may be NULL.
 * @return the canvas, or NULL with `error` set. `canvas->path` is set to `path`.
 */
dt_canvas_t *dt_canvas_load(const char *path, GError **error);

/**
 * @brief Write the canvas to `path`, atomically.
 * @details On success `canvas->path` is updated and `dirty` is cleared.
 */
gboolean dt_canvas_save(dt_canvas_t *canvas, const char *path, GError **error);

/* --- objects ---------------------------------------------------------------- */

/**
 * @brief Add an image frame for a library image whose render has not happened yet.
 * @param source_width the source's own dimensions, so the frame gets its aspect ratio at once.
 * @return the object, owned by the canvas.
 */
dt_canvas_object_t *dt_canvas_add_image(dt_canvas_t *canvas, double x, double y, int32_t source_width,
                                        int32_t source_height);

/** @brief Add a text frame. `markdown` is copied; NULL means empty. */
dt_canvas_object_t *dt_canvas_add_text(dt_canvas_t *canvas, double x, double y, double width, double height,
                                       const char *markdown);

/** @brief Add a map frame around a point, not rendered yet. */
dt_canvas_object_t *dt_canvas_add_map(dt_canvas_t *canvas, double x, double y, double latitude, double longitude,
                                      int32_t zoom, uint32_t source);

/** @brief Give the map frame its render. Takes a reference on `jpeg`. */
void dt_canvas_map_set_render(dt_canvas_t *canvas, dt_canvas_object_t *object, GBytes *jpeg, int32_t pixel_width,
                              int32_t pixel_height, int64_t rendered_at);

/** @brief The raster a frame shows: an image frame's or a map frame's JPEG, NULL for the others or when unrendered. */
GBytes *dt_canvas_object_raster(const dt_canvas_object_t *object);

/**
 * @brief Add a drawing read from an SVG file, sized to what the file itself says it is.
 *
 * A canvas unit is a point, and so is an SVG's own user unit when the file states a physical
 * size -- so a drawing arrives at the size its author meant, on the page, without a scale
 * factor anywhere. A file that states only a viewBox has no physical size to honour and is
 * given its viewBox in points, which is the same convention every browser applies.
 *
 * @return the object, or NULL with `error` set when the file cannot be read or parsed.
 */
dt_canvas_object_t *dt_canvas_add_svg(dt_canvas_t *canvas, double x, double y, const char *path,
                                      GError **error);

/**
 * @brief Read the drawing's file again from where it came, keeping the frame where it is.
 * @return TRUE when the bytes changed, so the caller knows whether anything needs repainting.
 */
gboolean dt_canvas_svg_reload(dt_canvas_t *canvas, dt_canvas_object_t *object, GError **error);

/** @brief The path an SVG object was read from, or an empty string; the buffer is the caller's. */
void dt_canvas_svg_path(const dt_canvas_object_t *object, char *path, size_t length);

/** @brief Add a connector between two objects. Refuses self-links and unknown ids. */
dt_canvas_object_t *dt_canvas_add_connector(dt_canvas_t *canvas, uint32_t from_id, uint32_t to_id);

/** @brief What a connector is born with, and a line nobody has styled yet. */
dt_canvas_line_style_t dt_canvas_line_style_default(void);

/** @brief Read a connector's styling. FALSE, and `style` untouched, for anything else. */
gboolean dt_canvas_line_style_get(const dt_canvas_object_t *object, dt_canvas_line_style_t *style);

/**
 * @brief Make a style handed in from outside the document one a line can be born with.
 * @details The atelier remembers the last line's style between sessions, and what comes back from
 * there was written by whatever wrote it: a width that is not a number goes back to the default, one
 * out of range is held to 0..DT_CANVAS_LINE_WIDTH_MAX, a colour channel is held to 0..1 (one that is not
 * a number to 0, so a damaged colour stays a colour), and the three switches read as plain TRUE or FALSE.
 * @return TRUE when the style was already sound and nothing was changed.
 */
gboolean dt_canvas_line_style_sanitize(dt_canvas_line_style_t *style);

/**
 * @brief Add a line with both ends free, from (x0, y0) to (x1, y1).
 * @details A cubic line is seeded into an arc (dt_canvas_connector_seed_curve()): a free cubic
 * with automatic tangents would follow its own chord and read as straight.
 * @param style how it is drawn; NULL is dt_canvas_line_style_default().
 */
dt_canvas_object_t *dt_canvas_add_line(dt_canvas_t *canvas, double x0, double y0, double x1, double y1,
                                       dt_canvas_routing_t routing, const dt_canvas_line_style_t *style);

/** @brief Whether this is a connector with at least one end at its own point rather than on a frame. */
gboolean dt_canvas_connector_has_free_end(const dt_canvas_object_t *object);

/**
 * @brief Whether this is a LINE: a connector with BOTH ends at their own points and no frame at all.
 * @details What the line and the curve tools draw, and what the atelier owns outright -- it moves,
 * duplicates and is styled as itself, and its style is what the next line is drawn with. One end on a
 * frame makes it that frame's connector however the other end is spelled, so the two questions must
 * never be asked with one word: dt_canvas_connector_has_free_end() answers the other one, which is
 * about what a connector owns rather than about what it is.
 */
gboolean dt_canvas_connector_is_line(const dt_canvas_object_t *object);

/**
 * @brief Move what a connector owns: its free points, and its waypoint when any end is free.
 * @details An anchored end follows its frame and is not the connector's to move. The caller
 * touches the canvas, once per gesture step, as for any in-place edit.
 */
void dt_canvas_connector_translate(dt_canvas_object_t *object, double dx, double dy);

/**
 * @brief Give a free cubic without a waypoint the tangents of a symmetric arc over its chord.
 * @details Only the free ends whose tangent is still automatic (0, 0) are seeded, so a steered
 * tangent is never overwritten; a caller that wants the arc again clears them first. The start
 * leaves 30 degrees to one side of the chord and the end arrives 30 degrees to the same side,
 * each at 0.4 of the chord's length, so the arc bulges up on screen for a line drawn rightward.
 * @param route the connector's current route, which is where an anchored end is.
 * @return TRUE when a tangent was written; the caller touches the canvas.
 */
gboolean dt_canvas_connector_seed_curve(dt_canvas_object_t *object, const dt_canvas_route_t *route);

/** @brief What a shape is born with before anybody has drawn or styled one: filled neutral grey,
 * the canvas's own border, a hexagon's six sides and straight edges. */
dt_canvas_shape_style_t dt_canvas_shape_style_default(void);

/** @brief Read a shape's styling. FALSE, and `style` untouched, for anything else. */
gboolean dt_canvas_shape_style_get(const dt_canvas_object_t *object, dt_canvas_shape_style_t *style);

/**
 * @brief Make a style handed in from outside the document one a shape can be born with.
 * @details The atelier remembers the last shape's style between sessions, and what comes back from
 * there was written by whatever wrote it: a colour channel is held to 0..1 (one that is not a number
 * to 0, so a damaged colour stays a colour), a width and a radius to what their rows allow, the sides
 * to 3..12, the depth to 0..DT_CANVAS_SHAPE_MAX_DEPTH, the roundness to 0..1, and every switch reads
 * as plain TRUE or FALSE.
 * @return TRUE when the style was already sound and nothing was changed.
 */
gboolean dt_canvas_shape_style_sanitize(dt_canvas_shape_style_t *style);

/**
 * @brief Add a shape of `geometry` filling `box`, styled as `style`.
 * @param box the frame, as a rectangle on the plane: the object's own centre is its middle. Either
 * side under a couple of units is held up to it, so no shape is born too small to take hold of.
 * @param style how it is drawn; NULL is dt_canvas_shape_style_default().
 * @return the object, owned by the canvas.
 */
dt_canvas_object_t *dt_canvas_add_shape(dt_canvas_t *canvas, dt_canvas_shape_geometry_t geometry,
                                        const dt_canvas_rect_t *box, const dt_canvas_shape_style_t *style);

/**
 * The most points a shape's outline is sampled to: a rectangle needs a handful, a rounded star the
 * lot. The worst case is a STRAIGHT twelve-pointed star whose vertices are filleted -- twenty-four
 * corners, each an arc of up to half a turn laid down every six degrees -- rather than a rounded
 * one, which has no corner to fillet and is sampled at `DT_POLYGON_OUTLINE_MAX_POINTS`.
 */
#define DT_CANVAS_SHAPE_OUTLINE_MAX 1024

/**
 * @brief A shape's outline, in the FRAME'S OWN coordinates: the centre is (0, 0) and the corners
 * are at plus or minus half the width and half the height, before the object's rotation.
 *
 * ONE outline answers for the shape's edge wherever it is asked about -- the hit test, the coverage
 * raster text flows against, and the outline the painter will stroke once a geometry arrives that
 * cairo cannot draw as a frame path -- so those cannot disagree about where the edge is. A
 * rectangle's is the very path `_frame_path()` draws, corner radius included. It is a closed
 * polyline: the last point joins the first, and the closing segment is not repeated.
 *
 * @param xy receives x0, y0, x1, y1, ...; at least `2 * max` doubles.
 * @param max how many POINTS the caller has room for; DT_CANVAS_SHAPE_OUTLINE_MAX is always enough.
 * @return how many points were written, 0 for an object that is not a shape.
 */
size_t dt_canvas_shape_outline(const dt_canvas_t *canvas, const dt_canvas_object_t *object, double *xy, size_t max);

/**
 * @brief Whether text flowing under this shape must be given its RASTER rather than its frame.
 * @details A filled rectangle covers its frame exactly, and the rounded-rectangle reach already
 * says so. Anything else -- an outline box, whose middle is a hole, and every polygon -- covers
 * only where it puts ink, and only a raster of the outline says where that is. A shape with a
 * cutout is asked for the cutout, as any other cut frame is.
 */
gboolean dt_canvas_shape_needs_coverage(const dt_canvas_object_t *object);

/**
 * @brief Whether the object's own outline is what it draws, rather than the frame it stands in.
 * @details True of a polygon and of a star, which is a polygon whose notches have a depth. It is
 * the one question the painter, the hit test, the silhouette and the coverage raster all ask before
 * reaching for the frame's rectangle, so none of them can answer it differently from the others --
 * a cutout among them: a polygon is not cut, whatever mask a hand-edited file left on it.
 */
gboolean dt_canvas_shape_is_polygon(const dt_canvas_object_t *object);

/**
 * @brief Whether a cutout stands between this frame and its own rectangle.
 * @details A frame carrying a drawn mask is cut out of its box, and its border follows the cutout
 * instead of its edge. A polygon answers FALSE whatever its mask holds: its outline is its own, and
 * nothing in the atelier offers to cut one.
 */
gboolean dt_canvas_object_is_cut(const dt_canvas_object_t *object);

/**
 * @brief Width over height of the box around a polygon's or a star's own outline.
 * @details A frame of that shape holds the outline with every side of it touching the frame and
 * nothing stretched: a triangle is `2 / sqrt(3)` wide for its height, a hexagon `sqrt(3) / 2`. The
 * arguments are held to what a shape may carry, so any three numbers give a usable ratio.
 */
double dt_canvas_shape_unit_aspect(uint32_t sides, float depth, float roundness);

/**
 * @brief Hold a box up to the smallest a shape may have, without changing its proportions.
 * @details Both sides are lifted by the one factor the smaller of them needs, never one at a time:
 * a regular shape held up side by side is not that shape any more -- a hexagon dragged out three
 * units across is 3 by 3.46 and would land in a SQUARE box -- and since a polygon keeps whatever
 * ratio it is given, that square is then what every later resize preserves. A box with no width or
 * no height states no proportions to keep, and each side is simply held up. This is the one place
 * that answers the question, so a drag in flight and the birth of the shape it draws agree.
 */
void dt_canvas_shape_hold_minimum(double *width, double *height);

/**
 * @brief Take the shape's height back to the one its outline asks for, about its own centre.
 * @details A polygon is regular: the sides, the notch depth and the roundness decide the shape of
 * its box, so every write to one of the three owes this. A shape told to keep no proportions
 * (`DT_CANVAS_OBJECT_FLAG_FREE_RATIO`) is left stretched as the user stretched it, and a rectangle
 * has no ratio of its own to go back to.
 * @return TRUE when the height moved.
 */
gboolean dt_canvas_shape_refit_height(dt_canvas_object_t *object);

/**
 * @brief Remove an object.
 * @details Removing a frame also removes every connector attached to it, and unlinks any
 * sidecar text frame that showed its note (the text stays, with the last content it had).
 */
gboolean dt_canvas_remove_object(dt_canvas_t *canvas, uint32_t id);

/**
 * @brief Duplicate a frame or a line, offset by a little so the copy is visible.
 * @details A line -- a connector with both ends free -- is copied with its points and waypoint
 * offset the same way. A connector with an anchored end belongs to its frames and is not duplicated.
 */
dt_canvas_object_t *dt_canvas_duplicate_object(dt_canvas_t *canvas, uint32_t id);

dt_canvas_object_t *dt_canvas_find_object(const dt_canvas_t *canvas, uint32_t id);
guint dt_canvas_object_count(const dt_canvas_t *canvas);
/** @brief The object at `index` in draw order (back to front). NULL past the end. */
dt_canvas_object_t *dt_canvas_object_at(const dt_canvas_t *canvas, guint index);

/** @brief Mark the canvas changed: bump the generation and set dirty. Call after any in-place edit. */
void dt_canvas_touch(dt_canvas_t *canvas);

/** @brief Draw-order edits. Each re-sorts the object list. */
void dt_canvas_object_to_front(dt_canvas_t *canvas, uint32_t id);
void dt_canvas_object_to_back(dt_canvas_t *canvas, uint32_t id);
void dt_canvas_object_raise(dt_canvas_t *canvas, uint32_t id);
void dt_canvas_object_lower(dt_canvas_t *canvas, uint32_t id);

/** @brief Give the image frame its render. Takes a reference on `jpeg`. */
void dt_canvas_image_set_render(dt_canvas_t *canvas, dt_canvas_object_t *object, GBytes *jpeg, int32_t pixel_width,
                                int32_t pixel_height, uint64_t history_hash, int64_t rendered_at, uint32_t colorspace);

/** @brief Replace a text frame's Markdown. Copied; NULL means empty. */
void dt_canvas_text_set_markdown(dt_canvas_t *canvas, dt_canvas_object_t *object, const char *markdown);

/** @brief The Markdown of a text frame, never NULL for a text object. */
const char *dt_canvas_text_get_markdown(const dt_canvas_object_t *object);

/** @brief The font of a text frame: its own, or the canvas default when it has none. */
const char *dt_canvas_text_effective_font(const dt_canvas_t *canvas, const dt_canvas_object_t *object);

/** @brief The border a frame is drawn with: its own when overridden, the canvas default otherwise. */
void dt_canvas_object_effective_border(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_color_t *color, float *width);

/** @brief The shadow an object is drawn with: its own with the override flag, else the canvas default. */
void dt_canvas_object_effective_shadow(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_shadow_t *shadow);

/** @brief The paper texture's four multipliers, an unset (zero) one read as 1. */
void dt_canvas_texture_get(const dt_canvas_t *canvas, float *contrast, float *detail, float *scale, float *grain);

/** @brief The colour a paper is traditionally sold in: what the background takes when a paper is chosen. */
dt_canvas_color_t dt_canvas_background_tint(uint32_t style);

/** @brief How many backgrounds there are to offer. */
int dt_canvas_background_count(void);

/** @brief The background shown at `position`, translated. */
const char *dt_canvas_background_name(int position);

/** @brief The dt_canvas_background_t to store for the background shown at `position`. */
uint32_t dt_canvas_background_code(int position);

/** @brief Where a stored dt_canvas_background_t sits in the list. */
int dt_canvas_background_position(uint32_t style);

/**
 * @brief Whether the plane is a hole rather than a colour.
 * @details Such a canvas composites to real transparency, which a format without an alpha
 * channel cannot carry: `dt_canvas_export_format_carries_alpha()` is the other half.
 */
gboolean dt_canvas_background_is_transparent(uint32_t style);

/** @brief Whether a shadow draws anything at all: a radius other than zero and some strength. */
gboolean dt_canvas_shadow_visible(const dt_canvas_shadow_t *shadow);

/**
 * @brief Is a TEXT's own shadow drawn?
 *
 * Not the same question as a frame's. A frame's shadow needs a blur to be anything -- an
 * unblurred copy of a rectangle offset behind a rectangle is a rectangle -- so
 * `dt_canvas_shadow_visible()` requires one. Offset with NO blur is the commonest drop shadow
 * in title work, and a hard-edged copy of the glyphs is exactly what it is, so a text's shadow
 * is drawn whenever it is coloured and displaced, blurred or grown at all -- grown alone, it is
 * the outline that keeps a caption legible over a busy picture.
 */
gboolean dt_canvas_text_shadow_visible(const dt_canvas_shadow_t *shadow);

/** @brief The corner radius a frame is drawn with, in canvas units, never past half its shorter side. */
double dt_canvas_object_effective_corner_radius(const dt_canvas_t *canvas, const dt_canvas_object_t *object);

/** @brief The colour under an object's content: a text frame's own, else the object's. */
dt_canvas_color_t dt_canvas_object_background(const dt_canvas_object_t *object);

/**
 * @brief Give an object a cutout of a shape, at a sensible default geometry; NONE removes it.
 * A shape already of that kind is kept as it is.
 */
void dt_canvas_mask_set_shape(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t shape);

/** @brief Replace the polygon's nodes; `nodes` holds count * DT_CANVAS_MASK_NODE_FLOATS floats. */
void dt_canvas_mask_set_nodes(dt_canvas_t *canvas, dt_canvas_object_t *object, const float *nodes, uint32_t count);

/** @brief Insert a corner node at `index` (0..count), at the unit-square point. */
gboolean dt_canvas_mask_insert_node(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t index, float x, float y);

/** @brief Remove a node; refused when three would not remain. */
gboolean dt_canvas_mask_remove_node(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t index);

/** @brief Drop the nodes; called by the object's owner before freeing it. */
void dt_canvas_mask_clear(dt_canvas_object_t *object);

/** @brief A hash of everything that changes the mask's raster: the key of a cached raster. */
uint64_t dt_canvas_mask_hash(const dt_canvas_mask_t *mask);

/* --- geometry --------------------------------------------------------------- */

/** @brief Is this object a frame -- anything with a box of its own -- rather than a connector? */
gboolean dt_canvas_object_is_frame(const dt_canvas_object_t *object);

/**
 * @brief Whether resizing this frame must keep its proportions.
 *
 * A picture and a drawing have a shape of their own to keep; a text frame does not, and neither
 * does a rectangle. A polygon and a star do, and theirs is not a source file's but their own
 * outline's -- dt_canvas_shape_unit_aspect() -- so a regular shape stays regular. The flag
 * only frees what would otherwise be kept, so everything that has proportions keeps them
 * until it is told otherwise.
 */
gboolean dt_canvas_object_keeps_ratio(const dt_canvas_object_t *object);

/**
 * @brief The four corners of a frame after rotation, in canvas units.
 * @param corners receives x0,y0 ... x3,y3, top-left first, clockwise on screen.
 */
void dt_canvas_object_corners(const dt_canvas_object_t *object, double corners[8]);

/** @brief The axis-aligned box around the rotated frame. */
dt_canvas_rect_t dt_canvas_object_bounds(const dt_canvas_object_t *object);

/**
 * @brief How far a point is from the nearest point of a segment, in the units of both.
 * @details The one measure a connector is picked by: dt_canvas_object_contains() asks it of every
 * leg, and a handle site shaped as a segment asks it too, so the band a line is caught in cannot
 * be spelled two ways.
 */
double dt_canvas_segment_distance(double px, double py, double ax, double ay, double bx, double by);

/** @brief Is the canvas point inside the rotated frame? Connectors answer by distance to their line. */
gboolean dt_canvas_object_contains(const dt_canvas_t *canvas, const dt_canvas_object_t *object, double x, double y,
                                   double tolerance);

/** @brief Transform a canvas point into the frame's own unrotated space, centred on the frame. */
void dt_canvas_object_to_local(const dt_canvas_object_t *object, double x, double y, double *local_x,
                               double *local_y);

/**
 * @brief A frame's cardinal point and the outward normal there.
 * @param anchor which point; AUTO picks the one nearest (target_x, target_y).
 */
void dt_canvas_object_anchor_point(const dt_canvas_t *canvas, const dt_canvas_object_t *frame,
                                   dt_canvas_anchor_t anchor, double target_x, double target_y, double *x, double *y,
                                   double *normal_x, double *normal_y);

/**
 * @brief Where the anchor's handle sits, which is where it is drawn and clicked.
 * @details Every anchor's handle is its attachment point, except the centre's: that one is at
 * the frame's centre, while what it attaches is out on the edge facing the other end.
 */
void dt_canvas_object_anchor_handle(const dt_canvas_t *canvas, const dt_canvas_object_t *frame,
                                    dt_canvas_anchor_t anchor, double *x, double *y);

/**
 * @brief How far from the frame's centre the object still draws something, along `dir` in the
 * frame's own axes.
 * @details What the object draws is its rounded rectangle, or -- where a cutout replaces it --
 * the cut shape grown by its fall-off and by the border band dilated from it, never past the
 * frame. It is what the centre anchor leaves by, so a connector meets the picture rather than
 * an empty corner of its bounding box. `dir` need not be normalised; the result is in canvas
 * units along it.
 */
double dt_canvas_object_silhouette_reach(const dt_canvas_t *canvas, const dt_canvas_object_t *frame, double dir_x,
                                         double dir_y);

/**
 * @brief Whether a canvas point falls on what a frame actually DRAWS, grown by `standoff`.
 * @details The silhouette, not the bounding box: a cut frame covers its cut shape and the
 * empty corner beside it covers nothing. Exact for a shape every ray from the centre leaves
 * once -- a circle, an ellipse, a rounded rectangle, a convex polygon -- and an approximation
 * for one that does not, which is the same bargain `dt_canvas_object_silhouette_reach()`
 * already makes for the connectors.
 */
gboolean dt_canvas_object_covers(const dt_canvas_t *canvas, const dt_canvas_object_t *frame, double x, double y,
                                 double standoff);

/**
 * @brief Resolve a connector to its geometry.
 * @details A free end (id 0) sits at its own point and leaves along its own tangent, or when
 * that is automatic toward the waypoint or the other end; an anchored end aims at the other
 * frame's centre, or at the other end's point when that end is free.
 * @return FALSE when an anchored end is missing.
 */
gboolean dt_canvas_connector_route(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route);

/**
 * @brief The two ends of a connector: its anchor points, or a free end's own point.
 * @return FALSE when an anchored end is missing.
 */
gboolean dt_canvas_connector_endpoints(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                       double *from_x, double *from_y, double *to_x, double *to_y);

/**
 * @brief The line a connector is actually drawn with: its own, or the canvas's when it has none.
 *
 * A connector owns its line exactly when `connector.line_width > 0`. The canvas's own pair is
 * read as a WHOLE record: both zero is a document written before the canvas had a line of its
 * own and answers with the built-in one, where one zero is a zero.
 *
 * @param color where the colour goes, or NULL.
 * @param width where the width goes, or NULL. Never zero.
 */
void dt_canvas_object_effective_line(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                     dt_canvas_color_t *color, float *width);

/**
 * @brief How far a connector's ink can reach past its route in any direction, in canvas units.
 * @details Half the painted width for a plain line, whose caps are round; with an arrowhead at
 * either end, the head's length grown with the width. It is one number for the whole route, so it
 * over-counts everywhere but at a head: a box that must hold the ink and nothing else asks
 * dt_canvas_object_extent() instead. A stored width of zero is painted
 * DT_CANVAS_CONNECTOR_LINE_WIDTH wide and is answered for as such.
 */
double dt_canvas_stroke_reach(double line_width, uint32_t style);

/**
 * @brief The triangle an arrowhead is painted as, at one end of a resolved route.
 * @details The tip is the route's end, and the head points along the last leg of the flattened
 * route there; its length and base grow with the line's width. The painter fills exactly this.
 * @param at_end TRUE for the head at the route's last point, FALSE for its first.
 * @param xy receives the tip, then the two corners of the base: six numbers, canvas units.
 */
void dt_canvas_route_arrow_head(const dt_canvas_route_t *route, gboolean at_end, double line_width, double xy[6]);

/**
 * @brief The axis-aligned box an object's ink occupies, in canvas units.
 * @details A frame's rotated bounds. A connector's stroke -- the true curve's extremes for a
 * cubic, which lie inside its control points but rarely on them -- grown by half the painted
 * width, together with the triangle of each arrowhead it has: the box a page or a sheet must hold
 * for the line to be on it, and no more, so a line running near a page's edge does not bring the
 * next page with it.
 * @return FALSE when the object is a connector that cannot be routed.
 */
gboolean dt_canvas_object_extent(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                 dt_canvas_rect_t *out);

/**
 * @brief The box around every visible frame and every visible line with a free end. Empty
 * (width 0) for an empty canvas.
 * @details A connector anchored at both ends lies between frames already counted, and is left
 * out so a document from before free ends is framed exactly as it was.
 */
dt_canvas_rect_t dt_canvas_bounds(const dt_canvas_t *canvas);

/** @brief Round `value` to the grid when snapping is on, else return it unchanged. */
double dt_canvas_snap(const dt_canvas_t *canvas, double value);

/**
 * @brief Where a line's end goes when it is pulled to (x, y), about the line's other end.
 * @details Without an angle step the point snaps to the grid on both axes, when snapping is on.
 * With one, the direction from the other end is rounded to a multiple of the step and the end
 * lands where the pointer's own position projects onto that direction, so along a locked axis it
 * follows the pointer exactly; the angle wins over the grid there, except that an end locked onto
 * an axis still snaps along the axis it moves on. A pointer on the other end has no direction and
 * is left where it is.
 * @param step_degrees 0 for no lock; 45 or 15 are what the atelier's modifiers ask for.
 */
void dt_canvas_constrain_line_end(const dt_canvas_t *canvas, double origin_x, double origin_y, int step_degrees,
                                  double *x, double *y);

/** @brief The object whose frame is under the point, frontmost first. NULL when none. */
dt_canvas_object_t *dt_canvas_pick(const dt_canvas_t *canvas, double x, double y, double tolerance);

/**
 * @brief Snap a moving box next to, or in line with, the other frames.
 * @details Candidates are the other frames' edges plus or minus the padding (side by side with
 * the canvas margin) and their edges themselves (aligned). The nearest candidate within
 * `threshold` wins per axis. This is the padding rule; it ignores the canvas's snap flags,
 * the caller consults them.
 * @param moving the box being moved, canvas units.
 * @param exclude object ids not to snap against (the selection itself); may be NULL.
 * @param edges which of the box's edges are moving and may snap (dt_canvas_edges_t bits).
 * @param delta_x receives the shift to apply on x, 0 when nothing is within reach.
 * @return TRUE when at least one axis snapped.
 */
gboolean dt_canvas_snap_to_neighbours(const dt_canvas_t *canvas, const dt_canvas_rect_t *moving,
                                      const GArray *exclude, double threshold, uint32_t edges, double *delta_x,
                                      double *delta_y);

/**
 * @brief Snap a size to another frame's width or height, or to a run of frames.
 * @details The same-size rule: the nearest candidate within `threshold` replaces `*width`, and
 * likewise for `*height`, each axis on its own. Candidates are every other frame's box and
 * every run of frames stacked one padding apart (masonry style), so a frame beside two stacked
 * ones can take their combined height.
 * @param width_reference receives the box the width was taken from, for a guide; may be NULL.
 * @return TRUE when at least one dimension snapped.
 */
gboolean dt_canvas_snap_size(const dt_canvas_t *canvas, const GArray *exclude, double threshold, double *width,
                             double *height, dt_canvas_rect_t *width_reference, dt_canvas_rect_t *height_reference);

/**
 * @brief The paper's size in canvas units (points), as oriented.
 * @return FALSE when the canvas has no paper.
 */
gboolean dt_canvas_paper_dimensions(const dt_canvas_t *canvas, double *width, double *height);

/**
 * @brief How many page sizes there are to offer, DT_CANVAS_PAPER_NONE included.
 * @details `position` runs 0..count-1 in the order a list should show them, which is NOT the
 * stored value: sizes are appended to `dt_canvas_paper_t` so old documents keep their page,
 * and appear in the list wherever they belong. `dt_canvas_paper_code()` turns a position into
 * the value to store, `dt_canvas_paper_position()` turns it back.
 */
int dt_canvas_paper_count(void);

/** @brief The page size shown at `position`, translated, or NULL past the end. */
const char *dt_canvas_paper_name(int position);

/** @brief The dt_canvas_paper_t to store for the size shown at `position`. */
uint32_t dt_canvas_paper_code(int position);

/** @brief Where a stored dt_canvas_paper_t sits in the list, or 0 when it is not one. */
int dt_canvas_paper_position(uint32_t paper);

/**
 * @brief The page size in points, portrait. FALSE for DT_CANVAS_PAPER_NONE and past the end.
 * @note A pixel-defined size (a story, a banner) is that many points, which is that many
 * pixels at 72 dpi.
 */
/**
 * The density a PIXEL is a physical length at: the W3C reference pixel, which is what every
 * browser and toolkit means by one. A screen format named in pixels is that many reference
 * pixels, so it has a size in points like any sheet of paper, and exporting it at this density
 * gives back exactly the pixel count it is named for.
 */
#define DT_CANVAS_REFERENCE_PIXEL_DPI 96.0

gboolean dt_canvas_paper_points(uint32_t paper, double *width, double *height);

/**
 * @brief Whether a page size is STATED in points, as a sheet of paper is, or in pixels.
 *
 * Both are physical: a pixel is a length as soon as a density is named for it, and
 * `dt_canvas_paper_points()` converts a pixel-stated format at
 * `DT_CANVAS_REFERENCE_PIXEL_DPI`. This says only which way the format is written down -- so a
 * panel can show a story as "1080 x 1920 px" and A4 as "595 x 842 pt" -- and never how it
 * reaches the plane, which is points either way.
 */
gboolean dt_canvas_paper_is_physical(uint32_t paper);

/**
 * @brief Is this a page size this build knows?
 *
 * What a number read from a document or a configuration is held to. The enum was renumbered
 * once and is append-only from here, so a file written by a later build can name a size this
 * one has never heard of -- and the two CLAMPs that used to be the only guard would have turned
 * it into whatever sits at the end of the table instead of into no page at all.
 */
gboolean dt_canvas_paper_known(uint32_t paper);

/**
 * @brief The size a CUSTOM page is, in points, or FALSE when it has never been given one.
 *
 * Zeros are what a page nobody has sized reads as, and every consumer already treats a page of
 * no size as no page -- which is the safe reading and needs no migration.
 */
gboolean dt_canvas_paper_custom(const dt_canvas_t *canvas, double *width, double *height);

/** @brief Set the size a CUSTOM page is, in points, each held to what a page may be. */
void dt_canvas_paper_custom_set(dt_canvas_t *canvas, double width, double height);

/**
 * @brief A text frame's four effective inner margins, top, right, bottom, left, in canvas
 * units -- the border's own inset NOT included.
 */
void dt_canvas_text_margins(const dt_canvas_object_t *object, double margins[4]);

/**
 * @brief The name and the explanation this build has for an OpenType tag.
 *
 * The stored form stays the string Pango reads -- that is what the renderer wants and what a
 * file can carry without a table of its own -- and these turn a tag into something a person
 * can tick. NULL for a tag this build has no name for, which is not an error: a font may ship
 * any tag its designer cut (a stylistic set, a script's own form), and one that cannot be
 * named is offered by its tag. Which tags to offer at all is asked of the FONT, through
 * `dt_canvas_paint_text_font_features()` -- a fixed list offers a plain face things it does
 * not have and hides a rich one's own.
 */
gchar *dt_canvas_text_feature_label(const char *tag);
gchar *dt_canvas_text_feature_hint(const char *tag);

/**
 * @brief Whether a tag is one a person chooses, rather than one the layout engine owns.
 *
 * A font ships glyph composition, mark placement, cursive joining forms and the language's own
 * substitutions so that text can be SHAPED at all; HarfBuzz turns those on and off as the
 * script requires and a checkbox overriding it breaks the rendering rather than styling it.
 */
gboolean dt_canvas_text_feature_offered(const char *tag);

/** @brief Whether a stored feature string switches a tag on, and how to switch one in it. */
gboolean dt_canvas_text_feature_is_on(const char *features, const char *tag);
void dt_canvas_text_feature_set(char *features, const size_t length, const char *tag, const gboolean on);

/**
 * @brief The density the page is RASTERISED at, in dots per inch. Nothing else.
 *
 * A canvas unit is a POINT, so the plane's geometry does not know this number: changing it
 * moves nothing on the page and only decides how many pixels an export carries
 * (`pixels = points * dpi / 72`). It used to scale the page and leave everything on the page
 * where it was, so raising it shrank the whole layout against its own paper -- measured on A4,
 * a twelve-point line went from 7.0% of the page's height at 72 dpi to 1.7% at 300.
 */
double dt_canvas_resolution(const dt_canvas_t *canvas);

/**
 * @brief The sheet a page belongs to: the block of pages that stays contiguous, and how many
 * pages it holds.
 *
 * With no spread the sheet IS the page, which is the uniform tiling every document had before
 * spreads existed. `cols`/`rows` may be NULL.
 */
gboolean dt_canvas_spread_rect(const dt_canvas_t *canvas, int col, int row, dt_canvas_rect_t *rect, int *cols,
                               int *rows);

/** @brief Where a page sits inside its own spread, so a caller can tell a fold from a trim. */
void dt_canvas_page_in_spread(const dt_canvas_t *canvas, int col, int row, int *across, int *down, int *cols,
                              int *rows);

/**
 * @brief The page rectangle inset by the margin, and by the bind gutter on whichever sides are
 * a fold.
 */
gboolean dt_canvas_page_margin_rect(const dt_canvas_t *canvas, int col, int row, dt_canvas_rect_t *rect);

/** @brief The page column and row whose rectangle covers a point, clamped into the nearest page. */
void dt_canvas_page_at(const dt_canvas_t *canvas, double x, double y, int *col, int *row);

/**
 * @brief The rectangle of one page, grown by `outset` on every side.
 * @details A negative outset is the page's inner margin, a positive one its bleed. FALSE when
 * the canvas is not divided into pages.
 */
gboolean dt_canvas_page_guide_rect(const dt_canvas_t *canvas, int col, int row, double outset,
                                   dt_canvas_rect_t *rect);

/** @brief The page rectangle at column `col`, row `row` of the paper tiling, from the origin. */
dt_canvas_rect_t dt_canvas_page_rect(const dt_canvas_t *canvas, int col, int row);

/**
 * @brief Snap a moving box's edges onto the page borders.
 * @details The page rule; ignores the canvas's snap flags, the caller consults them.
 * @return TRUE when at least one axis snapped.
 */
gboolean dt_canvas_snap_to_pages(const dt_canvas_t *canvas, const dt_canvas_rect_t *moving, double threshold,
                                 uint32_t edges, double *delta_x, double *delta_y);

/**
 * @brief The middle of a route by arc length, where a waypoint added to it would sit.
 * @details Pure: reads the route and nothing else, so a caller can ask where a waypoint WOULD go
 * without adding one -- dt_canvas_connector_add_via() touches the document.
 */
void dt_canvas_route_midpoint(const dt_canvas_route_t *route, double *x, double *y);
/**
 * @brief The point a fraction of the way along a route, by arc length: 0 its start, 1 its end.
 * @details The chord's point at that fraction when the route has no length to walk.
 */
void dt_canvas_route_point_at(const dt_canvas_route_t *route, double fraction, double *x, double *y);
/**
 * @brief How far along a route, by arc length, the route passes closest to a point.
 * @details The inverse of dt_canvas_route_point_at() for a point on the route, so a place picked
 * on a connector can be found again after the frames it joins -- or its own free ends -- have
 * moved. One half for a route with no length.
 * @return a fraction in [0, 1]
 */
double dt_canvas_route_fraction_at(const dt_canvas_route_t *route, double x, double y);
/** @brief Put a waypoint on a connector, at the middle of its current route. */
void dt_canvas_connector_add_via(dt_canvas_t *canvas, dt_canvas_object_t *connector);
/** @brief Remove a connector's waypoint. */
void dt_canvas_connector_remove_via(dt_canvas_t *canvas, dt_canvas_object_t *connector);

/* --- layout ----------------------------------------------------------------- */

typedef enum dt_canvas_layout_t
{
  DT_CANVAS_LAYOUT_GRID = 0,     ///< rows of equal cells, as many columns as fit a square
  DT_CANVAS_LAYOUT_MASONRY = 1,  ///< fixed columns of equal width, each frame under the shortest column
  DT_CANVAS_LAYOUT_ROW = 2,      ///< one row, frames scaled to the same height
  DT_CANVAS_LAYOUT_COLUMN = 3,   ///< one column, frames scaled to the same width
} dt_canvas_layout_t;

/**
 * @brief Arrange frames.
 * @param ids the object ids to arrange, in the order they should flow; NULL arranges every frame.
 * @param columns column count for masonry; ignored by the other layouts.
 * @details Rotations are reset. The gap between frames is the canvas padding. The arrangement is
 * anchored at the top-left of the box the frames currently occupy, so applying a layout does
 * not move the group elsewhere; with snapping on, that anchor and every cell land on the grid.
 */
/** The order frames are laid out in: the canvas's own draw order, or a key of the images, the lighttable's way. */
typedef enum dt_canvas_sort_t
{
  DT_CANVAS_SORT_CANVAS = 0,   ///< draw order, back to front
  DT_CANVAS_SORT_FILENAME = 1,
  DT_CANVAS_SORT_DATETIME = 2, ///< capture time
  DT_CANVAS_SORT_ID = 3,       ///< library id: import order
  DT_CANVAS_SORT_PATH = 4,     ///< folder then file name
  DT_CANVAS_SORT_LAST = 5,
} dt_canvas_sort_t;

/**
 * @brief Arrange frames -- `ids`, or every frame when NULL -- in a layout, in the order `sort`
 * gives; frames that are not images keep their draw order after the images.
 */
void dt_canvas_layout_apply(dt_canvas_t *canvas, const GArray *ids, dt_canvas_layout_t layout, int columns,
                            dt_canvas_sort_t sort);

/* --- colours ---------------------------------------------------------------- */

dt_canvas_color_t dt_canvas_color(float red, float green, float blue, float alpha);
/** @brief Parse `#rrggbb` or `#rrggbbaa`. FALSE leaves `color` untouched. */
gboolean dt_canvas_color_parse(const char *text, dt_canvas_color_t *color);
/** @brief Format as `#rrggbbaa` into `out`, which holds at least 10 bytes. */
void dt_canvas_color_format(const dt_canvas_color_t *color, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
