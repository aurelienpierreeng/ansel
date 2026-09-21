/*
    This file is part of darktable,
    Copyright (C) 2017 Edgardo Hoszowski.
    Copyright (C) 2020, 2022 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
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

#ifndef DT_PIXEL_HEAL_H
#define DT_PIXEL_HEAL_H

/** The domain the source-to-destination difference is interpolated in.
 *
 * LINEAR adds a harmonic correction to the source pixels as they are, so a source brighter than
 * the destination keeps its absolute noise and texture amplitude on a darker base: once encoded
 * for display, that noise is stronger than the destination's own.
 *
 * SQRT interpolates the difference of square roots, the variance-stabilising transform of shot
 * noise, so the pasted noise takes the amplitude it would have had at the destination level. Only
 * the three colour channels are transformed; the fourth stays linear. */
typedef enum dt_heal_domain_t
{
  DT_HEAL_DOMAIN_LINEAR = 0,
  DT_HEAL_DOMAIN_SQRT = 1
} dt_heal_domain_t;

/** Heal dest_buffer using src_buffer as a reference and mask_buffer to define the area to be healed.
 *  The 3 buffers must have the same size, but mask_buffer is 1 channel and is tested for != 0.f.
 *  domain selects the space the source-to-destination difference is interpolated in. */
void dt_heal(const float *const src_buffer, float *dest_buffer, const float *const mask_buffer, const int width,
             const int height, const int ch, const int max_iter, const dt_heal_domain_t domain);

#ifdef HAVE_OPENCL

typedef struct dt_heal_cl_global_t
{
  int kernel_dummy;
} dt_heal_cl_global_t;

typedef struct heal_params_cl_t
{
  dt_heal_cl_global_t *global;
  int devid;
} heal_params_cl_t;

void dt_heal_init_cl_global(void);
void dt_heal_free_cl_global(void);

heal_params_cl_t *dt_heal_init_cl(const int devid);
void dt_heal_free_cl(heal_params_cl_t *p);

cl_int dt_heal_cl(heal_params_cl_t *p, cl_mem dev_src, cl_mem dev_dest, const float *const mask_buffer,
                  const int width, const int height, const int max_iter, const dt_heal_domain_t domain);

#endif
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
