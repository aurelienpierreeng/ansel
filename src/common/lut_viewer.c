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

#include "common/lut_viewer.h"

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/matrices.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DT_LUT_VIEWER_MARGIN DT_PIXEL_APPLY_DPI(12)
#define DT_LUT_VIEWER_TARGET_SAMPLES 4096
#define DT_LUT_VIEWER_AXIS_LENGTH DT_PIXEL_APPLY_DPI(20.f)

typedef enum dt_lut_viewer_gamut_t
{
  DT_LUT_VIEWER_GAMUT_SRGB = 0,
  DT_LUT_VIEWER_GAMUT_ADOBE_RGB = 1,
  DT_LUT_VIEWER_GAMUT_DISPLAY_P3 = 2,
  DT_LUT_VIEWER_GAMUT_REC2020 = 3
} dt_lut_viewer_gamut_t;

typedef struct dt_lut_viewer_projection_t
{
  dt_aligned_pixel_simd_t screen_x;
  dt_aligned_pixel_simd_t screen_y;
  dt_aligned_pixel_simd_t screen_z;
  float min_x;
  float max_x;
  float min_y;
  float max_y;
  float min_depth;
  float max_depth;
  float scale;
  float offset_x;
  float offset_y;
  float slice_depth;
  float slice_half_thickness;
} dt_lut_viewer_projection_t;

typedef enum dt_lut_viewer_drag_mode_t
{
  DT_LUT_VIEWER_DRAG_NONE = 0,
  DT_LUT_VIEWER_DRAG_PAN = 1,
  DT_LUT_VIEWER_DRAG_ORBIT = 2
} dt_lut_viewer_drag_mode_t;

struct dt_lut_viewer_t
{
  GtkWidget *widget;
  GtkDrawingArea *area;
  GtkWidget *controls;
  GtkWidget *save_button;
  GtkWidget *rotation_around_axis;
  GtkWidget *rotation_of_axis;
  GtkWidget *slice_depth;
  GtkWidget *slice_thickness;
  GtkWidget *shift_threshold;
  GtkWidget *show_control_nodes;
  GtkWidget *gamut;

  const float *clut;
  uint16_t clut_level;
  dt_pthread_rwlock_t *clut_lock;
  const dt_iop_order_iccprofile_info_t *lut_profile;
  const dt_iop_order_iccprofile_info_t *display_profile;
  const dt_lut_viewer_control_node_t *control_nodes;
  size_t control_node_count;
  float zoom;
  float pan_x;
  float pan_y;
  dt_lut_viewer_drag_mode_t drag_mode;
  double drag_anchor_x;
  double drag_anchor_y;
  float drag_origin_pan_x;
  float drag_origin_pan_y;
  float drag_origin_azimuth;
  float drag_origin_tilt;

  int cached_width;
  int cached_height;
  float cached_rotation_around_axis;
  float cached_rotation_of_axis;
  float cached_slice_depth;
  float cached_slice_thickness;
  float cached_zoom;
  float cached_pan_x;
  float cached_pan_y;
  float cached_shift_threshold;
  int cached_gamut;
  const float *cached_clut;
  uint16_t cached_clut_level;
  const dt_iop_order_iccprofile_info_t *cached_lut_profile;
  const dt_iop_order_iccprofile_info_t *cached_display_profile;
  const dt_lut_viewer_control_node_t *cached_control_nodes;
  size_t cached_control_node_count;
  gboolean cached_show_control_nodes;
  double cached_ppd;
  cairo_surface_t *surface;

  dt_aligned_pixel_simd_t *sample_input_work;
  dt_aligned_pixel_simd_t *sample_output_work;
  dt_aligned_pixel_simd_t *sample_input_display;
  dt_aligned_pixel_simd_t *sample_output_display;
  size_t sample_capacity;
  size_t sample_count;
  size_t sample_white_index;
  gboolean sample_draw_white_last;
  gboolean sample_cache_valid;
  float sample_cache_rotation_around_axis;
  float sample_cache_rotation_of_axis;
  float sample_cache_slice_depth;
  float sample_cache_slice_thickness;
  float sample_cache_shift_threshold;
  int sample_cache_gamut;
  const float *sample_cache_clut;
  uint16_t sample_cache_clut_level;
  const dt_iop_order_iccprofile_info_t *sample_cache_lut_profile;
  const dt_iop_order_iccprofile_info_t *sample_cache_display_profile;
  const dt_lut_viewer_control_node_t *sample_cache_control_nodes;
  size_t sample_cache_control_node_count;
  gboolean sample_cache_show_control_nodes;
};

static inline dt_aligned_pixel_simd_t _set_vector(const float x, const float y, const float z)
{
  return (dt_aligned_pixel_simd_t){ x, y, z, 0.f };
}

static inline float _dot3(const dt_aligned_pixel_simd_t a, const dt_aligned_pixel_simd_t b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline dt_aligned_pixel_simd_t _cross3(const dt_aligned_pixel_simd_t a, const dt_aligned_pixel_simd_t b)
{
  return _set_vector(a[1] * b[2] - a[2] * b[1],
                     a[2] * b[0] - a[0] * b[2],
                     a[0] * b[1] - a[1] * b[0]);
}

static inline dt_aligned_pixel_simd_t _normalize3(const dt_aligned_pixel_simd_t vector)
{
  const float norm = sqrtf(_dot3(vector, vector));
  if(norm < 1e-6f) return vector;

  const float inv_norm = 1.f / norm;
  return (dt_aligned_pixel_simd_t){ vector[0] * inv_norm, vector[1] * inv_norm, vector[2] * inv_norm, 0.f };
}

static inline float _wrap_degrees_pm180(float angle)
{
  while(angle <= -180.f) angle += 360.f;
  while(angle > 180.f) angle -= 360.f;
  return angle;
}

static inline float _shift_distance_percent(const dt_aligned_pixel_simd_t input_rgb,
                                            const dt_aligned_pixel_simd_t output_rgb)
{
  const float dr = output_rgb[0] - input_rgb[0];
  const float dg = output_rgb[1] - input_rgb[1];
  const float db = output_rgb[2] - input_rgb[2];
  return 100.f * sqrtf((dr * dr + dg * dg + db * db) / 3.f);
}

static inline dt_aligned_pixel_simd_t _clamp01_simd(const dt_aligned_pixel_simd_t value)
{
  dt_aligned_pixel_simd_t out = dt_simd_max_zero(value);

  for(int c = 0; c < 3; c++)
    out[c] = fminf(out[c], 1.f);

  out[3] = 0.f;
  return out;
}

/**
 * The swatch colors are prepared in arrays before drawing, so using the SIMD
 * pixel type here keeps the repetitive clamp/copy stage cheap even when the
 * actual profile transform remains delegated to the ICC pipeline.
 */
static inline void _clamp_display_rgb_array_simd(const dt_aligned_pixel_simd_t *source_rgb,
                                                 dt_aligned_pixel_simd_t *display_rgb, const size_t count)
{
  if(IS_NULL_PTR(source_rgb) || !display_rgb || count == 0) return;
  __OMP_SIMD__(aligned(source_rgb, display_rgb:16))
  for(size_t k = 0; k < count; k++)
    display_rgb[k] = _clamp01_simd(source_rgb[k]);
}

/**
 * Cairo paints into the display-referred GUI surface, while the CLUT samples
 * live in the LUT internal RGB colorspace. Converting every swatch through the active
 * display profile keeps the overlay faithful to the actual preview colors
 * without changing the geometric coordinates, which must stay in LUT RGB.
 */
static inline dt_aligned_pixel_simd_t _transform_single_rgb_matrix(
  const dt_aligned_pixel_simd_t input,
  const dt_iop_order_iccprofile_info_t *const profile_info_from,
  const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  dt_aligned_pixel_t xyz = { 0.f };
  dt_aligned_pixel_t output = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz((const float *)&input, xyz, profile_info_from->matrix_in_transposed,
                             profile_info_from->lut_in, profile_info_from->unbounded_coeffs_in,
                             profile_info_from->lutsize, profile_info_from->nonlinearlut);

  if(profile_info_to->nonlinearlut)
  {
    dt_aligned_pixel_t linear_rgb = { 0.f };
    dt_apply_transposed_color_matrix(xyz, profile_info_to->matrix_out_transposed, linear_rgb);
    _apply_trc(linear_rgb, output, profile_info_to->lut_out, profile_info_to->unbounded_coeffs_out,
               profile_info_to->lutsize);
  }
  else
    dt_apply_transposed_color_matrix(xyz, profile_info_to->matrix_out_transposed, output);

  return _clamp01_simd(dt_load_simd_aligned(output));
}

static inline dt_aligned_pixel_simd_t _to_display_rgb(const dt_lut_viewer_t *viewer,
                                                      const dt_aligned_pixel_simd_t work_rgb)
{
  dt_aligned_pixel_simd_t display_rgb = _clamp01_simd(work_rgb);

  if(IS_NULL_PTR(viewer->lut_profile) || !viewer->display_profile) return display_rgb;

  if(!isnan(viewer->lut_profile->matrix_in[0][0]) && !isnan(viewer->lut_profile->matrix_out[0][0])
     && !isnan(viewer->display_profile->matrix_in[0][0]) && !isnan(viewer->display_profile->matrix_out[0][0]))
    return _transform_single_rgb_matrix(work_rgb, viewer->lut_profile, viewer->display_profile);

  dt_aligned_pixel_simd_t in = work_rgb;
  dt_aligned_pixel_simd_t out = work_rgb;
  dt_ioppr_transform_image_colorspace_rgb((float *)&in, (float *)&out, 1, 1, viewer->lut_profile, viewer->display_profile,
                                          "lut viewer swatch");

  return _clamp01_simd(out);
}

static inline void _to_display_rgb_array(const dt_lut_viewer_t *viewer, const dt_aligned_pixel_simd_t *work_rgb,
                                         dt_aligned_pixel_simd_t *display_rgb, const size_t count, const char *message)
{
  if(IS_NULL_PTR(work_rgb) || !display_rgb || count == 0) return;

  _clamp_display_rgb_array_simd(work_rgb, display_rgb, count);
  if(IS_NULL_PTR(viewer->lut_profile) || !viewer->display_profile) return;

  dt_ioppr_transform_image_colorspace_rgb((float *)work_rgb, (float *)display_rgb, (int)count, 1,
                                          viewer->lut_profile, viewer->display_profile, message);
  _clamp_display_rgb_array_simd(display_rgb, display_rgb, count);
}

static void _invalidate_surface(dt_lut_viewer_t *viewer)
{
  if(viewer->surface)
  {
    cairo_surface_destroy(viewer->surface);
    viewer->surface = NULL;
  }
}

static void _invalidate_sample_cache(dt_lut_viewer_t *viewer)
{
  viewer->sample_cache_valid = FALSE;
  viewer->sample_count = 0;
  viewer->sample_white_index = 0;
  viewer->sample_draw_white_last = FALSE;
}

static inline gboolean _show_control_nodes(const dt_lut_viewer_t *viewer)
{
  return viewer && viewer->show_control_nodes
         && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(viewer->show_control_nodes));
}

static int _ensure_sample_cache_capacity(dt_lut_viewer_t *viewer, const size_t capacity)
{
  if(viewer->sample_capacity >= capacity) return 0;

  dt_free_align(viewer->sample_input_work);
  dt_free_align(viewer->sample_output_work);
  dt_free_align(viewer->sample_input_display);
  dt_free_align(viewer->sample_output_display);
  viewer->sample_input_work = dt_calloc_align(capacity * sizeof(dt_aligned_pixel_simd_t));
  viewer->sample_output_work = dt_calloc_align(capacity * sizeof(dt_aligned_pixel_simd_t));
  viewer->sample_input_display = dt_calloc_align(capacity * sizeof(dt_aligned_pixel_simd_t));
  viewer->sample_output_display = dt_calloc_align(capacity * sizeof(dt_aligned_pixel_simd_t));

  if(IS_NULL_PTR(viewer->sample_input_work) || IS_NULL_PTR(viewer->sample_output_work) || !viewer->sample_input_display
     || !viewer->sample_output_display)
  {
    dt_free_align(viewer->sample_input_work);
    dt_free_align(viewer->sample_output_work);
    dt_free_align(viewer->sample_input_display);
    dt_free_align(viewer->sample_output_display);
    viewer->sample_input_work = NULL;
    viewer->sample_output_work = NULL;
    viewer->sample_input_display = NULL;
    viewer->sample_output_display = NULL;
    viewer->sample_capacity = 0;
    _invalidate_sample_cache(viewer);
    return 1;
  }

  viewer->sample_capacity = capacity;
  return 0;
}

static inline dt_colorspaces_color_profile_type_t _gamut_to_profile_type(const dt_lut_viewer_gamut_t gamut)
{
  switch(gamut)
  {
    case DT_LUT_VIEWER_GAMUT_ADOBE_RGB:
      return DT_COLORSPACE_ADOBERGB;
    case DT_LUT_VIEWER_GAMUT_DISPLAY_P3:
      return DT_COLORSPACE_DISPLAY_P3;
    case DT_LUT_VIEWER_GAMUT_REC2020:
      return DT_COLORSPACE_LIN_REC2020;
    case DT_LUT_VIEWER_GAMUT_SRGB:
    default:
      return DT_COLORSPACE_SRGB;
  }
}

static inline gboolean _gamut_matches_lut_profile(const dt_lut_viewer_t *viewer,
                                                  const dt_lut_viewer_gamut_t gamut)
{
  if(IS_NULL_PTR(viewer->lut_profile)) return FALSE;

  switch(gamut)
  {
    case DT_LUT_VIEWER_GAMUT_SRGB:
      return viewer->lut_profile->type == DT_COLORSPACE_SRGB
             || viewer->lut_profile->type == DT_COLORSPACE_REC709
             || viewer->lut_profile->type == DT_COLORSPACE_LIN_REC709;
    case DT_LUT_VIEWER_GAMUT_ADOBE_RGB:
      return viewer->lut_profile->type == DT_COLORSPACE_ADOBERGB;
    case DT_LUT_VIEWER_GAMUT_DISPLAY_P3:
      return viewer->lut_profile->type == DT_COLORSPACE_DISPLAY_P3
             || viewer->lut_profile->type == DT_COLORSPACE_HLG_P3
             || viewer->lut_profile->type == DT_COLORSPACE_PQ_P3;
    case DT_LUT_VIEWER_GAMUT_REC2020:
      return viewer->lut_profile->type == DT_COLORSPACE_LIN_REC2020
             || viewer->lut_profile->type == DT_COLORSPACE_HLG_REC2020
             || viewer->lut_profile->type == DT_COLORSPACE_PQ_REC2020;
    default:
      return FALSE;
  }
}

/**
 * The gamut selector operates in the ICC PCS of the application pipeline,
 * namely XYZ D50. Reading the colorant tags from the selected RGB profile gives
 * us the RGB -> XYZ matrix of that gamut in the same PCS, so inverting it lets
 * us test whether a LUT-space RGB point belongs to the selected gamut without
 * adding another transform stage.
 */
static int _get_xyz_to_rgb_matrix(const dt_lut_viewer_gamut_t gamut, dt_colormatrix_t xyz_to_rgb)
{
  const dt_colorspaces_color_profile_t *profile
      = dt_colorspaces_get_profile(_gamut_to_profile_type(gamut), "", DT_PROFILE_DIRECTION_ANY);
  if(IS_NULL_PTR(profile)|| IS_NULL_PTR(profile->profile)) return 1;

  const cmsCIEXYZ *red = cmsReadTag(profile->profile, cmsSigRedColorantTag);
  const cmsCIEXYZ *green = cmsReadTag(profile->profile, cmsSigGreenColorantTag);
  const cmsCIEXYZ *blue = cmsReadTag(profile->profile, cmsSigBlueColorantTag);
  if(IS_NULL_PTR(red) || IS_NULL_PTR(green) || IS_NULL_PTR(blue)) return 1;

  dt_colormatrix_t rgb_to_xyz = { { 0.f } };
  rgb_to_xyz[0][0] = red->X;
  rgb_to_xyz[0][1] = green->X;
  rgb_to_xyz[0][2] = blue->X;
  rgb_to_xyz[1][0] = red->Y;
  rgb_to_xyz[1][1] = green->Y;
  rgb_to_xyz[1][2] = blue->Y;
  rgb_to_xyz[2][0] = red->Z;
  rgb_to_xyz[2][1] = green->Z;
  rgb_to_xyz[2][2] = blue->Z;

  return mat3SSEinv(xyz_to_rgb, rgb_to_xyz);
}

static inline gboolean _sample_fits_gamut(const dt_iop_order_iccprofile_info_t *lut_profile,
                                          const dt_colormatrix_t xyz_to_rgb,
                                          const dt_aligned_pixel_simd_t rgb)
{
  const float epsilon = 1e-3f;
  dt_aligned_pixel_simd_t xyz = { 0.f };
  dt_aligned_pixel_simd_t gamut_rgb = { 0.f };
  dt_apply_transposed_color_matrix((float *)&rgb, lut_profile->matrix_in_transposed, (float *)&xyz);
  dot_product((float *)&xyz, xyz_to_rgb, (float *)&gamut_rgb);

  /**
   * The viewer geometry lives in LUT-space code values. Gamut filtering must
   * therefore operate on those coordinates directly, without an additional TRC
   * stage that would reinterpret shadows/highlights and distort the node cloud.
   * A small tolerance keeps boundary samples visible despite matrix roundoff.
   */
  return gamut_rgb[0] >= -epsilon && gamut_rgb[0] <= 1.f + epsilon
         && gamut_rgb[1] >= -epsilon && gamut_rgb[1] <= 1.f + epsilon
         && gamut_rgb[2] >= -epsilon && gamut_rgb[2] <= 1.f + epsilon;
}

static void _build_projection(dt_lut_viewer_projection_t *projection, const float rotation_around_axis,
                              const float rotation_of_axis, const float slice_depth, const float slice_thickness,
                              const float zoom,
                              const float pan_x, const float pan_y, const int width, const int height)
{
  static const dt_aligned_pixel_simd_t axis = { 0.5773502691896258f, 0.5773502691896258f, 0.5773502691896258f, 0.f };
  static const dt_aligned_pixel_simd_t chroma_x = { 0.7071067811865475f, -0.7071067811865475f, 0.f, 0.f };
  static const dt_aligned_pixel_simd_t chroma_y = { 0.4082482904638630f, 0.4082482904638630f, -0.8164965809277260f, 0.f };

  const float azimuth = rotation_around_axis * M_PI_F / 180.f;
  const float tilt = rotation_of_axis * M_PI_F / 180.f;
  dt_aligned_pixel_simd_t rotated_x = { 0.f };
  dt_aligned_pixel_simd_t rotated_y = { 0.f };

  for(int c = 0; c < 3; c++)
  {
    rotated_x[c] = cosf(azimuth) * chroma_x[c] + sinf(azimuth) * chroma_y[c];
    rotated_y[c] = -sinf(azimuth) * chroma_x[c] + cosf(azimuth) * chroma_y[c];
    projection->screen_z[c] = cosf(tilt) * rotated_y[c] + sinf(tilt) * axis[c];
    projection->screen_x[c] = rotated_x[c];
  }

  projection->screen_y = _normalize3(_cross3(projection->screen_x, projection->screen_z));
  projection->screen_z = _normalize3(projection->screen_z);

  projection->min_x = INFINITY;
  projection->max_x = -INFINITY;
  projection->min_y = INFINITY;
  projection->max_y = -INFINITY;
  projection->min_depth = INFINITY;
  projection->max_depth = -INFINITY;

  /**
   * The cube bounds define the orthographic framing for every sampled LUT
   * point. Using the eight corners keeps the viewer stable while the CLUT
   * itself deforms inside that fixed RGB domain.
   */
  for(int corner = 0; corner < 8; corner++)
  {
    const dt_aligned_pixel_simd_t centered = {
      ((corner & 1) ? 1.f : 0.f) - 0.5f,
      ((corner & 2) ? 1.f : 0.f) - 0.5f,
      ((corner & 4) ? 1.f : 0.f) - 0.5f,
      0.f
    };

    const float px = _dot3(centered, projection->screen_x);
    const float py = _dot3(centered, projection->screen_y);
    const float pz = _dot3(centered, projection->screen_z);

    projection->min_x = fminf(projection->min_x, px);
    projection->max_x = fmaxf(projection->max_x, px);
    projection->min_y = fminf(projection->min_y, py);
    projection->max_y = fmaxf(projection->max_y, py);
    projection->min_depth = fminf(projection->min_depth, pz);
    projection->max_depth = fmaxf(projection->max_depth, pz);
  }

  const float span_x = fmaxf(projection->max_x - projection->min_x, 1e-3f);
  const float span_y = fmaxf(projection->max_y - projection->min_y, 1e-3f);
  const float available_width = fmaxf((float)width - 2.f * DT_LUT_VIEWER_MARGIN, 1.f);
  const float available_height = fmaxf((float)height - 2.f * DT_LUT_VIEWER_MARGIN, 1.f);

  projection->scale = 0.9f * CLAMP(zoom, 0.25f, 8.f) * fminf(available_width / span_x, available_height / span_y);
  projection->offset_x = 0.5f * ((float)width - projection->scale * (projection->min_x + projection->max_x)) + pan_x;
  projection->offset_y = 0.5f * ((float)height + projection->scale * (projection->min_y + projection->max_y)) + pan_y;
  projection->slice_depth = projection->min_depth
                            + CLAMP(slice_depth, 0.f, 100.f) * 0.01f
                              * (projection->max_depth - projection->min_depth);
  projection->slice_half_thickness = CLAMP(slice_thickness, 0.f, 100.f) * 0.01f
                                     * (projection->max_depth - projection->min_depth);
}

static inline void _project_point(const dt_lut_viewer_projection_t *projection, const dt_aligned_pixel_simd_t rgb,
                                  float *const x, float *const y, float *const depth)
{
  const dt_aligned_pixel_simd_t centered = { rgb[0] - 0.5f, rgb[1] - 0.5f, rgb[2] - 0.5f, 0.f };
  const float px = _dot3(centered, projection->screen_x);
  const float py = _dot3(centered, projection->screen_y);

  *x = projection->offset_x + projection->scale * px;
  *y = projection->offset_y - projection->scale * py;
  *depth = _dot3(centered, projection->screen_z);
}

static inline void _draw_arrow(cairo_t *cr, const float x0, const float y0, const float x1, const float y1,
                               const float radius0, const float radius1, const dt_aligned_pixel_simd_t color)
{
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length = sqrtf(dx * dx + dy * dy);
  if(length < 1e-3f) return;

  const float ux = dx / length;
  const float uy = dy / length;
  const float start_x = x0 + radius0 * ux;
  const float start_y = y0 + radius0 * uy;
  const float end_x = x1 - radius1 * ux;
  const float end_y = y1 - radius1 * uy;
  const float visible_dx = end_x - start_x;
  const float visible_dy = end_y - start_y;
  const float visible_length = sqrtf(visible_dx * visible_dx + visible_dy * visible_dy);
  if(visible_length < 1e-3f) return;

  cairo_set_source_rgba(cr, color[0], color[1], color[2], 0.65);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.8f));
  cairo_move_to(cr, start_x, start_y);
  cairo_line_to(cr, end_x, end_y);
  cairo_stroke(cr);

  const float head = DT_PIXEL_APPLY_DPI(8.f);
  const float nx = -uy;
  const float ny = ux;

  cairo_move_to(cr, end_x, end_y);
  cairo_line_to(cr, end_x - head * ux + 0.5f * head * nx, end_y - head * uy + 0.5f * head * ny);
  cairo_line_to(cr, end_x - head * ux - 0.5f * head * nx, end_y - head * uy - 0.5f * head * ny);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, color[0], color[1], color[2], 0.75);
  cairo_fill(cr);
}

static void _draw_cube(cairo_t *cr, const dt_lut_viewer_t *viewer, const dt_lut_viewer_projection_t *projection)
{
  float x[8] = { 0.f };
  float y[8] = { 0.f };
  float depth = 0.f;

  for(int corner = 0; corner < 8; corner++)
  {
    const dt_aligned_pixel_simd_t rgb = {
      (float)((corner & 1) != 0),
      (float)((corner & 2) != 0),
      (float)((corner & 4) != 0),
      0.f
    };
    _project_point(projection, rgb, &x[corner], &y[corner], &depth);
  }

  set_color(cr, darktable.bauhaus->graph_border);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.f));

  for(int corner = 0; corner < 8; corner++)
  {
    for(int axis = 0; axis < 3; axis++)
    {
      const int other = corner ^ (1 << axis);
      if(other < corner) continue;

      cairo_move_to(cr, x[corner], y[corner]);
      cairo_line_to(cr, x[other], y[other]);
      cairo_stroke(cr);
    }
  }

  const dt_aligned_pixel_simd_t black = { 0.f, 0.f, 0.f, 0.f };
  const dt_aligned_pixel_simd_t white = { 1.f, 1.f, 1.f, 0.f };
  float x0 = 0.f, y0 = 0.f;
  float x1 = 0.f, y1 = 0.f;
  dt_aligned_pixel_simd_t display_rgb = { 0.f };
  cairo_pattern_t *gradient = NULL;
  _project_point(projection, black, &x0, &y0, &depth);
  _project_point(projection, white, &x1, &y1, &depth);

  /**
   * The achromatic diagonal is a geometric axis of the RGB cube, so painting
   * it with a black-to-white gradient gives a direct depth cue for where the
   * neutral values sit inside the current 3D orientation.
   */
  gradient = cairo_pattern_create_linear(x0, y0, x1, y1);
  for(int k = 0; k <= 16; k++)
  {
    const float grey = (float)k / 16.f;
    display_rgb = _to_display_rgb(viewer, (dt_aligned_pixel_simd_t){ grey, grey, grey, 0.f });
    cairo_pattern_add_color_stop_rgba(gradient, grey, display_rgb[0], display_rgb[1], display_rgb[2], 0.85);
  }

  cairo_set_source(cr, gradient);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.f));
  cairo_move_to(cr, x0, y0);
  cairo_line_to(cr, x1, y1);
  cairo_stroke(cr);
  cairo_pattern_destroy(gradient);
}

static void _draw_axes(cairo_t *cr, const dt_lut_viewer_t *viewer, const dt_lut_viewer_projection_t *projection)
{
  static const dt_aligned_pixel_simd_t center = { 0.5f, 0.5f, 0.5f, 0.f };
  const dt_aligned_pixel_simd_t red = { 0.5f + DT_LUT_VIEWER_AXIS_LENGTH / projection->scale, 0.5f, 0.5f, 0.f };
  const dt_aligned_pixel_simd_t green = { 0.5f, 0.5f + DT_LUT_VIEWER_AXIS_LENGTH / projection->scale, 0.5f, 0.f };
  const dt_aligned_pixel_simd_t blue = { 0.5f, 0.5f, 0.5f + DT_LUT_VIEWER_AXIS_LENGTH / projection->scale, 0.f };
  const dt_aligned_pixel_simd_t axis_work[3] = {
    { 1.f, 0.f, 0.f, 0.f },
    { 0.f, 1.f, 0.f, 0.f },
    { 0.f, 0.f, 1.f, 0.f }
  };
  dt_aligned_pixel_simd_t axis_display[3] = { { 0.f }, { 0.f }, { 0.f } };
  float cx = 0.f, cy = 0.f, depth = 0.f;
  float px = 0.f, py = 0.f;

  _project_point(projection, center, &cx, &cy, &depth);
  for(int k = 0; k < 3; k++) axis_display[k] = _to_display_rgb(viewer, axis_work[k]);

  _project_point(projection, red, &px, &py, &depth);
  cairo_set_source_rgba(cr, axis_display[0][0], axis_display[0][1], axis_display[0][2], 0.9);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.f));
  cairo_move_to(cr, cx, cy);
  cairo_line_to(cr, px, py);
  cairo_stroke(cr);

  _project_point(projection, green, &px, &py, &depth);
  cairo_set_source_rgba(cr, axis_display[1][0], axis_display[1][1], axis_display[1][2], 0.9);
  cairo_move_to(cr, cx, cy);
  cairo_line_to(cr, px, py);
  cairo_stroke(cr);

  _project_point(projection, blue, &px, &py, &depth);
  cairo_set_source_rgba(cr, axis_display[2][0], axis_display[2][1], axis_display[2][2], 0.9);
  cairo_move_to(cr, cx, cy);
  cairo_line_to(cr, px, py);
  cairo_stroke(cr);
}

static int _sample_stride(const int level)
{
  int stride = 1;

  while(((level + stride - 1) / stride) * ((level + stride - 1) / stride) * ((level + stride - 1) / stride)
        > DT_LUT_VIEWER_TARGET_SAMPLES)
    stride++;

  return stride;
}

static inline int _sample_count(const int level, const int stride)
{
  return MAX((level + stride - 1) / stride, 2);
}

static inline int _sample_index(const int sample, const int samples, const int level)
{
  if(samples <= 1) return 0;
  return MIN((int)lroundf((float)sample * (float)(level - 1) / (float)(samples - 1)), level - 1);
}

static void _draw_samples(cairo_t *cr, const dt_lut_viewer_t *viewer,
                          const dt_lut_viewer_projection_t *projection, const dt_lut_viewer_gamut_t gamut)
{
  const gboolean show_control_nodes = _show_control_nodes(viewer);
  if(IS_NULL_PTR(viewer->lut_profile)) return;
  if(show_control_nodes)
  {
    if(!viewer->control_nodes || viewer->control_node_count == 0) return;
  }
  else if(!viewer->clut || viewer->clut_level < 2)
    return;

  dt_lut_viewer_t *mutable_viewer = (dt_lut_viewer_t *)viewer;
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double total_start = log_perf ? dt_get_wtime() : 0.0;
  double collect_done = 0.0;
  double convert_done = 0.0;
  dt_colormatrix_t xyz_to_rgb = { { 0.f } };
  const gboolean same_gamut_as_lut_profile = _gamut_matches_lut_profile(viewer, gamut);
  if(!same_gamut_as_lut_profile && _get_xyz_to_rgb_matrix(gamut, xyz_to_rgb)) return;

  const int stride = show_control_nodes ? 1 : _sample_stride(viewer->clut_level);
  const int samples = show_control_nodes ? 0 : _sample_count(viewer->clut_level, stride);
  const int level2 = viewer->clut_level * viewer->clut_level;
  const size_t max_samples = show_control_nodes ? viewer->control_node_count
                                                : (size_t)samples * (size_t)samples * (size_t)samples;
  const float rotation_around_axis = dt_bauhaus_slider_get(viewer->rotation_around_axis);
  const float rotation_of_axis = dt_bauhaus_slider_get(viewer->rotation_of_axis);
  const float slice_depth = dt_bauhaus_slider_get(viewer->slice_depth);
  const float slice_thickness = dt_bauhaus_slider_get(viewer->slice_thickness);
  const float shift_threshold = dt_bauhaus_slider_get(viewer->shift_threshold);
  const float zoom_scale = sqrtf(fmaxf(viewer->zoom, 0.25f));
  const float target_radius = DT_PIXEL_APPLY_DPI(2.8f) * zoom_scale;
  const float destination_radius = DT_PIXEL_APPLY_DPI(3.4f) * zoom_scale;
  const gboolean rebuild_sample_cache
      = !viewer->sample_cache_valid
        || fabsf(viewer->sample_cache_rotation_around_axis - rotation_around_axis) > 1e-6f
        || fabsf(viewer->sample_cache_rotation_of_axis - rotation_of_axis) > 1e-6f
        || fabsf(viewer->sample_cache_slice_depth - slice_depth) > 1e-6f
        || fabsf(viewer->sample_cache_slice_thickness - slice_thickness) > 1e-6f
        || fabsf(viewer->sample_cache_shift_threshold - shift_threshold) > 1e-6f
        || viewer->sample_cache_gamut != gamut
        || viewer->sample_cache_clut != viewer->clut
        || viewer->sample_cache_clut_level != viewer->clut_level
        || viewer->sample_cache_lut_profile != viewer->lut_profile
        || viewer->sample_cache_display_profile != viewer->display_profile
        || viewer->sample_cache_control_nodes != viewer->control_nodes
        || viewer->sample_cache_control_node_count != viewer->control_node_count
        || viewer->sample_cache_show_control_nodes != show_control_nodes;

  if(rebuild_sample_cache)
  {
    if(_ensure_sample_cache_capacity(mutable_viewer, max_samples)) return;

    mutable_viewer->sample_count = 0;
    mutable_viewer->sample_white_index = 0;
    mutable_viewer->sample_draw_white_last = FALSE;

    if(show_control_nodes)
    {
      for(size_t k = 0; k < viewer->control_node_count; k++)
      {
        const dt_aligned_pixel_simd_t input_rgb = {
          CLAMP(viewer->control_nodes[k].input_rgb[0], 0.f, 1.f),
          CLAMP(viewer->control_nodes[k].input_rgb[1], 0.f, 1.f),
          CLAMP(viewer->control_nodes[k].input_rgb[2], 0.f, 1.f),
          0.f
        };
        const dt_aligned_pixel_simd_t output_rgb = {
          CLAMP(viewer->control_nodes[k].output_rgb[0], 0.f, 1.f),
          CLAMP(viewer->control_nodes[k].output_rgb[1], 0.f, 1.f),
          CLAMP(viewer->control_nodes[k].output_rgb[2], 0.f, 1.f),
          0.f
        };
        if(!same_gamut_as_lut_profile && !_sample_fits_gamut(viewer->lut_profile, xyz_to_rgb, input_rgb)) continue;

        float x0 = 0.f, y0 = 0.f, depth0 = 0.f;
        float x1 = 0.f, y1 = 0.f, depth1 = 0.f;
        _project_point(projection, input_rgb, &x0, &y0, &depth0);
        _project_point(projection, output_rgb, &x1, &y1, &depth1);

        const gboolean target_in_slice = fabsf(depth0 - projection->slice_depth) <= projection->slice_half_thickness;
        const gboolean destination_in_slice
            = fabsf(depth1 - projection->slice_depth) <= projection->slice_half_thickness;
        if(!target_in_slice && !destination_in_slice) continue;

        mutable_viewer->sample_input_work[mutable_viewer->sample_count] = input_rgb;
        mutable_viewer->sample_output_work[mutable_viewer->sample_count] = output_rgb;

        if(input_rgb[0] >= 1.f - 1e-6f && input_rgb[1] >= 1.f - 1e-6f && input_rgb[2] >= 1.f - 1e-6f)
        {
          mutable_viewer->sample_white_index = mutable_viewer->sample_count;
          mutable_viewer->sample_draw_white_last = TRUE;
        }

        mutable_viewer->sample_count++;
      }
    }
    else
    {
      /**
       * We sample the lattice sparsely enough to stay interactive in Cairo while
       * still covering the RGB cube evenly. The selected gamut is applied to the
       * target lattice points so the viewer previews how the LUT deforms the chosen
       * source volume inside the LUT RGB cube.
       */
      /**
       * The sparse Cairo preview must still sample the outer shell of the CLUT.
       * Mapping the reduced lattice back to `[0, level - 1]` guarantees that the
       * last sample of every axis lies exactly on the cube boundary instead of
       * stopping short whenever `stride` does not divide `level - 1`.
       */
      for(int sample_b = 0; sample_b < samples; sample_b++)
        for(int sample_g = 0; sample_g < samples; sample_g++)
          for(int sample_r = 0; sample_r < samples; sample_r++)
          {
            const int b = _sample_index(sample_b, samples, viewer->clut_level);
            const int g = _sample_index(sample_g, samples, viewer->clut_level);
            const int r = _sample_index(sample_r, samples, viewer->clut_level);
            const dt_aligned_pixel_simd_t input_rgb = {
              (float)r / (float)(viewer->clut_level - 1),
              (float)g / (float)(viewer->clut_level - 1),
              (float)b / (float)(viewer->clut_level - 1),
              0.f
            };

            if(!same_gamut_as_lut_profile && !_sample_fits_gamut(viewer->lut_profile, xyz_to_rgb, input_rgb))
              continue;

            const size_t index = (size_t)(r + g * viewer->clut_level + b * level2) * 3;
            const dt_aligned_pixel_simd_t output_rgb = {
              CLAMP(viewer->clut[index + 0], 0.f, 1.f),
              CLAMP(viewer->clut[index + 1], 0.f, 1.f),
              CLAMP(viewer->clut[index + 2], 0.f, 1.f),
              0.f
            };
            if(_shift_distance_percent(input_rgb, output_rgb) < shift_threshold) continue;

            float x0 = 0.f, y0 = 0.f, depth0 = 0.f;
            float x1 = 0.f, y1 = 0.f, depth1 = 0.f;
            _project_point(projection, input_rgb, &x0, &y0, &depth0);
            _project_point(projection, output_rgb, &x1, &y1, &depth1);

            const gboolean target_in_slice
                = fabsf(depth0 - projection->slice_depth) <= projection->slice_half_thickness;
            const gboolean destination_in_slice
                = fabsf(depth1 - projection->slice_depth) <= projection->slice_half_thickness;
            if(!target_in_slice && !destination_in_slice) continue;

            mutable_viewer->sample_input_work[mutable_viewer->sample_count] = input_rgb;
            mutable_viewer->sample_output_work[mutable_viewer->sample_count] = output_rgb;

            if(r == viewer->clut_level - 1 && g == viewer->clut_level - 1 && b == viewer->clut_level - 1)
            {
              mutable_viewer->sample_white_index = mutable_viewer->sample_count;
              mutable_viewer->sample_draw_white_last = TRUE;
            }
            mutable_viewer->sample_count++;
          }
    }

    if(log_perf) collect_done = dt_get_wtime();

    _to_display_rgb_array(viewer, viewer->sample_input_work, viewer->sample_input_display, viewer->sample_count,
                          "lut viewer swatch inputs");
    _to_display_rgb_array(viewer, viewer->sample_output_work, viewer->sample_output_display, viewer->sample_count,
                          "lut viewer swatch outputs");
    if(log_perf) convert_done = dt_get_wtime();

    mutable_viewer->sample_cache_rotation_around_axis = rotation_around_axis;
    mutable_viewer->sample_cache_rotation_of_axis = rotation_of_axis;
    mutable_viewer->sample_cache_slice_depth = slice_depth;
    mutable_viewer->sample_cache_slice_thickness = slice_thickness;
    mutable_viewer->sample_cache_shift_threshold = shift_threshold;
    mutable_viewer->sample_cache_gamut = gamut;
    mutable_viewer->sample_cache_clut = viewer->clut;
    mutable_viewer->sample_cache_clut_level = viewer->clut_level;
    mutable_viewer->sample_cache_lut_profile = viewer->lut_profile;
    mutable_viewer->sample_cache_display_profile = viewer->display_profile;
    mutable_viewer->sample_cache_control_nodes = viewer->control_nodes;
    mutable_viewer->sample_cache_control_node_count = viewer->control_node_count;
    mutable_viewer->sample_cache_show_control_nodes = show_control_nodes;
    mutable_viewer->sample_cache_valid = TRUE;
  }
  else if(log_perf)
  {
    collect_done = total_start;
    convert_done = total_start;
  }

  for(size_t k = 0; k < viewer->sample_count; k++)
  {
    if(viewer->sample_draw_white_last && k == viewer->sample_white_index) continue;

    float x0 = 0.f, y0 = 0.f, depth0 = 0.f;
    float x1 = 0.f, y1 = 0.f, depth1 = 0.f;
    _project_point(projection, viewer->sample_input_work[k], &x0, &y0, &depth0);
    _project_point(projection, viewer->sample_output_work[k], &x1, &y1, &depth1);

    _draw_arrow(cr, x0, y0, x1, y1, target_radius, destination_radius, viewer->sample_output_display[k]);

    cairo_arc(cr, x0, y0, target_radius, 0.f, 2.f * M_PI_F);
    cairo_set_source_rgba(cr, viewer->sample_input_display[k][0], viewer->sample_input_display[k][1],
                          viewer->sample_input_display[k][2], 0.8);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.6);
    cairo_stroke(cr);

    cairo_arc(cr, x1, y1, destination_radius, 0.f, 2.f * M_PI_F);
    cairo_set_source_rgba(cr, viewer->sample_output_display[k][0], viewer->sample_output_display[k][1],
                          viewer->sample_output_display[k][2], 0.9);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
    cairo_stroke(cr);
  }

  if(viewer->sample_draw_white_last)
  {
    const size_t k = viewer->sample_white_index;
    float x0 = 0.f, y0 = 0.f, depth0 = 0.f;
    float x1 = 0.f, y1 = 0.f, depth1 = 0.f;
    _project_point(projection, viewer->sample_input_work[k], &x0, &y0, &depth0);
    _project_point(projection, viewer->sample_output_work[k], &x1, &y1, &depth1);

    _draw_arrow(cr, x0, y0, x1, y1, target_radius, destination_radius, viewer->sample_output_display[k]);

    cairo_arc(cr, x0, y0, target_radius, 0.f, 2.f * M_PI_F);
    cairo_set_source_rgba(cr, viewer->sample_input_display[k][0], viewer->sample_input_display[k][1],
                          viewer->sample_input_display[k][2], 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.8);
    cairo_stroke(cr);

    cairo_arc(cr, x1, y1, destination_radius, 0.f, 2.f * M_PI_F);
    cairo_set_source_rgba(cr, viewer->sample_output_display[k][0], viewer->sample_output_display[k][1],
                          viewer->sample_output_display[k][2], 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.8);
    cairo_stroke(cr);
  }

  if(log_perf)
  {
    const double total_done = dt_get_wtime();
    dt_print(DT_DEBUG_PERF,
             "[lut_viewer] draw_samples mode=%s level=%u sparse=%d^3 max=%zu drawn=%zu gamut=%u cache=%s collect=%.3fms convert=%.3fms paint=%.3fms total=%.3fms\n",
             show_control_nodes ? "controls" : "lut", viewer->clut_level, samples, max_samples, viewer->sample_count, gamut,
             rebuild_sample_cache ? "rebuild" : "reuse",
             1000.0 * (collect_done - total_start),
             1000.0 * (convert_done - collect_done),
             1000.0 * (total_done - convert_done),
             1000.0 * (total_done - total_start));
  }
}

static void _draw_placeholder(cairo_t *cr, const int width, const int height, const char *message)
{
  cairo_text_extents_t extents;
  cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(12.f));
  cairo_text_extents(cr, message, &extents);
  cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.7);
  cairo_move_to(cr, 0.5f * ((float)width - extents.width), 0.5f * ((float)height - extents.height));
  cairo_show_text(cr, message);
}

static void _render_surface(dt_lut_viewer_t *viewer, const int width, const int height)
{
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double start = log_perf ? dt_get_wtime() : 0.0;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  if(viewer->clut_lock) dt_pthread_rwlock_rdlock(viewer->clut_lock);
  _invalidate_surface(viewer);
  viewer->surface = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                  MAX((int)ceil((double)width * ppd), 1),
                                                  MAX((int)ceil((double)height * ppd), 1));
  cairo_surface_set_device_scale(viewer->surface, ppd, ppd);
  cairo_t *cr = cairo_create(viewer->surface);
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(viewer->area));

  gtk_render_background(context, cr, 0, 0, width, height);
  gtk_render_frame(context, cr, 0, 0, width, height);

  if(IS_NULL_PTR(viewer->lut_profile))
  {
    _draw_placeholder(cr, width, height, _("no LUT to display"));
    cairo_destroy(cr);
    if(viewer->clut_lock) dt_pthread_rwlock_unlock(viewer->clut_lock);
    if(log_perf)
      dt_print(DT_DEBUG_PERF,
               "[lut_viewer] render_surface %dx%d ppd=%.2f placeholder=1 total=%.3fms\n",
               width, height, ppd, 1000.0 * (dt_get_wtime() - start));
    return;
  }

  if(_show_control_nodes(viewer))
  {
    if(!viewer->control_nodes || viewer->control_node_count == 0)
    {
      _draw_placeholder(cr, width, height, _("no control nodes to display"));
      cairo_destroy(cr);
      if(viewer->clut_lock) dt_pthread_rwlock_unlock(viewer->clut_lock);
      return;
    }
  }
  else if(!viewer->clut || viewer->clut_level < 2)
  {
    _draw_placeholder(cr, width, height, _("no LUT to display"));
    cairo_destroy(cr);
    if(viewer->clut_lock) dt_pthread_rwlock_unlock(viewer->clut_lock);
    if(log_perf)
      dt_print(DT_DEBUG_PERF,
               "[lut_viewer] render_surface %dx%d ppd=%.2f placeholder=1 total=%.3fms\n",
               width, height, ppd, 1000.0 * (dt_get_wtime() - start));
    return;
  }

  const dt_lut_viewer_gamut_t gamut
      = (dt_lut_viewer_gamut_t)dt_bauhaus_combobox_get(viewer->gamut);
  dt_lut_viewer_projection_t projection;
  _build_projection(&projection,
                    dt_bauhaus_slider_get(viewer->rotation_around_axis),
                    dt_bauhaus_slider_get(viewer->rotation_of_axis),
                    dt_bauhaus_slider_get(viewer->slice_depth),
                    dt_bauhaus_slider_get(viewer->slice_thickness),
                    viewer->zoom,
                    viewer->pan_x,
                    viewer->pan_y,
                    width, height);

  _draw_cube(cr, viewer, &projection);
  _draw_axes(cr, viewer, &projection);
  _draw_samples(cr, viewer, &projection, gamut);
  cairo_destroy(cr);
  if(viewer->clut_lock) dt_pthread_rwlock_unlock(viewer->clut_lock);

  if(log_perf)
    dt_print(DT_DEBUG_PERF,
             "[lut_viewer] render_surface %dx%d ppd=%.2f level=%u gamut=%u total=%.3fms\n",
             width, height, ppd, viewer->clut_level, gamut, 1000.0 * (dt_get_wtime() - start));
}

static gboolean _draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  const float rotation_around_axis = dt_bauhaus_slider_get(viewer->rotation_around_axis);
  const float rotation_of_axis = dt_bauhaus_slider_get(viewer->rotation_of_axis);
  const float slice_depth = dt_bauhaus_slider_get(viewer->slice_depth);
  const float slice_thickness = dt_bauhaus_slider_get(viewer->slice_thickness);
  const float shift_threshold = dt_bauhaus_slider_get(viewer->shift_threshold);
  const float zoom = viewer->zoom;
  const int gamut = dt_bauhaus_combobox_get(viewer->gamut);
  const gboolean show_control_nodes = _show_control_nodes(viewer);
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;

  if(!viewer->surface
     || viewer->cached_width != allocation.width
     || viewer->cached_height != allocation.height
     || fabsf(viewer->cached_rotation_around_axis - rotation_around_axis) > 1e-6f
     || fabsf(viewer->cached_rotation_of_axis - rotation_of_axis) > 1e-6f
     || fabsf(viewer->cached_slice_depth - slice_depth) > 1e-6f
     || fabsf(viewer->cached_slice_thickness - slice_thickness) > 1e-6f
     || fabsf(viewer->cached_shift_threshold - shift_threshold) > 1e-6f
     || fabsf(viewer->cached_zoom - zoom) > 1e-6f
     || fabsf(viewer->cached_pan_x - viewer->pan_x) > 1e-6f
     || fabsf(viewer->cached_pan_y - viewer->pan_y) > 1e-6f
     || viewer->cached_gamut != gamut
     || viewer->cached_clut != viewer->clut
     || viewer->cached_clut_level != viewer->clut_level
     || viewer->cached_lut_profile != viewer->lut_profile
     || viewer->cached_display_profile != viewer->display_profile
     || viewer->cached_control_nodes != viewer->control_nodes
     || viewer->cached_control_node_count != viewer->control_node_count
     || viewer->cached_show_control_nodes != show_control_nodes
     || fabs(viewer->cached_ppd - ppd) > 1e-9)
  {
    _render_surface(viewer, allocation.width, allocation.height);
    viewer->cached_width = allocation.width;
    viewer->cached_height = allocation.height;
    viewer->cached_rotation_around_axis = rotation_around_axis;
    viewer->cached_rotation_of_axis = rotation_of_axis;
    viewer->cached_slice_depth = slice_depth;
    viewer->cached_slice_thickness = slice_thickness;
    viewer->cached_shift_threshold = shift_threshold;
    viewer->cached_zoom = zoom;
    viewer->cached_pan_x = viewer->pan_x;
    viewer->cached_pan_y = viewer->pan_y;
    viewer->cached_gamut = gamut;
    viewer->cached_clut = viewer->clut;
    viewer->cached_clut_level = viewer->clut_level;
    viewer->cached_lut_profile = viewer->lut_profile;
    viewer->cached_display_profile = viewer->display_profile;
    viewer->cached_control_nodes = viewer->control_nodes;
    viewer->cached_control_node_count = viewer->control_node_count;
    viewer->cached_show_control_nodes = show_control_nodes;
    viewer->cached_ppd = ppd;
  }

  if(viewer->surface)
  {
    cairo_set_source_surface(cr, viewer->surface, 0, 0);
    cairo_paint(cr);
  }

  return TRUE;
}

static void _control_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  _invalidate_surface(viewer);
  gtk_widget_queue_draw(GTK_WIDGET(viewer->area));
}

static gboolean _scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  float zoom_step = 1.f;

  switch(event->direction)
  {
    case GDK_SCROLL_UP:
      zoom_step = 1.1f;
      break;
    case GDK_SCROLL_DOWN:
      zoom_step = 1.f / 1.1f;
      break;
    case GDK_SCROLL_SMOOTH:
      zoom_step = exp2f((float)(-event->delta_y) * 0.2f);
      break;
    default:
      return FALSE;
  }

  const float old_zoom = viewer->zoom;
  const float new_zoom = CLAMP(old_zoom * zoom_step, 0.25f, 8.f);
  const float zoom_ratio = new_zoom / old_zoom;
  const float center_x = 0.5f * allocation.width;
  const float center_y = 0.5f * allocation.height;

  viewer->pan_x = event->x - center_x - zoom_ratio * (event->x - center_x - viewer->pan_x);
  viewer->pan_y = event->y - center_y - zoom_ratio * (event->y - center_y - viewer->pan_y);
  viewer->zoom = new_zoom;
  _invalidate_surface(viewer);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_bauhaus_slider_reset(viewer->rotation_around_axis);
    dt_bauhaus_slider_reset(viewer->rotation_of_axis);
    dt_bauhaus_slider_reset(viewer->slice_depth);
    dt_bauhaus_slider_reset(viewer->slice_thickness);
    viewer->zoom = 1.f;
    viewer->pan_x = 0.f;
    viewer->pan_y = 0.f;
    viewer->drag_mode = DT_LUT_VIEWER_DRAG_NONE;
    _invalidate_surface(viewer);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  if(event->button == 1)
    viewer->drag_mode = DT_LUT_VIEWER_DRAG_PAN;
  else if(event->button == 2)
    viewer->drag_mode = DT_LUT_VIEWER_DRAG_ORBIT;
  else
    return FALSE;

  viewer->drag_anchor_x = event->x;
  viewer->drag_anchor_y = event->y;
  viewer->drag_origin_pan_x = viewer->pan_x;
  viewer->drag_origin_pan_y = viewer->pan_y;
  viewer->drag_origin_azimuth = dt_bauhaus_slider_get(viewer->rotation_around_axis);
  viewer->drag_origin_tilt = dt_bauhaus_slider_get(viewer->rotation_of_axis);
  return TRUE;
}

static gboolean _motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  if(viewer->drag_mode == DT_LUT_VIEWER_DRAG_NONE) return FALSE;

  if(viewer->drag_mode == DT_LUT_VIEWER_DRAG_PAN)
  {
    viewer->pan_x = viewer->drag_origin_pan_x + (float)(event->x - viewer->drag_anchor_x);
    viewer->pan_y = viewer->drag_origin_pan_y + (float)(event->y - viewer->drag_anchor_y);
    _invalidate_surface(viewer);
    gtk_widget_queue_draw(widget);
  }
  else if(viewer->drag_mode == DT_LUT_VIEWER_DRAG_ORBIT)
  {
    const float azimuth = _wrap_degrees_pm180(viewer->drag_origin_azimuth
                                              + 0.35f * (float)(event->x - viewer->drag_anchor_x));
    const float tilt = CLAMP(viewer->drag_origin_tilt
                             - 0.25f * (float)(event->y - viewer->drag_anchor_y), 0.f, 90.f);
    dt_bauhaus_slider_set(viewer->rotation_around_axis, azimuth);
    dt_bauhaus_slider_set(viewer->rotation_of_axis, tilt);
  }

  return TRUE;
}

static gboolean _button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  if((event->button == 1 && viewer->drag_mode == DT_LUT_VIEWER_DRAG_PAN)
     || (event->button == 2 && viewer->drag_mode == DT_LUT_VIEWER_DRAG_ORBIT))
    viewer->drag_mode = DT_LUT_VIEWER_DRAG_NONE;
  return TRUE;
}

static void _save_clut_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lut_viewer_t *viewer = (dt_lut_viewer_t *)user_data;
  if(IS_NULL_PTR(viewer) || IS_NULL_PTR(viewer->clut) || viewer->clut_level < 2)
  {
    dt_control_log(_("no LUT to save"));
    return;
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("save 3D LUT"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_save"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(filechooser), "lut-viewer-export.cube");

  gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if(lutfolder[0])
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), lutfolder);
  else
    dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));

  GtkFileFilter *filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_add_pattern(filter, "*.CUBE");
  gtk_file_filter_set_name(filter, _("3D lut (cube)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *path = filename;
    FILE *cube_file = NULL;

    if(!g_str_has_suffix(filename, ".cube") && !g_str_has_suffix(filename, ".CUBE"))
      path = g_strconcat(filename, ".cube", NULL);

    cube_file = g_fopen(path, "wb");
    if(IS_NULL_PTR(cube_file))
    {
      dt_control_log(_("failed to save LUT file"));
    }
    else
    {
      const char *const lut_profile_name
          = viewer->lut_profile ? dt_colorspaces_get_name(viewer->lut_profile->type, viewer->lut_profile->filename)
                                : _("unknown");
      fprintf(cube_file, "# generated by ansel LUT viewer\n");
      fprintf(cube_file, "# LUT_COLORSPACE %s\n", lut_profile_name);
      fprintf(cube_file, "TITLE \"lut viewer export\"\n");
      fprintf(cube_file, "LUT_3D_SIZE %u\n", viewer->clut_level);
      fprintf(cube_file, "DOMAIN_MIN 0.0 0.0 0.0\n");
      fprintf(cube_file, "DOMAIN_MAX 1.0 1.0 1.0\n");

      for(uint16_t b = 0; b < viewer->clut_level; b++)
        for(uint16_t g = 0; g < viewer->clut_level; g++)
          for(uint16_t r = 0; r < viewer->clut_level; r++)
          {
            const size_t index
              = ((size_t)b * viewer->clut_level * viewer->clut_level + (size_t)g * viewer->clut_level + r) * 3;
            fprintf(cube_file, "%.7f %.7f %.7f\n",
                    viewer->clut[index + 0], viewer->clut[index + 1], viewer->clut[index + 2]);
          }

      fclose(cube_file);
      dt_control_log(_("saved LUT to %s"), path);
      dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
    }

    if(path != filename) g_free(path);
    g_free(filename);
  }

  dt_free(lutfolder);
  g_object_unref(filechooser);
}

dt_lut_viewer_t *dt_lut_viewer_new(dt_gui_module_t *module)
{
  dt_lut_viewer_t *viewer = calloc(1, sizeof(*viewer));
  if(IS_NULL_PTR(viewer)) return NULL;

  viewer->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  viewer->controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  viewer->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.f));
  gtk_widget_set_size_request(GTK_WIDGET(viewer->area), -1, DT_PIXEL_APPLY_DPI(180));
  gtk_widget_add_events(GTK_WIDGET(viewer->area),
                        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK
                          | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(viewer->area), "draw", G_CALLBACK(_draw_callback), viewer);
  g_signal_connect(G_OBJECT(viewer->area), "scroll-event", G_CALLBACK(_scroll_callback), viewer);
  g_signal_connect(G_OBJECT(viewer->area), "button-press-event", G_CALLBACK(_button_press_callback), viewer);
  g_signal_connect(G_OBJECT(viewer->area), "motion-notify-event", G_CALLBACK(_motion_notify_callback), viewer);
  g_signal_connect(G_OBJECT(viewer->area), "button-release-event", G_CALLBACK(_button_release_callback), viewer);
  gtk_box_pack_start(GTK_BOX(viewer->widget), GTK_WIDGET(viewer->area), TRUE, TRUE, 0);

  viewer->rotation_around_axis
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module, -180.f, 180.f, 1.f, 35.f, 0);
  dt_bauhaus_widget_set_label(viewer->rotation_around_axis, _("azimuth"));
  dt_bauhaus_slider_set_format(viewer->rotation_around_axis, _("°"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->rotation_around_axis, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->rotation_around_axis), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->rotation_of_axis
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module, 0.f, 90.f, 1.f, 0.f, 0);
  dt_bauhaus_widget_set_label(viewer->rotation_of_axis, _("axis tilt"));
  dt_bauhaus_slider_set_format(viewer->rotation_of_axis, _("°"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->rotation_of_axis, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->rotation_of_axis), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->slice_depth
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module, 0.f, 100.f, 1.f, 50.f, 0);
  dt_bauhaus_widget_set_label(viewer->slice_depth, _("slice depth"));
  dt_bauhaus_slider_set_format(viewer->slice_depth, _(" %"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->slice_depth, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->slice_depth), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->slice_thickness
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module, 0.f, 100.f, 1.f, 100.f, 0);
  dt_bauhaus_widget_set_label(viewer->slice_thickness, _("slice thickness"));
  dt_bauhaus_slider_set_format(viewer->slice_thickness, _(" %"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->slice_thickness, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->slice_thickness), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->shift_threshold
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module, 0.f, 100.f, 0.1f, 0.1f, 1);
  dt_bauhaus_widget_set_label(viewer->shift_threshold, _("color shift threshold"));
  dt_bauhaus_slider_set_format(viewer->shift_threshold, _(" %"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->shift_threshold, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->shift_threshold), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->show_control_nodes = gtk_check_button_new_with_label(_("show control nodes"));
  gtk_widget_set_sensitive(viewer->show_control_nodes, FALSE);
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->show_control_nodes, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->show_control_nodes), "toggled", G_CALLBACK(_control_changed), viewer);

  viewer->gamut = dt_bauhaus_combobox_new(darktable.bauhaus, module);
  dt_bauhaus_widget_set_label(viewer->gamut, _("target gamut"));
  dt_bauhaus_combobox_add(viewer->gamut, _("sRGB/Rec709"));
  dt_bauhaus_combobox_add(viewer->gamut, _("Adobe RGB"));
  dt_bauhaus_combobox_add(viewer->gamut, _("Display P3"));
  dt_bauhaus_combobox_add(viewer->gamut, _("Rec2020"));
  dt_bauhaus_combobox_set(viewer->gamut, DT_LUT_VIEWER_GAMUT_REC2020);
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->gamut, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->gamut), "value-changed", G_CALLBACK(_control_changed), viewer);

  viewer->save_button = gtk_button_new_with_label(_("save to cLUT"));
  gtk_box_pack_start(GTK_BOX(viewer->controls), viewer->save_button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(viewer->save_button), "clicked", G_CALLBACK(_save_clut_callback), viewer);

  GtkWidget *expander = gtk_expander_new(_("viewer controls"));
  gtk_expander_set_expanded(GTK_EXPANDER(expander), FALSE);
  gtk_container_add(GTK_CONTAINER(expander), viewer->controls);
  gtk_box_pack_start(GTK_BOX(viewer->widget), expander, FALSE, FALSE, 0);

  viewer->zoom = 1.f;
  viewer->pan_x = 0.f;
  viewer->pan_y = 0.f;
  viewer->drag_mode = DT_LUT_VIEWER_DRAG_NONE;
  viewer->cached_gamut = -1;
  viewer->cached_rotation_around_axis = NAN;
  viewer->cached_rotation_of_axis = NAN;
  viewer->cached_slice_depth = NAN;
  viewer->cached_slice_thickness = NAN;
  viewer->cached_zoom = NAN;
  viewer->cached_pan_x = NAN;
  viewer->cached_pan_y = NAN;
  viewer->cached_shift_threshold = NAN;
  viewer->cached_ppd = NAN;
  viewer->sample_cache_rotation_around_axis = NAN;
  viewer->sample_cache_rotation_of_axis = NAN;
  viewer->sample_cache_slice_depth = NAN;
  viewer->sample_cache_slice_thickness = NAN;
  viewer->sample_cache_shift_threshold = NAN;
  viewer->sample_cache_gamut = -1;
  viewer->sample_cache_control_nodes = NULL;
  viewer->sample_cache_control_node_count = 0;
  viewer->sample_cache_show_control_nodes = FALSE;
  return viewer;
}

void dt_lut_viewer_destroy(dt_lut_viewer_t **viewer)
{
  if(IS_NULL_PTR(viewer) || !*viewer) return;

  _invalidate_surface(*viewer);
  dt_free_align((*viewer)->sample_input_work);
  dt_free_align((*viewer)->sample_output_work);
  dt_free_align((*viewer)->sample_input_display);
  dt_free_align((*viewer)->sample_output_display);
  free(*viewer);
  *viewer = NULL;
}

GtkWidget *dt_lut_viewer_get_widget(dt_lut_viewer_t *viewer)
{
  return viewer ? viewer->widget : NULL;
}

void dt_lut_viewer_set_lut(dt_lut_viewer_t *viewer, const float *clut, uint16_t level,
                           dt_pthread_rwlock_t *clut_lock,
                           const dt_iop_order_iccprofile_info_t *lut_profile,
                           const dt_iop_order_iccprofile_info_t *display_profile)
{
  if(IS_NULL_PTR(viewer)) return;

  viewer->clut = clut;
  viewer->clut_level = level;
  viewer->clut_lock = clut_lock;
  viewer->lut_profile = lut_profile;
  viewer->display_profile = display_profile;
  _invalidate_sample_cache(viewer);
  _invalidate_surface(viewer);
}

void dt_lut_viewer_set_control_nodes(dt_lut_viewer_t *viewer,
                                     const dt_lut_viewer_control_node_t *control_nodes,
                                     size_t control_node_count)
{
  if(IS_NULL_PTR(viewer)) return;

  viewer->control_nodes = control_nodes;
  viewer->control_node_count = control_node_count;

  if(viewer->show_control_nodes)
  {
    const gboolean enabled = control_nodes && control_node_count > 0;
    gtk_widget_set_sensitive(viewer->show_control_nodes, enabled);
    if(!enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(viewer->show_control_nodes), FALSE);
  }

  _invalidate_sample_cache(viewer);
  _invalidate_surface(viewer);
}

void dt_lut_viewer_queue_draw(dt_lut_viewer_t *viewer)
{
  if(viewer) gtk_widget_queue_draw(GTK_WIDGET(viewer->area));
}
