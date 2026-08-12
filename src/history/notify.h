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

/** @file history/notify.h
 *
 * @brief Everything the history, styles and presets code says to the outside world:
 * messages for the user, and the fact that something changed.
 *
 * @details Copying a history stack, applying a style or renaming one reported what it did
 * with `dt_control_log()`, and told the rest of the application with
 * `DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), ...)`. Both are calls
 * into `control/` (layer 3) from code that has no other reason to know the control loop
 * exists -- which meant this code could not be built, tested or reused without it, and
 * that where a message appears was decided by whatever produced it.
 *
 * Inverted the same way `metadata/notify.h` inverts it: the code states the fact, and
 * whoever is running it decides what that looks like. With no handler installed --
 * ansel-cli, a unit test -- the message is dropped and the notification goes nowhere,
 * which is correct for both.
 *
 * This lands in `src/history` when that module exists; it lives in `common/` only because
 * the module's layer is still an open question.
 */

#ifndef DT_HISTORY_NOTIFY_H
#define DT_HISTORY_NOTIFY_H

#include <glib.h>

G_BEGIN_DECLS

/** @brief Receives one formatted, already-translated message for the user. */
typedef void (*dt_history_message_handler_t)(const char *message);

/** @brief Install the handler, or NULL to remove it. Messages raised with none installed
 *  are dropped. */
void dt_history_set_message_handler(dt_history_message_handler_t handler);

/** @brief Tell the user something. Was dt_control_log(). Internal to this code: the format
 *  string is translated at the call site, because only the call site knows the plural. */
void dt_history_message(const char *format, ...) G_GNUC_PRINTF(1, 2);

/** @brief Same contract, transient acknowledgement flavour. Was dt_toast_log(). */
typedef void (*dt_history_toast_handler_t)(const char *message);
void dt_history_set_toast_handler(dt_history_toast_handler_t handler);
void dt_history_toast(const char *format, ...) G_GNUC_PRINTF(1, 2);

/** @brief What changed. Each value stands for one signal this code used to raise itself. */
typedef enum dt_history_change_t
{
  /** Tags were attached or detached along with a history operation. Was
   *  DT_SIGNAL_TAG_CHANGED. */
  DT_HISTORY_CHANGE_TAGS = 0,
  /** A style was created, edited, renamed or deleted. Was DT_SIGNAL_STYLE_CHANGED. */
  DT_HISTORY_CHANGE_STYLES,
  /** The development history of the interactive session changed -- after an undo, or a
   *  bulk apply. Whoever is showing the history panel or the module order should resync.
   *  Was DT_SIGNAL_DEVELOP_HISTORY_CHANGE. */
  DT_HISTORY_CHANGE_DEVELOP
} dt_history_change_t;

/**
 * @brief Whoever is displaying any of this should look again.
 *
 * @details Deliberately carries no payload, because the signals it replaces carried none:
 * every consumer re-reads. Raised on the thread that made the change -- a handler that
 * touches widgets is responsible for getting itself onto the GUI thread, exactly as the
 * signal emission it replaces was.
 */
typedef void (*dt_history_changed_handler_t)(const dt_history_change_t what);
void dt_history_set_changed_handler(dt_history_changed_handler_t handler);

/** @brief Raise it. Internal to this code. */
void dt_history_changed(const dt_history_change_t what);

/**
 * @brief The development of these images changed. Was DT_SIGNAL_IMAGE_INFO_CHANGED.
 *
 * @details Separate from dt_history_changed() because this one carries a payload, and the
 * signal it replaces takes *ownership* of the list it is given. The handler therefore does
 * the copying, at the raise site, exactly as the call site used to: @p imgs stays the
 * caller's, and a handler that needs to keep it copies it.
 */
typedef void (*dt_history_images_changed_handler_t)(const GList *imgs);
void dt_history_set_images_changed_handler(dt_history_images_changed_handler_t handler);
void dt_history_changed_images(const GList *imgs);

G_END_DECLS

#endif // DT_HISTORY_NOTIFY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
