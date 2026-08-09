/*
    This file is part of darktable,
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019-2021, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019 Marcus Rückert.
    Copyright (C) 2019-2020 Pascal Obry.
    Copyright (C) 2020-2021 Dan Torop.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2021 Heiko Bauke.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef DT_DEVELOP_IOP_PROFILE_H
#define DT_DEVELOP_IOP_PROFILE_H

/* The pipeline-facing half of the colour-profile module: which profile a module, pipe or
 * export should use, and transforming an image with it. Everything declared here takes a
 * develop/ type, which is exactly why it is here and not in colorprofiles/iop_profile.h. */

#include "colorprofiles/iop_profile.h"
#include "colorprofiles/colorspaces.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;

/** returns the profile info from dev profiles info list that matches (profile_type, profile_filename)
 * NULL if not found
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_profile_info_from_list(struct dt_develop_t *dev, dt_colorspaces_color_profile_type_t profile_type,
                                    const char *profile_filename);

/** adds the profile info from (profile_type, profile_filename) to the dev profiles info list if not already exists
 * returns the generated profile or the existing one
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev,
                                  const dt_colorspaces_color_profile_type_t profile_type,
                                  const char *profile_filename,
                                  const int intent);

/** returns a reference to the work profile info as set on colorin iop
 * only if module is between colorin and colorout, otherwise returns NULL
 * work profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_work_profile_info(struct dt_iop_module_t *module,
                                                                   GList *iop_list);

dt_iop_order_iccprofile_info_t *dt_ioppr_get_iop_input_profile_info(struct dt_iop_module_t *module,
                                                                    GList *iop_list);

/** set the work profile (type, filename) on the pipe, should be called on process*()
 * if matrix cannot be generated it default to linear rec 2020
 * returns the actual profile that has been set
 */
dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_work_profile_info(struct dt_develop_t *dev,
                                    struct dt_dev_pixelpipe_t *pipe,
                                    const dt_colorspaces_color_profile_type_t type,
                                    const char *filename,
                                    const int intent);

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_input_profile_info(struct dt_develop_t *dev,
                                     struct dt_dev_pixelpipe_t *pipe,
                                     const dt_colorspaces_color_profile_type_t type,
                                     const char *filename,
                                     const int intent, const dt_colormatrix_t matrix_in);

dt_iop_order_iccprofile_info_t *
dt_ioppr_set_pipe_output_profile_info(struct dt_develop_t *dev,
                                      struct dt_dev_pixelpipe_t *pipe,
                                      const dt_colorspaces_color_profile_type_t type,
                                      const char *filename,
                                      const int intent);

/** returns a reference to the histogram profile info
 * histogram profile must not be cleanup()
 */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_histogram_profile_info(struct dt_develop_t *dev);

/** returns the active work/input/output profile on the pipe */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_work_profile_info(const struct dt_dev_pixelpipe_t *pipe);

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_input_profile_info(const struct dt_dev_pixelpipe_t *pipe);

dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_output_profile_info(const struct dt_dev_pixelpipe_t *pipe);

/** Get the relevant RGB -> XYZ profile at the position of current module */
dt_iop_order_iccprofile_info_t *dt_ioppr_get_pipe_current_profile_info(struct dt_iop_module_t *module,
                                                                       const struct dt_dev_pixelpipe_t *pipe);

/** returns the current setting of the work profile on colorin iop */
void dt_ioppr_get_work_profile_type(struct dt_develop_t *dev,
                                    dt_colorspaces_color_profile_type_t *profile_type,
                                    const char **profile_filename);

/** returns the current setting of the input profile on colorin iop */
void dt_ioppr_get_input_profile_type(struct dt_develop_t *dev,
                                     dt_colorspaces_color_profile_type_t *profile_type,
                                     const char **profile_filename);

/** returns the current setting of the export profile on colorout iop */
void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev,
                                      dt_colorspaces_color_profile_type_t *profile_type,
                                      const char **profile_filename);

/** transforms image from cst_from to cst_to colorspace using profile_info */
void dt_ioppr_transform_image_colorspace(struct dt_iop_module_t *self, const float *const image_in,
                                         float *const image_out, const int width, const int height,
                                         const int cst_from, const int cst_to, int *converted_cst,
                                         const dt_iop_order_iccprofile_info_t *const profile_info);

#ifdef HAVE_OPENCL


/** same as the C version */
int dt_ioppr_transform_image_colorspace_cl(struct dt_iop_module_t *self, const int devid, cl_mem dev_img_in,
                                           cl_mem dev_img_out, const int width, const int height,
                                           const int cst_from, const int cst_to, int *converted_cst,
                                           const dt_iop_order_iccprofile_info_t *const profile_info);
#endif // HAVE_OPENCL

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_IOP_PROFILE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
