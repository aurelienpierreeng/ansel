/*
    This file is part of darktable,
    Copyright (C) 2025 Aurélien PIERRE.

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

/** @file colorprofiles/conversion.h
 *
 * @brief A PREPARED CONVERSION: everything needed to turn pixels in one colour space into
 * pixels in another, built once and applied many times.
 *
 * @details This is the "prepare" half of colour management, and it exists so that no module
 * outside `src/colorprofiles/` ever has to do it. Preparing a conversion by hand means
 * resolving two profiles, pinning them for exactly as long as the derivation takes, deciding
 * whether they reduce to matrices and tone curves or need lcms2, extracting six 65536-entry
 * curves, fitting the power laws that carry values above white, building a `cmsHTRANSFORM`
 * with the right flags when they do not, and closing whichever handles turned out to be
 * owned rather than borrowed. `iop/colorin.c` and `iop/colorout.c` each open-coded that,
 * differently, and between them held two of the bugs this module was closed to prevent: a
 * profile handle read without a lock across the window in which a monitor change replaces it,
 * and a transform built from settings that were re-read, field by field, after they had been
 * hashed.
 *
 * The object below is what both of them were re-implementing:
 *
 *     source RGB --[source curves]--> linear --[matrix]--> linear --[target curves]--> target
 *
 * with an optional clamp to [0,1] in a third space's primaries part-way through (gamut
 * clipping), and a whole-pipeline lcms2 fallback for the profiles that do not reduce to that
 * form. Which branch runs is decided here, from the profiles, and callers neither choose nor
 * see it -- exactly as dt_colorspaces_apply_profile() already does for the RGB<->Lab leg.
 *
 * @note Preparation is expensive (two 65536-entry curve extractions, or a 2.2-38 ms
 * `cmsCreateTransform`) and application is not. Build one in `commit_params()`, apply it in
 * `process()`, free it in `cleanup_pipe()`. Never build one per tile.
 *
 * @see colorprofiles/colorspaces.h for the profile list and the CRUDE metadata queries.
 * @see colorprofiles/iop_profile.h for dt_colorspaces_apply_profile(), the RGB<->Lab leg,
 *      which is the same idea against a single profile rather than a pair.
 */

#ifndef DT_COLORPROFILES_CONVERSION_H
#define DT_COLORPROFILES_CONVERSION_H

#include <glib.h>
#include <stddef.h>

#include "colorprofiles/profile_types.h"
#include "math/matrices.h"

/** @brief Entries per tone curve. Callers that upload the curves to a device need this to
 * size the buffer; the OpenCL paths in `iop/colorin.c` and `iop/colorout.c` upload it as a
 * 256 x 256 float image, which is exactly this many samples. */
#define DT_CONVERSION_LUT_SAMPLES 0x10000

struct dt_colorspaces_color_profile_t;

/**
 * @brief A prepared conversion. Opaque: its layout is the module's business, and the two
 * sentinel encodings that used to drive dispatch from the outside (`isnan(matrix[0][0])`,
 * `lut[c][0] < 0`) are no longer anyone else's to read.
 */
typedef struct dt_colorspaces_conversion_t dt_colorspaces_conversion_t;

/**
 * @brief One end of a conversion: a profile, named either by identity or by handing over one
 * this module already built for a single image.
 *
 * @details Most endpoints are registered profiles and are named by identity -- `{type,
 * filename}`, the same pair every preset and conf key stores. The exception is the
 * image-derived family (::DT_COLORSPACE_EMBEDDED_ICC through ::DT_COLORSPACE_ALTERNATE_MATRIX,
 * and an ICC embedded in the file being exported): those are not in the profile list and
 * cannot be resolved by identity at all, because their matrices come from one image's own
 * camera data. For those, resolve the image's profile first (`dt_image_get_input_profile()` /
 * `dt_image_get_output_profile()` in `imageio/imageio_profile.h`) and pass the container here
 * as ::resolved. The conversion borrows it: it must outlive the conversion, and the caller
 * still frees it with `dt_colorspaces_free_image_profile()`.
 */
typedef struct dt_colorspaces_endpoint_t
{
  /** @brief Profile identity. Ignored when ::resolved is set, except for tracing. */
  dt_colorspaces_color_profile_type_t type;
  /** @brief ICC file name, or "" / NULL for a built-in. Ignored when ::resolved is set. */
  const char *filename;
  /** @brief Which role the identity is looked up under. Load-bearing, never a formality:
   * ::DT_COLORSPACE_SRGB is registered twice and a multi-bit mask resolves to the first match
   * in registration order, so a working profile asked for with ::DT_PROFILE_ROLE_ANY comes
   * back as the input-only v4 variant. Ignored when ::resolved is set. */
  dt_colorspaces_profile_role_t role;
  /** @brief An already-resolved, image-owned profile, or NULL to resolve ::type / ::filename
   * from the registered list. BORROWED, not adopted -- see the struct details. */
  const struct dt_colorspaces_color_profile_t *resolved;
} dt_colorspaces_endpoint_t;

/**
 * @brief What the caller wants, and what the caller can consume.
 *
 * @details The two `*_CURVES` bits are not preferences, they are declarations of what the
 * caller's own OpenCL kernel can execute: a conversion that needs a curve stage the caller
 * cannot run has to fall back to lcms2 rather than silently drop the curve. On the CPU both
 * stages are always available, so a caller that only ever runs
 * dt_colorspaces_apply_conversion() may set both.
 */
typedef enum dt_colorspaces_conversion_flags_t
{
  DT_CONVERSION_NONE = 0,
  /** @brief Never take the matrix path, even when both profiles reduce to matrices. Backs the
   * `plugins/lighttable/export/force_lcms2` conf key. */
  DT_CONVERSION_FORCE_LCMS2 = 1 << 0,
  /** @brief The caller can apply the SOURCE profile's decoding curves before the matrix.
   * Without this, a non-linear source profile forces the lcms2 fallback. */
  DT_CONVERSION_SOURCE_CURVES = 1 << 1,
  /** @brief The caller can apply the TARGET profile's encoding curves after the matrix.
   * Without this, a non-linear target profile forces the lcms2 fallback. */
  DT_CONVERSION_TARGET_CURVES = 1 << 2,
  /** @brief Mark out-of-gamut pixels rather than merely proofing them. Requires a soft-proof
   * endpoint, and forces the lcms2 fallback because there is no matrix form of it. */
  DT_CONVERSION_GAMUTCHECK = 1 << 3,
} dt_colorspaces_conversion_flags_t;

/**
 * @brief Build a conversion from @p from to @p to. The expensive call; do it once.
 *
 * @details Resolves both endpoints (holding each profile's own lock across the derivation,
 * because the display profile's handle is replaced on a monitor change), then decides the
 * branch: if neither soft-proofing nor ::DT_CONVERSION_FORCE_LCMS2 is asked for, and both
 * profiles reduce to a colorant matrix, and every curve stage the profiles need is one the
 * caller declared it can run, the result is a composed matrix plus at most two curve sets.
 * Otherwise it is a `cmsHTRANSFORM`. Either way dt_colorspaces_apply_conversion() runs it.
 *
 * @param from source space. Must not be NULL.
 * @param to target space. Must not be NULL.
 * @param clip optional third space whose primaries bound the result: the conversion becomes
 *        source -> clip, clamp each channel to [0,1], clip -> target. Only the primaries are
 *        used, never the tone curves -- this is a gamut clamp, not a round trip. NULL for the
 *        ordinary direct conversion.
 * @param proof optional soft-proof space. Non-NULL builds a proofing transform with black
 *        point compensation, which always means the lcms2 branch. The profile is quantised
 *        first (several built-ins carry a parametric TRC that lcms2 would round-trip exactly,
 *        making the proof a no-op); if that quantisation fails, proofing is silently dropped
 *        and an ordinary transform is built, which is the pre-existing behaviour.
 * @param intent rendering intent for the lcms2 branch. The matrix branch has no intent.
 * @param flags see ::dt_colorspaces_conversion_flags_t.
 * @return A new conversion, or NULL if neither branch could be built (both endpoints
 *         unresolvable, or `cmsCreateTransform` refused the pair). Release it with
 *         dt_colorspaces_free_conversion().
 * @note Never call this from `process()`. It costs two 65536-entry curve extractions on the
 *       matrix branch and 2.2-38 ms on the lcms2 one.
 */
dt_colorspaces_conversion_t *dt_colorspaces_prepare_conversion(const dt_colorspaces_endpoint_t *const from,
                                                               const dt_colorspaces_endpoint_t *const to,
                                                               const dt_colorspaces_endpoint_t *const clip,
                                                               const dt_colorspaces_endpoint_t *const proof,
                                                               const dt_iop_color_intent_t intent,
                                                               const dt_colorspaces_conversion_flags_t flags);

/**
 * @brief Release a conversion and NULL the caller's pointer.
 *
 * @param conversion address OF the caller's pointer. A NULL address, or an address holding
 *        NULL, is a no-op. Closes whichever profile handles the conversion owns (never the
 *        borrowed ones) and deletes its transform.
 */
void dt_colorspaces_free_conversion(dt_colorspaces_conversion_t **conversion);

/**
 * @brief A per-pixel hook run between the source curves and the colour conversion proper.
 *
 * @details Exists for exactly one caller: `iop/colorin.c`'s "blue mapping", a legacy
 * per-pixel tweak that only old edits carry (nothing sets it for a new one). It is placed
 * where that module has always placed it -- after decoding, before the matrix on the matrix
 * branch; before `cmsDoTransform` on the lcms2 branch, which decodes internally. Do not add
 * callers: a function call per pixel defeats the vectorisation of the loop it sits in.
 *
 * @param in source pixel, 4 floats.
 * @param out destination pixel, 4 floats. May alias @p in.
 */
typedef void (*dt_colorspaces_conversion_hook_t)(const float *const in, float *const out);

/**
 * @brief Convert a 4-channel float image through a prepared conversion.
 *
 * @details THE apply entry point. Runs whichever branch dt_colorspaces_prepare_conversion()
 * settled on, over the whole buffer, parallelised. The 4th channel is not colour data and is
 * not preserved -- the matrix zeroes it and lcms2 leaves it undefined -- which is why callers
 * that carry a mask in it copy it back afterwards (`dt_iop_alpha_copy()`).
 *
 * @param conversion prepared conversion. NULL is a no-op, leaving @p out untouched.
 * @param in source, 4 floats per pixel, 16-byte aligned.
 * @param out destination, same layout. Must NOT alias @p in: the matrix branch uses
 *        non-temporal stores, and the clipping and curve stages read a pixel after writing
 *        earlier ones.
 * @param width pixels per row.
 * @param height rows.
 */
void dt_colorspaces_apply_conversion(const dt_colorspaces_conversion_t *const conversion,
                                     const float *const in, float *const out,
                                     const size_t width, const size_t height);

/**
 * @brief dt_colorspaces_apply_conversion() with a per-pixel hook. See
 * ::dt_colorspaces_conversion_hook_t for why this exists and why it should stay at one caller.
 *
 * @param hook applied to every pixel between decoding and conversion. NULL is exactly
 *        dt_colorspaces_apply_conversion().
 */
void dt_colorspaces_apply_conversion_hooked(const dt_colorspaces_conversion_t *const conversion,
                                            const float *const in, float *const out,
                                            const size_t width, const size_t height,
                                            const dt_colorspaces_conversion_hook_t hook);

/* --- What a device kernel needs -------------------------------------------
 *
 * A GPU kernel cannot call back into this module, so the numbers a conversion reduced to have
 * to be readable to upload them. These accessors are for that and nothing else: they are the
 * seam where the module hands a caller's own kernel its arguments, not a way to re-implement
 * apply on the host. A caller that finds itself running a matrix multiply from
 * dt_colorspaces_conversion_matrix() on the CPU has taken the wrong entry point.
 */

/**
 * @brief Whether the conversion reduced to matrices and curves, and can therefore be run by a
 * device kernel at all.
 *
 * @return TRUE for the matrix branch, FALSE for the lcms2 fallback (which is host-only, so
 *         the caller must clear `piece->process_cl_ready`). FALSE for a NULL conversion.
 */
gboolean dt_colorspaces_conversion_is_matrix(const dt_colorspaces_conversion_t *const conversion);

/**
 * @brief Whether the conversion has a gamut-clipping stage, i.e. whether a @p clip endpoint
 * was given AND survived preparation. Selects between a caller's clipping and non-clipping
 * kernels.
 */
gboolean dt_colorspaces_conversion_has_clipping(const dt_colorspaces_conversion_t *const conversion);

/**
 * @brief The composed source-to-target matrix, row-major.
 *
 * @details With a clipping stage this is the source-to-CLIP matrix, and
 * dt_colorspaces_conversion_clip_matrix() is the second leg -- which is the argument pair the
 * `colorin_clipping` kernel already takes.
 *
 * @param matrix filled with the matrix. Left untouched, and FALSE returned, on the lcms2
 *        branch or for a NULL conversion.
 * @return TRUE when @p matrix was written.
 */
gboolean dt_colorspaces_conversion_matrix(const dt_colorspaces_conversion_t *const conversion,
                                          dt_colormatrix_t matrix);

/**
 * @brief The SOURCE profile's own RGB -> XYZ (D50) matrix, before composition.
 *
 * @details Not for converting anything -- for describing the source space to something else.
 * `iop/colorin.c` hands it to the pipe as part of the input-profile record, which downstream
 * modules read to know what the buffer they receive is in.
 *
 * @param matrix filled with the matrix. Untouched, and FALSE returned, when the source
 *        profile does not reduce to a colorant matrix (a CLUT profile, say), which is the
 *        same answer as "there is no such matrix to report".
 * @return TRUE when @p matrix was written. Available on both branches: a conversion that
 *         runs through lcms2 can still have a perfectly good source matrix, and the reason
 *         it fell back may have been the target profile.
 */
gboolean dt_colorspaces_conversion_source_matrix(const dt_colorspaces_conversion_t *const conversion,
                                                 dt_colormatrix_t matrix);

/**
 * @brief The clip-to-target matrix, the second leg of a clipping conversion.
 *
 * @param matrix filled with the matrix. Untouched, and FALSE returned, when the conversion
 *        has no clipping stage.
 * @return TRUE when @p matrix was written.
 */
gboolean dt_colorspaces_conversion_clip_matrix(const dt_colorspaces_conversion_t *const conversion,
                                               dt_colormatrix_t matrix);

/**
 * @brief One channel of the source decoding curves, ::DT_CONVERSION_LUT_SAMPLES entries.
 *
 * @param channel 0, 1 or 2.
 * @return The curve, or NULL on the lcms2 branch or when ::DT_CONVERSION_SOURCE_CURVES was
 *         not asked for. Present-but-linear is NOT reported as NULL: a curve whose first
 *         entry is negative marks that channel linear, which is the convention both the CPU
 *         path and the kernels read, so a caller that declared it consumes this side always
 *         gets a buffer it can upload. Valid for the life of the conversion.
 */
const float *dt_colorspaces_conversion_source_curve(const dt_colorspaces_conversion_t *const conversion,
                                                    const int channel);

/**
 * @brief One channel of the target encoding curves. Same contract as
 * dt_colorspaces_conversion_source_curve().
 */
const float *dt_colorspaces_conversion_target_curve(const dt_colorspaces_conversion_t *const conversion,
                                                    const int channel);

/**
 * @brief The 3x3 power-law fits extrapolating the source curves past white, as one flat array
 * of 9 floats in channel-major order -- the layout the kernels upload verbatim.
 *
 * @return The coefficients, or NULL when the conversion has no source curve stage.
 * @see dt_ioppr_eval_trc(), which is what evaluates them.
 */
const float *dt_colorspaces_conversion_source_coeffs(const dt_colorspaces_conversion_t *const conversion);

/**
 * @brief The same fits for the target curves. Same contract.
 */
const float *dt_colorspaces_conversion_target_coeffs(const dt_colorspaces_conversion_t *const conversion);

#endif // DT_COLORPROFILES_CONVERSION_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
