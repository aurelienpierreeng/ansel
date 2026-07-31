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
                       float cor_dist_ft, float cor_vig_ft,
                       float cor_ca_r_ft, float cor_ca_b_ft,
                       float knots_dist[LENS_MAXKNOTS],
                       float knots_vig[LENS_MAXKNOTS],
                       float cor_rgb[3][LENS_MAXKNOTS],
                       float vig[LENS_MAXKNOTS],
                       float *out_scale)
{
  (void)cor_vig_ft;
  (void)out_scale;
  const dt_image_correction_olympus_t *const oly = &cd->olympus;
  const int nc = LENS_MAXKNOTS;

  float drs = 1.0f, dk2 = 0.0f, dk4 = 0.0f, dk6 = 0.0f;
  if(oly->has_dist)
  {
    dk2 = oly->dist[0];
    dk4 = oly->dist[1];
    dk6 = oly->dist[2];
    drs = oly->dist[3];
  }
  float car0 = 0.0f, car2 = 0.0f, car4 = 0.0f, cab0 = 0.0f, cab2 = 0.0f, cab4 = 0.0f;
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
    knots_dist[i] = knots_vig[i] = r;
    vig[i] = 1.0f;

    float base = 1.0f;
    if(oly->has_dist)
    {
      const float rs2 = (r * drs) * (r * drs);
      const float r_cor = drs * (1.0f + rs2 * (dk2 + rs2 * (dk4 + rs2 * dk6)));
      base = cor_dist_ft * (r_cor - 1.0f) + 1.0f;
    }
    cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = base;

    if(oly->has_ca && r > 0.0f)
    {
      const float rd = base * r;
      const float rd2 = rd * rd;
      cor_rgb[0][i] += cor_ca_r_ft * (rd * (car0 + rd2 * (car2 + rd2 * car4))) / r;
      cor_rgb[2][i] += cor_ca_b_ft * (rd * (cab0 + rd2 * (cab2 + rd2 * cab4))) / r;
    }
  }

  return nc;
}

} // extern "C"
