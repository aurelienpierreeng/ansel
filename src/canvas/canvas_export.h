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

#ifndef DT_CANVAS_CANVAS_EXPORT_H
#define DT_CANVAS_CANVAS_EXPORT_H

/**
 * @file canvas_export.h
 * @brief Write a canvas out as pages, colour-managed for an output profile.
 *
 * @details The page is rasterised: the painter draws the canvas in its own working space at
 * the requested resolution, the whole raster is converted to the output profile with LCMS,
 * and the profile is embedded so a print shop's RIP reads the numbers the way they were
 * meant. Text and connectors are therefore pixels, not vectors -- a trade for having one
 * painter and one colour path for the screen and the print.
 *
 * **The page is the canvas's own, never the exporter's.** A canvas divided into pages
 * (`dt_canvas_paper_dimensions()`) gives one page per page holding a frame, at that size; a
 * canvas without gives one page holding every frame. Page size and orientation are the
 * document's, set in the atelier, so what was laid out is what comes out.
 */

#include "canvas/canvas.h"
#include "colorprofiles/profile_types.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the pages are written as. A PDF holds them all; a TIFF too; PNG and JPEG take one file each. */
typedef enum dt_canvas_export_format_t
{
  DT_CANVAS_EXPORT_PDF = 0,
  DT_CANVAS_EXPORT_PNG = 1,
  DT_CANVAS_EXPORT_JPEG = 2,
  DT_CANVAS_EXPORT_TIFF = 3,
  DT_CANVAS_EXPORT_LAST = 4,
} dt_canvas_export_format_t;

typedef struct dt_canvas_export_options_t
{
  dt_canvas_export_format_t format;
  float bleed_mm;       ///< how far past every page edge the picture keeps going
  float dpi;            ///< raster resolution
  int quality;          ///< JPEG quality, and the PDF's own image streams; 100 keeps them lossless
  dt_colorspaces_color_profile_type_t icc_type; ///< output profile; NONE or SRGB keep sRGB
  char icc_filename[DT_IOP_COLOR_ICC_LEN];      ///< for DT_COLORSPACE_FILE
  dt_iop_color_intent_t intent;
} dt_canvas_export_options_t;

/** @brief PDF, no bleed, 300 dpi, quality 92, sRGB, perceptual. */
dt_canvas_export_options_t dt_canvas_export_options_default(void);

/** @brief The format's usual file extension, with its dot. */
const char *dt_canvas_export_extension(dt_canvas_export_format_t format);

/**
 * @brief Write the canvas's pages to `path`.
 * @details A format that holds one page per file numbers them from `path`'s stem
 * (`book_01.png`, `book_02.png`) unless there is only one, which keeps `path` itself.
 * `bleed_mm` grows every page by that much on all four sides and fills the growth with
 * whatever the canvas has there -- the picture a page break cut in two keeps going, which is
 * what a binder trims into or folds around. It is nothing to do with a margin: no content is
 * moved, the sheet is simply larger than the page.
 */
gboolean dt_canvas_export(const dt_canvas_t *canvas, const char *path, const dt_canvas_export_options_t *options,
                          GError **error);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_EXPORT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
