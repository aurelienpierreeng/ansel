#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>

extern "C" {

gboolean _sony_has_data(const dt_image_correction_data_t *cd)
{
  return cd->sony.nc >= 2 && cd->sony.nc <= LENS_MAXKNOTS;
}

gboolean _sony_has_distortion(const dt_image_correction_data_t *cd)
{
  for(int i = 0; i < cd->sony.nc; i++)
    if(cd->sony.distortion[i] != 0) return TRUE;
  return FALSE;
}

gboolean _sony_has_vignetting(const dt_image_correction_data_t *cd)
{
  for(int i = 0; i < cd->sony.nc; i++)
    if(cd->sony.vignetting[i] != 0) return TRUE;
  return FALSE;
}

gboolean _sony_has_ca(const dt_image_correction_data_t *cd)
{
  for(int i = 0; i < cd->sony.nc; i++)
    if(cd->sony.ca_r[i] != 0 || cd->sony.ca_b[i] != 0) return TRUE;
  return FALSE;
}

int _sony_populate(const dt_image_correction_data_t *cd,
                    const dt_embedded_lens_finetune_t *ft,
                    float knots_dist[LENS_MAXKNOTS],
                    float knots_vig[LENS_MAXKNOTS],
                    float cor_rgb[3][LENS_MAXKNOTS],
                    float vig[LENS_MAXKNOTS],
                    const float *out_scale)
{
  (void)out_scale;
  constexpr float SONY_DIST_SCALE = 1.0f / 16384.0f;
  constexpr float SONY_CA_SCALE = 1.0f / 2097152.0f;
  constexpr float SONY_VIG_SCALE = 1.0f / 8192.0f;

  const auto *const sony = &cd->sony;
  const int nc = sony->nc;

  for(int i = 0; i < nc; i++)
  {
    const float frac = (float)(i + 0.5) / (float)(nc - 1);
    knots_dist[i] = frac;
    knots_vig[i] = frac;

    const float dist_cor = ft->distortion * ((float)sony->distortion[i] * SONY_DIST_SCALE) + 1.0f;
    cor_rgb[0][i] = dist_cor;
    cor_rgb[1][i] = dist_cor;
    cor_rgb[2][i] = dist_cor;

    cor_rgb[0][i] *= ft->ca_red * ((float)sony->ca_r[i] * SONY_CA_SCALE) + 1.0f;
    cor_rgb[2][i] *= ft->ca_blue * ((float)sony->ca_b[i] * SONY_CA_SCALE) + 1.0f;

    const float val = ft->vignette * ((float)sony->vignetting[i] * SONY_VIG_SCALE);
    vig[i] = powf(2.0f, 0.5f - powf(2.0f, val - 1.0f));
  }

  return nc;
}

} // extern "C"
