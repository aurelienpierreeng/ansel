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

/** @file develop/masks_debug.h
 *
 * @brief Render a mask's rasterisation and its GUI overlay to an image file, headlessly.
 *
 * @details Mask defects come in two kinds that are easy to confuse and were confused, at cost:
 * the RASTER is wrong (the alpha the pipeline blends with), or the OVERLAY is wrong (what the
 * darkroom draws on top of it). A brush cusp losing coverage and a dashed outline drawing
 * self-intersecting circles are different bugs in different layers, and a fix aimed at the
 * wrong one is how an always-FALSE flag came to delete geometry the rasteriser needed in order
 * to tidy a line the GUI drew.
 *
 * So this renders BOTH, from one call, with the backdrop chosen by the caller: the overlay
 * alone, over black, or over the rasterised alpha. Seeing them superimposed is what tells the
 * two kinds apart at a glance.
 *
 * The overlay half runs THE PRODUCTION CODE -- dt_masks_events_post_expose_with(), the same
 * function the darkroom calls, differing only in the transform it is handed. A separate
 * drawing path for diagnostics would be a fork that stops agreeing with the GUI exactly when
 * it is needed to explain a GUI problem.
 *
 * Implemented in src/develop/masks/masks_debug.c.
 */

#ifndef DT_DEVELOP_MASKS_DEBUG_H
#define DT_DEVELOP_MASKS_DEBUG_H

#include <glib.h>

struct dt_develop_t;
struct dt_masks_form_t;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the overlay is drawn on top of. */
typedef enum dt_masks_debug_backdrop_t
{
  DT_MASKS_DEBUG_BACKDROP_TRANSPARENT = 0, /**< nothing: the overlay's own geometry, isolated */
  DT_MASKS_DEBUG_BACKDROP_BLACK,           /**< opaque black: the overlay as the eye sees it */
  DT_MASKS_DEBUG_BACKDROP_RASTER,          /**< the rasterised alpha in grey: the two layers superimposed */
} dt_masks_debug_backdrop_t;

/** @brief What to render. Zeroed means: raster only, at full image resolution, on black. */
typedef struct dt_masks_debug_request_t
{
  /** Output size. 0 takes the image's raw size; a width alone keeps the aspect. */
  int width, height;
  dt_masks_debug_backdrop_t backdrop;
  /** Run the GUI overlay code over the backdrop. */
  gboolean draw_overlay;
} dt_masks_debug_request_t;

/**
 * @brief Rasterise @p form into a freshly allocated width*height float buffer, 0..1.
 *
 * @details No pipeline distortion is applied: the shape is drawn against the raw image
 * geometry, which is what a geometry regression wants to see -- a difference then means the
 * mask code changed, not that some other module's distortion did.
 *
 * @return the buffer (free with dt_free), or NULL. @p form may be a group.
 */
float *dt_masks_debug_rasterise(struct dt_develop_t *dev, struct dt_masks_form_t *form,
                                int width, int height);

/**
 * @brief Render @p form per @p request and write it to @p path as a PNG.
 *
 * @return TRUE on success. @p form NULL uses the dev's currently visible form.
 */
gboolean dt_masks_debug_write_png(struct dt_develop_t *dev, struct dt_masks_form_t *form,
                                  const dt_masks_debug_request_t *request, const char *path);

/**
 * @brief Write the shape's outline buffers to @p path as CSV: index, centreline, border.
 *
 * @details The mask is the union of segments drawn from each centreline sample to its border
 * sample, so when coverage goes missing the answer is in these two arrays and nowhere else --
 * which sample stopped moving, where the border jumped, which directions were never visited.
 * Reading them beats reasoning about the recursion that produced them: three theories about
 * the cusp defect died against this data before the right one survived it.
 */
gboolean dt_masks_debug_write_outline_csv(struct dt_develop_t *dev, struct dt_masks_form_t *form,
                                          const char *path);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_DEBUG_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
