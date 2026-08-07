/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2020 Pascal Obry.
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

#ifndef DT_COMMON_DBUS_H
#define DT_COMMON_DBUS_H

#include <glib.h>
#include <gio/gio.h>

typedef struct dt_dbus_t
{
  int connected;

  GDBusNodeInfo *introspection_data;
  guint owner_id;
  guint registration_id;

  // used for client actions on the bus
  GDBusConnection *dbus_connection;
} dt_dbus_t;

/** allocates and initializes dbus */
dt_dbus_t *dt_dbus_init();

/** closes down database and frees memory */
void dt_dbus_destroy(const dt_dbus_t *);

/* Process-wide singleton with no per-call context to ride on: this accessor is the
 * intended end state (same category as dt_conf_*), implemented by the orchestrator. */
struct dt_dbus_t *dt_dbus_get_global(void);

/** have we managed to get the dbus name? when not, then there is already another instance of darktable
 * running */
gboolean dt_dbus_connected(const dt_dbus_t *);

#endif // DT_COMMON_DBUS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

