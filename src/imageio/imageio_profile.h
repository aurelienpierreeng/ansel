/*
    This file is part of darktable,
    Copyright (C) 2010 Alex Chateau.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2013, 2016-2017 johannes hanika.
    Copyright (C) 2010 José Carlos García Sogo.
    Copyright (C) 2010, 2012-2014 Pascal de Bruijn.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2018 Tobias Ellinghaus.
    Copyright (C) 2011, 2013-2014 Ulrich Pegelow.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 parafin.
    Copyright (C) 2016 Peter Budai.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019 Matthias Vogelgesang.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 a.
    Copyright (C) 2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Miloš Komarčević.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    
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

#ifndef DT_IMAGEIO_IMAGEIO_PROFILE_H
#define DT_IMAGEIO_IMAGEIO_PROFILE_H

/* Choosing an ICC profile for an image, and reading the one its file carries. Codec work: see
 * the .c for why it is not in common/colorspaces.c. */

#include "colorprofiles/colorspaces.h"

#include <lcms2.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief Best effort to find a suitable (input) color profile for a given image, using embedded ICC or EXIF whenever possible.
 * This will also init the profile in the image cache.
 *
 * @param imgid ID of the picture
 * @param output If not NULL, writes the generated color profile into this pointer.
 * @param new_profile Will be set to true if a new profile was generated (aka embedded profile) and will need to be freed.
 * If false and output profile is returned, the profile returned already exists on the public list of profiles and should not be freed.
 * @return dt_colorspaces_color_profile_type_t type of profile detected. This type is tested internally and guaranteed to work.
 */
dt_colorspaces_color_profile_type_t dt_image_find_best_color_profile(int32_t imgid, cmsHPROFILE *output, gboolean *new_profile);

/**
 * @brief Resolve an embedded/matrix input profile for a given image, honoring the requested type when possible.
 * For non-RAW images, falls back to dt_image_find_best_color_profile() to avoid invalid matrix usage.
 *
 * @param imgid ID of the picture
 * @param requested Requested embedded/matrix profile type
 * @param output If not NULL, writes the generated color profile into this pointer.
 * @param new_profile Will be set to true if a new profile was generated (aka embedded profile) and will need to be freed.
 * @return dt_colorspaces_color_profile_type_t resolved type, or DT_COLORSPACE_NONE on error.
 */
dt_colorspaces_color_profile_type_t dt_colorspaces_get_input_profile_from_image(
    int32_t imgid,
    dt_colorspaces_color_profile_type_t requested,
    cmsHPROFILE *output,
    gboolean *new_profile);

/** return the output profile as set in colorout, taking export override into account if passed in. */
const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile(const int32_t imgid,
                                                                        dt_colorspaces_color_profile_type_t *over_type,
                                                                        const char *over_filename);

/** return the embedded profile of a particular image **/
const cmsHPROFILE dt_colorspaces_get_embedded_profile(const int32_t imgid, dt_colorspaces_color_profile_type_t *type, gboolean *new_profile);

/** try to infer profile type from CICP */
dt_colorspaces_color_profile_type_t dt_colorspaces_cicp_to_type(const dt_colorspaces_cicp_t *cicp, const char *filename);

G_END_DECLS

#endif // DT_IMAGEIO_IMAGEIO_PROFILE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
