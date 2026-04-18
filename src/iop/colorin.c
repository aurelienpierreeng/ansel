/*
    This file is part of darktable,
    Copyright (C) 2009-2014, 2016 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 Milan Knížek.
    Copyright (C) 2010, 2012-2014 Pascal de Bruijn.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2017, 2019 Tobias Ellinghaus.
    Copyright (C) 2011-2012, 2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2012, 2020 Aldric Renaudin.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2018-2022 Pascal Obry.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014 Edouard Gomez.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2014-2017 Roman Lebedev.
    Copyright (C) 2017, 2019 Heiko Bauke.
    Copyright (C) 2018, 2020, 2023-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Kelvie Wong.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019, 2021 Andreas Schneider.
    Copyright (C) 2019-2020, 2022 Hanno Schwalm.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 Miroslav Silovic.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2021 Daniel Vogelbacher.
    Copyright (C) 2021 Miloš Komarčević.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023 Ricky Moon.
    
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
#include "common/darktable.h"
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/colormatrices.c"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"

#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "common/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "common/imageio_heif.h"
#endif
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lcms2.h>

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512

#define LUT_SAMPLES 0x10000

DT_MODULE_INTROSPECTION(7, dt_iop_colorin_params_t)

static void update_profile_list(dt_iop_module_t *self);

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,               //$DESCRIPTION: "off"
  DT_NORMALIZE_SRGB,              //$DESCRIPTION: "sRGB"
  DT_NORMALIZE_ADOBE_RGB,         //$DESCRIPTION: "Adobe RGB (compatible)"
  DT_NORMALIZE_LINEAR_REC709_RGB, //$DESCRIPTION: "linear Rec709 RGB"
  DT_NORMALIZE_LINEAR_REC2020_RGB //$DESCRIPTION: "linear Rec2020 RGB"
} dt_iop_color_normalize_t;

typedef struct dt_iop_colorin_params_t
{
  dt_colorspaces_color_profile_type_t type; // $DEFAULT: DT_COLORSPACE_ENHANCED_MATRIX
  char filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;       // $DEFAULT: DT_INTENT_PERCEPTUAL
  dt_iop_color_normalize_t normalize; // $DEFAULT: DT_NORMALIZE_OFF $DESCRIPTION: "gamut clipping"
  int blue_mapping;
  // working color profile
  dt_colorspaces_color_profile_type_t type_work; // $DEFAULT: DT_COLORSPACE_LIN_REC2020
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *profile_combobox, *clipping_combobox, *work_combobox;
  GList *image_profiles;
  int n_image_profiles;
} dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin_unbound;
  int kernel_colorin_clipping;
} dt_iop_colorin_global_data_t;

typedef struct dt_iop_colorin_data_t
{
  int clear_input;
  cmsHPROFILE input;
  cmsHPROFILE nrgb;
  cmsHTRANSFORM *xform_cam_Lab;
  cmsHTRANSFORM *xform_cam_nrgb;
  cmsHTRANSFORM *xform_nrgb_Lab;
  float lut[3][LUT_SAMPLES];
  dt_colormatrix_t cmatrix;
  dt_colormatrix_t nmatrix;
  dt_colormatrix_t lmatrix;
  float unbounded_coeffs[3][3]; // approximation for extrapolation of shaper curves
  int blue_mapping;
  int nonlinearlut;
  dt_colorspaces_color_profile_type_t type;
  dt_colorspaces_color_profile_type_t type_work;
  char filename[DT_IOP_COLOR_ICC_LEN];
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_colorin_data_t;


const char *name()
{
  return _("input color profile");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("convert any RGB input to pipeline reference RGB\n"
                                        "using color profiles to remap RGB values"),
                                      _("mandatory"),
                                      _("linear or non-linear, RGB, scene-referred"),
                                      _("defined by profile"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static dt_iop_colorspace_type_t _colorin_format_cst(dt_iop_module_t *self)
{
  const dt_iop_colorin_params_t *const p = (dt_iop_colorin_params_t *)self->params;
  return (p->type == DT_COLORSPACE_LAB) ? IOP_CS_LAB : IOP_CS_RGB;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  /* The sealed pipeline asks for the buffer contract before commit_params() has refreshed
   * `piece->data`. Read the module params snapshot instead of the previous image runtime data,
   * otherwise colorin can publish a stale Lab/RGB contract. */
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorin_format_cst(self);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = _colorin_format_cst(self);
}

static void _resolve_work_profile(dt_colorspaces_color_profile_type_t *work_type, char *work_filename)
{
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->work_pos > -1 && *work_type == prof->type
       && (prof->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(prof->filename, work_filename)))
      return;
  }

  dt_print(DT_DEBUG_COLORPROFILE,
           "[colorin] profile `%s' not suitable for work profile. it has been replaced by linear Rec2020 RGB!\n",
           dt_colorspaces_get_name(*work_type, work_filename));
  *work_type = DT_COLORSPACE_LIN_REC2020;
  work_filename[0] = '\0';
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
#define DT_IOP_COLOR_ICC_LEN_V5 100

  if(old_version == 1 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v1_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
    } dt_iop_colorin_params_v1_t;

    const dt_iop_colorin_params_v1_t *old = (dt_iop_colorin_params_v1_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = 0;
    new->blue_mapping = 1;
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';
    return 0;
  }
  if(old_version == 2 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v2_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
    } dt_iop_colorin_params_v2_t;

    const dt_iop_colorin_params_v2_t *old = (dt_iop_colorin_params_v2_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = 1;
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';
    return 0;
  }
  if(old_version == 3 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v3_t
    {
      char iccprofile[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
    } dt_iop_colorin_params_v3_t;

    const dt_iop_colorin_params_v3_t *old = (dt_iop_colorin_params_v3_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    if(!strcmp(old->iccprofile, "eprofile"))
      new->type = DT_COLORSPACE_EMBEDDED_ICC;
    else if(!strcmp(old->iccprofile, "ematrix"))
      new->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    else if(!strcmp(old->iccprofile, "cmatrix"))
      new->type = DT_COLORSPACE_STANDARD_MATRIX;
    else if(!strcmp(old->iccprofile, "darktable"))
      new->type = DT_COLORSPACE_ENHANCED_MATRIX;
    else if(!strcmp(old->iccprofile, "vendor"))
      new->type = DT_COLORSPACE_VENDOR_MATRIX;
    else if(!strcmp(old->iccprofile, "alternate"))
      new->type = DT_COLORSPACE_ALTERNATE_MATRIX;
    else if(!strcmp(old->iccprofile, "sRGB"))
      new->type = DT_COLORSPACE_SRGB;
    else if(!strcmp(old->iccprofile, "adobergb"))
      new->type = DT_COLORSPACE_ADOBERGB;
    else if(!strcmp(old->iccprofile, "linear_rec709_rgb") || !strcmp(old->iccprofile, "linear_rgb"))
      new->type = DT_COLORSPACE_LIN_REC709;
    else if(!strcmp(old->iccprofile, "linear_rec2020_rgb"))
      new->type = DT_COLORSPACE_LIN_REC2020;
    else if(!strcmp(old->iccprofile, "infrared"))
      new->type = DT_COLORSPACE_INFRARED;
    else if(!strcmp(old->iccprofile, "XYZ"))
      new->type = DT_COLORSPACE_XYZ;
    else if(!strcmp(old->iccprofile, "Lab"))
      new->type = DT_COLORSPACE_LAB;
    else
    {
      new->type = DT_COLORSPACE_FILE;
      g_strlcpy(new->filename, old->iccprofile, sizeof(new->filename));
    }

    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';

    return 0;
  }
  if(old_version == 4 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v4_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
    } dt_iop_colorin_params_v4_t;

    const dt_iop_colorin_params_v4_t *old = (dt_iop_colorin_params_v4_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    new->type = old->type;
    g_strlcpy(new->filename, old->filename, sizeof(new->filename));
    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;
    new->type_work = DT_COLORSPACE_LIN_REC709;
    new->filename_work[0] = '\0';

    return 0;
  }
  if(old_version == 5 && new_version == 7)
  {
    typedef struct dt_iop_colorin_params_v5_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN_V5];
      dt_iop_color_intent_t intent;
      int normalize;
      int blue_mapping;
      // working color profile
      dt_colorspaces_color_profile_type_t type_work;
      char filename_work[DT_IOP_COLOR_ICC_LEN_V5];
    } dt_iop_colorin_params_v5_t;

    const dt_iop_colorin_params_v5_t *old = (dt_iop_colorin_params_v5_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memset(new, 0, sizeof(*new));

    new->type = old->type;
    g_strlcpy(new->filename, old->filename, sizeof(new->filename));
    new->intent = old->intent;
    new->normalize = old->normalize;
    new->blue_mapping = old->blue_mapping;
    new->type_work = old->type_work;
    g_strlcpy(new->filename_work, old->filename_work, sizeof(new->filename_work));
    _resolve_work_profile(&new->type_work, new->filename_work);

    return 0;
  }
  if(old_version == 6 && new_version == 7)
  {
    // The structure is equal to to v7 (current) but a new version is introduced to convert invalid
    // working profile choice to the default, linear Rec2020.
    typedef struct dt_iop_colorin_params_v6_t
    {
      dt_colorspaces_color_profile_type_t type;
      char filename[DT_IOP_COLOR_ICC_LEN];
      dt_iop_color_intent_t intent;
      dt_iop_color_normalize_t normalize;
      int blue_mapping;
      // working color profile
      dt_colorspaces_color_profile_type_t type_work;
      char filename_work[DT_IOP_COLOR_ICC_LEN];
    } dt_iop_colorin_params_v6_t;

    const dt_iop_colorin_params_v6_t *old = (dt_iop_colorin_params_v6_t *)old_params;
    dt_iop_colorin_params_t *new = (dt_iop_colorin_params_t *)new_params;
    memcpy(new, old, sizeof(*new));
    _resolve_work_profile(&new->type_work, new->filename_work);

    return 0;
  }
  return 1;
#undef DT_IOP_COLOR_ICC_LEN_V5
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorin_global_data_t *gd
      = (dt_iop_colorin_global_data_t *)malloc(sizeof(dt_iop_colorin_global_data_t));
  module->data = gd;
  gd->kernel_colorin_unbound = dt_opencl_create_kernel(program, "colorin_unbound");
  gd->kernel_colorin_clipping = dt_opencl_create_kernel(program, "colorin_clipping");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorin_unbound);
  dt_opencl_free_kernel(gd->kernel_colorin_clipping);
  dt_free(module->data);
}

#if 0
static void intent_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}
#endif

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof;
  if(pos < g->n_image_profiles)
    prof = g->image_profiles;
  else
  {
    prof = darktable.color_profiles->profiles;
    pos -= g->n_image_profiles;
  }
  for(; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos == pos)
    {
      p->type = pp->type;
      memcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_INPUT);
      return;
    }
  }
  // should really never happen.
  dt_print(DT_DEBUG_COLORPROFILE, "[colorin] color profile %s seems to have disappeared!\n",
           dt_colorspaces_get_name(p->type, p->filename));
}

static void workicc_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  if(darktable.gui->reset) return;

  dt_iop_request_focus(self);

  dt_colorspaces_color_profile_type_t type_work = DT_COLORSPACE_NONE;
  char filename_work[DT_IOP_COLOR_ICC_LEN];

  int pos = dt_bauhaus_combobox_get(widget);
  for(const GList *prof = darktable.color_profiles->profiles; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos == pos)
    {
      type_work = pp->type;
      g_strlcpy(filename_work, pp->filename, sizeof(filename_work));
      break;
    }
  }

  if(type_work != DT_COLORSPACE_NONE)
  {
    p->type_work = type_work;
    g_strlcpy(p->filename_work, filename_work, sizeof(p->filename_work));

    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_add_profile_info_to_list(self->dev, p->type_work, p->filename_work, DT_INTENT_PERCEPTUAL);
    if(IS_NULL_PTR(work_profile) || isnan(work_profile->matrix_in[0][0]) || isnan(work_profile->matrix_out[0][0]))
    {
      dt_print(DT_DEBUG_COLORPROFILE,
               "[colorin] can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!\n",
               p->filename_work);
      dt_control_log(_("can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!"), p->filename_work);

    }
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_WORK);

    dt_dev_pixelpipe_rebuild_all(self->dev);
  }
  else
  {
    // should really never happen.
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] color profile %s seems to have disappeared!\n",
             dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->global_data;
  cl_mem dev_m = NULL, dev_l = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  int kernel;
  float cmat[12], lmat[12];

  if(d->nrgb)
  {
    kernel = gd->kernel_colorin_clipping;
    pack_3xSSE_to_3x4(d->nmatrix, cmat);
    pack_3xSSE_to_3x4(d->lmatrix, lmat);
  }
  else
  {
    kernel = gd->kernel_colorin_unbound;
    pack_3xSSE_to_3x4(d->cmatrix, cmat);
    pack_3xSSE_to_3x4(d->lmatrix, lmat);
  }

  cl_int err = -999;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&pipe->image);
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(d->type == DT_COLORSPACE_LAB)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { roi_in->width, roi_in->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, cmat);
  if(IS_NULL_PTR(dev_m)) goto error;
  dev_l = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, lmat);
  if(IS_NULL_PTR(dev_l)) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_r)) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_g)) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_b)) goto error;
  dev_coeffs
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (float *)d->unbounded_coeffs);
  if(IS_NULL_PTR(dev_coeffs)) goto error;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), (void *)&dev_l);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_int), (void *)&blue_mapping);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_l);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_l);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorin] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static inline void apply_blue_mapping(const float *const in, float *const out)
{
  out[0] = in[0];
  out[1] = in[1];
  out[2] = in[2];

  const float YY = out[0] + out[1] + out[2];
  if(YY > 0.0f)
  {
    const float zz = out[2] / YY;
    const float bound_z = 0.5f, bound_Y = 0.5f;
    const float amount = 0.11f;
    if(zz > bound_z)
    {
      const float t = (zz - bound_z) / (1.0f - bound_z) * fminf(1.0, YY / bound_Y);
      out[1] += t * amount;
      out[2] -= t * amount;
    }
  }
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t _colorin_clamp_rgb01_vec4(
    dt_aligned_pixel_simd_t in)
{
  in[0] = CLAMP(in[0], 0.0f, 1.0f);
  in[1] = CLAMP(in[1], 0.0f, 1.0f);
  in[2] = CLAMP(in[2], 0.0f, 1.0f);
  in[3] = 0.0f;
  return in;
}

__DT_CLONE_TARGETS__
static void process_cmatrix_bm(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int ch = 4;
  const int clipping = (!IS_NULL_PTR(d->nrgb));

  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  dt_colormatrix_t nmatrix;
  transpose_3xSSE(d->nmatrix, nmatrix);
  dt_colormatrix_t lmatrix;
  transpose_3xSSE(d->lmatrix, lmatrix);
  const dt_aligned_pixel_simd_t cm0 = dt_colormatrix_row_to_simd(cmatrix, 0);
  const dt_aligned_pixel_simd_t cm1 = dt_colormatrix_row_to_simd(cmatrix, 1);
  const dt_aligned_pixel_simd_t cm2 = dt_colormatrix_row_to_simd(cmatrix, 2);
  const dt_aligned_pixel_simd_t nm0 = dt_colormatrix_row_to_simd(nmatrix, 0);
  const dt_aligned_pixel_simd_t nm1 = dt_colormatrix_row_to_simd(nmatrix, 1);
  const dt_aligned_pixel_simd_t nm2 = dt_colormatrix_row_to_simd(nmatrix, 2);
  const dt_aligned_pixel_simd_t lm0 = dt_colormatrix_row_to_simd(lmatrix, 0);
  const dt_aligned_pixel_simd_t lm1 = dt_colormatrix_row_to_simd(lmatrix, 1);
  const dt_aligned_pixel_simd_t lm2 = dt_colormatrix_row_to_simd(lmatrix, 2);

    // fprintf(stderr, "Using cmatrix codepath\n");
    // only color matrix. use our optimized fast path!
  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * j * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * j * roi_out->width;
    dt_aligned_pixel_t cam;

    for(int i = 0; i < roi_out->width; i++)
    {
      const float *const in_pixel = in + (size_t)ch * i;
      float *const out_pixel = out + (size_t)ch * i;
      // memcpy(cam, buf_in, sizeof(float)*3);
      // avoid calling this for linear profiles (marked with negative entries), assures unbounded
      // color management without extrapolation.
      for(int c = 0; c < 3; c++)
        cam[c] = (d->lut[c][0] >= 0.0f) ? dt_ioppr_eval_trc(in_pixel[c], d->lut[c], d->unbounded_coeffs[c], LUT_SAMPLES)
                                        : in_pixel[c];
      cam[3] = 0.0f;

      apply_blue_mapping(cam, cam);
      const dt_aligned_pixel_simd_t cam_v = dt_load_simd_aligned(cam);

      if(!clipping)
      {
        dt_store_simd_nontemporal(out_pixel, dt_mat3x4_mul_vec4(cam_v, cm0, cm1, cm2));
      }
      else
      {
        const dt_aligned_pixel_simd_t nRGB = dt_mat3x4_mul_vec4(cam_v, nm0, nm1, nm2);
        const dt_aligned_pixel_simd_t cRGB = _colorin_clamp_rgb01_vec4(nRGB);
        dt_store_simd_nontemporal(out_pixel, dt_mat3x4_mul_vec4(cRGB, lm0, lm1, lm2));
      }
    }
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

__DT_CLONE_TARGETS__
static void process_cmatrix_fastpath_simple(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                                            const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  const float *const restrict in = DT_IS_ALIGNED(ivoid);
  float *const restrict out = DT_IS_ALIGNED(ovoid);

  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  const dt_aligned_pixel_simd_t cm0 = dt_colormatrix_row_to_simd(cmatrix, 0);
  const dt_aligned_pixel_simd_t cm1 = dt_colormatrix_row_to_simd(cmatrix, 1);
  const dt_aligned_pixel_simd_t cm2 = dt_colormatrix_row_to_simd(cmatrix, 2);

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
  __OMP_PARALLEL_FOR_SIMD__(aligned(in, out:64))
  for(size_t k = 0; k < npixels; k++)
  {
    const size_t idx = 4 * k;
    const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in + idx);
    dt_store_simd_nontemporal(out + idx, dt_mat3x4_mul_vec4(vin, cm0, cm1, cm2));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

__DT_CLONE_TARGETS__
static void process_cmatrix_fastpath_clipping(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                                              const void *const ivoid, void *const ovoid,
                                              const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float *const restrict in = DT_IS_ALIGNED(ivoid);
  float *const restrict out = DT_IS_ALIGNED(ovoid);

  dt_colormatrix_t nmatrix;
  dt_colormatrix_t lmatrix;
  transpose_3xSSE(d->nmatrix, nmatrix);
  transpose_3xSSE(d->lmatrix, lmatrix);
  const dt_aligned_pixel_simd_t nm0 = dt_colormatrix_row_to_simd(nmatrix, 0);
  const dt_aligned_pixel_simd_t nm1 = dt_colormatrix_row_to_simd(nmatrix, 1);
  const dt_aligned_pixel_simd_t nm2 = dt_colormatrix_row_to_simd(nmatrix, 2);
  const dt_aligned_pixel_simd_t lm0 = dt_colormatrix_row_to_simd(lmatrix, 0);
  const dt_aligned_pixel_simd_t lm1 = dt_colormatrix_row_to_simd(lmatrix, 1);
  const dt_aligned_pixel_simd_t lm2 = dt_colormatrix_row_to_simd(lmatrix, 2);

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
  __OMP_PARALLEL_FOR_SIMD__(aligned(in, out:64))
  for(size_t k = 0; k < npixels; k++)
  {
    const size_t idx = 4 * k;
    const dt_aligned_pixel_simd_t vin = dt_load_simd_aligned(in + idx);
    const dt_aligned_pixel_simd_t nRGB = dt_mat3x4_mul_vec4(vin, nm0, nm1, nm2);
    const dt_aligned_pixel_simd_t cRGB = _colorin_clamp_rgb01_vec4(nRGB);
    dt_store_simd_nontemporal(out + idx, dt_mat3x4_mul_vec4(cRGB, lm0, lm1, lm2));
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

static inline __attribute__((always_inline)) void process_cmatrix_fastpath(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                                     const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (!IS_NULL_PTR(d->nrgb));

  if(!clipping)
  {
    process_cmatrix_fastpath_simple(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_cmatrix_fastpath_clipping(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

__DT_CLONE_TARGETS__
static void process_cmatrix_proper(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int clipping = (!IS_NULL_PTR(d->nrgb));
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  dt_colormatrix_t cmatrix;
  transpose_3xSSE(d->cmatrix, cmatrix);
  dt_colormatrix_t nmatrix;
  transpose_3xSSE(d->nmatrix, nmatrix);
  dt_colormatrix_t lmatrix;
  transpose_3xSSE(d->lmatrix, lmatrix);
  const dt_aligned_pixel_simd_t cm0 = dt_colormatrix_row_to_simd(cmatrix, 0);
  const dt_aligned_pixel_simd_t cm1 = dt_colormatrix_row_to_simd(cmatrix, 1);
  const dt_aligned_pixel_simd_t cm2 = dt_colormatrix_row_to_simd(cmatrix, 2);
  const dt_aligned_pixel_simd_t nm0 = dt_colormatrix_row_to_simd(nmatrix, 0);
  const dt_aligned_pixel_simd_t nm1 = dt_colormatrix_row_to_simd(nmatrix, 1);
  const dt_aligned_pixel_simd_t nm2 = dt_colormatrix_row_to_simd(nmatrix, 2);
  const dt_aligned_pixel_simd_t lm0 = dt_colormatrix_row_to_simd(lmatrix, 0);
  const dt_aligned_pixel_simd_t lm1 = dt_colormatrix_row_to_simd(lmatrix, 1);
  const dt_aligned_pixel_simd_t lm2 = dt_colormatrix_row_to_simd(lmatrix, 2);

// fprintf(stderr, "Using cmatrix codepath\n");
// only color matrix. use our optimized fast path!
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < npixels; k++)
  {
    const float *in = (const float *)ivoid + 4 * k;
    float *out = (float *)ovoid + 4 * k;
    dt_aligned_pixel_t cam;

    // memcpy(cam, buf_in, sizeof(float)*3);
    // avoid calling this for linear profiles (marked with negative entries), assures unbounded
    // color management without extrapolation.
    for(int c = 0; c < 3; c++)
      cam[c] = (d->lut[c][0] >= 0.0f) ? dt_ioppr_eval_trc(in[c], d->lut[c], d->unbounded_coeffs[c], LUT_SAMPLES)
                                      : in[c];
    cam[3] = 0.0f;
    const dt_aligned_pixel_simd_t cam_v = dt_load_simd_aligned(cam);

    if(!clipping)
    {
      dt_store_simd_nontemporal(out, dt_mat3x4_mul_vec4(cam_v, cm0, cm1, cm2));
    }
    else
    {
      const dt_aligned_pixel_simd_t nRGB = dt_mat3x4_mul_vec4(cam_v, nm0, nm1, nm2);
      const dt_aligned_pixel_simd_t cRGB = _colorin_clamp_rgb01_vec4(nRGB);
      dt_store_simd_nontemporal(out, dt_mat3x4_mul_vec4(cRGB, lm0, lm1, lm2));
    }
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read output
}

static inline __attribute__((always_inline)) void process_cmatrix(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                            const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&pipe->image);

  if(!blue_mapping && d->nonlinearlut == 0)
  {
    process_cmatrix_fastpath(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else if(blue_mapping)
  {
    process_cmatrix_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_cmatrix_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

__DT_CLONE_TARGETS__
static void process_lcms2_bm(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  /* LCMS is handled through local aliases so the OpenMP region shares explicit
   * transform state rather than dereferencing `d->xform_*` inside the loop. */
  const cmsHTRANSFORM xform_cam_lab = d->xform_cam_Lab;
  const cmsHTRANSFORM xform_cam_nrgb = d->xform_cam_nrgb;
  const cmsHTRANSFORM xform_nrgb_lab = d->xform_nrgb_Lab;
  const gboolean use_nrgb = (!IS_NULL_PTR(d->nrgb));
  const int ch = 4;
// use general lcms2 fallback
  __OMP_PARALLEL_FOR__()
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * k * roi_out->width;

    float *camptr = (float *)out;
    for(int j = 0; j < roi_out->width; j++)
    {
      float *const pixel = camptr + 4 * j;
      apply_blue_mapping(in + 4 * j, pixel);
      pixel[3] = 0.0f;
    }

    // convert from input profile to pipeline work RGB.
    if(!use_nrgb)
    {
      dt_colorspaces_transform_rgba_float_row(xform_cam_lab, out, out, roi_out->width);
    }
    else
    {
      dt_colorspaces_transform_rgba_float_row(xform_cam_nrgb, out, out, roi_out->width);

      float *rgbptr = (float *)out;
      __OMP_SIMD__(aligned(rgbptr:64))
      for(int j = 0; j < roi_out->width; j++)
      {
        float *const pixel = rgbptr + 4 * j;
        for(int c = 0; c < 3; c++) pixel[c] = CLAMP(pixel[c], 0.0f, 1.0f);
        pixel[3] = 0.0f;
      }

      dt_colorspaces_transform_rgba_float_row(xform_nrgb_lab, out, out, roi_out->width);
    }
  }
}

__DT_CLONE_TARGETS__
static void process_lcms2_proper(struct dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  /* LCMS is handled through local aliases so the OpenMP region shares explicit
   * transform state rather than dereferencing `d->xform_*` inside the loop. */
  const cmsHTRANSFORM xform_cam_lab = d->xform_cam_Lab;
  const cmsHTRANSFORM xform_cam_nrgb = d->xform_cam_nrgb;
  const cmsHTRANSFORM xform_nrgb_lab = d->xform_nrgb_Lab;
  const gboolean use_nrgb = (!IS_NULL_PTR(d->nrgb));
  const int ch = 4;

// use general lcms2 fallback
  __OMP_PARALLEL_FOR__()
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = (const float *)ivoid + (size_t)ch * k * roi_out->width;
    float *out = (float *)ovoid + (size_t)ch * k * roi_out->width;

    // convert from input profile to pipeline work RGB.
    if(!use_nrgb)
    {
      dt_colorspaces_transform_rgba_float_row(xform_cam_lab, in, out, roi_out->width);
    }
    else
    {
      dt_colorspaces_transform_rgba_float_row(xform_cam_nrgb, in, out, roi_out->width);

      float *rgbptr = (float *)out;
      __OMP_SIMD__(aligned(rgbptr:64))
      for(int j = 0; j < roi_out->width; j++)
      {
        float *const pixel = rgbptr + 4 * j;
        for(int c = 0; c < 3; c++) pixel[c] = CLAMP(pixel[c], 0.0f, 1.0f);
      }

      dt_colorspaces_transform_rgba_float_row(xform_nrgb_lab, out, out, roi_out->width);
    }
  }
}

static inline __attribute__((always_inline)) void process_lcms2(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                          const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&pipe->image);

  // use general lcms2 fallback
  if(blue_mapping)
  {
    process_lcms2_bm(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_lcms2_proper(self, piece, ivoid, ovoid, roi_in, roi_out);
  }
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

  if(d->type == DT_COLORSPACE_LAB)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
  }
  else if(!isnan(d->cmatrix[0][0]))
  {
    process_cmatrix(self, pipe, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
  {
    process_lcms2(self, pipe, piece, ivoid, ovoid, roi_in, roi_out);
  }

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

static void _reset_input_transforms(dt_iop_colorin_data_t *d)
{
  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }
}

static void _reset_processing_state(dt_iop_colorin_data_t *d, dt_dev_pixelpipe_iop_t *piece)
{
  d->cmatrix[0][0] = d->nmatrix[0][0] = d->lmatrix[0][0] = NAN;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  d->nonlinearlut = 0;
  piece->process_cl_ready = 1;
}

static void _select_normalization_profile(const dt_iop_colorin_params_t *p, dt_iop_colorin_data_t *d)
{
  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_ADOBE_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC709_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_LINEAR_REC2020_RGB:
      d->nrgb = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_DIRECTION_IN)->profile;
      break;
    case DT_NORMALIZE_OFF:
    default:
      d->nrgb = NULL;
  }
}

static dt_colorspaces_color_profile_type_t _resolve_input_profile(const dt_iop_colorin_params_t *p,
                                                                  dt_dev_pixelpipe_t *pipe,
                                                                  dt_iop_colorin_data_t *d)
{
  dt_colorspaces_color_profile_type_t type = p->type;
  const dt_colorspaces_color_profile_type_t requested_type = type;

  if(type == DT_COLORSPACE_ENHANCED_MATRIX)
  {
    d->input = dt_colorspaces_create_darktable_profile(pipe->image.camera_makermodel);
    if(IS_NULL_PTR(d->input)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }
  if(type == DT_COLORSPACE_VENDOR_MATRIX)
  {
    d->input = dt_colorspaces_create_vendor_profile(pipe->image.camera_makermodel);
    if(IS_NULL_PTR(d->input)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }
  if(type == DT_COLORSPACE_ALTERNATE_MATRIX)
  {
    d->input = dt_colorspaces_create_alternate_profile(pipe->image.camera_makermodel);
    if(IS_NULL_PTR(d->input)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else d->clear_input = 1;
  }

  if(type == DT_COLORSPACE_EMBEDDED_ICC
     || type == DT_COLORSPACE_EMBEDDED_MATRIX
     || type == DT_COLORSPACE_STANDARD_MATRIX)
  {
    gboolean new_profile = FALSE;
    cmsHPROFILE profile = NULL;
    type = dt_colorspaces_get_input_profile_from_image(pipe->image.id, type, &profile, &new_profile);
    if(!IS_NULL_PTR(profile))
    {
      d->input = profile;
      d->clear_input = new_profile;
    }
  }

  if(requested_type == DT_COLORSPACE_STANDARD_MATRIX
     && type == DT_COLORSPACE_LIN_REC709
     && dt_image_is_matrix_correction_supported(&pipe->image))
  {
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] `%s' color matrix not found!\n", pipe->image.camera_makermodel);
    dt_control_log(_("`%s' color matrix not found!"), pipe->image.camera_makermodel);
  }

  if(IS_NULL_PTR(d->input))
  {
    const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, p->filename, DT_PROFILE_DIRECTION_IN);
    if(!IS_NULL_PTR(profile)) d->input = profile->profile;
  }

  if(IS_NULL_PTR(d->input) && type != DT_COLORSPACE_SRGB)
  {
    // use linear_rec709_rgb as fallback for missing non-sRGB profiles:
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  // final resort: sRGB
  if(IS_NULL_PTR(d->input))
  {
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
  }

  return type;
}

static void _set_input_profile_metadata(dt_iop_colorin_data_t *d,
                                        const dt_iop_colorin_params_t *p,
                                        const dt_colorspaces_color_profile_type_t type)
{
  d->type = type;
  if(type == DT_COLORSPACE_FILE)
    g_strlcpy(d->filename, p->filename, sizeof(d->filename));
  else
    d->filename[0] = '\0';
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;

  d->type_work = p->type_work;
  g_strlcpy(d->filename_work, p->filename_work, sizeof(d->filename_work));

  // only clean up when it's a type that we created here
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  d->input = NULL;
  d->clear_input = 0;
  d->nrgb = NULL;

  d->blue_mapping = p->blue_mapping;
  _select_normalization_profile(p, d);
  _reset_input_transforms(d);
  _reset_processing_state(d, piece);

  // commit and resolve working profile first, it is the target output profile of this module
  dt_iop_order_iccprofile_info_t *work_profile_info
      = dt_ioppr_set_pipe_work_profile_info(self->dev, pipe, d->type_work, d->filename_work, DT_INTENT_PERCEPTUAL);
  if(work_profile_info)
  {
    d->type_work = work_profile_info->type;
    g_strlcpy(d->filename_work, work_profile_info->filename, sizeof(d->filename_work));
  }

  const dt_colorspaces_color_profile_t *work_profile
      = dt_colorspaces_get_profile(d->type_work, d->filename_work, DT_PROFILE_DIRECTION_ANY);
  const cmsHPROFILE work = work_profile ? work_profile->profile : NULL;
  const gboolean can_use_work_matrix = (work_profile_info
                                        && !work_profile_info->nonlinearlut
                                        && !isnan(work_profile_info->matrix_out[0][0]));

  dt_colorspaces_color_profile_type_t type = p->type;
  if(type == DT_COLORSPACE_LAB)
  {
    _set_input_profile_metadata(d, p, type);
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;

  type = _resolve_input_profile(p, pipe, d);

  // should never happen, but catch that case to avoid a crash
  if(IS_NULL_PTR(d->input))
  {
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] input profile could not be generated!\n");
    dt_control_log(_("input profile could not be generated!"));
    piece->enabled = 0;
    return;
  }

  _set_input_profile_metadata(d, p, type);

  cmsColorSpaceSignature input_color_space = cmsGetColorSpace(d->input);
  cmsUInt32Number input_format;
  switch(input_color_space)
  {
    case cmsSigRgbData:
      input_format = TYPE_RGBA_FLT;
      break;
    case cmsSigXYZData:
      // FIXME: even though this is allowed/works, dt_ioppr_generate_profile_info still complains about these profiles
      input_format = TYPE_XYZA_FLT;
      break;
    default:
      // fprintf("%.*s", 4, input_color_space) doesn't work, it prints the string backwards :(
      dt_print(DT_DEBUG_COLORPROFILE, "[colorin] input profile color space `%c%c%c%c' not supported\n",
               (char)(input_color_space>>24),
               (char)(input_color_space>>16),
               (char)(input_color_space>>8),
               (char)(input_color_space));
      input_format = TYPE_RGBA_FLT; // this will fail later, triggering the linear rec709 fallback
  }

  gboolean use_matrix = FALSE;
  dt_colormatrix_t input_matrix;

  // prepare transformation matrix or lcms2 transforms as fallback
  if(d->nrgb)
  {
    // user wants us to clip to a given RGB profile before converting to work RGB
    if(!dt_colorspaces_get_matrix_from_input_profile(d->input, input_matrix, d->lut[0], d->lut[1], d->lut[2],
                                                     LUT_SAMPLES)
       && can_use_work_matrix)
    {
      float lutr[1], lutg[1], lutb[1];
      dt_colormatrix_t omat;
      dt_colormatrix_t nrgb_in;
      if(!dt_colorspaces_get_matrix_from_output_profile(d->nrgb, omat, lutr, lutg, lutb, 1)
         && !dt_colorspaces_get_matrix_from_input_profile(d->nrgb, nrgb_in, lutr, lutg, lutb, 1))
      {
        dt_colormatrix_mul(d->cmatrix, work_profile_info->matrix_out, input_matrix);
        dt_colormatrix_mul(d->nmatrix, omat, input_matrix);
        dt_colormatrix_mul(d->lmatrix, work_profile_info->matrix_out, nrgb_in);
        use_matrix = TRUE;
      }
    }

    if(!use_matrix)
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = work ? cmsCreateTransform(d->input, input_format, work, TYPE_RGBA_FLT, p->intent, 0) : NULL;
      d->xform_cam_nrgb = cmsCreateTransform(d->input, input_format, d->nrgb, TYPE_RGBA_FLT, p->intent, 0);
      d->xform_nrgb_Lab = work ? cmsCreateTransform(d->nrgb, TYPE_RGBA_FLT, work, TYPE_RGBA_FLT, p->intent, 0) : NULL;
    }
  }
  else
  {
    // default mode: unbound processing directly to work RGB
    if(!dt_colorspaces_get_matrix_from_input_profile(d->input, input_matrix, d->lut[0], d->lut[1], d->lut[2],
                                                     LUT_SAMPLES)
       && can_use_work_matrix)
    {
      dt_colormatrix_mul(d->cmatrix, work_profile_info->matrix_out, input_matrix);
      use_matrix = TRUE;
    }

    if(!use_matrix)
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = work ? cmsCreateTransform(d->input, input_format, work, TYPE_RGBA_FLT, p->intent, 0) : NULL;
    }
  }

  // we might have failed generating the clipping transformations, check that:
  if(d->nrgb && ((IS_NULL_PTR(d->xform_cam_nrgb) && isnan(d->nmatrix[0][0])) || (IS_NULL_PTR(d->xform_nrgb_Lab) && isnan(d->lmatrix[0][0]))))
  {
    if(d->xform_cam_nrgb)
    {
      cmsDeleteTransform(d->xform_cam_nrgb);
      d->xform_cam_nrgb = NULL;
    }
    if(d->xform_nrgb_Lab)
    {
      cmsDeleteTransform(d->xform_nrgb_Lab);
      d->xform_nrgb_Lab = NULL;
    }
    d->nrgb = NULL;
  }

  // user selected a non-supported input profile, check that:
  if(IS_NULL_PTR(d->xform_cam_Lab) && isnan(d->cmatrix[0][0]))
  {
    if(p->type == DT_COLORSPACE_FILE)
      dt_print(DT_DEBUG_COLORPROFILE,
               "[colorin] unsupported input profile `%s' has been replaced by linear Rec709 RGB!\n",
               p->filename);
    else
      dt_print(DT_DEBUG_COLORPROFILE, "[colorin] unsupported input profile has been replaced by linear Rec709 RGB!\n");
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
    d->nrgb = NULL;
    d->input = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_DIRECTION_IN)->profile;
    d->clear_input = 0;
    use_matrix = FALSE;
    if(!dt_colorspaces_get_matrix_from_input_profile(d->input, input_matrix, d->lut[0], d->lut[1], d->lut[2],
                                                     LUT_SAMPLES)
       && can_use_work_matrix)
    {
      dt_colormatrix_mul(d->cmatrix, work_profile_info->matrix_out, input_matrix);
      use_matrix = TRUE;
    }

    if(!use_matrix)
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0][0] = NAN;
      d->xform_cam_Lab = work ? cmsCreateTransform(d->input, TYPE_RGBA_FLT, work, TYPE_RGBA_FLT, p->intent, 0) : NULL;
    }
  }

  d->nonlinearlut = 0;

  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k = 0; k < 3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      d->nonlinearlut++;

      const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
      const float y[4] = { extrapolate_lut(d->lut[k], x[0], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[1], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[2], LUT_SAMPLES),
                           extrapolate_lut(d->lut[k], x[3], LUT_SAMPLES) };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else
      d->unbounded_coeffs[k][0] = -1.0f;
  }

  // commit input profile metadata to pipeline with the original input RGB -> XYZ matrix
  dt_colormatrix_t input_matrix_for_pipe = { { NAN } };
  if(d->input)
    dt_colorspaces_get_matrix_from_input_profile(d->input, input_matrix_for_pipe, NULL, NULL, NULL, 0);

  dt_ioppr_set_pipe_input_profile_info(self->dev, pipe, d->type, d->filename, p->intent,
                                       input_matrix_for_pipe);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_colorin_data_t));
  piece->data_size = sizeof(dt_iop_colorin_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input && d->clear_input) dt_colorspaces_cleanup_profile(d->input);
  if(d->xform_cam_Lab)
  {
    cmsDeleteTransform(d->xform_cam_Lab);
    d->xform_cam_Lab = NULL;
  }
  if(d->xform_cam_nrgb)
  {
    cmsDeleteTransform(d->xform_cam_nrgb);
    d->xform_cam_nrgb = NULL;
  }
  if(d->xform_nrgb_Lab)
  {
    cmsDeleteTransform(d->xform_nrgb_Lab);
    d->xform_nrgb_Lab = NULL;
  }

  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;

  dt_bauhaus_combobox_set(g->clipping_combobox, p->normalize);

  // working profile
  int idx = -1;
  for(const GList *prof = darktable.color_profiles->profiles; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->work_pos > -1
       && pp->type == p->type_work
       && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename_work)))
    {
      idx = pp->work_pos;
      break;
    }
  }

  if(idx < 0)
  {
    idx = 0;
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] could not find requested working profile `%s'!\n",
             dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
  dt_bauhaus_combobox_set(g->work_combobox, idx);

  for(const GList *prof = g->image_profiles; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->type == p->type
       && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos);
      return;
    }
  }

  for(const GList *prof = darktable.color_profiles->profiles; prof; prof = g_list_next(prof))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)prof->data;
    if(pp->in_pos > -1
       && pp->type == p->type
       && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, pp->in_pos + g->n_image_profiles);
      return;
    }
  }

  // Error happened, otherwise we would have returned earlier
  dt_bauhaus_combobox_set(g->profile_combobox, 0);

  const gboolean matrix_supported = dt_image_is_matrix_correction_supported(&self->dev->image_storage);
  if(p->type != DT_COLORSPACE_ENHANCED_MATRIX
     && !(dt_colorspaces_is_raw_matrix_profile_type(p->type) && !matrix_supported))
  {
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] could not find requested profile `%s'!\n",
             dt_colorspaces_get_name(p->type, p->filename));

    dt_control_log(_("The color profile `%s' referenced as input profile has not been found."), dt_colorspaces_get_name(p->type, p->filename));
  }
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 1;
  module->hide_enable_button = 1;

  dt_iop_colorin_params_t *d = module->default_params;
  gboolean new_profile;
  d->type = dt_image_find_best_color_profile(module->dev->image_storage.id, NULL, &new_profile);
  update_profile_list(module);
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  if(IS_NULL_PTR(g)) return;

  // clear and refill the image profile list
  g_list_free_full(g->image_profiles, dt_free_gpointer);
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_ICC, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_ICC;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }
  dt_image_cache_read_release(darktable.image_cache, cimg);
  // use the matrix embedded in some DNGs and EXRs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  if(dt_image_is_matrix_correction_supported(&self->dev->image_storage)
     && !(self->dev->image_storage.flags & DT_IMAGE_4BAYER))
  {
    dt_colorspaces_color_profile_t *prof
        = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_STANDARD_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_STANDARD_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->in_pos = ++pos;
  }

  // darktable built-in, if applicable
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(self->dev->image_storage.camera_makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ENHANCED_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ENHANCED_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_VENDOR_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_VENDOR_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      dt_colorspaces_color_profile_t *prof
          = (dt_colorspaces_color_profile_t *)calloc(1, sizeof(dt_colorspaces_color_profile_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ALTERNATE_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ALTERNATE_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->in_pos = ++pos;
      break;
    }
  }

  g->n_image_profiles = pos + 1;

  // update the gui
  dt_bauhaus_combobox_clear(g->profile_combobox);

  for(GList *l = g->image_profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->in_pos > -1) dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }

  // working profile
  dt_bauhaus_combobox_clear(g->work_combobox);

  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->work_pos > -1) dt_bauhaus_combobox_add(g->work_combobox, prof->name);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  dt_iop_colorin_gui_data_t *g = IOP_GUI_ALLOC(colorin);

  g->image_profiles = NULL;

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->profile_combobox = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->profile_combobox, N_("input profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile_combobox, TRUE, TRUE, 0);

  g->work_combobox = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->work_combobox, N_("working profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->work_combobox, TRUE, TRUE, 0);

  dt_bauhaus_combobox_set(g->profile_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "in", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "in", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->profile_combobox, tooltip);
    dt_free(system_profile_dir);
    dt_free(user_profile_dir);
    dt_free(tooltip);
  }

  dt_bauhaus_combobox_set(g->work_combobox, 0);
  {
    char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
    char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
    char *tooltip = g_strdup_printf(_("ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
    gtk_widget_set_tooltip_text(g->work_combobox, tooltip);
    dt_free(system_profile_dir);
    dt_free(user_profile_dir);
    dt_free(tooltip);
  }

  g_signal_connect(G_OBJECT(g->profile_combobox), "value-changed", G_CALLBACK(profile_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(g->work_combobox), "value-changed", G_CALLBACK(workicc_changed), (gpointer)self);

  g->clipping_combobox = dt_bauhaus_combobox_from_params(self, "normalize");
  gtk_widget_set_tooltip_text(g->clipping_combobox, _("confine Lab values to gamut of RGB color space"));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    dt_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
