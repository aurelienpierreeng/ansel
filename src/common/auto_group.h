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

/** @file common/auto_group.h
 *
 * @brief Automatic grouping orchestration.
 *
 * The implementation owns collection snapshots, grouping plans, the caller-owned database
 * transaction, and post-commit notification and reload. Repository writes remain in
 * database/image_repository.h and do not own any of those operations.
 */

#ifndef DT_COMMON_AUTO_GROUP_H
#define DT_COMMON_AUTO_GROUP_H

#include "database/image_repository.h"

#include <stdint.h>
#include <stddef.h>

typedef enum dt_auto_group_status_t
{
  DT_AUTO_GROUP_STATUS_OK,
  DT_AUTO_GROUP_STATUS_EMPTY,
  DT_AUTO_GROUP_STATUS_CONFLICT,
  DT_AUTO_GROUP_STATUS_ERROR
} dt_auto_group_status_t;

/** @brief Receives a translated restoration failure message; the string is borrowed. */
typedef void (*dt_auto_group_message_handler_t)(const char *message);

/** @brief Install the restoration-failure listener at startup, or NULL for headless use. */
void dt_auto_group_set_message_handler(dt_auto_group_message_handler_t handler);

/** @brief One image in an automatic-group collection snapshot. */
typedef struct dt_auto_group_image_t
{
  int32_t imgid;
  int32_t group_id;
  int32_t film_id;
  uint32_t group_members;
  int64_t datetime_taken;
  char camera_maker[64];
  char camera_model[64];
} dt_auto_group_image_t;

/** @brief Complete current-collection snapshot. */
typedef struct dt_auto_group_snapshot_t
{
  dt_auto_group_image_t *images;
  size_t count;
} dt_auto_group_snapshot_t;

/** @brief Assignments owned by the automatic grouping plan. */
typedef struct dt_auto_group_plan_t
{
  dt_image_group_assignment_t *assignments;
  size_t count;
  size_t groups; /**< New groups in the plan. */
  size_t ungrouped; /**< Eligible images left without a matching neighbour. */
  size_t skipped; /**< Existing group members and images without a capture time. */
} dt_auto_group_plan_t;

/**
 * @brief Copy every row of the complete current collection into @p snapshot.
 *
 * @details The caller must initialize @p snapshot to zero and release it with
 * dt_auto_group_snapshot_cleanup(). The function owns no transaction and makes no database
 * changes. It returns ::DT_AUTO_GROUP_STATUS_EMPTY when the collection has no images.
 */
dt_auto_group_status_t dt_auto_group_snapshot_collect(dt_auto_group_snapshot_t *snapshot);

/** @brief Release the image rows owned by @p snapshot. */
void dt_auto_group_snapshot_cleanup(dt_auto_group_snapshot_t *snapshot);

/**
 * @brief Plan automatic groups from a complete borrowed collection snapshot.
 *
 * @details @p snapshot stays owned by its caller. The planner allocates @p plan->assignments on
 * success; the caller releases it with dt_auto_group_plan_cleanup(). @p plan must be initialized
 * to zero and cleaned before reuse. Both @p snapshot and @p plan must be non-NULL. The interval
 * must be in [0, 3600] seconds. The function starts no transaction and writes nothing.
 */
dt_auto_group_status_t dt_auto_group_plan(const dt_auto_group_snapshot_t *snapshot,
                                          int32_t interval_seconds, dt_auto_group_plan_t *plan);

/** @brief Release the assignments owned by @p plan. */
void dt_auto_group_plan_cleanup(dt_auto_group_plan_t *plan);

/**
 * @brief Apply @p plan in one automatic-group transaction.
 *
 * @details The executor owns the transaction, rollback, one post-commit notification, and one
 * grouping reload. It does not take ownership of @p plan and must run on the GUI thread.
 */
dt_auto_group_status_t dt_auto_group_execute(const dt_auto_group_plan_t *plan);

#endif // DT_COMMON_AUTO_GROUP_H
