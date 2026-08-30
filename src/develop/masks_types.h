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

/** @file develop/masks_types.h
 *
 * @brief The masks vocabulary: the enumerations a caller needs to NAME a shape or an operation,
 * and the value types the module hands across its boundary.
 *
 * @details This exists for the same reason colorprofiles/profile_types.h does. develop/masks.h is
 * 880 lines that drag develop/develop.h, develop/pixelpipe.h, caches/pixelpipe_cache_alloc.h,
 * common/logging.h, system/atomic.h, system/simd.h and common/times.h behind them. A translation
 * unit that only needs to say DT_MASKS_CIRCLE, or to receive a description of a shape, should not
 * have to take the pixel pipeline with it -- and while it does, it can never drop the include,
 * which is the whole objective of the enclosure (issue #1299).
 *
 * Nothing here may include anything but <glib.h>. develop/masks.h includes this file first, so
 * every existing consumer keeps compiling unchanged.
 */

#ifndef DT_DEVELOP_MASKS_TYPES_H
#define DT_DEVELOP_MASKS_TYPES_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of dt_masks_form_t.name. Named because a caller receiving a copy needs the bound, and
 * because sizeof(form->name) stops being available once the struct is not in scope. */
#define DT_MASKS_FORM_NAME_LEN 128

/** The form itself, named but not defined here.
 *
 * A caller that only passes a form along, or hands it to develop/masks_group.h to be asked about,
 * needs the name and not the layout -- which is the whole point of this header. develop/masks.h
 * completes the type for the code that genuinely manipulates it; repeating the typedef is legal C11
 * and is what lets both headers stand alone. */
typedef struct dt_masks_form_t dt_masks_form_t;

/** How many shape kinds a user can draw: circle, ellipse, polygon, brush, gradient. Vocabulary,
 * not model -- the GUIs size their shape-button arrays with it. */
#define DEVELOP_MASKS_NB_SHAPES 5

/**forms types */
typedef enum dt_masks_type_t
{
  DT_MASKS_NONE = 0, // keep first
  DT_MASKS_CIRCLE = 1 << 0,
  DT_MASKS_POLYGON = 1 << 1,
  DT_MASKS_GROUP = 1 << 2,
  DT_MASKS_CLONE = 1 << 3,
  DT_MASKS_GRADIENT = 1 << 4,
  DT_MASKS_ELLIPSE = 1 << 5,
  DT_MASKS_BRUSH = 1 << 6,
  DT_MASKS_NON_CLONE = 1 << 7,

  DT_MASKS_ALL = DT_MASKS_CIRCLE | DT_MASKS_POLYGON | DT_MASKS_GROUP |
                 DT_MASKS_GRADIENT | DT_MASKS_ELLIPSE | DT_MASKS_BRUSH,

  DT_MASKS_IS_CLOSED_SHAPE = DT_MASKS_CIRCLE | DT_MASKS_ELLIPSE | DT_MASKS_POLYGON,
  DT_MASKS_IS_OPEN_SHAPE   = DT_MASKS_ALL & ~DT_MASKS_IS_CLOSED_SHAPE,
  
  DT_MASKS_IS_RETOUCHE = DT_MASKS_CLONE | DT_MASKS_NON_CLONE,

  DT_MASKS_IS_PATH_SHAPE   = DT_MASKS_POLYGON | DT_MASKS_BRUSH,
  DT_MASKS_IS_PRIMITIVE_SHAPE = DT_MASKS_CIRCLE | DT_MASKS_ELLIPSE | DT_MASKS_GRADIENT

} dt_masks_type_t;

typedef enum dt_masks_state_t
{
  DT_MASKS_STATE_NONE = 0,
  DT_MASKS_STATE_USE = 1 << 0,
  DT_MASKS_STATE_SHOW = 1 << 1,
  DT_MASKS_STATE_INVERSE = 1 << 2,
  DT_MASKS_STATE_UNION = 1 << 3,
  DT_MASKS_STATE_INTERSECTION = 1 << 4,
  DT_MASKS_STATE_DIFFERENCE = 1 << 5,
  DT_MASKS_STATE_EXCLUSION = 1 << 6,
  DT_MASKS_STATE_NOOP = 1 << 7,

  DT_MASKS_STATE_IS_COMBINE_OP = DT_MASKS_STATE_UNION | DT_MASKS_STATE_INTERSECTION | DT_MASKS_STATE_DIFFERENCE | DT_MASKS_STATE_EXCLUSION
} dt_masks_state_t;

typedef enum dt_masks_increment_t
{
  DT_MASKS_INCREMENT_ABSOLUTE = 0,
  DT_MASKS_INCREMENT_SCALE = 1,
  DT_MASKS_INCREMENT_OFFSET = 2
} dt_masks_increment_t;

typedef enum dt_masks_edit_mode_t
{
  DT_MASKS_EDIT_OFF = 0,
  DT_MASKS_EDIT_FULL = 1,
  DT_MASKS_EDIT_RESTRICTED = 2
} dt_masks_edit_mode_t;

/*
* Type of user interaction to map with internal properties of masks.
* Those used to be deduced implicitly by each shape from Shift/Ctrl/Shift+Ctrl + mouse
* scroll, which is a shitty design when using Wacom tablets. No shape reads key modifiers
* any more: the wheel is resolved once, against the user's mapping, by
* dt_masks_scroll_get_interaction() -- see masks_gui.h -- and every entry point
* (mouse_scroll callback, context-menu sliders) names the property it acts on.
*/
typedef enum dt_masks_interaction_t
{
  DT_MASKS_INTERACTION_UNDEF = 0,    // no property: an unmapped wheel combination does nothing
  DT_MASKS_INTERACTION_SIZE = 1,     // property of the form (shape), explicit
  DT_MASKS_INTERACTION_FADING = 2,   // property of the form (shape), explicit
  DT_MASKS_INTERACTION_OPACITY = 3,  // property of the group in which the form is included, explicit
  DT_MASKS_INTERACTION_ROTATION = 4, // property of the form (shape), explicit
  DT_MASKS_INTERACTION_LAST
} dt_masks_interaction_t;

/** structure used to store all forms's id for a group */
typedef struct dt_masks_form_group_t
{
  int formid;
  int parentid;
  int state;
  float opacity;
} dt_masks_form_group_t;

/**
 * @brief What a request that may change the masks model did.
 *
 * @details UNCHANGED is not a failure and is the reason this is not a gboolean: a caller that
 * asked for the value a member already has must skip its history commit, or every no-op slider
 * tick writes an undo step and a database row.
 */
typedef enum dt_masks_result_t
{
  DT_MASKS_OK = 0,     /* the model changed */
  DT_MASKS_UNCHANGED,  /* legal request, nothing to do -- caller skips the history commit */
  DT_MASKS_NOT_FOUND,  /* no such group, or no such member in it */
  DT_MASKS_INVALID     /* refused: not a group, self-inclusion, bad argument */
} dt_masks_result_t;

/**
 * @brief One shape's membership of one group, BY VALUE.
 *
 * @details The caller gets a copy, never a pointer into the group's own list. That is deliberate
 * twice over: the entry it would otherwise point at belongs to a refcounted, copy-on-write object
 * that the next dt_masks_cow_touch() replaces wholesale, and dt_masks_form_group_t itself can
 * never be made opaque -- common/xmp_sidecar.cc casts an XMP blob straight to an array of them and
 * validates the length against sizeof, so its size and field order ARE the on-disk format in every
 * user's sidecars. A value type is what lets everyone else stop depending on that layout.
 *
 * `index` is the position in the group's list, and position is meaning: it is the compositing
 * order, the GTK row identity, and the index into iop/retouch.c's rt_forms[] and iop/spots.c's
 * clone_algo[] -- both persisted in the user's database.
 */
typedef struct dt_masks_member_t
{
  int              formid;
  int              parentid;   /* the row's AUTHORED origin; not always the group holding it */
  guint            index;      /* position == compositing order == GTK row identity */
  dt_masks_state_t state;      /* tightened from the stored struct's plain int */
  float            opacity;
} dt_masks_member_t;

/**
 * @brief What a form IS, BY VALUE: identity, kind, and -- for a group -- how many members.
 *
 * @details `name` is COPIED rather than borrowed. A borrowed const char * into a refcounted form
 * is exactly the hazard develop/blend_gui.c already trips, caching the pointer across a call that
 * can clone the form underneath it.
 *
 * `member_count` is NOT recursive: it counts the group's own rows, which is what every caller
 * that asks means, and what the compositing order is defined over.
 */
typedef struct dt_masks_form_info_t
{
  int             formid;
  dt_masks_type_t type;
  int             version;
  gboolean        is_group;
  gboolean        is_retouch;   /* (type & DT_MASKS_IS_RETOUCHE) != 0 */
  guint           member_count; /* 0 unless is_group */
  char            name[DT_MASKS_FORM_NAME_LEN];
} dt_masks_form_info_t;

/**
 * @brief One cut in a shape's border outline: while walking the border buffer forward, on
 * reaching index `jump_from`, resume at index `resume_at`.
 *
 * @details A polygon's border is its path offset outward by the feathering radius, and that
 * offset curve folds over itself at every concave run tighter than the radius. The folds are
 * found by _polygon_find_self_intersection() and must be skipped by every walk that counts
 * crossings (the hit-test and the rasterisers), or the fold is filled as if it were shape.
 *
 * These used to travel IN-BAND, encoded into the border buffer itself: NaN in the x slot,
 * the jump target smuggled as an integer in the float y slot. Both bugs this mechanism ever
 * had lived in that encoding, not in the geometry -- a cycle when overlapping ranges pointed
 * into each other (fixed by sorting and merging), then issue #1313, where a fold straddling
 * the buffer seam was encoded as its own complement and swallowed 99.8% of the contour. An
 * out-of-band range cannot express either mistake: `resume_at > jump_from` IS the
 * forward-only invariant, checkable at a glance and validated by the consumers.
 *
 * Invariants a producer must guarantee (dt_masks_skip_ranges_build() does):
 *   - resume_at > jump_from (every skip moves the walk strictly forward);
 *   - ranges sorted by jump_from and pairwise disjoint (resume_at < next jump_from).
 */
typedef struct dt_masks_skip_range_t
{
  int jump_from;
  int resume_at;
} dt_masks_skip_range_t;

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_TYPES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
