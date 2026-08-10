/*
    This file is part of darktable,
    Copyright (C) 2018-2021, 2023, 2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Aldric Renaudin.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 Jacques Le Clerc.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2019, 2021 Philippe Weyland.
    Copyright (C) 2019, 2021 Sakari Kapanen.
    Copyright (C) 2019 Tobias Ellinghaus.
    Copyright (C) 2020-2021 Dan Torop.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 paolodepetrillo.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2024 Alynx Zhou.
    
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
/* The pipeline-facing half of the colour-profile module: resolving which profile a module,
 * pipe or export should use, and transforming an image with it. Everything here takes
 * dt_develop_t, dt_dev_pixelpipe_t or dt_iop_module_t, which is why it sits at layer 5.
 *
 * The profile maths it delegates to lives in colorprofiles/iop_profile.c -- see the note there.
 */

#include "colorprofiles/iop_profile.h"
#include "config.h"
#endif

#include "colorprofiles/colorspaces.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "develop/iop_profile.h"
#include "math/matrices.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "develop/develop.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system/target_clones.h"
#include "common/times.h"



__DT_CLONE_TARGETS__


dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module, GList *iop_list)
{
  dt_iop_order_iccprofile_info_t *profile = NULL;

  // first check if the module is between colorin and colorout
  gboolean in_between = FALSE;

  for(GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    // we reach the module, that's it
    if(strcmp(mod->op, module->op) == 0) break;

    // if we reach colorout means that the module is after it
    if(strcmp(mod->op, "colorout") == 0)
    {
      in_between = FALSE;
      break;
    }

    // we reach colorin, so far we're good
    if(strcmp(mod->op, "colorin") == 0)
    {
      in_between = TRUE;
      break;
    }
  }

  if(in_between)
  {
    dt_colorspaces_color_profile_type_t type = DT_COLORSPACE_NONE;
    const char *filename = NULL;
    dt_develop_t *dev = module->dev;

    dt_ioppr_get_work_profile_type(dev, &type, &filename);
    if(filename) profile = dt_colorspaces_add_profile(type, filename, DT_INTENT_PERCEPTUAL);
  }

  return profile;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev,
                                    struct dt_dev_pixelpipe_t *pipe,
                                    const dt_colorspaces_color_profile_type_t type,
                                    const char *filename,
                                    const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_colorspaces_add_profile(type, filename, intent);

  if(IS_NULL_PTR(profile_info) || isnan(profile_info->matrix_in[0][0]) || isnan(profile_info->matrix_out[0][0]))
  {
    fprintf(stderr, "[dt_ioppr_set_pipe_work_profile_info] unsupported working profile %i %s, it will be replaced with linear Rec2020\n", type, filename);
    profile_info = dt_colorspaces_add_profile(DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  pipe->work_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_input_profile_info(struct dt_develop_t *dev,
                                     struct dt_dev_pixelpipe_t *pipe,
                                     const dt_colorspaces_color_profile_type_t type,
                                     const char *filename,
                                     const int intent,
                                     const dt_colormatrix_t matrix_in)
{
  dt_iop_order_iccprofile_info_t *profile_info = NULL;

  /* An image-derived profile cannot be shared, and cannot be resolved by identity either:
   * these types are not registered in the profile list, so the memo entry for them is a
   * placeholder with NaN matrices that only becomes real when the matrix colorin computed
   * FROM THIS IMAGE is written into it below. Keyed on (type, "") that entry is common to
   * every image of the same camera-matrix kind. Give it storage the pipe owns instead. */
  const gboolean image_derived = (type >= DT_COLORSPACE_EMBEDDED_ICC && type <= DT_COLORSPACE_ALTERNATE_MATRIX);

  if(image_derived)
  {
    if(IS_NULL_PTR(pipe->owned_input_profile_info))
    {
      pipe->owned_input_profile_info = dt_alloc_align(sizeof(dt_iop_order_iccprofile_info_t));
      if(!IS_NULL_PTR(pipe->owned_input_profile_info))
        dt_ioppr_init_profile_info(pipe->owned_input_profile_info, 0);
    }
    profile_info = pipe->owned_input_profile_info;

    if(!IS_NULL_PTR(profile_info))
    {
      profile_info->type = type;
      g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
      profile_info->intent = intent;
      profile_info->nonlinearlut = 0;
      profile_info->grey = 0.1842f;
    }
  }
  else
  {
    profile_info = dt_colorspaces_add_profile(type, filename, intent);
  }

  if(IS_NULL_PTR(profile_info))
  {
    fprintf(stderr,
            "[dt_ioppr_set_pipe_input_profile_info] unsupported input profile %i %s, it will be replaced with "
            "linear Rec2020\n",
            type, filename);
    profile_info = dt_colorspaces_add_profile(DT_COLORSPACE_LIN_REC2020, "", intent);
  }
  else if(image_derived)
  {
    /* We have a camera input matrix, these are not generated from files but in colorin,
    * so we need to fetch and replace them from somewhere.
    */
    memcpy(profile_info->matrix_in, matrix_in, sizeof(profile_info->matrix_in));
    mat3SSEinv(profile_info->matrix_out, profile_info->matrix_in);
    transpose_3xSSE(profile_info->matrix_in, profile_info->matrix_in_transposed);
    transpose_3xSSE(profile_info->matrix_out, profile_info->matrix_out_transposed);
  }
  pipe->input_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_output_profile_info(struct dt_develop_t *dev,
                                      struct dt_dev_pixelpipe_t *pipe,
                                      const dt_colorspaces_color_profile_type_t type,
                                      const char *filename,
                                      const int intent)
{
  dt_iop_order_iccprofile_info_t *profile_info = dt_colorspaces_add_profile(type, filename, intent);

  if(IS_NULL_PTR(profile_info) || isnan(profile_info->matrix_in[0][0]) || isnan(profile_info->matrix_out[0][0]))
  {
    if (type != DT_COLORSPACE_DISPLAY)
    {
      // ??? this error output has been disabled for a display profile.
      // see discussion in https://github.com/darktable-org/darktable/issues/6774
      fprintf(stderr,
              "[dt_ioppr_set_pipe_output_profile_info] unsupported output"
              " profile %i %s, it will be replaced with sRGB\n",
              type, filename);
    }
    profile_info = dt_colorspaces_add_profile(DT_COLORSPACE_SRGB, "", intent);
  }
  pipe->output_profile_info = profile_info;

  return profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->work_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_input_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->input_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_output_profile_info(const struct dt_dev_pixelpipe_t *pipe)
{
  return pipe->output_profile_info;
}

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_current_profile_info(dt_iop_module_t *module,
                                                                       const struct dt_dev_pixelpipe_t *pipe)
{
  dt_iop_order_iccprofile_info_t *restrict color_profile;

  const int colorin_order = dt_ioppr_get_iop_order(module->dev->iop_order_list, "colorin", 0);
  const int colorout_order = dt_ioppr_get_iop_order(module->dev->iop_order_list, "colorout", 0);
  const int current_module_order = module->iop_order;

  if(current_module_order < colorin_order)
    color_profile = dt_ioppr_get_pipe_input_profile_info(pipe);
  else if(current_module_order < colorout_order)
    color_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  else
    color_profile = dt_ioppr_get_pipe_output_profile_info(pipe);

  return color_profile;
}

// returns a pointer to the filename of the work profile instead of the actual string data
// pointer must not be stored
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev,
                                    dt_colorspaces_color_profile_type_t *profile_type,
                                    const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorin_so = NULL;
  dt_iop_module_t *colorin = NULL;
  for(const GList *modules = dt_iop_get_modules_so(); modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorin"))
    {
      colorin_so = module_so;
      break;
    }
  }
  if(colorin_so && colorin_so->get_p)
  {
    for(const GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorin"))
      {
        colorin = module;
        break;
      }
    }
  }
  if(colorin)
  {
    dt_colorspaces_color_profile_type_t *_type = colorin_so->get_p(colorin->params, "type_work");
    char *_filename = colorin_so->get_p(colorin->params, "filename_work");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't get colorin parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_work_profile_type] can't find colorin iop\n");
}

void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev,
                                      dt_colorspaces_color_profile_type_t *profile_type,
                                      const char **profile_filename)
{
  *profile_type = DT_COLORSPACE_NONE;
  *profile_filename = NULL;

  // use introspection to get the params values
  dt_iop_module_so_t *colorout_so = NULL;
  dt_iop_module_t *colorout = NULL;
  for(const GList *modules = g_list_last(dt_iop_get_modules_so()); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_so_t *module_so = (dt_iop_module_so_t *)(modules->data);
    if(!strcmp(module_so->op, "colorout"))
    {
      colorout_so = module_so;
      break;
    }
  }
  if(colorout_so && colorout_so->get_p)
  {
    for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      if(!strcmp(module->op, "colorout"))
      {
        colorout = module;
        break;
      }
    }
  }
  if(colorout)
  {
    dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
    char *_filename = colorout_so->get_p(colorout->params, "filename");
    if(_type && _filename)
    {
      *profile_type = *_type;
      *profile_filename = _filename;
    }
    else
      fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't get colorout parameters\n");
  }
  else
    fprintf(stderr, "[dt_ioppr_get_export_profile_type] can't find colorout iop\n");
}



#undef DT_IOP_ORDER_PROFILE
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
