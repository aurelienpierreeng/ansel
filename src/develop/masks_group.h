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
 * @brief Validate a serialized group-membership blob and copy its child form IDs.
 *
 * The blob must contain exactly @p member_count group membership records in the masks on-disk
 * representation. On success, @p child_ids receives a newly allocated array in stored order;
 * the caller owns it and releases it with dt_free_align(). A zero-member group succeeds with
 * @p child_ids set to NULL. The implementation owns the wire layout, so callers never name or
 * dereference its internal record type.
 *
 * @param serialized the serialized membership blob, or NULL only when @p member_count is zero.
 * @param serialized_size the blob size in bytes.
 * @param member_count the number of membership records declared beside the blob.
 * @param child_ids receives the allocated child-ID array on success and NULL on failure.
 * @return TRUE for an exact, representable group blob; FALSE for invalid sizes, a missing blob,
 *         allocation failure, or a NULL @p child_ids output pointer.
 */
gboolean dt_masks_group_deserialize_child_ids(const void *serialized, int serialized_size,
                                              int member_count, int **child_ids);

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
 * declared in data/anselconfig.xml.in (".../polygon/fading" and friends), so the polygon token
 * is "polygon" and can never become "path": a shape reading a key that is not in confgen gets 0,
 * which would silently reset the user's setting. dt_masks_type_t is a bit field, so first match
 * wins and the order below is load-bearing.
 *
 * @return a static string, never NULL, never translated. For display, use the translated label in
 *         develop/masks_gui.h instead.
 */
const char *dt_masks_type_name(dt_masks_type_t type);

/**
 * @brief Set a group member's combination operator, or toggle its inversion.
 *
 * Takes the live list, and owns the copy-on-write itself -- which is the entire reason this
 * exists. A group is refcounted and shared with every history snapshot that references it, so a
 * caller must touch the group before mutating a row. Doing that correctly ALSO means the caller
 * may not resolve the row first: cloning a group clones its membership blocks too, so an entry
 * pointer taken before the touch belongs to the abandoned copy and the mutation lands nowhere.
 * An id-keyed signature is the only shape that cannot get this wrong, which is why it takes ids
 * and hands the result back rather than letting anyone hold a row.
 *
 * @param operation DT_MASKS_STATE_INVERSE toggles inversion; any of UNION / INTERSECTION /
 *                  DIFFERENCE / EXCLUSION replaces the combination operator. Anything else is
 *                  INVALID.
 * @param out (may be NULL) receives the row AFTER the change, including its index -- the caller
 *            usually needs both to refresh its own view, and this is the only pointer-free way to
 *            get them.
 * @return OK when the row changed, UNCHANGED when it already held that state (the caller must
 *         then skip its history commit, or every no-op click writes an undo step),
 *         NOT_FOUND for no such group or no such member, INVALID for a bad operation.
 */
dt_masks_result_t dt_masks_group_set_member_operation(struct dt_develop_t *dev, int group_id, int formid,
                                                      dt_masks_state_t operation, dt_masks_member_t *out);

/**
 * @brief Read one group member by identity.
 *
 * The pointer-free counterpart to dt_masks_group_copy_members() when only one row is wanted, and
 * the read half of the read-modify-write a caller needs when it can only express a change as an
 * increment on the current value.
 *
 * Deliberately does NOT copy-on-write. Touching a group on a plain read would clone a shared group
 * every time the GUI asks what a shape's opacity is -- copy-on-write is a writer's obligation.
 *
 * @param out (may be NULL, though then the call only answers "does this row exist") receives the
 *            row, including its index.
 * @return OK, or NOT_FOUND for no such group, a form that is not a group, or no such member.
 */
dt_masks_result_t dt_masks_group_get_member(struct dt_develop_t *dev, int group_id, int formid,
                                            dt_masks_member_t *out);

/**
 * @brief Whether @p container_id holds @p needle_id, at any depth (and trivially when they are
 * the same id).
 *
 * The question every caller wiring one group into another has to ask first: closing a cycle in
 * the membership graph makes every walk over it non-terminating, the tree builds included.
 *
 * @return DT_MASKS_OK when it does, DT_MASKS_NOT_FOUND when it does not, DT_MASKS_INVALID for a
 *         NULL dev. A @p container_id that names no group, or names a shape, is NOT_FOUND.
 */
dt_masks_result_t dt_masks_group_contains(struct dt_develop_t *dev, int container_id, int needle_id);

/**
 * @brief Whether every shape @p group_id ultimately holds is also held by @p target_id.
 *
 * Leaf by leaf and at any depth on both sides, so it answers for a group of groups too. What it
 * means is that nesting @p group_id into @p target_id would add no shape the target does not
 * already apply -- NOT that it would change nothing, which is a different question the combine
 * operators answer (a shape applied twice in difference or exclusion does change the mask).
 *
 * @param has_shapes (may be NULL) set to TRUE as soon as a leaf shape is met, so a caller can
 *                   tell "all covered" from "there was nothing in it to cover".
 * @return DT_MASKS_OK when every leaf is covered, DT_MASKS_NOT_FOUND when one is not,
 *         DT_MASKS_INVALID for a NULL dev.
 */
dt_masks_result_t dt_masks_group_covers_shapes(struct dt_develop_t *dev, int group_id, int target_id,
                                               gboolean *has_shapes);

/**
 * @brief Where the mask rooted at @p root_id FIRST applies @p formid.
 *
 * Walked in compositing order: a group's members in their own order, descending into a member
 * group at the position that group sits at. A shape can legitimately appear more than once in one
 * mask -- union and intersection are idempotent, difference and exclusion are not -- so this
 * answers which application the later ones are read against, not which ones are redundant.
 *
 * @param holder_id (may be NULL) the group directly holding that first use.
 * @param index (may be NULL) its position inside that group, i.e. its compositing rank.
 * @param holder_name (may be NULL) copied, not borrowed, like dt_masks_form_get_info()'s name.
 * @return DT_MASKS_OK when found, DT_MASKS_NOT_FOUND when the mask does not apply it at all,
 *         DT_MASKS_INVALID for a NULL dev.
 */
dt_masks_result_t dt_masks_group_first_use(struct dt_develop_t *dev, int root_id, int formid,
                                           int *holder_id, guint *index,
                                           char *holder_name, size_t holder_name_size);

/**
 * @brief Which group references @p formid, searching every group in dev->forms depth-first.
 *
 * A shape's own dt_masks_form_t does not record who holds it, and the row's parentid records where
 * it was AUTHORED, not where it currently lives -- so a caller holding only a shape id, as the
 * shape manager's tree does when it lists shapes at top level, has to search. Returns the first
 * holder found; a shape referenced by two groups has no single answer, and the caller wanting a
 * specific one already knows which.
 *
 * @return the holding group's formid, or 0 for none (and for a NULL dev or a zero @p formid).
 */
int dt_masks_group_find_holder(struct dt_develop_t *dev, int formid);

/**
 * @brief Describe every group of the live list, in dev->forms order, into caller storage.
 *
 * The catalogue a caller offers a destination group from -- the shape manager's "Attach to the
 * group" -- BY VALUE, so the caller neither walks dev->forms nor holds a pointer into a
 * refcounted form. Retouch and spot groups come too; a caller that must not offer them filters on
 * dt_masks_form_info_t.is_retouch.
 *
 * Takes the masks lock for reading across the walk.
 *
 * @param out caller storage, or NULL to query the count only.
 * @param out_max capacity of @p out in elements.
 * @return the TOTAL number of groups, which may exceed @p out_max; exactly MIN(total, out_max)
 *         elements are written. 0 for a NULL dev.
 */
guint dt_masks_group_list(struct dt_develop_t *dev, dt_masks_form_info_t *out, guint out_max);

/**
 * @brief Set a group member's opacity.
 *
 * Same id-keyed contract as dt_masks_group_set_member_operation() above, and it exists for a
 * failure this codebase actually shipped. Opacity used to be written through a resolved
 * dt_masks_form_group_t*, which CANNOT copy-on-write: it has the row but not the group that owns
 * it, so every caller had to touch the group itself. Callers compensated by touching the parent
 * once, when a context menu was built, and then mutating the row in place for the whole
 * interaction -- but the opacity slider commits history on every step, and each commit
 * re-snapshots dev->forms and shares the group again. The up-front touch was consumed by the first
 * commit, so from the second step on, every drag rewrote the opacity inside history snapshots that
 * were supposed to be frozen, and undo could not restore it. Taking ids instead of a row makes
 * that arrangement unrepresentable.
 *
 * @param opacity clamped to [0;1]. NaN is INVALID rather than clamped -- see the implementation.
 * @param out (may be NULL) receives the row AFTER the change, including the clamped opacity, so a
 *            caller driving a slider can show what was actually stored.
 * @return OK when the value changed, UNCHANGED when the row already held it (skip the history
 *         commit), NOT_FOUND for no such group or member, INVALID for a NULL dev or a NaN.
 */
dt_masks_result_t dt_masks_group_set_member_opacity(struct dt_develop_t *dev, int group_id, int formid,
                                                    float opacity, dt_masks_member_t *out);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_MASKS_GROUP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
