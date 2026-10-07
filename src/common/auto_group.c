/*
    This file is part of Ansel,
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

#include "common/auto_group.h"

#include "common/collection.h"
#include "common/undo.h"
#include "common/image_notify.h"
#include "database/database.h"
#include "system/mem_alloc.h"

#include <glib.h>
#include <string.h>

static dt_auto_group_message_handler_t _message_handler = NULL;

void dt_auto_group_set_message_handler(dt_auto_group_message_handler_t handler)
{
  _message_handler = handler;
}

typedef struct dt_auto_group_undo_t
{
  dt_image_group_assignment_t *forward;
  dt_image_group_assignment_t *reverse;
  size_t count;
} dt_auto_group_undo_t;

static gint _compare_auto_group_images(gconstpointer left, gconstpointer right)
{
  const dt_auto_group_image_t *const first = left;
  const dt_auto_group_image_t *const second = right;

  if(first->film_id != second->film_id) return first->film_id < second->film_id ? -1 : 1;

  const gint maker = g_strcmp0(first->camera_maker, second->camera_maker);
  if(maker) return maker;

  const gint model = g_strcmp0(first->camera_model, second->camera_model);
  if(model) return model;

  if(first->datetime_taken != second->datetime_taken)
    return first->datetime_taken < second->datetime_taken ? -1 : 1;

  return (first->imgid > second->imgid) - (first->imgid < second->imgid);
}

static gboolean _same_auto_group_identity(const dt_auto_group_image_t *first,
                                          const dt_auto_group_image_t *second)
{
  return first->film_id == second->film_id
         && !g_strcmp0(first->camera_maker, second->camera_maker)
         && !g_strcmp0(first->camera_model, second->camera_model);
}

typedef struct dt_auto_group_snapshot_collector_t
{
  dt_auto_group_snapshot_t *snapshot;
  gboolean failed;
} dt_auto_group_snapshot_collector_t;

static void _snapshot_image(void *user_data, const dt_image_t *info)
{
  dt_auto_group_snapshot_collector_t *const collector = user_data;
  dt_auto_group_snapshot_t *const snapshot = collector->snapshot;
  if(collector->failed || snapshot->count == G_MAXSIZE / sizeof(dt_auto_group_image_t))
  {
    collector->failed = TRUE;
    return;
  }

  dt_auto_group_image_t *const images = g_try_realloc_n(snapshot->images, snapshot->count + 1,
                                                         sizeof(dt_auto_group_image_t));
  if(IS_NULL_PTR(images))
  {
    collector->failed = TRUE;
    return;
  }

  dt_image_t normalized = *info;
  dt_image_refresh_makermodel(&normalized);

  snapshot->images = images;
  dt_auto_group_image_t *const image = &snapshot->images[snapshot->count++];
  *image = (dt_auto_group_image_t){ .imgid = normalized.id,
                                    .group_id = normalized.group_id,
                                    .film_id = normalized.film_id,
                                    .group_members = normalized.group_members,
                                    .datetime_taken = normalized.exif_datetime_taken };
  g_strlcpy(image->camera_maker, normalized.camera_maker, sizeof(image->camera_maker));
  g_strlcpy(image->camera_model, normalized.camera_model, sizeof(image->camera_model));
}

dt_auto_group_status_t dt_auto_group_snapshot_collect(dt_auto_group_snapshot_t *snapshot)
{
  if(IS_NULL_PTR(snapshot)) return DT_AUTO_GROUP_STATUS_ERROR;

  dt_auto_group_snapshot_cleanup(snapshot);
  dt_auto_group_snapshot_collector_t collector = { .snapshot = snapshot };
  if(!dt_image_repository_foreach_collected(_snapshot_image, &collector) || collector.failed)
  {
    dt_auto_group_snapshot_cleanup(snapshot);
    return DT_AUTO_GROUP_STATUS_ERROR;
  }
  return snapshot->count ? DT_AUTO_GROUP_STATUS_OK : DT_AUTO_GROUP_STATUS_EMPTY;
}

void dt_auto_group_snapshot_cleanup(dt_auto_group_snapshot_t *snapshot)
{
  if(IS_NULL_PTR(snapshot)) return;
  dt_free(snapshot->images);
  snapshot->images = NULL;
  snapshot->count = 0;
}

dt_auto_group_status_t dt_auto_group_plan(const dt_auto_group_snapshot_t *snapshot,
                                          const int32_t interval_seconds, dt_auto_group_plan_t *plan)
{
  if(IS_NULL_PTR(snapshot) || IS_NULL_PTR(plan) || (snapshot->count && IS_NULL_PTR(snapshot->images))
     || interval_seconds < 0 || interval_seconds > 3600)
    return DT_AUTO_GROUP_STATUS_ERROR;

  *plan = (dt_auto_group_plan_t){ 0 };

  GArray *const candidates = g_array_sized_new(FALSE, FALSE, sizeof(dt_auto_group_image_t), snapshot->count);
  GArray *const assignments = g_array_new(FALSE, FALSE, sizeof(dt_image_group_assignment_t));

  for(size_t image_index = 0; image_index < snapshot->count; image_index++)
  {
    const dt_auto_group_image_t *const image = &snapshot->images[image_index];
    if(image->group_id == image->imgid && image->group_members == 1 && image->datetime_taken > 0)
      g_array_append_val(candidates, *image);
  }

  g_array_sort(candidates, _compare_auto_group_images);
  const int64_t maximum_gap = (int64_t)interval_seconds * G_TIME_SPAN_SECOND;
  for(guint run_start = 0; run_start < candidates->len;)
  {
    guint run_end = run_start + 1;
    while(run_end < candidates->len)
    {
      const dt_auto_group_image_t *const previous = &g_array_index(candidates, dt_auto_group_image_t, run_end - 1);
      const dt_auto_group_image_t *const current = &g_array_index(candidates, dt_auto_group_image_t, run_end);
      if(!_same_auto_group_identity(previous, current)
         || current->datetime_taken - previous->datetime_taken > maximum_gap)
        break;
      run_end++;
    }

    if(run_end - run_start > 1)
    {
      plan->groups++;
      const int32_t representative = g_array_index(candidates, dt_auto_group_image_t, run_start).imgid;
      for(guint member_index = run_start; member_index < run_end; member_index++)
      {
        const dt_auto_group_image_t *const member = &g_array_index(candidates, dt_auto_group_image_t, member_index);
        const dt_image_group_assignment_t assignment = { .imgid = member->imgid,
                                                          .expected_group_id = member->group_id,
                                                          .new_group_id = representative };
        g_array_append_val(assignments, assignment);
      }
    }
    run_start = run_end;
  }

  plan->ungrouped = candidates->len - assignments->len;
  plan->skipped = snapshot->count - candidates->len;
  if(assignments->len)
  {
    plan->count = assignments->len;
    plan->assignments = (dt_image_group_assignment_t *)g_array_free(assignments, FALSE);
  }
  else g_array_free(assignments, TRUE);
  g_array_free(candidates, TRUE);
  return plan->count ? DT_AUTO_GROUP_STATUS_OK : DT_AUTO_GROUP_STATUS_EMPTY;
}

void dt_auto_group_plan_cleanup(dt_auto_group_plan_t *plan)
{
  if(IS_NULL_PTR(plan)) return;
  dt_free(plan->assignments);
  plan->assignments = NULL;
  plan->count = 0;
}

static void _auto_group_notify(const dt_image_group_assignment_t *assignments, const size_t count)
{
  GList *imgids = NULL;
  for(size_t index = 0; index < count; index++)
    imgids = g_list_prepend(imgids, GINT_TO_POINTER(assignments[index].imgid));

  dt_image_notify_changed(g_list_reverse(imgids));
  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_GROUPING, NULL);
}

static dt_auto_group_status_t _auto_group_apply(const dt_image_group_assignment_t *assignments,
                                                const size_t count)
{
  if(!dt_database_start_transaction_checked()) return DT_AUTO_GROUP_STATUS_ERROR;
  if(!dt_image_repository_assign_groups_if_unchanged(assignments, count))
  {
    if(!dt_database_rollback_transaction_checked())
      g_error("automatic grouping could not roll back its failed transaction");
    return DT_AUTO_GROUP_STATUS_CONFLICT;
  }
  if(dt_database_release_transaction_checked()) return DT_AUTO_GROUP_STATUS_OK;

  if(!dt_database_rollback_transaction_checked())
    g_error("automatic grouping could not recover from a failed commit");
  return DT_AUTO_GROUP_STATUS_ERROR;
}

static void _auto_group_undo_data_free(gpointer data)
{
  dt_auto_group_undo_t *const undo = data;
  dt_free(undo->forward);
  dt_free(undo->reverse);
  dt_free(undo);
}

static void _auto_group_undo_apply(gpointer user_data, dt_undo_type_t type G_GNUC_UNUSED, dt_undo_data_t data,
                                   const dt_undo_action_t action, GList **imgs G_GNUC_UNUSED)
{
  const dt_auto_group_undo_t *const undo = data;
  const dt_image_group_assignment_t *const assignments
      = action == DT_ACTION_UNDO ? undo->reverse : undo->forward;
  if(_auto_group_apply(assignments, undo->count) == DT_AUTO_GROUP_STATUS_OK)
  {
    _auto_group_notify(assignments, undo->count);
    return;
  }

  if(!IS_NULL_PTR(_message_handler)) _message_handler(_("automatic grouping could not be restored"));
  dt_undo_defer_clear(user_data, DT_UNDO_GROUPING);
}

dt_auto_group_status_t dt_auto_group_execute(const dt_auto_group_plan_t *plan)
{
  if(IS_NULL_PTR(plan) || (plan->count && IS_NULL_PTR(plan->assignments)))
    return DT_AUTO_GROUP_STATUS_ERROR;
  if(plan->count == 0) return DT_AUTO_GROUP_STATUS_EMPTY;

  dt_auto_group_undo_t *const undo = g_try_new0(dt_auto_group_undo_t, 1);
  if(IS_NULL_PTR(undo)) return DT_AUTO_GROUP_STATUS_ERROR;
  undo->forward = g_try_new(dt_image_group_assignment_t, plan->count);
  undo->reverse = g_try_new(dt_image_group_assignment_t, plan->count);
  if(IS_NULL_PTR(undo->forward) || IS_NULL_PTR(undo->reverse))
  {
    _auto_group_undo_data_free(undo);
    return DT_AUTO_GROUP_STATUS_ERROR;
  }
  memcpy(undo->forward, plan->assignments, plan->count * sizeof(*plan->assignments));
  undo->count = plan->count;

  GHashTable *const imgids = g_hash_table_new(g_direct_hash, g_direct_equal);
  if(IS_NULL_PTR(imgids))
  {
    _auto_group_undo_data_free(undo);
    return DT_AUTO_GROUP_STATUS_ERROR;
  }

  for(size_t index = 0; index < plan->count; index++)
  {
    const int32_t imgid = plan->assignments[index].imgid;
    if(imgid <= 0 || g_hash_table_contains(imgids, GINT_TO_POINTER(imgid)))
    {
      g_hash_table_destroy(imgids);
      _auto_group_undo_data_free(undo);
      return DT_AUTO_GROUP_STATUS_ERROR;
    }
    g_hash_table_add(imgids, GINT_TO_POINTER(imgid));
    undo->reverse[index] = (dt_image_group_assignment_t){ .imgid = imgid,
                                                            .expected_group_id = plan->assignments[index].new_group_id,
                                                            .new_group_id = plan->assignments[index].expected_group_id };
  }

  const dt_auto_group_status_t status = _auto_group_apply(plan->assignments, plan->count);
  if(status != DT_AUTO_GROUP_STATUS_OK)
  {
    g_hash_table_destroy(imgids);
    _auto_group_undo_data_free(undo);
    return status;
  }

  g_hash_table_destroy(imgids);
  dt_undo_t *const undo_stack = dt_undo_get_global();
  if(!IS_NULL_PTR(undo_stack))
  {
    dt_undo_clear(undo_stack, DT_UNDO_GROUPING);
    dt_undo_start_group(undo_stack, DT_UNDO_GROUPING);
    dt_undo_record(undo_stack, undo_stack, DT_UNDO_GROUPING, undo, _auto_group_undo_apply,
                   _auto_group_undo_data_free);
    dt_undo_end_group(undo_stack);
  }
  else
    _auto_group_undo_data_free(undo);
  _auto_group_notify(plan->assignments, plan->count);
  return DT_AUTO_GROUP_STATUS_OK;
}
