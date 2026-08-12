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

/** @file metadata/exif_internal.h
 *
 * @brief The seam between `metadata/exif.cc` and `common/xmp_sidecar.cc`.
 *
 * @details Those two files were one 4775-line translation unit until they were cut apart,
 * and three functions plus the exiv2 lock are genuinely wanted by both halves. They are
 * declared here rather than in `metadata/exif.h` because nothing outside those two files
 * has any business calling them: they are the private remainder of the cut, not API.
 *
 * Include it from exactly those two `.cc` files. If a third consumer ever appears, that is
 * the signal to promote what it needs into `metadata/exif.h` deliberately, rather than to
 * widen this one.
 */

#ifndef DT_METADATA_EXIF_INTERNAL_H
#define DT_METADATA_EXIF_INTERNAL_H

#include "common/global_mutexes.h"
#include "common/image.h"

#include <exiv2/exiv2.hpp>
#include <string>

/**
 * @brief RAII guard over the process-wide exiv2 lock.
 *
 * @details exiv2's readMetadata() is not thread safe in 0.26, and it throws, so the unlock
 * has to survive an exception -- hence a destructor rather than a matched pair of calls.
 * FIXME: check again once we rely on 0.27.
 *
 * The mutex itself is reached through dt_exiv2_threadsafe_mutex() rather than named
 * directly, so this header needs no `darktable.h` -- which it could not have anyway.
 */
class Lock
{
public:
  Lock() { dt_pthread_mutex_lock(dt_exiv2_threadsafe_mutex()); }
  ~Lock() { dt_pthread_mutex_unlock(dt_exiv2_threadsafe_mutex()); }
};

#define read_metadata_threadsafe(image)                       \
{                                                             \
  Lock exiv2_lock;                                            \
  image->readMetadata();                                      \
}

/** @brief Strip the given EXIF keys from @p exif, ignoring any that are absent. */
void dt_remove_exif_keys(Exiv2::ExifData &exif, const char *keys[], unsigned int n_keys);

/** @brief Read one EXIF tag into @p pos, FALSE if the image does not carry it.
 *
 * @details Behind the FIND_EXIF_TAG() macro in `exif.cc`; the sidecar reader calls it
 * directly, on the *old* EXIF block of an image being re-read. */
bool dt_exif_read_exif_tag(Exiv2::ExifData &exifData, Exiv2::ExifData::const_iterator *pos,
                           std::string key);

/** @brief Apply one XMP packet's metadata, tags and colour labels to @p img.
 *
 * @details @p version is the Xmp.darktable.xmp_version of the packet, or -1 when the packet
 * did not come from us. @p exif_read says whether EXIF was already read for this image, so
 * that XMP is allowed to trump it. */
bool dt_exif_decode_xmp_data(dt_image_t *img, Exiv2::XmpData &xmpData, int version,
                             bool exif_read);

#endif // DT_METADATA_EXIF_INTERNAL_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
