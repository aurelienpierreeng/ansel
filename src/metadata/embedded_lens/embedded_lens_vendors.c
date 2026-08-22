#include "embedded_lens_vendors.h"


  gboolean _sony_has_data(const dt_image_correction_data_t *cd);
  gboolean _sony_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _sony_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _sony_has_ca(const dt_image_correction_data_t *cd);
  int _sony_populate(const dt_image_correction_data_t *cd,
                      const dt_embedded_lens_finetune_t *ft,
                      struct dt_embedded_lens_knots_t *knots,
                      const float *out_scale);

  gboolean _fuji_has_data(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _fuji_has_ca(const dt_image_correction_data_t *cd);
  int _fuji_populate(const dt_image_correction_data_t *cd,
                      const dt_embedded_lens_finetune_t *ft,
                      struct dt_embedded_lens_knots_t *knots,
                      const float *out_scale);

  gboolean _dng_has_data(const dt_image_correction_data_t *cd);
  gboolean _dng_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _dng_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _dng_has_ca(const dt_image_correction_data_t *cd);
  int _dng_populate(const dt_image_correction_data_t *cd,
                     const dt_embedded_lens_finetune_t *ft,
                     struct dt_embedded_lens_knots_t *knots,
                     const float *out_scale);

  gboolean _olympus_has_data(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_distortion(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_vignetting(const dt_image_correction_data_t *cd);
  gboolean _olympus_has_ca(const dt_image_correction_data_t *cd);
  int _olympus_populate(const dt_image_correction_data_t *cd,
                         const dt_embedded_lens_finetune_t *ft,
                         struct dt_embedded_lens_knots_t *knots,
                         const float *out_scale);

const struct dt_embedded_lens_vendor_t dt_embedded_lens_vendors[] = // NOSONAR
{
  { CORRECTION_TYPE_NONE, "none", NULL, NULL, NULL, NULL, NULL },
  { CORRECTION_TYPE_SONY, "sony",
    _sony_has_data, _sony_has_distortion, _sony_has_vignetting, _sony_has_ca,
    _sony_populate },
  { CORRECTION_TYPE_FUJI, "fuji",
    _fuji_has_data, _fuji_has_distortion, _fuji_has_vignetting, _fuji_has_ca,
    _fuji_populate },
  { CORRECTION_TYPE_DNG, "dng",
    _dng_has_data, _dng_has_distortion, _dng_has_vignetting, _dng_has_ca,
    _dng_populate },
  { CORRECTION_TYPE_OLYMPUS, "olympus",
    _olympus_has_data, _olympus_has_distortion, _olympus_has_vignetting, _olympus_has_ca,
    _olympus_populate },
};

const size_t dt_embedded_lens_vendors_count
    = sizeof(dt_embedded_lens_vendors) / sizeof(dt_embedded_lens_vendors[0]);
