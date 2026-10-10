/*
    This file is part of darktable,
    Copyright (C) 2011-2012, 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014 johannes hanika.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    
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

#include "common/grouping.h"
#include "common/collection.h"
#include "common/undo.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "database/collection_query.h"
#include "database/image_repository.h"
#include "caches/image_cache.h"
#include "common/image_notify.h"

int32_t dt_grouping_get_image_group(const int32_t image_id)
{
  const dt_image_t *img = dt_image_cache_get(image_id, 'r');
  const int img_group_id = img->group_id;
  dt_image_cache_read_release(img);
  return img_group_id;
}

/** add an image to a group */
void dt_grouping_add_to_group(const int32_t group_id, const int32_t image_id)
{
  const dt_image_t *const current = dt_image_cache_get(image_id, 'r');
  const int32_t current_group_id = current->group_id;
  dt_image_cache_read_release(current);
  if(current_group_id == group_id) return;

  // remove from old group
  dt_grouping_remove_from_group(image_id);

  dt_image_t *img = dt_image_cache_get(image_id, 'w');
  img->group_id = group_id;
  dt_image_cache_write_release(img, DT_IMAGE_CACHE_SAFE);
  dt_undo_clear(dt_undo_get_global(), DT_UNDO_GROUPING);
  GList *members = dt_image_repository_get_group_members(group_id, -1);
  dt_image_notify_changed(members);
}

/** @brief Remove an image and refresh the derived counts of every member of its former group.
 * @details Snapshot membership before writing. Even rows whose group ID stays the same have a
 * new member count, so the final notification must include them after all write locks are released.
 */
int dt_grouping_remove_from_group(const int32_t image_id)
{
  int new_group_id = -1;

  const dt_image_t *img = dt_image_cache_get(image_id, 'r');
  if(IS_NULL_PTR(img)) return -1;
  const int img_group_id = img->group_id;
  dt_image_cache_read_release(img);
  GList *imgs = dt_image_repository_get_group_members(img_group_id, -1);
  if(img_group_id == image_id)
  {
    for(GList *o = imgs; !IS_NULL_PTR(o); o = g_list_next(o))
    {
      const int other_id = GPOINTER_TO_INT(o->data);
      if(other_id == image_id) continue;
      if(new_group_id == -1) new_group_id = other_id;
      dt_image_t *other_img = dt_image_cache_get(other_id, 'w');
      other_img->group_id = new_group_id;
      dt_image_cache_write_release(other_img, DT_IMAGE_CACHE_SAFE);
    }

    if(new_group_id != -1)
    {
      dt_image_repository_reassign_group(img_group_id, new_group_id, image_id);
    }
    else
    {
      g_list_free(imgs);
      return -1;
    }
  }
  else
  {
    // change the group_id for this image.
    dt_image_t *wimg = dt_image_cache_get(image_id, 'w');
    new_group_id = wimg->group_id;
    wimg->group_id = image_id;
    dt_image_cache_write_release(wimg, DT_IMAGE_CACHE_SAFE);
  }

  if(new_group_id != -1) dt_undo_clear(dt_undo_get_global(), DT_UNDO_GROUPING);
  dt_image_notify_changed(imgs);
  return new_group_id;
}

/** make an image the representative of the group it is in */
int dt_grouping_change_representative(const int32_t image_id)
{
  dt_image_t *img = dt_image_cache_get(image_id, 'r');
  const int group_id = img->group_id;
  dt_image_cache_read_release(img);
  if(group_id == image_id) return UNKNOWN_IMAGE;

  GList *members = dt_image_repository_get_group_members(group_id, -1);
  for(GList *m = members; m; m = g_list_next(m))
  {
    const int other_id = GPOINTER_TO_INT(m->data);
    dt_image_t *other_img = dt_image_cache_get(other_id, 'w');
    other_img->group_id = image_id;
    dt_image_cache_write_release(other_img, DT_IMAGE_CACHE_SAFE);
  }

  dt_undo_clear(dt_undo_get_global(), DT_UNDO_GROUPING);
  dt_image_notify_changed(members);
  return image_id;
}

/** get images of the group */
GList *dt_grouping_get_group_images(const int32_t imgid)
{
  GList *imgs = NULL;
  const dt_image_t *image = dt_image_cache_get(imgid, 'r');
  if(image)
  {
    const int img_group_id = image->group_id;
    dt_image_cache_read_release(image);

    GList *members = dt_image_repository_get_group_members(img_group_id, -1);
    for(GList *m = members; m; m = g_list_next(m))
      imgs = g_list_prepend(imgs, m->data);
    g_list_free(members);
  }
  return g_list_reverse(imgs);
}

/** add grouped images to images list */
void dt_grouping_add_grouped_images(GList **images)
{
  if(IS_NULL_PTR(*images)) return;
  GList *gimgs = NULL;
  for(GList *imgs = *images; imgs; imgs = g_list_next(imgs))
  {
    const dt_image_t *image = dt_image_cache_get(GPOINTER_TO_INT(imgs->data), 'r');
    if(image)
    {
      const int img_group_id = image->group_id;
      dt_image_cache_read_release(image);
      if(!IS_NULL_PTR(dt_collection_get_global()))
      {
        GList *members = dt_collection_query_get_group_members(img_group_id,
                                                               GPOINTER_TO_INT(imgs->data));
        for(GList *m = members; m; m = g_list_next(m))
          gimgs = g_list_prepend(gimgs, m->data);
        g_list_free(members);
      }
    }
  }

  if(gimgs)
    *images = g_list_concat(*images, g_list_reverse(gimgs));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
