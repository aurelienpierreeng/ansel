#include "embedded_lens_vendors.h"

extern "C" {
  gboolean _sony_has_data(const dt_image_correction_data_t *cd);
  gboolean _sony_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _sony_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _sony_has_ca(const dt_image_correction_data_t *cd);
  int _sony_populate(const dt_image_correction_data_t *cd,
                      float cor_dist_ft, float cor_vig_ft,
                      float cor_ca_r_ft, float cor_ca_b_ft,
                      float knots_dist[16], float knots_vig[16],
                      float cor_rgb[3][16], float vig[16],
                      float *out_scale);

  gboolean _fuji_has_data(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_ca(const dt_image_correction_data_t *cd);
  int _fuji_populate(const dt_image_correction_data_t *cd,
                      float cor_dist_ft, float cor_vig_ft,
                      float cor_ca_r_ft, float cor_ca_b_ft,
                      float knots_dist[16], float knots_vig[16],
                      float cor_rgb[3][16], float vig[16],
                      float *out_scale);

  gboolean _dng_has_data(const dt_image_correction_data_t *cd);
  gboolean _dng_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _dng_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _dng_has_ca(const dt_image_correction_data_t *cd);
  int _dng_populate(const dt_image_correction_data_t *cd,
                     float cor_dist_ft, float cor_vig_ft,
                     float cor_ca_r_ft, float cor_ca_b_ft,
                     float knots_dist[16], float knots_vig[16],
                     float cor_rgb[3][16], float vig[16],
                     float *out_scale);

  gboolean _olympus_has_data(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_ca(const dt_image_correction_data_t *cd);
  int _olympus_populate(const dt_image_correction_data_t *cd,
                         float cor_dist_ft, float cor_vig_ft,
                         float cor_ca_r_ft, float cor_ca_b_ft,
                         float knots_dist[16], float knots_vig[16],
                         float cor_rgb[3][16], float vig[16],
                         float *out_scale);
}

const struct dt_embedded_lens_vendor_t dt_embedded_lens_vendors[] =
{
  [CORRECTION_TYPE_NONE]    = { .id = CORRECTION_TYPE_NONE,    .name = "none" },
  [CORRECTION_TYPE_SONY]    = { .id = CORRECTION_TYPE_SONY,    .name = "sony",
                                .has_data       = _sony_has_data,
                                .has_distortion = _sony_has_distortion,
                                .has_vignetting = _sony_has_vignetting,
                                .has_ca         = _sony_has_ca,
                                .populate       = _sony_populate },
  [CORRECTION_TYPE_FUJI]    = { .id = CORRECTION_TYPE_FUJI,    .name = "fuji",
                                .has_data       = _fuji_has_data,
                                .has_distortion = _fuji_has_distortion,
                                .has_vignetting = _fuji_has_vignetting,
                                .has_ca         = _fuji_has_ca,
                                .populate       = _fuji_populate },
  [CORRECTION_TYPE_DNG]     = { .id = CORRECTION_TYPE_DNG,     .name = "dng",
                                .has_data       = _dng_has_data,
                                .has_distortion = _dng_has_distortion,
                                .has_vignetting = _dng_has_vignetting,
                                .has_ca         = _dng_has_ca,
                                .populate       = _dng_populate },
  [CORRECTION_TYPE_OLYMPUS] = { .id = CORRECTION_TYPE_OLYMPUS, .name = "olympus",
                                .has_data       = _olympus_has_data,
                                .has_distortion = _olympus_has_distortion,
                                .has_vignetting = _olympus_has_vignetting,
                                .has_ca         = _olympus_has_ca,
                                .populate       = _olympus_populate },
};

const size_t dt_embedded_lens_vendors_count = sizeof(dt_embedded_lens_vendors) / sizeof(dt_embedded_lens_vendors[0]);
