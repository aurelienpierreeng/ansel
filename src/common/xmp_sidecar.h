/*
    This file is part of darktable,
    Copyright (C) 2009-2013 johannes hanika.
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2011-2012, 2014-2016 Tobias Ellinghaus.
    Copyright (C) 2013-2022 Pascal Obry.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2019, 2021 Hanno Schwalm.
    Copyright (C) 2019-2020, 2022 Philippe Weyland.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file common/xmp_sidecar.h
 *
 * @brief The XMP document that carries an image's *development*: history stack, mask
 * shapes, module order, and the EXIF blob written alongside an export.
 *
 * @details This is the half of the old `common/exif.cc` that is not about the photograph's
 * own description. It reads and writes `Xmp.darktable.history`, `masks_history`,
 * `iop_order_list` and their neighbours, so it necessarily names `develop/` (layer 5) and
 * `imageio/` (layer 6) -- eleven `dt_ioppr_*` symbols, `dt_develop_blend_params_t`,
 * `dt_masks_form_group_t`, `dt_imageio_dng_write_tiff_header`. That is precisely why it
 * could not follow the tag half into `src/metadata` (layer 1), and why the metadata module
 * gate still reads zero without it.
 *
 * It belongs in `src/history` -- it serialises the development, not the photograph -- and
 * moves there when that module exists. The `dt_exif_*` names are kept for now so the cut
 * itself stays reviewable; renaming them is that move's business, not this one's.
 */

#ifndef DT_COMMON_XMP_SIDECAR_H
#define DT_COMMON_XMP_SIDECAR_H

#include "common/image.h"

#ifdef __cplusplus
extern "C" {
#endif

/** write exif to blob, return length in bytes. blob will be allocated by the function. sRGB should be true
 * if sRGB colorspace is used as output. */
int dt_exif_read_blob(uint8_t **blob, const char *path, const int32_t imgid, const int sRGB, const int out_width,
                      const int out_height, const int dng_mode);

/** write xmp sidecar file. */
int dt_exif_xmp_write_with_imgpath(const struct dt_image_t *image, const char *filename, const char *imgpath);

/** write xmp packet inside an image. */
int dt_exif_xmp_attach_export(const int32_t imgid, const char *filename, void *metadata);

/** get the xmp blob for imgid. */
char *dt_exif_xmp_read_string(const int32_t imgid);

/** read xmp sidecar file. */
int dt_exif_xmp_read(dt_image_t *img, const char *filename, const int history_only);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_XMP_SIDECAR_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
