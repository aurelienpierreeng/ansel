#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>

extern "C" {

gboolean _dng_has_data(const dt_image_correction_data_t *cd)
{
  return cd->dng.has_warp || cd->dng.has_vignette;
}

gboolean _dng_has_distortion(const dt_image_correction_data_t *cd)
{
  return cd->dng.has_warp;
}

gboolean _dng_has_vignetting(const dt_image_correction_data_t *cd)
{
  return cd->dng.has_vignette;
}

gboolean _dng_has_ca(const dt_image_correction_data_t *cd)
{
  if(!cd->dng.has_warp) return FALSE;
  return cd->dng.warp_planes > 1;
}

int _dng_populate(const dt_image_correction_data_t *cd,
                   const dt_embedded_lens_finetune_t *ft,
                   float knots_dist[LENS_MAXKNOTS],
                   float knots_vig[LENS_MAXKNOTS],
                   float cor_rgb[3][LENS_MAXKNOTS],
                   float vig[LENS_MAXKNOTS],
                   const float *out_scale)
{
  (void)out_scale;
  const auto *const dng = &cd->dng;
  const int nc = LENS_MAXKNOTS;
  const int nplanes = (int)MIN(dng->warp_planes, (uint32_t)3);
  const int canonical_plane = (dng->warp_planes > 1) ? 1 : 0;
  const gboolean apply_tca = dng->warp_planes > 1;

  auto warp_radial = [](const double coeffs[6], double r2) {
    return coeffs[0] + r2 * (coeffs[1] + r2 * (coeffs[2] + r2 * coeffs[3]));
  };

  for(int i = 0; i < nc; i++)
  {
    const float r = (float)i / (float)(nc - 1);
    knots_dist[i] = r;
    knots_vig[i] = r;
    const double r2 = (double)r * (double)r;

    if(dng->has_warp)
    {
      for(int c = 0; c < 3; c++)
      {
        const int plane = apply_tca ? MIN(c, nplanes - 1) : canonical_plane;
        const double r_cor = warp_radial(dng->warp_coeffs[plane], r2);
        cor_rgb[c][i] = (float)(ft->distortion * (r_cor - 1.0) + 1.0);
      }
    }
    else
    {
      cor_rgb[0][i] = 1.0f;
      cor_rgb[1][i] = 1.0f;
      cor_rgb[2][i] = 1.0f;
    }

    if(dng->has_vignette)
    {
      const double dvig = r2
          * (dng->vig_coeffs[0]
             + r2 * (dng->vig_coeffs[1] + r2 * (dng->vig_coeffs[2] + r2 * (dng->vig_coeffs[3] + r2 * dng->vig_coeffs[4]))));
      vig[i] = (float)(1.0 / (1.0 + ft->vignette * dvig));
    }
    else
    {
      vig[i] = 1.0f;
    }
  }

  return nc;
}

} // extern "C"
