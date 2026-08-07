/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_COMMON_THUMBNAIL_NOTIFY_H
#define DT_COMMON_THUMBNAIL_NOTIFY_H

#include <glib.h>
#include <stdint.h>

/* "This image's rendered thumbnail is stale" -- announced by the backend, acted on by
 * whoever is displaying thumbnails.
 *
 * The backend paths that invalidate a thumbnail (common/image.c, common/history.c) used
 * to call dt_thumbtable_refresh_thumbnail() on dt_gui_get_ui()'s two thumbtables
 * directly. That is common/ (layer 1) reaching into gui/ (layer 4): the bottom layer
 * deciding that a widget should repaint.
 *
 * Routing it through control/'s signals would not fix that -- it only trades a
 * common -> gui edge for a common -> control one. So the dependency is inverted instead:
 * common/ declares the handler type and owns the slot, and the GUI REGISTERS itself at
 * startup (gui/gtk.c). Nothing here includes anything from gui/.
 *
 * With no handler registered -- ansel-cli, ansel-generate-cache, any headless run -- the
 * notification is a no-op. That is also why the callers no longer need their own
 * "is there a GUI?" guard.
 *
 * Threading: the handler is set once during GUI init, before any worker thread can run a
 * path that notifies, and never cleared. Handlers must be safe to invoke from whatever
 * thread mutated the image -- dt_thumbtable_refresh_thumbnail() already marshals to the
 * GUI thread itself.
 */
typedef void (*dt_thumbnail_refresh_handler_t)(int32_t imgid, gboolean refresh_filmstrip);

/** Install the handler invoked whenever an image's thumbnail goes stale. Pass NULL to
 *  remove it. Called by the GUI at startup; there is exactly one handler. */
void dt_thumbnail_notify_set_handler(dt_thumbnail_refresh_handler_t handler);

/** Announce that `imgid`'s rendered thumbnail no longer matches the image.
 *
 *  `refresh_filmstrip` is best-effort and must be FALSE on darkroom write paths:
 *  refreshing the filmstrip spawns an export thread that competes with the realtime
 *  darkroom main preview. Lighttable operations pass TRUE.
 *
 *  Safe to call with no handler registered. */
void dt_thumbnail_notify_image_changed(int32_t imgid, gboolean refresh_filmstrip);

#endif // DT_COMMON_THUMBNAIL_NOTIFY_H
