/*
  This file is part of the Ansel project.
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/illuminants.h"
#include "common/matrices.h"
#include "develop/imageop_math.h"
#include "gaussian_elimination.h"
#include "iop/channelmixerrgb_shared.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 0.5773502691896258f

float dt_iop_channelmixer_shared_wrap_pi(float angle)
{
  while(angle <= -M_PI) angle += 2.f * (float)M_PI;
  while(angle > M_PI) angle -= 2.f * (float)M_PI;
  return angle;
}

float dt_iop_channelmixer_shared_wrap_half_pi(float angle)
{
  while(angle <= -(float)M_PI_2) angle += (float)M_PI;
  while(angle > (float)M_PI_2) angle -= (float)M_PI;
  return angle;
}

float dt_iop_channelmixer_shared_encode_simple_stretch(const float stretch)
{
  if(stretch >= 1.f) return fminf(0.5f * (stretch + 1.f), 1.5f);
  if(stretch >= -1.f) return stretch;
  return fmaxf(0.5f * (stretch - 1.f), -1.5f);
}

float dt_iop_channelmixer_shared_decode_simple_stretch(const float slider)
{
  if(slider >= 1.f) return 2.f * slider - 1.f;
  if(slider >= -1.f) return slider;
  return 2.f * slider + 1.f;
}

float dt_iop_channelmixer_shared_encode_simple_coupling_amount(const float amount)
{
  return atanf(fmaxf(amount, 0.f)) / DT_IOP_CHANNELMIXER_SHARED_SIMPLE_TAN_SCALE;
}

float dt_iop_channelmixer_shared_decode_simple_coupling_amount(const float slider)
{
  const float bounded = CLAMP(slider, 0.f, 0.999f);
  return tanf(bounded * DT_IOP_CHANNELMIXER_SHARED_SIMPLE_TAN_SCALE);
}

void dt_iop_channelmixer_shared_simple_from_sliders(GtkWidget *const widgets[6],
                                                    dt_iop_channelmixer_shared_simple_params_t *simple)
{
  simple->theta = dt_bauhaus_slider_get(widgets[0]) * (float)M_PI;
  simple->psi = dt_bauhaus_slider_get(widgets[1]) * (float)M_PI_2;
  simple->stretch_1 = dt_iop_channelmixer_shared_decode_simple_stretch(dt_bauhaus_slider_get(widgets[2]));
  simple->stretch_2 = dt_iop_channelmixer_shared_decode_simple_stretch(dt_bauhaus_slider_get(widgets[3]));
  simple->coupling_amount = dt_iop_channelmixer_shared_decode_simple_coupling_amount(dt_bauhaus_slider_get(widgets[4]));
  simple->coupling_hue = dt_bauhaus_slider_get(widgets[5]) * (float)M_PI;
}

void dt_iop_channelmixer_shared_simple_to_sliders(const dt_iop_channelmixer_shared_simple_params_t *const simple,
                                                  GtkWidget *const widgets[6])
{
  dt_bauhaus_slider_set(widgets[0], dt_iop_channelmixer_shared_wrap_pi(simple->theta) / (float)M_PI);
  dt_bauhaus_slider_set(widgets[1], dt_iop_channelmixer_shared_wrap_half_pi(simple->psi) / (float)M_PI_2);
  dt_bauhaus_slider_set(widgets[2], dt_iop_channelmixer_shared_encode_simple_stretch(simple->stretch_1));
  dt_bauhaus_slider_set(widgets[3], dt_iop_channelmixer_shared_encode_simple_stretch(simple->stretch_2));
  dt_bauhaus_slider_set(widgets[4], dt_iop_channelmixer_shared_encode_simple_coupling_amount(simple->coupling_amount));
  dt_bauhaus_slider_set(widgets[5], dt_iop_channelmixer_shared_wrap_pi(simple->coupling_hue) / (float)M_PI);
}

void dt_iop_channelmixer_shared_primaries_from_sliders(GtkWidget *const widgets[9],
                                                       dt_iop_channelmixer_shared_primaries_params_t *primaries)
{
  primaries->achromatic_hue = dt_bauhaus_slider_get(widgets[0]) * (float)M_PI_2;
  primaries->achromatic_purity = dt_bauhaus_slider_get(widgets[1]);
  primaries->red_hue = dt_bauhaus_slider_get(widgets[2]) * (float)M_PI_2;
  primaries->red_purity = dt_bauhaus_slider_get(widgets[3]);
  primaries->green_hue = dt_bauhaus_slider_get(widgets[4]) * (float)M_PI_2;
  primaries->green_purity = dt_bauhaus_slider_get(widgets[5]);
  primaries->blue_hue = dt_bauhaus_slider_get(widgets[6]) * (float)M_PI_2;
  primaries->blue_purity = dt_bauhaus_slider_get(widgets[7]);
  primaries->gain = dt_bauhaus_slider_get(widgets[8]);
}

void dt_iop_channelmixer_shared_primaries_to_sliders(const dt_iop_channelmixer_shared_primaries_params_t *const primaries,
                                                     GtkWidget *const widgets[9])
{
  dt_bauhaus_slider_set(widgets[0], CLAMP(primaries->achromatic_hue / (float)M_PI_2, -1.f, 1.f));
  dt_bauhaus_slider_set(widgets[1], primaries->achromatic_purity);
  dt_bauhaus_slider_set(widgets[2], CLAMP(primaries->red_hue / (float)M_PI_2, -1.f, 1.f));
  dt_bauhaus_slider_set(widgets[3], primaries->red_purity);
  dt_bauhaus_slider_set(widgets[4], CLAMP(primaries->green_hue / (float)M_PI_2, -1.f, 1.f));
  dt_bauhaus_slider_set(widgets[5], primaries->green_purity);
  dt_bauhaus_slider_set(widgets[6], CLAMP(primaries->blue_hue / (float)M_PI_2, -1.f, 1.f));
  dt_bauhaus_slider_set(widgets[7], primaries->blue_purity);
  dt_bauhaus_slider_set(widgets[8], primaries->gain);
}

gboolean dt_iop_channelmixer_shared_rows_are_normalized(const gboolean normalize[3])
{
  return normalize[0] && normalize[1] && normalize[2];
}

gboolean dt_iop_channelmixer_shared_get_matrix(const float rows[3][3], const gboolean normalize[3],
                                               const gboolean force_normalize, float M[3][3])
{
  for(int row = 0; row < 3; row++)
  {
    float sum = 1.f;
    if(normalize[row] || force_normalize)
    {
      sum = rows[row][0] + rows[row][1] + rows[row][2];
      if(sum == 0.f) return FALSE;
    }

    for(int col = 0; col < 3; col++) M[row][col] = rows[row][col] / sum;
  }

  return TRUE;
}

void dt_iop_channelmixer_shared_set_matrix(float rows[3][3], const float M[3][3])
{
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      rows[row][col] = M[row][col];
}

void dt_iop_channelmixer_shared_mul3x3(const float A[3][3], const float B[3][3], float C[3][3])
{
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
    {
      C[row][col] = 0.f;
      for(int k = 0; k < 3; k++) C[row][col] += A[row][k] * B[k][col];
    }
}

static void _mixer_to_chroma_basis(const float M[3][3], float B[3][3])
{
  static const float P[3][3]
      = { { 0.7071067811865475f,  0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { -0.7071067811865475f, 0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { 0.f,                 -0.8164965809277261f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 } };
  float PTM[3][3] = { { 0.f } };
  float PT[3][3] = { { 0.f } };

  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      PT[row][col] = P[col][row];

  dt_iop_channelmixer_shared_mul3x3(PT, M, PTM);
  dt_iop_channelmixer_shared_mul3x3(PTM, P, B);
}

static void _mixer_from_chroma_basis(const float B[3][3], float M[3][3])
{
  static const float P[3][3]
      = { { 0.7071067811865475f,  0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { -0.7071067811865475f, 0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { 0.f,                 -0.8164965809277261f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 } };
  float temp[3][3] = { { 0.f } };
  float PT[3][3] = { { 0.f } };

  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      PT[row][col] = P[col][row];

  dt_iop_channelmixer_shared_mul3x3(P, B, temp);
  dt_iop_channelmixer_shared_mul3x3(temp, PT, M);
}

void dt_iop_channelmixer_shared_simple_from_matrix(const float M[3][3],
                                                   dt_iop_channelmixer_shared_simple_params_t *simple)
{
  float B[3][3] = { { 0.f } };
  _mixer_to_chroma_basis(M, B);

  const float a = B[0][0];
  const float b = B[0][1];
  const float c = B[1][0];
  const float d = B[1][1];
  const float theta = dt_iop_channelmixer_shared_wrap_pi(atan2f(c - b, a + d));
  const float ctheta = cosf(theta);
  const float stheta = sinf(theta);

  const float s00 = ctheta * a + stheta * c;
  const float s01 = ctheta * b + stheta * d;
  const float s11 = -stheta * b + ctheta * d;
  const float diff = s00 - s11;
  const float psi = dt_iop_channelmixer_shared_wrap_half_pi(0.5f * atan2f(2.f * s01, diff));
  const float radius = hypotf(0.5f * diff, s01);
  const float trace = s00 + s11;

  simple->theta = theta;
  simple->psi = psi;
  simple->stretch_1 = 0.5f * trace + radius;
  simple->stretch_2 = 0.5f * trace - radius;
  simple->coupling_amount = hypotf(B[2][0], B[2][1]);
  simple->coupling_hue = simple->coupling_amount > 0.f
                             ? dt_iop_channelmixer_shared_wrap_pi(atan2f(B[2][1], B[2][0]))
                             : 0.f;
}

void dt_iop_channelmixer_shared_simple_to_matrix(const dt_iop_channelmixer_shared_simple_params_t *const simple,
                                                 float M[3][3])
{
  const float ctheta = cosf(simple->theta);
  const float stheta = sinf(simple->theta);
  const float cpsi = cosf(simple->psi);
  const float spsi = sinf(simple->psi);
  const float coupling_0 = simple->coupling_amount * cosf(simple->coupling_hue);
  const float coupling_1 = simple->coupling_amount * sinf(simple->coupling_hue);

  const float s00 = simple->stretch_1 * cpsi * cpsi + simple->stretch_2 * spsi * spsi;
  const float s01 = (simple->stretch_1 - simple->stretch_2) * cpsi * spsi;
  const float s11 = simple->stretch_1 * spsi * spsi + simple->stretch_2 * cpsi * cpsi;
  const float B[3][3]
      = { { ctheta * s00 - stheta * s01, ctheta * s01 - stheta * s11, 0.f },
          { stheta * s00 + ctheta * s01, stheta * s01 + ctheta * s11, 0.f },
          { coupling_0, coupling_1, 1.f } };

  _mixer_from_chroma_basis(B, M);
}

float dt_iop_channelmixer_shared_roundtrip_error(const float M[3][3], const float roundtrip[3][3])
{
  float max_error = 0.f;
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      max_error = fmaxf(max_error, fabsf(M[row][col] - roundtrip[row][col]));

  return max_error;
}

dt_iop_channelmixer_shared_primaries_basis_t
dt_iop_channelmixer_shared_primaries_basis_from_adaptation(const dt_adaptation_t adaptation)
{
  switch(adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
      return DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_BRADFORD;
    case DT_ADAPTATION_CAT16:
      return DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_CAT16;
    case DT_ADAPTATION_XYZ:
      return DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_XYZ;
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      return DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB;
  }
}

static void _primaries_reference_white(const dt_iop_channelmixer_shared_primaries_basis_t basis, float white[3])
{
  dt_aligned_pixel_t D50 = { 0.f };

  switch(basis)
  {
    case DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_BRADFORD:
      convert_D50_to_LMS(DT_ADAPTATION_LINEAR_BRADFORD, D50);
      break;
    case DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_CAT16:
      convert_D50_to_LMS(DT_ADAPTATION_CAT16, D50);
      break;
    case DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_XYZ:
      convert_D50_to_LMS(DT_ADAPTATION_XYZ, D50);
      break;
    case DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB:
    default:
      convert_D50_to_LMS(DT_ADAPTATION_RGB, D50);
      break;
  }

  for(int c = 0; c < 3; c++) white[c] = D50[c];
}

static inline float _affine_sum3(const float vector[3])
{
  return vector[0] + vector[1] + vector[2];
}

static gboolean _affine_normalize(const float vector[3], float normalized[3])
{
  const float sum = _affine_sum3(vector);
  if(fabsf(sum) < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FALSE;

  for(int c = 0; c < 3; c++) normalized[c] = vector[c] / sum;
  return TRUE;
}

static void _affine_project_difference(const float difference[3], float uv[2])
{
  uv[0] = 0.7071067811865475f * (difference[0] - difference[1]);
  uv[1] = 0.4082482904638631f * (difference[0] + difference[1]) - 0.8164965809277261f * difference[2];
}

static void _affine_unproject_difference(const float uv[2], float difference[3])
{
  difference[0] = 0.7071067811865475f * uv[0] + 0.4082482904638631f * uv[1];
  difference[1] = -0.7071067811865475f * uv[0] + 0.4082482904638631f * uv[1];
  difference[2] = -0.8164965809277261f * uv[1];
}

static void _rotate_2d(const float vector[2], const float angle, float rotated[2])
{
  const float cosine = cosf(angle);
  const float sine = sinf(angle);
  rotated[0] = cosine * vector[0] - sine * vector[1];
  rotated[1] = sine * vector[0] + cosine * vector[1];
}

static float _intersect_affine_ray_segment(const float direction[2], const float first[2], const float second[2])
{
  const float edge_x = second[0] - first[0];
  const float edge_y = second[1] - first[1];
  const float denominator = direction[0] * edge_y - direction[1] * edge_x;
  if(fabsf(denominator) < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FLT_MAX;

  const float t = (first[0] * edge_y - first[1] * edge_x) / denominator;
  const float u = (first[0] * direction[1] - first[1] * direction[0]) / denominator;
  if(t >= 0.f && u >= 0.f && u <= 1.f) return t;
  return FLT_MAX;
}

static float _affine_distance_to_edge(const float direction[2], const float reference_primaries[3][2])
{
  float distance_to_edge = FLT_MAX;

  for(int i = 0; i < 3; i++)
  {
    const int next = i == 2 ? 0 : i + 1;
    const float distance = _intersect_affine_ray_segment(direction, reference_primaries[i],
                                                         reference_primaries[next]);
    if(distance < distance_to_edge) distance_to_edge = distance;
  }

  return distance_to_edge;
}

static gboolean _build_affine_simplex(const dt_iop_channelmixer_shared_primaries_basis_t basis,
                                      float white_normalized[3], float reference_primaries[3][2])
{
  const float identity[3][3] = { { 1.f, 0.f, 0.f },
                                 { 0.f, 1.f, 0.f },
                                 { 0.f, 0.f, 1.f } };
  float white[3] = { 0.f };

  _primaries_reference_white(basis, white);
  if(!_affine_normalize(white, white_normalized)) return FALSE;

  for(int i = 0; i < 3; i++)
  {
    float difference[3] = { identity[i][0] - white_normalized[0],
                            identity[i][1] - white_normalized[1],
                            identity[i][2] - white_normalized[2] };
    _affine_project_difference(difference, reference_primaries[i]);
  }

  return TRUE;
}

static gboolean _affine_point_from_polar(const float white_normalized[3], const float reference_primaries[3][2],
                                         const int reference_index, const float hue, const float purity,
                                         float point_normalized[3])
{
  const float *const reference = reference_primaries[reference_index];
  const float radius = hypotf(reference[0], reference[1]);
  if(radius < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FALSE;

  const float direction_reference[2] = { reference[0] / radius, reference[1] / radius };
  float direction[2] = { 0.f };
  float difference[3] = { 0.f };

  _rotate_2d(direction_reference, hue, direction);

  const float distance_to_edge = _affine_distance_to_edge(direction, reference_primaries);
  if(distance_to_edge == FLT_MAX) return FALSE;

  const float uv[2] = { purity * distance_to_edge * direction[0],
                        purity * distance_to_edge * direction[1] };
  _affine_unproject_difference(uv, difference);

  for(int c = 0; c < 3; c++) point_normalized[c] = white_normalized[c] + difference[c];
  return TRUE;
}

static gboolean _affine_polar_from_point(const float white_normalized[3], const float reference_primaries[3][2],
                                         const int reference_index, const float point_normalized[3], float *hue,
                                         float *purity)
{
  const float *const reference = reference_primaries[reference_index];
  const float reference_angle = atan2f(reference[1], reference[0]);
  float difference[3] = { point_normalized[0] - white_normalized[0],
                          point_normalized[1] - white_normalized[1],
                          point_normalized[2] - white_normalized[2] };
  float uv[2] = { 0.f };

  _affine_project_difference(difference, uv);

  const float radius = hypotf(uv[0], uv[1]);
  if(radius < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS)
  {
    *hue = 0.f;
    *purity = 0.f;
    return TRUE;
  }

  const float direction[2] = { uv[0] / radius, uv[1] / radius };
  const float distance_to_edge = _affine_distance_to_edge(direction, reference_primaries);
  if(distance_to_edge == FLT_MAX || distance_to_edge < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FALSE;

  *hue = dt_iop_channelmixer_shared_wrap_pi(atan2f(direction[1], direction[0]) - reference_angle);
  *purity = radius / distance_to_edge;
  return TRUE;
}

gboolean dt_iop_channelmixer_shared_primaries_to_matrix(const dt_iop_channelmixer_shared_primaries_basis_t basis,
                                                        const dt_iop_channelmixer_shared_primaries_params_t *primaries,
                                                        float M[3][3])
{
  float white_reference[3] = { 0.f };
  float reference_primaries[3][2] = { { 0.f } };
  float white_reference_normalized[3] = { 0.f };
  float custom_white_normalized[3] = { 0.f };
  float custom_primaries[3][3] = { { 0.f } };
  dt_colormatrix_t normalized_inverse = { { 0.f } };
  dt_colormatrix_t normalized_primaries = { { 0.f } };

  _primaries_reference_white(basis, white_reference);
  const float white_reference_sum = _affine_sum3(white_reference);
  if(fabsf(white_reference_sum) < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FALSE;

  if(!_build_affine_simplex(basis, white_reference_normalized, reference_primaries)) return FALSE;

  if(!_affine_point_from_polar(white_reference_normalized, reference_primaries, 0, primaries->achromatic_hue,
                               primaries->achromatic_purity, custom_white_normalized))
    return FALSE;

  if(!_affine_point_from_polar(white_reference_normalized, reference_primaries, 0, primaries->red_hue,
                               primaries->red_purity, custom_primaries[0]))
    return FALSE;
  if(!_affine_point_from_polar(white_reference_normalized, reference_primaries, 1, primaries->green_hue,
                               primaries->green_purity, custom_primaries[1]))
    return FALSE;
  if(!_affine_point_from_polar(white_reference_normalized, reference_primaries, 2, primaries->blue_hue,
                               primaries->blue_purity, custom_primaries[2]))
    return FALSE;

  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      normalized_primaries[row][col] = custom_primaries[col][row];

  if(mat3SSEinv(normalized_inverse, normalized_primaries)) return FALSE;

  const float white_gain = primaries->gain * white_reference_sum;
  const dt_aligned_pixel_t custom_white = { white_gain * custom_white_normalized[0],
                                            white_gain * custom_white_normalized[1],
                                            white_gain * custom_white_normalized[2],
                                            0.f };
  dt_aligned_pixel_t column_scales = { 0.f };
  dt_apply_transposed_color_matrix(custom_white, normalized_inverse, column_scales);

  for(int col = 0; col < 3; col++)
    for(int row = 0; row < 3; row++)
      M[row][col] = column_scales[col] * custom_primaries[col][row];

  return TRUE;
}

gboolean dt_iop_channelmixer_shared_primaries_from_matrix(const dt_iop_channelmixer_shared_primaries_basis_t basis,
                                                          const float M[3][3],
                                                          dt_iop_channelmixer_shared_primaries_params_t *primaries)
{
  dt_colormatrix_t padded = { { 0.f } };
  dt_colormatrix_t inverse = { { 0.f } };
  float white_reference[3] = { 0.f };
  float white_reference_normalized[3] = { 0.f };
  float reference_primaries[3][2] = { { 0.f } };
  float custom_white[3] = { 0.f };
  float custom_white_normalized[3] = { 0.f };
  float custom_primary_normalized[3] = { 0.f };

  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      padded[row][col] = M[row][col];
  if(mat3SSEinv(inverse, padded)) return FALSE;

  _primaries_reference_white(basis, white_reference);
  const float white_reference_sum = _affine_sum3(white_reference);
  if(fabsf(white_reference_sum) < DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS) return FALSE;

  if(!_build_affine_simplex(basis, white_reference_normalized, reference_primaries)) return FALSE;

  for(int row = 0; row < 3; row++) custom_white[row] = M[row][0] + M[row][1] + M[row][2];
  if(!_affine_normalize(custom_white, custom_white_normalized)) return FALSE;

  primaries->gain = _affine_sum3(custom_white) / white_reference_sum;
  if(!_affine_polar_from_point(white_reference_normalized, reference_primaries, 0, custom_white_normalized,
                               &primaries->achromatic_hue, &primaries->achromatic_purity))
    return FALSE;

  for(int primary = 0; primary < 3; primary++)
  {
    const float column[3] = { M[0][primary], M[1][primary], M[2][primary] };
    if(!_affine_normalize(column, custom_primary_normalized)) return FALSE;

    float *hue = primary == 0 ? &primaries->red_hue : primary == 1 ? &primaries->green_hue : &primaries->blue_hue;
    float *purity = primary == 0 ? &primaries->red_purity
                                 : primary == 1 ? &primaries->green_purity
                                                : &primaries->blue_purity;

    if(!_affine_polar_from_point(white_reference_normalized, reference_primaries, primary,
                                 custom_primary_normalized, hue, purity))
      return FALSE;
  }

  return TRUE;
}

void dt_iop_channelmixer_shared_simple_probe_source(const dt_iop_channelmixer_shared_simple_probe_t probe,
                                                    float source[3])
{
  static const float P[3][3]
      = { { 0.7071067811865475f,  0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { -0.7071067811865475f, 0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { 0.f,                 -0.8164965809277261f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 } };
  const float basis[3]
      = { probe == DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_1 ? DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE
                                                                  : probe == DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_ROTATION
                                                                        ? DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * 0.7071067811865475f
                                                                        : 0.f,
          probe == DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_2 ? DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE
                                                                  : probe == DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_ROTATION
                                                                        ? DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * 0.7071067811865475f
                                                                        : 0.f,
          1.f };

  for(int row = 0; row < 3; row++)
  {
    source[row] = 0.f;
    for(int col = 0; col < 3; col++) source[row] += P[row][col] * basis[col];
  }
}

static void _normalize_linear_display_rgb(dt_aligned_pixel_t linear_display_rgb)
{
  const float max_RGB = fmaxf(fmaxf(linear_display_rgb[0], linear_display_rgb[1]), linear_display_rgb[2]);
  if(max_RGB > 1.f)
    for(int c = 0; c < 3; c++) linear_display_rgb[c] = fmaxf(linear_display_rgb[c] / max_RGB, 0.f);
  else
    for(int c = 0; c < 3; c++) linear_display_rgb[c] = fmaxf(linear_display_rgb[c], 0.f);
}

void dt_iop_channelmixer_shared_work_rgb_to_display(const dt_aligned_pixel_t work_rgb,
                                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                                    const dt_iop_order_iccprofile_info_t *const display_profile,
                                                    dt_aligned_pixel_t display_rgb)
{
  if(work_profile && display_profile)
  {
    dt_aligned_pixel_t XYZ = { 0.f };
    dt_aligned_pixel_t linear_display_rgb = { 0.f };

    dt_ioppr_rgb_matrix_to_xyz(work_rgb, XYZ, work_profile->matrix_in_transposed, work_profile->lut_in,
                               work_profile->unbounded_coeffs_in, work_profile->lutsize,
                               work_profile->nonlinearlut);
    dt_apply_transposed_color_matrix(XYZ, display_profile->matrix_out_transposed, linear_display_rgb);
    _normalize_linear_display_rgb(linear_display_rgb);

    if(display_profile->nonlinearlut)
      _apply_trc(linear_display_rgb, display_rgb, display_profile->lut_out, display_profile->unbounded_coeffs_out,
                 display_profile->lutsize);
    else
      for(int c = 0; c < 4; c++) display_rgb[c] = linear_display_rgb[c];
  }
  else
  {
    for(int c = 0; c < 4; c++) display_rgb[c] = work_rgb[c];
    _normalize_linear_display_rgb(display_rgb);
  }

  for(int c = 0; c < 3; c++) display_rgb[c] = CLAMP(display_rgb[c], 0.f, 1.f);
}

void dt_iop_channelmixer_shared_module_color_to_display(const float module_color[3], const dt_adaptation_t adaptation,
                                                        const dt_iop_order_iccprofile_info_t *const work_profile,
                                                        const dt_iop_order_iccprofile_info_t *const display_profile,
                                                        float display_rgb[3])
{
  dt_aligned_pixel_t work_rgb = { module_color[0], module_color[1], module_color[2], 0.f };
  dt_aligned_pixel_t display = { 0.f };

  if(adaptation != DT_ADAPTATION_RGB)
  {
    dt_aligned_pixel_t LMS = { module_color[0], module_color[1], module_color[2], 0.f };
    convert_any_LMS_to_RGB(LMS, work_rgb, adaptation);
  }

  dt_iop_channelmixer_shared_work_rgb_to_display(work_rgb, work_profile, display_profile, display);
  for(int c = 0; c < 3; c++) display_rgb[c] = display[c];
}

void dt_iop_channelmixer_shared_paint_temperature_slider(GtkWidget *const widget, const float temperature_min,
                                                         const float temperature_max)
{
  dt_bauhaus_slider_clear_stops(widget);

  const float temp_range = temperature_max - temperature_min;
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    const float temperature = temperature_min + stop * temp_range;
    dt_aligned_pixel_t RGB = { 0.f };

    illuminant_CCT_to_RGB(temperature, RGB);
    dt_bauhaus_slider_set_stop(widget, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(widget);
}

static void _paint_RGB_slider_stop(const dt_adaptation_t adaptation,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_order_iccprofile_info_t *const display_profile,
                                   GtkWidget *const widget, const float stop, const float c,
                                   const float r, const float g, const float b)
{
  const float module_color[3]
      = { 0.5f * (c * r + 1.f - r), 0.5f * (c * g + 1.f - g), 0.5f * (c * b + 1.f - b) };
  float display_rgb[3] = { 0.f };

  dt_iop_channelmixer_shared_module_color_to_display(module_color, adaptation, work_profile, display_profile,
                                                     display_rgb);
  dt_bauhaus_slider_set_stop(widget, stop, display_rgb[0], display_rgb[1], display_rgb[2]);
}

void dt_iop_channelmixer_shared_paint_row_sliders(dt_adaptation_t adaptation,
                                                  const dt_iop_order_iccprofile_info_t *const work_profile,
                                                  const dt_iop_order_iccprofile_info_t *const display_profile,
                                                  const float r, const float g, const float b,
                                                  const gboolean normalize, const float row[3],
                                                  GtkWidget *const widgets[3])
{
  dt_aligned_pixel_t RGB = { row[0], row[1], row[2], 0.f };

  if(normalize)
  {
    const float sum = RGB[0] + RGB[1] + RGB[2];
    if(sum != 0.f)
      for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  for(int widget = 0; widget < 3; widget++) dt_bauhaus_slider_clear_stops(widgets[widget]);

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    const float x_r = dt_bauhaus_slider_get_hard_min(widgets[0])
                      + stop * (dt_bauhaus_slider_get_hard_max(widgets[0]) - dt_bauhaus_slider_get_hard_min(widgets[0]));
    const float x_g = dt_bauhaus_slider_get_hard_min(widgets[1])
                      + stop * (dt_bauhaus_slider_get_hard_max(widgets[1]) - dt_bauhaus_slider_get_hard_min(widgets[1]));
    const float x_b = dt_bauhaus_slider_get_hard_min(widgets[2])
                      + stop * (dt_bauhaus_slider_get_hard_max(widgets[2]) - dt_bauhaus_slider_get_hard_min(widgets[2]));

    _paint_RGB_slider_stop(adaptation, work_profile, display_profile, widgets[0], stop, x_r + RGB[1] + RGB[2],
                           r, g, b);
    _paint_RGB_slider_stop(adaptation, work_profile, display_profile, widgets[1], stop, RGB[0] + x_g + RGB[2],
                           r, g, b);
    _paint_RGB_slider_stop(adaptation, work_profile, display_profile, widgets[2], stop, RGB[0] + RGB[1] + x_b,
                           r, g, b);
  }

  for(int widget = 0; widget < 3; widget++) gtk_widget_queue_draw(widgets[widget]);
}

static void _shared_paint_probe_matrix(const float source[3], const float M[3][3], float module_color[3])
{
  dt_colormatrix_t mix = { { 0.f } };
  dt_aligned_pixel_t padded_source = { source[0], source[1], source[2], 0.f };
  dt_aligned_pixel_t padded_color = { 0.f };

  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      mix[row][col] = M[row][col];

  dot_product(padded_source, mix, padded_color);
  for(int c = 0; c < 3; c++) module_color[c] = padded_color[c];
}

/**
 * @brief Build a stable hue cue in the fixed chroma basis for hue-like sliders.
 *
 * Some simple-mode parameters become ineffective for degenerate mixer states,
 * for example when the chroma stretches collapse or when the achromatic
 * coupling amount is zero. In those cases, effect-based slider painting can
 * legitimately converge to grey and the hue ramp disappears. This helper keeps
 * a readable hue cue in reserve by projecting a constant-chroma vector from the
 * fixed simple-mode basis into module RGB coordinates.
 *
 * @param[in] hue Angular direction in the chroma plane.
 * @param[out] module_color Probe color in module RGB coordinates.
 */
static void _shared_simple_hue_probe(const float hue, float module_color[3])
{
  static const float P[3][3]
      = { { 0.7071067811865475f,  0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { -0.7071067811865475f, 0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
          { 0.f,                 -0.8164965809277261f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 } };
  const float basis[3]
      = { DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * cosf(hue),
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * sinf(hue), 1.f };

  for(int row = 0; row < 3; row++)
  {
    module_color[row] = 0.f;
    for(int col = 0; col < 3; col++) module_color[row] += P[row][col] * basis[col];
  }
}

void dt_iop_channelmixer_shared_paint_simple_sliders(
    const dt_adaptation_t adaptation, const dt_iop_order_iccprofile_info_t *const work_profile,
    const dt_iop_order_iccprofile_info_t *const display_profile,
    const dt_iop_channelmixer_shared_simple_params_t *const simple, GtkWidget *const widgets[6])
{
  static const dt_iop_channelmixer_shared_simple_probe_t probes[6]
      = { DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_ROTATION,
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_ROTATION,
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_1,
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_2,
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_1,
          DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_2 };

  for(int widget = 0; widget < 6; widget++) dt_bauhaus_slider_clear_stops(widgets[widget]);

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    const float slider = 2.f * stop - 1.f;
    const float stretch_slider = 3.f * stop - 1.5f;

    for(int widget = 0; widget < 6; widget++)
    {
      dt_iop_channelmixer_shared_simple_params_t probe_simple = *simple;
      float source[3] = { 0.f };
      float M[3][3] = { { 0.f } };
      float module_color[3] = { 0.f };
      float display_rgb[3] = { 0.f };
      float coupling_hue = simple->coupling_hue;

      switch(widget)
      {
        case 0:
          probe_simple.theta = slider * (float)M_PI;
          break;
        case 1:
          probe_simple.psi = slider * (float)M_PI_2;
          break;
        case 2:
          probe_simple.stretch_1 = dt_iop_channelmixer_shared_decode_simple_stretch(stretch_slider);
          break;
        case 3:
          probe_simple.stretch_2 = dt_iop_channelmixer_shared_decode_simple_stretch(stretch_slider);
          break;
        case 4:
          probe_simple.coupling_amount = dt_iop_channelmixer_shared_decode_simple_coupling_amount(stop);
          break;
        case 5:
          probe_simple.coupling_hue = slider * (float)M_PI;
          coupling_hue = probe_simple.coupling_hue;
          break;
        default:
          break;
      }

      dt_iop_channelmixer_shared_simple_to_matrix(&probe_simple, M);
      if(widget < 4)
        dt_iop_channelmixer_shared_simple_probe_source(probes[widget], source);
      else
      {
        static const float P[3][3]
            = { { 0.7071067811865475f,  0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
                { -0.7071067811865475f, 0.4082482904638631f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 },
                { 0.f,                 -0.8164965809277261f, DT_IOP_CHANNELMIXER_SHARED_INV_SQRT_3 } };
        const float basis[3]
            = { DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * cosf(coupling_hue),
                DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE * sinf(coupling_hue), 1.f };

        for(int row = 0; row < 3; row++)
        {
          source[row] = 0.f;
          for(int col = 0; col < 3; col++) source[row] += P[row][col] * basis[col];
        }
      }

      _shared_paint_probe_matrix(source, M, module_color);
      dt_iop_channelmixer_shared_module_color_to_display(module_color, adaptation, work_profile, display_profile,
                                                         display_rgb);

      if((widget == 0 || widget == 5)
         && fabsf(fmaxf(fmaxf(display_rgb[0], display_rgb[1]), display_rgb[2])
                  - fminf(fminf(display_rgb[0], display_rgb[1]), display_rgb[2])) < 0.05f)
      {
        const float hue = widget == 0 ? probe_simple.theta : coupling_hue;
        _shared_simple_hue_probe(hue, module_color);
        dt_iop_channelmixer_shared_module_color_to_display(module_color, adaptation, work_profile, display_profile,
                                                           display_rgb);
      }

      dt_bauhaus_slider_set_stop(widgets[widget], stop, display_rgb[0], display_rgb[1], display_rgb[2]);
    }
  }

  for(int widget = 0; widget < 6; widget++) gtk_widget_queue_draw(widgets[widget]);
}

static void _shared_primaries_probe_color(const float M[3][3], const int widget_index, float module_color[3])
{
  if(widget_index <= 1 || widget_index == 8)
  {
    for(int row = 0; row < 3; row++)
      module_color[row] = M[row][0] + M[row][1] + M[row][2];
    return;
  }

  const int column = widget_index <= 3 ? 0 : widget_index <= 5 ? 1 : 2;
  for(int row = 0; row < 3; row++) module_color[row] = M[row][column];
}

void dt_iop_channelmixer_shared_paint_primaries_sliders(
    const dt_adaptation_t adaptation, const dt_iop_order_iccprofile_info_t *const work_profile,
    const dt_iop_order_iccprofile_info_t *const display_profile,
    const dt_iop_channelmixer_shared_primaries_basis_t basis,
    const dt_iop_channelmixer_shared_primaries_params_t *const primaries, GtkWidget *const widgets[9])
{
  for(int widget = 0; widget < 9; widget++) dt_bauhaus_slider_clear_stops(widgets[widget]);

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);

    for(int widget = 0; widget < 9; widget++)
    {
      dt_iop_channelmixer_shared_primaries_params_t probe = *primaries;
      float M[3][3] = { { 0.f } };
      float module_color[3] = { 0.f };
      float display_rgb[3] = { 0.f };
      const float hard_min = dt_bauhaus_slider_get_hard_min(widgets[widget]);
      const float hard_max = dt_bauhaus_slider_get_hard_max(widgets[widget]);
      const float value = hard_min + stop * (hard_max - hard_min);

      switch(widget)
      {
        case 0:
          probe.achromatic_hue = value * (float)M_PI_2;
          break;
        case 1:
          probe.achromatic_purity = value;
          break;
        case 2:
          probe.red_hue = value * (float)M_PI_2;
          break;
        case 3:
          probe.red_purity = value;
          break;
        case 4:
          probe.green_hue = value * (float)M_PI_2;
          break;
        case 5:
          probe.green_purity = value;
          break;
        case 6:
          probe.blue_hue = value * (float)M_PI_2;
          break;
        case 7:
          probe.blue_purity = value;
          break;
        case 8:
          probe.gain = value;
          break;
        default:
          break;
      }

      if(!dt_iop_channelmixer_shared_primaries_to_matrix(basis, &probe, M)) continue;

      _shared_primaries_probe_color(M, widget, module_color);
      dt_iop_channelmixer_shared_module_color_to_display(module_color, adaptation, work_profile, display_profile,
                                                         display_rgb);
      dt_bauhaus_slider_set_stop(widgets[widget], stop, display_rgb[0], display_rgb[1], display_rgb[2]);
    }
  }

  for(int widget = 0; widget < 9; widget++) gtk_widget_queue_draw(widgets[widget]);
}
