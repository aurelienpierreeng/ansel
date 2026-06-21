/*
    This file is part of Ansel
    Copyright (C) 2026 - Aurélien PIERRE

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "iop-autoset.h"
#include "develop/develop.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include "control/conf.h"
#include "control/control.h"

#include <glib.h>

static void _dt_iop_autoset_restart_cache_wait(gpointer user_data)
{
  dt_autoset_manager_t *manager = (dt_autoset_manager_t *)user_data;
  if(IS_NULL_PTR(manager) || IS_NULL_PTR(manager->dev)) return;

  dt_iop_autoset_advance(manager->dev, manager);
}

// Leave the busy cursor/progress state, exactly once.
static void _dt_iop_autoset_progress_leave(dt_autoset_manager_t *manager)
{
  if(!IS_NULL_PTR(manager) && manager->progress_cursor_active)
  {
    dt_control_log_busy_leave();
    manager->progress_cursor_active = FALSE;
  }
}

// Tear down an autoset run: drop any pending cache-wait, clear the pipe flag
// and release the busy cursor. Called whenever the queue can no longer be
// processed (exhausted, or every remaining module went stale).
static void _dt_iop_autoset_finish(dt_dev_pixelpipe_t *pipe, dt_autoset_manager_t *manager,
                                   dt_dev_pixelpipe_cache_wait_t *input_wait, const char *reason)
{
  if(!IS_NULL_PTR(input_wait))
    dt_dev_pixelpipe_cache_wait_cleanup(input_wait, reason);
  if(!IS_NULL_PTR(pipe))
    pipe->autoset = FALSE;
  _dt_iop_autoset_progress_leave(manager);
}

// Pop the next still-valid module off the queue, discarding stale entries.
// The pipeline can be torn down and rebuilt between two advance() calls, so a
// queued module pointer may dangle, get reused for a different module, or lose
// its autoset callback. Re-validate against the live module list before use.
static dt_iop_module_t *_dt_iop_autoset_peek_next(struct dt_develop_t *dev, dt_autoset_manager_t *manager)
{
  GList *mod = g_list_first(manager->iop_to_set);
  while(mod)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)mod->data;
    if(!IS_NULL_PTR(module) && g_list_find(dev->iop, module) && !IS_NULL_PTR(module->autoset))
      return module;

    // Stale entry: drop it and look at the next one.
    manager->iop_to_set = g_list_delete_link(manager->iop_to_set, mod);
    mod = g_list_first(manager->iop_to_set);
  }
  return NULL;
}

gchar *dt_iop_autoset_get_conf_key(const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return NULL;
  gchar *key = g_strdup_printf("plugins/darkroom/autoset/%s/%i", module->op, module->multi_priority);
  return key;
}

gboolean dt_iop_autoset_module_is_enabled(const dt_iop_module_t *module)
{
  gchar *key = dt_iop_autoset_get_conf_key(module);
  if(IS_NULL_PTR(key)) return FALSE;

  const gboolean enabled = !dt_conf_key_exists(key) || dt_conf_get_int(key) != 0;
  g_free(key);
  return enabled;
}

void dt_iop_autoset_module_set_enabled(const dt_iop_module_t *module, const gboolean enabled)
{
  gchar *key = dt_iop_autoset_get_conf_key(module);
  if(IS_NULL_PTR(key)) return;

  dt_conf_set_int(key, enabled ? 1 : 0);
  g_free(key);
}

void dt_iop_autoset_build_list(struct dt_develop_t *dev, dt_autoset_manager_t *manager)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(manager)) return;

  if(IS_NULL_PTR(manager->input_wait))
    manager->input_wait = g_malloc0(sizeof(dt_dev_pixelpipe_cache_wait_t));
  dt_dev_pixelpipe_cache_wait_cleanup((dt_dev_pixelpipe_cache_wait_t *)manager->input_wait,
                                      "autoset-build-list-reset");

  manager->dev = dev;
  _dt_iop_autoset_progress_leave(manager);

  g_list_free(manager->iop_to_set);
  manager->iop_to_set = NULL;
  dev->preview_pipe->autoset = TRUE;
  for(GList *mod = g_list_first(dev->iop); mod; mod = g_list_next(mod))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)mod->data;
    if(module->enabled && !IS_NULL_PTR(module->autoset) && dt_iop_autoset_module_is_enabled(module))
      manager->iop_to_set = g_list_append(manager->iop_to_set, module);
  }

  // Start immediately in case we already have the output in cache. If the
  // cacheline was not found, the request was sent to the pipeline, so retry later.
  dt_iop_autoset_advance(dev, manager);
}

int dt_iop_autoset_advance(struct dt_develop_t *dev, dt_autoset_manager_t *manager)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(manager)) return 1;

  if(IS_NULL_PTR(manager->input_wait))
    manager->input_wait = g_malloc0(sizeof(dt_dev_pixelpipe_cache_wait_t));
  dt_dev_pixelpipe_cache_wait_t *input_wait = (dt_dev_pixelpipe_cache_wait_t *)manager->input_wait;
  dt_dev_pixelpipe_t *pipe = dev->preview_pipe;

  dt_iop_module_t *module = _dt_iop_autoset_peek_next(dev, manager);
  if(IS_NULL_PTR(module))
  {
    _dt_iop_autoset_finish(pipe, manager, input_wait, "autoset-finished");
    return 1;
  }

  // Enter busy cursor exactly when the first autoset operation starts.
  if(!manager->progress_cursor_active)
  {
    dt_control_log_busy_enter();
    dt_control_change_cursor_by_name_and_flush("progress");
    manager->progress_cursor_active = TRUE;
  }

  // Module pieces (pipeline nodes) are not stable in time: the pipeline can be
  // destroyed and reconstructed, so grab the piece currently attached to the
  // module rather than caching a reference.
  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(pipe, module);
  if(IS_NULL_PTR(piece)) return 1;
  const dt_dev_pixelpipe_iop_t *const input_piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);
  if(IS_NULL_PTR(input_piece)) return 1;

  // Get the corresponding pipeline cache entry immediately if possible, else the
  // following function requests a partial pipe recompute and we retry later.
  dt_pixel_cache_entry_t *entry = NULL;
  void *input = NULL;
  dt_dev_pixelpipe_cache_wait_set_owner(input_wait, "autoset-input", manager);
  if(!dt_dev_pixelpipe_cache_peek_gui(pipe, input_piece, &input, &entry,
                                      input_wait, _dt_iop_autoset_restart_cache_wait, manager))
    return 1;

  // module->autoset manipulates the module's internal parameters outside of the
  // normal (GUI) control flow. Protect concurrent params writes from the GUI and
  // write history while we still hold the lock.
  dt_iop_gui_enter_critical_section(module);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);
  module->autoset(module, pipe, piece, input);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  dt_dev_add_history_item(dev, module, FALSE, FALSE);
  dt_iop_gui_leave_critical_section(module);

  // Params have changed: refresh the module GUI to reflect it.
  dt_iop_gui_update(module);

  manager->iop_to_set = g_list_remove(manager->iop_to_set, module);
  if(IS_NULL_PTR(manager->iop_to_set))
    _dt_iop_autoset_finish(pipe, manager, input_wait, "autoset-list-empty");

  return 0;
}
