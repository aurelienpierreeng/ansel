/*
    This file is part of darktable,
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019-2021, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019 Marcus Rückert.
    Copyright (C) 2019-2020 Pascal Obry.
    Copyright (C) 2020-2021 Dan Torop.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2021 Heiko Bauke.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef DT_COLORPROFILES_IOP_PROFILE_H
#define DT_COLORPROFILES_IOP_PROFILE_H

/**
 * @file colorprofiles/iop_profile.h
 * @brief The colour-profile struct and the maths over it: the derived matrix/LUT engine.
 *
 * @details Layer 2: nothing here mentions dt_develop_t, dt_dev_pixelpipe_t or
 * dt_iop_module_t, and the four forward declarations that used to sit here -- the only
 * reason this header reached two layers up -- went with the pipeline-facing half to
 * develop/iop_profile.h.
 *
 * pixel/rgb_norms.h and pixel/colorequal_shared.h consume this half, which is why it could
 * not simply move up to develop/ with the rest. (pixel/eaw.c was named here too, but as of
 * this writing it includes nothing from colorprofiles/ and names no type declared below.)
 *
 * Two things live here, and only these two:
 *
 * - the DERIVED form of a profile, ::dt_iop_order_iccprofile_info_t -- the two 3x3 matrices
 *   and the six tone-curve LUTs extracted from a `cmsHPROFILE` once, so that the pixel loop
 *   never calls lcms2 per pixel. Extracting them is expensive (two 65536-entry passes) and
 *   is a pure function of `(type, filename)`, so dt_colorspaces_add_profile() memoises the
 *   result process-wide;
 * - APPLY -- dt_colorspaces_apply_profile() and its siblings: the pixel loops that consume
 *   that struct. Each branches INTERNALLY between the vectorised matrix + LUT path and the
 *   lcms2 fallback, so callers neither choose nor see which one ran.
 *
 * No `cmsHPROFILE` and no `cmsHTRANSFORM` ever crosses this header, by design. There is no
 * `cmsDupProfile` in lcms2 -- the only true deep copy is serialise-and-reopen (~0.005 ms for
 * a built-in, but 1.02 ms for a real colord display profile), and copying a prepared
 * transform means rebuilding it (2.2-38 ms, not amortised). So the lifetime of an lcms2
 * handle is answered by a lock, not by a copy: dt_colorspaces_lock_profiles() /
 * dt_colorspaces_unlock_profiles() in colorprofiles/colorspaces.h. The functions below that
 * need one take that lock themselves, internally, and only for the one profile that can
 * change under them (DT_COLORSPACE_DISPLAY).
 *
 * @see colorprofiles/colorspaces.h -- the profile list itself (CRUDE on metadata, Lock and
 *      Apply on the data).
 * @see develop/iop_profile.h -- the pipeline-facing half, which knows about pipes and iops.
 */


#include "pixel/format.h"   // dt_iop_colorspace_type_t
#include "common/colorspaces_inline_conversions.h"
#include "colorprofiles/profile_types.h"
/* develop/imageop.h is deliberately NOT included. Including it made common/ depend on
 * develop/ (a layering inversion) and closed a 6-node include cycle with common/opencl.h.
 * It used to be needed for four iop/pixelpipe types used as POINTERS, which were then
 * tag-declared here instead; those declarations have since moved out with the
 * pipeline-facing half, and no declaration below names an iop or pipe type at all. The
 * `dt_iop_*` names that remain (dt_iop_colorspace_type_t, dt_iop_color_intent_t) are pixel
 * and profile vocabulary, from pixel/format.h and colorprofiles/profile_types.h. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_OPENCL
#include <CL/cl.h>           // for cl_mem
#endif


/**
 * @brief A profile reduced to the arithmetic the pixel loop can run: two matrices and six
 * tone-curve LUTs, plus the identity they were derived from.
 *
 * @details This is the DERIVED form of a profile, not the profile. It is produced once by
 * dt_colorspaces_add_profile() from the `cmsHPROFILE` that colorprofiles/colorspaces.c owns,
 * and from then on it is self-contained: nothing in it points back at lcms2, so the pixel
 * loops below need no lock and no lcms2 call per pixel.
 *
 * @warning **SOLE OWNERSHIP.** One of these is ~1.5 MB: six eagerly-allocated 65536-float
 * LUTs (`lut_in[3]` + `lut_out[3]`), see dt_ioppr_init_profile_info(). develop/blend.c
 * shallow-`memcpy`s the struct onto the stack (`dt_develop_blendif_init_masking_profile()`),
 * which ALIASES those six pointers into a second struct. Such a copy is read-only borrowed
 * state: it must never be handed to dt_ioppr_cleanup_profile_info(), and the original must
 * outlive it. Only the allocator of an instance may free it.
 *
 * @warning Two sentinel encodings drive every dispatch below, and neither is a flag:
 * `isnan(matrix_in[0][0])` means "not a matrix-shaper profile, use lcms2"
 * (dt_ioppr_mark_as_nonmatrix_profile()), and `lut_x[c][0] < 0.0f` means "channel c has no
 * tone curve, treat it as linear" (dt_ioppr_clear_lut_curves()). Code that memsets or
 * calloc's one of these structs instead of calling dt_ioppr_init_profile_info() gets the
 * opposite meaning for both.
 */
typedef struct dt_iop_order_iccprofile_info_t
{
  /** @brief Profile identity, half of the memo key. DT_COLORSPACE_NONE means "unset" and
   * makes every apply function below refuse the buffer rather than convert it. */
  dt_colorspaces_color_profile_type_t type;
  /** @brief ICC file name, the other half of the memo key; "" for every built-in.
   * Compared with `strcmp`, so it must be NUL-terminated (`g_strlcpy` writes it). */
  char filename[DT_IOP_COLOR_ICC_LEN];
  /** @brief Rendering intent, passed to `cmsCreateTransform` on the lcms2 fallback only.
   * The matrix path ignores it -- it is NOT part of the memo key, so the first caller to
   * ask for a given (type, filename) fixes the intent every later caller gets. */
  dt_iop_color_intent_t intent;
  /** @brief RGB -> XYZ (D50), row-major. `matrix_in[1][*]` is the luminance row.
   * NaN in `[0][0]` marks the whole profile as non-matrix; see the struct warning. */
  dt_colormatrix_t matrix_in; // don't align on more than 16 bits or OpenCL will fail
  /** @brief XYZ (D50) -> RGB, row-major; the inverse of ::matrix_in. */
  dt_colormatrix_t matrix_out;
  /** @brief Entry count of each of the six LUTs. Always 65536 in practice: both callers of
   * dt_ioppr_init_profile_info() pass 0, which selects that default.
   * @warning The OpenCL upload path hardcodes a 256 x 1536 image (= 6 x 65536 floats), so a
   * non-default `lutsize` would silently mismatch the device buffer. See
   * dt_ioppr_build_iccprofile_params_cl(). */
  int lutsize;
  /** @brief Per-channel encoded -> linear tone curve, ::lutsize entries each, sampled over
   * [0,1]. `lut_in[c][0] < 0` marks channel c linear (no curve). Above 1.0 the curve is
   * extrapolated from ::unbounded_coeffs_in instead; see dt_ioppr_eval_trc(). */
  float *lut_in[3];
  /** @brief Per-channel linear -> encoded tone curve, same convention as ::lut_in. */
  float *lut_out[3];
  /** @brief Power-law fit `{a, b, c}` of ::lut_in past 1.0, evaluated by eval_exp() as
   * `b * (a*x)^c`. Fitted on x = {0.7, 0.8, 0.9, 1.0} by dt_ioppr_init_unbounded_coeffs(),
   * which is what lets the pipeline carry values above white through a curve lcms2 would
   * have clipped. `[c][0] == -1.0f` marks channel c as having no fit. */
  float unbounded_coeffs_in[3][3] DT_ALIGNED_PIXEL;
  /** @brief Same fit for ::lut_out. */
  float unbounded_coeffs_out[3][3] DT_ALIGNED_PIXEL;
  /** @brief Non-zero when the profile has tone curves at all; tested as a boolean
   * everywhere, but it is really the COUNT (0..3) of non-linear channels returned by
   * dt_ioppr_init_unbounded_coeffs(), and it is taken from the INPUT curves only -- the
   * return value of the `lut_out` fit is discarded. A profile whose in-curves are linear
   * but whose out-curves are not would therefore read as linear. */
  int nonlinearlut;
  /** @brief Luminance of 18.42% grey through this profile, used by the tone-mapping modules
   * to place middle grey. Defaults to 0.1842 and is only recomputed (through ::matrix_in
   * and ::lut_in) when the profile has both valid matrices AND ::nonlinearlut -- so a linear
   * matrix profile legitimately keeps the 0.1842 literal.
   * @see dt_ioppr_get_profile_info_middle_grey() */
  float grey;
  dt_colormatrix_t matrix_in_transposed;  // same as matrix_in, but stored such as to permit vectorization
  dt_colormatrix_t matrix_out_transposed; // same as matrix_out, but stored such as to permit vectorization
} dt_iop_order_iccprofile_info_t;

/**
 * @brief Put a freshly allocated ::dt_iop_order_iccprofile_info_t into its empty state --
 * must be called before using profile_info, default lutsize = 0.
 *
 * @details Sets `type = DT_COLORSPACE_NONE`, marks the matrices non-matrix (NaN) and the
 * six curves linear (`[0] = -1`), and ALLOCATES the six LUT arrays with dt_alloc_align_float
 * -- so this is not a cheap "zero the struct" call: it is where the ~1.5 MB comes from, and
 * it must be paired with dt_ioppr_cleanup_profile_info() or every byte of it leaks.
 *
 * @param profile_info struct to initialise. Not NULL-checked, and not allocated here: the
 *        caller allocates (with dt_alloc_align, since teardown uses dt_free_align).
 * @param lutsize entries per tone curve, or 0 for the default of 65536. Every caller in the
 *        tree passes 0, which is also what the OpenCL upload path assumes.
 * @warning Do not call this twice on the same struct: it overwrites the six LUT pointers
 *          without freeing the previous arrays.
 */
void dt_ioppr_init_profile_info(dt_iop_order_iccprofile_info_t *profile_info, const int lutsize);
/**
 * @brief Release a profile info: its tone-curve LUTs, the struct itself, and the caller's
 * pointer -- must be called when done with profile_info.
 *
 * @details This owns the WHOLE teardown, not just the LUTs. A
 * ::dt_iop_order_iccprofile_info_t owns six aligned float arrays, so releasing one is "free
 * the curves, free the struct, drop the pointer" -- three steps every caller used to
 * open-code, and three chances to free the struct while leaving 1.5 MB of curves behind.
 *
 * @param profile_info address OF the caller's pointer. Taken by address, not by value, so
 *        the caller's variable is NULLed here and cannot be left dangling. A NULL address,
 *        or an address holding NULL, is a no-op.
 * @warning The struct is released with dt_free_align, so only a struct obtained from
 *          dt_alloc_align may be passed. Never pass a stack copy or a shallow `memcpy` of
 *          another instance -- develop/blend.c makes exactly such a copy, and its aliased
 *          LUT pointers belong to the original.
 */
void dt_ioppr_cleanup_profile_info(dt_iop_order_iccprofile_info_t **profile_info);
/**
 * @brief Find-or-build the derived matrix/LUT data for a profile identity, memoised.
 *
 * @details Deriving matrices and curves from a profile costs two 65536-entry extractions
 * and is a pure function of `(type, filename, intent)`, so the result is kept in a
 * process-wide memo. The lookup and the append are ONE critical section under the module's own mutex:
 * this is reached from the pipeline worker (iop/lut3d.c and iop/tonecurve.c call it from
 * `process()`/`process_cl()`, once per tile) and from the GUI thread (iop/colorin.c), and
 * two threads missing the same key concurrently would otherwise each build an entry --
 * 1.5 MB of tone-curve LUTs apiece -- and append both. Building the DT_COLORSPACE_DISPLAY
 * entry additionally takes dt_colorspaces_lock_profiles() internally, for the whole span
 * from resolving the handle to the last extraction, because that handle is the one datum in
 * the profile list that is replaced at runtime.
 *
 * @param profile_type profile identity; half of the memo key.
 * @param profile_filename ICC file name, "" for built-ins; the other half of the key.
 * @param intent rendering intent; the third part of the key. It is stored in the entry and
 *        read back by the lcms2 transform path, so a memo that ignored it handed the second
 *        caller a transform built for the first caller's intent -- silently, and depending
 *        on which module happened to commit first. Two intents for one profile now cost two
 *        entries, which is 1.5 MB each; that is the price of the answer being right.
 * @return A pointer the MODULE owns, valid until dt_colorspaces_flush_profile_memo() (or,
 *         for DT_COLORSPACE_DISPLAY, dt_colorspaces_invalidate_display_profile_memo()).
 *         Never free it, and never write through it -- the entry is shared by every caller
 *         asking for the same identity, including concurrent pipes. NULL only if the
 *         allocation failed or the profile could not be derived.
 * @warning NOT for the image-derived types, DT_COLORSPACE_EMBEDDED_ICC (9) through
 *          DT_COLORSPACE_ALTERNATE_MATRIX (14). Those are not registered in the profile list
 *          and cannot be resolved by identity at all: their matrices come from the image's
 *          own camera data via iop/colorin.c. Keyed on `(type, "")` they would all collide
 *          on one entry and stomp each other across images. They live on the pipe that built
 *          them instead (`dt_dev_pixelpipe_t.owned_input_profile_info`); see
 *          `dt_ioppr_set_pipe_input_profile_info()` in develop/iop_profile.c.
 */
dt_iop_order_iccprofile_info_t *
dt_colorspaces_add_profile(const dt_colorspaces_color_profile_type_t profile_type,
                           const char *profile_filename,
                           const int intent);

/**
 * @brief Drop every memoised entry. Called by dt_colorprofiles_cleanup().
 *
 * @details Frees each entry with dt_ioppr_cleanup_profile_info(), so it invalidates every
 * pointer ever returned by dt_colorspaces_add_profile(). Only legitimate at shutdown, once
 * no pipe can still be holding one.
 */
void dt_colorspaces_flush_profile_memo(void);

/**
 * @brief Drop the memoised DT_COLORSPACE_DISPLAY entry, whose source profile this module
 * replaces on a monitor change.
 *
 * @details Nothing invalidated it before, so a session kept the previous monitor's matrices
 * and tone curves for as long as the memo lived. That was already wrong when the memo hung
 * off one image; now that it is process-wide it would persist for the whole run. Dropped
 * rather than rebuilt: the next caller that wants it will build it, and rebuilding here
 * would mean deriving a profile under the memo lock from inside the profile-changed handler.
 * @warning Called from colorprofiles/colorspaces.c when the display profile changes. Any
 *          pointer a caller cached from a previous dt_colorspaces_add_profile(DISPLAY, ...)
 *          is freed by this -- resolve the display profile per use, do not hold it.
 */
void dt_colorspaces_invalidate_display_profile_memo(void);

/* --- APPLY: the pixel loop ------------------------------------------------
 *
 * Convert a buffer between colour spaces. Branches internally on what the profile is:
 * a matrix-shaper with tone curves takes our own vectorised matrix + LUT path, anything
 * else falls back to lcms2. Callers neither choose nor see which.
 *
 * `op_name`/`instance_name` label the -d perf trace only, and are plain strings rather
 * than the dt_iop_module_t they come from: this module sits below develop/ and cannot
 * name an iop. */

/**
 * @brief Convert a 4-channel float buffer between RGB and Lab through one profile.
 *
 * @details THE entry point for the RGB <-> Lab leg of the pipeline, called by
 * develop/pixelpipe_cpu.c around every module that wants a different working space than the
 * one the pipe carries. It dispatches on the profile itself, not on a caller's request:
 * if the profile reduced to real matrices (`!isnan(matrix_in[0][0])`) it runs
 * dt_ioppr_transform_matrix() -- our own vectorised matrix + tone-curve LUT loop, with
 * power-law extrapolation above white; otherwise (a CLUT profile, a v4 parametric curve
 * lcms2 will not reduce) it falls back to dt_ioppr_transform_lcms2(), which builds a
 * `cmsHTRANSFORM` and runs `cmsDoTransform`. Callers do not choose, and do not see which
 * ran except in the `-d perf` trace.
 *
 * The lcms2 branch resolves and pins the profile handle itself -- it takes
 * dt_colorspaces_lock_profiles() around resolve-through-`cmsCreateTransform` when the
 * profile is DT_COLORSPACE_DISPLAY -- so callers neither take that lock nor need to know
 * the branch exists.
 *
 * @param op_name module `op` string, for the `-d perf` trace ONLY.
 * @param instance_name module `multi_name` string, same.
 * @param image_in source, 4 floats per pixel, 16-byte aligned. May equal @p image_out
 *        (in-place is supported and used).
 * @param image_out destination, same layout. Alpha (the 4th float) is preserved by the
 *        matrix paths.
 * @param width pixels per row.
 * @param height rows.
 * @param cst_from source colorspace, a ::dt_iop_colorspace_type_t passed as plain int.
 * @param cst_to destination colorspace, same.
 * @param converted_cst out: set to @p cst_to when the buffer was converted, and left at
 *        @p cst_from when it was NOT -- which is how the caller learns the buffer is still
 *        in its original space. This is the only failure report; there is no return value.
 * @warning Refuses (leaving `*converted_cst == cst_from`) on a NULL @p profile_info or one
 *          whose `type` is DT_COLORSPACE_NONE, and prints to stderr on an unsupported pair.
 * @warning RGB -> RGB is a NO-OP here: it reports success (`*converted_cst = cst_to`) and
 *          copies nothing, not even between two different RGB profiles. Converting between
 *          two RGB spaces is dt_ioppr_transform_image_colorspace_rgb()'s job.
 * @see dt_ioppr_transform_image_colorspace_rgb() for the RGB -> RGB leg.
 */
void dt_colorspaces_apply_profile(const char *const op_name, const char *const instance_name,
                                  const float *const image_in, float *const image_out,
                                  const int width, const int height,
                                  const int cst_from, const int cst_to, int *converted_cst,
                                  const dt_iop_order_iccprofile_info_t *const profile_info);

#ifdef HAVE_OPENCL
/**
 * @brief OpenCL counterpart of dt_colorspaces_apply_profile(), same contract.
 *
 * @details Same internal branch, different fallback cost: a matrix profile is uploaded (the
 * scalar fields as a constant buffer, the six curves as a 256 x 1536 float image) and
 * converted by the `colorspaces_transform_*` kernels; a non-matrix profile has no kernel, so
 * the buffer is read back to host memory, run through dt_colorspaces_apply_profile() on the
 * CPU in place, and written back to the device.
 *
 * @param devid OpenCL device.
 * @param dev_img_in source device image.
 * @param dev_img_out destination device image.
 * @param converted_cst out: same "did it actually convert" report as the CPU version, and
 *        still meaningful on a FALSE return.
 * @return TRUE on success, FALSE on any OpenCL error or refusal -- an `int`, not a `cl_int`.
 *         Callers must fall back to the CPU path on FALSE.
 * @warning Asserts that both images are non-NULL and DISTINCT: unlike the CPU version, this
 *          one cannot run in place.
 */
int dt_colorspaces_apply_profile_cl(const char *const op_name, const char *const instance_name,
                                    const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                    const int width, const int height,
                                    const int cst_from, const int cst_to, int *converted_cst,
                                    const dt_iop_order_iccprofile_info_t *const profile_info);
#endif

/**
 * @brief Convert a 4-channel float buffer between two RGB profiles.
 *
 * @details The other half of APPLY: dt_colorspaces_apply_profile() handles RGB <-> Lab
 * through ONE profile and refuses RGB -> RGB; this one takes two profiles and does exactly
 * that leg. Same internal dispatch, on all four matrices this time: when both profiles
 * reduced to matrices, the two 3x3s are pre-multiplied ONCE into a single matrix so the loop
 * costs one matrix product per pixel instead of two; otherwise both profiles go to lcms2 as
 * a single RGB->RGB transform.
 *
 * @param image_in source, 4 floats per pixel. May equal @p image_out.
 * @param image_out destination.
 * @param profile_info_from source profile. Must NOT be NULL.
 * @param profile_info_to destination profile. Must NOT be NULL.
 * @param message label for the `-d perf` trace only; NULL is tolerated.
 * @warning No return value and no `converted_cst`: a refusal is SILENT. It returns without
 *          touching @p image_out when either profile's `type` is DT_COLORSPACE_NONE -- so
 *          the destination keeps whatever it held, which for a fresh buffer is garbage.
 * @warning Both pointers are dereferenced unguarded. This differs from
 *          dt_colorspaces_apply_profile(), which NULL-checks its profile.
 * @note When both profiles have the same `type` AND the same `filename` it degenerates to a
 *       `memcpy` (skipped when the buffers are the same), not to a no-op.
 */
void dt_ioppr_transform_image_colorspace_rgb(const float *const image_in, float *const image_out, const int width,
                                             const int height,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                             const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                             const char *message);

#ifdef HAVE_OPENCL
/**
 * @brief The three colorspace kernels, compiled once and held by common/opencl.c.
 *
 * @details Built from `colorspaces.cl` (program 23 in `data/kernels/programs.conf`) by
 * dt_colorspaces_init_cl_global(), reached from the pixel loops as
 * a file-static in colorprofiles/iop_profile.c. Members are kernel handles, not pointers.
 */
typedef struct dt_colorspaces_cl_global_t
{
  int kernel_colorspaces_transform_lab_to_rgb_matrix;
  int kernel_colorspaces_transform_rgb_matrix_to_lab;
  int kernel_colorspaces_transform_rgb_matrix_to_rgb;
} dt_colorspaces_cl_global_t;

/**
 * @brief The device-side view of a ::dt_iop_order_iccprofile_info_t: the scalar fields only.
 *
 * @details Flattened for upload as a `constant` buffer. The two matrices are stored as 9
 * contiguous floats, NOT as the host struct's 4x4 padded `dt_colormatrix_t`; the curves are
 * not here at all -- they travel separately as a float image built by dt_ioppr_get_trc_cl().
 * The host identity (`type`, `filename`, `intent`) is dropped: the kernels only do
 * arithmetic.
 *
 * @warning Must be in synch with `dt_colorspaces_iccprofile_info_cl_t` in
 *          `data/kernels/colorspaces.cl` -- there is no compile-time check on either side.
 */
// must be in synch with colorspaces.cl dt_colorspaces_iccprofile_info_cl_t
typedef struct dt_colorspaces_iccprofile_info_cl_t
{
  cl_float matrix_in[9];
  cl_float matrix_out[9];
  cl_int lutsize;
  cl_float unbounded_coeffs_in[3][3];
  cl_float unbounded_coeffs_out[3][3];
  cl_int nonlinearlut;
  cl_float grey;
} dt_colorspaces_iccprofile_info_cl_t;

/**
 * @brief Compile the colorspace kernels. Called once by common/opencl.c at device init.
 * @return A `malloc`ed struct the caller owns; release with dt_colorspaces_free_cl_global().
 */
void dt_colorspaces_init_cl_global(void);
/** @brief Release the kernels and the struct from dt_colorspaces_init_cl_global(). NULL-safe. */
void dt_colorspaces_free_cl_global(void);

/**
 * @brief sets profile_info_cl using profile_info, to be used as a parameter when calling
 * opencl.
 *
 * @param profile_info source, host side. Dereferenced unguarded.
 * @param profile_info_cl destination, caller-provided storage (a stack struct is the usual
 *        case). Every field is written, so it needs no prior initialisation.
 * @note Copies the scalars only. The tone curves are dt_ioppr_get_trc_cl()'s job.
 */
void dt_ioppr_get_profile_info_cl(const dt_iop_order_iccprofile_info_t *const profile_info, dt_colorspaces_iccprofile_info_cl_t *profile_info_cl);
/**
 * @brief returns the profile_info trc, to be used as a parameter when calling opencl.
 *
 * @details Flattens the six tone curves into one contiguous array, `lut_in[0..2]` then
 * `lut_out[0..2]`, `profile_info->lutsize` floats each.
 * @return A newly `malloc`ed array of `6 * lutsize` floats that the CALLER owns and frees
 *         with dt_free (the CL param helpers below do it for you). NULL if the allocation
 *         failed -- which callers in this file do not check.
 */
cl_float *dt_ioppr_get_trc_cl(const dt_iop_order_iccprofile_info_t *const profile_info);

/**
 * @brief build the required parameters for a kernel that uses a profile info.
 *
 * @details Packs the profile for the device in one call: the scalars into a `constant`
 * buffer and the six curves into a 256 x 1536 float image. Everything it allocates -- host
 * and device -- comes back through the four out-parameters, and is released as a set by
 * dt_ioppr_free_iccprofile_params_cl().
 *
 * @param profile_info profile to upload, or NULL. NULL is a supported "this kernel takes a
 *        profile argument but there is no profile" case: it uploads a 6-float dummy LUT and
 *        leaves `*_dev_profile_info` NULL while still returning CL_SUCCESS. Callers that
 *        pass a profile conditionally (develop/blend.c does) must therefore not assume a
 *        successful return means a valid device buffer.
 * @param devid OpenCL device.
 * @param _profile_info_cl out: host-side packed struct, always allocated.
 * @param _profile_lut_cl out: host-side flattened curves.
 * @param _dev_profile_info out: device constant buffer, or NULL as described above.
 * @param _dev_profile_lut out: device image holding the curves.
 * @return CL_SUCCESS, or a `cl_int` error. On error the out-parameters still carry whatever
 *         was allocated before the failure, so the free function must be called either way.
 * @warning The device LUT image is hardcoded 256 x (256*6), which equals `6 * lutsize`
 *          ONLY for the default `lutsize` of 65536. A profile built with a different
 *          `lutsize` would upload the wrong amount of data with no diagnostic.
 */
cl_int dt_ioppr_build_iccprofile_params_cl(const dt_iop_order_iccprofile_info_t *const profile_info,
                                           const int devid, dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                           cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                           cl_mem *_dev_profile_lut);
/**
 * @brief free parameters build with the previous function.
 *
 * @details Releases both host allocations and both device buffers and NULLs all four
 * caller variables, so it is idempotent and safe on a partially-built set (which is what an
 * error return from dt_ioppr_build_iccprofile_params_cl() leaves behind).
 * @warning Dereferences all four addresses unguarded -- pass addresses of real variables,
 *          never NULL.
 */
void dt_ioppr_free_iccprofile_params_cl(dt_colorspaces_iccprofile_info_cl_t **_profile_info_cl,
                                        cl_float **_profile_lut_cl, cl_mem *_dev_profile_info,
                                        cl_mem *_dev_profile_lut);
/**
 * @brief OpenCL counterpart of dt_ioppr_transform_image_colorspace_rgb().
 *
 * @details Same dispatch: both profiles reduced to matrices means one pre-multiplied matrix
 * and the `colorspaces_transform_rgb_matrix_to_rgb` kernel; otherwise the buffer is read
 * back, converted on the CPU by dt_ioppr_transform_image_colorspace_rgb(), and written back.
 * Unlike dt_colorspaces_apply_profile_cl(), in-place IS supported: when
 * `dev_img_in == dev_img_out` it allocates a device scratch image and copies through it.
 *
 * @return TRUE on success. FALSE on any OpenCL error AND on the "either profile is
 *         DT_COLORSPACE_NONE" refusal -- so a FALSE does not distinguish a device failure
 *         from a caller passing an unset profile. Identical source and destination profiles
 *         return TRUE after a device-side image copy.
 * @warning Both profile pointers are dereferenced unguarded.
 */
int dt_ioppr_transform_image_colorspace_rgb_cl(const int devid, cl_mem dev_img_in, cl_mem dev_img_out,
                                               const int width, const int height,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                               const dt_iop_order_iccprofile_info_t *const profile_info_to,
                                               const char *message);
#endif

/* --- The per-pixel primitives -------------------------------------------------------
 *
 * the following must have the matrix_in and matrix_out generated
 *
 * Everything below is `static inline` and published as inline code on purpose: these run
 * once per pixel per module, so a call would dominate the work. That is the one legitimate
 * reason a header carries an implementation, and it is why this header includes
 * common/colorspaces_inline_conversions.h -- a deliberate performance trade, not a
 * convenience.
 *
 * They all take the profile's fields as SEPARATE arguments rather than the struct, so the
 * OpenMP `uniform`/`aligned` clauses can promise the vectoriser that the matrix and the LUTs
 * are loop-invariant and aligned. Passing `profile_info` instead would forfeit that.
 *
 * NONE of them checks the "is this a matrix profile" sentinel: they assume the matrices are
 * real, which is what the leading comment means. Call them only on a profile that came back
 * from dt_colorspaces_add_profile() with `!isnan(matrix_in[0][0])`.
 */

/**
 * @brief Sample a tone curve, with linear interpolation between entries.
 *
 * @param lut curve, @p lutsize entries spanning [0,1].
 * @param v input value.
 * @param lutsize entry count.
 * @return the interpolated curve value.
 * @note Despite the name it does NOT extrapolate: @p v is clamped into the table, so
 *       everything above 1.0 returns the last entry. Extrapolation past white is eval_exp()'s
 *       job, and dt_ioppr_eval_trc() is the one that picks between them.
 */
__OMP_DECLARE_SIMD__(aligned(lut:64) uniform(lut))
static inline __attribute__((always_inline)) float extrapolate_lut(const float *const lut, const float v, const int lutsize)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v * (lutsize - 1), 0, lutsize - 1);
  const int t = (ft < lutsize - 2) ? ft : lutsize - 2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t + 1];
  return l1 * (1.0f - f) + l2 * f;
}


/**
 * @brief Evaluate the power-law continuation of a tone curve past white: `b * (a*x)^c`.
 *
 * @param coeff the `{a, b, c}` fit from dt_ioppr_init_unbounded_coeffs(), i.e. one row of
 *        `unbounded_coeffs_in`/`_out`.
 * @param x input value, expected >= 1.0.
 * @return the extrapolated curve value.
 * @note This is what lets the pipeline carry values above white through an encoding curve
 *       that only exists on [0,1]; lcms2 would have clipped them.
 */
__OMP_DECLARE_SIMD__(uniform(coeff))
static inline __attribute__((always_inline)) float eval_exp(const float coeff[3], const float x)
{
  return coeff[1] * powf(x * coeff[0], coeff[2]);
}

/**
 * @brief Evaluate a tone curve over the whole real line: table below white, power law above.
 *
 * @param x input value.
 * @param lut the curve, one of `lut_in[c]`/`lut_out[c]`.
 * @param coeff the matching `unbounded_coeffs_*[c]` fit.
 * @param lutsize entry count.
 * @return the curve value.
 * @warning Does not test the `lut[0] < 0` "linear channel" sentinel -- callers must, or a
 *          linear channel gets read as a curve whose first entry is -1. _apply_trc() and
 *          the loops in the .c do that test.
 */
__OMP_DECLARE_SIMD__(aligned(lut:64) uniform(lut, coeff))
static inline __attribute__((always_inline)) float dt_ioppr_eval_trc(const float x, const float *const lut, const float coeff[3], const int lutsize)
{
  return (x < 1.0f) ? extrapolate_lut(lut, x, lutsize) : eval_exp(coeff, x);
}
/**
 * @brief Apply one channel's tone curve to each of the three colour channels, or pass the
 * channel through untouched when it has none.
 *
 * @param rgb_in source pixel.
 * @param rgb_out destination pixel. Only channels 0..2 are written -- the 4th lane is left
 *        alone, which is what preserves alpha through an in-place conversion.
 * @param lut the profile's `lut_in` or `lut_out`. A NULL entry, or one whose `[0]` is
 *        negative, means "channel is linear" and is copied straight through.
 * @param unbounded_coeffs the matching `unbounded_coeffs_in`/`_out`.
 * @param lutsize entry count.
 */
#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb_in, rgb_out, unbounded_coeffs:16) \
  aligned(lut:64) \
  uniform(lut, unbounded_coeffs)
#endif
static inline __attribute__((always_inline)) void
_apply_trc(const dt_aligned_pixel_t rgb_in, dt_aligned_pixel_t rgb_out, float *const lut[3],
           const float unbounded_coeffs[3][3], const int lutsize)
{
  for(int c = 0; c < 3; c++)
  {
    rgb_out[c] = (!IS_NULL_PTR(lut[c]) && lut[c][0] >= 0.0f) ? dt_ioppr_eval_trc(rgb_in[c], lut[c], unbounded_coeffs[c], lutsize)
                                                              : rgb_in[c];
  }
}

/**
 * @brief Luminance (XYZ Y) of one RGB pixel in a given profile.
 *
 * @details Linearises through the input curves when the profile is non-linear, then applies
 * row 1 of the RGB->XYZ matrix, which is the Y row. Used for every "how bright is this
 * pixel" decision in the pipeline, and by dt_colorspaces_add_profile() itself to fill
 * `profile_info->grey`.
 *
 * @param rgb source pixel, in the profile's encoding.
 * @param matrix_in the profile's `matrix_in`.
 * @param lut_in the profile's `lut_in`.
 * @param unbounded_coeffs_in the profile's `unbounded_coeffs_in`.
 * @param lutsize entry count.
 * @param nonlinearlut the profile's `nonlinearlut`; 0 skips the curves entirely.
 * @return luminance, in the profile's linear units.
 * @warning This one takes the NON-transposed `matrix_in` -- it indexes `matrix_in[1][0..2]`
 *          by hand. Every other primitive here takes `matrix_*_transposed`. Passing the
 *          transposed matrix compiles fine and returns a plausible wrong number.
 */
#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb:16) \
  aligned(matrix_in:64) \
  aligned(unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(matrix_in, lut_in, unbounded_coeffs_in)
#endif
static inline float dt_ioppr_get_rgb_matrix_luminance(const dt_aligned_pixel_t rgb,
                                                      const dt_colormatrix_t matrix_in, float *const lut_in[3],
                                                      const float unbounded_coeffs_in[3][3],
                                                      const int lutsize, const int nonlinearlut)
{
  float luminance = 0.f;

  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    _apply_trc(rgb, linear_rgb, lut_in, unbounded_coeffs_in, lutsize);
    luminance = matrix_in[1][0] * linear_rgb[0] + matrix_in[1][1] * linear_rgb[1] + matrix_in[1][2] * linear_rgb[2];
  }
  else
    luminance = matrix_in[1][0] * rgb[0] + matrix_in[1][1] * rgb[1] + matrix_in[1][2] * rgb[2];

  return luminance;
}


/**
 * @brief RGB -> XYZ (D50) for one pixel: linearise through the input curves, then matrix.
 *
 * @param rgb source pixel, in the profile's encoding.
 * @param xyz destination.
 * @param matrix_in_transposed the profile's `matrix_in_transposed` -- the vectorisable
 *        layout, NOT `matrix_in`.
 * @param lut_in the profile's `lut_in`.
 * @param unbounded_coeffs_in the profile's `unbounded_coeffs_in`.
 * @param lutsize entry count.
 * @param nonlinearlut the profile's `nonlinearlut`; 0 skips the curves.
 * @note The 4th lane of @p xyz is written by the SIMD matrix product with matrix output, not
 *       with the caller's alpha. Anything holding a mask or an alpha there must save and
 *       restore it around this call.
 */
#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb, xyz:16) \
  aligned(matrix_in_transposed:64) \
  aligned(unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(matrix_in_transposed, lut_in, unbounded_coeffs_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_xyz(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t xyz,
                                              const dt_colormatrix_t matrix_in_transposed, float *const lut_in[3],
                                              const float unbounded_coeffs_in[3][3],
                                              const int lutsize, const int nonlinearlut)
{
  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    _apply_trc(rgb, linear_rgb, lut_in, unbounded_coeffs_in, lutsize);
    dt_apply_transposed_color_matrix(linear_rgb, matrix_in_transposed, xyz);
  }
  else
    dt_apply_transposed_color_matrix(rgb, matrix_in_transposed, xyz);
}

/**
 * @brief Lab -> RGB for one pixel: Lab to XYZ, matrix, then the output curves.
 *
 * @param lab source pixel.
 * @param rgb destination, in the profile's encoding.
 * @param matrix_out_transposed the profile's `matrix_out_transposed`.
 * @param lut_out the profile's `lut_out`.
 * @param unbounded_coeffs_out the profile's `unbounded_coeffs_out`.
 * @param lutsize entry count.
 * @param nonlinearlut the profile's `nonlinearlut`; 0 skips the curves.
 * @note Same 4th-lane caveat as dt_ioppr_rgb_matrix_to_xyz() on the linear branch: the
 *       matrix product writes all four lanes. On the non-linear branch _apply_trc() writes
 *       only three, so `rgb[3]` then keeps whatever the matrix product left there.
 */
#ifdef _OPENMP
#pragma omp declare simd \
  aligned(lab, rgb:16) \
  aligned(matrix_out_transposed:64) \
  aligned(unbounded_coeffs_out:16) \
  aligned(lut_out:64) \
  uniform(matrix_out_transposed, lut_out, unbounded_coeffs_out)
#endif
static inline void dt_ioppr_lab_to_rgb_matrix(const dt_aligned_pixel_t lab, dt_aligned_pixel_t rgb,
                                              const dt_colormatrix_t matrix_out_transposed, float *const lut_out[3],
                                              const float unbounded_coeffs_out[3][3],
                                              const int lutsize, const int nonlinearlut)
{
  dt_aligned_pixel_t xyz;
  dt_Lab_to_XYZ(lab, xyz);

  if(nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb;
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, linear_rgb);
    _apply_trc(linear_rgb, rgb, lut_out, unbounded_coeffs_out, lutsize);
  }
  else
  {
    dt_apply_transposed_color_matrix(xyz, matrix_out_transposed, rgb);
  }
}

/**
 * @brief RGB -> Lab for one pixel: dt_ioppr_rgb_matrix_to_xyz() then XYZ -> Lab.
 *
 * @param rgb source pixel, in the profile's encoding.
 * @param lab destination.
 * @param matrix_in_transposed the profile's `matrix_in_transposed`.
 * @param lut_in the profile's `lut_in`.
 * @param unbounded_coeffs_in the profile's `unbounded_coeffs_in`.
 * @param lutsize entry count.
 * @param nonlinearlut the profile's `nonlinearlut`; 0 skips the curves.
 */
#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb, lab:16) \
  aligned(matrix_in_transposed:64) \
  aligned(unbounded_coeffs_in:16) \
  aligned(lut_in:64) \
  uniform(matrix_in_transposed, lut_in, unbounded_coeffs_in)
#endif
static inline void dt_ioppr_rgb_matrix_to_lab(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t lab,
                                              const dt_colormatrix_t matrix_in_transposed, float *const lut_in[3],
                                              const float unbounded_coeffs_in[3][3],
                                              const int lutsize, const int nonlinearlut)
{
  dt_aligned_pixel_t xyz = { 0.f };
  dt_ioppr_rgb_matrix_to_xyz(rgb, xyz, matrix_in_transposed, lut_in, unbounded_coeffs_in, lutsize, nonlinearlut);
  dt_XYZ_to_Lab(xyz, lab);
}

/**
 * @brief The profile's middle grey, i.e. `profile_info->grey`.
 * @param profile_info profile. Dereferenced unguarded.
 * @return 0.1842 for a linear profile (the literal default), or the luminance of 18.42%
 *         grey through the profile's own curves and matrix otherwise. @see the `grey` member.
 */
static inline float dt_ioppr_get_profile_info_middle_grey(const dt_iop_order_iccprofile_info_t *const profile_info)
{
  return profile_info->grey;
}

/**
 * @brief Map a curve node from the image colorspace to perceptual (L* / 100) coordinates.
 *
 * @details Used by the tone-curve modules so that a node the user placed at "50% grey" means
 * the same perceptual position whatever working profile the pipe carries: the value is sent
 * as a neutral RGB triplet through the profile to Lab, and L* is rescaled to [0,1].
 *
 * @param x value in the image colorspace.
 * @param profile_info profile to interpret @p x in. Dereferenced unguarded, and its matrices
 *        must be real -- see the section note above.
 * @return the same value expressed as `L* / 100`.
 * @see dt_ioppr_uncompensate_middle_grey() for the exact inverse.
 */
__OMP_DECLARE_SIMD__(uniform(profile_info))
static inline float dt_ioppr_compensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from the image colorspace to lab
  dt_aligned_pixel_t lab = { 0.0f };
  const dt_aligned_pixel_t rgb = { x, x, x };
  dt_ioppr_rgb_matrix_to_lab(rgb, lab, profile_info->matrix_in_transposed, profile_info->lut_in,
                             profile_info->unbounded_coeffs_in, profile_info->lutsize, profile_info->nonlinearlut);
  return lab[0] * .01f;
}

/**
 * @brief Inverse of dt_ioppr_compensate_middle_grey(): perceptual (L* / 100) back to the
 * image colorspace.
 *
 * @param x value as `L* / 100`.
 * @param profile_info profile to express the result in. Dereferenced unguarded.
 * @return the value in the image colorspace (the R channel of the reconstructed neutral).
 */
__OMP_DECLARE_SIMD__(uniform(profile_info))
static inline float dt_ioppr_uncompensate_middle_grey(const float x, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  // we transform the curve nodes from lab to the image colorspace
  const dt_aligned_pixel_t lab = { x * 100.f, 0.0f, 0.0f };
  dt_aligned_pixel_t rgb = { 0.0f };

  dt_ioppr_lab_to_rgb_matrix(lab, rgb, profile_info->matrix_out_transposed, profile_info->lut_out,
                             profile_info->unbounded_coeffs_out, profile_info->lutsize, profile_info->nonlinearlut);
  return rgb[0];
}


/* --- The transform core, unwrapped ---------------------------------------------------
 *
 * Shared with the pipeline-facing half in develop/iop_profile.c. These were static; they are
 * the transform core both sides drive, not either side's private business. The two transform workers
 * take the module's name strings, not the module: naming it in a log line was the only use,
 * and a pointer would have kept this header reaching into develop/.
 *
 * These are the two branches dt_colorspaces_apply_profile() picks between, plus the three
 * builders that decide which branch a profile lands on, exposed for code that must build or
 * fix up a ::dt_iop_order_iccprofile_info_t by hand -- an image-derived input profile, say,
 * whose matrices come from iop/colorin.c rather than from a file. Prefer
 * dt_colorspaces_apply_profile(): calling a worker directly means choosing the branch
 * yourself, and choosing it wrong is silently wrong output rather than an error.
 *
 * As of this writing the tree has no caller for any of these five outside
 * colorprofiles/iop_profile.c itself.
 */

/**
 * @brief Mark a profile as NOT reducible to matrices, by writing NaN into `[0][0]` of all
 * four matrices.
 * @details This is the sentinel every dispatch below reads: it is what routes the profile to
 * the lcms2 branch instead of the vectorised one. @see the ::dt_iop_order_iccprofile_info_t
 * warning about sentinels.
 * @param profile_info profile to mark. Dereferenced unguarded.
 */
void dt_ioppr_mark_as_nonmatrix_profile(dt_iop_order_iccprofile_info_t *profile_info);
/**
 * @brief Mark all six tone curves linear, by writing -1.0 into entry 0 of each.
 * @details The curve equivalent of the NaN matrix sentinel. Requires the six LUT arrays to
 * be allocated already, i.e. dt_ioppr_init_profile_info() must have run.
 * @param profile_info profile to clear. Dereferenced unguarded.
 */
void dt_ioppr_clear_lut_curves(dt_iop_order_iccprofile_info_t *profile_info);
/**
 * @brief Fit the power-law continuation of three tone curves past white.
 *
 * @details For each channel whose curve is not marked linear, samples it at x = 0.7, 0.8,
 * 0.9, 1.0 and fits `b * (a*x)^c`, which eval_exp() then evaluates above 1.0. This is what
 * makes the pipeline able to carry values above white through an encoding curve defined only
 * on [0,1].
 *
 * @param lutr,lutg,lutb the three curves, `lutsize` entries each. A curve whose `[0]` is
 *        negative is skipped and its coefficients marked -1.
 * @param unbounded_coeffsr,unbounded_coeffsg,unbounded_coeffsb out: three floats each.
 * @param lutsize entry count.
 * @return the COUNT (0..3) of channels that actually got a fit -- which is what callers
 *         store into `profile_info->nonlinearlut` and then read as a boolean.
 */
int dt_ioppr_init_unbounded_coeffs(float *lutr, float *lutg, float *lutb,
                                    float *unbounded_coeffsr, float *unbounded_coeffsg,
                                    float *unbounded_coeffsb, const int lutsize);
/**
 * @brief The lcms2 branch of dt_colorspaces_apply_profile(): RGB <-> Lab via `cmsDoTransform`.
 *
 * @details Resolves the profile by identity out of the shared list, builds a
 * `cmsHTRANSFORM`, runs it over the buffer and deletes it -- so the transform is rebuilt on
 * EVERY call, which is why this is the fallback and not the default. It takes
 * dt_colorspaces_lock_profiles() itself, across resolve-through-`cmsCreateTransform`, when
 * the profile is DT_COLORSPACE_DISPLAY, because that handle can be closed and replaced by a
 * monitor change mid-flight.
 *
 * @param op module `op` string, for the `-d dev` trace only.
 * @param multi_name module `multi_name` string, same.
 * @param image_in,image_out 4-float pixels; may be the same buffer.
 * @param width,height buffer geometry.
 * @param cst_from,cst_to source and destination colorspaces. Only RGB <-> Lab is supported;
 *        anything else leaves `*converted_cst == cst_from` and logs to stderr.
 * @param converted_cst out: @p cst_to on success, @p cst_from on refusal.
 * @param profile_info profile identity to resolve. Only `type`, `filename` and `intent` are
 *        read -- the matrices and LUTs are ignored on this path. A `type` of
 *        DT_COLORSPACE_NONE silently substitutes linear Rec2020 here, unlike
 *        dt_colorspaces_apply_profile(), which refuses it before ever getting this far.
 */
void dt_ioppr_transform_lcms2(const char *op, const char *multi_name, const float *const image_in,
                              float *const image_out, const int width, const int height,
                              const dt_iop_colorspace_type_t cst_from,
                              const dt_iop_colorspace_type_t cst_to,
                              dt_iop_colorspace_type_t *converted_cst,
                              const dt_iop_order_iccprofile_info_t *const profile_info);
/**
 * @brief The vectorised branch of dt_colorspaces_apply_profile(): RGB <-> Lab by matrix and
 * tone-curve LUT.
 *
 * @details No lcms2, no lock, no per-call setup: it reads the matrices and curves already in
 * @p profile_info and runs an OpenMP/SIMD loop. Preserves the 4th float of each pixel on the
 * Lab -> RGB leg (some callers convert in place and rely on alpha surviving).
 *
 * @param op module `op` string, for the trace only.
 * @param multi_name module `multi_name` string, same.
 * @param image_in,image_out 4-float pixels; may be the same buffer.
 * @param width,height buffer geometry.
 * @param cst_from,cst_to source and destination colorspaces; only RGB <-> Lab is supported.
 * @param converted_cst out: @p cst_to on success, @p cst_from on refusal.
 * @param profile_info profile whose matrices and curves to use.
 * @warning Does NOT check the NaN sentinel. Called on a non-matrix profile it will happily
 *          multiply by NaN and fill the buffer with NaN. dt_colorspaces_apply_profile() is
 *          the thing that makes that check.
 */
void dt_ioppr_transform_matrix(const char *op, const char *multi_name, const float *const image_in,
                               float *const image_out, const int width, const int height,
                               const dt_iop_colorspace_type_t cst_from,
                               const dt_iop_colorspace_type_t cst_to,
                               dt_iop_colorspace_type_t *converted_cst,
                               const dt_iop_order_iccprofile_info_t *const profile_info);

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
