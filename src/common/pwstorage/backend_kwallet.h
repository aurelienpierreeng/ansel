/*
    This file is part of darktable,
    Copyright (C) 2010, 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2011 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2022 Martin Bařinka.
    
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

// darktable is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// darktable is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with darktable.  If not, see <http://www.gnu.org/licenses/>.

#ifndef DT_COMMON_PWSTORAGE_BACKEND_KWALLET_H
#define DT_COMMON_PWSTORAGE_BACKEND_KWALLET_H

#include <gio/gio.h>
#include <glib.h>

/** kwallet backend context */
typedef struct backend_kwallet_context_t
{
  // Connection to the DBus session bus.
  //   DBusGConnection* connection;
  GDBusConnection *connection;

  // Proxy to the kwallet DBus service.
  GDBusProxy *proxy;

  // The name of the wallet we've opened. Set during init_kwallet().
  gchar *wallet_name;
} backend_kwallet_context_t;

/** Initializes a new kwallet backend context. */
const backend_kwallet_context_t *dt_pwstorage_kwallet_new();
/** Cleanup and destroy kwallet backend context. */
void dt_pwstorage_kwallet_destroy(const backend_kwallet_context_t *context);
/** Store (key,value) pairs. */
gboolean dt_pwstorage_kwallet_set(const backend_kwallet_context_t *context, const gchar *slot,
                                  GHashTable *table);
/** Load (key,value) pairs. */
GHashTable *dt_pwstorage_kwallet_get(const backend_kwallet_context_t *context, const gchar *slot);

#endif // DT_COMMON_PWSTORAGE_BACKEND_KWALLET_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

