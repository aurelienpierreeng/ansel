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

#include "common/image_notify.h"
#include "system/macros.h"

/* Written once, at startup, before any import job exists; read from whichever thread
 * imports. A lock would be guarding against a race nobody can create. */
static dt_image_imported_handler_t _imported_handler = NULL;
static dt_image_changed_handler_t _changed_handler = NULL;

void dt_image_notify_set_changed_handler(dt_image_changed_handler_t handler)
{
  _changed_handler = handler;
}

void dt_image_notify_changed(GList *imgids)
{
  if(!IS_NULL_PTR(_changed_handler))
    _changed_handler(imgids);
  else
    g_list_free(imgids);
}

void dt_image_notify_set_imported_handler(dt_image_imported_handler_t handler)
{
  _imported_handler = handler;
}

void dt_image_notify_imported(const int32_t imgid)
{
  dt_image_imported_handler_t handler = _imported_handler;
  if(handler) handler(imgid);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
