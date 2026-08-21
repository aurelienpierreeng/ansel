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

#include "history/notify.h"

#include "system/mem_alloc.h"

#include <stdarg.h>

/* Written once, at startup, before any worker exists; read from whichever thread makes a
 * change. A lock would be guarding against a race nobody can create. */
static dt_history_message_handler_t _message_handler = NULL;
static dt_history_changed_handler_t _changed_handler = NULL;
static dt_history_images_changed_handler_t _images_changed_handler = NULL;

void dt_history_set_message_handler(dt_history_message_handler_t handler)
{
  _message_handler = handler;
}

void dt_history_message(const char *format, ...)
{
  dt_history_message_handler_t handler = _message_handler;
  if(handler == NULL) return;  // headless, or a unit test: nothing to tell

  va_list args;
  va_start(args, format);
  gchar *message = g_strdup_vprintf(format, args);
  va_end(args);

  if(message)
  {
    handler(message);
    dt_free(message);
  }
}

static dt_history_toast_handler_t _toast_handler = NULL;

void dt_history_set_toast_handler(dt_history_toast_handler_t handler)
{
  _toast_handler = handler;
}

void dt_history_toast(const char *format, ...)
{
  dt_history_toast_handler_t handler = _toast_handler;
  if(handler == NULL) return;

  va_list args;
  va_start(args, format);
  gchar *message = g_strdup_vprintf(format, args);
  va_end(args);

  if(message)
  {
    handler(message);
    dt_free(message);
  }
}

void dt_history_set_changed_handler(dt_history_changed_handler_t handler)
{
  _changed_handler = handler;
}

void dt_history_changed(const dt_history_change_t what)
{
  dt_history_changed_handler_t handler = _changed_handler;
  if(handler) handler(what);
}

void dt_history_set_images_changed_handler(dt_history_images_changed_handler_t handler)
{
  _images_changed_handler = handler;
}

void dt_history_changed_images(const GList *imgs)
{
  dt_history_images_changed_handler_t handler = _images_changed_handler;
  if(handler) handler(imgs);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
