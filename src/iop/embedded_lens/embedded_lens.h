#pragma once

#include "common/image.h"

#ifdef __cplusplus
constexpr int LENS_MAXKNOTS = 16;
#else
#define LENS_MAXKNOTS 16
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline float dt_embedded_lens_linear_spline(const float *xi, const float *yi, const int ni, const float x)
{
  if(ni <= 0) return 1.0f;
  if(x < xi[0]) return yi[0];

  for(int i = 1; i < ni; i++)
  {
    if(x >= xi[i - 1] && x <= xi[i])
    {
      const float denom = xi[i] - xi[i - 1];
      if(denom == 0.0f) return yi[i - 1];
      const float dydx = (yi[i] - yi[i - 1]) / denom;
      return yi[i - 1] + (x - xi[i - 1]) * dydx;
    }
  }

  return yi[ni - 1];
}

int dt_embedded_lens_init_coeffs(const dt_image_t *img,
                                  float cor_dist_ft, float cor_vig_ft,
                                  float cor_ca_r_ft, float cor_ca_b_ft,
                                  float knots_dist[LENS_MAXKNOTS],
                                  float knots_vig[LENS_MAXKNOTS],
                                  float cor_rgb[3][LENS_MAXKNOTS],
                                  float vig[LENS_MAXKNOTS],
                                  float *out_scale);

gboolean dt_embedded_lens_has_data(const dt_image_t *img);
gboolean dt_embedded_lens_has_distortion(const dt_image_t *img);
gboolean dt_embedded_lens_has_vignetting(const dt_image_t *img);
gboolean dt_embedded_lens_has_ca(const dt_image_t *img);

#ifdef __cplusplus
}
#endif
