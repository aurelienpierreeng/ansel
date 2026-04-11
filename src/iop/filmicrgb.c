/*
   This file is part of the Ansel project.
   Copyright (C) 2019-2020, 2022 Aldric Renaudin.
   Copyright (C) 2019-2026 Aurélien PIERRE.
   Copyright (C) 2019 Diederik ter Rahe.
   Copyright (C) 2019-2022 Pascal Obry.
   Copyright (C) 2019 Tobias Ellinghaus.
   Copyright (C) 2020-2021 Chris Elston.
   Copyright (C) 2020-2022 Diederik Ter Rahe.
   Copyright (C) 2020 Heiko Bauke.
   Copyright (C) 2020-2021 Hubert Kowalski.
   Copyright (C) 2020 Jeronimo Pellegrini.
   Copyright (C) 2020 Marco Carrarini.
   Copyright (C) 2020 Mark-64.
   Copyright (C) 2020 Martin Burri.
   Copyright (C) 2020-2021 Ralf Brown.
   Copyright (C) 2020-2021 rawfiner.
   Copyright (C) 2021 Dan Torop.
   Copyright (C) 2021 Fabio Heer.
   Copyright (C) 2021 lhietal.
   Copyright (C) 2021 luzpaz.
   Copyright (C) 2021 paolodepetrillo.
   Copyright (C) 2021-2022 Sakari Kapanen.
   Copyright (C) 2021 Victor Forsiuk.
   Copyright (C) 2022 Hanno Schwalm.
   Copyright (C) 2022 Martin Bařinka.
   Copyright (C) 2022 Nicolas Auffray.
   Copyright (C) 2022 Philipp Lutz.
   Copyright (C) 2023 Alban Gruin.
   Copyright (C) 2023-2024 Alynx Zhou.
   Copyright (C) 2023 Luca Zulberti.
   Copyright (C) 2025 Guillaume Stutin.
   
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
#include "common/colorspaces_inline_conversions.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable.h"
#include "common/bspline.h"
#include "common/dwt.h"
#include "common/image.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"

#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/gaussian_elimination.h"
#include "iop/iop_api.h"


#include "develop/imageop.h"
#include "gui/draw.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INVERSE_SQRT_3 0.5773502691896258f
#define SAFETY_MARGIN 0.01f

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


DT_MODULE_INTROSPECTION(5, dt_iop_filmicrgb_params_t)

/**
 * DOCUMENTATION
 *
 * This code ports :
 * 1. Troy Sobotka's filmic curves for Blender (and other softs)
 *      https://github.com/sobotka/OpenAgX/blob/master/lib/agx_colour.py
 * 2. ACES camera logarithmic encoding
 *        https://github.com/ampas/aces-dev/blob/master/transforms/ctl/utilities/ACESutil.Lin_to_Log2_param.ctl
 *
 * The ACES log implementation is taken from the profile_gamma.c IOP
 * where it works in camera RGB space. Here, it works on an arbitrary RGB
 * space. ProPhotoRGB has been chosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface useful to remap accurately and promptly the middle grey
 * to any arbitrary value chosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustments darktable
 * is prone to enforce.
 *
 * */


/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 **/
#if defined(__GNUC__)
#pragma GCC optimize("finite-math-only", "fp-contract=fast", "fast-math", "no-math-errno")
#endif


typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,              // $DESCRIPTION: "no"
  DT_FILMIC_METHOD_MAX_RGB = 1,           // $DESCRIPTION: "max RGB"
  DT_FILMIC_METHOD_LUMINANCE = 2,         // $DESCRIPTION: "luminance Y"
  DT_FILMIC_METHOD_POWER_NORM = 3,        // $DESCRIPTION: "RGB power norm"
  DT_FILMIC_METHOD_EUCLIDEAN_NORM_V2 = 5, // $DESCRIPTION: "RGB euclidean norm"
  DT_FILMIC_METHOD_EUCLIDEAN_NORM_V1 = 4, // $DESCRIPTION: "RGB euclidean norm (legacy)"
} dt_iop_filmicrgb_methods_type_t;


typedef enum dt_iop_filmicrgb_curve_type_t
{
  DT_FILMIC_CURVE_POLY_4 = 0, // $DESCRIPTION: "hard"
  DT_FILMIC_CURVE_POLY_3 = 1,  // $DESCRIPTION: "soft"
  DT_FILMIC_CURVE_RATIONAL = 2, // $DESCRIPTION: "safe"
} dt_iop_filmicrgb_curve_type_t;


typedef enum dt_iop_filmicrgb_colorscience_type_t
{
  DT_FILMIC_COLORSCIENCE_V1 = 0, // $DESCRIPTION: "v3 (2019)"
  DT_FILMIC_COLORSCIENCE_V2 = 1, // $DESCRIPTION: "v4 (2020)"
  DT_FILMIC_COLORSCIENCE_V3 = 2, // $DESCRIPTION: "v5 (2021)"
  DT_FILMIC_COLORSCIENCE_V4 = 3, // $DESCRIPTION: "v6 (2022)"
  DT_FILMIC_COLORSCIENCE_V5 = 4, // $DESCRIPTION: "v7 (2023)"
} dt_iop_filmicrgb_colorscience_type_t;

typedef enum dt_iop_filmicrgb_spline_version_type_t
{
  DT_FILMIC_SPLINE_VERSION_V1 = 0, // $DESCRIPTION: "v1 (2019)"
  DT_FILMIC_SPLINE_VERSION_V2 = 1, // $DESCRIPTION: "v2 (2020)"
  DT_FILMIC_SPLINE_VERSION_V3 = 2, // $DESCRIPTION: "v3 (2021)"
} dt_iop_filmicrgb_spline_version_type_t;

typedef enum dt_iop_filmicrgb_reconstruction_type_t
{
  DT_FILMIC_RECONSTRUCT_RGB = 0,
  DT_FILMIC_RECONSTRUCT_RATIOS = 1,
} dt_iop_filmicrgb_reconstruction_type_t;


typedef struct dt_iop_filmic_rgb_spline_t
{
  dt_aligned_pixel_t M1, M2, M3, M4, M5;                    // factors for the interpolation polynom
  float latitude_min, latitude_max;                         // bounds of the latitude == linear part by design
  float y[5];                                               // controls nodes
  float x[5];                                               // controls nodes
  dt_iop_filmicrgb_curve_type_t type[2];
} dt_iop_filmic_rgb_spline_t;


typedef enum dt_iop_filmic_rgb_gui_mode_t
{
  DT_FILMIC_GUI_LOOK = 0,      // default GUI, showing only the contrast curve in a log/gamma space
  DT_FILMIC_GUI_BASECURVE = 1, // basecurve-like GUI, showing the contrast and brightness curves, in lin/lin space
  DT_FILMIC_GUI_BASECURVE_LOG = 2, // same as previous, but log-scaled
  DT_FILMIC_GUI_RANGES = 3,        // zone-system-like GUI, showing the range to range mapping
  DT_FILMIC_GUI_LAST
} dt_iop_filmic_rgb_gui_mode_t;

// copy enum definition for introspection
typedef enum dt_iop_filmic_noise_distribution_t
{
  DT_FILMIC_NOISE_UNIFORM = DT_NOISE_UNIFORM,      // $DESCRIPTION: "uniform"
  DT_FILMIC_NOISE_GAUSSIAN = DT_NOISE_GAUSSIAN,    // $DESCRIPTION: "gaussian"
  DT_FILMIC_NOISE_POISSONIAN = DT_NOISE_POISSONIAN // $DESCRIPTION: "poissonian"
} dt_iop_filmic_noise_distribution_t;

// clang-format off
typedef struct dt_iop_filmicrgb_params_t
{
  float grey_point_source;     // $MIN: 0 $MAX: 100 $DEFAULT: 18.45 $DESCRIPTION: "middle gray luminance"
  float black_point_source;    // $MIN: -16 $MAX: -0.1 $DEFAULT: -8.0 $DESCRIPTION: "black relative exposure"
  float white_point_source;    // $MIN: 0.1 $MAX: 16 $DEFAULT: 4.0 $DESCRIPTION: "white relative exposure"
  float reconstruct_threshold; // $MIN: -6.0 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "threshold"
  float reconstruct_feather;   // $MIN: 0.25 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "transition"
  float reconstruct_bloom_vs_details; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "bloom \342\206\224 reconstruct"
  float reconstruct_grey_vs_color; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "gray \342\206\224 colorful details"
  float reconstruct_structure_vs_texture; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "structure \342\206\224 texture"
  float security_factor;                  // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "dynamic range scaling"
  float grey_point_target;                // $MIN: 1 $MAX: 50 $DEFAULT: 18.45 $DESCRIPTION: "target middle gray"
  float black_point_target; // $MIN: 0.000 $MAX: 20.000 $DEFAULT: 0.01517634 $DESCRIPTION: "target black luminance"
  float white_point_target; // $MIN: 0 $MAX: 1600 $DEFAULT: 100 $DESCRIPTION: "target white luminance"
  float output_power;       // $MIN: 1 $MAX: 10 $DEFAULT: 4.0 $DESCRIPTION: "hardness"
  float latitude;           // $MIN: 0.01 $MAX: 99 $DEFAULT: 33.0
  float contrast;           // $MIN: 0 $MAX: 5 $DEFAULT: 1.18
  float saturation;         // $MIN: -200 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "extreme luminance saturation"
  float balance;            // $MIN: -50 $MAX: 50 $DEFAULT: 0.0 $DESCRIPTION: "shadows \342\206\224 highlights balance"
  float noise_level;        // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 0.05f $DESCRIPTION: "add noise in highlights"
  dt_iop_filmicrgb_methods_type_t preserve_color; // $DEFAULT: DT_FILMIC_METHOD_MAX_RGB $DESCRIPTION: "preserve chrominance"
  dt_iop_filmicrgb_colorscience_type_t version; // $DEFAULT: DT_FILMIC_COLORSCIENCE_V5 $DESCRIPTION: "color science"
  gboolean auto_hardness;                       // $DEFAULT: TRUE $DESCRIPTION: "auto adjust hardness"
  gboolean custom_grey;                         // $DEFAULT: FALSE $DESCRIPTION: "use custom middle-gray values"
  int high_quality_reconstruction;       // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations of color inpainting"
  dt_iop_filmic_noise_distribution_t noise_distribution; // $DEFAULT: DT_NOISE_POISSONIAN $DESCRIPTION: "type of noise"
  dt_iop_filmicrgb_curve_type_t shadows; // $DEFAULT: DT_FILMIC_CURVE_RATIONAL $DESCRIPTION: "contrast in shadows"
  dt_iop_filmicrgb_curve_type_t highlights; // $DEFAULT: DT_FILMIC_CURVE_RATIONAL $DESCRIPTION: "contrast in highlights"
  gboolean compensate_icc_black; // $DEFAULT: FALSE $DESCRIPTION: "compensate output ICC profile black point"
  dt_iop_filmicrgb_spline_version_type_t spline_version; // $DEFAULT: DT_FILMIC_SPLINE_VERSION_V3 $DESCRIPTION: "spline handling"
} dt_iop_filmicrgb_params_t;
// clang-format on


// custom buttons in graph views
typedef enum dt_iop_filmicrgb_gui_button_t
{
  DT_FILMIC_GUI_BUTTON_TYPE = 0,
  DT_FILMIC_GUI_BUTTON_LABELS = 1,
  DT_FILMIC_GUI_BUTTON_LAST
} dt_iop_filmicrgb_gui_button_t;

// custom buttons in graph views - data
typedef struct dt_iop_filmicrgb_gui_button_data_t
{
  // coordinates in GUI - compute them only in the drawing function
  float left;
  float right;
  float top;
  float bottom;
  float w;
  float h;

  // properties
  gint mouse_hover; // whether it should be acted on / mouse is over it
  GtkStateFlags state;

  // icon drawing, function as set in dtgtk/paint.h
  DTGTKCairoPaintIconFunc icon;

} dt_iop_filmicrgb_gui_button_data_t;


typedef struct dt_iop_filmicrgb_gui_data_t
{
  GtkWidget *white_point_source;
  GtkWidget *grey_point_source;
  GtkWidget *black_point_source;
  GtkWidget *reconstruct_threshold, *reconstruct_bloom_vs_details, *reconstruct_grey_vs_color,
      *reconstruct_structure_vs_texture, *reconstruct_feather;
  GtkWidget *show_highlight_mask;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
  GtkWidget *grey_point_target;
  GtkWidget *white_point_target;
  GtkWidget *black_point_target;
  GtkWidget *output_power;
  GtkWidget *toe;
  GtkWidget *shoulder;
  GtkWidget *contrast;
  GtkWidget *saturation;
  GtkWidget *preserve_color;
  GtkWidget *autoset_display_gamma;
  GtkWidget *shadows, *highlights;
  GtkWidget *version;
  GtkWidget *auto_hardness;
  GtkWidget *custom_grey;
  GtkWidget *high_quality_reconstruction;
  GtkWidget *noise_level, *noise_distribution;
  GtkWidget *compensate_icc_black;
  GtkNotebook *notebook;
  GtkDrawingArea *area;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
  gint show_mask;
  dt_iop_filmic_rgb_gui_mode_t gui_mode; // graph display mode
  gint gui_show_labels;
  gint gui_hover;
  gint gui_sizes_inited;
  dt_iop_filmicrgb_gui_button_t active_button; // ID of the button under cursor
  dt_iop_filmicrgb_gui_button_data_t buttons[DT_FILMIC_GUI_BUTTON_LAST];

  // Cache Pango and Cairo stuff for the equalizer drawing
  float line_height;
  float sign_width;
  float zero_width;
  float graph_width;
  float graph_height;
  int inset;

  GtkAllocation allocation;
  PangoRectangle ink;
  GtkStyleContext *context;
} dt_iop_filmicrgb_gui_data_t;

typedef struct dt_iop_filmicrgb_data_t
{
  float max_grad;
  float white_source;
  float grey_source;
  float black_source;
  float reconstruct_threshold;
  float reconstruct_feather;
  float reconstruct_bloom_vs_details;
  float reconstruct_grey_vs_color;
  float reconstruct_structure_vs_texture;
  float normalize;
  float dynamic_range;
  float saturation;
  float output_power;
  float contrast;
  float sigma_toe, sigma_shoulder;
  float noise_level;
  int preserve_color;
  int version;
  int spline_version;
  int high_quality_reconstruction;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
  dt_noise_distribution_t noise_distribution;
} dt_iop_filmicrgb_data_t;


typedef struct dt_iop_filmicrgb_global_data_t
{
  int kernel_filmic_rgb_split;
  int kernel_filmic_rgb_chroma;
  int kernel_filmic_mask;
  int kernel_filmic_show_mask;
  int kernel_filmic_inpaint_noise;
  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_bspline_vertical_local;
  int kernel_filmic_bspline_horizontal_local;
  int kernel_filmic_init_reconstruct;
  int kernel_filmic_wavelets_detail;
  int kernel_filmic_wavelets_reconstruct;
  int kernel_filmic_compute_ratios;
  int kernel_filmic_restore_ratios;
} dt_iop_filmicrgb_global_data_t;


const char *name()
{
  return _("fil_mic rgb");
}

const char *aliases()
{
  return _("tone mapping|curve|view transform|contrast|saturation|highlights");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply a view transform to prepare the scene-referred pipeline\n"
                                        "for display on SDR screens and paper prints\n"
                                        "while preventing clipping in non-destructive ways"),
                                      _("corrective and creative"),
                                      _("linear or non-linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_TONES;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
}

inline static gboolean dt_iop_filmic_rgb_compute_spline(const dt_iop_filmicrgb_params_t *const p,
                                                    struct dt_iop_filmic_rgb_spline_t *const spline);

typedef struct dt_iop_filmicrgb_v3_geometry_t
{
  float grey_log;
  float grey_display;
  float black_display;
  float white_display;
  float contrast;
  float linear_intercept;
  float xmin;
  float xmax;
  gboolean contrast_clamped;
} dt_iop_filmicrgb_v3_geometry_t;

typedef struct dt_iop_filmicrgb_v3_nodes_t
{
  float toe_log;
  float shoulder_log;
  float toe_display;
  float shoulder_display;
} dt_iop_filmicrgb_v3_nodes_t;

/**
 * Recover the affine segment used by the v3 spline generator from the user parameters.
 *
 * Toe and shoulder are defined on that affine segment, as percentages of the available
 * room between middle grey and the points where the current slope hits display black
 * and display white.
 */
static inline gboolean filmic_v3_compute_geometry(const dt_iop_filmicrgb_params_t *const p,
                                                  dt_iop_filmicrgb_v3_geometry_t *const geometry)
{
  if(p->spline_version < DT_FILMIC_SPLINE_VERSION_V3) return FALSE;

  if(p->custom_grey)
    geometry->grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f,
                                  1.0f / p->output_power);
  else
    geometry->grey_display = powf(0.1845f, 1.0f / p->output_power);

  const float dynamic_range = p->white_point_source - p->black_point_source;
  geometry->grey_log = fabsf(p->black_point_source) / dynamic_range;
  geometry->black_display = powf(CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f,
                                 1.0f / p->output_power);
  geometry->white_display = powf(fmaxf(p->white_point_target, p->grey_point_target) / 100.0f,
                                 1.0f / p->output_power);

  const float slope = p->contrast * dynamic_range / 8.0f;
  float min_contrast = 1.0f;
  min_contrast = fmaxf(min_contrast,
                       (geometry->white_display - geometry->grey_display) / (1.0f - geometry->grey_log));
  min_contrast = fmaxf(min_contrast,
                       (geometry->grey_display - geometry->black_display) / geometry->grey_log);
  min_contrast += SAFETY_MARGIN;

  geometry->contrast = slope / (p->output_power * powf(geometry->grey_display, p->output_power - 1.0f));
  const float clamped_contrast = CLAMP(geometry->contrast, min_contrast, 100.0f);
  geometry->contrast_clamped = (clamped_contrast != geometry->contrast);
  geometry->contrast = clamped_contrast;

  geometry->linear_intercept = geometry->grey_display - geometry->contrast * geometry->grey_log;
  const float safety_margin = SAFETY_MARGIN * (geometry->white_display - geometry->black_display);
  geometry->xmin = (geometry->black_display + safety_margin - geometry->linear_intercept) / geometry->contrast;
  geometry->xmax = (geometry->white_display - safety_margin - geometry->linear_intercept) / geometry->contrast;
  return TRUE;
}

/**
 * Place the v3 toe and shoulder nodes from the legacy latitude/balance controls.
 *
 * The linear mid-tones segment is first expanded to the admissible [xmin ; xmax] interval
 * for the current contrast, then latitude picks a symmetric position around middle grey.
 * Balance finally translates that pair of nodes along the slope to favor shadows or highlights.
 */
static inline gboolean filmic_v3_compute_nodes_from_legacy(const dt_iop_filmicrgb_params_t *const p,
                                                           dt_iop_filmicrgb_v3_geometry_t *const geometry,
                                                           dt_iop_filmicrgb_v3_nodes_t *const nodes)
{
  if(!filmic_v3_compute_geometry(p, geometry)) return FALSE;

  const float latitude = CLAMP(p->latitude, 0.0f, 100.0f) / 100.0f;
  const float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f;

  // Latitude positions toe and shoulder symmetrically between middle grey and the
  // points where the current affine slope would meet output black and white.
  nodes->toe_log = (1.0f - latitude) * geometry->grey_log + latitude * geometry->xmin;
  nodes->shoulder_log = (1.0f - latitude) * geometry->grey_log + latitude * geometry->xmax;

  // Balance is a signed translation of the latitude segment along the slope.
  // Positive values protect highlights, negative values protect shadows.
  const float balance_correction = (balance > 0.0f)
                                       ? 2.0f * balance * (nodes->shoulder_log - geometry->grey_log)
                                       : 2.0f * balance * (geometry->grey_log - nodes->toe_log);
  nodes->toe_log -= balance_correction;
  nodes->shoulder_log -= balance_correction;
  nodes->toe_log = fmaxf(nodes->toe_log, geometry->xmin);
  nodes->shoulder_log = fminf(nodes->shoulder_log, geometry->xmax);

  nodes->toe_display = nodes->toe_log * geometry->contrast + geometry->linear_intercept;
  nodes->shoulder_display = nodes->shoulder_log * geometry->contrast + geometry->linear_intercept;
  return TRUE;
}

static inline void filmic_v3_legacy_to_direct(const dt_iop_filmicrgb_params_t *const p,
                                              float *const toe, float *const shoulder)
{
  dt_iop_filmicrgb_v3_geometry_t geometry;
  dt_iop_filmicrgb_v3_nodes_t nodes;
  if(!filmic_v3_compute_nodes_from_legacy(p, &geometry, &nodes))
  {
    *toe = CLAMP(p->latitude, 0.0f, 100.0f);
    *shoulder = CLAMP(p->latitude, 0.0f, 100.0f);
    return;
  }

  const float toe_span = fmaxf(geometry.grey_log - geometry.xmin, 1e-6f);
  const float shoulder_span = fmaxf(geometry.xmax - geometry.grey_log, 1e-6f);
  *toe = CLAMP((geometry.grey_log - nodes.toe_log) / toe_span, 0.0f, 1.0f) * 100.0f;
  *shoulder = CLAMP((nodes.shoulder_log - geometry.grey_log) / shoulder_span, 0.0f, 1.0f) * 100.0f;
}

static inline void filmic_v3_direct_to_legacy(const dt_iop_filmicrgb_params_t *const p,
                                              const float toe, const float shoulder,
                                              float *const latitude, float *const balance)
{
  dt_iop_filmicrgb_v3_geometry_t geometry;
  if(!filmic_v3_compute_geometry(p, &geometry))
  {
    *latitude = p->latitude;
    *balance = p->balance;
    return;
  }

  const float toe_value = CLAMP(toe, 0.0f, 100.0f) / 100.0f;
  const float shoulder_value = CLAMP(shoulder, 0.0f, 100.0f) / 100.0f;
  const float toe_span = fmaxf(geometry.grey_log - geometry.xmin, 1e-6f);
  const float shoulder_span = fmaxf(geometry.xmax - geometry.grey_log, 1e-6f);
  const float latitude_value = CLAMP((toe_span * toe_value + shoulder_span * shoulder_value)
                                         / (toe_span + shoulder_span),
                                     0.0f, 1.0f);

  float balance_value = 0.0f;
  if(latitude_value > 1e-6f)
  {
    if(toe_value > shoulder_value)
      balance_value = 0.5f * (1.0f - shoulder_value / latitude_value);
    else if(shoulder_value > toe_value)
      balance_value = 0.5f * (toe_value / latitude_value - 1.0f);
  }

  *latitude = latitude_value * 100.0f;
  *balance = CLAMP(balance_value, -0.5f, 0.5f) * 100.0f;
}

// convert parameters from spline v1 or v2 to spline v3
static inline void convert_to_spline_v3(dt_iop_filmicrgb_params_t* n)
{
  if(n->spline_version == DT_FILMIC_SPLINE_VERSION_V3)
    return;

  dt_iop_filmic_rgb_spline_t spline;
  dt_iop_filmic_rgb_compute_spline(n, &spline);

  // from the spline, compute new values for contrast, balance, and latitude to update spline_version to v3
  float grey_log = spline.x[2];
  float toe_log = fminf(spline.x[1], grey_log);
  float shoulder_log = fmaxf(spline.x[3], grey_log);
  float black_display = spline.y[0];
  float grey_display = spline.y[2];
  float white_display = spline.y[4];
  const float scaled_safety_margin = SAFETY_MARGIN * (white_display - black_display);
  float toe_display = fminf(spline.y[1], grey_display);
  float shoulder_display = fmaxf(spline.y[3], grey_display);

  float hardness = n->output_power;
  float contrast = (shoulder_display - toe_display) / (shoulder_log - toe_log);
  // sanitize toe and shoulder, for min and max values, while keeping the same contrast
  float linear_intercept = grey_display - (contrast * grey_log);
  if(toe_display < black_display + scaled_safety_margin)
  {
    toe_display = black_display + scaled_safety_margin;
    // compute toe_log to keep same slope
    toe_log = (toe_display - linear_intercept) / contrast;
  }
  if(shoulder_display > white_display - scaled_safety_margin)
  {
    shoulder_display = white_display - scaled_safety_margin;
    // compute shoulder_log to keep same slope
    shoulder_log = (shoulder_display - linear_intercept) / contrast;
  }
  // revert contrast adaptation that will be performed in dt_iop_filmic_rgb_compute_spline
  contrast *= 8.0f / (n->white_point_source - n->black_point_source);
  contrast *= hardness * powf(grey_display, hardness-1.0f);
  // latitude is the % of the segment [b+safety*(w-b),w-safety*(w-b)] which is covered, where b is black_display and w white_display
  const float latitude = CLAMP((shoulder_display - toe_display) / ((white_display - black_display) - 2.0f * scaled_safety_margin), 0.0f, 0.99f);
  // find balance
  float toe_display_ref = latitude * (black_display + scaled_safety_margin) + (1.0f - latitude) * grey_display;
  float shoulder_display_ref = latitude * (white_display - scaled_safety_margin) + (1.0f - latitude) * grey_display;
  float balance;
  if(shoulder_display < shoulder_display_ref)
    balance = 0.5f * (1.0f - fmaxf(shoulder_display - grey_display, 0.0f) / fmaxf(shoulder_display_ref - grey_display, 1E-5f));
  else
    balance = -0.5f * (1.0f - fmaxf(grey_display - toe_display, 0.0f) / fmaxf(grey_display - toe_display_ref, 1E-5f));

  if(n->spline_version == DT_FILMIC_SPLINE_VERSION_V1)
  {
    // black and white point need to be updated as well,
    // as code path for v3 will raise them to power 1.0f / hardness,
    // while code path for v1 did not.
    n->black_point_target = powf(black_display, hardness) * 100.0f;
    n->white_point_target = powf(white_display, hardness) * 100.0f;
  }
  n->latitude = latitude * 100.0f;
  n->contrast = contrast;
  n->balance = balance * 100.0f;
  n->spline_version = DT_FILMIC_SPLINE_VERSION_V3;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 5)
  {
    typedef struct dt_iop_filmicrgb_params_v1_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude;
      float contrast;
      float saturation;
      float balance;
      int preserve_color;
    } dt_iop_filmicrgb_params_v1_t;

    dt_iop_filmicrgb_params_v1_t *o = (dt_iop_filmicrgb_params_v1_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = DT_FILMIC_CURVE_POLY_4;
    n->highlights = DT_FILMIC_CURVE_POLY_3;
    n->reconstruct_threshold
        = 6.0f; // for old edits, this ensures clipping threshold >> white level, so it's a no-op
    n->reconstruct_bloom_vs_details = d->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = d->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = d->reconstruct_structure_vs_texture;
    n->reconstruct_feather = 3.0f;
    n->version = DT_FILMIC_COLORSCIENCE_V1;
    n->auto_hardness = TRUE;
    n->custom_grey = TRUE;
    n->high_quality_reconstruction = 0;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = 0.f;
    n->spline_version = DT_FILMIC_SPLINE_VERSION_V1;
    n->compensate_icc_black = FALSE;
    convert_to_spline_v3(n);
    return 0;
  }
  if(old_version == 2 && new_version == 5)
  {
    typedef struct dt_iop_filmicrgb_params_v2_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float reconstruct_threshold;
      float reconstruct_feather;
      float reconstruct_bloom_vs_details;
      float reconstruct_grey_vs_color;
      float reconstruct_structure_vs_texture;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude;
      float contrast;
      float saturation;
      float balance;
      int preserve_color;
      int version;
      int auto_hardness;
      int custom_grey;
      int high_quality_reconstruction;
      dt_iop_filmicrgb_curve_type_t shadows;
      dt_iop_filmicrgb_curve_type_t highlights;
    } dt_iop_filmicrgb_params_v2_t;

    dt_iop_filmicrgb_params_v2_t *o = (dt_iop_filmicrgb_params_v2_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = o->shadows;
    n->highlights = o->highlights;
    n->reconstruct_threshold = o->reconstruct_threshold;
    n->reconstruct_bloom_vs_details = o->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = o->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = o->reconstruct_structure_vs_texture;
    n->reconstruct_feather = o->reconstruct_feather;
    n->version = o->version;
    n->auto_hardness = o->auto_hardness;
    n->custom_grey = o->custom_grey;
    n->high_quality_reconstruction = o->high_quality_reconstruction;
    n->noise_level = d->noise_level;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = 0.f;
    n->spline_version = DT_FILMIC_SPLINE_VERSION_V1;
    n->compensate_icc_black = FALSE;
    convert_to_spline_v3(n);
    return 0;
  }
  if(old_version == 3 && new_version == 5)
  {
    typedef struct dt_iop_filmicrgb_params_v3_t
    {
      float grey_point_source;     // $MIN: 0 $MAX: 100 $DEFAULT: 18.45 $DESCRIPTION: "middle gray luminance"
      float black_point_source;    // $MIN: -16 $MAX: -0.1 $DEFAULT: -8.0 $DESCRIPTION: "black relative exposure"
      float white_point_source;    // $MIN: 0 $MAX: 16 $DEFAULT: 4.0 $DESCRIPTION: "white relative exposure"
      float reconstruct_threshold; // $MIN: -6.0 $MAX: 6.0 $DEFAULT: +3.0 $DESCRIPTION: "threshold"
      float reconstruct_feather;   // $MIN: 0.25 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "transition"
      float reconstruct_bloom_vs_details; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION:
                                          // "bloom/reconstruct"
      float reconstruct_grey_vs_color;    // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "gray/colorful
                                          // details"
      float reconstruct_structure_vs_texture; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION:
                                              // "structure/texture"
      float security_factor;    // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "dynamic range scaling"
      float grey_point_target;  // $MIN: 1 $MAX: 50 $DEFAULT: 18.45 $DESCRIPTION: "target middle gray"
      float black_point_target; // $MIN: 0 $MAX: 20 $DEFAULT: 0 $DESCRIPTION: "target black luminance"
      float white_point_target; // $MIN: 0 $MAX: 1600 $DEFAULT: 100 $DESCRIPTION: "target white luminance"
      float output_power;       // $MIN: 1 $MAX: 10 $DEFAULT: 4.0 $DESCRIPTION: "hardness"
      float latitude;           // $MIN: 0.01 $MAX: 100 $DEFAULT: 33.0
      float contrast;           // $MIN: 0 $MAX: 5 $DEFAULT: 1.50
      float saturation;         // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "extreme luminance saturation"
      float balance;            // $MIN: -50 $MAX: 50 $DEFAULT: 0.0 $DESCRIPTION: "shadows/highlights balance"
      float noise_level;        // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 0.1f $DESCRIPTION: "add noise in highlights"
      dt_iop_filmicrgb_methods_type_t preserve_color; // $DEFAULT: DT_FILMIC_METHOD_POWER_NORM $DESCRIPTION:
                                                      // "preserve chrominance"
      dt_iop_filmicrgb_colorscience_type_t version;   // $DEFAULT: DT_FILMIC_COLORSCIENCE_V3 $DESCRIPTION: "color
                                                      // science"
      gboolean auto_hardness;                         // $DEFAULT: TRUE $DESCRIPTION: "auto adjust hardness"
      gboolean custom_grey;            // $DEFAULT: FALSE $DESCRIPTION: "use custom middle-gray values"
      int high_quality_reconstruction; // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations of high-quality
                                       // reconstruction"
      int noise_distribution;          // $DEFAULT: DT_NOISE_POISSONIAN $DESCRIPTION: "type of noise"
      dt_iop_filmicrgb_curve_type_t shadows; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in shadows"
      dt_iop_filmicrgb_curve_type_t highlights; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in
                                                // highlights"
    } dt_iop_filmicrgb_params_v3_t;

    dt_iop_filmicrgb_params_v3_t *o = (dt_iop_filmicrgb_params_v3_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = o->shadows;
    n->highlights = o->highlights;
    n->reconstruct_threshold = o->reconstruct_threshold;
    n->reconstruct_bloom_vs_details = o->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = o->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = o->reconstruct_structure_vs_texture;
    n->reconstruct_feather = o->reconstruct_feather;
    n->version = o->version;
    n->auto_hardness = o->auto_hardness;
    n->custom_grey = o->custom_grey;
    n->high_quality_reconstruction = o->high_quality_reconstruction;
    n->noise_level = d->noise_level;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = d->noise_level;
    n->spline_version = DT_FILMIC_SPLINE_VERSION_V1;
    n->compensate_icc_black = FALSE;
    convert_to_spline_v3(n);
    return 0;
  }
  if(old_version == 4 && new_version == 5)
  {
    typedef struct dt_iop_filmicrgb_params_v4_t
    {
      float grey_point_source;     // $MIN: 0 $MAX: 100 $DEFAULT: 18.45 $DESCRIPTION: "middle gray luminance"
      float black_point_source;    // $MIN: -16 $MAX: -0.1 $DEFAULT: -8.0 $DESCRIPTION: "black relative exposure"
      float white_point_source;    // $MIN: 0 $MAX: 16 $DEFAULT: 4.0 $DESCRIPTION: "white relative exposure"
      float reconstruct_threshold; // $MIN: -6.0 $MAX: 6.0 $DEFAULT: +3.0 $DESCRIPTION: "threshold"
      float reconstruct_feather;   // $MIN: 0.25 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "transition"
      float reconstruct_bloom_vs_details; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "bloom \342\206\224 reconstruct"
      float reconstruct_grey_vs_color; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "gray \342\206\224 colorful details"
      float reconstruct_structure_vs_texture; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "structure \342\206\224 texture"
      float security_factor;                  // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "dynamic range scaling"
      float grey_point_target;                // $MIN: 1 $MAX: 50 $DEFAULT: 18.45 $DESCRIPTION: "target middle gray"
      float black_point_target; // $MIN: 0.000 $MAX: 20.000 $DEFAULT: 0.01517634 $DESCRIPTION: "target black luminance"
      float white_point_target; // $MIN: 0 $MAX: 1600 $DEFAULT: 100 $DESCRIPTION: "target white luminance"
      float output_power;       // $MIN: 1 $MAX: 10 $DEFAULT: 4.0 $DESCRIPTION: "hardness"
      float latitude;           // $MIN: 0.01 $MAX: 99 $DEFAULT: 50.0
      float contrast;           // $MIN: 0 $MAX: 5 $DEFAULT: 1.1
      float saturation;         // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "extreme luminance saturation"
      float balance;            // $MIN: -50 $MAX: 50 $DEFAULT: 0.0 $DESCRIPTION: "shadows \342\206\224 highlights balance"
      float noise_level;        // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 0.2f $DESCRIPTION: "add noise in highlights"
      dt_iop_filmicrgb_methods_type_t preserve_color; // $DEFAULT: DT_FILMIC_METHOD_POWER_NORM $DESCRIPTION: "preserve chrominance"
      dt_iop_filmicrgb_colorscience_type_t version; // $DEFAULT: DT_FILMIC_COLORSCIENCE_V3 $DESCRIPTION: "color science"
      gboolean auto_hardness;                       // $DEFAULT: TRUE $DESCRIPTION: "auto adjust hardness"
      gboolean custom_grey;                         // $DEFAULT: FALSE $DESCRIPTION: "use custom middle-gray values"
      int high_quality_reconstruction;       // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations of high-quality reconstruction"
      dt_iop_filmic_noise_distribution_t noise_distribution; // $DEFAULT: DT_NOISE_GAUSSIAN $DESCRIPTION: "type of noise"
      dt_iop_filmicrgb_curve_type_t shadows; // $DEFAULT: DT_FILMIC_CURVE_RATIONAL $DESCRIPTION: "contrast in shadows"
      dt_iop_filmicrgb_curve_type_t highlights; // $DEFAULT: DT_FILMIC_CURVE_RATIONAL $DESCRIPTION: "contrast in highlights"
      gboolean compensate_icc_black; // $DEFAULT: FALSE $DESCRIPTION: "compensate output ICC profile black point"
      gint internal_version; // $DEFAULT: 2020 $DESCRIPTION: "version of the spline generator"
    } dt_iop_filmicrgb_params_v4_t;

    dt_iop_filmicrgb_params_v4_t *o = (dt_iop_filmicrgb_params_v4_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    *n = *(dt_iop_filmicrgb_params_t*)o; // structure didn't change except the enum instead of gint for internal_version
    // we still need to convert the internal_version (in year) to the enum
    switch(o->internal_version)
    {
      case(2019):
        n->spline_version = DT_FILMIC_SPLINE_VERSION_V1;
        break;
      case(2020):
        n->spline_version = DT_FILMIC_SPLINE_VERSION_V2;
        break;
      case(2021):
        n->spline_version = DT_FILMIC_SPLINE_VERSION_V3;
        break;
      default:
        return 1;
    }
    convert_to_spline_v3(n);
    return 0;
  }
  return 1;
}

static inline __attribute__((always_inline)) float pixel_rgb_norm_power_simd(const dt_aligned_pixel_simd_t pixel)
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  // the full norm is (R^3 + G^3 + B^3) / (R^2 + G^2 + B^2) and it should be in ]0; +infinity[

  float numerator = 0.0f;
  float denominator = 0.0f;

  for(int c = 0; c < 3; c++)
  {
    const float value = fabsf(pixel[c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / fmaxf(denominator, 1e-12f); // prevent from division-by-0 (note: (1e-6)^2 = 1e-12
}

__OMP_DECLARE_SIMD__(aligned(pixel:16))
static inline __attribute__((always_inline)) float pixel_rgb_norm_power(const dt_aligned_pixel_t pixel)
{
  return pixel_rgb_norm_power_simd(dt_load_simd_aligned(pixel));
}


static inline __attribute__((always_inline)) float
get_pixel_norm_simd(const dt_aligned_pixel_simd_t pixel, const dt_iop_filmicrgb_methods_type_t variant,
                    const dt_iop_order_iccprofile_info_t *const work_profile)
{
  // a newly added norm should satisfy the condition that it is linear with respect to grey pixels:
  // norm(R, G, B) = norm(x, x, x) = x
  // the desaturation code in chroma preservation mode relies on this assumption.
  // DT_FILMIC_METHOD_EUCLIDEAN_NORM_V1 is an exception to this and is marked as legacy.
  // DT_FILMIC_METHOD_EUCLIDEAN_NORM_V2 takes the Euclidean norm and scales it such that
  // norm(1, 1, 1) = 1.
  switch(variant)
  {
    case(DT_FILMIC_METHOD_MAX_RGB):
      return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);

    case(DT_FILMIC_METHOD_LUMINANCE):
      if(work_profile)
      {
        if(work_profile->nonlinearlut)
        {
          dt_aligned_pixel_t rgb;
          dt_store_simd_aligned(rgb, pixel);
          return dt_ioppr_get_rgb_matrix_luminance(rgb, work_profile->matrix_in, work_profile->lut_in,
                                                   work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                                   work_profile->nonlinearlut);
        }

        return work_profile->matrix_in[1][0] * pixel[0] + work_profile->matrix_in[1][1] * pixel[1]
               + work_profile->matrix_in[1][2] * pixel[2];
      }

      return pixel[0] * 0.2225045f + pixel[1] * 0.7168786f + pixel[2] * 0.0606169f;

    case(DT_FILMIC_METHOD_POWER_NORM):
      return pixel_rgb_norm_power_simd(pixel);

    case(DT_FILMIC_METHOD_EUCLIDEAN_NORM_V1):
      return sqrtf(sqf(pixel[0]) + sqf(pixel[1]) + sqf(pixel[2]));

    case(DT_FILMIC_METHOD_EUCLIDEAN_NORM_V2):
      return sqrtf(sqf(pixel[0]) + sqf(pixel[1]) + sqf(pixel[2])) * INVERSE_SQRT_3;

    default:
      if(work_profile)
      {
        if(work_profile->nonlinearlut)
        {
          dt_aligned_pixel_t rgb;
          dt_store_simd_aligned(rgb, pixel);
          return dt_ioppr_get_rgb_matrix_luminance(rgb, work_profile->matrix_in, work_profile->lut_in,
                                                   work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                                   work_profile->nonlinearlut);
        }

        return work_profile->matrix_in[1][0] * pixel[0] + work_profile->matrix_in[1][1] * pixel[1]
               + work_profile->matrix_in[1][2] * pixel[2];
      }

      return pixel[0] * 0.2225045f + pixel[1] * 0.7168786f + pixel[2] * 0.0606169f;
  }
}

__OMP_DECLARE_SIMD__(aligned(pixel:16) uniform(variant, work_profile))
static inline __attribute__((always_inline)) float
get_pixel_norm(const dt_aligned_pixel_t pixel, const dt_iop_filmicrgb_methods_type_t variant,
               const dt_iop_order_iccprofile_info_t *const work_profile)
{
  return get_pixel_norm_simd(dt_load_simd_aligned(pixel), variant, work_profile);
}

__OMP_DECLARE_SIMD__(uniform(grey, black, dynamic_range))
static inline float log_tonemapping(const float x, const float grey, const float black,
                                       const float dynamic_range)
{
  return clamp_simd((log2f(x / grey) - black) / dynamic_range);
}

__OMP_DECLARE_SIMD__(uniform(grey, black, dynamic_range))
static inline float exp_tonemapping_v2(const float x, const float grey, const float black,
                                       const float dynamic_range)
{
  // inverse of log_tonemapping
  return grey * exp2f(dynamic_range * x + black);
}


__OMP_DECLARE_SIMD__(aligned(M1, M2, M3, M4 : 16) uniform(M1, M2, M3, M4, M5, latitude_min, latitude_max))
static inline __attribute__((always_inline)) float
filmic_spline(const float x, const dt_aligned_pixel_t M1, const dt_aligned_pixel_t M2,
              const dt_aligned_pixel_t M3, const dt_aligned_pixel_t M4,
              const dt_aligned_pixel_t M5, const float latitude_min,
              const float latitude_max, const dt_iop_filmicrgb_curve_type_t type[2])
{
  // if type polynomial :
  // y = M5 * x⁴ + M4 * x³ + M3 * x² + M2 * x¹ + M1 * x⁰
  // but we rewrite it using Horner factorisation, to spare ops and enable FMA in available
  // else if type rational :
  // y = M1 * (M2 * (x - x_0)² + (x - x_0)) / (M2 * (x - x_0)² + (x - x_0) + M3)

  float result;

  if(x < latitude_min)
  {
    // toe
    if(type[0] == DT_FILMIC_CURVE_POLY_4)
    {
      // polynomial toe, 4th order
      result = M1[0] + x * (M2[0] + x * (M3[0] + x * (M4[0] + x * M5[0])));
    }
    else if(type[0] == DT_FILMIC_CURVE_POLY_3)
    {
      // polynomial toe, 3rd order
      result = M1[0] + x * (M2[0] + x * (M3[0] + x * M4[0]));
    }
    else
    {
      // rational toe
      const float xi = latitude_min - x;
      const float rat = xi * (xi * M2[0] + 1.f);
      result = M4[0] - M1[0] * rat / (rat + M3[0]);
    }
  }
  else if(x > latitude_max)
  {
    // shoulder
    if(type[1] == DT_FILMIC_CURVE_POLY_4)
    {
      // polynomial shoulder, 4th order
      result = M1[1] + x * (M2[1] + x * (M3[1] + x * (M4[1] + x * M5[1])));
    }
    else if(type[1] == DT_FILMIC_CURVE_POLY_3)
    {
      // polynomial shoulder, 3rd order
      result = M1[1] + x * (M2[1] + x * (M3[1] + x * M4[1]));
    }
    else
    {
      // rational toe
      const float xi = x - latitude_max;
      const float rat = xi * (xi * M2[1] + 1.f);
      result = M4[1] + M1[1] * rat / (rat + M3[1]);
    }
  }
  else
  {
    // latitude
    result = M1[2] + x * M2[2];
  }

  return result;
}

__OMP_DECLARE_SIMD__(uniform(sigma_toe, sigma_shoulder))
static inline __attribute__((always_inline)) float
filmic_desaturate_v1(const float x, const float sigma_toe, const float sigma_shoulder,
                     const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = expf(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = expf(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp_simd((key_toe + key_shoulder) / saturation);
}


__OMP_DECLARE_SIMD__(uniform(sigma_toe, sigma_shoulder))
static inline __attribute__((always_inline)) float
filmic_desaturate_v2(const float x, const float sigma_toe, const float sigma_shoulder,
                     const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;
  const float sat2 = 0.5f / sqrtf(saturation);
  const float key_toe = expf(-radius_toe * radius_toe / sigma_toe * sat2);
  const float key_shoulder = expf(-radius_shoulder * radius_shoulder / sigma_shoulder * sat2);

  return (saturation - (key_toe + key_shoulder) * (saturation));
}


__OMP_DECLARE_SIMD__()
static inline float linear_saturation(const float x, const float luminance, const float saturation)
{
  return luminance + saturation * (x - luminance);
}


#define MAX_NUM_SCALES 10
__DT_CLONE_TARGETS__
static inline gint mask_clipped_pixels(const float *const restrict in, float *const restrict mask,
                                       const float normalize, const float feathering, const size_t width,
                                       const size_t height, const size_t ch)
{
  /* 1. Detect if pixels are clipped and count them,
   * 2. assign them a weight in [0. ; 1.] depending on how close from clipping they are. The weights are defined
   *    by a sigmoid centered in `reconstruct_threshold` so the transition is soft and symmetrical
   */

  int clipped = 0;

  #ifdef __SSE2__
    // flush denormals to zero for masking to avoid performance penalty
    // if there are a lot of zero values in the mask
    const unsigned int oldMode = _MM_GET_FLUSH_ZERO_MODE();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  #endif
  __OMP_PARALLEL_FOR_SIMD__(aligned(mask, in:64) reduction(+:clipped))
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float pix_max = fmaxf(sqrtf(sqf(in[k]) + sqf(in[k + 1]) + sqf(in[k + 2])), 0.f);
    const float argument = -pix_max * normalize + feathering;
    const float weight = clamp_simd(1.0f / (1.0f + exp2f(argument)));
    mask[k / ch] = weight;

    // at x = 4, the sigmoid produces opacity = 5.882 %.
    // any x > 4 will produce negligible changes over the image,
    // especially since we have reduced visual sensitivity in highlights.
    // so we discard pixels for argument > 4. for they are not worth computing.
    clipped += (4.f > argument);
  }

  #ifdef __SSE2__
    _MM_SET_FLUSH_ZERO_MODE(oldMode);
  #endif

  // If clipped area is < 9 pixels, recovery is not worth the computational cost, so skip it.
  return (clipped > 9);
}
inline static void inpaint_noise(const float *const in, const float *const mask,
                                 float *const inpainted, const float noise_level, const float threshold,
                                 const dt_noise_distribution_t noise_distribution,
                                 const size_t width, const size_t height)
{
  // add statistical noise in highlights to fill-in texture
  // this creates "particules" in highlights, that will help the implicit partial derivative equation
  // solver used in wavelets reconstruction to generate texture
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      // Init random number generator
      uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);

      // get the mask value in [0 ; 1]
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const float weight = mask[idx];
      const float *const restrict pix_in = __builtin_assume_aligned(in + index, 16);
      dt_aligned_pixel_t noise = { 0.f };
      dt_aligned_pixel_t sigma = { 0.f };
      const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };

      for_each_channel(c,aligned(pix_in))
        sigma[c] = pix_in[c] * noise_level / threshold;

      // create statistical noise
      dt_noise_generator_simd(noise_distribution, pix_in, sigma, flip, state, noise);

      // add noise to input
      float *const restrict pix_out = __builtin_assume_aligned(inpainted + index, 16);
      for_each_channel(c,aligned(pix_in,pix_out))
        pix_out[c] = fmaxf(pix_in[c] * (1.0f - weight) + weight * noise[c], 0.f);
    }
}

__DT_CLONE_TARGETS__
inline static void wavelets_reconstruct_RGB(const float *const restrict HF, const float *const restrict LF,
                                            const float *const restrict texture, const float *const restrict mask,
                                            float *const restrict reconstructed, const size_t width,
                                            const size_t height, const size_t ch, const float gamma,
                                            const float gamma_comp, const float beta, const float beta_comp,
                                            const float delta, const size_t s, const size_t scales)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += 4)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float *const restrict HF_c = __builtin_assume_aligned(HF + k, 16);
    const float *const restrict LF_c = __builtin_assume_aligned(LF + k, 16);
    const float *const restrict TT_c = __builtin_assume_aligned(texture + k, 16);

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other
    // channels
    const float grey_texture = fmaxabsf(fmaxabsf(TT_c[0], TT_c[1]), TT_c[2]);

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    const float grey_details = (HF_c[0] + HF_c[1] + HF_c[2]) / 3.f;

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_HF = beta_comp * (gamma_comp * grey_details + gamma * grey_texture);

    // synthesize the min of all low-frequency RGB channels as a flat structure term for the whole pixel
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or magenta highlights.
    const float grey_residual = beta_comp * (LF_c[0] + LF_c[1] + LF_c[2]) / 3.f;
    __OMP_SIMD__(aligned(reconstructed:64) aligned(HF_c, LF_c, TT_c:16))
    for(size_t c = 0; c < 4; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual

      // synthesize interpolated/inpainted RGB channels color details and weigh them
      // this brings back some color on top of the grey_details
      const float details = (gamma_comp * HF_c[c] + gamma * TT_c[c]) * beta + grey_HF;

      // reconstruction
      const float residual = (s == scales - 1) ? (grey_residual + LF_c[c] * beta) : 0.f;
      reconstructed[k + c] += alpha * (delta * details + residual);
    }
  }
}

__DT_CLONE_TARGETS__
inline static void wavelets_reconstruct_ratios(const float *const restrict HF, const float *const restrict LF,
                                               const float *const restrict texture,
                                               const float *const restrict mask,
                                               float *const restrict reconstructed, const size_t width,
                                               const size_t height, const size_t ch, const float gamma,
                                               const float gamma_comp, const float beta, const float beta_comp,
                                               const float delta, const size_t s, const size_t scales)
{
/*
 * This is the adapted version of the RGB reconstruction
 * RGB contain high frequencies that we try to recover, so we favor them in the reconstruction.
 * The ratios represent the chromaticity in image and contain low frequencies in the absence of noise or
 * aberrations, so, here, we favor them instead.
 *
 * Consequences :
 *  1. use min of interpolated channels details instead of max, to get smoother details
 *  4. use the max of low frequency channels instead of min, to favor achromatic solution.
 *
 * Note : ratios close to 1 mean higher spectral purity (more white). Ratios close to 0 mean lower spectral purity
 * (more colorful)
 */
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += 4)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float *const restrict HF_c = __builtin_assume_aligned(HF + k, 16);
    const float *const restrict LF_c = __builtin_assume_aligned(LF + k, 16);
    const float *const restrict TT_c = __builtin_assume_aligned(texture + k, 16);

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other
    // channels
    const float grey_texture = fmaxabsf(fmaxabsf(TT_c[0], TT_c[1]), TT_c[2]);

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    const float grey_details = (HF_c[0] + HF_c[1] + HF_c[2]) / 3.f;

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_HF = (gamma_comp * grey_details + gamma * grey_texture);
    __OMP_SIMD__(aligned(reconstructed:64) aligned(HF_c, TT_c, LF_c:16) linear(k:4))
    for(size_t c = 0; c < 4; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual
      const float details = 0.5f * ((gamma_comp * HF_c[c] + gamma * TT_c[c]) + grey_HF);

      // reconstruction
      const float residual = (s == scales - 1) ? LF_c[c] : 0.f;
      reconstructed[k + c] += alpha * (delta * details + residual);
    }
  }
}


__DT_CLONE_TARGETS__
static inline void init_reconstruct(const float *const restrict in, const float *const restrict mask,
                                    float *const restrict reconstructed, const size_t width,
                                    const size_t height)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
// Note : it's a simple multiplied alpha blending where mask = alpha weight
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width; k++)
  {
    for_each_channel(c,aligned(in,mask,reconstructed))
      reconstructed[4*k + c] = fmaxf(in[4*k + c] * (1.f - mask[k]), 0.f);
  }
}


__DT_CLONE_TARGETS__
static inline void wavelets_detail_level(const float *const restrict detail, const float *const restrict LF,
                                             float *const restrict HF, float *const restrict texture,
                                             const size_t width, const size_t height, const size_t ch)
{
  __OMP_PARALLEL_FOR_SIMD__(aligned(HF, LF, detail, texture : 64) collapse(2))
  for(size_t k = 0; k < height * width; k++)
    for(size_t c = 0; c < 4; ++c) HF[4*k + c] = texture[4*k + c] = detail[4*k + c] - LF[4*k + c];
}

__DT_CLONE_TARGETS__
static int get_scales(const dt_dev_pixelpipe_t *const pipe, const dt_iop_roi_t *roi_in,
                      const dt_dev_pixelpipe_iop_t *const piece)
{
  /* How many wavelets scales do we need to compute at current zoom level ?
   * 0. To get the same preview no matter the zoom scale, the relative image coverage ratio of the filter at
   * the coarsest wavelet level should always stay constant.
   * 1. The image coverage of each B spline filter of size `BSPLINE_FSIZE` is `2^(level) * (BSPLINE_FSIZE - 1) / 2 + 1` pixels
   * 2. The coarsest level filter at full resolution should cover `1/BSPLINE_FSIZE` of the largest image dimension.
   * 3. The coarsest level filter at current zoom level should cover `scale/BSPLINE_FSIZE` of the largest image dimension.
   *
   * So we compute the level that solves 1. subject to 3. Of course, integer rounding doesn't make that 1:1
   * accurate.
   */
  const float scale = 1.0f / dt_dev_get_module_scale(pipe, roi_in);
  const size_t size = MAX(piece->buf_in.height * pipe->iscale, piece->buf_in.width * pipe->iscale);
  const int scales = floorf(log2f((2.0f * size * scale / ((BSPLINE_FSIZE - 1) * BSPLINE_FSIZE)) - 1.0f));
  return CLAMP(scales, 1, MAX_NUM_SCALES);
}


static inline int reconstruct_highlights(const dt_dev_pixelpipe_t *const pipe,
                                         const float *const restrict in, const float *const restrict mask,
                                         float *const restrict reconstructed,
                                         const dt_iop_filmicrgb_reconstruction_type_t variant, const size_t ch,
                                         const dt_iop_filmicrgb_data_t *const data, const dt_dev_pixelpipe_iop_t *piece,
                                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  int err = 0;

  // wavelets scales
  const int scales = get_scales(pipe, roi_in, piece);

  // wavelets scales buffers
  float *const restrict LF_even = dt_pixelpipe_cache_alloc_align_float_cache(ch * roi_out->width * roi_out->height, 0); // low-frequencies RGB
  float *const restrict LF_odd = dt_pixelpipe_cache_alloc_align_float_cache(ch * roi_out->width * roi_out->height, 0);  // low-frequencies RGB
  float *const restrict HF_RGB = dt_pixelpipe_cache_alloc_align_float_cache(ch * roi_out->width * roi_out->height, 0);  // high-frequencies RGB
  float *const restrict HF_grey = dt_pixelpipe_cache_alloc_align_float_cache(ch * roi_out->width * roi_out->height, 0); // high-frequencies RGB backup

  // alloc a permanent reusable buffer for intermediate computations - avoid multiple alloc/free
  float *const restrict temp = dt_pixelpipe_cache_alloc_align_float_cache(darktable.num_openmp_threads * ch * roi_out->width, 0);

  if(!LF_even || !LF_odd || !HF_RGB || !HF_grey || !temp)
  {
    err = 1;
    goto error;
  }

  // Init reconstructed with valid parts of image
  init_reconstruct(in, mask, reconstructed, roi_out->width, roi_out->height);

  // structure inpainting vs. texture duplicating weight
  const float gamma = (data->reconstruct_structure_vs_texture);
  const float gamma_comp = 1.0f - data->reconstruct_structure_vs_texture;

  // colorful vs. grey weight
  const float beta = data->reconstruct_grey_vs_color;
  const float beta_comp = 1.f - data->reconstruct_grey_vs_color;

  // bloom vs reconstruct weight
  const float delta = data->reconstruct_bloom_vs_details;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  // but simplified because we don't need the edge-aware term, so we can separate the convolution kernel
  // with a vertical and horizontal blur, which is 10 multiply-add instead of 25 by pixel.
  for(int s = 0; s < scales; ++s)
  {
    const float *restrict detail;       // buffer containing this scale's input
    float *restrict LF;                 // output buffer for the current scale
    float *restrict HF_RGB_temp;        // temp buffer for HF_RBG terms before blurring

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
      HF_RGB_temp = LF_even;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
      HF_RGB_temp = LF_odd;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
      HF_RGB_temp = LF_even;
    }

    const int mult = 1 << s; // fancy-pants C notation for 2^s with integer type, don't be afraid

    // Compute wavelets low-frequency scales
    blur_2D_Bspline(detail, LF, temp, roi_out->width, roi_out->height, mult, TRUE); // clip negatives

    // Compute wavelets high-frequency scales and save the minimum of texture over the RGB channels
    // Note : HF_RGB = detail - LF, HF_grey = max(HF_RGB)
    wavelets_detail_level(detail, LF, HF_RGB_temp, HF_grey, roi_out->width, roi_out->height, ch);

    // interpolate/blur/inpaint (same thing) the RGB high-frequency to fill holes
    blur_2D_Bspline(HF_RGB_temp, HF_RGB, temp, roi_out->width, roi_out->height, 1, FALSE);

    // Reconstruct clipped parts
    if(variant == DT_FILMIC_RECONSTRUCT_RGB)
      wavelets_reconstruct_RGB(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, ch,
                               gamma, gamma_comp, beta, beta_comp, delta, s, scales);
    else if(variant == DT_FILMIC_RECONSTRUCT_RATIOS)
      wavelets_reconstruct_ratios(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, ch,
                               gamma, gamma_comp, beta, beta_comp, delta, s, scales);
  }

error:
  dt_pixelpipe_cache_free_align(temp);
  dt_pixelpipe_cache_free_align(LF_even);
  dt_pixelpipe_cache_free_align(LF_odd);
  dt_pixelpipe_cache_free_align(HF_RGB);
  dt_pixelpipe_cache_free_align(HF_grey);
  return err;
}


__DT_CLONE_TARGETS__
static inline void filmic_split_v1(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_filmicrgb_data_t *const data,
                                   const dt_iop_filmic_rgb_spline_t spline, const size_t width,
                                   const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t temp;

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping(fmaxf(pix_in[c], NORM_MIN), data->grey_source, data->black_source,
                                   data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile)
                          ? dt_ioppr_get_rgb_matrix_luminance(temp, work_profile->matrix_in, work_profile->lut_in,
                                                              work_profile->unbounded_coeffs_in,
                                                              work_profile->lutsize, work_profile->nonlinearlut)
                          : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v1(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(
          CLAMPF(filmic_spline(linear_saturation(temp[c], lum, desaturation),
                               spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                               spline.latitude_min, spline.latitude_max, spline.type),
                 spline.y[0], spline.y[4]),
          data->output_power);
  }
}


__DT_CLONE_TARGETS__
static inline void filmic_split_v2_v3(const float *const restrict in, float *const restrict out,
                                      const dt_iop_order_iccprofile_info_t *const work_profile,
                                      const dt_iop_filmicrgb_data_t *const data,
                                      const dt_iop_filmic_rgb_spline_t spline, const size_t width,
                                      const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t temp;

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping(fmaxf(pix_in[c], NORM_MIN), data->grey_source, data->black_source,
                                   data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile)
                          ? dt_ioppr_get_rgb_matrix_luminance(temp, work_profile->matrix_in, work_profile->lut_in,
                                                              work_profile->unbounded_coeffs_in,
                                                              work_profile->lutsize, work_profile->nonlinearlut)
                          : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v2(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(
          CLAMPF(filmic_spline(linear_saturation(temp[c], lum, desaturation),
                               spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                               spline.latitude_min, spline.latitude_max, spline.type),
                 spline.y[0], spline.y[4]),
          data->output_power);
  }
}


__DT_CLONE_TARGETS__
static inline void filmic_chroma_v1(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_filmicrgb_data_t *const data,
                                    const dt_iop_filmic_rgb_spline_t spline, const int variant,
                                    const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    dt_aligned_pixel_t ratios = { 0.0f };
    float norm = fmaxf(get_pixel_norm(pix_in, variant, work_profile), NORM_MIN);

    // Save the ratios
    for_each_channel(c,aligned(pix_in))
      ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    if(min_ratios < 0.0f)
      for_each_channel(c) ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v1(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    for_each_channel(c) ratios[c] *= norm;

    const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(
                          ratios, work_profile->matrix_in, work_profile->lut_in, work_profile->unbounded_coeffs_in,
                          work_profile->lutsize, work_profile->nonlinearlut)
                                     : dt_camera_rgb_luminance(ratios);

    // Desaturate on the non-linear parts of the curve and save ratios
    for(int c = 0; c < 3; c++) ratios[c] = linear_saturation(ratios[c], lum, desaturation) / norm;

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(CLAMPF(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                     spline.latitude_min, spline.latitude_max, spline.type),
                       spline.y[0], spline.y[4]),
                data->output_power);

    // Re-apply ratios
    for_each_channel(c,aligned(pix_out)) pix_out[c] = ratios[c] * norm;
  }
}


__DT_CLONE_TARGETS__
static inline void filmic_chroma_v2_v3(const float *const restrict in, float *const restrict out,
                                       const dt_iop_order_iccprofile_info_t *const work_profile,
                                       const dt_iop_filmicrgb_data_t *const data,
                                       const dt_iop_filmic_rgb_spline_t spline, const int variant,
                                       const size_t width, const size_t height, const size_t ch,
                                       const dt_iop_filmicrgb_colorscience_type_t colorscience_version)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    float norm = fmaxf(get_pixel_norm(pix_in, variant, work_profile), NORM_MIN);

    // Save the ratios
    dt_aligned_pixel_t ratios = { 0.0f };

    for_each_channel(c,aligned(pix_in))
      ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    const int sanitize = (min_ratios < 0.0f);

    if(sanitize)
      for_each_channel(c)
        ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v2(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(CLAMPF(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                         spline.latitude_min, spline.latitude_max, spline.type),
                       spline.y[0], spline.y[4]),
                data->output_power);

    // Re-apply ratios with saturation change
    for(int c = 0; c < 3; c++) ratios[c] = fmaxf(ratios[c] + (1.0f - ratios[c]) * (1.0f - desaturation), 0.0f);

    // color science v3: normalize again after desaturation - the norm might have changed by the desaturation
    // operation.
    if(colorscience_version == DT_FILMIC_COLORSCIENCE_V3)
      norm /= fmaxf(get_pixel_norm(ratios, variant, work_profile), NORM_MIN);

    for_each_channel(c,aligned(pix_out))
      pix_out[c] = ratios[c] * norm;

    // Gamut mapping
    const float max_pix = fmaxf(fmaxf(pix_out[0], pix_out[1]), pix_out[2]);
    const int penalize = (max_pix > 1.0f);

    // Penalize the ratios by the amount of clipping
    if(penalize)
    {
      for_each_channel(c,aligned(pix_out))
      {
        ratios[c] = fmaxf(ratios[c] + (1.0f - max_pix), 0.0f);
        pix_out[c] = ratios[c] * norm;
      }
    }
  }
}


static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
pipe_RGB_to_Ych_simd(const dt_aligned_pixel_simd_t in, const dt_aligned_pixel_simd_t matrix0,
                     const dt_aligned_pixel_simd_t matrix1, const dt_aligned_pixel_simd_t matrix2)
{
  // go from pipeline RGB to CIE 2006 LMS D65
  // go from CIE LMS 2006 to Kirk/Filmlight Yrg
  // rewrite in polar coordinates
  //
  // Note that we don't explicitly store the hue angle
  // but rather just the cosine and sine of the angle.
  // This is because we don't need the hue angle anywhere
  // and this way we can avoid calculating expensive
  // trigonometric functions.
  const dt_aligned_pixel_simd_t Yrg = LMS_to_Yrg_simd(dt_mat3x4_mul_vec4(in, matrix0, matrix1, matrix2));
  const float r = Yrg[1] - 0.21902143f;
  const float g = Yrg[2] - 0.54371398f;
  const float c = dt_fast_hypotf(g, r);
  const float cos_h = c != 0.f ? r / c : 1.f;
  const float sin_h = c != 0.f ? g / c : 0.f;
  return (dt_aligned_pixel_simd_t){ Yrg[0], c, cos_h, sin_h };
}


static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
Ych_to_pipe_RGB_simd(const dt_aligned_pixel_simd_t in, const dt_aligned_pixel_simd_t matrix0,
                     const dt_aligned_pixel_simd_t matrix1, const dt_aligned_pixel_simd_t matrix2)
{
  // rewrite in cartesian coordinates
  // go from Kirk/Filmlight Yrg to CIE LMS 2006
  // go from CIE LMS 2006 to pipeline RGB
  const dt_aligned_pixel_simd_t Yrg = {
    in[0],
    in[1] * in[2] + 0.21902143f,
    in[1] * in[3] + 0.54371398f,
    0.f
  };
  return dt_mat3x4_mul_vec4(Yrg_to_LMS_simd(Yrg), matrix0, matrix1, matrix2);
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
filmic_desaturate_v4(const dt_aligned_pixel_simd_t Ych_original, dt_aligned_pixel_simd_t Ych_final,
                     const float saturation)
{
  // Note : Ych is normalized trough the LMS conversion,
  // meaning c is actually a saturation (saturation ~= chroma / brightness).
  // So copy-pasting c and h from a different Y is equivalent to
  // tonemapping with a norm, which is equivalent to doing exposure compensation :
  // it's saturation-invariant, aka chroma will get increased
  // if Y is increased, and the other way around.
  const float chroma_original = Ych_original[1] * Ych_original[0];  // c2
  float chroma_final = Ych_final[1] * Ych_final[0];                 // c1

  // fit a linear model `chroma = f(y)`:
  // `chroma = c1 + (yc - y1) * (c2 - c1) / (y2 - y1)`
  // where `(yc - y1)` is user-defined as `saturation * (y2 - y1)`
  // so `chroma = c1 + saturation * (c2 - c1)`
  // when saturation = 0, we stay at the saturation-invariant final chroma
  // when saturation > 0, we go back towards the initial chroma before tone-mapping
  // when saturation < 0, we amplify the initial -> final chroma change
  const float delta_chroma = saturation * (chroma_original - chroma_final);

  const int filmic_brightens = (Ych_final[0] > Ych_original[0]);
  const int filmic_resat = (chroma_original < chroma_final);
  const int filmic_desat = (chroma_original > chroma_final);
  const int user_resat = (saturation > 0.f);
  const int user_desat = (saturation < 0.f);

  chroma_final = (filmic_brightens && filmic_resat)
                      ? (chroma_original + chroma_final) / 2.f // force original lower sat if brightening
                  : ((user_resat && filmic_desat) || user_desat)
                      ? chroma_final + delta_chroma // allow resaturation only if filmic desaturated, allow desat anytime
                      : chroma_final;

  Ych_final[1] = fmaxf(chroma_final / Ych_final[0], 0.f);
  return Ych_final;
}

// Pipeline and ICC luminance is CIE Y 1931
// Kirk Ych/Yrg uses CIE Y 2006
// 1 CIE Y 1931 = 1.05785528 CIE Y 2006, so we need to adjust that.
// This also accounts for the CAT16 D50->D65 adaptation that has to be done
// to go from RGB to CIE LMS 2006.
// Warning: only applies to achromatic pixels.
#define CIE_Y_1931_to_CIE_Y_2006(x) (1.05785528f * (x))


static inline __attribute__((always_inline)) float
clip_chroma_white_raw(const float coeffs[3], const float target_white, const float Y,
                      const float cos_h, const float sin_h)
{
  const float denominator_Y_coeff = coeffs[0] * (0.979381443298969f * cos_h + 0.391752577319588f * sin_h)
                                    + coeffs[1] * (0.0206185567010309f * cos_h + 0.608247422680412f * sin_h)
                                    - coeffs[2] * (cos_h + sin_h);
  const float denominator_target_term = target_white * (0.68285981628866f * cos_h + 0.482137060515464f * sin_h);

  // this channel won't limit the chroma
  if(denominator_Y_coeff == 0.f) return FLT_MAX;

  // The equation for max chroma has an asymptote at this point (zero of denominator).
  // Any Y below that value won't give us sensible results for the upper bound
  // and we should consider the lower bound instead.
  const float Y_asymptote = denominator_target_term / denominator_Y_coeff;
  if(Y <= Y_asymptote) return FLT_MAX;

  // Get chroma that brings one component of target RGB to the given target_rgb value.
  // coeffs are the transformation coeffs to get one components (R, G or B) from input LMS.
  // i.e. it is a row of the LMS -> RGB transformation matrix.
  // See tools/derive_filmic_v6_gamut_mapping.py for derivation of these equations.
  const float denominator = Y * denominator_Y_coeff - denominator_target_term;
  const float numerator = -0.427506877216495f
                          * (Y * (coeffs[0] + 0.856492345150334f * coeffs[1] + 0.554995960637719f * coeffs[2])
                             - 0.988237752433297f * target_white);

  return numerator / denominator;
}


static inline __attribute__((always_inline)) float
clip_chroma_white(const float coeffs[3], const float target_white, const float Y,
                  const float cos_h, const float sin_h)
{
  // Due to slight numerical inaccuracies in color matrices,
  // the chroma clipping curves for each RGB channel may be
  // slightly at the max luminance. Thus we linearly interpolate
  // each clipping line to zero chroma near max luminance.
  const float eps = 1e-3f;
  const float max_Y = CIE_Y_1931_to_CIE_Y_2006(target_white);
  const float delta_Y = MAX(max_Y - Y, 0.f);
  float max_chroma;
  if(delta_Y < eps)
  {
    max_chroma = delta_Y / (eps * max_Y) * clip_chroma_white_raw(coeffs, target_white, (1.f - eps) * max_Y, cos_h, sin_h);
  }
  else
  {
    max_chroma = clip_chroma_white_raw(coeffs, target_white, Y, cos_h, sin_h);
  }
  return max_chroma >= 0.f ? max_chroma : FLT_MAX;
}


static inline __attribute__((always_inline)) float
clip_chroma_black(const float coeffs[3], const float cos_h, const float sin_h)
{
  // N.B. this is the same as clip_chroma_white_raw() but with target value = 0.
  // This allows eliminating some computation.

  // Get chroma that brings one component of target RGB to zero.
  // coeffs are the transformation coeffs to get one components (R, G or B) from input LMS.
  // i.e. it is a row of the LMS -> RGB transformation matrix.
  // See tools/derive_filmic_v6_gamut_mapping.py for derivation of these equations.
  const float denominator = coeffs[0] * (0.979381443298969f * cos_h + 0.391752577319588f * sin_h)
                            + coeffs[1] * (0.0206185567010309f * cos_h + 0.608247422680412f * sin_h)
                            - coeffs[2] * (cos_h + sin_h);

  // this channel won't limit the chroma
  if(denominator == 0.f) return FLT_MAX;

  const float numerator = -0.427506877216495f * (coeffs[0] + 0.856492345150334f * coeffs[1] + 0.554995960637719f * coeffs[2]);
  const float max_chroma = numerator / denominator;
  return max_chroma >= 0.f ? max_chroma : FLT_MAX;
}


static inline __attribute__((always_inline)) float
clip_chroma(const dt_colormatrix_t matrix_out, const float target_white, const float Y,
            const float cos_h, const float sin_h, const float chroma)
{
  // Note: ideally we should figure out in advance which channel is going to clip first
  // (either go negative or over maximum allowed value) and calculate chroma clipping
  // curves only for those channels. That would avoid some ambiguities
  // (what do negative chroma values mean etc.) and reduce computation. However this
  // "brute-force" approach seems to work fine for now.

  const float chroma_R_white = clip_chroma_white(matrix_out[0], target_white, Y, cos_h, sin_h);
  const float chroma_G_white = clip_chroma_white(matrix_out[1], target_white, Y, cos_h, sin_h);
  const float chroma_B_white = clip_chroma_white(matrix_out[2], target_white, Y, cos_h, sin_h);
  const float max_chroma_white = MIN(MIN(chroma_R_white, chroma_G_white), chroma_B_white);

  const float chroma_R_black = clip_chroma_black(matrix_out[0], cos_h, sin_h);
  const float chroma_G_black = clip_chroma_black(matrix_out[1], cos_h, sin_h);
  const float chroma_B_black = clip_chroma_black(matrix_out[2], cos_h, sin_h);
  const float max_chroma_black = MIN(MIN(chroma_R_black, chroma_G_black), chroma_B_black);

  return MIN(MIN(chroma, max_chroma_black), max_chroma_white);
}


static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
gamut_check_Yrg_filmic_simd(const dt_aligned_pixel_simd_t Ych)
{
  // Check if the color fits in Yrg and LMS cone space
  // clip chroma at constant hue and luminance otherwise
  const dt_aligned_pixel_simd_t Yrg = {
    Ych[0],
    Ych[1] * Ych[2] + 0.21902143f,
    Ych[1] * Ych[3] + 0.54371398f,
    0.f
  };
  float max_c = Ych[1];

  if(Yrg[1] < 0.f) max_c = fminf(-0.21902143f / Ych[2], max_c);
  if(Yrg[2] < 0.f) max_c = fminf(-0.54371398f / Ych[3], max_c);
  if(Yrg[1] + Yrg[2] > 1.f) max_c = fminf((1.f - 0.21902143f - 0.54371398f) / (Ych[2] + Ych[3]), max_c);

  return (dt_aligned_pixel_simd_t){ Ych[0], max_c, Ych[2], Ych[3] };
}


static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
gamut_check_RGB_simd(const dt_colormatrix_t matrix_out, const dt_aligned_pixel_simd_t matrix_in0,
                     const dt_aligned_pixel_simd_t matrix_in1, const dt_aligned_pixel_simd_t matrix_in2,
                     const dt_aligned_pixel_simd_t matrix_out0, const dt_aligned_pixel_simd_t matrix_out1,
                     const dt_aligned_pixel_simd_t matrix_out2, const float display_black,
                     const float display_white, const dt_aligned_pixel_simd_t Ych_in)
{
  // Heuristic: if there are negatives, calculate the amount (luminance) of white light that
  // would need to be mixed in to bring the pixel back in gamut.
  dt_aligned_pixel_simd_t RGB_brightened = Ych_to_pipe_RGB_simd(Ych_in, matrix_out0, matrix_out1, matrix_out2);
  const float min_pix = MIN(MIN(RGB_brightened[0], RGB_brightened[1]), RGB_brightened[2]);
  const float black_offset = MAX(-min_pix, 0.f);
  RGB_brightened += dt_simd_set1(black_offset);

  const dt_aligned_pixel_simd_t Ych_brightened
      = pipe_RGB_to_Ych_simd(RGB_brightened, matrix_in0, matrix_in1, matrix_in2);

  // Increase the input luminance a little by the value we calculated above.
  // Note, however, that this doesn't actually desaturate the color like mixing
  // white would do. We will next find the chroma change needed to bring the pixel
  // into gamut.
  const float Y = CLAMP((Ych_in[0] + Ych_brightened[0]) / 2.f,
                        CIE_Y_1931_to_CIE_Y_2006(display_black),
                        CIE_Y_1931_to_CIE_Y_2006(display_white));
  const float new_chroma = clip_chroma(matrix_out, display_white, Y, Ych_in[2], Ych_in[3], Ych_in[1]);

  // Go to RGB, using existing luminance and hue and the new chroma
  dt_aligned_pixel_simd_t RGB_out
      = Ych_to_pipe_RGB_simd((dt_aligned_pixel_simd_t){ Y, new_chroma, Ych_in[2], Ych_in[3] },
                             matrix_out0, matrix_out1, matrix_out2);

  // Clamp in target RGB as a final catch-all
  for_each_channel(c) RGB_out[c] = CLAMP(RGB_out[c], 0.f, display_white);
  return RGB_out;
}


static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
gamut_mapping_simd(dt_aligned_pixel_simd_t Ych_final, const dt_aligned_pixel_simd_t Ych_original,
                   const dt_colormatrix_t output_matrix, const dt_aligned_pixel_simd_t input_matrix0,
                   const dt_aligned_pixel_simd_t input_matrix1, const dt_aligned_pixel_simd_t input_matrix2,
                   const dt_aligned_pixel_simd_t output_matrix0, const dt_aligned_pixel_simd_t output_matrix1,
                   const dt_aligned_pixel_simd_t output_matrix2,
                   const dt_colormatrix_t export_output_matrix, const dt_aligned_pixel_simd_t export_input_matrix0,
                   const dt_aligned_pixel_simd_t export_input_matrix1, const dt_aligned_pixel_simd_t export_input_matrix2,
                   const dt_aligned_pixel_simd_t export_output_matrix0, const dt_aligned_pixel_simd_t export_output_matrix1,
                   const dt_aligned_pixel_simd_t export_output_matrix2, const float display_black,
                   const float display_white, const float saturation, const int use_output_profile)
{
  // Force final hue to original
  Ych_final[2] = Ych_original[2];
  Ych_final[3] = Ych_original[3];
  // Clip luminance
  Ych_final[0] = CLAMP(Ych_final[0], CIE_Y_1931_to_CIE_Y_2006(display_black),
                       CIE_Y_1931_to_CIE_Y_2006(display_white));

  // Massage chroma
  Ych_final = filmic_desaturate_v4(Ych_original, Ych_final, saturation);
  Ych_final = gamut_check_Yrg_filmic_simd(Ych_final);

  if(!use_output_profile)
  {
    // Now, it is still possible that one channel > display white because of saturation.
    // We have already clipped Y, so we know that any problem now is caused by c
    return gamut_check_RGB_simd(output_matrix, input_matrix0, input_matrix1, input_matrix2,
                                output_matrix0, output_matrix1, output_matrix2,
                                display_black, display_white, Ych_final);
  }

  // Now, it is still possible that one channel > display white because of saturation.
  // We have already clipped Y, so we know that any problem now is caused by c
  dt_aligned_pixel_simd_t pix_out
      = gamut_check_RGB_simd(export_output_matrix, export_input_matrix0, export_input_matrix1, export_input_matrix2,
                             export_output_matrix0, export_output_matrix1, export_output_matrix2,
                             display_black, display_white, Ych_final);

  // Go from export RGB to CIE LMS 2006 D65
  const dt_aligned_pixel_simd_t LMS
      = dt_mat3x4_mul_vec4(pix_out, export_input_matrix0, export_input_matrix1, export_input_matrix2);
  // Go from CIE LMS 2006 D65 to pipeline RGB D50
  return dt_mat3x4_mul_vec4(LMS, output_matrix0, output_matrix1, output_matrix2);
}


static inline __attribute__((always_inline)) int filmic_v4_prepare_matrices(dt_colormatrix_t input_matrix, dt_colormatrix_t output_matrix,
                                       dt_colormatrix_t export_input_matrix, dt_colormatrix_t export_output_matrix,
                                       const dt_iop_order_iccprofile_info_t *const work_profile,
                                       const dt_iop_order_iccprofile_info_t *const export_profile)

{
  dt_colormatrix_t temp_matrix;

  // Prepare the pipeline RGB (D50) -> XYZ D50 -> XYZ D65 -> LMS 2006 matrix
  dt_colormatrix_mul(temp_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, temp_matrix);

  // Prepare the LMS 2006 -> XYZ D65 -> XYZ D50 -> pipeline RGB matrix (D50)
  dt_colormatrix_mul(temp_matrix, XYZ_D65_to_D50_CAT16, LMS_2006_D65_to_XYZ_D65);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, temp_matrix);

  // If the pipeline output profile is supported (matrix profile), we gamut map against it
  const int use_output_profile = (!IS_NULL_PTR(export_profile));
  if(use_output_profile)
  {
    // Prepare the LMS 2006 -> XYZ D65 -> XYZ D50 -> output RGB (D50) matrix
    dt_colormatrix_mul(temp_matrix, XYZ_D65_to_D50_CAT16, LMS_2006_D65_to_XYZ_D65);
    dt_colormatrix_mul(export_output_matrix, export_profile->matrix_out, temp_matrix);

    // Prepare the output RGB (D50) -> XYZ D50 -> XYZ D65 -> LMS 2006 matrix
    dt_colormatrix_mul(temp_matrix, XYZ_D50_to_D65_CAT16, export_profile->matrix_in);
    dt_colormatrix_mul(export_input_matrix, XYZ_D65_to_LMS_2006_D65, temp_matrix);
  }

  return use_output_profile;
}

typedef struct dt_iop_filmicrgb_simd_matrices_t
{
  dt_aligned_pixel_simd_t input[3];
  dt_aligned_pixel_simd_t output[3];
  dt_aligned_pixel_simd_t export_input[3];
  dt_aligned_pixel_simd_t export_output[3];
} dt_iop_filmicrgb_simd_matrices_t;

/**
 * Prepare the transposed matrix rows used by the v4/v5 SIMD pixel path.
 *
 * The v4/v5 CPU code repeatedly applies the same four RGB/LMS matrices to every pixel.
 * We transpose them once here, then cache the 3 SIMD rows so the processing loops only
 * perform the actual vector products.
 */
static inline void filmic_prepare_simd_matrices(const dt_colormatrix_t input_matrix,
                                                const dt_colormatrix_t output_matrix,
                                                const dt_colormatrix_t export_input_matrix,
                                                const dt_colormatrix_t export_output_matrix,
                                                dt_iop_filmicrgb_simd_matrices_t *const simd_matrices)
{
  dt_colormatrix_t input_matrix_t;
  dt_colormatrix_t output_matrix_t;
  dt_colormatrix_t export_input_matrix_t;
  dt_colormatrix_t export_output_matrix_t;

  transpose_3xSSE(input_matrix, input_matrix_t);
  transpose_3xSSE(output_matrix, output_matrix_t);
  transpose_3xSSE(export_input_matrix, export_input_matrix_t);
  transpose_3xSSE(export_output_matrix, export_output_matrix_t);

  // Convert each transposed row into a vec4 once, because every pixel reuses the same rows.
  for(size_t row = 0; row < 3; row++)
  {
    simd_matrices->input[row] = dt_colormatrix_row_to_simd(input_matrix_t, row);
    simd_matrices->output[row] = dt_colormatrix_row_to_simd(output_matrix_t, row);
    simd_matrices->export_input[row] = dt_colormatrix_row_to_simd(export_input_matrix_t, row);
    simd_matrices->export_output[row] = dt_colormatrix_row_to_simd(export_output_matrix_t, row);
  }
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
norm_tone_mapping_v4_simd(const dt_aligned_pixel_simd_t pix_in,
                          const dt_iop_filmicrgb_methods_type_t type,
                          const dt_iop_order_iccprofile_info_t *const work_profile,
                          const dt_iop_filmicrgb_data_t *const data,
                          const dt_iop_filmic_rgb_spline_t spline,
                          const float norm_min, const float norm_max)
{
  // Norm must be clamped before ratios are extracted, otherwise clipped highlights
  // would inherit a wrong chroma when the scalar norm is later saturated.
  float norm = CLAMPF(get_pixel_norm_simd(pix_in, type, work_profile), norm_min, norm_max);
  // Save the ratios
  const dt_aligned_pixel_simd_t ratios = pix_in / dt_simd_set1(norm);

  // Log tone-mapping
  norm = log_tonemapping(norm, data->grey_source, data->black_source, data->dynamic_range);
  // Filmic S curve on the max RGB
  // Apply the transfer function of the display
  norm = powf(CLAMPF(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                   spline.latitude_min, spline.latitude_max, spline.type),
                     spline.y[0], spline.y[4]),
              data->output_power);

  // Restore RGB
  return ratios * dt_simd_set1(norm);
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
RGB_tone_mapping_v4_simd(const dt_aligned_pixel_simd_t pix_in, const dt_iop_filmicrgb_data_t *const data,
                         const dt_iop_filmic_rgb_spline_t spline)
{
  // Filmic S curve on RGB
  // Apply the transfer function of the display
  dt_aligned_pixel_simd_t pix_out = pix_in;
  for(size_t c = 0; c < 3; c++)
  {
    const float mapped = log_tonemapping(pix_in[c], data->grey_source, data->black_source, data->dynamic_range);
    pix_out[c] = powf(CLAMPF(filmic_spline(mapped, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                           spline.latitude_min, spline.latitude_max, spline.type),
                             0.f, spline.y[4]),
                      data->output_power);
  }

  return pix_out;
}

__DT_CLONE_TARGETS__
static inline void filmic_chroma_v4(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_order_iccprofile_info_t *const export_profile,
                                    const dt_iop_filmicrgb_data_t *const data,
                                    const dt_iop_filmic_rgb_spline_t spline, const int variant,
                                    const size_t width, const size_t height, const size_t ch,
                                    const dt_iop_filmicrgb_colorscience_type_t colorscience_version,
                                    const float display_black, const float display_white)
{
  // See colorbalancergb.c for details
  dt_colormatrix_t input_matrix;         // pipeline RGB -> LMS 2006
  dt_colormatrix_t output_matrix;        // LMS 2006 -> pipeline RGB
  dt_colormatrix_t export_input_matrix = { { 0.f } };  // output RGB -> LMS 2006
  dt_colormatrix_t export_output_matrix = { { 0.f } }; // LMS 2006 -> output RGB

  const int use_output_profile = filmic_v4_prepare_matrices(input_matrix, output_matrix, export_input_matrix,
                                                            export_output_matrix, work_profile, export_profile);
  dt_iop_filmicrgb_simd_matrices_t simd_matrices;
  filmic_prepare_simd_matrices(input_matrix, output_matrix, export_input_matrix, export_output_matrix, &simd_matrices);

  const float norm_min = exp_tonemapping_v2(0.f, data->grey_source, data->black_source, data->dynamic_range);
  const float norm_max = exp_tonemapping_v2(1.f, data->grey_source, data->black_source, data->dynamic_range);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const dt_aligned_pixel_simd_t pix_in = dt_load_simd_aligned(in + k);
    const dt_aligned_pixel_simd_t pix_out
        = norm_tone_mapping_v4_simd(pix_in, variant, work_profile, data, spline, norm_min, norm_max);

    // Keep the expensive RGB <-> LMS <-> Ych path in vector form for the whole pixel.
    const dt_aligned_pixel_simd_t Ych_original = pipe_RGB_to_Ych_simd(pix_in, simd_matrices.input[0],
                                                                      simd_matrices.input[1], simd_matrices.input[2]);
    const dt_aligned_pixel_simd_t Ych_final = pipe_RGB_to_Ych_simd(pix_out, simd_matrices.input[0],
                                                                   simd_matrices.input[1], simd_matrices.input[2]);

    dt_store_simd_nontemporal(out + k,
                              gamut_mapping_simd(Ych_final, Ych_original, output_matrix,
                                                 simd_matrices.input[0], simd_matrices.input[1], simd_matrices.input[2],
                                                 simd_matrices.output[0], simd_matrices.output[1], simd_matrices.output[2],
                                                 export_output_matrix,
                                                 simd_matrices.export_input[0], simd_matrices.export_input[1], simd_matrices.export_input[2],
                                                 simd_matrices.export_output[0], simd_matrices.export_output[1], simd_matrices.export_output[2],
                                                 display_black, display_white, data->saturation, use_output_profile));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

__DT_CLONE_TARGETS__
static inline void filmic_split_v4(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_order_iccprofile_info_t *const export_profile,
                                   const dt_iop_filmicrgb_data_t *const data,
                                   const dt_iop_filmic_rgb_spline_t spline, const int variant,
                                   const size_t width, const size_t height, const size_t ch,
                                   const dt_iop_filmicrgb_colorscience_type_t colorscience_version,
                                   const float display_black, const float display_white)

{
  // See colorbalancergb.c for details
  dt_colormatrix_t input_matrix;         // pipeline RGB -> LMS 2006
  dt_colormatrix_t output_matrix;        // LMS 2006 -> pipeline RGB
  dt_colormatrix_t export_input_matrix = { { 0.f } };  // output RGB -> LMS 2006
  dt_colormatrix_t export_output_matrix = { { 0.f } }; // LMS 2006 -> output RGB

  const int use_output_profile = filmic_v4_prepare_matrices(input_matrix, output_matrix, export_input_matrix,
                                                            export_output_matrix, work_profile, export_profile);
  dt_iop_filmicrgb_simd_matrices_t simd_matrices;
  filmic_prepare_simd_matrices(input_matrix, output_matrix, export_input_matrix, export_output_matrix, &simd_matrices);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const dt_aligned_pixel_simd_t pix_in = dt_load_simd_aligned(in + k);
    const dt_aligned_pixel_simd_t pix_out = RGB_tone_mapping_v4_simd(pix_in, data, spline);
    const dt_aligned_pixel_simd_t Ych_original = pipe_RGB_to_Ych_simd(pix_in, simd_matrices.input[0],
                                                                      simd_matrices.input[1], simd_matrices.input[2]);
    dt_aligned_pixel_simd_t Ych_final = pipe_RGB_to_Ych_simd(pix_out, simd_matrices.input[0],
                                                             simd_matrices.input[1], simd_matrices.input[2]);

    Ych_final[1] = fminf(Ych_original[1], Ych_final[1]);

    dt_store_simd_nontemporal(out + k,
                              gamut_mapping_simd(Ych_final, Ych_original, output_matrix,
                                                 simd_matrices.input[0], simd_matrices.input[1], simd_matrices.input[2],
                                                 simd_matrices.output[0], simd_matrices.output[1], simd_matrices.output[2],
                                                 export_output_matrix,
                                                 simd_matrices.export_input[0], simd_matrices.export_input[1], simd_matrices.export_input[2],
                                                 simd_matrices.export_output[0], simd_matrices.export_output[1], simd_matrices.export_output[2],
                                                 display_black, display_white, data->saturation, use_output_profile));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}


__DT_CLONE_TARGETS__
static inline void filmic_v5(const float *const restrict in, float *const restrict out,
                             const dt_iop_order_iccprofile_info_t *const work_profile,
                             const dt_iop_order_iccprofile_info_t *const export_profile,
                             const dt_iop_filmicrgb_data_t *const data,
                             const dt_iop_filmic_rgb_spline_t spline, const size_t width,
                             const size_t height, const size_t ch, const float display_black,
                             const float display_white)

{
  // See colorbalancergb.c for details
  dt_colormatrix_t input_matrix;         // pipeline RGB -> LMS 2006
  dt_colormatrix_t output_matrix;        // LMS 2006 -> pipeline RGB
  dt_colormatrix_t export_input_matrix = { { 0.f } };  // output RGB -> LMS 2006
  dt_colormatrix_t export_output_matrix = { { 0.f } }; // LMS 2006 -> output RGB

  const int use_output_profile = filmic_v4_prepare_matrices(input_matrix, output_matrix, export_input_matrix,
                                                            export_output_matrix, work_profile, export_profile);
  dt_iop_filmicrgb_simd_matrices_t simd_matrices;
  filmic_prepare_simd_matrices(input_matrix, output_matrix, export_input_matrix, export_output_matrix, &simd_matrices);

  const float norm_min = exp_tonemapping_v2(0.f, data->grey_source, data->black_source, data->dynamic_range);
  const float norm_max = exp_tonemapping_v2(1.f, data->grey_source, data->black_source, data->dynamic_range);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const dt_aligned_pixel_simd_t pix_in = dt_load_simd_aligned(in + k);
    const dt_aligned_pixel_simd_t naive_rgb = RGB_tone_mapping_v4_simd(pix_in, data, spline);
    const dt_aligned_pixel_simd_t max_rgb
        = norm_tone_mapping_v4_simd(pix_in, DT_FILMIC_METHOD_MAX_RGB, work_profile, data, spline, norm_min, norm_max);
    // Mix max RGB with naive RGB
    dt_aligned_pixel_simd_t pix_out = dt_simd_set1(0.5f + data->saturation) * max_rgb;
    pix_out = dt_simd_set1(0.5f - data->saturation) * naive_rgb + pix_out;

    // Save Ych in Kirk/Filmlight Yrg
    const dt_aligned_pixel_simd_t Ych_original = pipe_RGB_to_Ych_simd(pix_in, simd_matrices.input[0],
                                                                      simd_matrices.input[1], simd_matrices.input[2]);
    // Get final Ych in Kirk/Filmlight Yrg
    dt_aligned_pixel_simd_t Ych_final = pipe_RGB_to_Ych_simd(pix_out, simd_matrices.input[0],
                                                             simd_matrices.input[1], simd_matrices.input[2]);

    Ych_final[1] = fminf(Ych_original[1], Ych_final[1]);

    dt_store_simd_nontemporal(out + k,
                              gamut_mapping_simd(Ych_final, Ych_original, output_matrix,
                                                 simd_matrices.input[0], simd_matrices.input[1], simd_matrices.input[2],
                                                 simd_matrices.output[0], simd_matrices.output[1], simd_matrices.output[2],
                                                 export_output_matrix,
                                                 simd_matrices.export_input[0], simd_matrices.export_input[1], simd_matrices.export_input[2],
                                                 simd_matrices.export_output[0], simd_matrices.export_output[1], simd_matrices.export_output[2],
                                                 display_black, display_white, 0.f, use_output_profile));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}


__DT_CLONE_TARGETS__
static inline void display_mask(const float *const restrict mask, float *const restrict out,
                                const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width; k++)
  {
    dt_store_simd_nontemporal(out + 4 * k, dt_simd_set1(mask[k]));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}


__DT_CLONE_TARGETS__
static inline void compute_ratios(const float *const restrict in, float *const restrict norms,
                                  float *const restrict ratios,
                                  const dt_iop_order_iccprofile_info_t *const work_profile,
                                  const int variant, const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    const dt_aligned_pixel_simd_t pix_in = dt_load_simd_aligned(in + k);
    const float norm = fmaxf(get_pixel_norm_simd(pix_in, variant, work_profile), NORM_MIN);
    norms[k / 4] = norm;
    dt_store_simd_nontemporal(ratios + k, pix_in / dt_simd_set1(norm));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read the ratios
}


__DT_CLONE_TARGETS__
static inline void restore_ratios(float *const restrict ratios, const float *const restrict norms,
                                  const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < height * width; k++)
  {
    dt_aligned_pixel_simd_t ratio = dt_load_simd_aligned(ratios + 4 * k);
    const float norm = norms[k];

    for_each_channel(c,aligned(norms,ratios))
      ratio[c] = clamp_simd(ratio[c]) * norm;

    dt_store_simd_nontemporal(ratios + 4 * k, ratio);
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read the ratios
}

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const int scales = get_scales(pipe, roi_in, piece);
  const int max_filter_radius = (1 << scales);

  // in + out + 2 * tmp + 2 * LF + 2 * temp + ratios
  tiling->factor = 9.0f;
  tiling->factor_cl = 9.0f;

  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = max_filter_radius;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

__DT_CLONE_TARGETS__
int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const restrict ivoid,
             void *const restrict ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_filmicrgb_data_t *const data = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const dt_iop_order_iccprofile_info_t *const export_profile = dt_ioppr_get_pipe_output_profile_info(pipe);

  const size_t ch = 4;

  /** The log2(x) -> -INF when x -> 0
   * thus very low values (noise) will get even lower, resulting in noise negative amplification,
   * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
   * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a
   * threshold. However, at this point of the pixelpipe, the RAW levels have already been corrected and everything
   * can happen with black levels in the exposure module. So we define the threshold as the first non-null 16 bit
   * integer
   */

  float *restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;
  float *const restrict mask = dt_pixelpipe_cache_alloc_align_float((size_t)roi_out->width * roi_out->height, pipe);
  if(!mask) return 1;

  // used to adjuste noise level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(dt_dev_get_module_scale(pipe, roi_in), 1.f);

  // build a mask of clipped pixels
  const int recover_highlights = mask_clipped_pixels(in, mask, data->normalize, data->reconstruct_feather, roi_out->width, roi_out->height, 4);

  // display mask and exit
  if(self->dev->gui_attached && pipe->type == DT_DEV_PIXELPIPE_FULL && mask)
  {
    dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    if(g->show_mask)
    {
      display_mask(mask, out, roi_out->width, roi_out->height);
      dt_pixelpipe_cache_free_align(mask);
      return 0;
    }
  }

  float *const restrict reconstructed = dt_pixelpipe_cache_alloc_align_float((size_t)roi_out->width * roi_out->height * 4, pipe);
  if(recover_highlights && !reconstructed)
  {
    dt_pixelpipe_cache_free_align(mask);
    return 1;
  }

  // if fast mode is not in use
  if(recover_highlights && mask && reconstructed)
  {
    // init the blown areas with noise to create particles
    float *const restrict inpainted =  dt_pixelpipe_cache_alloc_align_float((size_t)roi_out->width * roi_out->height * 4, pipe);
    if(!inpainted)
    {
      dt_pixelpipe_cache_free_align(mask);
      dt_pixelpipe_cache_free_align(reconstructed);
      return 1;
    }
    inpaint_noise(in, mask, inpainted, data->noise_level / scale, data->reconstruct_threshold, data->noise_distribution,
                  roi_out->width, roi_out->height);

    // diffuse particles with wavelets reconstruction
    // PASS 1 on RGB channels
    const int err_1 = reconstruct_highlights(pipe, inpainted, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RGB, ch, data, piece, roi_in, roi_out);
    int err_2 = 0;

    dt_pixelpipe_cache_free_align(inpainted);

    if(err_1)
    {
      dt_pixelpipe_cache_free_align(reconstructed);
      dt_pixelpipe_cache_free_align(mask);
      return 1;
    }

    if(data->high_quality_reconstruction > 0)
    {
      float *const restrict norms = dt_pixelpipe_cache_alloc_align_float((size_t)roi_out->width * roi_out->height, pipe);
      float *const restrict ratios = dt_pixelpipe_cache_alloc_align_float((size_t)roi_out->width * roi_out->height * 4, pipe);
      if(!norms || !ratios)
      {
        dt_pixelpipe_cache_free_align(norms);
        dt_pixelpipe_cache_free_align(ratios);
        dt_pixelpipe_cache_free_align(reconstructed);
        dt_pixelpipe_cache_free_align(mask);
        return 1;
      }

      // reconstruct highlights PASS 2 on ratios
      if(norms && ratios)
      {
        for(int i = 0; i < data->high_quality_reconstruction; i++)
        {
          compute_ratios(reconstructed, norms, ratios, work_profile, DT_FILMIC_METHOD_EUCLIDEAN_NORM_V1,
                         roi_out->width, roi_out->height);
          if(reconstruct_highlights(pipe, ratios, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RATIOS, ch,
                                    data, piece, roi_in, roi_out))
          {
            err_2 = 1;
            break;
          }
          restore_ratios(reconstructed, norms, roi_out->width, roi_out->height);
        }
      }

      dt_pixelpipe_cache_free_align(norms);
      dt_pixelpipe_cache_free_align(ratios);
    }

    if(err_2)
    {
      dt_pixelpipe_cache_free_align(reconstructed);
      dt_pixelpipe_cache_free_align(mask);
      return 1;
    }

    in = reconstructed; // use reconstructed buffer as tonemapping input
  }

  dt_pixelpipe_cache_free_align(mask);

  const float white_display = powf(data->spline.y[4], data->output_power);
  const float black_display = powf(data->spline.y[0], data->output_power);

  if(data->version == DT_FILMIC_COLORSCIENCE_V5)
  {
    filmic_v5(in, out, work_profile, export_profile, data, data->spline, roi_out->width,
              roi_out->height, ch, black_display, white_display);
  }
  else
  {
    if(data->preserve_color == DT_FILMIC_METHOD_NONE)
    {
      // no chroma preservation
      if(data->version == DT_FILMIC_COLORSCIENCE_V1)
        filmic_split_v1(in, out, work_profile, data, data->spline, roi_out->width, roi_in->height);
      else if(data->version == DT_FILMIC_COLORSCIENCE_V2 || data->version == DT_FILMIC_COLORSCIENCE_V3)
        filmic_split_v2_v3(in, out, work_profile, data, data->spline, roi_out->width, roi_in->height);
      else if(data->version == DT_FILMIC_COLORSCIENCE_V4)
        filmic_split_v4(in, out, work_profile, export_profile, data, data->spline, data->preserve_color, roi_out->width,
                        roi_out->height, ch, data->version, black_display, white_display);
    }
    else
    {
      // chroma preservation
      if(data->version == DT_FILMIC_COLORSCIENCE_V1)
        filmic_chroma_v1(in, out, work_profile, data, data->spline, data->preserve_color, roi_out->width,
                        roi_out->height);
      else if(data->version == DT_FILMIC_COLORSCIENCE_V2 || data->version == DT_FILMIC_COLORSCIENCE_V3)
        filmic_chroma_v2_v3(in, out, work_profile, data, data->spline, data->preserve_color, roi_out->width,
                            roi_out->height, ch, data->version);
      else if(data->version == DT_FILMIC_COLORSCIENCE_V4)
        filmic_chroma_v4(in, out, work_profile, export_profile, data, data->spline, data->preserve_color, roi_out->width,
                        roi_out->height, ch, data->version, black_display, white_display);
    }
  }

  dt_pixelpipe_cache_free_align(reconstructed);

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

#ifdef HAVE_OPENCL
static inline cl_int reconstruct_highlights_cl(const dt_dev_pixelpipe_t *pipe, cl_mem in, cl_mem mask, cl_mem reconstructed,
                                          const dt_iop_filmicrgb_reconstruction_type_t variant, dt_iop_filmicrgb_global_data_t *const gd,
                                          const dt_iop_filmicrgb_data_t *const data, const dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in)
{
  cl_int err = -999;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  // wavelets scales
  const int scales = get_scales(pipe, roi_in, piece);

  // wavelets scales buffers
  cl_mem LF_even = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // low-frequencies RGB
  cl_mem LF_odd = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);  // low-frequencies RGB
  cl_mem HF_RGB = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);  // high-frequencies RGB
  cl_mem HF_grey = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // high-frequencies RGB backup

  // alloc a permanent reusable buffer for intermediate computations - avoid multiple alloc/free
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);;

  if(!LF_even || !LF_odd || !HF_RGB || !HF_grey || !temp)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  // Init reconstructed with valid parts of image
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_init_reconstruct, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_init_reconstruct, 1, sizeof(cl_mem), (void *)&mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_init_reconstruct, 2, sizeof(cl_mem), (void *)&reconstructed);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_init_reconstruct, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_init_reconstruct, 4, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_init_reconstruct, sizes);
  if(err != CL_SUCCESS) goto error;

  // structure inpainting vs. texture duplicating weight
  const float gamma = (data->reconstruct_structure_vs_texture);
  const float gamma_comp = 1.0f - data->reconstruct_structure_vs_texture;

  // colorful vs. grey weight
  const float beta = data->reconstruct_grey_vs_color;
  const float beta_comp = 1.f - data->reconstruct_grey_vs_color;

  // bloom vs reconstruct weight
  const float delta = data->reconstruct_bloom_vs_details;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  // but simplified because we don't need the edge-aware term, so we can separate the convolution kernel
  // with a vertical and horizontal blur, which is 10 multiply-add instead of 25 by pixel.
  for(int s = 0; s < scales; ++s)
  {
    cl_mem detail;
    cl_mem LF;

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
    }

    const int mult = 1 << s; // fancy-pants C notation for 2^s with integer type, don't be afraid

    // Compute wavelets low-frequency scales
    const int clamp_lf = 1;
    int hblocksize;
    dt_opencl_local_buffer_t hlocopt = (dt_opencl_local_buffer_t){ .xoffset = 2 * mult, .xfactor = 1,
                                                                    .yoffset = 0, .yfactor = 1,
                                                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                    .sizex = 1 << 16, .sizey = 1 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_horizontal_local, &hlocopt))
      hblocksize = hlocopt.sizex;
    else
      hblocksize = 1;

    if(hblocksize > 1)
    {
      const size_t horizontal_sizes[3] = { ROUNDUP(width, hblocksize), ROUNDUPDHT(height, devid), 1 };
      const size_t horizontal_local[3] = { hblocksize, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 0, sizeof(cl_mem), (void *)&detail);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 1, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 5, sizeof(int), (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 6,
                               (hblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_horizontal_local,
                                                   horizontal_sizes, horizontal_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 0, sizeof(cl_mem), (void *)&detail);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 1, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    }
    if(err != CL_SUCCESS) goto error;

    int vblocksize;
    dt_opencl_local_buffer_t vlocopt = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1,
                                                                    .yoffset = 2 * mult, .yfactor = 1,
                                                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                    .sizex = 1, .sizey = 1 << 16 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_vertical_local, &vlocopt))
      vblocksize = vlocopt.sizey;
    else
      vblocksize = 1;

    if(vblocksize > 1)
    {
      const size_t vertical_sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUP(height, vblocksize), 1 };
      const size_t vertical_local[3] = { 1, vblocksize, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 1, sizeof(cl_mem), (void *)&LF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 5, sizeof(int), (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 6,
                               (vblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_vertical_local,
                                                   vertical_sizes, vertical_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 1, sizeof(cl_mem), (void *)&LF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    }
    if(err != CL_SUCCESS) goto error;

    // Compute wavelets high-frequency scales and backup the maximum of texture over the RGB channels
    // Note : HF_RGB = detail - LF
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 0, sizeof(cl_mem), (void *)&detail);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 1, sizeof(cl_mem), (void *)&LF);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 2, sizeof(cl_mem), (void *)&HF_RGB);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_detail, 4, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_wavelets_detail, sizes);
    if(err != CL_SUCCESS) goto error;

    // Take a backup copy of HF_RGB in HF_grey - only HF_RGB will be blurred
    size_t origin[] = { 0, 0, 0 };
    err = dt_opencl_enqueue_copy_image(devid, HF_RGB, HF_grey, origin, origin, sizes);
    if(err != CL_SUCCESS) goto error;

    // interpolate/blur/inpaint (same thing) the RGB high-frequency to fill holes
    const int blur_size = 1;
    const int clamp_hf = 0;
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 0, sizeof(cl_mem), (void *)&HF_RGB);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 1, sizeof(cl_mem), (void *)&temp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 4, sizeof(int), (void *)&blur_size);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 5, sizeof(int), (void *)&clamp_hf);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 0, sizeof(cl_mem), (void *)&temp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 1, sizeof(cl_mem), (void *)&HF_RGB);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 4, sizeof(int), (void *)&blur_size);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 5, sizeof(int), (void *)&clamp_hf);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    if(err != CL_SUCCESS) goto error;

    // Reconstruct clipped parts
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 0, sizeof(cl_mem), (void *)&HF_RGB);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 1, sizeof(cl_mem), (void *)&LF);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 2, sizeof(cl_mem), (void *)&HF_grey);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 3, sizeof(cl_mem), (void *)&mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 4, sizeof(cl_mem), (void *)&reconstructed);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 5, sizeof(cl_mem), (void *)&reconstructed);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 6, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 7, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 8, sizeof(float), (void *)&gamma);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 9, sizeof(float), (void *)&gamma_comp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 10, sizeof(float), (void *)&beta);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 11, sizeof(float), (void *)&beta_comp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 12, sizeof(float), (void *)&delta);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 13, sizeof(int), (void *)&s);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 14, sizeof(int), (void *)&scales);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_wavelets_reconstruct, 15, sizeof(int), (void *)&variant);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_wavelets_reconstruct, sizes);
    if(err != CL_SUCCESS) goto error;
  }

error:
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF_RGB);
  dt_opencl_release_mem_object(HF_grey);
  return err;
}


int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_filmicrgb_data_t *const d = (dt_iop_filmicrgb_data_t *)piece->data;
  dt_iop_filmicrgb_global_data_t *const gd = (dt_iop_filmicrgb_global_data_t *)self->global_data;

  cl_int err = -999;

  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem in = dev_in;
  cl_mem inpainted = NULL;
  cl_mem reconstructed = NULL;
  cl_mem mask = NULL;
  cl_mem ratios = NULL;
  cl_mem norms = NULL;

  // fetch working color profile
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const dt_iop_order_iccprofile_info_t *const export_profile = dt_ioppr_get_pipe_output_profile_info(pipe);
  const int use_work_profile = (IS_NULL_PTR(work_profile)) ? 0 : 1;

  // See colorbalancergb.c for details
  dt_colormatrix_t input_matrix;         // pipeline RGB -> LMS 2006
  dt_colormatrix_t output_matrix;        // LMS 2006 -> pipeline RGB
  dt_colormatrix_t export_input_matrix;  // output RGB -> LMS 2006
  dt_colormatrix_t export_output_matrix; // LMS 2006 -> output RGB

  const int use_output_profile = filmic_v4_prepare_matrices(input_matrix, output_matrix, export_input_matrix,
                                                            export_output_matrix, work_profile, export_profile);

  const float norm_min = exp_tonemapping_v2(0.f, d->grey_source, d->black_source, d->dynamic_range);
  const float norm_max = exp_tonemapping_v2(1.f, d->grey_source, d->black_source, d->dynamic_range);

  float input_matrix_3x4[12];
  float output_matrix_3x4[12];
  pack_3xSSE_to_3x4(input_matrix, input_matrix_3x4);
  pack_3xSSE_to_3x4(output_matrix, output_matrix_3x4);

  cl_mem input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(input_matrix_3x4), input_matrix_3x4);
  cl_mem output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(output_matrix_3x4), output_matrix_3x4);
  cl_mem export_input_matrix_cl = NULL;
  cl_mem export_output_matrix_cl = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  cl_mem clipped = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  if(use_output_profile)
  {
    float export_input_matrix_3x4[12];
    float export_output_matrix_3x4[12];
    pack_3xSSE_to_3x4(export_input_matrix, export_input_matrix_3x4);
    pack_3xSSE_to_3x4(export_output_matrix, export_output_matrix_3x4);
    export_input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(export_input_matrix_3x4), export_input_matrix_3x4);
    export_output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(export_output_matrix_3x4), export_output_matrix_3x4);
  }

  // used to adjust noise level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(dt_dev_get_module_scale(pipe, roi_in), 1.f);

  uint32_t is_clipped = 0;
  clipped = dt_opencl_alloc_device_buffer(devid, sizeof(uint32_t));
  err = dt_opencl_write_buffer_to_device(devid, &is_clipped, clipped, 0, sizeof(uint32_t), CL_TRUE);
  if(err != CL_SUCCESS) goto error;

  // build a mask of clipped pixels
  mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 1, sizeof(cl_mem), (void *)&mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 4, sizeof(float), (void *)&d->normalize);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 5, sizeof(float), (void *)&d->reconstruct_feather);
  dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_mask, 6, sizeof(cl_mem), (void *)&clipped);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_mask, sizes);
  if(err != CL_SUCCESS) goto error;

  // check for clipped pixels
  err = dt_opencl_read_buffer_from_device(devid, &is_clipped, clipped, 0, sizeof(uint32_t), CL_TRUE);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(clipped);
  clipped = NULL;

  // display mask and exit
  if(self->dev->gui_attached && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    if(g->show_mask)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_show_mask, 0, sizeof(cl_mem), (void *)&mask);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_show_mask, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_show_mask, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_show_mask, 3, sizeof(int), (void *)&height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_show_mask, sizes);
      dt_opencl_release_mem_object(mask);
      dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
      return TRUE;
    }
  }

  if(is_clipped > 0)
  {
    // Inpaint noise
    const float noise_level = d->noise_level / scale;
    inpainted = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 0, sizeof(cl_mem), (void *)&in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 1, sizeof(cl_mem), (void *)&mask);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 2, sizeof(cl_mem), (void *)&inpainted);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 5, sizeof(float), (void *)&noise_level);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 6, sizeof(float), (void *)&d->reconstruct_threshold);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_inpaint_noise, 7, sizeof(float), (void *)&d->noise_distribution);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_inpaint_noise, sizes);
    if(err != CL_SUCCESS) goto error;

    // first step of highlight reconstruction in RGB
    reconstructed = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
    err = reconstruct_highlights_cl(pipe, inpainted, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RGB, gd, d, piece, roi_in);
    if(err != CL_SUCCESS) goto error;
    dt_opencl_release_mem_object(inpainted);
    inpainted = NULL;

    if(d->high_quality_reconstruction > 0)
    {
      ratios = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
      norms = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));

      if(norms && ratios)
      {
        for(int i = 0; i < d->high_quality_reconstruction; i++)
        {
          // break ratios and norms
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 0, sizeof(cl_mem), (void *)&reconstructed);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 1, sizeof(cl_mem), (void *)&norms);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 2, sizeof(cl_mem), (void *)&ratios);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 3, sizeof(int), (void *)&d->preserve_color);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 4, sizeof(int), (void *)&width);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_compute_ratios, 5, sizeof(int), (void *)&height);
          err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_compute_ratios, sizes);
          if(err != CL_SUCCESS) goto error;

          // second step of reconstruction over ratios
          err = reconstruct_highlights_cl(pipe, ratios, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RATIOS, gd, d, piece, roi_in);
          if(err != CL_SUCCESS) goto error;

          // restore ratios to RGB
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_restore_ratios, 0, sizeof(cl_mem), (void *)&reconstructed);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_restore_ratios, 1, sizeof(cl_mem), (void *)&norms);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_restore_ratios, 2, sizeof(cl_mem), (void *)&reconstructed);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_restore_ratios, 3, sizeof(int), (void *)&width);
          dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_restore_ratios, 4, sizeof(int), (void *)&height);
          err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_restore_ratios, sizes);
          if(err != CL_SUCCESS) goto error;
        }
      }

      dt_opencl_release_mem_object(ratios);
      dt_opencl_release_mem_object(norms);
      ratios = NULL;
      norms = NULL;
    }

    in = reconstructed;
  }

  dt_opencl_release_mem_object(mask); // mask is only used for highlights reconstruction.
  mask = NULL;

  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)d->spline;

  const float white_display = powf(spline.y[4], d->output_power);
  const float black_display = powf(spline.y[0], d->output_power);

  if(d->preserve_color == DT_FILMIC_METHOD_NONE && d->version != DT_FILMIC_COLORSCIENCE_V5)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 0, sizeof(cl_mem), (void *)&in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 20, sizeof(float), (void *)&d->output_power);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 21, sizeof(int), (void *)&d->version);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 22, sizeof(int), (void *)&spline.type[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 23, sizeof(int), (void *)&spline.type[1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 24, sizeof(cl_mem), (void *)&input_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 25, sizeof(cl_mem), (void *)&output_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 26, sizeof(float), (void *)&black_display);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 27, sizeof(float), (void *)&white_display);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 28, sizeof(int), (void *)&use_output_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 29, sizeof(cl_mem), (void *)&export_input_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 30, sizeof(cl_mem), (void *)&export_output_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 31, sizeof(float), (void *)&spline.y[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 32, sizeof(float), (void *)&spline.y[4]);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_split, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 0, sizeof(cl_mem), (void *)&in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 20, sizeof(float), (void *)&d->output_power);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 21, sizeof(int), (void *)&d->preserve_color);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 22, sizeof(int), (void *)&d->version);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 23, sizeof(int), (void *)&spline.type[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 24, sizeof(int), (void *)&spline.type[1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 25, sizeof(cl_mem), (void *)&input_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 26, sizeof(cl_mem), (void *)&output_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 27, sizeof(float), (void *)&black_display);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 28, sizeof(float), (void *)&white_display);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 29, sizeof(int), (void *)&use_output_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 30, sizeof(cl_mem), (void *)&export_input_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 31, sizeof(cl_mem), (void *)&export_output_matrix_cl);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 32, sizeof(float), (void *)&norm_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 33, sizeof(float), (void *)&norm_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 34, sizeof(float), (void *)&spline.y[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 35, sizeof(float), (void *)&spline.y[4]);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_chroma, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(reconstructed);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(export_input_matrix_cl);
  dt_opencl_release_mem_object(export_output_matrix_cl);
  return TRUE;

error:
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(reconstructed);
  dt_opencl_release_mem_object(inpainted);
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(ratios);
  dt_opencl_release_mem_object(norms);
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(export_input_matrix_cl);
  dt_opencl_release_mem_object(export_output_matrix_cl);
  dt_opencl_release_mem_object(clipped);
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmicrgb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static void apply_auto_grey(dt_iop_module_t *self,
                            const dt_iop_order_iccprofile_info_t *const work_profile)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;

  const float prev_grey = p->grey_point_source;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  const float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self,
                             const dt_iop_order_iccprofile_info_t *const work_profile)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // Black
  const float black = get_pixel_norm(self->picked_color_min, DT_FILMIC_METHOD_MAX_RGB, work_profile);

  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}


static void apply_auto_white_point_source(dt_iop_module_t *self,
                                          const dt_iop_order_iccprofile_info_t *const work_profile)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // White
  const float white = get_pixel_norm(self->picked_color_max, DT_FILMIC_METHOD_MAX_RGB, work_profile);

  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void apply_autotune(dt_iop_module_t *self,
                           const dt_iop_order_iccprofile_info_t *const work_profile)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;

  // Grey
  if(p->custom_grey)
  {
    const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;
    p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  }

  // White
  const float white = get_pixel_norm(self->picked_color_max, DT_FILMIC_METHOD_MAX_RGB, work_profile);
  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  // Black
  const float black = get_pixel_norm(self->picked_color_min, DT_FILMIC_METHOD_MAX_RGB, work_profile);
  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->white_point_source = EVmax;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)piece;
  dt_print(DT_DEBUG_DEV, "[picker/filmicrgb] apply picker=%p pipe=%p min=%g max=%g avg=%g\n",
           (void *)picker, (void *)pipe,
           self->picked_color_min[0], self->picked_color_max[0], self->picked_color[0]);
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  const dt_iop_order_iccprofile_info_t *const work_profile
      = pipe ? dt_ioppr_get_pipe_current_profile_info(self, pipe)
             : dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  if(picker == g->grey_point_source)
    apply_auto_grey(self, work_profile);
  else if(picker == g->black_point_source)
    apply_auto_black(self, work_profile);
  else if(picker == g->white_point_source)
    apply_auto_white_point_source(self, work_profile);
  else if(picker == g->auto_button)
    apply_autotune(self, work_profile);
}

static void show_mask_callback(GtkToggleButton *button, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    // if blend module is displaying mask do not display it here
  if(self->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

  g->show_mask = !(g->show_mask);

  if(g->show_mask)
    self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_highlight_mask), !g->show_mask);
  dt_iop_set_cache_bypass(self, g->show_mask);
  dt_dev_pixelpipe_update_history_main(self->dev);
}

#define ORDER_4 5
#define ORDER_3 4

// returns true if contrast was clamped, false otherwise
// used in GUI, to show user when contrast clamping is happening
inline static gboolean dt_iop_filmic_rgb_compute_spline(const dt_iop_filmicrgb_params_t *const p,
                                                    struct dt_iop_filmic_rgb_spline_t *const spline)
{
  float grey_display = 0.4638f;
  gboolean clamping = FALSE;

  if(p->custom_grey)
  {
    // user set a custom value
    grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f,
                        1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  float black_display, white_display;

  if(p->spline_version == DT_FILMIC_SPLINE_VERSION_V1)
  {
    // this is a buggy version that doesn't take the output power function into account
    // it was silent because black and white display were set to 0 and 1 and users were advised to not touch them.
    // (since 0^x = 0 and 1^x = 1). It's not silent anymore if black display > 0,
    // for example if compensating ICC black level for target medium
    black_display = CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f; // in %
    white_display = fmaxf(p->white_point_target, p->grey_point_target) / 100.0f;       // in %
  }
  else //(p->spline_version >= DT_FILMIC_SPLINE_VERSION_V2)
  {
    // this is the fixed version
    black_display = powf(CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f,
                         1.0f / (p->output_power)); // in %
    white_display
        = powf(fmaxf(p->white_point_target, p->grey_point_target) / 100.0f, 1.0f / (p->output_power)); // in %
  }

  float toe_log, shoulder_log, toe_display, shoulder_display, contrast;
  float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f; // in %
  if(p->spline_version < DT_FILMIC_SPLINE_VERSION_V3)
  {
    float latitude = CLAMP(p->latitude, 0.0f, 100.0f) / 100.0f * dynamic_range; // in % of dynamic range
    contrast = CLAMP(p->contrast, 1.00001f, 6.0f);

    // nodes for mapping from log encoding to desired target luminance
    // X coordinates
    toe_log = grey_log - latitude / dynamic_range * fabsf(black_source / dynamic_range);
    shoulder_log = grey_log + latitude / dynamic_range * fabsf(white_source / dynamic_range);

    // interception
    float linear_intercept = grey_display - (contrast * grey_log);

    // y coordinates
    toe_display = (toe_log * contrast + linear_intercept);
    shoulder_display = (shoulder_log * contrast + linear_intercept);

    // Apply the highlights/shadows balance as a shift along the contrast slope
    const float norm = sqrtf(contrast * contrast + 1.0f);

    // negative values drag to the left and compress the shadows, on the UI negative is the inverse
    const float coeff = -((2.0f * latitude) / dynamic_range) * balance;

    toe_display += coeff * contrast / norm;
    shoulder_display += coeff * contrast / norm;
    toe_log += coeff / norm;
    shoulder_log += coeff / norm;
  }
  else // p->spline_version >= DT_FILMIC_SPLINE_VERSION_V3. Slope dependent on contrast only, and latitude as % of display range.
  {
    dt_iop_filmicrgb_v3_geometry_t geometry;
    dt_iop_filmicrgb_v3_nodes_t nodes;
    filmic_v3_compute_nodes_from_legacy(p, &geometry, &nodes);
    clamping = geometry.contrast_clamped;
    contrast = geometry.contrast;
    toe_log = nodes.toe_log;
    shoulder_log = nodes.shoulder_log;
    toe_display = nodes.toe_display;
    shoulder_display = nodes.shoulder_display;
  }

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
   **/

  // Build the curve from the nodes
  spline->x[0] = black_log;
  spline->x[1] = toe_log;
  spline->x[2] = grey_log;
  spline->x[3] = shoulder_log;
  spline->x[4] = white_log;

  spline->y[0] = black_display;
  spline->y[1] = toe_display;
  spline->y[2] = grey_display;
  spline->y[3] = shoulder_display;
  spline->y[4] = white_display;

  spline->latitude_min = spline->x[1];
  spline->latitude_max = spline->x[3];

  spline->type[0] = p->shadows;
  spline->type[1] = p->highlights;

  /**
   * For background and details, see :
   * https://eng.aurelienpierre.com/2018/11/30/filmic-darktable-and-the-quest-of-the-hdr-tone-mapping/#filmic_s_curve
   *
   **/
  const double Tl = spline->x[1];
  const double Tl2 = Tl * Tl;
  const double Tl3 = Tl2 * Tl;
  const double Tl4 = Tl3 * Tl;

  const double Sl = spline->x[3];
  const double Sl2 = Sl * Sl;
  const double Sl3 = Sl2 * Sl;
  const double Sl4 = Sl3 * Sl;

  // if type polynomial :
  // y = M5 * x⁴ + M4 * x³ + M3 * x² + M2 * x¹ + M1 * x⁰
  // else if type rational :
  // y = M1 * (M2 * (x - x_0)² + (x - x_0)) / (M2 * (x - x_0)² + (x - x_0) + M3)
  // We then compute M1 to M5 coeffs using the imposed conditions over the curve.
  // M1 to M5 are 3x1 vectors, where each element belongs to a part of the curve.

  // solve the linear central part - affine function
  spline->M2[2] = contrast;                                    // * x¹ (slope)
  spline->M1[2] = spline->y[1] - spline->M2[2] * spline->x[1]; // * x⁰ (offset)
  spline->M3[2] = 0.f;                                         // * x²
  spline->M4[2] = 0.f;                                         // * x³
  spline->M5[2] = 0.f;                                         // * x⁴

  // solve the toe part
  if(p->shadows == DT_FILMIC_CURVE_POLY_4)
  {
    // fourth order polynom - only mode in darktable 3.0.0
    double A0[ORDER_4 * ORDER_4] = { 0.,        0.,       0.,      0., 1.,   // position in 0
                                     0.,        0.,       0.,      1., 0.,   // first derivative in 0
                                     Tl4,       Tl3,      Tl2,     Tl, 1.,   // position at toe node
                                     4. * Tl3,  3. * Tl2, 2. * Tl, 1., 0.,   // first derivative at toe node
                                     12. * Tl2, 6. * Tl,  2.,      0., 0. }; // second derivative at toe node

    double b0[ORDER_4] = { spline->y[0], 0., spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_4);

    spline->M5[0] = b0[0]; // * x⁴
    spline->M4[0] = b0[1]; // * x³
    spline->M3[0] = b0[2]; // * x²
    spline->M2[0] = b0[3]; // * x¹
    spline->M1[0] = b0[4]; // * x⁰
  }
  else if(p->shadows == DT_FILMIC_CURVE_POLY_3)
  {
    // third order polynom
    double A0[ORDER_3 * ORDER_3] = { 0.,       0.,      0., 1.,   // position in 0
                                     Tl3,      Tl2,     Tl, 1.,   // position at toe node
                                     3. * Tl2, 2. * Tl, 1., 0.,   // first derivative at toe node
                                     6. * Tl,  2.,      0., 0. }; // second derivative at toe node

    double b0[ORDER_3] = { spline->y[0], spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_3);

    spline->M5[0] = 0.0f;  // * x⁴
    spline->M4[0] = b0[0]; // * x³
    spline->M3[0] = b0[1]; // * x²
    spline->M2[0] = b0[2]; // * x¹
    spline->M1[0] = b0[3]; // * x⁰
  }
  else
  {
    const float P1[2] = { black_log, black_display };
    const float P0[2] = { toe_log, toe_display };
    const float x = P0[0] - P1[0];
    const float y = P0[1] - P1[1];
    const float g = contrast;
    const float b = g / (2.f * y) + (sqrtf(sqf(x * g / y + 1.f) - 4.f) - 1.f) / (2.f * x);
    const float c = y / g * (b * sqf(x) + x) / (b * sqf(x) + x - (y / g));
    const float a = c * g;
    spline->M1[0] = a;
    spline->M2[0] = b;
    spline->M3[0] = c;
    spline->M4[0] = toe_display;
  }

  // solve the shoulder part
  if(p->highlights == DT_FILMIC_CURVE_POLY_3)
  {
    // 3rd order polynom - only mode in darktable 3.0.0
    double A1[ORDER_3 * ORDER_3] = { 1.,       1.,      1., 1.,   // position in 1
                                     Sl3,      Sl2,     Sl, 1.,   // position at shoulder node
                                     3. * Sl2, 2. * Sl, 1., 0.,   // first derivative at shoulder node
                                     6. * Sl,  2.,      0., 0. }; // second derivative at shoulder node

    double b1[ORDER_3] = { spline->y[4], spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_3);

    spline->M5[1] = 0.0f;  // * x⁴
    spline->M4[1] = b1[0]; // * x³
    spline->M3[1] = b1[1]; // * x²
    spline->M2[1] = b1[2]; // * x¹
    spline->M1[1] = b1[3]; // * x⁰
  }
  else if(p->highlights == DT_FILMIC_CURVE_POLY_4)
  {
    // 4th order polynom
    double A1[ORDER_4 * ORDER_4] = { 1.,        1.,       1.,      1., 1.,   // position in 1
                                     4.,        3.,       2.,      1., 0.,   // first derivative in 1
                                     Sl4,       Sl3,      Sl2,     Sl, 1.,   // position at shoulder node
                                     4. * Sl3,  3. * Sl2, 2. * Sl, 1., 0.,   // first derivative at shoulder node
                                     12. * Sl2, 6. * Sl,  2.,      0., 0. }; // second derivative at shoulder node

    double b1[ORDER_4] = { spline->y[4], 0., spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_4);

    spline->M5[1] = b1[0]; // * x⁴
    spline->M4[1] = b1[1]; // * x³
    spline->M3[1] = b1[2]; // * x²
    spline->M2[1] = b1[3]; // * x¹
    spline->M1[1] = b1[4]; // * x⁰
  }
  else
  {
    const float P1[2] = { white_log, white_display };
    const float P0[2] = { shoulder_log, shoulder_display };
    const float x = P1[0] - P0[0];
    const float y = P1[1] - P0[1];
    const float g = contrast;
    const float b = g / (2.f * y) + (sqrtf(sqf(x * g / y + 1.f) - 4.f) - 1.f) / (2.f * x);
    const float c = y / g * (b * sqf(x) + x) / (b * sqf(x) + x - (y / g));
    const float a = c * g;
    spline->M1[1] = a;
    spline->M2[1] = b;
    spline->M3[1] = c;
    spline->M4[1] = shoulder_display;
  }
  return clamping;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)p1;
  dt_iop_filmicrgb_data_t *d = (dt_iop_filmicrgb_data_t *)piece->data;

  // source and display greys
  float grey_source = 0.1845f, grey_display = 0.4638f;
  if(p->custom_grey)
  {
    // user set a custom value
    grey_source = p->grey_point_source / 100.0f; // in %
    grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_source = 0.1845f; // in %
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  // source luminance - Used only in the log encoding
  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;


  float contrast = p->contrast;
  if((p->spline_version < DT_FILMIC_SPLINE_VERSION_V3) && (contrast < grey_display / grey_log))
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    // this clamping is handled automatically for spline_version >= DT_FILMIC_SPLINE_VERSION_V3
    contrast = 1.0001f * grey_display / grey_log;
  }

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->contrast = contrast;
  d->version = p->version;
  d->spline_version = p->spline_version;
  d->preserve_color = p->preserve_color;
  d->high_quality_reconstruction = p->high_quality_reconstruction;
  d->noise_level = p->noise_level;
  d->noise_distribution = (dt_noise_distribution_t)p->noise_distribution;

  // compute the curves and their LUT
  dt_iop_filmic_rgb_compute_spline(p, &d->spline);

  if(p->version >= DT_FILMIC_COLORSCIENCE_V4)
    d->saturation = p->saturation / 100.0f;
  else
    d->saturation = (2.0f * p->saturation / 100.0f + 1.0f);

  d->sigma_toe = powf(d->spline.latitude_min / 3.0f, 2.0f);
  d->sigma_shoulder = powf((1.0f - d->spline.latitude_max) / 3.0f, 2.0f);

  d->reconstruct_threshold = powf(2.0f, white_source + p->reconstruct_threshold) * grey_source;
  d->reconstruct_feather = exp2f(12.f / p->reconstruct_feather);

  // offset and rescale user param to alpha blending so 0 -> 50% and 1 -> 100%
  d->normalize = d->reconstruct_feather / d->reconstruct_threshold;
  d->reconstruct_structure_vs_texture = (p->reconstruct_structure_vs_texture / 100.0f + 1.f) / 2.f;
  d->reconstruct_bloom_vs_details = (p->reconstruct_bloom_vs_details / 100.0f + 1.f) / 2.f;
  d->reconstruct_grey_vs_color = (p->reconstruct_grey_vs_color / 100.0f + 1.f) / 2.f;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(!in)
  {
    // lost focus - hide the mask
    gint mask_was_shown = g->show_mask;
    g->show_mask = FALSE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_highlight_mask), FALSE);
    if(mask_was_shown) dt_dev_pixelpipe_update_history_main(self->dev);
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_filmicrgb_data_t));
  piece->data_size = sizeof(dt_iop_filmicrgb_data_t);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void filmic_gui_sync_toe_shoulder(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  float toe = 0.0f;
  float shoulder = 0.0f;
  filmic_v3_legacy_to_direct(p, &toe, &shoulder);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->toe, toe);
  dt_bauhaus_slider_set(g->shoulder, shoulder);
  --darktable.gui->reset;
}

static void toe_shoulder_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  filmic_v3_direct_to_legacy(p, dt_bauhaus_slider_get(g->toe), dt_bauhaus_slider_get(g->shoulder),
                             &p->latitude, &p->balance);
  gui_changed(self, slider, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  g->show_mask = FALSE;
  g->gui_mode = dt_conf_get_int("plugins/darkroom/filmicrgb/graph_view");
  g->gui_show_labels = dt_conf_get_int("plugins/darkroom/filmicrgb/graph_show_labels");
  g->gui_hover = FALSE;
  g->gui_sizes_inited = FALSE;

  // fetch last view in dartablerc

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_hardness), p->auto_hardness);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->custom_grey), p->custom_grey);
  filmic_gui_sync_toe_shoulder(self);

  gui_changed(self, NULL, NULL);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_filmicrgb_params_t *d = module->default_params;

  d->black_point_source = module->so->get_f("black_point_source")->Float.Default;
  d->white_point_source = module->so->get_f("white_point_source")->Float.Default;
  d->output_power = module->so->get_f("output_power")->Float.Default;

  if(dt_image_is_raw(&module->dev->image_storage))
  {
    // For scene-referred workflow, auto-enable and adjust based on exposure
    // TODO: fetch actual exposure in module, don't assume 1.
    const float exposure = 0.7f - dt_image_get_exposure_bias(&module->dev->image_storage);

    // As global exposure increases, white exposure increases faster than black
    // this is probably because raw black/white points offsets the lower bound of the dynamic range to 0
    // so exposure compensation actually increases the dynamic range too (stretches only white).
    d->white_point_source = exposure + 2.45f;
    d->black_point_source = d->white_point_source - 12.f; // 12 EV of dynamic range is a good default for modern cameras
    d->output_power = logf(d->grey_point_target / 100.0f)
                      / logf(-d->black_point_source / (d->white_point_source - d->black_point_source));

    module->workflow_enabled = TRUE;
  }
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmicrgb_global_data_t *gd
      = (dt_iop_filmicrgb_global_data_t *)malloc(sizeof(dt_iop_filmicrgb_global_data_t));

  module->data = gd;
  gd->kernel_filmic_rgb_split = dt_opencl_create_kernel(program, "filmicrgb_split");
  gd->kernel_filmic_rgb_chroma = dt_opencl_create_kernel(program, "filmicrgb_chroma");
  gd->kernel_filmic_mask = dt_opencl_create_kernel(program, "filmic_mask_clipped_pixels");
  gd->kernel_filmic_show_mask = dt_opencl_create_kernel(program, "filmic_show_mask");
  gd->kernel_filmic_inpaint_noise = dt_opencl_create_kernel(program, "filmic_inpaint_noise");
  gd->kernel_filmic_init_reconstruct = dt_opencl_create_kernel(program, "init_reconstruct");
  gd->kernel_filmic_wavelets_reconstruct = dt_opencl_create_kernel(program, "wavelets_reconstruct");
  gd->kernel_filmic_compute_ratios = dt_opencl_create_kernel(program, "compute_ratios");
  gd->kernel_filmic_restore_ratios = dt_opencl_create_kernel(program, "restore_ratios");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_bspline_horizontal_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal_local");
  gd->kernel_filmic_bspline_vertical_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical_local");
  gd->kernel_filmic_wavelets_detail = dt_opencl_create_kernel(wavelets, "wavelets_detail_level");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_filmicrgb_global_data_t *gd = (dt_iop_filmicrgb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_split);
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_chroma);
  dt_opencl_free_kernel(gd->kernel_filmic_mask);
  dt_opencl_free_kernel(gd->kernel_filmic_show_mask);
  dt_opencl_free_kernel(gd->kernel_filmic_inpaint_noise);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical_local);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal_local);
  dt_opencl_free_kernel(gd->kernel_filmic_init_reconstruct);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_reconstruct);
  dt_opencl_free_kernel(gd->kernel_filmic_compute_ratios);
  dt_opencl_free_kernel(gd->kernel_filmic_restore_ratios);
  dt_free(module->data);
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

#define LOGBASE 20.f

static inline void dt_cairo_draw_arrow(cairo_t *cr, double origin_x, double origin_y, double destination_x,
                                       double destination_y, gboolean show_head)
{
  cairo_move_to(cr, origin_x, origin_y);
  cairo_line_to(cr, destination_x, destination_y);
  cairo_stroke(cr);

  if(show_head)
  {
    // arrow head is hard set to 45° - convert to radians
    const float angle_arrow = 45.f / 360.f * M_PI;
    const float angle_trunk = atan2f((destination_y - origin_y), (destination_x - origin_x));
    const float radius = DT_PIXEL_APPLY_DPI(3);

    const float x_1 = destination_x + radius / sinf(angle_arrow + angle_trunk);
    const float y_1 = destination_y + radius / cosf(angle_arrow + angle_trunk);

    const float x_2 = destination_x - radius / sinf(-angle_arrow + angle_trunk);
    const float y_2 = destination_y - radius / cosf(-angle_arrow + angle_trunk);

    cairo_move_to(cr, x_1, y_1);
    cairo_line_to(cr, destination_x, destination_y);
    cairo_line_to(cr, x_2, y_2);
    cairo_stroke(cr);
  }
}

void filmic_gui_draw_icon(cairo_t *cr, struct dt_iop_filmicrgb_gui_button_data_t *button,
                          struct dt_iop_filmicrgb_gui_data_t *g)
{
  if(!g->gui_sizes_inited) return;

  cairo_save(cr);

  GdkRGBA color;

  // copy color
  color.red = darktable.bauhaus->graph_fg.red;
  color.green = darktable.bauhaus->graph_fg.green;
  color.blue = darktable.bauhaus->graph_fg.blue;
  color.alpha = darktable.bauhaus->graph_fg.alpha;

  if(button->mouse_hover)
  {
    // use graph_fg color as-is if mouse hover
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
  }
  else
  {
    // use graph_fg color with transparency else
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha * 0.5);
  }

  cairo_rectangle(cr, button->left, button->top, button->w - DT_PIXEL_APPLY_DPI(0.5),
                  button->h - DT_PIXEL_APPLY_DPI(0.5));
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_stroke(cr);
  cairo_translate(cr, button->left + button->w / 2. - DT_PIXEL_APPLY_DPI(0.25),
                  button->top + button->h / 2. - DT_PIXEL_APPLY_DPI(0.25));

  const float scale = 0.85;
  cairo_scale(cr, scale, scale);
  button->icon(cr, -scale * button->w / 2., -scale * button->h / 2., scale * button->w, scale * button->h, 0, NULL);
  cairo_restore(cr);
}


static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  gboolean contrast_clamped = dt_iop_filmic_rgb_compute_spline(p, &g->spline);

  // Cache the graph objects to avoid recomputing all the view at each redraw
  gtk_widget_get_allocation(widget, &g->allocation);

  cairo_surface_t *cst =
    dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g->allocation.width, g->allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  g->context = gtk_widget_get_style_context(widget);

  char text[256];

  // reduce a bit the font size
  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);

  // Get the text line height for spacing
  g_strlcpy(text, "X", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  g->line_height = g->ink.height;

  // Get the width of a minus sign for legend labels spacing
  g_strlcpy(text, "-", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  g->sign_width = g->ink.width / 2.0;

  // Get the width of a zero for legend labels spacing
  g_strlcpy(text, "0", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  g->zero_width = g->ink.width;

  // Set the sizes, margins and paddings
  g->inset = INNER_PADDING;

  float margin_left;
  float margin_bottom;
  if(g->gui_show_labels)
  {
    // leave room for labels
    margin_left = 3. * g->zero_width + 2. * g->inset;
    margin_bottom = 2. * g->line_height + 4. * g->inset;
  }
  else
  {
    margin_left = g->inset;
    margin_bottom = g->inset;
  }

  const float margin_top = 2. * g->line_height + g->inset;
  const float margin_right = darktable.bauhaus->quad_width + 2. * g->inset;

  g->graph_width = g->allocation.width - margin_right - margin_left;   // align the right border on sliders
  g->graph_height = g->allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(g->context, cr, 0, 0, g->allocation.width, g->allocation.height);

  // Init icons bounds and cache them for mouse events
  for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++)
  {
    // put the buttons in the right margin and increment vertical position
    g->buttons[i].right = g->allocation.width;
    g->buttons[i].left = g->buttons[i].right - darktable.bauhaus->quad_width;
    g->buttons[i].top = margin_top + i * (g->inset + darktable.bauhaus->quad_width);
    g->buttons[i].bottom = g->buttons[i].top + darktable.bauhaus->quad_width;
    g->buttons[i].w = g->buttons[i].right - g->buttons[i].left;
    g->buttons[i].h = g->buttons[i].bottom - g->buttons[i].top;
    g->buttons[i].state = GTK_STATE_FLAG_NORMAL;
  }

  g->gui_sizes_inited = TRUE;

  g->buttons[0].icon = dtgtk_cairo_paint_refresh;
  g->buttons[1].icon = dtgtk_cairo_paint_text_label;

  if(g->gui_hover)
  {
    for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++) filmic_gui_draw_icon(cr, &g->buttons[i], g);
  }

  const float grey = p->grey_point_source / 100.f;
  const float DR = p->white_point_source - p->black_point_source;

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // write the graph legend at GUI default size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);
  if(g->gui_mode == DT_FILMIC_GUI_LOOK)
    g_strlcpy(text, _("look only"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
    g_strlcpy(text, _("look + mapping (lin)"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
    g_strlcpy(text, _("look + mapping (log)"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_RANGES)
    g_strlcpy(text, _("dynamic range mapping"), sizeof(text));

  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);

  // legend background
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, g->allocation.width - margin_left - g->ink.width - g->ink.x - 2. * g->inset,
                  -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y - g->inset,
                  g->ink.width + 3. * g->inset, g->ink.height + 2. * g->inset);
  cairo_fill(cr);

  // legend text
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_move_to(cr, g->allocation.width - margin_left - g->ink.width - g->ink.x - g->inset,
                -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y);
  pango_cairo_show_layout(cr, layout);
  cairo_stroke(cr);

  // reduce font size for the rest of the graph
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);

  if(g->gui_mode != DT_FILMIC_GUI_RANGES)
  {
    // Draw graph background then border
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
    cairo_rectangle(cr, 0, 0, g->graph_width, g->graph_height);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_border);
    cairo_stroke(cr);

    // draw grid
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
    set_color(cr, darktable.bauhaus->graph_border);

    // we need to tweak the coordinates system to match dt_draw_grid expectations
    cairo_save(cr);
    cairo_scale(cr, 1., -1.);
    cairo_translate(cr, 0., -g->graph_height);

    if(g->gui_mode == DT_FILMIC_GUI_LOOK || g->gui_mode == DT_FILMIC_GUI_BASECURVE)
      dt_draw_grid(cr, 4, 0, 0, g->graph_width, g->graph_height);
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      dt_draw_loglog_grid(cr, 4, 0, 0, g->graph_width, g->graph_height, LOGBASE);

    // reset coordinates
    cairo_restore(cr);

    // draw identity line
    cairo_move_to(cr, 0, g->graph_height);
    cairo_line_to(cr, g->graph_width, 0);
    cairo_stroke(cr);

    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

    // Draw the saturation curve
    const float saturation = (2.0f * p->saturation / 100.0f + 1.0f);
    const float sigma_toe = powf(g->spline.latitude_min / 3.0f, 2.0f);
    const float sigma_shoulder = powf((1.0f - g->spline.latitude_max) / 3.0f, 2.0f);

    cairo_set_source_rgb(cr, .5, .5, .5);

    // prevent graph overflowing
    cairo_save(cr);
    cairo_rectangle(cr, -DT_PIXEL_APPLY_DPI(2.), -DT_PIXEL_APPLY_DPI(2.),
                    g->graph_width + 2. * DT_PIXEL_APPLY_DPI(2.), g->graph_height + 2. * DT_PIXEL_APPLY_DPI(2.));
    cairo_clip(cr);

    if(p->version == DT_FILMIC_COLORSCIENCE_V1)
    {
      cairo_move_to(cr, 0,
                    g->graph_height * (1.0 - filmic_desaturate_v1(0.0f, sigma_toe, sigma_shoulder, saturation)));
      for(int k = 1; k < 256; k++)
      {
        float x = k / 255.0;
        const float y = filmic_desaturate_v1(x, sigma_toe, sigma_shoulder, saturation);

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);

        cairo_line_to(cr, x * g->graph_width, g->graph_height * (1.0 - y));
      }
    }
    else if(p->version == DT_FILMIC_COLORSCIENCE_V2 || p->version == DT_FILMIC_COLORSCIENCE_V3)
    {
      cairo_move_to(cr, 0,
                    g->graph_height * (1.0 - filmic_desaturate_v2(0.0f, sigma_toe, sigma_shoulder, saturation)));
      for(int k = 1; k < 256; k++)
      {
        float x = k / 255.0;
        const float y = filmic_desaturate_v2(x, sigma_toe, sigma_shoulder, saturation);

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);

        cairo_line_to(cr, x * g->graph_width, g->graph_height * (1.0 - y));
      }
    }
    cairo_stroke(cr);

    // draw the tone curve
    float x_start = 0.f;
    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      x_start = log_tonemapping(x_start, grey, p->black_point_source, DR);

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) x_start = dt_log_scale_axis(x_start, LOGBASE);

    float y_start = clamp_simd(filmic_spline(x_start, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                             g->spline.M5, g->spline.latitude_min, g->spline.latitude_max, g->spline.type));

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
      y_start = powf(y_start, p->output_power);
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      y_start = dt_log_scale_axis(powf(y_start, p->output_power), LOGBASE);

    cairo_move_to(cr, 0, g->graph_height * (1.0 - y_start));

    for(int k = 1; k < 256; k++)
    {
      // k / 255 step defines a linearly scaled space. This might produce large gaps in lowlights when using log
      // GUI scaling so we non-linearly rescale that step to get more points in lowlights
      float x = powf(k / 255.0f, 2.4f);
      float value = x;

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        value = log_tonemapping(x, grey, p->black_point_source, DR);

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) x = dt_log_scale_axis(x, LOGBASE);

      float y = filmic_spline(value, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5,
                              g->spline.latitude_min, g->spline.latitude_max, g->spline.type);

      // curve is drawn in orange when above maximum
      // or below minimum.
      // we use a small margin in the comparison
      // to avoid drawing curve in orange when it
      // is right above or right below the limit
      // due to floating point errors
      const float margin = 1E-5;
      if(y > g->spline.y[4] + margin)
      {
        y = fminf(y, 1.0f);
        cairo_set_source_rgb(cr, 0.75, .5, 0.);
      }
      else if(y < g->spline.y[0] - margin)
      {
        y = fmaxf(y, 0.f);
        cairo_set_source_rgb(cr, 0.75, .5, 0.);
      }
      else
      {
        set_color(cr, darktable.bauhaus->graph_fg);
      }

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
        y = powf(y, p->output_power);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        y = dt_log_scale_axis(powf(y, p->output_power), LOGBASE);

      cairo_line_to(cr, x * g->graph_width, g->graph_height * (1.0 - y));
      cairo_stroke(cr);
      cairo_move_to(cr, x * g->graph_width, g->graph_height * (1.0 - y));
    }

    cairo_restore(cr);

    // draw nodes

    // special case for the grey node
    cairo_save(cr);
    cairo_rectangle(cr, -DT_PIXEL_APPLY_DPI(4.), -DT_PIXEL_APPLY_DPI(4.),
                    g->graph_width + 2. * DT_PIXEL_APPLY_DPI(4.), g->graph_height + 2. * DT_PIXEL_APPLY_DPI(4.));
    cairo_clip(cr);
    float x_grey = g->spline.x[2];
    float y_grey = g->spline.y[2];

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
    {
      x_grey = exp_tonemapping_v2(x_grey, grey, p->black_point_source, DR);
      y_grey = powf(y_grey, p->output_power);
    }
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
    {
      x_grey = dt_log_scale_axis(exp_tonemapping_v2(x_grey, grey, p->black_point_source, DR), LOGBASE);
      y_grey = dt_log_scale_axis(powf(y_grey, p->output_power), LOGBASE);
    }

    cairo_set_source_rgb(cr, 0.75, 0.5, 0.0);
    cairo_arc(cr, x_grey * g->graph_width, (1.0 - y_grey) * g->graph_height, DT_PIXEL_APPLY_DPI(6), 0,
              2. * M_PI);
    cairo_fill(cr);
    cairo_stroke(cr);

    // latitude nodes
    float x_black = 0.f;
    float y_black = 0.f;

    float x_white = 1.f;
    float y_white = 1.f;

    const float central_slope = (g->spline.y[3] - g->spline.y[1]) * g->graph_width / ((g->spline.x[3] - g->spline.x[1]) * g->graph_height);
    const float central_slope_angle = atanf(central_slope) + M_PI / 2.0f;
    set_color(cr, darktable.bauhaus->graph_fg);
    for(int k = 0; k < 5; k++)
    {
      if(k != 2) // k == 2 : grey point, already processed above
      {
        float x = g->spline.x[k];
        float y = g->spline.y[k];
        const float ymin = g->spline.y[0];
        const float ymax = g->spline.y[4];
        // we multiply SAFETY_MARGIN by 1.1f to avoid possible false negatives due to float errors
        const float y_margin = SAFETY_MARGIN * 1.1f * (ymax - ymin);
        gboolean red = (((k == 1) && (y - ymin <= y_margin))
                     || ((k == 3) && (ymax - y <= y_margin)));
        float start_angle = 0.0f;
        float end_angle = 2.f * M_PI;
        // if contrast is clamped, show it on GUI with half circles
        // for points 1 and 3
        if(contrast_clamped)
        {
          if(k == 1)
          {
            start_angle = central_slope_angle + M_PI;
            end_angle = central_slope_angle;
          }
          if(k == 3)
          {
            start_angle = central_slope_angle;
            end_angle = start_angle + M_PI;
          }
        }

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
        {
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
          y = powf(y, p->output_power);
        }
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        {
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);
          y = dt_log_scale_axis(powf(y, p->output_power), LOGBASE);
        }

        // save the bounds of the curve to mark the axis graduation
        if(k == 0) // black point
        {
          x_black = x;
          y_black = y;
        }
        else if(k == 4) // white point
        {
          x_white = x;
          y_white = y;
        }

        if(red) cairo_set_source_rgb(cr, 0.8, 0.35, 0.35);

        // draw bullet
        cairo_arc(cr, x * g->graph_width, (1.0 - y) * g->graph_height, DT_PIXEL_APPLY_DPI(4), start_angle, end_angle);
        cairo_fill(cr);
        cairo_stroke(cr);

        // reset color for next points
        if(red) set_color(cr, darktable.bauhaus->graph_fg);
      }
    }
    cairo_restore(cr);

    if(g->gui_show_labels)
    {
      // position of the upper bound of x axis labels
      const float x_legend_top = g->graph_height + 0.5 * g->line_height;

      // mark the y axis graduation at grey spot
      set_color(cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->grey_point_target);
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_grey) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // mark the x axis graduation at grey spot
      set_color(cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", 0.f);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        snprintf(text, sizeof(text), "%.0f", p->grey_point_source);

      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, x_grey * g->graph_width - 0.5 * g->ink.width - g->ink.x, x_legend_top);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // mark the y axis graduation at black spot
      set_color(cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->black_point_target);
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_black) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // mark the y axis graduation at black spot
      set_color(cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->white_point_target);
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_white) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // mark the x axis graduation at black spot
      set_color(cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", p->black_point_source);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        snprintf(text, sizeof(text), "%.0f", exp2f(p->black_point_source) * p->grey_point_source);

      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, x_black * g->graph_width - 0.5 * g->ink.width - g->ink.x, x_legend_top);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // mark the x axis graduation at white spot
      set_color(cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", p->white_point_source);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      {
        if(x_white > 1.f)
          snprintf(text, sizeof(text), "%.0f \342\206\222", 100.f); // this marks the bound of the graph, not the actual white
        else
          snprintf(text, sizeof(text), "%.0f", exp2f(p->white_point_source) * p->grey_point_source);
      }

      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr,
                    fminf(x_white, 1.f) * g->graph_width - 0.5 * g->ink.width - g->ink.x
                        + 2. * (x_white > 1.f) * g->sign_width,
                    x_legend_top);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // handle the case where white > 100 %, so the node is out of the graph.
      // we still want to display the value to get a hint.
      set_color(cr, darktable.bauhaus->graph_fg);
      if((g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) && (x_white > 1.f))
      {
        // set to italic font
        PangoStyle backup = pango_font_description_get_style(desc);
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
        pango_layout_set_font_description(layout, desc);

        snprintf(text, sizeof(text), _("(%.0f %%)"), exp2f(p->white_point_source) * p->grey_point_source);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &g->ink, NULL);
        cairo_move_to(cr, g->allocation.width - g->ink.width - g->ink.x - margin_left,
                      g->graph_height + 3. * g->inset + g->line_height - g->ink.y);
        pango_cairo_show_layout(cr, layout);
        cairo_stroke(cr);

        // restore font
        pango_font_description_set_style(desc, backup);
        pango_layout_set_font_description(layout, desc);
      }

      // mark the y axis legend
      set_color(cr, darktable.bauhaus->graph_fg);
      /* xgettext:no-c-format */
      g_strlcpy(text, _("% display"), sizeof(text));
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, -2. * g->inset - g->zero_width - g->ink.x,
                    -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);


      // mark the x axis legend
      set_color(cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        g_strlcpy(text, _("EV scene"), sizeof(text));
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      {
        /* xgettext:no-c-format */
        g_strlcpy(text, _("% camera"), sizeof(text));
      }
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, 0.5 * g->graph_width - 0.5 * g->ink.width - g->ink.x,
                    g->graph_height + 3. * g->inset + g->line_height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);
    }
  }
  else
  {
    // mode ranges
    cairo_identity_matrix(cr); // reset coordinates

    // draw the dynamic range of display
    // if white = 100%, assume -11.69 EV because of uint8 output + sRGB OETF.
    // for uint10 output, white should be set to 400%, so anything above 100% increases DR
    // FIXME : if darktable becomes HDR-10bits compatible (for output), this needs to be updated
    const float display_DR = 12.f + log2f(p->white_point_target / 100.f);

    const float y_display = g->allocation.height / 3.f + g->line_height;
    const float y_scene = 2. * g->allocation.height / 3.f + g->line_height;

    const float display_top = y_display - g->line_height / 2;
    const float display_bottom = display_top + g->line_height;

    const float scene_top = y_scene - g->line_height / 2;
    const float scene_bottom = scene_top + g->line_height;

    float column_left;

    if(g->gui_show_labels)
    {
      // labels
      set_color(cr, darktable.bauhaus->graph_fg);
      g_strlcpy(text, _("display"), sizeof(text));
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, 0., y_display - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);
      const float display_label_width = g->ink.width;

      // axis legend
      g_strlcpy(text, _("(%)"), sizeof(text));
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, 0.5 * display_label_width - 0.5 * g->ink.width - g->ink.x,
                    display_top - 4. * g->inset - g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      set_color(cr, darktable.bauhaus->graph_fg);
      g_strlcpy(text, _("scene"), sizeof(text));
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, 0., y_scene - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);
      const float scene_label_width = g->ink.width;

      // axis legend
      g_strlcpy(text, _("(EV)"), sizeof(text));
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);
      cairo_move_to(cr, 0.5 * scene_label_width - 0.5 * g->ink.width - g->ink.x,
                    scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);

      // arrow between labels
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
      dt_cairo_draw_arrow(cr, fminf(scene_label_width, display_label_width) / 2.f, y_scene - g->line_height,
                          fminf(scene_label_width, display_label_width) / 2.f,
                          y_display + g->line_height + g->inset, TRUE);

      column_left = fmaxf(display_label_width, scene_label_width) + g->inset;
    }
    else
      column_left = darktable.bauhaus->quad_width;

    const float column_right = g->allocation.width - column_left - darktable.bauhaus->quad_width;

    // compute dynamic ranges left and right to middle grey
    const float display_HL_EV = -log2f(p->grey_point_target / p->white_point_target); // compared to white EV
    const float display_LL_EV = display_DR - display_HL_EV;                           // compared to black EV
    const float display_real_black_EV
        = -fmaxf(log2f(p->black_point_target / p->grey_point_target),
                 -11.685887601778058f + display_HL_EV - log2f(p->white_point_target / 100.f));
    const float scene_HL_EV = p->white_point_source;  // compared to white EV
    const float scene_LL_EV = -p->black_point_source; // compared to black EV

    // compute the max width needed to fit both dynamic ranges and derivate the unit size of a GUI EV
    const float max_DR = ceilf(fmaxf(display_HL_EV, scene_HL_EV)) + ceilf(fmaxf(display_LL_EV, scene_LL_EV));
    const float EV = (column_right) / max_DR;

    // all greys are aligned vertically in GUI since they are the fulcrum of the transform
    // so, get their coordinates
    const float grey_EV = fmaxf(ceilf(display_HL_EV), ceilf(scene_HL_EV));
    const float grey_x = g->allocation.width - (grey_EV)*EV - darktable.bauhaus->quad_width;

    // similarly, get black/white coordinates from grey point
    const float display_black_x = grey_x - display_real_black_EV * EV;
    const float display_DR_start_x = grey_x - display_LL_EV * EV;
    const float display_white_x = grey_x + display_HL_EV * EV;

    const float scene_black_x = grey_x - scene_LL_EV * EV;
    const float scene_white_x = grey_x + scene_HL_EV * EV;
    const float scene_lat_bottom = grey_x + (g->spline.x[1] - g->spline.x[2]) * EV * DR;
    const float scene_lat_top = grey_x + (g->spline.x[3] - g->spline.x[2]) * EV * DR;

    // show EV zones for display - zones are aligned on 0% and 100%
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

    // latitude bounds - show contrast expansion

    // Compute usual filmic  mapping
    float display_lat_bottom = filmic_spline(g->spline.latitude_min, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                  g->spline.M5, g->spline.latitude_min, g->spline.latitude_max, g->spline.type);
    display_lat_bottom = powf(fmaxf(display_lat_bottom, NORM_MIN), p->output_power); // clamp at -16 EV

    // rescale output to log scale
    display_lat_bottom = log2f(display_lat_bottom/ (p->grey_point_target / 100.f));

    // take clamping into account
    if(display_lat_bottom < 0.f) // clamp to - 8 EV (black)
      display_lat_bottom = fmaxf(display_lat_bottom, -display_real_black_EV);
    else if(display_lat_bottom > 0.f) // clamp to 0 EV (white)
      display_lat_bottom = fminf(display_lat_bottom, display_HL_EV);

    // get destination coordinate
    display_lat_bottom = grey_x + display_lat_bottom * EV;

    // Compute usual filmic  mapping
    float display_lat_top = filmic_spline(g->spline.latitude_max, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                  g->spline.M5, g->spline.latitude_min, g->spline.latitude_max, g->spline.type);
    display_lat_top = powf(fmaxf(display_lat_top, NORM_MIN), p->output_power); // clamp at -16 EV

    // rescale output to log scale
    display_lat_top = log2f(display_lat_top / (p->grey_point_target / 100.f));

    // take clamping into account
    if(display_lat_top < 0.f) // clamp to - 8 EV (black)
      display_lat_top = fmaxf(display_lat_top, -display_real_black_EV);
    else if(display_lat_top > 0.f) // clamp to 0 EV (white)
      display_lat_top = fminf(display_lat_top, display_HL_EV);

    // get destination coordinate and draw
    display_lat_top = grey_x + display_lat_top * EV;

    cairo_move_to(cr, scene_lat_bottom, scene_top);
    cairo_line_to(cr, scene_lat_top, scene_top);
    cairo_line_to(cr, display_lat_top, display_bottom);
    cairo_line_to(cr, display_lat_bottom, display_bottom);
    cairo_line_to(cr, scene_lat_bottom, scene_top);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);

    for(int i = 0; i < (int)ceilf(display_DR); i++)
    {
      // content
      const float shade = powf(exp2f(-11.f + (float)i), 1.f / 2.4f);
      cairo_set_source_rgb(cr, shade, shade, shade);
      cairo_rectangle(cr, display_DR_start_x + i * EV, display_top, EV, g->line_height);
      cairo_fill_preserve(cr);

      // borders
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
      cairo_stroke(cr);
    }

    // middle grey display
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_move_to(cr, grey_x, display_bottom + 2. * g->inset);
    cairo_line_to(cr, grey_x, display_top - 2. * g->inset);
    cairo_stroke(cr);

    // show EV zones for scene - zones are aligned on grey

    for(int i = floorf(p->black_point_source); i < ceilf(p->white_point_source); i++)
    {
      // content
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
      const float shade = powf(0.1845f * exp2f((float)i), 1.f / 2.4f);
      const float x_temp = grey_x + i * EV;
      cairo_set_source_rgb(cr, shade, shade, shade);
      cairo_rectangle(cr, x_temp, scene_top, EV, g->line_height);
      cairo_fill_preserve(cr);

      // borders
      cairo_set_source_rgb(cr, 0.75, .5, 0.);
      cairo_stroke(cr);

      // arrows
      if(i == 0)
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
      else
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

      if((float)i > p->black_point_source && (float)i < p->white_point_source)
      {
        // Compute usual filmic  mapping
        const float normal_value = ((float)i - p->black_point_source) / DR;
        float y_temp = filmic_spline(normal_value, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                     g->spline.M5, g->spline.latitude_min, g->spline.latitude_max, g->spline.type);
        y_temp = powf(fmaxf(y_temp, NORM_MIN), p->output_power); // clamp at -16 EV

        // rescale output to log scale
        y_temp = log2f(y_temp / (p->grey_point_target / 100.f));

        // take clamping into account
        if(y_temp < 0.f) // clamp to - 8 EV (black)
          y_temp = fmaxf(y_temp, -display_real_black_EV);
        else if(y_temp > 0.f) // clamp to 0 EV (white)
          y_temp = fminf(y_temp, display_HL_EV);

        // get destination coordinate and draw
        y_temp = grey_x + y_temp * EV;
        dt_cairo_draw_arrow(cr, x_temp, scene_top, y_temp, display_bottom, FALSE);
      }
    }

    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

    // arrows for black and white
    float x_temp = grey_x + p->black_point_source * EV;
    float y_temp = grey_x - display_real_black_EV * EV;
    dt_cairo_draw_arrow(cr, x_temp, scene_top, y_temp, display_bottom, FALSE);

    x_temp = grey_x + p->white_point_source * EV;
    y_temp = grey_x + display_HL_EV * EV;
    dt_cairo_draw_arrow(cr, x_temp, scene_top, y_temp, display_bottom, FALSE);

    // draw white - grey - black ticks

    // black display
    cairo_move_to(cr, display_black_x, display_bottom);
    cairo_line_to(cr, display_black_x, display_top - 2. * g->inset);
    cairo_stroke(cr);

    // middle grey display
    cairo_move_to(cr, grey_x, display_bottom);
    cairo_line_to(cr, grey_x, display_top - 2. * g->inset);
    cairo_stroke(cr);

    // white display
    cairo_move_to(cr, display_white_x, display_bottom);
    cairo_line_to(cr, display_white_x, display_top - 2. * g->inset);
    cairo_stroke(cr);

    // black scene
    cairo_move_to(cr, scene_black_x, scene_bottom + 2. * g->inset);
    cairo_line_to(cr, scene_black_x, scene_top);
    cairo_stroke(cr);

    // middle grey scene
    cairo_move_to(cr, grey_x, scene_bottom + 2. * g->inset);
    cairo_line_to(cr, grey_x, scene_top);
    cairo_stroke(cr);

    // white scene
    cairo_move_to(cr, scene_white_x, scene_bottom + 2. * g->inset);
    cairo_line_to(cr, scene_white_x, scene_top);
    cairo_stroke(cr);

    // legends
    set_color(cr, darktable.bauhaus->graph_fg);

    // black scene legend
    snprintf(text, sizeof(text), "%+.1f", p->black_point_source);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, scene_black_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    // grey scene legend
    snprintf(text, sizeof(text), "%+.1f", 0.f);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, grey_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    // white scene legend
    snprintf(text, sizeof(text), "%+.1f", p->white_point_source);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, scene_white_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    // black scene legend
    snprintf(text, sizeof(text), "%.0f", p->black_point_target);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, display_black_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    // grey scene legend
    snprintf(text, sizeof(text), "%.0f", p->grey_point_target);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, grey_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    // white scene legend
    snprintf(text, sizeof(text), "%.0f", p->white_point_target);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);
    cairo_move_to(cr, display_white_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
  }

  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return TRUE;
}

static gboolean area_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return TRUE;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);

  if(g->active_button != DT_FILMIC_GUI_BUTTON_LAST)
  {

    if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
    {
      // double click resets view
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        g->gui_mode = DT_FILMIC_GUI_LOOK;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
    else if(event->button == 1)
    {
      // simple left click cycles through modes in positive direction
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        // cycle type of graph
        if(g->gui_mode == DT_FILMIC_GUI_RANGES)
          g->gui_mode = DT_FILMIC_GUI_LOOK;
        else
          g->gui_mode++;

        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
      {
        g->gui_show_labels = !g->gui_show_labels;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_show_labels", g->gui_show_labels);
        return TRUE;
      }
      else
      {
        // we should never get there since (g->active_button != DT_FILMIC_GUI_BUTTON_LAST)
        // and any other case has been processed above.
        return FALSE;
      }
    }
    else if(event->button == 3)
    {
      // simple right click cycles through modes in negative direction
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        if(g->gui_mode == DT_FILMIC_GUI_LOOK)
          g->gui_mode = DT_FILMIC_GUI_RANGES;
        else
          g->gui_mode--;

        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
      {
        g->gui_show_labels = !g->gui_show_labels;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_show_labels", g->gui_show_labels);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
  }

  return FALSE;
}

static gboolean area_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;
  if(!self->enabled) return 0;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->gui_hover = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}


static gboolean area_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;
  if(!self->enabled) return 0;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->gui_hover = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}

static gboolean area_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  if(!g->gui_sizes_inited) return FALSE;

  // get in-widget coordinates
  const float y = event->y;
  const float x = event->x;

  if(x > 0. && x < g->allocation.width && y > 0. && y < g->allocation.height) g->gui_hover = TRUE;

  gint save_active_button = g->active_button;

  if(g->gui_hover)
  {
    // find out which button is under the mouse
    gint found_something = FALSE;
    for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++)
    {
      // check if mouse in in the button's bounds
      if(x > g->buttons[i].left && x < g->buttons[i].right && y > g->buttons[i].top && y < g->buttons[i].bottom)
      {
        // yeah, mouse is over that button
        g->buttons[i].mouse_hover = TRUE;
        g->active_button = i;
        found_something = TRUE;
      }
      else
      {
        // no luck with this button
        g->buttons[i].mouse_hover = FALSE;
      }
    }

    if(!found_something) g->active_button = DT_FILMIC_GUI_BUTTON_LAST; // mouse is over no known button

    // update the tooltips
    if(g->active_button == DT_FILMIC_GUI_BUTTON_LAST && x < g->buttons[0].left)
    {
      // we are over the graph area
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("use the parameters below to set the nodes.\n"
                                                         "the bright curve is the filmic tone mapping curve\n"
                                                         "the dark curve is the desaturation curve."));
    }
    else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("toggle axis labels and values display"));
    }
    else if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("cycle through graph views.\n"
                                                         "left click: cycle forward.\n"
                                                         "right click: cycle backward.\n"
                                                         "double-click: reset to look view."));
    }
    else
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), "");
    }

    if(save_active_button != g->active_button) gtk_widget_queue_draw(GTK_WIDGET(g->area));
    return TRUE;
  }
  else
  {
    g->active_button = DT_FILMIC_GUI_BUTTON_LAST;
    if(save_active_button != g->active_button) (GTK_WIDGET(g->area));
    return FALSE;
  }
}

static gboolean area_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    int delta_y;
    if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
    {
      //adjust aspect
      const int aspect = dt_conf_get_int("plugins/darkroom/filmicrgb/aspect_percent");
      dt_conf_set_int("plugins/darkroom/filmicrgb/aspect_percent", aspect + delta_y);
      dtgtk_drawing_area_set_aspect_ratio(widget, aspect / 100.0);
    }
    return TRUE; // Ensure that scrolling cannot move side panel when no delta
  }
  return FALSE;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = IOP_GUI_ALLOC(filmicrgb);

  g->show_mask = FALSE;
  g->gui_mode = DT_FILMIC_GUI_LOOK;
  g->gui_show_labels = TRUE;
  g->gui_hover = FALSE;
  g->gui_sizes_inited = FALSE;

  // don't make the area square to safe some vertical space -- it's not interactive anyway
  const float aspect = dt_conf_get_int("plugins/darkroom/filmicrgb/aspect_percent") / 100.0;
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(aspect));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);

  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                                 | GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(area_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(area_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "enter-notify-event", G_CALLBACK(area_enter_notify), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(area_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(area_scroll_callback), self);

  // Init GTK notebook
  g->notebook = dt_ui_notebook_new();

  // Page SCENE
  self->widget = dt_ui_notebook_page(g->notebook, N_("scene"), NULL);

  g->grey_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "grey_point_source"));
  dt_bauhaus_slider_set_soft_range(g->grey_point_source, .1, 36.0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%");
  gtk_widget_set_tooltip_text(g->grey_point_source,
                              _("adjust to match the average luminance of the image's subject.\n"
                                "the value entered here will then be remapped to 18.45%.\n"
                                "decrease the value to increase the overall brightness."));

  // White slider
  g->white_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_point_source"));
  dt_bauhaus_slider_set_soft_range(g->white_point_source, 2.0, 8.0);
  dt_bauhaus_slider_set_format(g->white_point_source, _(" EV"));
  gtk_widget_set_tooltip_text(g->white_point_source,
                              _("number of stops between middle gray and pure white.\n"
                                "this is a reading a lightmeter would give you on the scene.\n"
                                "adjust so highlights clipping is avoided"));

  // Black slider
  g->black_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "black_point_source"));
  dt_bauhaus_slider_set_soft_range(g->black_point_source, -14.0, -3);
  dt_bauhaus_slider_set_format(g->black_point_source, _(" EV"));
  gtk_widget_set_tooltip_text(
      g->black_point_source, _("number of stops between middle gray and pure black.\n"
                               "this is a reading a lightmeter would give you on the scene.\n"
                               "increase to get more contrast.\ndecrease to recover more details in low-lights."));

  // Dynamic range scaling
  g->security_factor = dt_bauhaus_slider_from_params(self, "security_factor");
  dt_bauhaus_slider_set_soft_max(g->security_factor, 50);
  dt_bauhaus_slider_set_format(g->security_factor, "%");
  gtk_widget_set_tooltip_text(g->security_factor, _("symmetrically enlarge or shrink the computed dynamic range.\n"
                                                    "useful to give a safety margin to extreme luminances."));

  // Auto tune slider
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("auto tune levels")), TRUE, TRUE, 0);
  g->auto_button = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, NULL);
  gtk_box_pack_start(GTK_BOX(hbox), g->auto_button, FALSE, FALSE, 0);
  dt_gui_add_class(g->auto_button, "dt_bauhaus_alignment");
  gtk_widget_set_tooltip_text(g->auto_button, _("try to optimize the settings with some statistical assumptions.\n"
                                                "this will fit the luminance range inside the histogram bounds.\n"
                                                "works better for landscapes and evenly-lit pictures\n"
                                                "but fails for high-keys, low-keys and high-ISO pictures.\n"
                                                "this is not an artificial intelligence, but a simple guess.\n"
                                                "ensure you understand its assumptions before using it."));
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  GtkWidget *label = dt_ui_section_label_new(_("advanced"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->custom_grey = dt_bauhaus_toggle_from_params(self, "custom_grey");
  gtk_widget_set_tooltip_text(g->custom_grey, _("enable to input custom middle-gray values.\n"
                                                "this is not recommended in general.\n"
                                                "fix the global exposure in the exposure module instead.\n"
                                                "disable to use standard 18.45 %% middle gray."));

  // Page RECONSTRUCT
  self->widget = dt_ui_notebook_page(g->notebook, N_("reconstruct"), NULL);

  label = dt_ui_section_label_new(_("highlights clipping"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->reconstruct_threshold = dt_bauhaus_slider_from_params(self, "reconstruct_threshold");
  dt_bauhaus_slider_set_format(g->reconstruct_threshold, _(" EV"));
  gtk_widget_set_tooltip_text(g->reconstruct_threshold,
                              _("set the exposure threshold upon which\n"
                                "clipped highlights get reconstructed.\n"
                                "values are relative to the scene white point.\n"
                                "0 EV means the threshold is the same as the scene white point.\n"
                                "decrease to include more areas,\n"
                                "increase to exclude more areas."));

  g->reconstruct_feather = dt_bauhaus_slider_from_params(self, "reconstruct_feather");
  dt_bauhaus_slider_set_format(g->reconstruct_feather, _(" EV"));
  gtk_widget_set_tooltip_text(g->reconstruct_feather,
                              _("soften the transition between clipped highlights and valid pixels.\n"
                                "decrease to make the transition harder and sharper,\n"
                                "increase to make the transition softer and blurrier."));

  // Highlight Reconstruction Mask
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("display highlight reconstruction mask")), TRUE, TRUE, 0);
  g->show_highlight_mask = dt_iop_togglebutton_new(self, NULL, N_("display highlight reconstruction mask"), NULL, G_CALLBACK(show_mask_callback),
                                           FALSE, 0, 0, dtgtk_cairo_paint_showmask, hbox);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->show_highlight_mask), dtgtk_cairo_paint_showmask, 0, NULL);
  dt_gui_add_class(g->show_highlight_mask, "dt_bauhaus_alignment");
  dt_gui_add_class(g->show_highlight_mask, "dt_transparent_background");
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  label = dt_ui_section_label_new(_("balance"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->reconstruct_structure_vs_texture = dt_bauhaus_slider_from_params(self, "reconstruct_structure_vs_texture");
  dt_bauhaus_slider_set_format(g->reconstruct_structure_vs_texture, "%");
  gtk_widget_set_tooltip_text(g->reconstruct_structure_vs_texture,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between inpainting a smooth color gradient,\n"
                                "or trying to recover the textured details.\n"
                                "0% is an equal mix of both.\n"
                                "increase if at least one RGB channel is not clipped.\n"
                                "decrease if all RGB channels are clipped over large areas."));

  g->reconstruct_bloom_vs_details = dt_bauhaus_slider_from_params(self, "reconstruct_bloom_vs_details");
  dt_bauhaus_slider_set_format(g->reconstruct_bloom_vs_details, "%");
  gtk_widget_set_tooltip_text(g->reconstruct_bloom_vs_details,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between blooming highlights like film does,\n"
                                "or trying to recover sharp details.\n"
                                "0% is an equal mix of both.\n"
                                "increase if you want more details.\n"
                                "decrease if you want more blur."));

  // Bloom threshold
  g->reconstruct_grey_vs_color = dt_bauhaus_slider_from_params(self, "reconstruct_grey_vs_color");
  dt_bauhaus_slider_set_format(g->reconstruct_grey_vs_color, "%");
  gtk_widget_set_tooltip_text(g->reconstruct_grey_vs_color,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between recovering monochromatic highlights,\n"
                                "or trying to recover colorful highlights.\n"
                                "0% is an equal mix of both.\n"
                                "increase if you want more color.\n"
                                "decrease if you see magenta or out-of-gamut highlights."));

  label = dt_ui_section_label_new(_("advanced"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  // Color inpainting
  g->high_quality_reconstruction = dt_bauhaus_slider_from_params(self, "high_quality_reconstruction");
  gtk_widget_set_tooltip_text(g->high_quality_reconstruction,
                              _("run extra passes of chromaticity reconstruction.\n"
                                "more iterations means more color propagation from neighbourhood.\n"
                                "this will be slower but will yield more neutral highlights.\n"
                                "it also helps with difficult cases of magenta highlights."));

  // Highlight noise
  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  gtk_widget_set_tooltip_text(g->noise_level, _("add statistical noise in reconstructed highlights.\n"
                                                "this avoids highlights to look too smooth\n"
                                                "when the picture is noisy overall,\n"
                                                "so they blend with the rest of the picture."));

  // Noise distribution
  g->noise_distribution = dt_bauhaus_combobox_from_params(self, "noise_distribution");
  gtk_widget_set_tooltip_text(g->noise_distribution, _("choose the statistical distribution of noise.\n"
                                                       "this is useful to match natural sensor noise pattern.\n"));

  // Page LOOK
  self->widget = dt_ui_notebook_page(g->notebook, N_("look"), NULL);

  label = dt_ui_section_label_new(_("tone mapping"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->contrast = dt_bauhaus_slider_from_params(self, N_("contrast"));
  dt_bauhaus_slider_set_soft_range(g->contrast, 0.5, 3.0);
  dt_bauhaus_slider_set_digits(g->contrast, 3);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve\n"
                                             "affects mostly the mid-tones"));

  // brightness slider
  g->output_power = dt_bauhaus_slider_from_params(self, "output_power");
  gtk_widget_set_tooltip_text(g->output_power, _("equivalent to paper grade in analog.\n"
                                                 "increase to make highlights brighter and less compressed.\n"
                                                 "decrease to mute highlights."));

  g->toe = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 0.0f, 33.0f, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), g->toe, FALSE, FALSE, 0);
  dt_bauhaus_widget_set_label(g->toe, N_("shadows"));
  dt_bauhaus_slider_set_soft_range(g->toe, 0.1f, 90.0f);
  dt_bauhaus_slider_set_format(g->toe, "%");
  gtk_widget_set_tooltip_text(g->toe,
                              _("distance between middle gray and the start of the shadows roll-off.\n"
                                "0% keeps the toe at middle gray, 100% pushes it to the point where the\n"
                                "current slope would hit the output black level."));
  g_signal_connect(G_OBJECT(g->toe), "value-changed", G_CALLBACK(toe_shoulder_callback), self);

  g->shoulder = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 0.0f, 33.0f, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shoulder, FALSE, FALSE, 0);
  dt_bauhaus_widget_set_label(g->shoulder, N_("highlights"));
  dt_bauhaus_slider_set_soft_range(g->shoulder, 0.1f, 90.0f);
  dt_bauhaus_slider_set_format(g->shoulder, "%");
  gtk_widget_set_tooltip_text(g->shoulder,
                              _("distance between middle gray and the start of the highlights roll-off.\n"
                                "0% keeps the shoulder at middle gray, 100% pushes it to the point where the\n"
                                "current slope would hit the output white level."));
  g_signal_connect(G_OBJECT(g->shoulder), "value-changed", G_CALLBACK(toe_shoulder_callback), self);

  // Curve type
  g->highlights = dt_bauhaus_combobox_from_params(self, "highlights");
  gtk_widget_set_tooltip_text(g->highlights, _("choose the desired curvature of the filmic spline in highlights.\n"
                                               "hard uses a high curvature resulting in more tonal compression.\n"
                                               "soft uses a low curvature resulting in less tonal compression."));

  g->shadows = dt_bauhaus_combobox_from_params(self, "shadows");
  gtk_widget_set_tooltip_text(g->shadows, _("choose the desired curvature of the filmic spline in shadows.\n"
                                            "hard uses a high curvature resulting in more tonal compression.\n"
                                            "soft uses a low curvature resulting in less tonal compression."));

  label = dt_ui_section_label_new(_("color mapping"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
  dt_bauhaus_slider_set_soft_range(g->saturation, -50.0, 50.0);
  dt_bauhaus_slider_set_format(g->saturation, "%");
  gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                               "specifically at extreme luminances.\n"
                                               "increase if shadows and/or highlights are under-saturated."));

  g->preserve_color = dt_bauhaus_combobox_from_params(self, "preserve_color");
  gtk_widget_set_tooltip_text(g->preserve_color, _("ensure the original color are preserved.\n"
                                                   "may reinforce chromatic aberrations and chroma noise,\n"
                                                   "so ensure they are properly corrected elsewhere.\n"));

  label = dt_ui_section_label_new(_("advanced"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  // Color science
  g->version = dt_bauhaus_combobox_from_params(self, "version");
  gtk_widget_set_tooltip_text(g->version,
                              _("v3 is darktable 3.0 desaturation method, same as color balance.\n"
                                "v4 is a newer desaturation method, based on spectral purity of light."));


  g->auto_hardness = dt_bauhaus_toggle_from_params(self, "auto_hardness");
  gtk_widget_set_tooltip_text(
      g->auto_hardness, _("enable to auto-set the look hardness depending on the scene white and black points.\n"
                          "this keeps the middle gray on the identity line and improves fast tuning.\n"
                          "disable if you want a manual control."));

  // Page DISPLAY
  self->widget = dt_ui_notebook_page(g->notebook, N_("display"), NULL);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_from_params(self, "black_point_target");
  dt_bauhaus_slider_set_digits(g->black_point_target, 4);
  dt_bauhaus_slider_set_format(g->black_point_target, "%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                       "this should be 0%\nexcept if you want a faded look"));

  g->grey_point_target = dt_bauhaus_slider_from_params(self, "grey_point_target");
  dt_bauhaus_slider_set_digits(g->grey_point_target, 4);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%");
  gtk_widget_set_tooltip_text(g->grey_point_target,
                              _("middle gray value of the target display or color space.\n"
                                "you should never touch that unless you know what you are doing."));

  g->white_point_target = dt_bauhaus_slider_from_params(self, "white_point_target");
  dt_bauhaus_slider_set_soft_max(g->white_point_target, 100.0);
  dt_bauhaus_slider_set_digits(g->white_point_target, 4);
  dt_bauhaus_slider_set_format(g->white_point_target, "%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                       "this should be 100%\nexcept if you want a faded look"));

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(!w || w == g->auto_hardness || w == g->security_factor || w == g->grey_point_source
     || w == g->black_point_source || w == g->white_point_source)
  {
    ++darktable.gui->reset;

    if(w == g->security_factor || w == g->grey_point_source)
    {
      float prev = *(float *)previous;
      if(w == g->security_factor)
      {
        float ratio = (p->security_factor - prev) / (prev + 100.0f);

        float EVmin = p->black_point_source;
        EVmin = EVmin + ratio * EVmin;

        float EVmax = p->white_point_source;
        EVmax = EVmax + ratio * EVmax;

        p->white_point_source = EVmax;
        p->black_point_source = EVmin;
      }
      else
      {
        float grey_var = log2f(prev / p->grey_point_source);
        p->black_point_source = p->black_point_source - grey_var;
        p->white_point_source = p->white_point_source + grey_var;
      }

      dt_bauhaus_slider_set(g->white_point_source, p->white_point_source);
      dt_bauhaus_slider_set(g->black_point_source, p->black_point_source);
    }

    if(p->auto_hardness)
      p->output_power = logf(p->grey_point_target / 100.0f)
                        / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

    gtk_widget_set_visible(GTK_WIDGET(g->output_power), !p->auto_hardness);
    dt_bauhaus_slider_set(g->output_power, p->output_power);

    --darktable.gui->reset;
  }

  if(!w || w == g->version)
  {
    if(p->version == DT_FILMIC_COLORSCIENCE_V1 || p->version == DT_FILMIC_COLORSCIENCE_V4)
    {
      dt_bauhaus_widget_set_label(g->saturation, N_("extreme luminance saturation"));
      gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                                   "specifically at extreme luminances.\n"
                                                   "increase if shadows and/or highlights are under-saturated."));
    }
    else if(p->version == DT_FILMIC_COLORSCIENCE_V2 || p->version == DT_FILMIC_COLORSCIENCE_V3)
    {
      dt_bauhaus_widget_set_label(g->saturation, N_("mid-tones saturation"));
      gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                                   "specifically at medium luminances.\n"
                                                   "increase if midtones are under-saturated."));
    }
    else if(p->version == DT_FILMIC_COLORSCIENCE_V5)
    {
      dt_bauhaus_widget_set_label(g->saturation, N_("highlights saturation mix"));
      gtk_widget_set_tooltip_text(g->saturation, _("Positive values ensure saturation is kept unchanged over the whole range.\n"
                                                   "Negative values bleache highlights at constant hue and luminance.\n"
                                                   "Zero is an equal mix of both strategies."));
      gtk_widget_set_visible(GTK_WIDGET(g->preserve_color), FALSE);
    }

    if(p->version != DT_FILMIC_COLORSCIENCE_V5)
      gtk_widget_set_visible(GTK_WIDGET(g->preserve_color), TRUE);

  }

  if(!w || w == g->reconstruct_bloom_vs_details)
  {
    if(p->reconstruct_bloom_vs_details == -100.f)
    {
      // user disabled the reconstruction in favor of full blooming
      // so the structure vs. texture setting doesn't make any difference
      // make it insensitive to not confuse users
      gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, FALSE);
    }
    else
    {
      gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, TRUE);
    }
  }

  if(!w || w == g->custom_grey)
  {
    gtk_widget_set_visible(g->grey_point_source, p->custom_grey);
    gtk_widget_set_visible(g->grey_point_target, p->custom_grey);
  }

  filmic_gui_sync_toe_shoulder(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
