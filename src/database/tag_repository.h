/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file database/tag_repository.h
 *
 * @brief `data.tags` and `main.tagged_images`.
 *
 * @warning **This repository is a seed, not the finished thing.** `common/tags.c` still
 * holds 258 SQL references across 31 functions, and they belong here. What is here now is
 * the one query that had to move because `dt_metadata_get()` -- which dispatches on an XMP
 * key and therefore reads four different tables -- could not be split without it.
 *
 * Extend this file when `common/tags.c` is done; do not start a second tag repository.
 */

#ifndef DT_DATABASE_TAG_REPOSITORY_H
#define DT_DATABASE_TAG_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief Names of the tags attached to an image.
 *
 * @param imgid the image, or a negative value for every selected image -- the convention
 *        dt_metadata_get() uses, which is this function's only caller. Across a selection
 *        the same name appears once per image carrying it, and that is deliberate: the
 *        caller counts occurrences.
 * @return a `GList` of newly allocated names. Free with `g_list_free_full(l, g_free)`.
 */
GList *dt_tag_repository_get_attached_names(const int32_t imgid);

G_END_DECLS

#endif // DT_DATABASE_TAG_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
