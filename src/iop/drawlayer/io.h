/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <glib.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/** @file
 *  @brief TIFF sidecar I/O API for drawlayer layers.
 */

#define DT_DRAWLAYER_IO_NAME_SIZE 64
#define DT_DRAWLAYER_IO_PROFILE_SIZE 256

/** @brief Raw float RGBA patch used by drawlayer I/O routines. */
typedef struct dt_drawlayer_io_patch_t
{
  int x;          /**< Patch origin X in raw layer coordinates. */
  int y;          /**< Patch origin Y in raw layer coordinates. */
  int width;      /**< Patch width in pixels. */
  int height;     /**< Patch height in pixels. */
  float *pixels;  /**< Interleaved RGBA float pixel data. */
} dt_drawlayer_io_patch_t;

/** @brief Metadata returned when probing one layer directory in sidecar TIFF. */
typedef struct dt_drawlayer_io_layer_info_t
{
  gboolean found;                                   /**< TRUE when target layer was found. */
  int index;                                        /**< Directory index of found layer. */
  int count;                                        /**< Number of layer directories scanned. */
  uint32_t width;                                   /**< Layer width from TIFF tags. */
  uint32_t height;                                  /**< Layer height from TIFF tags. */
  char name[DT_DRAWLAYER_IO_NAME_SIZE];             /**< Layer name tag. */
  char work_profile[DT_DRAWLAYER_IO_PROFILE_SIZE];  /**< Embedded working profile key. */
} dt_drawlayer_io_layer_info_t;

typedef struct _dt_job_t dt_job_t;

/** @brief Parameters owned by the async "create background from input" job. */
typedef struct dt_drawlayer_io_background_job_params_t
{
  int32_t imgid;
  int raw_width;
  int raw_height;
  int dst_x;
  int dst_y;
  int insert_after_order;
  char sidecar_path[PATH_MAX];
  char work_profile[DT_DRAWLAYER_IO_PROFILE_SIZE];
  char requested_bg_name[DT_DRAWLAYER_IO_NAME_SIZE];
  char filter[64];
  char initiator_layer_name[DT_DRAWLAYER_IO_NAME_SIZE];
  int initiator_layer_order;
  GSourceFunc done_idle;
} dt_drawlayer_io_background_job_params_t;

/** @brief Result posted back to the UI after background-layer creation. */
typedef struct dt_drawlayer_io_background_job_result_t
{
  gboolean success;
  int32_t imgid;
  int64_t sidecar_timestamp;
  char created_bg_name[DT_DRAWLAYER_IO_NAME_SIZE];
  char initiator_layer_name[DT_DRAWLAYER_IO_NAME_SIZE];
  int initiator_layer_order;
  char message[256];
} dt_drawlayer_io_background_job_result_t;

/** @brief Build absolute sidecar TIFF path from image id. */
gboolean dt_drawlayer_io_sidecar_path(int32_t imgid, char *path, size_t path_size);
/** @brief Lookup layer by name/order and return directory metadata. */
gboolean dt_drawlayer_io_find_layer(const char *path, const char *target_name, int target_order,
                                    dt_drawlayer_io_layer_info_t *info);
/** @brief Load one layer from TIFF sidecar into float RGBA patch. */
gboolean dt_drawlayer_io_load_layer(const char *path, const char *target_name, int target_order, int raw_width,
                                    int raw_height, dt_drawlayer_io_patch_t *patch);
/** @brief Store or replace one layer page in sidecar TIFF. */
gboolean dt_drawlayer_io_store_layer(const char *path, const char *target_name, int target_order,
                                     const char *work_profile, const dt_drawlayer_io_patch_t *patch, int raw_width,
                                     int raw_height, gboolean delete_target, int *final_order);
/** @brief Insert new layer after target order in sidecar TIFF. */
gboolean dt_drawlayer_io_insert_layer(const char *path, const char *target_name, int insert_after_order,
                                      const char *work_profile, const dt_drawlayer_io_patch_t *patch, int raw_width,
                                      int raw_height, int *final_order);
/** @brief Load full TIFF page as flat RGBA float image. */
gboolean dt_drawlayer_io_load_flat_rgba(const char *path, float **pixels, int *width, int *height);
/** @brief Check whether candidate layer name already exists. */
gboolean dt_drawlayer_io_layer_name_exists(const char *path, const char *candidate, int ignore_index);
/** @brief Build unique layer name with fallback and numeric suffixing. */
void dt_drawlayer_io_make_unique_name(const char *path, const char *requested, const char *fallback_name, char *name,
                                      size_t name_size);
/** @brief Build unique layer name without fallback override. */
void dt_drawlayer_io_make_unique_name_plain(const char *path, const char *requested, char *name, size_t name_size);
/** @brief List all layer names from sidecar TIFF. */
gboolean dt_drawlayer_io_list_layer_names(const char *path, char ***names, int *count);
/** @brief Free name list returned by `dt_drawlayer_io_list_layer_names`. */
void dt_drawlayer_io_free_layer_names(char ***names, int *count);
/** @brief Worker entrypoint for async "create background from input" sidecar jobs. */
int32_t dt_drawlayer_io_background_layer_job_run(dt_job_t *job);
