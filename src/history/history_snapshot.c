/*
    This file is part of darktable,
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019-2020, 2022 Pascal Obry.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2022-2023 Aurélien PIERRE.
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

#include "database/database.h"
#include "history/history_snapshot.h"
#include "history/notify.h"
#include "system/mem_alloc.h"
#include "database/history_snapshot_repository.h"
#include "history/history.h"
#include "database/history_repository.h"
#include "caches/image_cache.h"

dt_undo_lt_history_t *dt_history_snapshot_item_init(void)
{
  return (dt_undo_lt_history_t *)g_malloc0(sizeof(dt_undo_lt_history_t));
}

void dt_history_snapshot_undo_create(const int32_t imgid, int *snap_id, int *history_end)
{
  // create history & mask snapshots for imgid, return the snapshot id
  *history_end = dt_history_repository_get_end(imgid);
  *snap_id = dt_history_snapshot_repository_next_id(imgid);

  if(!dt_history_snapshot_repository_create(*snap_id, imgid, *history_end == 0))
    fprintf(stderr, "[dt_history_snapshot_undo_create] fails to create a snapshot for %d\n", imgid);
}

static void _history_snapshot_undo_restore(const int32_t imgid, const int snap_id, const int history_end)
{
  // restore the given snapshot for imgid
  gboolean all_ok = TRUE;

  dt_database_start_transaction();

  dt_history_delete_on_image_ext(imgid, FALSE);
  dt_history_changed(DT_HISTORY_CHANGE_TAGS);

  // if no history end it means the image history was discarded, nothing more to restore
  if(history_end != 0)
    all_ok = dt_history_snapshot_repository_restore(snap_id, imgid);

  // set history end
  all_ok &= dt_history_repository_set_end(imgid, history_end);

  if(all_ok)
    dt_database_release_transaction();
  else
  {
    dt_database_rollback_transaction();
    fprintf(stderr, "[_history_snapshot_undo_restore] fails to restore a snapshot for %d\n", imgid);
  }

  dt_image_t *image = dt_image_cache_get(imgid, 'w');
  if(image)
  {
    // FIXME: this might be wrong or need more accurate handling
    image->history_hash = UINT64_MAX;
    dt_image_cache_write_release(image, DT_IMAGE_CACHE_RELAXED);
  }
}

static void _clear_undo_snapshot(const int32_t imgid, const int snap_id)
{
  dt_history_snapshot_repository_clear(snap_id, imgid);
}

void dt_history_snapshot_undo_lt_history_data_free(gpointer data)
{
  dt_undo_lt_history_t *hist = (dt_undo_lt_history_t *)data;

  _clear_undo_snapshot(hist->imgid, hist->after);

  // this is the first element in for this image, it corresponds to the initial status, we can safely remove it now
  if(hist->before == 0)
    _clear_undo_snapshot(hist->imgid, hist->before);

  dt_free(hist);
}

void dt_history_snapshot_undo_pop(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_LT_HISTORY)
  {
    dt_undo_lt_history_t *hist = (dt_undo_lt_history_t *)data;

    if(action == DT_ACTION_UNDO)
    {
      _history_snapshot_undo_restore(hist->imgid, hist->before, hist->before_history_end);
    }
    else
    {
      _history_snapshot_undo_restore(hist->imgid, hist->after, hist->after_history_end);
    }

    *imgs = g_list_append(*imgs, GINT_TO_POINTER(hist->imgid));
  }
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
