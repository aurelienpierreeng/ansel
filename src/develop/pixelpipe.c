/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025-2026 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "develop/pixelpipe_hb.c"

const char *dt_pixelpipe_name(dt_dev_pixelpipe_type_t pipe)
{
  switch(pipe)
  {
    case DT_DEV_PIXELPIPE_NONE: return "NONE";
    case DT_DEV_PIXELPIPE_EXPORT: return "EXPORT";
    case DT_DEV_PIXELPIPE_FULL: return "FULL";
    case DT_DEV_PIXELPIPE_PREVIEW: return "PREVIEW";
    case DT_DEV_PIXELPIPE_THUMBNAIL: return "THUMBNAIL";
    default: return "(unknown)";
  }
}

uint64_t dt_dev_pixelpipe_rawdetail_mask_hash(const dt_dev_pixelpipe_iop_t *piece)
{
  static const char cache_tag[] = "detailmask:rawdetail";
  if(IS_NULL_PTR(piece) || piece->global_hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
    return DT_PIXELPIPE_CACHE_HASH_INVALID;
  return dt_hash(piece->global_hash, cache_tag, sizeof(cache_tag));
}

uint64_t dt_dev_pixelpipe_raster_mask_hash(const dt_dev_pixelpipe_iop_t *piece,
                                           const int raster_mask_id)
{
  static const char cache_tag[] = "raster-mask";
  if(IS_NULL_PTR(piece) || piece->global_mask_hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
    return DT_PIXELPIPE_CACHE_HASH_INVALID;

  uint64_t hash = dt_hash(piece->global_mask_hash, cache_tag, sizeof(cache_tag));
  return dt_hash(hash, (const char *)&raster_mask_id, sizeof(raster_mask_id));
}

void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, pipe->rawdetail_mask_hash);
  pipe->rawdetail_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  memset(&pipe->rawdetail_mask_roi, 0, sizeof(pipe->rawdetail_mask_roi));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
