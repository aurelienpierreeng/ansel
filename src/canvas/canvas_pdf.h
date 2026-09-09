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

#ifndef DT_CANVAS_CANVAS_PDF_H
#define DT_CANVAS_CANVAS_PDF_H

/**
 * @file canvas_pdf.h
 * @brief Print a canvas to a one-page PDF, colour-managed for an output profile.
 *
 * @details The page is rasterised: the painter draws the canvas in sRGB at the requested
 * resolution, the whole raster is converted to the output profile with LCMS, and the
 * profile is embedded so a print shop's RIP reads the numbers the way they were meant.
 * Text and connectors are therefore pixels, not vectors -- a trade for having one painter
 * and one colour path for the screen and the print.
 */

#include "canvas/canvas.h"
#include "colorprofiles/profile_types.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_canvas_pdf_options_t
{
  float page_width_mm;
  float page_height_mm;
  float margin_mm;      ///< kept clear on every side
  float dpi;            ///< raster resolution
  dt_colorspaces_color_profile_type_t icc_type; ///< output profile; NONE or SRGB keep sRGB
  char icc_filename[DT_IOP_COLOR_ICC_LEN];      ///< for DT_COLORSPACE_FILE
  dt_iop_color_intent_t intent;
} dt_canvas_pdf_options_t;

/** @brief A4 portrait, 10 mm margin, 300 dpi, sRGB, perceptual. */
dt_canvas_pdf_options_t dt_canvas_pdf_options_default(void);

/**
 * @brief Write the canvas to `path`.
 * @details The box around every visible frame is scaled to fit inside the margins and
 * centred; an empty canvas produces a page of background.
 */
gboolean dt_canvas_pdf_export(const dt_canvas_t *canvas, const char *path, const dt_canvas_pdf_options_t *options,
                              GError **error);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PDF_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
