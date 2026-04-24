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

#include <glib.h>

typedef struct dt_autoset_manager_t 
{
  GList *iop_to_set;

} dt_autoset_manager_t;

typedef struct dt_develop_t dt_develop_t;
typedef struct dt_iop_module_t dt_iop_module_t;

void dt_iop_autoset_build_list(struct dt_develop_t *dev, dt_autoset_manager_t *manager);

int dt_iop_autoset_advance(struct dt_develop_t *dev, dt_autoset_manager_t *manager);

gchar *dt_iop_autoset_get_conf_key(const dt_iop_module_t *module);

gboolean dt_iop_autoset_module_is_enabled(const dt_iop_module_t *module);

void dt_iop_autoset_module_set_enabled(const dt_iop_module_t *module, const gboolean enabled);
