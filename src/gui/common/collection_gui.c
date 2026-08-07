/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "gui/common/collection_gui.h"

#include "common/collection.h"
#include "common/macros.h"
#include "common/conf.h"

#include <stdio.h>
#include <string.h>

/* Store the n most recent collections in config for re-use in menu */
static void _update_recentcollections()
{
  // No GUI-presence test needed: with no GUI this handler is never registered.
  // Serialize current request
  char confname[200] = { 0 };
  char buf[4096];
  dt_collection_serialize(buf, sizeof(buf));

  int n = -1;
  gboolean found_duplicate = FALSE;

  // Check if current request already exist in history
  int num_items = dt_conf_get_int("plugins/lighttable/recentcollect/num_items");
  for(int k = 0; k < num_items; k++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
    const char *line = dt_conf_get_string_const(confname);
    if(IS_NULL_PTR(line)) continue;
    if(!strcmp(line, buf))
    {
      n = k;
      found_duplicate = TRUE;
      break;
    }
  }

  // Shift all history items one step behind. When the history is already full,
  // the last item has no destination slot and must be dropped before moving
  // the remaining entries down.
  const int max_items = CLAMP(dt_conf_get_int("plugins/lighttable/recentcollect/max_items"), 1,
                              NUM_LAST_COLLECTIONS);
  int shifted_index = MIN(num_items - (found_duplicate ? 1 : 0), max_items);
  for(int k = num_items - 1; k > -1; k--)
  {
    if(k == n) continue; // this is the duplicate of current collection we found, skip it

    // Get old records
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
    gchar *line1 = dt_conf_get_string(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", k);
    uint32_t pos1 = dt_conf_get_int(confname);

    // Write new records shifted by 1 slot
    if(IS_NULL_PTR(line1) || line1[0] == '\0')
    {
      dt_free(line1);
      continue;
    }

    if(shifted_index >= 0 && shifted_index < max_items)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", shifted_index);
      dt_conf_set_string(confname, line1);
      snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", shifted_index);
      dt_conf_set_int(confname, pos1);
    }
    shifted_index -= 1;
    dt_free(line1);
  }

  // Prepend current collection on top of history
  dt_conf_set_string("plugins/lighttable/recentcollect/line0", buf);

  // Increment items if we didn't find a duplicate
  num_items += found_duplicate ? 0 : 1;
  dt_conf_set_int("plugins/lighttable/recentcollect/num_items", CLAMP(num_items, 1, max_items));
}

void dt_collection_gui_register_handlers(void)
{
  dt_collection_set_recents_handler(_update_recentcollections);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
