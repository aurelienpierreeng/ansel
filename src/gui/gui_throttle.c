/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

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

#include "gui/gui_throttle.h"

#include "common/atomic.h"
#include "common/dtpthread.h"
#include "control/conf.h"
#include "develop/pixelpipe_hb.h"

#include <stdint.h>

#define DT_GUI_THROTTLE_RUNTIME_CONF "processing/gui_throttle_runtime_us"

typedef struct dt_gui_throttle_task_t
{
  gpointer source;
  dt_gui_throttle_callback_t callback;
  gpointer user_data;
} dt_gui_throttle_task_t;

typedef struct dt_gui_throttle_state_t
{
  dt_pthread_mutex_t runtime_mutex;
  uint32_t recent_runtime_us[5];
  uint8_t recent_runtime_count;
  uint8_t recent_runtime_pos;
  dt_atomic_int avg_runtime_us;
  uint32_t recent_full_runtime_us[5];
  uint8_t recent_full_runtime_count;
  uint8_t recent_full_runtime_pos;
  dt_atomic_int avg_full_runtime_us;
  uint32_t recent_preview_runtime_us[5];
  uint8_t recent_preview_runtime_count;
  uint8_t recent_preview_runtime_pos;
  dt_atomic_int avg_preview_runtime_us;

  GQueue pending_tasks;
  guint timeout_source;
} dt_gui_throttle_state_t;

static dt_gui_throttle_state_t _gui_throttle = { 0 };

static dt_gui_throttle_task_t *_find_task(gpointer source)
{
  for(GList *iter = _gui_throttle.pending_tasks.head; iter; iter = g_list_next(iter))
  {
    dt_gui_throttle_task_t *task = (dt_gui_throttle_task_t *)iter->data;
    if(task->source == source) return task;
  }

  return NULL;
}

static guint _get_user_timeout_ms(void)
{
  return (guint)MAX(dt_conf_get_int("processing/timeout"), 0);
}

static guint _runtime_us_to_ms(const int runtime_us)
{
  if(runtime_us <= 0) return 0;
  return (guint)MAX(1, (runtime_us + 999) / 1000);
}

static guint _effective_timeout_ms(void)
{
  const guint user_timeout_ms = _get_user_timeout_ms();
  const guint runtime_timeout_ms = _runtime_us_to_ms(dt_atomic_get_int(&_gui_throttle.avg_runtime_us));
  if(runtime_timeout_ms == 0) return user_timeout_ms;
  return MIN(user_timeout_ms, runtime_timeout_ms);
}

static gboolean _dispatch_pending_tasks(gpointer user_data)
{
  (void)user_data;

  _gui_throttle.timeout_source = 0;

  GQueue ready = G_QUEUE_INIT;
  ready.head = _gui_throttle.pending_tasks.head;
  ready.tail = _gui_throttle.pending_tasks.tail;
  ready.length = _gui_throttle.pending_tasks.length;
  g_queue_init(&_gui_throttle.pending_tasks);

  while(!g_queue_is_empty(&ready))
  {
    dt_gui_throttle_task_t *task = (dt_gui_throttle_task_t *)g_queue_pop_head(&ready);
    if(task->callback) task->callback(task->user_data);
    g_free(task);
  }

  return G_SOURCE_REMOVE;
}

void dt_gui_throttle_init(void)
{
  dt_pthread_mutex_init(&_gui_throttle.runtime_mutex, NULL);
  g_queue_init(&_gui_throttle.pending_tasks);

  const int saved_runtime_us = MAX(dt_conf_get_int(DT_GUI_THROTTLE_RUNTIME_CONF), 0);
  dt_atomic_set_int(&_gui_throttle.avg_runtime_us, saved_runtime_us);
  dt_atomic_set_int(&_gui_throttle.avg_full_runtime_us, saved_runtime_us);
  dt_atomic_set_int(&_gui_throttle.avg_preview_runtime_us, saved_runtime_us);

  if(saved_runtime_us > 0)
  {
    for(uint8_t i = 0; i < G_N_ELEMENTS(_gui_throttle.recent_runtime_us); i++)
    {
      _gui_throttle.recent_runtime_us[i] = (uint32_t)saved_runtime_us;
      _gui_throttle.recent_full_runtime_us[i] = (uint32_t)saved_runtime_us;
      _gui_throttle.recent_preview_runtime_us[i] = (uint32_t)saved_runtime_us;
    }

    _gui_throttle.recent_runtime_count = G_N_ELEMENTS(_gui_throttle.recent_runtime_us);
    _gui_throttle.recent_runtime_pos = 0;
    _gui_throttle.recent_full_runtime_count = G_N_ELEMENTS(_gui_throttle.recent_full_runtime_us);
    _gui_throttle.recent_full_runtime_pos = 0;
    _gui_throttle.recent_preview_runtime_count = G_N_ELEMENTS(_gui_throttle.recent_preview_runtime_us);
    _gui_throttle.recent_preview_runtime_pos = 0;
  }
}

void dt_gui_throttle_cleanup(void)
{
  dt_conf_set_int(DT_GUI_THROTTLE_RUNTIME_CONF, MAX(dt_atomic_get_int(&_gui_throttle.avg_runtime_us), 0));

  if(_gui_throttle.timeout_source)
  {
    g_source_remove(_gui_throttle.timeout_source);
    _gui_throttle.timeout_source = 0;
  }

  while(!g_queue_is_empty(&_gui_throttle.pending_tasks))
    g_free(g_queue_pop_head(&_gui_throttle.pending_tasks));

  dt_pthread_mutex_destroy(&_gui_throttle.runtime_mutex);
}

void dt_gui_throttle_record_runtime(const dt_dev_pixelpipe_t *pipe, const gint64 runtime_us)
{
  if(!pipe || runtime_us <= 0) return;
  if(pipe->type != DT_DEV_PIXELPIPE_FULL && pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return;

  const uint32_t clamped_runtime_us = (uint32_t)MIN(runtime_us, (gint64)G_MAXUINT32);

  dt_pthread_mutex_lock(&_gui_throttle.runtime_mutex);
  _gui_throttle.recent_runtime_us[_gui_throttle.recent_runtime_pos] = clamped_runtime_us;
  if(_gui_throttle.recent_runtime_count < G_N_ELEMENTS(_gui_throttle.recent_runtime_us))
    _gui_throttle.recent_runtime_count++;
  _gui_throttle.recent_runtime_pos
      = (uint8_t)((_gui_throttle.recent_runtime_pos + 1) % G_N_ELEMENTS(_gui_throttle.recent_runtime_us));

  uint64_t runtime_sum = 0;
  for(uint8_t i = 0; i < _gui_throttle.recent_runtime_count; i++)
    runtime_sum += _gui_throttle.recent_runtime_us[i];

  const int avg_runtime_us = (int)(runtime_sum / MAX(_gui_throttle.recent_runtime_count, (uint8_t)1));
  dt_atomic_set_int(&_gui_throttle.avg_runtime_us, avg_runtime_us);

  if(pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    _gui_throttle.recent_full_runtime_us[_gui_throttle.recent_full_runtime_pos] = clamped_runtime_us;
    if(_gui_throttle.recent_full_runtime_count < G_N_ELEMENTS(_gui_throttle.recent_full_runtime_us))
      _gui_throttle.recent_full_runtime_count++;
    _gui_throttle.recent_full_runtime_pos
        = (uint8_t)((_gui_throttle.recent_full_runtime_pos + 1) % G_N_ELEMENTS(_gui_throttle.recent_full_runtime_us));

    uint64_t full_runtime_sum = 0;
    for(uint8_t i = 0; i < _gui_throttle.recent_full_runtime_count; i++)
      full_runtime_sum += _gui_throttle.recent_full_runtime_us[i];

    const int avg_full_runtime_us
        = (int)(full_runtime_sum / MAX(_gui_throttle.recent_full_runtime_count, (uint8_t)1));
    dt_atomic_set_int(&_gui_throttle.avg_full_runtime_us, avg_full_runtime_us);
  }
  else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    _gui_throttle.recent_preview_runtime_us[_gui_throttle.recent_preview_runtime_pos] = clamped_runtime_us;
    if(_gui_throttle.recent_preview_runtime_count < G_N_ELEMENTS(_gui_throttle.recent_preview_runtime_us))
      _gui_throttle.recent_preview_runtime_count++;
    _gui_throttle.recent_preview_runtime_pos
        = (uint8_t)((_gui_throttle.recent_preview_runtime_pos + 1)
                    % G_N_ELEMENTS(_gui_throttle.recent_preview_runtime_us));

    uint64_t preview_runtime_sum = 0;
    for(uint8_t i = 0; i < _gui_throttle.recent_preview_runtime_count; i++)
      preview_runtime_sum += _gui_throttle.recent_preview_runtime_us[i];

    const int avg_preview_runtime_us
        = (int)(preview_runtime_sum / MAX(_gui_throttle.recent_preview_runtime_count, (uint8_t)1));
    dt_atomic_set_int(&_gui_throttle.avg_preview_runtime_us, avg_preview_runtime_us);
  }
  dt_pthread_mutex_unlock(&_gui_throttle.runtime_mutex);
}

int dt_gui_throttle_get_runtime_us(void)
{
  return MAX(dt_atomic_get_int(&_gui_throttle.avg_runtime_us), 0);
}

int dt_gui_throttle_get_pipe_runtime_us(const dt_dev_pixelpipe_type_t pipe_type)
{
  switch(pipe_type)
  {
    case DT_DEV_PIXELPIPE_FULL:
      return MAX(dt_atomic_get_int(&_gui_throttle.avg_full_runtime_us), 0);

    case DT_DEV_PIXELPIPE_PREVIEW:
      return MAX(dt_atomic_get_int(&_gui_throttle.avg_preview_runtime_us), 0);

    case DT_DEV_PIXELPIPE_NONE:
    case DT_DEV_PIXELPIPE_EXPORT:
    case DT_DEV_PIXELPIPE_THUMBNAIL:
    default:
      return dt_gui_throttle_get_runtime_us();
  }
}

guint dt_gui_throttle_get_timeout_ms(void)
{
  return _effective_timeout_ms();
}

gint64 dt_gui_throttle_get_timeout_us(void)
{
  const gint64 timeout_ms = _effective_timeout_ms();
  if(timeout_ms <= 0) return 0;
  return timeout_ms * 1000;
}

void dt_gui_throttle_queue(gpointer source, dt_gui_throttle_callback_t callback, gpointer user_data)
{
  if(!callback) return;
  if(!source) source = user_data;

  const guint timeout_ms = _effective_timeout_ms();
  if(timeout_ms == 0)
  {
    dt_gui_throttle_cancel(source);
    callback(user_data);
    return;
  }

  dt_gui_throttle_task_t *task = _find_task(source);
  if(task)
  {
    task->callback = callback;
    task->user_data = user_data;
  }
  else
  {
    task = g_malloc0(sizeof(*task));
    task->source = source;
    task->callback = callback;
    task->user_data = user_data;
    g_queue_push_tail(&_gui_throttle.pending_tasks, task);
  }

  if(!_gui_throttle.timeout_source)
    _gui_throttle.timeout_source = g_timeout_add(timeout_ms, _dispatch_pending_tasks, NULL);
}

void dt_gui_throttle_cancel(gpointer source)
{
  if(!source) return;

  for(GList *iter = _gui_throttle.pending_tasks.head; iter; iter = g_list_next(iter))
  {
    dt_gui_throttle_task_t *task = (dt_gui_throttle_task_t *)iter->data;
    if(task->source != source) continue;

    g_queue_delete_link(&_gui_throttle.pending_tasks, iter);
    g_free(task);
    break;
  }

  if(_gui_throttle.timeout_source && g_queue_is_empty(&_gui_throttle.pending_tasks))
  {
    g_source_remove(_gui_throttle.timeout_source);
    _gui_throttle.timeout_source = 0;
  }
}
