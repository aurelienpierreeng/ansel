/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include "common/act_on.h"
#include "common/collection.h"
#include "common/selection.h"
#include "views/view.h"


/**
 * We try selection first because it is global and should be reset when entering a view.
 * If the selection matters to the view, it should be copied to dt_view_active_images_set() when entering.
 * Selection can be restored from active_images when leaving the view.
 * Interactions with filmroll in other views than lighttable are sent to selection.
 * Therefore, if selection, target it. If not, target what should be the image of interest for the current view.
 */


// get the list of images to act on during global changes (libs, accels)
GList *dt_act_on_get_images()
{
  if(dt_selection_get_length(darktable.selection) > 0)
    return g_list_copy(dt_selection_get_list(darktable.selection));

  else if(dt_view_active_images_get_first() > -1)
    return g_list_copy(dt_view_active_images_get_all());

  return NULL;
}

// get only the number of images to act on
int dt_act_on_get_images_nb(const gboolean only_visible, const gboolean force)
{
  if(dt_selection_get_length(darktable.selection) > 0)
    return dt_selection_get_length(darktable.selection);

  else if(dt_view_active_images_get_first() > -1)
    return g_list_length(dt_view_active_images_get_all());

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
