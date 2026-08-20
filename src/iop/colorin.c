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
#include "system/macros.h"
#include "common/module_versioning.h"
#include "common/logging.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "config.h"
#endif
#include "widgets/bauhaus.h"
#include "common/imagebuf.h"
#include "develop/iop_profile.h"
#include "colorprofiles/colormatrices.c"
#include "colorprofiles/colorspaces.h"
#include "colorprofiles/conversion.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "caches/image_cache.h"
#include "common/opencl.h"
#include "control/user_message.h"
#include "develop/develop.h"

#ifdef HAVE_OPENJPEG
#endif
#ifdef HAVE_LIBAVIF
#endif
#ifdef HAVE_LIBHEIF
#endif
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "imageio/imageio_profile.h"
#include "control/signal.h"

// max iccprofile file name length
// must be in synch with dt_colorspaces_color_profile_t
#define DT_IOP_COLOR_ICC_LEN 512


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
  /* This module converts the image's INPUT profile to the pipeline's WORKING profile.
   * Both are RGB. The `_Lab` names these fields used to carry were left over from when the
   * pipe was Lab: nothing here has produced Lab since, except in the one case where the
   * user picks DT_COLORSPACE_LAB as the input profile itself, which _colorin_format_cst()
   * reports through output_format().
   *
   * The optional gamut-clipping detour -- "gamut clipping" set to anything but
   * DT_NORMALIZE_OFF, which is not the default -- goes input -> clip primaries, clamps to
   * [0,1] there, then clip -> work. That is a shape colorprofiles/conversion.c expresses, so
   * it is one endpoint on the request rather than a second set of fields here. */
  dt_colorspaces_conversion_t *conversion;
  int blue_mapping;
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
  if(dt_colorspaces_profile_exists(DT_PROFILE_ROLE_WORKING, *work_type, work_filename)) return;

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
    else if(!strcmp(old->iccprofile, "matrix_in_to_work"))
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
    else if(!strcmp(old->iccprofile, "matrix_in_to_work"))
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
    else if(!strcmp(old->iccprofile, "matrix_in_to_work"))
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
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}
#endif

static void profile_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)dt_iop_gui_data(self);
  int pos = dt_bauhaus_combobox_get(widget);

  /* The combo lists this image's own derived profiles first (g->image_profiles, which
   * belongs to this module and is NOT the module-wide list), then the registered input
   * profiles. */
  if(pos < g->n_image_profiles)
  {
    /* The combo lists these first and in order, so the row IS the list position. */
    const dt_colorprofile_desc_t *pp = (const dt_colorprofile_desc_t *)g_list_nth_data(g->image_profiles, pos);
    if(!IS_NULL_PTR(pp))
    {
      p->type = pp->type;
      memcpy(p->filename, pp->filename, sizeof(p->filename));
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_INPUT);
      return;
    }
  }
  else
  {
    dt_colorprofile_desc_t desc;
    if(dt_colorspaces_profile_at(DT_PROFILE_ROLE_INPUT, pos - g->n_image_profiles, &desc))
    {
      p->type = desc.type;
      memcpy(p->filename, desc.filename, sizeof(p->filename));
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_INPUT);
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
  if(dt_gui_widgets_suppressed()) return;

  dt_iop_request_focus(self);

  dt_colorspaces_color_profile_type_t type_work = DT_COLORSPACE_NONE;
  char filename_work[DT_IOP_COLOR_ICC_LEN];

  const int pos = dt_bauhaus_combobox_get(widget);
  dt_colorprofile_desc_t work_desc;
  if(dt_colorspaces_profile_at(DT_PROFILE_ROLE_WORKING, pos, &work_desc))
  {
    type_work = work_desc.type;
    g_strlcpy(filename_work, work_desc.filename, sizeof(filename_work));
  }

  if(type_work != DT_COLORSPACE_NONE)
  {
    p->type_work = type_work;
    g_strlcpy(p->filename_work, filename_work, sizeof(p->filename_work));

    const dt_iop_order_iccprofile_info_t *const work_profile = dt_colorspaces_add_profile(p->type_work, p->filename_work, DT_INTENT_PERCEPTUAL);
    if(IS_NULL_PTR(work_profile) || isnan(work_profile->matrix_in[0][0]) || isnan(work_profile->matrix_out[0][0]))
    {
      dt_print(DT_DEBUG_COLORPROFILE,
               "[colorin] can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!\n",
               p->filename_work);
      dt_control_log(_("can't extract matrix from colorspace `%s', it will be replaced by Rec2020 RGB!"), p->filename_work);

    }
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_WORK);

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

  /* The kernel needs the numbers the conversion reduced to. It cannot call back into
   * colorprofiles, so this is the one place the module reads them -- to upload them, not to
   * run the conversion here. */
  dt_colormatrix_t first_leg, second_leg = { { 0.0f } };
  if(!dt_colorspaces_conversion_matrix(d->conversion, first_leg)) return FALSE;
  const gboolean clipping = dt_colorspaces_conversion_has_clipping(d->conversion);
  if(clipping) dt_colorspaces_conversion_clip_matrix(d->conversion, second_leg);

  const int kernel = clipping ? gd->kernel_colorin_clipping : gd->kernel_colorin_unbound;
  float cmat[12], lmat[12];
  pack_3xSSE_to_3x4(first_leg, cmat);
  pack_3xSSE_to_3x4(second_leg, lmat);

  cl_int err = -999;
  const int blue_mapping = d->blue_mapping && dt_image_is_matrix_correction_supported(&pipe->dev->image_storage);
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
  const float *const curve_r = dt_colorspaces_conversion_source_curve(d->conversion, 0);
  const float *const curve_g = dt_colorspaces_conversion_source_curve(d->conversion, 1);
  const float *const curve_b = dt_colorspaces_conversion_source_curve(d->conversion, 2);
  const float *const coeffs = dt_colorspaces_conversion_source_coeffs(d->conversion);
  if(IS_NULL_PTR(curve_r) || IS_NULL_PTR(curve_g) || IS_NULL_PTR(curve_b) || IS_NULL_PTR(coeffs)) goto error;

  dev_r = dt_opencl_copy_host_to_device(devid, (void *)curve_r, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_r)) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, (void *)curve_g, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_g)) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, (void *)curve_b, 256, 256, sizeof(float));
  if(IS_NULL_PTR(dev_b)) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * 3, (void *)coeffs);
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

/* A legacy per-pixel tweak: it lifts green out of deep blues to work around a highlight
 * artefact of the old Lab pipeline. Nothing turns it on for a new edit -- only legacy_params
 * sets it, for history written by darktable v1 and v2 -- and it is why this module still has
 * two process variants instead of one. It runs on the values just before the colour
 * conversion proper, which colorprofiles/conversion.c takes as a hook. */
static void apply_blue_mapping(const float *const in, float *const out)
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

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;

  /* Lab in means Lab out: the pipe already carries it, and there is nothing to convert. A
   * NULL conversion means commit_params could not build one and disabled the piece. */
  if(d->type == DT_COLORSPACE_LAB || IS_NULL_PTR(d->conversion))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
  }
  else
  {
    const gboolean blue_mapping
        = d->blue_mapping && dt_image_is_matrix_correction_supported(&pipe->dev->image_storage);
    dt_colorspaces_apply_conversion_hooked(d->conversion, (const float *)ivoid, (float *)ovoid,
                                           roi_out->width, roi_out->height,
                                           blue_mapping ? apply_blue_mapping : NULL);
  }

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
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

/* The gamut-clipping detour bounds the conversion to one of four sets of primaries before it
 * reaches the working space. Off by default. */
static dt_colorspaces_color_profile_type_t _clipping_profile_type(const dt_iop_colorin_params_t *p)
{
  switch(p->normalize)
  {
    case DT_NORMALIZE_SRGB:               return DT_COLORSPACE_SRGB;
    case DT_NORMALIZE_ADOBE_RGB:          return DT_COLORSPACE_ADOBERGB;
    case DT_NORMALIZE_LINEAR_REC709_RGB:  return DT_COLORSPACE_LIN_REC709;
    case DT_NORMALIZE_LINEAR_REC2020_RGB: return DT_COLORSPACE_LIN_REC2020;
    case DT_NORMALIZE_OFF:
    default:                              return DT_COLORSPACE_NONE;
  }
}

static gboolean _is_image_derived(const dt_colorspaces_color_profile_type_t type)
{
  return type == DT_COLORSPACE_EMBEDDED_ICC || type == DT_COLORSPACE_EMBEDDED_MATRIX
         || type == DT_COLORSPACE_STANDARD_MATRIX || type == DT_COLORSPACE_ENHANCED_MATRIX
         || type == DT_COLORSPACE_VENDOR_MATRIX || type == DT_COLORSPACE_ALTERNATE_MATRIX;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;

  d->type_work = p->type_work;
  g_strlcpy(d->filename_work, p->filename_work, sizeof(d->filename_work));
  d->blue_mapping = p->blue_mapping;

  dt_colorspaces_free_conversion(&d->conversion);
  piece->process_cl_ready = 1;

  // commit and resolve working profile first, it is the target output profile of this module
  dt_iop_order_iccprofile_info_t *work_profile_info
      = dt_ioppr_set_pipe_work_profile_info(self->dev, pipe, d->type_work, d->filename_work, DT_INTENT_PERCEPTUAL);
  if(work_profile_info)
  {
    d->type_work = work_profile_info->type;
    g_strlcpy(d->filename_work, work_profile_info->filename, sizeof(d->filename_work));
  }

  if(p->type == DT_COLORSPACE_LAB)
  {
    _set_input_profile_metadata(d, p, p->type);
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;

  /* The six image-derived types are not in the profile list and cannot be named by identity:
   * their matrices come from THIS image's camera data. imageio resolves them into a container
   * that carries its own ownership answer, so this module never holds a raw handle next to a
   * "did I create it" flag -- which is what the old cascade did, across five early returns. */
  struct dt_colorspaces_color_profile_t *image_profile = NULL;
  dt_colorspaces_color_profile_type_t type = p->type;
  const dt_colorspaces_color_profile_type_t requested_type = p->type;

  if(_is_image_derived(type))
  {
    image_profile = dt_image_get_input_profile(pipe->dev->image_storage.id, type,
                                               pipe->dev->image_storage.camera_makermodel, &type);
    if(IS_NULL_PTR(image_profile)) type = requested_type;
  }

  if(requested_type == DT_COLORSPACE_STANDARD_MATRIX
     && type == DT_COLORSPACE_LIN_REC709
     && dt_image_is_matrix_correction_supported(&pipe->dev->image_storage))
  {
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] `%s' color matrix not found!\n", pipe->dev->image_storage.camera_makermodel);
    dt_control_log(_("`%s' color matrix not found!"), pipe->dev->image_storage.camera_makermodel);
  }

  const dt_colorspaces_color_profile_type_t clip_type = _clipping_profile_type(p);
  dt_colorspaces_endpoint_t clip = { .type = clip_type, .filename = "",
                                     .role = DT_PROFILE_ROLE_INPUT };
  /* WORK, not ANY. DT_COLORSPACE_SRGB is registered twice -- the v4 parametric-curve profile,
   * valid only as an input profile, and the v2 point-TRC profile, which is the one the
   * working-profile combo actually lists. The lookup filters on the direction bits before
   * matching the type and returns the first hit in registration order, and the v4 entry is
   * registered first, so ANY resolved the working profile to a variant the user was never
   * offered. */
  dt_colorspaces_endpoint_t target = { .type = d->type_work, .filename = d->filename_work,
                                       .role = DT_PROFILE_ROLE_WORKING };
  dt_colorspaces_endpoint_t source = { .type = type, .filename = p->filename,
                                       .role = DT_PROFILE_ROLE_INPUT,
                                       .resolved = image_profile };

  /* This module's kernel decodes the INPUT profile's curves before the matrix and has no
   * stage for the output side, so a non-linear working profile goes through lcms2. */
  const dt_colorspaces_conversion_flags_t flags = DT_CONVERSION_SOURCE_CURVES;

  d->conversion = dt_colorspaces_prepare_conversion(&source, &target,
                                                    clip_type == DT_COLORSPACE_NONE ? NULL : &clip,
                                                    NULL, p->intent, flags);

  if(IS_NULL_PTR(d->conversion))
  {
    /* Whatever the user asked for cannot be rendered. Linear Rec709 is the fallback that has
     * always been here; the clipping detour goes with the profile that needed it. */
    if(p->type == DT_COLORSPACE_FILE)
      dt_print(DT_DEBUG_COLORPROFILE,
               "[colorin] unsupported input profile `%s' has been replaced by linear Rec709 RGB!\n",
               p->filename);
    else
      dt_print(DT_DEBUG_COLORPROFILE, "[colorin] unsupported input profile has been replaced by linear Rec709 RGB!\n");
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));

    type = DT_COLORSPACE_LIN_REC709;
    source.type = type;
    source.filename = "";
    source.resolved = NULL;
    d->conversion = dt_colorspaces_prepare_conversion(&source, &target, NULL, NULL, p->intent, flags);
  }

  _set_input_profile_metadata(d, p, type);

  dt_iop_fmt_log(self, "commit: class=%s matrix_supported=%d requested_input=%d resolved_input=%d blue_mapping=%d",
                 dt_image_pipe_class_name(dt_image_pipe_class(&pipe->dev->image_storage)),
                 dt_image_is_matrix_correction_supported(&pipe->dev->image_storage),
                 requested_type, type, d->blue_mapping);

  if(IS_NULL_PTR(d->conversion))
  {
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] input profile could not be generated!\n");
    dt_control_log(_("input profile could not be generated!"));
    dt_colorspaces_free_image_profile(image_profile);
    piece->enabled = 0;
    return;
  }

  // Only the vectorised branch has a kernel; the lcms2 one is host-only.
  if(!dt_colorspaces_conversion_is_matrix(d->conversion)) piece->process_cl_ready = 0;

  /* The pipe records what space it is being handed, which is the input profile's own
   * RGB -> XYZ, uncomposed -- not the conversion this module runs. */
  dt_colormatrix_t input_matrix_for_pipe = { { NAN } };
  dt_colorspaces_conversion_source_matrix(d->conversion, input_matrix_for_pipe);

  dt_ioppr_set_pipe_input_profile_info(self->dev, pipe, d->type, d->filename, p->intent,
                                       input_matrix_for_pipe);

  /* An lcms2 transform does not retain the profiles it was built from, and the matrix branch
   * copied out everything it needed, so the container's job ended when the conversion was
   * built. */
  dt_colorspaces_free_image_profile(image_profile);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_colorin_data_t));
  piece->data_size = sizeof(dt_iop_colorin_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  /* init_pipe() may have failed to allocate, and cleanup runs regardless. */
  if(IS_NULL_PTR(piece->data)) return;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_colorspaces_free_conversion(&d->conversion);

  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;

  dt_bauhaus_combobox_set(g->clipping_combobox, p->normalize);

  // working profile
  int idx = dt_colorspaces_profile_index(DT_PROFILE_ROLE_WORKING, p->type_work, p->filename_work);

  if(idx < 0)
  {
    idx = 0;
    dt_print(DT_DEBUG_COLORPROFILE, "[colorin] could not find requested working profile `%s'!\n",
             dt_colorspaces_get_name(p->type_work, p->filename_work));
  }
  dt_bauhaus_combobox_set(g->work_combobox, idx);

  int image_row = 0;
  for(const GList *prof = g->image_profiles; prof; prof = g_list_next(prof), image_row++)
  {
    const dt_colorprofile_desc_t *pp = (const dt_colorprofile_desc_t *)prof->data;
    if(pp->type == p->type
       && (pp->type != DT_COLORSPACE_FILE || dt_colorspaces_is_profile_equal(pp->filename, p->filename)))
    {
      dt_bauhaus_combobox_set(g->profile_combobox, image_row);
      return;
    }
  }

  const int in_idx = dt_colorspaces_profile_index(DT_PROFILE_ROLE_INPUT, p->type, p->filename);
  if(in_idx > -1)
  {
    dt_bauhaus_combobox_set(g->profile_combobox, in_idx + g->n_image_profiles);
    return;
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
  dt_iop_fmt_log(module, "reload_defaults: class=%s matrix_supported=%d -> default_input_profile=%d new_profile=%d",
                 dt_image_pipe_class_name(dt_image_pipe_class(&module->dev->image_storage)),
                 dt_image_is_matrix_correction_supported(&module->dev->image_storage), d->type, new_profile);
  update_profile_list(module);
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)dt_iop_gui_data(self);

  if(IS_NULL_PTR(g)) return;

  // clear and refill the image profile list
  g_list_free_full(g->image_profiles, dt_free_gpointer);
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg, j2k, tiff and png
  const dt_image_t *cimg = dt_image_cache_get(self->dev->image_storage.id, 'r');
  if(cimg->profile)
  {
    dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_ICC, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_ICC;
    g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
  }
  dt_image_cache_read_release(cimg);
  // use the matrix embedded in some DNGs and EXRs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_EMBEDDED_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_EMBEDDED_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
  }

  if(dt_image_is_matrix_correction_supported(&self->dev->image_storage)
     && !(self->dev->image_storage.flags & DT_IMAGE_4BAYER))
  {
    dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
    g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_STANDARD_MATRIX, ""), sizeof(prof->name));
    prof->type = DT_COLORSPACE_STANDARD_MATRIX;
    g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
  }

  // darktable built-in, if applicable
  for(int k = 0; k < dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(self->dev->image_storage.camera_makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ENHANCED_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ENHANCED_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k = 0; k < dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_VENDOR_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_VENDOR_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k = 0; k < dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(self->dev->image_storage.camera_makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      dt_colorprofile_desc_t *prof = (dt_colorprofile_desc_t *)calloc(1, sizeof(dt_colorprofile_desc_t));
      g_strlcpy(prof->name, dt_colorspaces_get_name(DT_COLORSPACE_ALTERNATE_MATRIX, ""), sizeof(prof->name));
      prof->type = DT_COLORSPACE_ALTERNATE_MATRIX;
      g->image_profiles = g_list_append(g->image_profiles, prof);
      pos++;
      break;
    }
  }

  g->n_image_profiles = pos + 1;

  // update the gui
  dt_bauhaus_combobox_clear(g->profile_combobox);

  for(GList *l = g->image_profiles; l; l = g_list_next(l))
  {
    const dt_colorprofile_desc_t *prof = (const dt_colorprofile_desc_t *)l->data;
    dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
  gboolean input_system_profile_separator_added = FALSE;
  gboolean input_file_profile_separator_added = FALSE;
  dt_colorprofile_desc_t *in_profiles = NULL;
  const size_t n_in_profiles = dt_colorspaces_enumerate_profiles(DT_PROFILE_ROLE_INPUT, &in_profiles);
  for(size_t k = 0; k < n_in_profiles; k++)
  {
    const dt_colorprofile_desc_t *const prof = &in_profiles[k];

    if(g->n_image_profiles > 0 && !input_system_profile_separator_added)
    {
      dt_bauhaus_combobox_add_separator(g->profile_combobox);
      input_system_profile_separator_added = TRUE;
    }
    if(prof->type == DT_COLORSPACE_FILE && !input_file_profile_separator_added)
    {
      dt_bauhaus_combobox_add_separator(g->profile_combobox);
      input_file_profile_separator_added = TRUE;
    }
    if(prof->type == DT_COLORSPACE_FILE)
      dt_bauhaus_combobox_add_with_tooltip(g->profile_combobox, prof->name, prof->filename);
    else
      dt_bauhaus_combobox_add(g->profile_combobox, prof->name);
  }
  dt_free_align(in_profiles);

  // working profile
  dt_bauhaus_combobox_clear(g->work_combobox);

  gboolean work_file_profile_separator_added = FALSE;
  dt_colorprofile_desc_t *work_profiles = NULL;
  const size_t n_work_profiles = dt_colorspaces_enumerate_profiles(DT_PROFILE_ROLE_WORKING, &work_profiles);
  for(size_t k = 0; k < n_work_profiles; k++)
  {
    const dt_colorprofile_desc_t *const prof = &work_profiles[k];
    if(prof->type == DT_COLORSPACE_FILE && !work_file_profile_separator_added)
    {
      dt_bauhaus_combobox_add_separator(g->work_combobox);
      work_file_profile_separator_added = TRUE;
    }
    if(prof->type == DT_COLORSPACE_FILE)
      dt_bauhaus_combobox_add_with_tooltip(g->work_combobox, prof->name, prof->filename);
    else
      dt_bauhaus_combobox_add(g->work_combobox, prof->name);
  }
  dt_free_align(work_profiles);
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

  self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  g->profile_combobox = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->profile_combobox, N_("input profile"));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->profile_combobox, TRUE, TRUE, 0);

  g->work_combobox = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->work_combobox, N_("working profile"));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->work_combobox, TRUE, TRUE, 0);

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
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)dt_iop_gui_data(self);
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
