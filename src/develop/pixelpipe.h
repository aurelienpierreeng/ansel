/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdint.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

typedef enum dt_dev_pixelpipe_type_t
{
  DT_DEV_PIXELPIPE_NONE      = 0,
  DT_DEV_PIXELPIPE_EXPORT    = 1,
  DT_DEV_PIXELPIPE_FULL      = 2,
  DT_DEV_PIXELPIPE_PREVIEW   = 3,
  DT_DEV_PIXELPIPE_THUMBNAIL = 4
} dt_dev_pixelpipe_type_t;

/** when to collect histogram */
typedef enum dt_dev_request_flags_t
{
  DT_REQUEST_NONE = 0,
  DT_REQUEST_ON = 1 << 0,
  DT_REQUEST_ONLY_IN_GUI = 1 << 1
} dt_dev_request_flags_t;

// params to be used to collect histogram
typedef struct dt_dev_histogram_collection_params_t
{
  /** histogram_collect: if NULL, correct is set; else should be set manually */
  const struct dt_histogram_roi_t *roi;
  /** count of histogram bins. */
  uint32_t bins_count;
  /** in most cases, bins_count-1. */
  float mul;
} dt_dev_histogram_collection_params_t;

// params used to collect histogram during last histogram capture
typedef struct dt_dev_histogram_stats_t
{
  /** count of histogram bins. */
  uint32_t bins_count;
  /** count of pixels sampled during histogram capture. */
  uint32_t pixels;
  /** count of channels: 1 for RAW, 3 for rgb/Lab. */
  uint32_t ch;
} dt_dev_histogram_stats_t;

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

const char *dt_pixelpipe_name(dt_dev_pixelpipe_type_t pipe);

#include "develop/pixelpipe_hb.h"

GHashTable *dt_pixelpipe_raster_alloc();
void dt_pixelpipe_raster_cleanup(GHashTable *raster_masks);

/**
 * @brief Replace a raster mask in the raster masks hashtable of the pixelpipe.
 * 
 * @param raster_masks the raster masks hashtable of the pixelpipe
 * @param mask the pointer to the mask
 * @return gboolean TRUE if the key did not exist and was added, FALSE if it existed and was replaced.
 */
gboolean dt_pixelpipe_raster_replace(GHashTable *raster_masks, float *mask);

/**
 * @brief Remove the raster mask with id 0 from the raster masks hashtable of the pixelpipe.
 * 
 * @param raster_masks the raster masks hashtable of the pixelpipe
 * @return gboolean TRUE if the key existed and was removed, FALSE if it did not exist and nothing changed.
 */
gboolean dt_pixelpipe_raster_remove(GHashTable *raster_masks);

/**
 * @brief Get the raster mask with given id from the raster masks hashtable of the pixelpipe.
 * 
 * @param raster_masks the raster masks hashtable of the pixelpipe
 * @param raster_mask_id the raster mask id
 * @return float* the pointer to the raster mask, or NULL if not found
 */
float *dt_pixelpipe_raster_get(GHashTable *raster_masks, const int raster_mask_id);


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
