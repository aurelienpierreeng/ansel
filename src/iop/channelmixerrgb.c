
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
  float x, y;
  float temperature;
} dt_iop_channelmixer_rgb_params_t;

typedef struct dt_iop_channelmixer_rgb_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *illuminant, *temperature;
  GtkWidget *illum_fluo, *illum_led, *illum_x, *illum_y;
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
#pragma omp declare simd uniform(M) aligned(M:64) aligned(v_in, v_out:16)
#endif
static inline void dot_product(const float v_in[4], const float M[3][4], float v_out[4])
{
  // specialized 3×3 dot products of 4×1 RGB-alpha pixels
  v_out[0] = M[0][0] * v_in[0] + M[0][1] * v_in[1] + M[0][2] * v_in[2];
  v_out[1] = M[1][0] * v_in[0] + M[1][1] * v_in[1] + M[1][2] * v_in[2];
  v_out[2] = M[2][0] * v_in[0] + M[2][1] * v_in[1] + M[2][2] * v_in[2];
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


// modified LMS cone response space for Bradford transform
// explanation here : https://onlinelibrary.wiley.com/doi/pdf/10.1002/9781119021780.app3
// but coeffs are wrong in the above, so they come from :
// http://www2.cmp.uea.ac.uk/Research/compvis/Papers/FinSuss_COL00.pdf
// At any time, ensure XYZ_to_LMS is the exact matrice inverse of LMS_to_XYZ
#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_XYZ_to_LMS(const float XYZ[4], float LMS[4])
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  static const float DT_ALIGNED_ARRAY XYZ_to_LMS[3][4] = { {  0.8951f,  0.2664f, -0.1614f, 0.f },
                                                           { -0.7502f,  1.7135f,  0.0367f, 0.f },
                                                           {  0.0389f, -0.0685f,  1.0296f, 0.f } };
  dot_product(XYZ, XYZ_to_LMS, LMS);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_LMS_to_XYZ(const float LMS[4], float XYZ[4])
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  static const float DT_ALIGNED_ARRAY LMS_to_XYZ[3][4] = { {  0.9870f, -0.1471f,  0.1600f, 0.f },
                                                           {  0.4323f,  0.5184f,  0.0493f, 0.f },
                                                           { -0.0085f,  0.0400f,  0.9685f, 0.f } };
  dot_product(LMS, LMS_to_XYZ, XYZ);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void bradford_adapt(const float lms_in[4],
                                  const float origin_illuminant[4],
                                  const float p,
                                  float lms_out[4])
{
  // Bradford chromatic adaptation from origin to target D50 illuminant in LMS space

  // Precomputed D50 primaries in Bradford LMS for darktable's pipeline between colorin and colorout
  // darktable's pipeline is hard-set to D50 because it is the ICC default connection space.
  // FIXME: if darktable's pipeline standard illuminant is EVER CHANGED in the future,
  // this const needs to be updated or become a variable.
  static const float DT_ALIGNED_PIXEL D50[4] = { 0.996078f, 1.020646f, 0.818155f, 0.f };

  float DT_ALIGNED_PIXEL temp[4] = { lms_in[0] / origin_illuminant[0],
                                     lms_in[1] / origin_illuminant[1],
                                     lms_in[2] / origin_illuminant[2],
                                     0.f };

  temp[2] = powf(fmaxf(temp[2], 0.f), p);

  lms_out[0] = D50[0] * temp[0];
  lms_out[1] = D50[1] * temp[1];
  lms_out[2] = D50[2] * temp[2];
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
  const int valid = (scaling != 0.f) && (scaling != -scaling);

  vector[0] = (valid) ? vector[0] / scaling : 0.0f;
  vector[1] = (valid) ? vector[1] / scaling : 0.0f;
  vector[2] = (valid) ? vector[2] / scaling : 0.0f;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void upscale_vector(float vector[4], const float scaling)
{
  // check NaN
  const int valid = (scaling != -scaling);

  vector[0] = (valid) ? vector[0] * scaling : 0.0f;
  vector[1] = (valid) ? vector[1] * scaling : 0.0f;
  vector[2] = (valid) ? vector[2] * scaling : 0.0f;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_channelmixer_rbg_data_t *data = (dt_iop_channelmixer_rbg_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  float DT_ALIGNED_ARRAY RGB_to_XYZ[3][4];
  float DT_ALIGNED_ARRAY XYZ_to_RGB[3][4];

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile) // this can't be fetched in commit_params since work_profile is not yet initialised
  {
    // Input
    RGB_to_XYZ[0][0] = work_profile->matrix_in[0];
    RGB_to_XYZ[0][1] = work_profile->matrix_in[1];
    RGB_to_XYZ[0][2] = work_profile->matrix_in[2];
    RGB_to_XYZ[0][3] = 0.0f;

    RGB_to_XYZ[1][0] = work_profile->matrix_in[3];
    RGB_to_XYZ[1][1] = work_profile->matrix_in[4];
    RGB_to_XYZ[1][2] = work_profile->matrix_in[5];
    RGB_to_XYZ[1][3] = 0.0f;

    RGB_to_XYZ[2][0] = work_profile->matrix_in[6];
    RGB_to_XYZ[2][1] = work_profile->matrix_in[7];
    RGB_to_XYZ[2][2] = work_profile->matrix_in[8];
    RGB_to_XYZ[2][3] = 0.0f;

    // Output
    XYZ_to_RGB[0][0] = work_profile->matrix_out[0];
    XYZ_to_RGB[0][1] = work_profile->matrix_out[1];
    XYZ_to_RGB[0][2] = work_profile->matrix_out[2];
    XYZ_to_RGB[0][3] = 0.0f;

    XYZ_to_RGB[1][0] = work_profile->matrix_out[3];
    XYZ_to_RGB[1][1] = work_profile->matrix_out[4];
    XYZ_to_RGB[1][2] = work_profile->matrix_out[5];
    XYZ_to_RGB[1][3] = 0.0f;

    XYZ_to_RGB[2][0] = work_profile->matrix_out[6];
    XYZ_to_RGB[2][1] = work_profile->matrix_out[7];
    XYZ_to_RGB[2][2] = work_profile->matrix_out[8];
    XYZ_to_RGB[2][3] = 0.0f;
  }

  assert(piece->colors == 4);
  const size_t ch = 4;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, in, out, roi_out, data, XYZ_to_RGB, RGB_to_XYZ) \
  aligned(in, out, XYZ_to_RGB, RGB_to_XYZ:64) \
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
    convert_XYZ_to_LMS(temp_one, temp_two);

    // Bradford chromatic adaptation / white balance -> fancy LMS scaling
    bradford_adapt(temp_two, data->illuminant, data->p, temp_one);

    // Compute the 3D mix - this is a rotation + homothety of the vector base of LMS primaries
    // This is equavilent of correcting the RGB primaries from input profile matrice
    dot_product(temp_one, data->MIX, temp_two);

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
    convert_LMS_to_XYZ(temp_two, temp_one);
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

  // find x y coordinates of illuminant for CIE 1931 2° observer
  float x = p->x;
  float y = p->y;
  illuminant_to_xy(p->illuminant, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  // Convert illuminant from xyY to XYZ
  float XYZ[3];
  illuminant_xy_to_XYZ(x, y, XYZ);

  // Convert illuminant from XYZ to Bradford modified LMS
  convert_XYZ_to_LMS(XYZ, d->illuminant);
  d->illuminant[3] = 0.f;

  //fprintf(stdout, "illuminant: %i\n", p->illuminant);
  //fprintf(stdout, "x: %f, y: %f\n", x, y);
  //fprintf(stdout, "X: %f - Y: %f - Z: %f\n", XYZ[0], XYZ[1], XYZ[2]);
  //fprintf(stdout, "L: %f - M: %f - S: %f\n", d->illuminant[0], d->illuminant[1], d->illuminant[2]);

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


static int find_temperature_from_raw_coeffs(dt_iop_module_t *self, float *chroma_x, float *chroma_y,
                                            float *temperature, dt_illuminant_t *illuminant)
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

      float XYZ[4];
      // Simulate white point, aka convert (1, 1, 1) in camera space to XYZ
      // warning : we multiply the transpose of CAM_to_XYZ  since the inverse transpose it
      XYZ[0] = CAM_to_XYZ[0][0] / WB[0] + CAM_to_XYZ[1][0] / WB[1] + CAM_to_XYZ[2][0] / WB[2];
      XYZ[1] = CAM_to_XYZ[0][1] / WB[0] + CAM_to_XYZ[1][1] / WB[1] + CAM_to_XYZ[2][1] / WB[2];
      XYZ[2] = CAM_to_XYZ[0][2] / WB[0] + CAM_to_XYZ[1][2] / WB[1] + CAM_to_XYZ[2][2] / WB[2];

      // Get white point chromaticity
      XYZ[0] /= XYZ[1];
      XYZ[2] /= XYZ[1];
      XYZ[1] /= XYZ[1];

      float x = XYZ[0] / (XYZ[0] + XYZ[1] + XYZ[2]);
      float y = XYZ[1] / (XYZ[0] + XYZ[1] + XYZ[2]);
      *chroma_x = x;
      *chroma_y = y;

      // Try to find correlated color temperature from chromaticity
      // Valid for 3000 K to 50000 K
      // Reference : https://www.usna.edu/Users/oceano/raylee/papers/RLee_AO_CCTpaper.pdf
      const float n = (x - 0.3366f)/(y - 0.1735f);
      const float t = -949.86315f + 6253.80338f * expf(-n / 0.92159f) + 28.70599f * expf(-n / 0.20039f) + 0.00004f * expf(-n / 0.07125f);
      *temperature = t;

      // Compute again the chromaticity from the daylight model
      illuminant_to_xy(DT_ILLUMINANT_D, &x, &y, t, DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);

      // check the error
      const float err = sqrtf(sqf(*chroma_x - x) + sqf(*chroma_y - y)) / sqrtf(sqf(*chroma_x) + sqf(*chroma_y));

      // The use of CCT is discouraged if err > 5 %
      // reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
      // so if err < 5 %, we default to D illuminant with CCT for better UX
      // or else we use the custom x and y.
      if(err < 0.05f) *illuminant = DT_ILLUMINANT_D;
      else *illuminant = DT_ILLUMINANT_CUSTOM;

      return TRUE;
    }
  }
  return FALSE;
}

static void update_illuminants(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;

  // Put current illuminant x y directly in params in case user want to start custom edit from standard values
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

static void illuminant_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = (dt_iop_channelmixer_rgb_gui_data_t *)self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;

  p->illuminant = dt_bauhaus_combobox_get(combo);

  if(p->illuminant == DT_ILLUMINANT_LAST)
  {
    // Get camera WB
    int found = find_temperature_from_raw_coeffs(self, &(p->x), &(p->y), &(p->temperature), &(p->illuminant));

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
  darktable.gui->reset = reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void illum_x_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->x = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void illum_y_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->y = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void red_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->red[2] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void green_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->green[2] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_R_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[0] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_G_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[1] = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blue_B_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->blue[2] = dt_bauhaus_slider_get(slider);
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
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_G_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_G = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void normalize_B_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)self->params;
  p->normalize_B = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
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
                                                                             DT_ILLUMINANT_D, DT_ILLUMINANT_FLUO_F3, DT_ILLUMINANT_LED_B5,
                                                                             0.33f, 0.33f, 5003.f};

  if(find_temperature_from_raw_coeffs(module, &(tmp.x), &(tmp.y), &(tmp.temperature), &(tmp.illuminant)))
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
                                                                             DT_ILLUMINANT_D, DT_ILLUMINANT_FLUO_F3, DT_ILLUMINANT_LED_B5,
                                                                             0.33f, 0.33f, 5003.f};

  if(find_temperature_from_raw_coeffs(module, &(tmp.x), &(tmp.y), &(tmp.temperature), &(tmp.illuminant)))
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

  g->illuminant = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->illuminant, NULL, _("illuminant"));
  dt_bauhaus_combobox_add(g->illuminant, _("pipeline default (D50)"));
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


  g->temperature = dt_bauhaus_slider_new_with_range(self, 2800., 24000., 50., p->temperature, 0);
  dt_bauhaus_widget_set_label(g->temperature, NULL, _("temperature"));
  dt_bauhaus_slider_set_format(g->temperature, "%.0f K");
  g_signal_connect(G_OBJECT(g->temperature), "value-changed", G_CALLBACK(temperature_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->temperature), FALSE, FALSE, 0);

  g->illum_x = dt_bauhaus_slider_new_with_range(self, 0., 1., 0.01, p->x, 4);
  dt_bauhaus_widget_set_label(g->illum_x, NULL, _("x"));
  g_signal_connect(G_OBJECT(g->illum_x), "value-changed", G_CALLBACK(illum_x_callback), self);
  gtk_box_pack_start(GTK_BOX(page0), GTK_WIDGET(g->illum_x), FALSE, FALSE, 0);

  g->illum_y = dt_bauhaus_slider_new_with_range(self, 0., 1., 0.01, p->y, 4);
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
