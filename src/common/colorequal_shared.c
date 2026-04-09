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
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/colorequal_shared.h"

#include "common/chromatic_adaptation.h"
#include "common/curve_tools.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/interpolation.h"
#include "common/splines.h"

#include <float.h>
#include <math.h>
#include <string.h>

static inline void _xyz_d50_to_profile_rgb(const dt_aligned_pixel_t XYZ_D50,
                                           const dt_iop_order_iccprofile_info_t *profile,
                                           dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t linear_rgb = { 0.f };
  dt_apply_transposed_color_matrix(XYZ_D50, profile->matrix_out_transposed, linear_rgb);

  if(profile->nonlinearlut)
    _apply_trc(linear_rgb, RGB, profile->lut_out, profile->unbounded_coeffs_out, profile->lutsize);
  else
    for_each_channel(c, aligned(RGB, linear_rgb)) RGB[c] = linear_rgb[c];
}

static inline void _xyz_d50_to_profile_linear_rgb(const dt_aligned_pixel_t XYZ_D50,
                                                  const dt_iop_order_iccprofile_info_t *profile,
                                                  dt_aligned_pixel_t RGB)
{
  if(profile)
    dt_apply_transposed_color_matrix(XYZ_D50, profile->matrix_out_transposed, RGB);
  else
    dt_XYZ_to_linearRGB(XYZ_D50, RGB);
}

static inline void _dt_ucs_hsb_to_preview_rgb_unclamped(const dt_aligned_pixel_t HSB, const float white,
                                                        dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t XYZ_D65 = { 0.f };
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_UCS_HSB_to_XYZ(HSB, white, XYZ_D65);
  XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
  dt_XYZ_to_sRGB(XYZ_D50, RGB);
}

static inline void _profile_linear_rgb_to_display_rgb_normalized(const dt_aligned_pixel_t linear_rgb_in,
                                                                 const dt_iop_order_iccprofile_info_t *display_profile,
                                                                 dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t linear_rgb = { linear_rgb_in[0], linear_rgb_in[1], linear_rgb_in[2], 0.f };

  /**
   * The GUI gradients are hue/chroma visual guides, not photometric previews.
   * Keep in-gamut display colors untouched and only compress actual
   * out-of-range linear RGB back into the unit cube before applying the
   * display TRC. This keeps color equalizer and color primaries on the exact
   * same display-rendering path.
   */
  const float max_rgb = fmaxf(linear_rgb[0], fmaxf(linear_rgb[1], linear_rgb[2]));
  if(linear_rgb[0] > 1.f || linear_rgb[1] > 1.f || linear_rgb[2] > 1.f)
    for_each_channel(c, aligned(linear_rgb)) linear_rgb[c] /= max_rgb;

  if(display_profile && display_profile->nonlinearlut)
    _apply_trc(linear_rgb, RGB, display_profile->lut_out, display_profile->unbounded_coeffs_out,
               display_profile->lutsize);
  else if(display_profile)
    for_each_channel(c, aligned(RGB, linear_rgb)) RGB[c] = linear_rgb[c];
  else
    for_each_channel(c, aligned(RGB, linear_rgb))
      RGB[c] = linear_rgb[c] <= 0.0031308f ? 12.92f * linear_rgb[c]
                                           : (1.0f + 0.055f) * powf(linear_rgb[c], 1.0f / 2.4f) - 0.055f;
}

static inline void _dt_ucs_hsb_to_display_rgb_normalized(const dt_aligned_pixel_t HSB, const float white,
                                                         const dt_iop_order_iccprofile_info_t *display_profile,
                                                         dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t XYZ_D65 = { 0.f };
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t linear_rgb = { 0.f };
  dt_UCS_HSB_to_XYZ(HSB, white, XYZ_D65);
  XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
  _xyz_d50_to_profile_linear_rgb(XYZ_D50, display_profile, linear_rgb);
  _profile_linear_rgb_to_display_rgb_normalized(linear_rgb, display_profile, RGB);
}

float dt_colorrings_graph_white(void)
{
  return Y_to_dt_UCS_L_star(1.f);
}

float dt_colorrings_wrap_hue_2pi(float hue)
{
  while(hue < 0.f) hue += 2.f * M_PI_F;
  while(hue >= 2.f * M_PI_F) hue -= 2.f * M_PI_F;
  return hue;
}

float dt_colorrings_wrap_hue_pi(float hue)
{
  hue = dt_colorrings_wrap_hue_2pi(hue + M_PI_F);
  return hue - M_PI_F;
}

float dt_colorrings_curve_x_to_hue(const float x)
{
  return dt_colorrings_wrap_hue_pi((360.f * x + DT_COLORRINGS_ANGLE_SHIFT) * M_PI_F / 180.f);
}

float dt_colorrings_hue_to_curve_x(const float hue)
{
  return dt_colorrings_wrap_hue_2pi(hue - DT_COLORRINGS_ANGLE_SHIFT * M_PI_F / 180.f) / (2.f * M_PI_F);
}

float dt_colorrings_curve_periodic_distance(const float x0, const float x1)
{
  const float distance = fabsf(x0 - x1);
  return fminf(distance, 1.f - distance);
}

float dt_colorrings_ring_brightness(const dt_colorrings_ring_t ring)
{
  switch(ring)
  {
    case DT_COLORRINGS_RING_DARK:
      return 0.15f;
    case DT_COLORRINGS_RING_LIGHT:
      return 0.75f;
    case DT_COLORRINGS_RING_MID:
    default:
      return 0.45f;
  }
}

float dt_colorrings_curve_periodic_sample(const dt_colorrings_node_t *curve, const int nodes, const float x)
{
  /**
   * GUI state and history entries are expected to normalize the editable hue
   * nodes before they reach the shared sampler. Keep the interpolation backend
   * away from empty or degenerate anchor sets anyway so it never throws from
   * the underlying C++ spline code while a module is still repairing its
   * params.
   */
  if(!curve || nodes < 2) return 0.5f;

  CurveAnchorPoint anchors[DT_COLORRINGS_MAXNODES];

  for(int k = 0; k < nodes; k++)
  {
    anchors[k].x = curve[k].x;
    anchors[k].y = curve[k].y;
  }

  return interpolate_val_V2_periodic(nodes, anchors, x, MONOTONE_HERMITE, 1.f);
}

gboolean dt_colorrings_apply_rgb_lut(const dt_aligned_pixel_t input_rgb, const float white_level,
                                     const dt_iop_order_iccprofile_info_t *work_profile,
                                     const dt_iop_order_iccprofile_info_t *lut_profile, const float *clut,
                                     const uint16_t clut_level, dt_pthread_rwlock_t *clut_lock,
                                     const dt_lut3d_interpolation_t interpolation, dt_aligned_pixel_t output_rgb)
{
  if(!output_rgb) return FALSE;

  memcpy(output_rgb, input_rgb, sizeof(dt_aligned_pixel_t));

  if(!work_profile || !lut_profile || !clut || clut_level == 0) return FALSE;

  const float normalized_white = fmaxf(white_level, 1e-6f);
  output_rgb[0] = input_rgb[0] / normalized_white;
  output_rgb[1] = input_rgb[1] / normalized_white;
  output_rgb[2] = input_rgb[2] / normalized_white;
  output_rgb[3] = 0.f;

  dt_ioppr_transform_image_colorspace_rgb((float *)output_rgb, (float *)output_rgb, 1, 1, work_profile, lut_profile,
                                          "colorrings swatch work to HLG Rec2020");
  if(clut_lock) dt_pthread_rwlock_rdlock(clut_lock);
  dt_lut3d_apply((float *)output_rgb, (float *)output_rgb, 1, clut, clut_level, 1.f, interpolation);
  if(clut_lock) dt_pthread_rwlock_unlock(clut_lock);
  dt_ioppr_transform_image_colorspace_rgb((float *)output_rgb, (float *)output_rgb, 1, 1, lut_profile, work_profile,
                                          "colorrings swatch HLG Rec2020 to work");

  output_rgb[0] *= normalized_white;
  output_rgb[1] *= normalized_white;
  output_rgb[2] *= normalized_white;
  output_rgb[3] = 0.f;
  return TRUE;
}

void dt_colorrings_hsb_to_profile_rgb(const dt_aligned_pixel_t HSB, const float white,
                                      const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t XYZ_D65 = { 0.f };
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_UCS_HSB_to_XYZ(HSB, white, XYZ_D65);
  XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
  _xyz_d50_to_profile_rgb(XYZ_D50, profile, RGB);
}

void dt_colorrings_hsb_to_display_rgb(const dt_aligned_pixel_t HSB, const float white,
                                      const dt_iop_order_iccprofile_info_t *display_profile, dt_aligned_pixel_t RGB)
{
  _dt_ucs_hsb_to_display_rgb_normalized(HSB, white, display_profile, RGB);
  for_each_channel(c, aligned(RGB)) RGB[c] = CLAMP(RGB[c], 0.f, 1.f);
}

void dt_colorrings_profile_rgb_to_display_rgb(const dt_aligned_pixel_t RGB,
                                              const dt_iop_order_iccprofile_info_t *profile,
                                              const dt_iop_order_iccprofile_info_t *display_profile,
                                              dt_aligned_pixel_t display_rgb)
{
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t linear_rgb = { 0.f };

  if(profile)
    dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, profile->matrix_in_transposed, profile->lut_in,
                               profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
  else
    dt_linearRGB_to_XYZ(RGB, XYZ_D50);

  _xyz_d50_to_profile_linear_rgb(XYZ_D50, display_profile, linear_rgb);
  _profile_linear_rgb_to_display_rgb_normalized(linear_rgb, display_profile, display_rgb);
  for_each_channel(c, aligned(display_rgb)) display_rgb[c] = CLAMP(display_rgb[c], 0.f, 1.f);
}

void dt_colorrings_profile_rgb_to_dt_ucs_jch(const dt_aligned_pixel_t RGB, const float white,
                                             const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t JCH)
{
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t XYZ_D65 = { 0.f };
  dt_aligned_pixel_t xyY = { 0.f };

  if(!profile)
  {
    memset(JCH, 0, sizeof(dt_aligned_pixel_t));
    return;
  }

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, profile->matrix_in_transposed, profile->lut_in,
                             profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  for_each_channel(c, aligned(XYZ_D65)) XYZ_D65[c] = fmaxf(XYZ_D65[c], 0.f);

  if(XYZ_D65[0] + XYZ_D65[1] + XYZ_D65[2] <= 1e-6f)
  {
    memset(JCH, 0, sizeof(dt_aligned_pixel_t));
    return;
  }

  dt_XYZ_to_xyY(XYZ_D65, xyY);
  xyY_to_dt_UCS_JCH(xyY, white, JCH);
}

void dt_colorrings_profile_rgb_to_dt_ucs_hsb(const dt_aligned_pixel_t RGB, const float white,
                                             const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t HSB)
{
  dt_aligned_pixel_t JCH = { 0.f };
  dt_colorrings_profile_rgb_to_dt_ucs_jch(RGB, white, profile, JCH);
  dt_UCS_JCH_to_HSB(JCH, HSB);
}

void dt_colorrings_profile_rgb_to_Ych(const dt_aligned_pixel_t RGB, const dt_iop_order_iccprofile_info_t *profile,
                                      dt_aligned_pixel_t Ych)
{
  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t XYZ_D65 = { 0.f };

  if(!profile)
  {
    memset(Ych, 0, sizeof(dt_aligned_pixel_t));
    return;
  }

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, profile->matrix_in_transposed, profile->lut_in,
                             profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);

  if(Ych[2] < 0.f) Ych[2] = 2.f * M_PI_F + Ych[2];
}

static float _compute_reference_saturation(const float white, const float brightness)
{
  float low = 0.f;
  float high = 1.f;

  /**
   * The graph background should stay inside the preview gamut across the whole
   * hue circle, so we binary-search the highest dt UCS saturation that keeps
   * every sampled hue inside sRGB.
   */
  for(int iter = 0; iter < 18; iter++)
  {
    const float candidate = 0.5f * (low + high);
    gboolean valid = TRUE;

    for(int hue = 0; hue < DT_COLORRINGS_HUE_SAMPLES; hue++)
    {
      dt_aligned_pixel_t RGB = { 0.f };
      const dt_aligned_pixel_t HSB = { dt_colorrings_curve_x_to_hue((float)hue / (float)DT_COLORRINGS_HUE_SAMPLES),
                                       candidate, brightness, 0.f };
      _dt_ucs_hsb_to_preview_rgb_unclamped(HSB, white, RGB);

      if(RGB[0] < 0.f || RGB[0] > 1.f || RGB[1] < 0.f || RGB[1] > 1.f || RGB[2] < 0.f || RGB[2] > 1.f)
      {
        valid = FALSE;
        break;
      }
    }

    if(valid)
      low = candidate;
    else
      high = candidate;
  }

  return low;
}

void dt_colorrings_compute_reference_saturations(const float white,
                                                 float reference_saturation[DT_COLORRINGS_NUM_RINGS])
{
  for(int ring = 0; ring < DT_COLORRINGS_NUM_RINGS; ring++)
    if(reference_saturation[ring] == 0.f)
      reference_saturation[ring]
          = _compute_reference_saturation(white, dt_colorrings_ring_brightness((dt_colorrings_ring_t)ring));
}

float dt_colorrings_ring_axis_position_from_brightness(const float brightness, const float white,
                                                       const dt_iop_order_iccprofile_info_t *profile)
{
  const dt_aligned_pixel_t HSB = { 0.f, 0.f, CLAMP(brightness, 0.f, 1.f), 0.f };
  dt_aligned_pixel_t RGB = { 0.f };
  dt_colorrings_hsb_to_profile_rgb(HSB, white, profile, RGB);
  return CLAMP((RGB[0] + RGB[1] + RGB[2]) / 3.f, 0.f, 1.f);
}

void dt_colorrings_brightness_to_axis_rgb(const float brightness, const float white,
                                          const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t RGB)
{
  const float axis = dt_colorrings_ring_axis_position_from_brightness(brightness, white, profile);
  RGB[0] = axis;
  RGB[1] = axis;
  RGB[2] = axis;
  RGB[3] = 0.f;
}

float dt_colorrings_distance_to_cube_shell(const dt_aligned_pixel_t axis, const dt_aligned_pixel_t direction)
{
  float distance = INFINITY;

  for(int c = 0; c < 3; c++)
  {
    if(fabsf(direction[c]) < 1e-6f) continue;

    const float bound = (direction[c] > 0.f) ? 1.f : 0.f;
    const float candidate = (bound - axis[c]) / direction[c];
    if(candidate > 0.f && candidate < distance) distance = candidate;
  }

  return isfinite(distance) ? distance : 0.f;
}

void dt_colorrings_project_to_cube_shell(const dt_aligned_pixel_t axis, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t vector = { RGB[0] - axis[0], RGB[1] - axis[1], RGB[2] - axis[2], 0.f };

  if(dt_colorrings_vector_norm3(vector) < 1e-6f) return;

  const float shell_scale = dt_colorrings_distance_to_cube_shell(axis, vector);
  if(shell_scale < 1.f)
  {
    RGB[0] = axis[0] + shell_scale * vector[0];
    RGB[1] = axis[1] + shell_scale * vector[1];
    RGB[2] = axis[2] + shell_scale * vector[2];
  }

  RGB[0] = CLAMP(RGB[0], 0.f, 1.f);
  RGB[1] = CLAMP(RGB[1], 0.f, 1.f);
  RGB[2] = CLAMP(RGB[2], 0.f, 1.f);
}

float dt_colorrings_vector_norm3(const dt_aligned_pixel_t vector)
{
  return sqrtf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2]));
}

float dt_colorrings_dot3(const dt_aligned_pixel_t a, const dt_aligned_pixel_t b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void dt_colorrings_cross3(const dt_aligned_pixel_t a, const dt_aligned_pixel_t b, dt_aligned_pixel_t out)
{
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
  out[3] = 0.f;
}

void dt_colorrings_normalize3(dt_aligned_pixel_t vector)
{
  const float norm = dt_colorrings_vector_norm3(vector);
  if(norm < 1e-6f) return;

  vector[0] /= norm;
  vector[1] /= norm;
  vector[2] /= norm;
}

void dt_colorrings_rotate_around_axis(const dt_aligned_pixel_t input, const dt_aligned_pixel_t axis,
                                      const float cos_angle, const float sin_angle, dt_aligned_pixel_t output)
{
  dt_aligned_pixel_t cross = { 0.f };
  dt_colorrings_cross3(axis, input, cross);
  const float axis_dot = dt_colorrings_dot3(axis, input);

  for(int c = 0; c < 3; c++)
    output[c] = input[c] * cos_angle + cross[c] * sin_angle + axis[c] * axis_dot * (1.f - cos_angle);
  output[3] = 0.f;
}

void dt_colorrings_rgb_to_gray_cyl(const float rgb[3], float *L, float *rho, float *theta)
{
  const float eL0 = 0.5773502691896258f;
  const float eL1 = 0.5773502691896258f;
  const float eL2 = 0.5773502691896258f;

  const float eu0 = 0.7071067811865475f;
  const float eu1 = -0.7071067811865475f;
  const float eu2 = 0.0f;

  const float ev0 = 0.4082482904638630f;
  const float ev1 = 0.4082482904638630f;
  const float ev2 = -0.8164965809277260f;

  *L = rgb[0] * eL0 + rgb[1] * eL1 + rgb[2] * eL2;

  const float u = rgb[0] * eu0 + rgb[1] * eu1 + rgb[2] * eu2;
  const float v = rgb[0] * ev0 + rgb[1] * ev1 + rgb[2] * ev2;

  *rho = sqrtf(u * u + v * v);
  *theta = atan2f(v, u);
}

void dt_colorrings_gray_basis_to_rgb(const float L, const float u, const float v, float rgb[3])
{
  const float eL0 = 0.5773502691896258f;
  const float eL1 = 0.5773502691896258f;
  const float eL2 = 0.5773502691896258f;

  const float eu0 = 0.7071067811865475f;
  const float eu1 = -0.7071067811865475f;
  const float eu2 = 0.0f;

  const float ev0 = 0.4082482904638630f;
  const float ev1 = 0.4082482904638630f;
  const float ev2 = -0.8164965809277260f;

  rgb[0] = L * eL0 + u * eu0 + v * ev0;
  rgb[1] = L * eL1 + u * eu1 + v * ev1;
  rgb[2] = L * eL2 + u * eu2 + v * ev2;
}

void dt_colorrings_gray_axis_rgb_from_L(const float L, dt_aligned_pixel_t RGB)
{
  const float value = L * 0.5773502691896258f;
  RGB[0] = value;
  RGB[1] = value;
  RGB[2] = value;
  RGB[3] = 0.f;
}

float dt_colorrings_wendland_c2(float d)
{
  if(d >= 1.0f) return 0.0f;
  const float t = 1.0f - d;
  return t * t * t * t * (4.0f * d + 1.0f);
}

float dt_colorrings_wrap_pi(float x)
{
  const float two_pi = 2.0f * (float)M_PI;
  while(x <= -(float)M_PI) x += two_pi;
  while(x > (float)M_PI) x -= two_pi;
  return x;
}

void dt_colorrings_eval_local_field(
    const float x[3], const float anchor_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_rho[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float chroma_scale[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES], const float inv_sigma_L,
    const float inv_sigma_rho, const float inv_sigma_theta, const float rho0, float out[3])
{
  float Lx, rhox, thetax;
  dt_colorrings_rgb_to_gray_cyl(x, &Lx, &rhox, &thetax);

  if(rhox <= 1e-6f)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }

  float sum_w = 0.f;
  float sum_dL = 0.f;
  float sum_scale = 0.f;
  float sum_dtheta = 0.f;

  const int n = DT_COLORRINGS_LOCAL_FIELD_RINGS * DT_COLORRINGS_HUE_SAMPLES;
  const int axis_ring = DT_COLORRINGS_LOCAL_FIELD_RINGS - 1;
  const float axis_weight_scale = 1.0f / (float)DT_COLORRINGS_HUE_SAMPLES;

  /**
   * We loop over every sparse control node in the cylindrical basis to rebuild
   * a dense displacement field directly inside the LUT RGB cube.
   */
  for(int k = 0; k < n; k++)
  {
    const int ring = k / DT_COLORRINGS_HUE_SAMPLES;
    const int h = k - ring * DT_COLORRINGS_HUE_SAMPLES;
    const float dL = (Lx - anchor_L[ring][h]) * inv_sigma_L;
    const float dr = (rhox - anchor_rho[ring][h]) * inv_sigma_rho;
    const float dh = dt_colorrings_wrap_pi(thetax - anchor_theta[ring][h]) * inv_sigma_theta;
    const float d2 = dL * dL + dr * dr + dh * dh;

    if(d2 >= 1.f) continue;

    float w = dt_colorrings_wendland_c2(sqrtf(d2));
    if(ring == axis_ring) w *= axis_weight_scale;

    sum_w += w;
    sum_dL += w * delta_L[ring][h];
    sum_scale += w * chroma_scale[ring][h];
    sum_dtheta += w * delta_theta[ring][h];
  }

  if(sum_w <= FLT_MIN)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }

  const float inv_w = 1.0f / sum_w;
  const float target_delta_L = sum_dL * inv_w;
  const float scale = sum_scale * inv_w;
  const float target_delta_theta = sum_dtheta * inv_w;
  if(fabsf(target_delta_L) <= 1e-6f && fabsf(scale - 1.f) <= 1e-6f && fabsf(target_delta_theta) <= 1e-6f)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }
  const float t = CLAMP(rhox / rho0, 0.f, 1.f);
  const float alpha = t * t * (3.0f - 2.0f * t);
  const float target_L = Lx + alpha * target_delta_L;
  const float target_rho = rhox * fmaxf(1.f + alpha * (scale - 1.f), 0.f);
  const float target_theta = thetax + alpha * target_delta_theta;
  dt_aligned_pixel_t target_rgb = { 0.f };
  dt_aligned_pixel_t axis = { 0.f };
  dt_colorrings_gray_basis_to_rgb(target_L, target_rho * cosf(target_theta), target_rho * sinf(target_theta),
                                  target_rgb);
  dt_colorrings_gray_axis_rgb_from_L(target_L, axis);
  dt_colorrings_project_to_cube_shell(axis, target_rgb);

  out[0] = target_rgb[0] - x[0];
  out[1] = target_rgb[1] - x[1];
  out[2] = target_rgb[2] - x[2];
}

void dt_colorrings_fill_lut_local_field(
    float *lut, const int level, const float anchor_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_rho[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float chroma_scale[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES], const float inv_sigma_L,
    const float inv_sigma_rho, const float inv_sigma_theta, const float rho0)
{
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static) collapse(3) firstprivate(                          \
        lut, level, anchor_L, anchor_rho, anchor_theta, delta_L, chroma_scale, delta_theta, inv_sigma_L,          \
        inv_sigma_rho, inv_sigma_theta, rho0)
#endif
  for(int b = 0; b < level; b++)
    for(int g = 0; g < level; g++)
      for(int r = 0; r < level; r++)
      {
        const float x[3]
            = { (float)r / (float)(level - 1), (float)g / (float)(level - 1), (float)b / (float)(level - 1) };
        float d[3];
        dt_colorrings_eval_local_field(x, anchor_L, anchor_rho, anchor_theta, delta_L, chroma_scale, delta_theta,
                                       inv_sigma_L, inv_sigma_rho, inv_sigma_theta, rho0, d);

        const size_t idx = (((size_t)b * level + (size_t)g) * level + (size_t)r) * 3u;
        lut[idx + 0] = CLAMP(x[0] + d[0], 0.f, 1.f);
        lut[idx + 1] = CLAMP(x[1] + d[1], 0.f, 1.f);
        lut[idx + 2] = CLAMP(x[2] + d[2], 0.f, 1.f);
      }
}

void dt_colorrings_eval_sparse_local_field(const float x[3], const dt_colorrings_sparse_anchor_t *const anchors,
                                           const int anchor_count, const float inv_sigma_L,
                                           const float inv_sigma_rho, const float inv_sigma_theta,
                                           const float rho0, float out[3])
{
  float Lx, rhox, thetax;
  dt_colorrings_rgb_to_gray_cyl(x, &Lx, &rhox, &thetax);

  if(rhox <= 1e-6f || !anchors || anchor_count <= 0)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }

  float sum_w = 0.f;
  float sum_dL = 0.f;
  float sum_scale = 0.f;
  float sum_dtheta = 0.f;

  /**
   * Sparse anchors let modules author their own control geometry while
   * reusing the same RGB cylindrical local field as the original color rings.
   * We therefore loop only over the caller-provided anchors instead of
   * synthesizing fake ring samples that would bias the normalization.
   */
  for(int k = 0; k < anchor_count; k++)
  {
    const float dL = (Lx - anchors[k].L) * inv_sigma_L;
    const float dr = (rhox - anchors[k].rho) * inv_sigma_rho;
    const float dh = dt_colorrings_wrap_pi(thetax - anchors[k].theta) * inv_sigma_theta;
    const float d2 = dL * dL + dr * dr + dh * dh;

    if(d2 >= 1.f) continue;

    const float w = anchors[k].weight * dt_colorrings_wendland_c2(sqrtf(d2));
    if(w <= FLT_MIN) continue;

    sum_w += w;
    sum_dL += w * anchors[k].delta_L;
    sum_scale += w * anchors[k].chroma_scale;
    sum_dtheta += w * anchors[k].delta_theta;
  }

  if(sum_w <= FLT_MIN)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }

  const float inv_w = 1.0f / sum_w;
  const float target_delta_L = sum_dL * inv_w;
  const float scale = sum_scale * inv_w;
  const float target_delta_theta = sum_dtheta * inv_w;
  if(fabsf(target_delta_L) <= 1e-6f && fabsf(scale - 1.f) <= 1e-6f && fabsf(target_delta_theta) <= 1e-6f)
  {
    out[0] = 0.f;
    out[1] = 0.f;
    out[2] = 0.f;
    return;
  }
  const float t = CLAMP(rhox / rho0, 0.f, 1.f);
  const float alpha = t * t * (3.0f - 2.0f * t);
  const float target_L = Lx + alpha * target_delta_L;
  const float target_rho = rhox * fmaxf(1.f + alpha * (scale - 1.f), 0.f);
  const float target_theta = thetax + alpha * target_delta_theta;
  dt_aligned_pixel_t target_rgb = { 0.f };
  dt_aligned_pixel_t axis = { 0.f };
  dt_colorrings_gray_basis_to_rgb(target_L, target_rho * cosf(target_theta), target_rho * sinf(target_theta),
                                  target_rgb);
  dt_colorrings_gray_axis_rgb_from_L(target_L, axis);
  dt_colorrings_project_to_cube_shell(axis, target_rgb);

  out[0] = target_rgb[0] - x[0];
  out[1] = target_rgb[1] - x[1];
  out[2] = target_rgb[2] - x[2];
}

void dt_colorrings_fill_lut_sparse_local_field(float *lut, const int level,
                                               const dt_colorrings_sparse_anchor_t *const anchors,
                                               const int anchor_count, const float inv_sigma_L,
                                               const float inv_sigma_rho, const float inv_sigma_theta,
                                               const float rho0)
{
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) schedule(static) collapse(3) firstprivate(                          \
        lut, level, anchors, anchor_count, inv_sigma_L, inv_sigma_rho, inv_sigma_theta, rho0)
#endif
  for(int b = 0; b < level; b++)
    for(int g = 0; g < level; g++)
      for(int r = 0; r < level; r++)
      {
        const float x[3]
            = { (float)r / (float)(level - 1), (float)g / (float)(level - 1), (float)b / (float)(level - 1) };
        float d[3] = { 0.f };
        if(anchors && anchor_count > 0)
          dt_colorrings_eval_sparse_local_field(x, anchors, anchor_count, inv_sigma_L, inv_sigma_rho, inv_sigma_theta,
                                                rho0, d);

        const size_t idx = (((size_t)b * level + (size_t)g) * level + (size_t)r) * 3u;
        lut[idx + 0] = CLAMP(x[0] + d[0], 0.f, 1.f);
        lut[idx + 1] = CLAMP(x[1] + d[1], 0.f, 1.f);
        lut[idx + 2] = CLAMP(x[2] + d[2], 0.f, 1.f);
      }
}
