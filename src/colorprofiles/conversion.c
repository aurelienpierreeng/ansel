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

#include "colorprofiles/conversion.h"
#include "colorprofiles/colorspaces.h"
#include "colorprofiles/iop_profile.h"

#include "common/colorspaces_inline_conversions.h"
#include "common/logging.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"
#include "system/simd.h"
#include "system/target_clones.h"

#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * The prepared conversion. Two shapes live in here, and exactly one of them is active:
 *
 *   is_matrix == TRUE   source curves -> matrix -> [clamp in clip primaries -> matrix] ->
 *                       target curves
 *   is_matrix == FALSE  one or two `cmsHTRANSFORM`, with the clamp between them when the
 *                       conversion clips.
 *
 * The sentinel encodings the two IOPs used to dispatch on from the outside -- `isnan(m[0][0])`
 * for "no matrix", `lut[c][0] < 0` for "this channel is linear" -- are still how the curves
 * are stored, because that is what dt_ioppr_eval_trc() and the OpenCL kernels read. The
 * difference is that they are now read here and by the kernels, not by module logic.
 */
struct dt_colorspaces_conversion_t
{
  dt_colorspaces_color_profile_type_t from_type;
  dt_colorspaces_color_profile_type_t to_type;

  gboolean is_matrix;
  gboolean has_clipping;

  dt_colormatrix_t matrix;      //!< source -> target, or source -> clip when clipping
  dt_colormatrix_t clip_matrix; //!< clip -> target; meaningless unless has_clipping

  /* The source profile's own RGB -> XYZ, kept uncomposed so a caller can describe the space
   * the buffer arrives in. Derived on both branches -- falling back to lcms2 says nothing
   * about whether the SOURCE reduced to a matrix. */
  dt_colormatrix_t source_matrix;
  gboolean have_source_matrix;

  float *lut_source[3];         //!< NULL when this conversion has no source curve stage
  float coeffs_source[3][3];    //!< 9 contiguous floats: what the kernels upload verbatim
  int nonlinear_source;

  float *lut_target[3];
  float coeffs_target[3][3];
  int nonlinear_target;

  cmsHTRANSFORM xform;          //!< source -> target, or source -> clip when clipping
  cmsHTRANSFORM clip_xform;     //!< clip -> target; NULL unless has_clipping
  gboolean gamutcheck;

  /* Handles this conversion created and must close. An endpoint resolved from the profile
   * list is BORROWED -- the list owns it -- and never lands here. The only owned handle in
   * practice is the quantised soft-proof copy. */
  cmsHPROFILE owned[1];
  int n_owned;
};

/* Some built-in profiles carry a parametric TRC, which lcms2 reproduces exactly: a round trip
 * through such a profile is the identity, and soft-proofing it shows nothing. Serialising and
 * reopening quantises the curve into a sampled table, which is what makes the proof visible.
 * Moved here verbatim from iop/colorout.c -- it is the only reason that module still named a
 * cmsHPROFILE. */
static cmsHPROFILE _quantise_profile(cmsHPROFILE profile)
{
  cmsUInt32Number size;
  cmsHPROFILE quantised = NULL;

  if(profile && cmsSaveProfileToMem(profile, NULL, &size))
  {
    char *data = malloc(size);
    if(!IS_NULL_PTR(data))
    {
      if(cmsSaveProfileToMem(profile, data, &size)) quantised = cmsOpenProfileFromMem(data, size);
      dt_free(data);
    }
  }

  return quantised;
}

/* Resolve one endpoint to a handle. Returns the container when the identity came from the
 * profile list, so the caller can hold that entry's lock across the derivation, or NULL when
 * the endpoint carried an already-resolved image-owned profile (which the caller pins by
 * outliving us) or could not be resolved at all. */
static cmsHPROFILE _resolve_endpoint(const dt_colorspaces_endpoint_t *const endpoint,
                                     const dt_colorspaces_color_profile_t **entry)
{
  *entry = NULL;
  if(IS_NULL_PTR(endpoint)) return NULL;

  if(!IS_NULL_PTR(endpoint->resolved)) return endpoint->resolved->profile;

  const dt_colorspaces_color_profile_t *const found
      = dt_colorspaces_get_profile(endpoint->type, endpoint->filename ? endpoint->filename : "",
                                   endpoint->role);
  if(IS_NULL_PTR(found)) return NULL;

  *entry = found;
  return found->profile;
}

/* lcms2 pixel format for a handle. colorin used to derive this from cmsGetColorSpace() and
 * colorout from the type enum; the profile itself is the authority for both, and it answers
 * the export case (an ICC embedded in the source file) that no type enum describes. */
static cmsUInt32Number _format_for(cmsHPROFILE profile, gboolean *supported)
{
  *supported = TRUE;
  if(IS_NULL_PTR(profile)) return TYPE_RGBA_FLT;

  const cmsColorSpaceSignature space = cmsGetColorSpace(profile);
  switch(space)
  {
    case cmsSigRgbData:
      return TYPE_RGBA_FLT;
    case cmsSigXYZData:
      return TYPE_XYZA_FLT;
    default:
      /* The signature is four packed characters, most significant first. */
      dt_print(DT_DEBUG_COLORPROFILE, "[colorspaces] profile color space `%c%c%c%c' not supported\n",
               (char)(space >> 24), (char)(space >> 16), (char)(space >> 8), (char)(space));
      *supported = FALSE;
      return TYPE_RGBA_FLT;
  }
}

static gboolean _allocate_curves(float *lut[3])
{
  for(int c = 0; c < 3; c++)
  {
    lut[c] = dt_alloc_align_float(DT_CONVERSION_LUT_SAMPLES);
    if(IS_NULL_PTR(lut[c])) return FALSE;
    lut[c][0] = -1.0f; // linear until proven otherwise
  }
  return TRUE;
}

static void _free_curves(float *lut[3])
{
  for(int c = 0; c < 3; c++)
  {
    dt_free_align(lut[c]);
    lut[c] = NULL;
  }
}

dt_colorspaces_conversion_t *dt_colorspaces_prepare_conversion(const dt_colorspaces_endpoint_t *const from,
                                                               const dt_colorspaces_endpoint_t *const to,
                                                               const dt_colorspaces_endpoint_t *const clip,
                                                               const dt_colorspaces_endpoint_t *const proof,
                                                               const dt_iop_color_intent_t intent,
                                                               const dt_colorspaces_conversion_flags_t flags)
{
  if(IS_NULL_PTR(from) || IS_NULL_PTR(to)) return NULL;

  dt_colorspaces_conversion_t *conversion = calloc(1, sizeof(dt_colorspaces_conversion_t));
  if(IS_NULL_PTR(conversion)) return NULL;

  conversion->from_type = from->type;
  conversion->to_type = to->type;
  conversion->matrix[0][0] = NAN;
  conversion->clip_matrix[0][0] = NAN;

  /* Resolve, THEN pin, and hold until the last derivation is done. The display profile's
   * handle is replaced whenever the window lands on another monitor, so every read of it --
   * the matrix extraction as much as cmsCreateTransform() -- has to sit inside the lock. The
   * lock is per profile, so pinning the monitor profile here does not stand between an
   * unrelated thumbnail conversion and the profile it uses. */
  const dt_colorspaces_color_profile_t *from_entry = NULL, *to_entry = NULL;
  const dt_colorspaces_color_profile_t *clip_entry = NULL, *proof_entry = NULL;
  cmsHPROFILE from_profile = _resolve_endpoint(from, &from_entry);
  cmsHPROFILE to_profile = _resolve_endpoint(to, &to_entry);
  cmsHPROFILE clip_profile = _resolve_endpoint(clip, &clip_entry);
  cmsHPROFILE proof_profile = _resolve_endpoint(proof, &proof_entry);

  dt_colorspaces_lock_profile(from_entry);
  dt_colorspaces_lock_profile(to_entry);
  dt_colorspaces_lock_profile(clip_entry);
  dt_colorspaces_lock_profile(proof_entry);

  /* Everything the failure path needs is declared before the first goto: jumping forward past
   * an initialisation is legal C but leaves the object indeterminate, and -Wjump-misses-init
   * is right to complain about it. */
  gboolean source_supported = TRUE, target_supported = TRUE;
  cmsUInt32Number source_format = TYPE_RGBA_FLT, target_format = TYPE_RGBA_FLT;
  gboolean proofing = FALSE;
  gboolean matrix_allowed = FALSE;

  if(IS_NULL_PTR(from_profile) || IS_NULL_PTR(to_profile)) goto give_up;
  if(!IS_NULL_PTR(clip) && IS_NULL_PTR(clip_profile)) goto give_up;

  source_format = _format_for(from_profile, &source_supported);
  target_format = _format_for(to_profile, &target_supported);

  /* A quantised copy of the soft-proof profile is the one handle this object owns. If
   * quantising fails we drop proofing rather than proof against something that would show
   * nothing -- which is what this code has always done. */
  if(!IS_NULL_PTR(proof_profile))
  {
    proof_profile = _quantise_profile(proof_profile);
    if(!IS_NULL_PTR(proof_profile)) conversion->owned[conversion->n_owned++] = proof_profile;
  }

  proofing = !IS_NULL_PTR(proof_profile);
  conversion->gamutcheck = proofing && (flags & DT_CONVERSION_GAMUTCHECK);
  conversion->has_clipping = !IS_NULL_PTR(clip_profile);

  /* --- the matrix branch ---
   *
   * Both factors are the plain colorant matrices lcms2 reports, composed as
   * `target_out x source_in`. That is the same expression, on the same handles, that
   * iop/colorin.c and iop/colorout.c each built by hand, so the result is bit-identical to
   * what they produced -- which matters, because these two modules decide the colour of every
   * exported pixel. */
  matrix_allowed = !proofing && !(flags & DT_CONVERSION_FORCE_LCMS2) && source_supported && target_supported;

  conversion->have_source_matrix
      = dt_colorspaces_get_matrix_from_input_profile(from_profile, conversion->source_matrix, NULL, NULL, NULL, 0)
        == 0;
  if(!conversion->have_source_matrix) conversion->source_matrix[0][0] = NAN;

  if(matrix_allowed && _allocate_curves(conversion->lut_source) && _allocate_curves(conversion->lut_target))
  {
    dt_colormatrix_t source_in, target_out;
    const gboolean have_source
        = dt_colorspaces_get_matrix_from_input_profile(from_profile, source_in, conversion->lut_source[0],
                                                       conversion->lut_source[1], conversion->lut_source[2],
                                                       DT_CONVERSION_LUT_SAMPLES) == 0;
    const gboolean have_target
        = dt_colorspaces_get_matrix_from_output_profile(to_profile, target_out, conversion->lut_target[0],
                                                        conversion->lut_target[1], conversion->lut_target[2],
                                                        DT_CONVERSION_LUT_SAMPLES) == 0;

    if(have_source && have_target)
    {
      conversion->nonlinear_source
          = dt_ioppr_init_unbounded_coeffs(conversion->lut_source[0], conversion->lut_source[1],
                                           conversion->lut_source[2], conversion->coeffs_source[0],
                                           conversion->coeffs_source[1], conversion->coeffs_source[2],
                                           DT_CONVERSION_LUT_SAMPLES);
      conversion->nonlinear_target
          = dt_ioppr_init_unbounded_coeffs(conversion->lut_target[0], conversion->lut_target[1],
                                           conversion->lut_target[2], conversion->coeffs_target[0],
                                           conversion->coeffs_target[1], conversion->coeffs_target[2],
                                           DT_CONVERSION_LUT_SAMPLES);

      /* A curve stage the caller cannot execute is not a curve stage we may silently skip:
       * dropping it would render the image through the wrong transfer function. Fall back to
       * lcms2, which applies both sides itself. */
      const gboolean source_ok = (conversion->nonlinear_source == 0) || (flags & DT_CONVERSION_SOURCE_CURVES);
      const gboolean target_ok = (conversion->nonlinear_target == 0) || (flags & DT_CONVERSION_TARGET_CURVES);

      if(source_ok && target_ok)
      {
        if(conversion->has_clipping)
        {
          /* The clipping space contributes its PRIMARIES only: the clamp bounds the colour to
           * that gamut, it is not a round trip through its transfer function. Hence the two
           * matrices and no curves. */
          dt_colormatrix_t clip_out, clip_in;
          if(dt_colorspaces_get_matrix_from_output_profile(clip_profile, clip_out, NULL, NULL, NULL, 0) == 0
             && dt_colorspaces_get_matrix_from_input_profile(clip_profile, clip_in, NULL, NULL, NULL, 0) == 0)
          {
            dt_colormatrix_mul(conversion->matrix, clip_out, source_in);
            dt_colormatrix_mul(conversion->clip_matrix, target_out, clip_in);
            conversion->is_matrix = TRUE;
          }
        }
        else
        {
          dt_colormatrix_mul(conversion->matrix, target_out, source_in);
          conversion->is_matrix = TRUE;
        }
      }
    }
  }

  if(conversion->is_matrix)
  {
    /* Keep exactly the sides the caller declared it consumes, whether or not they turned out
     * to be linear. A device kernel reads `curve[0] < 0` as "this channel is linear" and
     * needs the buffer present to read it, so handing back NULL for a linear-but-requested
     * side would make every caller invent a ramp to upload instead. The side nobody asked
     * for is 768 KB of table nothing will read. */
    if(!(flags & DT_CONVERSION_SOURCE_CURVES)) _free_curves(conversion->lut_source);
    if(!(flags & DT_CONVERSION_TARGET_CURVES)) _free_curves(conversion->lut_target);
  }
  else
  {
    _free_curves(conversion->lut_source);
    _free_curves(conversion->lut_target);
    conversion->nonlinear_source = conversion->nonlinear_target = 0;
    conversion->matrix[0][0] = NAN;
    conversion->clip_matrix[0][0] = NAN;

    /* --- the lcms2 branch ---
     *
     * cmsCreateProofingTransform() with a NULL proof and no proofing flags IS an ordinary
     * transform, so one call covers both cases. NOCACHE because these transforms are driven
     * from OpenMP loops and lcms2's 1-pixel memo is per-transform mutable state. */
    cmsUInt32Number transform_flags = cmsFLAGS_NOCACHE;
    if(proofing)
    {
      transform_flags |= cmsFLAGS_SOFTPROOFING | cmsFLAGS_BLACKPOINTCOMPENSATION;
      if(flags & DT_CONVERSION_GAMUTCHECK) transform_flags |= cmsFLAGS_GAMUTCHECK;
    }

    if(conversion->has_clipping)
    {
      conversion->xform = cmsCreateTransform(from_profile, source_format, clip_profile, TYPE_RGBA_FLT,
                                             intent, transform_flags);
      conversion->clip_xform = cmsCreateTransform(clip_profile, TYPE_RGBA_FLT, to_profile, target_format,
                                                  intent, transform_flags);
      if(IS_NULL_PTR(conversion->xform) || IS_NULL_PTR(conversion->clip_xform))
      {
        if(!IS_NULL_PTR(conversion->xform)) cmsDeleteTransform(conversion->xform);
        if(!IS_NULL_PTR(conversion->clip_xform)) cmsDeleteTransform(conversion->clip_xform);
        conversion->xform = conversion->clip_xform = NULL;
        conversion->has_clipping = FALSE;
      }
    }

    if(IS_NULL_PTR(conversion->xform))
    {
      conversion->has_clipping = FALSE;
      conversion->xform = cmsCreateProofingTransform(from_profile, source_format, to_profile, target_format,
                                                     proof_profile, intent, INTENT_RELATIVE_COLORIMETRIC,
                                                     transform_flags);
    }

    if(IS_NULL_PTR(conversion->xform)) goto give_up;
  }

  dt_colorspaces_unlock_profile(proof_entry);
  dt_colorspaces_unlock_profile(clip_entry);
  dt_colorspaces_unlock_profile(to_entry);
  dt_colorspaces_unlock_profile(from_entry);
  return conversion;

give_up:
  /* Unlocked against the entries the locks were taken on, never against a re-test of the
   * conditions that produced them: those conditions have been rewritten by now. */
  dt_colorspaces_unlock_profile(proof_entry);
  dt_colorspaces_unlock_profile(clip_entry);
  dt_colorspaces_unlock_profile(to_entry);
  dt_colorspaces_unlock_profile(from_entry);
  dt_colorspaces_free_conversion(&conversion);
  return NULL;
}

void dt_colorspaces_free_conversion(dt_colorspaces_conversion_t **conversion)
{
  if(IS_NULL_PTR(conversion) || IS_NULL_PTR(*conversion)) return;
  dt_colorspaces_conversion_t *c = *conversion;

  _free_curves(c->lut_source);
  _free_curves(c->lut_target);

  if(!IS_NULL_PTR(c->xform)) cmsDeleteTransform(c->xform);
  if(!IS_NULL_PTR(c->clip_xform)) cmsDeleteTransform(c->clip_xform);

  for(int k = 0; k < c->n_owned; k++) dt_colorspaces_cleanup_profile(c->owned[k]);

  free(c);
  *conversion = NULL;
}

/* --- apply ---------------------------------------------------------------- */

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t _clamp_unit(dt_aligned_pixel_simd_t v)
{
  v[0] = CLAMP(v[0], 0.0f, 1.0f);
  v[1] = CLAMP(v[1], 0.0f, 1.0f);
  v[2] = CLAMP(v[2], 0.0f, 1.0f);
  v[3] = 0.0f;
  return v;
}

__DT_CLONE_TARGETS__
static void _apply_target_curves(const dt_colorspaces_conversion_t *const c, float *const restrict out,
                                 const size_t npixels)
{
  const float *const restrict lut0 = c->lut_target[0];
  const float *const restrict lut1 = c->lut_target[1];
  const float *const restrict lut2 = c->lut_target[2];
  const int run_lut0 = lut0[0] >= 0.0f;
  const int run_lut1 = lut1[0] >= 0.0f;
  const int run_lut2 = lut2[0] >= 0.0f;
  if(!(run_lut0 || run_lut1 || run_lut2)) return;

  const float *const coeff0 = c->coeffs_target[0];
  const float *const coeff1 = c->coeffs_target[1];
  const float *const coeff2 = c->coeffs_target[2];

  if(run_lut0 && run_lut1 && run_lut2)
  {
    __OMP_PARALLEL_FOR__()
    for(size_t k = 0; k < npixels; k++)
    {
      const size_t idx = 4 * k;
      out[idx + 0] = dt_ioppr_eval_trc(out[idx + 0], lut0, coeff0, DT_CONVERSION_LUT_SAMPLES);
      out[idx + 1] = dt_ioppr_eval_trc(out[idx + 1], lut1, coeff1, DT_CONVERSION_LUT_SAMPLES);
      out[idx + 2] = dt_ioppr_eval_trc(out[idx + 2], lut2, coeff2, DT_CONVERSION_LUT_SAMPLES);
    }
  }
  else
  {
    __OMP_PARALLEL_FOR__()
    for(size_t k = 0; k < npixels; k++)
    {
      const size_t idx = 4 * k;
      if(run_lut0) out[idx + 0] = dt_ioppr_eval_trc(out[idx + 0], lut0, coeff0, DT_CONVERSION_LUT_SAMPLES);
      if(run_lut1) out[idx + 1] = dt_ioppr_eval_trc(out[idx + 1], lut1, coeff1, DT_CONVERSION_LUT_SAMPLES);
      if(run_lut2) out[idx + 2] = dt_ioppr_eval_trc(out[idx + 2], lut2, coeff2, DT_CONVERSION_LUT_SAMPLES);
    }
  }
}

/* The target curves are a SEPARATE pass over the output buffer, not a stage fused into the
 * matrix loop. Fusing them is the obvious simplification and it is wrong: the matrix loop is
 * `__OMP_PARALLEL_FOR_SIMD__`, so the compiler vectorises it across pixels and contracts the
 * multiply-adds into FMAs, and folding a table lookup into the loop body changes what it can
 * contract. Measured: the fused form moved 747159 of 2549760 exported pixels by one LSB on a
 * raw. One LSB is small; a colour-management change that moves pixels for no stated reason is
 * not, so the structure stays as the two modules had it. */
__DT_CLONE_TARGETS__
static void _apply_matrix(const dt_colorspaces_conversion_t *const c, const float *const restrict in,
                          float *const restrict out, const size_t npixels,
                          const dt_colorspaces_conversion_hook_t hook)
{
  dt_colormatrix_t m, cm;
  transpose_3xSSE(c->matrix, m);
  transpose_3xSSE(c->clip_matrix, cm);
  const dt_aligned_pixel_simd_t m0 = dt_colormatrix_row_to_simd(m, 0);
  const dt_aligned_pixel_simd_t m1 = dt_colormatrix_row_to_simd(m, 1);
  const dt_aligned_pixel_simd_t m2 = dt_colormatrix_row_to_simd(m, 2);
  const dt_aligned_pixel_simd_t c0 = dt_colormatrix_row_to_simd(cm, 0);
  const dt_aligned_pixel_simd_t c1 = dt_colormatrix_row_to_simd(cm, 1);
  const dt_aligned_pixel_simd_t c2 = dt_colormatrix_row_to_simd(cm, 2);

  /* Gate on the buffer, not on the count: the side the caller did not ask for is released
   * even when the profile turned out to have curves, so `nonlinear_target > 0` can be true
   * with nothing to read. */
  const gboolean decode = !IS_NULL_PTR(c->lut_source[0]) && c->nonlinear_source > 0;
  const gboolean encode = !IS_NULL_PTR(c->lut_target[0]) && c->nonlinear_target > 0;
  const gboolean clipping = c->has_clipping;

  if(!decode && IS_NULL_PTR(hook))
  {
    /* Nothing to do per pixel but the matrix. Non-temporal stores unless a second pass is
     * about to read this buffer straight back, which is what they are bad at. */
    if(encode)
    {
      __OMP_PARALLEL_FOR_SIMD__(aligned(in, out : 64))
      for(size_t k = 0; k < npixels; k++)
      {
        const size_t idx = 4 * k;
        dt_aligned_pixel_simd_t v = dt_mat3x4_mul_vec4(dt_load_simd_aligned(in + idx), m0, m1, m2);
        if(clipping) v = dt_mat3x4_mul_vec4(_clamp_unit(v), c0, c1, c2);
        dt_store_simd_aligned(out + idx, v);
      }
    }
    else
    {
      __OMP_PARALLEL_FOR_SIMD__(aligned(in, out : 64))
      for(size_t k = 0; k < npixels; k++)
      {
        const size_t idx = 4 * k;
        dt_aligned_pixel_simd_t v = dt_mat3x4_mul_vec4(dt_load_simd_aligned(in + idx), m0, m1, m2);
        if(clipping) v = dt_mat3x4_mul_vec4(_clamp_unit(v), c0, c1, c2);
        dt_store_simd_nontemporal(out + idx, v);
      }
      dt_omploop_sfence();
    }
  }
  else
  {
    const float *const lut_r = decode ? c->lut_source[0] : NULL;
    const float *const lut_g = decode ? c->lut_source[1] : NULL;
    const float *const lut_b = decode ? c->lut_source[2] : NULL;

    __OMP_PARALLEL_FOR__()
    for(size_t k = 0; k < npixels; k++)
    {
      const float *const in_pixel = in + 4 * k;
      float *const out_pixel = out + 4 * k;

      dt_aligned_pixel_t staged;
      /* A channel marked linear is passed through rather than sampled: that is what keeps
       * values above white unbounded instead of clipped at the top of the table. */
      staged[0] = (decode && lut_r[0] >= 0.0f)
                      ? dt_ioppr_eval_trc(in_pixel[0], lut_r, c->coeffs_source[0], DT_CONVERSION_LUT_SAMPLES)
                      : in_pixel[0];
      staged[1] = (decode && lut_g[0] >= 0.0f)
                      ? dt_ioppr_eval_trc(in_pixel[1], lut_g, c->coeffs_source[1], DT_CONVERSION_LUT_SAMPLES)
                      : in_pixel[1];
      staged[2] = (decode && lut_b[0] >= 0.0f)
                      ? dt_ioppr_eval_trc(in_pixel[2], lut_b, c->coeffs_source[2], DT_CONVERSION_LUT_SAMPLES)
                      : in_pixel[2];
      staged[3] = 0.0f;

      if(!IS_NULL_PTR(hook)) hook(staged, staged);

      dt_aligned_pixel_simd_t v = dt_mat3x4_mul_vec4(dt_load_simd_aligned(staged), m0, m1, m2);
      if(clipping) v = dt_mat3x4_mul_vec4(_clamp_unit(v), c0, c1, c2);

      if(encode)
        dt_store_simd_aligned(out_pixel, v);
      else
        dt_store_simd_nontemporal(out_pixel, v);
    }
    if(!encode) dt_omploop_sfence();
  }

  if(encode) _apply_target_curves(c, out, npixels);
}

__DT_CLONE_TARGETS__
static void _apply_lcms2(const dt_colorspaces_conversion_t *const c, const float *const in, float *const out,
                         const size_t width, const size_t height, const dt_colorspaces_conversion_hook_t hook)
{
  /* Alias the transforms outside the parallel region and share the aliases explicitly,
   * rather than reaching through the struct from inside the loop. */
  const cmsHTRANSFORM xform = c->xform;
  const cmsHTRANSFORM clip_xform = c->clip_xform;
  const gboolean clipping = c->has_clipping;
  const gboolean gamutcheck = c->gamutcheck;

  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; row++)
  {
    const float *const restrict source = in + 4 * row * width;
    float *const restrict target = out + 4 * row * width;

    if(!IS_NULL_PTR(hook))
    {
      /* The hook runs on the values just before the colour conversion proper. lcms2 decodes
       * internally, so here that means before cmsDoTransform -- which is where colorin has
       * always applied it. Staged through the output row, as it was. */
      for(size_t j = 0; j < width; j++)
      {
        hook(source + 4 * j, target + 4 * j);
        target[4 * j + 3] = 0.0f;
      }
      dt_colorspaces_transform_rgba_float_row(xform, target, target, width);
    }
    else
    {
      dt_colorspaces_transform_rgba_float_row(xform, source, target, width);
    }

    if(clipping)
    {
      float *const restrict clipped = target;
      __OMP_SIMD__(aligned(clipped : 64))
      for(size_t j = 0; j < width; j++)
      {
        for(int ch = 0; ch < 3; ch++) clipped[4 * j + ch] = CLAMP(clipped[4 * j + ch], 0.0f, 1.0f);
      }
      dt_colorspaces_transform_rgba_float_row(clip_xform, target, target, width);
    }

    if(gamutcheck)
    {
      for(size_t j = 0; j < width; j++)
      {
        if(target[4 * j + 0] < 0.0f || target[4 * j + 1] < 0.0f || target[4 * j + 2] < 0.0f)
        {
          target[4 * j + 0] = 0.0f;
          target[4 * j + 1] = 1.0f;
          target[4 * j + 2] = 1.0f;
        }
      }
    }
  }
}

void dt_colorspaces_apply_conversion_hooked(const dt_colorspaces_conversion_t *const conversion,
                                            const float *const in, float *const out, const size_t width,
                                            const size_t height, const dt_colorspaces_conversion_hook_t hook)
{
  if(IS_NULL_PTR(conversion) || IS_NULL_PTR(in) || IS_NULL_PTR(out)) return;

  if(conversion->is_matrix)
    _apply_matrix(conversion, in, out, width * height, hook);
  else
    _apply_lcms2(conversion, in, out, width, height, hook);
}

void dt_colorspaces_apply_conversion(const dt_colorspaces_conversion_t *const conversion, const float *const in,
                                     float *const out, const size_t width, const size_t height)
{
  dt_colorspaces_apply_conversion_hooked(conversion, in, out, width, height, NULL);
}

/* --- what a device kernel needs ------------------------------------------- */

gboolean dt_colorspaces_conversion_is_matrix(const dt_colorspaces_conversion_t *const conversion)
{
  return !IS_NULL_PTR(conversion) && conversion->is_matrix;
}

gboolean dt_colorspaces_conversion_has_clipping(const dt_colorspaces_conversion_t *const conversion)
{
  return !IS_NULL_PTR(conversion) && conversion->has_clipping;
}

gboolean dt_colorspaces_conversion_matrix(const dt_colorspaces_conversion_t *const conversion,
                                          dt_colormatrix_t matrix)
{
  if(IS_NULL_PTR(conversion) || !conversion->is_matrix) return FALSE;
  memcpy(matrix, conversion->matrix, sizeof(dt_colormatrix_t));
  return TRUE;
}

gboolean dt_colorspaces_conversion_source_matrix(const dt_colorspaces_conversion_t *const conversion,
                                                 dt_colormatrix_t matrix)
{
  if(IS_NULL_PTR(conversion) || !conversion->have_source_matrix) return FALSE;
  memcpy(matrix, conversion->source_matrix, sizeof(dt_colormatrix_t));
  return TRUE;
}

gboolean dt_colorspaces_conversion_clip_matrix(const dt_colorspaces_conversion_t *const conversion,
                                               dt_colormatrix_t matrix)
{
  if(IS_NULL_PTR(conversion) || !conversion->is_matrix || !conversion->has_clipping) return FALSE;
  memcpy(matrix, conversion->clip_matrix, sizeof(dt_colormatrix_t));
  return TRUE;
}

const float *dt_colorspaces_conversion_source_curve(const dt_colorspaces_conversion_t *const conversion,
                                                    const int channel)
{
  if(IS_NULL_PTR(conversion) || channel < 0 || channel > 2) return NULL;
  return conversion->lut_source[channel];
}

const float *dt_colorspaces_conversion_target_curve(const dt_colorspaces_conversion_t *const conversion,
                                                    const int channel)
{
  if(IS_NULL_PTR(conversion) || channel < 0 || channel > 2) return NULL;
  return conversion->lut_target[channel];
}

const float *dt_colorspaces_conversion_source_coeffs(const dt_colorspaces_conversion_t *const conversion)
{
  if(IS_NULL_PTR(conversion) || IS_NULL_PTR(conversion->lut_source[0])) return NULL;
  return &conversion->coeffs_source[0][0];
}

const float *dt_colorspaces_conversion_target_coeffs(const dt_colorspaces_conversion_t *const conversion)
{
  if(IS_NULL_PTR(conversion) || IS_NULL_PTR(conversion->lut_target[0])) return NULL;
  return &conversion->coeffs_target[0][0];
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
