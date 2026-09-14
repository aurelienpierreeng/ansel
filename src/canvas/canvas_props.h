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

#ifndef DT_CANVAS_CANVAS_PROPS_H
#define DT_CANVAS_CANVAS_PROPS_H

/**
 * @file canvas_props.h
 * @brief The per-object properties of a canvas, as one table and one reader and writer.
 *
 * @details Everything a person can set on one object -- a text frame's font, a picture's
 * border, a connector's arrowheads, a cutout's feather -- is described here ONCE: what it is
 * called, which kinds have it, where it is shown, what it is edited with, what range it takes
 * and what "untouched" means for it. A frontend builds its rows from the table and edits
 * through `dt_canvas_prop_write()`, so the rules of an edit live in one place instead of in
 * every widget's signal handler, and two frontends (or two layouts of one) cannot drift apart
 * on them.
 *
 * The rules that used to be spread over the property bar's handlers are the writer's now: a
 * picture that keeps its proportions answers a width with a height too; opacity is stored as
 * transparency; a text frame keeps its own background field; a font equal to the canvas's is
 * stored as none; the four insets are all literal once one is set, and all four at zero are a
 * real zero; an arrowhead is a bit; a text frame whose height follows its content is refitted.
 *
 * A property belonging to an OVERRIDE GROUP (border, corners, shadow, font) reads the value
 * the object is DRAWN with, whichever of the object and the canvas supplies it. Writing one
 * while the object still inherits seeds every field of the group from what is on screen,
 * applies the edit and sets the group's flag -- unless the value written is the inherited one,
 * in which case nothing happens at all and the object goes on inheriting. That is what lets a
 * frontend reset a control to the canvas's value without making the object own it.
 *
 * GTK-free and conf-free: nothing here raises a signal, records an undo step or writes a
 * configuration key, and nothing here touches the document's generation itself -- though a few
 * document functions the writer calls (a cutout's new shape, a connector's waypoint) do. The
 * writer reports what the caller owes instead, as effect bits, and the caller -- which owns the
 * undo stack, the renders and the configuration -- settles the bill once per gesture.
 */

#include "canvas/canvas.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every property, in table order. The order is the order rows are shown in: by section, and
 * within a section from the strip to the card's essentials to its extras. Not stored anywhere,
 * so a property may be inserted; a frontend that remembers one does so by `key`.
 */
typedef enum dt_canvas_prop_id_t
{
  DT_CANVAS_PROP_NONE = 0,
  /* text: character */
  DT_CANVAS_PROP_TEXT_FONT,
  DT_CANVAS_PROP_TEXT_SIZE,
  DT_CANVAS_PROP_TEXT_COLOR,
  DT_CANVAS_PROP_TEXT_LETTER_SPACING,
  DT_CANVAS_PROP_TEXT_FEATURES,
  /* text: paragraph */
  DT_CANVAS_PROP_TEXT_ALIGN_H,
  DT_CANVAS_PROP_TEXT_LINE_HEIGHT,
  DT_CANVAS_PROP_TEXT_FIRST_LINE_INDENT,
  DT_CANVAS_PROP_TEXT_PARAGRAPH_SPACING,
  DT_CANVAS_PROP_TEXT_OPTICAL_MARGINS,
  /* text: the box */
  DT_CANVAS_PROP_TEXT_ALIGN_V,
  DT_CANVAS_PROP_TEXT_AUTO_HEIGHT,
  DT_CANVAS_PROP_TEXT_INSET,
  DT_CANVAS_PROP_TEXT_INSET_TOP,
  DT_CANVAS_PROP_TEXT_INSET_RIGHT,
  DT_CANVAS_PROP_TEXT_INSET_BOTTOM,
  DT_CANVAS_PROP_TEXT_INSET_LEFT,
  DT_CANVAS_PROP_TEXT_WRAP,
  DT_CANVAS_PROP_TEXT_WRAP_GAP,
  /* picture */
  DT_CANVAS_PROP_IMAGE_SUMMARY,
  DT_CANVAS_PROP_IMAGE_SOURCE,
  DT_CANVAS_PROP_IMAGE_REFRESH,
  DT_CANVAS_PROP_IMAGE_NOTE,
  DT_CANVAS_PROP_IMAGE_ADD_MAP,
  /* drawing */
  DT_CANVAS_PROP_SVG_SUMMARY,
  DT_CANVAS_PROP_SVG_SOURCE,
  /* map */
  DT_CANVAS_PROP_MAP_ZOOM,
  DT_CANVAS_PROP_MAP_FETCH,
  DT_CANVAS_PROP_MAP_STYLE,
  DT_CANVAS_PROP_MAP_LATITUDE,
  DT_CANVAS_PROP_MAP_LONGITUDE,
  /* connector route */
  DT_CANVAS_PROP_CONNECTOR_ROUTING,
  DT_CANVAS_PROP_CONNECTOR_ARROW_START,
  DT_CANVAS_PROP_CONNECTOR_ARROW_END,
  DT_CANVAS_PROP_CONNECTOR_WAYPOINT,
  DT_CANVAS_PROP_CONNECTOR_REVERSE,
  /* arrange */
  DT_CANVAS_PROP_X,
  DT_CANVAS_PROP_Y,
  DT_CANVAS_PROP_WIDTH,
  DT_CANVAS_PROP_KEEP_RATIO,
  DT_CANVAS_PROP_HEIGHT,
  DT_CANVAS_PROP_ROTATION,
  /* fill */
  DT_CANVAS_PROP_OPACITY,
  DT_CANVAS_PROP_BACKGROUND,
  /* stroke: a frame's border, a connector's line */
  DT_CANVAS_PROP_BORDER_WIDTH,
  DT_CANVAS_PROP_BORDER_COLOR,
  DT_CANVAS_PROP_LINE_WIDTH,
  DT_CANVAS_PROP_LINE_COLOR,
  DT_CANVAS_PROP_LINE_DASHED,
  /* corners */
  DT_CANVAS_PROP_CORNER_RADIUS,
  /* shadow */
  DT_CANVAS_PROP_SHADOW_OFFSET_X,
  DT_CANVAS_PROP_SHADOW_OFFSET_Y,
  DT_CANVAS_PROP_SHADOW_BLUR,
  DT_CANVAS_PROP_SHADOW_COLOR,
  /* cutout */
  DT_CANVAS_PROP_CUTOUT_SHAPE,
  DT_CANVAS_PROP_CUTOUT_FEATHER,
  DT_CANVAS_PROP_CUTOUT_INVERT,
  DT_CANVAS_PROP_CUTOUT_EDIT,
  DT_CANVAS_PROP_CUTOUT_SIZE_X,
  DT_CANVAS_PROP_CUTOUT_SIZE_Y,
  DT_CANVAS_PROP_CUTOUT_ROTATION,
  DT_CANVAS_PROP_CUTOUT_CURVATURE,
  DT_CANVAS_PROP_COUNT,
} dt_canvas_prop_id_t;

/**
 * Where a property is shown. The enum order IS the screen order: the kind's own sections
 * first, then what several kinds share. A kind that lacks a section simply has no rows in it.
 */
typedef enum dt_canvas_prop_section_t
{
  DT_CANVAS_SECTION_CHARACTER = 0,
  DT_CANVAS_SECTION_PARAGRAPH,
  DT_CANVAS_SECTION_TEXT_BOX,
  DT_CANVAS_SECTION_PICTURE,
  DT_CANVAS_SECTION_DRAWING,
  DT_CANVAS_SECTION_MAP,
  DT_CANVAS_SECTION_ROUTE,
  DT_CANVAS_SECTION_ARRANGE,
  DT_CANVAS_SECTION_FILL,
  DT_CANVAS_SECTION_STROKE,
  DT_CANVAS_SECTION_CORNERS,
  DT_CANVAS_SECTION_SHADOW,
  DT_CANVAS_SECTION_CUTOUT,
  DT_CANVAS_SECTION_COUNT,
} dt_canvas_prop_section_t;

/** How far a person has to reach for a property. */
typedef enum dt_canvas_prop_tier_t
{
  DT_CANVAS_TIER_STRIP = 0,  ///< always at hand, beside the object: the kind's everyday controls
  DT_CANVAS_TIER_ESSENTIAL,  ///< in its section, as soon as the section is open
  DT_CANVAS_TIER_MORE,       ///< in its section, after the rule: what an expert reaches for
} dt_canvas_prop_tier_t;

/**
 * What kind of value a property is, which decides the control it gets. The same nature is
 * the same control wherever the property is shown, so a row never changes shape by moving.
 */
typedef enum dt_canvas_prop_widget_t
{
  DT_CANVAS_WIDGET_TUNE = 0,  ///< a bounded, perceptual number: dragged until it looks right
  DT_CANVAS_WIDGET_MEASURE,   ///< an exact number, typed: a position, a size, a type size
  DT_CANVAS_WIDGET_ICONS,     ///< one of a few choices, each with a glyph
  DT_CANVAS_WIDGET_ICON_FLAG, ///< one switch with a glyph
  DT_CANVAS_WIDGET_CHOICE,    ///< one of a list of named choices
  DT_CANVAS_WIDGET_FLAG,      ///< one switch with a label
  DT_CANVAS_WIDGET_COLOR,
  DT_CANVAS_WIDGET_FONT,      ///< a Pango font description, family and style
  DT_CANVAS_WIDGET_FEATURES,  ///< the OpenType features the object's own face ships
  DT_CANVAS_WIDGET_INFO,      ///< read only: a description of the object
  DT_CANVAS_WIDGET_ACTION,    ///< a button: the write is the request, the caller carries it out
} dt_canvas_prop_widget_t;

/** The fields an object takes from the canvas until it is given its own. */
typedef enum dt_canvas_prop_group_t
{
  DT_CANVAS_GROUP_NONE = 0,
  DT_CANVAS_GROUP_BORDER, ///< DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE: width and colour
  DT_CANVAS_GROUP_CORNER, ///< DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE: the radius
  DT_CANVAS_GROUP_SHADOW, ///< DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE: the whole shadow
  DT_CANVAS_GROUP_FONT,   ///< no flag: an empty `text.font` is the canvas's font
  DT_CANVAS_GROUP_COUNT,
} dt_canvas_prop_group_t;

/** Where an override group's values come from. */
typedef enum dt_canvas_own_state_t
{
  DT_CANVAS_OWN_INHERIT = 0, ///< the canvas's
  /**
   * The object's own, and still exactly what its kind is born with: a drawing is born with no
   * border and no shadow whatever the canvas says, because a card and a rule around a logo turn
   * it into a rectangle. It owns those values, but nobody chose them.
   */
  DT_CANVAS_OWN_KIND_DEFAULT,
  DT_CANVAS_OWN_CUSTOM,      ///< the object's own, chosen
} dt_canvas_own_state_t;

/** What a write changed, and so what the caller owes. Zero means nothing changed at all. */
enum
{
  /** the document changed: touch it, record the undo step */
  DT_CANVAS_EFFECT_CHANGED = 1 << 0,
  /** geometry another text frame flows around moved: dt_canvas_props_settle_all() */
  DT_CANVAS_EFFECT_SETTLE_ALL = 1 << 1,
  /** other properties changed with it (a kept ratio, a refitted height): refill them */
  DT_CANVAS_EFFECT_COUPLED = 1 << 2,
  /** a map's tiles must be fetched again, once, when the gesture ends */
  DT_CANVAS_EFFECT_COMMIT_RENDER = 1 << 3,
  /** the map's settings are the next map's defaults: store them, once */
  DT_CANVAS_EFFECT_COMMIT_CONF = 1 << 4,
  /** the view holds this state or carries this action: the caller acts on it */
  DT_CANVAS_EFFECT_VIEW = 1 << 5,
  /** which rows apply changed (a shape, a face, a switch another row depends on) */
  DT_CANVAS_EFFECT_RESTRUCTURE = 1 << 6,
};

/**
 * How an edit reaches the document. LIVE edits follow a control while it is being moved and
 * share one undo step; COMMIT ends the gesture and pays for what LIVE deferred -- the undo
 * record, the renders, the configuration; ONCE is a whole gesture in one go: a typed number, a
 * click. The writer does not read it; it is here so a frontend's header needs nothing else.
 */
typedef enum dt_canvas_edit_phase_t
{
  DT_CANVAS_EDIT_LIVE = 0,
  DT_CANVAS_EDIT_COMMIT,
  DT_CANVAS_EDIT_ONCE,
} dt_canvas_edit_phase_t;

/** Room in a value for a font description, a feature string or a line of description. */
#define DT_CANVAS_PROP_TEXT_LEN 512

/**
 * One property's value, whatever its nature. Only the member its nature uses is meaningful:
 * `number` for TUNE and MEASURE, `choice` for ICONS and CHOICE, `flag` for FLAG and ICON_FLAG,
 * `color` for COLOR, `text` for FONT, FEATURES and INFO. An ACTION carries nothing.
 *
 * A number is in the unit the property is SHOWN in (`unit`): percent, degrees, points. The
 * writer converts it to what the document stores, so a frontend never needs to.
 */
typedef struct dt_canvas_prop_value_t
{
  double number;
  int choice;
  gboolean flag;
  dt_canvas_color_t color;
  char text[DT_CANVAS_PROP_TEXT_LEN];
} dt_canvas_prop_value_t;

/** One property, described. Every string is untranslated (N_()); the frontend translates. */
typedef struct dt_canvas_prop_t
{
  dt_canvas_prop_id_t id;
  const char *key;          ///< a stable name, "text.line_height": what a frontend remembers a row by
  const char *label;
  const char *tooltip;
  const char *unit;         ///< what `number` is counted in, NULL when it has no unit
  uint32_t kinds;           ///< 1u << dt_canvas_object_kind_t, for every kind that has it
  dt_canvas_prop_section_t section;
  dt_canvas_prop_tier_t tier;
  dt_canvas_prop_widget_t widget;
  dt_canvas_prop_group_t group;
  double min;               ///< the hard range: nothing outside it is ever stored
  double max;
  double soft_min;          ///< the range a slider offers before it is typed past
  double soft_max;
  double step;
  double factor;            ///< the stored value per shown unit (radians per degree); 1 when it is stored as shown
  /**
   * The value that means "left as it is": a MORE row whose value is not this stays in view,
   * so a setting nobody can see is never a setting nobody knows about. NAN for a property that
   * has no such value -- a size, a position -- which is never kept in view for being set.
   */
  double neutral;
  int digits;               ///< the precision a value is shown at, and an inherited one compared at
  const char *const *choices; ///< NULL-terminated labels for ICONS and CHOICE; NULL for the map styles
  const char *const *icons;   ///< NULL-terminated glyph ids for ICONS and ICON_FLAG; the frontend maps them
  dt_canvas_prop_id_t pair_with;    ///< shown side by side with this one (X with Y); symmetric
  dt_canvas_prop_id_t visible_if;   ///< the row exists only while this property's value is in `visible_values`
  uint32_t visible_values;          ///< bit `choice` for ICONS and CHOICE, bit 0 (off) or bit 1 (on) for a switch
  dt_canvas_prop_id_t sensitive_if; ///< the row is editable only while this one's value is in `sensitive_values`
  uint32_t sensitive_values;
} dt_canvas_prop_t;

/** @brief The whole table, in screen order; `table[idx].id == idx + 1`. */
const dt_canvas_prop_t *dt_canvas_props(size_t *count);

/** @brief One property's description, or NULL for an id that names none. */
const dt_canvas_prop_t *dt_canvas_prop_get(dt_canvas_prop_id_t prop_id);

/** @brief Whether objects of this kind have the property at all. */
gboolean dt_canvas_prop_for_kind(const dt_canvas_prop_t *prop, uint32_t kind);

/**
 * @brief Whether this object shows the property: its kind has it, and the property it
 * depends on (a cutout's shape, the flow switch) currently allows it.
 */
gboolean dt_canvas_prop_applies(const dt_canvas_prop_t *prop, const dt_canvas_object_t *object);

/** @brief Whether the property can be edited on this object right now: a height that follows its text cannot. */
gboolean dt_canvas_prop_sensitive(const dt_canvas_prop_t *prop, const dt_canvas_t *canvas,
                                  const dt_canvas_object_t *object);

/** @brief How many choices an ICONS or CHOICE property offers, the map styles included. */
int dt_canvas_prop_choice_count(const dt_canvas_prop_t *prop);

/** @brief The label of one choice: untranslated for a fixed list, the provider's own name for a map style. */
const char *dt_canvas_prop_choice_label(const dt_canvas_prop_t *prop, int choice);

/**
 * @brief A section's heading for an object of a kind, untranslated. The stroke section is a
 * frame's "Border" and a connector's "Line": the same slot, the same rows, a different word.
 */
const char *dt_canvas_prop_section_label(dt_canvas_prop_section_t section, uint32_t kind);

/** @brief The override group a section shows, for a kind; DT_CANVAS_GROUP_NONE for most. */
dt_canvas_prop_group_t dt_canvas_prop_section_group(dt_canvas_prop_section_t section, uint32_t kind);

/**
 * @brief Read the value the object is DRAWN with.
 * @details An override group's member reads the canvas's value while the object inherits it;
 * a line height left unset reads 1; a corner radius reads what was set, before the frame's own
 * size limits what is drawn. A view-held property (editing the cutout) reads nothing: `out`
 * is zeroed first, always.
 */
void dt_canvas_prop_read(const dt_canvas_t *canvas, const dt_canvas_object_t *object, dt_canvas_prop_id_t prop_id,
                         dt_canvas_prop_value_t *out);

/**
 * @brief Write one property, with every rule that comes with it.
 * @details Refuses a kind that does not have the property. A value equal to the one stored
 * changes nothing; an inherited value written while inheriting -- equal at the precision its
 * row shows -- changes nothing either, so the object goes on inheriting. Never calls dt_canvas_touch(): a
 * few document functions the writer relies on do, and the caller touches once whatever else
 * happened.
 * @return DT_CANVAS_EFFECT_* bits; 0 when nothing changed.
 */
uint32_t dt_canvas_prop_write(dt_canvas_t *canvas, dt_canvas_object_t *object, dt_canvas_prop_id_t prop_id,
                              const dt_canvas_prop_value_t *in);

/**
 * @brief Whether the property is at its `neutral` value on this object; a group member is while
 * the group is not CUSTOM.
 */
gboolean dt_canvas_prop_is_neutral(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                   dt_canvas_prop_id_t prop_id);

/** @brief Where the object's values for an override group come from. */
dt_canvas_own_state_t dt_canvas_group_state(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                            dt_canvas_prop_group_t group);

/**
 * @brief Give the object its own values for a group, or give them back to the canvas.
 * @details Owning SEEDS every field from what is drawn, then sets the flag, so nothing changes
 * on screen. Giving back clears the flag and keeps the fields, which undo restores anyway --
 * except for the font, whose flag IS its field: giving it back empties it.
 * @return DT_CANVAS_EFFECT_* bits: 0 when the state already was that. Giving a group back
 * refits a text frame whose height follows its content, and says so, as a write would.
 */
uint32_t dt_canvas_group_set_own(dt_canvas_t *canvas, dt_canvas_object_t *object, dt_canvas_prop_group_t group,
                                 gboolean take_ownership);

/** @brief One line saying where a group's values come from and what they are, translated. */
void dt_canvas_group_summary(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                             dt_canvas_prop_group_t group, char *buffer, size_t length);

/**
 * @brief Refit one text frame to its text, when its height follows its content.
 * @details At EDIT time, never at paint time: every path that changes what the text or its box
 * is owes this call, because a frame that flows around its neighbours, measured on every
 * repaint, re-measures into a different answer each frame.
 * @return TRUE when the frame's height changed.
 */
gboolean dt_canvas_props_settle(const dt_canvas_t *canvas, dt_canvas_object_t *object);

/**
 * @brief Refit every text frame whose height follows its content.
 * @details For an edit that moved geometry some OTHER frame's text flows around: a frame that
 * was moved, resized, cut or shadowed changes what its neighbours' lines have to avoid.
 * @return TRUE when any frame's height changed.
 */
gboolean dt_canvas_props_settle_all(dt_canvas_t *canvas);

/** Called once per OpenType feature a text frame's face offers a person, in the face's own order. */
typedef void (*dt_canvas_props_feature_cb)(const char *feature_tag, const char *label, const char *hint,
                                           gboolean is_on, gpointer user_data);

/**
 * @brief The features the frame's own face ships and a person may choose, and whether each is on.
 * @details Asked of the FONT, not of a table: a plain face offers nothing it has not got and
 * a rich one keeps its own stylistic sets. The features the layout engine owns are left out --
 * a checkbox overriding glyph composition breaks the text rather than styling it. `label` is
 * translated, or the tag itself when this build has no name for it; `hint` likewise.
 * @return how many were offered.
 */
uint32_t dt_canvas_props_text_features(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_props_feature_cb callback, gpointer user_data);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PROPS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
