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

#include "common/darktable.h"

#include <limits.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dt_image_t;

typedef struct dt_thumbnail_image_info_t
{
  int32_t imgid;
  int32_t film_id;
  int32_t groupid;
  uint32_t group_members;
  uint32_t history_items;
  int32_t version;
  int32_t width;
  int32_t height;
  int32_t orientation;
  int32_t p_width;
  int32_t p_height;
  int32_t flags;
  int32_t loader;

  int rating;
  int colorlabels;
  gboolean has_localcopy;
  gboolean has_audio;
  gboolean is_bw;
  gboolean is_bw_flow;
  gboolean is_hdr;

  GTimeSpan import_timestamp;
  GTimeSpan change_timestamp;
  GTimeSpan export_timestamp;
  GTimeSpan print_timestamp;

  float exif_exposure;
  float exif_exposure_bias;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  float exif_focus_distance;
  GTimeSpan exif_datetime_taken;

  double geoloc_latitude;
  double geoloc_longitude;
  double geoloc_elevation;

  char filename[DT_MAX_FILENAME_LEN];
  char fullpath[PATH_MAX];
  char local_copy_path[PATH_MAX];
  char local_copy_legacy_path[PATH_MAX];
  char filmroll[PATH_MAX];
  char folder[PATH_MAX];
  char datetime[200];
  char camera[128];
  char exif_maker[64];
  char exif_model[64];
  char exif_lens[128];
} dt_thumbnail_image_info_t;

static inline gboolean dt_thumbtable_info_is_altered(const dt_thumbnail_image_info_t *info)
{
  return info && info->history_items > 0;
}

static inline gboolean dt_thumbtable_info_is_grouped(const dt_thumbnail_image_info_t *info)
{
  return info && info->group_members > 1;
}


sqlite3_stmt *dt_thumbtable_info_get_collection_stmt(void);
void dt_thumbtable_info_cleanup(void);

void dt_thumbtable_info_from_stmt(dt_thumbnail_image_info_t *info, sqlite3_stmt *stmt,
                                  uint32_t history_items, uint32_t group_members);
void dt_thumbtable_info_from_image(dt_thumbnail_image_info_t *info, const struct dt_image_t *img);
void dt_thumbtable_info_finalize(dt_thumbnail_image_info_t *info, gboolean expensive);

#ifndef NDEBUG
void dt_thumbtable_info_debug_assert_matches_cache(const dt_thumbnail_image_info_t *sql_info,
                                                   uint32_t history_items, uint32_t group_members);
#endif

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
