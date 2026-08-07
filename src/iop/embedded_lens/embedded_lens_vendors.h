#pragma once

#include "common/image.h"

struct dt_embedded_lens_knots_t;

struct dt_embedded_lens_finetune_t
{
  float distortion;
  float vignette;
  float ca_red;
  float ca_blue;
};

struct dt_embedded_lens_vendor_t
{
  dt_image_correction_type_t id;
  const char *name;

  gboolean (*has_data)       (const dt_image_correction_data_t *cd);
  gboolean (*has_distortion) (const dt_image_correction_data_t *cd);
  gboolean (*has_vignetting) (const dt_image_correction_data_t *cd);
  gboolean (*has_ca)         (const dt_image_correction_data_t *cd);

  int (*populate)(const dt_image_correction_data_t *cd,
                  const dt_embedded_lens_finetune_t *ft,
                  struct dt_embedded_lens_knots_t *knots,
                  const float *out_scale);
};

extern const struct dt_embedded_lens_vendor_t dt_embedded_lens_vendors[]; // NOSONAR
extern const size_t dt_embedded_lens_vendors_count;
