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


#include "gui/common/styles_gui.h"

#include "control/user_message.h"
#include "gui/styles.h"

#include <glib/gi18n.h>

void dt_styles_create_from_list(const GList *list)
{
  gboolean selected = FALSE;
  /* for each image create style */
  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    dt_gui_styles_dialog_new(imgid);
    selected = TRUE;
  }

  if(!selected) dt_control_log(_("no image selected!"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
