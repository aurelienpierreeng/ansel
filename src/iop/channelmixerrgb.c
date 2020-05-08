
/*
  This file is part of darktable,
  Copyright (C) 2010-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "external/adobe_coeff.c"
#include "dtgtk/drawingarea.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "common/illuminants.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_channelmixer_rgb_params_t)

/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math", \
                      "tree-vectorize", "no-math-errno")
#endif


#define CHANNEL_SIZE 4

typedef struct dt_iop_channelmixer_rgb_params_t
{
  float red[CHANNEL_SIZE];
  float green[CHANNEL_SIZE];
  float blue[CHANNEL_SIZE];
  float saturation[CHANNEL_SIZE];
  float lightness[CHANNEL_SIZE];
  float grey[CHANNEL_SIZE];
  int normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
  dt_illuminant_t illuminant;
  dt_illuminant_fluo_t illum_fluo;
  dt_illuminant_led_t illum_led;
  dt_adaptation_t adaptation;
  float x, y;
  float temperature;
} dt_iop_channelmixer_rgb_params_t;

typedef struct dt_iop_channelmixer_rgb_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *illuminant, *temperature, *adaptation;
  GtkWidget *illum_fluo, *illum_led, *illum_x, *illum_y, *approx_cct, *illum_color;
  GtkWidget *scale_red_R, *scale_red_G, *scale_red_B;
  GtkWidget *scale_green_R, *scale_green_G, *scale_green_B;
  GtkWidget *scale_blue_R, *scale_blue_G, *scale_blue_B;
  GtkWidget *scale_saturation_R, *scale_saturation_G, *scale_saturation_B;
  GtkWidget *scale_lightness_R, *scale_lightness_G, *scale_lightness_B;
  GtkWidget *scale_grey_R, *scale_grey_G, *scale_grey_B;
  GtkWidget *normalize_R, *normalize_G, *normalize_B, *normalize_sat, *normalize_light, *normalize_grey;
} dt_iop_channelmixer_rgb_gui_data_t;

typedef struct dt_iop_channelmixer_rbg_data_t
{
  float DT_ALIGNED_ARRAY MIX[3][4];
  float DT_ALIGNED_PIXEL saturation[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL lightness[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL grey[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL illuminant[4]; // XYZ coordinates of illuminant
  float p;
  int apply_grey;
  dt_adaptation_t adaptation;
} dt_iop_channelmixer_rbg_data_t;


const char *name()
{
  return _("channel mixer rgb");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(v_2) aligned(v_1, v_2:16)
#endif
static inline float scalar_product(const float v_1[4], const float v_2[4])
{
  // specialized 3×1 dot products 2 4×1 RGB-alpha pixels.
  // v_2 needs to be uniform along loop increments, e.g. independent from current pixel values
  return v_1[0] * v_2[0] + v_1[1] * v_2[1] + v_1[2] * v_2[2];
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  return x * x;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline float euclidean_norm(const float vector[4])
{
  return sqrtf(fmaxf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2]), 1e-6f));
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void downscale_vector(float vector[4], const float scaling)
{
  // check zero or NaN
  const int valid = (scaling != 0.f) && !isnan(scaling);

  vector[0] = (valid) ? vector[0] / scaling : 0.0f;
  vector[1] = (valid) ? vector[1] / scaling : 0.0f;
  vector[2] = (valid) ? vector[2] / scaling : 0.0f;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void upscale_vector(float vector[4], const float scaling)
{
  const int valid = !isnan(scaling);

  vector[0] = (valid) ? vector[0] * scaling : 0.0f;
  vector[1] = (valid) ? vector[1] * scaling : 0.0f;
  vector[2] = (valid) ? vector[2] * scaling : 0.0f;
}


static inline void repack_3x3_to_3xSSE(const float input[9], float output[3][4])
{
  // Repack a 3×3 array/matrice into a 3×1 SSE2 vector to enable SSE4/AVX/AVX2 dot products
  output[0][0] = input[0];
  output[0][1] = input[1];
  output[0][2] = input[2];
  output[0][3] = 0.0f;

  output[1][0] = input[3];
  output[1][1] = input[4];
  output[1][2] = input[5];
  output[1][3] = 0.0f;

  output[2][0] = input[6];
  output[2][1] = input[7];
  output[2][2] = input[8];
  output[2][3] = 0.0f;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_rbg_data_t *data = (dt_iop_channelmixer_rbg_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  float DT_ALIGNED_ARRAY XYZ_to_RGB[3][4];

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    repack_3x3_to_3xSSE(work_profile->matrix_in, RGB_to_XYZ);
    repack_3x3_to_3xSSE(work_profile->matrix_out, XYZ_to_RGB);
  }

  assert(piece->colors == 4);
  const size_t ch = 4;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;
  const float *const restrict illuminant = data->illuminant;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, in, out, roi_out, data, XYZ_to_RGB, RGB_to_XYZ, illuminant) \
  aligned(in, out, XYZ_to_RGB, RGB_to_XYZ:64) aligned(illuminant:16)\
  schedule(simd:static)
#endif
  for(size_t k = 0; k < roi_out->height * roi_out->width * ch; k += ch)
  {
    // intermediate temp buffers
    float DT_ALIGNED_PIXEL temp_one[4];
    float DT_ALIGNED_PIXEL temp_two[4];

    // Convert from RGB to XYZ to LMS
    dot_product(in + k, RGB_to_XYZ, temp_one);
    const float Y = temp_one[1];
    downscale_vector(temp_one, Y);

    switch(data->adaptation)
    {
      case DT_ADAPTATION_BRADFORD:
      {
        convert_XYZ_to_bradford_LMS(temp_one, temp_two);
        bradford_adapt_D65(temp_two, illuminant, data->p, temp_one);
        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        convert_XYZ_to_CAT16_LMS(temp_one, temp_two);
        CAT16_adapt_D65(temp_two, illuminant, 1.0f, temp_one); // force full-adaptation
        break;
      }
      case DT_ADAPTATION_LAST:
      {
        break;
      }
    }

    // Compute the 3D mix - this is a rotation + homothety of the vector base of LMS primaries
    // This is equavilent of correcting the RGB primaries from input profile matrice
    dot_product(temp_one, data->MIX, temp_two);

    // Clip negatives -> gamut mapping
    temp_two[0] = fmaxf(temp_two[0], 0.f);
    temp_two[1] = fmaxf(temp_two[1], 0.f);
    temp_two[2] = fmaxf(temp_two[2], 0.f);

    // Compute euclidean norm and ratios for the lightness/colorfulness demodulation
    float norm = euclidean_norm(temp_two);
    temp_one[0] = temp_two[0] / norm;
    temp_one[1] = temp_two[1] / norm;
    temp_one[2] = temp_two[2] / norm;

    // Compute and apply a flat lightness adjustment for the whole pixel
    const float avg = (temp_two[0] + temp_two[1] + temp_two[2]) / 3.0f;
    const float mix = scalar_product(temp_two, data->lightness);
    norm *= fmaxf(1.f + mix / avg, 0.f);

    // Compute a flat colorfulness adjustment for the whole pixel
    float coeff_ratio = 0.f;
    for(size_t c = 0; c < 3; c++) coeff_ratio += sqf(1.0f - temp_one[c]) * data->saturation[c];
    coeff_ratio /= 3.f;

    // Apply colorfulness adjustment channel-wise and repack with lightness to get LMS back
    for(size_t c = 0; c < 3; c++)
    {
      const float ratio = fmaxf(temp_one[c] + (1.0f - temp_one[c]) * coeff_ratio, 0.f);
      temp_two[c] = ratio * norm;
    }

    // Turn RGB into monochrome
    const float grey = Y * scalar_product(temp_two, data->grey);

    // Convert back LMS to XYZ to RGB
    switch(data->adaptation)
    {
      case DT_ADAPTATION_BRADFORD :
      {
        convert_bradford_LMS_to_XYZ(temp_two, temp_one);
        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        convert_CAT16_LMS_to_XYZ(temp_two, temp_one);
        break;
      }
      case DT_ADAPTATION_LAST:
      {
        temp_one[0] = temp_two[0];
        temp_one[1] = temp_two[1];
        temp_one[2] = temp_two[2];
        break;
      }
    }

    upscale_vector(temp_one, Y);
    dot_product(temp_one, XYZ_to_RGB, temp_two);

    // Save
    out[k]     = (data->apply_grey) ? grey : temp_two[0];
    out[k + 1] = (data->apply_grey) ? grey : temp_two[1];
    out[k + 2] = (data->apply_grey) ? grey : temp_two[2];
    out[k + 3] = in[k + 3]; // alpha mask
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)p1;
  dt_iop_channelmixer_rbg_data_t *d = (dt_iop_channelmixer_rbg_data_t *)piece->data;

  float norm_R = 1.0f;
  if(p->normalize_R) norm_R = p->red[0] + p->red[1] + p->red[2];

  float norm_G = 1.0f;
  if(p->normalize_G) norm_G = p->green[0] + p->green[1] + p->green[2];

  float norm_B = 1.0f;
  if(p->normalize_B) norm_B = p->blue[0] + p->blue[1] + p->blue[2];

  float norm_sat = 0.0f;
  if(p->normalize_sat) norm_sat = (p->saturation[0] + p->saturation[1] + p->saturation[2]) / 3.f;

  float norm_light = 0.0f;
  if(p->normalize_light) norm_light = (p->lightness[0] + p->lightness[1] + p->lightness[2]) / 3.f;

  float norm_grey = p->grey[0] + p->grey[1] + p->grey[2];
  d->apply_grey = (norm_grey != 0.0f);

  for(int i = 0; i < 3; i++)
  {
    d->MIX[0][i] = p->red[i] / norm_R;
    d->MIX[1][i] = p->green[i] / norm_B;
    d->MIX[2][i] = p->blue[i] / norm_G;
    d->saturation[i] = -p->saturation[i] - norm_sat;
    d->lightness[i] = p->lightness[i] - norm_light;
    d->grey[i] = p->grey[i] / norm_grey; // = NaN if (norm_grey == 0.f) but we don't care since (d->apply_grey == FALSE)
  }

  // just in case compiler feels clever and uses SSE 4×1 dot product
  d->saturation[CHANNEL_SIZE - 1] = 0.0f;
  d->lightness[CHANNEL_SIZE - 1] = 0.0f;
  d->grey[CHANNEL_SIZE - 1] = 0.0f;

  d->adaptation = p->adaptation;

  // find x y coordinates of illuminant for CIE 1931 2° observer
  float x = p->x;
  float y = p->y;
  illuminant_to_xy(p->illuminant, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  // Convert illuminant from xyY to XYZ
  float XYZ[3];
  illuminant_xy_to_XYZ(x, y, XYZ);

  // Convert illuminant from XYZ to Bradford modified LMS
  if(d->adaptation == DT_ADAPTATION_BRADFORD)
    convert_XYZ_to_bradford_LMS(XYZ, d->illuminant);
  else if(d->adaptation == DT_ADAPTATION_CAT16)
    convert_XYZ_to_CAT16_LMS(XYZ, d->illuminant);

  d->illuminant[3] = 0.f;

  //fprintf(stdout, "illuminant: %i\n", p->illuminant);
  //fprintf(stdout, "x: %f, y: %f\n", x, y);
  //fprintf(stdout, "X: %f - Y: %f - Z: %f\n", XYZ[0], XYZ[1], XYZ[2]);
  fprintf(stdout, "L: %f - M: %f - S: %f\n", d->illuminant[0], d->illuminant[1], d->illuminant[2]);

  // blue compensation for Bradford transform = (test illuminant blue / reference illuminant blue)^0.0834
  // reference illuminant is hard-set D50 for darktable's pipeline
  // test illuminant is user params
  d->p = powf(d->illuminant[2] / 0.818155f, 0.0834f);
}


static inline void dt_colorspaces_pseudoinverse(float (*in)[3], float (*out)[3], int size)
{
  float work[3][6];

  for(int i = 0; i < 3; i++)
  {
    for(int j = 0; j < 6; j++)
      work[i][j] = j == i+3;
    for(int j = 0; j < 3; j++)
      for(int k = 0; k < size; k++)
        work[i][j] += in[k][i] * in[k][j];
  }
  for(int i = 0; i < 3; i++)
  {
    float num = work[i][i];
    for(int j = 0; j < 6; j++)
      work[i][j] /= num;
    for(int k = 0; k < 3; k++)
    {
      if(k==i) continue;
      num = work[k][i];
      for(int j = 0; j < 6; j++)
        work[k][j] -= work[i][j] * num;
    }
  }
  for(int i = 0; i < size; i++)
    for(int j = 0; j < 3; j++)
    {
      out[i][j] = 0.0f;
      for(int k = 0; k < 3; k++)
        out[i][j] += work[j][k+3] * in[i][k];
    }
}


static int find_temperature_from_raw_coeffs(dt_iop_module_t *self, float *chroma_x, float *chroma_y,float *temperature,
                                            dt_illuminant_t *illuminant, dt_adaptation_t *adaptation)
{
  const dt_image_t *img = &self->dev->image_storage;
  const int is_raw = dt_image_is_matrix_correction_supported(img);

  if(is_raw)
  {
    int has_valid_coeffs = TRUE;
    const int num_coeffs = (img->flags & DT_IMAGE_4BAYER) ? 4 : 3;

    // Check coeffs
    for(int k = 0; has_valid_coeffs && k < num_coeffs; k++)
      if(!isnormal(img->wb_coeffs[k]) || img->wb_coeffs[k] == 0.0f) has_valid_coeffs = FALSE;

    if(has_valid_coeffs)
    {
      // Get white balance camera factors
      float WB[4] = { img->wb_coeffs[0],
                      img->wb_coeffs[1],
                      img->wb_coeffs[2],
                      img->wb_coeffs[3] };

      // Get the camera input profile (matrice of primaries)
      float XYZ_to_CAM[4][3];
      XYZ_to_CAM[0][0] = NAN;
      dt_dcraw_adobe_coeff(self->dev->image_storage.camera_makermodel, (float(*)[12])XYZ_to_CAM);

      if(isnan(XYZ_to_CAM[0][0])) return FALSE;

      // Bloody input matrices define XYZ -> CAM transform, as if we often needed camera profiles to output
      // So we need to invert them. Here go your CPU cycles again.
      float CAM_to_XYZ[4][3];
      CAM_to_XYZ[0][0] = NAN;
      dt_colorspaces_pseudoinverse(XYZ_to_CAM, CAM_to_XYZ, 3);
      if(isnan(CAM_to_XYZ[0][0])) return FALSE;

      float x, y;
      WB_coeffs_to_illuminant_xy(CAM_to_XYZ, WB, &x, &y);

      *chroma_x = x;
      *chroma_y = y;

      // Get the correlated color temperature
      const float t = xy_to_CCT(x, y);
      *temperature = t;

      // Compute again the chromaticity from the daylight model
      illuminant_to_xy(DT_ILLUMINANT_D, &x, &y, t, DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);

      // check the error with absolute chromaticity computed directly
      const float err = sqrtf(sqf(*chroma_x - x) + sqf(*chroma_y - y)) / sqrtf(sqf(*chroma_x) + sqf(*chroma_y));

      // The use of CCT is discouraged if err > 5 %
      // reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
      // so if err < 5 %, we default to D illuminant with CCT for better UX
      // or else we use the custom x and y for better accuracy.
      if(err < 0.05f)
      {
        *illuminant = DT_ILLUMINANT_D;
        *adaptation = DT_ADAPTATION_BRADFORD; // Bradford is better suited for daylight
      }
      else
      {
        *illuminant = DT_ILLUMINANT_CUSTOM;
        *adaptation = DT_ADAPTATION_CAT16; // CAT16 is less accurate but more robust for non-daylight
      }

      return TRUE;
    }
  }
  return FALSE;
}

static void update_illuminants(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  if(p->adaptation == DT_ADAPTATION_LAST)
  {
    // user disabled CAT at all, hide everything and exit
    gtk_widget_set_visible(g->illuminant, FALSE);
    gtk_widget_set_visible(g->illum_color, FALSE);
    gtk_widget_set_visible(g->approx_cct, FALSE);
    gtk_widget_set_visible(g->temperature, FALSE);
    gtk_widget_set_visible(g->illum_fluo, FALSE);
    gtk_widget_set_visible(g->illum_led, FALSE);
    gtk_widget_set_visible(g->illum_x, FALSE);
    gtk_widget_set_visible(g->illum_y, FALSE);
    return;
  }
  else
  {
    // set everything visible again and carry on
    gtk_widget_set_visible(g->illuminant, TRUE);
    gtk_widget_set_visible(g->illum_color, TRUE);
    gtk_widget_set_visible(g->approx_cct, TRUE);
    gtk_widget_set_visible(g->temperature, TRUE);
    gtk_widget_set_visible(g->illum_fluo, TRUE);
    gtk_widget_set_visible(g->illum_led, TRUE);
    gtk_widget_set_visible(g->illum_x, TRUE);
  }

  // Put current illuminant x y derivated from standard options
  // directly in user params x and y in case user wants take over manually
  float x = p->x;
  float y = p->y;

  int changed = illuminant_to_xy(p->illuminant, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  if(changed)
  {
    p->x = x;
    p->y = y;
    dt_bauhaus_slider_set(g->illum_x, x);
    dt_bauhaus_slider_set(g->illum_y, y);
  }

  // Display only the relevant sliders
  switch(p->illuminant)
  {
    case DT_ILLUMINANT_PIPE:
    case DT_ILLUMINANT_A:
    case DT_ILLUMINANT_E:
    {
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_D:
    case DT_ILLUMINANT_BB:
    {
      gtk_widget_set_visible(g->temperature, TRUE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_F:
    {
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, TRUE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_LED:
    {
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, TRUE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_CUSTOM:
    {
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, TRUE);
      gtk_widget_set_visible(g->illum_y, TRUE);
      break;
    }
    case DT_ILLUMINANT_LAST:
    {
      break;
    }
  }
}

static void update_xy_color(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float x_min = DT_BAUHAUS_WIDGET(g->illum_x)->data.slider.soft_min;
  const float x_max = DT_BAUHAUS_WIDGET(g->illum_x)->data.slider.soft_max;
  const float y_min = DT_BAUHAUS_WIDGET(g->illum_y)->data.slider.soft_min;
  const float y_max = DT_BAUHAUS_WIDGET(g->illum_y)->data.slider.soft_max;
  const float x_range = x_max - x_min;
  const float y_range = y_max - y_min;

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float x = x_min + stop * x_range;
    illuminant_xy_to_RGB(x, p->y, RGB);
    dt_bauhaus_slider_set_stop(g->illum_x, stop, RGB[0], RGB[1], RGB[2]);
  }

  // Varies y in range around current x params
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float y = y_min + stop * y_range;
    illuminant_xy_to_RGB(p->x, y, RGB);
    dt_bauhaus_slider_set_stop(g->illum_y, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(self->widget);

}


static void update_R_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // scale params if needed
  float RGB[3] = { p->red[0], p->red[1], p->red[2] };

  if(p->normalize_R)
  {
    float sum = RGB[0] + RGB[1] + RGB[2];
    for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float RR_min = DT_BAUHAUS_WIDGET(g->scale_red_R)->data.slider.soft_min;
  const float RR_max = DT_BAUHAUS_WIDGET(g->scale_red_R)->data.slider.soft_max;
  const float RR_range = RR_max - RR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float RR = RR_min + stop * RR_range;
    float stop_R = RR + RGB[1] + RGB[2];
    float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_red_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float RG_min = DT_BAUHAUS_WIDGET(g->scale_red_G)->data.slider.soft_min;
  const float RG_max = DT_BAUHAUS_WIDGET(g->scale_red_G)->data.slider.soft_max;
  const float RG_range = RG_max - RG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float RG = RG_min + stop * RG_range;
    float stop_R = RGB[0] + RG + RGB[2];
    float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_red_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float RB_min = DT_BAUHAUS_WIDGET(g->scale_red_B)->data.slider.soft_min;
  const float RB_max = DT_BAUHAUS_WIDGET(g->scale_red_B)->data.slider.soft_max;
  const float RB_range = RB_max - RB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float RB = RB_min + stop * RB_range;
    float stop_R = RGB[0] + RGB[1] + RB;
    float LMS[4] = { 0.5f * stop_R, 0.5f, 0.5f };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_red_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);

}


static void update_B_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // scale params if needed
  float RGB[3] = { p->blue[0], p->blue[1], p->blue[2] };

  if(p->normalize_B)
  {
    float sum = RGB[0] + RGB[1] + RGB[2];
    for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float BR_min = DT_BAUHAUS_WIDGET(g->scale_blue_R)->data.slider.soft_min;
  const float BR_max = DT_BAUHAUS_WIDGET(g->scale_blue_R)->data.slider.soft_max;
  const float BR_range = BR_max - BR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float BR = BR_min + stop * BR_range;
    float stop_B = BR + RGB[1] + RGB[2];
    float LMS[4] = { 0.5f, 0.5f, 0.5f * stop_B };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_blue_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float BG_min = DT_BAUHAUS_WIDGET(g->scale_blue_G)->data.slider.soft_min;
  const float BG_max = DT_BAUHAUS_WIDGET(g->scale_blue_G)->data.slider.soft_max;
  const float BG_range = BG_max - BG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float BG = BG_min + stop * BG_range;
    float stop_B = RGB[0] + BG + RGB[2];
    float LMS[4] = { 0.5f , 0.5f, 0.5f * stop_B };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_blue_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float BB_min = DT_BAUHAUS_WIDGET(g->scale_blue_B)->data.slider.soft_min;
  const float BB_max = DT_BAUHAUS_WIDGET(g->scale_blue_B)->data.slider.soft_max;
  const float BB_range = BB_max - BB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float BB = BB_min + stop * BB_range;
    float stop_B = RGB[0] + RGB[1] + BB;
    float LMS[4] = { 0.5f, 0.5f, 0.5f * stop_B };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_blue_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);

}



static void update_G_colors(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // scale params if needed
  float RGB[3] = { p->green[0], p->green[1], p->green[2] };

  if(p->normalize_G)
  {
    float sum = RGB[0] + RGB[1] + RGB[2];
    for(int c = 0; c < 3; c++) RGB[c] /= sum;
  }

  // Get the current values bound of the slider, taking into account the possible soft rescaling
  const float GR_min = DT_BAUHAUS_WIDGET(g->scale_green_R)->data.slider.soft_min;
  const float GR_max = DT_BAUHAUS_WIDGET(g->scale_green_R)->data.slider.soft_max;
  const float GR_range = GR_max - GR_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float GR = GR_min + stop * GR_range;
    float stop_G = GR + RGB[1] + RGB[2];
    float LMS[4] = { 0.5f , 0.5f * stop_G, 0.5f };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_green_R, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float GG_min = DT_BAUHAUS_WIDGET(g->scale_green_G)->data.slider.soft_min;
  const float GG_max = DT_BAUHAUS_WIDGET(g->scale_green_G)->data.slider.soft_max;
  const float GG_range = GG_max - GG_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float GG = GG_min + stop * GG_range;
    float stop_G = RGB[0] + GG + RGB[2];
    float LMS[4] = { 0.5f, 0.5f * stop_G, 0.5f };
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_green_G, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  const float GB_min = DT_BAUHAUS_WIDGET(g->scale_green_B)->data.slider.soft_min;
  const float GB_max = DT_BAUHAUS_WIDGET(g->scale_green_B)->data.slider.soft_max;
  const float GB_range = GB_max - GB_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB_t[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float GB = GB_min + stop * GB_range;
    float stop_G = RGB[0] + RGB[1] + GB;
    float LMS[4] = { 0.5f, 0.5f * stop_G , 0.5f};
    convert_any_LMS_to_RGB(LMS, RGB_t, p->adaptation);
    dt_bauhaus_slider_set_stop(g->scale_green_B, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
  }

  gtk_widget_queue_draw(self->widget);

}


static void update_illuminant_color(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  gtk_widget_queue_draw(g->illum_color);
  update_xy_color(self);
}

static gboolean illuminant_color_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  static const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(darktable.bauhaus->line_space);
  cairo_translate(cr, DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width), margin);
  width -= 2. * DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width) + INNER_PADDING;
  height -= 2 * margin;

  // Paint illuminant color
  float RGB[4];
  illuminant_xy_to_RGB(p->x, p->y, RGB);
  cairo_set_source_rgb(cr, RGB[0], RGB[1], RGB[2]);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}



static void update_approx_cct(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  const float t = xy_to_CCT(p->x, p->y);
  gchar *str = g_strdup_printf(_("CCT: %.0f K"), t);
  gtk_label_set_text(GTK_LABEL(g->approx_cct), str);
}


static void illuminant_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  p->illuminant = dt_bauhaus_combobox_get(combo);

  if(p->illuminant == DT_ILLUMINANT_LAST)
  {
    // Get camera WB
    int found = find_temperature_from_raw_coeffs(self, &(p->x), &(p->y), &(p->temperature), &(p->illuminant), &(p->adaptation));

    if(found)
    {
      dt_control_log(_("white balance successfuly extracted from raw image"));

      // find_temperature either set illuminant to custom or D, so update the combobox
      const int reset = darktable.gui->reset;
      darktable.gui->reset = 1;
      dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
      darktable.gui->reset = reset;
    }
    else
    {
      dt_control_log(_("no white balance was found in raw image"));
    }
  }

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void fluo_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->illum_fluo = dt_bauhaus_combobox_get(combo);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void led_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->illum_led = dt_bauhaus_combobox_get(combo);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void temperature_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->temperature = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void illum_x_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->x = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void illum_y_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->y = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_approx_cct(self);
  update_illuminant_color(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void adaptation_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->adaptation = dt_bauhaus_combobox_get(combo);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_illuminants(self);
  update_R_colors(self);
  update_G_colors(self);
  update_B_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[0] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_R_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[1] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_R_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[2] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_R_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[0] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_G_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[1] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_G_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[2] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_G_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[0] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_B_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[1] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_B_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[2] = dt_bauhaus_slider_get(slider);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_B_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->saturation[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->saturation[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->saturation[2] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lightness_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->lightness[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lightness_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->lightness[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lightness_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->lightness[2] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->grey[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->grey[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->grey[2] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_R_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_R = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_R_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_G_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_G = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_G_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_B_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_B = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  update_B_colors(self);
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_sat_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_sat = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_light_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_light = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_grey_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_grey = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_channelmixer_rbg_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)module->params;

  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->illum_fluo, p->illum_fluo);
  dt_bauhaus_combobox_set(g->illum_led, p->illum_led);
  dt_bauhaus_slider_set(g->temperature, p->temperature);
  dt_bauhaus_slider_set(g->illum_x, p->x);
  dt_bauhaus_slider_set(g->illum_y, p->y);
  dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

  dt_bauhaus_slider_set(g->scale_red_R, p->red[0]);
  dt_bauhaus_slider_set(g->scale_red_G, p->red[1]);
  dt_bauhaus_slider_set(g->scale_red_B, p->red[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_R), p->normalize_R);

  dt_bauhaus_slider_set(g->scale_green_R, p->green[0]);
  dt_bauhaus_slider_set(g->scale_green_G, p->green[1]);
  dt_bauhaus_slider_set(g->scale_green_B, p->green[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_G), p->normalize_G);

  dt_bauhaus_slider_set(g->scale_blue_R, p->blue[0]);
  dt_bauhaus_slider_set(g->scale_blue_G, p->blue[1]);
  dt_bauhaus_slider_set(g->scale_blue_B, p->blue[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_B), p->normalize_B);

  dt_bauhaus_slider_set(g->scale_saturation_R, p->saturation[0]);
  dt_bauhaus_slider_set(g->scale_saturation_G, p->saturation[1]);
  dt_bauhaus_slider_set(g->scale_saturation_B, p->saturation[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_sat), p->normalize_sat);

  dt_bauhaus_slider_set(g->scale_lightness_R, p->lightness[0]);
  dt_bauhaus_slider_set(g->scale_lightness_G, p->lightness[1]);
  dt_bauhaus_slider_set(g->scale_lightness_B, p->lightness[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_light), p->normalize_light);

  dt_bauhaus_slider_set(g->scale_grey_R, p->grey[0]);
  dt_bauhaus_slider_set(g->scale_grey_G, p->grey[1]);
  dt_bauhaus_slider_set(g->scale_grey_B, p->grey[2]);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_grey), p->normalize_grey);

  update_illuminants(self);
  update_approx_cct(self);
  update_illuminant_color(self);

  update_R_colors(self);
  update_G_colors(self);
  update_B_colors(self);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_channelmixer_rgb_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_channelmixer_rgb_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_channelmixer_rgb_params_t);
  module->gui_data = NULL;
  dt_iop_channelmixer_rgb_params_t tmp = (dt_iop_channelmixer_rgb_params_t){ { 1.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 1.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 1.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                                                             DT_ILLUMINANT_D, DT_ILLUMINANT_FLUO_F3, DT_ILLUMINANT_LED_B5, DT_ADAPTATION_BRADFORD,
                                                                             0.33f, 0.33f, 5003.f};

  find_temperature_from_raw_coeffs(module, &(tmp.x), &(tmp.y), &(tmp.temperature), &(tmp.illuminant), &(tmp.adaptation));
  memcpy(module->params, &tmp, sizeof(dt_iop_channelmixer_rgb_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_channelmixer_rgb_params_t));
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_channelmixer_rgb_params_t tmp = (dt_iop_channelmixer_rgb_params_t){ { 1.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 1.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 1.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             { 0.f, 0.f, 0.f, 0.f },
                                                                             FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                                                             DT_ILLUMINANT_D, DT_ILLUMINANT_FLUO_F3, DT_ILLUMINANT_LED_B5, DT_ADAPTATION_BRADFORD,
                                                                             0.33f, 0.33f, 5003.f};

  find_temperature_from_raw_coeffs(module, &(tmp.x), &(tmp.y), &(tmp.temperature), &(tmp.illuminant), &(tmp.adaptation));
  if(module->gui_data)
    update_illuminants(module);

  memcpy(module->default_params, &tmp, sizeof(dt_iop_channelmixer_rgb_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_channelmixer_rgb_gui_data_t));
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  const dt_image_t *img = &self->dev->image_storage;
  const int is_raw = dt_image_is_matrix_correction_supported(img);

  // Init GTK notebook
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page0 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page4 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page5 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page6 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page0, gtk_label_new(_("CAT")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page1, gtk_label_new(_("R")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page2, gtk_label_new(_("G")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page3, gtk_label_new(_("B")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page4, gtk_label_new(_("colorfulness")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page5, gtk_label_new(_("brightness")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page6, gtk_label_new(_("grey")));
  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->notebook, 0)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  dtgtk_justify_notebook_tabs(g->notebook);

  g->adaptation = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->adaptation, NULL, _("adaptation"));
  dt_bauhaus_combobox_add(g->adaptation, _("Bradford (ICC v4)"));
  dt_bauhaus_combobox_add(g->adaptation, _("CAT16 (CIECAM16)"));
  dt_bauhaus_combobox_add(g->adaptation, _("none"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->adaptation), _("choose the method to adapt the illuminant: \n"
                                                           "• Bradford (1999) is more accurate for illuminants close to daylight\n"
                                                           "but can push colors out of the gamut for difficult illuminants.\n"
                                                           "• CAT16 (2016) is more robust to avoid imaginary colours\n"
                                                           "while working with large gamut or saturated cyan and purple.\n"
                                                           "• none disables any illuminant adaptation."));
  g_signal_connect(G_OBJECT(g->adaptation), "value-changed", G_CALLBACK(adaptation_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), g->adaptation, FALSE, FALSE, 0);

  g->illuminant = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->illuminant, NULL, _("illuminant"));
  dt_bauhaus_combobox_add(g->illuminant, _("same as pipeline (D65)"));
  dt_bauhaus_combobox_add(g->illuminant, _("A (incandescent)"));
  dt_bauhaus_combobox_add(g->illuminant, _("D (daylight)"));
  dt_bauhaus_combobox_add(g->illuminant, _("E (equi-energy)"));
  dt_bauhaus_combobox_add(g->illuminant, _("F (fluorescent)"));
  dt_bauhaus_combobox_add(g->illuminant, _("LED (LED light)"));
  dt_bauhaus_combobox_add(g->illuminant, _("Planckian (black body)"));
  dt_bauhaus_combobox_add(g->illuminant, _("custom"));

  if(is_raw)
     dt_bauhaus_combobox_add(g->illuminant, _("compute from camera..."));

  g_signal_connect(G_OBJECT(g->illuminant), "value-changed", G_CALLBACK(illuminant_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), g->illuminant, FALSE, FALSE, 0);

  g->illum_fluo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->illum_fluo, NULL, _("source"));
  // CIE fluorescent standards : https://en.wikipedia.org/wiki/Standard_illuminant
  dt_bauhaus_combobox_add(g->illum_fluo, _("F1 (Daylight 6430 K) – medium CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F2 (Cool White 4230 K) – medium CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F3 (White 3450 K) – medium CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F4 (Warm White 2940 K) – medium CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F5 (Daylight 6350 K) – medium CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F6 (Lite White 4150 K) – medium CR"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F7 (D65 simulator 6500 K) – high CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F8 (D50 simulator 5000 K) – high CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F9 (Cool White Deluxe 4150 K) – high CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F10 (Tuned RGB 5000 K) – low CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F11 (Tuned RGB 4000 K) – low CRI"));
  dt_bauhaus_combobox_add(g->illum_fluo, _("F12 (Tuned RGB 3000 K) – low CRI"));
  g_signal_connect(G_OBJECT(g->illum_fluo), "value-changed", G_CALLBACK(fluo_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), g->illum_fluo, FALSE, FALSE, 0);

  g->illum_led = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->illum_led, NULL, _("source"));
  // CIE LED standards : https://en.wikipedia.org/wiki/Standard_illuminant
  dt_bauhaus_combobox_add(g->illum_led, _("B1 (Blue 2733 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("B2 (Blue 2998 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("B3 (Blue 4103 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("B4 (Blue 5109 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("B5 (Blue 6598 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("BH1 (Blue-Red hybrid 2851 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("RGB1 (RGB 2840 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("V1 (Violet 2724 K)"));
  dt_bauhaus_combobox_add(g->illum_led, _("V2 (Violet 4070 K)"));
  g_signal_connect(G_OBJECT(g->illum_led), "value-changed", G_CALLBACK(led_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), g->illum_led, FALSE, FALSE, 0);


  const float max_temp = 15000.f;
  const float min_temp = 1700.f;
  g->temperature = dt_bauhaus_slider_new_with_range(self, min_temp, max_temp, 50., p->temperature, 0);
  dt_bauhaus_widget_set_label(g->temperature, NULL, _("temperature"));
  dt_bauhaus_slider_set_format(g->temperature, "%.0f K");

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    float RGB[4];
    float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float t = min_temp + stop * (max_temp - min_temp);
    illuminant_CCT_to_RGB(t, RGB);
    dt_bauhaus_slider_set_stop(g->temperature, stop, RGB[0], RGB[1], RGB[2]);
  }

  g_signal_connect(G_OBJECT(g->temperature), "value-changed", G_CALLBACK(temperature_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->temperature), FALSE, FALSE, 0);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());

  g->approx_cct = gtk_label_new("CCT:");
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->approx_cct), _("approximated correlated color temperature\n"
                                                           "this is the closest equivalent illuminant in daylight spectrum\n"
                                                           "but the value is inacurate for non-daylight and below 3000 K.\n"
                                                           "information for what it is worth only."));
  //gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->approx_cct), FALSE, FALSE, 0);
  gtk_grid_attach(grid, GTK_WIDGET(g->approx_cct), 0, 0, 1, 1);

  g->illum_color = GTK_WIDGET(gtk_drawing_area_new());
  const float size = DT_PIXEL_APPLY_DPI(2 * darktable.bauhaus->line_space + darktable.bauhaus->line_height);
  gtk_widget_set_size_request(g->illum_color, size, size);
  gtk_widget_set_hexpand(GTK_WIDGET(g->illum_color), TRUE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->illum_color), _("corresponding color of the illuminant in source\n"
                                                            "image before chromatic adaptation.\n"
                                                            "this will be turned into white by adaptation."));

  g_signal_connect(G_OBJECT(g->illum_color), "draw", G_CALLBACK(illuminant_color_draw), self);
  gtk_grid_attach(grid, GTK_WIDGET(g->illum_color), 1, 0, 1, 1);

  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(grid), FALSE, FALSE, 0);

  g->illum_x = dt_bauhaus_slider_new_with_range(self, 0., 0.5, 0.005, p->x, 4);
  dt_bauhaus_widget_set_label(g->illum_x, NULL, _("x"));
  g_signal_connect(G_OBJECT(g->illum_x), "value-changed", G_CALLBACK(illum_x_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->illum_x), FALSE, FALSE, 0);

  g->illum_y = dt_bauhaus_slider_new_with_range(self, 0., 0.5, 0.005, p->y, 4);
  dt_bauhaus_widget_set_label(g->illum_y, NULL, _("y"));
  g_signal_connect(G_OBJECT(g->illum_y), "value-changed", G_CALLBACK(illum_y_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->illum_y), FALSE, FALSE, 0);

  /* red */
  g->scale_red_R = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->red[0], 3);
  dt_bauhaus_widget_set_label(g->scale_red_R, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_red_R), "value-changed", G_CALLBACK(red_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->scale_red_R), FALSE, FALSE, 0);

  g->scale_red_G = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->red[1], 3);
  dt_bauhaus_widget_set_label(g->scale_red_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_red_G), "value-changed", G_CALLBACK(red_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->scale_red_G), FALSE, FALSE, 0);

  g->scale_red_B = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->red[2], 3);
  dt_bauhaus_widget_set_label(g->scale_red_B, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_red_B), "value-changed", G_CALLBACK(red_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page1), GTK_WIDGET(g->scale_red_B), FALSE, FALSE, 0);

  g->normalize_R = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_R), p->normalize_R);
  gtk_box_pack_start(GTK_BOX(page1), g->normalize_R, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_R), "toggled", G_CALLBACK(normalize_R_callback), self);

  /* green */
  g->scale_green_R = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->green[0], 3);
  dt_bauhaus_widget_set_label(g->scale_green_R, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_green_R), "value-changed", G_CALLBACK(green_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->scale_green_R), FALSE, FALSE, 0);

  g->scale_green_G = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->green[1], 3);
  dt_bauhaus_widget_set_label(g->scale_green_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_green_G), "value-changed", G_CALLBACK(green_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->scale_green_G), FALSE, FALSE, 0);

  g->scale_green_B = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->green[2], 3);
  dt_bauhaus_widget_set_label(g->scale_green_B, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_green_B), "value-changed", G_CALLBACK(green_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->scale_green_B), FALSE, FALSE, 0);

  g->normalize_G = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_G), p->normalize_G);
  gtk_box_pack_start(GTK_BOX(page2), g->normalize_G, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_G), "toggled", G_CALLBACK(normalize_G_callback), self);


  /* blue */
  g->scale_blue_R = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->blue[0], 3);
  dt_bauhaus_widget_set_label(g->scale_blue_R, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_blue_R), "value-changed", G_CALLBACK(blue_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->scale_blue_R), FALSE, FALSE, 0);

  g->scale_blue_G = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->blue[1], 3);
  dt_bauhaus_widget_set_label(g->scale_blue_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_blue_G), "value-changed", G_CALLBACK(blue_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->scale_blue_G), FALSE, FALSE, 0);

  g->scale_blue_B = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.005, p->blue[2], 3);
  dt_bauhaus_widget_set_label(g->scale_blue_B, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_blue_B), "value-changed", G_CALLBACK(blue_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->scale_blue_B), FALSE, FALSE, 0);

  g->normalize_B = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_B), p->normalize_B);
  gtk_box_pack_start(GTK_BOX(page3), g->normalize_B, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_B), "toggled", G_CALLBACK(normalize_B_callback), self);


  /* saturation */
  /* warning: the effect of color controls over image are inversed : blue controls red, and the other way. */
  g->scale_saturation_B = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->saturation[2], 3);
  dt_bauhaus_widget_set_label(g->scale_saturation_B, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_saturation_B), "value-changed", G_CALLBACK(saturation_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page4), GTK_WIDGET(g->scale_saturation_B), FALSE, FALSE, 0);

  g->scale_saturation_G = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->saturation[1], 3);
  dt_bauhaus_widget_set_label(g->scale_saturation_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_saturation_G), "value-changed", G_CALLBACK(saturation_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page4), GTK_WIDGET(g->scale_saturation_G), FALSE, FALSE, 0);

  g->scale_saturation_R = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->saturation[0], 3);
  dt_bauhaus_widget_set_label(g->scale_saturation_R, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_saturation_R), "value-changed", G_CALLBACK(saturation_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page4), GTK_WIDGET(g->scale_saturation_R), FALSE, FALSE, 0);

  g->normalize_sat = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_sat), p->normalize_sat);
  gtk_box_pack_start(GTK_BOX(page4), g->normalize_sat, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_sat), "toggled", G_CALLBACK(normalize_sat_callback), self);


  /* lightness */
  g->scale_lightness_R = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lightness[0], 3);
  dt_bauhaus_widget_set_label(g->scale_lightness_R, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_lightness_R), "value-changed", G_CALLBACK(lightness_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page5), GTK_WIDGET(g->scale_lightness_R), FALSE, FALSE, 0);

  g->scale_lightness_G = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lightness[1], 3);
  dt_bauhaus_widget_set_label(g->scale_lightness_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_lightness_G), "value-changed", G_CALLBACK(lightness_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page5), GTK_WIDGET(g->scale_lightness_G), FALSE, FALSE, 0);

  g->scale_lightness_B = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->lightness[2], 3);
  dt_bauhaus_widget_set_label(g->scale_lightness_B, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_lightness_B), "value-changed", G_CALLBACK(lightness_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page5), GTK_WIDGET(g->scale_lightness_B), FALSE, FALSE, 0);

  g->normalize_light = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_light), p->normalize_light);
  gtk_box_pack_start(GTK_BOX(page5), g->normalize_light, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_light), "toggled", G_CALLBACK(normalize_light_callback), self);

  /* grey */
  g->scale_grey_R = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->grey[0], 3);
  dt_bauhaus_widget_set_label(g->scale_grey_R, NULL, _("input red"));
  g_signal_connect(G_OBJECT(g->scale_grey_R), "value-changed", G_CALLBACK(grey_R_callback), self);
  gtk_box_pack_start(GTK_BOX(page6), GTK_WIDGET(g->scale_grey_R), FALSE, FALSE, 0);

  g->scale_grey_G = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->grey[1], 3);
  dt_bauhaus_widget_set_label(g->scale_grey_G, NULL, _("input green"));
  g_signal_connect(G_OBJECT(g->scale_grey_G), "value-changed", G_CALLBACK(grey_G_callback), self);
  gtk_box_pack_start(GTK_BOX(page6), GTK_WIDGET(g->scale_grey_G), FALSE, FALSE, 0);

  g->scale_grey_B = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.005, p->grey[2], 3);
  dt_bauhaus_widget_set_label(g->scale_grey_B, NULL, _("input blue"));
  g_signal_connect(G_OBJECT(g->scale_grey_B), "value-changed", G_CALLBACK(grey_B_callback), self);
  gtk_box_pack_start(GTK_BOX(page6), GTK_WIDGET(g->scale_grey_B), FALSE, FALSE, 0);

  g->normalize_grey = gtk_check_button_new_with_label(_("normalize channels"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(g->normalize_grey), p->normalize_grey);
  gtk_box_pack_start(GTK_BOX(page6), g->normalize_grey, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->normalize_grey), "toggled", G_CALLBACK(normalize_grey_callback), self);
}


void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
