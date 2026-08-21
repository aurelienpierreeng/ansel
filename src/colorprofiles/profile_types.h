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

/**
 * @file profile_types.h
 * @brief The colour-profile vocabulary, and nothing else.
 *
 * @details These enums are serialised into iop params blobs in the library database and into
 * XMP sidecars, so their numeric values are frozen ABI -- they cannot move or be renumbered.
 * That is not an abstract worry: `dt_iop_colorin_params_t` stores two
 * `dt_colorspaces_color_profile_type_t` (input and working), a `dt_iop_color_intent_t` and two
 * `char[DT_IOP_COLOR_ICC_LEN]`; `dt_iop_colorout_params_t` stores one of each. Those structs are
 * memcpy'd into `main.history.op_params` and base64'd into `Xmp.darktable.history_params`
 * (`common/exif.cc`). The export module serialises `icctype`/`iccintent` into its preset blobs
 * too, and already carries several `legacy_params` versions to migrate them (`libs/export.c`).
 * Renumbering any enumerator below silently repoints every stored edit at a different colour
 * space.
 *
 * @details Almost everything that includes colorprofiles/ wants only this: a profile type to
 * store in its params, an intent to pass along. Carrying that vocabulary in the same header as
 * the module's API meant <lcms2.h> and <pthread.h> reached several hundred translation units
 * that never call either -- 300 of 425 at the point this split was made.
 *
 * @warning Nothing here may include lcms2, pthread or GTK. <glib.h> is the one dependency, for
 * gboolean and MIN. Adding any other include re-creates the fan-out this file exists to stop.
 *
 * @see colorprofiles/colorspaces.h for the API that acts on these values (CRUDE metadata
 * queries, the lock, and dt_colorspaces_apply_profile()).
 */

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Size of every ICC filename buffer that crosses a module boundary.
 *
 * @details 512 bytes. This is the declared width of `filename` inside the serialised iop params
 * of colorin and colorout, so it is part of the params ABI: changing it changes the size of
 * every stored history entry and every XMP blob. It is also the width of
 * `dt_colorspaces_color_profile_t::filename` and of the filename in the value-copy descriptor
 * the CRUDE calls hand back, so the three never need a length negotiation.
 */
#define DT_IOP_COLOR_ICC_LEN 512


/**
 * @brief ICC rendering intent, as stored in iop params and in conf.
 *
 * @details Spelled as literals rather than as lcms2's INTENT_* so this header needs no
 * <lcms2.h> -- the values are fixed by the ICC specification, and colorspaces.c static-asserts
 * that they still match lcms2's (four `_Static_assert`s, one per intent). A divergence is
 * therefore a compile error in the one translation unit that talks to lcms2, not a silently
 * wrong render everywhere else.
 */
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL = 0,
  DT_INTENT_RELATIVE_COLORIMETRIC = 1,
  DT_INTENT_SATURATION = 2,
  DT_INTENT_ABSOLUTE_COLORIMETRIC = 3,
  /**
   * @brief Count of real intents, and the codebase's "unset" marker -- never a user choice.
   *
   * @details `dt_dev_pixelpipe_t::icc_intent` is initialised to it, and colorout only lets the
   * pipe override the module's own intent when `pipe->icc_intent < DT_INTENT_LAST`; ansel-cli
   * returns it for an unrecognised `--icc-intent` string. Storing it in params would mean
   * storing an out-of-range intent.
   */
  DT_INTENT_LAST
} dt_iop_color_intent_t;

/**
 * @brief Which of the five user-visible profile SLOTS a notification concerns.
 *
 * @details Not a colour space: it names a role in the UI. Its only use is
 * as the payload of DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, raised by colorin (input, working),
 * the display actions (display) and the darkroom toolbox (softproof), and read back by
 * listeners such as basicadj and the thumbtable. The signal bus carries it as a `uint8_t`.
 *
 * @note Numbering starts at 1: no member has the value 0, so a zero-initialised variable is not
 * a valid slot and cannot accidentally compare equal to one.
 * @note DT_COLORSPACES_PROFILE_TYPE_EXPORT is declared but dead -- nothing in the tree raises
 * or tests it, so changing the export profile notifies no one. Kept for the numbering.
 */
typedef enum dt_colorspaces_profile_type_t
{
  DT_COLORSPACES_PROFILE_TYPE_INPUT = 1,
  DT_COLORSPACES_PROFILE_TYPE_WORK = 2,
  DT_COLORSPACES_PROFILE_TYPE_EXPORT = 3,
  DT_COLORSPACES_PROFILE_TYPE_DISPLAY = 4,
  DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF = 5
} dt_colorspaces_profile_type_t;

/**
 * @brief Identity of a colour space, as stored in iop params, conf keys and export presets.
 *
 * @details Frozen numbering (see the @file block). The surprise is that being a member here
 * does NOT imply having an entry in the module's profile list: several values name things the
 * list cannot hold, and each of those is a distinct trap, documented on the enumerator itself.
 *
 * @details The list is built once by `_colorspaces_build()` and never appended to afterwards.
 * It holds 21 built-in entries in a fixed registration order, then the user's own ICC files
 * (type DT_COLORSPACE_FILE) read from `color/in` and then `color/out` under the user config
 * dir, falling back to the data dir, sorted by name within each batch. Registration order IS
 * the combo-box order for every role: the k-th entry serving a role IS row k of that menu,
 * which is what every stored combo index in every preset and conf key refers to. Appending an
 * entry anywhere but the end therefore shifts other people's saved settings.
 */
typedef enum dt_colorspaces_color_profile_type_t
{
  /** @brief No profile / "take it from the image settings". Never matches a list entry. */
  DT_COLORSPACE_NONE = -1,
  /**
   * @brief A user ICC file on disk; the only type whose `filename` is meaningful.
   *
   * @details Every lookup for this type compares filenames rather than identity, and does so
   * with `dt_colorspaces_is_profile_equal()` (basename-tolerant, because an iop may have
   * recorded a bare basename). Two different files therefore share this one enum value.
   */
  DT_COLORSPACE_FILE = 0,
  /**
   * @brief sRGB -- registered TWICE, and the two entries are not interchangeable.
   *
   * @details A v4 profile with parametric curves is registered for INPUT, and a v2 profile
   * with a point TRC is registered for output/display/category/work. Nothing but the pattern
   * of -1 in their five position fields distinguishes them: same type, same empty filename.
   * A lookup with a multi-bit role mask returns the FIRST match in registration order,
   * which for sRGB is the v4 input entry -- so asking for "sRGB, any role" while meaning
   * the working profile hands back the wrong variant. Pass DT_PROFILE_ROLE_WORKING (or OUT,
   * or DISPLAY) explicitly.
   */
  DT_COLORSPACE_SRGB = 1,
  DT_COLORSPACE_ADOBERGB = 2,
  DT_COLORSPACE_LIN_REC709 = 3,
  DT_COLORSPACE_LIN_REC2020 = 4,
  DT_COLORSPACE_XYZ = 5,
  DT_COLORSPACE_LAB = 6,
  DT_COLORSPACE_INFRARED = 7,
  /**
   * @brief The monitor profile -- the one list entry that mutates after init.
   *
   * @details Registered with an sRGB placeholder so that code running before the real profile
   * can be fetched has something to work with, then replaced in place (old handle closed, new
   * one stored) every time the system profile changes -- which includes moving or resizing the
   * window onto another monitor.
   *
   * @warning Anything that derives from this entry's `cmsHPROFILE` (a matrix extraction, a
   * `cmsCreateTransform`) must hold `dt_colorspaces_lock_profiles()` for the span of the
   * derivation, or it can be reading a handle that has just been deleted.
   */
  DT_COLORSPACE_DISPLAY = 8,
  /**
   * @brief Image-derived profiles: enum 9..14, NOT registered in the list.
   *
   * @details These describe a profile that exists only for one image -- the ICC embedded in
   * that file, or a matrix taken from that camera's data by colorin. They cannot be resolved by
   * identity, because there is no list entry to find, and they must not be shared: the pipe
   * that built one owns it (`dt_dev_pixelpipe_t::owned_input_profile_info`) rather than putting
   * it in the module-wide derived-profile memo, where a `(type, "")` key would be common to
   * every image of the same camera-matrix kind. `develop/iop_profile.c` tests exactly this
   * closed range, so a new image-derived type has to be added inside it.
   */
  DT_COLORSPACE_EMBEDDED_ICC = 9,
  DT_COLORSPACE_EMBEDDED_MATRIX = 10,
  DT_COLORSPACE_STANDARD_MATRIX = 11,
  DT_COLORSPACE_ENHANCED_MATRIX = 12,
  DT_COLORSPACE_VENDOR_MATRIX = 13,
  DT_COLORSPACE_ALTERNATE_MATRIX = 14,
  DT_COLORSPACE_BRG = 15,
  /**
   * @brief Category placeholders: "whatever the export/softproof/work setting currently says".
   *
   * @details These three ARE registered, with `profile == NULL` and with an EMPTY role mask
   * -- they exist to occupy a row in a combo box, not to be resolved.
   * `dt_colorspaces_get_profile()` returns NULL for them, but not because it knows they are
   * categories: its predicate tests the role mask, and theirs selects nothing. That omission is the entire reason a lookup cannot hand back an entry whose
   * `->profile` is NULL, which many call sites dereference unchecked. Giving categories a
   * role of their own would break them all.
   */
  DT_COLORSPACE_EXPORT = 16,
  DT_COLORSPACE_SOFTPROOF = 17,
  DT_COLORSPACE_WORK = 18,
  /**
   * @brief Dead value, kept only because old params and conf keys contain it.
   *
   * @details The second-display feature it belonged to is gone: there is no list entry, no
   * backing position field, and `dt_colorspaces_get_name()` answers "Not used. Shouldn't be
   * here." It cannot be deleted (the numbering is frozen), so it is remapped instead --
   * see sanitize_colorspaces().
   */
  DT_COLORSPACE_DISPLAY2 = 19,
  DT_COLORSPACE_REC709 = 20,
  DT_COLORSPACE_PROPHOTO_RGB = 21,
  DT_COLORSPACE_PQ_REC2020 = 22,
  DT_COLORSPACE_HLG_REC2020 = 23,
  DT_COLORSPACE_PQ_P3 = 24,
  DT_COLORSPACE_HLG_P3 = 25,
  DT_COLORSPACE_ITUR_BT1886 = 26,
  DT_COLORSPACE_DISPLAY_P3 = 27,
  /** @brief Count, not a colour space. The last usable value is DT_COLORSPACE_LAST - 1. */
  DT_COLORSPACE_LAST = 28
} dt_colorspaces_color_profile_type_t;

/**
 * @brief What the output transform is being asked to show: the picture, or a proof of it.
 *
 * @details Application-wide state, not per-image: one setting shared by the darkroom toolbox
 * toggles, the backbuf overlay label and colorout's transform construction (GAMUTCHECK adds
 * `cmsFLAGS_GAMUTCHECK` to the proofing transform). It is persisted across sessions in the
 * `ui_last/color/mode` conf key and range-checked on load, so an out-of-range stored value
 * falls back to DT_PROFILE_NORMAL rather than reaching lcms2.
 */
typedef enum dt_colorspaces_color_mode_t
{
  DT_PROFILE_NORMAL = 0,
  DT_PROFILE_SOFTPROOF,
  DT_PROFILE_GAMUTCHECK
} dt_colorspaces_color_mode_t;

/**
 * @brief Which use a profile is eligible for -- the mandatory filter on every lookup and
 * enumeration.
 *
 * @details A bitmask, but read it as "which combo box would list this profile", not as a
 * colour-management direction. That distinction matters because the module has two entries for
 * DT_COLORSPACE_SRGB distinguished by nothing else (see DT_COLORSPACE_SRGB): the role is
 * what picks between them, so it cannot be omitted or approximated.
 *
 * @details A multi-bit mask resolves to the FIRST entry in registration order that serves any
 * of its bits. Calls that return or consume a combo-box INDEX therefore require a single bit:
 * an index means nothing outside the enumeration that produced it, and an index taken from
 * INPUT|OUTPUT equals neither menu's row number.
 *
 * @note This used to be called a "direction", which it is not: a profile is RGB->PCS or
 * PCS->RGB, and nothing else. What these bits select is which MENU an entry belongs to, and
 * the menus genuinely differ from one another -- see DT_PROFILE_ROLE_MONITOR. Two further
 * bits, CATEGORY and DISPLAY2, were declared and never tested by any lookup: one had a
 * position field no predicate consulted, the other had no field at all. They are gone, which
 * changes nothing at runtime and stops DT_PROFILE_ROLE_ANY claiming six meanings when it had
 * four.
 */
typedef enum dt_colorspaces_profile_role_t
{
  /** @brief Listed in the input-profile combo (colorin). */
  DT_PROFILE_ROLE_INPUT = 1 << 0,
  /** @brief Listed in the output/export-profile combo (colorout, export). */
  DT_PROFILE_ROLE_OUTPUT = 1 << 1,
  /**
   * @brief Eligible for the monitor-profile menu.
   *
   * @details Not the same set as OUTPUT, which is why it exists. Of the 21 built-in
   * registrations, five diverge: DT_COLORSPACE_DISPLAY is monitor-only, DT_COLORSPACE_REC709
   * and DT_COLORSPACE_ITUR_BT1886 are output-only, and DT_COLORSPACE_XYZ and DT_COLORSPACE_LAB
   * become output-only once the `allow_lab_output` conf key gives them the OUTPUT role.
   * Substituting one for the other is a behaviour change, not a rename.
   */
  DT_PROFILE_ROLE_MONITOR = 1 << 2,
  /** @brief Listed in the working-profile combo (colorin). */
  DT_PROFILE_ROLE_WORKING = 1 << 3,
  /**
   * @brief All four roles, in registration order.
   *
   * @details Multi-bit, so it returns the FIRST entry that serves any role -- which for
   * DT_COLORSPACE_SRGB, registered twice, is the v4 input-only variant. Never pass it when the
   * answer will be used as a combo index, and never pass it when the caller means one specific
   * role. It is the right answer only for "resolve this identity to a profile handle, I do not
   * care which menu it appears in".
   */
  DT_PROFILE_ROLE_ANY = DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR
                        | DT_PROFILE_ROLE_WORKING
} dt_colorspaces_profile_role_t;

/**
 * @brief CICP colour primaries, as tagged in AVIF/HEIF/JPEG XL containers.
 *
 * @details Values are fixed by Recommendation ITU-T H.273, not by Ansel: this is an external
 * ABI and the gaps in the numbering are the codepoints for primaries the codebase does not
 * handle. Only imageio reads them, to map a container's CICP triplet onto one of the
 * DT_COLORSPACE_* values above.
 */
typedef enum dt_colorspaces_cicp_color_primaries_t
{
    DT_CICP_COLOR_PRIMARIES_REC709 = 1,
    DT_CICP_COLOR_PRIMARIES_UNSPECIFIED = 2,
    DT_CICP_COLOR_PRIMARIES_REC2020 = 9,
    DT_CICP_COLOR_PRIMARIES_XYZ = 10,
    DT_CICP_COLOR_PRIMARIES_P3 = 12 // D65
} dt_colorspaces_cicp_color_primaries_t;

/**
 * @brief CICP transfer characteristics, as tagged in AVIF/HEIF/JPEG XL containers.
 *
 * @details Values fixed by Recommendation ITU-T H.273. Several are treated as equivalent by the
 * imageio mapping specifically to tolerate mistagged files (Rec601 and the two Rec2020 bit
 * depths are accepted wherever Rec709 is expected).
 */
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

/**
 * @brief CICP matrix coefficients, as tagged in AVIF/HEIF/JPEG XL containers.
 *
 * @details Values fixed by Recommendation ITU-T H.273. IDENTITY is what a lossless or 4:4:4 RGB
 * file carries; the imageio mapping accepts it, the matching YCbCr codepoint, UNSPECIFIED, and
 * a couple of commonly mistagged neighbours for the same primaries.
 */
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

/**
 * @brief Coerce an integer read back from storage into a colour space this build still has.
 *
 * @details Two distinct rescues, both for values that were valid when they were written:
 * DT_COLORSPACE_DISPLAY2 is remapped to DT_COLORSPACE_DISPLAY (the second-display feature is
 * gone, but the number survives in old conf keys and params), and anything at or above
 * DT_COLORSPACE_LAST -- a file written by a newer build -- is clamped to the last enumerator
 * this build knows.
 *
 * @param colorspace the stored value, typically straight out of `dt_conf_get_int()`.
 * @return a value this build can look up. Not necessarily the caller's colour space: clamping
 * silently substitutes DT_COLORSPACE_DISPLAY_P3 for anything unknown.
 * @warning Only the upper end is clamped. DT_COLORSPACE_NONE (-1) passes through unchanged,
 * which is intended, but so does any other negative value -- MIN() cannot catch those. A caller
 * that must reject garbage has to test for it separately.
 */
static inline dt_colorspaces_color_profile_type_t sanitize_colorspaces(dt_colorspaces_color_profile_type_t colorspace)
{
  // Remap unused colorspaces to valid ones
  if(colorspace == DT_COLORSPACE_DISPLAY2)
    return DT_COLORSPACE_DISPLAY;
  else
    return (dt_colorspaces_color_profile_type_t)MIN(colorspace, DT_COLORSPACE_LAST - 1);
}

/**
 * @brief Is this one of the four camera-matrix profile kinds?
 *
 * @details Matches DT_COLORSPACE_STANDARD_MATRIX, _ENHANCED_MATRIX, _VENDOR_MATRIX and
 * _ALTERNATE_MATRIX -- the types whose matrix comes from the camera data for one image, and
 * which therefore have no entry in the profile list to resolve against.
 *
 * @param type the profile type stored in a module's params.
 * @return TRUE for the four raw-matrix types only.
 * @note Deliberately narrower than the image-derived range documented on
 * DT_COLORSPACE_EMBEDDED_ICC: DT_COLORSPACE_EMBEDDED_ICC and DT_COLORSPACE_EMBEDDED_MATRIX are
 * also unregistered, but they come from the file rather than from the camera database, so they
 * are not covered here.
 * @note The one caller uses it to stay quiet rather than to select behaviour: colorin suppresses
 * its "profile not found" warning when a stored raw-matrix type cannot be resolved on an image
 * whose matrix correction is unsupported, which is an expected outcome and not a user error.
 */
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
