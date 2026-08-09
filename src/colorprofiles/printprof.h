/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 johannes hanika.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2014 Jérémy Rosen.
    Copyright (C) 2014-2015, 2020 Pascal Obry.
    Copyright (C) 2015-2017 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
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

/**
 * @file printprof.h
 * @brief The printer-profile application path: the last colour transform an image goes
 * through before it is handed to the PDF spooler.
 *
 * @details The print job (`_print_job_run` in `libs/print_settings.c`) exports each image
 * through the normal pipeline into a plain interleaved RGB buffer, then calls in here to
 * convert that buffer from the image's output profile to the printer's own profile.
 *
 * This is the one entry point of the module where the caller supplies both cmsHPROFILE
 * handles itself instead of naming a profile by identity. Nothing lcms2 escapes even so:
 * the cmsHTRANSFORM is built, used and destroyed inside the single call below, and the two
 * profiles are only ever borrowed. There is no profile list, no memo and no cached
 * transform behind this header — it is a pure "apply" step, with no CRUDE half.
 */

#ifndef DT_COLORPROFILES_PRINTPROF_H
#define DT_COLORPROFILES_PRINTPROF_H

#include <glib.h>
#include <inttypes.h>
#include <lcms2.h>
#include <stddef.h>

/**
 * @brief Convert an interleaved RGB buffer from @p hInProfile to @p hOutProfile, replacing
 * the caller's buffer with a freshly allocated 8 bpp result.
 *
 * @details Builds a one-shot transform (RGB in, `bpp == 8 ? 1 : 2` bytes per channel;
 * always 1 byte per channel out), runs it one image row per `cmsDoTransform()` call over an
 * OpenMP-parallel row loop, then destroys the transform. On success the incoming buffer is
 * released and @p in is repointed at the result.
 *
 * The routine takes an image of 8 or 16 bpp but always returns an 8 bpp result. It is indeed
 * better to apply the profile to a 16-bit input, but we do not need more than 8 bits out for
 * printing — and the caller relies on exactly that: `_export_image()` exports at 16 bpp
 * precisely when a printer profile is to be applied, at 8 bpp otherwise, and in both cases
 * the buffer that reaches the PDF is 8 bpp.
 *
 * The output colour space is read from the printer profile (`cmsGetColorSpace()`), but the
 * channel count and the byte count of the output format descriptor are hard-coded to 3 and 1,
 * and the destination buffer is sized `3 * width * height` to match. Only three-channel,
 * RGB-like printer profiles are therefore handled. Planarity is inherited from the input
 * descriptor, which is always interleaved, so the result is always interleaved too.
 *
 * Both handles are BORROWED for the duration of the call: neither is closed, duplicated nor
 * retained. lcms2 has no `cmsDupProfile()` and a transform does not keep a reference to its
 * source profiles, so the handles only have to stay valid until `cmsCreateTransform()`
 * returns — which happens inside this call.
 *
 * No lock is taken here. A caller passing a handle that belongs to the application-wide
 * profile list must hold dt_colorspaces_lock_profiles() across the call, because the
 * DT_COLORSPACE_DISPLAY entry's handle can be closed and replaced underneath it.
 *
 * Thread: the print job is queued on DT_JOB_QUEUE_USER_EXPORT, so this runs on an export
 * worker thread and never on the GUI thread. Inside the call, the single transform handle is
 * shared by every OpenMP thread of the row loop; no `cmsFLAGS_NOCACHE` is requested.
 *
 * @param in In/out. On entry, the caller's interleaved RGB buffer of @p width × @p height
 * pixels at @p bpp bits per channel. On success, the old buffer is freed with dt_free() and
 * `*in` is overwritten with a newly allocated `3 * width * height` byte buffer that the
 * caller then owns (the sole caller releases it with dt_free()). On failure `*in` is left
 * exactly as it was and still belongs to the caller.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param bpp Bits per channel of the input buffer: 8 or 16. Anything that is not exactly 8
 * is treated as 16.
 * @param hInProfile Source profile — the profile the input buffer is already encoded in.
 * @param hOutProfile Destination (printer) profile.
 * @param intent Rendering intent, passed straight to `cmsCreateTransform()` with no
 * translation. `dt_iop_color_intent_t` and the lcms2 `INTENT_*` macros share their numeric
 * values (perceptual 0, relative colorimetric 1, saturation 2, absolute colorimetric 3), which
 * is why either spelling works and why introducing a value in one of them alone would silently
 * mean something else here.
 * @param black_point_compensation TRUE adds `cmsFLAGS_BLACKPOINTCOMPENSATION` to the
 * transform; nothing else about the conversion changes.
 * @return 0 on success. 1 on failure, which is either handle being NULL, or
 * `cmsCreateTransform()` returning NULL (reported on stderr as "error printer profile may be
 * corrupted"). Both failure paths return before the buffer is touched.
 * @warning The destination allocation is not checked: at print resolutions this is a large
 * buffer, and an allocation failure dereferences NULL in the transform loop rather than
 * returning 1.
 * @note The incoming buffer is released with dt_free() — that is `g_free()` — while the
 * replacement comes from plain `malloc()`. The sole caller likewise allocates the incoming
 * buffer with `malloc()` and releases the result with dt_free(), so the two allocators are
 * used interchangeably all along this path.
 */
int dt_apply_printer_profile(void **in, uint32_t width, uint32_t height, int bpp, cmsHPROFILE hInProfile,
                             cmsHPROFILE hOutProfile, int intent, gboolean black_point_compensation);

#endif // DT_COLORPROFILES_PRINTPROF_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

