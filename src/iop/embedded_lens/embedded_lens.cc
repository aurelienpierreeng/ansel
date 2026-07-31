#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>

int dt_embedded_lens_init_coeffs(const dt_image_t *img,
                                  float cor_dist_ft, float cor_vig_ft,
                                  float cor_ca_r_ft, float cor_ca_b_ft,
                                  float knots_dist[LENS_MAXKNOTS],
                                  float knots_vig[LENS_MAXKNOTS],
                                  float cor_rgb[3][LENS_MAXKNOTS],
                                  float vig[LENS_MAXKNOTS],
                                  float *out_scale)
{
  if(out_scale) *out_scale = 1.0f;

  const dt_image_correction_data_t *const cd = &img->exif_correction_data;
  int nc = 0;

  if(img->exif_correction_type < dt_embedded_lens_vendors_count)
  {
    const struct dt_embedded_lens_vendor_t *const v =
        &dt_embedded_lens_vendors[img->exif_correction_type];
    if(v->has_data && v->has_data(cd))
      nc = v->populate(cd, cor_dist_ft, cor_vig_ft, cor_ca_r_ft, cor_ca_b_ft,
                       knots_dist, knots_vig, cor_rgb, vig, out_scale);
  }

  if(nc <= 0) return 0;

  const float iwd2 = 0.5f * (float)img->p_width;
  const float iht2 = 0.5f * (float)img->p_height;
  const float diag = hypotf(iwd2, iht2);
  const float sr = fminf(iwd2, iht2);
  const float srr = (diag > 1e-6f) ? sr / diag : 0.0f;

  const float tested = 200.0f;
  float scale = 0.0f;
  for(float i = 0.0f; i < tested; i++)
  {
    const float x = srr + (1.0f - srr) * i / (tested - 1.0f);
    for(int c = 0; c < 3; c++)
      scale = fmaxf(scale, dt_embedded_lens_linear_spline(knots_dist, cor_rgb[c], nc, x));
  }
  if(scale <= 1e-6f) scale = 1.0f;

  for(int i = 0; i < nc; i++)
  {
    knots_dist[i] *= scale;
    for(int c = 0; c < 3; c++) cor_rgb[c][i] /= scale;
  }

  if(out_scale) *out_scale = scale;
  return nc;
}

gboolean dt_embedded_lens_has_data(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v =
      &dt_embedded_lens_vendors[img->exif_correction_type];
  return v->has_data && v->has_data(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_distortion(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v =
      &dt_embedded_lens_vendors[img->exif_correction_type];
  return v->has_distortion && v->has_distortion(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_vignetting(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v =
      &dt_embedded_lens_vendors[img->exif_correction_type];
  return v->has_vignetting && v->has_vignetting(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_ca(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v =
      &dt_embedded_lens_vendors[img->exif_correction_type];
  return v->has_ca && v->has_ca(&img->exif_correction_data);
}
