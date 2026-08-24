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

#include "colorprofiles/colorspaces.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "colorprofiles/iop_profile.h"   // dt_colorspaces_invalidate_display_profile_memo()

#include <stddef.h>   // offsetof(), for the startup self-test

/* dt_iop_color_intent_t is spelled with literal values in profile_types.h so that header
 * needs no <lcms2.h>. The values are fixed by the ICC specification and are serialised into
 * iop params, so they cannot change on either side -- but this is the one place that sees
 * both definitions, so it is the place to say so out loud. */
_Static_assert(DT_INTENT_PERCEPTUAL == INTENT_PERCEPTUAL, "ICC intent renumbered by lcms2");
_Static_assert(DT_INTENT_RELATIVE_COLORIMETRIC == INTENT_RELATIVE_COLORIMETRIC, "ICC intent renumbered by lcms2");
_Static_assert(DT_INTENT_SATURATION == INTENT_SATURATION, "ICC intent renumbered by lcms2");
_Static_assert(DT_INTENT_ABSOLUTE_COLORIMETRIC == INTENT_ABSOLUTE_COLORIMETRIC, "ICC intent renumbered by lcms2");
#include "colorprofiles/colormatrices.c"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "math/matrices.h"
#include "common/utility.h"
#include "common/conf.h"
#include "common/logging.h"

#include <strings.h>

#ifdef USE_COLORDGTK
#include "colord-gtk.h"
#endif

#ifdef _WIN32
#include <dwmapi.h>
#include <gdk/gdkwin32.h>
#endif

#include "system/target_clones.h"
#include "system/display_profile.h"
#include <glib/gi18n.h>

#if 0
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreServices/CoreServices.h>
#endif

/* The module's single instance. Private: nothing outside src/colorprofiles/ names it. */
static dt_colorspaces_t *dt_colorspaces_get_global(void);

static dt_colorspaces_color_profile_t *_create_profile(dt_colorspaces_color_profile_type_t type,
                                                       cmsHPROFILE profile, const char *name,
                                                       dt_colorspaces_profile_role_t roles);

static const cmsCIEXYZ d65 = {0.95045471, 1.00000000, 1.08905029};

//D65 (sRGB, AdobeRGB, Rec2020)
static const cmsCIExyY D65xyY = {0.312700492, 0.329000939, 1.0};

//D60
//static const cmsCIExyY d60 = {0.32168, 0.33767, 1.0};

//D50 (ProPhoto RGB)
static const cmsCIExyY D50xyY = {0.3457, 0.3585, 1.0};

// D65:
static const cmsCIExyYTRIPLE sRGB_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.3000, 0.6000, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Rec2020_Primaries = {
  {0.7080, 0.2920, 1.0}, // red
  {0.1700, 0.7970, 1.0}, // green
  {0.1310, 0.0460, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Rec709_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.3000, 0.6000, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE Adobe_Primaries = {
  {0.6400, 0.3300, 1.0}, // red
  {0.2100, 0.7100, 1.0}, // green
  {0.1500, 0.0600, 1.0}  // blue
};

// D65:
static const cmsCIExyYTRIPLE P3_Primaries = {
  {0.680, 0.320, 1.0}, // red
  {0.265, 0.690, 1.0}, // green
  {0.150, 0.060, 1.0}  // blue
};

// https://en.wikipedia.org/wiki/ProPhoto_RGB_color_space
// D50:
static const cmsCIExyYTRIPLE ProPhoto_Primaries = {
  /*       x,        y,       Y */
  { 0.734699, 0.265301, 1.0000 }, /* red   */
  { 0.159597, 0.840403, 1.0000 }, /* green */
  { 0.036598, 0.000105, 1.0000 }, /* blue  */
};

cmsCIEXYZTRIPLE Rec709_Primaries_Prequantized;

/* Someone to tell when the display profile changes. The application puts it on its signal bus;
 * this module has no bus and no business knowing there is a control loop. Unregistered, the
 * notification is dropped -- correct for a headless run, where nothing is watching a monitor. */
static dt_colorspaces_profile_changed_handler_t _profile_changed_handler = NULL;

/* Defined next to the counter it advances, in the settings block below. */
static void _advance_settings_generation(void);

void dt_colorspaces_set_profile_changed_handler(dt_colorspaces_profile_changed_handler_t handler)
{
  _profile_changed_handler = handler;
}

static void _notify_profile_changed(void)
{
  /* The monitor profile just changed, so anything derived from the old one is stale.
   * Nothing dropped the memoised DISPLAY entry before this, so a session kept the previous
   * monitor's matrices and tone curves indefinitely -- silently, since every hash and ROI
   * in the chain stayed consistent. */
  dt_colorspaces_invalidate_display_profile_memo();

  /* The DISPLAY entry's identity -- DT_COLORSPACE_DISPLAY, no filename -- is exactly what it
   * was a moment ago; only the bytes behind that name changed. So a consumer keyed on the
   * profile's NAME cannot tell that anything happened, and would go on serving pixels rendered
   * through the previous monitor's profile. The generation is the one thing that can say
   * "same name, different profile", which is why a prepared conversion folds it into its
   * identity (colorprofiles/conversion.c) and why the counter is advanced here and not only
   * by the setters. */
  _advance_settings_generation();

  if(_profile_changed_handler) _profile_changed_handler();
}

#define generate_mat3inv_body(c_type, A, B)                                                                  \
  int mat3inv_##c_type(c_type *const dst, const c_type *const src)                                           \
  {                                                                                                          \
                                                                                                             \
    const c_type det = A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3))                                     \
                       - A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3))                                   \
                       + A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                  \
                                                                                                             \
    const c_type epsilon = 1e-7f;                                                                            \
    if(fabs(det) < epsilon) return 1;                                                                        \
                                                                                                             \
    const c_type invDet = 1.0 / det;                                                                         \
                                                                                                             \
    B(1, 1) = invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));                                              \
    B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));                                             \
    B(1, 3) = invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                              \
                                                                                                             \
    B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));                                             \
    B(2, 2) = invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));                                              \
    B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));                                             \
                                                                                                             \
    B(3, 1) = invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));                                              \
    B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));                                             \
    B(3, 3) = invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));                                              \
    return 0;                                                                                                \
  }

#define A(y, x) src[(y - 1) * 3 + (x - 1)]
#define B(y, x) dst[(y - 1) * 3 + (x - 1)]
/** inverts the given 3x3 matrix */
generate_mat3inv_body(float, A, B)

    int mat3inv(float *const dst, const float *const src)
{
  return mat3inv_float(dst, src);
}

generate_mat3inv_body(double, A, B)
#undef B
#undef A
#undef generate_mat3inv_body


static const dt_colorspaces_color_profile_t *_get_profile(dt_colorspaces_t *self,
                                                          dt_colorspaces_color_profile_type_t type,
                                                          const char *filename,
                                                          dt_colorspaces_profile_role_t role);

__DT_CLONE_TARGETS__
static int dt_colorspaces_get_matrix_from_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize, const int input)
{
  // create an OpenCL processable matrix + tone curves from an cmsHPROFILE:
  // NOTE: may be invoked with matrix and LUT pointers set to null to find
  // out if the profile can be created at all.

  // check this first:
  if(IS_NULL_PTR(prof) || !cmsIsMatrixShaper(prof)) return 1;

  // there are some profiles that contain both a color LUT for some specific
  // intent and a generic matrix. in some cases the matrix might be
  // deliberately wrong with swapped blue and red channels in order to easily
  // detect if a color managed software is applying the LUT or the matrix.
  // thus, if this profile contains LUT for any intent, it might also contain
  // swapped matrix, so the only right way to handle it is to let LCMS apply it.
  const int UsedDirection = input ? LCMS_USED_AS_INPUT : LCMS_USED_AS_OUTPUT;

  if(cmsIsCLUT(prof, INTENT_PERCEPTUAL, UsedDirection)
     || cmsIsCLUT(prof, INTENT_RELATIVE_COLORIMETRIC, UsedDirection)
     || cmsIsCLUT(prof, INTENT_ABSOLUTE_COLORIMETRIC, UsedDirection)
     || cmsIsCLUT(prof, INTENT_SATURATION, UsedDirection))
    return 1;

  cmsToneCurve *red_curve = cmsReadTag(prof, cmsSigRedTRCTag);
  cmsToneCurve *green_curve = cmsReadTag(prof, cmsSigGreenTRCTag);
  cmsToneCurve *blue_curve = cmsReadTag(prof, cmsSigBlueTRCTag);

  cmsCIEXYZ *red_color = cmsReadTag(prof, cmsSigRedColorantTag);
  cmsCIEXYZ *green_color = cmsReadTag(prof, cmsSigGreenColorantTag);
  cmsCIEXYZ *blue_color = cmsReadTag(prof, cmsSigBlueColorantTag);

  if(IS_NULL_PTR(red_curve) || IS_NULL_PTR(green_curve) || IS_NULL_PTR(blue_curve) || IS_NULL_PTR(red_color) || IS_NULL_PTR(green_color) || IS_NULL_PTR(blue_color)) return 2;

  dt_colormatrix_t matrix_tmp = { { red_color->X, green_color->X, blue_color->X },
                                  { red_color->Y, green_color->Y, blue_color->Y },
                                  { red_color->Z, green_color->Z,  blue_color->Z } };

  // some camera ICC profiles claim to have color locations for red, green and blue base colors defined,
  // but in fact these are all set to zero. we catch this case here.
  float sum = 0.0f;
  for(int k1 = 0; k1 < 3; k1++)
    for(int k2 = 0; k2 < 3; k2++)
      sum += matrix_tmp[k1][k2];
  if(sum == 0.0f) return 3;

  if(input)
  {
    // mark as linear, if they are:
    if(lutr && lutg && lutb)
    {
      if(cmsIsToneCurveLinear(red_curve))
        lutr[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(red_curve, k / (lutsize - 1.0f));
      if(cmsIsToneCurveLinear(green_curve))
        lutg[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(green_curve, k / (lutsize - 1.0f));
      if(cmsIsToneCurveLinear(blue_curve))
        lutb[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(blue_curve, k / (lutsize - 1.0f));
    }
  }
  else
  {
    // invert profile->XYZ matrix for output profiles
    dt_colormatrix_t tmp;
    memcpy(tmp, matrix_tmp, sizeof(dt_colormatrix_t));
    if(mat3SSEinv(matrix_tmp, tmp))
      return 3;
    // also need to reverse gamma, to apply reverse before matrix multiplication:
    cmsToneCurve *rev_red = cmsReverseToneCurveEx(0x8000, red_curve);
    cmsToneCurve *rev_green = cmsReverseToneCurveEx(0x8000, green_curve);
    cmsToneCurve *rev_blue = cmsReverseToneCurveEx(0x8000, blue_curve);
    if(IS_NULL_PTR(rev_red) || IS_NULL_PTR(rev_green) || IS_NULL_PTR(rev_blue))
    {
      cmsFreeToneCurve(rev_red);
      cmsFreeToneCurve(rev_green);
      cmsFreeToneCurve(rev_blue);
      return 4;
    }

    if(lutr && lutg && lutb)
    {
      // pass on tonecurves, in case lutsize > 0:
      if(cmsIsToneCurveLinear(red_curve))
        lutr[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutr[k] = cmsEvalToneCurveFloat(rev_red, k / (lutsize - 1.0f));
      if(cmsIsToneCurveLinear(green_curve))
        lutg[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutg[k] = cmsEvalToneCurveFloat(rev_green, k / (lutsize - 1.0f));
      if(cmsIsToneCurveLinear(blue_curve))
        lutb[0] = -1.0f;
      else
        for(int k = 0; k < lutsize; k++) lutb[k] = cmsEvalToneCurveFloat(rev_blue, k / (lutsize - 1.0f));
    }

    cmsFreeToneCurve(rev_red);
    cmsFreeToneCurve(rev_green);
    cmsFreeToneCurve(rev_blue);
  }

  if(matrix)
    memcpy(matrix, matrix_tmp, sizeof(dt_colormatrix_t));

  return 0;
}

int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 1);
}

int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize)
{
  return dt_colorspaces_get_matrix_from_profile(prof, matrix, lutr, lutg, lutb, lutsize, 0);
}

static cmsHPROFILE dt_colorspaces_create_lab_profile()
{
  return cmsCreateLab4Profile(cmsD50_xyY());
}

static void _compute_prequantized_primaries(const cmsCIExyY* whitepoint,
                                            const cmsCIExyYTRIPLE* primaries,
                                            cmsCIEXYZTRIPLE *primaries_prequantized)
{
  cmsHPROFILE profile = cmsCreateRGBProfile(whitepoint, primaries, NULL);

  cmsCIEXYZ *R = cmsReadTag(profile, cmsSigRedColorantTag);
  cmsCIEXYZ *G = cmsReadTag(profile, cmsSigGreenColorantTag);
  cmsCIEXYZ *B = cmsReadTag(profile, cmsSigBlueColorantTag);

  primaries_prequantized->Red.X   = (double)R->X;
  primaries_prequantized->Red.Y   = (double)R->Y;
  primaries_prequantized->Red.Z   = (double)R->Z;

  primaries_prequantized->Green.X = (double)G->X;
  primaries_prequantized->Green.Y = (double)G->Y;
  primaries_prequantized->Green.Z = (double)G->Z;

  primaries_prequantized->Blue.X  = (double)B->X;
  primaries_prequantized->Blue.Y  = (double)B->Y;
  primaries_prequantized->Blue.Z  = (double)B->Z;

  cmsCloseProfile(profile);
}

static cmsHPROFILE _create_lcms_profile(const char *desc, const char *dmdd,
                                        const cmsCIExyY *whitepoint, const cmsCIExyYTRIPLE *primaries, cmsToneCurve *trc,
                                        gboolean v2)
{
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu3 = cmsMLUalloc(NULL, 1);
  cmsMLU *mlu4 = cmsMLUalloc(NULL, 1);

  cmsToneCurve *out_curves[3] = { trc, trc, trc };
  cmsHPROFILE profile = cmsCreateRGBProfile(whitepoint, primaries, out_curves);

  if(v2) cmsSetProfileVersion(profile, 2.4);

  cmsSetHeaderFlags(profile, cmsEmbeddedProfileTrue);

  cmsMLUsetASCII(mlu1, "en", "US", "Public Domain");
  cmsWriteTag(profile, cmsSigCopyrightTag, mlu1);

  cmsMLUsetASCII(mlu2, "en", "US", desc);
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu2);

  cmsMLUsetASCII(mlu3, "en", "US", dmdd);
  cmsWriteTag(profile, cmsSigDeviceModelDescTag, mlu3);

  cmsMLUsetASCII(mlu4, "en", "US", "darktable");
  cmsWriteTag(profile, cmsSigDeviceMfgDescTag, mlu4);

  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);
  cmsMLUfree(mlu3);
  cmsMLUfree(mlu4);

  return profile;
}

// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-2-201807-I!!PDF-F.pdf
// Perceptual Quantization / SMPTE standard ST.2084
static double _PQ_fct(double x)
{
  static const double M1 = 2610.0 / 16384.0;
  static const double M2 = (2523.0 / 4096.0) * 128.0;
  static const double C1 = 3424.0 / 4096.0;
  static const double C2 = (2413.0 / 4096.0) * 32.0;
  static const double C3 = (2392.0 / 4096.0) * 32.0;

  if (x == 0.0) return 0.0;
  const double sign = x;
  x = fabs(x);

  const double xpo = pow(x, 1.0 / M2);
  const double num = MAX(xpo - C1, 0.0);
  const double den = C2 - C3 * xpo;
  const double res = pow(num / den, 1.0 / M1);

  return copysign(res, sign);
}

// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-2-201807-I!!PDF-F.pdf
// Hybrid Log-Gamma
static double _HLG_fct(double x)
{
  static const double A = 0.17883277;
  static const double B = 0.28466892;
  static const double C = 0.55991073;

  /**
   * BT.2100 HLG EOTF inverse, mapping non-linear HLG code values to linear
   * light. The standard is defined on `[0, +inf)`, but we extend it by odd
   * symmetry so profile round-trips keep signed RGB values continuous around
   * black instead of clipping negative excursions.
   */
  const double sign = x;
  const double e = fabs(x);

  if(e <= 0.5)
    return copysign((e * e) / 3.0, sign);

  return copysign((exp((e - C) / A) + B) / 12.0, sign);
}

static cmsToneCurve* _colorspaces_create_transfer(int32_t size, double (*fct)(double))
{
  float *values = g_malloc(sizeof(float) * size);

  for (int32_t i = 0; i < size; ++i)
  {
    const double x = (float)i / (size - 1);
    const double y = MIN(fct(x), 1.0f);
    values[i] = (float)y;
  }

  cmsToneCurve* result = cmsBuildTabulatedToneCurveFloat(NULL, size, values);
  dt_free(values);
  return result;
}

static cmsHPROFILE _colorspaces_create_srgb_profile(gboolean v2)
{
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsHPROFILE profile = _create_lcms_profile("sRGB", "sRGB",
                                             &D65xyY, &sRGB_Primaries, transferFunction, v2);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_srgb_profile()
{
  return _colorspaces_create_srgb_profile(TRUE);
}

static cmsHPROFILE dt_colorspaces_create_srgb_profile_v4()
{
  return _colorspaces_create_srgb_profile(FALSE);
}

static cmsHPROFILE dt_colorspaces_create_brg_profile()
{
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsCIExyYTRIPLE BRG_Primaries = { sRGB_Primaries.Blue, sRGB_Primaries.Red, sRGB_Primaries.Green };

  cmsHPROFILE profile = _create_lcms_profile("BRG", "BRG",
                                             &D65xyY, &BRG_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_gamma_rec709_rgb_profile(void)
{
  cmsFloat64Number srgb_parameters[5] = { 1/0.45, 1.0 / 1.099,  0.099 / 1.099, 1.0 / 4.5, 0.081 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsHPROFILE profile = _create_lcms_profile("Gamma Rec709 RGB", "Gamma Rec709 RGB",
                                             &D65xyY, &Rec709_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_itur_bt1886_rgb_profile(void)
{
  // https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1886-0-201103-I!!PDF-E.pdf
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 2.19921875);

  cmsHPROFILE profile = _create_lcms_profile("ITU-R BT.1886 (gamma 2.4 Rec709)", "ITU-R BT.1886 (gamma 2.4 Rec709)",
                                             &D65xyY, &Rec709_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}


// Create the ICC virtual profile for adobe rgb space
static cmsHPROFILE dt_colorspaces_create_adobergb_profile(void)
{
  // AdobeRGB's "2.2" gamma is technically defined as 2 + 51/256
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 2.19921875);

  cmsHPROFILE profile = _create_lcms_profile("Adobe RGB (compatible)", "Adobe RGB",
                                             &D65xyY, &Adobe_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}


cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel)
{
  const dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      preset = dt_alternate_colormatrices + k;
      break;
    }
  }
  if(IS_NULL_PTR(preset)) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(IS_NULL_PTR(hp)) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable alternate %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel)
{
  const dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      preset = dt_vendor_colormatrices + k;
      break;
    }
  }
  if(IS_NULL_PTR(preset)) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(IS_NULL_PTR(hp)) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable vendor %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel)
{
  const dt_profiled_colormatrix_t *preset = NULL;
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      preset = dt_profiled_colormatrices + k;
      break;
    }
  }
  if(IS_NULL_PTR(preset)) return NULL;

  const float wxyz = preset->white[0] + preset->white[1] + preset->white[2];
  const float rxyz = preset->rXYZ[0] + preset->rXYZ[1] + preset->rXYZ[2];
  const float gxyz = preset->gXYZ[0] + preset->gXYZ[1] + preset->gXYZ[2];
  const float bxyz = preset->bXYZ[0] + preset->bXYZ[1] + preset->bXYZ[2];
  cmsCIExyY WP = { preset->white[0] / wxyz, preset->white[1] / wxyz, 1.0 };
  cmsCIExyYTRIPLE XYZPrimaries = { { preset->rXYZ[0] / rxyz, preset->rXYZ[1] / rxyz, 1.0 },
                                   { preset->gXYZ[0] / gxyz, preset->gXYZ[1] / gxyz, 1.0 },
                                   { preset->bXYZ[0] / bxyz, preset->bXYZ[1] / bxyz, 1.0 } };
  cmsToneCurve *Gamma[3];
  cmsHPROFILE hp;

  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);

  hp = cmsCreateRGBProfile(&WP, &XYZPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(IS_NULL_PTR(hp)) return NULL;

  char name[512];
  snprintf(name, sizeof(name), "darktable profiled %s", makermodel);
  cmsSetProfileVersion(hp, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", name);
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", name);
  cmsWriteTag(hp, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hp, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hp, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hp;
}

static cmsHPROFILE dt_colorspaces_create_xyz_profile(void)
{
  cmsHPROFILE hXYZ = cmsCreateXYZProfile();
  cmsSetPCS(hXYZ, cmsSigXYZData);
  cmsSetHeaderRenderingIntent(hXYZ, INTENT_PERCEPTUAL);

  if(IS_NULL_PTR(hXYZ)) return NULL;

  cmsSetProfileVersion(hXYZ, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "linear XYZ");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "darktable linear XYZ");
  cmsWriteTag(hXYZ, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(hXYZ, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(hXYZ, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return hXYZ;
}

static cmsHPROFILE dt_colorspaces_create_linear_rec709_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear Rec709 RGB", "Linear Rec709 RGB",
                                             &D65xyY, &Rec709_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear Rec2020 RGB", "Linear Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_pq_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _PQ_fct);

  cmsHPROFILE profile = _create_lcms_profile("PQ Rec2020 RGB", "PQ Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_hlg_rec2020_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _HLG_fct);

  cmsHPROFILE profile = _create_lcms_profile("HLG Rec2020 RGB", "HLG Rec2020 RGB",
                                             &D65xyY, &Rec2020_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_pq_p3_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _PQ_fct);

  cmsHPROFILE profile = _create_lcms_profile("PQ P3 RGB", "PQ P3 RGB",
                                             &D65xyY, &P3_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_hlg_p3_rgb_profile(void)
{
  cmsToneCurve *transferFunction = _colorspaces_create_transfer(4096, _HLG_fct);

  cmsHPROFILE profile = _create_lcms_profile("HLG P3 RGB", "HLG P3 RGB",
                                             &D65xyY, &P3_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_display_p3_rgb_profile(void)
{
  cmsFloat64Number srgb_parameters[5] = { 2.4, 1.0 / 1.055,  0.055 / 1.055, 1.0 / 12.92, 0.04045 };
  cmsToneCurve *transferFunction = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);

  cmsHPROFILE profile = _create_lcms_profile("Display P3 RGB", "Display P3 RGB",
                                             &D65xyY, &P3_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_prophoto_rgb_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  cmsHPROFILE profile = _create_lcms_profile("Linear ProPhoto RGB", "Linear ProPhoto RGB",
                                             &D50xyY,  &ProPhoto_Primaries, transferFunction, TRUE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}

static cmsHPROFILE dt_colorspaces_create_linear_infrared_profile(void)
{
  cmsToneCurve *transferFunction = cmsBuildGamma(NULL, 1.0);

  // linear rgb with r and b swapped:
  cmsCIExyYTRIPLE BGR_Primaries = { sRGB_Primaries.Blue, sRGB_Primaries.Green, sRGB_Primaries.Red };

  cmsHPROFILE profile = _create_lcms_profile("Linear Infrared BGR", "darktable Linear Infrared BGR",
                                             &D65xyY, &BGR_Primaries, transferFunction, FALSE);

  cmsFreeToneCurve(transferFunction);

  return profile;
}







struct dt_colorspaces_color_profile_t *dt_colorspaces_new_image_profile(
    dt_colorspaces_color_profile_type_t type, cmsHPROFILE profile, gboolean owns_profile)
{
  // No role: a profile belonging to one image has no place in any combo box, so no
  // enumeration and no lookup can ever reach it.
  dt_colorspaces_color_profile_t *container = _create_profile(type, profile, "", 0);
  if(container) container->owns_profile = owns_profile;
  return container;
}

void dt_colorspaces_free_image_profile(struct dt_colorspaces_color_profile_t *profile)
{
  if(IS_NULL_PTR(profile)) return;
  // Only close what this container created; a borrowed profile belongs to the application list.
  if(profile->owns_profile) dt_colorspaces_cleanup_profile(profile->profile);
  dt_free(profile);
}


#if 0
static void dt_colorspaces_create_cmatrix(float cmatrix[4][3], float mat[3][3])
{
  // sRGB D65, the linear part:
  static const dt_colormatrix_t rgb_to_xyz = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.0f },
                                        { 0.2126729f, 0.7151522f, 0.0721750f, 0.0f },
                                        { 0.0193339f, 0.1191920f, 0.9503041f, 0.0f } };

  for(int c = 0; c < 3; c++)
  {
    for(int j = 0; j < 3; j++)
    {
      mat[c][j] = 0.0f;
      for(int k = 0; k < 3; k++)
      {
        mat[c][j] += rgb_to_xyz[k][j] * cmatrix[c][k];
      }
    }
  }
}
#endif

static cmsHPROFILE dt_colorspaces_create_xyzmatrix_profile(const float mat[3][3])
{
  // mat: cam -> xyz
  dt_aligned_pixel_t x, y;
  for(int k = 0; k < 3; k++)
  {
    const float norm = mat[0][k] + mat[1][k] + mat[2][k];
    x[k] = mat[0][k] / norm;
    y[k] = mat[1][k] / norm;
  }
  cmsCIExyYTRIPLE CameraPrimaries = { { x[0], y[0], 1.0 }, { x[1], y[1], 1.0 }, { x[2], y[2], 1.0 } };
  cmsHPROFILE profile;

  cmsCIExyY D65;
  cmsXYZ2xyY(&D65, &d65);

  cmsToneCurve *Gamma[3];
  Gamma[0] = Gamma[1] = Gamma[2] = cmsBuildGamma(NULL, 1.0);
  profile = cmsCreateRGBProfile(&D65, &CameraPrimaries, Gamma);
  cmsFreeToneCurve(Gamma[0]);
  if(IS_NULL_PTR(profile)) return NULL;

  cmsSetProfileVersion(profile, 2.1);
  cmsMLU *mlu0 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu0, "en", "US", "(dt internal)");
  cmsMLU *mlu1 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu1, "en", "US", "color matrix built-in");
  cmsMLU *mlu2 = cmsMLUalloc(NULL, 1);
  cmsMLUsetASCII(mlu2, "en", "US", "color matrix built-in");
  cmsWriteTag(profile, cmsSigDeviceMfgDescTag, mlu0);
  cmsWriteTag(profile, cmsSigDeviceModelDescTag, mlu1);
  // this will only be displayed when the embedded profile is read by for example GIMP
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu2);
  cmsMLUfree(mlu0);
  cmsMLUfree(mlu1);
  cmsMLUfree(mlu2);

  return profile;
}

cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float mat[3][3])
{
  // mat: xyz -> cam
  float imat[3][3];
  mat3inv((float *)imat, (float *)mat);
  return dt_colorspaces_create_xyzmatrix_profile(imat);
}

static cmsHPROFILE _ensure_rgb_profile(cmsHPROFILE profile)
{
  if(profile && cmsGetColorSpace(profile) == cmsSigGrayData)
  {
    cmsToneCurve *trc = cmsReadTag(profile, cmsSigGrayTRCTag);
    cmsCIEXYZ *wtpt = cmsReadTag(profile, cmsSigMediaWhitePointTag);
    cmsCIEXYZ *bkpt = cmsReadTag(profile, cmsSigMediaBlackPointTag);
    cmsCIEXYZ *chad = cmsReadTag(profile, cmsSigChromaticAdaptationTag);

    cmsMLU *cprt = cmsReadTag(profile, cmsSigCopyrightTag);
    cmsMLU *desc = cmsReadTag(profile, cmsSigProfileDescriptionTag);
    cmsMLU *dmnd = cmsReadTag(profile, cmsSigDeviceMfgDescTag);
    cmsMLU *dmdd = cmsReadTag(profile, cmsSigDeviceModelDescTag);

    cmsHPROFILE rgb_profile = cmsCreateProfilePlaceholder(0);

    cmsSetDeviceClass(rgb_profile, cmsSigDisplayClass);
    cmsSetColorSpace(rgb_profile, cmsSigRgbData);
    cmsSetPCS(rgb_profile, cmsSigXYZData);

    cmsWriteTag(rgb_profile, cmsSigCopyrightTag, cprt);
    cmsWriteTag(rgb_profile, cmsSigProfileDescriptionTag, desc);
    cmsWriteTag(rgb_profile, cmsSigDeviceMfgDescTag, dmnd);
    cmsWriteTag(rgb_profile, cmsSigDeviceModelDescTag, dmdd);

    cmsWriteTag(rgb_profile, cmsSigMediaBlackPointTag, bkpt);
    cmsWriteTag(rgb_profile, cmsSigMediaWhitePointTag, wtpt);
    cmsWriteTag(rgb_profile, cmsSigChromaticAdaptationTag, chad);
    cmsSetColorSpace(rgb_profile, cmsSigRgbData);
    cmsSetPCS(rgb_profile, cmsSigXYZData);

    // TODO: we still use prequantized primaries here, we will probably want to rework this
    // part to create a profile using cmsCreateRGBProfile() as done in _create_lcms_profile().
    cmsWriteTag(rgb_profile, cmsSigRedColorantTag, (void *)&Rec709_Primaries_Prequantized.Red);
    cmsWriteTag(rgb_profile, cmsSigGreenColorantTag, (void *)&Rec709_Primaries_Prequantized.Green);
    cmsWriteTag(rgb_profile, cmsSigBlueColorantTag, (void *)&Rec709_Primaries_Prequantized.Blue);

    cmsWriteTag(rgb_profile, cmsSigRedTRCTag, (void *)trc);
    cmsLinkTag(rgb_profile, cmsSigGreenTRCTag, cmsSigRedTRCTag);
    cmsLinkTag(rgb_profile, cmsSigBlueTRCTag, cmsSigRedTRCTag);

    cmsCloseProfile(profile);
    profile = rgb_profile;
  }

  return profile;
}

cmsHPROFILE dt_colorspaces_get_rgb_profile_from_mem(uint8_t *data, uint32_t size)
{
  cmsHPROFILE profile = _ensure_rgb_profile(cmsOpenProfileFromMem(data, size));

  return profile;
}

void dt_colorspaces_cleanup_profile(cmsHPROFILE p)
{
  if(IS_NULL_PTR(p)) return;
  cmsCloseProfile(p);
}

void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len)
{
  cmsUInt32Number size;
  gchar *buf = NULL;
  wchar_t *wbuf = NULL;
  gchar *utf8 = NULL;

  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, NULL, 0);
  if(size == 0) goto error;

  buf = (char *)calloc(size + 1, sizeof(char));
  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, language, country, buf, size);
  if(size == 0) goto error;

  // most unix like systems should work with this, but at least Windows doesn't
  if(sizeof(wchar_t) != 4 || g_utf8_validate(buf, -1, NULL))
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
  {
    wbuf = (wchar_t *)calloc(size + 1, sizeof(wchar_t));
    size = cmsGetProfileInfo(p, cmsInfoDescription, language, country, wbuf, sizeof(wchar_t) * size);
    if(size == 0) goto error;
    utf8 = g_ucs4_to_utf8((gunichar *)wbuf, -1, NULL, NULL, NULL);
    if(IS_NULL_PTR(utf8)) goto error;
    g_strlcpy(name, utf8, len);
  }

  dt_free(buf);
  dt_free(wbuf);
  dt_free(utf8);
  return;

error:
  if(buf)
    g_strlcpy(name, buf, len); // better a little weird than totally borked
  else
    *name = '\0'; // nothing to do here
  dt_free(buf);
  dt_free(wbuf);
  dt_free(utf8);
}

void rgb2hsl(const dt_aligned_pixel_t rgb, float *h, float *s, float *l)
{
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float pmax = fmaxf(r, fmax(g, b));
  const float pmin = fminf(r, fmin(g, b));
  const float delta = (pmax - pmin);

  float hv = 0, sv = 0, lv = (pmin + pmax) / 2.0;

  if(delta != 0.0f)
  {
    sv = lv < 0.5 ? delta / fmaxf(pmax + pmin, 1.52587890625e-05f)
                  : delta / fmaxf(2.0 - pmax - pmin, 1.52587890625e-05f);

    if(pmax == r)
      hv = (g - b) / delta;
    else if(pmax == g)
      hv = 2.0 + (b - r) / delta;
    else if(pmax == b)
      hv = 4.0 + (r - g) / delta;
    hv /= 6.0;
    if(hv < 0.0)
      hv += 1.0;
    else if(hv > 1.0)
      hv -= 1.0;
  }
  *h = hv;
  *s = sv;
  *l = lv;
}

// for efficiency, 'hue' must be pre-scaled to be in 0..6
static inline __attribute__((always_inline)) float hue2rgb(float m1, float m2, float hue)
{
  // compute the value for one of the RGB channels from the hue angle.
  // If 1 <= angle < 3, return m2; if 4 <= angle <= 6, return m1; otherwise, linearly interpolate between m1 and m2.
  if(hue < 1.0f)
    return (m1 + (m2 - m1) * hue);
  else if(hue < 3.0f)
    return m2;
  else
    return hue < 4.0f ? (m1 + (m2 - m1) * (4.0f - hue)) : m1;
}

void hsl2rgb(dt_aligned_pixel_t rgb, float h, float s, float l)
{
  float m1, m2;
  if(s == 0)
  {
    rgb[0] = rgb[1] = rgb[2] = l;
    return;
  }
  m2 = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  m1 = (2.0 * l - m2);
  h *= 6.0f;  // pre-scale hue angle
  rgb[0] = hue2rgb(m1, m2, h < 4.0f ? h + 2.0f : h - 4.0f);
  rgb[1] = hue2rgb(m1, m2, h);
  rgb[2] = hue2rgb(m1, m2, h > 2.0f ? h - 2.0f : h + 4.0f);
}

static dt_colorspaces_color_profile_t *_create_profile(dt_colorspaces_color_profile_type_t type,
                                                       cmsHPROFILE profile, const char *name,
                                                       dt_colorspaces_profile_role_t roles)
{
  dt_colorspaces_color_profile_t *prof;
  prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
  pthread_rwlock_init(&prof->lock, NULL);
  prof->type = type;
  g_strlcpy(prof->name, name, sizeof(prof->name));
  prof->profile = profile;
  prof->roles = roles;
  return prof;
}

// this function is basically thread safe, at least when not called on the global color profiles
/* cmsFLAGS_NOCACHE on every transform built here, and it is not an optimisation choice.
 *
 * lcms2 gives each transform a 1-pixel memoisation cache, ENABLED when flags are 0. That
 * cache is mutable state inside the transform, and lcms2 only sanctions sharing a
 * transform between threads when it is inhibited. These four are built once and then
 * driven by several threads at a time from the __OMP_PARALLEL_FOR__ loops below, so with
 * the cache left on they are a data race on lcms2's internals.
 *
 * iop/colorout.c already sets the flag on its proofing transform for the same reason. The
 * cache only pays on runs of identical adjacent pixels, which photographic data does not
 * have, so nothing is lost. */
static void _update_display_transforms(dt_colorspaces_t *self)
{
  if(self->transform_srgb_to_display) cmsDeleteTransform(self->transform_srgb_to_display);
  self->transform_srgb_to_display = NULL;

  if(self->transform_adobe_rgb_to_display) cmsDeleteTransform(self->transform_adobe_rgb_to_display);
  self->transform_adobe_rgb_to_display = NULL;

  if(self->transform_xyz_to_display) cmsDeleteTransform(self->transform_xyz_to_display);
  self->transform_xyz_to_display = NULL;

  if(self->transform_display_to_adobe_rgb) cmsDeleteTransform(self->transform_display_to_adobe_rgb);
  self->transform_display_to_adobe_rgb = NULL;

  const dt_colorspaces_color_profile_t *display_dt_profile = _get_profile(self, self->display_type,
                                                                          self->display_filename,
                                                                          DT_PROFILE_ROLE_MONITOR);
  if(IS_NULL_PTR(display_dt_profile)) return;
  cmsHPROFILE display_profile = display_dt_profile->profile;
  if(IS_NULL_PTR(display_profile)) return;

  self->transform_srgb_to_display = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_SRGB, "",
                                                                    DT_PROFILE_ROLE_MONITOR)->profile,
                                                       TYPE_RGBA_8,
                                                       display_profile,
                                                       TYPE_BGRA_8,
                                                       self->display_intent,
                                                       cmsFLAGS_NOCACHE);

  self->transform_xyz_to_display = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_XYZ, "",
                                                                    DT_PROFILE_ROLE_INPUT)->profile,
                                                       TYPE_XYZA_FLT,
                                                       display_profile,
                                                       TYPE_RGBA_FLT,
                                                       self->display_intent,
                                                       cmsFLAGS_NOCACHE);

  self->transform_adobe_rgb_to_display = cmsCreateTransform(_get_profile(self, DT_COLORSPACE_ADOBERGB, "",
                                                                         DT_PROFILE_ROLE_MONITOR)->profile,
                                                            TYPE_RGBA_8,
                                                            display_profile,
                                                            TYPE_BGRA_8,
                                                            self->display_intent,
                                                            cmsFLAGS_NOCACHE);

  self->transform_display_to_adobe_rgb = cmsCreateTransform(display_profile,
                                                            TYPE_BGRA_8,
                                                            _get_profile(self, DT_COLORSPACE_ADOBERGB, "",
                                                                         DT_PROFILE_ROLE_MONITOR)->profile,
                                                            TYPE_RGBA_8,
                                                            self->display_intent,
                                                            cmsFLAGS_NOCACHE);
}

// update cached transforms for color management of thumbnails
// caller holds _transforms_lock for writing
void dt_colorspaces_update_display_transforms()
{
  _update_display_transforms(dt_colorspaces_get_global());
}

/* ---------------------------------------------------------------------------
 * Display and soft-proofing settings.
 *
 * Seven fields the GUI writes and the pipeline reads: the display profile identity
 * and intent, the soft-proof identity and intent, and the proofing mode. They were
 * read and written through the global struct with no lock of any kind, one field at
 * a time — so a reader could see a new display_type paired with the previous
 * display_filename, and a 512-byte filename being g_strlcpy'd concurrently is a torn
 * string rather than a merely stale one.
 *
 * They cross the boundary only as a whole struct now, copied under one lock, so a
 * group can never be observed half-updated. `generation` advances on every accepted
 * change: a pipeline module can fold that single number into its hash instead of the
 * individual fields.
 *
 * LOCK ORDER, where both are involved: _transforms_lock OUTER, _settings_lock INNER.
 * The display setters need both, because changing the display profile also rebuilds
 * the four prepared transforms. Nothing takes them the other way round.
 * ------------------------------------------------------------------------- */

/* The module's single instance. It used to hang off darktable_t, which meant the whole
 * application could reach in and read, write and lock what is this module's private
 * business. It is file-static now: dt_colorprofiles_init() builds it, dt_colorprofiles_
 * cleanup() destroys it, and nothing outside this file has a way to name it.
 *
 * dt_colorspaces_get_global() survives ONLY as an internal shorthand while the remaining
 * consumers are migrated to the query API; it is no longer declared in the public header
 * and will disappear with the last of them. */
static dt_colorspaces_t *_colorprofiles = NULL;

/* The four prepared display transforms, and the byte cache the monitor refresh compares
 * against, are module-wide: they belong to no single profile, so they cannot be covered by
 * a per-entry lock. This is that lock.
 *
 * Separate from the per-entry locks on purpose. A thumbnail conversion holds this for the
 * duration of a whole image; a caller deriving from an unrelated profile must not queue
 * behind it, and a monitor-profile change must contend only with users of the display
 * entry and of these transforms -- not with everything that touches colour.
 *
 * LOCK ORDER where a writer needs both: the profile ENTRY lock first, then this one.
 * Readers take exactly one. */
static pthread_rwlock_t _transforms_lock = PTHREAD_RWLOCK_INITIALIZER;

static dt_colorspaces_t *_colorspaces_build(void);
static void _colorspaces_destroy(dt_colorspaces_t *self);

void dt_colorprofiles_init(void)
{
  if(!IS_NULL_PTR(_colorprofiles)) return;
  _colorprofiles = _colorspaces_build();
}

void dt_colorprofiles_cleanup(void)
{
  if(IS_NULL_PTR(_colorprofiles)) return;

  // the derived matrix/LUT memo is built from these profiles; it goes first
  dt_colorspaces_flush_profile_memo();

  _colorspaces_destroy(_colorprofiles);
  _colorprofiles = NULL;
}

/* Module-internal shorthand. It is static now: nothing outside this directory names the
 * module's state, which is the whole point of the exercise. */
static dt_colorspaces_t *dt_colorspaces_get_global(void)
{
  return _colorprofiles;
}

static pthread_rwlock_t _settings_lock = PTHREAD_RWLOCK_INITIALIZER;
static uint64_t _settings_generation = 0;

/* Both callers of _notify_profile_changed() release _transforms_lock before calling it, so this
 * takes no lock but its own -- and the settings lock is the INNER one either way. */
static void _advance_settings_generation(void)
{
  pthread_rwlock_wrlock(&_settings_lock);
  _settings_generation++;
  pthread_rwlock_unlock(&_settings_lock);
}

void dt_colorprofiles_get_settings(dt_colorprofiles_settings_t *const out)
{
  if(IS_NULL_PTR(out)) return;

  const dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_rdlock(&_settings_lock);
  out->mode = self->mode;
  out->display_type = self->display_type;
  g_strlcpy(out->display_filename, self->display_filename, sizeof(out->display_filename));
  out->display_intent = self->display_intent;
  out->softproof_type = self->softproof_type;
  g_strlcpy(out->softproof_filename, self->softproof_filename, sizeof(out->softproof_filename));
  out->softproof_intent = self->softproof_intent;
  out->generation = _settings_generation;
  pthread_rwlock_unlock(&_settings_lock);
}

/* Did (type, filename) differ from what is stored at (cur_type, cur_filename)?
 * Caller holds _settings_lock. filename is only meaningful for DT_COLORSPACE_FILE. */
static gboolean _profile_choice_differs(const dt_colorspaces_color_profile_type_t cur_type,
                                        const char *const cur_filename,
                                        const dt_colorspaces_color_profile_type_t type,
                                        const char *const filename)
{
  if(cur_type != type) return TRUE;
  if(type != DT_COLORSPACE_FILE) return FALSE;
  return strcmp(cur_filename, IS_NULL_PTR(filename) ? "" : filename) != 0;
}

gboolean dt_colorprofiles_set_display_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                     const char *const filename)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_wrlock(&_transforms_lock);
  pthread_rwlock_wrlock(&_settings_lock);

  const gboolean changed = _profile_choice_differs(self->display_type, self->display_filename, type, filename);
  if(changed)
  {
    self->display_type = type;
    g_strlcpy(self->display_filename, IS_NULL_PTR(filename) ? "" : filename, sizeof(self->display_filename));
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  // Still under the transforms lock: the new identity and the transforms built from it land together.
  if(changed) _update_display_transforms(self);
  pthread_rwlock_unlock(&_transforms_lock);

  return changed;
}

gboolean dt_colorprofiles_set_display_intent(const dt_iop_color_intent_t intent)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_wrlock(&_transforms_lock);
  pthread_rwlock_wrlock(&_settings_lock);

  const gboolean changed = (self->display_intent != intent);
  if(changed)
  {
    self->display_intent = intent;
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  if(changed) _update_display_transforms(self);
  pthread_rwlock_unlock(&_transforms_lock);

  return changed;
}

/* The soft-proof settings feed transforms that iop/colorout.c builds per commit_params;
 * nothing cached in this module derives from them, so no rebuild here. */
gboolean dt_colorprofiles_set_softproof_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                       const char *const filename)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_wrlock(&_settings_lock);
  const gboolean changed = _profile_choice_differs(self->softproof_type, self->softproof_filename, type, filename);
  if(changed)
  {
    self->softproof_type = type;
    g_strlcpy(self->softproof_filename, IS_NULL_PTR(filename) ? "" : filename, sizeof(self->softproof_filename));
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  return changed;
}

gboolean dt_colorprofiles_set_softproof_intent(const dt_iop_color_intent_t intent)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_wrlock(&_settings_lock);
  const gboolean changed = (self->softproof_intent != intent);
  if(changed)
  {
    self->softproof_intent = intent;
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  return changed;
}

gboolean dt_colorprofiles_set_mode(const dt_colorspaces_color_mode_t mode)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_wrlock(&_settings_lock);
  const gboolean changed = (self->mode != mode);
  if(changed)
  {
    self->mode = mode;
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  return changed;
}

dt_colorspaces_color_mode_t dt_colorprofiles_toggle_mode(const dt_colorspaces_color_mode_t mode)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  /* One locked read-modify-write. The two toggle buttons each open-coded
   * "read mode, compare, write the opposite", which is not atomic: two accelerator
   * presses in flight could both read DT_PROFILE_NORMAL and leave soft-proof and
   * gamut-check disagreeing about which of them is on. */
  pthread_rwlock_wrlock(&_settings_lock);
  const dt_colorspaces_color_mode_t now = (self->mode == mode) ? DT_PROFILE_NORMAL : mode;
  if(self->mode != now)
  {
    self->mode = now;
    _settings_generation++;
  }
  pthread_rwlock_unlock(&_settings_lock);

  return now;
}

void dt_colorspaces_transform_rgba_float_row(const cmsHTRANSFORM transform, const float *in, float *out,
                                             const int width)
{
  cmsDoTransform(transform, in, out, width);
}

__DT_CLONE_TARGETS__
void dt_colorspaces_transform_rgba_float_image(const cmsHTRANSFORM transform, const float *image_in, float *image_out,
                                               const int width, const int height)
{
  if(IS_NULL_PTR(transform) || IS_NULL_PTR(image_in) || IS_NULL_PTR(image_out) || width <= 0 || height <= 0) return;

  /* Share the aliased LCMS transform explicitly. Do not read it indirectly
   * through module state inside the loop body. */
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const float *const in = image_in + (size_t)y * width * 4;
    float *const out = image_out + (size_t)y * width * 4;
    dt_colorspaces_transform_rgba_float_row(transform, in, out, width);
  }
}

/* Byte swap + optional colour conversion over a whole 8-bit plane. Private: the only
 * callers are the prepared-transform entry points below, which own the locking. */
static void _transform_rgba8_to_bgra8(const cmsHTRANSFORM transform, const uint8_t *image_in, uint8_t *image_out,
                                      const int width, const int height)
{
  if(IS_NULL_PTR(image_in) || IS_NULL_PTR(image_out) || width <= 0 || height <= 0) return;

  /* Same threading rule as float transforms: pass an aliased transform handle
   * into the helper and share only that stable local state. */
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    /* NOT restrict: callers pass the same buffer for both (common/mipmap_cache.c converts
     * a thumbnail in place), so promising the compiler these do not overlap is a lie it is
     * entitled to vectorise on. */
    const uint8_t *const in = image_in + (size_t)y * width * 4u;
    uint8_t *const out = image_out + (size_t)y * width * 4u;

    if(transform)
    {
      // lcms2 permits in == out when the two formats have the same pixel size; both are 4 bytes.
      cmsDoTransform(transform, in, out, width);
      for(int x = 0; x < width; x++) out[4 * x + 3] = UINT8_MAX;
    }
    else
    {
      for(int x = 0; x < width; x++)
      {
        /* Read the whole pixel before writing any of it. Storing straight through --
         * out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; -- loses the red channel when
         * in == out, because the first store overwrites in[0] before the third reads it,
         * leaving R and B both holding the original blue. */
        const uint8_t r = in[4 * x + 0];
        const uint8_t g = in[4 * x + 1];
        const uint8_t b = in[4 * x + 2];

        out[4 * x + 0] = b;
        out[4 * x + 1] = g;
        out[4 * x + 2] = r;
        out[4 * x + 3] = UINT8_MAX;
      }
    }
  }
}

/* ---------------------------------------------------------------------------
 * Prepared display transforms.
 *
 * The four cached cmsHTRANSFORMs are rebuilt whenever the monitor profile or the
 * display intent changes, so a handle handed to a caller can be freed under it. The
 * functions below are therefore the only way to use them: each takes the read lock,
 * aliases the handle to a local, runs, and releases. No cmsHTRANSFORM crosses the
 * module boundary.
 *
 * Holding the read lock across the pixel work is deliberate and is what the previous
 * caller-side code already did — it is what keeps the handle alive for the duration
 * of the conversion.
 * ------------------------------------------------------------------------- */

void dt_colorprofiles_xyz_to_display(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t RGB)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_rdlock(&_transforms_lock);
  const cmsHTRANSFORM transform = self->transform_xyz_to_display;
  if(transform)
    cmsDoTransform(transform, XYZ, RGB, 1);
  pthread_rwlock_unlock(&_transforms_lock);

  /* No display profile resolved yet (startup, or a monitor whose profile could not be
   * read): fall back to sRGB rather than dereferencing NULL, which is what the two
   * open-coded copies of this function did. */
  if(IS_NULL_PTR(transform)) dt_XYZ_to_sRGB(XYZ, RGB);
}

gboolean dt_colorprofiles_rgba8_to_display_bgra8(const uint8_t *const in, uint8_t *const out,
                                                 const int width, const int height,
                                                 const dt_colorspaces_color_profile_type_t src_space)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  cmsHTRANSFORM transform = NULL;
  gboolean owned = FALSE;
  gboolean managed = TRUE;

  pthread_rwlock_rdlock(&_transforms_lock);

  if(src_space == DT_COLORSPACE_SRGB)
  {
    transform = self->transform_srgb_to_display;
  }
  else if(src_space == DT_COLORSPACE_ADOBERGB)
  {
    transform = self->transform_adobe_rgb_to_display;
  }
  else if(src_space == DT_COLORSPACE_DISPLAY)
  {
    // already in display space: pass through, swapping R <-> B (transform stays NULL)
  }
  else
  {
    const dt_colorspaces_color_profile_t *const from
        = _get_profile(self, src_space, "", DT_PROFILE_ROLE_MONITOR);
    const dt_colorspaces_color_profile_t *const to
        = _get_profile(self, DT_COLORSPACE_DISPLAY, "", DT_PROFILE_ROLE_MONITOR);

    /* Not every colorspace has a profile registered for the MONITOR role (a thumbnail
     * cached with an exotic tag). Fall back to the same passthrough as DT_COLORSPACE_DISPLAY
     * instead of dereferencing NULL in cmsCreateTransform(). */
    if(!IS_NULL_PTR(from) && !IS_NULL_PTR(to))
    {
      transform = cmsCreateTransform(from->profile, TYPE_RGBA_8, to->profile, TYPE_BGRA_8,
                                     INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
      owned = TRUE;
    }
  }

  /* DT_COLORSPACE_DISPLAY needs no transform and is not a failure; every other space
   * reaching the swap-only path means we could not colour-manage it. */
  if(IS_NULL_PTR(transform) && src_space != DT_COLORSPACE_DISPLAY) managed = FALSE;

  _transform_rgba8_to_bgra8(transform, in, out, width, height);

  if(owned && transform) cmsDeleteTransform(transform);
  pthread_rwlock_unlock(&_transforms_lock);

  return managed;
}

gboolean dt_colorprofiles_bgra8_to_adobergb_rgba8(const uint8_t *const in, uint8_t *const out,
                                                  const int width, const int height,
                                                  const dt_colorspaces_color_profile_type_t src_space)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  cmsHTRANSFORM transform = NULL;
  gboolean owned = FALSE;

  pthread_rwlock_rdlock(&_transforms_lock);

  if(src_space == DT_COLORSPACE_DISPLAY)
  {
    transform = self->transform_display_to_adobe_rgb;
  }
  else
  {
    const dt_colorspaces_color_profile_t *const from
        = _get_profile(self, src_space, "", DT_PROFILE_ROLE_MONITOR);
    const dt_colorspaces_color_profile_t *const to
        = _get_profile(self, DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_ROLE_MONITOR);
    if(!IS_NULL_PTR(from) && !IS_NULL_PTR(to))
    {
      transform = cmsCreateTransform(from->profile, TYPE_BGRA_8, to->profile, TYPE_RGBA_8,
                                     INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
      owned = TRUE;
    }
  }

  const gboolean managed = !IS_NULL_PTR(transform);

  /* With no transform this only swaps R <-> B, which is what turns the BGRA input back
   * into RGBA. The helper name says bgra8, but it is the same byte swap either way. */
  _transform_rgba8_to_bgra8(transform, in, out, width, height);

  if(owned && transform) cmsDeleteTransform(transform);
  pthread_rwlock_unlock(&_transforms_lock);

  return managed;
}

/* One row of a strided, packed-RGB(A) buffer: widen to RGBA8, convert, write back
 * narrowed and R <-> B swapped. `row_in`/`row_out` are width*4 scratch. */
static void _srgb_to_display_row(const cmsHTRANSFORM transform, uint8_t *const src, const int width,
                                 const int n_channels, const gboolean has_alpha,
                                 uint8_t *const row_in, uint8_t *const row_out)
{
  for(int x = 0; x < width; x++)
  {
    const int s = x * n_channels;
    const int d = x * 4;
    row_in[d + 0] = src[s + 0];
    row_in[d + 1] = src[s + 1];
    row_in[d + 2] = src[s + 2];
    row_in[d + 3] = has_alpha ? src[s + 3] : UINT8_MAX;
  }

  cmsDoTransform(transform, row_in, row_out, width);

  for(int x = 0; x < width; x++)
  {
    const int s = x * 4;
    const int d = x * n_channels;
    src[d + 0] = row_out[s + 2];
    src[d + 1] = row_out[s + 1];
    src[d + 2] = row_out[s + 0];
    if(has_alpha) src[d + 3] = row_out[s + 3];
  }
}

gboolean dt_colorprofiles_srgb_to_display_strided(uint8_t *const pixels, const int width, const int height,
                                                  const int rowstride, const int n_channels,
                                                  const gboolean has_alpha)
{
  if(IS_NULL_PTR(pixels) || width <= 0 || height <= 0 || n_channels < 3) return FALSE;

  dt_colorspaces_t *const self = dt_colorspaces_get_global();

  pthread_rwlock_rdlock(&_transforms_lock);
  const cmsHTRANSFORM transform = self->transform_srgb_to_display;
  if(IS_NULL_PTR(transform))
  {
    pthread_rwlock_unlock(&_transforms_lock);
    return FALSE;
  }

  /* Two width*4 scratch rows per thread, in ONE allocation made before the parallel
   * region: a per-thread allocation that could fail would put the worksharing loop
   * behind a condition some threads take and others do not, which hangs. */
  const size_t row_bytes = (size_t)width * 4u;
  const int nthreads = MAX(dt_get_num_openmp_threads(), 1);
  uint8_t *const scratch = g_try_malloc((size_t)nthreads * 2u * row_bytes);

  if(IS_NULL_PTR(scratch))
  {
    pthread_rwlock_unlock(&_transforms_lock);
    return FALSE;
  }

  __OMP_PARALLEL__()
  {
    uint8_t *const row_in = scratch + (size_t)2 * dt_get_thread_num() * row_bytes;
    uint8_t *const row_out = row_in + row_bytes;

    __OMP_FOR__()
    for(int y = 0; y < height; y++)
      _srgb_to_display_row(transform, pixels + (size_t)y * rowstride, width, n_channels, has_alpha,
                           row_in, row_out);
  }

  g_free(scratch);
  pthread_rwlock_unlock(&_transforms_lock);

  return TRUE;
}

// caller holds _transforms_lock for writing
static void _update_display_profile(guchar *tmp_data, gsize size, char *name, size_t name_size)
{
  dt_colorspaces_t *color_profiles = dt_colorspaces_get_global();

  dt_free(color_profiles->xprofile_data);
  color_profiles->xprofile_data = tmp_data;
  color_profiles->xprofile_size = size;

  cmsHPROFILE profile = cmsOpenProfileFromMem(tmp_data, size);
  if(profile)
  {
    for(GList *iter = color_profiles->profiles; iter; iter = g_list_next(iter))
    {
      dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
      if(p->type == DT_COLORSPACE_DISPLAY)
      {
        /* This is the ONE handle in the list that is replaced at runtime, and the reason
         * every entry carries a lock. Take this entry's WRITE lock across the swap, so a
         * caller that resolved this profile and is deriving from it under the read lock
         * cannot have the handle closed underneath it.
         *
         * The caller already holds _transforms_lock for writing; entry lock inside it,
         * per the lock-order note there. */
        pthread_rwlock_wrlock(&p->lock);

        if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
        p->profile = profile;

        pthread_rwlock_unlock(&p->lock);

        if(name)
          dt_colorspaces_get_profile_name(profile, "en", "US", name, name_size);

        // update cached transforms for color management of thumbnails
        dt_colorspaces_update_display_transforms();

        break;
      }
    }
  }
}


static void cms_error_handler(cmsContext ContextID, cmsUInt32Number ErrorCode, const char *text)
{
  dt_print(DT_DEBUG_COLORPROFILE, "[lcms2] error %d: %s\n", ErrorCode, text);
}

static gint _sort_profiles(gconstpointer a, gconstpointer b)
{
  const dt_colorspaces_color_profile_t *profile_a = (dt_colorspaces_color_profile_t *)a;
  const dt_colorspaces_color_profile_t *profile_b = (dt_colorspaces_color_profile_t *)b;

  gchar *name_a = g_utf8_casefold(profile_a->name, -1);
  gchar *name_b = g_utf8_casefold(profile_b->name, -1);

  gint result = g_strcmp0(name_a, name_b);

  dt_free(name_a);
  dt_free(name_b);

  return result;
}

static GList *load_profile_from_dir(const char *subdir)
{
  GList *temp_profiles = NULL;
  const gchar *d_name;
  char datadir[DT_PATH_MAX] = { 0 };
  char confdir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *lang = getenv("LANG");
  if(IS_NULL_PTR(lang)) lang = "en_US";

  char *dirname = g_build_filename(confdir, "color", subdir, NULL);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
  {
    dt_free(dirname);
    dirname = g_build_filename(datadir, "color", subdir, NULL);
  }
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      char *filename = g_build_filename(dirname, d_name, NULL);
      const char *cc = filename + strlen(filename);
      for(; *cc != '.' && cc > filename; cc--)
        ;
      if(!g_ascii_strcasecmp(cc, ".icc") || !g_ascii_strcasecmp(cc, ".icm"))
      {
        size_t end;
        char *icc_content = dt_read_file(filename, &end);
        if(IS_NULL_PTR(icc_content)) goto icc_loading_done;

        // TODO: add support for grayscale profiles, then remove _ensure_rgb_profile() from here
        cmsHPROFILE tmpprof = _ensure_rgb_profile(cmsOpenProfileFromMem(icc_content, sizeof(char) * end));
        if(tmpprof)
        {
          dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
          dt_colorspaces_get_profile_name(tmpprof, lang, lang + 3, prof->name, sizeof(prof->name));
          if(prof->name[0] == '\0')
            g_strlcpy(prof->name, _("(unknown name)"), sizeof(prof->name));
            
          g_strlcpy(prof->filename, filename, sizeof(prof->filename));
          prof->type = DT_COLORSPACE_FILE;
          prof->profile = tmpprof;
          // roles are assigned by the caller, after sorting, from the directory it came from
          prof->roles = 0;
          temp_profiles = g_list_prepend(temp_profiles, prof);
        }

icc_loading_done:
        dt_free(icc_content);
      }
      dt_free(filename);
    }
    g_dir_close(dir);
    temp_profiles = g_list_sort(temp_profiles, _sort_profiles);
  }
  dt_free(dirname);
  return temp_profiles;
}

static dt_colorspaces_t *_colorspaces_build(void)
{
  cmsSetLogErrorHandler(cms_error_handler);

  dt_colorspaces_t *res = (dt_colorspaces_t *)calloc(1, sizeof(dt_colorspaces_t));

  _compute_prequantized_primaries(&D65xyY, &Rec709_Primaries, &Rec709_Primaries_Prequantized);


  // init the category profile with NULL profile, the actual profile must be retrieved dynamically by the caller
  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_WORK, NULL, _("work profile"), 0));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_EXPORT, NULL, _("export profile"), 0));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SOFTPROOF, NULL, _("softproof profile"), 0));

  // init the display profile with srgb so some stupid code that runs before the real profile could be fetched has something to work with
  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_DISPLAY, dt_colorspaces_create_srgb_profile(),
                                     _("System display profile (recommended)"), DT_PROFILE_ROLE_MONITOR));

  // we want a v4 with parametric curve for input and a v2 with point trc for output
  // see http://ninedegreesbelow.com/photography/lcms-make-icc-profiles.html#profile-variants-and-versions
  // TODO: what about display?
  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB, dt_colorspaces_create_srgb_profile_v4(),
                                                     _("sRGB (e.g. JPG)"), DT_PROFILE_ROLE_INPUT));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_SRGB, dt_colorspaces_create_srgb_profile(),
                                                     _("sRGB"), DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(res->profiles,
                                _create_profile(DT_COLORSPACE_ADOBERGB, dt_colorspaces_create_adobergb_profile(),
                                                _("Adobe RGB (compatible)"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LIN_REC709, dt_colorspaces_create_linear_rec709_rgb_profile(),
                                     _("linear Rec709 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_REC709, dt_colorspaces_create_gamma_rec709_rgb_profile(),
                                     _("gamma Rec709 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_ITUR_BT1886, dt_colorspaces_create_itur_bt1886_rgb_profile(),
                                     _("ITU-R BT.1886 (gamma 2.4 Rec709)"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LIN_REC2020, dt_colorspaces_create_linear_rec2020_rgb_profile(),
                                     _("linear Rec2020 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_PQ_REC2020, dt_colorspaces_create_pq_rec2020_rgb_profile(),
                                     _("PQ Rec2020 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_HLG_REC2020, dt_colorspaces_create_hlg_rec2020_rgb_profile(),
                                     _("HLG Rec2020 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_PQ_P3, dt_colorspaces_create_pq_p3_rgb_profile(),
                                     _("PQ P3 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_HLG_P3, dt_colorspaces_create_hlg_p3_rgb_profile(),
                                     _("HLG P3 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_DISPLAY_P3, dt_colorspaces_create_display_p3_rgb_profile(),
                                     _("Display P3 RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
     res->profiles, _create_profile(DT_COLORSPACE_PROPHOTO_RGB, dt_colorspaces_create_linear_prophoto_rgb_profile(),
                                    _("linear ProPhoto RGB"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR | DT_PROFILE_ROLE_WORKING));

  res->profiles = g_list_append(
      res->profiles,
      _create_profile(DT_COLORSPACE_XYZ, dt_colorspaces_create_xyz_profile(), _("linear XYZ"),
                      DT_PROFILE_ROLE_INPUT | (dt_conf_get_bool("allow_lab_output") ? DT_PROFILE_ROLE_OUTPUT : 0)));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_LAB, dt_colorspaces_create_lab_profile(), _("Lab"),
                                     DT_PROFILE_ROLE_INPUT | (dt_conf_get_bool("allow_lab_output") ? DT_PROFILE_ROLE_OUTPUT : 0)));

  res->profiles = g_list_append(
      res->profiles, _create_profile(DT_COLORSPACE_INFRARED, dt_colorspaces_create_linear_infrared_profile(),
                                     _("linear infrared BGR"), DT_PROFILE_ROLE_INPUT));

  res->profiles
      = g_list_append(res->profiles, _create_profile(DT_COLORSPACE_BRG, dt_colorspaces_create_brg_profile(),
                                                     _("BRG (for testing)"), DT_PROFILE_ROLE_INPUT | DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR));

  // init display profile and softproof/gama checking from conf
  res->display_type = dt_conf_get_int("ui_last/color/display_type");
  res->softproof_type = dt_conf_get_int("ui_last/color/softproof_type");
  const char *tmp = dt_conf_get_string_const("ui_last/color/display_filename");
  g_strlcpy(res->display_filename, tmp, sizeof(res->display_filename));
  tmp = dt_conf_get_string_const("ui_last/color/softproof_filename");
  g_strlcpy(res->softproof_filename, tmp, sizeof(res->softproof_filename));
  res->display_intent = dt_conf_get_int("ui_last/color/display_intent");
  res->softproof_intent = dt_conf_get_int("ui_last/color/softproof_intent");
  res->mode = dt_conf_get_int("ui_last/color/mode");

  // sanity checks to ensure the profile filenames are present

  if((unsigned int)res->display_type >= DT_COLORSPACE_LAST
     || (res->display_type == DT_COLORSPACE_FILE
         && (!res->display_filename[0] || !g_file_test(res->display_filename, G_FILE_TEST_IS_REGULAR))))
    res->display_type = DT_COLORSPACE_DISPLAY;

  if((unsigned int)res->softproof_type >= DT_COLORSPACE_LAST
     || (res->softproof_type == DT_COLORSPACE_FILE
         && (!res->softproof_filename[0] || !g_file_test(res->softproof_filename, G_FILE_TEST_IS_REGULAR))))
    res->softproof_type = DT_COLORSPACE_SRGB;

  // temporary list of profiles to be added, we keep this separate to be able to sort it before adding
  GList *temp_profiles;

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  temp_profiles = load_profile_from_dir("in");
  for(GList *iter = temp_profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)iter->data;
    prof->roles = DT_PROFILE_ROLE_INPUT;
  }
  res->profiles = g_list_concat(res->profiles, temp_profiles);

  // read {conf,data}dir/color/out/*.icc
  temp_profiles = load_profile_from_dir("out");
  for(GList *iter = temp_profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)iter->data;
    // FIXME: do want to filter out non-RGB profiles for cases besides histogram profile? colorin is OK with RGB or XYZ, print is OK with anything which LCMS likes, otherwise things are more choosey
    const cmsColorSpaceSignature color_space = cmsGetColorSpace(prof->profile);
    // The histogram profile is used for histogram, clipping indicators and the global color picker.
    // Some of these also assume a matrix profile. LUT profiles don't make much sense in these applications
    // so filter out any profile that doesn't implement the relative colorimetric intent as a matrix (+ TRC).
    // For discussion, see e.g.
    // https://github.com/darktable-org/darktable/issues/7660#issuecomment-760143437
    // For the working profile we also require a matrix profile.
    const gboolean is_valid_matrix_profile
        = dt_colorspaces_get_matrix_from_output_profile(prof->profile, NULL, NULL, NULL, NULL, 0) == 0
          && dt_colorspaces_get_matrix_from_input_profile(prof->profile, NULL, NULL, NULL, NULL, 0) == 0;
    prof->roles = DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR;
    if(is_valid_matrix_profile)
    {
      prof->roles |= DT_PROFILE_ROLE_WORKING;
    }
    else
    {
      dt_print(DT_DEBUG_DEV,
               "output profile `%s' color space `%c%c%c%c' not supported for work or histogram profile\n",
               prof->name, (char)(color_space >> 24), (char)(color_space >> 16), (char)(color_space >> 8),
               (char)(color_space));
    }
  }
  res->profiles = g_list_concat(res->profiles, temp_profiles);


  if((unsigned int)res->mode > DT_PROFILE_GAMUTCHECK) res->mode = DT_PROFILE_NORMAL;

  _update_display_transforms(res);

  return res;
}

static void _colorspaces_destroy(dt_colorspaces_t *self)
{
  // remember display profile and softproof/gama checking from conf
  dt_conf_set_int("ui_last/color/display_type", self->display_type);
  dt_conf_set_int("ui_last/color/softproof_type", self->softproof_type);
  dt_conf_set_string("ui_last/color/display_filename", self->display_filename);
  dt_conf_set_string("ui_last/color/softproof_filename", self->softproof_filename);
  dt_conf_set_int("ui_last/color/display_intent", self->display_intent);
  dt_conf_set_int("ui_last/color/softproof_intent", self->softproof_intent);
  dt_conf_set_int("ui_last/color/mode", self->mode);

  if(self->transform_srgb_to_display) cmsDeleteTransform(self->transform_srgb_to_display);
  self->transform_srgb_to_display = NULL;

  if(self->transform_adobe_rgb_to_display) cmsDeleteTransform(self->transform_adobe_rgb_to_display);
  self->transform_adobe_rgb_to_display = NULL;

  if(self->transform_display_to_adobe_rgb) cmsDeleteTransform(self->transform_display_to_adobe_rgb);
  self->transform_display_to_adobe_rgb = NULL;

  if(self->transform_xyz_to_display) cmsDeleteTransform(self->transform_xyz_to_display);
  self->transform_xyz_to_display = NULL;

  for(GList *iter = self->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    if(p)
    {
      dt_colorspaces_cleanup_profile(p->profile);
      pthread_rwlock_destroy(&p->lock);
    }
  }
  g_list_free_full(self->profiles, dt_free_gpointer);
  self->profiles = NULL;

  dt_free(self->colord_profile_file);
  dt_free(self->xprofile_data);

  dt_free(self);
}

const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type,
                                    const char *filename)
{
  switch (type)
  {
    case DT_COLORSPACE_NONE:
      return NULL;
    case DT_COLORSPACE_FILE:
      return filename;
    case DT_COLORSPACE_SRGB:
      return _("sRGB");
    case DT_COLORSPACE_ADOBERGB:
      return _("Adobe RGB (compatible)");
    case DT_COLORSPACE_LIN_REC709:
      return _("linear Rec709 RGB");
    case DT_COLORSPACE_LIN_REC2020:
      return _("linear Rec2020 RGB");
    case DT_COLORSPACE_XYZ:
      return _("linear XYZ");
    case DT_COLORSPACE_LAB:
      return _("Lab");
    case DT_COLORSPACE_INFRARED:
      return _("linear infrared BGR");
    case DT_COLORSPACE_DISPLAY:
      return _("System display profile (recommended)");
    case DT_COLORSPACE_EMBEDDED_ICC:
      return _("embedded ICC profile");
    case DT_COLORSPACE_EMBEDDED_MATRIX:
      return _("embedded matrix");
    case DT_COLORSPACE_STANDARD_MATRIX:
      return _("standard color matrix");
    case DT_COLORSPACE_ENHANCED_MATRIX:
      return _("enhanced color matrix");
    case DT_COLORSPACE_VENDOR_MATRIX:
      return _("vendor color matrix");
    case DT_COLORSPACE_ALTERNATE_MATRIX:
      return _("alternate color matrix");
    case DT_COLORSPACE_BRG:
      return _("BRG (experimental)");
    case DT_COLORSPACE_EXPORT:
      return _("export profile");
    case DT_COLORSPACE_SOFTPROOF:
      return _("softproof profile");
    case DT_COLORSPACE_WORK:
      return _("work profile");
    case DT_COLORSPACE_DISPLAY2:
      return _("Not used. Shouldn't be here.");
    case DT_COLORSPACE_REC709:
      return _("Rec709 RGB");
    case DT_COLORSPACE_PROPHOTO_RGB:
      return _("linear ProPhoto RGB");
    case DT_COLORSPACE_PQ_REC2020:
      return _("PQ Rec2020");
    case DT_COLORSPACE_HLG_REC2020:
      return _("HLG Rec2020");
    case DT_COLORSPACE_PQ_P3:
      return _("PQ P3");
    case DT_COLORSPACE_HLG_P3:
      return _("HLG P3");
    case DT_COLORSPACE_DISPLAY_P3:
      return _("Display P3");
    case DT_COLORSPACE_ITUR_BT1886:
      return _("ITU-R BT.1886");
    case DT_COLORSPACE_LAST:
      break;
  }

  return NULL;
}

#ifdef USE_COLORDGTK
static void dt_colorspaces_get_display_profile_colord_callback(GObject *source, GAsyncResult *res, gpointer user_data)
{
  dt_colorspaces_t *color_profiles = dt_colorspaces_get_global();

  /* Writer: takes the DISPLAY entry's own lock (its cmsHPROFILE is replaced) and then the
   * transforms lock (all four are rebuilt from it). Entry before transforms -- see the
   * lock-order note on _transforms_lock. */
  pthread_rwlock_wrlock(&_transforms_lock);

  int profile_changed = 0;
  CdWindow *window = CD_WINDOW(source);
  GError *error = NULL;
  CdProfile *profile = cd_window_get_profile_finish(window, res, &error);
  if(IS_NULL_PTR(error) && !IS_NULL_PTR(profile))
  {
    const gchar *filename = cd_profile_get_filename(profile);
    if(filename)
    {
      if(g_strcmp0(filename, color_profiles->colord_profile_file))
      {
        /* the profile has changed (either because the user changed the colord settings or because we are on a
         * different screen now) */
        // update the cached colord profile file
        dt_free(color_profiles->colord_profile_file);
        color_profiles->colord_profile_file = g_strdup(filename);

        // read the file
        guchar *tmp_data = NULL;
        gsize size;
        g_file_get_contents(filename, (gchar **)&tmp_data, &size, NULL);
        profile_changed = size > 0 && (color_profiles->xprofile_size != size
                                        || memcmp(color_profiles->xprofile_data, tmp_data, size) != 0);

        if(profile_changed)
        {
          _update_display_profile(tmp_data, size, NULL, 0);
          dt_print(DT_DEBUG_CONTROL,
                   "[color profile] colord gave us a new screen profile: '%s' (size: %" G_GSIZE_FORMAT ")\n", filename, size);
        }
        else
        {
          dt_free(tmp_data);
        }
      }
    }
  }
  if(profile) g_object_unref(profile);
  g_object_unref(window);

  pthread_rwlock_unlock(&_transforms_lock);

  if(profile_changed) _notify_profile_changed();
}
#endif

#if defined GDK_WINDOWING_X11
#endif

// Get the display ICC profile of the monitor associated with the widget.
// For X display, uses the ICC profile specifications version 0.2 from
// http://burtonini.com/blog/computers/xicc
// Based on code from Gimp's modules/cdisplay_lcms.c
void dt_colorspaces_set_display_profile(const dt_colorspaces_color_profile_type_t profile_type,
                                       GtkWidget *widget)
{
  if(IS_NULL_PTR(widget)) return;

  dt_colorspaces_t *color_profiles = dt_colorspaces_get_global();

  // make sure that no one gets a broken profile
  // FIXME: benchmark if the try is really needed when moving/resizing the window. Maybe we can just lock it
  // and block
  /* trywrlock, not wrlock, and this is load-bearing: this runs from a configure-event
   * handler, i.e. on every tick of a window drag. Blocking the GUI thread behind a
   * thumbnail conversion would stutter the drag, so a refresh that cannot get the lock is
   * dropped and the next event retries. */
  if(pthread_rwlock_trywrlock(&_transforms_lock))
    return; // we are already updating the profile. Or someone is reading right now. Too bad we can't
            // distinguish that. Whatever ...

  guint8 *buffer = NULL;
  gint buffer_size = 0;
  gchar *profile_source = NULL;

#if defined GDK_WINDOWING_X11

  // we will use the xatom no matter what configured when compiled without colord
  gboolean use_xatom = TRUE;
#if defined USE_COLORDGTK
  gboolean use_colord = TRUE;
  const char *display_profile_source = dt_conf_get_string_const("ui_last/display_profile_source");

  if(display_profile_source)
  {
    if(!strcmp(display_profile_source, "xatom"))
      use_colord = FALSE;
    else if(!strcmp(display_profile_source, "colord"))
      use_xatom = FALSE;
  }
#endif

  /* let's have a look at the xatom, just in case ... */
  if(use_xatom)
    dt_display_profile_read(widget, &buffer, &buffer_size, &profile_source);

#ifdef USE_COLORDGTK
  /* also try to get the profile from colord. this will set the value asynchronously!
   * Stays here rather than in system/: the callback writes this module's own state. */
  if(use_colord)
  {
    CdWindow *window = cd_window_new();
    cd_window_get_profile(window, widget, NULL, dt_colorspaces_get_display_profile_colord_callback,
                          GINT_TO_POINTER(profile_type));
  }
#endif

#else // every non-X11 platform: no xatom/colord choice to make
  dt_display_profile_read(widget, &buffer, &buffer_size, &profile_source);
#endif

  int profile_changed = buffer_size > 0 && (color_profiles->xprofile_size != buffer_size
                              || memcmp(color_profiles->xprofile_data, buffer, buffer_size) != 0);

  if(profile_changed)
  {
    char name[512] = { 0 };
    _update_display_profile(buffer, buffer_size, name, sizeof(name));
    dt_print(DT_DEBUG_CONTROL, "[color profile] we got a new screen profile `%s' from the %s (size: %d)\n",
             *name ? name : "(unknown)", profile_source, buffer_size);
  }
  else
  {
    dt_free(buffer);
  }
  pthread_rwlock_unlock(&_transforms_lock);
  if(profile_changed) _notify_profile_changed();
  dt_free(profile_source);
}

static gboolean _colorspaces_is_base_name(const char *profile)
{
  const char *f = profile;
  while(*f != '\0')
  {
    if(*f == '/' || *f == '\\') return FALSE;
    f++;
  }
  return TRUE;
}

static const char *_colorspaces_get_base_name(const char *profile)
{
  const char* f = profile + strlen(profile);
  for (; f >= profile; f--)
  {
    if(*f == '/' || *f == '\\')
      return ++f;   // path separator found - return the filename only, without the leading separator
  }
  return f;         // no separator found - consider profile_name to be a "base" one
}

gboolean dt_colorspaces_is_profile_equal(const char *fullname, const char *filename)
{
  // for backward compatibility we need to also ensure that we check
  // for basename, indeed filename parameter may be in fact just a
  // basename as recorded in an iop.
  return _colorspaces_is_base_name(filename)
    ? !strcmp(_colorspaces_get_base_name(fullname), filename)
    : !strcmp(_colorspaces_get_base_name(fullname), _colorspaces_get_base_name(filename));
}


static const dt_colorspaces_color_profile_t *_get_profile(dt_colorspaces_t *self,
                                                          dt_colorspaces_color_profile_type_t type,
                                                          const char *filename,
                                                          dt_colorspaces_profile_role_t role)
{
  for(GList *iter = self->profiles; iter; iter = g_list_next(iter))
  {
    dt_colorspaces_color_profile_t *p = (dt_colorspaces_color_profile_t *)iter->data;
    if((p->roles & role)
       && (p->type == type
           && (type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(p->filename, filename))))
    {
      return p;
    }
  }

  return NULL;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type,
                                                                 const char *filename,
                                                                 dt_colorspaces_profile_role_t role)
{
  return _get_profile(dt_colorspaces_get_global(), type, filename, role);
}


/* ---------------------------------------------------------------------------
 * CRUDE: the metadata half of the module interface.
 *
 * Everything here answers a question ABOUT a profile -- which ones exist for a use,
 * what is this one called, where does it sit in a combo box -- and answers it with
 * plain values. No cmsHPROFILE crosses this boundary and no caller iterates the list.
 *
 * These deliberately take no lock. The list is built once by init and never appended
 * to again; the ONE datum that mutates at runtime is the DT_COLORSPACE_DISPLAY entry's
 * cmsHPROFILE, which _update_display_profile() replaces in place and which nothing
 * here reads. Adding a lock around these would put one on 39 call sites that are
 * lock-free today, to protect fields nobody writes.
 *
 * THE ROLE PREDICATE. `role` is mandatory and is not a nicety:
 * DT_COLORSPACE_SRGB is registered twice -- a v4 parametric-curve profile valid only
 * as input, and a v2 point-TRC profile valid for out/display/category/work -- and the
 * two are distinguished by nothing else. A multi-bit mask resolves to the first match
 * in registration order, which for sRGB is the v4 input entry. The index-valued calls
 * therefore REQUIRE a single bit: an index means nothing outside the enumeration that
 * produced it, and an index taken from INPUT|OUTPUT equals neither menu's row number.
 * ------------------------------------------------------------------------- */

/* Exactly the predicate _get_profile() applies, so enumeration and lookup can never disagree
 * about what a role contains. An entry with an empty mask -- the three category entries, and
 * every per-image container -- is unreachable by either. */
static gboolean _entry_serves(const dt_colorspaces_color_profile_t *const p,
                              const dt_colorspaces_profile_role_t role)
{
  return (p->roles & role) != 0;
}

static gboolean _is_single_role(const dt_colorspaces_profile_role_t role)
{
  return role != 0 && (role & (role - 1)) == 0;
}

static void _fill_desc(const dt_colorspaces_color_profile_t *const p, dt_colorprofile_desc_t *const out)
{
  out->type = p->type;
  g_strlcpy(out->filename, p->filename, sizeof(out->filename));
  g_strlcpy(out->name, p->name, sizeof(out->name));
}

/* --- LOCK: pin ONE profile's handle for the span of a derivation -----------
 *
 * Per profile, not per module. Only the DT_COLORSPACE_DISPLAY entry's cmsHPROFILE is
 * actually replaced at runtime -- _update_display_profile() closes and swaps it on every
 * window move or resize that lands on a different monitor -- but the lock lives on every
 * entry rather than on that one, so the next entry that becomes mutable does not
 * reintroduce the hazard by default.
 *
 * Why not one module-wide lock: several pipelines and the GUI derive from profiles
 * concurrently. A module-wide reader held across a whole thumbnail conversion queues a
 * monitor-profile change behind work that has nothing to do with the display profile, and
 * a queued writer in turn blocks every unrelated reader. Per entry, a monitor change
 * contends only with users of the display entry.
 *
 * The entry POINTER is stable for the process: entries are allocated at init and never
 * freed or moved, only their ->profile is swapped. So resolving a profile and then locking
 * it is sound -- but read ->profile only AFTER taking the lock. */
void dt_colorspaces_lock_profile(const dt_colorspaces_color_profile_t *const profile)
{
  if(IS_NULL_PTR(profile)) return;
  pthread_rwlock_rdlock((pthread_rwlock_t *)&profile->lock);
}

void dt_colorspaces_unlock_profile(const dt_colorspaces_color_profile_t *const profile)
{
  if(IS_NULL_PTR(profile)) return;
  pthread_rwlock_unlock((pthread_rwlock_t *)&profile->lock);
}

size_t dt_colorspaces_enumerate_profiles(const dt_colorspaces_profile_role_t role,
                                         dt_colorprofile_desc_t **out)
{
  if(IS_NULL_PTR(out)) return 0;
  *out = NULL;

  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  if(IS_NULL_PTR(self)) return 0;

  size_t count = 0;
  for(const GList *l = self->profiles; l; l = g_list_next(l))
    if(_entry_serves((const dt_colorspaces_color_profile_t *)l->data, role)) count++;

  if(count == 0) return 0;

  dt_colorprofile_desc_t *list = dt_alloc_align(count * sizeof(dt_colorprofile_desc_t));
  if(IS_NULL_PTR(list)) return 0;

  size_t k = 0;
  for(const GList *l = self->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *const p = (const dt_colorspaces_color_profile_t *)l->data;
    if(_entry_serves(p, role)) _fill_desc(p, &list[k++]);
  }

  *out = list;
  return count;
}

int dt_colorspaces_profile_index(const dt_colorspaces_profile_role_t role,
                                 const dt_colorspaces_color_profile_type_t type,
                                 const char *const filename)
{
  if(!_is_single_role(role)) return -1;

  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  if(IS_NULL_PTR(self)) return -1;

  int index = 0;
  for(const GList *l = self->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *const p = (const dt_colorspaces_color_profile_t *)l->data;
    if(!_entry_serves(p, role)) continue;

    if(p->type == type
       && (type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(p->filename, filename)))
      return index;

    index++;
  }

  return -1;
}

gboolean dt_colorspaces_profile_at(const dt_colorspaces_profile_role_t role,
                                   const int index,
                                   dt_colorprofile_desc_t *const out)
{
  if(!_is_single_role(role) || index < 0 || IS_NULL_PTR(out)) return FALSE;

  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  if(IS_NULL_PTR(self)) return FALSE;

  int k = 0;
  for(const GList *l = self->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *const p = (const dt_colorspaces_color_profile_t *)l->data;
    if(!_entry_serves(p, role)) continue;
    if(k == index)
    {
      _fill_desc(p, out);
      return TRUE;
    }
    k++;
  }

  return FALSE;
}

gboolean dt_colorspaces_profile_exists(const dt_colorspaces_profile_role_t role,
                                       const dt_colorspaces_color_profile_type_t type,
                                       const char *const filename)
{
  dt_colorspaces_t *const self = dt_colorspaces_get_global();
  if(IS_NULL_PTR(self)) return FALSE;

  for(const GList *l = self->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *const p = (const dt_colorspaces_color_profile_t *)l->data;
    if(!_entry_serves(p, role)) continue;
    if(p->type == type
       && (type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(p->filename, filename)))
      return TRUE;
  }

  return FALSE;
}

// Copied from dcraw's pseudoinverse()
__DT_CLONE_TARGETS__
static void dt_colorspaces_pseudoinverse(double (*in)[3], double (*out)[3], int size)
{
  double work[3][6];

  for(int i = 0; i < 3; i++) {
    for(int j = 0; j < 6; j++)
      work[i][j] = j == i+3;
    for(int j = 0; j < 3; j++)
      for(int k = 0; k < size; k++)
        work[i][j] += in[k][i] * in[k][j];
  }
  for(int i = 0; i < 3; i++) {
    double num = work[i][i];
    for(int j = 0; j < 6; j++)
      work[i][j] /= num;
    for(int k = 0; k < 3; k++) {
      if(k==i) continue;
      num = work[k][i];
      for(int j = 0; j < 6; j++)
        work[k][j] -= work[i][j] * num;
    }
  }
  for(int i = 0; i < size; i++)
    for(int j = 0; j < 3; j++)
    {
      out[i][j] = 0.0f;
      for(int k = 0; k < 3; k++)
        out[i][j] += work[j][k+3] * in[i][k];
    }
}

int dt_colorspaces_conversion_matrices_xyz(const float adobe_XYZ_to_CAM[4][3], float in_XYZ_to_CAM[9], double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4])
{
  if(!isnan(in_XYZ_to_CAM[0]))
  {
    for(int i = 0; i < 9; i++)
        XYZ_to_CAM[i/3][i%3] = (double) in_XYZ_to_CAM[i];
    for(int i = 0; i < 3; i++)
      XYZ_to_CAM[3][i] = 0.0f;
  }
  else
  {
    if(isnan(adobe_XYZ_to_CAM[0][0]))
      return FALSE;

    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 3; j++)
        XYZ_to_CAM[i][j] = (double)adobe_XYZ_to_CAM[i][j];
  }

  // Invert the matrix
  double inverse[4][3];
  dt_colorspaces_pseudoinverse (XYZ_to_CAM, inverse, 4);
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 4; j++)
      CAM_to_XYZ[i][j] = inverse[j][i];

  return TRUE;
}

// Converted from dcraw's cam_xyz_coeff()
// Build the camera RGB to sRGB conversion matrix
__DT_CLONE_TARGETS__
int dt_colorspaces_conversion_matrices_rgb(const float adobe_XYZ_to_CAM[4][3],
                                           double out_RGB_to_CAM[4][3], double out_CAM_to_RGB[3][4],
                                           const float *embedded_matrix,
                                           double mul[4])
{
  double RGB_to_CAM[4][3];

  float XYZ_to_CAM[4][3];
  XYZ_to_CAM[0][0] = NAN;

  if(IS_NULL_PTR(embedded_matrix) || isnan(embedded_matrix[0]))
  {
    for(int k=0; k<4; k++)
      for(int i=0; i<3; i++)
        XYZ_to_CAM[k][i] = adobe_XYZ_to_CAM[k][i];
  }
  else
  {
    // keep in sync with reload_defaults from colorin.c
    // embedded matrix is used with higher priority than standard one
    XYZ_to_CAM[0][0] = embedded_matrix[0];
    XYZ_to_CAM[0][1] = embedded_matrix[1];
    XYZ_to_CAM[0][2] = embedded_matrix[2];

    XYZ_to_CAM[1][0] = embedded_matrix[3];
    XYZ_to_CAM[1][1] = embedded_matrix[4];
    XYZ_to_CAM[1][2] = embedded_matrix[5];

    XYZ_to_CAM[2][0] = embedded_matrix[6];
    XYZ_to_CAM[2][1] = embedded_matrix[7];
    XYZ_to_CAM[2][2] = embedded_matrix[8];
  }

  if(isnan(XYZ_to_CAM[0][0]))
    return FALSE;

  const double RGB_to_XYZ[3][3] = {
  // sRGB D65
    { 0.412453, 0.357580, 0.180423 },
    { 0.212671, 0.715160, 0.072169 },
    { 0.019334, 0.119193, 0.950227 },
  };

  // Multiply RGB matrix
  for(int i = 0; i < 4; i++)
    for(int j = 0; j < 3; j++)
    {
      RGB_to_CAM[i][j] = 0.0f;
      for(int k = 0; k < 3; k++)
        RGB_to_CAM[i][j] += XYZ_to_CAM[i][k] * RGB_to_XYZ[k][j];
    }

  // Normalize cam_rgb so that cam_rgb * (1,1,1) is (1,1,1,1)
  for(int i = 0; i < 4; i++) {
    double num = 0.0f;
    for(int j = 0; j < 3; j++)
      num += RGB_to_CAM[i][j];
    for(int j = 0; j < 3; j++)
       RGB_to_CAM[i][j] /= num;
    if(mul) mul[i] = 1.0f / num;
  }

  if(out_RGB_to_CAM)
    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 3; j++)
        out_RGB_to_CAM[i][j] = RGB_to_CAM[i][j];

  if(out_CAM_to_RGB)
  {
    // Invert the matrix
    double inverse[4][3];
    dt_colorspaces_pseudoinverse (RGB_to_CAM, inverse, 4);
    for(int i = 0; i < 3; i++)
      for(int j = 0; j < 4; j++)
        out_CAM_to_RGB[i][j] = inverse[j][i];
  }

  return TRUE;
}

void dt_colorspaces_cygm_apply_coeffs_to_rgb(float *out, const float *in, int num, double RGB_to_CAM[4][3],
                                             double CAM_to_RGB[3][4], dt_aligned_pixel_t coeffs)
{
  // Create the CAM to RGB with applied WB matrix
  double CAM_to_RGB_WB[3][4];
  for (int a=0; a<3; a++)
    for (int b=0; b<4; b++)
      CAM_to_RGB_WB[a][b] = CAM_to_RGB[a][b] * coeffs[b];

  // Create the RGB->RGB+WB matrix
  double RGB_to_RGB_WB[3][3];
  for (int a=0; a<3; a++)
    for (int b=0; b<3; b++) {
      RGB_to_RGB_WB[a][b] = 0.0f;
      for (int c=0; c<4; c++)
        RGB_to_RGB_WB[a][b] += CAM_to_RGB_WB[a][c] * RGB_to_CAM[c][b];
    }
  __OMP_PARALLEL_FOR__()
  for(int i = 0; i < num; i++)
  {
    const float *inpos = &in[i*4];
    float *outpos = &out[i*4];
    outpos[0]=outpos[1]=outpos[2] = 0.0f;
    for (int a=0; a<3; a++)
      for (int b=0; b<3; b++)
        outpos[a] += RGB_to_RGB_WB[a][b] * inpos[b];
  }
}

__DT_CLONE_TARGETS__
void dt_colorspaces_cygm_to_rgb(float *out, int num, double CAM_to_RGB[3][4])
{
  __OMP_PARALLEL_FOR__()
  for(int i = 0; i < num; i++)
  {
    float *in = &out[i*4];
    dt_aligned_pixel_t o = {0.0f,0.0f,0.0f};
    for(int c = 0; c < 3; c++)
      for(int k = 0; k < 4; k++)
        o[c] += CAM_to_RGB[c][k] * in[k];
    for(int c = 0; c < 3; c++)
      in[c] = o[c];
  }
}

void dt_colorspaces_rgb_to_cygm(float *out, int num, double RGB_to_CAM[4][3])
{
  __OMP_PARALLEL_FOR__()
  for(int i = 0; i < num; i++)
  {
    float *in = &out[i*3];
    dt_aligned_pixel_t o = {0.0f,0.0f,0.0f,0.0f};
    for(int c = 0; c < 4; c++)
      for(int k = 0; k < 3; k++)
        o[c] += RGB_to_CAM[c][k] * in[k];
    for(int c = 0; c < 4; c++)
      in[c] = o[c];
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
