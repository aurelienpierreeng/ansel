/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.
    Copyright (C) 2026 Aurélien PIERRE.

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

#ifndef DT_DATABASE_LEGACY_PRESETS_H
#define DT_DATABASE_LEGACY_PRESETS_H

#include <sqlite3.h>

/** (Re)create main.legacy_presets and fill it with the presets darktable shipped before the
 *  auto-apply cleanup. Drops and re-inserts every time it is called.
 *
 *  Takes the connection rather than the database object because it runs from inside
 *  dt_database_open(), while the schema is still being built and before the module has
 *  published anything -- there is nothing to look up yet. */
void dt_legacy_presets_create(sqlite3 *handle);

#endif // DT_DATABASE_LEGACY_PRESETS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
