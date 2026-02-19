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
#include "common/image.h"
#include "common/image_cache.h"

#include <limits.h>
#include <stdint.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline gboolean dt_thumbtable_info_is_altered(const dt_image_t info)
{
  return info.history_items > 0;
}

static inline gboolean dt_thumbtable_info_is_grouped(const dt_image_t info)
{
  return info.group_members > 1;
}


sqlite3_stmt *dt_thumbtable_info_get_collection_stmt(void);
void dt_thumbtable_info_cleanup(void);

void dt_thumbtable_copy_image(dt_image_t *info, const dt_image_t *const img);
void dt_thumbtable_info_seed_image_cache(const dt_image_t *info);

#ifndef NDEBUG
void dt_thumbtable_info_debug_assert_matches_cache(const dt_image_t *sql_info);
#endif

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
