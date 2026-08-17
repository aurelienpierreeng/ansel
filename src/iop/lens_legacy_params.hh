#pragma once

#include <stddef.h>
#include <string.h>

#include <glib.h>

#include "lens_predicates.h"

enum class dt_iop_lens_method_t
{
  LENSFUN = 0,
  EMBEDDED_METADATA = 1
};

typedef struct dt_iop_lensfun_params_t
{
  int modify_flags;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[128];
  char lens[128];
  float tca_r;
  float tca_b;
  int has_been_set;
  dt_iop_lens_correction_source_t vignetting_method;
  dt_iop_lens_correction_source_t distortion_method;
  dt_iop_lens_tca_source_t tca_method;
} dt_iop_lensfun_params_t;

typedef struct dt_iop_lensfun_params_v2_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[52];
  char lens[52];
  int tca_override;
  float tca_r;
  float tca_b;
} dt_iop_lensfun_params_v2_t;

typedef struct dt_iop_lensfun_params_v3_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[128];
  char lens[128];
  int tca_override;
  float tca_r;
  float tca_b;
} dt_iop_lensfun_params_v3_t;

typedef struct dt_iop_lensfun_params_v4_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[128];
  char lens[128];
  int tca_override;
  float tca_r;
  float tca_b;
  int modified;
} dt_iop_lensfun_params_v4_t;

typedef struct dt_iop_lensfun_params_v5_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[128];
  char lens[128];
  gboolean tca_override;
  float tca_r;
  float tca_b;
  int modified;
} dt_iop_lensfun_params_v5_t;

typedef struct dt_iop_lensfun_params_v6_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[128];
  char lens[128];
  gboolean tca_override;
  float tca_r;
  float tca_b;
  int has_been_set;
  dt_iop_lens_method_t method;
  float cor_dist_ft;
  float cor_vig_ft;
  float cor_ca_r_ft;
  float cor_ca_b_ft;
  float scale_md;
} dt_iop_lensfun_params_v6_t;

template<size_t destination_size, size_t source_size>
static inline void dt_iop_lensfun_copy_legacy_string(char (&destination)[destination_size],
                                                      const char (&source)[source_size])
{
  size_t source_length = 0;
  while(source_length < source_size && source[source_length]) source_length++;

  const size_t copy_length = source_length < destination_size - 1 ? source_length : destination_size - 1;
  memcpy(destination, source, copy_length);
  destination[copy_length] = '\0';
}

template<typename legacy_params_t>
static inline void dt_iop_lensfun_copy_legacy_params(const legacy_params_t *legacy,
                                                       const dt_iop_lensfun_params_t *defaults,
                                                       dt_iop_lensfun_params_t *output)
{
  *output = *defaults;
  output->modify_flags = legacy->modify_flags;
  output->scale = legacy->scale;
  output->crop = legacy->crop;
  output->focal = legacy->focal;
  output->aperture = legacy->aperture;
  output->distance = legacy->distance;
  output->target_geom = legacy->target_geom;
  dt_iop_lensfun_copy_legacy_string(output->camera, legacy->camera);
  dt_iop_lensfun_copy_legacy_string(output->lens, legacy->lens);
  output->tca_r = legacy->tca_b;
  output->tca_b = legacy->tca_r;
}

static inline int dt_iop_lensfun_convert_legacy_params(const void *old_params,
                                                        const int old_version,
                                                        const dt_iop_lensfun_params_t *defaults,
                                                        dt_iop_lensfun_params_t *new_params,
                                                        const int new_version)
{
  if(new_version != 7) return 1;

  switch(old_version)
  {
    case 2:
    {
      const auto *legacy = static_cast<const dt_iop_lensfun_params_v2_t *>(old_params);
      dt_iop_lensfun_copy_legacy_params(legacy, defaults, new_params);
      new_params->has_been_set = 0;
      return 0;
    }
    case 3:
    {
      const auto *legacy = static_cast<const dt_iop_lensfun_params_v3_t *>(old_params);
      dt_iop_lensfun_copy_legacy_params(legacy, defaults, new_params);
      new_params->has_been_set = 0;
      return 0;
    }
    case 4:
    {
      const auto *legacy = static_cast<const dt_iop_lensfun_params_v4_t *>(old_params);
      dt_iop_lensfun_copy_legacy_params(legacy, defaults, new_params);
      new_params->has_been_set = !legacy->modified;
      return 0;
    }
    case 5:
    {
      const auto *legacy = static_cast<const dt_iop_lensfun_params_v5_t *>(old_params);
      dt_iop_lensfun_copy_legacy_params(legacy, defaults, new_params);
      new_params->has_been_set = !legacy->modified;
      return 0;
    }
    case 6:
    {
      const auto *legacy = static_cast<const dt_iop_lensfun_params_v6_t *>(old_params);
      dt_iop_lensfun_copy_legacy_params(legacy, defaults, new_params);
      new_params->has_been_set = legacy->has_been_set;
      if(legacy->method == dt_iop_lens_method_t::EMBEDDED_METADATA)
      {
        new_params->vignetting_method = dt_iop_lens_correction_source_t::EMBEDDED;
        new_params->distortion_method = dt_iop_lens_correction_source_t::EMBEDDED;
        new_params->tca_method = dt_iop_lens_tca_source_t::EMBEDDED;
        return 0;
      }
      new_params->vignetting_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
      new_params->distortion_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
      new_params->tca_method = dt_iop_lens_tca_source_t::LENSFUN_DB;
      return 0;
    }
    default: return 1;
  }
}
