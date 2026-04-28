/*
    This file is part of darktable,
    Copyright (C) 2018-2021, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019, 2021 Aldric Renaudin.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2021 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 Jacques Le Clerc.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2019, 2021 Sakari Kapanen.
    Copyright (C) 2019 Tobias Ellinghaus.
    Copyright (C) 2020 rawfiner.
    Copyright (C) 2021 Anna.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Guillaume Stutin.
    
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/iop_order.h"
#include "common/styles.h"
#include "common/debug.h"
#include "common/deprecations.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_IOP_ORDER_VERSION 5

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

static void _ioppr_reset_iop_order(GList *iop_order_list);
static dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name,
                                                          const int multi_priority);
static gboolean dt_ioppr_write_iop_order(const dt_iop_order_t kind, GList *iop_order_list, const int32_t imgid);
static void dt_ioppr_resync_iop_list(dt_develop_t *dev);
static void dt_ioppr_update_for_entries(dt_develop_t *dev, GList *entry_list, gboolean append);
static void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list);
static void dt_ioppr_migrate_iop_order(struct dt_develop_t *dev, const int32_t imgid);
static GList *dt_ioppr_extract_multi_instances_list(GList *iop_order_list);
static GList *dt_ioppr_merge_multi_instance_iop_order_list(GList *iop_order_list, GList *multi_instance_list);
static gboolean _ioppr_sanity_check_iop_order(GList *list);

const char *iop_order_string[] =
{
  N_("custom"),
  N_("legacy"),
  N_("v3.0 RAW"),
  N_("v3.0 JPEG"),
  N_("Ansel RAW"),
  N_("Ansel JPEG")
};

const char *dt_iop_order_string(const dt_iop_order_t order)
{
  if(order >= DT_IOP_ORDER_LAST)
    return "???";
  else
    return iop_order_string[order];
}

// note legacy_order & v30_order have the original iop-order double that is
// used only for the initial database migration.
//
// in the new code only the iop-order as int is used to order the module on the GUI.

// @@_NEW_MODULE: For new module it is required to insert the new module name in both lists below.

const dt_iop_order_entry_t legacy_order[] = {
  { { 0.5f }, "basebuffer", 0},
  { { 1.0f }, "rawprepare", 0},
  { { 2.0f }, "invert", 0},
  { { 3.0f }, "temperature", 0},
  { { 4.0f }, "highlights", 0},
  { { 5.0f }, "cacorrect", 0},
  { { 6.0f }, "hotpixels", 0},
  { { 7.0f }, "rawdenoise", 0},
  { { 8.0f }, "demosaic", 0},
  { { 8.5f }, "detailmask", 0},
  { { 9.0f }, "mask_manager", 0},
  { {10.0f }, "denoiseprofile", 0},
  { {11.0f }, "tonemap", 0},
  { {12.0f }, "exposure", 0},
  { {13.0f }, "spots", 0},
  { {14.0f }, "retouch", 0},
  { {15.0f }, "lens", 0},
  { {15.5f }, "cacorrectrgb", 0},
  { {15.5f }, "initialscale", 0},
  { {16.0f }, "ashift", 0},
  { {17.0f }, "liquify", 0},
  { {18.0f }, "rotatepixels", 0},
  { {19.0f }, "scalepixels", 0},
  { {20.0f }, "flip", 0},
  { {21.0f }, "clipping", 0},
  { {21.5f }, "toneequal", 0},
  { {21.7f }, "crop", 0},
  { {22.0f }, "graduatednd", 0},
  { {23.0f }, "basecurve", 0},
  { {24.0f }, "bilateral", 0},
  { {25.0f }, "profile_gamma", 0},
  { {26.0f }, "hazeremoval", 0},
  { {27.0f }, "colorin", 0},
  { {27.5f }, "channelmixerrgb", 0},
  { {27.5f }, "diffuse", 0},
  { {27.5f }, "censorize", 0},
  { {27.5f }, "negadoctor", 0},
  { {27.5f }, "blurs", 0},
  { {27.5f }, "basicadj", 0},
  { {28.0f }, "colorreconstruct", 0},
  { {29.0f }, "colorchecker", 0},
  { {30.0f }, "defringe", 0},
  { {31.0f }, "equalizer", 0},
  { {32.0f }, "vibrance", 0},
  { {33.0f }, "colorbalance", 0},
  { {33.4f }, "splittoningrgb", 0},
  { {33.45f }, "colorprimaries", 0},
  { {33.5f }, "colorbalancergb", 0},
  { {33.6f }, "colorequal", 0},
  { {33.7f }, "drawlayer", 0},
  { {34.0f }, "colorize", 0},
  { {36.0f }, "colormapping", 0},
  { {37.0f }, "bloom", 0},
  { {38.0f }, "nlmeans", 0},
  { {39.0f }, "globaltonemap", 0},
  { {40.0f }, "shadhi", 0},
  { {41.0f }, "atrous", 0},
  { {42.0f }, "bilat", 0},
  { {43.0f }, "colorzones", 0},
  { {44.0f }, "lowlight", 0},
  { {45.0f }, "monochrome", 0},
  { {46.0f }, "filmic", 0},
  { {46.25f }, "crystgrain", 0},
  { {46.5f }, "filmicrgb", 0},
  { {47.0f }, "colisa", 0},
  { {48.0f }, "zonesystem", 0},
  { {49.0f }, "tonecurve", 0},
  { {50.0f }, "levels", 0},
  { {50.2f }, "rgblevels", 0},
  { {50.5f }, "rgbcurve", 0},
  { {51.0f }, "relight", 0},
  { {52.0f }, "colorcorrection", 0},
  { {53.0f }, "sharpen", 0},
  { {54.0f }, "lowpass", 0},
  { {55.0f }, "highpass", 0},
  { {56.0f }, "grain", 0},
  { {56.5f }, "lut3d", 0},
  { {57.0f }, "colorcontrast", 0},
  { {58.0f }, "colorout", 0},
  { {59.0f }, "channelmixer", 0},
  { {60.0f }, "soften", 0},
  { {61.0f }, "vignette", 0},
  { {62.0f }, "splittoning", 0},
  { {63.0f }, "velvia", 0},
  { {65.0f }, "finalscale", 0},
  { {66.0f }, "overexposed", 0},
  { {67.0f }, "rawoverexposed", 0},
  { {67.5f }, "dither", 0},
  { {68.0f }, "borders", 0},
  { {69.0f }, "watermark", 0},
  { {71.0f }, "gamma", 0},
  { { 0.0f }, "", 0}
};

// default order for RAW files, assumed to be linear from start
const dt_iop_order_entry_t v30_order[] = {
  { { 0.5 }, "basebuffer", 0},
  { { 1.0 }, "rawprepare", 0},
  { { 2.0 }, "invert", 0},
  { { 3.0f }, "temperature", 0},
  { { 4.0f }, "highlights", 0},
  { { 5.0f }, "cacorrect", 0},
  { { 6.0f }, "hotpixels", 0},
  { { 7.0f }, "rawdenoise", 0},
  { { 8.0f }, "demosaic", 0},
  { { 8.5f }, "detailmask", 0},
  { { 9.0f }, "denoiseprofile", 0},
  { {10.0f }, "bilateral", 0},
  { {11.0f }, "rotatepixels", 0},
  { {12.0f }, "scalepixels", 0},
  { {13.0f }, "lens", 0},
  { {13.5f }, "cacorrectrgb", 0}, // correct chromatic aberrations after lens correction so that lensfun
                                  // does not reintroduce chromatic aberrations when trying to correct them
  { {14.0f }, "hazeremoval", 0},
  { {14.0f }, "initialscale", 0},
  { {15.0f }, "ashift", 0},
  { {16.0f }, "flip", 0},
  { {17.0f }, "clipping", 0},
  { {18.0f }, "liquify", 0},
  { {19.0f }, "spots", 0},
  { {20.0f }, "retouch", 0},
  { {21.0f }, "exposure", 0},
  { {22.0f }, "mask_manager", 0},
  { {23.0f }, "tonemap", 0},
  { {24.0f }, "toneequal", 0},       // last module that need enlarged roi_in
  { {24.5f }, "crop", 0},            // should go after all modules that may need a wider roi_in
  { {25.0f }, "graduatednd", 0},
  { {26.0f }, "profile_gamma", 0},
  { {28.0f }, "colorin", 0},
  { {28.5f }, "channelmixerrgb", 0},
  { {28.5f }, "diffuse", 0},
  { {28.5f }, "censorize", 0},
  { {28.5f }, "negadoctor", 0},      // Cineon film encoding comes after scanner input color profile
  { {28.5f }, "blurs", 0},           // physically-accurate blurs (motion and lens)
  { {29.0f }, "nlmeans", 0},         // signal processing (denoising)
                                  //    -> needs a signal as scene-referred as possible (even if it works in Lab)
  { {30.0f }, "colorchecker", 0},    // calibration to "neutral" exchange colour space
                                  //    -> improve colour calibration of colorin and reproductibility
                                  //    of further edits (styles etc.)
  { {31.0f }, "defringe", 0},        // desaturate fringes in Lab, so needs properly calibrated colours
                                  //    in order for chromaticity to be meaningful,
  { {32.0f }, "atrous", 0},          // frequential operation, needs a signal as scene-referred as possible to avoid halos
  { {33.0f }, "lowpass", 0},         // same
  { {34.0f }, "highpass", 0},        // same
  { {35.0f }, "sharpen", 0},         // same, worst than atrous in same use-case, less control overall

  { {37.0f }, "colortransfer", 0},   // probably better if source and destination colours are neutralized in the same
                                  //    colour exchange space, hence after colorin and colorcheckr,
                                  //    but apply after frequential ops in case it does non-linear witchcraft,
                                  //    just to be safe
  { {38.0f }, "colormapping", 0},    // same
  { {39.0f }, "channelmixer", 0},    // does exactly the same thing as colorin, aka RGB to RGB matrix conversion,
                                  //    but coefs are user-defined instead of calibrated and read from ICC profile.
                                  //    Really versatile yet under-used module, doing linear ops,
                                  //    very good in scene-referred workflow
  { {40.0f }, "basicadj", 0},        // module mixing view/model/control at once, usage should be discouraged
  { {41.0f }, "colorbalance", 0},    // scene-referred color manipulation
  { {41.4f }, "splittoningrgb", 0},  // keyed CAT16 plus RGB mixer before primary warping
  { {41.45f }, "colorprimaries", 0}, // editable RGB/CYM primary nodes in dt UCS
  { {41.5f }, "colorbalancergb", 0}, // scene-referred color manipulation
  { {41.6f }, "colorequal", 0},      // dynamic hue-defined RGB stretching around the achromatic axis
  { {41.7f }, "drawlayer", 0},       // TIFF-backed paint layers in scene-referred RGB
  { {42.0f }, "rgbcurve", 0},        // really versatile way to edit colour in scene-referred and display-referred workflow
  { {43.0f }, "rgblevels", 0},       // same
  { {44.0f }, "basecurve", 0},       // conversion from scene-referred to display referred, reverse-engineered
                                  //    on camera JPEG default look
  { {45.0f }, "filmic", 0},          // same, but different (parametric) approach
  { {45.5f }, "crystgrain", 0},      // scene-referred grain, before filmic RGB
  { {46.0f }, "filmicrgb", 0},       // same, upgraded
  { {36.0f }, "lut3d", 0},           // apply a creative style or film emulation, possibly non-linear
  { {47.0f }, "colisa", 0},          // edit contrast while damaging colour
  { {48.0f }, "tonecurve", 0},       // same
  { {49.0f }, "levels", 0},          // same
  { {50.0f }, "shadhi", 0},          // same
  { {51.0f }, "zonesystem", 0},      // same
  { {52.0f }, "globaltonemap", 0},   // same
  { {53.0f }, "relight", 0},         // flatten local contrast while pretending do add lightness
  { {54.0f }, "bilat", 0},           // improve clarity/local contrast after all the bad things we have done
                                  //    to it with tonemapping
  { {55.0f }, "colorcorrection", 0}, // now that the colours have been damaged by contrast manipulations,
                                  // try to recover them - global adjustment of white balance for shadows and highlights
  { {56.0f }, "colorcontrast", 0},   // adjust chrominance globally
  { {57.0f }, "velvia", 0},          // same
  { {58.0f }, "vibrance", 0},        // same, but more subtle
  { {60.0f }, "colorzones", 0},      // same, but locally
  { {61.0f }, "bloom", 0},           // creative module
  { {62.0f }, "colorize", 0},        // creative module
  { {63.0f }, "lowlight", 0},        // creative module
  { {64.0f }, "monochrome", 0},      // creative module
  { {65.0f }, "grain", 0},           // creative module
  { {66.0f }, "soften", 0},          // creative module
  { {67.0f }, "splittoning", 0},     // creative module
  { {68.0f }, "vignette", 0},        // creative module
  { {69.0f }, "colorreconstruct", 0},// try to salvage blown areas before ICC intents in LittleCMS2 do things with them.
  { {70.0f }, "colorout", 0},
  { {72.0f }, "finalscale", 0},
  { {73.0f }, "overexposed", 0},
  { {74.0f }, "rawoverexposed", 0},
  { {75.0f }, "dither", 0},
  { {76.0f }, "borders", 0},
  { {77.0f }, "watermark", 0},
  { {78.0f }, "gamma", 0},
  { { 0.0f }, "", 0 }
};

// default order for JPEG/TIFF/PNG files, non-linear before colorin
const dt_iop_order_entry_t v30_jpg_order[] = {
  // the following modules are not used anyway for non-RAW images :
  { { 0.5 }, "basebuffer", 0 },
  { { 1.0 }, "rawprepare", 0 },
  { { 2.0 }, "invert", 0 },
  { { 3.0f }, "temperature", 0 },
  { { 4.0f }, "highlights", 0 },
  { { 5.0f }, "cacorrect", 0 },
  { { 6.0f }, "hotpixels", 0 },
  { { 7.0f }, "rawdenoise", 0 },
  { { 8.0f }, "demosaic", 0 },
  { { 8.5f }, "detailmask", 0 },
  // all the modules between [8; 28] expect linear RGB, so they need to be moved after colorin
  { { 28.0f }, "colorin", 0 },
  // moved modules : (copy-pasted in the same order)
  { { 28.0f }, "denoiseprofile", 0},
  { { 28.0f }, "bilateral", 0},
  { { 28.0f }, "rotatepixels", 0},
  { { 28.0f }, "scalepixels", 0},
  { { 28.0f }, "lens", 0},
  { { 28.0f }, "cacorrectrgb", 0}, // correct chromatic aberrations after lens correction so that lensfun
                                  // does not reintroduce chromatic aberrations when trying to correct them
  { { 28.0f }, "hazeremoval", 0},
  { { 28.0f }, "initialscale", 0 },
  { { 28.0f }, "ashift", 0},
  { { 28.0f }, "flip", 0},
  { { 28.0f }, "clipping", 0},
  { { 28.0f }, "liquify", 0},
  { { 28.0f }, "spots", 0},
  { { 28.0f }, "retouch", 0},
  { { 28.0f }, "exposure", 0},
  { { 28.0f }, "mask_manager", 0},
  { { 28.0f }, "tonemap", 0},
  { { 28.0f }, "toneequal", 0},       // last module that need enlarged roi_in
  { { 28.0f }, "crop", 0},            // should go after all modules that may need a wider roi_in
  { { 28.0f }, "graduatednd", 0},
  { { 28.0f }, "profile_gamma", 0},
  // from there, it's the same as the raw order
  { { 28.5f }, "channelmixerrgb", 0 },
  { { 28.5f }, "diffuse", 0 },
  { { 28.5f }, "censorize", 0 },
  { { 28.5f }, "negadoctor", 0 },   // Cineon film encoding comes after scanner input color profile
  { { 28.5f }, "blurs", 0 },        // physically-accurate blurs (motion and lens)
  { { 29.0f }, "nlmeans", 0 },      // signal processing (denoising)
                                    //    -> needs a signal as scene-referred as possible (even if it works in Lab)
  { { 30.0f }, "colorchecker", 0 }, // calibration to "neutral" exchange colour space
                                    //    -> improve colour calibration of colorin and reproductibility
                                    //    of further edits (styles etc.)
  { { 31.0f }, "defringe", 0 },     // desaturate fringes in Lab, so needs properly calibrated colours
                                    //    in order for chromaticity to be meaningful,
  { { 32.0f }, "atrous", 0 }, // frequential operation, needs a signal as scene-referred as possible to avoid halos
  { { 33.0f }, "lowpass", 0 },       // same
  { { 34.0f }, "highpass", 0 },      // same
  { { 35.0f }, "sharpen", 0 },       // same, worst than atrous in same use-case, less control overall
  { { 38.0f }, "colormapping", 0 },  // same
  { { 39.0f }, "channelmixer", 0 },  // does exactly the same thing as colorin, aka RGB to RGB matrix conversion,
                                     //    but coefs are user-defined instead of calibrated and read from ICC
                                    //    profile. Really versatile yet under-used module, doing linear ops, very
                                    //    good in scene-referred workflow
  { { 40.0f }, "basicadj", 0 },        // module mixing view/model/control at once, usage should be discouraged
  { { 41.0f }, "colorbalance", 0 },    // scene-referred color manipulation
  { { 41.4f }, "splittoningrgb", 0 },  // keyed CAT16 plus RGB mixer before primary warping
  { { 41.45f }, "colorprimaries", 0 }, // editable RGB/CYM primary nodes in dt UCS
  { { 41.5f }, "colorbalancergb", 0 }, // scene-referred color manipulation
  { { 41.6f }, "colorequal", 0 },      // dynamic hue-defined RGB stretching around the achromatic axis
  { { 41.7f }, "drawlayer", 0 },       // TIFF-backed paint layers in scene-referred RGB
  { { 42.0f }, "rgbcurve", 0 },      // really versatile way to edit colour in scene-referred and display-referred
                                     // workflow
  { { 43.0f }, "rgblevels", 0 },     // same
  { { 44.0f }, "basecurve", 0 },     // conversion from scene-referred to display referred, reverse-engineered
                                     //    on camera JPEG default look
  { { 45.0f }, "filmic", 0 },        // same, but different (parametric) approach
  { { 45.5f }, "crystgrain", 0 },       // scene-referred grain, before filmic RGB
  { { 46.0f }, "filmicrgb", 0 },     // same, upgraded
  { { 36.0f }, "lut3d", 0 },         // apply a creative style or film emulation, possibly non-linear
  { { 47.0f }, "colisa", 0 },        // edit contrast while damaging colour
  { { 48.0f }, "tonecurve", 0 },     // same
  { { 49.0f }, "levels", 0 },        // same
  { { 50.0f }, "shadhi", 0 },        // same
  { { 51.0f }, "zonesystem", 0 },    // same
  { { 52.0f }, "globaltonemap", 0 }, // same
  { { 53.0f }, "relight", 0 },       // flatten local contrast while pretending do add lightness
  { { 54.0f }, "bilat", 0 },         // improve clarity/local contrast after all the bad things we have done
                                     //    to it with tonemapping
  { { 55.0f }, "colorcorrection", 0 },  // now that the colours have been damaged by contrast manipulations,
                                        // try to recover them - global adjustment of white balance for shadows and
                                        // highlights
  { { 56.0f }, "colorcontrast", 0 },    // adjust chrominance globally
  { { 57.0f }, "velvia", 0 },           // same
  { { 58.0f }, "vibrance", 0 },         // same, but more subtle
  { { 60.0f }, "colorzones", 0 },       // same, but locally
  { { 61.0f }, "bloom", 0 },            // creative module
  { { 62.0f }, "colorize", 0 },         // creative module
  { { 63.0f }, "lowlight", 0 },         // creative module
  { { 64.0f }, "monochrome", 0 },       // creative module
  { { 65.0f }, "grain", 0 },            // creative module
  { { 66.0f }, "soften", 0 },           // creative module
  { { 67.0f }, "splittoning", 0 },      // creative module
  { { 68.0f }, "vignette", 0 },         // creative module
  { { 69.0f }, "colorreconstruct", 0 }, // try to salvage blown areas before ICC intents in LittleCMS2 do things
                                        // with them.
  { { 70.0f }, "colorout", 0 },
  { { 72.0f }, "finalscale", 0 },
  { { 73.0f }, "overexposed", 0 },
  { { 74.0f }, "rawoverexposed", 0 },
  { { 75.0f }, "dither", 0 },
  { { 76.0f }, "borders", 0 },
  { { 77.0f }, "watermark", 0 },
  { { 78.0f }, "gamma", 0 },
  { { 0.0f }, "", 0 }
};

const dt_iop_order_entry_t ansel_jpg_order[] = {
  // RAW modules. Not used on JPG anyway
  { { 0.5f}, "basebuffer", 0 },
  { { 1.0f }, "rawprepare", 0 },
  { { 2.0f }, "invert", 0 },
  { { 3.0f }, "highlights", 0 },
  { { 4.0f }, "cacorrect", 0 },
  { { 5.0f }, "hotpixels", 0 },
  { { 6.0f }, "rawdenoise", 0 },
  { { 7.0f }, "demosaic", 0 },

  // input color profile: undo RGB TRC/gamma/EOTF
  { { 8.0f }, "colorin", 0 },

  // so from there we are in "linear RGB", meaning there has probably been some tone curve applied
  // on the pixels before saving to raster, but now we don't carry the uint8_t encoding

  { { 9.0f }, "detailmask", 0 },
  { { 10.0f }, "temperature", 0 },
  { { 11.0f }, "denoiseprofile", 0},
  { { 12.0f }, "bilateral", 0},  // RGB surface blur
  { { 13.0f }, "rotatepixels", 0},
  { { 14.0f }, "scalepixels", 0},
  { { 15.0f }, "lens", 0},
  { { 16.0f }, "cacorrectrgb", 0}, // correct chromatic aberrations after lens correction so that lensfun
                                  // does not reintroduce chromatic aberrations when trying to correct them
  { { 17.0f }, "hazeremoval", 0},
  { { 18.0f }, "initialscale", 0 },
  { { 19.0f }, "ashift", 0},
  { { 20.0f }, "flip", 0},
  { { 21.0f }, "clipping", 0},
  { { 22.0f }, "liquify", 0},
  { { 23.0f }, "spots", 0},
  { { 24.0f }, "retouch", 0},
  { { 25.0f }, "mask_manager", 0},

  // Tone corrections
  { { 26.0f }, "exposure", 0},
  { { 27.0f }, "vignette", 0 },       // creative module but emulates lens vignetting, RGB, linear
  { { 28.0f }, "graduatednd", 0},
  { { 29.0f }, "toneequal", 0},       // last module that need enlarged roi_in
  { { 30.0f }, "crop", 0},            // should go after all modules that may need a wider roi_in
  { { 31.0f }, "profile_gamma", 0},   // shouldn't be needed for JPG

  // from there, it's the same as the raw order

  // Linear color handling
  { { 32.0f }, "negadoctor", 0 },      // Cineon film encoding comes after scanner input color profile
  { { 33.0f }, "channelmixerrgb", 0 }, // CAT & new channel mixer
  { { 34.0f }, "channelmixer", 0 },    // Old channel mixer : used HSL...

  // Linear convolutions
  { { 35.0f }, "diffuse", 0 },
  { { 36.0f }, "censorize", 0 },
  { { 37.0f }, "blurs", 0 },        // physically-accurate blurs (motion and lens)

  // Color work in RGB
  { { 38.0f }, "basicadj", 0 },        // legacy shit duplicating features
  { { 39.0f }, "splittoningrgb", 0 },  // keyed CAT16 plus RGB mixer before primary warping
  { { 40.0f }, "colorprimaries", 0 },  // editable RGB/CYM primary nodes in dt UCS
  { { 41.0f }, "colorbalance", 0 },    // scene-referred color manipulation
  { { 42.0f }, "colorbalancergb", 0 }, // scene-referred color manipulation
  { { 43.0f }, "colorequal", 0 },      // dynamic hue-defined RGB stretching around the achromatic axis
  { { 44.0f }, "drawlayer", 0 },       // TIFF-backed paint layers in scene-referred RGB
  { { 45.0f }, "crystgrain", 0 },    // scene-referred grain, before filmic RGB

  // Interpolation for export pipelines: works better before non-linear transforms
  { { 46.0f }, "finalscale", 0 },    

  { { 47.0f }, "tonemap", 0},         // shitty but at least it's unbounded RGB

  // Display transforms: HDR -> SDR
  { { 48.0f }, "filmic", 0 },        // same, but different (parametric) approach
  { { 49.0f }, "filmicrgb", 0 },     // same, upgraded
  { { 50.0f }, "basecurve", 0 },     // conversion from scene-referred to display referred, reverse-engineered
                                     //    on camera JPEG default look

  // SDR modules :

  // Wannabe signal-processing modules but they work in Lab so it's shit
  { { 51.0f }, "nlmeans", 0 },      // denoise
  { { 52.0f }, "defringe", 0 },     // desaturate fringes
  { { 53.0f }, "bilat", 0 },         // local contrast
  { { 54.0f }, "atrous", 0 }, // frequential operation, needs a signal as scene-referred as possible to avoid halos
  { { 55.0f }, "lowpass", 0 },       // same
  { { 56.0f }, "highpass", 0 },      // same
  { { 57.0f }, "sharpen", 0 },       // same, worst than atrous in same use-case, less control overall

  // RGB modules but don't support HDR white
  { { 58.0f }, "lut3d", 0 },         
  { { 59.0f }, "rgbcurve", 0 },
  { { 60.0f }, "rgblevels", 0 },
  { { 61.0f }, "splittoning", 0 },      // HSL inside

  // Lab color modules
  { { 62.0f }, "colorchecker", 0 },  // calibration
  { { 63.0f }, "colormapping", 0 },  // automagic shit. toy filter
  { { 64.0f }, "colorcorrection", 0 },  // now that the colours have been damaged by contrast manipulations,
                                        // try to recover them - global adjustment of white balance for shadows and
                                        // highlights
  { { 65.0f }, "colorcontrast", 0 },    // adjust chrominance globally
  { { 66.0f }, "velvia", 0 },           // same
  { { 67.0f }, "vibrance", 0 },         // same, but more subtle
  { { 68.0f }, "colorzones", 0 },       // same, but locally

  // Legacy Lab shit that should never have existed
  { { 69.0f }, "colisa", 0 },        // contrast, lightness, saturation
  { { 70.0f }, "tonecurve", 0 },     // same
  { { 71.0f }, "levels", 0 },        // same
  { { 72.0f }, "shadhi", 0 },        // same
  { { 73.0f }, "zonesystem", 0 },    // same
  { { 74.0f }, "globaltonemap", 0 }, 

  // Lab toy filters
  { { 75.0f }, "relight", 0 },          // tone EQ but worse
  { { 76.0f }, "bloom", 0 },            // blurs but worse
  { { 77.0f }, "colorize", 0 },         // somewhere between channel mixer and color balance
  { { 78.0f }, "lowlight", 0 },         // simulate scotopic (night) vision
  { { 79.0f }, "monochrome", 0 },       // channel mixer B&W mode but worse
  { { 80.0f }, "grain", 0 },            // crystgrain but worse
  { { 81.0f }, "soften", 0 },           // blurs but worse
  { { 82.0f }, "colorreconstruct", 0 }, // try to salvage blown areas before ICC intents in LittleCMS2 do things
                                        // with them.

  // Display RGB from there
  { { 83.0f }, "colorout", 0 },
  { { 84.0f }, "overexposed", 0 },
  { { 85.0f }, "rawoverexposed", 0 },

  // Those 2 are shit because they internally rely on display RGB being sRGB
  // Doesn't work for large gamut displays...
  { { 86.0f }, "borders", 0 },
  { { 87.0f }, "watermark", 0 },

  // Hide quantization errors with noise
  { { 88.0f }, "dither", 0 },

  // Float to uint8 but only for darkroom pipelines. 
  // Also handles mask previews.
  { { 89.0f }, "gamma", 0 },

  { { 0.0f }, "", 0 }
};

// default order for RAW files, assumed to be linear from start
const dt_iop_order_entry_t ansel_raw_order[] = {
  // RAW stuff
  { { 0.0f }, "basebuffer", 0 },
  { { 1.0f }, "rawprepare", 0},
  { { 2.0f }, "invert", 0},
  { { 3.0f }, "temperature", 0},
  { { 4.0f }, "highlights", 0},
  { { 5.0f }, "cacorrect", 0},
  { { 6.0f }, "hotpixels", 0},
  { { 7.0f }, "rawdenoise", 0},
  { { 8.0f }, "demosaic", 0},

  // Sensor RGB
  { { 9.0f }, "denoiseprofile", 0},
  { {10.0f }, "bilateral", 0},
  { {11.0f }, "rotatepixels", 0},
  { {12.0f }, "scalepixels", 0},
  { {13.0f }, "detailmask", 0},
  { {14.0f }, "lens", 0},
  { {15.0f }, "cacorrectrgb", 0}, // correct chromatic aberrations after lens correction so that lensfun
                                  // does not reintroduce chromatic aberrations when trying to correct them
  { {16.0f }, "hazeremoval", 0},

  { {17.0f }, "initialscale", 0},
  { {18.0f }, "ashift", 0},
  { {19.0f }, "flip", 0},
  { {20.0f }, "clipping", 0},
  { {21.0f }, "liquify", 0},
  { {22.0f }, "spots", 0},
  { {23.0f }, "retouch", 0},

  // From there we support masking in modules
  { {24.0f }, "mask_manager", 0},

  // Linear tone corrections
  { {25.0f }, "exposure", 0},
  { {26.0f }, "vignette", 0 },       // creative module but emulates lens vignetting, RGB, linear
  { {27.0f }, "graduatednd", 0},
  { {28.0f }, "toneequal", 0},       // last module that need enlarged roi_in
  { {29.0f }, "crop", 0},            // should go after all modules that may need a wider roi_in

  // Needed by some very old cameras in RAW mode
  { {30.0f }, "profile_gamma", 0},

  { {31.0f }, "colorin", 0},

  // from there, it's the same as the JPEG order

  // Linear color handling
  { { 32.0f }, "negadoctor", 0 },      // Cineon film encoding comes after scanner input color profile
  { { 33.0f }, "channelmixerrgb", 0 }, // CAT & new channel mixer
  { { 34.0f }, "channelmixer", 0 },    // Old channel mixer : used HSL...

  // Linear convolutions
  { { 35.0f }, "diffuse", 0 },
  { { 36.0f }, "censorize", 0 },
  { { 37.0f }, "blurs", 0 },        // physically-accurate blurs (motion and lens)

  // Color work in RGB
  { { 38.0f }, "basicadj", 0 },        // legacy shit duplicating features
  { { 39.0f }, "splittoningrgb", 0 },  // keyed CAT16 plus RGB mixer before primary warping
  { { 40.0f }, "colorprimaries", 0 },  // editable RGB/CYM primary nodes in dt UCS
  { { 41.0f }, "colorbalance", 0 },    // scene-referred color manipulation
  { { 42.0f }, "colorbalancergb", 0 }, // scene-referred color manipulation
  { { 43.0f }, "colorequal", 0 },      // dynamic hue-defined RGB stretching around the achromatic axis
  { { 44.0f }, "drawlayer", 0 },       // TIFF-backed paint layers in scene-referred RGB
  { { 45.0f }, "crystgrain", 0 },    // scene-referred grain, before filmic RGB

  // Interpolation for export pipelines: works better before non-linear transforms
  { { 46.0f }, "finalscale", 0 },    

  { { 47.0f }, "tonemap", 0},         // shitty but at least it's unbounded RGB

  // Display transforms: HDR -> SDR
  { { 48.0f }, "filmic", 0 },        // same, but different (parametric) approach
  { { 49.0f }, "filmicrgb", 0 },     // same, upgraded
  { { 50.0f }, "basecurve", 0 },     // conversion from scene-referred to display referred, reverse-engineered
                                     //    on camera JPEG default look

  // SDR modules :

  // Wannabe signal-processing modules but they work in Lab so it's shit
  { { 51.0f }, "nlmeans", 0 },      // denoise
  { { 52.0f }, "defringe", 0 },     // desaturate fringes
  { { 53.0f }, "bilat", 0 },         // local contrast
  { { 54.0f }, "atrous", 0 }, // frequential operation, needs a signal as scene-referred as possible to avoid halos
  { { 55.0f }, "lowpass", 0 },       // same
  { { 56.0f }, "highpass", 0 },      // same
  { { 57.0f }, "sharpen", 0 },       // same, worst than atrous in same use-case, less control overall

  // RGB modules but don't support HDR white
  { { 58.0f }, "lut3d", 0 },         
  { { 59.0f }, "rgbcurve", 0 },
  { { 60.0f }, "rgblevels", 0 },
  { { 61.0f }, "splittoning", 0 },      // HSL inside

  // Lab color modules
  { { 62.0f }, "colorchecker", 0 },  // calibration
  { { 63.0f }, "colormapping", 0 },  // automagic shit. toy filter
  { { 64.0f }, "colorcorrection", 0 },  // now that the colours have been damaged by contrast manipulations,
                                        // try to recover them - global adjustment of white balance for shadows and
                                        // highlights
  { { 65.0f }, "colorcontrast", 0 },    // adjust chrominance globally
  { { 66.0f }, "velvia", 0 },           // same
  { { 67.0f }, "vibrance", 0 },         // same, but more subtle
  { { 68.0f }, "colorzones", 0 },       // same, but locally

  // Legacy Lab shit that should never have existed
  { { 69.0f }, "colisa", 0 },        // contrast, lightness, saturation
  { { 70.0f }, "tonecurve", 0 },     // same
  { { 71.0f }, "levels", 0 },        // same
  { { 72.0f }, "shadhi", 0 },        // same
  { { 73.0f }, "zonesystem", 0 },    // same
  { { 74.0f }, "globaltonemap", 0 }, 

  // Lab toy filters
  { { 75.0f }, "relight", 0 },          // tone EQ but worse
  { { 76.0f }, "bloom", 0 },            // blurs but worse
  { { 77.0f }, "colorize", 0 },         // somewhere between channel mixer and color balance
  { { 78.0f }, "lowlight", 0 },         // simulate scotopic (night) vision
  { { 79.0f }, "monochrome", 0 },       // channel mixer B&W mode but worse
  { { 80.0f }, "grain", 0 },            // crystgrain but worse
  { { 81.0f }, "soften", 0 },           // blurs but worse
  { { 82.0f }, "colorreconstruct", 0 }, // try to salvage blown areas before ICC intents in LittleCMS2 do things
                                        // with them.

  // Display RGB from there
  { { 83.0f }, "colorout", 0 },
  { { 84.0f }, "overexposed", 0 },
  { { 85.0f }, "rawoverexposed", 0 },

  // Those 2 are shit because they internally rely on display RGB being sRGB
  // Doesn't work for large gamut displays...
  { { 86.0f }, "borders", 0 },
  { { 87.0f }, "watermark", 0 },

  // Hide quantization errors with noise
  { { 88.0f }, "dither", 0 },

  // Float to uint8 but only for darkroom pipelines. 
  // Also handles mask previews.
  { { 89.0f }, "gamma", 0 },

  { { 0.0f }, "", 0 }
};

static void *_dup_iop_order_entry(const void *src, gpointer data);
static int _count_entries_operation(GList *e_list, const char *operation);


/**
 * @brief Insert a missing module entry before another module in an order list.
 *
 * This is used to migrate older/custom lists when new modules appear in
 * built-in orders.
 *
 * @param iop_order_list Order list to update.
 * @param module Existing module name to insert before.
 * @param new_module New module name to insert if missing.
 * @return Updated list head.
 */
static GList *_insert_before(GList *iop_order_list, const char *module, const char *new_module)
{
  gboolean exists = FALSE;

  // First check that new module is missing
  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(entry->operation, new_module))
    {
      exists = TRUE;
      break;
    }
  }

  // Insert it if needed
  if(!exists)
  {
    for(GList *l = iop_order_list; l; l = g_list_next(l))
    {
      dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;

      if(!strcmp(entry->operation, module))
      {
        dt_iop_order_entry_t *new_entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

        g_strlcpy(new_entry->operation, new_module, sizeof(new_entry->operation));
        new_entry->instance = 0;
        new_entry->o.iop_order = 0;

        // Capture the returned pointer! g_list_insert_before may change the head.
        iop_order_list = g_list_insert_before(iop_order_list, l, new_entry);
        break;
      }
    }
  }

  return iop_order_list;
}


dt_iop_order_t dt_ioppr_get_iop_order_version(const int32_t imgid)
{
  dt_iop_order_t iop_order_version = DT_IOP_ORDER_ANSEL_RAW;

  // check current iop order version
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT version FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    iop_order_version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  return iop_order_version;
}

// a rule prevents operations to be switched,
// that is a prev operation will not be allowed to be moved on top of the next operation.
GList *dt_ioppr_get_iop_order_rules()
{
  GList *rules = NULL;

  const dt_iop_order_rule_t rule_entry[] = {
    { .op_prev = "basebuffer",  .op_next = "rawprepare"  },
    { .op_prev = "rawprepare",  .op_next = "invert"      },
    { .op_prev = "invert",      .op_next = "temperature" },
    { .op_prev = "temperature", .op_next = "highlights"  },
    { .op_prev = "highlights",  .op_next = "cacorrect"   },
    { .op_prev = "cacorrect",   .op_next = "hotpixels"   },
    { .op_prev = "hotpixels",   .op_next = "rawdenoise"  },
    { .op_prev = "rawdenoise",  .op_next = "demosaic"    },
    { .op_prev = "demosaic",    .op_next = "colorin"     },
    { .op_prev = "colorin",     .op_next = "colorout"    },
    { .op_prev = "colorout",    .op_next = "gamma"       },
    { .op_prev = "flip",        .op_next = "crop"        }, // crop GUI broken if flip is done on top
    { .op_prev = "flip",        .op_next = "clipping"    }, // clipping GUI broken if flip is done on top
    { .op_prev = "ashift",      .op_next = "clipping"    }, // clipping GUI broken if ashift is done on top
    { .op_prev = "colorin",     .op_next = "channelmixerrgb"},
    { "\0", "\0" } };

  int i = 0;
  while(rule_entry[i].op_prev[0])
  {
    dt_iop_order_rule_t *rule = calloc(1, sizeof(dt_iop_order_rule_t));

    memcpy(rule->op_prev, rule_entry[i].op_prev, sizeof(rule->op_prev));
    memcpy(rule->op_next, rule_entry[i].op_next, sizeof(rule->op_next));

    rules = g_list_prepend(rules, rule);
    i++;
  }

  return g_list_reverse(rules);  // list was built in reverse order, so un-reverse it
}

GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  GList *link = NULL;

  for(GList *iops_order = iop_order_list; iops_order; iops_order = g_list_next(iops_order))
  {
    dt_iop_order_entry_t *order_entry = (dt_iop_order_entry_t *)iops_order->data;

    if(strcmp(order_entry->operation, op_name) == 0
       && (order_entry->instance == multi_priority || multi_priority == -1))
    {
      link = iops_order;
      break;
    }
  }

  return link;
}

/**
 * @brief Return the first order entry matching operation/instance.
 *
 * @param iop_order_list Order list.
 * @param op_name Operation name.
 * @param multi_priority Instance priority (or -1 for any).
 * @return Matching entry or NULL.
 */
static dt_iop_order_entry_t *dt_ioppr_get_iop_order_entry(GList *iop_order_list, const char *op_name,
                                                          const int multi_priority)
{
  const GList * const restrict link = dt_ioppr_get_iop_order_link(iop_order_list, op_name, multi_priority);
  if(link)
    return (dt_iop_order_entry_t *)link->data;
  else
    return NULL;
}

// returns the iop_order associated with the iop order entry that matches operation == op_name
int dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name, const int multi_priority)
{
  int iop_order = INT_MAX;
  const dt_iop_order_entry_t *const restrict order_entry =
    dt_ioppr_get_iop_order_entry(iop_order_list, op_name, multi_priority);

  if(order_entry)
  {
    iop_order = order_entry->o.iop_order;
  }
  else if(!dt_deprecated(op_name))
    fprintf(stderr, "cannot get iop-order for %s instance %d\n", op_name, multi_priority);

  return iop_order;
}

gint dt_sort_iop_list_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *const restrict am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *const restrict bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order > bm->o.iop_order) return 1;
  if(am->o.iop_order < bm->o.iop_order) return -1;
  return 0;
}

gint dt_sort_iop_list_by_order_f(gconstpointer a, gconstpointer b)
{
  const dt_iop_order_entry_t *const restrict am = (const dt_iop_order_entry_t *)a;
  const dt_iop_order_entry_t *const restrict bm = (const dt_iop_order_entry_t *)b;
  if(am->o.iop_order_f > bm->o.iop_order_f) return 1;
  if(am->o.iop_order_f < bm->o.iop_order_f) return -1;
  return 0;
}

static const dt_iop_order_entry_t *orders[5]
    = { legacy_order, v30_order, v30_jpg_order, ansel_raw_order, ansel_jpg_order };

dt_iop_order_t dt_ioppr_get_iop_order_list_kind(GList *iop_order_list)
{
  for(int i = 0; i < 5; i++)
  {
    int k = 0;
    GList *l = iop_order_list;
    gboolean ok = TRUE;

    while(l)
    {
      const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
      if(strcmp(orders[i][k].operation, entry->operation))
      {
        ok = FALSE;
        break;
      }
      else
      {
        // skip all the other instance of same module if any
        while(g_list_next(l)
              && !strcmp(orders[i][k].operation, ((dt_iop_order_entry_t *)(g_list_next(l)->data))->operation))
          l = g_list_next(l);
      }

      k++;
      l = g_list_next(l);
    }

    if(ok) return i;
  }

  return DT_IOP_ORDER_CUSTOM;
}

gboolean dt_ioppr_has_multiple_instances(GList *iop_order_list)
{
  GList *l = iop_order_list;

  while(l)
  {
    GList *next = g_list_next(l);
    if(next
       && (strcmp(((dt_iop_order_entry_t *)(l->data))->operation,
                  ((dt_iop_order_entry_t *)(next->data))->operation) == 0))
    {
      return TRUE;
    }
    l = next;
  }
  return FALSE;
}

/**
 * @brief Persist an order list for a given image with a specific kind.
 *
 * Handles both built-in orders (stores version only) and custom orders
 * (stores serialized list in DB).
 *
 * @param kind Order kind to store.
 * @param iop_order_list Order list to serialize if needed.
 * @param imgid Image id.
 * @return TRUE on success, FALSE on error.
 */
static gboolean dt_ioppr_write_iop_order(const dt_iop_order_t kind, GList *iop_order_list, const int32_t imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO main.module_order VALUES (?1, 0, NULL)", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
  sqlite3_finalize(stmt);

  if(kind == DT_IOP_ORDER_CUSTOM || dt_ioppr_has_multiple_instances(iop_order_list))
  {
    gchar *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_order_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = ?3 WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list_txt, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
    sqlite3_finalize(stmt);

    dt_free(iop_list_txt);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.module_order SET version = ?2, iop_list = NULL WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, kind);
    if(sqlite3_step(stmt) != SQLITE_DONE) return FALSE;
    sqlite3_finalize(stmt);
  }

  return TRUE;
}

gboolean dt_ioppr_write_iop_order_list(GList *iop_order_list, const int32_t imgid)
{
  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(iop_order_list);
  return dt_ioppr_write_iop_order(kind, iop_order_list, imgid);
}

/**
 * @brief Build an order list from a static entry table.
 *
 * @param entries Zero-terminated array of @ref dt_iop_order_entry_t.
 * @return Newly-allocated order list.
 */
GList *_table_to_list(const dt_iop_order_entry_t entries[])
{
  GList *iop_order_list = NULL;
  int k = 0;
  while(entries[k].operation[0])
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    g_strlcpy(entry->operation, entries[k].operation, sizeof(entry->operation));
    entry->instance = 0;
    entry->o.iop_order_f = entries[k].o.iop_order_f;
    iop_order_list = g_list_prepend(iop_order_list, entry);

    k++;
  }

  return g_list_reverse(iop_order_list);  // list was built in reverse order, so un-reverse it
}

GList *dt_ioppr_get_iop_order_list_version(dt_iop_order_t version)
{
  GList *iop_order_list = NULL;

  if(version == DT_IOP_ORDER_LEGACY)
  {
    iop_order_list = _table_to_list(legacy_order);
  }
  else if(version == DT_IOP_ORDER_V30)
  {
    iop_order_list = _table_to_list(v30_order);
  }
  else if(version == DT_IOP_ORDER_V30_JPG)
  {
    iop_order_list = _table_to_list(v30_jpg_order);
  }
  else if(version == DT_IOP_ORDER_ANSEL_RAW)
  {
    iop_order_list = _table_to_list(ansel_raw_order);
  }
  else if(version == DT_IOP_ORDER_ANSEL_JPG)
  {
    iop_order_list = _table_to_list(ansel_jpg_order);
  }


  return iop_order_list;
}

gboolean dt_ioppr_has_iop_order_list(int32_t imgid)
{
  gboolean result = FALSE;
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT version, iop_list"
                              " FROM main.module_order"
                              " WHERE imgid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    result = (sqlite3_column_type(stmt, 1) != SQLITE_NULL);
  }

  sqlite3_finalize(stmt);

  return result;
}

GList *dt_ioppr_get_iop_order_list(int32_t imgid, gboolean sorted)
{
  GList *iop_order_list = NULL;

  if(imgid > 0)
  {
    sqlite3_stmt *stmt;

    // we read the iop-order-list in the preset table, the actual version is
    // the first int32_t serialized into the io_params. This is then a sequential
    // search, but there will not be many such presets and we do call this routine
    // only when loading an image and when changing the iop-order.

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid=?1", -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const dt_iop_order_t version = sqlite3_column_int(stmt, 0);
      const gboolean has_iop_list = (sqlite3_column_type(stmt, 1) != SQLITE_NULL);

      if(version == DT_IOP_ORDER_CUSTOM || has_iop_list)
      {
        const char *buf = (char *)sqlite3_column_text(stmt, 1);
        if(buf) iop_order_list = dt_ioppr_deserialize_text_iop_order_list(buf);

        if(!iop_order_list)
        {
          // preset not found, fall back to last built-in version, will be loaded below
          fprintf(stderr, "[dt_ioppr_get_iop_order_list] error building iop_order_list imgid %d\n", imgid);
        }
        else
        {
          // @@_NEW_MODULE: For new module it is required to insert the new module name in the iop-order list here.
          //                The insertion can be done depending on the current iop-order list kind.
          iop_order_list = _insert_before(iop_order_list, "nlmeans", "negadoctor");
          iop_order_list = _insert_before(iop_order_list, "negadoctor", "channelmixerrgb");
          iop_order_list = _insert_before(iop_order_list, "negadoctor", "censorize");
          iop_order_list = _insert_before(iop_order_list, "rgbcurve", "colorbalancergb");
          iop_order_list = _insert_before(iop_order_list, "colorbalancergb", "colorprimaries");
          iop_order_list = _insert_before(iop_order_list, "colorprimaries", "splittoningrgb");
          iop_order_list = _insert_before(iop_order_list, "rgbcurve", "drawlayer");
          iop_order_list = _insert_before(iop_order_list, "drawlayer", "colorequal");
          iop_order_list = _insert_before(iop_order_list, "ashift", "cacorrectrgb");
          iop_order_list = _insert_before(iop_order_list, "graduatednd", "crop");
          iop_order_list = _insert_before(iop_order_list, "colorbalance", "diffuse");
          iop_order_list = _insert_before(iop_order_list, "nlmeans", "blurs");
          iop_order_list = _insert_before(iop_order_list, "ashift", "initialscale");
          iop_order_list = _insert_before(iop_order_list, "filmicrgb", "crystgrain");
          iop_order_list = _insert_before(iop_order_list, "maskmanager", "detailmask");
          iop_order_list = _insert_before(iop_order_list, "rawprepare", "basebuffer");
        }
      }
      else if(version == DT_IOP_ORDER_LEGACY)
      {
        iop_order_list = _table_to_list(legacy_order);
      }
      else if(version == DT_IOP_ORDER_V30)
      {
        iop_order_list = _table_to_list(v30_order);
      }
      else if(version == DT_IOP_ORDER_V30_JPG)
      {
        iop_order_list = _table_to_list(v30_jpg_order);
      }
      else if(version == DT_IOP_ORDER_ANSEL_RAW)
      {
        iop_order_list = _table_to_list(ansel_raw_order);
      }
      else if(version == DT_IOP_ORDER_ANSEL_JPG)
      {
        iop_order_list = _table_to_list(ansel_jpg_order);
      }
      else
        fprintf(stderr, "[dt_ioppr_get_iop_order_list] invalid iop order version %d for imgid %d\n", version, imgid);

      if(iop_order_list)
      {
        _ioppr_reset_iop_order(iop_order_list);
        // Perform sanity check after migration to ensure the list is valid
        if(!_ioppr_sanity_check_iop_order(iop_order_list))
        {
          g_list_free_full(iop_order_list, dt_free_gpointer);
          iop_order_list = NULL;
          fprintf(stderr, "[dt_ioppr_get_iop_order_list] sanity check failed for imgid %d, falling back to default\n", imgid);
        }
      }
    }

    sqlite3_finalize(stmt);
  }

  // fallback to last iop order list (also used to initialize the pipe when imgid = UNKNOWN_IMAGE)
  // and new image not yet loaded or whose history has been reset.
  if(!iop_order_list) iop_order_list = _table_to_list(ansel_raw_order);

  if(sorted) iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order);

  return iop_order_list;
}

/**
 * @brief Reset iop_order values to a sequential order for a list.
 *
 * Ensures the list has monotonically increasing iop_order values.
 *
 * @param iop_order_list Order list to normalize.
 */
static void _ioppr_reset_iop_order(GList *iop_order_list)
{
  // iop-order must start with a number > 0 and be incremented. There is no
  // other constraints.
  int iop_order = 1;
  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)l->data;
    e->o.iop_order = iop_order++;
  }
}

/**
 * @brief Resynchronize dev->iop list order against dev->iop_order_list.
 *
 * This updates each module's iop_order and then sorts the module list.
 *
 * @param dev Develop context.
 */
static void dt_ioppr_resync_iop_list(dt_develop_t *dev)
{
  // make sure that the iop_order_list does not contains possibly removed modules

  GList *l = dev->iop_order_list;
  while(l)
  {
    GList *next = g_list_next(l); // need to get next pointer now, as we may be deleting this node
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
    const dt_iop_module_t *const restrict mod = dt_iop_get_module_by_op_priority(dev->iop, e->operation, e->instance);
    if(IS_NULL_PTR(mod))
    {
      dev->iop_order_list = g_list_remove_link(dev->iop_order_list, l);
    }

    l = next;
  }
}

void dt_ioppr_resync_pipeline(dt_develop_t *dev, const int32_t imgid, const char *msg, gboolean check_duplicates)
{
  dt_ioppr_resync_modules_order(dev);
  dt_ioppr_resync_iop_list(dev);
  if(check_duplicates) dt_ioppr_check_duplicate_iop_order(&dev->iop, dev->history);
  if(msg) dt_ioppr_check_iop_order(dev, imgid, msg);
}

void dt_ioppr_resync_modules_order(dt_develop_t *dev)
{
  _ioppr_reset_iop_order(dev->iop_order_list);

  // and reset all module iop_order

  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);
    GList *next = g_list_next(modules);

    // modules with iop_order set to INT_MAX we keep them as they will be removed (non visible)
    // _update_iop_visibility.
    if(mod->iop_order != INT_MAX)
      mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);

    modules = next;
  }

  dev->iop = g_list_sort(dev->iop, dt_sort_iop_by_order);
}

void dt_ioppr_rebuild_iop_order_from_modules(struct dt_develop_t *dev, GList *ordered_modules)
{
  if(IS_NULL_PTR(dev) || !ordered_modules) return;

  GList *new_iop_order_list = NULL;
  int order = 1;

  for(const GList *l = ordered_modules; l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(IS_NULL_PTR(mod)) continue;

    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)calloc(1, sizeof(dt_iop_order_entry_t));
    g_strlcpy(entry->operation, mod->op, sizeof(entry->operation));
    entry->instance = mod->multi_priority;
    g_strlcpy(entry->name, mod->multi_name, sizeof(entry->name));
    entry->o.iop_order = order++;
    new_iop_order_list = g_list_append(new_iop_order_list, entry);
  }

  if(new_iop_order_list)
  {
    g_list_free_full(dev->iop_order_list, dt_free_gpointer);
    dev->iop_order_list = new_iop_order_list;
    dt_ioppr_resync_modules_order(dev);
  }
}

// sets the iop_order on each module of *_iop_list
// iop_order is set only for base modules, multi-instances will be flagged as unused with INT_MAX
// if a module do not exists on iop_order_list it is flagged as unused with INT_MAX
void dt_ioppr_set_default_iop_order(dt_develop_t *dev, const int32_t imgid)
{
  // get the iop-order for this image

  GList *iop_order_list = dt_ioppr_get_iop_order_list(imgid, FALSE);

  // we assign a single iop-order to each module

  _ioppr_reset_iop_order(iop_order_list);

  if(dev->iop_order_list)
  {
    g_list_free_full(dev->iop_order_list, dt_free_gpointer);
    dev->iop_order_list = NULL;
  }
  dev->iop_order_list = iop_order_list;

  // we now set the module list given to this iop-order

  dt_ioppr_resync_modules_order(dev);
}

/**
 * @brief Apply a new order list by reloading history and rebuilding UI/pipelines.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 */
static void dt_ioppr_migrate_iop_order(struct dt_develop_t *dev, const int32_t imgid)
{
  dt_ioppr_set_default_iop_order(dev, imgid);
  dt_dev_reload_history_items(dev, dev->image_storage.id);
  dt_dev_history_gui_update(dev);
  dt_dev_history_pixelpipe_update(dev, TRUE);
  dt_dev_history_notify_change(dev, imgid);
}

void dt_ioppr_change_iop_order(struct dt_develop_t *dev, const int32_t imgid, GList *new_iop_list)
{
  GList *iop_list = dt_ioppr_iop_order_copy_deep(new_iop_list);
  GList *mi = dt_ioppr_extract_multi_instances_list(darktable.develop->iop_order_list);

  if(mi) iop_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi);

  dt_dev_write_history(darktable.develop, FALSE);
  dt_ioppr_write_iop_order(DT_IOP_ORDER_CUSTOM, iop_list, imgid);
  g_list_free_full(iop_list, dt_free_gpointer);
  iop_list = NULL;

  dt_ioppr_migrate_iop_order(darktable.develop, imgid);
}

/**
 * @brief Extract all order entries that have multiple instances.
 *
 * @param iop_order_list Order list to scan.
 * @return List of duplicated entries corresponding to multi-instance ops.
 */
static GList *dt_ioppr_extract_multi_instances_list(GList *iop_order_list)
{
  GList *mi = NULL;

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;

    if(_count_entries_operation(iop_order_list, entry->operation) > 1)
    {
      dt_iop_order_entry_t *copy = (dt_iop_order_entry_t *)_dup_iop_order_entry((void *)entry, NULL);
      mi = g_list_prepend(mi, copy);
    }
  }

  return g_list_reverse(mi);  // list was built in reverse order, so un-reverse it
}

/**
 * @brief Merge an operation's multiple instances into the order list.
 *
 * Updates instance numbers in-place and inserts additional entries as needed.
 *
 * @param iop_order_list Base order list.
 * @param operation Operation name.
 * @param multi_instance_list List of instances for that operation.
 * @return Updated order list.
 */
GList *dt_ioppr_merge_module_multi_instance_iop_order_list(GList *iop_order_list,
                                                           const char *operation, GList *multi_instance_list)
{
  const int count_to = _count_entries_operation(iop_order_list, operation);

  int item_nb = 0;

  GList *link = iop_order_list;

  for(const GList *l = multi_instance_list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;

    item_nb++;

    if(item_nb <= count_to)
    {
      link = dt_ioppr_get_iop_order_link(link, operation, -1);
      dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)link->data;
      e->instance = entry->instance;

      // free this entry as not merged into the list
      dt_free(entry);

      // next replace should happen to any module after this one
      link = g_list_next(link);
    }
    else
    {
      iop_order_list = g_list_insert_before(iop_order_list, link, entry);
    }
  }

  // if needed removes all other instance of this operation which are superfluous
  if(g_list_shorter_than(multi_instance_list, count_to))
  {
    while(link)
    {
      const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)link->data;
      GList *next = g_list_next(link);
      if(strcmp(operation, entry->operation) == 0)
      {
        iop_order_list = g_list_remove_link(iop_order_list, link);
      }

      link = next;
    }
  }

  return iop_order_list;
}

/**
 * @brief Merge multiple-instance entries into an order list.
 *
 * Groups all instances of the same operation together, then merges them
 * back into the main list while preserving relative ordering.
 *
 * @param iop_order_list Base order list to update.
 * @param multi_instance_list List of entries containing multiple instances.
 * @return Updated order list.
 */
static GList *dt_ioppr_merge_multi_instance_iop_order_list(GList *iop_order_list, GList *multi_instance_list)
{
  GList *op = NULL;

  GList *copy = dt_ioppr_iop_order_copy_deep(multi_instance_list);
  GList *l = copy;

  while(l)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)l->data;
    GList *l_next = g_list_next(l);

    op = g_list_append(op, entry);

    copy = g_list_remove_link(copy, l);

    GList *mi = l_next;
    while(mi)
    {
      GList *next = g_list_next(mi);
      dt_iop_order_entry_t *mi_entry = (dt_iop_order_entry_t *)mi->data;
      if(strcmp(entry->operation, mi_entry->operation) == 0)
      {
        op = g_list_append(op, mi_entry);
        copy = g_list_remove_link(copy, mi);
      }

      mi = next;
    }

    // copy operation as entry may be freed
    char operation[20];
    memcpy(operation, entry->operation, sizeof(entry->operation));

    iop_order_list = dt_ioppr_merge_module_multi_instance_iop_order_list(iop_order_list, operation, op);

    g_list_free(op);
    op = NULL;

    l = copy;
  }

  return iop_order_list;
}

/**
 * @brief Count module instances and track their highest priorities.
 *
 * @param iop Module list.
 * @param operation Operation name to count.
 * @param max_multi_priority Output: maximum multi_priority among all instances.
 * @param count Output: total number of instances.
 * @param max_multi_priority_enabled Output: maximum multi_priority among enabled instances.
 * @param count_enabled Output: number of enabled instances.
 */
static void _count_iop_module(GList *iop, const char *operation, int *max_multi_priority, int *count,
                              int *max_multi_priority_enabled, int *count_enabled)
{
  *max_multi_priority = 0;
  *count = 0;
  *max_multi_priority_enabled = 0;
  *count_enabled = 0;

  for(const GList *modules = iop; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
    if(!strcmp(mod->op, operation))
    {
      (*count)++;
      if(*max_multi_priority < mod->multi_priority) *max_multi_priority = mod->multi_priority;

      if(mod->enabled)
      {
        (*count_enabled)++;
        if(*max_multi_priority_enabled < mod->multi_priority) *max_multi_priority_enabled = mod->multi_priority;
      }
    }
  }

  assert(*count >= *count_enabled);
}

/**
 * @brief Count order-list entries matching an operation name.
 *
 * @param e_list Entry list.
 * @param operation Operation name to match.
 * @return Count of entries.
 */
static int _count_entries_operation(GList *e_list, const char *operation)
{
  int count = 0;

  for(const GList *l = e_list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) count++;
  }

  return count;
}

/**
 * @brief Check if an operation was already handled earlier in the list.
 *
 * @param e_list Current list node.
 * @param operation Operation name to search.
 * @return TRUE if found earlier, FALSE otherwise.
 */
static gboolean _operation_already_handled(GList *e_list, const char *operation)
{
  for(const GList *l = g_list_previous(e_list); l; l = g_list_previous(l))
  {
    const dt_iop_order_entry_t *const restrict ep = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(ep->operation, operation)) return TRUE;
  }
  return FALSE;
}

/**
 * @brief Return the multi_priority of the n-th instance of an operation.
 *
 * @param dev Develop context.
 * @param operation Operation name.
 * @param n Instance index (1-based).
 * @param only_disabled If TRUE, only consider disabled instances.
 * @return multi_priority or INT_MAX if not found.
 */
int _get_multi_priority(dt_develop_t *dev, const char *operation, const int n, const gboolean only_disabled)
{
  int count = 0;
  for(const GList *l = dev->iop; l; l = g_list_next(l))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)l->data;
    if((!only_disabled || mod->enabled == FALSE) && !strcmp(mod->op, operation))
    {
      count++;
      if(count == n) return mod->multi_priority;
    }
  }

  return INT_MAX;
}

/**
 * @brief Update dev->iop_order_list to include entries from @p entry_list.
 *
 * Used by update paths for modules and style items.
 *
 * @param dev Develop context.
 * @param entry_list List of @ref dt_iop_order_entry_t.
 * @param append Whether to append missing entries at the end.
 */
static void dt_ioppr_update_for_entries(dt_develop_t *dev, GList *entry_list, gboolean append)
{

  // for each priority list to be checked
  for(GList *e_list = entry_list; e_list; e_list = g_list_next(e_list))
  {
    const dt_iop_order_entry_t *const restrict ep = (dt_iop_order_entry_t *)e_list->data;

    gboolean force_append = FALSE;

    // we also need to force append (even if overwrite mode is
    // selected - append = FALSE) when a module has a specific name
    // and this name is not present into the current iop list.

    if(*ep->name && !dt_iop_get_module_by_instance_name(dev->iop, ep->operation, ep->name))
      force_append = TRUE;

    int max_multi_priority = 0, count = 0;
    int max_multi_priority_enabled = 0, count_enabled = 0;

    // is it a currently active module and if so how many active instances we have
    _count_iop_module(dev->iop, ep->operation,
                      &max_multi_priority, &count, &max_multi_priority_enabled, &count_enabled);

    // look for this operation into the target iop-order list and add there as much operation as needed

    for(GList *l = g_list_last(dev->iop_order_list); l; l = g_list_previous(l))
    {
      const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
      if(!strcmp(e->operation, ep->operation) && !_operation_already_handled(e_list, ep->operation))
      {
        // how many instances of this module in the entry list, and re-number multi-priority accordingly
        const int new_active_instances = _count_entries_operation(entry_list, ep->operation);

        int add_count = 0;
        int start_multi_priority = 0;
        int nb_replace = 0;

        if(append || force_append)
        {
          nb_replace = count - count_enabled;
          add_count = MAX(0, new_active_instances - nb_replace);
          start_multi_priority = max_multi_priority + 1;
        }
        else
        {
          nb_replace = count;
          add_count = MAX(0, new_active_instances - count);
          start_multi_priority = max_multi_priority + 1;
        }

        // update multi_priority to be unique in iop list
        int multi_priority = start_multi_priority;
        int nb = 0;

        for(const GList *s = entry_list; s; s = g_list_next(s))
        {
          dt_iop_order_entry_t *item = (dt_iop_order_entry_t *)s->data;
          if(!strcmp(item->operation, e->operation))
          {
            nb++;
            if(nb <= nb_replace)
            {
              // this one replaces current module, get it's multi-priority
              item->instance = _get_multi_priority(dev, item->operation, nb, append);
            }
            else
            {
              // otherwise create a new multi-priority
              item->instance = multi_priority++;
            }
          }
        }

        multi_priority = start_multi_priority;

        l = g_list_next(l);

        for(int k = 0; k<add_count; k++)
        {
          dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
          g_strlcpy(n->operation, ep->operation, sizeof(n->operation));
          n->instance = multi_priority++;
          n->o.iop_order = 0;
          dev->iop_order_list = g_list_insert_before(dev->iop_order_list, l, n);
        }
        break;
      }
    }
  }

  _ioppr_reset_iop_order(dev->iop_order_list);

//  dt_ioppr_print_iop_order(dev->iop_order_list, "upd sitem");
}

void dt_ioppr_update_for_style_items(dt_develop_t *dev, GList *st_items, gboolean append)
{
  GList *e_list = NULL;

  // for each priority list to be checked
  for(const GList *si_list = st_items; si_list; si_list = g_list_next(si_list))
  {
    const dt_style_item_t *const restrict si = (dt_style_item_t *)si_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    memcpy(n->operation, si->operation, sizeof(n->operation));
    n->instance = si->multi_priority;
    g_strlcpy(n->name, si->multi_name, sizeof(n->name));
    n->o.iop_order = 0;
    e_list = g_list_prepend(e_list, n);
  }
  e_list = g_list_reverse(e_list);  // list was built in reverse order, so un-reverse it

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  GList *el = e_list;
  for(const GList *si_list = st_items; si_list; si_list = g_list_next(si_list))
  {
    dt_style_item_t *si = (dt_style_item_t *)si_list->data;
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)el->data;

    si->multi_priority = e->instance;
    si->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, si->operation, si->multi_priority);
    el = g_list_next(el);
  }

  g_list_free(e_list);
  e_list = NULL;
}

void dt_ioppr_update_for_modules(dt_develop_t *dev, GList *modules, gboolean append)
{
  GList *e_list = NULL;

  // for each priority list to be checked
  for(const GList *m_list = modules; m_list; m_list = g_list_next(m_list))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)m_list->data;

    dt_iop_order_entry_t *n = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    g_strlcpy(n->operation, mod->op, sizeof(n->operation));
    n->instance = mod->multi_priority;
    g_strlcpy(n->name, mod->multi_name, sizeof(n->name));
    n->o.iop_order = 0;
    e_list = g_list_prepend(e_list, n);
  }
  e_list = g_list_reverse(e_list);  // list was built in reverse order, so un-reverse it

  dt_ioppr_update_for_entries(dev, e_list, append);

  // write back the multi-priority

  GList *el = e_list;
  for(const GList *m_list = modules; m_list; m_list = g_list_next(m_list))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m_list->data;
    dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)el->data;

    mod->multi_priority = e->instance;
    mod->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, mod->op, mod->multi_priority);

    el = g_list_next(el);
  }

  g_list_free_full(e_list, dt_free_gpointer);
  e_list = NULL;
}

// returns the first dt_dev_history_item_t on history_list where hist->module == mod
/**
 * @brief Find a history item referencing a given module instance.
 *
 * @param history_list History list.
 * @param mod Module instance.
 * @return First matching history item or NULL.
 */
static dt_dev_history_item_t *_ioppr_search_history_by_module(GList *history_list, dt_iop_module_t *mod)
{
  dt_dev_history_item_t *hist_entry = NULL;

  for(const GList *history = history_list; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == mod)
    {
      hist_entry = hist;
      break;
    }
  }

  return hist_entry;
}

// check if there's duplicate iop_order entries in iop_list
// if so, updates the iop_order to be unique, but only if the module is disabled and not in history
/**
 * @brief Detect and resolve duplicate iop_order values.
 *
 * Walks the module list, reorders or removes disabled duplicates not present
 * in history to keep a consistent ordering.
 *
 * @param _iop_list Pointer to module list to mutate.
 * @param history_list History list used to preserve required instances.
 */
static void dt_ioppr_check_duplicate_iop_order(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  dt_iop_module_t *mod_prev = NULL;

  // get the first module
  GList *modules = iop_list;
  if(modules)
  {
    mod_prev = (dt_iop_module_t *)(modules->data);
    modules = g_list_next(modules);
  }
  // check for each module if iop_order is the same as the previous one
  // if so, change it, but only if disabled and not in history
  while(modules)
  {
    int reset_list = 0;
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    if(mod->iop_order == mod_prev->iop_order && mod->iop_order != INT_MAX)
    {
      int can_move = 0;

      if(!mod->enabled && _ioppr_search_history_by_module(history_list, mod) == NULL)
      {
        can_move = 1;

        GList *modules1 = g_list_next(modules);
        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);
          if(mod->iop_order != mod_next->iop_order)
          {
            mod->iop_order += (mod_next->iop_order - mod->iop_order) / 2.0;
          }
          else
          {
            dt_ioppr_check_duplicate_iop_order(&modules, history_list);
            reset_list = 1;
          }
        }
        else
        {
          mod->iop_order += 1.0;
        }
      }
      else if(!mod_prev->enabled && _ioppr_search_history_by_module(history_list, mod_prev) == NULL)
      {
        can_move = 1;

        GList *modules1 = g_list_previous(modules);
        if(modules1) modules1 = g_list_previous(modules1);
        if(modules1)
        {
          dt_iop_module_t *mod_next = (dt_iop_module_t *)(modules1->data);
          if(mod_prev->iop_order != mod_next->iop_order)
          {
            mod_prev->iop_order -= (mod_prev->iop_order - mod_next->iop_order) / 2.0;
          }
          else
          {
            can_move = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_duplicate_iop_order 1] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                    mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
          }
        }
        else
        {
          mod_prev->iop_order -= 0.5;
        }
      }

      if(!can_move)
      {
        fprintf(stderr,
                "[dt_ioppr_check_duplicate_iop_order] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
                mod_prev->op, mod_prev->multi_name, mod_prev->iop_order, mod->op, mod->multi_name, mod->iop_order);
      }
    }

    if(reset_list)
    {
      modules = iop_list;
      if(modules)
      {
        mod_prev = (dt_iop_module_t *)(modules->data);
        modules = g_list_next(modules);
      }
    }
    else
    {
      mod_prev = mod;
      modules = g_list_next(modules);
    }
  }

  *_iop_list = iop_list;
}

// check if all so modules on iop_list have a iop_order defined in iop_order_list
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list)
{
  int iop_order_missing = 0;

  // check if all the modules have their iop_order assigned
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_so_t *const restrict mod = (dt_iop_module_so_t *)(modules->data);
    const dt_iop_order_entry_t *const restrict entry =
      dt_ioppr_get_iop_order_entry(iop_order_list, mod->op, 0); // mod->multi_priority);
    if(IS_NULL_PTR(entry) && !dt_deprecated(mod->op))
    {
      iop_order_missing = 1;
      fprintf(stderr, "[dt_ioppr_check_so_iop_order] missing iop_order for module %s\n", mod->op);
    }
  }

  return iop_order_missing;
}

/**
 * @brief Deep-copy callback for @ref dt_iop_order_entry_t.
 *
 * @param src Source entry.
 * @param data Unused.
 * @return Newly-allocated copy.
 */
static void *_dup_iop_order_entry(const void *src, gpointer data)
{
  const dt_iop_order_entry_t *const restrict scr_entry = (dt_iop_order_entry_t *)src;
  dt_iop_order_entry_t *new_entry = malloc(sizeof(dt_iop_order_entry_t));
  memcpy(new_entry, scr_entry, sizeof(dt_iop_order_entry_t));
  return (void *)new_entry;
}

// returns a duplicate of iop_order_list
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list)
{
  return (GList *)g_list_copy_deep(iop_order_list, _dup_iop_order_entry, NULL);
}

// helper to sort a GList of dt_iop_module_t by iop_order
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *const restrict am = (const dt_iop_module_t *)a;
  const dt_iop_module_t *const restrict bm = (const dt_iop_module_t *)b;
  if(am->iop_order > bm->iop_order) return 1;
  if(am->iop_order < bm->iop_order) return -1;
  return 0;
}

// if module can be placed before than module_next on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_before_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  // we should't be here if the next module is using a raster mask and and our module is that raster mask source
  if(module_next->raster_mask.sink.source == module)
    return FALSE;

  gboolean can_move = FALSE;

  // module is before on the pipe
  // move it up
  if(module->iop_order < module_next->iop_order)
  {
    // let's first search for module
    GList *modules = iop_list;
    for(; modules; modules = g_list_next(modules))
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
    }

    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;

      // now search for module_next and the one previous to that, so iop_order can be calculated
      // also check the rules
      for(modules = g_list_next(modules); modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

        // if we reach module_next everything is OK
        if(mod == module_next)
        {
          mod2 = mod;
          break;
        }

        // moving a module that is the source for a raster mask ABOVE the module using it is forbidden
        if(mod->raster_mask.sink.source == module)
          break;

        // is there a rule about swapping this two?
        int rule_found = 0;
        for(const GList *rules = darktable.iop_order_rules; rules; rules = g_list_next(rules))
        {
          const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(module->op, rule->op_prev) == 0 && strcmp(mod->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }
        }
        if(rule_found) break;

        mod1 = mod;
      }

      // we reach the module_next module
      if(mod2)
      {
        // this is already the previous module!
        if(module == mod1)
        {
          ;
        }
        else if(mod1->iop_order == mod2->iop_order)
        {
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        }
        else
        {
          can_move = TRUE;
        }
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  // module is next on the pipe
  // move it down
  else if(module->iop_order > module_next->iop_order)
  {
    // let's first search for module
    GList *modules = g_list_last(iop_list);
    for(; modules; modules = g_list_previous(modules))
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod == module) break;
    }

    // we found the module
    if(modules)
    {
      dt_iop_module_t *mod1 = NULL;
      dt_iop_module_t *mod2 = NULL;

      // now search for module_next and the one next to that, so iop_order can be calculated
      // also check the rules
      for(modules = g_list_previous(modules); modules; modules = g_list_previous(modules))
      {
        dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

        // we reach the module next to module_next, everything is OK
        if(!IS_NULL_PTR(mod2))
        {
          mod1 = mod;
          break;
        }

        // moving a module using a raster mask BELOW its raster source module is forbidden
        if(module->raster_mask.sink.source == mod)
          break;

        // is there a rule about swapping this two?
        int rule_found = 0;
        for(const GList *rules = darktable.iop_order_rules; rules; rules = g_list_next(rules))
        {
          const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

          if(strcmp(mod->op, rule->op_prev) == 0 && strcmp(module->op, rule->op_next) == 0)
          {
            rule_found = 1;
            break;
          }
        }
        if(rule_found) break;

        if(mod == module_next) mod2 = mod;
      }

      // we reach the module_next module
      if(mod1)
      {
        // this is already the previous module!
        if(module == mod2)
        {
          ;
        }
        else if(mod1->iop_order == mod2->iop_order)
        {
          fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] %s %s(%d) and %s %s(%d) have the same iop_order\n",
              mod1->op, mod1->multi_name, mod1->iop_order, mod2->op, mod2->multi_name, mod2->iop_order);
        }
        else
        {
          can_move = TRUE;
        }
      }
    }
    else
      fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] can't find module %s %s\n", module->op, module->multi_name);
  }
  else
  {
    fprintf(stderr, "[dt_ioppr_get_iop_order_before_iop] modules %s %s(%d) and %s %s(%d) have the same iop_order\n",
        module->op, module->multi_name, module->iop_order, module_next->op, module_next->multi_name, module_next->iop_order);
  }

  return can_move;
}

// if module can be placed after than module_prev on the pipe
// it returns the new iop_order
// if it cannot be placed it returns -1.0
// this assumes that the order is always positive
gboolean dt_ioppr_check_can_move_after_iop(GList *iop_list, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  // we shouldn't be here if the previous module is the a raster mask's source and our module is using it
  if(module->raster_mask.sink.source == module_prev)
    return FALSE;

  gboolean can_move = FALSE;

  // moving after module_prev is the same as moving before the very next one after module_prev
  dt_iop_module_t *module_next = NULL;

  for(const GList *modules = g_list_last(iop_list); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module_prev) break;

    module_next = mod;
  }
  if(IS_NULL_PTR(module_next))
  {
    fprintf(
        stderr,
        "[dt_ioppr_get_iop_order_after_iop] can't find module previous to %s %s(%d) while moving %s %s(%d) after it\n",
        module_prev->op, module_prev->multi_name, module_prev->iop_order, module->op, module->multi_name,
        module->iop_order);
  }
  else
    can_move = dt_ioppr_check_can_move_before_iop(iop_list, module, module_next);

  return can_move;
}

// changes the module->iop_order so it comes before in the pipe than module_next
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
gboolean dt_ioppr_move_iop_before(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_next)
{
  GList *next = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_next->op, module_next->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(IS_NULL_PTR(next)) return FALSE;

  if(IS_NULL_PTR(current))
  {
    // Module not in iop_order_list, create entry
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    g_strlcpy(entry->operation, module->op, sizeof(entry->operation));
    entry->instance = module->multi_priority;
    entry->o.iop_order = 0; // will be reset later
    current = g_list_alloc();
    current->data = entry;
  }

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);
  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);

  g_list_free(current);
  current = NULL;

  dt_ioppr_resync_modules_order(dev);

  return TRUE;
}

// changes the module->iop_order so it comes after in the pipe than module_prev
// sort dev->iop to reflect the changes
// return 1 if iop_order is changed, 0 otherwise
gboolean dt_ioppr_move_iop_after(struct dt_develop_t *dev, dt_iop_module_t *module, dt_iop_module_t *module_prev)
{
  GList *prev = dt_ioppr_get_iop_order_link(dev->iop_order_list, module_prev->op, module_prev->multi_priority);
  GList *current = dt_ioppr_get_iop_order_link(dev->iop_order_list, module->op, module->multi_priority);

  if(IS_NULL_PTR(prev)) return FALSE;

  if(IS_NULL_PTR(current))
  {
    // Module not in iop_order_list, create entry
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    g_strlcpy(entry->operation, module->op, sizeof(entry->operation));
    entry->instance = module->multi_priority;
    entry->o.iop_order = 0; // will be reset later
    current = g_list_alloc();
    current->data = entry;
  }

  dev->iop_order_list = g_list_remove_link(dev->iop_order_list, current);

  // we want insert after => so insert before the next item
  GList *next = g_list_next(prev);
  if(prev)
    dev->iop_order_list = g_list_insert_before(dev->iop_order_list, next, current->data);
  else
    dev->iop_order_list = g_list_append(dev->iop_order_list, current->data);

  g_list_free(current);
  current = NULL;

  dt_ioppr_resync_modules_order(dev);

  return TRUE;
}

/**
 * @brief Validate pipeline order against fence and rule constraints.
 *
 * Emits debug messages when violations are detected.
 *
 * @param iop_list Module list.
 * @param imgid Image id (for diagnostics).
 * @param msg Optional debug label.
 */
static void _ioppr_check_rules(GList *iop_list, const int32_t imgid, const char *msg)
{
  // for each module check if it doesn't break a rule
  for(const GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
    if(mod->iop_order == INT_MAX)
    {
      continue;
    }

    // we have a module, now check each rule
    for(const GList *rules = darktable.iop_order_rules; rules; rules = g_list_next(rules))
    {
      const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

      // mod must be before rule->op_next
      if(strcmp(mod->op, rule->op_prev) == 0)
      {
        // check if there's a rule->op_next module before mod
        for(const GList *modules_prev = g_list_previous(modules);
            modules_prev;
            modules_prev = g_list_previous(modules_prev))
        {
          const dt_iop_module_t *const restrict mod_prev = (dt_iop_module_t *)modules_prev->data;

          if(strcmp(mod_prev->op, rule->op_next) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is after %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_prev->op,
                    mod_prev->multi_name, mod_prev->iop_order, imgid, msg);
          }
        }
      }
      // mod must be after rule->op_prev
      else if(strcmp(mod->op, rule->op_next) == 0)
      {
        // check if there's a rule->op_prev module after mod
        for(const GList *modules_next = g_list_next(modules); modules_next;  modules_next = g_list_next(modules_next))
        {
          const dt_iop_module_t *const restrict mod_next = (dt_iop_module_t *)modules_next->data;

          if(strcmp(mod_next->op, rule->op_prev) == 0)
          {
            fprintf(stderr, "[_ioppr_check_rules] found rule %s %s module %s %s(%d) is before %s %s(%d) image %i (%s)\n",
                    rule->op_prev, rule->op_next, mod->op, mod->multi_name, mod->iop_order, mod_next->op,
                    mod_next->multi_name, mod_next->iop_order, imgid, msg);
          }
        }
      }
    }
  }
}

void dt_ioppr_insert_module_instance(struct dt_develop_t *dev, dt_iop_module_t *module)
{
  const char *operation = module->op;
  const int32_t instance = module->multi_priority;

  dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

  g_strlcpy(entry->operation, operation, sizeof(entry->operation));
  entry->instance = instance;
  entry->o.iop_order = 0;

  GList *place = NULL;

  int max_instance = -1;

  for(GList *l = dev->iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict e = (dt_iop_order_entry_t *)l->data;
    if(!strcmp(e->operation, operation) && e->instance > max_instance)
    {
      place = l;
      max_instance = e->instance;
    }
  }

  dev->iop_order_list = g_list_insert_before(dev->iop_order_list, place, entry);
}

int dt_ioppr_check_iop_order(dt_develop_t *dev, const int32_t imgid, const char *msg)
{
  int iop_order_ok = 1;

  // check if gamma is the last iop
  {
    GList *modules;
    for(modules = g_list_last(dev->iop); modules; modules = g_list_previous(dev->iop))
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
        break;
    }
    if(modules)
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;

      if(strcmp(mod->op, "gamma") != 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] gamma is not the last iop, last is %s %s(%d) image %i (%s)\n",
                mod->op, mod->multi_name, mod->iop_order,imgid, msg);
      }
    }
    else
    {
      // fprintf(stderr, "[dt_ioppr_check_iop_order] dev->iop is empty image %i (%s)\n",imgid, msg);
    }
  }

  // some other checks
  {
    for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(dev->iop))
    {
      const dt_iop_module_t *const restrict mod = (dt_iop_module_t *)modules->data;
      if(!mod->default_enabled && mod->iop_order != INT_MAX)
      {
        if(mod->enabled)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] module not used but enabled!! %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
        if(mod->multi_priority == 0)
        {
          iop_order_ok = 0;
          fprintf(stderr, "[dt_ioppr_check_iop_order] base module set as not used %s %s(%d) image %i (%s)\n",
                  mod->op, mod->multi_name, mod->iop_order,imgid, msg);
        }
      }
    }
  }

  // check if there's duplicate or out-of-order iop_order
  {
    dt_iop_module_t *mod_prev = NULL;
    for(const GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->iop_order != INT_MAX)
      {
        if(mod_prev)
        {
          if(mod->iop_order < mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(stderr,
                    "[dt_ioppr_check_iop_order] module %s %s(%d) should be after %s %s(%d) image %i (%s)\n",
                    mod->op, mod->multi_name, mod->iop_order, mod_prev->op, mod_prev->multi_name,
                    mod_prev->iop_order, imgid, msg);
          }
          else if(mod->iop_order == mod_prev->iop_order)
          {
            iop_order_ok = 0;
            fprintf(
                stderr,
                "[dt_ioppr_check_iop_order] module %s %s(%i)(%d) and %s %s(%i)(%d) have the same order image %i (%s)\n",
                mod->op, mod->multi_name, mod->multi_priority, mod->iop_order, mod_prev->op,
                mod_prev->multi_name, mod_prev->multi_priority, mod_prev->iop_order, imgid, msg);
          }
        }
      }
      mod_prev = mod;
    }
  }

  _ioppr_check_rules(dev->iop, imgid, msg);

  for(const GList *history = dev->history; history; history = g_list_next(history))
  {
    const dt_dev_history_item_t *const restrict hist = (dt_dev_history_item_t *)(history->data);

    if(hist->iop_order == INT_MAX)
    {
      if(hist->enabled)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history module not used but enabled!! %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order, imgid, msg);
      }
      if(hist->multi_priority == 0)
      {
        iop_order_ok = 0;
        fprintf(stderr, "[dt_ioppr_check_iop_order] history base module set as not used %s %s(%d) image %i (%s)\n",
            hist->op_name, hist->multi_name, hist->iop_order, imgid, msg);
      }
    }
  }

  dt_print(DT_DEBUG_PARAMS, "[dt_ioppr_check_iop_order] IOP order passed (called from %s)\n", msg);

  return iop_order_ok;
}

void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size)
{
  g_return_val_if_fail(!IS_NULL_PTR(iop_order_list), NULL);
  g_return_val_if_fail(!IS_NULL_PTR(size), NULL);
  // compute size of all modules
  *size = 0;

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    *size += strlen(entry->operation) + sizeof(int32_t) * 2;
  }

  if(*size == 0)
    return NULL;

  // allocate the parameter buffer
  char *params = (char *)malloc(*size);

  // set set preset iop-order version
  int pos = 0;

  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    // write the len of the module name
    const int32_t len = strlen(entry->operation);
    memcpy(params+pos, &len, sizeof(int32_t));
    pos += sizeof(int32_t);

    // write the module name
    memcpy(params+pos, entry->operation, len);
    pos += len;

    // write the instance number
    memcpy(params+pos, &(entry->instance), sizeof(int32_t));
    pos += sizeof(int32_t);
  }

  return params;
}

char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list)
{
  gchar *text = g_strdup("");

  const GList *const last = g_list_last(iop_order_list);
  for(const GList *l = iop_order_list; l; l = g_list_next(l))
  {
    const dt_iop_order_entry_t *const restrict entry = (dt_iop_order_entry_t *)l->data;
    gchar buf[64];
    snprintf(buf, sizeof(buf), "%s,%d%s", entry->operation, entry->instance, (l == last) ? "" : ",");
    text = g_strconcat(text, buf, NULL);
  }

  return text;
}

/* this sanity check routine is used to correct wrong iop-list that
 * could have been stored while some bugs were present in
 * dartkable. There was a window around Sep 2019 where such issue
 * existed and some xmp may have been corrupt at this time making dt
 * crash while reimporting using the xmp.
 *
 * One common case seems that the list does not end with gamma.
*/

/**
 * @brief Basic sanity check for an order list.
 *
 * Ensures all entries are valid and ordering values are non-zero.
 *
 * @param list Order list.
 * @return TRUE if list looks sane, FALSE otherwise.
 */
static gboolean _ioppr_sanity_check_iop_order(GList *list)
{
  gboolean ok = TRUE;

  // First check that first module is basebuffer (even for a jpeg, we
  // are speaking of the module ordering not the activated modules.

  GList *first = g_list_first(list);
  dt_iop_order_entry_t *entry_first = (dt_iop_order_entry_t *)first->data;

  ok = ok && (g_strcmp0(entry_first->operation, "basebuffer") == 0);

  // Then check that last module is gamma

  GList *last = g_list_last(list);
  dt_iop_order_entry_t *entry_last = (dt_iop_order_entry_t *)last->data;

  ok = ok && (g_strcmp0(entry_last->operation, "gamma") == 0);

  return ok;
}

GList *dt_ioppr_deserialize_text_iop_order_list(const char *buf)
{
  GList *iop_order_list = NULL;

  GList *list = dt_util_str_to_glist(",", buf);
  for(GList *l = list; l; l = g_list_next(l))
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
    entry->o.iop_order = 0;

    // first operation name
    g_strlcpy(entry->operation, (char *)l->data, sizeof(entry->operation));

    // then operation instance
    l = g_list_next(l);
    if(IS_NULL_PTR(l)) goto error;

    const char *data = (char *)l->data;
    int inst = 0;
    sscanf(data, "%d", &inst);
    entry->instance = inst;

    // append to the list
    iop_order_list = g_list_prepend(iop_order_list, entry);
  }
  iop_order_list = g_list_reverse(iop_order_list);  // list was built in reverse order, so un-reverse it

  g_list_free_full(list, dt_free_gpointer);
  list = NULL;

  _ioppr_reset_iop_order(iop_order_list);

  // Remove sanity check from here; it's now handled after migration in dt_ioppr_get_iop_order_list()

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, dt_free_gpointer);
  iop_order_list = NULL;
  return NULL;
}

GList *dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size)
{
  GList *iop_order_list = NULL;

  // parse all modules
  while(size)
  {
    dt_iop_order_entry_t *entry = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));

    entry->o.iop_order = 0;

    // get length of module name
    const int32_t len = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(len < 0 || len > 20) { dt_free(entry); goto error; }

    // set module name
    memcpy(entry->operation, buf, len);
    *(entry->operation + len) = '\0';
    buf += len;

    // get the instance number
    entry->instance = *(int32_t *)buf;
    buf += sizeof(int32_t);

    if(entry->instance < 0 || entry->instance > 1000) { dt_free(entry); goto error; }

    // append to the list
    iop_order_list = g_list_prepend(iop_order_list, entry);

    size -= (2 * sizeof(int32_t) + len);
  }
  iop_order_list = g_list_reverse(iop_order_list);  // list was built in reverse order, so un-reverse it

  _ioppr_reset_iop_order(iop_order_list);

  return iop_order_list;

 error:
  g_list_free_full(iop_order_list, dt_free_gpointer);
  iop_order_list = NULL;
  return NULL;
}

#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
