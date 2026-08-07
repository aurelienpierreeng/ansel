/*
    This file is part of Ansel
    Copyright (C) 2026 Guillaume Stutin.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file
 * Lifecycle and sharing of dt_masks_form_t objects between dev->forms and the
 * per-history-item / undo-redo snapshots (hist->forms).
 *
 * Forms are refcounted instead of deep-copied: dt_masks_snapshot_current_forms()
 * and dt_masks_replace_current_forms() share pointers with dev->forms rather than
 * cloning the whole flat list. A form is only cloned, on the spot, the moment it
 * is actually about to be mutated while something else still references it
 * (dt_masks_cow_touch).
 */

#ifndef DT_DEVELOP_MASKS_MASKS_HISTORY_H
#define DT_DEVELOP_MASKS_MASKS_HISTORY_H

/* Deliberately does NOT include develop/masks.h: that header includes this one at
 * its bottom (dt_masks_form_t must be complete first), so including it back formed
 * an include cycle that only #pragma once made compilable. Every declaration below
 * takes its arguments by POINTER, so tag declarations are sufficient. */

#include <glib.h>

struct dt_develop_t;
struct dt_masks_form_t;

#ifdef __cplusplus
extern "C" {
#endif

/** take a reference on a form (increments its refcount); returns the same pointer */
struct dt_masks_form_t *dt_masks_form_ref(struct dt_masks_form_t *form);

/** release a reference on a form; frees it once the last reference is dropped.
 *  Signature matches GDestroyNotify for use with g_list_free_full(). */
void dt_masks_form_unref(struct dt_masks_form_t *form);

/** Ensure `form` is private to `dev->forms` before mutating it in place: if
 *  anything else still references it (refcount > 1, e.g. a history snapshot),
 *  clone it, splice the clone into dev->forms (and dev->form_gui->form_visible
 *  if it pointed at the old object) in its place, and release the old
 *  reference. Returns the form to mutate: either the original (fast path,
 *  refcount == 1) or the fresh clone. No-op (returns `form` unchanged) if
 *  `form` is not found in dev->forms (e.g. a transient, not-yet-appended
 *  in-creation shape). */
struct dt_masks_form_t *dt_masks_cow_touch(struct dt_develop_t *dev, struct dt_masks_form_t *form);

/** replace dev->forms with forms: shares references with `forms`, does not
 *  deep-copy. `forms` is typically a hist->forms snapshot. */
void dt_masks_replace_current_forms(struct dt_develop_t *dev, GList *forms);

/** snapshot current dev->forms: shares references, does not deep-copy.
 *  Optionally reset dev->forms_changed. */
GList *dt_masks_snapshot_current_forms(struct dt_develop_t *dev, gboolean reset_changed);

#ifdef __cplusplus
}
#endif
#endif // DT_DEVELOP_MASKS_MASKS_HISTORY_H
