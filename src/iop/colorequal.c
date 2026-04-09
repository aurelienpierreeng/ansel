/*
    This file is part of Ansel,
    Copyright (C) 2022-2026 Aurélien PIERRE.

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
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/lut3d.h"
#include "common/lut_viewer.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe_cache.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/gui_throttle.h"
#include "iop/iop_api.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/**
 * We keep the same GUI hue convention as the old prototype: the darktable UCS
 * hue of sRGB red sits around +20 degrees, so the graph is shifted to place red
 * at the left edge of the widget.
 */
#define DT_IOP_COLOREQUAL_NUM_CHANNELS 3
#define DT_IOP_COLOREQUAL_NUM_RINGS 3
#define DT_IOP_COLOREQUAL_MAXNODES 20
#define DT_IOP_COLOREQUAL_DEFAULT_NODES 8
#define DT_IOP_COLOREQUAL_GRAPH_RES 360
#define DT_IOP_COLOREQUAL_GRAPH_GRADIENTS 48
#define DT_IOP_COLOREQUAL_HUE_SAMPLES 64
#define DT_IOP_COLOREQUAL_VIEWER_CONTROL_NODES (DT_IOP_COLOREQUAL_NUM_RINGS * DT_IOP_COLOREQUAL_HUE_SAMPLES)
#define DT_IOP_COLOREQUAL_AXIAL_SAMPLES 64
#define DT_IOP_COLOREQUAL_CLUT_LEVEL 64
#define DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS (DT_IOP_COLOREQUAL_NUM_RINGS + 1)
#define DT_IOP_COLOREQUAL_MIN_X_DISTANCE 0.01f
#define DT_IOP_COLOREQUAL_PREVIEW_CURSOR_RADIUS DT_PIXEL_APPLY_DPI(14.f)
#define DT_IOP_COLOREQUAL_GRAPH_INSET DT_PIXEL_APPLY_DPI(4)
#define DT_IOP_COLOREQUAL_AXIS_HEIGHT DT_PIXEL_APPLY_DPI(14)
#define DT_IOP_COLOREQUAL_SCROLL_SIGMA 2.f * M_PI_F / 128.f
#define DT_IOP_COLOREQUAL_SCROLL_STEP 0.05f
#define DT_IOP_COLOREQUAL_SCROLL_STEP_FINE 0.02f
#define DT_IOP_COLOREQUAL_SCROLL_STEP_COARSE 0.10f
#define DT_IOP_COLOREQUAL_SCROLL_HUE_STEP (5.f * M_PI_F / 180.f)
#define DT_IOP_COLOREQUAL_SCROLL_HUE_STEP_FINE (1.f * M_PI_F / 180.f)
#define DT_IOP_COLOREQUAL_SCROLL_HUE_STEP_COARSE (10.f * M_PI_F / 180.f)

DT_MODULE_INTROSPECTION(1, dt_iop_colorequal_params_t)

typedef enum dt_iop_colorequal_channel_t
{
  DT_IOP_COLOREQUAL_SATURATION = 0,
  DT_IOP_COLOREQUAL_HUE = 1,
  DT_IOP_COLOREQUAL_BRIGHTNESS = 2,
} dt_iop_colorequal_channel_t;

typedef enum dt_iop_colorequal_ring_t
{
  DT_IOP_COLOREQUAL_RING_DARK = 0,
  DT_IOP_COLOREQUAL_RING_MID = 1,
  DT_IOP_COLOREQUAL_RING_LIGHT = 2,
} dt_iop_colorequal_ring_t;

typedef enum dt_iop_colorequal_interpolation_t
{
  DT_IOP_COLOREQUAL_TETRAHEDRAL = 0, // $DESCRIPTION: "tetrahedral"
  DT_IOP_COLOREQUAL_TRILINEAR = 1,   // $DESCRIPTION: "trilinear"
  DT_IOP_COLOREQUAL_PYRAMID = 2,     // $DESCRIPTION: "pyramid"
} dt_iop_colorequal_interpolation_t;

typedef struct dt_iop_colorequal_node_t
{
  float x;
  float y;
} dt_iop_colorequal_node_t;

typedef struct dt_iop_colorequal_params_t
{
  float white_level; // $MIN: -2.0 $MAX: 16.0 $DEFAULT: 1.0 $DESCRIPTION: "white level"
  float sigma_L;     // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "brightness smoothing"
  float sigma_rho;   // $MIN: 0.01 $MAX: 2.0 $DEFAULT: 1 $DESCRIPTION: "saturation smoothing"
  float sigma_theta; // $MIN: 0.01 $MAX: 6.28318531 $DEFAULT: 0.40 $DESCRIPTION: "hue smoothing"
  float neutral_protection; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.05 $DESCRIPTION: "neutral protection"
  dt_iop_colorequal_interpolation_t interpolation; // $DEFAULT: DT_IOP_COLOREQUAL_TETRAHEDRAL $DESCRIPTION: "interpolation"
  dt_iop_colorequal_node_t curve[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS]
                                [DT_IOP_COLOREQUAL_MAXNODES];
  int curve_num_nodes[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
} dt_iop_colorequal_params_t;

typedef struct dt_iop_colorequal_data_t
{
  float *clut;
  uint16_t clut_level;
  float white_level;
  float reference_saturation[DT_IOP_COLOREQUAL_NUM_RINGS];
  dt_iop_order_iccprofile_info_t *lut_profile;
  dt_iop_order_iccprofile_info_t *work_profile;
  dt_lut3d_interpolation_t interpolation;
} dt_iop_colorequal_data_t;

typedef struct dt_iop_colorequal_global_data_t
{
  dt_pthread_rwlock_t lock;
  dt_iop_colorequal_data_t cache;
  dt_iop_colorequal_params_t params;
  gboolean cache_valid;
  uint64_t cache_generation;
  int kernel_lut3d_tetrahedral;
  int kernel_lut3d_trilinear;
  int kernel_lut3d_pyramid;
  int kernel_exposure;
} dt_iop_colorequal_global_data_t;

typedef struct dt_iop_colorequal_gui_data_t
{
  GtkDrawingArea *area[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  GtkNotebook *ring_notebook;
  GtkNotebook *channel_notebook[DT_IOP_COLOREQUAL_NUM_RINGS];
  GtkWidget *white_level;
  GtkWidget *module_picker;
  GtkWidget *picker_info;
  GtkWidget *sigma_L;
  GtkWidget *sigma_rho;
  GtkWidget *sigma_theta;
  GtkWidget *neutral_protection;
  GtkWidget *interpolation;
  dt_lut_viewer_t *viewer;
  dt_iop_colorequal_data_t viewer_lut;
  uint64_t viewer_lut_generation;
  dt_iop_colorequal_params_t gui_params;
  dt_iop_colorequal_params_t cached_curve_params;
  dt_draw_curve_t *curve[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  int curve_nodes[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  float draw_ys[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS][DT_IOP_COLOREQUAL_GRAPH_RES];
  int selected[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  gboolean dragging[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  gboolean curve_cache_valid;
  gboolean viewer_lut_dirty;
  gboolean viewer_lut_valid;
  gboolean preview_signal_connected;
  uint64_t pending_preview_hash;
  gboolean has_focus;
  gboolean picker_valid;
  gboolean cursor_valid;
  gboolean cursor_sample_valid;
  float picker_hue;
  float picker_brightness;
  float cursor_hue;
  int cursor_pos_x;
  int cursor_pos_y;
  dt_aligned_pixel_t cursor_input_display;
  dt_aligned_pixel_t cursor_output_display;
  dt_lut_viewer_control_node_t viewer_control_nodes[DT_IOP_COLOREQUAL_VIEWER_CONTROL_NODES];
  size_t viewer_control_node_count;
  float reference_saturation[DT_IOP_COLOREQUAL_NUM_RINGS];
  float cached_white[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  float cached_reference_saturation[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  int cached_width[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  int cached_height[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
  const dt_iop_order_iccprofile_info_t *cached_display_profile[DT_IOP_COLOREQUAL_NUM_RINGS]
                                                              [DT_IOP_COLOREQUAL_NUM_CHANNELS];
  cairo_surface_t *background_surface[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_NUM_CHANNELS];
} dt_iop_colorequal_gui_data_t;

const char *name()
{
  return _("color equalizer");
}

const char *aliases()
{
  return _("color zones");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(
      self, _("stretch RGB colors around the achromatic axis from editable dt UCS hue nodes"), _("creative"),
      _("linear, RGB, display-referred"), _("linear, RGB"), _("linear, RGB, display-referred"));
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
static void _switch_preview_cursor(dt_iop_module_t *self);
static gboolean _refresh_preview_cursor_sample(dt_iop_module_t *self);

static inline float _channel_value_from_y(const dt_iop_colorequal_channel_t channel, const float y)
{
  switch(channel)
  {
    case DT_IOP_COLOREQUAL_HUE:
      return (y - 0.5f) * 2.f * M_PI_F;
    case DT_IOP_COLOREQUAL_SATURATION:
    case DT_IOP_COLOREQUAL_BRIGHTNESS:
    default:
      return CLAMP(y * 2.f, 0.f, 2.f);
  }
}

static inline float _channel_y_from_value(const dt_iop_colorequal_channel_t channel, const float value)
{
  switch(channel)
  {
    case DT_IOP_COLOREQUAL_HUE:
      return CLAMP(value / (2.f * M_PI_F) + 0.5f, 0.f, 1.f);
    case DT_IOP_COLOREQUAL_SATURATION:
    case DT_IOP_COLOREQUAL_BRIGHTNESS:
    default:
      return CLAMP(value * 0.5f, 0.f, 1.f);
  }
}

static inline const char *_ring_label(const dt_iop_colorequal_ring_t ring)
{
  switch(ring)
  {
    case DT_IOP_COLOREQUAL_RING_DARK:
      return _("shadows");
    case DT_IOP_COLOREQUAL_RING_LIGHT:
      return _("highlights");
    case DT_IOP_COLOREQUAL_RING_MID:
    default:
      return _("midtones");
  }
}

static inline dt_iop_colorequal_node_t *_curve_nodes(dt_iop_colorequal_params_t *p,
                                                     const dt_iop_colorequal_ring_t ring,
                                                     const dt_iop_colorequal_channel_t channel)
{
  return p->curve[ring][channel];
}

static inline const dt_iop_colorequal_node_t *_curve_nodes_const(const dt_iop_colorequal_params_t *p,
                                                                 const dt_iop_colorequal_ring_t ring,
                                                                 const dt_iop_colorequal_channel_t channel)
{
  return p->curve[ring][channel];
}

static inline int *_curve_nodes_count(dt_iop_colorequal_params_t *p, const dt_iop_colorequal_ring_t ring,
                                      const dt_iop_colorequal_channel_t channel)
{
  return &p->curve_num_nodes[ring][channel];
}

static inline int _curve_nodes_count_const(const dt_iop_colorequal_params_t *p,
                                           const dt_iop_colorequal_ring_t ring,
                                           const dt_iop_colorequal_channel_t channel)
{
  return p->curve_num_nodes[ring][channel];
}

static void _reset_channel_nodes(dt_iop_colorequal_params_t *p, const dt_iop_colorequal_ring_t ring,
                                 const dt_iop_colorequal_channel_t channel)
{
  *_curve_nodes_count(p, ring, channel) = DT_IOP_COLOREQUAL_DEFAULT_NODES;
  dt_iop_colorequal_node_t *curve = _curve_nodes(p, ring, channel);
  for(int k = 0; k < DT_IOP_COLOREQUAL_DEFAULT_NODES; k++)
  {
    curve[k].x = (float)k / (float)DT_IOP_COLOREQUAL_DEFAULT_NODES;
    curve[k].y = 0.5f;
  }
}

static void _init_default_curves(dt_iop_colorequal_params_t *p)
{
  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
    for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
      _reset_channel_nodes(p, (dt_iop_colorequal_ring_t)ring, (dt_iop_colorequal_channel_t)ch);
}

static inline gboolean _curve_fields_equal(const dt_iop_colorequal_params_t *const a,
                                           const dt_iop_colorequal_params_t *const b)
{
  return !memcmp(a->curve_num_nodes, b->curve_num_nodes, sizeof(a->curve_num_nodes))
         && !memcmp(a->curve, b->curve, sizeof(a->curve));
}

static inline gboolean _lut_fields_equal(const dt_iop_colorequal_params_t *const a,
                                         const dt_iop_colorequal_params_t *const b)
{
  return _curve_fields_equal(a, b) && a->sigma_L == b->sigma_L && a->sigma_rho == b->sigma_rho
         && a->sigma_theta == b->sigma_theta && a->neutral_protection == b->neutral_protection;
}

static inline float _curve_periodic_distance(const float x0, const float x1)
{
  const float distance = fabsf(x0 - x1);
  return fminf(distance, 1.f - distance);
}

static inline dt_iop_colorequal_channel_t _channel_from_page(const int page)
{
  switch(CLAMP(page, 0, DT_IOP_COLOREQUAL_NUM_CHANNELS - 1))
  {
    case 1:
      return DT_IOP_COLOREQUAL_BRIGHTNESS;
    case 2:
      return DT_IOP_COLOREQUAL_HUE;
    case 0:
    default:
      return DT_IOP_COLOREQUAL_SATURATION;
  }
}

static inline dt_iop_colorequal_ring_t _active_ring_from_gui(const dt_iop_colorequal_gui_data_t *g)
{
  const int page = (g && g->ring_notebook) ? gtk_notebook_get_current_page(g->ring_notebook) : -1;
  if(page >= 0 && page < DT_IOP_COLOREQUAL_NUM_RINGS) return (dt_iop_colorequal_ring_t)page;
  return (dt_iop_colorequal_ring_t)CLAMP(dt_conf_get_int("plugins/darkroom/colorequal/gui_ring_page"), 0,
                                         DT_IOP_COLOREQUAL_NUM_RINGS - 1);
}

static inline dt_iop_colorequal_channel_t _active_channel_from_gui(const dt_iop_colorequal_gui_data_t *g,
                                                                   const dt_iop_colorequal_ring_t ring)
{
  const int page = (g && g->channel_notebook[ring]) ? gtk_notebook_get_current_page(g->channel_notebook[ring]) : -1;
  if(page >= 0 && page < DT_IOP_COLOREQUAL_NUM_CHANNELS) return _channel_from_page(page);
  return _channel_from_page(dt_conf_get_int("plugins/darkroom/colorequal/gui_channel_page"));
}

static inline void _invalidate_preview_cursor(dt_iop_colorequal_gui_data_t *g)
{
  if(!g) return;
  g->cursor_sample_valid = FALSE;
  g->cursor_hue = 0.f;
  memset(g->cursor_input_display, 0, sizeof(g->cursor_input_display));
  memset(g->cursor_output_display, 0, sizeof(g->cursor_output_display));
}

static inline gboolean _cursor_curve_state(const dt_iop_colorequal_params_t *p, const dt_iop_colorequal_ring_t ring,
                                           const dt_iop_colorequal_channel_t channel, const float hue,
                                           float *curve_x, float *curve_y, float *offset_normalized)
{
  if(!p || !isfinite(hue)) return FALSE;

  const int nodes = _curve_nodes_count_const(p, ring, channel);
  const dt_iop_colorequal_node_t *curve = _curve_nodes_const(p, ring, channel);
  if(nodes < 1 || !curve) return FALSE;

  const float x = dt_colorrings_hue_to_curve_x(hue);
  const float y = dt_colorrings_curve_periodic_sample((const dt_colorrings_node_t *)curve, nodes, x);
  const float value = _channel_value_from_y(channel, y);

  if(curve_x) *curve_x = x;
  if(curve_y) *curve_y = y;

  if(offset_normalized)
  {
    switch(channel)
    {
      case DT_IOP_COLOREQUAL_HUE:
        *offset_normalized = CLAMP(value / M_PI_F, -1.f, 1.f);
        break;
      case DT_IOP_COLOREQUAL_SATURATION:
      case DT_IOP_COLOREQUAL_BRIGHTNESS:
      default:
        *offset_normalized = CLAMP(value - 1.f, -1.f, 1.f);
        break;
    }
  }

  return TRUE;
}

static inline void _clamp_display_rgb(dt_aligned_pixel_t RGB)
{
  RGB[0] = CLAMP(RGB[0], 0.f, 1.f);
  RGB[1] = CLAMP(RGB[1], 0.f, 1.f);
  RGB[2] = CLAMP(RGB[2], 0.f, 1.f);
  RGB[3] = 0.f;
}

static void _work_rgb_to_display_rgb(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_aligned_pixel_t work_rgb,
                                     dt_aligned_pixel_t display_rgb)
{
  if(!display_rgb) return;

  memcpy(display_rgb, work_rgb, sizeof(dt_aligned_pixel_t));
  _clamp_display_rgb(display_rgb);

  if(!self || !pipe) return;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(pipe);
  if(!work_profile || !display_profile) return;

  float in[4] = { work_rgb[0], work_rgb[1], work_rgb[2], 0.f };
  float out[4] = { work_rgb[0], work_rgb[1], work_rgb[2], 0.f };
  dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, work_profile, display_profile, "colorequal swatch");
  display_rgb[0] = out[0];
  display_rgb[1] = out[1];
  display_rgb[2] = out[2];
  _clamp_display_rgb(display_rgb);
}

static inline void _mix_rgb_anchors(const dt_aligned_pixel_t low, const dt_aligned_pixel_t high, const float mix,
                                    dt_aligned_pixel_t RGB)
{
  for(int c = 0; c < 3; c++) RGB[c] = low[c] * (1.f - mix) + high[c] * mix;
}

static inline void
_sample_ring_hue(const float ring_surface[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES][3],
                 const int ring, const float hue_position, dt_aligned_pixel_t RGB)
{
  const float hue = dt_colorrings_wrap_hue_2pi(hue_position * 2.f * M_PI_F) / (2.f * M_PI_F)
                    * DT_IOP_COLOREQUAL_HUE_SAMPLES;
  const int hue0 = ((int)floorf(hue)) % DT_IOP_COLOREQUAL_HUE_SAMPLES;
  const int hue1 = (hue0 + 1) % DT_IOP_COLOREQUAL_HUE_SAMPLES;
  const float mix = hue - floorf(hue);

  for(int c = 0; c < 3; c++)
    RGB[c] = ring_surface[ring][hue0][c] * (1.f - mix) + ring_surface[ring][hue1][c] * mix;
}

/**
 * The editable hue rings are defined in dt UCS HSB brightness. We therefore
 * sample them by that same brightness coordinate when extending the sparse ring
 * displacements to the full RGB cube. Black and white stay fixed and provide
 * the boundary conditions of the interpolation.
 */
static inline void
_sample_ring_anchor(const float ring_surface[DT_IOP_COLOREQUAL_NUM_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES][3],
                    const float brightness, const float hue_position, const float white,
                    const dt_iop_order_iccprofile_info_t *const profile, dt_aligned_pixel_t RGB)
{
  const float anchor_positions[DT_IOP_COLOREQUAL_NUM_RINGS + 2]
      = { 0.f,
          dt_colorrings_ring_brightness((dt_colorrings_ring_t)DT_IOP_COLOREQUAL_RING_DARK),
          dt_colorrings_ring_brightness((dt_colorrings_ring_t)DT_IOP_COLOREQUAL_RING_MID),
          dt_colorrings_ring_brightness((dt_colorrings_ring_t)DT_IOP_COLOREQUAL_RING_LIGHT),
          1.f };
  int segment = 0;

  while(segment < DT_IOP_COLOREQUAL_NUM_RINGS + 1 && brightness > anchor_positions[segment + 1]) segment++;

  const float low_position = anchor_positions[segment];
  const float high_position = anchor_positions[segment + 1];
  const float mix
      = (high_position > low_position) ? (brightness - low_position) / (high_position - low_position) : 0.f;
  dt_aligned_pixel_t low = { 0.f };
  dt_aligned_pixel_t high = { 0.f };

  if(segment == 0)
  {
    dt_colorrings_brightness_to_axis_rgb(0.f, white, profile, low);
    _sample_ring_hue(ring_surface, 0, hue_position, high);
  }
  else if(segment == DT_IOP_COLOREQUAL_NUM_RINGS)
  {
    _sample_ring_hue(ring_surface, DT_IOP_COLOREQUAL_NUM_RINGS - 1, hue_position, low);
    dt_colorrings_brightness_to_axis_rgb(1.f, white, profile, high);
  }
  else
  {
    _sample_ring_hue(ring_surface, segment - 1, hue_position, low);
    _sample_ring_hue(ring_surface, segment, hue_position, high);
  }

  _mix_rgb_anchors(low, high, CLAMP(mix, 0.f, 1.f), RGB);
}

/**
 * Build a procedural RGB CLUT from the three dt UCS HSB hue rings.
 *
 * The editable control surface lives in dt UCS HSB because it gives uniform
 * hue, saturation and brightness handles in the GUI. The CLUT itself however
 * is authored in HLG Rec2020 code values, so the LUT generation never leaves
 * those coordinates once the sparse `before -> after` ring samples have been
 * turned into Rec2020 HLG RGB anchors.
 *
 * The deformation model is:
 * - black and white stay fixed,
 * - each CLUT lattice point is first interpreted in dt UCS HSB only to know
 *   which ring displacement to sample from the user controls,
 * - that sampled ring displacement is then applied as a local 3D scale and
 *   rotation around the neutral point of the same dt UCS brightness, in HLG
 *   Rec2020 code coordinates,
 * - every dense lattice point is finally projected back to the RGB cube shell
 *   when needed, so the authored LUT itself never carries out-of-gamut values.
 *
 * This avoids assuming that the dt UCS hue rings become flat slices orthogonal
 * to the RGB diagonal once projected into the LUT profile.
 */
static void _build_clut(dt_iop_colorequal_data_t *d, const dt_iop_colorequal_params_t *p,
                        const dt_iop_order_iccprofile_info_t *lut_profile)
{
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double start = log_perf ? dt_get_wtime() : 0.0;
  const float white = dt_colorrings_graph_white();
  const size_t clut_size
      = (size_t)DT_IOP_COLOREQUAL_CLUT_LEVEL * DT_IOP_COLOREQUAL_CLUT_LEVEL * DT_IOP_COLOREQUAL_CLUT_LEVEL * 3;

  if(!d->clut) d->clut = dt_alloc_align_float(clut_size);
  d->lut_profile = (dt_iop_order_iccprofile_info_t *)lut_profile;

  dt_colorrings_compute_reference_saturations(white, d->reference_saturation);
  float anchor_L[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES] = { { 0.f } };
  float anchor_rho[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES] = { { 0.f } };
  float anchor_theta[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES] = { { 0.f } };
  float delta_L[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES] = { { 0.f } };
  float chroma_scale[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES];
  float delta_theta[DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS][DT_IOP_COLOREQUAL_HUE_SAMPLES] = { { 0.f } };

  for(int ring = 0; ring < DT_IOP_COLOREQUAL_LOCAL_FIELD_RINGS; ring++)
    for(int hue_sample = 0; hue_sample < DT_IOP_COLOREQUAL_HUE_SAMPLES; hue_sample++)
      chroma_scale[ring][hue_sample] = 1.f;

  /**
   * Sample every ring on 64 evenly-spaced hue positions. Those samples are the
   * sparse control surface the user actually edits: one in-gamut source ring
   * and one deformed destination ring, both expressed in the current work RGB.
   */
  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
  {
    const dt_iop_colorequal_ring_t current_ring = (dt_iop_colorequal_ring_t)ring;
    const float brightness = dt_colorrings_ring_brightness((dt_colorrings_ring_t)current_ring);
    const float reference_saturation = d->reference_saturation[ring];
    dt_aligned_pixel_t neutral_rgb = { 0.f };
    dt_colorrings_brightness_to_axis_rgb(brightness, white, lut_profile, neutral_rgb);

    for(int hue_sample = 0; hue_sample < DT_IOP_COLOREQUAL_HUE_SAMPLES; hue_sample++)
    {
      const float x = (float)hue_sample / (float)DT_IOP_COLOREQUAL_HUE_SAMPLES;
      const float hue = dt_colorrings_curve_x_to_hue(x);
      const float hue_shift = _channel_value_from_y(
          DT_IOP_COLOREQUAL_HUE,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_HUE),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_HUE), x));
      const float sat_gain = _channel_value_from_y(
          DT_IOP_COLOREQUAL_SATURATION,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_SATURATION),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_SATURATION), x));
      const float bright_gain = _channel_value_from_y(
          DT_IOP_COLOREQUAL_BRIGHTNESS,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_BRIGHTNESS),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_BRIGHTNESS), x));

      const dt_aligned_pixel_t before_hsb = { hue, reference_saturation, brightness, 0.f };
      const dt_aligned_pixel_t after_hsb
          = { dt_colorrings_wrap_hue_pi(hue + hue_shift), CLAMP(reference_saturation * sat_gain, 0.f, 1.f),
              CLAMP(brightness * bright_gain, 0.f, 1.f), 0.f };

      dt_aligned_pixel_t before_rgb = { 0.f };
      dt_aligned_pixel_t after_rgb = { 0.f };
      dt_colorrings_hsb_to_profile_rgb(before_hsb, white, lut_profile, before_rgb);
      dt_colorrings_hsb_to_profile_rgb(after_hsb, white, lut_profile, after_rgb);
      dt_colorrings_project_to_cube_shell(neutral_rgb, before_rgb);
      dt_colorrings_project_to_cube_shell(neutral_rgb, after_rgb);

      float Lp, rhop, thetap;
      float La, rhoa;
      float unused_theta;
      dt_colorrings_rgb_to_gray_cyl(before_rgb, &Lp, &rhop, &thetap);
      dt_colorrings_rgb_to_gray_cyl(after_rgb, &La, &rhoa, &unused_theta);

      /**
       * The user edits hue and saturation in dt UCS HSB, so the local field
       * must preserve that semantic separation in work RGB too:
       * - hue rotations are explicit angular edits and must not contract chroma
       *   just because target hues sit at a different radius in the RGB cube,
       * - saturation edits are the only thing allowed to scale chroma,
       * - brightness edits are carried by the achromatic-axis shift measured on
       *   the projected work-RGB anchors.
       *
       * When saturation expands beyond the cube shell, clamp the requested
       * scale to the actually projected anchor so the sparse ring samples stay
       * inside gamut.
       */
      const float requested_scale = sat_gain;
      const float projected_scale = (rhop > 1e-6f) ? (rhoa / rhop) : 1.f;
      const float effective_scale
          = (requested_scale <= 1.f) ? requested_scale : fminf(requested_scale, projected_scale);

      anchor_L[ring][hue_sample] = Lp;
      anchor_rho[ring][hue_sample] = rhop;
      anchor_theta[ring][hue_sample] = thetap;
      delta_L[ring][hue_sample] = La - Lp;
      chroma_scale[ring][hue_sample] = effective_scale;
      delta_theta[ring][hue_sample] = dt_colorrings_wrap_pi(hue_shift);
    }
  }

  // Fill the achromatic locus after the hue rings
  for(int sample = 0; sample < DT_IOP_COLOREQUAL_HUE_SAMPLES; sample++)
  {
    const float value = (float)sample / (float)(DT_IOP_COLOREQUAL_HUE_SAMPLES - 1);
    anchor_L[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = value * 1.7320508075688772f;
    anchor_rho[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = 0.f;
    anchor_theta[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = 0.f;
    delta_L[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = 0.f;
    chroma_scale[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = 1.f;
    delta_theta[DT_IOP_COLOREQUAL_NUM_RINGS][sample] = 0.f;
  }

  /**
   * The local field is evaluated in LUT code coordinates, so the smoothing
   * sigmas belong to that geometry too:
   * - sigma_L is exposed as a percentage of the normalized achromatic-axis
   *   length because that is easier to reason about in the GUI,
   * - sigma_rho is the radial support around the neutral axis,
   * - sigma_theta controls how many neighbouring hue anchors contribute to a
   *   query point on the ring,
   * - neutral_protection defines how wide the fade-to-zero region is around
   *   the achromatic axis before color shifts reach full strength.
   */
  const float sigma_L = fmaxf(p->sigma_L * 0.01f, 1e-6f);
  const float sigma_rho = fmaxf(p->sigma_rho, 1e-6f);
  const float sigma_theta = fmaxf(p->sigma_theta, 1e-6f);
  const float neutral_protection = fmaxf(p->neutral_protection, 0.f);
  dt_colorrings_fill_lut_local_field(d->clut, DT_IOP_COLOREQUAL_CLUT_LEVEL, anchor_L, anchor_rho, anchor_theta,
                                     delta_L, chroma_scale, delta_theta, 1.f / sigma_L, 1.f / sigma_rho,
                                     1.f / sigma_theta, neutral_protection * sigma_rho);

  d->clut_level = DT_IOP_COLOREQUAL_CLUT_LEVEL;

  if(log_perf)
    dt_print(DT_DEBUG_PERF, "[colorequal] build_clut level=%u total=%.3fms\n", d->clut_level,
             1000.0 * (dt_get_wtime() - start));
}

static size_t _build_viewer_control_nodes(const dt_iop_colorequal_params_t *p,
                                          const dt_iop_order_iccprofile_info_t *lut_profile,
                                          dt_lut_viewer_control_node_t *control_nodes)
{
  if(!p || !lut_profile || !control_nodes) return 0;

  const float white = dt_colorrings_graph_white();
  float reference_saturation[DT_IOP_COLOREQUAL_NUM_RINGS] = { 0.f };
  size_t count = 0;

  dt_colorrings_compute_reference_saturations(white, reference_saturation);

  /**
   * The LUT viewer should display the same sparse control surface the local
   * field actually interpolates. Rebuild the ring samples here from the GUI
   * curves so the control-node overlay matches the authored RGB anchors
   * exactly, not just the user-visible spline control points.
   */
  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
  {
    const dt_iop_colorequal_ring_t current_ring = (dt_iop_colorequal_ring_t)ring;
    const float brightness = dt_colorrings_ring_brightness((dt_colorrings_ring_t)current_ring);
    const float saturation = reference_saturation[ring];
    dt_aligned_pixel_t neutral_rgb = { 0.f };

    dt_colorrings_brightness_to_axis_rgb(brightness, white, lut_profile, neutral_rgb);

    for(int hue_sample = 0; hue_sample < DT_IOP_COLOREQUAL_HUE_SAMPLES; hue_sample++)
    {
      const float x = (float)hue_sample / (float)DT_IOP_COLOREQUAL_HUE_SAMPLES;
      const float hue = dt_colorrings_curve_x_to_hue(x);
      const float hue_shift = _channel_value_from_y(
          DT_IOP_COLOREQUAL_HUE,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_HUE),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_HUE), x));
      const float sat_gain = _channel_value_from_y(
          DT_IOP_COLOREQUAL_SATURATION,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_SATURATION),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_SATURATION), x));
      const float bright_gain = _channel_value_from_y(
          DT_IOP_COLOREQUAL_BRIGHTNESS,
          dt_colorrings_curve_periodic_sample(
              (const dt_colorrings_node_t *)_curve_nodes_const(p, current_ring, DT_IOP_COLOREQUAL_BRIGHTNESS),
              _curve_nodes_count_const(p, current_ring, DT_IOP_COLOREQUAL_BRIGHTNESS), x));
      const dt_aligned_pixel_t before_hsb = { hue, saturation, brightness, 0.f };
      const dt_aligned_pixel_t after_hsb
          = { dt_colorrings_wrap_hue_pi(hue + hue_shift), CLAMP(saturation * sat_gain, 0.f, 1.f),
              CLAMP(brightness * bright_gain, 0.f, 1.f), 0.f };
      dt_aligned_pixel_t before_rgb = { 0.f };
      dt_aligned_pixel_t after_rgb = { 0.f };

      dt_colorrings_hsb_to_profile_rgb(before_hsb, white, lut_profile, before_rgb);
      dt_colorrings_hsb_to_profile_rgb(after_hsb, white, lut_profile, after_rgb);
      dt_colorrings_project_to_cube_shell(neutral_rgb, before_rgb);
      dt_colorrings_project_to_cube_shell(neutral_rgb, after_rgb);

      for(int c = 0; c < 3; c++)
      {
        control_nodes[count].input_rgb[c] = before_rgb[c];
        control_nodes[count].output_rgb[c] = after_rgb[c];
      }
      count++;
    }
  }

  return count;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorequal_params_t *p = (const dt_iop_colorequal_params_t *)p1;
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  const dt_iop_order_iccprofile_info_t *lut_profile
      = self->dev ? dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL)
                  : NULL;
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);

  if(!work_profile || !lut_profile || !gd)
  {
    d->clut = NULL;
    d->clut_level = 0;
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
  memcpy(d->reference_saturation, gd->cache.reference_saturation, sizeof(d->reference_saturation));
  dt_pthread_rwlock_unlock(&gd->lock);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_data_t *d = dt_calloc_align(sizeof(dt_iop_colorequal_data_t));
  piece->data = d;
  piece->data_size = sizeof(dt_iop_colorequal_data_t);
  d->clut = NULL;
  d->clut_level = 0;
  d->white_level = 1.f;
  d->lut_profile = NULL;
  memset(d->reference_saturation, 0, sizeof(d->reference_saturation));
  d->work_profile = NULL;
  d->interpolation = DT_LUT3D_INTERP_TETRAHEDRAL;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  d->clut = NULL;
  dt_free_align(piece->data);
  piece->data = NULL;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  const dt_iop_colorequal_data_t *d = (const dt_iop_colorequal_data_t *)piece->data;
  const int width = piece->roi_in.width;
  const int height = piece->roi_in.height;
  const int devid = pipe->devid;
  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  const float white_level = fmaxf(d->white_level, 1e-6f);
  const float normalize = 1.f / white_level;
  const float denormalize = white_level;
  const float black = 0.f;
  cl_mem clut_cl = NULL;
  cl_int err = CL_SUCCESS;
  const int kernel = (d->interpolation == DT_LUT3D_INTERP_TRILINEAR) ? gd->kernel_lut3d_trilinear
                     : (d->interpolation == DT_LUT3D_INTERP_PYRAMID) ? gd->kernel_lut3d_pyramid
                     : gd->kernel_lut3d_tetrahedral;

  if(!gd || !d->clut || d->clut_level == 0 || !d->lut_profile || !d->work_profile) return FALSE;

  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 4, sizeof(float), &black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 5, sizeof(float), &normalize);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_exposure, sizes);
  if(err != CL_SUCCESS) goto cleanup;

  if(!dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_out, dev_out, width, height, d->work_profile,
                                                 d->lut_profile, "colorequal work to HLG Rec2020"))
  {
    err = CL_INVALID_OPERATION;
    goto cleanup;
  }

  dt_pthread_rwlock_rdlock(&gd->lock);
  clut_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * d->clut_level * d->clut_level * d->clut_level,
                                                   gd->cache.clut);
  dt_pthread_rwlock_unlock(&gd->lock);
  if(clut_cl == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &clut_cl);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &d->clut_level);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto cleanup;

  if(!dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_out, dev_out, width, height, d->lut_profile,
                                                 d->work_profile, "colorequal HLG Rec2020 to work"))
  {
    err = CL_INVALID_OPERATION;
    goto cleanup;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 0, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 4, sizeof(float), &black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_exposure, 5, sizeof(float), &denormalize);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_exposure, sizes);

cleanup:
  dt_opencl_release_mem_object(clut_cl);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_colorequal] couldn't enqueue kernel! %d\n", err);
  return err == CL_SUCCESS;
}
#endif

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ibuf, void *const obuf)
{
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  const dt_iop_colorequal_data_t *d = (const dt_iop_colorequal_data_t *)piece->data;
  const int width = piece->roi_in.width;
  const int height = piece->roi_in.height;
  const int ch = piece->dsc_in.channels;

  if(!gd || !d->clut || d->clut_level == 0)
  {
    dt_iop_image_copy_by_size(obuf, ibuf, width, height, ch);
    return 0;
  }

  if(!d->lut_profile || !d->work_profile)
  {
    dt_iop_image_copy_by_size(obuf, ibuf, width, height, ch);
    return 0;
  }

  const float white_level = fmaxf(d->white_level, 1e-6f);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(ibuf, obuf, width, height, ch, white_level)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    const float *const in = (const float *)ibuf + k * ch;
    float *const out = (float *)obuf + k * ch;
    out[0] = in[0] / white_level;
    out[1] = in[1] / white_level;
    out[2] = in[2] / white_level;
    if(ch > 3) out[3] = in[3];
  }

  dt_ioppr_transform_image_colorspace_rgb((float *)obuf, (float *)obuf, width, height, d->work_profile,
                                          d->lut_profile, "colorequal work to HLG Rec2020");

  dt_pthread_rwlock_rdlock(&gd->lock);
  dt_lut3d_apply((float *)obuf, (float *)obuf, (size_t)width * height, d->clut, d->clut_level, 1.f,
                 d->interpolation);
  dt_pthread_rwlock_unlock(&gd->lock);

  dt_ioppr_transform_image_colorspace_rgb((float *)obuf, (float *)obuf, width, height, d->lut_profile, d->work_profile,
                                          "colorequal HLG Rec2020 to work");

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(obuf, width, height, ch, white_level)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    float *const out = (float *)obuf + k * ch;
    out[0] *= white_level;
    out[1] *= white_level;
    out[2] *= white_level;
  }
  return 0;
}

/**
 * The authoritative CLUT lives in module-global data so pixelpipes and the GUI
 * can reuse the same build. The viewer only receives that shared pointer plus
 * the global read/write lock, which lets it read safely while the cache is
 * rebuilt on parameter changes.
 */
static void _update_gui_lut_cache(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  const dt_iop_colorequal_params_t *p = g ? &g->gui_params : (const dt_iop_colorequal_params_t *)self->params;
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double start = log_perf ? dt_get_wtime() : 0.0;
  uint64_t cache_generation = 0;

  if(!g || !g->viewer || !gd) return;

  dt_pthread_rwlock_rdlock(&gd->lock);
  cache_generation = gd->cache_generation;
  dt_pthread_rwlock_unlock(&gd->lock);

  if(!g->viewer_lut_dirty && g->viewer_lut_valid && g->viewer_lut_generation == cache_generation) return;

  const dt_iop_order_iccprofile_info_t *lut_profile
      = self->dev ? dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL)
                  : NULL;
  const dt_iop_order_iccprofile_info_t *display_profile
      = (self->dev && self->dev->preview_pipe) ? dt_ioppr_get_pipe_output_profile_info(self->dev->preview_pipe)
                                               : NULL;

  if(!lut_profile)
  {
    dt_lut_viewer_set_lut(g->viewer, NULL, 0, NULL, NULL, NULL);
    dt_lut_viewer_set_control_nodes(g->viewer, NULL, 0);
    g->viewer_lut_dirty = FALSE;
    g->viewer_lut_valid = FALSE;
    g->viewer_lut_generation = 0;
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
  memcpy(g->viewer_lut.reference_saturation, gd->cache.reference_saturation, sizeof(g->viewer_lut.reference_saturation));
  g->viewer_control_node_count = _build_viewer_control_nodes(p, lut_profile, g->viewer_control_nodes);
  dt_lut_viewer_set_lut(g->viewer, g->viewer_lut.clut, g->viewer_lut.clut_level, &gd->lock,
                        g->viewer_lut.lut_profile, display_profile);
  dt_lut_viewer_set_control_nodes(g->viewer, g->viewer_control_nodes, g->viewer_control_node_count);
  dt_pthread_rwlock_unlock(&gd->lock);
  g->viewer_lut_dirty = FALSE;
  g->viewer_lut_valid = TRUE;
  g->viewer_lut_generation = cache_generation;

  if(log_perf)
    dt_print(DT_DEBUG_PERF, "[colorequal] gui LUT cache sync level=%u total=%.3fms\n", g->viewer_lut.clut_level,
             1000.0 * (dt_get_wtime() - start));
}

static void _update_curve_cache(dt_iop_colorequal_gui_data_t *g, const dt_iop_colorequal_params_t *p)
{
  const gboolean log_perf = (darktable.unmuted & DT_DEBUG_PERF) != 0;
  const double start = log_perf ? dt_get_wtime() : 0.0;
  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
    for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
    {
      const dt_iop_colorequal_ring_t current_ring = (dt_iop_colorequal_ring_t)ring;
      const dt_iop_colorequal_channel_t channel = (dt_iop_colorequal_channel_t)ch;
      const int nodes = _curve_nodes_count_const(p, current_ring, channel);
      const dt_iop_colorequal_node_t *curve = _curve_nodes_const(p, current_ring, channel);

      if(!g->curve[ring][ch] || g->curve_nodes[ring][ch] != nodes || g->curve[ring][ch]->c.m_numAnchors != nodes)
      {
        if(g->curve[ring][ch]) dt_draw_curve_destroy(g->curve[ring][ch]);
        g->curve[ring][ch] = dt_draw_curve_new(0.f, 1.f, MONOTONE_HERMITE);
        g->curve_nodes[ring][ch] = nodes;

        for(int k = 0; k < nodes; k++) dt_draw_curve_add_point(g->curve[ring][ch], curve[k].x, curve[k].y);
      }
      else
      {
        for(int k = 0; k < nodes; k++) dt_draw_curve_set_point(g->curve[ring][ch], k, curve[k].x, curve[k].y);
      }

      dt_draw_curve_calc_values_V2(g->curve[ring][ch], 0.f, 1.f, DT_IOP_COLOREQUAL_GRAPH_RES, NULL,
                                   g->draw_ys[ring][ch], TRUE);
    }

  memcpy(&g->cached_curve_params, p, sizeof(*p));
  g->curve_cache_valid = TRUE;

  if(log_perf)
    dt_print(DT_DEBUG_PERF, "[colorequal] gui curve cache rebuild total=%.3fms\n",
             1000.0 * (dt_get_wtime() - start));
}

static inline void _graph_background_hsb(const dt_iop_colorequal_channel_t channel, const float x, const float y,
                                         const float ring_brightness, const float reference_saturation,
                                         dt_aligned_pixel_t HSB)
{
  const float hue = dt_colorrings_curve_x_to_hue(x);

  switch(channel)
  {
    case DT_IOP_COLOREQUAL_HUE:
      HSB[0] = dt_colorrings_wrap_hue_pi(hue + _channel_value_from_y(channel, y));
      HSB[1] = reference_saturation;
      HSB[2] = ring_brightness;
      break;
    case DT_IOP_COLOREQUAL_BRIGHTNESS:
      HSB[0] = hue;
      HSB[1] = reference_saturation;
      HSB[2] = CLAMP(ring_brightness * _channel_value_from_y(channel, y), 0.f, 1.f);
      break;
    case DT_IOP_COLOREQUAL_SATURATION:
    default:
      HSB[0] = hue;
      HSB[1] = CLAMP(reference_saturation * _channel_value_from_y(channel, y), 0.f, 1.f);
      HSB[2] = ring_brightness;
      break;
  }
}

static void _draw_graph_background(cairo_t *cr, const dt_iop_colorequal_channel_t channel,
                                   const dt_iop_colorequal_ring_t ring, const float graph_width,
                                   const float graph_height, const float white, const float reference_saturation,
                                   const dt_iop_order_iccprofile_info_t *display_profile)
{
  const float ring_brightness = dt_colorrings_ring_brightness((dt_colorrings_ring_t)ring);

  for(int slice = 0; slice < DT_IOP_COLOREQUAL_GRAPH_GRADIENTS; slice++)
  {
    const float y = (float)(DT_IOP_COLOREQUAL_GRAPH_GRADIENTS - slice) / (float)DT_IOP_COLOREQUAL_GRAPH_GRADIENTS;
    dt_aligned_pixel_t colors[DT_IOP_COLOREQUAL_GRAPH_RES];
    cairo_pattern_t *gradient = cairo_pattern_create_linear(0.0, 0.0, graph_width, 0.0);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(channel, ring_brightness,          \
                                                                            reference_saturation, white, y,     \
                                                                            display_profile)                    \
  dt_omp_firstprivate(colors)
#endif
    for(int k = 0; k < DT_IOP_COLOREQUAL_GRAPH_RES; k++)
    {
      const float x = (float)k / (float)(DT_IOP_COLOREQUAL_GRAPH_RES - 1);
      dt_aligned_pixel_t HSB = { 0.f };
      _graph_background_hsb(channel, x, y, ring_brightness, reference_saturation, HSB);
      dt_colorrings_hsb_to_display_rgb(HSB, white, display_profile, colors[k]);
    }

    /**
     * Cairo pattern construction is not thread-safe, so the expensive color
     * conversions are prepared in parallel above and the color stops are
     * submitted here in one visible serial pass.
     */
    for(int k = 0; k < DT_IOP_COLOREQUAL_GRAPH_RES; k++)
    {
      const float x = (float)k / (float)(DT_IOP_COLOREQUAL_GRAPH_RES - 1);
      cairo_pattern_add_color_stop_rgba(gradient, x, colors[k][0], colors[k][1], colors[k][2], 1.0);
    }

    cairo_rectangle(cr, 0.f, graph_height / (float)DT_IOP_COLOREQUAL_GRAPH_GRADIENTS * (float)slice, graph_width,
                    graph_height / (float)DT_IOP_COLOREQUAL_GRAPH_GRADIENTS);
    cairo_set_source(cr, gradient);
    cairo_fill(cr);
    cairo_pattern_destroy(gradient);
  }
}

static gboolean _draw_curve(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const dt_iop_colorequal_params_t *p = &g->gui_params;
  const dt_iop_colorequal_ring_t ring
      = (dt_iop_colorequal_ring_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-ring"));
  const dt_iop_colorequal_channel_t channel
      = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-channel"));

  if(!g->curve_cache_valid || !_curve_fields_equal(&g->cached_curve_params, p)) _update_curve_cache(g, p);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  const float inset = DT_IOP_COLOREQUAL_GRAPH_INSET;
  const float graph_width = allocation.width - 2.f * inset;
  const float graph_height = allocation.height - DT_IOP_COLOREQUAL_AXIS_HEIGHT - 2.f * inset;
  const float white = dt_colorrings_graph_white();
  const dt_iop_order_iccprofile_info_t *const display_profile
      = (self->dev && self->dev->preview_pipe) ? dt_ioppr_get_pipe_output_profile_info(self->dev->preview_pipe)
                                               : NULL;
  dt_colorrings_compute_reference_saturations(white, g->reference_saturation);
  /**
   * Hue backgrounds need to keep the ring lightness cue readable across the
   * three tabs. Pushing their saturation beyond the in-gamut reference sends
   * more hues into the display normalization path, which flattens the intended
   * 15/45/75% brightness separation. Keep the hue tab on the reference ring
   * and only boost the other channels for contrast.
   */
  const float background_saturation
      = (channel == DT_IOP_COLOREQUAL_HUE) ? g->reference_saturation[ring]
                                           : CLAMP(g->reference_saturation[ring] * 1.35f, 0.f, 1.f);

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  if(!g->background_surface[ring][channel] || g->cached_width[ring][channel] != allocation.width
     || g->cached_height[ring][channel] != allocation.height
     || fabsf(g->cached_white[ring][channel] - white) > 1e-6f
     || fabsf(g->cached_reference_saturation[ring][channel] - background_saturation) > 1e-6f
     || g->cached_display_profile[ring][channel] != display_profile)
  {
    if(g->background_surface[ring][channel]) cairo_surface_destroy(g->background_surface[ring][channel]);
    g->background_surface[ring][channel]
        = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *background_cr = cairo_create(g->background_surface[ring][channel]);

    gtk_render_background(context, background_cr, 0, 0, allocation.width, allocation.height);
    cairo_translate(background_cr, inset, inset);
    _draw_graph_background(background_cr, channel, ring, graph_width, graph_height, white, background_saturation,
                           display_profile);

    cairo_rectangle(background_cr, 0.f, 0.f, graph_width, graph_height);
    cairo_clip(background_cr);

    cairo_set_line_width(background_cr, DT_PIXEL_APPLY_DPI(0.5f));
    set_color(background_cr, darktable.bauhaus->graph_border);
    dt_draw_grid(background_cr, 8, 0, 0, graph_width, graph_height);

    set_color(background_cr, darktable.bauhaus->graph_fg);
    cairo_set_line_width(background_cr, DT_PIXEL_APPLY_DPI(1.f));
    cairo_move_to(background_cr, 0.f, 0.5f * graph_height);
    cairo_line_to(background_cr, graph_width, 0.5f * graph_height);
    cairo_stroke(background_cr);

    cairo_reset_clip(background_cr);
    cairo_translate(background_cr, 0.f, graph_height);
    cairo_pattern_t *axis = cairo_pattern_create_linear(0.0, 0.0, graph_width, 0.0);

    for(int k = 0; k < DT_IOP_COLOREQUAL_GRAPH_RES; k++)
    {
      const float x = (float)k / (float)(DT_IOP_COLOREQUAL_GRAPH_RES - 1);
      dt_aligned_pixel_t RGB = { 0.f };
      dt_colorrings_hsb_to_display_rgb(
          (dt_aligned_pixel_t){ dt_colorrings_curve_x_to_hue(x), background_saturation,
                                dt_colorrings_ring_brightness((dt_colorrings_ring_t)ring), 0.f },
          white, display_profile, RGB);
      cairo_pattern_add_color_stop_rgba(axis, x, RGB[0], RGB[1], RGB[2], 1.0);
    }

    cairo_rectangle(background_cr, 0.f, 0.f, graph_width, DT_IOP_COLOREQUAL_AXIS_HEIGHT);
    cairo_set_source(background_cr, axis);
    cairo_fill(background_cr);
    cairo_pattern_destroy(axis);
    cairo_destroy(background_cr);

    g->cached_width[ring][channel] = allocation.width;
    g->cached_height[ring][channel] = allocation.height;
    g->cached_white[ring][channel] = white;
    g->cached_reference_saturation[ring][channel] = background_saturation;
    g->cached_display_profile[ring][channel] = display_profile;
  }

  cairo_set_source_surface(cr, g->background_surface[ring][channel], 0, 0);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);

  if(g->picker_valid)
  {
    const float picker_x = dt_colorrings_hue_to_curve_x(g->picker_hue) * graph_width;
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5f));
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_move_to(cr, picker_x, 0.f);
    cairo_line_to(cr, picker_x, graph_height + DT_IOP_COLOREQUAL_AXIS_HEIGHT);
    cairo_stroke(cr);
  }

  const dt_iop_colorequal_ring_t active_ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t active_channel = _active_channel_from_gui(g, active_ring);
  if(g->cursor_sample_valid && ring == active_ring && channel == active_channel)
  {
    const float cursor_x = dt_colorrings_hue_to_curve_x(g->cursor_hue) * graph_width;
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.5f));
    cairo_set_source_rgba(cr, 0.f, 0.f, 0.f, 0.75f);
    cairo_move_to(cr, cursor_x, 0.f);
    cairo_line_to(cr, cursor_x, graph_height + DT_IOP_COLOREQUAL_AXIS_HEIGHT);
    cairo_stroke(cr);

    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.25f));
    cairo_set_source_rgba(cr, g->cursor_output_display[0], g->cursor_output_display[1], g->cursor_output_display[2],
                          0.95);
    cairo_move_to(cr, cursor_x, 0.f);
    cairo_line_to(cr, cursor_x, graph_height + DT_IOP_COLOREQUAL_AXIS_HEIGHT);
    cairo_stroke(cr);
  }

  cairo_rectangle(cr, 0.f, 0.f, graph_width, graph_height);
  cairo_clip(cr);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.f));
  set_color(cr, darktable.bauhaus->graph_fg);

  for(int k = 0; k < DT_IOP_COLOREQUAL_GRAPH_RES; k++)
  {
    const float x = (float)k / (float)(DT_IOP_COLOREQUAL_GRAPH_RES - 1) * graph_width;
    const float y = (1.f - g->draw_ys[ring][channel][k]) * graph_height;

    if(k == 0)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }

  cairo_stroke(cr);

  const dt_iop_colorequal_node_t *curve = _curve_nodes_const(p, ring, channel);
  const int nodes = _curve_nodes_count_const(p, ring, channel);

  for(int k = 0; k < nodes; k++)
  {
    const float x = curve[k].x * graph_width;
    const float y = (1.f - curve[k].y) * graph_height;
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.f));
    cairo_arc(cr, x, y, DT_PIXEL_APPLY_DPI(k == g->selected[ring][channel] ? 5.f : 4.f), 0.f, 2.f * M_PI_F);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);
  }

  if(g->cursor_sample_valid && ring == active_ring && channel == active_channel)
  {
    float curve_x = 0.f;
    float curve_y = 0.f;
    float offset_normalized = 0.f;
    if(_cursor_curve_state(p, ring, channel, g->cursor_hue, &curve_x, &curve_y, &offset_normalized))
    {
      const float marker_x = curve_x * graph_width;
      const float marker_y = (1.f - curve_y) * graph_height;
      const float outer_radius = DT_PIXEL_APPLY_DPI(7.f);
      const float inner_radius = DT_PIXEL_APPLY_DPI(4.f);
      const float intensity_radius = DT_PIXEL_APPLY_DPI(11.f);

      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.25f));
      cairo_set_source_rgba(cr, 0.f, 0.f, 0.f, 0.4f);
      cairo_arc(cr, marker_x, marker_y, intensity_radius, 0.f, 2.f * M_PI_F);
      cairo_stroke(cr);

      if(fabsf(offset_normalized) > 1e-4f)
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.25f));
        cairo_set_source_rgba(cr, g->cursor_output_display[0], g->cursor_output_display[1],
                              g->cursor_output_display[2], 0.95);
        if(offset_normalized > 0.f)
          cairo_arc(cr, marker_x, marker_y, intensity_radius, -M_PI_F / 2.f,
                    -M_PI_F / 2.f + 2.f * M_PI_F * fabsf(offset_normalized));
        else
          cairo_arc_negative(cr, marker_x, marker_y, intensity_radius, -M_PI_F / 2.f,
                             -M_PI_F / 2.f - 2.f * M_PI_F * fabsf(offset_normalized));
        cairo_stroke(cr);
      }

      cairo_arc(cr, marker_x, marker_y, outer_radius, 0.f, 2.f * M_PI_F);
      cairo_set_source_rgba(cr, g->cursor_output_display[0], g->cursor_output_display[1],
                            g->cursor_output_display[2], 0.95);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, 0.f, 0.f, 0.f, 0.9f);
      cairo_stroke(cr);

      cairo_arc(cr, marker_x, marker_y, inner_radius, 0.f, 2.f * M_PI_F);
      cairo_set_source_rgba(cr, g->cursor_input_display[0], g->cursor_input_display[1], g->cursor_input_display[2],
                            0.95);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, 1.f, 1.f, 1.f, 0.8f);
      cairo_stroke(cr);
    }
  }

  cairo_reset_clip(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void _history_resync_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  uint64_t cache_generation = 0;
  if(!g || !gd) return;

  if(g->has_focus && g->cursor_valid && self->enabled)
  {
    _refresh_preview_cursor_sample(self);
    const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
    const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
    gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
    dt_control_queue_redraw_center();
  }

  _switch_preview_cursor(self);
  if(!g->viewer) return;

  dt_pthread_rwlock_rdlock(&gd->lock);
  cache_generation = gd->cache_generation;
  dt_pthread_rwlock_unlock(&gd->lock);

  if(!g->viewer_lut_dirty && g->viewer_lut_valid && g->viewer_lut_generation == cache_generation) return;

  _update_gui_lut_cache(self);
  dt_lut_viewer_queue_draw(g->viewer);
}

static void _cacheline_ready_callback(gpointer instance, const guint64 hash, gpointer user_data)
{
  (void)instance;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!g || g->pending_preview_hash != hash || !_refresh_preview_cursor_sample(self)) return;

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
  dt_control_queue_redraw_center();
}

static int _find_selected_node(const dt_iop_module_t *self, const dt_iop_colorequal_ring_t ring,
                               const dt_iop_colorequal_channel_t channel, const float mouse_x, const float mouse_y,
                               const float graph_width, const float graph_height)
{
  const dt_iop_colorequal_gui_data_t *g = (const dt_iop_colorequal_gui_data_t *)self->gui_data;
  const dt_iop_colorequal_params_t *p = &g->gui_params;
  int selected = -1;
  float best = DT_PIXEL_APPLY_DPI(10.f) * DT_PIXEL_APPLY_DPI(10.f);
  const dt_iop_colorequal_node_t *curve = _curve_nodes_const(p, ring, channel);
  const int nodes = _curve_nodes_count_const(p, ring, channel);

  for(int k = 0; k < nodes; k++)
  {
    const float node_x = curve[k].x * graph_width;
    const float node_y = (1.f - curve[k].y) * graph_height;
    const float dx = mouse_x - node_x;
    const float dy = mouse_y - node_y;
    const float distance = dx * dx + dy * dy;

    if(distance < best)
    {
      best = distance;
      selected = k;
    }
  }

  return selected;
}

static int _add_node(dt_iop_colorequal_node_t *curve, int *nodes, const float x, const float y)
{
  int selected = -1;

  if(curve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
      if(curve[k].x > x)
      {
        selected = k;
        break;
      }
  }

  if(selected == -1) selected = *nodes;

  if((selected > 0 && x - curve[selected - 1].x <= DT_IOP_COLOREQUAL_MIN_X_DISTANCE)
     || (selected < *nodes && curve[selected].x - x <= DT_IOP_COLOREQUAL_MIN_X_DISTANCE))
    return -1;

  for(int k = *nodes; k > selected; k--)
  {
    curve[k].x = curve[k - 1].x;
    curve[k].y = curve[k - 1].y;
  }

  curve[selected].x = x;
  curve[selected].y = y;
  (*nodes)++;
  return selected;
}

static gboolean _move_selected_node(dt_iop_module_t *self, const dt_iop_colorequal_ring_t ring,
                                    const dt_iop_colorequal_channel_t channel, const int node, const float x,
                                    const float y)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = &g->gui_params;
  dt_iop_colorequal_node_t *curve = _curve_nodes(p, ring, channel);
  const int nodes = _curve_nodes_count_const(p, ring, channel);

  float new_x = CLAMP(x, 0.f, 0.999f);
  const float new_y = CLAMP(y, 0.f, 1.f);

  if(node == 0)
  {
    if(new_x + 1.f - curve[nodes - 1].x < DT_IOP_COLOREQUAL_MIN_X_DISTANCE)
      new_x = curve[nodes - 1].x + DT_IOP_COLOREQUAL_MIN_X_DISTANCE - 1.f;
  }
  else if(node == nodes - 1)
  {
    if(curve[0].x + 1.f - new_x < DT_IOP_COLOREQUAL_MIN_X_DISTANCE)
      new_x = curve[0].x + 1.f - DT_IOP_COLOREQUAL_MIN_X_DISTANCE;
  }
  else if((new_x - curve[node - 1].x) < DT_IOP_COLOREQUAL_MIN_X_DISTANCE
          || (curve[node + 1].x - new_x) < DT_IOP_COLOREQUAL_MIN_X_DISTANCE)
  {
    return FALSE;
  }

  curve[node].x = new_x;
  curve[node].y = new_y;
  return TRUE;
}

static gboolean _area_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const dt_iop_colorequal_ring_t ring
      = (dt_iop_colorequal_ring_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-ring"));
  const dt_iop_colorequal_channel_t channel
      = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-channel"));
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  const float graph_width = allocation.width - 2.f * DT_IOP_COLOREQUAL_GRAPH_INSET;
  const float graph_height
      = allocation.height - DT_IOP_COLOREQUAL_AXIS_HEIGHT - 2.f * DT_IOP_COLOREQUAL_GRAPH_INSET;
  const float mouse_x = CLAMP(event->x - DT_IOP_COLOREQUAL_GRAPH_INSET, 0.f, graph_width);
  const float mouse_y = CLAMP(event->y - DT_IOP_COLOREQUAL_GRAPH_INSET, 0.f, graph_height);

  if(g->dragging[ring][channel] && g->selected[ring][channel] >= 0)
  {
    if(_move_selected_node(self, ring, channel, g->selected[ring][channel], mouse_x / graph_width,
                           1.f - mouse_y / graph_height))
    {
      memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
      g->curve_cache_valid = FALSE;
      g->viewer_lut_dirty = TRUE;
      if(g->cursor_valid && g->has_focus)
      {
        _update_gui_lut_cache(self);
        _refresh_preview_cursor_sample(self);
        dt_control_queue_redraw_center();
      }
      dt_gui_throttle_queue(self, dt_iop_throttled_history_update, self);
      gtk_widget_queue_draw(widget);
    }

    return TRUE;
  }

  g->selected[ring][channel]
      = _find_selected_node(self, ring, channel, mouse_x, mouse_y, graph_width, graph_height);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _area_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const dt_iop_colorequal_ring_t ring
      = (dt_iop_colorequal_ring_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-ring"));
  const dt_iop_colorequal_channel_t channel
      = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-channel"));
  dt_iop_colorequal_params_t *p = &g->gui_params;
  dt_iop_colorequal_params_t *d = (dt_iop_colorequal_params_t *)self->default_params;
  dt_iop_colorequal_node_t *curve = _curve_nodes(p, ring, channel);
  dt_iop_colorequal_node_t *default_curve = _curve_nodes(d, ring, channel);
  int *nodes = _curve_nodes_count(p, ring, channel);
  const int default_nodes = _curve_nodes_count_const(d, ring, channel);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float graph_width = allocation.width - 2.f * DT_IOP_COLOREQUAL_GRAPH_INSET;
  const float graph_height
      = allocation.height - DT_IOP_COLOREQUAL_AXIS_HEIGHT - 2.f * DT_IOP_COLOREQUAL_GRAPH_INSET;
  const float mouse_x = CLAMP(event->x - DT_IOP_COLOREQUAL_GRAPH_INSET, 0.f, graph_width) / graph_width;
  const float mouse_y = 1.f - CLAMP(event->y - DT_IOP_COLOREQUAL_GRAPH_INSET, 0.f, graph_height) / graph_height;

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    *nodes = default_nodes;
    for(int k = 0; k < default_nodes; k++) curve[k] = default_curve[k];

    g->selected[ring][channel] = -1;
    memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
    g->curve_cache_valid = FALSE;
    g->viewer_lut_dirty = TRUE;
    if(g->cursor_valid && g->has_focus)
    {
      _update_gui_lut_cache(self);
      _refresh_preview_cursor_sample(self);
      dt_control_queue_redraw_center();
    }
    dt_gui_throttle_queue(self, dt_iop_throttled_history_update, self);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  if(event->button == 1 && dt_modifier_is(event->state, GDK_CONTROL_MASK) && *nodes < DT_IOP_COLOREQUAL_MAXNODES)
  {
    const float y
        = dt_colorrings_curve_periodic_sample((const dt_colorrings_node_t *)curve, *nodes, mouse_x);
    const int selected = _add_node(curve, nodes, mouse_x, y);

    if(selected >= 0)
    {
      g->selected[ring][channel] = selected;
      memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
      g->curve_cache_valid = FALSE;
      g->viewer_lut_dirty = TRUE;
      if(g->cursor_valid && g->has_focus)
      {
        _update_gui_lut_cache(self);
        _refresh_preview_cursor_sample(self);
        dt_control_queue_redraw_center();
      }
      dt_gui_throttle_queue(self, dt_iop_throttled_history_update, self);
      gtk_widget_queue_draw(widget);
    }

    return TRUE;
  }

  if(event->button == 1)
  {
    g->selected[ring][channel] = _find_selected_node(self, ring, channel, mouse_x * graph_width,
                                                     (1.f - mouse_y) * graph_height, graph_width, graph_height);
    g->dragging[ring][channel] = (g->selected[ring][channel] >= 0);
    return TRUE;
  }

  if(event->button == 3 && g->selected[ring][channel] >= 0 && *nodes > 2)
  {
    for(int k = g->selected[ring][channel]; k < *nodes - 1; k++) curve[k] = curve[k + 1];

    (*nodes)--;
    g->selected[ring][channel] = -1;
    memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
    g->curve_cache_valid = FALSE;
    g->viewer_lut_dirty = TRUE;
    if(g->cursor_valid && g->has_focus)
    {
      _update_gui_lut_cache(self);
      _refresh_preview_cursor_sample(self);
      dt_control_queue_redraw_center();
    }
    dt_gui_throttle_queue(self, dt_iop_throttled_history_update, self);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  return FALSE;
}

static gboolean _area_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    const dt_iop_colorequal_ring_t ring
        = (dt_iop_colorequal_ring_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-ring"));
    const dt_iop_colorequal_channel_t channel
        = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "colorequal-channel"));
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
    const gboolean was_dragging = g->dragging[ring][channel];
    g->dragging[ring][channel] = FALSE;

    /**
     * Curve drags are throttled while the pointer moves, but the final pointer
     * release must always commit the last state to history so the pixelpipes
     * recompute even if another GUI refresh happens before the throttle timer
     * expires.
     */
    if(was_dragging)
    {
      dt_gui_throttle_cancel(self);
      dt_iop_throttled_history_update(self);
    }
  }

  return TRUE;
}

static void _channel_tabs_switch_callback(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                          gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const int source_ring = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(notebook), "colorequal-ring"));
  const dt_iop_colorequal_channel_t channel
      = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "colorequal-channel"));
  dt_conf_set_int("plugins/darkroom/colorequal/gui_channel_page", (int)page_num);

  if(channel < DT_IOP_COLOREQUAL_NUM_CHANNELS)
  {
    ++darktable.gui->reset;
    for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
      if(ring != source_ring) gtk_notebook_set_current_page(g->channel_notebook[ring], (int)page_num);
    --darktable.gui->reset;

    for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
      gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));

    if(g->cursor_valid && g->has_focus) dt_control_queue_redraw_center();
  }
}

static void _ring_tabs_switch_callback(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  if(page_num < DT_IOP_COLOREQUAL_NUM_RINGS)
  {
    dt_conf_set_int("plugins/darkroom/colorequal/gui_ring_page", (int)page_num);
    GtkWidget *page_widget = gtk_notebook_get_nth_page(
        g->channel_notebook[page_num], gtk_notebook_get_current_page(g->channel_notebook[page_num]));
    if(page_widget)
    {
      const dt_iop_colorequal_channel_t channel = (dt_iop_colorequal_channel_t)GPOINTER_TO_INT(
          g_object_get_data(G_OBJECT(page_widget), "colorequal-channel"));
      if(channel < DT_IOP_COLOREQUAL_NUM_CHANNELS) gtk_widget_queue_draw(GTK_WIDGET(g->area[page_num][channel]));
    }
  }

  if(g->cursor_valid && g->has_focus)
  {
    const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
    const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
    gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
    dt_control_queue_redraw_center();
  }
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_develop_t *dev = self ? self->dev : NULL;
  if(!self || !g || !dev || !g->has_focus) return 0;

  if(dt_iop_color_picker_is_visible(dev))
  {
    g->cursor_valid = FALSE;
    _invalidate_preview_cursor(g);
    _switch_preview_cursor(self);
    dt_control_queue_redraw_center();
    return 0;
  }

  const int wd = dev->roi.preview_width;
  const int ht = dev->roi.preview_height;
  if(wd < 1 || ht < 1) return 0;

  float point[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(dev, point, 1);
  dt_dev_coordinates_image_norm_to_preview_abs(dev, point, 1);

  const int cursor_x = (int)point[0];
  const int cursor_y = (int)point[1];
  if(cursor_x >= 0 && cursor_x < wd && cursor_y >= 0 && cursor_y < ht)
  {
    g->cursor_valid = TRUE;
    g->cursor_pos_x = cursor_x;
    g->cursor_pos_y = cursor_y;

    if(dev->preview_pipe && !dev->preview_pipe->processing) _refresh_preview_cursor_sample(self);
  }
  else
  {
    g->cursor_valid = FALSE;
    _invalidate_preview_cursor(g);
  }

  _switch_preview_cursor(self);

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
  dt_control_queue_redraw_center();
  return g->cursor_valid ? 1 : 0;
}

int mouse_leave(struct dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!g) return 0;

  g->cursor_valid = FALSE;
  _invalidate_preview_cursor(g);
  _switch_preview_cursor(self);

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
  dt_control_queue_redraw_center();
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!self || !g || !g->has_focus || which != 3 || !g->cursor_valid || !g->cursor_sample_valid
     || dt_iop_color_picker_is_visible(self->dev))
    return 0;

  if(!self->enabled)
  {
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    return 1;
  }

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  dt_iop_colorequal_node_t *curve = _curve_nodes(&g->gui_params, ring, channel);
  int *nodes = _curve_nodes_count(&g->gui_params, ring, channel);
  if(*nodes >= DT_IOP_COLOREQUAL_MAXNODES) return 1;

  const float curve_x = dt_colorrings_hue_to_curve_x(g->cursor_hue);
  const float curve_y
      = dt_colorrings_curve_periodic_sample((const dt_colorrings_node_t *)curve, *nodes, curve_x);
  const int selected = _add_node(curve, nodes, curve_x, curve_y);
  if(selected < 0) return 1;

  g->selected[ring][channel] = selected;
  memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
  g->curve_cache_valid = FALSE;
  g->viewer_lut_dirty = TRUE;
  _update_gui_lut_cache(self);
  _refresh_preview_cursor_sample(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
  dt_control_queue_redraw_center();
  dt_dev_add_history_item(darktable.develop, self, FALSE, TRUE);
  return 1;
}

int scrolled(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!self || !g || !g->has_focus || !g->cursor_valid || !g->cursor_sample_valid || dt_iop_color_picker_is_visible(self->dev))
    return 0;

  if(!self->enabled)
  {
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    return 1;
  }

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  dt_iop_colorequal_node_t *curve = _curve_nodes(&g->gui_params, ring, channel);
  const int nodes = _curve_nodes_count_const(&g->gui_params, ring, channel);
  if(nodes < 1) return 1;

  const float direction = up ? 1.f : -1.f;
  const float curve_x = dt_colorrings_hue_to_curve_x(g->cursor_hue);
  const float sigma = DT_IOP_COLOREQUAL_SCROLL_SIGMA;
  const float sigma2 = 2.f * sigma * sigma;
  const float step = (channel == DT_IOP_COLOREQUAL_HUE)
                         ? (dt_modifier_is(state, GDK_SHIFT_MASK) ? DT_IOP_COLOREQUAL_SCROLL_HUE_STEP_COARSE
                            : dt_modifier_is(state, GDK_CONTROL_MASK) ? DT_IOP_COLOREQUAL_SCROLL_HUE_STEP_FINE
                                                                      : DT_IOP_COLOREQUAL_SCROLL_HUE_STEP)
                         : (dt_modifier_is(state, GDK_SHIFT_MASK) ? DT_IOP_COLOREQUAL_SCROLL_STEP_COARSE
                            : dt_modifier_is(state, GDK_CONTROL_MASK) ? DT_IOP_COLOREQUAL_SCROLL_STEP_FINE
                                                                      : DT_IOP_COLOREQUAL_SCROLL_STEP);

  for(int k = 0; k < nodes; k++)
  {
    const float distance = _curve_periodic_distance(curve[k].x, curve_x);
    const float weight = expf(-(distance * distance) / sigma2);
    float value = _channel_value_from_y(channel, curve[k].y);
    value += direction * step * weight;

    switch(channel)
    {
      case DT_IOP_COLOREQUAL_HUE:
        value = CLAMP(value, -M_PI_F, M_PI_F);
        break;
      case DT_IOP_COLOREQUAL_SATURATION:
      case DT_IOP_COLOREQUAL_BRIGHTNESS:
      default:
        value = CLAMP(value, 0.f, 2.f);
        break;
    }

    curve[k].y = _channel_y_from_value(channel, value);
  }

  memcpy(self->params, &g->gui_params, sizeof(dt_iop_colorequal_params_t));
  g->curve_cache_valid = FALSE;
  g->viewer_lut_dirty = TRUE;
  _update_gui_lut_cache(self);
  _refresh_preview_cursor_sample(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][channel]));
  dt_control_queue_redraw_center();
  dt_dev_add_history_item(darktable.develop, self, FALSE, TRUE);
  return 1;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_develop_t *dev = self ? self->dev : NULL;
  if(!self || !g || !dev || !g->has_focus || !self->enabled || !g->cursor_valid || dt_iop_color_picker_is_visible(dev))
    return;

  if((dev->preview_pipe && dev->preview_pipe->processing) || !g->cursor_sample_valid)
  {
    if(!_refresh_preview_cursor_sample(self)) return;
  }

  const dt_iop_colorequal_ring_t ring = _active_ring_from_gui(g);
  const dt_iop_colorequal_channel_t channel = _active_channel_from_gui(g, ring);
  float curve_x = 0.f;
  float curve_y = 0.f;
  float offset_normalized = 0.f;
  if(!_cursor_curve_state(&g->gui_params, ring, channel, g->cursor_hue, &curve_x, &curve_y, &offset_normalized))
    return;

  const float zoom_scale = dt_dev_get_overlay_scale(dev);
  dt_dev_rescale_roi(dev, cr, width, height);

  const float pointer_x = g->cursor_pos_x;
  const float pointer_y = g->cursor_pos_y;
  const float outer_radius = DT_IOP_COLOREQUAL_PREVIEW_CURSOR_RADIUS / zoom_scale;
  const float inner_radius = outer_radius * 0.55f;
  const float intensity_radius = outer_radius * 1.55f;
  const float crosshair_gap = outer_radius * 0.25f;
  const float crosshair_extent = intensity_radius + DT_PIXEL_APPLY_DPI(5.f) / zoom_scale;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5f) / zoom_scale);
  cairo_set_source_rgba(cr, 0.f, 0.f, 0.f, 0.35f);
  cairo_arc(cr, pointer_x, pointer_y, intensity_radius, 0.f, 2.f * M_PI_F);
  cairo_stroke(cr);

  if(fabsf(offset_normalized) > 1e-4f)
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.f) / zoom_scale);
    cairo_set_source_rgba(cr, g->cursor_output_display[0], g->cursor_output_display[1], g->cursor_output_display[2],
                          0.95);
    if(offset_normalized > 0.f)
      cairo_arc(cr, pointer_x, pointer_y, intensity_radius, -M_PI_F / 2.f,
                -M_PI_F / 2.f + 2.f * M_PI_F * fabsf(offset_normalized));
    else
      cairo_arc_negative(cr, pointer_x, pointer_y, intensity_radius, -M_PI_F / 2.f,
                         -M_PI_F / 2.f - 2.f * M_PI_F * fabsf(offset_normalized));
    cairo_stroke(cr);
  }

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.25f) / zoom_scale);
  cairo_set_source_rgba(cr, 1.f, 1.f, 1.f, 0.6f);
  cairo_move_to(cr, pointer_x - crosshair_extent, pointer_y);
  cairo_line_to(cr, pointer_x - outer_radius - crosshair_gap, pointer_y);
  cairo_move_to(cr, pointer_x + outer_radius + crosshair_gap, pointer_y);
  cairo_line_to(cr, pointer_x + crosshair_extent, pointer_y);
  cairo_move_to(cr, pointer_x, pointer_y - crosshair_extent);
  cairo_line_to(cr, pointer_x, pointer_y - outer_radius - crosshair_gap);
  cairo_move_to(cr, pointer_x, pointer_y + outer_radius + crosshair_gap);
  cairo_line_to(cr, pointer_x, pointer_y + crosshair_extent);
  cairo_stroke(cr);

  cairo_arc(cr, pointer_x, pointer_y, outer_radius, 0.f, 2.f * M_PI_F);
  cairo_set_source_rgba(cr, g->cursor_output_display[0], g->cursor_output_display[1], g->cursor_output_display[2],
                        0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.f, 0.f, 0.f, 0.9f);
  cairo_stroke(cr);

  cairo_arc(cr, pointer_x, pointer_y, inner_radius, 0.f, 2.f * M_PI_F);
  cairo_set_source_rgba(cr, g->cursor_input_display[0], g->cursor_input_display[1], g->cursor_input_display[2], 0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1.f, 1.f, 1.f, 0.8f);
  cairo_stroke(cr);
}

static void _pipe_rgb_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_aligned_pixel_t RGB,
                             dt_aligned_pixel_t Ych)
{
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(!work_profile) return;
  dt_colorrings_profile_rgb_to_Ych(RGB, work_profile, Ych);
}

static void _pipe_rgb_to_dt_ucs_hsb(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_aligned_pixel_t RGB,
                                    dt_aligned_pixel_t HSB)
{
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(!work_profile) return;
  dt_colorrings_profile_rgb_to_dt_ucs_hsb(RGB, dt_colorrings_graph_white(), work_profile, HSB);
}

static void _switch_preview_cursor(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);

  if(!g || !widget || !gtk_widget_get_window(widget)) return;

  if(!g->has_focus || dt_iop_color_picker_is_visible(self->dev))
  {
    GdkCursor *const cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
    return;
  }

  if(g->cursor_valid && self->dev && self->dev->preview_pipe && self->dev->preview_pipe->processing)
  {
    GdkCursor *const cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "wait");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
    return;
  }

  if(g->cursor_valid && self->enabled)
  {
    dt_control_change_cursor(GDK_BLANK_CURSOR);
    dt_control_set_cursor(GDK_BLANK_CURSOR);
    dt_control_hinter_message(darktable.control,
                              _("scroll over image to adjust the selected color graph\n"
                                "right-click to add a node at the sampled hue"));
    return;
  }

  GdkCursor *const cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
}

static gboolean _refresh_preview_cursor_sample(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)self->global_data;
  dt_develop_t *dev = self ? self->dev : NULL;
  if(!self || !g || !gd || !dev || !dev->preview_pipe || !self->enabled || !g->cursor_valid)
  {
    _invalidate_preview_cursor(g);
    return FALSE;
  }

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(dev->preview_pipe, self);
  const dt_dev_pixelpipe_iop_t *const previous_piece
      = piece ? dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece) : NULL;
  if(!piece || !previous_piece || previous_piece->dsc_out.datatype != TYPE_FLOAT || previous_piece->dsc_out.channels < 3)
  {
    g->pending_preview_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    _invalidate_preview_cursor(g);
    return FALSE;
  }

  g->pending_preview_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  void *input = NULL;
  dt_pixel_cache_entry_t *input_entry = NULL;
  if(!dt_dev_pixelpipe_cache_peek_gui(dev->preview_pipe, previous_piece, &input, &input_entry, NULL, NULL, NULL) || !input || !input_entry)
  {
    g->pending_preview_hash = previous_piece->global_hash;
    if(!dev->preview_pipe->processing) dt_dev_pixelpipe_update_history_preview(dev);
    _invalidate_preview_cursor(g);
    return FALSE;
  }

  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);

  const float point_preview[2] = { (float)g->cursor_pos_x, (float)g->cursor_pos_y };
  float point_image[2] = { point_preview[0], point_preview[1] };
  dt_dev_coordinates_preview_abs_to_image_norm(dev, point_image, 1);

  const float scale_x = (previous_piece->buf_out.width > 0)
                            ? (float)previous_piece->roi_out.width / (float)previous_piece->buf_out.width
                            : 1.f;
  const float scale_y = (previous_piece->buf_out.height > 0)
                            ? (float)previous_piece->roi_out.height / (float)previous_piece->buf_out.height
                            : 1.f;
  const float sample_x
      = point_image[0] * (float)previous_piece->buf_out.width * scale_x - (float)previous_piece->roi_out.x;
  const float sample_y
      = point_image[1] * (float)previous_piece->buf_out.height * scale_y - (float)previous_piece->roi_out.y;
  const int xi = CLAMP((int)lroundf(sample_x), 0, previous_piece->roi_out.width - 1);
  const int yi = CLAMP((int)lroundf(sample_y), 0, previous_piece->roi_out.height - 1);

  dt_aligned_pixel_t input_rgb = { 0.f };
  const float *const input_rgbf
      = (const float *)input + ((size_t)yi * (size_t)previous_piece->roi_out.width + (size_t)xi) * previous_piece->dsc_out.channels;
  input_rgb[0] = input_rgbf[0];
  input_rgb[1] = input_rgbf[1];
  input_rgb[2] = input_rgbf[2];
  input_rgb[3] = 0.f;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  if(g->viewer && (g->viewer_lut_dirty || !g->viewer_lut_valid)) _update_gui_lut_cache(self);

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, dev->preview_pipe);
  const dt_iop_order_iccprofile_info_t *const lut_profile = g->viewer_lut.lut_profile;
  dt_aligned_pixel_t output_rgb = { input_rgb[0], input_rgb[1], input_rgb[2], 0.f };
  dt_colorrings_apply_rgb_lut(input_rgb, exp2f(g->gui_params.white_level), work_profile, lut_profile,
                              g->viewer_lut.clut, g->viewer_lut.clut_level, &gd->lock,
                              (dt_lut3d_interpolation_t)g->gui_params.interpolation, output_rgb);

  dt_aligned_pixel_t projected_rgb = { 0.f };
  const float white_level = fmaxf(exp2f(g->gui_params.white_level), 1e-6f);
  projected_rgb[0] = input_rgb[0] / white_level;
  projected_rgb[1] = input_rgb[1] / white_level;
  projected_rgb[2] = input_rgb[2] / white_level;

  const float neutral = CLAMP((projected_rgb[0] + projected_rgb[1] + projected_rgb[2]) / 3.f, 0.f, 1.f);
  dt_aligned_pixel_t axis = { neutral, neutral, neutral, 0.f };
  dt_colorrings_project_to_cube_shell(axis, projected_rgb);

  dt_aligned_pixel_t HSB = { 0.f };
  _pipe_rgb_to_dt_ucs_hsb(self, dev->preview_pipe, projected_rgb, HSB);
  if(!isfinite(HSB[0]))
  {
    _invalidate_preview_cursor(g);
    return FALSE;
  }

  g->cursor_hue = dt_colorrings_wrap_hue_pi(HSB[0]);
  _work_rgb_to_display_rgb(self, dev->preview_pipe, input_rgb, g->cursor_input_display);
  _work_rgb_to_display_rgb(self, dev->preview_pipe, output_rgb, g->cursor_output_display);
  g->cursor_sample_valid = TRUE;
  return TRUE;
}

/**
 * The module-level picker reports where the sampled dt UCS brightness sits
 * between the three editable rings, using the same fixed ring boundaries as
 * the control surface. This gives users an immediate reading of which graph
 * pair will mostly influence the picked color.
 */
static void _format_picker_brightness_position(const float brightness, char *text, const size_t size)
{
  if(brightness <= 0.15f)
  {
    g_snprintf(text, size, "%d%% %s", 100, _("shadows"));
  }
  else if(brightness < 0.45f)
  {
    const float t = (brightness - 0.15f) / (0.45f - 0.15f);
    const int shadows = CLAMP((int)lroundf((1.f - t) * 100.f), 0, 100);
    g_snprintf(text, size, "%d%% %s, %d%% %s", shadows, _("shadows"), 100 - shadows, _("midtones"));
  }
  else if(brightness < 0.75f)
  {
    const float t = (brightness - 0.45f) / (0.75f - 0.45f);
    const int midtones = CLAMP((int)lroundf((1.f - t) * 100.f), 0, 100);
    g_snprintf(text, size, "%d%% %s, %d%% %s", midtones, _("midtones"), 100 - midtones, _("highlights"));
  }
  else
  {
    g_snprintf(text, size, "%d%% %s", 100, _("highlights"));
  }
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe,
                        dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;
  const dt_iop_module_t *sampled_module = piece && piece->module ? piece->module : self;

  if(!g) return;

  if(picker == g->white_level)
  {
    dt_aligned_pixel_t max_Ych = { 0.f };
    _pipe_rgb_to_Ych(self, pipe, (const float *)sampled_module->picked_color_max, max_Ych);

    ++darktable.gui->reset;
    p->white_level = log2f(max_Ych[0]);
    g->gui_params.white_level = p->white_level;
    dt_bauhaus_slider_set(g->white_level, p->white_level);
    --darktable.gui->reset;

    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
  }
  else if(picker == g->module_picker)
  {
    if(sampled_module->picked_color_max[0] < sampled_module->picked_color_min[0])
    {
      g->picker_valid = FALSE;
      gtk_label_set_text(GTK_LABEL(g->picker_info), _("no sample"));
      for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
        for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
          gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][ch]));
      return;
    }

    dt_aligned_pixel_t HSB = { 0.f };
    dt_aligned_pixel_t RGB = { 0.f };
    dt_aligned_pixel_t axis = { 0.f };
    char text[128] = { 0 };
    const float white_level = fmaxf(exp2f(g->gui_params.white_level), 1e-6f);

    RGB[0] = sampled_module->picked_color[0] / white_level;
    RGB[1] = sampled_module->picked_color[1] / white_level;
    RGB[2] = sampled_module->picked_color[2] / white_level;

    /**
     * The pipe sample is scene-linear working RGB and may be negative or far
     * above diffuse white. The GUI rings however live on bounded dt UCS hue
     * rings derived from RGB code values in [0, 1]. Project the normalized
     * sample back to the RGB cube shell before converting it to dt UCS HSB so
     * the marker reports where that color sits in the editable control space.
     */
    const float neutral = CLAMP((RGB[0] + RGB[1] + RGB[2]) / 3.f, 0.f, 1.f);
    axis[0] = neutral;
    axis[1] = neutral;
    axis[2] = neutral;
    dt_colorrings_project_to_cube_shell(axis, RGB);
    _pipe_rgb_to_dt_ucs_hsb(self, pipe, RGB, HSB);

    g->picker_valid = TRUE;
    g->picker_hue = dt_colorrings_wrap_hue_pi(HSB[0]);
    g->picker_brightness = CLAMP(HSB[2], 0.f, 1.f);
    _format_picker_brightness_position(g->picker_brightness, text, sizeof(text));
    gtk_label_set_text(GTK_LABEL(g->picker_info), text);

    for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
      for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
        gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][ch]));
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(g)
  {
    const dt_iop_colorequal_params_t *p = (const dt_iop_colorequal_params_t *)self->params;
    const gboolean curves_changed = !_curve_fields_equal(&g->gui_params, p);
    const gboolean lut_changed = !_lut_fields_equal(&g->gui_params, p);
    memcpy(&g->gui_params, self->params, sizeof(dt_iop_colorequal_params_t));
    if(curves_changed)
    {
      g->curve_cache_valid = FALSE;
      for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
        for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
          gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][ch]));

      if(g->cursor_valid && g->has_focus) dt_control_queue_redraw_center();
    }

    if(lut_changed)
    {
      g->viewer_lut_dirty = TRUE;
      if(self->dev && self->dev->gui_module == self && self->dev->preview_pipe && !self->dev->preview_pipe->processing)
      {
        _update_gui_lut_cache(self);
        if(g->cursor_valid && g->has_focus) _refresh_preview_cursor_sample(self);
        dt_lut_viewer_queue_draw(g->viewer);
        if(g->cursor_valid && g->has_focus) dt_control_queue_redraw_center();
      }
    }
  }
}

void gui_update(dt_iop_module_t *self)
{
  const dt_iop_colorequal_params_t *p = (const dt_iop_colorequal_params_t *)self->params;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  if(g)
  {
    /**
     * History navigation and fast module toggles can reload a newer parameter
     * state into the module while an older throttled graph edit is still
     * queued. Once GUI update sees that the history-backed module params no
     * longer match the live graph cache, that delayed local commit is stale and
     * must be dropped or it may replay obsolete parameters after the history
     * stack already moved on.
     */
    if(!_lut_fields_equal(&g->gui_params, p)) dt_gui_throttle_cancel(self);

    const gboolean curves_changed = !_curve_fields_equal(&g->gui_params, p);
    const gboolean lut_changed = !_lut_fields_equal(&g->gui_params, p);
    memcpy(&g->gui_params, p, sizeof(dt_iop_colorequal_params_t));
    dt_bauhaus_slider_set(g->white_level, p->white_level);
    dt_bauhaus_slider_set(g->sigma_L, p->sigma_L);
    dt_bauhaus_slider_set(g->sigma_rho, p->sigma_rho);
    dt_bauhaus_slider_set(g->sigma_theta, p->sigma_theta);
    dt_bauhaus_slider_set(g->neutral_protection, p->neutral_protection);
    dt_bauhaus_combobox_set(g->interpolation, p->interpolation);
    if(curves_changed)
    {
      g->curve_cache_valid = FALSE;
      for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
        for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
          gtk_widget_queue_draw(GTK_WIDGET(g->area[ring][ch]));

      if(g->cursor_valid && g->has_focus) dt_control_queue_redraw_center();
    }

    if(lut_changed)
    {
      g->viewer_lut_dirty = TRUE;
      if(self->dev && self->dev->gui_module == self && self->dev->preview_pipe && !self->dev->preview_pipe->processing)
      {
        _update_gui_lut_cache(self);
        if(g->cursor_valid && g->has_focus) _refresh_preview_cursor_sample(self);
        dt_lut_viewer_queue_draw(g->viewer);
        if(g->cursor_valid && g->has_focus) dt_control_queue_redraw_center();
      }
    }
  }
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!g) return;

  g->has_focus = in;

  if(in)
  {
    if(!g->preview_signal_connected)
    {
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_HISTORY_RESYNC,
                                      G_CALLBACK(_history_resync_callback), self);
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CACHELINE_READY,
                                      G_CALLBACK(_cacheline_ready_callback), self);
      g->preview_signal_connected = TRUE;
    }

    if(g->viewer && (g->viewer_lut_dirty || !g->viewer_lut_valid))
    {
      _update_gui_lut_cache(self);
      dt_lut_viewer_queue_draw(g->viewer);
    }

    if(g->cursor_valid && self->dev && self->dev->preview_pipe && !self->dev->preview_pipe->processing)
      _refresh_preview_cursor_sample(self);
  }
  else if(g->preview_signal_connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_history_resync_callback), self);
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_cacheline_ready_callback), self);
    g->preview_signal_connected = FALSE;
    g->pending_preview_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  }

  _switch_preview_cursor(self);
  dt_control_queue_redraw_center();
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_gui_throttle_cancel(self);

  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
    for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++)
    {
      if(g->curve[ring][ch]) dt_draw_curve_destroy(g->curve[ring][ch]);
      g->curve[ring][ch] = NULL;
      if(g->background_surface[ring][ch]) cairo_surface_destroy(g->background_surface[ring][ch]);
      g->background_surface[ring][ch] = NULL;
    }

  g->viewer_lut.clut = NULL;

  dt_lut_viewer_destroy(&g->viewer);
  if(g->preview_signal_connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_history_resync_callback), self);
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_cacheline_ready_callback), self);
    g->preview_signal_connected = FALSE;
  }

  const int current_primary = CLAMP(gtk_notebook_get_current_page(g->ring_notebook), 0, DT_IOP_COLOREQUAL_NUM_RINGS);
  dt_conf_set_int("plugins/darkroom/colorequal/gui_ring_page", current_primary);
  dt_conf_set_int("plugins/darkroom/colorequal/gui_channel_page",
                  gtk_notebook_get_current_page(g->channel_notebook[0]));
  IOP_GUI_FREE;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = IOP_GUI_ALLOC(colorequal);
  memcpy(&g->gui_params, self->params, sizeof(dt_iop_colorequal_params_t));
  g->curve_cache_valid = FALSE;
  g->viewer_lut_dirty = TRUE;
  g->viewer_lut_valid = FALSE;
  g->viewer_lut_generation = 0;
  g->preview_signal_connected = FALSE;
  g->pending_preview_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  g->has_focus = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  GtkWidget *ring_tabs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), ring_tabs_box, TRUE, TRUE, 0);

  g->ring_notebook = GTK_NOTEBOOK(gtk_notebook_new());
  gtk_widget_set_name(GTK_WIDGET(g->ring_notebook), "colorequal-ring-tabs");
  gtk_notebook_set_show_border(g->ring_notebook, FALSE);
  g_signal_connect(G_OBJECT(g->ring_notebook), "switch_page", G_CALLBACK(_ring_tabs_switch_callback), self);
  gtk_box_pack_start(GTK_BOX(ring_tabs_box), GTK_WIDGET(g->ring_notebook), TRUE, TRUE, 0);

  const dt_iop_colorequal_channel_t channel_order[DT_IOP_COLOREQUAL_NUM_CHANNELS]
      = { DT_IOP_COLOREQUAL_SATURATION, DT_IOP_COLOREQUAL_BRIGHTNESS, DT_IOP_COLOREQUAL_HUE };
  const char *channel_labels[DT_IOP_COLOREQUAL_NUM_CHANNELS] = { _("saturation"), _("brightness"), _("hue") };

  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
  {
    GtkWidget *ring_label = gtk_label_new(_ring_label((dt_iop_colorequal_ring_t)ring));
    dt_gui_add_class(ring_label, "dt_modulegroups_tab_label");
    GtkWidget *ring_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    gtk_notebook_append_page(g->ring_notebook, ring_page, ring_label);
    gtk_container_child_set(GTK_CONTAINER(g->ring_notebook), ring_page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

    g->channel_notebook[ring] = GTK_NOTEBOOK(gtk_notebook_new());
    g_object_set_data(G_OBJECT(g->channel_notebook[ring]), "colorequal-ring", GINT_TO_POINTER(ring));
    g_signal_connect(G_OBJECT(g->channel_notebook[ring]), "switch_page", G_CALLBACK(_channel_tabs_switch_callback),
                     self);
    gtk_box_pack_start(GTK_BOX(ring_page), GTK_WIDGET(g->channel_notebook[ring]), TRUE, TRUE, 0);

    for(int order = 0; order < DT_IOP_COLOREQUAL_NUM_CHANNELS; order++)
    {
      const dt_iop_colorequal_channel_t ch = channel_order[order];
      GtkWidget *channel_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
      g_object_set_data(G_OBJECT(channel_page), "colorequal-channel", GINT_TO_POINTER(ch));
      g->area[ring][ch] = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(2.f / 3.f));
      gtk_widget_add_events(GTK_WIDGET(g->area[ring][ch]),
                            GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
      g_object_set_data(G_OBJECT(g->area[ring][ch]), "colorequal-ring", GINT_TO_POINTER(ring));
      g_object_set_data(G_OBJECT(g->area[ring][ch]), "colorequal-channel", GINT_TO_POINTER(ch));
      g_signal_connect(G_OBJECT(g->area[ring][ch]), "draw", G_CALLBACK(_draw_curve), self);
      g_signal_connect(G_OBJECT(g->area[ring][ch]), "motion-notify-event",
                       G_CALLBACK(_area_motion_notify_callback), self);
      g_signal_connect(G_OBJECT(g->area[ring][ch]), "button-press-event", G_CALLBACK(_area_button_press_callback),
                       self);
      g_signal_connect(G_OBJECT(g->area[ring][ch]), "button-release-event",
                       G_CALLBACK(_area_button_release_callback), self);
      gtk_box_pack_start(GTK_BOX(channel_page), GTK_WIDGET(g->area[ring][ch]), TRUE, TRUE, 0);
      gtk_notebook_append_page(g->channel_notebook[ring], channel_page, gtk_label_new(channel_labels[order]));
      gtk_container_child_set(GTK_CONTAINER(g->channel_notebook[ring]), channel_page, "tab-expand", TRUE,
                              "tab-fill", TRUE, NULL);
    }
  }

  GtkWidget *options_label = gtk_label_new(_("options"));
  dt_gui_add_class(options_label, "dt_modulegroups_tab_label");
  GtkWidget *options_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_notebook_append_page(g->ring_notebook, options_page, options_label);
  gtk_container_child_set(GTK_CONTAINER(g->ring_notebook), options_page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  GtkWidget *const module_root = self->widget;
  self->widget = options_page;

  g->white_level
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_level"));
  dt_bauhaus_slider_set_soft_range(g->white_level, -2.f, 2.f);
  dt_bauhaus_slider_set_format(g->white_level, _(" EV"));

  g->sigma_L = dt_bauhaus_slider_from_params(self, "sigma_L");
  dt_bauhaus_slider_set_soft_range(g->sigma_L, 1.f, 100.f);
  dt_bauhaus_slider_set_format(g->sigma_L, _(" %"));

  g->sigma_rho = dt_bauhaus_slider_from_params(self, "sigma_rho");
  dt_bauhaus_slider_set_soft_range(g->sigma_rho, 0.1f, 1.5f);

  g->sigma_theta = dt_bauhaus_slider_from_params(self, "sigma_theta");
  dt_bauhaus_slider_set_soft_range(g->sigma_theta, 0.05f, 0.8f);

  g->neutral_protection = dt_bauhaus_slider_from_params(self, "neutral_protection");
  dt_bauhaus_slider_set_soft_range(g->neutral_protection, 0.f, 1.f);

  g->interpolation = dt_bauhaus_combobox_from_params(self, "interpolation");
  gtk_widget_set_tooltip_text(g->interpolation, _("select the interpolation method"));

  self->widget = module_root;

  GtkWidget *picker_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(ring_tabs_box), picker_box, FALSE, FALSE, 0);

  g->module_picker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_AREA, NULL, IOP_CS_RGB);
  gtk_box_pack_start(GTK_BOX(picker_box), g->module_picker, FALSE, FALSE, 0);

  g->picker_info = gtk_label_new(_("no sample"));
  gtk_widget_set_hexpand(g->picker_info, TRUE);
  gtk_widget_set_halign(g->picker_info, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(g->picker_info), 0.f);
  gtk_box_pack_start(GTK_BOX(picker_box), g->picker_info, TRUE, TRUE, 0);

  const int active_ring = dt_conf_get_int("plugins/darkroom/colorequal/gui_ring_page");
  const int active_channel = dt_conf_get_int("plugins/darkroom/colorequal/gui_channel_page");
  const int current_ring_page = CLAMP(active_ring, 0, DT_IOP_COLOREQUAL_NUM_RINGS);
  const int current_channel_page = CLAMP(active_channel, 0, DT_IOP_COLOREQUAL_NUM_CHANNELS - 1);

  ++darktable.gui->reset;
  gtk_notebook_set_current_page(g->ring_notebook, current_ring_page);
  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
    gtk_notebook_set_current_page(g->channel_notebook[ring], current_channel_page);
  --darktable.gui->reset;

  for(int ring = 0; ring < DT_IOP_COLOREQUAL_NUM_RINGS; ring++)
    for(int ch = 0; ch < DT_IOP_COLOREQUAL_NUM_CHANNELS; ch++) g->selected[ring][ch] = -1;

  g->viewer_lut.clut = NULL;
  g->viewer_lut.clut_level = 0;
  memset(g->viewer_lut.reference_saturation, 0, sizeof(g->viewer_lut.reference_saturation));
  g->viewer_lut.lut_profile = NULL;
  g->viewer_lut.work_profile = NULL;
  g->viewer_control_node_count = 0;
  g->viewer = dt_lut_viewer_new(DT_GUI_MODULE(self));
  if(g->viewer)
    gtk_box_pack_start(GTK_BOX(GTK_BOX(self->widget)), dt_lut_viewer_get_widget(g->viewer), TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorequal_global_data_t *gd = malloc(sizeof(*gd));
  module->data = gd;
  dt_pthread_rwlock_init(&gd->lock, NULL);
  gd->cache.clut = NULL;
  gd->cache.clut_level = 0;
  gd->cache.white_level = 1.f;
  memset(gd->cache.reference_saturation, 0, sizeof(gd->cache.reference_saturation));
  gd->cache.lut_profile = NULL;
  gd->cache.work_profile = NULL;
  memset(&gd->params, 0, sizeof(gd->params));
  gd->cache_valid = FALSE;
  gd->cache_generation = 0;
  gd->kernel_lut3d_tetrahedral = -1;
  gd->kernel_lut3d_trilinear = -1;
  gd->kernel_lut3d_pyramid = -1;
  gd->kernel_exposure = -1;

#ifdef HAVE_OPENCL
  const int lut_program = 28; // lut3d.cl, from programs.conf
  const int basic_program = 2; // basic.cl, from programs.conf
  gd->kernel_lut3d_tetrahedral = dt_opencl_create_kernel(lut_program, "lut3d_tetrahedral");
  gd->kernel_lut3d_trilinear = dt_opencl_create_kernel(lut_program, "lut3d_trilinear");
  gd->kernel_lut3d_pyramid = dt_opencl_create_kernel(lut_program, "lut3d_pyramid");
  gd->kernel_exposure = dt_opencl_create_kernel(basic_program, "exposure");
#endif
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorequal_global_data_t *gd = (dt_iop_colorequal_global_data_t *)module->data;
  dt_free_align(gd->cache.clut);
  gd->cache.clut = NULL;
#ifdef HAVE_OPENCL
  dt_opencl_free_kernel(gd->kernel_lut3d_tetrahedral);
  dt_opencl_free_kernel(gd->kernel_lut3d_trilinear);
  dt_opencl_free_kernel(gd->kernel_lut3d_pyramid);
  dt_opencl_free_kernel(gd->kernel_exposure);
#endif
  dt_pthread_rwlock_destroy(&gd->lock);
  dt_free(module->data);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  _init_default_curves((dt_iop_colorequal_params_t *)module->default_params);
  memcpy(module->params, module->default_params, module->params_size);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
