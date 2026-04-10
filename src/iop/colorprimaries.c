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

#include "common/extra_optimizations.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorequal_shared.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/lut_viewer.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_colorprimaries_params_t)

#define DT_IOP_COLORPRIMARIES_NODE_COUNT 6
#define DT_IOP_COLORPRIMARIES_EDGE_COUNT 6
#define DT_IOP_COLORPRIMARIES_RADIAL_COUNT (DT_IOP_COLORPRIMARIES_NODE_COUNT + DT_IOP_COLORPRIMARIES_EDGE_COUNT)
#define DT_IOP_COLORPRIMARIES_BLACK_WHITE_COUNT (2 * DT_IOP_COLORPRIMARIES_NODE_COUNT)
#define DT_IOP_COLORPRIMARIES_AXIS_ANCHORS 64
#define DT_IOP_COLORPRIMARIES_MAX_ANCHORS \
  (DT_IOP_COLORPRIMARIES_NODE_COUNT + DT_IOP_COLORPRIMARIES_EDGE_COUNT + DT_IOP_COLORPRIMARIES_RADIAL_COUNT \
   + DT_IOP_COLORPRIMARIES_BLACK_WHITE_COUNT + DT_IOP_COLORPRIMARIES_AXIS_ANCHORS)
#define DT_IOP_COLORPRIMARIES_VIEWER_CONTROL_NODES \
  (DT_IOP_COLORPRIMARIES_NODE_COUNT + DT_IOP_COLORPRIMARIES_EDGE_COUNT + DT_IOP_COLORPRIMARIES_RADIAL_COUNT \
   + DT_IOP_COLORPRIMARIES_BLACK_WHITE_COUNT)
#define DT_IOP_COLORPRIMARIES_SQRT3 1.7320508075688772f
#define DT_IOP_COLORPRIMARIES_INV_SQRT2 0.7071067811865475f

typedef enum dt_iop_colorprimaries_interpolation_t
{
  DT_IOP_COLORPRIMARIES_TETRAHEDRAL = 0, // $DESCRIPTION: "tetrahedral"
  DT_IOP_COLORPRIMARIES_TRILINEAR = 1,   // $DESCRIPTION: "trilinear"
  DT_IOP_COLORPRIMARIES_PYRAMID = 2,     // $DESCRIPTION: "pyramid"
} dt_iop_colorprimaries_interpolation_t;

typedef enum dt_iop_colorprimaries_node_t
{
  DT_IOP_COLORPRIMARIES_RED = 0,
  DT_IOP_COLORPRIMARIES_YELLOW = 1,
  DT_IOP_COLORPRIMARIES_GREEN = 2,
  DT_IOP_COLORPRIMARIES_CYAN = 3,
  DT_IOP_COLORPRIMARIES_BLUE = 4,
  DT_IOP_COLORPRIMARIES_MAGENTA = 5,
} dt_iop_colorprimaries_node_t;

typedef struct dt_iop_colorprimaries_edge_t
{
  dt_iop_colorprimaries_node_t a;
  dt_iop_colorprimaries_node_t b;
} dt_iop_colorprimaries_edge_t;

typedef struct dt_iop_colorprimaries_params_t
{
  float white_level; // $MIN: -2.0 $MAX: 16.0 $DEFAULT: 1.0 $DESCRIPTION: "white level"
  float gamut_coverage; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 67.0 $DESCRIPTION: "gamut coverage"
  float sigma_L; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "brightness smoothing"
  float sigma_rho; // $MIN: 0.01 $MAX: 2.0 $DEFAULT: 0.70710678 $DESCRIPTION: "saturation smoothing"
  float sigma_theta; // $MIN: 0.01 $MAX: 6.28318531 $DEFAULT: 0.70710678 $DESCRIPTION: "hue smoothing"
  float neutral_protection; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "neutral protection"
  dt_iop_colorprimaries_interpolation_t interpolation; // $DEFAULT: DT_IOP_COLORPRIMARIES_TETRAHEDRAL $DESCRIPTION: "interpolation"
  float hue[DT_IOP_COLORPRIMARIES_NODE_COUNT]; // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float saturation[DT_IOP_COLORPRIMARIES_NODE_COUNT]; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation"
  float brightness[DT_IOP_COLORPRIMARIES_NODE_COUNT]; // $MIN: -0.25 $MAX: 0.25 $DEFAULT: 0.0 $DESCRIPTION: "brightness"
} dt_iop_colorprimaries_params_t;

typedef struct dt_iop_colorprimaries_data_t
{
  float *clut;
  uint16_t clut_level;
  float white_level;
  dt_iop_order_iccprofile_info_t *lut_profile;
  dt_iop_order_iccprofile_info_t *work_profile;
  dt_lut3d_interpolation_t interpolation;
} dt_iop_colorprimaries_data_t;

typedef struct dt_iop_colorprimaries_global_data_t
{
  dt_pthread_rwlock_t lock;
  dt_iop_colorprimaries_data_t cache;
  dt_iop_colorprimaries_params_t params;
  gboolean cache_valid;
  uint64_t cache_generation;
} dt_iop_colorprimaries_global_data_t;

typedef struct dt_iop_colorprimaries_gui_data_t
{
  GtkNotebook *tabs;
  GtkWidget *white_level;
  GtkWidget *gamut_coverage;
  GtkWidget *sigma_L;
  GtkWidget *sigma_rho;
  GtkWidget *sigma_theta;
  GtkWidget *neutral_protection;
  GtkWidget *interpolation;
  GtkWidget *node_hue[DT_IOP_COLORPRIMARIES_NODE_COUNT];
  GtkWidget *node_saturation[DT_IOP_COLORPRIMARIES_NODE_COUNT];
  GtkWidget *node_brightness[DT_IOP_COLORPRIMARIES_NODE_COUNT];
  dt_lut_viewer_t *viewer;
  dt_iop_colorprimaries_data_t viewer_lut;
  uint64_t viewer_lut_generation;
  gboolean viewer_lut_dirty;
  gboolean viewer_lut_valid;
  gboolean preview_signal_connected;
  const dt_iop_order_iccprofile_info_t *viewer_display_profile;
  dt_lut_viewer_control_node_t viewer_control_nodes[DT_IOP_COLORPRIMARIES_VIEWER_CONTROL_NODES];
  size_t viewer_control_node_count;
} dt_iop_colorprimaries_gui_data_t;

const char *name()
{
  return _("color primaries");
}

const char *aliases()
{
  return _("RGB primaries|primary colors");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("edit RGB/CYM primary control nodes in dt UCS and interpolate their RGB shifts "
                                  "through a cylindrical local field"),
                                _("creative"), _("linear, RGB, display-referred"), _("linear, RGB"),
                                _("linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void _update_gui_lut_cache(dt_iop_module_t *self);

static const dt_iop_colorprimaries_edge_t _chroma_edges[DT_IOP_COLORPRIMARIES_EDGE_COUNT] = {
  { DT_IOP_COLORPRIMARIES_RED, DT_IOP_COLORPRIMARIES_YELLOW },
  { DT_IOP_COLORPRIMARIES_YELLOW, DT_IOP_COLORPRIMARIES_GREEN },
  { DT_IOP_COLORPRIMARIES_GREEN, DT_IOP_COLORPRIMARIES_CYAN },
  { DT_IOP_COLORPRIMARIES_CYAN, DT_IOP_COLORPRIMARIES_BLUE },
  { DT_IOP_COLORPRIMARIES_BLUE, DT_IOP_COLORPRIMARIES_MAGENTA },
  { DT_IOP_COLORPRIMARIES_MAGENTA, DT_IOP_COLORPRIMARIES_RED },
};

static inline const char *_node_name(const dt_iop_colorprimaries_node_t node)
{
  switch(node)
  {
    case DT_IOP_COLORPRIMARIES_YELLOW:
      return _("yellow");
    case DT_IOP_COLORPRIMARIES_GREEN:
      return _("green");
    case DT_IOP_COLORPRIMARIES_CYAN:
      return _("cyan");
    case DT_IOP_COLORPRIMARIES_BLUE:
      return _("blue");
    case DT_IOP_COLORPRIMARIES_MAGENTA:
      return _("magenta");
    case DT_IOP_COLORPRIMARIES_RED:
    default:
      return _("red");
  }
}

static void _init_default_params(dt_iop_colorprimaries_params_t *p)
{
  memset(p, 0, sizeof(*p));
  p->white_level = 1.f;
  p->gamut_coverage = 67.f;
  p->sigma_L = 100.f;
  p->sigma_rho = DT_IOP_COLORPRIMARIES_INV_SQRT2;
  p->sigma_theta = DT_IOP_COLORPRIMARIES_INV_SQRT2;
  p->neutral_protection = 0.f;
  p->interpolation = DT_IOP_COLORPRIMARIES_TETRAHEDRAL;
}

static inline gboolean _lut_fields_equal(const dt_iop_colorprimaries_params_t *const a,
                                         const dt_iop_colorprimaries_params_t *const b)
{
  return !memcmp(a, b, sizeof(*a));
}

static inline void _node_corner_rgb(const dt_iop_colorprimaries_node_t node, dt_aligned_pixel_t RGB)
{
  switch(node)
  {
    case DT_IOP_COLORPRIMARIES_YELLOW:
      RGB[0] = 1.f;
      RGB[1] = 1.f;
      RGB[2] = 0.f;
      break;
    case DT_IOP_COLORPRIMARIES_GREEN:
      RGB[0] = 0.f;
      RGB[1] = 1.f;
      RGB[2] = 0.f;
      break;
    case DT_IOP_COLORPRIMARIES_CYAN:
      RGB[0] = 0.f;
      RGB[1] = 1.f;
      RGB[2] = 1.f;
      break;
    case DT_IOP_COLORPRIMARIES_BLUE:
      RGB[0] = 0.f;
      RGB[1] = 0.f;
      RGB[2] = 1.f;
      break;
    case DT_IOP_COLORPRIMARIES_MAGENTA:
      RGB[0] = 1.f;
      RGB[1] = 0.f;
      RGB[2] = 1.f;
      break;
    case DT_IOP_COLORPRIMARIES_RED:
    default:
      RGB[0] = 1.f;
      RGB[1] = 0.f;
      RGB[2] = 0.f;
      break;
  }
  RGB[3] = 0.f;
}

static inline void _node_base_rgb(const dt_iop_colorprimaries_node_t node, const float gamut_coverage, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t corner = { 0.f };
  _node_corner_rgb(node, corner);

  const float mu = (corner[0] + corner[1] + corner[2]) / 3.f;
  for_each_channel(c, aligned(RGB, corner)) RGB[c] = mu + gamut_coverage * (corner[c] - mu);
  RGB[3] = 0.f;
}

static inline float _mix_hue_delta_weighted(const float hue_a, const float hue_b, const float weight_a,
                                            const float weight_b)
{
  const float angle_a = hue_a * M_PI_F / 180.f;
  const float angle_b = hue_b * M_PI_F / 180.f;
  return atan2f(weight_a * sinf(angle_a) + weight_b * sinf(angle_b),
                weight_a * cosf(angle_a) + weight_b * cosf(angle_b));
}

static inline float _hsb_distance(const dt_aligned_pixel_t a, const dt_aligned_pixel_t b)
{
  const float dh = dt_colorrings_wrap_pi(a[0] - b[0]);
  const float ds = a[1] - b[1];
  const float db = a[2] - b[2];
  return sqrtf(dh * dh + ds * ds + db * db);
}

static inline void _black_white_rgb(const gboolean white, dt_aligned_pixel_t RGB)
{
  RGB[0] = white ? 1.f : 0.f;
  RGB[1] = white ? 1.f : 0.f;
  RGB[2] = white ? 1.f : 0.f;
  RGB[3] = 0.f;
}

static void _halfway_to_axis_rgb(const dt_aligned_pixel_t source_rgb, dt_aligned_pixel_t halfway_rgb)
{
  float L = 0.f;
  float rho = 0.f;
  float theta = 0.f;
  dt_colorrings_rgb_to_gray_cyl(source_rgb, &L, &rho, &theta);
  dt_colorrings_gray_basis_to_rgb(L, 0.5f * rho * cosf(theta), 0.5f * rho * sinf(theta), halfway_rgb);
  halfway_rgb[3] = 0.f;
}

static gboolean _build_anchor_from_source_rgb(const dt_aligned_pixel_t source_rgb, const float hue_delta,
                                              const float saturation_delta, const float brightness_delta,
                                              const dt_iop_order_iccprofile_info_t *const lut_profile,
                                              dt_colorrings_sparse_anchor_t *const anchor)
{
  const float white = dt_colorrings_graph_white();
  dt_aligned_pixel_t source_hsb = { 0.f };
  dt_aligned_pixel_t source_axis = { 0.f };
  dt_aligned_pixel_t target_axis = { 0.f };
  float source_L = 0.f;
  float source_rho = 0.f;
  float source_theta = 0.f;
  float source_axis_L = 0.f;
  float target_axis_L = 0.f;
  float unused_rho = 0.f;
  float unused_theta = 0.f;
  float source_brightness = 0.f;
  float target_brightness = 0.f;
  float requested_scale = 1.f;

  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_rgb, white, lut_profile, source_hsb);
  dt_colorrings_rgb_to_gray_cyl(source_rgb, &source_L, &source_rho, &source_theta);

  if(source_rho <= 1e-6f) return FALSE;

  /**
   * Express the user edits directly as a cylindrical RGB transform instead of
   * reconstructing a temporary target RGB anchor first.
   *
   * The important subtlety is that dt UCS HSB source saturations can exceed
   * the normalized [0, 1] GUI interval for some recessed nodes, especially
   * near blue. Clamping that source state before applying a zero user delta
   * silently turns a neutral setting into a non-neutral desaturation. Build
   * the sparse transform from the slider deltas themselves so the identity
   * transform stays exact regardless of where the source node lands in dt UCS.
   */
  source_brightness = CLAMP(source_hsb[2], 0.f, 1.f);
  target_brightness = CLAMP(source_brightness + brightness_delta, 0.f, 1.f);
  dt_colorrings_brightness_to_axis_rgb(source_brightness, white, lut_profile, source_axis);
  dt_colorrings_rgb_to_gray_cyl(source_axis, &source_axis_L, &unused_rho, &unused_theta);
  dt_colorrings_brightness_to_axis_rgb(target_brightness, white, lut_profile, target_axis);
  dt_colorrings_rgb_to_gray_cyl(target_axis, &target_axis_L, &unused_rho, &unused_theta);

  if(source_hsb[1] > 1e-6f)
    requested_scale = fmaxf(source_hsb[1] + saturation_delta, 0.f) / source_hsb[1];

  anchor->L = source_L;
  anchor->rho = source_rho;
  anchor->theta = source_theta;
  anchor->delta_L = target_axis_L - source_axis_L;
  anchor->chroma_scale = requested_scale;
  anchor->delta_theta = dt_colorrings_wrap_pi(hue_delta);
  anchor->weight = 1.f;
  return TRUE;
}

static gboolean _build_halfway_radial_anchor_from_source_rgb(const dt_aligned_pixel_t source_rgb,
                                                             const float hue_delta, const float saturation_delta,
                                                             const float brightness_delta,
                                                             const dt_iop_order_iccprofile_info_t *const lut_profile,
                                                             dt_colorrings_sparse_anchor_t *const anchor,
                                                             dt_aligned_pixel_t halfway_rgb)
{
  dt_aligned_pixel_t source_hsb = { 0.f };
  dt_aligned_pixel_t axis_rgb = { 0.f };
  dt_aligned_pixel_t axis_hsb = { 0.f };
  dt_aligned_pixel_t halfway_hsb = { 0.f };
  float distance_source = 0.f;
  float distance_axis = 0.f;
  float weight_source = 0.5f;
  float source_L = 0.f;
  float source_rho = 0.f;
  float source_theta = 0.f;

  dt_colorrings_rgb_to_gray_cyl(source_rgb, &source_L, &source_rho, &source_theta);
  if(source_rho <= 1e-6f) return FALSE;

  _halfway_to_axis_rgb(source_rgb, halfway_rgb);
  dt_colorrings_gray_axis_rgb_from_L(source_L, axis_rgb);

  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_rgb, dt_colorrings_graph_white(), lut_profile, source_hsb);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(axis_rgb, dt_colorrings_graph_white(), lut_profile, axis_hsb);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(halfway_rgb, dt_colorrings_graph_white(), lut_profile, halfway_hsb);

  /**
   * The achromatic endpoint has no meaningful hue of its own. Reuse the outer
   * control-node hue there so the HSB distance only measures how far the
   * halfway point sits from the colored control versus the neutral axis,
   * instead of introducing a branch-cut-dependent hue error from a grey value.
   */
  axis_hsb[0] = source_hsb[0];
  distance_source = _hsb_distance(halfway_hsb, source_hsb);
  distance_axis = _hsb_distance(halfway_hsb, axis_hsb);
  if(distance_source + distance_axis > 1e-6f)
    weight_source = distance_axis / (distance_source + distance_axis);

  return _build_anchor_from_source_rgb(halfway_rgb, weight_source * hue_delta, weight_source * saturation_delta,
                                       weight_source * brightness_delta, lut_profile, anchor);
}

static gboolean _build_halfway_extreme_anchor_from_source_rgb(const dt_aligned_pixel_t source_rgb,
                                                              const dt_aligned_pixel_t extreme_rgb,
                                                              const float hue_delta, const float saturation_delta,
                                                              const float brightness_delta,
                                                              const dt_iop_order_iccprofile_info_t *const lut_profile,
                                                              dt_colorrings_sparse_anchor_t *const anchor,
                                                              dt_aligned_pixel_t halfway_rgb)
{
  dt_aligned_pixel_t source_hsb = { 0.f };
  dt_aligned_pixel_t extreme_hsb = { 0.f };
  dt_aligned_pixel_t halfway_hsb = { 0.f };
  float distance_source = 0.f;
  float distance_extreme = 0.f;
  float weight_source = 0.5f;

  for(int c = 0; c < 3; c++) halfway_rgb[c] = 0.5f * (source_rgb[c] + extreme_rgb[c]);
  halfway_rgb[3] = 0.f;

  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_rgb, dt_colorrings_graph_white(), lut_profile, source_hsb);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(extreme_rgb, dt_colorrings_graph_white(), lut_profile, extreme_hsb);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(halfway_rgb, dt_colorrings_graph_white(), lut_profile, halfway_hsb);

  extreme_hsb[0] = source_hsb[0];
  distance_source = _hsb_distance(halfway_hsb, source_hsb);
  distance_extreme = _hsb_distance(halfway_hsb, extreme_hsb);
  if(distance_source + distance_extreme > 1e-6f)
    weight_source = distance_extreme / (distance_source + distance_extreme);

  return _build_anchor_from_source_rgb(halfway_rgb, weight_source * hue_delta, weight_source * saturation_delta,
                                       weight_source * brightness_delta, lut_profile, anchor);
}

static void _apply_anchor_to_rgb(const dt_aligned_pixel_t source_rgb, const dt_colorrings_sparse_anchor_t *const anchor,
                                 dt_aligned_pixel_t target_rgb)
{
  dt_aligned_pixel_t axis = { 0.f };
  float source_L = 0.f;
  float source_rho = 0.f;
  float source_theta = 0.f;

  dt_colorrings_rgb_to_gray_cyl(source_rgb, &source_L, &source_rho, &source_theta);
  dt_colorrings_gray_basis_to_rgb(source_L + anchor->delta_L,
                                  source_rho * anchor->chroma_scale * cosf(source_theta + anchor->delta_theta),
                                  source_rho * anchor->chroma_scale * sinf(source_theta + anchor->delta_theta),
                                  target_rgb);
  dt_colorrings_gray_axis_rgb_from_L(source_L + anchor->delta_L, axis);
  dt_colorrings_project_to_cube_shell(axis, target_rgb);
  target_rgb[3] = 0.f;
}

static gboolean _build_node_anchor(const dt_iop_colorprimaries_params_t *const p, const dt_iop_colorprimaries_node_t node,
                                   const dt_iop_order_iccprofile_info_t *const lut_profile,
                                   dt_colorrings_sparse_anchor_t *const anchor)
{
  dt_aligned_pixel_t source_rgb = { 0.f };
  _node_base_rgb(node, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_rgb);
  return _build_anchor_from_source_rgb(source_rgb, p->hue[node] * M_PI_F / 180.f, p->saturation[node] * 0.01f,
                                       p->brightness[node], lut_profile, anchor);
}

static gboolean _build_edge_edit(const dt_iop_colorprimaries_params_t *const p, const dt_iop_colorprimaries_edge_t edge,
                                 const dt_iop_order_iccprofile_info_t *const lut_profile, dt_aligned_pixel_t source_rgb,
                                 float *const hue_delta, float *const saturation_delta,
                                 float *const brightness_delta)
{
  dt_aligned_pixel_t source_a = { 0.f };
  dt_aligned_pixel_t source_b = { 0.f };
  dt_aligned_pixel_t hsb_a = { 0.f };
  dt_aligned_pixel_t hsb_b = { 0.f };
  dt_aligned_pixel_t hsb_mid = { 0.f };
  float distance_a = 0.f;
  float distance_b = 0.f;
  float weight_a = 0.5f;
  float weight_b = 0.5f;

  _node_base_rgb(edge.a, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_a);
  _node_base_rgb(edge.b, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_b);

  /**
   * The user only edits the six chromatic vertices, but the local field gets
   * unstable when those anchors are too far apart on the hue circle. Insert a
   * midpoint anchor on every chromatic edge and inherit its HSB transform from
   * the two neighbouring vertex edits.
   *
   * The midpoint of an RGB edge is not generally halfway between its endpoints
   * once mapped to dt UCS HSB. Weight the synthetic control in that same HSB
   * geometry so the extra anchor follows the perceptual spacing of the source
   * nodes instead of assuming a fixed 50/50 split.
   */
  for_each_channel(c, aligned(source_a, source_b, source_rgb)) source_rgb[c] = 0.5f * (source_a[c] + source_b[c]);
  source_rgb[3] = 0.f;
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_a, dt_colorrings_graph_white(), lut_profile, hsb_a);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_b, dt_colorrings_graph_white(), lut_profile, hsb_b);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(source_rgb, dt_colorrings_graph_white(), lut_profile, hsb_mid);

  distance_a = _hsb_distance(hsb_mid, hsb_a);
  distance_b = _hsb_distance(hsb_mid, hsb_b);
  if(distance_a + distance_b > 1e-6f)
  {
    weight_a = distance_b / (distance_a + distance_b);
    weight_b = distance_a / (distance_a + distance_b);
  }

  *hue_delta = _mix_hue_delta_weighted(p->hue[edge.a], p->hue[edge.b], weight_a, weight_b);
  *saturation_delta = (weight_a * p->saturation[edge.a] + weight_b * p->saturation[edge.b]) * 0.01f;
  *brightness_delta = weight_a * p->brightness[edge.a] + weight_b * p->brightness[edge.b];
  return TRUE;
}

static inline void _store_viewer_control_node(dt_lut_viewer_control_node_t *const control_nodes, int *const count,
                                              const dt_aligned_pixel_t input_rgb, const dt_aligned_pixel_t output_rgb)
{
  for(int c = 0; c < 3; c++)
  {
    control_nodes[*count].input_rgb[c] = input_rgb[c];
    control_nodes[*count].output_rgb[c] = output_rgb[c];
  }
  (*count)++;
}

static inline void _append_anchor(dt_colorrings_sparse_anchor_t *const anchors, int *const anchor_count,
                                  const dt_colorrings_sparse_anchor_t *const anchor)
{
  anchors[*anchor_count] = *anchor;
  (*anchor_count)++;
}

static gboolean _build_edge_anchor(const dt_iop_colorprimaries_params_t *const p, const dt_iop_colorprimaries_edge_t edge,
                                   const dt_iop_order_iccprofile_info_t *const lut_profile,
                                   dt_colorrings_sparse_anchor_t *const anchor)
{
  dt_aligned_pixel_t source_rgb = { 0.f };
  float hue_delta = 0.f;
  float saturation_delta = 0.f;
  float brightness_delta = 0.f;

  if(!_build_edge_edit(p, edge, lut_profile, source_rgb, &hue_delta, &saturation_delta, &brightness_delta))
    return FALSE;

  return _build_anchor_from_source_rgb(source_rgb, hue_delta, saturation_delta, brightness_delta, lut_profile, anchor);
}

static gboolean _build_node_radial_midpoint_anchor(const dt_iop_colorprimaries_params_t *const p,
                                                   const dt_iop_colorprimaries_node_t node,
                                                   const dt_iop_order_iccprofile_info_t *const lut_profile,
                                                   dt_colorrings_sparse_anchor_t *const anchor,
                                                   dt_aligned_pixel_t midpoint_rgb)
{
  dt_aligned_pixel_t source_rgb = { 0.f };
  _node_base_rgb(node, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_rgb);
  return _build_halfway_radial_anchor_from_source_rgb(source_rgb, p->hue[node] * M_PI_F / 180.f,
                                                      p->saturation[node] * 0.01f, p->brightness[node],
                                                      lut_profile, anchor, midpoint_rgb);
}

static gboolean _build_node_black_white_midpoint_anchor(const dt_iop_colorprimaries_params_t *const p,
                                                        const dt_iop_colorprimaries_node_t node,
                                                        const gboolean toward_white,
                                                        const dt_iop_order_iccprofile_info_t *const lut_profile,
                                                        dt_colorrings_sparse_anchor_t *const anchor,
                                                        dt_aligned_pixel_t midpoint_rgb)
{
  dt_aligned_pixel_t source_rgb = { 0.f };
  dt_aligned_pixel_t extreme_rgb = { 0.f };
  _node_base_rgb(node, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_rgb);
  _black_white_rgb(toward_white, extreme_rgb);
  return _build_halfway_extreme_anchor_from_source_rgb(source_rgb, extreme_rgb, p->hue[node] * M_PI_F / 180.f,
                                                       p->saturation[node] * 0.01f, p->brightness[node],
                                                       lut_profile, anchor, midpoint_rgb);
}

static gboolean _build_edge_radial_midpoint_anchor(const dt_iop_colorprimaries_params_t *const p,
                                                   const dt_iop_colorprimaries_edge_t edge,
                                                   const dt_iop_order_iccprofile_info_t *const lut_profile,
                                                   dt_colorrings_sparse_anchor_t *const anchor,
                                                   dt_aligned_pixel_t midpoint_rgb)
{
  dt_aligned_pixel_t source_rgb = { 0.f };
  float hue_delta = 0.f;
  float saturation_delta = 0.f;
  float brightness_delta = 0.f;

  if(!_build_edge_edit(p, edge, lut_profile, source_rgb, &hue_delta, &saturation_delta, &brightness_delta))
    return FALSE;

  return _build_halfway_radial_anchor_from_source_rgb(source_rgb, hue_delta, saturation_delta, brightness_delta,
                                                      lut_profile, anchor, midpoint_rgb);
}

static int _build_viewer_control_nodes(const dt_iop_colorprimaries_params_t *const p,
                                       const dt_iop_order_iccprofile_info_t *const lut_profile,
                                       dt_lut_viewer_control_node_t *const control_nodes)
{
  int count = 0;

  if(!control_nodes || !lut_profile) return 0;

  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    dt_aligned_pixel_t source_rgb = { 0.f };
    dt_aligned_pixel_t target_rgb = { 0.f };

    _node_base_rgb((dt_iop_colorprimaries_node_t)node, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), source_rgb);
    if(!_build_node_anchor(p, (dt_iop_colorprimaries_node_t)node, lut_profile, &anchor)) continue;
    _apply_anchor_to_rgb(source_rgb, &anchor, target_rgb);
    _store_viewer_control_node(control_nodes, &count, source_rgb, target_rgb);
  }

  for(int edge = 0; edge < DT_IOP_COLORPRIMARIES_EDGE_COUNT; edge++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    dt_aligned_pixel_t source_rgb = { 0.f };
    dt_aligned_pixel_t target_rgb = { 0.f };
    float hue_delta = 0.f;
    float saturation_delta = 0.f;
    float brightness_delta = 0.f;

    if(!_build_edge_edit(p, _chroma_edges[edge], lut_profile, source_rgb, &hue_delta, &saturation_delta, &brightness_delta))
      continue;
    if(!_build_anchor_from_source_rgb(source_rgb, hue_delta, saturation_delta, brightness_delta, lut_profile, &anchor))
      continue;
    _apply_anchor_to_rgb(source_rgb, &anchor, target_rgb);
    _store_viewer_control_node(control_nodes, &count, source_rgb, target_rgb);
  }

  for(int white = 0; white < 2; white++)
    for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    dt_aligned_pixel_t midway_rgb = { 0.f };
    dt_aligned_pixel_t target_rgb = { 0.f };

    if(!_build_node_black_white_midpoint_anchor(p, (dt_iop_colorprimaries_node_t)node, white != 0, lut_profile, &anchor,
                                                midway_rgb))
      continue;
    _apply_anchor_to_rgb(midway_rgb, &anchor, target_rgb);
    _store_viewer_control_node(control_nodes, &count, midway_rgb, target_rgb);
  }

  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    dt_aligned_pixel_t inner_source_rgb = { 0.f };
    dt_aligned_pixel_t inner_target_rgb = { 0.f };

    if(!_build_node_radial_midpoint_anchor(p, (dt_iop_colorprimaries_node_t)node, lut_profile, &anchor, inner_source_rgb))
      continue;
    _apply_anchor_to_rgb(inner_source_rgb, &anchor, inner_target_rgb);
    _store_viewer_control_node(control_nodes, &count, inner_source_rgb, inner_target_rgb);
  }

  for(int edge = 0; edge < DT_IOP_COLORPRIMARIES_EDGE_COUNT; edge++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    dt_aligned_pixel_t inner_source_rgb = { 0.f };
    dt_aligned_pixel_t inner_target_rgb = { 0.f };
    if(!_build_edge_radial_midpoint_anchor(p, _chroma_edges[edge], lut_profile, &anchor, inner_source_rgb))
      continue;
    _apply_anchor_to_rgb(inner_source_rgb, &anchor, inner_target_rgb);
    _store_viewer_control_node(control_nodes, &count, inner_source_rgb, inner_target_rgb);
  }

  return count;
}

static void _node_source_hsb(const dt_iop_colorprimaries_params_t *const p, const dt_iop_colorprimaries_node_t node,
                             const dt_iop_order_iccprofile_info_t *const lut_profile, dt_aligned_pixel_t HSB)
{
  dt_aligned_pixel_t RGB = { 0.f };
  _node_base_rgb(node, CLAMP(p->gamut_coverage * 0.01f, 0.f, 1.f), RGB);
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(RGB, dt_colorrings_graph_white(), lut_profile, HSB);
}

static void _node_target_hsb(const dt_iop_colorprimaries_params_t *const p, const dt_iop_colorprimaries_node_t node,
                             const dt_iop_order_iccprofile_info_t *const lut_profile, dt_aligned_pixel_t HSB)
{
  dt_aligned_pixel_t source_hsb = { 0.f };
  _node_source_hsb(p, node, lut_profile, source_hsb);

  HSB[0] = dt_colorrings_wrap_hue_pi(source_hsb[0] + p->hue[node] * M_PI_F / 180.f);
  HSB[1] = CLAMP(source_hsb[1] + p->saturation[node] * 0.01f, 0.f, 1.f);
  HSB[2] = CLAMP(source_hsb[2] + p->brightness[node], 0.f, 1.f);
  HSB[3] = 0.f;
}

static void _build_clut(dt_iop_colorprimaries_data_t *d, const dt_iop_colorprimaries_params_t *p,
                        const dt_iop_order_iccprofile_info_t *lut_profile)
{
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double start = log_perf ? dt_get_wtime() : 0.0;
  const size_t clut_size = (size_t)DT_COLORRINGS_CLUT_LEVEL * DT_COLORRINGS_CLUT_LEVEL * DT_COLORRINGS_CLUT_LEVEL * 3u;
  const float inv_sigma_L = 1.f / fmaxf(p->sigma_L * 0.01f, 1e-6f);
  const float inv_sigma_rho = 1.f / fmaxf(p->sigma_rho, 1e-6f);
  const float inv_sigma_theta = 1.f / fmaxf(p->sigma_theta, 1e-6f);
  dt_colorrings_sparse_anchor_t anchors[DT_IOP_COLORPRIMARIES_MAX_ANCHORS] = { 0 };
  int anchor_count = 0;

  if(!d->clut) d->clut = dt_alloc_align_float(clut_size);
  d->lut_profile = (dt_iop_order_iccprofile_info_t *)lut_profile;

  /**
   * The user edits six recessed RGB/CYM vertices of the Rec2020/HLG gamut.
   * Each edited HSB node is converted back to RGB, then the same RGB
   * cylindrical local field as color equalizer spreads those sparse
   * displacements over the whole LUT volume. A dense ladder of no-op axis
   * anchors keeps the black-white diagonal fixed and stabilizes near-neutral
   * interpolation.
   */
  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    if(_build_node_anchor(p, (dt_iop_colorprimaries_node_t)node, lut_profile, &anchor))
      _append_anchor(anchors, &anchor_count, &anchor);
  }

  for(int edge = 0; edge < DT_IOP_COLORPRIMARIES_EDGE_COUNT; edge++)
  {
    dt_colorrings_sparse_anchor_t anchor = { 0.f };
    if(_build_edge_anchor(p, _chroma_edges[edge], lut_profile, &anchor))
      _append_anchor(anchors, &anchor_count, &anchor);
  }

  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t inner_anchor = { 0.f };
    dt_aligned_pixel_t inner_source_rgb = { 0.f };
    if(_build_node_radial_midpoint_anchor(p, (dt_iop_colorprimaries_node_t)node, lut_profile, &inner_anchor,
                                          inner_source_rgb))
      _append_anchor(anchors, &anchor_count, &inner_anchor);
  }

  for(int edge = 0; edge < DT_IOP_COLORPRIMARIES_EDGE_COUNT; edge++)
  {
    dt_colorrings_sparse_anchor_t inner_anchor = { 0.f };
    dt_aligned_pixel_t inner_source_rgb = { 0.f };
    if(_build_edge_radial_midpoint_anchor(p, _chroma_edges[edge], lut_profile, &inner_anchor, inner_source_rgb))
      _append_anchor(anchors, &anchor_count, &inner_anchor);
  }

  for(int white = 0; white < 2; white++)
    for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_colorrings_sparse_anchor_t extreme_anchor = { 0.f };
    dt_aligned_pixel_t midway_rgb = { 0.f };
    if(_build_node_black_white_midpoint_anchor(p, (dt_iop_colorprimaries_node_t)node, white != 0, lut_profile,
                                               &extreme_anchor, midway_rgb))
      _append_anchor(anchors, &anchor_count, &extreme_anchor);
  }

  for(int sample = 0; sample < DT_IOP_COLORPRIMARIES_AXIS_ANCHORS; sample++)
  {
    const float value = (float)sample / (float)(DT_IOP_COLORPRIMARIES_AXIS_ANCHORS - 1);
    anchors[anchor_count].L = value * DT_IOP_COLORPRIMARIES_SQRT3;
    anchors[anchor_count].rho = 0.f;
    anchors[anchor_count].theta = 0.f;
    anchors[anchor_count].delta_L = 0.f;
    anchors[anchor_count].chroma_scale = 1.f;
    anchors[anchor_count].delta_theta = 0.f;
    anchors[anchor_count].weight = 1.f / (float)DT_IOP_COLORPRIMARIES_AXIS_ANCHORS;
    anchor_count++;
  }

  dt_colorrings_fill_lut_sparse_local_field(d->clut, DT_COLORRINGS_CLUT_LEVEL, anchors, anchor_count, inv_sigma_L,
                                            inv_sigma_rho, inv_sigma_theta, fmaxf(p->neutral_protection, 1e-6f));
  d->clut_level = DT_COLORRINGS_CLUT_LEVEL;

  if(log_perf)
    dt_print(DT_DEBUG_PERF, "[colorprimaries] build_clut level=%u anchors=%d total=%.3fms\n", d->clut_level,
             anchor_count, 1000.0 * (dt_get_wtime() - start));
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorprimaries_params_t *p = (const dt_iop_colorprimaries_params_t *)p1;
  dt_iop_colorprimaries_data_t *d = (dt_iop_colorprimaries_data_t *)piece->data;
  dt_iop_colorprimaries_global_data_t *gd = (dt_iop_colorprimaries_global_data_t *)self->global_data;
  const dt_iop_order_iccprofile_info_t *lut_profile
      = self->dev ? dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL)
                  : NULL;
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);

  if(!d)
  {
    return;
  }

  if(!gd || !lut_profile || !work_profile)
  {
    d->clut = NULL;
    d->clut_level = 0;
    d->white_level = 1.f;
    d->lut_profile = NULL;
    d->work_profile = NULL;
    d->interpolation = DT_LUT3D_INTERP_TETRAHEDRAL;
    return;
  }

  dt_pthread_rwlock_wrlock(&gd->lock);
  if(!gd->cache_valid || !_lut_fields_equal(&gd->params, p))
  {
    _build_clut(&gd->cache, p, lut_profile);
    memcpy(&gd->params, p, sizeof(*p));
    gd->cache_valid = TRUE;
    gd->cache_generation++;
  }

  d->clut = gd->cache.clut;
  d->clut_level = gd->cache.clut_level;
  d->white_level = exp2f(p->white_level);
  d->lut_profile = (dt_iop_order_iccprofile_info_t *)lut_profile;
  d->work_profile = (dt_iop_order_iccprofile_info_t *)work_profile;
  d->interpolation = (dt_lut3d_interpolation_t)p->interpolation;
  dt_pthread_rwlock_unlock(&gd->lock);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorprimaries_data_t *d = dt_calloc_align(sizeof(dt_iop_colorprimaries_data_t));
  piece->data = d;
  piece->data_size = sizeof(dt_iop_colorprimaries_data_t);
  d->white_level = 1.f;
  d->interpolation = DT_LUT3D_INTERP_TETRAHEDRAL;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorprimaries_data_t *d = (dt_iop_colorprimaries_data_t *)piece->data;
  d->clut = NULL;
  dt_free_align(piece->data);
  piece->data = NULL;
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ibuf, void *const obuf)
{
  const dt_iop_colorprimaries_data_t *d = (const dt_iop_colorprimaries_data_t *)piece->data;
  dt_iop_colorprimaries_global_data_t *gd = (dt_iop_colorprimaries_global_data_t *)self->global_data;
  const int width = piece->roi_in.width;
  const int height = piece->roi_in.height;
  const int ch = piece->dsc_in.channels;

  if(!d || !gd || !d->clut || d->clut_level == 0 || !d->lut_profile || !d->work_profile)
  {
    dt_iop_image_copy_by_size(obuf, ibuf, width, height, ch);
    return 0;
  }

  const float white_level = fmaxf(d->white_level, 1e-6f);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    const float *in = (const float *)ibuf + k * ch;
    float *out = (float *)obuf + k * ch;
    out[0] = in[0] / white_level;
    out[1] = in[1] / white_level;
    out[2] = in[2] / white_level;
    if(ch > 3) out[3] = in[3];
  }

  dt_ioppr_transform_image_colorspace_rgb((float *)obuf, (float *)obuf, width, height, d->work_profile, d->lut_profile,
                                          "colorprimaries work to HLG Rec2020");
  dt_pthread_rwlock_rdlock(&gd->lock);
  dt_lut3d_apply((float *)obuf, (float *)obuf, (size_t)width * height, d->clut, d->clut_level, 1.f, d->interpolation);
  dt_pthread_rwlock_unlock(&gd->lock);
  dt_ioppr_transform_image_colorspace_rgb((float *)obuf, (float *)obuf, width, height, d->lut_profile, d->work_profile,
                                          "colorprimaries HLG Rec2020 to work");
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    float *out = (float *)obuf + k * ch;
    out[0] *= white_level;
    out[1] *= white_level;
    out[2] *= white_level;
  }

  return 0;
}

static void _pipe_rgb_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_aligned_pixel_t RGB,
                             dt_aligned_pixel_t Ych)
{
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(!work_profile)
  {
    memset(Ych, 0, sizeof(dt_aligned_pixel_t));
    return;
  }

  dt_colorrings_profile_rgb_to_Ych(RGB, work_profile, Ych);
}

static void _set_slider_stop_from_hsb(GtkWidget *slider, const float stop, const dt_aligned_pixel_t HSB,
                                      const dt_iop_order_iccprofile_info_t *display_profile)
{
  dt_aligned_pixel_t RGB = { 0.f };
  dt_colorrings_hsb_to_display_rgb(HSB, dt_colorrings_graph_white(), display_profile, RGB);
  dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
}

static void _set_slider_stop_from_profile_rgb(GtkWidget *slider, const float stop, const dt_aligned_pixel_t RGB,
                                              const dt_iop_order_iccprofile_info_t *lut_profile,
                                              const dt_iop_order_iccprofile_info_t *display_profile)
{
  dt_aligned_pixel_t display_rgb = { 0.f };
  dt_colorrings_profile_rgb_to_display_rgb(RGB, lut_profile, display_profile, display_rgb);
  dt_bauhaus_slider_set_stop(slider, stop, display_rgb[0], display_rgb[1], display_rgb[2]);
}

static void _refresh_slider_gradients(dt_iop_module_t *self)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  const dt_iop_colorprimaries_params_t *p = (const dt_iop_colorprimaries_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *lut_profile
      = self->dev ? dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL)
                  : NULL;
  const dt_iop_order_iccprofile_info_t *display_profile
      = (self->dev && self->dev->preview_pipe) ? dt_ioppr_get_pipe_output_profile_info(self->dev->preview_pipe)
                                               : NULL;

  if(!g || !lut_profile || !p) return;

  /**
   * The slider stops are purely visual guides. Convert every dt UCS HSB stop
   * through the current display profile so the gradients match the monitor
   * gamut and tone response used by the graph backgrounds elsewhere.
   */
  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_aligned_pixel_t source_hsb = { 0.f };
    dt_aligned_pixel_t target_hsb = { 0.f };
    _node_source_hsb(p, (dt_iop_colorprimaries_node_t)node, lut_profile, source_hsb);
    _node_target_hsb(p, (dt_iop_colorprimaries_node_t)node, lut_profile, target_hsb);

    dt_bauhaus_slider_clear_stops(g->node_hue[node]);
    for(int stop = 0; stop <= 6; stop++)
    {
      const float hue_shift = -M_PI_F + 2.f * M_PI_F * (float)stop / 6.f;
      dt_aligned_pixel_t HSB = { dt_colorrings_wrap_hue_2pi(source_hsb[0] + hue_shift), target_hsb[1], target_hsb[2],
                                 0.f };
      _set_slider_stop_from_hsb(g->node_hue[node], (float)stop / 6.f, HSB, display_profile);
    }

    dt_bauhaus_slider_clear_stops(g->node_saturation[node]);
    {
      dt_aligned_pixel_t axis_rgb = { 0.f };
      dt_aligned_pixel_t target_rgb = { 0.f };
      dt_aligned_pixel_t shell_rgb = { 0.f };

      /**
       * The saturation slider is meant to show the actual chroma axis of the
       * edited node, not arbitrary dt UCS saturation extrema. Build its color
       * ramp from the edited node RGB itself: grey at the same brightness,
       * current node color in the middle, and the same RGB direction extended
       * to the LUT cube shell at the top end.
       */
      dt_colorrings_brightness_to_axis_rgb(target_hsb[2], dt_colorrings_graph_white(), lut_profile, axis_rgb);
      dt_colorrings_hsb_to_profile_rgb(target_hsb, dt_colorrings_graph_white(), lut_profile, target_rgb);
      memcpy(shell_rgb, target_rgb, sizeof(shell_rgb));
      dt_colorrings_project_to_cube_shell(axis_rgb, shell_rgb);

      _set_slider_stop_from_profile_rgb(g->node_saturation[node], 0.f, axis_rgb, lut_profile, display_profile);
      _set_slider_stop_from_profile_rgb(g->node_saturation[node], 0.5f, target_rgb, lut_profile, display_profile);
      _set_slider_stop_from_profile_rgb(g->node_saturation[node], 1.f, shell_rgb, lut_profile, display_profile);
    }

    dt_bauhaus_slider_clear_stops(g->node_brightness[node]);
    _set_slider_stop_from_hsb(g->node_brightness[node], 0.f,
                              (dt_aligned_pixel_t){ target_hsb[0], target_hsb[1], CLAMP(source_hsb[2] - 0.05f, 0.f, 1.f),
                                                    0.f },
                              display_profile);
    _set_slider_stop_from_hsb(g->node_brightness[node], 0.5f,
                              (dt_aligned_pixel_t){ target_hsb[0], target_hsb[1], source_hsb[2], 0.f }, display_profile);
    _set_slider_stop_from_hsb(g->node_brightness[node], 1.f,
                              (dt_aligned_pixel_t){ target_hsb[0], target_hsb[1], CLAMP(source_hsb[2] + 0.05f, 0.f, 1.f),
                                                    0.f },
                              display_profile);
  }
}

static void _preview_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  if(!g || !g->viewer) return;

  _refresh_slider_gradients(self);
  _update_gui_lut_cache(self);
  dt_lut_viewer_queue_draw(g->viewer);
}

static void _update_gui_lut_cache(dt_iop_module_t *self)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  dt_iop_colorprimaries_global_data_t *gd = (dt_iop_colorprimaries_global_data_t *)self->global_data;
  const dt_iop_colorprimaries_params_t *p = (const dt_iop_colorprimaries_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *lut_profile
      = self->dev ? dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL)
                  : NULL;
  const dt_iop_order_iccprofile_info_t *display_profile
      = (self->dev && self->dev->preview_pipe) ? dt_ioppr_get_pipe_output_profile_info(self->dev->preview_pipe)
                                               : NULL;
  uint64_t cache_generation = 0;

  if(!g || !g->viewer || !gd) return;

  dt_pthread_rwlock_rdlock(&gd->lock);
  cache_generation = gd->cache_generation;
  dt_pthread_rwlock_unlock(&gd->lock);

  if(!g->viewer_lut_dirty && g->viewer_lut_valid && g->viewer_lut_generation == cache_generation
     && g->viewer_display_profile == display_profile)
    return;

  if(!lut_profile)
  {
    dt_lut_viewer_set_lut(g->viewer, NULL, 0, NULL, NULL, NULL);
    dt_lut_viewer_set_control_nodes(g->viewer, NULL, 0);
    g->viewer_lut_dirty = FALSE;
    g->viewer_lut_valid = FALSE;
    g->viewer_lut_generation = 0;
    g->viewer_display_profile = display_profile;
    g->viewer_control_node_count = 0;
    return;
  }

  dt_pthread_rwlock_wrlock(&gd->lock);
  if(!gd->cache_valid || !_lut_fields_equal(&gd->params, p))
  {
    _build_clut(&gd->cache, p, lut_profile);
    memcpy(&gd->params, p, sizeof(*p));
    gd->cache_valid = TRUE;
    gd->cache_generation++;
  }

  cache_generation = gd->cache_generation;
  g->viewer_lut.clut = gd->cache.clut;
  g->viewer_lut.clut_level = gd->cache.clut_level;
  g->viewer_lut.lut_profile = (dt_iop_order_iccprofile_info_t *)lut_profile;
  g->viewer_control_node_count
      = _build_viewer_control_nodes(p, lut_profile, g->viewer_control_nodes);
  dt_lut_viewer_set_lut(g->viewer, g->viewer_lut.clut, g->viewer_lut.clut_level, &gd->lock, g->viewer_lut.lut_profile,
                        display_profile);
  dt_lut_viewer_set_control_nodes(g->viewer, g->viewer_control_nodes, g->viewer_control_node_count);
  dt_pthread_rwlock_unlock(&gd->lock);

  g->viewer_lut_dirty = FALSE;
  g->viewer_lut_valid = TRUE;
  g->viewer_lut_generation = cache_generation;
  g->viewer_display_profile = display_profile;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  if(!g) return;

  g->viewer_lut_dirty = TRUE;
  _refresh_slider_gradients(self);
  if(g->viewer)
  {
    _update_gui_lut_cache(self);
    dt_lut_viewer_queue_draw(g->viewer);
  }
}

void gui_update(dt_iop_module_t *self)
{
  const dt_iop_colorprimaries_params_t *p = (const dt_iop_colorprimaries_params_t *)self->params;
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  if(!g) return;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->white_level, p->white_level);
  dt_bauhaus_slider_set(g->gamut_coverage, p->gamut_coverage);
  dt_bauhaus_slider_set(g->sigma_L, p->sigma_L);
  dt_bauhaus_slider_set(g->sigma_rho, p->sigma_rho);
  dt_bauhaus_slider_set(g->sigma_theta, p->sigma_theta);
  dt_bauhaus_slider_set(g->neutral_protection, p->neutral_protection);
  dt_bauhaus_combobox_set(g->interpolation, p->interpolation);

  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    dt_bauhaus_slider_set(g->node_hue[node], p->hue[node]);
    dt_bauhaus_slider_set(g->node_saturation[node], p->saturation[node]);
    dt_bauhaus_slider_set(g->node_brightness[node], p->brightness[node]);
  }
  --darktable.gui->reset;

  _refresh_slider_gradients(self);
  g->viewer_lut_dirty = TRUE;
  if(g->viewer)
  {
    _update_gui_lut_cache(self);
    dt_lut_viewer_queue_draw(g->viewer);
  }
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  if(!g) return;

  if(in)
  {
    if(!g->preview_signal_connected)
    {
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                      G_CALLBACK(_preview_pipe_finished_callback), self);
      g->preview_signal_connected = TRUE;
    }

    if(g->viewer)
    {
      _refresh_slider_gradients(self);
      _update_gui_lut_cache(self);
      dt_lut_viewer_queue_draw(g->viewer);
    }
  }
  else if(g->preview_signal_connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_preview_pipe_finished_callback), self);
    g->preview_signal_connected = FALSE;
  }
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  if(g->preview_signal_connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_preview_pipe_finished_callback), self);
    g->preview_signal_connected = FALSE;
  }

  g->viewer_lut.clut = NULL;
  dt_lut_viewer_destroy(&g->viewer);
  IOP_GUI_FREE;
}

static GtkWidget *_new_section_label(GtkWidget *box, const char *label)
{
  GtkWidget *section = dt_ui_section_label_new(label);
  gtk_widget_set_hexpand(section, TRUE);
  gtk_box_pack_start(GTK_BOX(box), section, FALSE, FALSE, 0);
  return section;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe,
                        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorprimaries_gui_data_t *g = (dt_iop_colorprimaries_gui_data_t *)self->gui_data;
  dt_iop_colorprimaries_params_t *p = (dt_iop_colorprimaries_params_t *)self->params;
  const dt_iop_module_t *sampled_module = piece && piece->module ? piece->module : self;

  if(!g || picker != g->white_level) return;
  if(sampled_module->picked_color_max[0] < sampled_module->picked_color_min[0]) return;

  dt_aligned_pixel_t max_Ych = { 0.f };
  _pipe_rgb_to_Ych(self, pipe, (const float *)sampled_module->picked_color_max, max_Ych);
  ++darktable.gui->reset;
  p->white_level = log2f(fmaxf(max_Ych[0], 1e-6f));
  dt_bauhaus_slider_set(g->white_level, p->white_level);
  --darktable.gui->reset;
  g->viewer_lut_dirty = TRUE;
  _update_gui_lut_cache(self);
  if(g->viewer) dt_lut_viewer_queue_draw(g->viewer);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorprimaries_gui_data_t *g = IOP_GUI_ALLOC(colorprimaries);
  const dt_iop_colorprimaries_params_t *defaults = (const dt_iop_colorprimaries_params_t *)self->default_params;

  g->viewer_lut_dirty = TRUE;
  g->viewer_lut_valid = FALSE;
  g->viewer_lut_generation = 0;
  g->viewer_lut.clut = NULL;
  g->viewer_lut.clut_level = 0;
  g->viewer_lut.lut_profile = NULL;
  g->viewer_lut.work_profile = NULL;
  g->preview_signal_connected = FALSE;
  g->viewer_display_profile = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->tabs = GTK_NOTEBOOK(gtk_notebook_new());
  gtk_notebook_set_show_border(g->tabs, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->tabs), TRUE, TRUE, 0);

  GtkWidget *colors_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  GtkWidget *colors_tab = gtk_label_new(_("colors"));
  dt_gui_add_class(colors_tab, "dt_modulegroups_tab_label");
  gtk_notebook_append_page(g->tabs, colors_page, colors_tab);
  gtk_container_child_set(GTK_CONTAINER(g->tabs), colors_page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  GtkWidget *const module_root = self->widget;
  self->widget = colors_page;
  for(int node = 0; node < DT_IOP_COLORPRIMARIES_NODE_COUNT; node++)
  {
    char hue_name[16] = { 0 };
    char saturation_name[24] = { 0 };
    char brightness_name[24] = { 0 };
    snprintf(hue_name, sizeof(hue_name), "hue[%d]", node);
    snprintf(saturation_name, sizeof(saturation_name), "saturation[%d]", node);
    snprintf(brightness_name, sizeof(brightness_name), "brightness[%d]", node);

    _new_section_label(colors_page, _node_name((dt_iop_colorprimaries_node_t)node));
    g->node_hue[node] = dt_bauhaus_slider_from_params(self, hue_name);
    dt_bauhaus_widget_set_label(g->node_hue[node], N_("hue"));
    dt_bauhaus_slider_set_format(g->node_hue[node], _("°"));
    dt_bauhaus_slider_set_default(g->node_hue[node], defaults->hue[node]);

    g->node_saturation[node] = dt_bauhaus_slider_from_params(self, saturation_name);
    dt_bauhaus_widget_set_label(g->node_saturation[node], N_("saturation"));
    dt_bauhaus_slider_set_format(g->node_saturation[node], _(" %"));
    dt_bauhaus_slider_set_default(g->node_saturation[node], defaults->saturation[node]);

    g->node_brightness[node] = dt_bauhaus_slider_from_params(self, brightness_name);
    dt_bauhaus_widget_set_label(g->node_brightness[node], N_("brightness"));
    dt_bauhaus_slider_set_format(g->node_brightness[node], _(" %"));
    dt_bauhaus_slider_set_soft_range(g->node_brightness[node], -0.25f, 0.25f);
    dt_bauhaus_slider_set_default(g->node_brightness[node], defaults->brightness[node]);
  }
  self->widget = module_root;

  GtkWidget *options_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  GtkWidget *options_tab = gtk_label_new(_("options"));
  dt_gui_add_class(options_tab, "dt_modulegroups_tab_label");
  gtk_notebook_append_page(g->tabs, options_page, options_tab);
  gtk_container_child_set(GTK_CONTAINER(g->tabs), options_page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  self->widget = options_page;
  g->white_level = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_level"));
  dt_bauhaus_slider_set_default(g->white_level, defaults->white_level);
  dt_bauhaus_slider_set_format(g->white_level, _(" EV"));
  dt_bauhaus_slider_set_soft_range(g->white_level, -2.f, 2.f);

  g->gamut_coverage = dt_bauhaus_slider_from_params(self, "gamut_coverage");
  dt_bauhaus_slider_set_default(g->gamut_coverage, defaults->gamut_coverage);
  dt_bauhaus_slider_set_format(g->gamut_coverage, _(" %"));
  dt_bauhaus_slider_set_soft_range(g->gamut_coverage, 0.f, 100.f);

  g->sigma_L = dt_bauhaus_slider_from_params(self, "sigma_L");
  dt_bauhaus_slider_set_default(g->sigma_L, defaults->sigma_L);
  dt_bauhaus_slider_set_format(g->sigma_L, _(" %"));
  dt_bauhaus_slider_set_soft_range(g->sigma_L, 1.f, 100.f);

  g->sigma_rho = dt_bauhaus_slider_from_params(self, "sigma_rho");
  dt_bauhaus_slider_set_default(g->sigma_rho, defaults->sigma_rho);
  dt_bauhaus_slider_set_soft_range(g->sigma_rho, 0.1f, 2.f);

  g->sigma_theta = dt_bauhaus_slider_from_params(self, "sigma_theta");
  dt_bauhaus_slider_set_default(g->sigma_theta, defaults->sigma_theta);
  dt_bauhaus_slider_set_soft_range(g->sigma_theta, 0.05f, 2.f);

  g->neutral_protection = dt_bauhaus_slider_from_params(self, "neutral_protection");
  dt_bauhaus_slider_set_default(g->neutral_protection, defaults->neutral_protection);
  dt_bauhaus_slider_set_soft_range(g->neutral_protection, 0.f, 1.f);

  g->interpolation = dt_bauhaus_combobox_from_params(self, "interpolation");
  dt_bauhaus_combobox_set_default(g->interpolation, defaults->interpolation);
  self->widget = module_root;

  g->viewer = dt_lut_viewer_new(DT_GUI_MODULE(self));
  if(g->viewer)
    gtk_box_pack_start(GTK_BOX(options_page), dt_lut_viewer_get_widget(g->viewer), TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);
  gui_update(self);
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorprimaries_global_data_t *gd = malloc(sizeof(*gd));
  memset(gd, 0, sizeof(*gd));
  module->data = gd;
  dt_pthread_rwlock_init(&gd->lock, NULL);
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorprimaries_global_data_t *gd = (dt_iop_colorprimaries_global_data_t *)module->data;
  dt_free_align(gd->cache.clut);
  gd->cache.clut = NULL;
  dt_pthread_rwlock_destroy(&gd->lock);
  dt_free(module->data);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  _init_default_params((dt_iop_colorprimaries_params_t *)module->default_params);
  memcpy(module->params, module->default_params, module->params_size);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
