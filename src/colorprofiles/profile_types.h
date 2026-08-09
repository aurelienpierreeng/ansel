/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010-2012, 2017 johannes hanika.
    Copyright (C) 2010 José Carlos García Sogo.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012, 2014 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2014, 2019-2022 Pascal Obry.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2014 Ulrich Pegelow.
    Copyright (C) 2015-2016 Pedro Côrte-Real.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020, 2022-2025 Aurélien PIERRE.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Miloš Komarčević.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025 Alynx Zhou.
    
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

#ifndef DT_COLORPROFILES_PROFILE_TYPES_H
#define DT_COLORPROFILES_PROFILE_TYPES_H

/* The colour-profile vocabulary, and nothing else.
 *
 * These enums are serialised into iop params blobs in the library database and into XMP
 * sidecars, so their numeric values are frozen ABI -- they cannot move or be renumbered.
 * Almost everything that includes colorprofiles/ wants only this: a profile type to store
 * in its params, an intent to pass along. Carrying that vocabulary in the same header as
 * the module's API meant <lcms2.h> and <pthread.h> reached several hundred translation
 * units that never call either.
 *
 * Nothing here may include lcms2, pthread or GTK. <glib.h> is the one dependency, for
 * gboolean and MIN. */

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 512


/* ICC rendering intents. Spelled as literals rather than as lcms2's INTENT_* so this
 * header needs no <lcms2.h> -- the values are fixed by the ICC specification, and
 * colorspaces.c static-asserts that they still match lcms2's. */
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL = 0,
  DT_INTENT_RELATIVE_COLORIMETRIC = 1,
  DT_INTENT_SATURATION = 2,
  DT_INTENT_ABSOLUTE_COLORIMETRIC = 3,
  DT_INTENT_LAST
} dt_iop_color_intent_t;

typedef enum dt_colorspaces_profile_type_t
{
  DT_COLORSPACES_PROFILE_TYPE_INPUT = 1,
  DT_COLORSPACES_PROFILE_TYPE_WORK = 2,
  DT_COLORSPACES_PROFILE_TYPE_EXPORT = 3,
  DT_COLORSPACES_PROFILE_TYPE_DISPLAY = 4,
  DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF = 5
} dt_colorspaces_profile_type_t;

typedef enum dt_colorspaces_color_profile_type_t
{
  DT_COLORSPACE_NONE = -1,
  DT_COLORSPACE_FILE = 0,
  DT_COLORSPACE_SRGB = 1,
  DT_COLORSPACE_ADOBERGB = 2,
  DT_COLORSPACE_LIN_REC709 = 3,
  DT_COLORSPACE_LIN_REC2020 = 4,
  DT_COLORSPACE_XYZ = 5,
  DT_COLORSPACE_LAB = 6,
  DT_COLORSPACE_INFRARED = 7,
  DT_COLORSPACE_DISPLAY = 8,
  DT_COLORSPACE_EMBEDDED_ICC = 9,
  DT_COLORSPACE_EMBEDDED_MATRIX = 10,
  DT_COLORSPACE_STANDARD_MATRIX = 11,
  DT_COLORSPACE_ENHANCED_MATRIX = 12,
  DT_COLORSPACE_VENDOR_MATRIX = 13,
  DT_COLORSPACE_ALTERNATE_MATRIX = 14,
  DT_COLORSPACE_BRG = 15,
  DT_COLORSPACE_EXPORT = 16, // export and softproof are categories and will return NULL with dt_colorspaces_get_profile()
  DT_COLORSPACE_SOFTPROOF = 17,
  DT_COLORSPACE_WORK = 18,
  DT_COLORSPACE_DISPLAY2 = 19,
  DT_COLORSPACE_REC709 = 20,
  DT_COLORSPACE_PROPHOTO_RGB = 21,
  DT_COLORSPACE_PQ_REC2020 = 22,
  DT_COLORSPACE_HLG_REC2020 = 23,
  DT_COLORSPACE_PQ_P3 = 24,
  DT_COLORSPACE_HLG_P3 = 25,
  DT_COLORSPACE_ITUR_BT1886 = 26,
  DT_COLORSPACE_DISPLAY_P3 = 27,
  DT_COLORSPACE_LAST = 28
} dt_colorspaces_color_profile_type_t;

typedef enum dt_colorspaces_color_mode_t
{
  DT_PROFILE_NORMAL = 0,
  DT_PROFILE_SOFTPROOF,
  DT_PROFILE_GAMUTCHECK
} dt_colorspaces_color_mode_t;

typedef enum dt_colorspaces_profile_direction_t
{
  DT_PROFILE_DIRECTION_IN = 1 << 0,
  DT_PROFILE_DIRECTION_OUT = 1 << 1,
  DT_PROFILE_DIRECTION_DISPLAY = 1 << 2,
  DT_PROFILE_DIRECTION_CATEGORY = 1 << 3, // categories will return NULL with dt_colorspaces_get_profile()
  DT_PROFILE_DIRECTION_WORK = 1 << 4,
  DT_PROFILE_DIRECTION_DISPLAY2 = 1 << 5,
  DT_PROFILE_DIRECTION_ANY = DT_PROFILE_DIRECTION_IN | DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY
                             | DT_PROFILE_DIRECTION_CATEGORY
                             | DT_PROFILE_DIRECTION_WORK
                             | DT_PROFILE_DIRECTION_DISPLAY2
} dt_colorspaces_profile_direction_t;

/* CICP color primaries (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_color_primaries_t
{
    DT_CICP_COLOR_PRIMARIES_REC709 = 1,
    DT_CICP_COLOR_PRIMARIES_UNSPECIFIED = 2,
    DT_CICP_COLOR_PRIMARIES_REC2020 = 9,
    DT_CICP_COLOR_PRIMARIES_XYZ = 10,
    DT_CICP_COLOR_PRIMARIES_P3 = 12 // D65
} dt_colorspaces_cicp_color_primaries_t;

/* CICP transfer characteristics (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_transfer_characteristics_t
{
    DT_CICP_TRANSFER_CHARACTERISTICS_REC709 = 1,
    DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED = 2,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC601 = 6,
    DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR = 8,
    DT_CICP_TRANSFER_CHARACTERISTICS_SRGB = 13,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_10B = 14,
    DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_12B = 15,
    DT_CICP_TRANSFER_CHARACTERISTICS_PQ = 16,
    DT_CICP_TRANSFER_CHARACTERISTICS_HLG = 18
} dt_colorspaces_cicp_transfer_characteristics_t;

/* CICP matrix coefficients (Recommendation ITU-T H.273) */
typedef enum dt_colorspaces_cicp_matrix_coefficients_t
{
    DT_CICP_MATRIX_COEFFICIENTS_IDENTITY = 0,
    DT_CICP_MATRIX_COEFFICIENTS_REC709 = 1,
    DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED = 2,
    DT_CICP_MATRIX_COEFFICIENTS_SYCC = 5,
    DT_CICP_MATRIX_COEFFICIENTS_REC601 = 6,
    DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL = 9,
    DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL = 12
} dt_colorspaces_cicp_matrix_coefficients_t;

static inline dt_colorspaces_color_profile_type_t sanitize_colorspaces(dt_colorspaces_color_profile_type_t colorspace)
{
  // Remap unused colorspaces to valid ones
  if(colorspace == DT_COLORSPACE_DISPLAY2)
    return DT_COLORSPACE_DISPLAY;
  else
    return (dt_colorspaces_color_profile_type_t)MIN(colorspace, DT_COLORSPACE_LAST - 1);
}

static inline gboolean dt_colorspaces_is_raw_matrix_profile_type(const dt_colorspaces_color_profile_type_t type)
{
  return (type == DT_COLORSPACE_STANDARD_MATRIX
          || type == DT_COLORSPACE_ENHANCED_MATRIX
          || type == DT_COLORSPACE_VENDOR_MATRIX
          || type == DT_COLORSPACE_ALTERNATE_MATRIX);
}

#ifdef __cplusplus
}
#endif

#endif /* DT_COLORPROFILES_PROFILE_TYPES_H */
