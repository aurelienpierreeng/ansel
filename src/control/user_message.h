/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

/** @file control/user_message.h
 *
 * @brief Telling the user something happened.
 *
 * @details Split out of control/control.h, which 125 files include and 36 of them include for
 * nothing else: twelve are IOP plugins compiling the whole view API to report one error, and one
 * is in src/pixel, two layers below. control.h reaches libs/lib.h (a layer-7 header in a layer-3
 * one, because the progress vtable types its callbacks in dt_lib_module_t) and so hands 24
 * project headers to every includer.
 *
 * This header deliberately includes NOTHING. Every signature below is plain C, so a file that
 * only wants to say something to the user pays for exactly that. Keep it that way: the moment
 * this needs a type from somewhere else, that type belongs in the caller's argument list, not
 * in an include here.
 *
 * WHERE THE MESSAGE GOES is not this header's business, and none of these is GUI-only --
 * dt_control_log() is called from worker threads throughout, and the render half lives in
 * control.c (log/toast rings) and, for the pipeline banner, in the orchestrator.
 */

#ifndef DT_CONTROL_USER_MESSAGE_H
#define DT_CONTROL_USER_MESSAGE_H

/* These are defined in a C translation unit and called from C++ ones (imageio/format/exr.cc),
 * so the declarations must carry C linkage -- control/control.h wraps its own in the same
 * guard. Without it the C++ caller emits a mangled reference (_Z14dt_control_logPKcz) that
 * nothing defines.
 *
 * On Linux that still LINKS: a shared object may carry undefined symbols and resolve them when
 * the plugin is dlopen()ed, so the breakage would first appear as a module failing to load at
 * runtime. macOS and Windows resolve plugin symbols at link time and fail the build instead,
 * which is how this was caught. Three green Linux configurations do not cover it. */
#ifdef __cplusplus
extern "C" {
#endif

/** @brief Post a message to the log overlay drawn over the centre view. */
void dt_control_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));

/** @brief Post a transient toast, escaping markup in @p msg. */
void dt_toast_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));

/** @brief Post a transient toast, interpreting Pango markup in @p msg. */
void dt_toast_markup_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));

/* Busy counters. Each enter must be matched by a leave: they are counters, not flags, so
 * nested work does not clear the indicator early. */
void dt_control_log_busy_enter();
void dt_control_toast_busy_enter();
void dt_control_log_busy_leave();
void dt_control_toast_busy_leave();

#ifdef __cplusplus
}
#endif

#endif // DT_CONTROL_USER_MESSAGE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
