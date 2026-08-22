#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>

/**
 * @brief The vendor handling this image, or NULL if none does.
 *
 * @details Looked up BY ID, not by index. The table's row order happening to match
 * dt_image_correction_type_t is not something the compiler checks, and the four public
 * predicates below reach this table on every render and every GUI refresh -- indexing it
 * with an out-of-range or reordered type would call a garbage function pointer rather
 * than fail visibly. A five-element scan costs nothing at these call rates.
 */
static const struct dt_embedded_lens_vendor_t *_vendor_for(const dt_image_t *img)
{
  if(!img) return NULL;

  for(size_t i = 0; i < dt_embedded_lens_vendors_count; i++)
    if(dt_embedded_lens_vendors[i].id == img->exif_correction_type)
      return &dt_embedded_lens_vendors[i];

  return NULL;
}

int dt_embedded_lens_init_coeffs(const dt_image_t *img,
                                  const struct dt_embedded_lens_finetune_t *ft,
                                  struct dt_embedded_lens_knots_t *knots,
                                  float *out_scale)
{
  if(out_scale) *out_scale = 1.0f;
  if(!img || !ft || !knots) return 0;

  const dt_image_correction_data_t *const cd = &img->exif_correction_data;
  int nc = 0;

  const struct dt_embedded_lens_vendor_t *const v = _vendor_for(img);
  if(v && v->has_data && v->has_data(cd) && v->populate)
  {
    nc = v->populate(cd, ft, knots, out_scale);
  }

  if(nc <= 0) return 0;

  const float iwd2 = 0.5f * (float)img->p_width;
  const float iht2 = 0.5f * (float)img->p_height;
  const float diag = hypotf(iwd2, iht2);
  const float sr = fminf(iwd2, iht2);
  const float srr = (diag > 1e-6f) ? sr / diag : 0.0f;

  const int tested = 200;
  float scale = 0.0f;
  for(int i = 0; i < tested; i++)
  {
    const float x = srr + (1.0f - srr) * (float)i / (float)(tested - 1);
    for(int c = 0; c < 3; c++)
      scale = fmaxf(scale, dt_embedded_lens_linear_spline(knots->knots_dist, knots->cor_rgb[c], nc, x));
  }
  if(scale <= 1e-6f) scale = 1.0f;

  for(int i = 0; i < nc; i++)
  {
    knots->knots_dist[i] *= scale;
    for(int c = 0; c < 3; c++) knots->cor_rgb[c][i] /= scale;
  }

  if(out_scale) *out_scale = scale;
  return nc;
}

gboolean dt_embedded_lens_has_data(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v = _vendor_for(img);
  return v && v->has_data && v->has_data(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_distortion(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v = _vendor_for(img);
  return v && v->has_distortion && v->has_distortion(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_vignetting(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v = _vendor_for(img);
  return v && v->has_vignetting && v->has_vignetting(&img->exif_correction_data);
}

gboolean dt_embedded_lens_has_ca(const dt_image_t *img)
{
  const struct dt_embedded_lens_vendor_t *const v = _vendor_for(img);
  return v && v->has_ca && v->has_ca(&img->exif_correction_data);
}
