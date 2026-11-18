/*
    This file is part of the Ansel project.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020, 2022 Pascal Obry.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2020 Tobias Ellinghaus.
    Copyright (C) 2021 Chris Elston.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023, 2025-2026 Guillaume Stutin.
    Copyright (C) 2023 Luca Zulberti.
    
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
#include "common/conf.h"
#endif

#include "widgets/bauhaus.h"
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/iop_profile.h"
#include "math/openmp_maths.h"

#include "gui/presets.h"
#include "gui/bauhaus_conf.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include "widgets/label.h"
#include "widgets/notebook.h"
#include "widgets/togglebutton.h"

/** DOCUMENTATION
 *
 * This module allows to invert scanned negatives and simulate their print on paper, based on Kodak Cineon
 * densitometry algorithm. It is better than the old invert module because it takes into account the Dmax of the film
 * and allows white balance adjustments, as well as paper grade (gamma) simulation. It also allows density correction
 * in log space, to account for the exposure settings of the scanner. Finally, it is applied after input colour profiling,
 * which means the inversion happens after the scanner or the camera got color-corrected, while the old invert module
 * invert the RAW, non-demosaiced, file before any colour correction.
 *
 * References :
 *
 *  - https://www.kodak.com/uploadedfiles/motion/US_plugins_acrobat_en_motion_education_sensitometry_workbook.pdf
 *  - http://www.digital-intermediate.co.uk/film/pdf/Cineon.pdf
 *  - https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
 **/

 #define THRESHOLD 2.3283064365386963e-10f // -32 EV


DT_MODULE_INTROSPECTION(3, dt_iop_negadoctor_params_t)


typedef enum dt_iop_negadoctor_filmstock_t
{
  // What kind of emulsion are we working on ?
  DT_FILMSTOCK_NB = 0,   // $DESCRIPTION: "black and white film"
  DT_FILMSTOCK_COLOR = 1 // $DESCRIPTION: "color film"
} dt_iop_negadoctor_filmstock_t;

// How the shadows color cast moves the black point of each channel
typedef enum dt_iop_negadoctor_version_t
{
  DT_NEGADOCTOR_V1 = 0, // $DESCRIPTION: "v1 (shadows scale the bias)"
  DT_NEGADOCTOR_V2 = 1  // $DESCRIPTION: "v2 (a black point per channel)"
} dt_iop_negadoctor_version_t;


typedef struct dt_iop_negadoctor_params_t
{
  dt_iop_negadoctor_filmstock_t film_stock; /* $DEFAULT: DT_FILMSTOCK_COLOR $DESCRIPTION: "film stock" */
  float Dmin[4];                            /* color of film substrate
                                               $MIN: 0.00001 $MAX: 1.5 $DEFAULT: 1.0 */
  float wb_high[4];                         /* white balance RGB coeffs (illuminant)
                                               $MIN: 0.05 $MAX: 2 $DEFAULT: 1.0 */
  float wb_low[4];                          /* white balance RGB offsets (base light)
                                               $MIN: 0.05 $MAX: 2 $DEFAULT: 1.0 */
  float D_max;                              /* max density of film
                                               $MIN: 0.1 $MAX: 6 $DEFAULT: 2.046 */
  float offset;                             /* inversion offset in v1; in v2, which has no use for it, the saturation of
                                               the print relative to the paper grade
                                               $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "film exposure bias" */
  float black;                              /* display black level
                                               $MIN: -0.5 $MAX: 0.5 $DEFAULT: 0.0755 $DESCRIPTION: "paper black (density correction)" */
  float gamma;                              /* display gamma
                                               $MIN: 1.0 $MAX: 20.0 $DEFAULT: 4.0 $DESCRIPTION: "paper grade (gamma)" */
  float soft_clip;                          /* highlights roll-off
                                               $MIN: 0.0001 $MAX: 1.0 $DEFAULT: 0.75 $DESCRIPTION: "paper gloss (specular highlights)" */
  float exposure;                           /* extra exposure
                                               $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.9245 $DESCRIPTION: "print exposure adjustment" */
  dt_iop_negadoctor_version_t version;      /* $DEFAULT: DT_NEGADOCTOR_V2 $DESCRIPTION: "version" */
  float black_point[4];                     /* v2 density of the blacks above the film base in each channel, as a fraction
                                               of D max: the bias and the shadows color cast in one
                                               $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 */
} dt_iop_negadoctor_params_t;


typedef struct dt_iop_negadoctor_data_t
{
  dt_aligned_pixel_t Dmin;                // color of film substrate
  dt_aligned_pixel_t wb_high;             // white balance RGB coeffs / Dmax
  dt_aligned_pixel_t offset;              // inversion offset
  float black;                            // display black level
  float gamma;                            // display gamma
  float color_gamma;                      // power of the channels' ratios to the luminance, gamma unless v2 saturates
  float soft_clip;                        // highlights roll-off
  float soft_clip_comp;                   // 1 - softclip, complement to 1
  float exposure;                         // extra exposure
} dt_iop_negadoctor_data_t;


typedef struct dt_iop_negadoctor_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *film_stock;
  GtkWidget *Dmin_R, *Dmin_G, *Dmin_B;
  GtkWidget *wb_high_R, *wb_high_G, *wb_high_B;
  GtkWidget *wb_low_R, *wb_low_G, *wb_low_B;
  GtkWidget *black_point_R, *black_point_G, *black_point_B, *black_point_row, *black_point_sampler;
  GtkWidget *density_unit;
  GtkWidget *version;
  GtkWidget *D_max;
  GtkWidget *offset;
  GtkWidget *black, *gamma, *saturation, *soft_clip, *exposure, *auto_print, *auto_print_label;
  GtkWidget *Dmin_picker, *Dmin_sampler;
  GtkWidget *WB_high_picker, *WB_high_norm, *WB_high_sampler, *WB_high_row;
  GtkWidget *WB_low_picker, *WB_low_norm, *WB_low_sampler, *WB_low_row, *WB_low_section;
  // v2: the two neutrals the contrast of the layers is measured between, then the one that sets the color alone
  GtkWidget *neutral_sampler[3], *neutral_row[3];
  // what each of the two picked last, until the other is picked too
  dt_aligned_pixel_t neutral[2];
  gboolean neutral_picked[2];
  // under the two neutrals: which one is still to pick, how far apart they are, or why they cannot be used
  GtkWidget *neutrals_status;
  // after the black point picker: the densities above the film base of the pixel it took
  GtkWidget *black_point_density;
  dt_aligned_pixel_t black_point_pixel;
  gboolean black_point_picked;
} dt_iop_negadoctor_gui_data_t;


typedef struct dt_iop_negadoctor_global_data_t
{
  int kernel_negadoctor;
} dt_iop_negadoctor_global_data_t;


const char *name()
{
  return _("ne_gadoctor");
}

const char *aliases()
{
  return _("film|invert|negative|scan");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("invert film negative scans and simulate printing on paper"),
                                      _("corrective and creative"),
                                      _("linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}


int default_group()
{
  return IOP_GROUP_FILM;
}


int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  typedef struct dt_iop_negadoctor_params_v2_t
  {
    dt_iop_negadoctor_filmstock_t film_stock;
    float Dmin[4];
    float wb_high[4];
    float wb_low[4];
    float D_max;
    float offset;
    float black;
    float gamma;
    float soft_clip;
    float exposure;
  } dt_iop_negadoctor_params_v2_t;

  if(new_version != 3 || (old_version != 1 && old_version != 2)) return 1;

  dt_iop_negadoctor_params_t *n = (dt_iop_negadoctor_params_t *)new_params;
  const dt_iop_negadoctor_params_t *d = (dt_iop_negadoctor_params_t *)self->default_params;
  *n = *d; // start with a fresh copy of default parameters

  // version 3 appends to version 2
  if(old_version == 2) memcpy(n, old_params, sizeof(dt_iop_negadoctor_params_v2_t));

  if(old_version == 1)
  {
    typedef struct dt_iop_negadoctor_params_v1_t
    {
      dt_iop_negadoctor_filmstock_t film_stock;
      dt_aligned_pixel_t Dmin;                // color of film substrate
      dt_aligned_pixel_t wb_high;             // white balance RGB coeffs (illuminant)
      dt_aligned_pixel_t wb_low;              // white balance RGB offsets (base light)
      float D_max;                            // max density of film
      float offset;                           // inversion offset
      float black;                            // display black level
      float gamma;                            // display gamma
      float soft_clip;                        // highlights roll-off
      float exposure;                         // extra exposure
    } dt_iop_negadoctor_params_v1_t;

    const dt_iop_negadoctor_params_v1_t *o = (dt_iop_negadoctor_params_v1_t *)old_params;

    // WARNING: when copying the arrays in a for loop, gcc wrongly assumed
    //          that n and o were aligned and used AVX instructions for me,
    //          which segfaulted. let's hope this doesn't get optimized too much.
    n->film_stock = o->film_stock;
    n->Dmin[0] = o->Dmin[0];
    n->Dmin[1] = o->Dmin[1];
    n->Dmin[2] = o->Dmin[2];
    n->Dmin[3] = o->Dmin[3];
    n->wb_high[0] = o->wb_high[0];
    n->wb_high[1] = o->wb_high[1];
    n->wb_high[2] = o->wb_high[2];
    n->wb_high[3] = o->wb_high[3];
    n->wb_low[0] = o->wb_low[0];
    n->wb_low[1] = o->wb_low[1];
    n->wb_low[2] = o->wb_low[2];
    n->wb_low[3] = o->wb_low[3];
    n->D_max = o->D_max;
    n->offset = o->offset;
    n->black = o->black;
    n->gamma = o->gamma;
    n->soft_clip = o->soft_clip;
    n->exposure = o->exposure;
  }

  // older edits keep the shadows color cast that scales the bias, whose black points v2 would start from
  n->version = DT_NEGADOCTOR_V1;
  for(int c = 0; c < 4; c++) n->black_point[c] = n->offset * n->wb_low[c];
  return 0;
}

// The density each channel prints at the paper black, as a fraction of D max: in v1 the bias, which the
// shadows color cast scales; in v2 one black point per channel, which holds both.
static inline float _black_point(const dt_iop_negadoctor_params_t *const p, const int c)
{
  return (p->version == DT_NEGADOCTOR_V1) ? p->offset * p->wb_low[c] : p->black_point[c];
}

// The coefficients process() applies. The pickers solve against these same ones, so what they set is what
// the image shows.
static void _data_from_params(const dt_iop_negadoctor_params_t *const p, dt_iop_negadoctor_data_t *const d)
{
  // keep WB_high even in B&W mode to apply sepia or warm tone look
  // but premultiply it aheard with Dmax to spare one div per pixel
  for(size_t c = 0; c < 4; c++) d->wb_high[c] = p->wb_high[c] / p->D_max;

  for(size_t c = 0; c < 4; c++) d->offset[c] = p->wb_high[c] * _black_point(p, c);

  // ensure we use a monochrome Dmin for B&W film
  for(size_t c = 0; c < 4; c++)
  {
    const size_t mono = (p->film_stock == DT_FILMSTOCK_NB) ? 0 : c;
    d->Dmin[c] = p->Dmin[mono];
  }

  // arithmetic trick allowing to rewrite the pixel inversion as FMA
  d->black = -p->exposure * (1.0f + p->black);

  // highlights soft clip
  d->soft_clip = p->soft_clip;
  d->soft_clip_comp = 1.0f - p->soft_clip;

  // copy
  d->exposure = p->exposure;
  d->gamma = p->gamma;

  // v2 keeps its saturation in the field v1 uses for the bias: 0 raises the channels' ratios to the grade, as
  // v1 always does, -1 drops them, +1 doubles their power
  d->color_gamma = (p->version == DT_NEGADOCTOR_V2) ? p->gamma * (1.0f + p->offset) : p->gamma;
}

// The luminance weights of the pipe's working RGB, Rec. 2020's when it is not a matrix profile
static void _working_luminance(const dt_dev_pixelpipe_t *const pipe, dt_aligned_pixel_t luminance)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const gboolean matrix = !IS_NULL_PTR(work_profile) && !isnan(work_profile->matrix_in[0][0]);
  const dt_aligned_pixel_t rec2020 = { 0.2627002f, 0.6779981f, 0.0593017f, 0.0f };
  for(int c = 0; c < 3; c++) luminance[c] = matrix ? work_profile->matrix_in[1][c] : rec2020[c];
  luminance[3] = 0.0f;
}

// The corrected density process() gives value v in channel c
static inline float _corrected_density(const dt_iop_negadoctor_data_t *const d, const int c, const float v)
{
  return d->wb_high[c] * -log10f(d->Dmin[c] / fmaxf(v, THRESHOLD)) + d->offset[c];
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  _data_from_params((const dt_iop_negadoctor_params_t *)p1, (dt_iop_negadoctor_data_t *)piece->data);
}


__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *const self, const dt_dev_pixelpipe_t *const pipe,
            const dt_dev_pixelpipe_iop_t *const piece, const void *const restrict ivoid,
            void *const restrict ovoid)
{
  const dt_iop_roi_t *const restrict roi_out = &piece->roi_out;
  const dt_iop_negadoctor_data_t *const d = piece->data;
  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;
  // The paper grade raises the luminance of the print to its power, and the ratios of the channels to the
  // luminance to color_gamma: at color_gamma = gamma this is each channel raised to gamma.
  const gboolean saturate = (d->color_gamma != d->gamma);
  dt_aligned_pixel_t luminance;
  _working_luminance(pipe, luminance);

  __OMP_FOR_SIMD__(aligned(in, out:64))
  for(size_t k = 0; k < (size_t)roi_out->height * roi_out->width * 4; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    const float *const restrict Dmin = __builtin_assume_aligned(d->Dmin, 16);
    const float *const restrict wb_high = __builtin_assume_aligned(d->wb_high, 16);
    const float *const restrict offset = __builtin_assume_aligned(d->offset, 16);

    dt_aligned_pixel_t print_linear;
    for(size_t c = 0; c < 4; c++)
    {
      // Convert transmission to density using Dmin as a fulcrum
      const float density = - log10f(Dmin[c] / fmaxf(pix_in[c], THRESHOLD)); // threshold to -32 EV

      // Correct density in log space
      const float corrected_de = wb_high[c] * density + offset[c];

      // Print density on paper : ((1 - 10^corrected_de + black) * exposure)^gamma rewritten for FMA
      print_linear[c] = fmaxf(-(d->exposure * fast_exp10f(corrected_de) + d->black), 0.0f);
    }

    const float grey = luminance[0] * print_linear[0] + luminance[1] * print_linear[1] + luminance[2] * print_linear[2];
    const float grey_gamma = powf(grey, d->gamma);

    for(size_t c = 0; c < 4; c++)
    {
      // note : this is always >= 0
      const float print_gamma = !saturate ? powf(print_linear[c], d->gamma)
                                : (grey > 0.0f) ? grey_gamma * powf(print_linear[c] / grey, d->color_gamma)
                                                : 0.0f;

      // Compress highlights. from https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
      pix_out[c] =  (print_gamma > d->soft_clip) ? d->soft_clip + (1.0f - fast_expf(-(print_gamma - d->soft_clip) / d->soft_clip_comp)) * d->soft_clip_comp
                                                 : print_gamma;
    }
  }

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *const self, const dt_dev_pixelpipe_t *const pipe,
               const dt_dev_pixelpipe_iop_t *const piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const restrict roi_in = &piece->roi_in;
  const dt_iop_negadoctor_data_t *const d = (dt_iop_negadoctor_data_t *)piece->data;
  const dt_iop_negadoctor_global_data_t *const gd = (dt_iop_negadoctor_global_data_t *)self->global_data;

  cl_int err = -999;

  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_aligned_pixel_t luminance;
  _working_luminance(pipe, luminance);

  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 4, 4 * sizeof(float), (void *)&d->Dmin);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 5, 4 * sizeof(float), (void *)&d->wb_high);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 6, 4 * sizeof(float), (void *)&d->offset);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 7, sizeof(float), (void *)&d->exposure);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 8, sizeof(float), (void *)&d->black);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 9, sizeof(float), (void *)&d->gamma);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 10, sizeof(float), (void *)&d->soft_clip);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 11, sizeof(float), (void *)&d->soft_clip_comp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 12, 4 * sizeof(float), (void *)&luminance);
  dt_opencl_set_kernel_arg(devid, gd->kernel_negadoctor, 13, sizeof(float), (void *)&d->color_gamma);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_negadoctor, sizes);
  if(err != CL_SUCCESS) goto error;
    return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_negadoctor] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_negadoctor_params_t *d = module->default_params;

  d->Dmin[0] = 1.00f;
  d->Dmin[1] = 0.45f;
  d->Dmin[2] = 0.25f;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_negadoctor_params_t tmp = (dt_iop_negadoctor_params_t){ .film_stock = DT_FILMSTOCK_COLOR,
                                                                 .Dmin = { 1.13f, 0.49f, 0.27f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 1.6f,
                                                                 .offset = 0.0f,
                                                                 .gamma = 4.0f,
                                                                 .soft_clip = 0.75f,
                                                                 .exposure = 0.9245f,
                                                                 .black = 0.0755f,
                                                                 .version = DT_NEGADOCTOR_V2 };


  dt_gui_presets_add_generic(_("color film"), self->op,
                             self->version(), &tmp, sizeof(tmp), 1);

  dt_iop_negadoctor_params_t tmq = (dt_iop_negadoctor_params_t){ .film_stock = DT_FILMSTOCK_NB,
                                                                 .Dmin = { 1.0f, 1.0f, 1.0f, 0.0f},
                                                                 .wb_high = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .wb_low = { 1.0f, 1.0f, 1.0f, 0.0f },
                                                                 .D_max = 2.2f,
                                                                 .offset = 0.0f,
                                                                 .gamma = 5.0f,
                                                                 .soft_clip = 0.75f,
                                                                 .exposure = 1.f,
                                                                 .black = 0.0755f,
                                                                 .version = DT_NEGADOCTOR_V2 };


  dt_gui_presets_add_generic(_("black and white film"), self->op,
                             self->version(), &tmq, sizeof(tmq), 1);
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_negadoctor_global_data_t *gd
      = (dt_iop_negadoctor_global_data_t *)calloc(1, sizeof(dt_iop_negadoctor_global_data_t));
  if(IS_NULL_PTR(gd)) return;

  module->data = gd;
  const int program = 30; // negadoctor.cl, from programs.conf
  gd->kernel_negadoctor = dt_opencl_create_kernel(program, "negadoctor");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_negadoctor_global_data_t *gd = module->data;
  dt_opencl_free_kernel(gd->kernel_negadoctor);
  dt_free(module->data);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_negadoctor_data_t));
  piece->data_size = sizeof(dt_iop_negadoctor_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}


/* Global GUI stuff */

static void setup_color_variables(dt_iop_negadoctor_gui_data_t *const g, const gint state)
{
  gtk_widget_set_visible(g->Dmin_G, state);
  gtk_widget_set_visible(g->Dmin_B, state);
}


static void toggle_stock_controls(dt_iop_module_t *const self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  if(p->film_stock == DT_FILMSTOCK_NB)
  {
    // Hide color controls
    setup_color_variables(g, FALSE);
    dt_bauhaus_widget_set_label(g->Dmin_R, N_("D min"));
  }
  else if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    // Show color controls
    setup_color_variables(g, TRUE);
    dt_bauhaus_widget_set_label(g->Dmin_R, N_("D min red component"));
  }
  else
  {
    // We shouldn't be there
    fprintf(stderr, "negadoctor film stock: undefined behaviour\n");
  }
}


static void Dmin_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  if(p->film_stock == DT_FILMSTOCK_COLOR)
  {
    color.red = p->Dmin[0];
    color.green = p->Dmin[1];
    color.blue = p->Dmin[2];
  }
  else if(p->film_stock == DT_FILMSTOCK_NB)
  {
    color.red = color.green = color.blue = p->Dmin[0];
  }

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->Dmin_picker), &color);
}

static void Dmin_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->Dmin[0] = c.red;
  p->Dmin[1] = c.green;
  p->Dmin[2] = c.blue;

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0]);
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1]);
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2]);
  dt_gui_freeze_end();

  Dmin_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// Show the black points of both versions in their sliders, without going through their callbacks
static void _black_point_sliders_set(dt_iop_negadoctor_gui_data_t *const g, const dt_iop_negadoctor_params_t *const p)
{
  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->offset, p->offset);
  dt_bauhaus_slider_set(g->saturation, p->offset);
  dt_bauhaus_slider_set(g->wb_low_R, p->wb_low[0]);
  dt_bauhaus_slider_set(g->wb_low_G, p->wb_low[1]);
  dt_bauhaus_slider_set(g->wb_low_B, p->wb_low[2]);
  dt_bauhaus_slider_set(g->black_point_R, p->black_point[0]);
  dt_bauhaus_slider_set(g->black_point_G, p->black_point[1]);
  dt_bauhaus_slider_set(g->black_point_B, p->black_point[2]);
  dt_gui_freeze_end();
}

// The bias and the v2 black points are stored as fractions of D max, and their sliders show densities in the
// unit the user chose: D, or printer points (0.025 log exposure). process() multiplies the light through the
// negative by 10^(black point in D), as a printer's lamp does, so a black point is a printing log exposure.
static void _density_sliders_update(dt_iop_negadoctor_gui_data_t *const g, const dt_iop_negadoctor_params_t *const p)
{
  static const float factors[2] = { 1.0f, 40.0f };
  static const int digits[2] = { 3, 1 };
  static const char *const formats[2] = { " D", N_(" pt") };

  const int u = !g_strcmp0(dt_conf_get_string_const("plugins/darkroom/negadoctor/density_unit"), "printer points");

  GtkWidget *const sliders[4] = { g->offset, g->black_point_R, g->black_point_G, g->black_point_B };
  for(int k = 0; k < 4; k++)
  {
    dt_bauhaus_slider_set_factor(sliders[k], p->D_max * factors[u]);
    dt_bauhaus_slider_set_digits(sliders[k], digits[u]);
    dt_bauhaus_slider_set_format(sliders[k], _(formats[u]));
    gtk_widget_queue_draw(sliders[k]);
  }
}

// The densities above the film base of the pixel the black point picker took, in the unit of the black points.
// The pixel is kept rather than its densities, so a film base picked afterwards changes them.
static void _black_point_density_update(dt_iop_negadoctor_gui_data_t *const g, const dt_iop_negadoctor_params_t *const p)
{
  if(!g->black_point_picked)
  {
    gtk_label_set_text(GTK_LABEL(g->black_point_density), "");
    return;
  }

  const gboolean points = !g_strcmp0(dt_conf_get_string_const("plugins/darkroom/negadoctor/density_unit"), "printer points");
  dt_aligned_pixel_t D;
  for(int c = 0; c < 3; c++)
    D[c] = log10f(p->Dmin[c] / fmaxf(g->black_point_pixel[c], THRESHOLD)) * (points ? 40.0f : 1.0f);

  gchar *text = points ? g_strdup_printf(_("%.1f / %.1f / %.1f pt"), D[0], D[1], D[2])
                             : g_strdup_printf(_("%.3f / %.3f / %.3f D"), D[0], D[1], D[2]);
  gtk_label_set_text(GTK_LABEL(g->black_point_density), text);
  dt_free(text);
}

static void _density_unit_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (const dt_iop_negadoctor_params_t *)self->params;
  _density_sliders_update(g, p);
  _black_point_density_update(g, p);
}

static void WB_low_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  dt_aligned_pixel_t WB_low_invert;
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] = 2.0f - p->wb_low[c];
  const float WB_low_max = v_maxf(WB_low_invert);
  for(size_t c = 0; c < 3; ++c) WB_low_invert[c] /= WB_low_max;

  color.red = WB_low_invert[0];
  color.green = WB_low_invert[1];
  color.blue = WB_low_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_low_picker), &color);
}

static void WB_low_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  dt_aligned_pixel_t RGB = { 2.0f - c.red, 2.0f - c.green, 2.0f - c.blue };
  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_low[k] = RGB[k] / RGB_min;

  _black_point_sliders_set(g, p);

  WB_low_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}


static void WB_high_picker_update(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  GdkRGBA color;
  color.alpha = 1.0f;

  dt_aligned_pixel_t WB_high_invert;
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] = 2.0f - p->wb_high[c];
  const float WB_high_max = v_maxf(WB_high_invert);
  for(size_t c = 0; c < 3; ++c) WB_high_invert[c] /= WB_high_max;

  color.red = WB_high_invert[0];
  color.green = WB_high_invert[1];
  color.blue = WB_high_invert[2];

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->WB_high_picker), &color);
}

static void WB_high_picker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);

  dt_aligned_pixel_t RGB = { 2.0f - c.red, 2.0f - c.green, 2.0f - c.blue };
  float RGB_min = v_minf(RGB);
  for(size_t k = 0; k < 3; k++) p->wb_high[k] = RGB[k] / RGB_min;

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  dt_gui_freeze_end();

  WB_high_picker_update(self);
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void Wb_low_norm_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;

  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;


  const float WB_low_max = v_maxf(p->wb_low);
  for(size_t c = 0; c < 3; ++c)
    p->wb_low[c] /= WB_low_max;

  _black_point_sliders_set(g, p);

  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void Wb_high_norm_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;

  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  const float WB_high_min = v_minf(p->wb_high);
  for(size_t c = 0; c < 3; ++c)
    p->wb_high[c] /= WB_high_min;


  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  dt_gui_freeze_end();

  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/* Color pickers auto-tuners */

// measure Dmin from the film edges first
static void apply_auto_Dmin(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  for(int k = 0; k < 4; k++) p->Dmin[k] = self->picked_color[k];

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->Dmin_R, p->Dmin[0]);
  dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[1]);
  dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[2]);
  dt_gui_freeze_end();

  Dmin_picker_update(self);
  _black_point_density_update(g, p);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// The scale on which the black point and paper black pickers look for the pixel high in all three channels,
// a channel's score being gain · log10(pixel) + bias: the corrected density process() prints, so the winner
// is the pixel the print makes dark in all channels.
static void _picker_channel_scale(const dt_iop_module_t *self, GtkWidget *picker, dt_aligned_pixel_t gain,
                                  dt_aligned_pixel_t bias)
{
  dt_iop_negadoctor_data_t d;
  _data_from_params((const dt_iop_negadoctor_params_t *)self->params, &d);

  for(int c = 0; c < 3; c++)
  {
    // -log10(Dmin / pixel) = log10(pixel) - log10(Dmin), then as _corrected_density()
    gain[c] = d.wb_high[c];
    bias[c] = d.offset[c] - gain[c] * log10f(d.Dmin[c]);
  }
}

// The channel v_maxf() / v_minf() return, which the darkroom marks as the one that decided.
static inline int _channel_of_max(const dt_aligned_pixel_t RGB)
{
  return (RGB[0] >= RGB[1]) ? ((RGB[0] >= RGB[2]) ? 0 : 2) : ((RGB[1] >= RGB[2]) ? 1 : 2);
}

static inline int _channel_of_min(const dt_aligned_pixel_t RGB)
{
  return (RGB[0] <= RGB[1]) ? ((RGB[0] <= RGB[2]) ? 0 : 2) : ((RGB[1] <= RGB[2]) ? 1 : 2);
}

// from Dmin, find out the range of density values of the film and compute Dmax
static void apply_auto_Dmax(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = log10f(d.Dmin[c] / fmaxf(self->picked_color_min[c], THRESHOLD));
  }

  // Take the max(RGB) for safety. Big values unclip whites
  p->D_max = v_maxf(RGB);
  dt_iop_color_picker_set_deciding_channel(self, _channel_of_max(RGB));

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->D_max, p->D_max);
  dt_gui_freeze_end();

  _density_sliders_update(g, p);
  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// from Dmax, compute the offset so the range of density is rescaled between [0; 1]
static void apply_auto_offset(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
    RGB[c] = log10f(d.Dmin[c] / fmaxf(self->picked_color_max[c], THRESHOLD)) / p->D_max;

  // Take the min(RGB) for safety. Negative values unclip blacks
  p->offset = v_minf(RGB);
  dt_iop_color_picker_set_deciding_channel(self, _channel_of_min(RGB));

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->offset, p->offset);
  dt_gui_freeze_end();

  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// v2: the level of the black points, which keeps the layer contrasts and the color the neutrals set. The pixel
// is the one the print makes dark in all three channels: the darkest print of one channel can be a saturated
// color instead, a red object being clear in blue and green on the film but dense in red.
static void apply_auto_black_point(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  // The level of the blacks alone: the pixel the print makes dark in all channels prints at the paper black in
  // the mean of its channels, and the three black points move by the same corrected density, so a neutral stays
  // neutral. Without the pixel itself, each channel's darkest print stands in for it.
  dt_aligned_pixel_t pixel;
  const gboolean located
      = !dt_iop_color_picker_get_extreme_pixel(self, DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN, 0, pixel);
  float corrected = 0.0f;
  for(int c = 0; c < 3; c++)
  {
    g->black_point_pixel[c] = located ? pixel[c] : self->picked_color_max[c];
    corrected += _corrected_density(&d, c, g->black_point_pixel[c]) / 3.0f;
  }
  g->black_point_picked = TRUE;
  _black_point_density_update(g, p);

  for(int c = 0; c < 3; c++)
    p->black_point[c] = CLAMPF(p->black_point[c] - corrected / p->wb_high[c],
                               dt_bauhaus_slider_get_hard_min(g->black_point_R),
                               dt_bauhaus_slider_get_hard_max(g->black_point_R));

  _black_point_sliders_set(g, p);

  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// from Dmax and offset, compute the white balance correction as multipliers of the offset
// such that offset x wb[c] make black monochrome
static void apply_auto_WB_low(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  dt_aligned_pixel_t RGB_min;
  for(int c = 0; c < 3; c++)
    RGB_min[c] = log10f(d.Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max;

  const float RGB_v_min = v_minf(RGB_min); // warning: can be negative
  for(int c = 0; c < 3; c++) p->wb_low[c] =  RGB_v_min / RGB_min[c];

  _black_point_sliders_set(g, p);

  WB_low_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// from Dmax, offset and white balance multipliers, compute the white balance of the illuminant as multipliers of 1/Dmax
// such that WB[c] / Dmax make white monochrome
static void apply_auto_WB_high(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  dt_aligned_pixel_t RGB_min;
  for(int c = 0; c < 3; c++)
    RGB_min[c] = fabsf(-1.0f / (_black_point(p, c) - log10f(d.Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max));

  const float RGB_v_min = v_minf(RGB_min); // warning : must be positive
  for(int c = 0; c < 3; c++) p->wb_high[c] = RGB_min[c] / RGB_v_min;

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  dt_gui_freeze_end();

  WB_high_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// The two neutrals must be this far apart in density above the film base, in every channel, to measure a slope
#define NEGADOCTOR_NEUTRALS_MIN_SPAN 0.05f

// The line under the two neutral pickers, shown only when it says something, and with the pickers
static void _neutrals_status_show(dt_iop_negadoctor_gui_data_t *const g)
{
  const char *const text = gtk_label_get_text(GTK_LABEL(g->neutrals_status));
  gtk_widget_set_visible(g->neutrals_status, text[0] != '\0' && gtk_widget_get_visible(g->neutral_row[0]));
}

static void _neutrals_status_set(dt_iop_negadoctor_gui_data_t *const g, const char *const text)
{
  gtk_label_set_text(GTK_LABEL(g->neutrals_status), text);
  _neutrals_status_show(g);
}

// v2: the white balance of the frame, the color of the black points from one neutral, which keeps the layer
// contrasts and, in every pixel, the mean over the channels of the corrected density, so the level of the
// blacks: each black point moves by δ[c] with the mean of wb_high[c] · δ[c] at zero, and this is what the
// neutral's own mean does. The neutral of densities D[c] above the film base then prints at that mean in every
// channel: wb_high[c] · (black_point[c] − D[c] / D_max) = corrected.
static void apply_auto_neutral(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  float corrected = 0.0f;
  for(int c = 0; c < 3; c++) corrected += _corrected_density(&d, c, self->picked_color[c]) / 3.0f;
  for(int c = 0; c < 3; c++)
    p->black_point[c] = CLAMPF(log10f(d.Dmin[c] / fmaxf(self->picked_color[c], THRESHOLD)) / p->D_max
                                   + corrected / p->wb_high[c],
                               dt_bauhaus_slider_get_hard_min(g->black_point_R),
                               dt_bauhaus_slider_get_hard_max(g->black_point_R));

  _black_point_sliders_set(g, p);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// v2: from two neutrals of different tones, the contrast of each layer, and nothing else. A cast of the scene's
// light adds the same density to both in a channel, so it cancels in their difference, which leaves the slope
// of the layer: a property of the film, its development and the scan. The black points are the frame's: the
// gains pivot on them, so a frame shows the cast of its light until the white balance picker takes it out.
static void apply_auto_neutrals(dt_iop_module_t *self, const int which)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  for(int c = 0; c < 4; c++) g->neutral[which][c] = self->picked_color[c];
  g->neutral_picked[which] = TRUE;
  if(!g->neutral_picked[1 - which])
  {
    _neutrals_status_set(g, which == 0 ? _("now pick the lighter neutral") : _("now pick the darker neutral"));
    return;
  }

  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  // densities above the film base, the clearer neutral first
  dt_aligned_pixel_t D[2];
  for(int k = 0; k < 2; k++)
    for(int c = 0; c < 3; c++) D[k][c] = log10f(d.Dmin[c] / fmaxf(g->neutral[k][c], THRESHOLD));
  const int clear = (D[0][0] + D[0][1] + D[0][2] <= D[1][0] + D[1][1] + D[1][2]) ? 0 : 1;
  const float *const D_clear = D[clear];
  const float *const D_dense = D[1 - clear];

  dt_aligned_pixel_t span = { 0.0f };
  for(int c = 0; c < 3; c++) span[c] = D_dense[c] - D_clear[c];
  const char *const channels[3] = { _("red"), _("green"), _("blue") };
  int closest = 0;
  for(int c = 1; c < 3; c++)
    if(span[c] < span[closest]) closest = c;
  if(span[closest] < NEGADOCTOR_NEUTRALS_MIN_SPAN)
  {
    gchar *text = g_strdup_printf(_("the two neutrals are too close in density to measure the contrast "
                                          "of the layers: %.3f D apart in %s, at least %.2f D needed"),
                                        span[closest], channels[closest], NEGADOCTOR_NEUTRALS_MIN_SPAN);
    _neutrals_status_set(g, text);
    dt_free(text);
    return;
  }

  gchar *text = g_strdup_printf(_("the neutrals are %.3f / %.3f / %.3f D apart"), span[0], span[1], span[2]);
  _neutrals_status_set(g, text);
  dt_free(text);

  const float span_max = v_maxf(span);
  for(int c = 0; c < 3; c++)
    p->wb_high[c] = CLAMPF(span_max / span[c], dt_bauhaus_slider_get_hard_min(g->wb_high_R),
                           dt_bauhaus_slider_get_hard_max(g->wb_high_R));

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->wb_high_R, p->wb_high[0]);
  dt_bauhaus_slider_set(g->wb_high_G, p->wb_high[1]);
  dt_bauhaus_slider_set(g->wb_high_B, p->wb_high[2]);
  dt_gui_freeze_end();

  WB_high_picker_update(self);
  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// from Dmax, offset and both white balances, compute the print black adjustment
// such that the printed values range from 0 to + infinity
static void apply_auto_black(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  // The pixel the print makes dark in all three channels: the per-channel maxima can each sit on a
  // saturated color, clear on the film in one channel only, whose darkest channel would lift the blacks.
  // Without the pixel itself, fall back to those maxima.
  dt_aligned_pixel_t pixel;
  if(dt_iop_color_picker_get_extreme_pixel(self, DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN, 0, pixel))
    for(int c = 0; c < 4; c++) pixel[c] = self->picked_color_max[c];

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    // actually, remap between -3.32 EV and infinity for safety because gamma comes later
    RGB[c] = 0.1f - (1.0f - fast_exp10f(_corrected_density(&d, c, pixel[c])));
  }
  p->black = v_maxf(RGB);

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->black, p->black);
  dt_gui_freeze_end();

  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// from Dmax, offset, both white balances, and printblack, compute the print exposure adjustment as a scaling factor
// such that the printed values range from 0 to 1
static void apply_auto_exposure(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  dt_aligned_pixel_t RGB;
  for(int c = 0; c < 3; c++)
  {
    // actually, remap in [0; 0.96] for safety
    RGB[c] = 0.96f / (1.0f - fast_exp10f(_corrected_density(&d, c, self->picked_color_min[c])) + p->black);
  }
  p->exposure = v_minf(RGB);
  dt_iop_color_picker_set_deciding_channel(self, _channel_of_min(RGB));

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));
  dt_gui_freeze_end();

  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

// Kodak's Cineon aims: mid grey at code 470 prints at 18 % reflectance, diffuse white at code 685 at 90 %,
// 215 codes of 0.002 D above it.
#define NEGADOCTOR_GREY_PRINT 0.18f
#define NEGADOCTOR_WHITE_PRINT 0.90f
#define NEGADOCTOR_WHITE_ABOVE_GREY 0.43f

// The print value before the paper gloss that the gloss turns into `print`, which must be below 1.
static inline float _unglossed(const float print, const float soft_clip)
{
  return (print > soft_clip) ? soft_clip - (1.0f - soft_clip) * logf(1.0f - (print - soft_clip) / (1.0f - soft_clip))
                             : print;
}

// The saturation that takes out of the grade the share that makes up for the paper gloss, so that the colors keep
// the scene's ratio between grey and white, 0.90 / 0.18, one to one, as the tones do. The wand's grade is
// log(unglossed white / unglossed grey) over the print's log span between them, and its colors' power the same
// with the glossless ratio: their quotient, which this is plus one, depends on the gloss alone.
static inline float _saturation_without_gloss(const dt_iop_negadoctor_params_t *const p)
{
  const float tones = logf(_unglossed(NEGADOCTOR_WHITE_PRINT, p->soft_clip) / _unglossed(NEGADOCTOR_GREY_PRINT, p->soft_clip));
  return logf(NEGADOCTOR_WHITE_PRINT / NEGADOCTOR_GREY_PRINT) / tones - 1.0f;
}

// log(1 + black - 10^corrected) in channel c for the film transmittance `value`, made `denser` D denser: the log
// of the print before exposure and grade.
static inline float _log_print(const dt_iop_negadoctor_params_t *const p, const dt_iop_negadoctor_data_t *const d,
                               const float value, const int c, const float denser)
{
  return logf(1.0f + p->black - fast_exp10f(_corrected_density(d, c, value) - d->wb_high[c] * denser));
}

// Set print exposure and paper grade from the film settings and the paper black as they stand, so that the
// brightest point of the picked area, a white that keeps its detail, prints as diffuse white and a density 0.43 D
// clearer than it as mid grey, at their Cineon reflectances, in the channel closest to white; in v2, set the
// saturation so that the colors keep the scene's ratio between them too.
// The brightest point is each channel's densest value on the film, as the print exposure picker takes it, and
// the channel that prints it lightest sets both aims, so that no channel of the white passes 90 %. The white is
// the frame's own: Cineon's aims sit at fixed densities, which only a normally exposed negative reaches, so an
// underexposed frame printed from them comes out dark.
static void apply_auto_print(dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_data_t d;
  _data_from_params(p, &d);

  // (exposure · print)^grade = aim at grey and white: two equations, linear in grade and log(exposure).
  const float log_grey = logf(_unglossed(NEGADOCTOR_GREY_PRINT, p->soft_clip));
  const float log_white = logf(_unglossed(NEGADOCTOR_WHITE_PRINT, p->soft_clip));
  dt_aligned_pixel_t white = { 0.0f };
  for(int c = 0; c < 3; c++) white[c] = _log_print(p, &d, self->picked_color_min[c], c, 0.0f);
  const int closest = _channel_of_max(white);
  dt_iop_color_picker_set_deciding_channel(self, closest);
  const float print_white = white[closest];
  const float print_grey = _log_print(p, &d, self->picked_color_min[closest], closest, -NEGADOCTOR_WHITE_ABOVE_GREY);

  // A paper black so low that the grey prints at zero or below (it prints darker than the white, so it reaches
  // zero first) has no solution: keep exposure and grade.
  if(isfinite(print_grey) && print_white > print_grey)
  {
    p->gamma = CLAMPF((log_white - log_grey) / (print_white - print_grey), dt_bauhaus_slider_get_hard_min(g->gamma),
                      dt_bauhaus_slider_get_hard_max(g->gamma));
    // Once the grade is clamped, the grey misses its aim and the white, which was picked, keeps it.
    p->exposure = CLAMPF(expf(log_white / p->gamma - print_white), powf(2.0f, dt_bauhaus_slider_get_hard_min(g->exposure)),
                         powf(2.0f, dt_bauhaus_slider_get_hard_max(g->exposure)));

    if(p->version == DT_NEGADOCTOR_V2)
      p->offset = CLAMPF(_saturation_without_gloss(p), dt_bauhaus_slider_get_hard_min(g->saturation),
                         dt_bauhaus_slider_get_hard_max(g->saturation));
  }

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));
  dt_bauhaus_slider_set(g->gamma, p->gamma);
  dt_bauhaus_slider_set(g->saturation, p->offset);
  dt_gui_freeze_end();

  dt_control_queue_redraw_widget(self->gui->widget);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}


// The button of the saturation slider: the wand's saturation, which depends on the gloss alone, for
// the gloss as it stands
static void _auto_saturation_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;

  p->offset = CLAMPF(_saturation_without_gloss(p), dt_bauhaus_slider_get_hard_min(g->saturation),
                     dt_bauhaus_slider_get_hard_max(g->saturation));

  dt_gui_freeze_begin();
  dt_bauhaus_slider_set(g->saturation, p->offset);
  dt_gui_freeze_end();

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)pipe;
  (void)piece;
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);

  if     (picker == g->Dmin_sampler)
    apply_auto_Dmin(self);
  else if(picker == g->WB_high_sampler)
    apply_auto_WB_high(self);
  else if(picker == g->neutral_sampler[2])
    apply_auto_neutral(self);
  else if(picker == g->neutral_sampler[0])
    apply_auto_neutrals(self, 0);
  else if(picker == g->neutral_sampler[1])
    apply_auto_neutrals(self, 1);
  else if(picker == g->offset)
    apply_auto_offset(self);
  else if(picker == g->black_point_sampler)
    apply_auto_black_point(self);
  else if(picker == g->D_max)
    apply_auto_Dmax(self);
  else if(picker == g->WB_low_sampler)
    apply_auto_WB_low(self);
  else if(picker == g->exposure)
    apply_auto_exposure(self);
  else if(picker == g->black)
    apply_auto_black(self);
  else if(picker == g->auto_print)
    apply_auto_print(self);
  else
    fprintf(stderr, "[negadoctor] unknown color picker\n");
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *g = IOP_GUI_ALLOC(negadoctor);

  g->notebook = dt_ui_notebook_new();
  // Each page holds its own pickers (Dmin, D_max, offset, white balance samplers,
  // black/exposure); reset any active one when the page it lives on is switched
  // away from.
  dt_ui_notebook_set_picker_owner(g->notebook, self);

  // Page FILM PROPERTIES
  GtkWidget *page1 = self->gui->widget = dt_ui_notebook_page(g->notebook, N_("film properties"), NULL);

  // Dmin

  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("color of the film base")), FALSE, FALSE, 0);

  GtkWidget *row1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));

  g->Dmin_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->Dmin_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->Dmin_picker), _("select color of film material from a swatch"));
  gtk_box_pack_start(GTK_BOX(row1), GTK_WIDGET(g->Dmin_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->Dmin_picker), "color-set", G_CALLBACK(Dmin_picker_callback), self);

  g->Dmin_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, row1);
  gtk_widget_set_tooltip_text(g->Dmin_sampler , _("Pick color of film material from image"));

  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(row1), FALSE, FALSE, 0);

  g->Dmin_R = dt_bauhaus_slider_from_params(self, "Dmin[0]");
  dt_bauhaus_slider_set_digits(g->Dmin_R, 4);
  dt_bauhaus_slider_set_format(g->Dmin_R, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_R, 100);
  dt_bauhaus_widget_set_label(g->Dmin_R, N_("D min red component"));
  gtk_widget_set_tooltip_text(g->Dmin_R, _("Adjust the color and shade of the film transparent base.\n"
                                           "This value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  g->Dmin_G = dt_bauhaus_slider_from_params(self, "Dmin[1]");
  dt_bauhaus_slider_set_digits(g->Dmin_G, 4);
  dt_bauhaus_slider_set_format(g->Dmin_G, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_G, 100);
  dt_bauhaus_widget_set_label(g->Dmin_G, N_("D min green component"));
  gtk_widget_set_tooltip_text(g->Dmin_G, _("Adjust the color and shade of the film transparent base.\n"
                                           "This value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  g->Dmin_B = dt_bauhaus_slider_from_params(self, "Dmin[2]");
  dt_bauhaus_slider_set_digits(g->Dmin_B, 4);
  dt_bauhaus_slider_set_format(g->Dmin_B, "%");
  dt_bauhaus_slider_set_factor(g->Dmin_B, 100);
  dt_bauhaus_widget_set_label(g->Dmin_B, N_("D min blue component"));
  gtk_widget_set_tooltip_text(g->Dmin_B, _("Adjust the color and shade of the film transparent base.\n"
                                           "This value depends on the film material, \n"
                                           "the chemical fog produced while developing the film,\n"
                                           "and the scanner white balance."));

  // D max

  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("dynamic range of the film")), FALSE, FALSE, 0);

  g->D_max = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "D_max"));
  dt_color_picker_set_extremes(g->D_max, DT_COLOR_PICKER_EXTREMES_MIN);
  dt_bauhaus_slider_set_format(g->D_max, " D");
  gtk_widget_set_tooltip_text(g->D_max, _("Maximum density of the film, corresponding to white after inversion.\n"
                                          "This value depends on the film specifications, the developing process,\n"
                                          "the dynamic range of the scene and the scanner exposure settings."));
  // The calibration of the layers, their contrast: a property of the film, its development and the scan, not of the scene
  gtk_box_pack_start(GTK_BOX(page1), dt_ui_section_label_new(_("layer calibration")), FALSE, FALSE, 0);

  // v2: measured between two neutrals
  const char *const neutral_labels[2] = { N_("darker neutral"), N_("lighter neutral") };
  const char *const neutral_tooltips[2] = {
    _("Pick a neutral area below the mid tones, such as the second darkest grey\n"
      "of a chart, but not its black: in the deep shadows the film flattens the colors.\n"
      "With the lighter neutral, it measures the contrast of each layer, a property\n"
      "of the film, its development and the scan: set it once for a roll.\n"
      "Two greys well apart, but both clear of the black and the white,\n"
      "keep the most tones neutral.\n"
      "The black points stay: the white balance and the black point\n"
      "of each frame are set on the corrections page."),
    _("Pick a neutral area above the mid tones, such as the second lightest grey\n"
      "of a chart, rather than its white.\n"
      "With the darker neutral, it measures the contrast of each layer, a property\n"
      "of the film, its development and the scan: set it once for a roll.\n"
      "Two greys well apart, but both clear of the black and the white,\n"
      "keep the most tones neutral.\n"
      "The black points stay: the white balance and the black point\n"
      "of each frame are set on the corrections page.")
  };
  for(int k = 0; k < 2; k++)
  {
    g->neutral_row[k] = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
    gtk_box_pack_start(GTK_BOX(g->neutral_row[k]), dt_ui_label_new(_(neutral_labels[k])), TRUE, TRUE, 0);
    g->neutral_sampler[k] = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, g->neutral_row[k]);
    gtk_widget_set_tooltip_text(g->neutral_sampler[k], neutral_tooltips[k]);
    gtk_box_pack_start(GTK_BOX(page1), g->neutral_row[k], FALSE, FALSE, 0);
  }
  g->neutrals_status = dt_ui_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(g->neutrals_status), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(g->neutrals_status), PANGO_ELLIPSIZE_NONE);
  gtk_widget_set_no_show_all(g->neutrals_status, TRUE);
  gtk_box_pack_start(GTK_BOX(page1), g->neutrals_status, FALSE, FALSE, 0);

  // v1: a swatch, and a picker that neutralizes one area against the black points
  GtkWidget *row2 = g->WB_high_row = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));

  g->WB_high_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_high_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_high_picker), _("select color of illuminant from a swatch"));
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->WB_high_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->WB_high_picker), "color-set", G_CALLBACK(WB_high_picker_callback), self);

  g->WB_high_norm = dt_action_button_new((dt_lib_module_t *)self, N_("normalize"), Wb_high_norm_callback, self, _("normalize highlight white balance settings"), 0, 0);
  gtk_box_pack_start(GTK_BOX(row2), GTK_WIDGET(g->WB_high_norm), FALSE, FALSE, 0);

  g->WB_high_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, row2);
  gtk_widget_set_tooltip_text(g->WB_high_sampler , _("Pick illuminant color from image"));

  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(row2), FALSE, FALSE, 0);

  g->wb_high_R = dt_bauhaus_slider_from_params(self, "wb_high[0]");
  dt_bauhaus_widget_set_label(g->wb_high_R, N_("red layer contrast"));
  g->wb_high_G = dt_bauhaus_slider_from_params(self, "wb_high[1]");
  dt_bauhaus_widget_set_label(g->wb_high_G, N_("green layer contrast"));
  g->wb_high_B = dt_bauhaus_slider_from_params(self, "wb_high[2]");
  dt_bauhaus_widget_set_label(g->wb_high_B, N_("blue layer contrast"));

  const char *const wb_high_tooltip
      = _("Contrast of this layer of the film against the others, as the scan sees it:\n"
          "it multiplies the density of the channel above its black point,\n"
          "so the black point and the paper white keep their color.\n"
          "It depends on the film, its development and the scan, not on the scene:\n"
          "set it once for a roll, from two neutrals, and copy it to its other frames.\n"
          "The color cast of the light belongs to each frame: the white balance\n"
          "picker of the corrections page puts it into the black points.");
  GtkWidget *const wb_highs[3] = { g->wb_high_R, g->wb_high_G, g->wb_high_B };
  for(int c = 0; c < 3; c++) gtk_widget_set_tooltip_text(wb_highs[c], wb_high_tooltip);

  // Page CORRECTIONS
  GtkWidget *page2 = self->gui->widget = dt_ui_notebook_page(g->notebook, N_("corrections"), NULL);

  // the black point, which the shadows color cast then corrects
  gtk_box_pack_start(GTK_BOX(page2), dt_ui_section_label_new(_("exposure of the film")), FALSE, FALSE, 0);

  g->density_unit = dt_bauhaus_combobox_from_conf(dt_bauhaus_get_global(), DT_GUI_MODULE(self),
                                                  "plugins/darkroom/negadoctor/density_unit");
  gtk_box_pack_start(GTK_BOX(page2), g->density_unit, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->density_unit), "value-changed", G_CALLBACK(_density_unit_callback), self);

  // v1: the bias

  g->offset = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "offset"));
  dt_color_picker_set_extremes(g->offset, DT_COLOR_PICKER_EXTREMES_MAX);
  gtk_widget_set_tooltip_text(g->offset, _("This is the density of the blacks above the film base,\n"
                                           "which is printed at the paper black.\n"
                                           "Correct the exposure error of the image on the film, made when shooting,\n"
                                           "for all RGB channels, before the inversion, so blacks are neither clipped or too pale.\n"
                                           "The exposure of the scanner is cancelled by the film base color,\n"
                                           "once it is picked on the same scan."));

  // v2: one black point per channel, which holds both the bias and the shadows color cast
  g->black_point_row = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
  gtk_box_pack_start(GTK_BOX(g->black_point_row), dt_ui_label_new(_("black point")), TRUE, TRUE, 0);
  g->black_point_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, g->black_point_row);
  dt_color_picker_set_extremes(g->black_point_sampler, DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN);
  dt_color_picker_set_channel_scale(g->black_point_sampler, _picker_channel_scale);
  g->black_point_density = dt_ui_label_new("");
  gtk_widget_set_tooltip_text(g->black_point_density,
                              _("Densities above the film base, in red, green and blue,\n"
                                "of the pixel the black point picker took."));
  gtk_box_pack_start(GTK_BOX(g->black_point_row), g->black_point_density, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(g->black_point_sampler,
                              _("Pick the blacks from the image to set their level alone:\n"
                                "the three black points move together, so that the pixel the print\n"
                                "makes dark in all three channels prints at the paper black.\n"
                                "The contrast of the layers and the white balance stay."));
  gtk_box_pack_start(GTK_BOX(page2), g->black_point_row, FALSE, FALSE, 0);

  // v2: the white balance of the frame, the color of the black points from one neutral, which keeps their level
  g->neutral_row[2] = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
  gtk_box_pack_start(GTK_BOX(g->neutral_row[2]), dt_ui_label_new(_("white balance")), TRUE, TRUE, 0);
  g->neutral_sampler[2] = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, g->neutral_row[2]);
  gtk_widget_set_tooltip_text(g->neutral_sampler[2],
                              _("Pick a neutral area of the frame to remove the color cast of the light:\n"
                                "the black points get the color that prints it neutral.\n"
                                "A mid grey is safer than a black, where the film flattens the colors.\n"
                                "The contrast of the layers and the level of the blacks stay."));
  gtk_box_pack_start(GTK_BOX(page2), g->neutral_row[2], FALSE, FALSE, 0);

  g->black_point_R = dt_bauhaus_slider_from_params(self, "black_point[0]");
  dt_bauhaus_widget_set_label(g->black_point_R, N_("black point red"));
  g->black_point_G = dt_bauhaus_slider_from_params(self, "black_point[1]");
  dt_bauhaus_widget_set_label(g->black_point_G, N_("black point green"));
  g->black_point_B = dt_bauhaus_slider_from_params(self, "black_point[2]");
  dt_bauhaus_widget_set_label(g->black_point_B, N_("black point blue"));

  GtkWidget *const black_points[3] = { g->black_point_R, g->black_point_G, g->black_point_B };
  const char *const black_point_tooltips[3] = {
    _("Density of the blacks above the film base in the red channel,\n"
      "which is printed at the paper black. Raising it darkens\n"
      "the red of the print, so the blacks get more cyan."),
    _("Density of the blacks above the film base in the green channel,\n"
      "which is printed at the paper black. Raising it darkens\n"
      "the green of the print, so the blacks get more magenta."),
    _("Density of the blacks above the film base in the blue channel,\n"
      "which is printed at the paper black. Raising it darkens\n"
      "the blue of the print, so the blacks get more yellow.")
  };
  for(int c = 0; c < 3; c++) gtk_widget_set_tooltip_text(black_points[c], black_point_tooltips[c]);

  // v1: the shadows color cast, which scales the bias
  g->WB_low_section = dt_ui_section_label_new(_("shadows color cast"));
  gtk_box_pack_start(GTK_BOX(page2), g->WB_low_section, FALSE, FALSE, 0);

  GtkWidget *row3 = g->WB_low_row = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));

  g->WB_low_picker = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->WB_low_picker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->WB_low_picker), _("select color of shadows from a swatch"));
  gtk_box_pack_start(GTK_BOX(row3), GTK_WIDGET(g->WB_low_picker), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->WB_low_picker), "color-set", G_CALLBACK(WB_low_picker_callback), self);

  g->WB_low_norm = dt_action_button_new((dt_lib_module_t *)self, N_("normalize"), Wb_low_norm_callback, self, _("normalize shadows white balance settings"), 0, 0);
  gtk_box_pack_start(GTK_BOX(row3), GTK_WIDGET(g->WB_low_norm), FALSE, FALSE, 0);

  g->WB_low_sampler = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, row3);
  gtk_widget_set_tooltip_text(g->WB_low_sampler, _("Pick shadows color from image"));

  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(row3), FALSE, FALSE, 0);

  g->wb_low_R = dt_bauhaus_slider_from_params(self, "wb_low[0]");
  dt_bauhaus_widget_set_label(g->wb_low_R, N_("shadows red offset"));
  gtk_widget_set_tooltip_text(g->wb_low_R, _("Correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  g->wb_low_G = dt_bauhaus_slider_from_params(self, "wb_low[1]");
  dt_bauhaus_widget_set_label(g->wb_low_G, N_("shadows green offset"));
  gtk_widget_set_tooltip_text(g->wb_low_G, _("Correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  g->wb_low_B = dt_bauhaus_slider_from_params(self, "wb_low[2]");
  dt_bauhaus_widget_set_label(g->wb_low_B, N_("shadows blue offset"));
  gtk_widget_set_tooltip_text(g->wb_low_B, _("Correct the color cast in shadows so blacks are\n"
                                             "truly achromatic. Setting this value before\n"
                                             "the highlights illuminant white balance will help\n"
                                             "recovering the global white balance in difficult cases."));

  // Page PRINT PROPERTIES
  GtkWidget *page3 = self->gui->widget = dt_ui_notebook_page(g->notebook, N_("print properties"), NULL);

  // the wand sets the grade of the paper and the exposure of the print together, above both
  GtkWidget *auto_print_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  g->auto_print_label = gtk_label_new(_("grade and exposure from a white"));
  gtk_label_set_xalign(GTK_LABEL(g->auto_print_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(g->auto_print_label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(auto_print_row), g->auto_print_label, TRUE, TRUE, 0);
  g->auto_print = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, auto_print_row);
  dt_color_picker_set_extremes(g->auto_print, DT_COLOR_PICKER_EXTREMES_MIN);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->auto_print), dtgtk_cairo_paint_wand, 0, NULL);
  // its tooltip depends on the version, and is set with the widgets the version shows
  gtk_box_pack_start(GTK_BOX(page3), auto_print_row, FALSE, FALSE, 0);

  // the light through the negative onto the paper, which process() applies first
  gtk_box_pack_start(GTK_BOX(page3), dt_ui_section_label_new(_("virtual print emulation")), FALSE, FALSE, 0);

  g->exposure = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "exposure"));
  dt_color_picker_set_extremes(g->exposure, DT_COLOR_PICKER_EXTREMES_MIN);
  dt_bauhaus_slider_set_soft_min(g->exposure, -1.0);
  dt_bauhaus_slider_set_soft_max(g->exposure, 1.0);
  dt_bauhaus_slider_set_default(g->exposure, 0.0);
  dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
  gtk_widget_set_tooltip_text(g->exposure, _("Correct the printing exposure after inversion to adjust\n"
                                             "the global contrast and avoid clipping highlights.\n"
                                             "The picker brings the brightest point of the area just below white:\n"
                                             "pick a very bright part that should keep its detail,\n"
                                             "not a light source, a specular reflection or dust."));

  // the paper's response to that light, in the order process() applies it
  gtk_box_pack_start(GTK_BOX(page3), dt_ui_section_label_new(_("virtual paper properties")), FALSE, FALSE, 0);

  g->black = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "black"));
  dt_color_picker_set_extremes(g->black, DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN);
  dt_color_picker_set_channel_scale(g->black, _picker_channel_scale);
  dt_bauhaus_slider_set_digits(g->black, 4);
  dt_bauhaus_slider_set_factor(g->black, 100);
  dt_bauhaus_slider_set_format(g->black, "%");
  gtk_widget_set_tooltip_text(g->black, _("Correct the density of black after the inversion,\n"
                                          "to adjust the global contrast while avoiding clipping shadows."));

  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");
  dt_bauhaus_slider_set_soft_range(g->gamma, 1.0f, 10.0f);
  dt_bauhaus_widget_set_label(g->gamma, N_("paper grade (gamma)"));
  gtk_widget_set_tooltip_text(g->gamma, _("Select the grade of the virtual paper, which is actually\n"
                                          "equivalent to applying a gamma. It compensates the film D max\n"
                                          "and recovers the contrast. Use a high grade for high D max."));

  // v2: the saturation, kept in the field v1 uses for the bias, with the button that sets it in the
  // slider's own icon space, where the pickers of the sliders above sit
  g->saturation = dt_bauhaus_slider_from_params(self, "offset");
  dt_bauhaus_widget_set_label(g->saturation, N_("paper saturation"));
  dt_bauhaus_slider_set_factor(g->saturation, 100.0f);
  dt_bauhaus_slider_set_digits(g->saturation, 1);
  dt_bauhaus_slider_set_format(g->saturation, "%");
  gtk_widget_set_tooltip_text(g->saturation, _("The paper grade raises the colors with the contrast.\n"
                                               "At 0 %, they follow the grade, as in v1.\n"
                                               "Lower it to raise the grade without saturating the colors,\n"
                                               "-100 % prints in grey. The neutrals do not change.\n"
                                               "The wand sets it as the wand of the grade does: the colors keep\n"
                                               "the same ratio to the scene as grey and white, without the gloss."));
  dt_bauhaus_widget_set_quad_paint(g->saturation, dtgtk_cairo_paint_wand, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->saturation, FALSE);
  g_signal_connect(G_OBJECT(g->saturation), "quad-pressed", G_CALLBACK(_auto_saturation_callback), self);

  g->soft_clip = dt_bauhaus_slider_from_params(self, "soft_clip");
  dt_bauhaus_slider_set_factor(g->soft_clip, 100);
  dt_bauhaus_slider_set_digits(g->soft_clip, 4);
  dt_bauhaus_slider_set_format(g->soft_clip, "%");
  gtk_widget_set_tooltip_text(g->soft_clip, _("Gradually compress specular highlights past this value\n"
                                              "to avoid clipping while pushing the exposure for mid-tones.\n"
                                              "This somewhat reproduces the behaviour of matte paper."));

  // start building top level widget
  self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  // Film emulsion
  g->film_stock = dt_bauhaus_combobox_from_params(self, "film_stock");
  gtk_widget_set_tooltip_text(g->film_stock, _("Toggle on or off the color controls"));

  g->version = dt_bauhaus_combobox_from_params(self, "version");
  gtk_widget_set_tooltip_text(g->version, _("v1: the shadows color cast scales the film exposure bias,\n"
                                            "so it does nothing at zero bias and reverses with a negative one.\n"
                                            "v2: one black point per channel holds both the bias and the color cast.\n"
                                            "Switching keeps the image as it is, except where v1 cannot hold it."));

  gtk_box_pack_start(GTK_BOX(self->gui->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);
  const int active_page = dt_conf_get_int("plugins/darkroom/negadoctor/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  if(!IS_NULL_PTR(g) && GTK_IS_NOTEBOOK(g->notebook))
    dt_conf_set_int("plugins/darkroom/negadoctor/gui_page", gtk_notebook_get_current_page(g->notebook));
  IOP_GUI_FREE;
}

// The wand's tooltip, which tells about the paper saturation only while its slider shows
static void _auto_print_tooltip_update(dt_iop_negadoctor_gui_data_t *const g, const gboolean saturation_shown)
{
  const char *const tones = _("Set the grade and print exposure from the film settings and the paper black:\n"
                              "pick a white that should keep its detail, as for the print exposure:\n"
                              "the brightest point of the area prints as a white at 90 %\n"
                              "and a density 0.43 D clearer than it as a mid grey at 18 %, as in the Cineon aims.\n"
                              "Not a light source, a specular reflection or dust.");
  gchar *text = saturation_shown
                    ? g_strconcat(tones, "\n",
                                  _("It also sets the paper saturation so that the colors keep\n"
                                    "the same ratio to the scene as grey and white, without the gloss."),
                                  NULL)
                    : g_strdup(tones);
  gtk_widget_set_tooltip_text(g->auto_print, text);
  gtk_widget_set_tooltip_text(g->auto_print_label, text);
  dt_free(text);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_negadoctor_params_t *p = (dt_iop_negadoctor_params_t *)self->params;
  dt_iop_negadoctor_gui_data_t *g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  if(IS_NULL_PTR(w) || w == g->film_stock)
  {
    toggle_stock_controls(self);
    Dmin_picker_update(self);
  }
  else if(w == g->Dmin_R && p->film_stock == DT_FILMSTOCK_NB)
  {
    dt_bauhaus_slider_set(g->Dmin_G, p->Dmin[0]);
    dt_bauhaus_slider_set(g->Dmin_B, p->Dmin[0]);
  }
  else if(w == g->Dmin_R || w == g->Dmin_G || w == g->Dmin_B)
  {
    Dmin_picker_update(self);
  }
  else if(w == g->exposure)
  {
    p->exposure = powf(2.0f, p->exposure);
  }

  if(IS_NULL_PTR(w) || w == g->wb_high_R || w == g->wb_high_G || w == g->wb_high_B)
  {
    WB_high_picker_update(self);
  }

  // switching versions moves the black points to the other version's sliders, so the image stays the same
  if(w == g->version)
  {
    if(p->version == DT_NEGADOCTOR_V2)
    {
      for(int c = 0; c < 3; c++) p->black_point[c] = p->offset * p->wb_low[c];
      // in v2 the bias field holds the saturation, which starts as v1 renders
      p->offset = 0.0f;
    }
    else
    {
      // the bias is the black point farthest from the film base, which the others scale down from
      int far = 0;
      for(int c = 1; c < 3; c++)
        if(fabsf(p->black_point[c]) > fabsf(p->black_point[far])) far = c;
      p->offset = p->black_point[far];
      for(int c = 0; c < 3; c++)
        p->wb_low[c] = isnormal(p->offset) ? CLAMP(p->black_point[c] / p->offset,
                                                   dt_bauhaus_slider_get_hard_min(g->wb_low_R),
                                                   dt_bauhaus_slider_get_hard_max(g->wb_low_R))
                                           : 1.0f;
    }
    _black_point_sliders_set(g, p);
  }

  if(IS_NULL_PTR(w) || w == g->version)
  {
    const gboolean v1 = (p->version == DT_NEGADOCTOR_V1);
    GtkWidget *const v1_widgets[7] = { g->offset, g->WB_low_section, g->WB_low_row,
                                       g->wb_low_R, g->wb_low_G, g->wb_low_B, g->WB_high_row };
    GtkWidget *const v2_widgets[8] = { g->black_point_row, g->black_point_R, g->black_point_G, g->black_point_B,
                                       g->saturation, g->neutral_row[0], g->neutral_row[1], g->neutral_row[2] };
    for(int k = 0; k < 7; k++) gtk_widget_set_visible(v1_widgets[k], v1);
    for(int k = 0; k < 8; k++) gtk_widget_set_visible(v2_widgets[k], !v1);
    _neutrals_status_show(g);
    _auto_print_tooltip_update(g, !v1);
  }

  // a film base picked or typed since moves the densities of the pixel the black point picker took
  if(IS_NULL_PTR(w) || w == g->Dmin_R || w == g->Dmin_G || w == g->Dmin_B) _black_point_density_update(g, p);

  if(IS_NULL_PTR(w) || w == g->D_max) _density_sliders_update(g, p);

  if(IS_NULL_PTR(w) || w == g->wb_low_R || w == g->wb_low_G || w == g->wb_low_B || w == g->version)
  {
    WB_low_picker_update(self);
  }
}


void gui_update(dt_iop_module_t *const self)
{
  // let gui slider match current parameters:
  dt_iop_negadoctor_gui_data_t *const g = (dt_iop_negadoctor_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_negadoctor_params_t *const p = (dt_iop_negadoctor_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  // a neutral picked on another image, or before an undo, is not one of this image's
  for(int k = 0; k < 2; k++) g->neutral_picked[k] = FALSE;
  _neutrals_status_set(g, "");
  g->black_point_picked = FALSE;

  dt_bauhaus_slider_set(g->exposure, log2f(p->exposure));     // warning: GUI is in EV

  // Update custom stuff
  gui_changed(self, NULL, NULL);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
