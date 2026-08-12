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

/** @file develop/pipeline_notify.h
 *
 * @brief Everything the pixel pipeline says to whoever is watching: messages for the
 * user, and the busy banner naming the module being processed.
 *
 * @details Tiling failures, blending allocation errors, OpenCL fallbacks and the
 * per-module progress banner were raised by the worker thread calling straight into
 * `control/` — dt_control_log(), dt_set_main_message() under control->log_mutex, forced
 * centre redraws. That made every pure pixel file (tiling.c, blend.c, pixelpipe_gpu.c,
 * the raster-mask transport) carry control/control.h for a handful of user-facing
 * strings, and made the pipeline unbuildable without the control loop.
 *
 * Inverted the same way as metadata/notify.h, history/notify.h and
 * common/image_notify.h: the pipeline states the fact; whoever is running it decides
 * what that looks like. With no handler installed — ansel-cli, a unit test — the message
 * is dropped, which is what a headless export wants.
 *
 * Both handlers are called from pipeline worker threads, exactly as the calls they
 * replace were; dt_control_log() and dt_control_queue_redraw_center() are worker-safe
 * today and the darktable.c handlers keep that contract.
 */

#ifndef DT_DEVELOP_PIPELINE_NOTIFY_H
#define DT_DEVELOP_PIPELINE_NOTIFY_H

#include <glib.h>

G_BEGIN_DECLS

/** @brief Receives one formatted, already-translated message for the user.
 *  Was dt_control_log() from pipeline code. */
typedef void (*dt_pipeline_message_handler_t)(const char *message);

/** @brief Install the handler, or NULL to remove it. Messages raised with none
 *  installed are dropped. */
void dt_pipeline_set_message_handler(dt_pipeline_message_handler_t handler);

/** @brief Tell the user something went sideways in the pipe. Internal to the pipeline:
 *  the format string is translated at the call site. */
void dt_pipeline_message(const char *format, ...) G_GNUC_PRINTF(1, 2);

/**
 * @brief Receives the busy-banner text, or NULL when processing finished and the banner
 * should clear. Was dt_set_main_message() under control->log_mutex plus a forced centre
 * redraw; the handler owns both. The string is the callee's only for the duration of the
 * call — copy it to keep it.
 */
typedef void (*dt_pipeline_busy_handler_t)(const char *message_or_null);
void dt_pipeline_set_busy_handler(dt_pipeline_busy_handler_t handler);

/** @brief Publish what the pipe is chewing on ("Processing module `%s' ..."). */
void dt_pipeline_busy_printf(const char *format, ...) G_GNUC_PRINTF(1, 2);

/** @brief Processing finished; clear the banner. */
void dt_pipeline_busy_clear(void);

G_END_DECLS

#endif // DT_DEVELOP_PIPELINE_NOTIFY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
