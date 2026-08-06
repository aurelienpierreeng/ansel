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
#ifndef DT_COMMON_IMAGE_EXTENSIONS_H
#define DT_COMMON_IMAGE_EXTENSIONS_H

#include <glib.h>

/** TRUE if `filename`'s extension is one this build can decode. Declared here, beside
 * the extension table it consults; implemented in common/darktable.c. */
gboolean dt_supported_image(const gchar *filename);

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single source of truth for "what kind of image is this extension" across the whole app:
 * decoder routing (dt_imageio_open()), the provisional img->flags guess
 * (dt_imageio_get_type_from_extension()), dt_supported_image(), and the import dialog's
 * Raw/Raster GUI filter. Each of raw/ldr/hdr below has exactly one list per extension --
 * mirroring the decoder-dispatch reality (dt_imageio_open_raw/_raster/_hdr are 3 distinct
 * codec chains) -- entries are gated behind the same HAVE_* macros the codecs themselves use,
 * so an extension is never reported as available when its codec isn't actually compiled in. */

/* Decode routing + dt_supported_image(): exactly one of these is true per extension (except the
 * handful covered by dt_image_ext_is_ambiguous(), which still route through exactly one of
 * these three, but must not have a flag guessed from the extension alone -- see below).
 *
 * There is deliberately no plain "is_raster" (ldr || hdr) helper here: dt_imageio_open() has two
 * DISTINCT decoder chains for ldr (dt_imageio_open_raster, generic/exotic-capable) and hdr
 * (dt_imageio_open_hdr, OpenEXR/Radiance/PFM/AVIF/HEIF). Collapsing them would let the generic
 * chain be tried before the dedicated one for an hdr-only extension. Combine is_ldr()/is_hdr()
 * explicitly at each call site instead, so the distinction can't be silently lost again. */
gboolean dt_image_ext_is_raw(const char *ext);
gboolean dt_image_ext_is_ldr(const char *ext);
gboolean dt_image_ext_is_hdr(const char *ext);
gboolean dt_image_ext_is_supported(const char *ext); // raw || ldr || hdr

/* Containers whose actual dynamic range (or, for dng, raw-vs-already-processed nature) cannot be
 * known from the extension alone -- only after decoding. dt_imageio_get_type_from_extension()
 * must return 0 (unknown) for these instead of guessing from dt_image_ext_is_raw/_ldr/_hdr, even
 * though they do route to one of the 3 decoders above. See doc/image-type-detection.md. */
gboolean dt_image_ext_is_ambiguous(const char *ext);

/* GUI-facing "Raw image files" / "Raster image files" buckets for the import dialog's file
 * filter (common/import.c). Mostly mirror is_raw()/is_raster() above, plus one deliberate UX
 * override: a linear/already-demosaiced DNG behaves like a normal processed photo, so dng is
 * shown under both buckets rather than raw-only. This is a display-only concern -- it must
 * never influence decoder routing. */
gboolean dt_image_ext_is_gui_raw(const char *ext);
gboolean dt_image_ext_is_gui_raster(const char *ext); // ldr || hdr || the dng override

/* Read-only access to the 3 routing lists (NULL-terminated), so a caller that needs to build a
 * GtkFileFilter pattern list (common/import.c) can enumerate every known extension instead of
 * hand-maintaining its own copy. Filter membership (raw/ldr/hdr) with the is_*() predicates
 * above -- do not assume list order or that raw/ldr/hdr partition disjointly for GUI purposes
 * (dt_image_ext_is_gui_raster() can be true for an extension physically stored in the raw list). */
const char *const *dt_image_ext_raw_list(void);
const char *const *dt_image_ext_ldr_list(void);
const char *const *dt_image_ext_hdr_list(void);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_IMAGE_EXTENSIONS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
