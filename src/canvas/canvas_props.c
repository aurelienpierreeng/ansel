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

#include "canvas/canvas_props.h"

#include "canvas/canvas_paint.h"
#include "canvas/canvas_render.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <glib/gi18n.h>
#include <math.h>
#include <pango/pango.h>
#include <string.h>

#define KIND_BIT(kind) (1u << (kind))
#define KINDS_TEXT KIND_BIT(DT_CANVAS_OBJECT_TEXT)
#define KINDS_IMAGE KIND_BIT(DT_CANVAS_OBJECT_IMAGE)
#define KINDS_MAP KIND_BIT(DT_CANVAS_OBJECT_MAP)
#define KINDS_SVG KIND_BIT(DT_CANVAS_OBJECT_SVG)
#define KINDS_CONNECTOR KIND_BIT(DT_CANVAS_OBJECT_CONNECTOR)
#define KINDS_FRAMES (KINDS_TEXT | KINDS_IMAGE | KINDS_MAP | KINDS_SVG)
#define KINDS_ALL (KINDS_FRAMES | KINDS_CONNECTOR)

/** A switch's value as a bit of `visible_values`: bit 0 while it is off, bit 1 while it is on. */
#define PROP_WHEN_OFF (1u << 0)
#define PROP_WHEN_ON (1u << 1)

/** The largest coordinate a typed position or size may take: the plane is infinite, a spin button is not. */
#define PROP_PLANE_LIMIT 1.0e6

static const char *const _align_h_choices[] = { N_("Left"), N_("Centred"), N_("Right"), N_("Justified"), NULL };
static const char *const _align_h_icons[]
    = { "text_align_left", "text_align_center", "text_align_right", "text_align_justify", NULL };
static const char *const _align_v_choices[] = { N_("Top"), N_("Middle"), N_("Bottom"), NULL };
static const char *const _align_v_icons[]
    = { "text_valign_top", "text_valign_middle", "text_valign_bottom", NULL };
static const char *const _routing_choices[] = { N_("Straight"), N_("Square"), N_("Cubic spline"), NULL };
static const char *const _routing_icons[] = { "route_straight", "route_square", "route_cubic", NULL };
static const char *const _shape_choices[]
    = { N_("None"), N_("Circle"), N_("Ellipse"), N_("Polygon"), N_("Gradient"), NULL };
static const char *const _shape_icons[]
    = { "cancel", "masks_circle", "masks_ellipse", "masks_polygon", "masks_gradient", NULL };
static const char *const _arrow_start_icons[] = { "arrowhead_start", NULL };
static const char *const _arrow_end_icons[] = { "arrowhead_end", NULL };
static const char *const _waypoint_icons[] = { "waypoint", NULL };
static const char *const _reverse_icons[] = { "reverse", NULL };
static const char *const _refresh_icons[] = { "refresh", NULL };
static const char *const _link_icons[] = { "link", NULL };
static const char *const _invert_icons[] = { "masks_inverse", NULL };
static const char *const _edit_icons[] = { "masks_edit", NULL };

/** The shapes whose own radius a row edits, as `visible_values` bits of the cutout's shape. */
#define SHAPE_BIT(shape) (1u << (shape))
#define SHAPES_ANY (SHAPE_BIT(DT_CANVAS_MASK_CIRCLE) | SHAPE_BIT(DT_CANVAS_MASK_ELLIPSE) \
                    | SHAPE_BIT(DT_CANVAS_MASK_POLYGON) | SHAPE_BIT(DT_CANVAS_MASK_GRADIENT))

/*
 * The table. Its order is the screen order and every kind's rows come out of it in
 * non-decreasing section order, the strip's first within a section: test_canvas_props checks
 * both, and `id` is the row's index plus one so a lookup is an index.
 */
static const dt_canvas_prop_t _props[] = {
  /* --- text: character ------------------------------------------------------------------ */
  { .id = DT_CANVAS_PROP_TEXT_FONT, .key = "text.font", .label = N_("Font"),
    .tooltip = N_("The typeface and its style. The canvas's own font until one is chosen here."),
    .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_CHARACTER, .tier = DT_CANVAS_TIER_STRIP,
    .widget = DT_CANVAS_WIDGET_FONT, .group = DT_CANVAS_GROUP_FONT, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_TEXT_SIZE, .key = "text.size", .label = N_("Size"),
    .tooltip = N_("The type size, in points: a point is a canvas unit, so twelve points is twelve units on "
                  "every page"),
    .unit = N_("pt"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_CHARACTER,
    .tier = DT_CANVAS_TIER_STRIP,
    .widget = DT_CANVAS_WIDGET_MEASURE, .group = DT_CANVAS_GROUP_FONT, .min = 1.0, .max = 2000.0, .soft_min = 6.0,
    .soft_max = 144.0, .step = 0.5, .factor = 1.0, .neutral = NAN, .digits = 1 },
  { .id = DT_CANVAS_PROP_TEXT_COLOR, .key = "text.color", .label = N_("Colour"),
    .tooltip = N_("Text colour and opacity"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_CHARACTER,
    .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_COLOR, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_TEXT_LETTER_SPACING, .key = "text.letter_spacing", .label = N_("Letter spacing"),
    .tooltip = N_("Letter spacing in thousandths of an em, so it follows the type size: negative condenses the "
                  "line, positive opens it out. A condensed CUT is chosen in the font name instead."),
    .unit = N_("‰ em"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_CHARACTER, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = -200.0, .max = 500.0, .soft_min = -50.0, .soft_max = 200.0,
    .step = 5.0, .factor = 1.0, .neutral = 0.0, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_FEATURES, .key = "text.features", .label = N_("OpenType features"),
    .tooltip = N_("What this font is asked to do with its own alternates: ligatures, figure styles, small "
                  "capitals. Only what the font actually ships is offered."),
    .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_CHARACTER, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_FEATURES, .factor = 1.0, .neutral = NAN },

  /* --- text: paragraph ------------------------------------------------------------------ */
  { .id = DT_CANVAS_PROP_TEXT_ALIGN_H, .key = "text.align_h", .label = N_("Alignment"),
    .tooltip = N_("Horizontal alignment"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_PARAGRAPH,
    .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ICONS, .max = 3.0, .soft_max = 3.0, .step = 1.0,
    .factor = 1.0, .neutral = NAN, .choices = _align_h_choices, .icons = _align_h_icons },
  { .id = DT_CANVAS_PROP_TEXT_LINE_HEIGHT, .key = "text.line_height", .label = N_("Line height"),
    .tooltip = N_("Line height, as a multiple of what the font asks for: 1 is the font's own leading"),
    .unit = N_("×"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_PARAGRAPH, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.5, .max = 4.0, .soft_min = 0.5, .soft_max = 2.5, .step = 0.05,
    .factor = 1.0, .neutral = 1.0, .digits = 2 },
  { .id = DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT, .key = "text.first_line_indent", .label = N_("First-line indent"),
    .tooltip = N_("How far the first line of every paragraph is moved in from the measure, in canvas units. "
                  "Negative hangs it out instead, which is what a bibliography or a dictionary wants."),
    .unit = N_("pt"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_PARAGRAPH, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = -2000.0, .max = 2000.0, .soft_min = -100.0, .soft_max = 100.0,
    .step = 1.0, .factor = 1.0, .neutral = 0.0, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_PARAGRAPH_SPACING, .key = "text.paragraph_spacing", .label = N_("Paragraph spacing"),
    .tooltip = N_("Extra space before every paragraph but the first, in canvas units. A typographer sets this "
                  "INSTEAD of an indent rather than as well as one. Paragraphs are separated by a BLANK LINE, as "
                  "Markdown has it."),
    .unit = N_("pt"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_PARAGRAPH, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.0, .max = 2000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0,
    .factor = 1.0, .neutral = 0.0, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_OPTICAL_MARGINS, .key = "text.optical_margins", .label = N_("Optical margins"),
    .tooltip = N_("Hang punctuation into the margin, so the column's edge reads from the stems rather than from a "
                  "quote or a full stop"),
    .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_PARAGRAPH, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_FLAG, .max = 1.0, .factor = 1.0, .neutral = 0.0 },

  /* --- text: the box -------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_TEXT_ALIGN_V, .key = "text.align_v", .label = N_("Vertical"),
    .tooltip = N_("Vertical alignment"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_TEXT_BOX,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_ICONS, .max = 2.0, .soft_max = 2.0, .step = 1.0,
    .factor = 1.0, .neutral = NAN, .choices = _align_v_choices, .icons = _align_v_icons },
  { .id = DT_CANVAS_PROP_TEXT_AUTO_HEIGHT, .key = "text.auto_height", .label = N_("Auto height"),
    .tooltip = N_("The frame's height follows its content"), .kinds = KINDS_TEXT,
    .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_FLAG,
    .max = 1.0, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_TEXT_INSET, .key = "text.inset", .label = N_("Inset"),
    .tooltip = N_("How far the text is held off each of the frame's four edges, inside its border. What keeps a "
                  "coloured frame from having its text run into the edge."),
    .unit = N_("pt"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.0, .max = 4000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0,
    .factor = 1.0, .neutral = DT_CANVAS_TEXT_DEFAULT_PADDING, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_INSET_TOP, .key = "text.inset.top", .label = N_("Top"),
    .tooltip = N_("How far the text is held off the frame's top edge"), .unit = N_("pt"), .kinds = KINDS_TEXT,
    .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = 0.0, .max = 4000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0, .factor = 1.0,
    .neutral = DT_CANVAS_TEXT_DEFAULT_PADDING, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_INSET_RIGHT, .key = "text.inset.right", .label = N_("Right"),
    .tooltip = N_("How far the text is held off the frame's right edge"), .unit = N_("pt"), .kinds = KINDS_TEXT,
    .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = 0.0, .max = 4000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0, .factor = 1.0,
    .neutral = DT_CANVAS_TEXT_DEFAULT_PADDING, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_INSET_BOTTOM, .key = "text.inset.bottom", .label = N_("Bottom"),
    .tooltip = N_("How far the text is held off the frame's bottom edge"), .unit = N_("pt"), .kinds = KINDS_TEXT,
    .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = 0.0, .max = 4000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0, .factor = 1.0,
    .neutral = DT_CANVAS_TEXT_DEFAULT_PADDING, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_INSET_LEFT, .key = "text.inset.left", .label = N_("Left"),
    .tooltip = N_("How far the text is held off the frame's left edge"), .unit = N_("pt"), .kinds = KINDS_TEXT,
    .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = 0.0, .max = 4000.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0, .factor = 1.0,
    .neutral = DT_CANVAS_TEXT_DEFAULT_PADDING, .digits = 0 },
  { .id = DT_CANVAS_PROP_TEXT_WRAP, .key = "text.wrap", .label = N_("Flow around frames"),
    .tooltip = N_("Flow the text around the frames laid over it, following what each of them actually draws "
                  "rather than the box around it"),
    .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_FLAG, .max = 1.0, .factor = 1.0, .neutral = 0.0 },
  { .id = DT_CANVAS_PROP_TEXT_WRAP_GAP, .key = "text.wrap_gap", .label = N_("Gap"),
    .tooltip = N_("The clear space the text keeps around whatever it flows past, in canvas units"),
    .unit = N_("pt"), .kinds = KINDS_TEXT, .section = DT_CANVAS_SECTION_TEXT_BOX, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.0, .max = 500.0, .soft_min = 0.0, .soft_max = 100.0, .step = 1.0,
    .factor = 1.0, .neutral = NAN, .digits = 0, .visible_if = DT_CANVAS_PROP_TEXT_WRAP,
    .visible_values = PROP_WHEN_ON },

  /* --- picture -------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_IMAGE_SUMMARY, .key = "image.summary", .label = N_("Picture"),
    .tooltip = N_("The source file, its size and whether the render still matches its development"),
    .kinds = KINDS_IMAGE, .section = DT_CANVAS_SECTION_PICTURE, .tier = DT_CANVAS_TIER_STRIP,
    .widget = DT_CANVAS_WIDGET_INFO, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_IMAGE_SOURCE, .key = "image.source", .label = N_("Film roll"),
    .tooltip = N_("The folder the source image was in when it was rendered"), .kinds = KINDS_IMAGE,
    .section = DT_CANVAS_SECTION_PICTURE, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_INFO,
    .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_IMAGE_REFRESH, .key = "image.refresh", .label = N_("Refresh from the library"),
    .tooltip = N_("Render the picture again from its development in the library"), .kinds = KINDS_IMAGE,
    .section = DT_CANVAS_SECTION_PICTURE, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_ACTION,
    .factor = 1.0, .neutral = NAN, .icons = _refresh_icons },
  { .id = DT_CANVAS_PROP_IMAGE_NOTE, .key = "image.note", .label = N_("Show the image's text note"),
    .tooltip = N_("Add a text frame beside the picture, showing its sidecar note and following it"),
    .kinds = KINDS_IMAGE, .section = DT_CANVAS_SECTION_PICTURE, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_ACTION, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_IMAGE_ADD_MAP, .key = "image.add_map", .label = N_("Add a map of where it was taken"),
    .tooltip = N_("Add a map frame centred on the place the picture was taken"), .kinds = KINDS_IMAGE,
    .section = DT_CANVAS_SECTION_PICTURE, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_ACTION,
    .factor = 1.0, .neutral = NAN },

  /* --- drawing -------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_SVG_SUMMARY, .key = "svg.summary", .label = N_("Drawing"),
    .tooltip = N_("The drawing's file and the size it states for itself"), .kinds = KINDS_SVG,
    .section = DT_CANVAS_SECTION_DRAWING, .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_INFO,
    .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_SVG_SOURCE, .key = "svg.source", .label = N_("Source"),
    .tooltip = N_("Where the drawing was read from, and is read again from"), .kinds = KINDS_SVG,
    .section = DT_CANVAS_SECTION_DRAWING, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_INFO,
    .factor = 1.0, .neutral = NAN },

  /* --- map ------------------------------------------------------------------------------ */
  { .id = DT_CANVAS_PROP_MAP_ZOOM, .key = "map.zoom", .label = N_("Zoom"),
    .tooltip = N_("Zoom level, 1 (the world) to 19 (a street)"), .kinds = KINDS_MAP,
    .section = DT_CANVAS_SECTION_MAP, .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_MEASURE, .min = 1.0,
    .max = 19.0, .soft_min = 1.0, .soft_max = 19.0, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 0 },
  { .id = DT_CANVAS_PROP_MAP_FETCH, .key = "map.fetch", .label = N_("Fetch"),
    .tooltip = N_("Fetch the tiles again"), .kinds = KINDS_MAP, .section = DT_CANVAS_SECTION_MAP,
    .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ACTION, .factor = 1.0, .neutral = NAN,
    .icons = _refresh_icons },
  { .id = DT_CANVAS_PROP_MAP_STYLE, .key = "map.style", .label = N_("Style"),
    .tooltip = N_("Map style and provider"), .kinds = KINDS_MAP, .section = DT_CANVAS_SECTION_MAP,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_CHOICE, .step = 1.0, .factor = 1.0,
    .neutral = NAN },
  { .id = DT_CANVAS_PROP_MAP_LATITUDE, .key = "map.latitude", .label = N_("Latitude"),
    .tooltip = N_("Latitude of the map's centre, degrees"), .unit = N_("°"), .kinds = KINDS_MAP,
    .section = DT_CANVAS_SECTION_MAP, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_MEASURE,
    .min = -85.0, .max = 85.0, .soft_min = -85.0, .soft_max = 85.0, .step = 0.0001, .factor = 1.0, .neutral = NAN,
    .digits = 5, .pair_with = DT_CANVAS_PROP_MAP_LONGITUDE },
  { .id = DT_CANVAS_PROP_MAP_LONGITUDE, .key = "map.longitude", .label = N_("Longitude"),
    .tooltip = N_("Longitude of the map's centre, degrees"), .unit = N_("°"), .kinds = KINDS_MAP,
    .section = DT_CANVAS_SECTION_MAP, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_MEASURE,
    .min = -180.0, .max = 180.0, .soft_min = -180.0, .soft_max = 180.0, .step = 0.0001, .factor = 1.0,
    .neutral = NAN, .digits = 5, .pair_with = DT_CANVAS_PROP_MAP_LATITUDE },

  /* --- connector route ------------------------------------------------------------------ */
  { .id = DT_CANVAS_PROP_CONNECTOR_ROUTING, .key = "connector.routing", .label = N_("Route"),
    .tooltip = N_("How the connector travels between its two ends"), .kinds = KINDS_CONNECTOR,
    .section = DT_CANVAS_SECTION_ROUTE, .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ICONS, .max = 2.0,
    .soft_max = 2.0, .step = 1.0, .factor = 1.0, .neutral = NAN, .choices = _routing_choices,
    .icons = _routing_icons },
  { .id = DT_CANVAS_PROP_CONNECTOR_ARROW_START, .key = "connector.arrow_start", .label = N_("Arrow at the start"),
    .tooltip = N_("An arrowhead where the connector starts"), .kinds = KINDS_CONNECTOR,
    .section = DT_CANVAS_SECTION_ROUTE, .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ICON_FLAG,
    .max = 1.0, .factor = 1.0, .neutral = NAN, .icons = _arrow_start_icons },
  { .id = DT_CANVAS_PROP_CONNECTOR_ARROW_END, .key = "connector.arrow_end", .label = N_("Arrow at the end"),
    .tooltip = N_("An arrowhead where the connector ends"), .kinds = KINDS_CONNECTOR,
    .section = DT_CANVAS_SECTION_ROUTE, .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ICON_FLAG,
    .max = 1.0, .factor = 1.0, .neutral = NAN, .icons = _arrow_end_icons },
  { .id = DT_CANVAS_PROP_CONNECTOR_WAYPOINT, .key = "connector.waypoint", .label = N_("Waypoint"),
    .tooltip = N_("Add a point the connector passes by, to go around other frames. Drag it into place."),
    .kinds = KINDS_CONNECTOR, .section = DT_CANVAS_SECTION_ROUTE, .tier = DT_CANVAS_TIER_STRIP,
    .widget = DT_CANVAS_WIDGET_ICON_FLAG, .max = 1.0, .factor = 1.0, .neutral = NAN, .icons = _waypoint_icons },
  { .id = DT_CANVAS_PROP_CONNECTOR_REVERSE, .key = "connector.reverse", .label = N_("Reverse"),
    .tooltip = N_("Swap the start and the end"), .kinds = KINDS_CONNECTOR, .section = DT_CANVAS_SECTION_ROUTE,
    .tier = DT_CANVAS_TIER_STRIP, .widget = DT_CANVAS_WIDGET_ACTION, .factor = 1.0, .neutral = NAN,
    .icons = _reverse_icons },

  /* --- arrange -------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_X, .key = "arrange.x", .label = N_("X"),
    .tooltip = N_("Horizontal position of the centre, canvas units"), .unit = N_("pt"), .kinds = KINDS_FRAMES,
    .section = DT_CANVAS_SECTION_ARRANGE, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_MEASURE,
    .min = -PROP_PLANE_LIMIT, .max = PROP_PLANE_LIMIT, .soft_min = -PROP_PLANE_LIMIT, .soft_max = PROP_PLANE_LIMIT,
    .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 0, .pair_with = DT_CANVAS_PROP_Y },
  { .id = DT_CANVAS_PROP_Y, .key = "arrange.y", .label = N_("Y"),
    .tooltip = N_("Vertical position of the centre, canvas units"), .unit = N_("pt"), .kinds = KINDS_FRAMES,
    .section = DT_CANVAS_SECTION_ARRANGE, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_MEASURE,
    .min = -PROP_PLANE_LIMIT, .max = PROP_PLANE_LIMIT, .soft_min = -PROP_PLANE_LIMIT, .soft_max = PROP_PLANE_LIMIT,
    .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 0, .pair_with = DT_CANVAS_PROP_X },
  { .id = DT_CANVAS_PROP_WIDTH, .key = "arrange.width", .label = N_("W"), .tooltip = N_("Width, canvas units"),
    .unit = N_("pt"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_ARRANGE,
    .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_MEASURE, .min = 1.0, .max = PROP_PLANE_LIMIT, .soft_min = 1.0,
    .soft_max = PROP_PLANE_LIMIT, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 0,
    .pair_with = DT_CANVAS_PROP_HEIGHT },
  { .id = DT_CANVAS_PROP_KEEP_RATIO, .key = "arrange.keep_ratio", .label = N_("Proportions"),
    .tooltip = N_("Keep the frame's shape when it is resized, so a picture or a drawing is never stretched. Off, "
                  "the two sides move independently."),
    .kinds = KINDS_IMAGE | KINDS_SVG, .section = DT_CANVAS_SECTION_ARRANGE, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_ICON_FLAG, .max = 1.0, .factor = 1.0, .neutral = NAN, .icons = _link_icons },
  { .id = DT_CANVAS_PROP_HEIGHT, .key = "arrange.height", .label = N_("H"), .tooltip = N_("Height, canvas units"),
    .unit = N_("pt"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_ARRANGE,
    .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_MEASURE, .min = 1.0, .max = PROP_PLANE_LIMIT, .soft_min = 1.0,
    .soft_max = PROP_PLANE_LIMIT, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 0,
    .pair_with = DT_CANVAS_PROP_WIDTH, .sensitive_if = DT_CANVAS_PROP_TEXT_AUTO_HEIGHT,
    .sensitive_values = PROP_WHEN_OFF },
  { .id = DT_CANVAS_PROP_ROTATION, .key = "arrange.rotation", .label = N_("Angle"),
    .tooltip = N_("Rotation, degrees clockwise"), .unit = N_("°"), .kinds = KINDS_FRAMES,
    .section = DT_CANVAS_SECTION_ARRANGE, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_MEASURE,
    .min = -360.0, .max = 360.0, .soft_min = -180.0, .soft_max = 180.0, .step = 1.0, .factor = G_PI / 180.0,
    .neutral = 0.0, .digits = 1 },

  /* --- fill ----------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_OPACITY, .key = "fill.opacity", .label = N_("Opacity"), .tooltip = N_("Opacity, percent"),
    .unit = N_("%"), .kinds = KINDS_ALL, .section = DT_CANVAS_SECTION_FILL, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.0, .max = 100.0, .soft_min = 0.0, .soft_max = 100.0, .step = 5.0,
    .factor = 1.0, .neutral = 100.0, .digits = 0 },
  { .id = DT_CANVAS_PROP_BACKGROUND, .key = "fill.background", .label = N_("Background"),
    .tooltip = N_("Colour under the content, filling the frame or the whole cutout: what a feather dissolves "
                  "into. Its own opacity, at zero, lets the canvas show through."),
    .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_FILL, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_COLOR, .factor = 1.0, .neutral = NAN },

  /* --- stroke --------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_BORDER_WIDTH, .key = "stroke.border_width", .label = N_("Width"),
    .tooltip = N_("Border width, in canvas units. A rectangular frame's border sits inside its edge; a cut-out "
                  "frame's starts past the feather, outward."),
    .unit = N_("pt"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_STROKE, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .group = DT_CANVAS_GROUP_BORDER, .min = 0.0, .max = 500.0, .soft_min = 0.0,
    .soft_max = 50.0, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 1 },
  { .id = DT_CANVAS_PROP_BORDER_COLOR, .key = "stroke.border_color", .label = N_("Colour"),
    .tooltip = N_("Border colour and opacity"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_STROKE,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_COLOR, .group = DT_CANVAS_GROUP_BORDER,
    .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_LINE_WIDTH, .key = "stroke.line_width", .label = N_("Width"),
    .tooltip = N_("Line width, in canvas units"), .unit = N_("pt"), .kinds = KINDS_CONNECTOR,
    .section = DT_CANVAS_SECTION_STROKE, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = 1.0, .max = DT_CANVAS_LINE_WIDTH_MAX, .soft_min = 1.0, .soft_max = 20.0, .step = 1.0, .factor = 1.0,
    .neutral = NAN, .digits = 1 },
  { .id = DT_CANVAS_PROP_LINE_COLOR, .key = "stroke.line_color", .label = N_("Colour"),
    .tooltip = N_("Line colour and opacity"), .kinds = KINDS_CONNECTOR, .section = DT_CANVAS_SECTION_STROKE,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_COLOR, .factor = 1.0, .neutral = NAN },
  { .id = DT_CANVAS_PROP_LINE_DASHED, .key = "stroke.line_dashed", .label = N_("Dashed"),
    .tooltip = N_("Draw the line in dashes"), .kinds = KINDS_CONNECTOR, .section = DT_CANVAS_SECTION_STROKE,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_FLAG, .max = 1.0, .factor = 1.0, .neutral = NAN },

  /* --- corners -------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_CORNER_RADIUS, .key = "corners.radius", .label = N_("Radius"),
    .tooltip = N_("Radius of the frame's rounded corners, in canvas units: 0 is square. Never drawn past half the "
                  "frame's shorter side."),
    .unit = N_("pt"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CORNERS,
    .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .group = DT_CANVAS_GROUP_CORNER, .min = 0.0, .max = 5000.0, .soft_min = 0.0,
    .soft_max = 200.0, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 1 },

  /* --- shadow --------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_SHADOW_OFFSET_X, .key = "shadow.offset_x", .label = N_("Offset X"),
    .tooltip = N_("Shadow offset to the right, in canvas units"), .unit = N_("pt"), .kinds = KINDS_ALL,
    .section = DT_CANVAS_SECTION_SHADOW, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_TUNE,
    .group = DT_CANVAS_GROUP_SHADOW, .min = -500.0, .max = 500.0, .soft_min = -50.0, .soft_max = 50.0, .step = 1.0,
    .factor = 1.0, .neutral = NAN, .digits = 1, .pair_with = DT_CANVAS_PROP_SHADOW_OFFSET_Y },
  { .id = DT_CANVAS_PROP_SHADOW_OFFSET_Y, .key = "shadow.offset_y", .label = N_("Offset Y"),
    .tooltip = N_("Shadow offset downwards, in canvas units"), .unit = N_("pt"), .kinds = KINDS_ALL,
    .section = DT_CANVAS_SECTION_SHADOW, .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_TUNE,
    .group = DT_CANVAS_GROUP_SHADOW, .min = -500.0, .max = 500.0, .soft_min = -50.0, .soft_max = 50.0, .step = 1.0,
    .factor = 1.0, .neutral = NAN, .digits = 1, .pair_with = DT_CANVAS_PROP_SHADOW_OFFSET_X },
  { .id = DT_CANVAS_PROP_SHADOW_BLUR, .key = "shadow.blur", .label = N_("Blur"),
    .tooltip = N_("Shadow radius, in canvas units: 0 is no shadow, positive drops it outside the object, negative "
                  "casts it inside along the edges"),
    .unit = N_("pt"), .kinds = KINDS_ALL, .section = DT_CANVAS_SECTION_SHADOW, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .group = DT_CANVAS_GROUP_SHADOW, .min = -500.0, .max = 500.0,
    .soft_min = -50.0, .soft_max = 100.0, .step = 1.0, .factor = 1.0, .neutral = NAN, .digits = 1 },
  { .id = DT_CANVAS_PROP_SHADOW_COLOR, .key = "shadow.color", .label = N_("Colour"),
    .tooltip = N_("Shadow colour and strength"), .kinds = KINDS_ALL, .section = DT_CANVAS_SECTION_SHADOW,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_COLOR, .group = DT_CANVAS_GROUP_SHADOW,
    .factor = 1.0, .neutral = NAN },

  /* --- cutout --------------------------------------------------------------------------- */
  { .id = DT_CANVAS_PROP_CUTOUT_SHAPE, .key = "cutout.shape", .label = N_("Shape"),
    .tooltip = N_("A drawn shape that cuts the frame out of its rectangle, with a fall-off past its edge"),
    .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_ICONS, .max = 4.0, .soft_max = 4.0, .step = 1.0, .factor = 1.0, .neutral = 0.0,
    .choices = _shape_choices, .icons = _shape_icons },
  { .id = DT_CANVAS_PROP_CUTOUT_FEATHER, .key = "cutout.feather", .label = N_("Feather"),
    .tooltip = N_("Fall-off past the shape's edge, percent of the frame's shorter side. The wheel over the frame "
                  "changes it while editing."),
    .unit = N_("%"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.0, .max = 100.0, .soft_min = 0.0, .soft_max = 50.0, .step = 1.0,
    .factor = 0.01, .neutral = NAN, .digits = 0, .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE,
    .visible_values = SHAPE_BIT(DT_CANVAS_MASK_CIRCLE) | SHAPE_BIT(DT_CANVAS_MASK_ELLIPSE)
                      | SHAPE_BIT(DT_CANVAS_MASK_POLYGON) },
  { .id = DT_CANVAS_PROP_CUTOUT_INVERT, .key = "cutout.invert", .label = N_("Invert"),
    .tooltip = N_("Keep what is outside the shape"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT,
    .tier = DT_CANVAS_TIER_ESSENTIAL, .widget = DT_CANVAS_WIDGET_ICON_FLAG, .max = 1.0, .factor = 1.0,
    .neutral = NAN, .icons = _invert_icons, .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE,
    .visible_values = SHAPES_ANY },
  { .id = DT_CANVAS_PROP_CUTOUT_EDIT, .key = "cutout.edit", .label = N_("Edit shape"),
    .tooltip = N_("Show the shape's handles: drag them, Ctrl to keep one on a single axis; the wheel sets the "
                  "feather, Shift+wheel the opacity. The right-click menu edits the shape's nodes."),
    .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_ESSENTIAL,
    .widget = DT_CANVAS_WIDGET_ICON_FLAG, .max = 1.0, .factor = 1.0, .neutral = NAN, .icons = _edit_icons,
    .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE, .visible_values = SHAPES_ANY },
  { .id = DT_CANVAS_PROP_CUTOUT_SIZE_X, .key = "cutout.size_x", .label = N_("Size"),
    .tooltip = N_("The circle's radius, the gradient's extent, or the ellipse's horizontal radius with its "
                  "vertical one scaled in proportion, percent of the frame's shorter side"),
    .unit = N_("%"), .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.05, .max = 200.0, .soft_min = 0.5, .soft_max = 100.0, .step = 0.5,
    .factor = 0.01, .neutral = NAN, .digits = 1, .pair_with = DT_CANVAS_PROP_CUTOUT_SIZE_Y,
    .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE,
    .visible_values = SHAPE_BIT(DT_CANVAS_MASK_CIRCLE) | SHAPE_BIT(DT_CANVAS_MASK_ELLIPSE)
                      | SHAPE_BIT(DT_CANVAS_MASK_GRADIENT) },
  { .id = DT_CANVAS_PROP_CUTOUT_SIZE_Y, .key = "cutout.size_y", .label = N_("Size Y"),
    .tooltip = N_("The ellipse's vertical radius alone, which changes its proportions, percent of the frame's "
                  "shorter side"), .unit = N_("%"),
    .kinds = KINDS_FRAMES, .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_MORE,
    .widget = DT_CANVAS_WIDGET_TUNE, .min = 0.5, .max = 200.0, .soft_min = 0.5, .soft_max = 100.0, .step = 0.5,
    .factor = 0.01, .neutral = NAN, .digits = 1, .pair_with = DT_CANVAS_PROP_CUTOUT_SIZE_X,
    .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE, .visible_values = SHAPE_BIT(DT_CANVAS_MASK_ELLIPSE) },
  { .id = DT_CANVAS_PROP_CUTOUT_ROTATION, .key = "cutout.rotation", .label = N_("Rotation"),
    .tooltip = N_("The ellipse's or the gradient's rotation, degrees"), .unit = N_("°"), .kinds = KINDS_FRAMES,
    .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE,
    .min = -180.0, .max = 180.0, .soft_min = -180.0, .soft_max = 180.0, .step = 1.0, .factor = 1.0, .neutral = 0.0,
    .digits = 0, .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE,
    .visible_values = SHAPE_BIT(DT_CANVAS_MASK_ELLIPSE) | SHAPE_BIT(DT_CANVAS_MASK_GRADIENT) },
  { .id = DT_CANVAS_PROP_CUTOUT_CURVATURE, .key = "cutout.curvature", .label = N_("Curvature"),
    .tooltip = N_("How far the gradient's line bends: 0 is straight"), .kinds = KINDS_FRAMES,
    .section = DT_CANVAS_SECTION_CUTOUT, .tier = DT_CANVAS_TIER_MORE, .widget = DT_CANVAS_WIDGET_TUNE, .min = -2.0,
    .max = 2.0, .soft_min = -2.0, .soft_max = 2.0, .step = 0.05, .factor = 1.0, .neutral = 0.0, .digits = 2,
    .visible_if = DT_CANVAS_PROP_CUTOUT_SHAPE, .visible_values = SHAPE_BIT(DT_CANVAS_MASK_GRADIENT) },
};

G_STATIC_ASSERT(G_N_ELEMENTS(_props) == DT_CANVAS_PROP_COUNT - 1);

const dt_canvas_prop_t *dt_canvas_props(size_t *count)
{
  if(!IS_NULL_PTR(count)) *count = G_N_ELEMENTS(_props);
  return _props;
}

const dt_canvas_prop_t *dt_canvas_prop_get(const dt_canvas_prop_id_t prop_id)
{
  if(prop_id <= DT_CANVAS_PROP_NONE || prop_id >= DT_CANVAS_PROP_COUNT) return NULL;
  return &_props[prop_id - 1];
}

gboolean dt_canvas_prop_for_kind(const dt_canvas_prop_t *prop, const uint32_t kind)
{
  if(IS_NULL_PTR(prop) || kind >= 32u) return FALSE;
  return (prop->kinds & KIND_BIT(kind)) != 0;
}

/** A switch's or a choice's value as the bit `visible_values` and `sensitive_values` test. */
static uint32_t _value_bit(const dt_canvas_prop_t *prop, const dt_canvas_prop_value_t *value)
{
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_FLAG:
    case DT_CANVAS_WIDGET_ICON_FLAG:
      return value->flag ? PROP_WHEN_ON : PROP_WHEN_OFF;
    case DT_CANVAS_WIDGET_ICONS:
    case DT_CANVAS_WIDGET_CHOICE:
      return (value->choice >= 0 && value->choice < 32) ? (1u << value->choice) : 0u;
    default:
      return 0u;
  }
}

/**
 * Whether the property a row depends on currently lets it through. The properties rows depend
 * on are the object's own switches and choices, which read the same with no canvas, so the
 * caller need not have one; a property of another kind puts no condition on this one.
 */
static gboolean _condition_holds(const dt_canvas_object_t *object, const dt_canvas_prop_id_t condition,
                                 const uint32_t allowed)
{
  const dt_canvas_prop_t *controller = dt_canvas_prop_get(condition);
  if(IS_NULL_PTR(controller)) return TRUE;
  if(!dt_canvas_prop_for_kind(controller, object->kind)) return TRUE;
  dt_canvas_prop_value_t value;
  dt_canvas_prop_read(NULL, object, condition, &value);
  return (_value_bit(controller, &value) & allowed) != 0;
}

gboolean dt_canvas_prop_applies(const dt_canvas_prop_t *prop, const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(object)) return FALSE;
  if(!dt_canvas_prop_for_kind(prop, object->kind)) return FALSE;
  return _condition_holds(object, prop->visible_if, prop->visible_values);
}

gboolean dt_canvas_prop_sensitive(const dt_canvas_prop_t *prop, const dt_canvas_t *canvas,
                                  const dt_canvas_object_t *object)
{
  if(!dt_canvas_prop_applies(prop, object)) return FALSE;
  return _condition_holds(object, prop->sensitive_if, prop->sensitive_values);
}

int dt_canvas_prop_choice_count(const dt_canvas_prop_t *prop)
{
  if(IS_NULL_PTR(prop)) return 0;
  if(prop->id == DT_CANVAS_PROP_MAP_STYLE) return dt_canvas_map_source_count();
  if(IS_NULL_PTR(prop->choices)) return 0;
  int count = 0;
  while(!IS_NULL_PTR(prop->choices[count])) count++;
  return count;
}

const char *dt_canvas_prop_choice_label(const dt_canvas_prop_t *prop, const int choice)
{
  if(choice < 0 || choice >= dt_canvas_prop_choice_count(prop)) return NULL;
  if(prop->id == DT_CANVAS_PROP_MAP_STYLE) return dt_canvas_map_source_name(choice);
  return prop->choices[choice];
}

const char *dt_canvas_prop_section_label(const dt_canvas_prop_section_t section, const uint32_t kind)
{
  switch(section)
  {
    case DT_CANVAS_SECTION_CHARACTER:
      return N_("Character");
    case DT_CANVAS_SECTION_PARAGRAPH:
      return N_("Paragraph");
    case DT_CANVAS_SECTION_TEXT_BOX:
      return N_("Text box");
    case DT_CANVAS_SECTION_PICTURE:
      return N_("Picture");
    case DT_CANVAS_SECTION_DRAWING:
      return N_("Drawing");
    case DT_CANVAS_SECTION_MAP:
      return N_("Map");
    case DT_CANVAS_SECTION_ROUTE:
      return N_("Route");
    case DT_CANVAS_SECTION_ARRANGE:
      return N_("Arrange");
    case DT_CANVAS_SECTION_FILL:
      return N_("Fill");
    case DT_CANVAS_SECTION_STROKE:
      return kind == DT_CANVAS_OBJECT_CONNECTOR ? N_("Line") : N_("Border");
    case DT_CANVAS_SECTION_CORNERS:
      return N_("Corners");
    case DT_CANVAS_SECTION_SHADOW:
      return N_("Shadow");
    case DT_CANVAS_SECTION_CUTOUT:
      return N_("Cutout");
    default:
      return NULL;
  }
}

/**
 * What each section is called where the value outlives the build that wrote it -- a configuration
 * file. The labels above are translated, and the enum's order is the screen's, which is free to
 * change; neither can name a section in a file read years later, so these names do, and they never
 * move. The assert is what makes a section added to the enum arrive with a name of its own rather
 * than take its neighbour's.
 */
static const char *const _section_names[] = {
  "character", "paragraph", "text_box", "picture", "drawing", "map", "route",
  "arrange",   "fill",      "stroke",   "corners", "shadow",  "cutout",
};

G_STATIC_ASSERT(G_N_ELEMENTS(_section_names) == DT_CANVAS_SECTION_COUNT);

const char *dt_canvas_prop_section_name(const dt_canvas_prop_section_t section)
{
  if((int)section < 0 || section >= DT_CANVAS_SECTION_COUNT) return NULL;
  return _section_names[section];
}

dt_canvas_prop_group_t dt_canvas_prop_section_group(const dt_canvas_prop_section_t section, const uint32_t kind)
{
  switch(section)
  {
    case DT_CANVAS_SECTION_CHARACTER:
      return kind == DT_CANVAS_OBJECT_TEXT ? DT_CANVAS_GROUP_FONT : DT_CANVAS_GROUP_NONE;
    case DT_CANVAS_SECTION_STROKE:
      // A connector's line has no canvas default to inherit: it is always its own.
      return kind == DT_CANVAS_OBJECT_CONNECTOR ? DT_CANVAS_GROUP_NONE : DT_CANVAS_GROUP_BORDER;
    case DT_CANVAS_SECTION_CORNERS:
      return DT_CANVAS_GROUP_CORNER;
    case DT_CANVAS_SECTION_SHADOW:
      return DT_CANVAS_GROUP_SHADOW;
    default:
      return DT_CANVAS_GROUP_NONE;
  }
}

/* --- comparing values ----------------------------------------------------------------------- */

/** Two numbers are the same value when they show the same at the property's own precision. */
static gboolean _numbers_equal(const dt_canvas_prop_t *prop, const double first, const double second)
{
  const double quantum = pow(10.0, -(double)prop->digits);
  return fabs(first - second) < quantum * 0.5;
}

/**
 * Two numbers are the same STORED value when they differ by less than a hundredth of the step
 * the row shows: below that is a float's rounding, not anything a person typed. Showing the same
 * digits is not enough -- a 100 typed over a dragged 100.4 is how two frames are snapped level.
 */
static gboolean _numbers_same(const dt_canvas_prop_t *prop, const double first, const double second)
{
  const double quantum = pow(10.0, -(double)prop->digits);
  return fabs(first - second) < quantum * 0.01;
}

/** Two colours are the same when they land on the same 8-bit codes: all a colour chooser can tell apart. */
static gboolean _colors_equal(const dt_canvas_color_t *first, const dt_canvas_color_t *second)
{
  return lround(first->red * 255.0f) == lround(second->red * 255.0f)
         && lround(first->green * 255.0f) == lround(second->green * 255.0f)
         && lround(first->blue * 255.0f) == lround(second->blue * 255.0f)
         && lround(first->alpha * 255.0f) == lround(second->alpha * 255.0f);
}

static double _clamp_number(const dt_canvas_prop_t *prop, const double number)
{
  if(isnan(number)) return prop->min;
  return CLAMP(number, prop->min, prop->max);
}

/* --- the font ----------------------------------------------------------------------------- */

/** The type size a font description asks for, in points; the default font's when it names none. */
static double _font_size_points(const dt_canvas_t *canvas, const char *font)
{
  PangoFontDescription *description = pango_font_description_from_string(IS_NULL_PTR(font) ? "" : font);
  double points = 0.0;
  if(!IS_NULL_PTR(description) && (pango_font_description_get_set_fields(description) & PANGO_FONT_MASK_SIZE))
    points = (double)pango_font_description_get_size(description) / PANGO_SCALE;
  if(!IS_NULL_PTR(description)) pango_font_description_free(description);
  if(points > 0.0) return points;
  // A description without a size is drawn at whatever the context defaults to, which is not
  // ours to know: the default font's size is the honest guess, and twelve the last resort.
  const char *fallback = dt_canvas_text_effective_font(canvas, NULL);
  if(g_strcmp0(fallback, font) == 0) return 12.0;
  PangoFontDescription *fallback_description = pango_font_description_from_string(fallback);
  if(!IS_NULL_PTR(fallback_description)
     && (pango_font_description_get_set_fields(fallback_description) & PANGO_FONT_MASK_SIZE))
    points = (double)pango_font_description_get_size(fallback_description) / PANGO_SCALE;
  if(!IS_NULL_PTR(fallback_description)) pango_font_description_free(fallback_description);
  return points > 0.0 ? points : 12.0;
}

/**
 * Store a font description on a text frame. A font equal to the canvas's is stored as NONE
 * of its own, which is what keeps a frame following the canvas's font after it was merely
 * reselected: the comparison is Pango's, so "Sans 12" and "Sans Regular 12" are one font.
 * @return TRUE when the stored field changed.
 */
static gboolean _store_font(const dt_canvas_t *canvas, dt_canvas_object_t *object,
                            const PangoFontDescription *wanted)
{
  gchar *spelled = pango_font_description_to_string(wanted);
  PangoFontDescription *canvas_font
      = pango_font_description_from_string(dt_canvas_text_effective_font(canvas, NULL));
  const gboolean inherits = pango_font_description_equal(wanted, canvas_font);
  pango_font_description_free(canvas_font);
  const char *stored = inherits ? "" : spelled;
  const gboolean changed = g_strcmp0(object->text.font, stored) != 0;
  if(changed) g_strlcpy(object->text.font, stored, sizeof(object->text.font));
  dt_free(spelled);
  return changed;
}

/* --- the insets --------------------------------------------------------------------------- */

/**
 * Store four inner margins. All four literal, since any one of them set makes all four
 * literal; and all four at zero is a real zero, so the uniform padding they would otherwise
 * fall back to is zeroed with them -- or the frame's birth inset comes back the moment the
 * last side reaches nothing.
 */
static void _store_margins(dt_canvas_object_t *object, const double margins[4])
{
  gboolean all_zero = TRUE;
  for(int side = 0; side < 4; side++)
  {
    object->text.margins[side] = (float)fmax(margins[side], 0.0);
    all_zero = all_zero && !(object->text.margins[side] > 0.0f);
  }
  if(all_zero) object->text.padding = 0.0f;
}

static int _inset_side(const dt_canvas_prop_id_t prop_id)
{
  switch(prop_id)
  {
    case DT_CANVAS_PROP_TEXT_INSET_TOP:
      return DT_CANVAS_TEXT_MARGIN_TOP;
    case DT_CANVAS_PROP_TEXT_INSET_RIGHT:
      return DT_CANVAS_TEXT_MARGIN_RIGHT;
    case DT_CANVAS_PROP_TEXT_INSET_BOTTOM:
      return DT_CANVAS_TEXT_MARGIN_BOTTOM;
    case DT_CANVAS_PROP_TEXT_INSET_LEFT:
      return DT_CANVAS_TEXT_MARGIN_LEFT;
    default:
      return -1;
  }
}

/* --- override groups ------------------------------------------------------------------------- */

static gboolean _group_for_object(const dt_canvas_object_t *object, const dt_canvas_prop_group_t group)
{
  if(IS_NULL_PTR(object)) return FALSE;
  switch(group)
  {
    case DT_CANVAS_GROUP_BORDER:
    case DT_CANVAS_GROUP_CORNER:
      return dt_canvas_object_is_frame(object);
    case DT_CANVAS_GROUP_SHADOW:
      return TRUE;
    case DT_CANVAS_GROUP_FONT:
      return object->kind == DT_CANVAS_OBJECT_TEXT;
    default:
      return FALSE;
  }
}

static uint32_t _group_flag(const dt_canvas_prop_group_t group)
{
  switch(group)
  {
    case DT_CANVAS_GROUP_BORDER:
      return DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
    case DT_CANVAS_GROUP_CORNER:
      return DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
    case DT_CANVAS_GROUP_SHADOW:
      return DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
    default:
      return 0u;
  }
}

static gboolean _group_owned(const dt_canvas_object_t *object, const dt_canvas_prop_group_t group)
{
  if(group == DT_CANVAS_GROUP_FONT) return object->text.font[0] != '\0';
  return (object->flags & _group_flag(group)) != 0;
}

dt_canvas_own_state_t dt_canvas_group_state(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                            const dt_canvas_prop_group_t group)
{
  if(!_group_for_object(object, group)) return DT_CANVAS_OWN_INHERIT;
  if(!_group_owned(object, group)) return DT_CANVAS_OWN_INHERIT;
  // Only a drawing is born owning anything, and what it owns is nothing: no border, no shadow.
  // Those values are its kind's, not a choice, until they are edited.
  if(object->kind == DT_CANVAS_OBJECT_SVG)
  {
    if(group == DT_CANVAS_GROUP_BORDER && !(object->border_width > 0.0f)) return DT_CANVAS_OWN_KIND_DEFAULT;
    if(group == DT_CANVAS_GROUP_SHADOW && object->shadow.offset_x == 0.0f && object->shadow.offset_y == 0.0f
       && object->shadow.blur == 0.0f && object->shadow.color.red == 0.0f && object->shadow.color.green == 0.0f
       && object->shadow.color.blue == 0.0f && object->shadow.color.alpha == 0.0f)
      return DT_CANVAS_OWN_KIND_DEFAULT;
  }
  return DT_CANVAS_OWN_CUSTOM;
}

uint32_t dt_canvas_group_set_own(dt_canvas_t *canvas, dt_canvas_object_t *object,
                                 const dt_canvas_prop_group_t group, const gboolean take_ownership)
{
  if(!_group_for_object(object, group)) return 0u;
  const gboolean owned = _group_owned(object, group);
  if(owned == take_ownership) return 0u;
  if(!take_ownership)
  {
    uint32_t effects = DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_SETTLE_ALL;
    // The font has no flag of its own: an empty field is the inheritance, and giving it back is
    // another face, whose features are other rows. Any other group keeps its fields: they are
    // what the object would get back, and undo restores them anyway.
    if(group == DT_CANVAS_GROUP_FONT)
    {
      object->text.font[0] = '\0';
      effects = DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_RESTRUCTURE;
    }
    else
    {
      object->flags &= ~_group_flag(group);
    }
    // What is drawn changed -- another face, another border -- so a frame whose height follows its
    // text is refitted here, exactly as a write that made the same change would refit it.
    if(dt_canvas_props_settle(canvas, object))
      effects |= DT_CANVAS_EFFECT_COUPLED | DT_CANVAS_EFFECT_SETTLE_ALL;
    return effects;
  }
  if(group == DT_CANVAS_GROUP_FONT)
  {
    // Owning the font writes the canvas's font literally, bypassing the rule that stores an equal
    // font as none, because owning it is precisely the request to stop following the canvas.
    // The face is the one already drawn, so nothing is refitted.
    g_strlcpy(object->text.font, dt_canvas_text_effective_font(canvas, object), sizeof(object->text.font));
    return DT_CANVAS_EFFECT_CHANGED;
  }
  // Seeded from what is DRAWN, so taking ownership changes nothing on screen. The corner radius
  // is the canvas's own number, not the one the frame's size limits it to: a frame that later
  // grows must round its corners exactly as the canvas would have.
  switch(group)
  {
    case DT_CANVAS_GROUP_BORDER:
      dt_canvas_object_effective_border(canvas, NULL, &object->border_color, &object->border_width);
      break;
    case DT_CANVAS_GROUP_CORNER:
      object->corner_radius = IS_NULL_PTR(canvas) ? 0.0f : canvas->corner_radius;
      break;
    case DT_CANVAS_GROUP_SHADOW:
      dt_canvas_object_effective_shadow(canvas, NULL, &object->shadow);
      break;
    default:
      break;
  }
  object->flags |= _group_flag(group);
  return DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_SETTLE_ALL;
}

void dt_canvas_group_summary(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                             const dt_canvas_prop_group_t group, char *buffer, const size_t length)
{
  if(IS_NULL_PTR(buffer) || length == 0) return;
  buffer[0] = '\0';
  if(!_group_for_object(object, group)) return;
  const dt_canvas_own_state_t state = dt_canvas_group_state(canvas, object, group);
  if(state == DT_CANVAS_OWN_KIND_DEFAULT)
  {
    g_strlcpy(buffer, _("none (drawing)"), length);
    return;
  }
  gchar *value = NULL;
  switch(group)
  {
    case DT_CANVAS_GROUP_BORDER:
    {
      dt_canvas_color_t color;
      float width = 0.0f;
      dt_canvas_object_effective_border(canvas, object, &color, &width);
      value = width > 0.0f ? g_strdup_printf(_("%.1f pt"), width) : g_strdup(_("none"));
      break;
    }
    case DT_CANVAS_GROUP_CORNER:
    {
      const float radius = (object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE)
                               ? object->corner_radius
                               : (IS_NULL_PTR(canvas) ? 0.0f : canvas->corner_radius);
      value = radius > 0.0f ? g_strdup_printf(_("%.1f pt"), radius) : g_strdup(_("square"));
      break;
    }
    case DT_CANVAS_GROUP_SHADOW:
    {
      dt_canvas_shadow_t shadow;
      dt_canvas_object_effective_shadow(canvas, object, &shadow);
      if(!dt_canvas_shadow_visible(&shadow))
        value = g_strdup(_("none"));
      else if(shadow.blur > 0.0f)
        value = g_strdup_printf(_("blur %.1f pt"), shadow.blur);
      else
        value = g_strdup_printf(_("inset %.1f pt"), -shadow.blur);
      break;
    }
    case DT_CANVAS_GROUP_FONT:
      value = g_strdup(dt_canvas_text_effective_font(canvas, object));
      break;
    default:
      break;
  }
  if(IS_NULL_PTR(value)) return;
  gchar *line = state == DT_CANVAS_OWN_INHERIT ? g_strdup_printf(_("canvas default · %s"), value)
                                               : g_strdup_printf(_("own · %s"), value);
  g_strlcpy(buffer, line, length);
  dt_free(line);
  dt_free(value);
}

/* --- what a folded card says ------------------------------------------------------------------ */

/** Whether a property is one side of a text frame's inset, which the uniform inset speaks for. */
static gboolean _inset_side_prop(const dt_canvas_prop_id_t prop_id)
{
  return prop_id == DT_CANVAS_PROP_TEXT_INSET_TOP || prop_id == DT_CANVAS_PROP_TEXT_INSET_RIGHT
         || prop_id == DT_CANVAS_PROP_TEXT_INSET_BOTTOM || prop_id == DT_CANVAS_PROP_TEXT_INSET_LEFT;
}

/** A number as its row shows it, with its unit. */
static gchar *_summary_number(const dt_canvas_prop_t *prop, const double number)
{
  const int digits = MAX(prop->digits, 0);
  if(IS_NULL_PTR(prop->unit)) return g_strdup_printf("%.*f", digits, number);
  return g_strdup_printf("%.*f %s", digits, number, _(prop->unit));
}

/**
 * One row's value as a summary names it; NULL for a row that has nothing to say there. An extra
 * row is named by its label as well as its value, since unlike an essential one it is not the
 * row everybody expects to find in the section.
 */
static gchar *_summary_piece(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                             const dt_canvas_prop_t *prop, const gboolean extra)
{
  dt_canvas_prop_value_t value;
  dt_canvas_prop_read(canvas, object, prop->id, &value);
  gchar *piece = NULL;
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_TUNE:
    case DT_CANVAS_WIDGET_MEASURE:
      if(prop->id == DT_CANVAS_PROP_TEXT_INSET && !dt_canvas_props_inset_uniform(object))
      {
        // Four sides apart are four numbers, top first, clockwise as CSS has it.
        double margins[4];
        dt_canvas_text_margins(object, margins);
        const int digits = MAX(prop->digits, 0);
        piece = g_strdup_printf("%.*f/%.*f/%.*f/%.*f %s", digits, margins[0], digits, margins[1], digits, margins[2],
                                digits, margins[3], _(prop->unit));
      }
      else
      {
        piece = _summary_number(prop, value.number);
      }
      break;
    case DT_CANVAS_WIDGET_ICONS:
    case DT_CANVAS_WIDGET_CHOICE:
    {
      const char *label = dt_canvas_prop_choice_label(prop, value.choice);
      // A map style is the provider's own name, spelled the provider's way.
      if(!IS_NULL_PTR(label))
        piece = IS_NULL_PTR(prop->choices) ? g_strdup(label) : g_utf8_strdown(_(label), -1);
      break;
    }
    case DT_CANVAS_WIDGET_FLAG:
      // A switch that is on is named; one that is off goes without saying.
      if(value.flag) return g_utf8_strdown(_(prop->label), -1);
      return NULL;
    case DT_CANVAS_WIDGET_FEATURES:
      if(value.text[0] != '\0') return g_strdup(_(prop->label));
      return NULL;
    case DT_CANVAS_WIDGET_INFO:
      if(value.text[0] != '\0') piece = g_strdup(value.text);
      break;
    default:
      break;
  }
  if(IS_NULL_PTR(piece) || !extra) return piece;
  gchar *label = g_utf8_strdown(_(prop->label), -1);
  gchar *named = g_strdup_printf("%s %s", label, piece);
  dt_free(label);
  dt_free(piece);
  return named;
}

void dt_canvas_prop_section_summary(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                    const dt_canvas_prop_section_t section, char *buffer, const size_t length)
{
  if(IS_NULL_PTR(buffer) || length == 0) return;
  buffer[0] = '\0';
  if(IS_NULL_PTR(object) || section >= DT_CANVAS_SECTION_COUNT) return;
  const dt_canvas_prop_group_t group = dt_canvas_prop_section_group(section, object->kind);
  GString *text = g_string_new(NULL);
  if(group != DT_CANVAS_GROUP_NONE)
  {
    char line[DT_CANVAS_PROP_TEXT_LEN] = { 0 };
    dt_canvas_group_summary(canvas, object, group, line, sizeof(line));
    g_string_append(text, line);
  }
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->section != section || prop->tier == DT_CANVAS_TIER_STRIP || !dt_canvas_prop_applies(prop, object))
      continue;
    // The group's own line already says what its members hold, and the uniform inset names the sides.
    if(prop->group != DT_CANVAS_GROUP_NONE || _inset_side_prop(prop->id)) continue;
    const gboolean extra = prop->tier == DT_CANVAS_TIER_MORE;
    if(extra && dt_canvas_prop_is_neutral(canvas, object, prop->id)) continue;
    // An override section's essentials are its group's; any other section lists its own.
    if(!extra && group != DT_CANVAS_GROUP_NONE) continue;
    gchar *piece = _summary_piece(canvas, object, prop, extra);
    if(IS_NULL_PTR(piece)) continue;
    // The two halves of a pair read as one, "120, 80".
    const gboolean second_half = prop->pair_with != DT_CANVAS_PROP_NONE && prop->pair_with < prop->id
                                 && dt_canvas_prop_applies(dt_canvas_prop_get(prop->pair_with), object);
    if(text->len > 0) g_string_append(text, second_half ? ", " : " · ");
    g_string_append(text, piece);
    dt_free(piece);
  }
  g_strlcpy(buffer, text->str, length);
  g_string_free(text, TRUE);
}

gboolean dt_canvas_prop_section_opens_by_itself(const dt_canvas_prop_section_t section, const uint32_t kind)
{
  return dt_canvas_prop_section_group(section, kind) == DT_CANVAS_GROUP_NONE && section != DT_CANVAS_SECTION_CUTOUT;
}

gboolean dt_canvas_prop_section_stays_open(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                           const dt_canvas_prop_section_t section)
{
  if(IS_NULL_PTR(object)) return FALSE;
  const dt_canvas_prop_group_t group = dt_canvas_prop_section_group(section, object->kind);
  if(group == DT_CANVAS_GROUP_NONE) return TRUE;
  return dt_canvas_group_state(canvas, object, group) == DT_CANVAS_OWN_CUSTOM;
}

gboolean dt_canvas_props_card_altered(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return FALSE;
  // A group is counted by its switch, which sits in the card whether or not its members do:
  // the font's are on the strip, and the font the frame chose is still the card's to point at.
  for(int section = 0; section < DT_CANVAS_SECTION_COUNT; section++)
  {
    const dt_canvas_prop_group_t group = dt_canvas_prop_section_group((dt_canvas_prop_section_t)section, object->kind);
    if(group == DT_CANVAS_GROUP_NONE || !_group_for_object(object, group)) continue;
    if(dt_canvas_group_state(canvas, object, group) == DT_CANVAS_OWN_CUSTOM) return TRUE;
  }
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_prop_t *prop = &table[idx];
    if(prop->tier == DT_CANVAS_TIER_STRIP || !dt_canvas_prop_applies(prop, object)) continue;
    if(!dt_canvas_prop_is_neutral(canvas, object, prop->id)) return TRUE;
  }
  return FALSE;
}

gboolean dt_canvas_props_inset_uniform(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return TRUE;
  // Compared as the uniform inset's writer compares a side with the value written, so that what
  // shows one inset and what writes one never disagree about a float's rounding.
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(DT_CANVAS_PROP_TEXT_INSET);
  double margins[4];
  dt_canvas_text_margins(object, margins);
  for(int side = 1; side < 4; side++)
  {
    if(!_numbers_same(prop, margins[0], margins[side])) return FALSE;
  }
  return TRUE;
}

/* --- reading -------------------------------------------------------------------------------- */

static const char *_sync_word(const dt_canvas_sync_status_t status)
{
  switch(status)
  {
    case DT_CANVAS_SYNC_CURRENT:
      return _("in sync");
    case DT_CANVAS_SYNC_STALE:
      return _("stale");
    case DT_CANVAS_SYNC_MISSING:
      return _("missing");
    case DT_CANVAS_SYNC_RENDERING:
      return _("rendering");
    default:
      return NULL;
  }
}

static void _read_info(const dt_canvas_object_t *object, const dt_canvas_prop_id_t prop_id,
                       dt_canvas_prop_value_t *out)
{
  gchar *line = NULL;
  switch(prop_id)
  {
    case DT_CANVAS_PROP_IMAGE_SUMMARY:
    {
      const char *sync = _sync_word(object->image.sync_status);
      if(IS_NULL_PTR(sync))
        line = g_strdup_printf("%s · %d×%d", object->image.filename, object->image.source_width,
                               object->image.source_height);
      else
        line = g_strdup_printf("%s · %d×%d · %s", object->image.filename, object->image.source_width,
                               object->image.source_height, sync);
      break;
    }
    case DT_CANVAS_PROP_IMAGE_SOURCE:
      line = g_strdup(object->image.folder);
      break;
    case DT_CANVAS_PROP_SVG_SUMMARY:
      line = g_strdup_printf(_("%s · %.0f×%.0f pt"), object->svg.filename, object->svg.source_width,
                             object->svg.source_height);
      break;
    case DT_CANVAS_PROP_SVG_SOURCE:
      dt_canvas_svg_path(object, out->text, sizeof(out->text));
      return;
    default:
      return;
  }
  if(IS_NULL_PTR(line)) return;
  g_strlcpy(out->text, line, sizeof(out->text));
  dt_free(line);
}

void dt_canvas_prop_read(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                         const dt_canvas_prop_id_t prop_id, dt_canvas_prop_value_t *out)
{
  if(IS_NULL_PTR(out)) return;
  memset(out, 0, sizeof(*out));
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(object) || !dt_canvas_prop_for_kind(prop, object->kind)) return;
  switch(prop_id)
  {
    case DT_CANVAS_PROP_TEXT_FONT:
      g_strlcpy(out->text, dt_canvas_text_effective_font(canvas, object), sizeof(out->text));
      break;
    case DT_CANVAS_PROP_TEXT_SIZE:
      out->number = _font_size_points(canvas, dt_canvas_text_effective_font(canvas, object));
      break;
    case DT_CANVAS_PROP_TEXT_COLOR:
      out->color = object->text.text_color;
      break;
    case DT_CANVAS_PROP_TEXT_LETTER_SPACING:
      out->number = object->text.letter_spacing;
      break;
    case DT_CANVAS_PROP_TEXT_FEATURES:
      g_strlcpy(out->text, object->text.features, sizeof(out->text));
      break;
    case DT_CANVAS_PROP_TEXT_ALIGN_H:
      out->choice = CLAMP((int)object->text.align_h, 0, 3);
      break;
    case DT_CANVAS_PROP_TEXT_LINE_HEIGHT:
      // Unset is the font's own leading, which reads as 1 rather than as nothing.
      out->number = object->text.line_height > 0.0f ? object->text.line_height : 1.0;
      break;
    case DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT:
      out->number = object->text.first_line_indent;
      break;
    case DT_CANVAS_PROP_TEXT_PARAGRAPH_SPACING:
      out->number = object->text.paragraph_spacing;
      break;
    case DT_CANVAS_PROP_TEXT_OPTICAL_MARGINS:
      out->flag = (object->text.text_flags & DT_CANVAS_TEXT_OPTICAL_MARGINS) != 0;
      break;
    case DT_CANVAS_PROP_TEXT_ALIGN_V:
      out->choice = CLAMP((int)object->text.align_v, 0, 2);
      break;
    case DT_CANVAS_PROP_TEXT_AUTO_HEIGHT:
      out->flag = (object->text.text_flags & DT_CANVAS_TEXT_AUTO_HEIGHT) != 0;
      break;
    case DT_CANVAS_PROP_TEXT_INSET:
    {
      // One number for four sides: it describes them only while they agree, and a frontend
      // shows it alone only then. The top is as good a representative as any, and a stable one.
      double margins[4];
      dt_canvas_text_margins(object, margins);
      out->number = margins[DT_CANVAS_TEXT_MARGIN_TOP];
      break;
    }
    case DT_CANVAS_PROP_TEXT_INSET_TOP:
    case DT_CANVAS_PROP_TEXT_INSET_RIGHT:
    case DT_CANVAS_PROP_TEXT_INSET_BOTTOM:
    case DT_CANVAS_PROP_TEXT_INSET_LEFT:
    {
      double margins[4];
      dt_canvas_text_margins(object, margins);
      out->number = margins[_inset_side(prop_id)];
      break;
    }
    case DT_CANVAS_PROP_TEXT_WRAP:
      out->flag = (object->text.text_flags & DT_CANVAS_TEXT_WRAP_AROUND) != 0;
      break;
    case DT_CANVAS_PROP_TEXT_WRAP_GAP:
      out->number = object->text.wrap_standoff;
      break;
    case DT_CANVAS_PROP_IMAGE_SUMMARY:
    case DT_CANVAS_PROP_IMAGE_SOURCE:
    case DT_CANVAS_PROP_SVG_SUMMARY:
    case DT_CANVAS_PROP_SVG_SOURCE:
      _read_info(object, prop_id, out);
      break;
    case DT_CANVAS_PROP_MAP_ZOOM:
      out->number = object->map.zoom;
      break;
    case DT_CANVAS_PROP_MAP_STYLE:
      out->choice = dt_canvas_map_source_index(object->map.source);
      break;
    case DT_CANVAS_PROP_MAP_LATITUDE:
      out->number = object->map.latitude;
      break;
    case DT_CANVAS_PROP_MAP_LONGITUDE:
      out->number = object->map.longitude;
      break;
    case DT_CANVAS_PROP_CONNECTOR_ROUTING:
      out->choice = CLAMP((int)object->connector.routing, 0, 2);
      break;
    case DT_CANVAS_PROP_CONNECTOR_ARROW_START:
      out->flag = (object->connector.style & DT_CANVAS_CONNECTOR_ARROW_START) != 0;
      break;
    case DT_CANVAS_PROP_CONNECTOR_ARROW_END:
      out->flag = (object->connector.style & DT_CANVAS_CONNECTOR_ARROW_END) != 0;
      break;
    case DT_CANVAS_PROP_CONNECTOR_WAYPOINT:
      out->flag = object->connector.via_count > 0;
      break;
    case DT_CANVAS_PROP_X:
      out->number = object->x;
      break;
    case DT_CANVAS_PROP_Y:
      out->number = object->y;
      break;
    case DT_CANVAS_PROP_WIDTH:
      out->number = object->width;
      break;
    case DT_CANVAS_PROP_KEEP_RATIO:
      out->flag = dt_canvas_object_keeps_ratio(object);
      break;
    case DT_CANVAS_PROP_HEIGHT:
      out->number = object->height;
      break;
    case DT_CANVAS_PROP_ROTATION:
      out->number = object->rotation / prop->factor;
      break;
    case DT_CANVAS_PROP_OPACITY:
      out->number = (1.0 - CLAMP(object->transparency, 0.0f, 1.0f)) * 100.0;
      break;
    case DT_CANVAS_PROP_BACKGROUND:
      out->color = dt_canvas_object_background(object);
      break;
    case DT_CANVAS_PROP_BORDER_WIDTH:
    case DT_CANVAS_PROP_BORDER_COLOR:
    {
      float width = 0.0f;
      dt_canvas_object_effective_border(canvas, object, &out->color, &width);
      out->number = width;
      if(prop_id == DT_CANVAS_PROP_BORDER_WIDTH) memset(&out->color, 0, sizeof(out->color));
      break;
    }
    case DT_CANVAS_PROP_LINE_WIDTH:
      out->number = object->connector.line_width;
      break;
    case DT_CANVAS_PROP_LINE_COLOR:
      out->color = object->connector.color;
      break;
    case DT_CANVAS_PROP_LINE_DASHED:
      out->flag = (object->connector.style & DT_CANVAS_CONNECTOR_DASHED) != 0;
      break;
    case DT_CANVAS_PROP_CORNER_RADIUS:
      // What was set, the object's or the canvas's -- not what the frame's size lets be drawn,
      // or a slider would jump back to half the shorter side under a pointer still holding it.
      out->number = (object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE)
                        ? object->corner_radius
                        : (IS_NULL_PTR(canvas) ? 0.0f : canvas->corner_radius);
      break;
    case DT_CANVAS_PROP_SHADOW_OFFSET_X:
    case DT_CANVAS_PROP_SHADOW_OFFSET_Y:
    case DT_CANVAS_PROP_SHADOW_BLUR:
    case DT_CANVAS_PROP_SHADOW_COLOR:
    {
      dt_canvas_shadow_t shadow;
      dt_canvas_object_effective_shadow(canvas, object, &shadow);
      if(prop_id == DT_CANVAS_PROP_SHADOW_OFFSET_X) out->number = shadow.offset_x;
      else if(prop_id == DT_CANVAS_PROP_SHADOW_OFFSET_Y) out->number = shadow.offset_y;
      else if(prop_id == DT_CANVAS_PROP_SHADOW_BLUR) out->number = shadow.blur;
      else out->color = shadow.color;
      break;
    }
    case DT_CANVAS_PROP_CUTOUT_SHAPE:
      out->choice = CLAMP((int)object->mask.shape, 0, DT_CANVAS_MASK_GRADIENT);
      break;
    case DT_CANVAS_PROP_CUTOUT_FEATHER:
      out->number = object->mask.feather / prop->factor;
      break;
    case DT_CANVAS_PROP_CUTOUT_INVERT:
      out->flag = (object->mask.flags & DT_CANVAS_MASK_INVERT) != 0;
      break;
    case DT_CANVAS_PROP_CUTOUT_SIZE_X:
      out->number = object->mask.radius_x / prop->factor;
      break;
    case DT_CANVAS_PROP_CUTOUT_SIZE_Y:
      out->number = object->mask.radius_y / prop->factor;
      break;
    case DT_CANVAS_PROP_CUTOUT_ROTATION:
      out->number = object->mask.rotation;
      break;
    case DT_CANVAS_PROP_CUTOUT_CURVATURE:
      out->number = object->mask.radius_y;
      break;
    default:
      // The actions carry nothing, and editing the cutout is the view's state, not the document's.
      break;
  }
}

void dt_canvas_prop_read_inherited(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                   const dt_canvas_prop_id_t prop_id, dt_canvas_prop_value_t *out)
{
  if(IS_NULL_PTR(out)) return;
  memset(out, 0, sizeof(*out));
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(object) || !dt_canvas_prop_for_kind(prop, object->kind)) return;
  // Asked of the resolvers with no object, which is what they answer for a group the object does
  // not own: the reader and this one cannot then disagree about where the canvas's value lives.
  switch(prop_id)
  {
    case DT_CANVAS_PROP_TEXT_FONT:
      g_strlcpy(out->text, dt_canvas_text_effective_font(canvas, NULL), sizeof(out->text));
      return;
    case DT_CANVAS_PROP_TEXT_SIZE:
      out->number = _font_size_points(canvas, dt_canvas_text_effective_font(canvas, NULL));
      return;
    case DT_CANVAS_PROP_BORDER_WIDTH:
    case DT_CANVAS_PROP_BORDER_COLOR:
    {
      float width = 0.0f;
      dt_canvas_object_effective_border(canvas, NULL, &out->color, &width);
      out->number = width;
      if(prop_id == DT_CANVAS_PROP_BORDER_WIDTH) memset(&out->color, 0, sizeof(out->color));
      return;
    }
    case DT_CANVAS_PROP_CORNER_RADIUS:
      out->number = IS_NULL_PTR(canvas) ? 0.0f : canvas->corner_radius;
      return;
    case DT_CANVAS_PROP_SHADOW_OFFSET_X:
    case DT_CANVAS_PROP_SHADOW_OFFSET_Y:
    case DT_CANVAS_PROP_SHADOW_BLUR:
    case DT_CANVAS_PROP_SHADOW_COLOR:
    {
      dt_canvas_shadow_t shadow;
      dt_canvas_object_effective_shadow(canvas, NULL, &shadow);
      if(prop_id == DT_CANVAS_PROP_SHADOW_OFFSET_X) out->number = shadow.offset_x;
      else if(prop_id == DT_CANVAS_PROP_SHADOW_OFFSET_Y) out->number = shadow.offset_y;
      else if(prop_id == DT_CANVAS_PROP_SHADOW_BLUR) out->number = shadow.blur;
      else out->color = shadow.color;
      return;
    }
    default:
      dt_canvas_prop_read(canvas, object, prop_id, out);
      return;
  }
}

gboolean dt_canvas_prop_is_neutral(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                   const dt_canvas_prop_id_t prop_id)
{
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(object) || !dt_canvas_prop_for_kind(prop, object->kind)) return TRUE;
  if(prop->group != DT_CANVAS_GROUP_NONE)
    return dt_canvas_group_state(canvas, object, prop->group) != DT_CANVAS_OWN_CUSTOM;
  dt_canvas_prop_value_t value;
  dt_canvas_prop_read(canvas, object, prop_id, &value);
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_FEATURES:
      return value.text[0] == '\0';
    case DT_CANVAS_WIDGET_TUNE:
    case DT_CANVAS_WIDGET_MEASURE:
      if(isnan(prop->neutral)) return TRUE;
      if(prop_id == DT_CANVAS_PROP_TEXT_INSET)
      {
        // Four sides are neutral together or not at all.
        double margins[4];
        dt_canvas_text_margins(object, margins);
        for(int side = 0; side < 4; side++)
        {
          if(!_numbers_equal(prop, margins[side], prop->neutral)) return FALSE;
        }
        return TRUE;
      }
      return _numbers_equal(prop, value.number, prop->neutral);
    case DT_CANVAS_WIDGET_FLAG:
    case DT_CANVAS_WIDGET_ICON_FLAG:
      if(isnan(prop->neutral)) return TRUE;
      return value.flag == (prop->neutral != 0.0);
    case DT_CANVAS_WIDGET_ICONS:
    case DT_CANVAS_WIDGET_CHOICE:
      if(isnan(prop->neutral)) return TRUE;
      return value.choice == (int)lround(prop->neutral);
    default:
      return TRUE;
  }
}

/* --- writing -------------------------------------------------------------------------------- */

/**
 * Whether a write asks for what is already there. An override group's member reads what is
 * drawn, so the same test answers both "the object already has this" and "this is what the
 * canvas gives it": either way nothing is written, and an inheriting object goes on inheriting.
 *
 * An inherited number is compared at the precision its row shows, because a frontend resetting
 * a control to the canvas's value rounds it there, and the reset must leave the object
 * inheriting. A number the object holds itself is compared to what is stored.
 */
static gboolean _unchanged(const dt_canvas_prop_t *prop, const dt_canvas_prop_value_t *current,
                           const dt_canvas_prop_value_t *wanted, const gboolean inherited)
{
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_TUNE:
    case DT_CANVAS_WIDGET_MEASURE:
      if(inherited) return _numbers_equal(prop, current->number, _clamp_number(prop, wanted->number));
      return _numbers_same(prop, current->number, _clamp_number(prop, wanted->number));
    case DT_CANVAS_WIDGET_FLAG:
    case DT_CANVAS_WIDGET_ICON_FLAG:
      return (current->flag != 0) == (wanted->flag != 0);
    case DT_CANVAS_WIDGET_ICONS:
    case DT_CANVAS_WIDGET_CHOICE:
      return current->choice == wanted->choice;
    case DT_CANVAS_WIDGET_COLOR:
      return _colors_equal(&current->color, &wanted->color);
    case DT_CANVAS_WIDGET_FEATURES:
      return g_strcmp0(current->text, wanted->text) == 0;
    default:
      // A font is compared once it is spelled the way it would be stored; an action always acts.
      return FALSE;
  }
}

static void _set_bit(uint32_t *bits, const uint32_t flag_bit, const gboolean switched_on)
{
  if(switched_on)
    *bits |= flag_bit;
  else
    *bits &= ~flag_bit;
}

/** The effects of a change to a map's own settings: its tiles and the next map's defaults. */
#define MAP_EFFECTS (DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_COMMIT_RENDER | DT_CANVAS_EFFECT_COMMIT_CONF)
/** The effects of a change to what other frames' text flows around. */
#define OBSTACLE_EFFECTS (DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_SETTLE_ALL)

static uint32_t _write_text(const dt_canvas_t *canvas, dt_canvas_object_t *object, const dt_canvas_prop_t *prop,
                            const dt_canvas_prop_value_t *in)
{
  switch(prop->id)
  {
    case DT_CANVAS_PROP_TEXT_FONT:
    {
      // An empty description is the canvas's font.
      if(in->text[0] == '\0')
      {
        if(object->text.font[0] == '\0') return 0u;
        object->text.font[0] = '\0';
        return DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_RESTRUCTURE;
      }
      PangoFontDescription *wanted = pango_font_description_from_string(in->text);
      // A chooser that offers a family and a style says nothing about the size, and choosing a
      // face must not change it: the size the text is set at is kept.
      if(!(pango_font_description_get_set_fields(wanted) & PANGO_FONT_MASK_SIZE))
      {
        const double points = _font_size_points(canvas, dt_canvas_text_effective_font(canvas, object));
        pango_font_description_set_size(wanted, (gint)lround(points * PANGO_SCALE));
      }
      const gboolean changed = _store_font(canvas, object, wanted);
      pango_font_description_free(wanted);
      return changed ? (DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_RESTRUCTURE) : 0u;
    }
    case DT_CANVAS_PROP_TEXT_SIZE:
    {
      // The size lives inside the font description. Setting it on a frame that follows the
      // canvas's font writes that font out with the new size -- same family, same style -- since
      // a canvas font at another size is no longer the canvas's font.
      PangoFontDescription *wanted
          = pango_font_description_from_string(dt_canvas_text_effective_font(canvas, object));
      const double points = _clamp_number(prop, in->number);
      pango_font_description_set_size(wanted, (gint)lround(points * PANGO_SCALE));
      const gboolean changed = _store_font(canvas, object, wanted);
      pango_font_description_free(wanted);
      return changed ? DT_CANVAS_EFFECT_CHANGED : 0u;
    }
    case DT_CANVAS_PROP_TEXT_COLOR:
      object->text.text_color = in->color;
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_LETTER_SPACING:
      object->text.letter_spacing = (float)_clamp_number(prop, in->number);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_FEATURES:
      g_strlcpy(object->text.features, in->text, sizeof(object->text.features));
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_ALIGN_H:
      object->text.align_h = (uint32_t)CLAMP(in->choice, 0, 3);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_LINE_HEIGHT:
      object->text.line_height = (float)_clamp_number(prop, in->number);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT:
      object->text.first_line_indent = (float)_clamp_number(prop, in->number);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_PARAGRAPH_SPACING:
      object->text.paragraph_spacing = (float)_clamp_number(prop, in->number);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_OPTICAL_MARGINS:
      _set_bit(&object->text.text_flags, DT_CANVAS_TEXT_OPTICAL_MARGINS, in->flag);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_ALIGN_V:
      object->text.align_v = (uint32_t)CLAMP(in->choice, 0, 2);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_TEXT_AUTO_HEIGHT:
      _set_bit(&object->text.text_flags, DT_CANVAS_TEXT_AUTO_HEIGHT, in->flag);
      // The height row stops (or starts) being editable.
      return DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_COUPLED;
    case DT_CANVAS_PROP_TEXT_INSET:
    {
      const double inset = _clamp_number(prop, in->number);
      double margins[4];
      dt_canvas_text_margins(object, margins);
      gboolean same = TRUE;
      for(int side = 0; side < 4; side++)
      {
        same = same && _numbers_same(prop, margins[side], inset);
        margins[side] = inset;
      }
      // Four sides that already agree on it: the one number read above said so only for the top.
      if(same) return 0u;
      _store_margins(object, margins);
      return DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_COUPLED;
    }
    case DT_CANVAS_PROP_TEXT_INSET_TOP:
    case DT_CANVAS_PROP_TEXT_INSET_RIGHT:
    case DT_CANVAS_PROP_TEXT_INSET_BOTTOM:
    case DT_CANVAS_PROP_TEXT_INSET_LEFT:
    {
      // Every side is written, not just this one: the others keep what they showed, literally.
      double margins[4];
      dt_canvas_text_margins(object, margins);
      margins[_inset_side(prop->id)] = _clamp_number(prop, in->number);
      _store_margins(object, margins);
      return DT_CANVAS_EFFECT_CHANGED | DT_CANVAS_EFFECT_COUPLED;
    }
    case DT_CANVAS_PROP_TEXT_WRAP:
      _set_bit(&object->text.text_flags, DT_CANVAS_TEXT_WRAP_AROUND, in->flag);
      return OBSTACLE_EFFECTS | DT_CANVAS_EFFECT_RESTRUCTURE;
    case DT_CANVAS_PROP_TEXT_WRAP_GAP:
      object->text.wrap_standoff = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    default:
      return 0u;
  }
}

static uint32_t _write_map(dt_canvas_object_t *object, const dt_canvas_prop_t *prop,
                           const dt_canvas_prop_value_t *in)
{
  switch(prop->id)
  {
    case DT_CANVAS_PROP_MAP_ZOOM:
      object->map.zoom = (int32_t)lround(_clamp_number(prop, in->number));
      return MAP_EFFECTS;
    case DT_CANVAS_PROP_MAP_FETCH:
      // Nothing about the document changes: the tiles are fetched again, and nothing is undone.
      return DT_CANVAS_EFFECT_COMMIT_RENDER;
    case DT_CANVAS_PROP_MAP_STYLE:
    {
      const int count = dt_canvas_map_source_count();
      if(in->choice < 0 || in->choice >= count) return 0u;
      object->map.source = dt_canvas_map_source_id(in->choice);
      return MAP_EFFECTS;
    }
    case DT_CANVAS_PROP_MAP_LATITUDE:
      object->map.latitude = _clamp_number(prop, in->number);
      return MAP_EFFECTS;
    case DT_CANVAS_PROP_MAP_LONGITUDE:
      object->map.longitude = _clamp_number(prop, in->number);
      return MAP_EFFECTS;
    default:
      return 0u;
  }
}

/**
 * The effects of a change to how a connector is drawn. A LINE's style is what the next line is drawn
 * with, so the caller is asked to remember it, once. A connector holding a frame is always born with
 * the defaults and teaches the next line nothing -- both ends free is the whole test, not one end, or
 * a connector left half free by a hand-edited file would style every line drawn after it.
 */
static uint32_t _line_style_effects(const dt_canvas_object_t *object)
{
  const uint32_t remembered = dt_canvas_connector_is_line(object) ? DT_CANVAS_EFFECT_COMMIT_CONF : 0u;
  return DT_CANVAS_EFFECT_CHANGED | remembered;
}

static uint32_t _write_connector(dt_canvas_t *canvas, dt_canvas_object_t *object, const dt_canvas_prop_t *prop,
                                 const dt_canvas_prop_value_t *in)
{
  switch(prop->id)
  {
    case DT_CANVAS_PROP_CONNECTOR_ROUTING:
    {
      object->connector.routing = (uint32_t)CLAMP(in->choice, 0, 2);
      // A free end with an automatic tangent leaves toward the other end, so a line made cubic
      // would follow its own chord and look as straight as it was. It is bent into an arc instead;
      // the seed leaves a steered tangent alone, so a curve made straight and cubic again comes back
      // with the bend it had. Writing the value a connector already holds never gets here.
      dt_canvas_route_t route;
      if(object->connector.routing == DT_CANVAS_ROUTING_CUBIC && dt_canvas_connector_route(canvas, object, &route))
        dt_canvas_connector_seed_curve(object, &route);
      return DT_CANVAS_EFFECT_CHANGED;
    }
    case DT_CANVAS_PROP_CONNECTOR_ARROW_START:
      _set_bit(&object->connector.style, DT_CANVAS_CONNECTOR_ARROW_START, in->flag);
      return _line_style_effects(object);
    case DT_CANVAS_PROP_CONNECTOR_ARROW_END:
      _set_bit(&object->connector.style, DT_CANVAS_CONNECTOR_ARROW_END, in->flag);
      return _line_style_effects(object);
    case DT_CANVAS_PROP_CONNECTOR_WAYPOINT:
      if(in->flag)
        dt_canvas_connector_add_via(canvas, object);
      else
        dt_canvas_connector_remove_via(canvas, object);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_CONNECTOR_REVERSE:
    {
      const uint32_t from_id = object->connector.from_id;
      const uint32_t from_anchor = object->connector.from_anchor;
      object->connector.from_id = object->connector.to_id;
      object->connector.from_anchor = object->connector.to_anchor;
      object->connector.to_id = from_id;
      object->connector.to_anchor = from_anchor;
      // A free end carries its place and its tangent with it: the id alone would send a line's
      // start to where its end was while leaving its point behind.
      const double from_x = object->connector.from_x;
      const double from_y = object->connector.from_y;
      object->connector.from_x = object->connector.to_x;
      object->connector.from_y = object->connector.to_y;
      object->connector.to_x = from_x;
      object->connector.to_y = from_y;
      const float from_tangent_x = object->connector.from_tangent_x;
      const float from_tangent_y = object->connector.from_tangent_y;
      object->connector.from_tangent_x = object->connector.to_tangent_x;
      object->connector.from_tangent_y = object->connector.to_tangent_y;
      object->connector.to_tangent_x = from_tangent_x;
      object->connector.to_tangent_y = from_tangent_y;
      // An anchored end's handle length belongs to that end, not to the start or the finish, so it
      // follows its end across the swap; left behind, the curve bends at the wrong frame.
      const float from_reach = object->connector.from_reach;
      object->connector.from_reach = object->connector.to_reach;
      object->connector.to_reach = from_reach;
      // The waypoint's tangent points toward the finish, and the finish is now the other end: the
      // same handle read the other way round is its negation.
      object->connector.via_tangent_x = -object->connector.via_tangent_x;
      object->connector.via_tangent_y = -object->connector.via_tangent_y;
      return DT_CANVAS_EFFECT_CHANGED;
    }
    case DT_CANVAS_PROP_LINE_WIDTH:
      object->connector.line_width = (float)_clamp_number(prop, in->number);
      return _line_style_effects(object);
    case DT_CANVAS_PROP_LINE_COLOR:
      object->connector.color = in->color;
      return _line_style_effects(object);
    case DT_CANVAS_PROP_LINE_DASHED:
      _set_bit(&object->connector.style, DT_CANVAS_CONNECTOR_DASHED, in->flag);
      return _line_style_effects(object);
    default:
      return 0u;
  }
}

/** A width or a height: a frame that keeps its proportions answers one with the other. */
static uint32_t _write_size(dt_canvas_object_t *object, const dt_canvas_prop_t *prop,
                            const dt_canvas_prop_value_t *in)
{
  const double typed = _clamp_number(prop, in->number);
  uint32_t effects = OBSTACLE_EFFECTS;
  // A typed size obeys the same rule the drag does, or the two fight: whichever side was edited
  // leads, and the other follows the shape the frame is keeping.
  if(dt_canvas_object_keeps_ratio(object) && object->width > 0.0 && object->height > 0.0)
  {
    const double ratio = object->width / object->height;
    if(prop->id == DT_CANVAS_PROP_HEIGHT)
    {
      object->height = typed;
      object->width = fmax(typed * ratio, 1.0);
    }
    else
    {
      object->width = typed;
      object->height = fmax(typed / ratio, 1.0);
    }
    effects |= DT_CANVAS_EFFECT_COUPLED;
  }
  else if(prop->id == DT_CANVAS_PROP_HEIGHT)
  {
    object->height = typed;
  }
  else
  {
    object->width = typed;
  }
  // A map's tiles are fetched at its size, so a new size wants new tiles; a move does not.
  if(object->kind == DT_CANVAS_OBJECT_MAP) effects |= DT_CANVAS_EFFECT_COMMIT_RENDER;
  return effects;
}

static uint32_t _write_shared(dt_canvas_t *canvas, dt_canvas_object_t *object, const dt_canvas_prop_t *prop,
                              const dt_canvas_prop_value_t *in)
{
  if(prop->group != DT_CANVAS_GROUP_NONE)
  {
    // Editing one field of a group the object inherits makes the object own the whole group,
    // seeded from what is drawn, so the fields not edited stay exactly as they were on screen.
    dt_canvas_group_set_own(canvas, object, prop->group, TRUE);
  }
  switch(prop->id)
  {
    case DT_CANVAS_PROP_X:
      object->x = _clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_Y:
      object->y = _clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_WIDTH:
    case DT_CANVAS_PROP_HEIGHT:
      return _write_size(object, prop, in);
    case DT_CANVAS_PROP_KEEP_RATIO:
      _set_bit(&object->flags, DT_CANVAS_OBJECT_FLAG_FREE_RATIO, !in->flag);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_ROTATION:
      object->rotation = _clamp_number(prop, in->number) * prop->factor;
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_OPACITY:
      // Stored as transparency, so an older file's zeros mean opaque.
      object->transparency = 1.0f - (float)(_clamp_number(prop, in->number) / 100.0);
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_BACKGROUND:
      // A text frame keeps its own field for it; the other frames share one.
      if(object->kind == DT_CANVAS_OBJECT_TEXT)
        object->text.background = in->color;
      else
        object->background = in->color;
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_BORDER_WIDTH:
      object->border_width = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_BORDER_COLOR:
      object->border_color = in->color;
      return DT_CANVAS_EFFECT_CHANGED;
    case DT_CANVAS_PROP_CORNER_RADIUS:
      object->corner_radius = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_SHADOW_OFFSET_X:
      object->shadow.offset_x = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_SHADOW_OFFSET_Y:
      object->shadow.offset_y = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_SHADOW_BLUR:
      object->shadow.blur = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_SHADOW_COLOR:
      // Its strength decides whether it is there at all, and so whether text keeps off it.
      object->shadow.color = in->color;
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_CUTOUT_SHAPE:
      dt_canvas_mask_set_shape(canvas, object, (uint32_t)CLAMP(in->choice, 0, DT_CANVAS_MASK_GRADIENT));
      // Which rows apply follows the shape, and the view enters or leaves editing it.
      return OBSTACLE_EFFECTS | DT_CANVAS_EFFECT_VIEW | DT_CANVAS_EFFECT_RESTRUCTURE;
    case DT_CANVAS_PROP_CUTOUT_FEATHER:
      object->mask.feather = (float)CLAMP(_clamp_number(prop, in->number) * prop->factor, 0.0, 1.0);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_CUTOUT_INVERT:
      _set_bit(&object->mask.flags, DT_CANVAS_MASK_INVERT, in->flag);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_CUTOUT_EDIT:
      return DT_CANVAS_EFFECT_VIEW;
    case DT_CANVAS_PROP_CUTOUT_SIZE_X:
    {
      const double size = _clamp_number(prop, in->number) * prop->factor;
      // The gradient's extent is a fraction of the frame; a radius may reach past it.
      if(object->mask.shape == DT_CANVAS_MASK_GRADIENT)
      {
        object->mask.radius_x = (float)CLAMP(size, 0.0005, 1.0);
        return OBSTACLE_EFFECTS;
      }
      const double previous_radius = object->mask.radius_x;
      object->mask.radius_x = (float)CLAMP(size, 0.005, 2.0);
      if(object->mask.shape != DT_CANVAS_MASK_ELLIPSE || !(previous_radius > 0.0)) return OBSTACLE_EFFECTS;
      // The size of an ellipse scales it whole, so it keeps its proportions; the vertical radius alone
      // is what changes them. Nothing else resizes an ellipse in proportion -- a handle drags one radius,
      // the wheel sets the feather -- and the context menu's Size, which did, is gone.
      const double scale = object->mask.radius_x / previous_radius;
      object->mask.radius_y = (float)CLAMP(object->mask.radius_y * scale, 0.005, 2.0);
      return OBSTACLE_EFFECTS | DT_CANVAS_EFFECT_COUPLED;
    }
    case DT_CANVAS_PROP_CUTOUT_SIZE_Y:
      object->mask.radius_y = (float)CLAMP(_clamp_number(prop, in->number) * prop->factor, 0.005, 2.0);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_CUTOUT_ROTATION:
      object->mask.rotation = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    case DT_CANVAS_PROP_CUTOUT_CURVATURE:
      object->mask.radius_y = (float)_clamp_number(prop, in->number);
      return OBSTACLE_EFFECTS;
    default:
      return 0u;
  }
}

uint32_t dt_canvas_prop_write(dt_canvas_t *canvas, dt_canvas_object_t *object, const dt_canvas_prop_id_t prop_id,
                              const dt_canvas_prop_value_t *in)
{
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(object) || IS_NULL_PTR(in)) return 0u;
  if(!dt_canvas_prop_for_kind(prop, object->kind)) return 0u;
  if(prop->widget == DT_CANVAS_WIDGET_INFO) return 0u;
  // The uniform inset compares all four sides itself: the one number it reads is the top's.
  if(prop->widget != DT_CANVAS_WIDGET_ACTION && prop_id != DT_CANVAS_PROP_CUTOUT_EDIT
     && prop_id != DT_CANVAS_PROP_TEXT_INSET)
  {
    dt_canvas_prop_value_t current;
    dt_canvas_prop_read(canvas, object, prop_id, &current);
    const gboolean inherited = prop->group != DT_CANVAS_GROUP_NONE
                               && dt_canvas_group_state(canvas, object, prop->group) == DT_CANVAS_OWN_INHERIT;
    if(_unchanged(prop, &current, in, inherited)) return 0u;
  }

  uint32_t effects = 0u;
  switch(prop->section)
  {
    case DT_CANVAS_SECTION_CHARACTER:
    case DT_CANVAS_SECTION_PARAGRAPH:
    case DT_CANVAS_SECTION_TEXT_BOX:
      effects = _write_text(canvas, object, prop, in);
      break;
    case DT_CANVAS_SECTION_PICTURE:
      // Refreshing, the note and the map of the place are the view's to carry out.
      effects = DT_CANVAS_EFFECT_VIEW;
      break;
    case DT_CANVAS_SECTION_MAP:
      effects = _write_map(object, prop, in);
      break;
    case DT_CANVAS_SECTION_ROUTE:
      effects = _write_connector(canvas, object, prop, in);
      break;
    case DT_CANVAS_SECTION_STROKE:
      effects = object->kind == DT_CANVAS_OBJECT_CONNECTOR ? _write_connector(canvas, object, prop, in)
                                                           : _write_shared(canvas, object, prop, in);
      break;
    default:
      effects = _write_shared(canvas, object, prop, in);
      break;
  }

  // A text frame whose height follows its content is refitted at edit time and never at paint
  // time: nearly anything about it -- its font, its insets, its width, where it sits among the
  // frames it flows around -- can change how tall its text is.
  if((effects & DT_CANVAS_EFFECT_CHANGED) && dt_canvas_props_settle(canvas, object))
    effects |= DT_CANVAS_EFFECT_COUPLED | DT_CANVAS_EFFECT_SETTLE_ALL;
  return effects;
}

/* --- settling and the features list -------------------------------------------------------- */

gboolean dt_canvas_props_settle(const dt_canvas_t *canvas, dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return FALSE;
  if(!(object->text.text_flags & DT_CANVAS_TEXT_AUTO_HEIGHT)) return FALSE;
  return dt_canvas_paint_text_fit_height(canvas, object);
}

gboolean dt_canvas_props_settle_all(dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return FALSE;
  gboolean changed = FALSE;
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    if(dt_canvas_props_settle(canvas, dt_canvas_object_at(canvas, idx))) changed = TRUE;
  }
  return changed;
}

uint32_t dt_canvas_props_text_features(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       const dt_canvas_props_feature_cb callback, gpointer user_data)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT || IS_NULL_PTR(callback)) return 0u;
  char tags[DT_CANVAS_TEXT_FEATURE_LIST_MAX][DT_CANVAS_FONT_FEATURE_TAG_LEN];
  const uint32_t count = dt_canvas_paint_text_font_features(canvas, object, tags, DT_CANVAS_TEXT_FEATURE_LIST_MAX);
  uint32_t offered = 0u;
  for(uint32_t idx = 0; idx < count; idx++)
  {
    if(!dt_canvas_text_feature_offered(tags[idx])) continue;
    gchar *label = dt_canvas_text_feature_label(tags[idx]);
    gchar *hint = dt_canvas_text_feature_hint(tags[idx]);
    // A tag with no name is offered by its tag: that is how a font's own stylistic sets stay reachable.
    gchar *tooltip = IS_NULL_PTR(hint) ? g_strdup_printf(_("The font's own \"%s\" feature"), tags[idx]) : hint;
    callback(tags[idx], IS_NULL_PTR(label) ? tags[idx] : label, tooltip,
             dt_canvas_text_feature_is_on(object->text.features, tags[idx]), user_data);
    dt_free(tooltip);
    dt_free(label);
    offered++;
  }
  return offered;
}

gboolean dt_canvas_click_sequence_press(dt_canvas_click_sequence_t *sequence, const dt_canvas_click_t *click,
                                        const guint delay_ms, const guint distance_px, const uint32_t shown_id)
{
  if(IS_NULL_PTR(sequence) || IS_NULL_PTR(click)) return FALSE;
  const dt_canvas_click_t *last = &sequence->last;
  // The difference of two unsigned timestamps is right across their wrap, where a signed one
  // would read a pause of seven weeks.
  const guint32 gap_ms = click->time_ms - last->time_ms;
  const gboolean first_press = last->button == 0;
  // GDK's own test for a second press, condition for condition: the same button, strictly
  // sooner than the delay, and within the distance along each axis separately.
  const gboolean paired = !first_press && click->button == last->button && gap_ms < delay_ms
                          && fabs(click->x - last->x) <= (double)distance_px
                          && fabs(click->y - last->y) <= (double)distance_px;
  sequence->last = *click;
  sequence->press_count++;
  sequence->previous_shown_id = sequence->latest_shown_id;
  sequence->latest_shown_id = shown_id;
  sequence->previous_took_handle = sequence->latest_took_handle;
  sequence->latest_took_handle = FALSE;
  if(paired) return FALSE;
  sequence->answered = FALSE;
  return TRUE;
}

void dt_canvas_click_sequence_took_handle(dt_canvas_click_sequence_t *sequence)
{
  if(IS_NULL_PTR(sequence)) return;
  sequence->latest_took_handle = TRUE;
}

gboolean dt_canvas_click_sequence_began_on_handle(const dt_canvas_click_sequence_t *sequence)
{
  if(IS_NULL_PTR(sequence)) return FALSE;
  return sequence->previous_took_handle;
}

void dt_canvas_click_sequence_closed(dt_canvas_click_sequence_t *sequence)
{
  if(IS_NULL_PTR(sequence)) return;
  sequence->previous_shown_id = 0;
  sequence->latest_shown_id = 0;
}

dt_canvas_double_click_t dt_canvas_click_sequence_double(dt_canvas_click_sequence_t *sequence,
                                                         const uint32_t object_id)
{
  if(IS_NULL_PTR(sequence) || object_id == 0 || sequence->answered) return DT_CANVAS_DOUBLE_CLICK_NOTHING;
  sequence->answered = TRUE;
  // The double click GDK reports is the latest press, so its own first press is the one before:
  // drilling needs this object's properties to have been showing then, and ever since.
  if(sequence->previous_shown_id == object_id) return DT_CANVAS_DOUBLE_CLICK_DRILL;
  return DT_CANVAS_DOUBLE_CLICK_OPEN;
}

gboolean dt_canvas_props_has_content_action(const uint32_t kind)
{
  return kind == DT_CANVAS_OBJECT_TEXT || kind == DT_CANVAS_OBJECT_IMAGE || kind == DT_CANVAS_OBJECT_SVG;
}

const char *dt_canvas_props_content_action_tooltip(const uint32_t kind)
{
  switch(kind)
  {
    case DT_CANVAS_OBJECT_TEXT:
      return N_("Edit the text (Return, or double-click the frame again)");
    case DT_CANVAS_OBJECT_IMAGE:
      return N_("Develop the picture in the darkroom (Return, or double-click the frame again)");
    case DT_CANVAS_OBJECT_SVG:
      return N_("Read the drawing again from its file (Return, or double-click the frame again)");
    default:
      return NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
