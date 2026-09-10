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
 * Coordinates are "canvas units": one unit is one screen pixel at zoom 1, the origin is
 * the centre of the plane, y grows downwards, and an object's `x`/`y` is the centre of its
 * frame. Rotation is in radians, clockwise on screen.
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
#define DT_CANVAS_HEADER_RESERVED 1020 ///< 1024 at format 1, minus the gutter
#define DT_CANVAS_OBJECT_RESERVED 256
#define DT_CANVAS_IMAGE_RESERVED 512
#define DT_CANVAS_TEXT_RESERVED 256
#define DT_CANVAS_CONNECTOR_RESERVED 96 ///< 128 at format 1, minus the anchors and routing (12) and the waypoint (20)

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
} dt_canvas_object_flags_t;

typedef enum dt_canvas_grid_flags_t
{
  DT_CANVAS_GRID_NONE = 0,
  DT_CANVAS_GRID_VISIBLE = 1 << 0,
  DT_CANVAS_GRID_SNAP = 1 << 1,     ///< positions and sizes round to the grid
  DT_CANVAS_SNAP_GUTTER = 1 << 2,   ///< edges land one gutter from a neighbour, or in line with it
  DT_CANVAS_SNAP_SIZE = 1 << 3,     ///< a resized frame takes a neighbour's width or height
  DT_CANVAS_SNAP_ALL = DT_CANVAS_GRID_SNAP | DT_CANVAS_SNAP_GUTTER | DT_CANVAS_SNAP_SIZE,
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

typedef struct dt_canvas_text_t
{
  char font[DT_CANVAS_FONT_LEN];  ///< a Pango font description, empty for the canvas default
  dt_canvas_color_t text_color;
  dt_canvas_color_t background;
  uint32_t source;                ///< dt_canvas_text_source_t
  uint32_t linked_object;         ///< image object id for a sidecar frame, 0 otherwise
  float padding;                  ///< inner margin in canvas units
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

/** Where on a frame a connector attaches. The cardinal points are the frame's own, so
 * they rotate with it; AUTO picks, of the four, the one nearest the other end's frame. */
typedef enum dt_canvas_anchor_t
{
  DT_CANVAS_ANCHOR_AUTO = 0,
  DT_CANVAS_ANCHOR_NORTH = 1,
  DT_CANVAS_ANCHOR_EAST = 2,
  DT_CANVAS_ANCHOR_SOUTH = 3,
  DT_CANVAS_ANCHOR_WEST = 4,
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
  uint8_t reserved[DT_CANVAS_CONNECTOR_RESERVED];
} dt_canvas_connector_t;

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
  uint8_t reserved[DT_CANVAS_OBJECT_RESERVED];
  union
  {
    dt_canvas_image_t image;
    dt_canvas_text_t text;
    dt_canvas_connector_t connector;
  };
} dt_canvas_object_t;

typedef struct dt_canvas_t
{
  uint32_t format_version;
  char title[DT_CANVAS_TITLE_LEN];
  dt_canvas_color_t background;
  dt_canvas_color_t border_color;   ///< default border for frames without an override
  float border_width;
  float grid_size;                  ///< canvas units between grid lines
  uint32_t grid_flags;              ///< dt_canvas_grid_flags_t bits
  float gutter;                     ///< the margin frames keep from each other when snapped side by side or laid out
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

/** @brief Add a connector between two objects. Refuses self-links and unknown ids. */
dt_canvas_object_t *dt_canvas_add_connector(dt_canvas_t *canvas, uint32_t from_id, uint32_t to_id);

/**
 * @brief Remove an object.
 * @details Removing a frame also removes every connector attached to it, and unlinks any
 * sidecar text frame that showed its note (the text stays, with the last content it had).
 */
gboolean dt_canvas_remove_object(dt_canvas_t *canvas, uint32_t id);

/** @brief Duplicate a frame, offset by a little so the copy is visible. Connectors are not duplicated. */
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
                                int32_t pixel_height, uint64_t history_hash, int64_t rendered_at);

/** @brief Replace a text frame's Markdown. Copied; NULL means empty. */
void dt_canvas_text_set_markdown(dt_canvas_t *canvas, dt_canvas_object_t *object, const char *markdown);

/** @brief The Markdown of a text frame, never NULL for a text object. */
const char *dt_canvas_text_get_markdown(const dt_canvas_object_t *object);

/** @brief The font of a text frame: its own, or the canvas default when it has none. */
const char *dt_canvas_text_effective_font(const dt_canvas_t *canvas, const dt_canvas_object_t *object);

/** @brief The border a frame is drawn with: its own when overridden, the canvas default otherwise. */
void dt_canvas_object_effective_border(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_color_t *color, float *width);

/* --- geometry --------------------------------------------------------------- */

/** @brief Is this object a frame (image or text) rather than a connector? */
gboolean dt_canvas_object_is_frame(const dt_canvas_object_t *object);

/**
 * @brief The four corners of a frame after rotation, in canvas units.
 * @param corners receives x0,y0 ... x3,y3, top-left first, clockwise on screen.
 */
void dt_canvas_object_corners(const dt_canvas_object_t *object, double corners[8]);

/** @brief The axis-aligned box around the rotated frame. */
dt_canvas_rect_t dt_canvas_object_bounds(const dt_canvas_object_t *object);

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
void dt_canvas_object_anchor_point(const dt_canvas_object_t *frame, dt_canvas_anchor_t anchor, double target_x,
                                   double target_y, double *x, double *y, double *normal_x, double *normal_y);

/**
 * @brief Resolve a connector to its geometry.
 * @return FALSE when either end is missing.
 */
gboolean dt_canvas_connector_route(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route);

/**
 * @brief The two ends of a connector: its anchor points.
 * @return FALSE when either end is missing.
 */
gboolean dt_canvas_connector_endpoints(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                       double *from_x, double *from_y, double *to_x, double *to_y);

/** @brief The box around every visible frame. Empty (width 0) for an empty canvas. */
dt_canvas_rect_t dt_canvas_bounds(const dt_canvas_t *canvas);

/** @brief Round `value` to the grid when snapping is on, else return it unchanged. */
double dt_canvas_snap(const dt_canvas_t *canvas, double value);

/** @brief The object whose frame is under the point, frontmost first. NULL when none. */
dt_canvas_object_t *dt_canvas_pick(const dt_canvas_t *canvas, double x, double y, double tolerance);

/**
 * @brief Snap a moving box next to, or in line with, the other frames.
 * @details Candidates are the other frames' edges plus or minus the gutter (side by side with
 * the canvas margin) and their edges themselves (aligned). The nearest candidate within
 * `threshold` wins per axis. This is the gutter rule; it ignores the canvas's snap flags,
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
 * @brief Snap a size to another frame's width or height.
 * @details The same-size rule: the nearest other frame's width within `threshold` replaces
 * `*width`, and likewise for `*height`, each axis on its own.
 * @return TRUE when at least one dimension snapped.
 */
gboolean dt_canvas_snap_size(const dt_canvas_t *canvas, const GArray *exclude, double threshold, double *width,
                             double *height);

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
 * @details Rotations are reset. The gap between frames is the canvas gutter. The arrangement is
 * anchored at the top-left of the box the frames currently occupy, so applying a layout does
 * not move the group elsewhere; with snapping on, that anchor and every cell land on the grid.
 */
void dt_canvas_layout_apply(dt_canvas_t *canvas, const GArray *ids, dt_canvas_layout_t layout, int columns);

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
