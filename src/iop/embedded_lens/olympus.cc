#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>

extern "C" {

gboolean _olympus_has_data(const dt_image_correction_data_t *cd)
{
  return cd->olympus.has_dist || cd->olympus.has_ca;
}

gboolean _olympus_has_distortion(const dt_image_correction_data_t *cd)
{
  return cd->olympus.has_dist;
}

gboolean _olympus_has_vignetting(const dt_image_correction_data_t *cd)
{
  (void)cd;
  return FALSE;
}

gboolean _olympus_has_ca(const dt_image_correction_data_t *cd)
{
  return cd->olympus.has_ca;
}

int _olympus_populate(const dt_image_correction_data_t *cd,
                       const dt_embedded_lens_finetune_t *ft,
                       struct dt_embedded_lens_knots_t *knots,
                       const float *out_scale)
{
  (void)out_scale;
  const auto *const oly = &cd->olympus;
  const int nc = LENS_MAXKNOTS;

  float drs = 1.0f;
  float dk2 = 0.0f;
  float dk4 = 0.0f;
  float dk6 = 0.0f;
  if(oly->has_dist)
  {
    dk2 = oly->dist[0];
    dk4 = oly->dist[1];
    dk6 = oly->dist[2];
    drs = oly->dist[3];
  }
  float car0 = 0.0f;
  float car2 = 0.0f;
  float car4 = 0.0f;
  float cab0 = 0.0f;
  float cab2 = 0.0f;
  float cab4 = 0.0f;
  if(oly->has_ca)
  {
    car0 = oly->ca[0];
    car2 = oly->ca[1];
    car4 = oly->ca[2];
    cab0 = oly->ca[3];
    cab2 = oly->ca[4];
    cab4 = oly->ca[5];
  }

  for(int i = 0; i < nc; i++)
  {
    const float r = (float)i / (float)(nc - 1);
    knots->knots_dist[i] = r;
    knots->knots_vig[i] = r;
    knots->vig[i] = 1.0f;

    float base = 1.0f;
    if(oly->has_dist)
    {
      const float rs2 = (r * drs) * (r * drs);
      const float r_cor = drs * (1.0f + rs2 * (dk2 + rs2 * (dk4 + rs2 * dk6)));
      base = ft->distortion * (r_cor - 1.0f) + 1.0f;
    }
    knots->cor_rgb[0][i] = base;
    knots->cor_rgb[1][i] = base;
    knots->cor_rgb[2][i] = base;

    if(oly->has_ca && r > 0.0f)
    {
      const float rd = base * r;
      const float rd2 = rd * rd;
      knots->cor_rgb[0][i] += ft->ca_red * (rd * (car0 + rd2 * (car2 + rd2 * car4))) / r;
      knots->cor_rgb[2][i] += ft->ca_blue * (rd * (cab0 + rd2 * (cab2 + rd2 * cab4))) / r;
    }
  }

  return nc;
}

} // extern "C"
