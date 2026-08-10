/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2011-2012, 2014-2017, 2019-2020 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014 Ulrich Pegelow.
    Copyright (C) 2014 Jérémy Rosen.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020-2022 Pascal Obry.
    Copyright (C) 2021 Daniel Vogelbacher.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    
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

#ifndef DT_IMAGEIO_IMAGEIO_CORE_H
#define DT_IMAGEIO_IMAGEIO_CORE_H

#include "system/atomic.h"
#include "common/image.h"
#include "imageio/imageio_module.h"
#include "caches/mipmap_cache.h"
#include <glib.h>
#include <stdio.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FILTERS_ARE_CYGM(filters)                                                                                 \
  ((filters) == 0xb4b4b4b4 || (filters) == 0x4b4b4b4b || (filters) == 0x1e1e1e1e || (filters) == 0xe1e1e1e1)

#define FILTERS_ARE_RGBE(filters)                                                                                 \
  ((filters) == 0x63636363 || (filters) == 0x36363636 || (filters) == 0x9c9c9c9c || (filters) == 0xc9c9c9c9)

// FIXME: kill this pls.
#define FILTERS_ARE_4BAYER(filters) (FILTERS_ARE_CYGM(filters) || FILTERS_ARE_RGBE(filters))

// for Adobe coefficients from LibRaw & RawSpeed
#define ADOBE_COEFF_FACTOR 10000

typedef enum dt_imageio_levels_t
{
  IMAGEIO_INT8 = 0x0,
  IMAGEIO_INT12 = 0x1,
  IMAGEIO_INT16 = 0x2,
  IMAGEIO_INT32 = 0x3,
  IMAGEIO_FLOAT = 0x4,
  IMAGEIO_BW = 0x5,
  IMAGEIO_PREC_MASK = 0xFF,

  IMAGEIO_RGB = 0x100,
  IMAGEIO_GRAY = 0x200,
  IMAGEIO_CHANNEL_MASK = 0xFF00
} dt_imageio_levels_t;

// Checks that the image is indeed an ldr image
gboolean dt_imageio_is_ldr(const char *filename);
// checks that the image has a monochrome preview attached
gboolean dt_imageio_has_mono_preview(const char *filename);

// opens the file using pfm, hdr, exr.
dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);
// opens file using imagemagick
dt_imageio_retval_t dt_imageio_open_raster(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);
// try all the options in sequence
dt_imageio_retval_t dt_imageio_open(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);

/**
 * @brief Decode a file into a buffer the CALLER owns, with no cache involvement.
 *
 * @details dt_imageio_open() writes into a ::dt_mipmap_buffer_t whose `cache_entry` the mipmap
 * cache supplies and owns. A caller that just wants one image decoded once -- the import
 * dialog's preview, say -- has no cache entry to give it, and used to fabricate a zeroed
 * `dt_cache_entry_t` on its own stack, hand over its address, and free `cache_entry.data`
 * itself. That is a caller assembling the cache's internal bookkeeping by hand in order to
 * borrow the decoder, and it is why gui/import.c had to include the cache's substrate header.
 *
 * This pair owns that bookkeeping instead. Nothing outside src/caches needs to know what a
 * ::dt_cache_entry_t is.
 *
 * @param img image metadata, filled by the decoder as with dt_imageio_open().
 * @param filename file to decode.
 * @param buf receives the decoded image. Zeroed first; release with
 *        dt_imageio_close_standalone() whether this succeeds or fails.
 * @return ::DT_IMAGEIO_OK on success, otherwise whatever the decoder reported.
 */
dt_imageio_retval_t dt_imageio_open_standalone(dt_image_t *img, const char *filename,
                                               dt_mipmap_buffer_t *buf);

/**
 * @brief Release a buffer from dt_imageio_open_standalone(). NULL-safe, and safe to call on a
 * buffer whose open failed.
 */
void dt_imageio_close_standalone(dt_mipmap_buffer_t *buf);
// tries to open the files not opened by the other routines using GraphicsMagick (if supported)
dt_imageio_retval_t dt_imageio_open_exotic(dt_image_t *img, const char *filename,
                                           dt_mipmap_buffer_t *buf);

struct dt_imageio_module_format_t;
struct dt_imageio_module_data_t;
int dt_imageio_export(const int32_t imgid, const char *filename, struct dt_imageio_module_format_t *format,
                      struct dt_imageio_module_data_t *format_params, const gboolean high_quality,
                      const gboolean copy_metadata, const gboolean export_masks,
                      dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                      dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                      dt_imageio_module_data_t *storage_params, int num, int total, dt_export_metadata_t *metadata);

int dt_imageio_export_with_flags(const int32_t imgid, const char *filename,
                                 struct dt_imageio_module_format_t *format,
                                 struct dt_imageio_module_data_t *format_params, const gboolean ignore_exif,
                                 const gboolean display_byteorder, const gboolean high_quality, gboolean is_scaling,
                                 const gboolean thumbnail_export, const char *filter, const gboolean copy_metadata,
                                 const gboolean export_masks, dt_colorspaces_color_profile_type_t icc_type,
                                 const gchar *icc_filename, dt_iop_color_intent_t icc_intent,
                                 dt_imageio_module_storage_t *storage, dt_imageio_module_data_t *storage_params,
                                 int num, int total, dt_export_metadata_t *metadata,
                                 dt_atomic_int *shutdown);

// general, efficient buffer flipping function using memcopies
void dt_imageio_flip_buffers(char *out, const char *in,
                             const size_t bpp, // bytes per pixel
                             const int wd, const int ht, const int fwd, const int fht, const int stride,
                             const dt_image_orientation_t orientation);

void dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white,
                                          const int ch, const int wd, const int ht, const int fwd,
                                          const int fht, const int stride,
                                          const dt_image_orientation_t orientation);

/**
 * @brief Load the thumbnail embedded into a RAW file having at least the size MAX(width, height) x MAX(width, height)
 *
 *
 * @param filename
 * @param buffer returned image buffer (allocated)
 * @param th_width returned actual width of the thumbnail
 * @param th_height returned actual height of the thumbnail
 * @param color_space returned color space found for the thumbnail
 * @param width input target width. th_width will be at least this.
 * @param height input target height th_height will be at least this.
 * @return int 0 on success
 */
int dt_imageio_large_thumbnail(const char *filename, uint8_t **buffer, int32_t *th_width, int32_t *th_height,
                               dt_colorspaces_color_profile_type_t *color_space, const int width,
                               const int height);

/**
 * @brief Crop an 8-bit RGBA preview buffer in place to a normalized bounding box.
 *
 * Cameras that record selected framing (DNG DefaultUserCrop) still render their embedded
 * previews from the full default-crop rectangle, so a preview taken straight out of the raw
 * shows more than the photographer framed. This trims it back.
 *
 * The box must be expressed in the buffer's own orientation. For previews read from a raw that
 * is the un-oriented sensor frame (dt_image_get_usercrop()), because Ansel applies the EXIF
 * rotation afterwards, when downscaling.
 *
 * Only ever apply this to a preview embedded in the raw itself: a companion/sidecar JPEG is
 * written by the camera at the framing it displayed and is already cropped.
 *
 * @param box normalized (top, left, bottom, right); an identity box is a no-op
 * @param buffer 8-bit RGBA pixels, compacted in place
 * @param width in/out buffer width, updated to the cropped width
 * @param height in/out buffer height, updated to the cropped height
 * @return TRUE if the buffer was cropped, FALSE if it was left untouched
 */
gboolean dt_imageio_crop_thumbnail(const dt_boundingbox_t box, uint8_t *const buffer,
                                   int32_t *width, int32_t *height);

// lookup maker and model, dispatch lookup to rawspeed or libraw
gboolean dt_imageio_lookup_makermodel(const char *maker, const char *model,
                                      char *mk, int mk_len, char *md, int md_len,
                                      char *al, int al_len);


#ifdef __cplusplus
}
#endif

#endif // DT_IMAGEIO_IMAGEIO_CORE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
