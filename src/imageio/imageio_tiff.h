/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010-2011, 2014 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    
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

#ifndef DT_IMAGEIO_IMAGEIO_TIFF_H
#define DT_IMAGEIO_IMAGEIO_TIFF_H

#include "common/image.h"
#include "caches/mipmap_cache.h"

dt_imageio_retval_t dt_imageio_open_tiff(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);

int dt_imageio_tiff_read_profile(const char *filename, uint8_t **out);

/**
 * @brief Decode a TIFF held in memory into 8-bit RGBx, for the previews raw files embed.
 *
 * @details
 * Raws that do not embed a JPEG preview embed a TIFF one -- every DNG written by Adobe's
 * converter does, as do Phase One IIQ, Hasselblad 3FR and film scanners. This is the only
 * decoder those previews need; libtiff reads a blob through TIFFClientOpen just as well as
 * a file.
 *
 * @param blob Encoded TIFF bytes. Not retained.
 * @param bufsize Length of @p blob.
 * @param out On success, receives a freshly allocated buffer of 4 bytes per pixel, laid out
 *            R, G, B, unused -- the layout the JPEG preview path also produces. The caller
 *            owns it and must release it with dt_pixelpipe_cache_free_align(). Untouched on
 *            failure.
 * @param width Receives the decoded width. Untouched on failure.
 * @param height Receives the decoded height. Untouched on failure.
 * @return TRUE on success, FALSE if the blob is not a readable TIFF or allocation failed.
 */
gboolean dt_imageio_tiff_decode_blob(const uint8_t *blob, size_t bufsize, uint8_t **out,
                                     int32_t *width, int32_t *height);

#endif // DT_IMAGEIO_IMAGEIO_TIFF_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

