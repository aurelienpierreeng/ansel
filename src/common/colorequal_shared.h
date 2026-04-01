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

#pragma once

#include "common/dtpthread.h"
#include "common/iop_profile.h"
#include "common/lut3d.h"

/**
 * We keep the same GUI hue convention as the original color equalizer so the
 * red primary sits at the left edge of the hue graphs.
 */
#define DT_COLORRINGS_ANGLE_SHIFT +20.f

#define DT_COLORRINGS_NUM_RINGS 3
#define DT_COLORRINGS_MAXNODES 20
#define DT_COLORRINGS_DEFAULT_NODES 8
#define DT_COLORRINGS_HUE_SAMPLES 64
#define DT_COLORRINGS_CLUT_LEVEL 64
#define DT_COLORRINGS_LOCAL_FIELD_RINGS (DT_COLORRINGS_NUM_RINGS + 1)

typedef enum dt_colorrings_ring_t
{
  DT_COLORRINGS_RING_DARK = 0,
  DT_COLORRINGS_RING_MID = 1,
  DT_COLORRINGS_RING_LIGHT = 2,
} dt_colorrings_ring_t;

typedef struct dt_colorrings_node_t
{
  float x;
  float y;
} dt_colorrings_node_t;

typedef struct dt_colorrings_sparse_anchor_t
{
  float L;
  float rho;
  float theta;
  float delta_L;
  float chroma_scale;
  float delta_theta;
  float weight;
} dt_colorrings_sparse_anchor_t;

float dt_colorrings_graph_white(void);
float dt_colorrings_wrap_hue_2pi(float hue);
float dt_colorrings_wrap_hue_pi(float hue);
float dt_colorrings_curve_x_to_hue(float x);
float dt_colorrings_hue_to_curve_x(float hue);
float dt_colorrings_curve_periodic_distance(float x0, float x1);
float dt_colorrings_ring_brightness(dt_colorrings_ring_t ring);
float dt_colorrings_curve_periodic_sample(const dt_colorrings_node_t *curve, int nodes, float x);

gboolean dt_colorrings_apply_rgb_lut(const dt_aligned_pixel_t input_rgb, float white_level,
                                     const dt_iop_order_iccprofile_info_t *work_profile,
                                     const dt_iop_order_iccprofile_info_t *lut_profile, const float *clut,
                                     uint16_t clut_level, dt_pthread_rwlock_t *clut_lock,
                                     dt_lut3d_interpolation_t interpolation, dt_aligned_pixel_t output_rgb);

void dt_colorrings_hsb_to_profile_rgb(const dt_aligned_pixel_t HSB, float white,
                                      const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t RGB);
void dt_colorrings_hsb_to_display_rgb(const dt_aligned_pixel_t HSB, float white,
                                      const dt_iop_order_iccprofile_info_t *display_profile,
                                      dt_aligned_pixel_t RGB);
void dt_colorrings_profile_rgb_to_display_rgb(const dt_aligned_pixel_t RGB,
                                              const dt_iop_order_iccprofile_info_t *profile,
                                              const dt_iop_order_iccprofile_info_t *display_profile,
                                              dt_aligned_pixel_t display_rgb);
void dt_colorrings_profile_rgb_to_dt_ucs_hsb(const dt_aligned_pixel_t RGB, float white,
                                             const dt_iop_order_iccprofile_info_t *profile,
                                             dt_aligned_pixel_t HSB);
void dt_colorrings_profile_rgb_to_dt_ucs_jch(const dt_aligned_pixel_t RGB, float white,
                                             const dt_iop_order_iccprofile_info_t *profile,
                                             dt_aligned_pixel_t JCH);
void dt_colorrings_profile_rgb_to_Ych(const dt_aligned_pixel_t RGB, const dt_iop_order_iccprofile_info_t *profile,
                                      dt_aligned_pixel_t Ych);

void dt_colorrings_compute_reference_saturations(float white, float reference_saturation[DT_COLORRINGS_NUM_RINGS]);

float dt_colorrings_ring_axis_position_from_brightness(float brightness, float white,
                                                       const dt_iop_order_iccprofile_info_t *profile);
void dt_colorrings_brightness_to_axis_rgb(float brightness, float white,
                                          const dt_iop_order_iccprofile_info_t *profile, dt_aligned_pixel_t RGB);
float dt_colorrings_distance_to_cube_shell(const dt_aligned_pixel_t axis, const dt_aligned_pixel_t direction);
void dt_colorrings_project_to_cube_shell(const dt_aligned_pixel_t axis, dt_aligned_pixel_t RGB);

float dt_colorrings_vector_norm3(const dt_aligned_pixel_t vector);
float dt_colorrings_dot3(const dt_aligned_pixel_t a, const dt_aligned_pixel_t b);
void dt_colorrings_cross3(const dt_aligned_pixel_t a, const dt_aligned_pixel_t b, dt_aligned_pixel_t out);
void dt_colorrings_normalize3(dt_aligned_pixel_t vector);
void dt_colorrings_rotate_around_axis(const dt_aligned_pixel_t input, const dt_aligned_pixel_t axis,
                                      float cos_angle, float sin_angle, dt_aligned_pixel_t output);

void dt_colorrings_rgb_to_gray_cyl(const float rgb[3], float *L, float *rho, float *theta);
void dt_colorrings_gray_basis_to_rgb(float L, float u, float v, float rgb[3]);
void dt_colorrings_gray_axis_rgb_from_L(float L, dt_aligned_pixel_t RGB);
float dt_colorrings_wendland_c2(float d);
float dt_colorrings_wrap_pi(float x);

void dt_colorrings_eval_local_field(
    const float x[3], const float anchor_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_rho[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float chroma_scale[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES], float inv_sigma_L,
    float inv_sigma_rho, float inv_sigma_theta, float rho0, float out[3]);

void dt_colorrings_fill_lut_local_field(
    float *lut, int level, const float anchor_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_rho[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float anchor_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_L[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float chroma_scale[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES],
    const float delta_theta[DT_COLORRINGS_LOCAL_FIELD_RINGS][DT_COLORRINGS_HUE_SAMPLES], float inv_sigma_L,
    float inv_sigma_rho, float inv_sigma_theta, float rho0);

void dt_colorrings_eval_sparse_local_field(const float x[3], const dt_colorrings_sparse_anchor_t *anchors,
                                           int anchor_count, float inv_sigma_L, float inv_sigma_rho,
                                           float inv_sigma_theta, float rho0, float out[3]);

void dt_colorrings_fill_lut_sparse_local_field(float *lut, int level, const dt_colorrings_sparse_anchor_t *anchors,
                                               int anchor_count, float inv_sigma_L, float inv_sigma_rho,
                                               float inv_sigma_theta, float rho0);
