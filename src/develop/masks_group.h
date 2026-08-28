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

/** @file develop/masks_group.h
 *
 * @brief Ask the masks module about a shape or a group, instead of reading its structs.
 *
 * @details Part of the enclosure of src/develop/masks (issue #1299, phase P2). Eight files outside
 * the module reach directly into dt_masks_form_t and dt_masks_form_group_t; this is where the
 * questions they are really asking get names.
 *
 * WHAT THE FIRST PARAMETER MEANS -- the convention that replaces per-function threading notes:
 *
 *   GList *forms first            -- resolve against a borrowed refcounted snapshot (pipe->forms,
 *                                    hist->forms). No lock, no copy-on-write, read-only.
 *   const dt_masks_form_t * first -- an already-resolved handle. Thread-neutral: reads only that
 *                                    object's own memory. No lock, no copy-on-write.
 *   dt_develop_t *dev first       -- touches the live list. Returns dt_masks_result_t => it writes,
 *                                    and owns the lock and the copy-on-write internally.
 *
 * Only the resolvers come in pairs, which is what makes a cross-thread resolve visible at the call
 * site rather than invisible.
 *
 * Everything crosses this boundary BY VALUE (dt_masks_form_info_t, dt_masks_member_t). A caller
 * never holds a pointer into a refcounted form: the next dt_masks_cow_touch() replaces the object
 * wholesale, so such a pointer is a use-after-free waiting for a slow enough reader.
 *
 * This header includes develop/masks_types.h and nothing else -- in particular no GTK, unlike
 * develop/masks_gui.h. Model: develop/masks/masks_history.h, which already compiles against an
 * opaque dt_masks_form_t on tag declarations alone.
 */

#ifndef DT_DEVELOP_MASKS_MASKS_GROUP_H
#define DT_DEVELOP_MASKS_MASKS_GROUP_H

#include "develop/masks_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dt_develop_t;
struct dt_masks_form_t;
struct dt_iop_module_t;

/**
 * @brief Describe a form: identity, kind, and -- for a group -- how many members it holds.
 *
 * Thread-neutral: reads only @p form's own memory. Takes no lock and does not copy-on-write.
 *
 * @param form the form to describe. NULL is not an error, it is simply not a form: returns FALSE.
 * @param out filled on TRUE, and left COMPLETELY UNTOUCHED on FALSE, so a caller may keep a
 *            default in it across a failed call.
 * @return TRUE when @p out was filled.
 */
gboolean dt_masks_form_get_info(const struct dt_masks_form_t *form, dt_masks_form_info_t *out);

/**
 * @brief Copy a group's membership rows, in order, into caller storage.
 *
 * Thread-neutral: reads only @p group's own memory. Takes no lock and does not copy-on-write.
 *
 * ORDER IS THE CONTRACT, and it is not cosmetic. The stored order is the compositing order, the
 * GTK row order, the index into iop/retouch.c's rt_forms[] and the index into iop/spots.c's
 * clone_algo[] -- the last two persisted in every user's database. This function must never
 * filter, never recurse into sub-groups, and never reorder. A row that cannot be read still
 * consumes its index (it comes back zeroed), because dropping it would silently re-pair every
 * later shape with the wrong algorithm.
 *
 * @param group a group form. Anything else -- including NULL, and including a shape whose
 *              ->points holds geometry nodes rather than membership rows -- returns 0. That check
 *              is what keeps the polymorphic ->points unreachable from outside the module.
 * @param out caller storage, or NULL to query the count only.
 * @param out_max capacity of @p out in elements.
 * @return the TOTAL number of members, which may exceed @p out_max; exactly
 *         MIN(total, out_max) elements are written.
 */
guint dt_masks_group_copy_members(const struct dt_masks_form_t *group,
                                  dt_masks_member_t *out, guint out_max);

/**
 * @brief The stable, untranslated token for a shape kind: circle, ellipse, polygon, brush,
 * gradient, group, or "unknown".
 *
 * Takes a VALUE, not a form, so a caller can name a kind it recorded earlier, after the form it
 * came from may be gone.
 *
 * THESE TOKENS ARE PERSISTED. They build the conf keys plugins/darkroom/<plugin>/<type>/<feature>
 * declared in data/anselconfig.xml.in (".../polygon/hardness" and friends), so the polygon token
 * is "polygon" and can never become "path": a shape reading a key that is not in confgen gets 0,
 * which would silently reset the user's setting. dt_masks_type_t is a bit field, so first match
 * wins and the order below is load-bearing.
 *
 * @return a static string, never NULL, never translated. For display, use the translated label in
 *         develop/masks_gui.h instead.
 */
const char *dt_masks_type_name(dt_masks_type_t type);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_MASKS_GROUP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
