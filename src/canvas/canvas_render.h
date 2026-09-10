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

#ifndef DT_CANVAS_CANVAS_RENDER_H
#define DT_CANVAS_CANVAS_RENDER_H

/**
 * @file canvas_render.h
 * @brief Where a canvas meets the library: describing, locating, rendering and decoding images.
 *
 * @details Three concerns, one boundary:
 *
 * - **Identity.** dt_canvas_render_describe_source() copies what the library knows about an
 *   image (ids, folder, file name, EXIF, history hash) into an image frame, and
 *   dt_canvas_render_locate_source() finds the library image again from that record --
 *   by id when the id still names the same file, by folder and file name otherwise, and
 *   nothing when the image left the library. dt_canvas_render_check() turns the answer
 *   into a sync status by comparing history hashes.
 *
 * - **Rendering.** dt_canvas_render_start() runs the full pixel pipeline on a background
 *   job, exports to sRGB 8-bit at the canvas's long edge, encodes a JPEG with the sRGB
 *   profile embedded, and hands the bytes to a callback ON THE GUI THREAD. The job never
 *   touches the canvas: it is given a library id and an object id, and the callback
 *   carries a `token` the caller chose, so a result for a canvas that was closed meanwhile
 *   is recognised and dropped.
 *
 * - **Decoding.** A surface cache turns a frame's JPEG into a cairo surface, either
 *   colour-managed for the display (the atelier) or left in sRGB (the PDF export), and
 *   holds decoded surfaces under a byte budget with LRU eviction, so a canvas of a hundred
 *   frames does not keep a hundred full decodes in memory.
 */

#include "canvas/canvas.h"

#include <cairo.h>
#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- identity --------------------------------------------------------------- */

/**
 * @brief Fill a frame's source record from the library.
 * @return FALSE when `imgid` is not a library image. The frame's JPEG is left alone.
 */
gboolean dt_canvas_render_describe_source(int32_t imgid, dt_canvas_image_t *image);

/**
 * @brief Find the library image a frame was rendered from.
 * @return the library id, or -1 when no image matches.
 */
int32_t dt_canvas_render_locate_source(const dt_canvas_image_t *image);

/**
 * @brief Compare the frame's render with the library and record the answer in `sync_status`.
 * @return the status. A frame with no JPEG yet reports STALE when the source exists.
 */
dt_canvas_sync_status_t dt_canvas_render_check(dt_canvas_image_t *image);

/* --- rendering -------------------------------------------------------------- */

/**
 * @brief Delivered on the GUI thread when a render finishes.
 * @param object_id the frame the render was started for.
 * @param token the value handed to dt_canvas_render_start().
 * @param jpeg the sRGB JPEG, or NULL when the render failed. Borrowed: reference it to keep it.
 * @param history_hash the source's history hash at render time.
 */
typedef void (*dt_canvas_render_done_t)(uint32_t object_id, uint64_t token, GBytes *jpeg, int32_t pixel_width,
                                        int32_t pixel_height, uint64_t history_hash, gpointer user_data);

/**
 * @brief Queue a render of a library image for a frame.
 * @param long_edge pixels on the long edge of the export.
 * @param quality JPEG quality, 1..100.
 * @return FALSE when the job could not be queued.
 */
gboolean dt_canvas_render_start(int32_t imgid, uint32_t object_id, uint64_t token, int32_t long_edge,
                                int32_t quality, dt_canvas_render_done_t done, gpointer user_data);

/** @brief Ask every render still running or queued to stop as soon as it can. */
void dt_canvas_render_cancel_all(void);

/* --- maps ------------------------------------------------------------------- */

/** @brief How many tile providers are available. */
int dt_canvas_map_source_count(void);
/** @brief The provider at `index`: its stable id (stored in the frame) and its name. */
uint32_t dt_canvas_map_source_id(int index);
const char *dt_canvas_map_source_name(int index);
/** @brief The index of a stored provider id, or 0 (the default) when it is not available. */
int dt_canvas_map_source_index(uint32_t source);
/** @brief The attribution a provider requires, shown on the frame. */
const char *dt_canvas_map_source_attribution(uint32_t source);

/**
 * @brief Queue a render of a slippy map around a point, `pixel_width` x `pixel_height` pixels.
 * @details Tiles are fetched over HTTP into a disk cache under the user's cache directory,
 * composed, and delivered like an image render, as an sRGB JPEG through `done`. The
 * `history_hash` of the callback is 0 for a map.
 */
gboolean dt_canvas_render_map_start(uint32_t object_id, uint64_t token, double latitude, double longitude,
                                    int32_t zoom, uint32_t source, int32_t pixel_width, int32_t pixel_height,
                                    int32_t quality, dt_canvas_render_done_t done, gpointer user_data);

/* --- decoding --------------------------------------------------------------- */

typedef struct dt_canvas_surface_cache_t dt_canvas_surface_cache_t;

/**
 * @brief A cache of decoded frames.
 * @param for_display TRUE converts every decode to the display profile; FALSE keeps sRGB.
 * @param budget_bytes how many bytes of surfaces to keep before evicting the least recently used.
 */
dt_canvas_surface_cache_t *dt_canvas_surface_cache_new(gboolean for_display, size_t budget_bytes);
void dt_canvas_surface_cache_free(dt_canvas_surface_cache_t *cache);

/**
 * @brief The surface for a frame's current JPEG.
 * @details Keyed on the JPEG bytes' identity and, for a display cache, on the display
 * profile generation, so a re-render or a monitor change decodes afresh. The cache owns
 * the surface: do not destroy it, and do not keep it past the next call.
 * @return NULL when the frame has no JPEG or it does not decode.
 */
cairo_surface_t *dt_canvas_surface_cache_get(dt_canvas_surface_cache_t *cache, const dt_canvas_object_t *object);

/** @brief Drop every surface, e.g. when the display profile changed. */
void dt_canvas_surface_cache_clear(dt_canvas_surface_cache_t *cache);

/**
 * @brief Decode a JPEG into a cairo RGB24 surface.
 * @param for_display convert to the display profile, else keep the JPEG's sRGB values.
 * @return a new surface the caller destroys, or NULL.
 */
cairo_surface_t *dt_canvas_render_decode(GBytes *jpeg, gboolean for_display);

/**
 * @brief An sRGB colour as the display should show it.
 * @param out receives red, green, blue in [0, 1], colour-managed when `for_display`.
 */
void dt_canvas_render_color(const dt_canvas_color_t *color, gboolean for_display, double out[3]);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_RENDER_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
