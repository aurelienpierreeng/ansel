/*
    This file is part of darktable,
    Copyright (C) 2018, 2020 rawfiner.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Chris Elston.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Jim Robinson.
    Copyright (C) 2022-2023 Aurélien PIERRE.
    Copyright (C) 2022 Diederik Ter Rahe.
    Copyright (C) 2022 Martin Bařinka.
    
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

#include "common/usermanual_url.h"
#include "common/darktable.h"
#include "common/l10n.h"


// The base_url is: ansel.photos/
// The full format for the documentation pages is:
//    <base-url>/<lang>/doc[/path/to/page]
// Where:
//   <lang> = en / fr ...              (default = en)

const char *base_url = "https://ansel.photos/";
const char *doc_url = "doc/";


const char *get_lang()
{
  char *lang = "en";

  // array of languages the usermanual supports.
  // NULL MUST remain the last element of the array
  const char *supported_languages[] =
    { "en", "fr", NULL };

  int lang_index = 0;
  gboolean is_language_supported = FALSE;

  if(darktable.l10n != NULL)
  {
    dt_l10n_language_t *language = NULL;

    if(darktable.l10n->selected != -1)
        language = (dt_l10n_language_t *)g_list_nth(darktable.l10n->languages, darktable.l10n->selected)->data;

    if (language != NULL)
      lang = language->code;

    while(supported_languages[lang_index])
    {
      gchar *nlang = g_strdup(lang);
      is_language_supported = !g_ascii_strcasecmp(nlang, supported_languages[lang_index]);

      if(!is_language_supported)
      {
        // keep only first part up to _
        for(gchar *p = nlang; *p; p++)
          if(*p == '_') *p = '\0';

        if(!g_ascii_strcasecmp(nlang, supported_languages[lang_index]))
        {
          is_language_supported = TRUE;
        }
      }

      g_free(nlang);
      if(is_language_supported) break;

      lang_index++;
    }
  }

  // language not found, default to EN
  if(!is_language_supported) lang_index = 0;

  return supported_languages[lang_index];
}

typedef struct _help_url
{
  char *name;
  char *url;
} dt_help_url;

dt_help_url urls_db[] =
{
  {"ratings",                    "views/lighttable/digital-asset-management/star-color/#star-ratings"},
  {"layout_filemanager",         "views/lighttable/lighttable-modes/filemanager/"},
  {"layout_zoomable",            "views/lighttable/lighttable-modes/zoomable-lighttable/"},
  {"layout_preview",             "views/lighttable/lighttable-modes/full-preview/"},
  {"filter",                     NULL},
  {"colorlabels",                "views/lighttable/digital-asset-management/star-color/#color-labels"},
  {"import",                     "modules/utility-modules/lighttable/import/"},
  {"select",                     "modules/utility-modules/lighttable/select/"},
  {"image",                      "modules/utility-modules/lighttable/selected-image/"},
  {"copy_history",               "modules/utility-modules/lighttable/history-stack/"},
  {"styles",                     "modules/utility-modules/lighttable/styles/#module-controls"},
  {"metadata",                   "modules/utility-modules/shared/metadata-editor/"},
  {"tagging",                    "modules/utility-modules/shared/tagging/"},
  {"geotagging",                 "modules/utility-modules/shared/geotagging/"},
  {"collect",                    "modules/utility-modules/shared/collections/"},
  {"recentcollect",              "modules/utility-modules/shared/recent-collections/"},
  {"metadata_view",              "modules/utility-modules/shared/image-information/"},
  {"export",                     "modules/utility-modules/shared/export/"},
  {"histogram",                  "modules/utility-modules/shared/histogram/"},
  {"navigation",                 "modules/utility-modules/darkroom/navigation/"},
  {"snapshots",                  "modules/utility-modules/darkroom/snapshots/"},
  {"modulegroups",               "modules/utility-modules/darkroom/manage-module-layouts/"},
  {"history",                    "modules/utility-modules/darkroom/history-stack/"},
  {"colorpicker",                "modules/utility-modules/darkroom/global-color-picker/"},
  {"masks",                      "modules/utility-modules/darkroom/mask-manager/"},
  {"masks_drawn",                "views/darkroom/masking-and-blending/masks/drawn/"},
  {"masks_parametric",           "views/darkroom/masking-and-blending/masks/parametric/"},
  {"masks_raster",               "views/darkroom/masking-and-blending/masks/raster/"},
  {"masks_blending_op",          "views/darkroom/masking-and-blending/masks/drawn-and-parametric/"},
  {"masks_blending",             "views/darkroom/masking-and-blending/overview/"},
  {"masks_combined",             "views/darkroom/masking-and-blending/masks/drawn-and-parametric/"},
  {"masks_refinement",           "views/darkroom/masking-and-blending/masks/refinement-controls/"},
  {"duplicate",                  "modules/utility-modules/darkroom/duplicate-manager/"},
  {"location",                   "modules/utility-modules/map/find-location/"},
  {"map_settings",               "modules/utility-modules/map/map-settings/"},
  {"print_settings",             "modules/utility-modules/print/print-settings/"},
  {"print_settings_printer"      "modules/utility-modules/print/print-settings/#printer"},
  {"print_settings_page"         "modules/utility-modules/print/print-settings/#page"},
  {"print_settings_button"       "modules/utility-modules/print/print-settings/#print-button"},
  {"print_overview",             "print/overview/"},
  {"camera",                     "modules/utility-modules/tethering/camera-settings/"},
  {"import_camera",              "overview/workflow/import-rate-tag/"},
  {"import_fr",                  "overview/workflow/import-rate-tag/"},
  {"global_toolbox",             "overview/user-interface/top-panel/#on-the-right-hand-side"},
  {"views/lighttable_mode",            "views/lighttable/overview/"},
  {"views/lighttable_filemanager",     "views/lighttable/lighttable-modes/filemanager/"},
  {"views/lighttable_zoomable",        "views/lighttable/lighttable-modes/zoomable-lighttable/"},
  {"views/darkroom_bottom_panel",      "views/darkroom/darkroom-view-layout/#bottom-panel"},
  {"module_header",              "views/darkroom/processing-modules/module-header/"},
  {"session",                    "modules/utility-modules/tethering/session/"},
  {"live_view",                  "modules/utility-modules/tethering/live-view/"},
  {"module_toolbox",             NULL},
  {"view_toolbox",               NULL},
  {"backgroundjobs",             NULL},
  {"hinter",                     NULL},
  {"filter",                     NULL},
  {"filmstrip",                  "overview/user-interface/filmstrip/"},
  {"viewswitcher",               "overview/user-interface/views/"},
  {"favorite_presets",           "views/darkroom/darkroom-view-layout/#bottom-panel"},
  {"bottom_panel_styles",        "views/darkroom/darkroom-view-layout/#bottom-panel"},
  {"rawoverexposed",             "modules/utility-modules/darkroom/raw-overexposed/"},
  {"overexposed",                "modules/utility-modules/darkroom/clipping/"},
  {"softproof",                  "modules/utility-modules/darkroom/soft-proof/"},
  {"gamut",                      "modules/utility-modules/darkroom/gamut/"},
  {"focuspeaking",               "modules/utility-modules/shared/focus-peaking/"},

  // iop links
  {"ashift",                     "modules/processing-modules/rotate-perspective/"},
  {"atrous",                     "modules/processing-modules/contrast-equalizer/"},
  {"basecurve",                  "modules/processing-modules/base-curve/"},
  {"bilateral",                  "modules/processing-modules/surface-blur/"},
  {"bilat",                      "modules/processing-modules/local-contrast/"},
  {"bloom",                      "modules/processing-modules/bloom/"},
  {"borders",                    "modules/processing-modules/framing/"},
  {"cacorrect",                  "modules/processing-modules/raw-chromatic-aberrations/"},
  {"cacorrectrgb",               "modules/processing-modules/chromatic-aberrations/"},
  {"censorize",                  "modules/processing-modules/censorize/"},
  {"channelmixer",               "modules/processing-modules/channel-mixer/"},
  {"channelmixerrgb",            "modules/processing-modules/color-calibration/"},
  {"clahe",                      NULL}, // deprecated, replaced by bilat.
  {"clipping",                   "modules/processing-modules/crop-rotate/"},
  {"colisa",                     "modules/processing-modules/contrast-brightness-saturation/"},
  {"colorbalance",               "modules/processing-modules/color-balance/"},
  {"colorbalancergb",            "modules/processing-modules/color-balance-rgb/"},
  {"colorchecker",               "modules/processing-modules/color-look-up-table/"},
  {"colorcontrast",              "modules/processing-modules/color-contrast/"},
  {"colorcorrection",            "modules/processing-modules/color-correction/"},
  {"colorin",                    "modules/processing-modules/input-color-profile/"},
  {"colorize",                   "modules/processing-modules/colorize/"},
  {"colormapping",               "modules/processing-modules/color-mapping/"},
  {"colorout",                   "modules/processing-modules/output-color-profile/"},
  {"colorreconstruct",           "modules/processing-modules/color-reconstruction/"},
  {"colortransfer",              NULL}, // deprecate
  {"colorzones",                 "modules/processing-modules/color-zones/"},
  {"crop",                       "modules/processing-modules/crop/"},
  {"defringe",                   "modules/processing-modules/defringe/"},
  {"demosaic",                   "modules/processing-modules/demosaic/"},
  {"denoiseprofile",             "modules/processing-modules/denoise-profiled/"},
  {"dither",                     "modules/processing-modules/dithering/"},
  {"equalizer",                  NULL}, // deprecated, replaced by atrous
  {"exposure",                   "modules/processing-modules/exposure/"},
  {"filmic",                     "modules/processing-modules/filmic-rgb/"},
  {"filmicrgb",                  "modules/processing-modules/filmic-rgb/"},
  {"flip",                       "modules/processing-modules/orientation/"},
  {"globaltonemap",              "modules/processing-modules/global-tonemap/"},
  {"graduatednd",                "modules/processing-modules/graduated-density/"},
  {"grain",                      "modules/processing-modules/grain/"},
  {"hazeremoval",                "modules/processing-modules/haze-removal/"},
  {"highlights",                 "modules/processing-modules/highlight-reconstruction/"},
  {"highpass",                   "modules/processing-modules/highpass/"},
  {"hotpixels",                  "modules/processing-modules/hot-pixels/"},
  {"invert",                     "modules/processing-modules/invert/"},
  {"lens",                       "modules/processing-modules/lens-correction/"},
  {"levels",                     "modules/processing-modules/levels/"},
  {"liquify",                    "modules/processing-modules/liquify/"},
  {"lowlight",                   "modules/processing-modules/lowlight-vision/"},
  {"lowpass",                    "modules/processing-modules/lowpass/"},
  {"lut3d",                      "modules/processing-modules/lut-3d/"},
  {"monochrome",                 "modules/processing-modules/monochrome/"},
  {"negadoctor",                 "modules/processing-modules/negadoctor/"},
  {"nlmeans",                    "modules/processing-modules/astrophoto-denoise/"},
  {"profile_gamma",              "modules/processing-modules/unbreak-input-profile/"},
  {"rawdenoise",                 "modules/processing-modules/raw-denoise/"},
  {"rawprepare",                 "modules/processing-modules/raw-black-white-point/"},
  {"relight",                    "modules/processing-modules/fill-light/"},
  {"retouch",                    "modules/processing-modules/retouch/"},
  {"rgbcurve",                   "modules/processing-modules/rgb-curve/"},
  {"rgblevels",                  "modules/processing-modules/rgb-levels/"},
  {"rotatepixels",               "modules/processing-modules/rotate-pixels/"},
  {"scalepixels",                "modules/processing-modules/scale-pixels/"},
  {"shadhi",                     "modules/processing-modules/shadows-and-highlights/"},
  {"sharpen",                    "modules/processing-modules/sharpen/"},
  {"soften",                     "modules/processing-modules/soften/"},
  {"splittoning",                "modules/processing-modules/split-toning/"},
  {"spots",                      "modules/processing-modules/spot-removal/"},
  {"temperature",                "modules/processing-modules/white-balance/"},
  {"tonecurve",                  "modules/processing-modules/tone-curve/"},
  {"toneequal",                  "modules/processing-modules/tone-equalizer/"},
  {"tonemap",                    "modules/processing-modules/tone-mapping/"},
  {"velvia",                     "modules/processing-modules/velvia/"},
  {"vibrance",                   "modules/processing-modules/vibrance/"},
  {"vignette",                   "modules/processing-modules/vignetting/"},
  {"watermark",                  "modules/processing-modules/watermark/"},
  {"zonesystem",                 "modules/processing-modules/zone-system/"},
};

char *dt_get_help_url(char *name)
{
  if(name == NULL) return NULL;

  for(int k=0; k< sizeof(urls_db)/2/sizeof(char *); k++)
    if(!strcmp(urls_db[k].name, name))
        return g_build_path("/", base_url, get_lang(), doc_url, urls_db[k].url, NULL);

  return NULL;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
