#ifndef DT_IOP_EMBEDDED_LENS_EMBEDDED_LENS_H
#define DT_IOP_EMBEDDED_LENS_EMBEDDED_LENS_H

#include "common/image.h"
#include "embedded_lens_vendors.h"

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

typedef struct dt_embedded_lens_knots_t {
  float knots_dist[LENS_MAXKNOTS];
  float knots_vig[LENS_MAXKNOTS];
  float cor_rgb[3][LENS_MAXKNOTS];
  float vig[LENS_MAXKNOTS];
} dt_embedded_lens_knots_t;

int dt_embedded_lens_init_coeffs(const dt_image_t *img,
                                  const struct dt_embedded_lens_finetune_t *ft,
                                  struct dt_embedded_lens_knots_t *knots,
                                  float *out_scale);

gboolean dt_embedded_lens_has_data(const dt_image_t *img);
gboolean dt_embedded_lens_has_distortion(const dt_image_t *img);
gboolean dt_embedded_lens_has_vignetting(const dt_image_t *img);
gboolean dt_embedded_lens_has_ca(const dt_image_t *img);

#ifdef __cplusplus
}
#endif

#endif // DT_IOP_EMBEDDED_LENS_EMBEDDED_LENS_H
