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

#include <glib.h>

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
  g_list_free(manager->iop_to_set);
  manager->iop_to_set = NULL;
  dev->preview_pipe->autoset = TRUE;
  for(GList *mod = g_list_first(dev->iop); mod; mod = g_list_next(mod))
  {
    dt_iop_module_t * module = (dt_iop_module_t *)mod->data;
    if(module->enabled && !IS_NULL_PTR(module->autoset) && dt_iop_autoset_module_is_enabled(module))
    {
      manager->iop_to_set = g_list_append(manager->iop_to_set, module);
      fprintf(stdout, "adding %s\n", module->op);
    }
  }

  // Start immediately in case we already have the output in cache
  dt_iop_autoset_advance(dev, manager);
  // If the cacheline was not found, request was sent to pipeline so just retry later.
}

int dt_iop_autoset_advance(struct dt_develop_t *dev, dt_autoset_manager_t *manager)
{
  GList *mod = g_list_first(manager->iop_to_set);
  dt_dev_pixelpipe_t *pipe = dev->preview_pipe;
  if(IS_NULL_PTR(mod)) 
  {
    pipe->autoset = FALSE;
    return 1;
  }

  dt_iop_module_t *module = (dt_iop_module_t *)mod->data;
  fprintf(stdout, "trying to fetch cache from %s\n", module->op);

  // Note: module pieces (aka pipeline nodes) are not stable in time:
  // pipeline can be completely destroyed and reconstructed. So we can't store 
  // direct references, we need to grab the current piece attached to module
  // in the current pipeline.
  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(pipe, module);
  if(IS_NULL_PTR(piece))
    return 1;
  const dt_dev_pixelpipe_iop_t *const input_piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);

  // Get the corresponding pipeline cache entry immediately if possible,
  // else the following function requests a partial pipe recompute
  dt_pixel_cache_entry_t *entry = NULL;
  void *input = NULL;
  if(!dt_dev_pixelpipe_cache_peek_gui(pipe, input_piece ? input_piece : piece, &input, &entry, NULL, NULL, NULL))
    return 1;

  fprintf(stdout, "processing %s\n", module->op);

  // module->autoset will manipulate the internal parameters of the module
  // outside of the normal control flow (GUI).
  // We need to protect any concurrent params writing from the GUI
  // and write history while we still have the lock.
  dt_iop_gui_enter_critical_section(module);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);
  module->autoset(module, pipe, piece, input);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);

  dt_dev_add_history_item(dev, module, FALSE, FALSE);
  dt_iop_gui_leave_critical_section(module);

  // Params have changed, update the module GUI to reflect it.
  dt_iop_gui_update(module);

  manager->iop_to_set = g_list_remove_link(manager->iop_to_set, mod);
  return 0;
}
