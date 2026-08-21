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

/** @file common/image_notify.h
 *
 * @brief `common/image.c` states that an image entered the library; whoever is showing
 * the collection decides what that looks like.
 *
 * @details The import path used to raise `DT_SIGNAL_IMAGE_IMPORT` itself — a call into
 * `control/` (layer 3) from a layer-1 file that had no other reason to know the control
 * loop exists, and whose include line was missing anyway: the symbols arrived through
 * `develop/imageop.h → control/settings.h → control/signal.h`, a supply line three
 * headers long. Same inversion as `metadata/notify.h` and `history/notify.h`; same
 * shape as `common/thumbnail_notify.h` next door. With no handler installed — ansel-cli,
 * a unit test — the fact is dropped, which is correct for both.
 */

#ifndef DT_COMMON_IMAGE_NOTIFY_H
#define DT_COMMON_IMAGE_NOTIFY_H

#include <glib.h>
#include <inttypes.h>

G_BEGIN_DECLS

/** @brief Receives the id of one freshly imported image. Raised on the thread that did
 *  the import; a handler that touches widgets gets itself onto the GUI thread, exactly
 *  as the signal emission it replaces did. */
typedef void (*dt_image_imported_handler_t)(const int32_t imgid);

/** @brief Install the handler, or NULL to remove it. */
void dt_image_notify_set_imported_handler(dt_image_imported_handler_t handler);

/** @brief Raise it. Internal to common/image.c. */
void dt_image_notify_imported(const int32_t imgid);

G_END_DECLS

#endif // DT_COMMON_IMAGE_NOTIFY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
