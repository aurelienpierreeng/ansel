/*
    This file is part of darktable,
    Copyright (C) 2012 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016-2019 Tobias Ellinghaus.
    Copyright (C) 2012-2015, 2017, 2019 Ulrich Pegelow.
    Copyright (C) 2013, 2016, 2022 Aldric Renaudin.
    Copyright (C) 2013 Dennis Gnad.
    Copyright (C) 2013 parafin.
    Copyright (C) 2013, 2018-2022 Pascal Obry.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2017-2018, 2020 Heiko Bauke.
    Copyright (C) 2017, 2019, 2021 luzpaz.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019-2022 Diederik Ter Rahe.
    Copyright (C) 2019 Florian Wernert.
    Copyright (C) 2019 mepi0011.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2021 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Marco.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021-2022 Nicolas Auffray.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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
#include "common/darktable.h"
#include "develop/blend.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/iop_profile.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"

#include "gui/actions/menu.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <assert.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NEUTRAL_GRAY 0.5
#define BLEND_MASKMODE_CONF_KEY "plugins/darkroom/blending/mask_mode_tab"

const dt_develop_name_value_t dt_develop_blend_mode_names[]
    = { { NC_("blendmode", "normal"), DEVELOP_BLEND_NORMAL2 },
        { NC_("blendmode", "normal bounded"), DEVELOP_BLEND_BOUNDED },
        { NC_("blendmode", "lighten"), DEVELOP_BLEND_LIGHTEN },
        { NC_("blendmode", "darken"), DEVELOP_BLEND_DARKEN },
        { NC_("blendmode", "multiply"), DEVELOP_BLEND_MULTIPLY },
        { NC_("blendmode", "average"), DEVELOP_BLEND_AVERAGE },
        { NC_("blendmode", "addition"), DEVELOP_BLEND_ADD },
        { NC_("blendmode", "subtract"), DEVELOP_BLEND_SUBTRACT },
        { NC_("blendmode", "difference"), DEVELOP_BLEND_DIFFERENCE2 },
        { NC_("blendmode", "screen"), DEVELOP_BLEND_SCREEN },
        { NC_("blendmode", "overlay"), DEVELOP_BLEND_OVERLAY },
        { NC_("blendmode", "softlight"), DEVELOP_BLEND_SOFTLIGHT },
        { NC_("blendmode", "hardlight"), DEVELOP_BLEND_HARDLIGHT },
        { NC_("blendmode", "vividlight"), DEVELOP_BLEND_VIVIDLIGHT },
        { NC_("blendmode", "linearlight"), DEVELOP_BLEND_LINEARLIGHT },
        { NC_("blendmode", "pinlight"), DEVELOP_BLEND_PINLIGHT },
        { NC_("blendmode", "lightness"), DEVELOP_BLEND_LIGHTNESS },
        { NC_("blendmode", "chromaticity"), DEVELOP_BLEND_CHROMATICITY },
        { NC_("blendmode", "hue"), DEVELOP_BLEND_HUE },
        { NC_("blendmode", "color"), DEVELOP_BLEND_COLOR },
        { NC_("blendmode", "coloradjustment"), DEVELOP_BLEND_COLORADJUST },
        { NC_("blendmode", "Lab lightness"), DEVELOP_BLEND_LAB_LIGHTNESS },
        { NC_("blendmode", "Lab color"), DEVELOP_BLEND_LAB_COLOR },
        { NC_("blendmode", "Lab L-channel"), DEVELOP_BLEND_LAB_L },
        { NC_("blendmode", "Lab a-channel"), DEVELOP_BLEND_LAB_A },
        { NC_("blendmode", "Lab b-channel"), DEVELOP_BLEND_LAB_B },
        { NC_("blendmode", "HSV value"), DEVELOP_BLEND_HSV_VALUE },
        { NC_("blendmode", "HSV color"), DEVELOP_BLEND_HSV_COLOR },
        { NC_("blendmode", "RGB red channel"), DEVELOP_BLEND_RGB_R },
        { NC_("blendmode", "RGB green channel"), DEVELOP_BLEND_RGB_G },
        { NC_("blendmode", "RGB blue channel"), DEVELOP_BLEND_RGB_B },
        { NC_("blendmode", "divide"), DEVELOP_BLEND_DIVIDE },
        { NC_("blendmode", "geometric mean"), DEVELOP_BLEND_GEOMETRIC_MEAN },
        { NC_("blendmode", "harmonic mean"), DEVELOP_BLEND_HARMONIC_MEAN },

        /** deprecated blend modes: make them available as legacy history stacks might want them */
        { NC_("blendmode", "difference (deprecated)"), DEVELOP_BLEND_DIFFERENCE },
        { NC_("blendmode", "subtract inverse (deprecated)"), DEVELOP_BLEND_SUBTRACT_INVERSE },
        { NC_("blendmode", "divide inverse (deprecated)"), DEVELOP_BLEND_DIVIDE_INVERSE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_blend_mode_flag_names[]
    = { { NC_("blendoperation", "normal"), 0 },
        { NC_("blendoperation", "reverse"), DEVELOP_BLEND_REVERSE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_blend_colorspace_names[]
    = { { N_("default"), DEVELOP_BLEND_CS_NONE },
        { N_("RAW"), DEVELOP_BLEND_CS_RAW },
        { N_("Lab"), DEVELOP_BLEND_CS_LAB },
        { N_("RGB (display)"), DEVELOP_BLEND_CS_RGB_DISPLAY },
        { N_("RGB (scene)"), DEVELOP_BLEND_CS_RGB_SCENE },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_mask_mode_names[]
    = { { N_("None"), 0 },
        { N_("Uniform"), 1 },
        { N_("Parametric mask"), 2 },
        { N_("Drawn mask"), 3 },
        { N_("Drawn & parametric mask"), 4 },
        { N_("Reuse an existing mask"), 5 },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_combine_masks_names[]
    = { { N_("exclusive"), DEVELOP_COMBINE_NORM_EXCL },
        { N_("inclusive"), DEVELOP_COMBINE_NORM_INCL },
        { N_("exclusive & inverted"), DEVELOP_COMBINE_INV_EXCL },
        { N_("inclusive & inverted"), DEVELOP_COMBINE_INV_INCL },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_feathering_guide_names[]
    = { { N_("output before blur"), DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR },
        { N_("input before blur"), DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR },
        { N_("output after blur"), DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR },
        { N_("input after blur"), DEVELOP_MASK_GUIDE_IN_AFTER_BLUR },
        { "", 0 } };

const dt_develop_name_value_t dt_develop_invert_mask_names[]
    = { { N_("off"), DEVELOP_COMBINE_NORM },
        { N_("on"), DEVELOP_COMBINE_INV },
        { "", 0 } };

const dt_iop_gui_blendif_colorstop_t _gradient_L[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

// The values for "a" are generated in the following way:
//   Lab (with L=[90 to 68], b=0, and a=[-56 to 56] -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
const dt_iop_gui_blendif_colorstop_t _gradient_a[] = {
    { 0.000f, { 0.0112790f, 0.7500000f, 0.5609999f, 1.0f } },
    { 0.250f, { 0.2888855f, 0.7500000f, 0.6318934f, 1.0f } },
    { 0.375f, { 0.4872486f, 0.7500000f, 0.6825501f, 1.0f } },
    { 0.500f, { 0.7500000f, 0.7499399f, 0.7496052f, 1.0f } },
    { 0.625f, { 0.7500000f, 0.5054633f, 0.5676756f, 1.0f } },
    { 0.750f, { 0.7500000f, 0.3423850f, 0.4463195f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.1399815f, 0.2956989f, 1.0f } },
};

// The values for "b" are generated in the following way:
//   Lab (with L=[58 to 62], a=0, and b=[-65 to 65] -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
const dt_iop_gui_blendif_colorstop_t _gradient_b[] = {
    { 0.000f, { 0.0162050f, 0.1968228f, 0.7500000f, 1.0f } },
    { 0.250f, { 0.2027354f, 0.3168822f, 0.7500000f, 1.0f } },
    { 0.375f, { 0.3645722f, 0.4210476f, 0.7500000f, 1.0f } },
    { 0.500f, { 0.6167146f, 0.5833379f, 0.7500000f, 1.0f } },
    { 0.625f, { 0.7500000f, 0.6172369f, 0.5412091f, 1.0f } },
    { 0.750f, { 0.7500000f, 0.5590797f, 0.3071980f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.4963975f, 0.0549797f, 1.0f } },
};

const dt_iop_gui_blendif_colorstop_t _gradient_gray[]
    = { { 0.0f,   { 0, 0, 0, 1.0 } },
        { 0.125f, { NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, NEUTRAL_GRAY / 8, 1.0 } },
        { 0.25f,  { NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, NEUTRAL_GRAY / 4, 1.0 } },
        { 0.5f,   { NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, NEUTRAL_GRAY / 2, 1.0 } },
        { 1.0f,   { NEUTRAL_GRAY, NEUTRAL_GRAY, NEUTRAL_GRAY, 1.0 } } };

const dt_iop_gui_blendif_colorstop_t _gradient_red[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0937500f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.250f, { 0.1875000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.500f, { 0.3750000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.0000000f, 0.0000000f, 1.0f } }
};

const dt_iop_gui_blendif_colorstop_t _gradient_green[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0000000f, 0.0937500f, 0.0000000f, 1.0f } },
    { 0.250f, { 0.0000000f, 0.1875000f, 0.0000000f, 1.0f } },
    { 0.500f, { 0.0000000f, 0.3750000f, 0.0000000f, 1.0f } },
    { 1.000f, { 0.0000000f, 0.7500000f, 0.0000000f, 1.0f } }
};

const dt_iop_gui_blendif_colorstop_t _gradient_blue[] = {
    { 0.000f, { 0.0000000f, 0.0000000f, 0.0000000f, 1.0f } },
    { 0.125f, { 0.0000000f, 0.0000000f, 0.0937500f, 1.0f } },
    { 0.250f, { 0.0000000f, 0.0000000f, 0.1875000f, 1.0f } },
    { 0.500f, { 0.0000000f, 0.0000000f, 0.3750000f, 1.0f } },
    { 1.000f, { 0.0000000f, 0.0000000f, 0.7500000f, 1.0f } }
};

// The chroma values are displayed in a gradient from {0.5,0.5,0.5} to {0.5,0.0,0.5} (pink)
const dt_iop_gui_blendif_colorstop_t _gradient_chroma[] = {
    { 0.000f, { 0.5000000f, 0.5000000f, 0.5000000f, 1.0f } },
    { 0.125f, { 0.5000000f, 0.4375000f, 0.5000000f, 1.0f } },
    { 0.250f, { 0.5000000f, 0.3750000f, 0.5000000f, 1.0f } },
    { 0.500f, { 0.5000000f, 0.2500000f, 0.5000000f, 1.0f } },
    { 1.000f, { 0.5000000f, 0.0000000f, 0.5000000f, 1.0f } }
};

// The hue values for LCh are generated in the following way:
//   LCh (with L=65 and C=37) -> sRGB (D65 linear) -> normalize with MAX(R,G,B) = 0.75
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_LCh_hue[] = {
    { 0.000f, { 0.7500000f, 0.2200405f, 0.4480174f, 1.0f } },
    { 0.104f, { 0.7500000f, 0.2475123f, 0.2488547f, 1.0f } },
    { 0.200f, { 0.7500000f, 0.3921083f, 0.2017670f, 1.0f } },
    { 0.295f, { 0.7500000f, 0.7440329f, 0.3011876f, 1.0f } },
    { 0.377f, { 0.3813996f, 0.7500000f, 0.3799668f, 1.0f } },
    { 0.503f, { 0.0747526f, 0.7500000f, 0.7489037f, 1.0f } },
    { 0.650f, { 0.0282981f, 0.3736209f, 0.7500000f, 1.0f } },
    { 0.803f, { 0.2583821f, 0.2591069f, 0.7500000f, 1.0f } },
    { 0.928f, { 0.7500000f, 0.2788102f, 0.7492077f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.2200405f, 0.4480174f, 1.0f } },
};

// The hue values for HSL are generated in the following way:
//   HSL (with S=0.5 and L=0.5) -> any RGB(linear) -> (normalize with MAX(R,G,B) = 0.75)
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_HSL_hue[] = {
    { 0.000f, { 0.7500000f, 0.2500000f, 0.2500000f, 1.0f } },
    { 0.167f, { 0.7500000f, 0.7500000f, 0.2500000f, 1.0f } },
    { 0.333f, { 0.2500000f, 0.7500000f, 0.2500000f, 1.0f } },
    { 0.500f, { 0.2500000f, 0.7500000f, 0.7500000f, 1.0f } },
    { 0.667f, { 0.2500000f, 0.2500000f, 0.7500000f, 1.0f } },
    { 0.833f, { 0.7500000f, 0.2500000f, 0.7500000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.2500000f, 0.2500000f, 1.0f } },
};

// The hue values for JzCzhz are generated in the following way:
//   JzCzhz (with Jz=0.011 and Cz=0.01) -> sRGB(D65 linear) -> normalize with MAX(R,G,B) = 0.75
// Please keep in sync with the display in the gamma module
const dt_iop_gui_blendif_colorstop_t _gradient_JzCzhz_hue[] = {
    { 0.000f, { 0.7500000f, 0.1946971f, 0.3697612f, 1.0f } },
    { 0.082f, { 0.7500000f, 0.2278141f, 0.2291548f, 1.0f } },
    { 0.150f, { 0.7500000f, 0.3132381f, 0.1653960f, 1.0f } },
    { 0.275f, { 0.7483232f, 0.7500000f, 0.1939316f, 1.0f } },
    { 0.378f, { 0.2642865f, 0.7500000f, 0.2642768f, 1.0f } },
    { 0.570f, { 0.0233180f, 0.7493543f, 0.7500000f, 1.0f } },
    { 0.650f, { 0.1119025f, 0.5116763f, 0.7500000f, 1.0f } },
    { 0.762f, { 0.3331225f, 0.3337235f, 0.7500000f, 1.0f } },
    { 0.883f, { 0.7464700f, 0.2754816f, 0.7500000f, 1.0f } },
    { 1.000f, { 0.7500000f, 0.1946971f, 0.3697612f, 1.0f } },
};

enum _channel_indexes
{
  CHANNEL_INDEX_L = 0,
  CHANNEL_INDEX_a = 1,
  CHANNEL_INDEX_b = 2,
  CHANNEL_INDEX_C = 3,
  CHANNEL_INDEX_h = 4,
  CHANNEL_INDEX_g = 0,
  CHANNEL_INDEX_R = 1,
  CHANNEL_INDEX_G = 2,
  CHANNEL_INDEX_B = 3,
  CHANNEL_INDEX_H = 4,
  CHANNEL_INDEX_S = 5,
  CHANNEL_INDEX_l = 6,
  CHANNEL_INDEX_Jz = 4,
  CHANNEL_INDEX_Cz = 5,
  CHANNEL_INDEX_hz = 6,
};

static void _blendop_blendif_update_tab(dt_iop_module_t *module, const int tab);

static inline dt_iop_colorspace_type_t _blendif_colorpicker_cst(dt_iop_gui_blend_data_t *data)
{
  dt_iop_colorspace_type_t cst = dt_iop_color_picker_get_active_cst(data->module);
  if(cst == IOP_CS_NONE)
  {
    switch(data->channel_tabs_csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        cst = IOP_CS_LAB;
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
      case DEVELOP_BLEND_CS_RGB_SCENE:
        cst = IOP_CS_RGB;
        break;
      case DEVELOP_BLEND_CS_RAW:
      case DEVELOP_BLEND_CS_NONE:
        cst = IOP_CS_NONE;
        break;
    }
  }
  return cst;
}

static gboolean _blendif_blend_parameter_enabled(dt_develop_blend_colorspace_t csp, dt_develop_blend_mode_t mode)
{
  if(csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    switch(mode & ~DEVELOP_BLEND_REVERSE)
    {
      case DEVELOP_BLEND_ADD:
      case DEVELOP_BLEND_MULTIPLY:
      case DEVELOP_BLEND_SUBTRACT:
      case DEVELOP_BLEND_SUBTRACT_INVERSE:
      case DEVELOP_BLEND_DIVIDE:
      case DEVELOP_BLEND_DIVIDE_INVERSE:
      case DEVELOP_BLEND_RGB_R:
      case DEVELOP_BLEND_RGB_G:
      case DEVELOP_BLEND_RGB_B:
        return TRUE;
      default:
        return FALSE;
    }
  }
  return FALSE;
}

static inline float _get_boost_factor(const dt_iop_gui_blend_data_t *data, const int channel, const int in_out)
{
  return exp2f(data->module->blend_params->blendif_boost_factors[data->channel[channel].param_channels[in_out]]);
}

static void _blendif_scale(dt_iop_gui_blend_data_t *data, dt_iop_colorspace_type_t cst, const float *in,
                           float *out, const dt_iop_order_iccprofile_info_t *work_profile, int in_out)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case IOP_CS_LAB:
      out[CHANNEL_INDEX_L] = (in[0] / _get_boost_factor(data, 0, in_out)) / 100.0f;
      out[CHANNEL_INDEX_a] = ((in[1] / _get_boost_factor(data, 1, in_out)) + 128.0f) / 256.0f;
      out[CHANNEL_INDEX_b] = ((in[2] / _get_boost_factor(data, 2, in_out)) + 128.0f) / 256.0f;
      break;
    case IOP_CS_RGB:
    case IOP_CS_RGB_DISPLAY:
      if(IS_NULL_PTR(work_profile))
        out[CHANNEL_INDEX_g] = 0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2];
      else
        out[CHANNEL_INDEX_g] = dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in,
                                                                 work_profile->lut_in,
                                                                 work_profile->unbounded_coeffs_in,
                                                                 work_profile->lutsize,
                                                                 work_profile->nonlinearlut);
      out[CHANNEL_INDEX_g] = out[CHANNEL_INDEX_g] / _get_boost_factor(data, 0, in_out);
      out[CHANNEL_INDEX_R] = in[0] / _get_boost_factor(data, 1, in_out);
      out[CHANNEL_INDEX_G] = in[1] / _get_boost_factor(data, 2, in_out);
      out[CHANNEL_INDEX_B] = in[2] / _get_boost_factor(data, 3, in_out);
      break;
    case IOP_CS_LCH:
      out[CHANNEL_INDEX_C] = (in[1] / _get_boost_factor(data, 3, in_out)) / (128.0f * sqrtf(2.0f));
      out[CHANNEL_INDEX_h] = in[2] / _get_boost_factor(data, 4, in_out);
      break;
    case IOP_CS_HSL:
      out[CHANNEL_INDEX_H] = in[0] / _get_boost_factor(data, 4, in_out);
      out[CHANNEL_INDEX_S] = in[1] / _get_boost_factor(data, 5, in_out);
      out[CHANNEL_INDEX_l] = in[2] / _get_boost_factor(data, 6, in_out);
      break;
    case IOP_CS_JZCZHZ:
      out[CHANNEL_INDEX_Jz] = in[0] / _get_boost_factor(data, 4, in_out);
      out[CHANNEL_INDEX_Cz] = in[1] / _get_boost_factor(data, 5, in_out);
      out[CHANNEL_INDEX_hz] = in[2] / _get_boost_factor(data, 6, in_out);
      break;
    default:
      break;
  }
}

static void _blendif_cook(dt_iop_colorspace_type_t cst, const float *in, float *out,
                          const dt_iop_order_iccprofile_info_t *const work_profile)
{
  out[0] = out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = out[7] = -1.0f;

  switch(cst)
  {
    case IOP_CS_LAB:
      out[CHANNEL_INDEX_L] = in[0];
      out[CHANNEL_INDEX_a] = in[1];
      out[CHANNEL_INDEX_b] = in[2];
      break;
    case IOP_CS_RGB:
    case IOP_CS_RGB_DISPLAY:
      if(IS_NULL_PTR(work_profile))
        out[CHANNEL_INDEX_g] = (0.3f * in[0] + 0.59f * in[1] + 0.11f * in[2]) * 100.0f;
      else
        out[CHANNEL_INDEX_g] = dt_ioppr_get_rgb_matrix_luminance(in, work_profile->matrix_in,
                                                                 work_profile->lut_in,
                                                                 work_profile->unbounded_coeffs_in,
                                                                 work_profile->lutsize,
                                                                 work_profile->nonlinearlut) * 100.0f;
      out[CHANNEL_INDEX_R] = in[0] * 100.0f;
      out[CHANNEL_INDEX_G] = in[1] * 100.0f;
      out[CHANNEL_INDEX_B] = in[2] * 100.0f;
      break;
    case IOP_CS_LCH:
      out[CHANNEL_INDEX_C] = in[1] / (128.0f * sqrtf(2.0f)) * 100.0f;
      out[CHANNEL_INDEX_h] = in[2] * 360.0f;
      break;
    case IOP_CS_HSL:
      out[CHANNEL_INDEX_H] = in[0] * 360.0f;
      out[CHANNEL_INDEX_S] = in[1] * 100.0f;
      out[CHANNEL_INDEX_l] = in[2] * 100.0f;
      break;
    case IOP_CS_JZCZHZ:
      out[CHANNEL_INDEX_Jz] = in[0] * 100.0f;
      out[CHANNEL_INDEX_Cz] = in[1] * 100.0f;
      out[CHANNEL_INDEX_hz] = in[2] * 360.0f;
      break;
    default:
      break;
  }
}

static inline int _blendif_print_digits_default(float value)
{
  int digits;
  if(value < 0.0001f) digits = 0;
  else if(value < 0.01f) digits = 2;
  else if(value < 0.999f) digits = 1;
  else digits = 0;

  return digits;
}

static inline int _blendif_print_digits_ab(float value)
{
  int digits;
  if(fabsf(value) < 10.0f) digits = 1;
  else digits = 0;

  return digits;
}

static void _blendif_scale_print_ab(float value, float boost_factor, char *string, int n)
{
  const float scaled = (value * 256.0f - 128.0f) * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_ab(scaled), scaled);
}

static void _blendif_scale_print_hue(float value, float boost_factor, char *string, int n)
{
  snprintf(string, n, "%-5.0f", value * 360.0f);
}

static void _blendif_scale_print_default(float value, float boost_factor, char *string, int n)
{
  const float scaled = value * boost_factor;
  snprintf(string, n, "%-5.*f", _blendif_print_digits_default(scaled), scaled * 100.0f);
}

static gboolean _blendif_are_output_channels_used(const dt_develop_blend_params_t *const blend,
                                                  const dt_develop_blend_colorspace_t cst)
{
  const gboolean mask_inclusive = blend->mask_combine & DEVELOP_COMBINE_INCL;
  const uint32_t mask = cst == DEVELOP_BLEND_CS_LAB
    ? DEVELOP_BLENDIF_Lab_MASK & DEVELOP_BLENDIF_OUTPUT_MASK
    : DEVELOP_BLENDIF_RGB_MASK & DEVELOP_BLENDIF_OUTPUT_MASK;
  const uint32_t active_channels = blend->blendif & mask;
  const uint32_t inverted_channels = (blend->blendif >> 16) ^ (mask_inclusive ? mask : 0);
  const uint32_t cancel_channels = inverted_channels & ~blend->blendif & mask;
  return active_channels || cancel_channels;
}

static gboolean _blendif_clean_output_channels(dt_iop_module_t *module)
{
  const dt_iop_gui_blend_data_t *const bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return FALSE;

  gboolean changed = FALSE;
  if(!bd->output_channels_shown)
  {
    const uint32_t mask = bd->csp == DEVELOP_BLEND_CS_LAB
      ? DEVELOP_BLENDIF_Lab_MASK & DEVELOP_BLENDIF_OUTPUT_MASK
      : DEVELOP_BLENDIF_RGB_MASK & DEVELOP_BLENDIF_OUTPUT_MASK;

    dt_develop_blend_params_t *const d = module->blend_params;

    // clear the output channels and invert them when needed
    const uint32_t old_blendif = d->blendif;
    const uint32_t need_inversion = d->mask_combine & DEVELOP_COMBINE_INCL ? (mask << 16) : 0;

    d->blendif = (d->blendif & ~(mask | (mask << 16))) | need_inversion;

    changed = (d->blendif != old_blendif);

    for (size_t ch = 0; ch < DEVELOP_BLENDIF_SIZE; ch++)
    {
      if ((DEVELOP_BLENDIF_OUTPUT_MASK & (1 << ch))
          && (   d->blendif_parameters[ch * 4 + 0] != 0.0f
              || d->blendif_parameters[ch * 4 + 1] != 0.0f
              || d->blendif_parameters[ch * 4 + 2] != 1.0f
              || d->blendif_parameters[ch * 4 + 3] != 1.0f))
      {
        changed = TRUE;
        d->blendif_parameters[ch * 4 + 0] = 0.0f;
        d->blendif_parameters[ch * 4 + 1] = 0.0f;
        d->blendif_parameters[ch * 4 + 2] = 1.0f;
        d->blendif_parameters[ch * 4 + 3] = 1.0f;
      }
    }
  }
  return changed;
}

static void _blendop_masks_mode_callback(const unsigned int mask_mode, dt_iop_gui_blend_data_t *data)
{
  data->module->blend_params->mask_mode = mask_mode;

  dt_iop_set_mask_mode(data->module, mask_mode);

  if(data->blending_body_box)
    dt_iop_gui_update_blending(data->module);
}

static void _blendop_blend_mode_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;
  dt_develop_blend_mode_t new_blend_mode = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(combo));
  if(new_blend_mode != (bp->blend_mode & DEVELOP_BLEND_MODE_MASK))
  {
    bp->blend_mode = new_blend_mode | (bp->blend_mode & DEVELOP_BLEND_REVERSE);
    if(_blendif_blend_parameter_enabled(data->blend_modes_csp, bp->blend_mode))
    {
      gtk_widget_set_sensitive(data->blend_mode_parameter_slider, TRUE);
    }
    else
    {
      bp->blend_parameter = 0.0f;
      dt_bauhaus_slider_set(data->blend_mode_parameter_slider, bp->blend_parameter);
      gtk_widget_set_sensitive(data->blend_mode_parameter_slider, FALSE);
    }
    dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
  }
}

static gboolean _blendop_blend_order_clicked(GtkWidget *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)module->blend_params;
  const gboolean active = !(bp->blend_mode & DEVELOP_BLEND_REVERSE);
  if(!active)
    bp->blend_mode &= ~DEVELOP_BLEND_REVERSE;
  else
    bp->blend_mode |= DEVELOP_BLEND_REVERSE;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);

  dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(button));

  return TRUE;
}

static void _blendop_masks_combine_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  dt_develop_blend_params_t *const d = data->module->blend_params;

  const unsigned combine = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_combine_combo));
  d->mask_combine &= ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  d->mask_combine |= combine;

  // inverts the parametric mask channels that are not used
  if(data->blendif_support && data->blendif_inited)
  {
    const uint32_t mask = data->csp == DEVELOP_BLEND_CS_LAB ? DEVELOP_BLENDIF_Lab_MASK : DEVELOP_BLENDIF_RGB_MASK;
    const uint32_t unused_channels = mask & ~d->blendif;
    d->blendif &= ~(unused_channels << 16);
    if(d->mask_combine & DEVELOP_COMBINE_INCL)
    {
      d->blendif |= unused_channels << 16;
    }
    _blendop_blendif_update_tab(data->module, data->tab);
  }

  _blendif_clean_output_channels(data->module);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
}

static void _blendop_masks_invert_callback(GtkWidget *combo, dt_iop_gui_blend_data_t *data)
{
  unsigned int invert = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(data->masks_invert_combo))
                        & DEVELOP_COMBINE_INV;
  if(invert)
    data->module->blend_params->mask_combine |= DEVELOP_COMBINE_INV;
  else
    data->module->blend_params->mask_combine &= ~DEVELOP_COMBINE_INV;
  _blendif_clean_output_channels(data->module);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
}

static void _blendop_blendif_sliders_callback(GtkDarktableGradientSlider *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (slider == data->filter[1].slider) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
  GtkLabel **label = data->filter[in_out].label;

  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker))
     && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values)))
  {
    dt_iop_color_picker_reset(data->module, FALSE);
  }

  float *parameters = &(bp->blendif_parameters[4 * ch]);

  dt_pthread_mutex_lock(&data->lock);
  for(int k = 0; k < 4; k++) parameters[k] = dtgtk_gradient_slider_multivalue_get_value(slider, k);
  dt_pthread_mutex_unlock(&data->lock);

  const float boost_factor = _get_boost_factor(data, data->tab, in_out);
  for(int k = 0; k < 4; k++)
  {
    char text[256];
    (channel->scale_print)(parameters[k], boost_factor, text, sizeof(text));
    gtk_label_set_text(label[k], text);
  }

  /** de-activate processing of this channel if maximum span is selected */
  if(parameters[1] == 0.0f && parameters[2] == 1.0f)
    bp->blendif &= ~(1 << ch);
  else
    bp->blendif |= (1 << ch);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
}

static void _blendop_blendif_sliders_reset_callback(GtkDarktableGradientSlider *slider,
                                                    dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (slider == data->filter[1].slider) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];

  // invert the parametric mask if needed
  if(bp->mask_combine & DEVELOP_COMBINE_INCL)
    bp->blendif |= (1 << (16 + ch));
  else
    bp->blendif &= ~(1 << (16 + ch));

  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
  _blendop_blendif_update_tab(data->module, data->tab);
}

static void _blendop_blendif_polarity_callback(GtkToggleButton *togglebutton, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset) return;

  int active = gtk_toggle_button_get_active(togglebutton);

  dt_develop_blend_params_t *bp = data->module->blend_params;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];

  const int in_out = (GTK_WIDGET(togglebutton) == data->filter[1].polarity) ? 1 : 0;
  dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
  GtkDarktableGradientSlider *slider = data->filter[in_out].slider;

  if(!active)
    bp->blendif |= (1 << (ch + 16));
  else
    bp->blendif &= ~(1 << (ch + 16));

  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
  dtgtk_gradient_slider_multivalue_set_marker(
      slider, active ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

static float log10_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (log10(CLAMP(inval, 0.0001f, 1.0f)) + 4.0f) / 4.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = CLAMP(exp(M_LN10 * (4.0f * inval - 4.0f)), 0.0f, 1.0f);
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}


static float magnifier_scale_callback(GtkWidget *self, float inval, int dir)
{
  float outval;
  const float range = 6.0f;
  const float invrange = 1.0f/range;
  const float scale = tanh(range * 0.5f);
  const float invscale = 1.0f/scale;
  const float eps = 1.0e-6f;
  const float tiny = 1.0e-4f;
  switch(dir)
  {
    case GRADIENT_SLIDER_SET:
      outval = (invscale * tanh(range * (CLAMP(inval, 0.0f, 1.0f) - 0.5f)) + 1.0f) * 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    case GRADIENT_SLIDER_GET:
      outval = invrange * atanh((2.0f * CLAMP(inval, eps, 1.0f - eps) - 1.0f) * scale) + 0.5f;
      if(outval <= tiny) outval = 0.0f;
      if(outval >= 1.0f - tiny) outval = 1.0f;
      break;
    default:
      outval = inval;
  }
  return outval;
}

static int _blendop_blendif_disp_alternative_worker(GtkWidget *widget, dt_iop_module_t *module, int mode,
                                                    float (*scale_callback)(GtkWidget*, float, int), const char *label)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  GtkDarktableGradientSlider *slider = (GtkDarktableGradientSlider *)widget;

  int in_out = (slider == data->filter[1].slider) ? 1 : 0;

  dtgtk_gradient_slider_multivalue_set_scale_callback(slider, (mode == 1) ? scale_callback : NULL);
  gchar *text = g_strdup_printf("%s%s",
                                (in_out == 0) ? _("input") : _("output"),
                                (mode == 1) ? label : "");
  gtk_label_set_text(data->filter[in_out].head, text);
  dt_free(text);

  return (mode == 1) ? 1 : 0;
}


static int _blendop_blendif_disp_alternative_mag(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, magnifier_scale_callback, _(" (zoom)"));
}

static int _blendop_blendif_disp_alternative_log(GtkWidget *widget, dt_iop_module_t *module, int mode)
{
  return _blendop_blendif_disp_alternative_worker(widget, module, mode, log10_scale_callback, _(" (log)"));
}

static void _blendop_blendif_disp_alternative_reset(GtkWidget *widget, dt_iop_module_t *module)
{
  (void) _blendop_blendif_disp_alternative_worker(widget, module, 0, NULL, "");
}


static dt_iop_colorspace_type_t _blendop_blendif_get_picker_colorspace(dt_iop_gui_blend_data_t *bd)
{
  dt_iop_colorspace_type_t picker_cst = IOP_CS_NONE;

  if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
  {
    if(bd->tab < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_HSL;
  }
  else if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    if(bd->tab < 4)
      picker_cst = IOP_CS_RGB;
    else
      picker_cst = IOP_CS_JZCZHZ;
  }
  else if(bd->channel_tabs_csp == DEVELOP_BLEND_CS_LAB)
  {
    if(bd->tab < 3)
      picker_cst = IOP_CS_LAB;
    else
      picker_cst = IOP_CS_LCH;
  }

  return picker_cst;
}

static inline int _blendif_print_digits_picker(float value)
{
  int digits;
  if(value < 10.0f) digits = 2;
  else digits = 1;

  return digits;
}

static void _update_gradient_slider_pickers(GtkWidget *callback_dummy, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_iop_color_picker_set_cst(module, _blendop_blendif_get_picker_colorspace(data));

  float *raw_mean, *raw_min, *raw_max;

  ++darktable.gui->reset;

  for(int in_out = 1; in_out >= 0; in_out--)
  {
    if(in_out)
    {
      raw_mean = module->picked_output_color;
      raw_min = module->picked_output_color_min;
      raw_max = module->picked_output_color_max;
    }
    else
    {
      raw_mean = module->picked_color;
      raw_min = module->picked_color_min;
      raw_max = module->picked_color_max;
    }

    if((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker)) ||
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))) &&
       (raw_min[0] != FLT_MAX))
    {
      float picker_mean[8], picker_min[8], picker_max[8];
      float cooked[8];

      const dt_develop_blend_colorspace_t blend_csp = data->channel_tabs_csp;
      const dt_iop_colorspace_type_t cst = _blendif_colorpicker_cst(data);
      const dt_iop_order_iccprofile_info_t *work_profile = (blend_csp == DEVELOP_BLEND_CS_RGB_SCENE)
          ? dt_ioppr_get_pipe_current_profile_info(module, module->dev->pipe)
          : dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);

      _blendif_scale(data, cst, raw_mean, picker_mean, work_profile, in_out);
      _blendif_scale(data, cst, raw_min, picker_min, work_profile, in_out);
      _blendif_scale(data, cst, raw_max, picker_max, work_profile, in_out);
      _blendif_cook(cst, raw_mean, cooked, work_profile);

      gchar *text = g_strdup_printf("(%.*f)", _blendif_print_digits_picker(cooked[data->tab]), cooked[data->tab]);

      dtgtk_gradient_slider_multivalue_set_picker_meanminmax(
          data->filter[in_out].slider,
          CLAMP(picker_mean[data->tab], 0.0f, 1.0f),
          CLAMP(picker_min[data->tab], 0.0f, 1.0f),
          CLAMP(picker_max[data->tab], 0.0f, 1.0f));
      gtk_label_set_text(data->filter[in_out].picker_label, text);

      dt_free(text);
    }
    else
    {
      dtgtk_gradient_slider_multivalue_set_picker(data->filter[in_out].slider, NAN);
      gtk_label_set_text(data->filter[in_out].picker_label, "");
    }
  }

  --darktable.gui->reset;
}


static void _blendop_blendif_update_tab(dt_iop_module_t *module, const int tab)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;
  dt_develop_blend_params_t *dp = module->default_blendop_params;

  ++darktable.gui->reset;

  const dt_iop_gui_blendif_channel_t *channel = &data->channel[tab];

  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &data->filter[in_out];

    float *parameters = &(bp->blendif_parameters[4 * ch]);
    float *defaults = &(dp->blendif_parameters[4 * ch]);

    const int polarity = !(bp->blendif & (1 << (ch + 16)));

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sl->polarity), polarity);

    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 0);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 1);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG : GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG, 2);
    dtgtk_gradient_slider_multivalue_set_marker(
        sl->slider,
        polarity ? GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG : GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG, 3);

    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
    {
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, parameters[k], k);
      dtgtk_gradient_slider_multivalue_set_resetvalue(sl->slider, defaults[k], k);
    }
    dt_pthread_mutex_unlock(&data->lock);

    const float boost_factor = _get_boost_factor(data, tab, in_out);
    for(int k = 0; k < 4; k++)
    {
      char text[256];
      channel->scale_print(parameters[k], boost_factor, text, sizeof(text));
      gtk_label_set_text(sl->label[k], text);
    }

    dtgtk_gradient_slider_multivalue_clear_stops(sl->slider);

    for(int k = 0; k < channel->numberstops; k++)
    {
      dtgtk_gradient_slider_multivalue_set_stop(sl->slider, channel->colorstops[k].stoppoint,
                                                channel->colorstops[k].color);
    }

    dtgtk_gradient_slider_multivalue_set_increment(sl->slider, channel->increment);

    if(channel->altdisplay)
    {
      data->altmode[tab][in_out] = channel->altdisplay(GTK_WIDGET(sl->slider), module, data->altmode[tab][in_out]);
    }
    else
    {
      _blendop_blendif_disp_alternative_reset(GTK_WIDGET(sl->slider), module);
    }
  }

  _update_gradient_slider_pickers(NULL, module);

  const gboolean boost_factor_enabled = channel->boost_factor_enabled;
  float boost_factor = 0.0f;
  if(boost_factor_enabled)
  {
    boost_factor = bp->blendif_boost_factors[channel->param_channels[0]] - channel->boost_factor_offset;
  }
  gtk_widget_set_sensitive(GTK_WIDGET(data->channel_boost_factor_slider), boost_factor_enabled);
  dt_bauhaus_slider_set(GTK_WIDGET(data->channel_boost_factor_slider), boost_factor);

  --darktable.gui->reset;
}


static void _blendop_blendif_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                        dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || IS_NULL_PTR(data) || !data->blendif_inited) return;
  const int cst_old = _blendop_blendif_get_picker_colorspace(data);
  dt_iop_color_picker_reset(data->module, FALSE);

  data->tab = page_num;

  if(cst_old != _blendop_blendif_get_picker_colorspace(data)
     && (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker))
         || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->colorpicker_set_values))))
  {
    dt_iop_color_picker_set_cst(data->module, _blendop_blendif_get_picker_colorspace(data));
    dt_dev_pixelpipe_update_history_all(data->module->dev);
  }

  _blendop_blendif_update_tab(data->module, data->tab);
}

/**
 * @brief Persist the active blending notebook tab in anselrc.
 *
 * The blending UI is rebuilt per module instance, so we store only the last-used
 * page index to restore a consistent view across sessions.
 */
static void _blendop_blending_notebook_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                              dt_iop_gui_blend_data_t *data)
{
  (void)notebook;
  (void)page;
  if(IS_NULL_PTR(data)) return;

  dt_conf_set_int(BLEND_MASKMODE_CONF_KEY, (int)page_num);
}

static void _blendop_blendif_boost_factor_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || IS_NULL_PTR(data) || !data->blendif_inited) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;
  const int tab = data->tab;

  const float value = dt_bauhaus_slider_get(slider);
  for(int in_out = 1; in_out >= 0; in_out--)
  {
    const int ch = data->channel[tab].param_channels[in_out];
    float off = 0.0f;
    if(data->csp == DEVELOP_BLEND_CS_LAB && (ch == DEVELOP_BLENDIF_A_in || ch == DEVELOP_BLENDIF_A_out
        || ch == DEVELOP_BLENDIF_B_in || ch == DEVELOP_BLENDIF_B_out))
    {
      off = 0.5f;
    }
    const float new_value = value + data->channel[tab].boost_factor_offset;
    const float old_value = bp->blendif_boost_factors[ch];
    const float factor = exp2f(old_value) / exp2f(new_value);
    float *parameters = &(bp->blendif_parameters[4 * ch]);
    if(parameters[0] > 0.0f) parameters[0] = clamp_range_f((parameters[0] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[1] > 0.0f) parameters[1] = clamp_range_f((parameters[1] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[2] < 1.0f) parameters[2] = clamp_range_f((parameters[2] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[3] < 1.0f) parameters[3] = clamp_range_f((parameters[3] - off) * factor + off, 0.0f, 1.0f);
    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      bp->blendif &= ~(1 << ch);
    bp->blendif_boost_factors[ch] = new_value;
  }
  _blendop_blendif_update_tab(data->module, tab);

  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
}

static void _blendop_blendif_details_callback(GtkWidget *slider, dt_iop_gui_blend_data_t *data)
{
  if(darktable.gui->reset || IS_NULL_PTR(data) || !data->blendif_inited) return;
  dt_develop_blend_params_t *bp = data->module->blend_params;
  const float oldval = bp->details;
  bp->details = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);

  if((oldval == 0.0f) && (bp->details != 0.0f))
  {
    dt_dev_pixelpipe_update_history_all(data->module->dev);
  }
}

static gboolean _blendop_blendif_showmask_clicked(GtkToggleButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.
  if(darktable.gui->reset) return TRUE;

  if(event->button == 1)
  {
    const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);

    module->request_mask_display &= ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_ANY);

    if(dt_modifier_is(event->state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
      module->request_mask_display |= (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
    else if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
    else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;
    else
      module->request_mask_display |= (has_mask_display ? 0 : DT_DEV_PIXELPIPE_DISPLAY_MASK);

    gtk_toggle_button_set_active(button,
                                 module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

    module->enabled = TRUE;
    if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

    ++darktable.gui->reset;
    // (re)set the header mask indicator too
    if(module->mask_indicator)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->mask_indicator),
                                   module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    --darktable.gui->reset;

    dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    dt_iop_request_focus(module);

    // We don't want to re-read the history here
    dt_dev_pixelpipe_update_zoom_main(module->dev);
  }

  return TRUE;
}

static void _blendop_masks_mode_changed(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  const unsigned int bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(togglebutton), "mask-bit"));
  if(!bit) return;
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->blend_data)) return;

  uint32_t mask_mode = module->blend_params->mask_mode;
  const gboolean active = gtk_toggle_button_get_active(togglebutton);

  switch(bit)
  {
    case DEVELOP_MASK_ENABLED:
      if(active)
        mask_mode |= DEVELOP_MASK_ENABLED;
      else
        mask_mode &= ~DEVELOP_MASK_ENABLED;
      break;

    case DEVELOP_MASK_RASTER:
      if(active)
        mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
      else
        mask_mode &= ~DEVELOP_MASK_RASTER;
      break;

    case DEVELOP_MASK_MASK:
      if(active)
        mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
      else
        mask_mode &= ~DEVELOP_MASK_MASK;
      break;

    case DEVELOP_MASK_CONDITIONAL:
      if(active)
        mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
      else
        mask_mode &= ~DEVELOP_MASK_CONDITIONAL;
      break;

    default:
      break;
  }

  dt_iop_gui_blend_data_t *data = module->blend_data;
  _blendop_masks_mode_callback(mask_mode, data);
  dt_iop_add_remove_mask_indicator(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED);
  dt_dev_add_history_item(darktable.develop, data->module, TRUE, TRUE);
}


static gboolean _blendop_blendif_suppress_toggled(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *module)
{
  module->suppress_mask = !gtk_toggle_button_get_active(togglebutton);
  if(darktable.gui->reset) return FALSE;

  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);
  dt_iop_request_focus(module);

  gtk_toggle_button_set_active(togglebutton, module->suppress_mask);

  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
  dt_dev_pixelpipe_update_history_main(module->dev);

  return TRUE;
}

static gboolean _blendop_blendif_reset(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  module->blend_params->blendif = module->default_blendop_params->blendif;
  memcpy(module->blend_params->blendif_parameters, module->default_blendop_params->blendif_parameters,
         4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
  module->blend_params->details = module->default_blendop_params->details;

  dt_iop_color_picker_reset(module, FALSE);
  dt_iop_gui_update_blendif(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);

  return TRUE;
}

static gboolean _blendop_blendif_invert(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return TRUE;

  const dt_iop_gui_blend_data_t *data = module->blend_data;

  unsigned int toggle_mask = 0;

  switch(data->channel_tabs_csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      toggle_mask = DEVELOP_BLENDIF_Lab_MASK << 16;
      break;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      toggle_mask = DEVELOP_BLENDIF_RGB_MASK << 16;
      break;
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_NONE:
      toggle_mask = 0;
      break;
  }

  module->blend_params->blendif ^= toggle_mask;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_MASKS_POS;
  module->blend_params->mask_combine ^= DEVELOP_COMBINE_INCL;
  dt_iop_gui_update_blending(module);
  dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);

  return TRUE;
}

static gboolean _blendop_masks_add_shape(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset || event->button != GDK_BUTTON_PRIMARY) return TRUE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  // find out who we are
  int this = -1;
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
  {
    if(widget == bd->masks_shapes[n])
    {
      this = n;
      break;
    }
  }

  if(this < 0) return FALSE;

  // set all shape buttons to inactive
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

  // we want to be sure that the iop has focus
  dt_iop_request_focus(self);
  dt_iop_color_picker_reset(self, FALSE);
  bd->masks_shown = DT_MASKS_EDIT_FULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
  if(GTK_IS_TOGGLE_BUTTON(bd->masks_edit))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
  // we create the new form
  dt_masks_creation_mode(self, bd->masks_type[this]);
  dt_control_queue_redraw_center();

  return TRUE;
}

static gboolean _blendop_masks_show_and_edit(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)self->blend_data;

  if(event->button == 1)
  {
    ++darktable.gui->reset;

    dt_iop_request_focus(self);
    dt_iop_color_picker_reset(self, FALSE);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, self->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP) && grp->points)
    {
      const gboolean control_button_pressed = dt_modifier_is(event->state, GDK_CONTROL_MASK);

      switch(bd->masks_shown)
      {
        case DT_MASKS_EDIT_FULL:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_OFF;
          break;

        case DT_MASKS_EDIT_RESTRICTED:
          bd->masks_shown = !control_button_pressed ? DT_MASKS_EDIT_FULL : DT_MASKS_EDIT_OFF;
          break;

        default:
        case DT_MASKS_EDIT_OFF:
          bd->masks_shown = control_button_pressed ? DT_MASKS_EDIT_RESTRICTED : DT_MASKS_EDIT_FULL;
      }
    }
    else
    {
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      /* remove hinter messages */
      dt_control_hinter_message(darktable.control, "");
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);
    dt_masks_set_edit_mode(self, bd->masks_shown);

    // set all add shape buttons to inactive
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);

    --darktable.gui->reset;

    return TRUE;
  }

  return FALSE;
}

static gboolean _blendop_masks_polarity_callback(GtkToggleButton *togglebutton, GdkEventButton *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  const int active = !gtk_toggle_button_get_active(togglebutton);
  gtk_toggle_button_set_active(togglebutton, active);

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  if(active)
    bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
  else
    bp->mask_combine &= ~DEVELOP_COMBINE_MASKS_POS;

  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));

  return TRUE;
}

typedef enum _blendop_masks_all_cols_t
{
  BLENDOP_MASKS_ALL_COL_ACTIVE = 0,
  BLENDOP_MASKS_ALL_COL_NAME,
  BLENDOP_MASKS_ALL_COL_FORMID,
  BLENDOP_MASKS_ALL_COL_SENSITIVE,
  BLENDOP_MASKS_ALL_COL_MARKUP,
  BLENDOP_MASKS_ALL_COL_STATUS_MARKUP,
  BLENDOP_MASKS_ALL_COL_COUNT
} _blendop_masks_all_cols_t;

typedef enum _blendop_masks_group_cols_t
{
  BLENDOP_MASKS_GROUP_COL_OP_ICON = 0,
  BLENDOP_MASKS_GROUP_COL_INV_ICON,
  BLENDOP_MASKS_GROUP_COL_NAME,
  BLENDOP_MASKS_GROUP_COL_FORMID,
  BLENDOP_MASKS_GROUP_COL_PARENTID,
  BLENDOP_MASKS_GROUP_COL_STATE,
  BLENDOP_MASKS_GROUP_COL_INDEX,
  BLENDOP_MASKS_GROUP_COL_COUNT
} _blendop_masks_group_cols_t;

static dt_masks_form_t *_blendop_masks_group_from_module(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return NULL;
  if(IS_NULL_PTR(module->blend_params)) return NULL;
  return dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
}

static void _blendop_masks_check_id(dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return;

  int new_form_id = 100;
  for(GList *form_node = darktable.develop->forms; form_node;)
  {
    const dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->formid == mask_form->formid)
    {
      mask_form->formid = new_form_id++;
      form_node = darktable.develop->forms;
    }
    else
    {
      form_node = g_list_next(form_node);
    }
  }
}

static dt_masks_form_t *_blendop_masks_group_create(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return NULL;

  dt_masks_form_t *group_form = dt_masks_create(DT_MASKS_GROUP);
  if(IS_NULL_PTR(group_form)) return NULL;

  //gchar *module_label = dt_history_item_get_name(module);
  gchar *module_label = g_strdup(module->multi_name);
  if(g_strcmp0(module_label, "") == 0)
  {
    dt_free(module_label);
    module_label = dt_history_item_get_name(module);
  }
  g_snprintf(group_form->name, sizeof(group_form->name), "%s %s", _("Mask"), module_label);
  dt_free(module_label);

  _blendop_masks_check_id(group_form);
  dt_masks_append_form(darktable.develop, group_form);
  module->blend_params->mask_id = group_form->formid;

  return group_form;
}

static dt_masks_form_group_t *_blendop_masks_find_group_entry(dt_masks_form_t *group_form, const int formid, int *index)
{
  if(!IS_NULL_PTR(index)) *index = -1;
  if(IS_NULL_PTR(group_form)) return NULL;
  if(!(group_form->type & DT_MASKS_GROUP)) return NULL;

  int group_index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    if(group_entry->formid == formid)
    {
      if(index) *index = group_index;
      return group_entry;
    }
    group_index++;
  }

  return NULL;
}

static void _blendop_masks_init_icons(dt_iop_gui_blend_data_t *bd)
{
  if(IS_NULL_PTR(bd)) return;

  // Only initialize icons if they haven't been created yet
  if(bd->masks_ic_inverse && bd->masks_ic_union && bd->masks_ic_intersection
     && bd->masks_ic_difference && bd->masks_ic_exclusion)
    return;

  const int icon_size = DT_PIXEL_APPLY_DPI(13);
  if(IS_NULL_PTR(bd->masks_ic_inverse))
    bd->masks_ic_inverse = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, icon_size, icon_size);
  if(IS_NULL_PTR(bd->masks_ic_union))
    bd->masks_ic_union = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, icon_size * 2, icon_size);
  if(IS_NULL_PTR(bd->masks_ic_intersection))
    bd->masks_ic_intersection = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, icon_size * 2, icon_size);
  if(IS_NULL_PTR(bd->masks_ic_difference))
    bd->masks_ic_difference = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, icon_size * 2, icon_size);
  if(IS_NULL_PTR(bd->masks_ic_exclusion))
    bd->masks_ic_exclusion = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, icon_size * 2, icon_size);
}

static const GdkPixbuf *_blendop_masks_get_op_icon(const dt_iop_gui_blend_data_t *bd, const int state,
                                                   const int index)
{
  if(IS_NULL_PTR(bd) || index == 0) return NULL;

  if(state & DT_MASKS_STATE_UNION) return bd->masks_ic_union;
  if(state & DT_MASKS_STATE_INTERSECTION) return bd->masks_ic_intersection;
  if(state & DT_MASKS_STATE_DIFFERENCE) return bd->masks_ic_difference;
  if(state & DT_MASKS_STATE_EXCLUSION) return bd->masks_ic_exclusion;
  return NULL;
}

static const GdkPixbuf *_blendop_masks_get_inverse_icon(const dt_iop_gui_blend_data_t *bd, const int state)
{
  return (bd && (state & DT_MASKS_STATE_INVERSE)) ? bd->masks_ic_inverse : NULL;
}

static int _blendop_masks_group_tree_append(const dt_iop_gui_blend_data_t *bd, GtkTreeStore *tree_store,
                                            GtkTreeIter *parent_iter, const dt_masks_form_t *parent_group);

// Check if this is a parent mask reusing a single parent mask (and no shapes).
// Such a wrapper is redundant and should be hidden: the child mask should be used directly instead.
static gboolean _blendop_masks_is_single_group_wrapper(const dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return FALSE;
  if(!(mask_form->type & DT_MASKS_GROUP)) return FALSE;
  if(IS_NULL_PTR(mask_form->points)) return FALSE;
  
  // Check if there is exactly one item
  if(g_list_length(mask_form->points) != 1) return FALSE;
  
  // Get the single child
  const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)mask_form->points->data;
  if(IS_NULL_PTR(group_entry)) return FALSE;
  
  const dt_masks_form_t *child_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
  if(IS_NULL_PTR(child_form)) return FALSE;
  
  // Check if the single child is a group
  return (child_form->type & DT_MASKS_GROUP) ? TRUE : FALSE;
}

static gboolean _blendop_masks_is_group_with_shapes(const dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return FALSE;
  if(!(mask_form->type & DT_MASKS_GROUP)) return FALSE;
  if(IS_NULL_PTR(mask_form->points)) return FALSE;

  for(const GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)group_node->data;
    if(IS_NULL_PTR(group_entry)) continue;

    const dt_masks_form_t *child_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
    if(IS_NULL_PTR(child_form)) continue;
    if(!(child_form->type & DT_MASKS_GROUP)) return TRUE;
    if(_blendop_masks_is_group_with_shapes(child_form)) return TRUE;
  }

  return FALSE;
}

static int _blendop_masks_group_tree_append_entry(const dt_iop_gui_blend_data_t *bd, GtkTreeStore *tree_store,
                                                  GtkTreeIter *parent_iter,
                                                  const dt_masks_form_group_t *group_entry,
                                                  const dt_masks_form_t *mask_form, const int index)
{
  if(IS_NULL_PTR(bd) || IS_NULL_PTR(tree_store) || IS_NULL_PTR(group_entry) || IS_NULL_PTR(mask_form)) return 0;

  GtkTreeIter iter;
  gtk_tree_store_append(tree_store, &iter, parent_iter);
  gtk_tree_store_set(tree_store, &iter, BLENDOP_MASKS_GROUP_COL_OP_ICON,
                     _blendop_masks_get_op_icon(bd, group_entry->state, index),
                     BLENDOP_MASKS_GROUP_COL_INV_ICON,
                     _blendop_masks_get_inverse_icon(bd, group_entry->state),
                     BLENDOP_MASKS_GROUP_COL_NAME, mask_form->name,
                     BLENDOP_MASKS_GROUP_COL_FORMID, group_entry->formid,
                     BLENDOP_MASKS_GROUP_COL_PARENTID, group_entry->parentid,
                     BLENDOP_MASKS_GROUP_COL_STATE, group_entry->state,
                     BLENDOP_MASKS_GROUP_COL_INDEX, index, -1);

  int row_count = 1;
  if(mask_form->type & DT_MASKS_GROUP)
    row_count += _blendop_masks_group_tree_append(bd, tree_store, &iter, mask_form);

  return row_count;
}

static int _blendop_masks_group_tree_append(const dt_iop_gui_blend_data_t *bd, GtkTreeStore *tree_store,
                                            GtkTreeIter *parent_iter, const dt_masks_form_t *parent_group)
{
  if(IS_NULL_PTR(bd)) return 0;
  if(IS_NULL_PTR(tree_store)) return 0;
  if(IS_NULL_PTR(parent_group)) return 0;
  if(!(parent_group->type & DT_MASKS_GROUP)) return 0;

  int row_count = 0;

  // First pass: groups containing shapes.
  int index = 0;
  for(const GList *group_node = parent_group->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)group_node->data;
    const dt_masks_form_t *mask_form = group_entry
                                           ? dt_masks_get_from_id(darktable.develop, group_entry->formid)
                                           : NULL;
    if(mask_form && _blendop_masks_is_group_with_shapes(mask_form))
      row_count += _blendop_masks_group_tree_append_entry(bd, tree_store, parent_iter, group_entry, mask_form,
                                                          index);
    index++;
  }

  // Second pass: all remaining entries.
  index = 0;
  for(const GList *group_node = parent_group->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)group_node->data;
    const dt_masks_form_t *mask_form = group_entry
                                           ? dt_masks_get_from_id(darktable.develop, group_entry->formid)
                                           : NULL;
    if(mask_form && !_blendop_masks_is_group_with_shapes(mask_form))
    {
      row_count += _blendop_masks_group_tree_append_entry(bd, tree_store, parent_iter, group_entry, mask_form,
                                                          index);
    }
    index++;
  }

  return row_count;
}

static gboolean _blendop_masks_find_iter_by_formid(GtkTreeModel *model, GtkTreeIter *iter,
                                                    const int formid_col, const int formid)
{
  if(IS_NULL_PTR(model) || IS_NULL_PTR(iter)) return FALSE;

  gboolean valid = gtk_tree_model_get_iter_first(model, iter);
  while(valid)
  {
    int current_formid = -1;
    gtk_tree_model_get(model, iter, formid_col, &current_formid, -1);
    if(current_formid == formid) return TRUE;
    valid = gtk_tree_model_iter_next(model, iter);
  }

  return FALSE;
}

static void _blendop_masks_all_selection_changed(GtkTreeSelection *selection, dt_iop_module_t *module);
static gboolean _blendop_masks_all_button_pressed(GtkWidget *treeview, GdkEventButton *event,
                                                  dt_iop_module_t *module);
static void _blendop_masks_group_selection_changed(GtkTreeSelection *selection, dt_iop_module_t *module);
static gboolean _blendop_masks_group_button_pressed(GtkWidget *treeview, GdkEventButton *event,
                                                    dt_iop_module_t *module);
static void _blendop_masks_group_name_activate(GtkEntry *entry, dt_iop_module_t *module);
static gboolean _blendop_masks_group_name_focus_out(GtkWidget *widget, GdkEventFocus *event,
                                                    dt_iop_module_t *module);

static void _blendop_masks_refresh_lists(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->blend_data)) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd)) return;
  if(!GTK_IS_TREE_VIEW(bd->masks_treeview)) return;
  if(!GTK_IS_TREE_VIEW(bd->masks_group_treeview)) return;

  GtkTreeModel *all_model = gtk_tree_view_get_model(GTK_TREE_VIEW(bd->masks_treeview));
  GtkTreeModel *group_model = gtk_tree_view_get_model(GTK_TREE_VIEW(bd->masks_group_treeview));
  if(!GTK_IS_LIST_STORE(all_model)) return;
  if(!GTK_IS_TREE_STORE(group_model)) return;

  // Block signals during model updates to prevent callbacks from accessing inconsistent state
  GtkTreeSelection *all_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(bd->masks_treeview));
  GtkTreeSelection *group_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(bd->masks_group_treeview));
  g_signal_handlers_block_by_func(all_selection, G_CALLBACK(_blendop_masks_all_selection_changed), module);
  g_signal_handlers_block_by_func(group_selection, G_CALLBACK(_blendop_masks_group_selection_changed), module);
  g_signal_handlers_block_by_func(bd->masks_treeview, G_CALLBACK(_blendop_masks_all_button_pressed), module);
  g_signal_handlers_block_by_func(bd->masks_group_treeview, G_CALLBACK(_blendop_masks_group_button_pressed), module);

  gtk_list_store_clear(GTK_LIST_STORE(all_model));
  gtk_tree_store_clear(GTK_TREE_STORE(group_model));

  dt_masks_form_t *group_form = _blendop_masks_group_from_module(module);

  // First pass: groups containing shapes first.
  for(const GList *form_node = darktable.develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_node->data;
    if(IS_NULL_PTR(mask_form)) continue;
    if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) continue;
    if(!IS_NULL_PTR(group_form) && mask_form->formid == group_form->formid) continue;
    if(!_blendop_masks_is_group_with_shapes(mask_form)) continue;
    // Skip groups containing only a single group
    if(_blendop_masks_is_single_group_wrapper(mask_form)) continue;

    const gboolean active = _blendop_masks_find_group_entry(group_form, mask_form->formid, NULL) != NULL;
    GtkTreeIter iter;
    gtk_list_store_append(GTK_LIST_STORE(all_model), &iter);
    gchar *display_markup = g_markup_printf_escaped("%s", mask_form->name);
    gtk_list_store_set(GTK_LIST_STORE(all_model), &iter, BLENDOP_MASKS_ALL_COL_ACTIVE, active,
                       BLENDOP_MASKS_ALL_COL_NAME, mask_form->name, BLENDOP_MASKS_ALL_COL_FORMID, mask_form->formid,
                       BLENDOP_MASKS_ALL_COL_SENSITIVE, TRUE,
               BLENDOP_MASKS_ALL_COL_MARKUP, display_markup,
               BLENDOP_MASKS_ALL_COL_STATUS_MARKUP, "",
                       -1);
    g_free(display_markup);
  }

  // Second pass: then all non-group (and empty-group) entries.
  for(const GList *form_node = darktable.develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_node->data;
    if(IS_NULL_PTR(mask_form)) continue;
    if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) continue;
    if(!IS_NULL_PTR(group_form) && mask_form->formid == group_form->formid) continue;
    if(_blendop_masks_is_group_with_shapes(mask_form)) continue;

    const gboolean active = _blendop_masks_find_group_entry(group_form, mask_form->formid, NULL) != NULL;
    gboolean sensitive = TRUE;
    const gchar *locked_group_name = NULL;
    
    // Check if this shape belongs to an active group
    for(const GList *parent_node = darktable.develop->forms; parent_node; parent_node = g_list_next(parent_node))
    {
      dt_masks_form_t *parent_form = (dt_masks_form_t *)parent_node->data;
      if(IS_NULL_PTR(parent_form) || !_blendop_masks_is_group_with_shapes(parent_form)) continue;
      
      const gboolean parent_active = _blendop_masks_find_group_entry(group_form, parent_form->formid, NULL) != NULL;
      if(parent_active && _blendop_masks_find_group_entry(parent_form, mask_form->formid, NULL))
      {
        sensitive = FALSE;
        locked_group_name = parent_form->name;
        break;
      }
    }

    const gboolean display_active = active || !sensitive;
    gchar *display_markup = NULL;
    gchar *status_markup = NULL;
    display_markup = g_markup_printf_escaped("%s", mask_form->name);

    if(!sensitive && !IS_NULL_PTR(locked_group_name) && *locked_group_name)
    {
      gchar *already_in = g_strdup_printf(_("Already in '%s'"), locked_group_name);
      status_markup = g_markup_printf_escaped("<i>%s</i>", already_in);
      g_free(already_in);
    }
    else
      status_markup = g_strdup("");

    GtkTreeIter iter;
    gtk_list_store_append(GTK_LIST_STORE(all_model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(all_model), &iter, BLENDOP_MASKS_ALL_COL_ACTIVE, display_active,
                       BLENDOP_MASKS_ALL_COL_NAME, mask_form->name, BLENDOP_MASKS_ALL_COL_FORMID, mask_form->formid,
                       BLENDOP_MASKS_ALL_COL_SENSITIVE, sensitive, BLENDOP_MASKS_ALL_COL_MARKUP, display_markup,
                       BLENDOP_MASKS_ALL_COL_STATUS_MARKUP, status_markup,
                       -1);
    g_free(display_markup);
    g_free(status_markup);
  }

  if(!IS_NULL_PTR(group_form) && (group_form->type & DT_MASKS_GROUP))
  {
    if(GTK_IS_ENTRY(bd->group_shapes_label))
      gtk_entry_set_text(GTK_ENTRY(bd->group_shapes_label), group_form->name);
    _blendop_masks_group_tree_append(bd, GTK_TREE_STORE(group_model), NULL, group_form);
  }

  // Unblock signals after model updates are complete
  g_signal_handlers_unblock_by_func(all_selection, G_CALLBACK(_blendop_masks_all_selection_changed), module);
  g_signal_handlers_unblock_by_func(group_selection, G_CALLBACK(_blendop_masks_group_selection_changed), module);
  g_signal_handlers_unblock_by_func(bd->masks_treeview, G_CALLBACK(_blendop_masks_all_button_pressed), module);
  g_signal_handlers_unblock_by_func(bd->masks_group_treeview, G_CALLBACK(_blendop_masks_group_button_pressed), module);
}

static void _blendop_masks_apply_and_commit(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->dev)) return;

  dt_masks_iop_update(module);
  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
  dt_control_queue_redraw_center();
}

static void _blendop_masks_group_name_commit(dt_iop_module_t *module, const gchar *new_text)
{
  if(IS_NULL_PTR(module)) return;
  dt_masks_form_t *group_form = _blendop_masks_group_from_module(module);
  if(IS_NULL_PTR(group_form)) return;

  gchar *mask_default_name = dt_dev_get_masks_group_name(module);
  gchar *text = (new_text && *new_text) ? g_strdup(new_text) : g_strdup(mask_default_name);
  g_free(mask_default_name);

  g_strlcpy(group_form->name, text, sizeof(group_form->name));
  _blendop_masks_apply_and_commit(module);
  g_free(text);

  _blendop_masks_refresh_lists(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, group_form->formid, 0,
                                DT_MASKS_EVENT_CHANGE);
}

static void _blendop_masks_group_name_activate(GtkEntry *entry, dt_iop_module_t *module)
{
  if(!GTK_IS_ENTRY(entry)) return;
  _blendop_masks_group_name_commit(module, gtk_entry_get_text(entry));
}

static gboolean _blendop_masks_group_name_focus_out(GtkWidget *widget, GdkEventFocus *event,
                                                    dt_iop_module_t *module)
{
  (void)event;
  if(GTK_IS_ENTRY(widget))
    _blendop_masks_group_name_commit(module, gtk_entry_get_text(GTK_ENTRY(widget)));
  return FALSE;
}

static void _blendop_masks_all_name_edited(GtkCellRendererText *cell, gchar *path_string,
                                           gchar *new_text, dt_iop_module_t *module)
{
  (void)cell;
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->blend_data)) return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(bd->masks_treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  int formid = -1;
  gtk_tree_model_get(model, &iter, BLENDOP_MASKS_ALL_COL_FORMID, &formid, -1);
  dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, formid);
  if(IS_NULL_PTR(mask_form)) return;

  const gchar *text = (new_text && *new_text) ? new_text : " ";
  g_strlcpy(mask_form->name, text, sizeof(mask_form->name));

  dt_dev_add_history_item(darktable.develop, NULL, FALSE, TRUE);
  _blendop_masks_refresh_lists(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, mask_form->formid, 0,
                                DT_MASKS_EVENT_CHANGE);
}

static void _blendop_masks_all_selection_changed(GtkTreeSelection *selection, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(selection)) return;

  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  if(!GTK_IS_TREE_MODEL(model)) return;

  int formid = -1;
  gtk_tree_model_get(model, &iter, BLENDOP_MASKS_ALL_COL_FORMID, &formid, -1);
  if(formid <= 0) return;

  dt_dev_masks_selection_change(darktable.develop, NULL, formid, TRUE);
}

static void _blendop_masks_all_toggled(GtkCellRendererToggle *cell, gchar *path_string, dt_iop_module_t *module)
{
  (void)cell;
  if(darktable.gui->reset) return;
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->blend_data)) return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(bd->masks_treeview));
  if(!GTK_IS_TREE_MODEL(model)) return;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  gboolean active = FALSE;
  gboolean sensitive = TRUE;
  int formid = -1;
  gtk_tree_model_get(model, &iter, BLENDOP_MASKS_ALL_COL_ACTIVE, &active,
                     BLENDOP_MASKS_ALL_COL_FORMID, &formid,
                     BLENDOP_MASKS_ALL_COL_SENSITIVE, &sensitive, -1);

  if(!sensitive) return;

  dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, formid);
  if(IS_NULL_PTR(mask_form)) return;

  dt_masks_form_t *group_form = _blendop_masks_group_from_module(module);
  const int parentid_before = group_form ? group_form->formid : 0;

  if(active)
  {
    if(!IS_NULL_PTR(group_form))
      dt_masks_form_delete(module, group_form, mask_form);
  }
  else
  {
    if(IS_NULL_PTR(group_form))
      group_form = _blendop_masks_group_create(module);
    if(IS_NULL_PTR(group_form)) return;

    if(!_blendop_masks_find_group_entry(group_form, mask_form->formid, NULL))
    {
      dt_masks_group_add_form(group_form, mask_form);
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    }
  }

  // If this is a group with shapes, update sensitive state of child shapes
  if(_blendop_masks_is_group_with_shapes(mask_form))
  {
    const gboolean new_active = !active;  // Toggle state
    GtkTreeIter search_iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &search_iter);
    
    while(valid)
    {
      int child_formid = -1;
      gtk_tree_model_get(model, &search_iter, BLENDOP_MASKS_ALL_COL_FORMID, &child_formid, -1);
      
      // Check if this child belongs to the toggled group
      if(_blendop_masks_find_group_entry(mask_form, child_formid, NULL))
      {
        // Lock (gray out) if group is being activated, unlock if deactivated
        gtk_list_store_set(GTK_LIST_STORE(model), &search_iter, 
                          BLENDOP_MASKS_ALL_COL_SENSITIVE, !new_active, -1);
      }
      
      valid = gtk_tree_model_iter_next(model, &search_iter);
    }
  }

  _blendop_masks_apply_and_commit(module);

  const int parentid_after = group_form ? group_form->formid : parentid_before;
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, formid,
                                active ? parentid_before : parentid_after,
                                active ? DT_MASKS_EVENT_REMOVE : DT_MASKS_EVENT_ADD);
}

static void _blendop_masks_all_delete_callback(GtkWidget *menu_item, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;

  const int formid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-formid"));
  dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, formid);
  if(IS_NULL_PTR(mask_form)) return;

  dt_masks_change_form_gui(NULL);
  dt_masks_form_delete(module, NULL, mask_form);
  _blendop_masks_apply_and_commit(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, formid, 0, DT_MASKS_EVENT_DELETE);
}

static void _blendop_masks_all_duplicate_callback(GtkWidget *menu_item, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;

  const int formid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-formid"));
  if(dt_masks_form_duplicate(darktable.develop, formid) <= 0) return;

  _blendop_masks_apply_and_commit(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, 0, 0, DT_MASKS_EVENT_RESET);
}

static void _blendop_masks_all_rename_callback(GtkWidget *menu_item, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(module->blend_data)) return;

  const int formid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-formid"));
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  GtkTreeView *view = GTK_TREE_VIEW(bd->masks_treeview);
  GtkTreeModel *model = gtk_tree_view_get_model(view);
  if(!GTK_IS_TREE_MODEL(model)) return;

  GtkTreeIter iter;
  if(!_blendop_masks_find_iter_by_formid(model, &iter, BLENDOP_MASKS_ALL_COL_FORMID, formid)) return;

  GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
  GtkTreeViewColumn *column = gtk_tree_view_get_column(view, 1);
  GtkCellRenderer *renderer = g_object_get_data(G_OBJECT(view), "blendop-masks-name-renderer");
  if(path && column && renderer)
    gtk_tree_view_set_cursor_on_cell(view, path, column, renderer, TRUE);
  if(path) gtk_tree_path_free(path);
}

static gboolean _blendop_masks_all_handle_left_click(GtkWidget *treeview, GtkTreePath *path,
                                                        GtkTreeViewColumn *column, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return FALSE;
  if(IS_NULL_PTR(path) || IS_NULL_PTR(column)) return FALSE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd) || column != bd->all_shapes_col) return FALSE;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(model, &iter, path)) return FALSE;

  gboolean sensitive = TRUE;
  int formid = -1;
  gtk_tree_model_get(model, &iter, BLENDOP_MASKS_ALL_COL_SENSITIVE, &sensitive,
                     BLENDOP_MASKS_ALL_COL_FORMID, &formid, -1);

  if(formid <= 0) return FALSE;

  if(!sensitive)
  {
    gtk_tree_path_free(path);
    return TRUE;
  }

  // Toggle the checkbox value.
  gchar *path_string = gtk_tree_path_to_string(path);
  _blendop_masks_all_toggled(NULL, path_string, module);
  dt_free(path_string);
  gtk_tree_path_free(path);
  return TRUE; // Block default selection behavior.
}

static gboolean _blendop_masks_all_button_pressed(GtkWidget *treeview, GdkEventButton *event,
                                                  dt_iop_module_t *module)
{
  if(IS_NULL_PTR(event) || event->type != GDK_BUTTON_PRESS) return FALSE;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path,
                                    &column, NULL, NULL))
    return FALSE;

  // Handle left click on checkbox column - toggle directly without requiring selection
  if(event->button == GDK_BUTTON_PRIMARY && !IS_NULL_PTR(column))
  {
    if(_blendop_masks_all_handle_left_click(treeview, path, column, module))
      return TRUE;
  }

  // Handle right click for context menu
  if(event->button != GDK_BUTTON_SECONDARY)
  {
    gtk_tree_path_free(path);
    return FALSE;
  }

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_unselect_all(selection);
  gtk_tree_selection_select_path(selection, path);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  GtkTreeIter iter;
  int formid = -1;
  if(gtk_tree_model_get_iter(model, &iter, path))
    gtk_tree_model_get(model, &iter, BLENDOP_MASKS_ALL_COL_FORMID, &formid, -1);
  gtk_tree_path_free(path);
  if(formid <= 0) return TRUE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *item = gtk_menu_item_new_with_label(_("Delete"));
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_signal_connect(item, "activate", G_CALLBACK(_blendop_masks_all_delete_callback), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  item = gtk_menu_item_new_with_label(_("Duplicate"));
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_signal_connect(item, "activate", G_CALLBACK(_blendop_masks_all_duplicate_callback), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  item = gtk_menu_item_new_with_label(_("Rename"));
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_signal_connect(item, "activate", G_CALLBACK(_blendop_masks_all_rename_callback), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  return TRUE;
}

static void _blendop_masks_group_operation_callback(GtkWidget *menu_item, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(IS_NULL_PTR(module)) return;

  const int formid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-formid"));
  const int parentid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-parentid"));
  const dt_masks_state_t state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-state"));

  dt_masks_form_group_t *group_entry = dt_masks_form_group_from_parentid(parentid, formid);
  if(IS_NULL_PTR(group_entry)) return;

  const int old_state = group_entry->state;
  apply_operation(group_entry, state);
  if(group_entry->state == old_state) return;

  _blendop_masks_apply_and_commit(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, formid, parentid,
                                DT_MASKS_EVENT_UPDATE);
}

static void _blendop_masks_group_selection_changed(GtkTreeSelection *selection, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  if(IS_NULL_PTR(module)) return;
  if(IS_NULL_PTR(selection)) return;

  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  if(!GTK_IS_TREE_MODEL(model)) return;

  int formid = -1;
  gtk_tree_model_get(model, &iter, BLENDOP_MASKS_GROUP_COL_FORMID, &formid, -1);
  if(formid <= 0) return;

  // Switching edit mode is required to actually display mask overlays in the center view.
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
  // Keep lib/masks tree selection in sync without re-triggering its selection handler,
  // otherwise the visible overlay can be replaced by only the clicked shape.
  dt_dev_masks_selection_change(darktable.develop, module, formid, FALSE);

  // Mark the selected shape as active in the central mask GUI state.
  if(module->dev && module->dev->form_gui)
  {
    dt_masks_form_gui_t *gui = module->dev->form_gui;
    dt_masks_form_t *visible_form = dt_masks_get_visible_form(module->dev);
    const int selected_index = dt_masks_group_index_from_formid(visible_form, formid);
    if(selected_index >= 0)
    {
      gui->group_selected = selected_index;
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
      gui->source_selected = FALSE;
      gui->node_selected = FALSE;
      gui->handle_selected = FALSE;
      gui->seg_selected = FALSE;
      gui->handle_border_selected = FALSE;
      gui->node_selected_idx = -1;
      gui->form_dragging = FALSE;
      gui->source_dragging = FALSE;
      gui->form_rotating = FALSE;
      gui->pivot_selected = FALSE;
    }
  }
}

static gboolean _blendop_masks_group_move_by_index(dt_masks_form_t *group_form, const int index,
                                                   const gboolean move_up)
{
  if(IS_NULL_PTR(group_form)) return FALSE;
  if(!(group_form->type & DT_MASKS_GROUP)) return FALSE;
  if(index < 0) return FALSE;

  const guint length = g_list_length(group_form->points);
  if(length == 0 || (guint)index >= length) return FALSE;
  if(move_up && index == 0) return FALSE;
  if(!move_up && (guint)index >= length - 1) return FALSE;

  dt_masks_form_group_t *entry = (dt_masks_form_group_t *)g_list_nth_data(group_form->points, index);
  if(IS_NULL_PTR(entry)) return FALSE;

  const int new_index = move_up ? index - 1 : index + 1;
  group_form->points = g_list_remove(group_form->points, entry);
  group_form->points = g_list_insert(group_form->points, entry, new_index);
  return TRUE;
}

static void _blendop_masks_group_move_callback(GtkWidget *menu_item, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;

  const int parentid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-parentid"));
  const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-index"));
  const gboolean move_up = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item), "blend-move-up"));
  dt_masks_form_t *group_form = dt_masks_get_from_id(darktable.develop, parentid);
  if(!_blendop_masks_group_move_by_index(group_form, index, move_up)) return;

  dt_masks_change_form_gui(NULL);
  _blendop_masks_apply_and_commit(module);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, 0, parentid, DT_MASKS_EVENT_UPDATE);
}

static void _blendop_masks_edit_list_toggle(GtkToggleButton *togglebutton, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  if(!GTK_IS_TOGGLE_BUTTON(togglebutton)) return;

  // The edit toggle swaps the visible list between:
  // - current module mask tree
  // - global all-shapes list
  const gboolean edit_mode = gtk_toggle_button_get_active(togglebutton);

  if(edit_mode)
    gtk_button_set_label(GTK_BUTTON(togglebutton), _("OK"));
  else
    gtk_button_set_label(GTK_BUTTON(togglebutton), _("Wire shapes"));

  if(GTK_IS_STACK(bd->lists_stack))
    gtk_stack_set_visible_child_name(GTK_STACK(bd->lists_stack), edit_mode ? "all" : "group");

  if(GTK_IS_ENTRY(bd->group_shapes_label))
    gtk_widget_set_sensitive(bd->group_shapes_label, !edit_mode);
}

static GtkWidget *_blendop_masks_shape_buttons(dt_iop_module_t *module, dt_iop_gui_blend_data_t *bd)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(bd)) return NULL;

  GtkWidget *all_shapes_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(all_shapes_buttons, GTK_ALIGN_END);
  gtk_widget_set_valign(all_shapes_buttons, GTK_ALIGN_START);

  bd->masks_type[0] = DT_MASKS_GRADIENT;
  bd->masks_shapes[0] = dt_iop_togglebutton_new_no_register(module, "blend`shapes", N_("add gradient"),
                                                            N_("add multiple gradients"),
                                                            G_CALLBACK(_blendop_masks_add_shape),
                                                            FALSE, 0, 0, dtgtk_cairo_paint_masks_gradient,
                                                            all_shapes_buttons);
  gtk_widget_set_valign(bd->masks_shapes[0], GTK_ALIGN_START);

  bd->masks_type[4] = DT_MASKS_BRUSH;
  bd->masks_shapes[4] = dt_iop_togglebutton_new_no_register(module, "blend`shapes", N_("add brush"),
                                                            N_("add multiple brush strokes"),
                                                            G_CALLBACK(_blendop_masks_add_shape),
                                                            FALSE, 0, 0, dtgtk_cairo_paint_masks_brush,
                                                            all_shapes_buttons);
  gtk_widget_set_valign(bd->masks_shapes[4], GTK_ALIGN_START);

  bd->masks_type[1] = DT_MASKS_POLYGON;
  bd->masks_shapes[1] = dt_iop_togglebutton_new(module, "blend`shapes", N_("add polygon"),
                                                N_("add multiple polygons"),
                                                G_CALLBACK(_blendop_masks_add_shape),
                                                FALSE, 0, 0, dtgtk_cairo_paint_masks_polygon,
                                                all_shapes_buttons);
  gtk_widget_set_valign(bd->masks_shapes[1], GTK_ALIGN_START);

  bd->masks_type[2] = DT_MASKS_ELLIPSE;
  bd->masks_shapes[2] = dt_iop_togglebutton_new_no_register(module, "blend`shapes", N_("add ellipse"),
                                                            N_("add multiple ellipses"),
                                                            G_CALLBACK(_blendop_masks_add_shape),
                                                            FALSE, 0, 0, dtgtk_cairo_paint_masks_ellipse,
                                                            all_shapes_buttons);
  gtk_widget_set_valign(bd->masks_shapes[2], GTK_ALIGN_START);

  bd->masks_type[3] = DT_MASKS_CIRCLE;
  bd->masks_shapes[3] = dt_iop_togglebutton_new_no_register(module, "blend`shapes", N_("add circle"),
                                                            N_("add multiple circles"),
                                                            G_CALLBACK(_blendop_masks_add_shape),
                                                            FALSE, 0, 0, dtgtk_cairo_paint_masks_circle,
                                                            all_shapes_buttons);
  gtk_widget_set_valign(bd->masks_shapes[3], GTK_ALIGN_START);

  return all_shapes_buttons;
}

static GtkWidget *_blendop_masks_group_ctx_menu(dt_iop_gui_blend_data_t *bd, dt_iop_module_t *module,
                                                const int formid, const int parentid, const int state,
                                                const int index, const int list_length)
{
  if(IS_NULL_PTR(bd) || IS_NULL_PTR(module)) return NULL;

  // Initialize mask operation icons if needed
  _blendop_masks_init_icons(bd);

  GtkWidget *menu = gtk_menu_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(menu), "dt-masks-context-menu");

  GtkWidget *op_item = gtk_menu_item_new_with_label(_("Operation"));
  GtkWidget *op_submenu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(op_item), op_submenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), op_item);

  GtkWidget *item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(_("Invert"), bd->masks_ic_inverse,
                                                                        op_submenu,
                                                                        _blendop_masks_group_operation_callback,
                                                                        module,
                                                                        (state & DT_MASKS_STATE_INVERSE) != 0, TRUE);
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-state", GINT_TO_POINTER(DT_MASKS_STATE_INVERSE));

  gtk_menu_shell_append(GTK_MENU_SHELL(op_submenu), gtk_separator_menu_item_new());

  item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(_("Union"), bd->masks_ic_union, op_submenu,
                                                            _blendop_masks_group_operation_callback, module,
                                                            (state & DT_MASKS_STATE_UNION) != 0, FALSE);
  gtk_widget_set_sensitive(item, index > 0);
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-state", GINT_TO_POINTER(DT_MASKS_STATE_UNION));

  item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(_("Intersection"), bd->masks_ic_intersection, op_submenu,
                                                            _blendop_masks_group_operation_callback, module,
                                                            (state & DT_MASKS_STATE_INTERSECTION) != 0, FALSE);
  gtk_widget_set_sensitive(item, index > 0);
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-state", GINT_TO_POINTER(DT_MASKS_STATE_INTERSECTION));

  item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(_("Difference"), bd->masks_ic_difference, op_submenu,
                                                            _blendop_masks_group_operation_callback, module,
                                                            (state & DT_MASKS_STATE_DIFFERENCE) != 0, FALSE);
  gtk_widget_set_sensitive(item, index > 0);
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-state", GINT_TO_POINTER(DT_MASKS_STATE_DIFFERENCE));

  item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(_("Exclusion"), bd->masks_ic_exclusion, op_submenu,
                                                            _blendop_masks_group_operation_callback, module,
                                                            (state & DT_MASKS_STATE_EXCLUSION) != 0, FALSE);
  gtk_widget_set_sensitive(item, index > 0);
  g_object_set_data(G_OBJECT(item), "blend-formid", GINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-state", GINT_TO_POINTER(DT_MASKS_STATE_EXCLUSION));

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  item = gtk_menu_item_new_with_label(_("Move Up"));
  gtk_widget_set_sensitive(item, index > 0);
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-index", GINT_TO_POINTER(index));
  g_object_set_data(G_OBJECT(item), "blend-move-up", GINT_TO_POINTER(TRUE));
  g_signal_connect(item, "activate", G_CALLBACK(_blendop_masks_group_move_callback), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  item = gtk_menu_item_new_with_label(_("Move Down"));
  gtk_widget_set_sensitive(item, index >= 0 && index < list_length - 1);
  g_object_set_data(G_OBJECT(item), "blend-parentid", GINT_TO_POINTER(parentid));
  g_object_set_data(G_OBJECT(item), "blend-index", GINT_TO_POINTER(index));
  g_object_set_data(G_OBJECT(item), "blend-move-up", GINT_TO_POINTER(FALSE));
  g_signal_connect(item, "activate", G_CALLBACK(_blendop_masks_group_move_callback), module);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  return menu;
}

static gboolean _blendop_masks_group_button_pressed(GtkWidget *treeview, GdkEventButton *event,
                                                    dt_iop_module_t *module)
{
  if(IS_NULL_PTR(event) || event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY) return FALSE;

  GtkTreePath *path = NULL;
  if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path,
                                    NULL, NULL, NULL))
    return FALSE;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_unselect_all(selection);
  gtk_tree_selection_select_path(selection, path);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
  GtkTreeIter iter;
  int formid = -1;
  int parentid = -1;
  int state = 0;
  int index = -1;
  if(gtk_tree_model_get_iter(model, &iter, path))
    gtk_tree_model_get(model, &iter, BLENDOP_MASKS_GROUP_COL_FORMID, &formid,
                       BLENDOP_MASKS_GROUP_COL_PARENTID, &parentid,
                       BLENDOP_MASKS_GROUP_COL_STATE, &state,
                       BLENDOP_MASKS_GROUP_COL_INDEX, &index, -1);
  gtk_tree_path_free(path);

  if(formid <= 0 || parentid <= 0) return TRUE;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_masks_form_t *parent_group = dt_masks_get_from_id(darktable.develop, parentid);
  const int list_length = (parent_group && (parent_group->type & DT_MASKS_GROUP))
                              ? g_list_length(parent_group->points)
                              : 0;

  GtkWidget *menu = _blendop_masks_group_ctx_menu(bd, module, formid, parentid, state, index, list_length);
  if(IS_NULL_PTR(menu)) return TRUE;
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  return TRUE;
}

static void _blendop_masks_handler_callback(gpointer instance, const int formid, const int parentid,
                                            const dt_masks_event_t event, dt_iop_module_t *module)
{
  (void)instance;
  (void)formid;
  (void)parentid;
  (void)event;
  _blendop_masks_refresh_lists(module);
}

gboolean blend_color_picker_apply(dt_iop_module_t *module, GtkWidget *picker, dt_dev_pixelpipe_t *pipe,
                                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gui_blend_data_t *data = module->blend_data;

  if(picker == data->colorpicker_set_values)
  {
    if(darktable.gui->reset) return TRUE;

    ++darktable.gui->reset;

    dt_develop_blend_params_t *bp = module->blend_params;

    const int tab = data->tab;
    dt_aligned_pixel_t raw_min, raw_max;
    float picker_min[8] DT_ALIGNED_PIXEL, picker_max[8] DT_ALIGNED_PIXEL;
    dt_aligned_pixel_t picker_values;

    const int in_out = ((dt_key_modifier_state() == GDK_CONTROL_MASK) && data->output_channels_shown) ? 1 : 0;

    if(in_out)
    {
      for(size_t i = 0; i < 4; i++)
      {
        raw_min[i] = module->picked_output_color_min[i];
        raw_max[i] = module->picked_output_color_max[i];
      }
    }
    else
    {
      for(size_t i = 0; i < 4; i++)
      {
        raw_min[i] = module->picked_color_min[i];
        raw_max[i] = module->picked_color_max[i];
      }
    }

    const dt_iop_gui_blendif_channel_t *channel = &data->channel[data->tab];
    dt_develop_blendif_channels_t ch = channel->param_channels[in_out];
    dt_iop_gui_blendif_filter_t *sl = &data->filter[in_out];

    float *parameters = &(bp->blendif_parameters[4 * ch]);

    const dt_develop_blend_colorspace_t blend_csp = data->channel_tabs_csp;
    const dt_iop_colorspace_type_t cst = _blendif_colorpicker_cst(data);
    const dt_iop_order_iccprofile_info_t *work_profile = (blend_csp == DEVELOP_BLEND_CS_RGB_SCENE)
        ? dt_ioppr_get_pipe_current_profile_info(piece->module, pipe)
        : dt_ioppr_get_iop_work_profile_info(module, module->dev->iop);

    gboolean reverse_hues = FALSE;
    if(cst == IOP_CS_HSL && tab == CHANNEL_INDEX_H)
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[0] - raw_min[0]) && raw_min[3] < 0.5f && raw_max[3] > 0.5f)
      {
        raw_max[0] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[0] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }
    else if((cst == IOP_CS_LCH && tab == CHANNEL_INDEX_h) || (cst == IOP_CS_JZCZHZ && tab == CHANNEL_INDEX_hz))
    {
      if((raw_max[3] - raw_min[3]) < (raw_max[2] - raw_min[2]) && raw_min[3] < 0.5f && raw_max[3] > 0.5f)
      {
        raw_max[2] = raw_max[3] < 0.5f ? raw_max[3] + 0.5f : raw_max[3] - 0.5f;
        raw_min[2] = raw_min[3] < 0.5f ? raw_min[3] + 0.5f : raw_min[3] - 0.5f;
        reverse_hues = TRUE;
      }
    }

    _blendif_scale(data, cst, raw_min, picker_min, work_profile, in_out);
    _blendif_scale(data, cst, raw_max, picker_max, work_profile, in_out);

    const float feather = 0.01f;

    if(picker_min[tab] > picker_max[tab])
    {
      const float tmp = picker_min[tab];
      picker_min[tab] = picker_max[tab];
      picker_max[tab] = tmp;
    }

    picker_values[0] = CLAMP(picker_min[tab] - feather, 0.f, 1.f);
    picker_values[1] = CLAMP(picker_min[tab] + feather, 0.f, 1.f);
    picker_values[2] = CLAMP(picker_max[tab] - feather, 0.f, 1.f);
    picker_values[3] = CLAMP(picker_max[tab] + feather, 0.f, 1.f);

    if(picker_values[1] > picker_values[2])
    {
      picker_values[1] = CLAMP(picker_min[tab], 0.f, 1.f);
      picker_values[2] = CLAMP(picker_max[tab], 0.f, 1.f);
    }

    picker_values[0] = CLAMP(picker_values[0], 0.f, picker_values[1]);
    picker_values[3] = CLAMP(picker_values[3], picker_values[2], 1.f);

    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
      dtgtk_gradient_slider_multivalue_set_value(sl->slider, picker_values[k], k);
    dt_pthread_mutex_unlock(&data->lock);

    // update picked values
    _update_gradient_slider_pickers(NULL, module);

    const float boost_factor = _get_boost_factor(data, data->tab, in_out);
    for(int k = 0; k < 4; k++)
    {
      char text[256];
      channel->scale_print(dtgtk_gradient_slider_multivalue_get_value(sl->slider, k), boost_factor,
                           text, sizeof(text));
      gtk_label_set_text(sl->label[k], text);
    }

    --darktable.gui->reset;

    // save values to parameters
    dt_pthread_mutex_lock(&data->lock);
    for(int k = 0; k < 4; k++)
      parameters[k] = dtgtk_gradient_slider_multivalue_get_value(sl->slider, k);
    dt_pthread_mutex_unlock(&data->lock);

    // de-activate processing of this channel if maximum span is selected
    if(parameters[1] == 0.0f && parameters[2] == 1.0f)
      bp->blendif &= ~(1 << ch);
    else
      bp->blendif |= (1 << ch);

    // set the polarity of the channel to include the picked values
    if(reverse_hues == ((bp->mask_combine & DEVELOP_COMBINE_INV) == DEVELOP_COMBINE_INV))
      bp->blendif &= ~(1 << (16 + ch));
    else
      bp->blendif |= 1 << (16 + ch);

    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
    _blendop_blendif_update_tab(module, tab);

    return TRUE;
  }
  else if(picker == data->colorpicker)
  {
    if(darktable.gui->reset) return TRUE;

    _update_gradient_slider_pickers(NULL, module);

    return TRUE;
  }
  else return FALSE; // needs to be handled by module
}

static gboolean _blendif_change_blend_colorspace(dt_iop_module_t *module, dt_develop_blend_colorspace_t cst)
{
  switch(cst)
  {
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      break;
    default:
      cst = dt_develop_blend_default_module_blend_colorspace(module);
      break;
  }
  if(cst != module->blend_params->blend_cst)
  {
    dt_develop_blend_init_blendif_parameters(module->blend_params, cst);

    // look for last history item for this module with the selected blending mode to copy parametric mask settings
    for(const GList *history = g_list_last(darktable.develop->history); history; history = g_list_previous(history))
    {
      const dt_dev_history_item_t *data = (dt_dev_history_item_t *)(history->data);
      if(data->module == module && data->blend_params->blend_cst == cst)
      {
        const dt_develop_blend_params_t *hp = data->blend_params;
        dt_develop_blend_params_t *np = module->blend_params;

        np->blend_mode = hp->blend_mode;
        np->blend_parameter = hp->blend_parameter;
        np->blendif = hp->blendif;
        memcpy(np->blendif_parameters, hp->blendif_parameters, sizeof(hp->blendif_parameters));
        memcpy(np->blendif_boost_factors, hp->blendif_boost_factors, sizeof(hp->blendif_boost_factors));
        break;
      }
    }

    dt_iop_gui_blend_data_t *bd = module->blend_data;
    const int cst_old = _blendop_blendif_get_picker_colorspace(bd);
    dt_dev_add_history_item(darktable.develop, module, FALSE, TRUE);
    dt_iop_gui_update(module);

    if(cst_old != _blendop_blendif_get_picker_colorspace(bd) &&
       (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->colorpicker)) ||
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->colorpicker_set_values))))
    {
      dt_iop_color_picker_set_cst(bd->module, _blendop_blendif_get_picker_colorspace(bd));
      dt_dev_pixelpipe_update_history_all(bd->module->dev);
    }

    return TRUE;
  }
  return FALSE;
}

static void _blendif_select_colorspace(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_develop_blend_colorspace_t cst = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "dt-blend-cst"));
  if(_blendif_change_blend_colorspace(module, cst))
  {
    gtk_widget_queue_draw(module->widget);
  }
}

static void _blendif_show_output_channels(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return;
  if(!bd->output_channels_shown)
  {
    bd->output_channels_shown = TRUE;
    dt_iop_gui_update(module);
  }
}

static void _blendif_hide_output_channels(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return;
  if(bd->output_channels_shown)
  {
    bd->output_channels_shown = FALSE;
    if(_blendif_clean_output_channels(module))
    {
      dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
    }
    dt_iop_gui_update(module);
  }
}

static void _blendif_options_callback(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  if(event->button != 1 && event->button != 2) return;
  const dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return;

  GtkWidget *mi;
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  // add a section to switch blending color spaces
  const dt_develop_blend_colorspace_t module_cst = dt_develop_blend_default_module_blend_colorspace(module);
  const dt_develop_blend_colorspace_t module_blend_cst = module->blend_params->blend_cst;
  if(module_cst == DEVELOP_BLEND_CS_LAB || module_cst == DEVELOP_BLEND_CS_RGB_DISPLAY
      || module_cst == DEVELOP_BLEND_CS_RGB_SCENE)
  {

    mi = gtk_menu_item_new_with_label(_("reset to default blend colorspace"));
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_NONE), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    // only show Lab blending when the module is a Lab module to avoid using it at the wrong place (Lab blending
    // should not be activated for RGB modules before colorin and after colorout)
    if(module_cst == DEVELOP_BLEND_CS_LAB)
    {
      mi = gtk_check_menu_item_new_with_label(_("Lab"));
      dt_gui_add_class(mi, "dt_transparent_background");
      if(module_blend_cst == DEVELOP_BLEND_CS_LAB)
      {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
        dt_gui_add_class(mi, "active_menu_item");
      }
      g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_LAB), NULL);
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    mi = gtk_check_menu_item_new_with_label(_("RGB (display)"));
    dt_gui_add_class(mi, "dt_transparent_background");
    if(module_blend_cst == DEVELOP_BLEND_CS_RGB_DISPLAY)
    {
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
      dt_gui_add_class(mi, "active_menu_item");
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_RGB_DISPLAY), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    mi = gtk_check_menu_item_new_with_label(_("RGB (scene)"));
    dt_gui_add_class(mi, "dt_transparent_background");
    if(module_blend_cst == DEVELOP_BLEND_CS_RGB_SCENE)
    {
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
      dt_gui_add_class(mi, "active_menu_item");
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-blend-cst", GINT_TO_POINTER(DEVELOP_BLEND_CS_RGB_SCENE), NULL);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_select_colorspace), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    if(bd->output_channels_shown)
    {
      mi = gtk_menu_item_new_with_label(_("reset and hide output channels"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_hide_output_channels), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    else
    {
      mi = gtk_menu_item_new_with_label(_("show output channels"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_blendif_show_output_channels), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
  }

  dt_gui_menu_popup(darktable.gui->presets_popup_menu, GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}

// activate channel/mask view
static void _blendop_blendif_channel_mask_view(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.

  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display | mode;

  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    dt_dev_pixelpipe_display_mask_t channel = data->channel[data->tab].display_channel;

    if(widget == GTK_WIDGET(data->filter[1].slider))
      channel |= DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  // only if something has changed: reprocess center view
  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    dt_dev_pixelpipe_update_history_main(module->dev);
  }
}

// toggle channel/mask view
static void _blendop_blendif_channel_mask_view_toggle(GtkWidget *widget, dt_iop_module_t *module, dt_dev_pixelpipe_display_mask_t mode)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.

  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t new_request_mask_display = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;

  // toggle mode
  if(module->request_mask_display & mode)
    new_request_mask_display &= ~mode;
  else
    new_request_mask_display |= mode;

  dt_pthread_mutex_lock(&data->lock);
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY)
    data->save_for_leave |= DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  else
    data->save_for_leave &= ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  dt_pthread_mutex_unlock(&data->lock);

  new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;

  // in case user requests channel display: get the cannel
  if(new_request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
  {
    dt_dev_pixelpipe_display_mask_t channel = data->channel[data->tab].display_channel;

    if(widget == GTK_WIDGET(data->filter[1].slider))
      channel |= DT_DEV_PIXELPIPE_DISPLAY_OUTPUT;

    new_request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_ANY;
    new_request_mask_display |= channel;
  }

  if(new_request_mask_display != module->request_mask_display)
  {
    module->request_mask_display = new_request_mask_display;
    dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    dt_dev_pixelpipe_update_history_main(module->dev);
  }
}


// magic mode: if mouse cursor enters a gradient slider with shift and/or control pressed we
// enter channel display and/or mask display mode
static gboolean _blendop_blendif_enter(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.

  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  dt_dev_pixelpipe_display_mask_t mode = 0;

  // depending on shift modifiers we activate channel and/or mask display
  if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    mode = (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  }
  else if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
  {
    mode = DT_DEV_PIXELPIPE_DISPLAY_CHANNEL;
  }
  else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    mode = DT_DEV_PIXELPIPE_DISPLAY_MASK;
  }

  dt_pthread_mutex_lock(&data->lock);
  if(mode && data->timeout_handle)
  {
    // purge any remaining timeout handlers
    g_source_remove(data->timeout_handle);
    data->timeout_handle = 0;
  }
  else if(!data->timeout_handle && !(data->save_for_leave & DT_DEV_PIXELPIPE_DISPLAY_STICKY))
  {
    // save request_mask_display to restore later
    data->save_for_leave = module->request_mask_display & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
  }
  dt_pthread_mutex_unlock(&data->lock);

  _blendop_blendif_channel_mask_view(widget, module, mode);

  gtk_widget_grab_focus(widget);
  return FALSE;
}


// handler for delayed mask/channel display mode switch-off
static gboolean _blendop_blendif_leave_delayed(gpointer data)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.

  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  int reprocess = 0;

  dt_pthread_mutex_lock(&bd->lock);
  // restore saved request_mask_display and reprocess image
  if(bd->timeout_handle && (module->request_mask_display != (bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
  {
    module->request_mask_display = bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
    dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
    reprocess = 1;
  }
  bd->timeout_handle = 0;
  dt_pthread_mutex_unlock(&bd->lock);

  if(reprocess)
    dt_dev_pixelpipe_update_history_main(module->dev);
  // return FALSE and thereby terminate the handler
  return FALSE;
}

// de-activate magic mode when leaving the gradient slider
static gboolean _blendop_blendif_leave(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_gui_blend_data_t *data = module->blend_data;

  // do not immediately switch-off mask/channel display in case user leaves gradient only briefly.
  // instead we activate a handler function that gets triggered after some timeout
  dt_pthread_mutex_lock(&data->lock);
  if(!(module->request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_STICKY) && !data->timeout_handle &&
    (module->request_mask_display != (data->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY)))
      data->timeout_handle = g_timeout_add(1000, _blendop_blendif_leave_delayed, module);
  dt_pthread_mutex_unlock(&data->lock);

  return FALSE;
}


static gboolean _blendop_blendif_key_press(GtkWidget *widget, GdkEventKey *event, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_blend_data_t *data = module->blend_data;
  gboolean handled = FALSE;

  const int tab = data->tab;
  const int in_out = (widget == GTK_WIDGET(data->filter[1].slider)) ? 1 : 0;

  switch(event->keyval)
  {
    case GDK_KEY_a:
    case GDK_KEY_A:
      if(data->channel[tab].altdisplay)
        data->altmode[tab][in_out] = (data->channel[tab].altdisplay)(widget, module, data->altmode[tab][in_out] + 1);
      handled = TRUE;
      break;
    case GDK_KEY_c:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
      handled = TRUE;
      break;
    case GDK_KEY_C:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_CHANNEL | DT_DEV_PIXELPIPE_DISPLAY_STICKY);
      handled = TRUE;
      break;
    case GDK_KEY_m:
    case GDK_KEY_M:
      _blendop_blendif_channel_mask_view_toggle(widget, module, DT_DEV_PIXELPIPE_DISPLAY_MASK);
      handled = TRUE;
  }

  if(handled)
    dt_iop_request_focus(module);

  return handled;
}


#define COLORSTOPS(gradient) sizeof(gradient) / sizeof(dt_iop_gui_blendif_colorstop_t), gradient

const dt_iop_gui_blendif_channel_t Lab_channels[]
    = { { N_("L"), N_("sliders for L channel"), 1.0f / 100.0f, COLORSTOPS(_gradient_L), TRUE, 0.0f,
          { DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out }, DT_DEV_PIXELPIPE_DISPLAY_L,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("lightness") },
        { N_("a"), N_("sliders for a channel"), 1.0f / 256.0f, COLORSTOPS(_gradient_a), TRUE, 0.0f,
          { DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out }, DT_DEV_PIXELPIPE_DISPLAY_a,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag, N_("green/red") },
        { N_("b"), N_("sliders for b channel"), 1.0f / 256.0f, COLORSTOPS(_gradient_b), TRUE, 0.0f,
          { DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out }, DT_DEV_PIXELPIPE_DISPLAY_b,
          _blendif_scale_print_ab, _blendop_blendif_disp_alternative_mag, N_("blue/yellow") },
        { N_("C"), N_("sliders for chroma channel (of LCh)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          TRUE, 0.0f,
          { DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_C,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("saturation") },
        { N_("h"), N_("sliders for hue channel (of LCh)"), 1.0f / 360.0f, COLORSTOPS(_gradient_LCh_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out }, DT_DEV_PIXELPIPE_DISPLAY_LCH_h,
          _blendif_scale_print_hue, NULL, N_("hue") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgb_channels[]
    = { { N_("g"), N_("sliders for gray value"), 1.0f / 255.0f, COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out }, DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("gray") },
        { N_("R"), N_("sliders for red channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out }, DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("red") },
        { N_("G"), N_("sliders for green channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out }, DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("green") },
        { N_("B"), N_("sliders for blue channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out }, DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("blue") },
        { N_("H"), N_("sliders for hue channel (of HSL)"), 1.0f / 360.0f, COLORSTOPS(_gradient_HSL_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_H_in, DEVELOP_BLENDIF_H_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_H,
          _blendif_scale_print_hue, NULL, N_("hue") },
        { N_("S"), N_("sliders for chroma channel (of HSL)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_S_in, DEVELOP_BLENDIF_S_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_S,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("chroma") },
        { N_("L"), N_("sliders for value channel (of HSL)"), 1.0f / 100.0f, COLORSTOPS(_gradient_gray),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_l_in, DEVELOP_BLENDIF_l_out }, DT_DEV_PIXELPIPE_DISPLAY_HSL_l,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("luminance") },
        { NULL } };

const dt_iop_gui_blendif_channel_t rgbj_channels[]
    = { { N_("g"), N_("sliders for gray value"), 1.0f / 255.0f, COLORSTOPS(_gradient_gray), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GRAY_in, DEVELOP_BLENDIF_GRAY_out }, DT_DEV_PIXELPIPE_DISPLAY_GRAY,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("gray") },
        { N_("R"), N_("sliders for red channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_red), TRUE, 0.0f,
          { DEVELOP_BLENDIF_RED_in, DEVELOP_BLENDIF_RED_out }, DT_DEV_PIXELPIPE_DISPLAY_R,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("red") },
        { N_("G"), N_("sliders for green channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_green), TRUE, 0.0f,
          { DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out }, DT_DEV_PIXELPIPE_DISPLAY_G,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("green") },
        { N_("B"), N_("sliders for blue channel"), 1.0f / 255.0f, COLORSTOPS(_gradient_blue), TRUE, 0.0f,
          { DEVELOP_BLENDIF_BLUE_in, DEVELOP_BLENDIF_BLUE_out }, DT_DEV_PIXELPIPE_DISPLAY_B,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("blue") },
        { N_("Jz"), N_("sliders for value channel (of JzCzhz)"), 1.0f / 100.0f, COLORSTOPS(_gradient_gray),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Jz_in, DEVELOP_BLENDIF_Jz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("luminance") },
        { N_("Cz"), N_("sliders for chroma channel (of JzCzhz)"), 1.0f / 100.0f, COLORSTOPS(_gradient_chroma),
          TRUE, -6.64385619f, // cf. _blend_init_blendif_boost_parameters
          { DEVELOP_BLENDIF_Cz_in, DEVELOP_BLENDIF_Cz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz,
          _blendif_scale_print_default, _blendop_blendif_disp_alternative_log, N_("chroma") },
        { N_("hz"), N_("sliders for hue channel (of JzCzhz)"), 1.0f / 360.0f, COLORSTOPS(_gradient_JzCzhz_hue),
          FALSE, 0.0f,
          { DEVELOP_BLENDIF_hz_in, DEVELOP_BLENDIF_hz_out }, DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz,
          _blendif_scale_print_hue, NULL, N_("hue") },
        { NULL } };

const char *slider_tooltip[] = { N_("adjustment based on input received by this module:\n* range defined by upper markers: "
                                    "blend fully\n* range defined by lower markers: do not blend at all\n* range between "
                                    "adjacent upper/lower markers: blend gradually"),
                                 N_("adjustment based on unblended output of this module:\n* range defined by upper "
                                    "markers: blend fully\n* range defined by lower markers: do not blend at all\n* range "
                                    "between adjacent upper/lower markers: blend gradually") };

void dt_iop_gui_update_blendif(dt_iop_module_t *module)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.
  dt_iop_gui_blend_data_t *bd = module->blend_data;

  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return;

  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
  {
    g_source_remove(bd->timeout_handle);
    bd->timeout_handle = 0;
    if(module->request_mask_display != (bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY))
    {
      module->request_mask_display = bd->save_for_leave & ~DT_DEV_PIXELPIPE_DISPLAY_STICKY;
      dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);
      dt_dev_pixelpipe_update_history_all(module->dev);
    }
  }
  dt_pthread_mutex_unlock(&bd->lock);

  /* update output channel mask visibility */
  gtk_widget_set_visible(GTK_WIDGET(bd->filter[1].box), bd->output_channels_shown);

  /* update tabs */
  if(bd->channel_tabs_csp != bd->csp)
  {
    bd->channel = NULL;

    switch(bd->csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        bd->channel = Lab_channels;
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
        bd->channel = rgb_channels;
        break;
      case DEVELOP_BLEND_CS_RGB_SCENE:
        bd->channel = rgbj_channels;
        break;
      default:
        assert(FALSE); // blendif not supported for RAW, which is already caught upstream; we should not get
                       // here
    }

    dt_iop_color_picker_reset(module, TRUE);

    /* remove tabs before adding others */
    dt_gui_container_destroy_children(GTK_CONTAINER(bd->channel_tabs));

    bd->channel_tabs_csp = bd->csp;

    int index = 0;
    for(const dt_iop_gui_blendif_channel_t *ch = bd->channel; !IS_NULL_PTR(ch->label); ch++, index++)
    {
      dt_ui_notebook_page(bd->channel_tabs, ch->label, _(ch->tooltip));
      gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(bd->channel_tabs, index)));
    }

    bd->tab = 0;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->channel_tabs), bd->tab);
  }

  _blendop_blendif_update_tab(module, bd->tab);
  dt_iop_add_remove_mask_indicator(module);

  --darktable.gui->reset;
}


void dt_iop_gui_init_blendif(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->blendif_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  // add event box so that one can click into the area to get help for parametric masks
  GtkWidget* event_box = gtk_event_box_new();
  dt_gui_add_help_link(GTK_WIDGET(event_box), dt_get_help_url("masks_parametric"));
  gtk_container_add(GTK_CONTAINER(blendw), event_box);
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->blendif_box));

  /* create and add blendif support if module supports it */
  if(bd->blendif_support)
  {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    gtk_box_pack_start(GTK_BOX(section), dt_ui_label_new(_("parametric mask")), TRUE, TRUE, 0);
    dt_gui_add_class(section, "dt_section_label");

    dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("reset blend mask settings"), NULL,
                                        G_CALLBACK(_blendop_blendif_reset), FALSE, 0, 0,
                                        dtgtk_cairo_paint_reset, section);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(section), TRUE, FALSE, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->tab = 0;
    bd->channel_tabs_csp = DEVELOP_BLEND_CS_NONE;
    bd->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

    gtk_notebook_set_scrollable(bd->channel_tabs, TRUE);
    gtk_box_pack_start(GTK_BOX(header), GTK_WIDGET(bd->channel_tabs), TRUE, TRUE, 0);

    // a little padding between the notbook with all channels and the icons for pickers.
    gtk_box_pack_start(GTK_BOX(header), gtk_label_new(""),
                       FALSE, FALSE, DT_PIXEL_APPLY_DPI(10));

    bd->colorpicker = dt_color_picker_new(module, DT_COLOR_PICKER_POINT_AREA, header);
    gtk_widget_set_tooltip_text(bd->colorpicker, _("pick GUI color from image\nctrl+click or right-click to select an area"));

    bd->colorpicker_set_values = dt_color_picker_new(module, DT_COLOR_PICKER_AREA, header);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(bd->colorpicker_set_values),
                                 dtgtk_cairo_paint_colorpicker, CPF_ALTER, NULL);
    dt_gui_add_class(bd->colorpicker_set_values, "dt_transparent_background");
    gtk_widget_set_tooltip_text(bd->colorpicker_set_values, _("set the range based on an area from the image\n"
                                                              "drag to use the input image\n"
                                                              "ctrl+drag to use the output image"));

    GtkWidget *btn = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("invert all channel's polarities"), NULL,
                                                         G_CALLBACK(_blendop_blendif_invert), FALSE, 0, 0,
                                                         dtgtk_cairo_paint_invert, header);
    dt_gui_add_class(btn, "dt_ignore_fg_state");

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(header), TRUE, FALSE, 0);

    for(int in_out = 1; in_out >= 0; in_out--)
    {
      dt_iop_gui_blendif_filter_t *sl = &bd->filter[in_out];

      GtkWidget *slider_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

      sl->slider = DTGTK_GRADIENT_SLIDER_MULTIVALUE(dtgtk_gradient_slider_multivalue_new_with_name(4,
                                                   in_out ? "blend-upper" : "blend-lower"));
      gtk_box_pack_start(GTK_BOX(slider_box), GTK_WIDGET(sl->slider), TRUE, TRUE, 0);

      sl->polarity = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, 0, NULL);
      dt_gui_add_class(sl->polarity, "dt_ignore_fg_state");
      dt_gui_add_class(sl->polarity, "dt_transparent_background");
      gtk_widget_set_tooltip_text(sl->polarity, _("toggle polarity. best seen by enabling 'display mask'"));
      gtk_box_pack_end(GTK_BOX(slider_box), GTK_WIDGET(sl->polarity), FALSE, FALSE, 0);

      GtkWidget *label_box = gtk_grid_new();
      gtk_grid_set_column_homogeneous(GTK_GRID(label_box), TRUE);

      sl->head = GTK_LABEL(dt_ui_label_new(in_out ? _("output") : _("input")));
      gtk_grid_attach(GTK_GRID(label_box), GTK_WIDGET(sl->head), 0, 0, 1, 1);

      GtkWidget *overlay = gtk_overlay_new();
      gtk_grid_attach(GTK_GRID(label_box), overlay, 1, 0, 3, 1);

      sl->picker_label = GTK_LABEL(gtk_label_new(""));
      gtk_widget_set_name(GTK_WIDGET(sl->picker_label), "blend-data");
      gtk_label_set_xalign(sl->picker_label, .0);
      gtk_label_set_yalign(sl->picker_label, 1.0);
      gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(sl->picker_label));

      for(int k = 0; k < 4; k++)
      {
        sl->label[k] = GTK_LABEL(gtk_label_new(NULL));
        gtk_widget_set_name(GTK_WIDGET(sl->label[k]), "blend-data");
        gtk_label_set_xalign(sl->label[k], .35 + k * .65/3);
        gtk_label_set_yalign(sl->label[k], k % 2);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(sl->label[k]));
      }

      gtk_widget_set_tooltip_text(GTK_WIDGET(sl->slider), _("double-click to reset.\npress 'a' to toggle available slider modes.\npress 'c' to toggle view of channel data.\npress 'm' to toggle mask view."));
      gtk_widget_set_tooltip_text(GTK_WIDGET(sl->head), _(slider_tooltip[in_out]));

      g_signal_connect(G_OBJECT(sl->slider), "value-changed", G_CALLBACK(_blendop_blendif_sliders_callback), bd);
      g_signal_connect(G_OBJECT(sl->slider), "value-reset", G_CALLBACK(_blendop_blendif_sliders_reset_callback), bd);
      g_signal_connect(G_OBJECT(sl->slider), "leave-notify-event", G_CALLBACK(_blendop_blendif_leave), module);
      g_signal_connect(G_OBJECT(sl->slider), "enter-notify-event", G_CALLBACK(_blendop_blendif_enter), module);
      g_signal_connect(G_OBJECT(sl->slider), "key-press-event", G_CALLBACK(_blendop_blendif_key_press), module);
      g_signal_connect(G_OBJECT(sl->polarity), "toggled", G_CALLBACK(_blendop_blendif_polarity_callback), bd);

      sl->box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
      gtk_box_pack_start(GTK_BOX(sl->box), GTK_WIDGET(label_box), TRUE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(sl->box), GTK_WIDGET(slider_box), TRUE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(sl->box), TRUE, FALSE, 0);
    }

    bd->channel_boost_factor_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), 0.0f, 18.0f, 0, 0.0f, 3);
    dt_bauhaus_disable_module_list(bd->channel_boost_factor_slider);
    dt_bauhaus_set_use_default_callback(bd->channel_boost_factor_slider);
    dt_bauhaus_slider_set_format(bd->channel_boost_factor_slider, _(" EV"));
    dt_bauhaus_widget_set_label(bd->channel_boost_factor_slider, N_("boost factor"));
    dt_bauhaus_slider_set_soft_range(bd->channel_boost_factor_slider, 0.0, 3.0);
    gtk_widget_set_tooltip_text(bd->channel_boost_factor_slider, _("adjust the boost factor of the channel mask"));
    gtk_widget_set_sensitive(bd->channel_boost_factor_slider, FALSE);

    g_signal_connect(G_OBJECT(bd->channel_boost_factor_slider), "value-changed",
                     G_CALLBACK(_blendop_blendif_boost_factor_callback), bd);

    gtk_box_pack_start(GTK_BOX(bd->blendif_box), GTK_WIDGET(bd->channel_boost_factor_slider), TRUE, FALSE, 0);

    g_signal_connect(G_OBJECT(bd->channel_tabs), "switch_page", G_CALLBACK(_blendop_blendif_tab_switch), bd);
    g_signal_connect(G_OBJECT(bd->colorpicker), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);
    g_signal_connect(G_OBJECT(bd->colorpicker_set_values), "toggled", G_CALLBACK(_update_gradient_slider_pickers), module);

    bd->blendif_inited = 1;
  }
}

void dt_masks_iop_update(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(IS_NULL_PTR(bd) || !bd->masks_support || !bd->masks_inited) return;

  ++darktable.gui->reset;

  /* update masks state */
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  const gboolean has_group_shapes = grp && (grp->type & DT_MASKS_GROUP) && grp->points;
  if(GTK_IS_WIDGET(bd->masks_combo))
  {
    dt_bauhaus_combobox_clear(bd->masks_combo);
    if(has_group_shapes)
    {
      char txt[512];
      const guint n = g_list_length(grp->points);
      snprintf(txt, sizeof(txt), ngettext("%d shape used", "%d shapes used", n), n);
      dt_bauhaus_combobox_add(bd->masks_combo, txt);
    }
    else
    {
      dt_bauhaus_combobox_add(bd->masks_combo, _("no mask used"));
    }
    dt_bauhaus_combobox_set(bd->masks_combo, 0);
    gtk_widget_queue_draw(bd->masks_combo);
  }

  if(!has_group_shapes)
  {
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    // reset the gui
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  }

  if(bd->masks_support)
  {
    if(GTK_IS_TOGGLE_BUTTON(bd->masks_edit))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), bd->masks_shown != DT_MASKS_EDIT_OFF);

    if(GTK_IS_TOGGLE_BUTTON(bd->masks_polarity))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_polarity),
                                   bp->mask_combine & DEVELOP_COMBINE_MASKS_POS);
  }

  // update buttons status
  for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
  {
    if(!GTK_IS_TOGGLE_BUTTON(bd->masks_shapes[n])) continue;

    dt_masks_form_t *visible_form = dt_masks_get_visible_form(module->dev);
    if(module->dev->form_gui && visible_form && module->dev->form_gui->creation
       && module->dev->form_gui->creation_module == module
       && (visible_form->type & bd->masks_type[n]))
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), TRUE);
    }
    else
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), FALSE);
    }
  }

  _blendop_masks_refresh_lists(module);
  dt_iop_add_remove_mask_indicator(module);

  --darktable.gui->reset;
}

void dt_iop_gui_init_masks(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->masks_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  // add event box so that one can click into the area to get help for drawn masks
  GtkWidget* event_box = gtk_event_box_new();
  dt_gui_add_help_link(GTK_WIDGET(event_box), dt_get_help_url("masks_drawn"));
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add masks support if module supports it */
  if(bd->masks_support)
  {
    bd->masks_combo_ids = NULL;
    bd->masks_shown = DT_MASKS_EDIT_OFF;

    bd->lists_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkCellRenderer *renderer = NULL;
    GtkTreeSelection *selection = NULL;

    GtkWidget *group_shapes_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    dt_gui_add_class(group_shapes_header, "dt_section_label");
    bd->group_shapes_label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(bd->group_shapes_label), dt_dev_get_masks_group_name(module));
    gtk_widget_set_tooltip_text(bd->group_shapes_label, _("Edit current module mask name"));
    gtk_widget_set_halign(bd->group_shapes_label, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(bd->group_shapes_label, TRUE);
    g_signal_connect(bd->group_shapes_label, "activate", G_CALLBACK(_blendop_masks_group_name_activate), module);
    g_signal_connect(bd->group_shapes_label, "focus-out-event",
             G_CALLBACK(_blendop_masks_group_name_focus_out), module);
    gtk_box_pack_start(GTK_BOX(group_shapes_header), bd->group_shapes_label, TRUE, TRUE, 0);

    // buttons for mask polarity and edit mode
    bd->masks_polarity = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("toggle polarity of drawn mask"), NULL,
                                                             G_CALLBACK(_blendop_masks_polarity_callback),
                                                             FALSE, 0, 0, dtgtk_cairo_paint_plusminus, group_shapes_header);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(bd->masks_polarity), dtgtk_cairo_paint_plusminus, 0, NULL);
    dt_gui_add_class(bd->masks_polarity, "dt_ignore_fg_state");

    bd->masks_edit = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("show and edit mask elements"),
                                                         N_("show and edit in restricted mode"),
                                                         G_CALLBACK(_blendop_masks_show_and_edit),
                                                         FALSE, 0, 0, dtgtk_cairo_paint_masks_edit, group_shapes_header);

    gtk_box_pack_start(GTK_BOX(bd->lists_box), group_shapes_header, FALSE, FALSE, 0);

    // Current mask list
    bd->masks_group_treeview = gtk_tree_view_new();
    bd->group_shapes_store = gtk_tree_store_new(BLENDOP_MASKS_GROUP_COL_COUNT, GDK_TYPE_PIXBUF,
                                                          GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT,
                                                          G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
    gtk_tree_view_set_model(GTK_TREE_VIEW(bd->masks_group_treeview), GTK_TREE_MODEL(bd->group_shapes_store));

    // Initialize mask operation icons for treeview display
    _blendop_masks_init_icons(bd);

    bd->group_shapes_col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(bd->masks_group_treeview), bd->group_shapes_col);

    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(bd->group_shapes_col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(bd->group_shapes_col, renderer, "pixbuf", BLENDOP_MASKS_GROUP_COL_OP_ICON);

    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(bd->group_shapes_col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(bd->group_shapes_col, renderer, "pixbuf", BLENDOP_MASKS_GROUP_COL_INV_ICON);

    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(bd->group_shapes_col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(bd->group_shapes_col, renderer, "text", BLENDOP_MASKS_GROUP_COL_NAME);

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(bd->masks_group_treeview));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed", G_CALLBACK(_blendop_masks_group_selection_changed), module);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(bd->masks_group_treeview), FALSE);
    gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(bd->masks_group_treeview), TRUE);
    g_signal_connect(bd->masks_group_treeview, "button-press-event",
                     G_CALLBACK(_blendop_masks_group_button_pressed), module);

    bd->group_shapes_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(bd->group_shapes_sw), bd->masks_group_treeview);
    dt_gui_widget_init_auto_height(bd->masks_group_treeview, TREE_LIST_MIN_ROWS, TREE_LIST_MAX_ROWS);

    // Creating shapes buttons (circle, ellipse ....)
    bd->all_shapes_buttons = _blendop_masks_shape_buttons(module, bd);
    if(!GTK_IS_WIDGET(bd->all_shapes_buttons)) return;
    // Wire shapes toggle button
    bd->wire_shape_toggle = gtk_toggle_button_new_with_label(_("Wire shapes"));
    gtk_widget_set_tooltip_text(bd->wire_shape_toggle, _("Show all shapes and groups to choose which ones to connect to or disconnect from the mask."));

    GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(bottom_bar), bd->wire_shape_toggle, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom_bar), bd->all_shapes_buttons, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bd->lists_box), bottom_bar, FALSE, FALSE, 0);


    // All shapes list
    bd->masks_treeview = gtk_tree_view_new();
    bd->all_shapes_store = gtk_list_store_new(BLENDOP_MASKS_ALL_COL_COUNT, G_TYPE_BOOLEAN,
                                              G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN,
                                              G_TYPE_STRING, G_TYPE_STRING);
    gtk_tree_view_set_model(GTK_TREE_VIEW(bd->masks_treeview), GTK_TREE_MODEL(bd->all_shapes_store));

    bd->all_shapes_col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(bd->masks_treeview), bd->all_shapes_col);

    renderer = gtk_cell_renderer_toggle_new();
    g_object_set(renderer, "activatable", TRUE, NULL);
    g_signal_connect(renderer, "toggled", G_CALLBACK(_blendop_masks_all_toggled), module);
    gtk_tree_view_column_pack_start(bd->all_shapes_col, renderer, FALSE);
    gtk_tree_view_column_add_attribute(bd->all_shapes_col, renderer, "active", BLENDOP_MASKS_ALL_COL_ACTIVE);
    gtk_tree_view_column_add_attribute(bd->all_shapes_col, renderer, "sensitive", BLENDOP_MASKS_ALL_COL_SENSITIVE);

    GtkTreeViewColumn *all_shapes_name_col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(bd->masks_treeview), all_shapes_name_col);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "editable", TRUE, NULL);
    g_signal_connect(renderer, "edited", G_CALLBACK(_blendop_masks_all_name_edited), module);
    gtk_tree_view_column_pack_start(all_shapes_name_col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(all_shapes_name_col, renderer, "markup", BLENDOP_MASKS_ALL_COL_MARKUP);
    gtk_tree_view_column_add_attribute(all_shapes_name_col, renderer, "sensitive", BLENDOP_MASKS_ALL_COL_SENSITIVE);
    g_object_set_data(G_OBJECT(bd->masks_treeview), "blendop-masks-name-renderer", renderer);

    GtkTreeViewColumn *all_shapes_status_col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(bd->masks_treeview), all_shapes_status_col);

    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 1.0f, NULL);
    gtk_tree_view_column_pack_end(all_shapes_status_col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(all_shapes_status_col, renderer, "markup",
                       BLENDOP_MASKS_ALL_COL_STATUS_MARKUP);
    gtk_tree_view_column_add_attribute(all_shapes_status_col, renderer, "sensitive",
                       BLENDOP_MASKS_ALL_COL_SENSITIVE);

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(bd->masks_treeview));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed", G_CALLBACK(_blendop_masks_all_selection_changed), module);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(bd->masks_treeview), FALSE);
    g_signal_connect(bd->masks_treeview, "button-press-event",
                     G_CALLBACK(_blendop_masks_all_button_pressed), module);

    bd->all_shapes_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(bd->all_shapes_sw), bd->masks_treeview);
    dt_gui_widget_init_auto_height(bd->masks_treeview, TREE_LIST_MIN_ROWS, TREE_LIST_MAX_ROWS);

    // The two lists share the same slot; Edit selects which one is visible.
    bd->lists_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(bd->lists_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_homogeneous(GTK_STACK(bd->lists_stack), FALSE);
    gtk_stack_add_named(GTK_STACK(bd->lists_stack), bd->group_shapes_sw, "group");
    gtk_stack_add_named(GTK_STACK(bd->lists_stack), bd->all_shapes_sw, "all");
    gtk_box_pack_start(GTK_BOX(bd->lists_box), bd->lists_stack, FALSE, FALSE, 0);

    // Default state:
    // - current-module tree is visible
    // - all-shapes tree is hidden
    // The toggle callback flips these two scrolled windows.
    g_signal_connect(bd->wire_shape_toggle, "toggled", G_CALLBACK(_blendop_masks_edit_list_toggle), module);
    if(GTK_IS_TOGGLE_BUTTON(bd->wire_shape_toggle))
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->wire_shape_toggle), FALSE);
      _blendop_masks_edit_list_toggle(GTK_TOGGLE_BUTTON(bd->wire_shape_toggle), module);
    }

    gtk_box_pack_start(GTK_BOX(bd->masks_box), bd->lists_box, FALSE, FALSE, 0);

    bd->masks_inited = 1;
    _blendop_masks_refresh_lists(module);
  }
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->masks_box));
}

typedef struct raster_combo_entry_t
{
  dt_iop_module_t *module;
  int id;
} raster_combo_entry_t;

static void _raster_combo_populate(GtkWidget *w, void *m)
{
  dt_iop_module_t *module = (dt_iop_module_t *)m;
  dt_iop_request_focus(module);

  dt_bauhaus_combobox_clear(w);

  raster_combo_entry_t *entry = (raster_combo_entry_t *)calloc(1, sizeof(raster_combo_entry_t));
  if(IS_NULL_PTR(entry)) return;
  dt_bauhaus_combobox_add_full(w, _("no mask used"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);

  int i = 1;

  for(GList* iter = darktable.develop->iop; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)iter->data;
    if(iop == module)
      break;

    GHashTableIter masks_iter;
    gpointer key, value;

    g_hash_table_iter_init(&masks_iter, iop->raster_mask.source.masks);
    while(g_hash_table_iter_next(&masks_iter, &key, &value))
    {
      const int id = GPOINTER_TO_INT(key);
      const char *modulename = (char *)value;
      entry = (raster_combo_entry_t *)malloc(sizeof(raster_combo_entry_t));
      entry->module = iop;
      entry->id = id;
      dt_bauhaus_combobox_add_full(w, modulename, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, entry, free, TRUE);
      if(iop == module->raster_mask.sink.source && module->raster_mask.sink.id == id)
        dt_bauhaus_combobox_set(w, i);
      i++;
    }
  }
}

static void _raster_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  raster_combo_entry_t *entry = dt_bauhaus_combobox_get_data(widget);

  // nothing to do
  if(entry->module == module->raster_mask.sink.source && entry->id == module->raster_mask.sink.id)
    return;

  if(module->raster_mask.sink.source)
  {
    // we no longer use this one
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);
  }

  module->raster_mask.sink.source = entry->module;
  module->raster_mask.sink.id = entry->id;

  if(entry->module)
  {
    g_hash_table_add(entry->module->raster_mask.source.users, module);

    // update blend_params!
    memcpy(module->blend_params->raster_mask_source, entry->module->op, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = entry->module->multi_priority;
    module->blend_params->raster_mask_id = entry->id;
  }
  else
  {
    memset(module->blend_params->raster_mask_source, 0, sizeof(module->blend_params->raster_mask_source));
    module->blend_params->raster_mask_instance = 0;
    module->blend_params->raster_mask_id = 0;
  }

  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
}

void dt_iop_gui_update_raster(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  dt_develop_blend_params_t *bp = module->blend_params;

  if(IS_NULL_PTR(bd) || !bd->masks_support || !bd->raster_inited) return;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), bp->raster_mask_invert);

  _raster_combo_populate(bd->raster_combo, module);
}

static void _raster_polarity_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_develop_blend_params_t *bp = (dt_develop_blend_params_t *)self->blend_params;

  bp->raster_mask_invert = gtk_toggle_button_get_active(togglebutton);

  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
  dt_control_queue_redraw_widget(GTK_WIDGET(togglebutton));
}

void dt_iop_gui_init_raster(GtkBox *blendw, dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->raster_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  // add event box so that one can click into the area to get help for drawn masks
  GtkWidget* event_box = gtk_event_box_new();
  dt_gui_add_help_link(GTK_WIDGET(event_box), dt_get_help_url("masks_raster"));
  gtk_container_add(GTK_CONTAINER(blendw), event_box);

  /* create and add raster support if module supports it (it's coupled to masks at the moment) */
  if(bd->masks_support)
  {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    bd->raster_combo = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(module));
    dt_bauhaus_disable_module_list(bd->raster_combo);
    dt_bauhaus_set_use_default_callback(bd->raster_combo);
    dt_bauhaus_widget_set_label(bd->raster_combo, N_("raster mask"));
    dt_bauhaus_combobox_add(bd->raster_combo, _("no mask used"));
    g_signal_connect(G_OBJECT(bd->raster_combo), "value-changed",
                     G_CALLBACK(_raster_value_changed_callback), module);
    dt_bauhaus_combobox_add_populate_fct(bd->raster_combo, _raster_combo_populate);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_combo, TRUE, TRUE, 0);

    bd->raster_polarity = dtgtk_togglebutton_new(dtgtk_cairo_paint_plusminus, 0, NULL);
    dt_gui_add_class(bd->raster_polarity, "dt_ignore_fg_state");
    gtk_widget_set_tooltip_text(bd->raster_polarity, _("toggle polarity of raster mask"));
    g_signal_connect(G_OBJECT(bd->raster_polarity), "toggled", G_CALLBACK(_raster_polarity_callback), module);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_polarity), FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), bd->raster_polarity, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bd->raster_box), GTK_WIDGET(hbox), TRUE, TRUE, 0);

    bd->raster_inited = 1;
  }
  gtk_container_add(GTK_CONTAINER(event_box), GTK_WIDGET(bd->raster_box));
}

void dt_iop_gui_cleanup_blending(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module->blend_data)) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  dt_iop_gui_cleanup_blending_body(module);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_blendop_masks_handler_callback), module);

  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
    g_source_remove(bd->timeout_handle);

  dt_free(bd->masks_combo_ids);
  dt_pthread_mutex_unlock(&bd->lock);
  dt_pthread_mutex_destroy(&bd->lock);

  dt_free(module->blend_data);
}


static gboolean _add_blendmode_combo(GtkWidget *combobox, dt_develop_blend_mode_t mode)
{
  for(const dt_develop_name_value_t *bm = dt_develop_blend_mode_names; *bm->name; bm++)
  {
    if(bm->value == mode)
    {
      dt_bauhaus_combobox_add_full(combobox, g_dpgettext2(NULL, "blendmode", bm->name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GUINT_TO_POINTER(bm->value), NULL, TRUE);

      return TRUE;
    }
  }

  return FALSE;
}

static GtkWidget *_combobox_new_from_list(dt_iop_module_t *module, const gchar *label,
                                          const dt_develop_name_value_t *list, uint32_t *field, const gchar *tooltip)
{
  GtkWidget *combo = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(module));
  dt_bauhaus_disable_module_list(combo);
  dt_bauhaus_disable_accels(combo);
  dt_bauhaus_set_use_default_callback(combo);

  if(field)
    dt_bauhaus_widget_set_field(combo, field, DT_INTROSPECTION_TYPE_ENUM);
  dt_bauhaus_widget_set_label(combo, label);
  gtk_widget_set_tooltip_text(combo, tooltip);
  for(; *list->name; list++)
    dt_bauhaus_combobox_add_full(combo, _(list->name), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                 GUINT_TO_POINTER(list->value), NULL, TRUE);

  return combo;
}

static void _notebook_append_full_width_page(GtkWidget *notebook, GtkWidget *page, const gchar *label)
{
  GtkWidget *tab = gtk_label_new(label);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, tab);
  gtk_container_child_set(GTK_CONTAINER(notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
}

static GtkWidget *_blendop_create_enable_toggle(dt_iop_module_t *module, const unsigned int mask_bit)
{
  GtkWidget *toggle = gtk_check_button_new_with_label(_("Enable"));
  g_object_set_data(G_OBJECT(toggle), "mask-bit", GUINT_TO_POINTER(mask_bit));
  g_signal_connect(G_OBJECT(toggle), "toggled", G_CALLBACK(_blendop_masks_mode_changed), module);
  return toggle;
}

static void _blendop_update_top_enable_label(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || !module->blend_data) return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!GTK_IS_BUTTON(bd->top_enable)) return;

  gchar *clean_name = delete_underscore(module->name());
  const char *multi_name = module->multi_name[0] ? module->multi_name : "0";
  gchar *multi_name_dup = ((g_strcmp0(multi_name, "0") == 0) || (g_strcmp0(multi_name, "") == 0)) ?  g_strdup("") : g_strdup_printf(" (%s)", multi_name);
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(bd->top_enable));
  gchar *label = g_markup_printf_escaped(_("Enable blending/masking in <b>%s%s</b>"), clean_name, multi_name_dup);
  if(GTK_IS_LABEL(child))
  {
    gtk_label_set_markup(GTK_LABEL(child), label);
    gtk_label_set_line_wrap(GTK_LABEL(child), TRUE);
    gtk_label_set_xalign(GTK_LABEL(child), 0.0f);
  }
  dt_free(label);
  dt_free(clean_name);
  dt_free(multi_name_dup);
}

static GtkWidget *_blendop_create_notebook_page(GtkWidget *notebook, const gchar *label, GtkWidget **content)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(page), *content, TRUE, TRUE, 0);
  _notebook_append_full_width_page(notebook, page, label);
  return page;
}

static GtkWidget *_blendop_create_toggle_page(GtkWidget *notebook, const gchar *label,
                                              dt_iop_module_t *module, const unsigned int mask_bit,
                                              GtkWidget **toggle, GtkWidget **content)
{
  GtkWidget *page = _blendop_create_notebook_page(notebook, label, content);
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  *toggle = _blendop_create_enable_toggle(module, mask_bit);
  gtk_box_pack_start(GTK_BOX(header), *toggle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
  gtk_box_reorder_child(GTK_BOX(page), header, 0);
  return header;
}

static void _blendop_toggle_button_set_active(GtkWidget *toggle, const gboolean enabled)
{
  if(GTK_IS_TOGGLE_BUTTON(toggle))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), enabled);
}

static void _blendop_sync_toggle_state(GtkWidget *toggle, const gboolean available,
                                       const gboolean enabled, GtkWidget *content)
{
  _blendop_toggle_button_set_active(toggle, enabled);
  if(GTK_IS_WIDGET(toggle))
    gtk_widget_set_sensitive(toggle, available);
  if(GTK_IS_WIDGET(content))
    gtk_widget_set_sensitive(content, available && enabled);
}

void dt_iop_gui_update_blending(dt_iop_module_t *module)
{
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || IS_NULL_PTR(bd)) return;

  ++darktable.gui->reset;

  // update color space from parameters
  const dt_develop_blend_colorspace_t default_csp = dt_develop_blend_default_module_blend_colorspace(module);
  switch(default_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      bd->csp = DEVELOP_BLEND_CS_RAW;
      break;
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      switch(module->blend_params->blend_cst)
      {
        case DEVELOP_BLEND_CS_LAB:
        case DEVELOP_BLEND_CS_RGB_DISPLAY:
        case DEVELOP_BLEND_CS_RGB_SCENE:
          bd->csp = module->blend_params->blend_cst;
          break;
        default:
          bd->csp = default_csp;
          break;
      }
      break;
    case DEVELOP_BLEND_CS_NONE:
    default:
      bd->csp = DEVELOP_BLEND_CS_NONE;
      break;
  }

  // (un)set the mask indicator
  dt_iop_add_remove_mask_indicator(module);

  if(!bd->blending_body_box)
  {
    --darktable.gui->reset;
    return;
  }

  // initialization of blending modes
  if(bd->csp != bd->blend_modes_csp)
  {
    dt_bauhaus_combobox_clear(bd->blend_modes_combo);

    if(bd->csp == DEVELOP_BLEND_CS_LAB
       || bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY
       || bd->csp == DEVELOP_BLEND_CS_RAW )
    {
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_NORMAL2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_BOUNDED);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_AVERAGE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIFFERENCE2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTEN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_ADD);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SCREEN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DARKEN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SUBTRACT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_MULTIPLY);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_OVERLAY);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SOFTLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HARDLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_VIVIDLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LINEARLIGHT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_PINLIGHT);

      if(bd->csp == DEVELOP_BLEND_CS_LAB)
      {
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_A);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_B);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LAB_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLORADJUST);
      }
      else if(bd->csp == DEVELOP_BLEND_CS_RGB_DISPLAY)
      {
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_R);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_G);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_B);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HSV_VALUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HSV_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HUE);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLOR);
        _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_COLORADJUST);
      }
    }
    else if(bd->csp == DEVELOP_BLEND_CS_RGB_SCENE)
    {
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_NORMAL2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_MULTIPLY);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIVIDE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_ADD);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_SUBTRACT);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_DIFFERENCE2);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_AVERAGE);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_GEOMETRIC_MEAN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_HARMONIC_MEAN);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_R);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_G);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_RGB_B);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_LIGHTNESS);
      _add_blendmode_combo(bd->blend_modes_combo, DEVELOP_BLEND_CHROMATICITY);
    }
    bd->blend_modes_csp = bd->csp;
  }

  dt_develop_blend_mode_t blend_mode = module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK;
  // module->blend_params->mask_mode
  if(!dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode))
  {
    // add deprecated blend mode
    if(!_add_blendmode_combo(bd->blend_modes_combo, blend_mode))
    {
      // should never happen: unknown blend mode
      dt_control_log("unknown blend mode '%d' in module '%s'", blend_mode, module->op);
      module->blend_params->blend_mode = DEVELOP_BLEND_NORMAL2;
      blend_mode = DEVELOP_BLEND_NORMAL2;
    }

    dt_bauhaus_combobox_set_from_value(bd->blend_modes_combo, blend_mode);
  }

  gboolean blend_mode_reversed = (module->blend_params->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE;
  _blendop_toggle_button_set_active(bd->blend_modes_blend_order, blend_mode_reversed);

  dt_bauhaus_slider_set(bd->blend_mode_parameter_slider, module->blend_params->blend_parameter);
  gtk_widget_set_sensitive(bd->blend_mode_parameter_slider,
                           _blendif_blend_parameter_enabled(bd->blend_modes_csp, module->blend_params->blend_mode));
  gtk_widget_set_visible(bd->blend_mode_parameter_slider, bd->blend_modes_csp == DEVELOP_BLEND_CS_RGB_SCENE);

  dt_bauhaus_combobox_set_from_value(bd->masks_combine_combo,
                                     module->blend_params->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
  dt_bauhaus_combobox_set_from_value(bd->masks_invert_combo,
                                     module->blend_params->mask_combine & DEVELOP_COMBINE_INV);
  dt_bauhaus_slider_set(bd->opacity_slider, module->blend_params->opacity);
  dt_bauhaus_combobox_set_from_value(bd->masks_feathering_guide_combo,
                                     module->blend_params->feathering_guide);
  dt_bauhaus_slider_set(bd->feathering_radius_slider, module->blend_params->feathering_radius);
  dt_bauhaus_slider_set(bd->blur_radius_slider, module->blend_params->blur_radius);
  dt_bauhaus_slider_set(bd->brightness_slider, module->blend_params->brightness);
  dt_bauhaus_slider_set(bd->contrast_slider, module->blend_params->contrast);
  dt_bauhaus_slider_set(bd->details_slider, module->blend_params->details);

  /* reset all alternative display modes for blendif */
  memset(bd->altmode, 0, sizeof(bd->altmode));

  // force the visibility of output channels if they contain some setting
  bd->output_channels_shown = bd->output_channels_shown
      || _blendif_are_output_channels_used(module->blend_params, bd->csp);

  dt_iop_gui_update_blendif(module);
  dt_masks_iop_update(module);
  dt_iop_gui_update_raster(module);

  /* sync page states from mask mode */
  const unsigned int mask_mode = module->blend_params->mask_mode;
  const gboolean top_enabled = (mask_mode & DEVELOP_MASK_ENABLED) != 0;
  const gboolean masks_enabled = (mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean raster_enabled = (mask_mode & DEVELOP_MASK_RASTER) != 0;
  const gboolean blendif_enabled = (mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  const gboolean bottom_enabled = top_enabled && ((bd->masks_inited && masks_enabled)
                                                  || (bd->raster_inited && raster_enabled)
                                                  || (bd->blendif_inited && blendif_enabled));

  _blendop_update_top_enable_label(module);
  _blendop_toggle_button_set_active(bd->top_enable, top_enabled);
  if(GTK_IS_WIDGET(bd->top_content))
    gtk_widget_set_sensitive(bd->top_content, top_enabled);
  if(GTK_IS_WIDGET(bd->blending_notebook))
    gtk_widget_set_sensitive(bd->blending_notebook, top_enabled);
  _blendop_sync_toggle_state(bd->masks_enable, bd->masks_inited, masks_enabled, bd->masks_content);
  _blendop_sync_toggle_state(bd->raster_enable, bd->raster_inited, raster_enabled, bd->raster_content);
  _blendop_sync_toggle_state(bd->blendif_enable, bd->blendif_inited, blendif_enabled, bd->blendif_content);
  gtk_widget_set_sensitive(bd->bottom_content, bottom_enabled);

  // Details mask is deprecated. Show it only if it was used in an old edit,
  // but do not hide it solely because the current input is not RAW.
  gtk_widget_set_visible(bd->details_slider, module->blend_params->details != 0.0f);

  if(bd->blendif_inited && blendif_enabled)
  {
    gtk_widget_hide(GTK_WIDGET(bd->masks_invert_combo));
    gtk_widget_show(GTK_WIDGET(bd->masks_combine_combo));
  }
  else
  {
    gtk_widget_show(GTK_WIDGET(bd->masks_invert_combo));
    gtk_widget_hide(GTK_WIDGET(bd->masks_combine_combo));
  }

  /*
   * if this iop is operating in raw space, it has only 1 channel per pixel,
   * thus there is no alpha channel where we would normally store mask
   * that would get displayed if following button have been pressed.
   */
  if(module->blend_colorspace(module, NULL, NULL) == IOP_CS_RAW)
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dt_iop_set_cache_bypass(module, FALSE);
    _blendop_toggle_button_set_active(bd->showmask, FALSE);
    gtk_widget_hide(GTK_WIDGET(bd->showmask));

    // disable also guided filters on RAW based color space
    gtk_widget_set_sensitive(bd->masks_feathering_guide_combo, FALSE);
    gtk_widget_hide(bd->masks_feathering_guide_combo);
    gtk_widget_set_sensitive(bd->feathering_radius_slider, FALSE);
    gtk_widget_hide(bd->feathering_radius_slider);
    gtk_widget_set_sensitive(bd->brightness_slider, FALSE);
    gtk_widget_hide(bd->brightness_slider);
    gtk_widget_set_sensitive(bd->contrast_slider, FALSE);
    gtk_widget_hide(bd->contrast_slider);
    gtk_widget_set_sensitive(bd->details_slider, FALSE);
    gtk_widget_hide(bd->details_slider);
  }
  else
  {
    gtk_widget_show(GTK_WIDGET(bd->showmask));
    gtk_widget_set_sensitive(bd->masks_feathering_guide_combo, TRUE);
    gtk_widget_show(bd->masks_feathering_guide_combo);
    gtk_widget_set_sensitive(bd->feathering_radius_slider, TRUE);
    gtk_widget_show(bd->feathering_radius_slider);
    gtk_widget_set_sensitive(bd->brightness_slider, TRUE);
    gtk_widget_show(bd->brightness_slider);
    gtk_widget_set_sensitive(bd->contrast_slider, TRUE);
    gtk_widget_show(bd->contrast_slider);
    gtk_widget_set_sensitive(bd->details_slider, TRUE);
    gtk_widget_show(bd->details_slider);
  }

  if(!bottom_enabled)
  {
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dt_iop_set_cache_bypass(module, FALSE);
    _blendop_toggle_button_set_active(bd->showmask, FALSE);
    module->suppress_mask = 0;
    _blendop_toggle_button_set_active(bd->suppress, FALSE);
  }

  if(bd->masks_inited && !masks_enabled)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
    _blendop_toggle_button_set_active(bd->masks_edit, FALSE);
  }

  if(bd->masks_support && !masks_enabled)
  {
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      if(bd->masks_shapes[n])
        _blendop_toggle_button_set_active(bd->masks_shapes[n], FALSE);
  }

  if(bd->blendif_inited && !blendif_enabled)
  {
    dt_iop_color_picker_reset(module, FALSE);
  }

  --darktable.gui->reset;
}

void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module)
{
  // FIXME : there are more than 3 functions getting clever about how to setup module->request_mask_display depending on user input.
  // These should all use an uniform setter function.
  //
  // The lack of setter implies we don't guaranty that only 1 module can request mask display at a time.
  // The consequence is pipeline needs to check if module->request_mask_display AND module == dev->gui_module,
  // but the global pipe->mask_display is set from *_blend_process() at runtime, so it's a pipe property
  // that changes over the pipe lifecycle.
  //
  // This is a self-feeding loop of madness because it ties the pipeline to GUI states
  // (but not all pipes are connected to a GUI, so you need to cover all cases all the time and don't forget to test everything),
  // and because the pipeline is executed recursively from the end, but pipe->mask_display is set in the middle,
  // when it reaches the process() method of the module capturing mask preview, so you don't have this info when
  // planning for pipeline execution.
  // And you need to plan for mask preview ahead in pipe because mask preview needs to work
  // without using the pixelpipe cache, at least between the module requiring mask preview and gamma.c, which will actually render
  // the preview at the far end of the pipe.
  // So the not-so-clever workaround inherited from darktable was to flush all cache lines when requesting mask preview,
  // which flushed lines that could be reused later and were only temporarily not needed.

  if(darktable.gui->reset) return;
  if(IS_NULL_PTR(module)) return;

  const int has_mask_display = module->request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  const int suppress = module->suppress_mask;

  if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && module->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
    if(bd->showmask)
      _blendop_toggle_button_set_active(bd->showmask, FALSE);
    if(bd->suppress)
      _blendop_toggle_button_set_active(bd->suppress, FALSE);
    module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    dt_iop_set_cache_bypass(module, FALSE);
    module->suppress_mask = 0;

    // (re)set the header mask indicator too
    if(bd->masks_support && bd->masks_edit)
    {
      // unselect all tools
      _blendop_toggle_button_set_active(bd->masks_edit, FALSE);
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);

      for(int k=0; k < DEVELOP_MASKS_NB_SHAPES; k++)
        if(bd->masks_shapes[k])
          _blendop_toggle_button_set_active(bd->masks_shapes[k], FALSE);
    }

    dt_pthread_mutex_lock(&bd->lock);
    bd->save_for_leave = DT_DEV_PIXELPIPE_DISPLAY_NONE;
    if(bd->timeout_handle)
    {
      // purge any remaining timeout handlers
      g_source_remove(bd->timeout_handle);
      bd->timeout_handle = 0;
    }
    dt_pthread_mutex_unlock(&bd->lock);

    // reprocess main center image if needed
    if (has_mask_display || suppress)
      dt_dev_pixelpipe_update_history_main(module->dev);
  }
}

void dt_iop_gui_blending_reload_defaults(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  dt_iop_gui_blend_data_t *bd = module->blend_data;
  if(IS_NULL_PTR(bd) || !bd->blendif_support || !bd->blendif_inited) return;
  bd->output_channels_shown = FALSE;
}

void dt_iop_gui_cleanup_blending_body(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || !module->blend_data) return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(IS_NULL_PTR(bd->blending_body_box)) return;
  dt_pthread_mutex_lock(&bd->lock);
  if(bd->timeout_handle)
  {
    g_source_remove(bd->timeout_handle);
    bd->timeout_handle = 0;
  }
  dt_free(bd->masks_combo_ids);
  dt_pthread_mutex_unlock(&bd->lock);

  if(bd->masks_ic_inverse) g_object_unref(bd->masks_ic_inverse);
  if(bd->masks_ic_union) g_object_unref(bd->masks_ic_union);
  if(bd->masks_ic_intersection) g_object_unref(bd->masks_ic_intersection);
  if(bd->masks_ic_difference) g_object_unref(bd->masks_ic_difference);
  if(bd->masks_ic_exclusion) g_object_unref(bd->masks_ic_exclusion);
  if(bd->group_shapes_store) g_object_unref(bd->group_shapes_store);
  if(bd->all_shapes_store) g_object_unref(bd->all_shapes_store);

  gtk_widget_destroy(bd->blending_body_box);

  bd->blending_body_box = NULL;
  bd->blending_notebook = NULL;
  bd->top_enable = NULL;
  bd->masks_enable = NULL;
  bd->raster_enable = NULL;
  bd->blendif_enable = NULL;
  bd->top_content = NULL;
  bd->masks_content = NULL;
  bd->raster_content = NULL;
  bd->blendif_content = NULL;
  bd->bottom_content = NULL;
  bd->blendif_box = NULL;
  bd->masks_box = NULL;
  bd->raster_box = NULL;
  bd->colorpicker = NULL;
  bd->colorpicker_set_values = NULL;
  memset(bd->filter, 0, sizeof(bd->filter));
  bd->showmask = NULL;
  bd->suppress = NULL;
  bd->masks_combine_combo = NULL;
  bd->blend_modes_combo = NULL;
  bd->blend_modes_blend_order = NULL;
  bd->blend_mode_parameter_slider = NULL;
  bd->masks_invert_combo = NULL;
  bd->opacity_slider = NULL;
  bd->masks_feathering_guide_combo = NULL;
  bd->feathering_radius_slider = NULL;
  bd->blur_radius_slider = NULL;
  bd->contrast_slider = NULL;
  bd->brightness_slider = NULL;
  bd->channel_boost_factor_slider = NULL;
  bd->details_slider = NULL;
  bd->masks_combo = NULL;
  memset(bd->masks_shapes, 0, sizeof(bd->masks_shapes));
  bd->masks_edit = NULL;
  bd->group_shapes_label = NULL;
  bd->masks_polarity = NULL;
  bd->wire_shape_toggle = NULL;
  bd->masks_treeview = NULL;
  bd->masks_group_treeview = NULL;
  bd->group_shapes_store = NULL;
  bd->group_shapes_col = NULL;
  bd->all_shapes_store = NULL;
  bd->group_shapes_sw = NULL;
  bd->all_shapes_col = NULL;
  bd->all_shapes_sw = NULL;
  bd->lists_stack = NULL;
  bd->all_shapes_buttons = NULL;
  bd->lists_box = NULL;
  bd->masks_ic_inverse = NULL;
  bd->masks_ic_union = NULL;
  bd->masks_ic_intersection = NULL;
  bd->masks_ic_difference = NULL;
  bd->masks_ic_exclusion = NULL;
  bd->raster_combo = NULL;
  bd->raster_polarity = NULL;
  bd->channel_tabs = NULL;
  bd->blendif_inited = 0;
  bd->masks_inited = 0;
  bd->raster_inited = 0;
  bd->blend_modes_csp = DEVELOP_BLEND_CS_NONE;
  bd->channel_tabs_csp = DEVELOP_BLEND_CS_NONE;
  module->fusion_slider = NULL;
}

void dt_iop_gui_init_blending_body(GtkBox *blendw, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(blendw)) return;
  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return;
  if(IS_NULL_PTR(module->blend_data)) return;

  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(bd->blending_body_box) return;

  ++darktable.gui->reset;

  GtkWidget *presets_button = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  gtk_widget_set_tooltip_text(presets_button, _("blending options"));
  if(bd->blendif_support)
  {
    g_signal_connect(G_OBJECT(presets_button), "button-press-event", G_CALLBACK(_blendif_options_callback), module);
  }
  else
  {
    gtk_widget_set_sensitive(GTK_WIDGET(presets_button), FALSE);
  }

  GtkWidget *blend_modes_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  bd->blend_modes_combo = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(module));
  dt_bauhaus_disable_module_list(bd->blend_modes_combo);
  dt_bauhaus_set_use_default_callback(bd->blend_modes_combo);
  dt_bauhaus_disable_accels(bd->blend_modes_combo);
  dt_bauhaus_widget_set_label(bd->blend_modes_combo, N_("blend mode"));
  gtk_widget_set_tooltip_text(bd->blend_modes_combo, _("choose blending mode"));

  g_signal_connect(G_OBJECT(bd->blend_modes_combo), "value-changed",
                   G_CALLBACK(_blendop_blend_mode_callback), bd);
  dt_gui_add_help_link(GTK_WIDGET(bd->blend_modes_combo), dt_get_help_url("masks_blending_op"));
  gtk_box_pack_start(GTK_BOX(blend_modes_hbox), bd->blend_modes_combo, TRUE, TRUE, 0);

  bd->blend_modes_blend_order = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("toggle blend order"), NULL,
                                                                    G_CALLBACK(_blendop_blend_order_clicked), FALSE,
                                                                    0, 0, dtgtk_cairo_paint_invert, blend_modes_hbox);
  gtk_widget_set_tooltip_text(bd->blend_modes_blend_order, _("toggle the blending order between the input and the output of the module,"
                                                             "\nby default the output will be blended on top of the input,"
                                                             "\norder can be reversed by clicking on the icon (input on top of output)"));

  bd->blend_mode_parameter_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), -18.0f, 18.0f, 0, 0.0f, 3);
  dt_bauhaus_disable_module_list(bd->blend_mode_parameter_slider);
  dt_bauhaus_set_use_default_callback(bd->blend_mode_parameter_slider);
  dt_bauhaus_disable_accels(bd->blend_mode_parameter_slider);
  dt_bauhaus_widget_set_field(bd->blend_mode_parameter_slider, &module->blend_params->blend_parameter, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->blend_mode_parameter_slider, N_("blend fulcrum"));
  dt_bauhaus_slider_set_format(bd->blend_mode_parameter_slider, _(" EV"));
  dt_bauhaus_slider_set_soft_range(bd->blend_mode_parameter_slider, -3.0, 3.0);
  gtk_widget_set_tooltip_text(bd->blend_mode_parameter_slider, _("adjust the fulcrum used by some blending"
                                                                 " operations"));
  gtk_widget_set_visible(bd->blend_mode_parameter_slider, FALSE);

  bd->opacity_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), 0.0, 100.0, 0, 100.0, 0);
  dt_bauhaus_disable_module_list(bd->opacity_slider);
  dt_bauhaus_set_use_default_callback(bd->opacity_slider);
  dt_bauhaus_disable_accels(bd->opacity_slider);
  dt_bauhaus_widget_set_field(bd->opacity_slider, &module->blend_params->opacity, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->opacity_slider, N_("opacity"));
  dt_bauhaus_slider_set_format(bd->opacity_slider, "%");
  module->fusion_slider = bd->opacity_slider;
  gtk_widget_set_tooltip_text(bd->opacity_slider, _("set the opacity of the blending"));

  bd->masks_combine_combo = _combobox_new_from_list(module, _("combine masks"), dt_develop_combine_masks_names, NULL,
                                                    _("how to combine individual drawn mask and different channels of parametric mask"));
  g_signal_connect(G_OBJECT(bd->masks_combine_combo), "value-changed",
                   G_CALLBACK(_blendop_masks_combine_callback), bd);
  dt_gui_add_help_link(GTK_WIDGET(bd->masks_combine_combo), dt_get_help_url("masks_combined"));

  bd->masks_invert_combo = _combobox_new_from_list(module, _("invert mask"), dt_develop_invert_mask_names, NULL,
                                                   _("apply mask in normal or inverted mode"));
  g_signal_connect(G_OBJECT(bd->masks_invert_combo), "value-changed",
                   G_CALLBACK(_blendop_masks_invert_callback), bd);

  bd->details_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), -1.0f, 1.0f, 0, 0.0f, 2);
  dt_bauhaus_disable_module_list(bd->details_slider);
  dt_bauhaus_set_use_default_callback(bd->details_slider);
  dt_bauhaus_disable_accels(bd->details_slider);
  dt_bauhaus_widget_set_label(bd->details_slider, N_("details threshold"));
  dt_bauhaus_slider_set_format(bd->details_slider, "%");
  gtk_widget_set_tooltip_text(bd->details_slider, _("adjust the threshold for the details mask, "
                                                    "\npositive values selects areas with strong details, "
                                                    "\nnegative values select flat areas"));
  g_signal_connect(G_OBJECT(bd->details_slider), "value-changed", G_CALLBACK(_blendop_blendif_details_callback), bd);

  bd->masks_feathering_guide_combo = _combobox_new_from_list(module, _("feathering guide"), dt_develop_feathering_guide_names,
                                                             &module->blend_params->feathering_guide,
                                                             _("choose to guide mask by input or output image and"
                                                               "\nchoose to apply feathering before or after mask blur"));

  bd->feathering_radius_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), 0.0, 250.0, 0, 0.0, 1);
  dt_bauhaus_disable_module_list(bd->feathering_radius_slider);
  dt_bauhaus_set_use_default_callback(bd->feathering_radius_slider);
  dt_bauhaus_disable_accels(bd->feathering_radius_slider);
  dt_bauhaus_widget_set_field(bd->feathering_radius_slider, &module->blend_params->feathering_radius, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->feathering_radius_slider, N_("feathering radius"));
  dt_bauhaus_slider_set_format(bd->feathering_radius_slider, " px");
  gtk_widget_set_tooltip_text(bd->feathering_radius_slider, _("spatial radius of feathering"));

  bd->blur_radius_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), 0.0, 100.0, 0, 0.0, 1);
  dt_bauhaus_disable_module_list(bd->blur_radius_slider);
  dt_bauhaus_set_use_default_callback(bd->blur_radius_slider);
  dt_bauhaus_disable_accels(bd->blur_radius_slider);
  dt_bauhaus_widget_set_field(bd->blur_radius_slider, &module->blend_params->blur_radius, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->blur_radius_slider, N_("blurring radius"));
  dt_bauhaus_slider_set_format(bd->blur_radius_slider, " px");
  gtk_widget_set_tooltip_text(bd->blur_radius_slider, _("radius for gaussian blur of blend mask"));

  bd->brightness_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), -1.0, 1.0, 0, 0.0, 2);
  dt_bauhaus_disable_module_list(bd->brightness_slider);
  dt_bauhaus_set_use_default_callback(bd->brightness_slider);
  dt_bauhaus_disable_accels(bd->brightness_slider);
  dt_bauhaus_widget_set_field(bd->brightness_slider, &module->blend_params->brightness, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->brightness_slider, N_("mask opacity"));
  dt_bauhaus_slider_set_format(bd->brightness_slider, "%");
  gtk_widget_set_tooltip_text(bd->brightness_slider, _("shifts and tilts the tone curve of the blend mask to adjust its "
                                                       "brightness without affecting fully transparent/fully opaque "
                                                       "regions"));

  bd->contrast_slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(module), -1.0, 1.0, 0, 0.0, 2);
  dt_bauhaus_disable_module_list(bd->contrast_slider);
  dt_bauhaus_set_use_default_callback(bd->contrast_slider);
  dt_bauhaus_disable_accels(bd->contrast_slider);
  dt_bauhaus_widget_set_field(bd->contrast_slider, &module->blend_params->contrast, DT_INTROSPECTION_TYPE_FLOAT);
  dt_bauhaus_widget_set_label(bd->contrast_slider, N_("mask contrast"));
  dt_bauhaus_slider_set_format(bd->contrast_slider, "%");
  gtk_widget_set_tooltip_text(bd->contrast_slider, _("gives the tone curve of the blend mask an s-like shape to "
                                                     "adjust its contrast"));

  GtkWidget *refine_mask_label = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(refine_mask_label), dt_ui_label_new(_("mask refinement")), TRUE, TRUE, 0);
  dt_gui_add_class(refine_mask_label, "dt_section_label");

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  bd->showmask = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("display mask and/or color channel"), NULL, G_CALLBACK(_blendop_blendif_showmask_clicked),
                                                     FALSE, 0, 0, dtgtk_cairo_paint_showmask, hbox);
  gtk_widget_set_tooltip_text(bd->showmask, _("display mask and/or color channel. ctrl+click to display mask, "
                                              "shift+click to display channel. hover over parametric mask slider to "
                                              "select channel for display"));
  dt_gui_add_class(bd->showmask, "dt_transparent_background");

  bd->suppress = dt_iop_togglebutton_new_no_register(module, "blend`tools", N_("temporarily switch off blend mask"), NULL, G_CALLBACK(_blendop_blendif_suppress_toggled),
                                                     FALSE, 0, 0, dtgtk_cairo_paint_eye_toggle, hbox);
  gtk_widget_set_tooltip_text(bd->suppress, _("temporarily switch off blend mask. only for module in focus"));
  dt_gui_add_class(bd->suppress, "dt_transparent_background");


  
  bd->blending_body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(blendw), bd->blending_body_box, TRUE, TRUE, 0);

  GtkWidget *top_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  bd->top_enable = _blendop_create_enable_toggle(module, DEVELOP_MASK_ENABLED);
  _blendop_update_top_enable_label(module);
  gtk_box_pack_start(GTK_BOX(top_header), bd->top_enable, FALSE, FALSE, 0);
  dt_gui_add_help_link(top_header, dt_get_help_url("masks_blending"));
  gtk_box_pack_start(GTK_BOX(bd->blending_body_box), top_header, FALSE, FALSE, 0);

  bd->top_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *top_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(top_box), blend_modes_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(top_box), bd->blend_mode_parameter_slider, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(top_box), bd->opacity_slider, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(top_box), hbox, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(bd->top_content), top_box, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bd->blending_body_box), bd->top_content, FALSE, FALSE, 0);

  bd->blending_notebook = gtk_notebook_new();
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(bd->blending_notebook), TRUE);
  gtk_notebook_set_action_widget(GTK_NOTEBOOK(bd->blending_notebook), presets_button, GTK_PACK_END);
  gtk_widget_show(presets_button);
  gtk_box_pack_start(GTK_BOX(bd->blending_body_box), bd->blending_notebook, TRUE, TRUE, 0);

  _blendop_create_toggle_page(bd->blending_notebook, _("Raster"), module,
                              DEVELOP_MASK_RASTER, &bd->raster_enable, &bd->raster_content);
  dt_iop_gui_init_raster(GTK_BOX(bd->raster_content), module);

  _blendop_create_toggle_page(bd->blending_notebook, _("Drawn"), module,
                              DEVELOP_MASK_MASK, &bd->masks_enable, &bd->masks_content);
  dt_iop_gui_init_masks(GTK_BOX(bd->masks_content), module);

  _blendop_create_toggle_page(bd->blending_notebook, _("Parametric"), module,
                              DEVELOP_MASK_CONDITIONAL, &bd->blendif_enable,
                              &bd->blendif_content);
  dt_iop_gui_init_blendif(GTK_BOX(bd->blendif_content), module);

  _blendop_create_notebook_page(bd->blending_notebook, _("Options"), &bd->bottom_content);
  GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), GTK_WIDGET(bd->masks_combine_combo), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), GTK_WIDGET(bd->masks_invert_combo), TRUE, TRUE, 0);
  
  gtk_box_pack_start(GTK_BOX(bottom_box), refine_mask_label, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(bottom_box), bd->details_slider, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), bd->masks_feathering_guide_combo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), bd->feathering_radius_slider, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), bd->blur_radius_slider, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), bd->brightness_slider, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_box), bd->contrast_slider, TRUE, TRUE, 0);
  GtkWidget *event_box = gtk_event_box_new();
  dt_gui_add_help_link(event_box, dt_get_help_url("masks_refinement"));
  gtk_container_add(GTK_CONTAINER(event_box), bottom_box);
  gtk_box_pack_start(GTK_BOX(bd->bottom_content), event_box, TRUE, TRUE, 0);

  gtk_widget_set_name(top_box, "blending-box");
  gtk_widget_set_name(GTK_WIDGET(bd->masks_box), "blending-box");
  gtk_widget_set_name(GTK_WIDGET(bd->raster_box), "blending-box");
  gtk_widget_set_name(GTK_WIDGET(bd->blendif_box), "blending-box");
  gtk_widget_set_name(bottom_box, "blending-box");
  gtk_widget_set_name(GTK_WIDGET(bd->blending_body_box), "blending-wrapper");

  const unsigned int mask_mode = module->blend_params->mask_mode;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->top_enable), (mask_mode & DEVELOP_MASK_ENABLED) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_enable), (mask_mode & DEVELOP_MASK_MASK) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->raster_enable), (mask_mode & DEVELOP_MASK_RASTER) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->blendif_enable), (mask_mode & DEVELOP_MASK_CONDITIONAL) != 0);

  gtk_widget_show_all(GTK_WIDGET(bd->blending_body_box));
  gtk_widget_set_sensitive(bd->top_content, (mask_mode & DEVELOP_MASK_ENABLED) != 0);
  gtk_widget_set_sensitive(bd->blending_notebook, (mask_mode & DEVELOP_MASK_ENABLED) != 0);

  g_signal_connect(G_OBJECT(bd->blending_notebook), "switch_page", G_CALLBACK(_blendop_blending_notebook_switch), bd);
  const int page_count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(bd->blending_notebook));
  if(page_count > 0)
  {
    // Restore the last-used tab after the notebook has been fully realized.
    int page = dt_conf_get_int(BLEND_MASKMODE_CONF_KEY);
    page = CLAMP(page, 0, page_count - 1);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bd->blending_notebook), page);
  }

  --darktable.gui->reset;
}

void dt_iop_gui_init_blending(dt_iop_module_t *module)
{
  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || module->blend_data) return;

  module->blend_data = g_malloc0(sizeof(dt_iop_gui_blend_data_t));
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  bd->module = module;
  bd->csp = DEVELOP_BLEND_CS_NONE;
  bd->blend_modes_csp = DEVELOP_BLEND_CS_NONE;
  bd->channel_tabs_csp = DEVELOP_BLEND_CS_NONE;

  const dt_iop_colorspace_type_t cst = module->blend_colorspace(module, NULL, NULL);
  bd->blendif_support = (cst == IOP_CS_LAB || dt_iop_colorspace_is_rgb(cst));
  bd->masks_support = !(module->flags() & IOP_FLAGS_NO_MASKS);

  dt_pthread_mutex_init(&bd->lock, NULL);
  dt_pthread_mutex_lock(&bd->lock);
  dt_pthread_mutex_unlock(&bd->lock);

  if(bd->masks_support)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MASK_CHANGED,
                                    G_CALLBACK(_blendop_masks_handler_callback), module);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
