/*
    This file is part of the Ansel project.
    Copyright (C) 2023-2026 Aurélien PIERRE.

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
#include "common/image_extensions.h"
#include "system/macros.h"

#include <string.h>

/* These 3 lists are the decode-routing ground truth: each extension appears in AT MOST one of
 * them. Do NOT list an extension in more than one -- dt_imageio_open() tries the matching
 * decoder(s) in order and a generic/exotic fallback can silently "succeed" on data it
 * misinterprets (e.g. GraphicsMagick reading a .dng's embedded TIFF structure as a normal image
 * instead of letting rawspeed/libraw demosaic the actual sensor data). Ambiguity about the
 * eventual *class* of a file (raw-vs-processed, LDR-vs-HDR) is handled separately by
 * dt_image_ext_is_ambiguous() below, which never adds a second routing entry. */

static const char *_raw_extensions[] = {
  "3fr", "ari", "arw", "bay", "bmq", "cap", "cine", "cr2",
  "crw", "cs1", "dc2", "dcr", "dng", "gpr", "erf", "fff",
  "ia", "iiq", "k25", "kc2", "kdc", "mdc", "mef", "mos",
  "mrw", "nef", "nrw", "orf", "ori", "pef", "pxn", "qtk",
  "raf", "raw", "rdc", "rw2", "rwl", "sr2", "srf", "srw", "x3f",
#ifdef HAVE_LIBRAW
  "cr3",
#endif
  NULL
};

static const char *_ldr_extensions[] = {
  "jpg", "jpeg", "png", "tif", "tiff", "pgm", "pbm", "ppm",
#ifdef HAVE_OPENJPEG
  "jp2", "j2k", "jpc",
#endif
#ifdef HAVE_WEBP
  "webp",
#endif
#if defined(HAVE_GRAPHICSMAGICK) || defined(HAVE_IMAGEMAGICK)
  // Formats only reachable through the generic exotic decoder inside dt_imageio_open_raster(),
  // gated on the same libraries CMake requires to actually decode them (src/CMakeLists.txt).
  "gif", "bmp", "dcm", "jng", "miff", "mng", "pnm", "pam",
#endif
  NULL
};

static const char *_hdr_extensions[] = {
  "pfm", "hdr",
#ifdef HAVE_OPENEXR
  "exr",
#endif
#ifdef HAVE_LIBAVIF
  "avif",
#endif
#ifdef HAVE_LIBHEIF
  "heif", "heic", "hif",
#endif
  NULL
};

/* Containers whose real class needs a decode to settle -- see doc/image-type-detection.md.
 * Each of these still appears in exactly one of the 3 lists above (for routing); this list only
 * suppresses guessing a flag from the extension in dt_imageio_get_type_from_extension(). */
static const char *_ambiguous_extensions[] = {
  "dng", "tif", "tiff", "heif", "heic", "avif", "hif", NULL
};

/* Extensions shown under both GUI buckets in the import dialog despite having a single routing
 * category -- a display-only nicety, never consulted by decoder routing. */
static const char *_gui_dual_bucket_extensions[] = { "dng", NULL };

static gboolean _ext_in_list(const char *ext, const char *const *list)
{
  if(IS_NULL_PTR(ext)) return FALSE;
  for(; *list; list++)
    if(!g_ascii_strncasecmp(ext, *list, strlen(*list)))
      return TRUE;
  return FALSE;
}

gboolean dt_image_ext_is_raw(const char *ext)
{
  return _ext_in_list(ext, _raw_extensions);
}

gboolean dt_image_ext_is_ldr(const char *ext)
{
  return _ext_in_list(ext, _ldr_extensions);
}

gboolean dt_image_ext_is_hdr(const char *ext)
{
  return _ext_in_list(ext, _hdr_extensions);
}

gboolean dt_image_ext_is_supported(const char *ext)
{
  return dt_image_ext_is_raw(ext) || dt_image_ext_is_ldr(ext) || dt_image_ext_is_hdr(ext);
}

gboolean dt_image_ext_is_ambiguous(const char *ext)
{
  return _ext_in_list(ext, _ambiguous_extensions);
}

gboolean dt_image_ext_is_gui_raw(const char *ext)
{
  return dt_image_ext_is_raw(ext);
}

gboolean dt_image_ext_is_gui_raster(const char *ext)
{
  return dt_image_ext_is_ldr(ext) || dt_image_ext_is_hdr(ext) || _ext_in_list(ext, _gui_dual_bucket_extensions);
}

const char *const *dt_image_ext_raw_list(void) { return _raw_extensions; }
const char *const *dt_image_ext_ldr_list(void) { return _ldr_extensions; }
const char *const *dt_image_ext_hdr_list(void) { return _hdr_extensions; }

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
