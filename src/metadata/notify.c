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

#include "metadata/notify.h"

#include "system/mem_alloc.h"

#include <stdarg.h>

/* Written once, at startup, before any worker exists; read from whichever thread raises a
 * message. A lock would be guarding against a race nobody can create. */
static dt_metadata_notify_handler_t _handler = NULL;
static dt_metadata_tags_changed_handler_t _tags_changed_handler = NULL;

void dt_metadata_set_notify_handler(dt_metadata_notify_handler_t handler)
{
  _handler = handler;
}

void dt_metadata_notify(const dt_metadata_notice_t kind, const char *format, ...)
{
  dt_metadata_notify_handler_t handler = _handler;
  if(handler == NULL) return;  // headless, or a unit test: nothing to tell

  va_list args;
  va_start(args, format);
  gchar *message = g_strdup_vprintf(format, args);
  va_end(args);

  if(message)
  {
    handler(kind, message);
    dt_free(message);
  }
}

void dt_metadata_set_tags_changed_handler(dt_metadata_tags_changed_handler_t handler)
{
  _tags_changed_handler = handler;
}

void dt_metadata_tags_changed(void)
{
  dt_metadata_tags_changed_handler_t handler = _tags_changed_handler;
  if(handler) handler();
}

static dt_metadata_geotags_changed_handler_t _geotags_changed_handler = NULL;

void dt_metadata_set_geotags_changed_handler(dt_metadata_geotags_changed_handler_t handler)
{
  _geotags_changed_handler = handler;
}

void dt_metadata_geotags_changed(const GList *imgs)
{
  dt_metadata_geotags_changed_handler_t handler = _geotags_changed_handler;
  if(handler) handler(imgs);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
