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

/** @file metadata/notify.h
 *
 * @brief Everything this module says to the outside world: messages for the user, and the
 * fact that the tag vocabulary changed.
 *
 * @details Setting a rating or a colour label on a batch reports what it did, and editing
 * tags has to tell whoever is displaying them to look again. Both were done by calling
 * into `control/` (layer 3) from a layer-1 module -- `dt_control_log()`/`dt_toast_log()`
 * for the first, `DT_DEBUG_CONTROL_SIGNAL_RAISE(..., DT_SIGNAL_TAG_CHANGED)` for the
 * second -- which meant the module could not be built, tested or reused without the whole
 * control loop, and that where a message appears was decided by the code that produced it.
 *
 * The dependency is inverted instead: the module states the fact, and whoever is running
 * it decides how to show it. With no handler installed -- ansel-cli, a unit test -- the
 * message is silently dropped, which is the correct behaviour for both.
 *
 * The same shape as dt_database_set_renamed_handler(); see `src/database/database.h`.
 */

#ifndef DT_METADATA_NOTIFY_H
#define DT_METADATA_NOTIFY_H

#include <glib.h>

G_BEGIN_DECLS

/** @brief Which affordance the message belongs in. The two are not interchangeable, and
 *  the module knows which it means even though it does not know what either looks like. */
typedef enum dt_metadata_notice_t
{
  /** Transient acknowledgement of something that succeeded ("Rating set to 3 for 12
   *  images"). Was dt_toast_log(). */
  DT_METADATA_NOTICE_TOAST = 0,
  /** The running commentary, including refusals ("no images selected to apply rating").
   *  Was dt_control_log(). */
  DT_METADATA_NOTICE_MESSAGE
} dt_metadata_notice_t;

/** @brief Receives one formatted, already-translated message. */
typedef void (*dt_metadata_notify_handler_t)(const dt_metadata_notice_t kind, const char *message);

/** @brief Install the handler, or NULL to remove it. Messages raised with none installed
 *  are dropped. */
void dt_metadata_set_notify_handler(dt_metadata_notify_handler_t handler);

/** @brief Raise one message. Internal to the module: the format string is translated at the
 *  call site, because only the call site knows the plural form. */
void dt_metadata_notify(const dt_metadata_notice_t kind, const char *format, ...) G_GNUC_PRINTF(2, 3);

/**
 * @brief The tag vocabulary changed: one was created, renamed, deleted, attached or
 *        detached. Whoever is showing tags should look again.
 *
 * @details Deliberately carries no payload, because the signal it replaces carried none:
 * every consumer re-reads. Raised on the thread that made the change -- a handler that
 * touches widgets is responsible for getting itself onto the GUI thread, exactly as the
 * signal emission it replaces was.
 */
typedef void (*dt_metadata_tags_changed_handler_t)(void);
void dt_metadata_set_tags_changed_handler(dt_metadata_tags_changed_handler_t handler);

/** @brief Raise it. Internal to the module. */
void dt_metadata_tags_changed(void);

/**
 * @brief The geo-location of these images changed. Whoever is showing a map, or the
 *        geotag panel, should look again.
 *
 * @details Carries the image list because the signal it replaces did. The handler does
 * the copying, at the raise site, exactly as the old call sites used to: @p imgs stays
 * the caller's, and a handler that needs to keep it copies it.
 */
typedef void (*dt_metadata_geotags_changed_handler_t)(const GList *imgs);
void dt_metadata_set_geotags_changed_handler(dt_metadata_geotags_changed_handler_t handler);
void dt_metadata_geotags_changed(const GList *imgs);

G_END_DECLS

#endif // DT_METADATA_NOTIFY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
